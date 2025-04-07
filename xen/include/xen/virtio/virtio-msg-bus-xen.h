/*
 * virtio-msg-bus-xen interface.
 *
 * Copyright (c) 2025 Advanced Micro Devices, Inc.
 * Written by Edgar E. Iglesias <edgar.iglesias@amd.com>
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#ifndef __XEN_VIRTIO_MSG_BUS_XEN_H__
#define __XEN_VIRTIO_MSG_BUS_XEN_H__

#include <xen/virtio/virtio-msg-bus.h>
#include <xen/virtio/spsc_queue.h>

struct virtio_msg_bus_xen {
    /* Inherit from generic bus  */
    struct virtio_msg_bus bd;

    domid_t device_domid;
    evtchn_port_t evtchn;

    struct {
        spsc_queue driver;
        spsc_queue device;
    } queues;

    struct {
        struct page_info *page;
        void *ptr;
    } shm;
};

int virtio_msg_bus_xen_init(struct virtio_msg_bus_xen *bus, struct domain *d,
                            domid_t device_domid);

int virtio_msg_bus_xen_dm_connect(struct domain *d, uint32_t bus_id,
                                  uint64_t shm_fifo_gfn, uint32_t *port);
#endif /* __XEN_VIRTIO_MSG_BUS_XEN_H__ */
