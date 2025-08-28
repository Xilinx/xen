/*
 * Non-blocking virtio-mmio to virtio-msg proxy.
 *
 * Implements an extension to virtio-mmio allowing for indirect and
 * non-blocking accesses.
 *
 * Copyright (c) 2025 Advanced Micro Devices, Inc.
 * Written by Edgar E. Iglesias <edgar.iglesias@amd.com>
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include <xen/sched.h>
#include <xen/errno.h>
#include <xen/event.h>
#include <xen/bitmap.h>

#include <xen/virtio/std/virtio_mmio.h>
#include <xen/virtio/std/virtio_mmio_ind_ext.h>
#include <xen/virtio/std/virtio-msg-prot.h>
#include <xen/virtio/virtio-msg-bus.h>
#include <xen/virtio/vmp.h>

#define VMP_DEBUG 0

#define VIRT_VERSION 2
#define VIRT_VENDOR 0x58656e20 /* 'Xen ' */
/* Size of virtio-mmio's register range.  */
#define VIRTIO_MMIO_REG_SIZE 0x200

static void vmp_set_status(struct vmp *s);

static inline void vmp_update_interrupts(struct vmp *s)
{
    vmp_arch_set_irq(s, s->regs.interrupt_status);
}

/* Set or clear the busy bit. Must be called with s->lock held.  */
static inline void vmp_indirect_set_busy(struct vmp *s, bool busy)
{
    /*
     * Make sure stores to access_data are observed
     * before we clear the busy flag.
     */
    smp_wmb();

    s->regs.access &= ~VIRTIO_MMIO_ACCESS_BUSY;
    if ( busy )
    {
        s->regs.access |= VIRTIO_MMIO_ACCESS_BUSY;
    }
}

/* VQ enable/disable tracking.  */
static void vmp_vq_enable(struct vmp *s, unsigned int i, bool val)
{
    ASSERT(i < VIRTIO_MSG_MAX_QUEUES);

    if ( val )
        bitmap_set(s->vq.enabled_bitmap, i, 1);
    else
        bitmap_clear(s->vq.enabled_bitmap, i, 1);
}

static bool vmp_vq_is_enabled(struct vmp *s, unsigned int i)
{
    ASSERT(i < VIRTIO_MSG_MAX_QUEUES);
    return test_bit(i, s->vq.enabled_bitmap);
}

void vmp_print_stats(struct vmp *s)
{
#if VMP_STATS
    unsigned int i;

    gprintk(XENLOG_INFO, "%s:\n", __func__);
    for ( i = 0; i < ARRAY_SIZE(s->stats.state); i++ )
    {
        gprintk(XENLOG_INFO, "state[%d]=%d\n", i, s->stats.state[i]);
    }
    for ( i = 0; i < ARRAY_SIZE(s->stats.regs); i++ )
    {
        if ( s->stats.regs[i] )
            gprintk(XENLOG_INFO, "dir_reg[0x%x]=%d\n", i * 4,
                    s->stats.regs[i]);
    }
    for ( i = 0; i < ARRAY_SIZE(s->stats.ind_regs); i++ )
    {
        if ( s->stats.ind_regs[i] )
            gprintk(XENLOG_INFO, "ind_reg[0x%x]=%d\n", i * 4,
                    s->stats.ind_regs[i]);
    }
    for ( i = 0; i < ARRAY_SIZE(s->stats.ind_regs); i++ )
    {
        if ( s->stats.ind_regs[i] || s->stats.regs[i] )
            gprintk(XENLOG_INFO, "reg[0x%x]=%d\n", i * 4,
                    s->stats.regs[i] + s->stats.ind_regs[i]);
    }
#endif
}

/*
 * Validates and moves the state-machine into a new state.
 */
static void vmp_set_state(struct vmp *s, int new_state)
{
    bool bad_state = false;

    /*
     * Changing to the same state is not an error but it's an indication
     * that the code may not expect what just happened. So we print a warning
     * and return OK.
     */
    if ( s->state == new_state )
    {
        gdprintk(XENLOG_DEBUG, "State change to same state %d -> %d\n",
                 s->state, new_state);
    }

    /* Out of bounds?  */
    if ( new_state < VMP_STATE_IDLE || new_state >= VMP_STATE_MAX )
        bad_state = true;
    /* We can only go from STATE_IDLE to something and back.  */
    if ( s->state != VMP_STATE_IDLE && new_state != VMP_STATE_IDLE )
        bad_state = true;

    WARN_ON(bad_state);

    /* OK.  */
    s->state = new_state;
#if VMP_STATS
    s->stats.state[new_state]++;
#endif
}

static void vmp_receive_req(struct vmp *s, VirtIOMSG *msg)
{
    switch ( msg->msg_id )
    {
    case VIRTIO_MSG_EVENT_USED:
        s->regs.interrupt_status |= VIRTIO_MMIO_INT_VRING;
        vmp_update_interrupts(s);
        break;
    case VIRTIO_MSG_EVENT_CONFIG:
        s->regs.interrupt_status |= VIRTIO_MMIO_INT_CONFIG;
        vmp_update_interrupts(s);
        break;
    default:
        gprintk(XENLOG_DEBUG, "Dropped msg. Request %x\n", msg->msg_id);
        if ( VMP_DEBUG )
            virtio_msg_print(msg);
        /* Dropped.  */
        break;
    }
}

/*
 * Validate that this message is one we're expecting.
 */
static bool vmp_expected_msg_resp(struct vmp *s, VirtIOMSG *msg)
{
#define VMP_STATE_CHECK_MSGID(X) [VMP_STATE_ ## X] = VIRTIO_MSG_ ## X
    static const int valid[] = {
        VMP_STATE_CHECK_MSGID(GET_DEVICE_STATUS),
        VMP_STATE_CHECK_MSGID(SET_DEVICE_STATUS),
        VMP_STATE_CHECK_MSGID(DEVICE_INFO),
        VMP_STATE_CHECK_MSGID(GET_FEATURES),
        VMP_STATE_CHECK_MSGID(SET_FEATURES),
        VMP_STATE_CHECK_MSGID(GET_CONFIG),
        VMP_STATE_CHECK_MSGID(SET_CONFIG),
        VMP_STATE_CHECK_MSGID(GET_VQUEUE),
        VMP_STATE_CHECK_MSGID(SET_VQUEUE),
        VMP_STATE_CHECK_MSGID(RESET_VQUEUE),
    };

    ASSERT(s->state < ARRAY_SIZE(valid));
    return msg->msg_id == valid[s->state];
}

static void vmp_receive_msg(void *opaque, VirtIOMSG *msg)
{
    struct vmp *s = opaque;

    if ( VMP_DEBUG )
        virtio_msg_print(msg);

    spin_lock(&s->lock);
    if ( !(msg->type & VIRTIO_MSG_TYPE_RESPONSE) )
    {
        vmp_receive_req(s, msg);
        goto done;
    }

    /* Responses.  */
    if ( !vmp_expected_msg_resp(s, msg) )
    {
        gdprintk(XENLOG_DEBUG, "Dropping unexpected response! state %d\n",
                 s->state);
        if ( VMP_DEBUG )
            virtio_msg_print(msg);
        goto done;
    }

    switch ( msg->msg_id )
    {
    case VIRTIO_MSG_GET_DEVICE_STATUS:
        s->regs.access_data = msg->get_device_status_resp.status;
        break;
    case VIRTIO_MSG_DEVICE_INFO:
        s->regs.access_data = msg->get_device_info_resp.device_id;
        break;
    case VIRTIO_MSG_GET_FEATURES:
        s->regs.access_data = msg->get_features_resp.b32[0];
        break;
    case VIRTIO_MSG_SET_FEATURES:
        if ( s->next_state == VMP_STATE_SET_DEVICE_STATUS )
        {
            /* Go back to IDLE and then to new state to keep checker happy. */
            vmp_set_state(s, VMP_STATE_IDLE);
            vmp_set_state(s, VMP_STATE_SET_DEVICE_STATUS);
            s->next_state = VMP_STATE_IDLE;
            goto done;
        }
        break;
    case VIRTIO_MSG_GET_CONFIG:
        s->generation = msg->get_config_resp.generation;

        if ( msg->get_config_resp.size == 0 )
            s->regs.access_data = s->generation;
        else
        {
            unsigned int size = msg->get_config_resp.size;
            if ( size <= sizeof(s->regs.access_data) )
                memcpy(&s->regs.access_data, msg->get_config_resp.data, size);
        }
        break;
    case VIRTIO_MSG_GET_VQUEUE:
        s->regs.queue_max_size = msg->get_vqueue_resp.max_size;
        break;
    case VIRTIO_MSG_SET_CONFIG:
    case VIRTIO_MSG_SET_DEVICE_STATUS:
    case VIRTIO_MSG_SET_VQUEUE:
    case VIRTIO_MSG_RESET_VQUEUE:
        break;
    default:
        /* Already dropped unexpected responses, this should never happen. */
        virtio_msg_print(msg);
        ASSERT_UNREACHABLE();
        goto done;
    }

    vmp_indirect_set_busy(s, false);
    vmp_set_state(s, VMP_STATE_IDLE);
done:
    spin_unlock(&s->lock);
}

static void vmp_get_status(struct vmp *s)
{
    VirtIOMSG msg;

    virtio_msg_pack_get_device_status(&msg);
    virtio_msg_bus_send(s->bd, &msg);
}

static void vmp_set_status(struct vmp *s)
{
    VirtIOMSG msg;

    virtio_msg_pack_set_device_status(&msg, s->regs.status);
    virtio_msg_bus_send(s->bd, &msg);
}

static void vmp_get_device_info(struct vmp *s)
{
    VirtIOMSG msg;

    virtio_msg_pack_get_device_info(&msg);
    virtio_msg_bus_send(s->bd, &msg);
}

static void vmp_get_features(struct vmp *s)
{
    VirtIOMSG msg;

    /* Both virtio-mmio and virtio-msg use a 32-bit feature word array.  */
    virtio_msg_pack_get_features(&msg, s->regs.device_features_sel, 1);
    virtio_msg_bus_send(s->bd, &msg);
}

static void vmp_get_config(struct vmp *s, uint8_t size, uint32_t offset)
{
    VirtIOMSG msg;

    virtio_msg_pack_get_config(&msg, size, offset);
    virtio_msg_bus_send(s->bd, &msg);
}

static void vmp_set_config(struct vmp *s, uint8_t size, uint32_t offset,
                           uint32_t data)
{
    VirtIOMSG msg;

    virtio_msg_pack_set_config(&msg, size, offset, s->generation, &data);
    virtio_msg_bus_send(s->bd, &msg);
}

static void vmp_get_vqueue(struct vmp *s)
{
    VirtIOMSG msg;

    virtio_msg_pack_get_vqueue(&msg, s->regs.queue_sel);
    virtio_msg_bus_send(s->bd, &msg);
}

static void vmp_set_vqueue(struct vmp *s)
{
    VirtIOMSG msg;

    virtio_msg_pack_set_vqueue(&msg, s->regs.queue_sel, s->vq.size,
                               s->vq.descriptor_addr, s->vq.driver_addr,
                               s->vq.device_addr);
    virtio_msg_bus_send(s->bd, &msg);
}

static void vmp_reset_vqueue(struct vmp *s)
{
    VirtIOMSG msg;

    virtio_msg_pack_reset_vqueue(&msg, s->regs.queue_sel);
    virtio_msg_bus_send(s->bd, &msg);
}

static void vmp_set_features(struct vmp *s)
{
    VirtIOMSG msg;

    virtio_msg_pack_set_features(&msg, 0, ARRAY_SIZE(s->regs.driver_features),
                                 s->regs.driver_features);
    virtio_msg_bus_send(s->bd, &msg);
}

static void vmp_event_avail(struct vmp *s, uint32_t data)
{
    VirtIOMSG msg;
    uint32_t vq_index;
    uint64_t next_offset;
    uint64_t next_wrap;

    /*
     * spec:
     * le32 {
     *   vqn : 16;
     *   next_off : 15;
     *   next_wrap : 1;
     * };
     */
    vq_index = data & 0xffff;
    next_offset = (data >> 16) & GENMASK(14, 0);
    next_wrap = data >> 31;

    virtio_msg_pack_event_avail(&msg, vq_index, next_offset, next_wrap);
    virtio_msg_bus_send(s->bd, &msg);
}

static void vmp_indirect_read(struct vmp *s, int size, uint16_t offset)
{
    uint32_t data = 0;

#if VMP_STATS
    if ( offset < VIRTIO_MMIO_CONFIG )
        s->stats.ind_regs[offset / 4]++;
#endif

    switch ( offset )
    {
    case VIRTIO_MMIO_MAGIC_VALUE:
        data = VIRT_MAGIC_NON_BLOCKING;
        break;
    case VIRTIO_MMIO_VERSION:
        data = VIRT_VERSION;
        break;
    case VIRTIO_MMIO_VENDOR_ID:
        data = VIRT_VENDOR;
        break;
    case VIRTIO_MMIO_DEVICE_ID:
        vmp_indirect_set_busy(s, true);
        vmp_get_device_info(s);
        vmp_set_state(s, VMP_STATE_DEVICE_INFO);
        break;
    case VIRTIO_MMIO_INTERRUPT_STATUS:
        data = s->regs.interrupt_status;
        break;
    case VIRTIO_MMIO_STATUS:
        vmp_indirect_set_busy(s, true);
        vmp_get_status(s);
        vmp_set_state(s, VMP_STATE_GET_DEVICE_STATUS);
        break;
    case VIRTIO_MMIO_DEVICE_FEATURES:
        vmp_indirect_set_busy(s, true);
        vmp_get_features(s);
        vmp_set_state(s, VMP_STATE_GET_FEATURES);
        break;
    case VIRTIO_MMIO_CONFIG_GENERATION:
        vmp_indirect_set_busy(s, true);
        vmp_get_config(s, 0, 0);
        vmp_set_state(s, VMP_STATE_GET_CONFIG);
        break;
    case VIRTIO_MMIO_QUEUE_NUM_MAX:
        data = s->regs.queue_max_size;
        break;
    case VIRTIO_MMIO_QUEUE_READY:
        data = vmp_vq_is_enabled(s, s->regs.queue_sel);
        break;
    case VIRTIO_MMIO_CONFIG ... (VIRTIO_MMIO_CONFIG + 0x100):
        vmp_indirect_set_busy(s, true);
        vmp_get_config(s, size, offset - VIRTIO_MMIO_CONFIG);
        vmp_set_state(s, VMP_STATE_GET_CONFIG);
        break;
    case VIRTIO_MMIO_SHM_LEN_LOW:
    case VIRTIO_MMIO_SHM_LEN_HIGH:
    case VIRTIO_MMIO_SHM_BASE_LOW:
    case VIRTIO_MMIO_SHM_BASE_HIGH:
        /* All ones means the region doesn't exist.  */
        data = 0xffffffff;
        break;
    default:
        gprintk(XENLOG_DEBUG, "unhandled 0x%x = 0x%x\n", offset, data);
        break;
    }

    s->regs.access_data = data;
}

static void vmp_write64_low(uint64_t *reg, uint32_t val)
{
    uint64_t v = *reg;

    v &= GENMASK_ULL(63, 32);
    v |= val;
    *reg = v;
}

static void vmp_write64_high(uint64_t *reg, uint32_t val)
{
    uint64_t v = *reg;

    v &= GENMASK_ULL(31, 0);
    v |= ((uint64_t) val) << 32;
    *reg = v;
}

static void vmp_indirect_write(struct vmp *s, int size, uint16_t offset,
                               uint32_t data)
{
#if VMP_STATS
    if ( offset < VIRTIO_MMIO_CONFIG )
        s->stats.ind_regs[offset / 4]++;
#endif

    switch ( offset )
    {
    case VIRTIO_MMIO_DEVICE_FEATURES_SEL:
        s->regs.device_features_sel = data;
        break;
    case VIRTIO_MMIO_DRIVER_FEATURES_SEL:
        s->regs.driver_features_sel = data;
        break;
    case VIRTIO_MMIO_DRIVER_FEATURES:
        if ( s->regs.driver_features_sel < ARRAY_SIZE(s->regs.driver_features) )
            s->regs.driver_features[s->regs.driver_features_sel] = data;
        break;
    case VIRTIO_MMIO_INTERRUPT_ACK:
        s->regs.interrupt_status &= ~data;
        vmp_update_interrupts(s);
        break;
    case VIRTIO_MMIO_STATUS:
        s->regs.status = data;

        vmp_indirect_set_busy(s, true);
        if ( data & VIRTIO_CONFIG_S_FEATURES_OK )
        {
            /* Send both messages.  */
            vmp_set_features(s);
            vmp_set_status(s);
            vmp_set_state(s, VMP_STATE_SET_FEATURES);
            s->next_state = VMP_STATE_SET_DEVICE_STATUS;
        }
        else
        {
            vmp_set_status(s);
            vmp_set_state(s, VMP_STATE_SET_DEVICE_STATUS);
        }
        break;
    case VIRTIO_MMIO_QUEUE_SEL:
        if ( data >= VIRTIO_MSG_MAX_QUEUES )
        {
            gprintk(XENLOG_DEBUG, "VIRTIO_MMIO_QUEUE_SEL out of bounds.\n");
            return;
        }

        s->regs.queue_sel = data;

        vmp_indirect_set_busy(s, true);
        vmp_get_vqueue(s);
        vmp_set_state(s, VMP_STATE_GET_VQUEUE);
        break;
    case VIRTIO_MMIO_QUEUE_NUM:
        s->vq.size = data;
        break;
    case VIRTIO_MMIO_QUEUE_DESC_LOW:
        vmp_write64_low(&s->vq.descriptor_addr, data);
        break;
    case VIRTIO_MMIO_QUEUE_DESC_HIGH:
        vmp_write64_high(&s->vq.descriptor_addr, data);
        break;
    case VIRTIO_MMIO_QUEUE_AVAIL_LOW:
        vmp_write64_low(&s->vq.driver_addr, data);
        break;
    case VIRTIO_MMIO_QUEUE_AVAIL_HIGH:
        vmp_write64_high(&s->vq.driver_addr, data);
        break;
    case VIRTIO_MMIO_QUEUE_USED_LOW:
        vmp_write64_low(&s->vq.device_addr, data);
        break;
    case VIRTIO_MMIO_QUEUE_USED_HIGH:
        vmp_write64_high(&s->vq.device_addr, data);
        break;
    case VIRTIO_MMIO_QUEUE_READY:
        vmp_indirect_set_busy(s, true);
        vmp_vq_enable(s, s->regs.queue_sel, data);

        if ( data )
        {
            vmp_set_vqueue(s);
            vmp_set_state(s, VMP_STATE_SET_VQUEUE);
        }
        else
        {
            /* Resetting vqueue's nuke our internal state too.  */
            memset(&s->vq, 0, sizeof s->vq);
            vmp_reset_vqueue(s);
            vmp_set_state(s, VMP_STATE_RESET_VQUEUE);
        }
        break;
    case VIRTIO_MMIO_QUEUE_NOTIFY:
        vmp_event_avail(s, data);
        break;
    case VIRTIO_MMIO_CONFIG ... (VIRTIO_MMIO_CONFIG + 0x100):
        vmp_indirect_set_busy(s, true);
        vmp_set_config(s, size, offset - VIRTIO_MMIO_CONFIG, data);
        vmp_set_state(s, VMP_STATE_SET_CONFIG);
        break;
    default:
        gprintk(XENLOG_DEBUG, "Unhandled indirect write 0x%x = 0x%x\n",
                offset, data);
        break;
    }
}

int vmp_mmio_read(struct vmp *s, uint64_t addr, int size, uint32_t *data)
{
    paddr_t offset = addr - s->base_addr;

    ASSERT(local_irq_is_enabled());

#if VMP_STATS
    spin_lock(&s->lock);
    if ( offset < 0x100 )
        s->stats.regs[offset / 4]++;
    spin_unlock(&s->lock);
#endif

    /* Only 32-bit direct reads are supported */
    if ( size != 4 )
        return 0;

    switch ( offset )
    {
    case VIRTIO_MMIO_MAGIC_VALUE:
        *data = VIRT_MAGIC_NON_BLOCKING;
        break;
    case VIRTIO_MMIO_VERSION:
        *data = VIRT_VERSION;
        break;
    case VIRTIO_MMIO_VENDOR_ID:
        *data = VIRT_VENDOR;
        break;
    case VIRTIO_MMIO_ACCESS:
        *data = s->regs.access;

        /* Bus ops require the lock.  */
        spin_lock(&s->lock);

        /* Signal busy while disconnected or when applying back-pressure  */
        if ( !virtio_msg_bus_ready(s->bd) )
        {
            *data |= VIRTIO_MMIO_ACCESS_BUSY;
        }
        spin_unlock(&s->lock);
        break;
    case VIRTIO_MMIO_ACCESS_DATA:
        /*
         * Make sure loads from regs.access don't get reorderd beyond here.
         * We need to read access before access_data if guest issued them in
         * that order.
         */
        smp_rmb();
        *data = s->regs.access_data;
        break;
    case VIRTIO_MMIO_INTERRUPT_STATUS:
        spin_lock(&s->lock);
        *data = s->regs.interrupt_status;
        spin_unlock(&s->lock);
        break;
    default:
        gprintk(XENLOG_DEBUG, "Bad read from addr=%lx\n", offset);
        return 0;
    }
    return 1;
}

int vmp_mmio_write(struct vmp *s, uint64_t addr, int size, uint32_t data)
{
    paddr_t offset = addr - s->base_addr;
    struct {
        unsigned int offset;
        unsigned int size;
        bool is_write;
    } ind;

    ASSERT(local_irq_is_enabled());

    spin_lock(&s->lock);
#if VMP_STATS
    if ( offset < 0x100 )
        s->stats.regs[offset / 4]++;
#endif

    /* Only 32-bit direct writes are supported */
    if ( size != 4 )
        goto io_abort;

    switch ( offset )
    {
    case VIRTIO_MMIO_MAGIC_VALUE:
        vmp_print_stats(s);
        break;
    case VIRTIO_MMIO_ACCESS:
        if ( s->state != VMP_STATE_IDLE )
        {
            gdprintk(XENLOG_DEBUG, "New access in non idle state!\n");
            goto io_abort;
        }

        if ( (s->regs.access & VIRTIO_MMIO_ACCESS_BUSY) ||
             !virtio_msg_bus_ready(s->bd) )
        {
            gdprintk(XENLOG_DEBUG, "New access while busy!\n");
            goto io_abort;
        }

        if ( virtio_msg_bus_get_state(s->bd) != STATE_CONNECTED )
        {
            gdprintk(XENLOG_DEBUG, "New access while disconnected!\n");
            goto io_abort;
        }

        ind.offset = data & 0xffff;
        ind.size = 1 << ((data >> VIRTIO_MMIO_ACCESS_SIZE_SHIFT) & 3);
        ind.is_write = data & VIRTIO_MMIO_ACCESS_WRITE;

        /*
         * Check that size is 1, 2 or 4. Nothing else is supported.
         *
         * Size cannot be 0 since we're starting by 1 and shifting left.
         * The shift value is 2 bits, can only be between 0 - 3.
         * ind.size can only be a power of 2.
         */
        if ( ind.size > 4 )
        {
            gdprintk(XENLOG_DEBUG, "Bad indirect access size %d!\n", ind.size);
            goto io_abort;
        }

        /* Validate alignment.  */
        if ( ind.offset & (ind.size - 1) )
        {
            gdprintk(XENLOG_DEBUG,
                     "Bad indirect unaligned access! size %d offset %x\n",
                     ind.size, ind.offset);
            goto io_abort;
        }

        /* Validate range.  */
        if ( ind.offset >= VIRTIO_MMIO_REG_SIZE )
        {
            gdprintk(XENLOG_DEBUG, "Out of range indirect access! offset %x\n",
                     ind.offset);
            goto io_abort;
        }

        /*
         * Good access, default regs.access to not busy and no error.
         * Let indirect handlers override it when needed.
         */
        s->regs.access = data;
        vmp_indirect_set_busy(s, false);
        if ( ind.is_write )
            vmp_indirect_write(s, ind.size, ind.offset, s->regs.access_data);
        else
            vmp_indirect_read(s, ind.size, ind.offset);
        break;
    case VIRTIO_MMIO_ACCESS_DATA:
        if ( s->state != VMP_STATE_IDLE )
        {
            gdprintk(XENLOG_DEBUG, "Clobbering access_data while busy\n");
            goto io_abort;
        }
        s->regs.access_data = data;
        break;
    case VIRTIO_MMIO_INTERRUPT_ACK:
        s->regs.interrupt_status &= ~data;
        vmp_update_interrupts(s);
        break;
    default:
        gdprintk(XENLOG_DEBUG, "Write to unknown register! base=%lx %lx = %x\n",
                 s->base_addr, offset, data);
        goto io_abort;
    }

    spin_unlock(&s->lock);
    return 1;
io_abort:
    gdprintk(XENLOG_DEBUG,
             "FATAL: Bad write! state %d size=%d offset %lx data=%x\n",
             s->state, size, offset, data);
    spin_unlock(&s->lock);
    return 0;
}

static void vmp_deinit(void *opaque)
{
    struct vmp *s = opaque;

    vmp_arch_deinit(s);
    xfree(s);
}

static const struct virtio_msg_bus_client vmp_client = {
    .rx_msg = vmp_receive_msg,
    .deinit = vmp_deinit
};

int vmp_init(struct vmp *s, struct virtio_msg_bus *bd, struct domain *d,
             paddr_t base_addr, unsigned int virq)
{
    int rc;

    spin_lock_init(&s->lock);
    s->domain = d;
    s->base_addr = base_addr;
    s->bd = bd;

    rc = vmp_arch_init(s, virq);
    if ( rc )
        goto out;

    virtio_msg_bus_attach_client(bd, &vmp_client, s);

    /* Save after reservation indicating the need to later free it.  */
    s->virq = virq;
    return 0;
out:
    s->domain = NULL;
    return rc;
}
