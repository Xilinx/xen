/*
 * virtio-msg-bus over shared memory queues and event channels.
 *
 * Copyright (c) 2025 Advanced Micro Devices, Inc.
 * Written by Edgar E. Iglesias <edgar.iglesias@amd.com>
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include <xen/sched.h>
#include <xen/errno.h>
#include <xen/event.h>

#include <xen/virtio/virtio-msg-bus-xen.h>

/* Size of each side of the queue.  */
#define VIRTIO_MSG_QUEUE_SIZE 1024

static bool virtio_msg_bus_xen_back_pressure(struct virtio_msg_bus *bd)
{
    struct virtio_msg_bus_xen *bus = (void *) bd;

    return spsc_queue_is_full(&bus->queues.driver);
}

static bool virtio_msg_bus_xen_send(struct virtio_msg_bus *bd,
                                    VirtIOMSG *msg_req)
{
    struct virtio_msg_bus_xen *bus = (void *) bd;
    bool sent;

    sent = spsc_send(&bus->queues.driver, msg_req, sizeof(*msg_req));
    if ( sent )
        notify_via_xen_event_channel(bd->domain, bus->evtchn);

    return sent;
}

/* Rx budget of messages to process back-to-back before returning.  */
#define VMP_RX_BUDGET 2
static void virtio_msg_bus_xen_rx_process(struct virtio_msg_bus *bd)
{
    struct virtio_msg_bus_xen *bus = (void *) bd;
    unsigned int i = 0;
    spsc_queue *q;
    VirtIOMSG msg;
    bool r;

    /*
     * We process the opposite queue, i.e, a driver will want to receive
     * messages on the backend queue (and send messages on the driver queue).
     */
    q = &bus->queues.device;
    do
    {
        r = spsc_recv(q, &msg, sizeof(msg));
        if ( r )
        {
            virtio_msg_unpack(&msg);
            bd->client->rx_msg(bd->client_opaque, &msg);
        }
    } while ( r && i++ < VMP_RX_BUDGET );
}

static void virtio_msg_bus_xen_deinit(struct virtio_msg_bus *bd)
{
    struct virtio_msg_bus_xen *bus = (void *) bd;

    ASSERT(bd);

    if ( bus->shm.ptr )
        destroy_ring_for_helper(&bus->shm.ptr, bus->shm.page);

    if ( bus->evtchn )
        free_xen_event_channel(bd->domain, bus->evtchn);

    /* Zero out. */
    memset(&bus->queues, 0, sizeof(bus->queues));
    memset(&bus->shm, 0, sizeof(bus->shm));
    bus->device_domid = 0;
    bus->evtchn = 0;
}

static void vmb_xen_notify(struct vcpu *v, unsigned int port)
{
    struct domain *d = v->domain;
    unsigned int i;

    if ( d->is_dying )
        return;

    /* Event-channel notifiers run under rcu_lock_domain.  */
    for ( i = 0; i < ARRAY_SIZE(d->virtio_msg_bus); i++ )
    {
        struct virtio_msg_bus *bd = rcu_dereference(d->virtio_msg_bus[i]);
        struct virtio_msg_bus_xen *bus = (void *) bd;

        if ( bd && virtio_msg_bus_get_state(bd) != STATE_DISABLED &&
             bus->evtchn == port )
        {
            ASSERT(bd->domain == d);
            virtio_msg_bus_rx_process(bd);
            break;
        }
    }
}

static const struct virtio_msg_bus_channel vmbus_xen_channel = {
    .name = "Xen",
    .process = virtio_msg_bus_xen_rx_process,
    .back_pressure = virtio_msg_bus_xen_back_pressure,
    .send = virtio_msg_bus_xen_send,
    .deinit = virtio_msg_bus_xen_deinit,
};

int virtio_msg_bus_xen_dm_connect(struct domain *d, uint32_t bus_id,
                                  uint64_t shm_fifo_gfn, uint32_t *port)
{
    const int capacity = spsc_capacity(VIRTIO_MSG_QUEUE_SIZE);
    struct virtio_msg_bus_xen *bus;
    struct virtio_msg_bus *bd;
    struct vcpu *v = current;
    int rc;

    ASSERT(port);

    if ( bus_id >= ARRAY_SIZE(d->virtio_msg_bus) )
    {
        gprintk(XENLOG_ERR, "%s: bus_id %d out of bounds!\n",
                 __func__, bus_id);
        return -ENOENT;
    }

    /* DM ops already run under rcu_lock_domain(d).  */
    bd = rcu_dereference(d->virtio_msg_bus[bus_id]);

    if ( !bd )
    {
        gprintk(XENLOG_ERR, "%s: bus_id %d doesn't exist!\n",
                 __func__, bus_id);
        return -ENODEV;
    }

    if ( virtio_msg_bus_get_state(bd) == STATE_CONNECTED )
    {
        gprintk(XENLOG_ERR, "%s: bus_id %d already in use!\n",
                 __func__, bus_id);
        return -EBUSY;
    }

    /* Check that bus_id points to a virtio-msg-bus-xen.  */
    if ( bd->channel != &vmbus_xen_channel )
    {
        gdprintk(XENLOG_ERR, "%s: virtio_msg_bus.%d of wrong type!\n",
                 __func__, bus_id);
        return -EINVAL;
    }

    bus = (struct virtio_msg_bus_xen *) bd;

    /* Check that the issuing domain has permissions.  */
    if ( v->domain->domain_id != bus->device_domid )
    {
        gprintk(XENLOG_ERR,
                 "%s: virtio_msg_bus.%d owned by dom%d not by dom%d\n",
                 __func__, bus_id, bus->device_domid, v->domain->domain_id);
        return -EACCES;
    }

    rc = prepare_ring_for_helper(v->domain, shm_fifo_gfn, &bus->shm.page,
                                 &bus->shm.ptr);
    if ( rc < 0 )
        return rc;

    /* Initialize our queues.  */
    memset(bus->shm.ptr, 0, PAGE_SIZE);
    spsc_init(&bus->queues.driver, "driver", capacity, bus->shm.ptr);
    spsc_init(&bus->queues.device, "device", capacity,
              bus->shm.ptr + VIRTIO_MSG_QUEUE_SIZE);

    *port = bus->evtchn;
    virtio_msg_bus_set_state(bd, STATE_CONNECTED);

    gdprintk(XENLOG_INFO, "virtio-msg-bus.%d connected\n", bus_id);
    return 0;
}

int virtio_msg_bus_xen_init(struct virtio_msg_bus_xen *bus, struct domain *d,
                            domid_t device_domid)
{
    const int capacity = spsc_capacity(VIRTIO_MSG_QUEUE_SIZE);
    struct virtio_msg_bus *bd = &bus->bd;
    int rc;

    ASSERT(capacity >= 2 && capacity <= 32);
    BUILD_BUG_ON(VIRTIO_MSG_QUEUE_SIZE * 2 > PAGE_SIZE);
    BUILD_BUG_ON(sizeof(VirtIOMSG) > SPSC_QUEUE_MAX_PACKET_SIZE);

    virtio_msg_bus_init(bd, &vmbus_xen_channel, d);

    rc = alloc_unbound_xen_event_channel(d, 0, device_domid, vmb_xen_notify);
    if ( rc < 0 )
        return rc;

    bus->device_domid = device_domid;
    bus->evtchn = rc;
    return 0;
}
