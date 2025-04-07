/*
 * virtio-msg-bus core.
 *
 * Copyright (c) 2025 Advanced Micro Devices, Inc.
 * Written by Edgar E. Iglesias <edgar.iglesias@amd.com>
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include <xen/dm.h>
#include <xen/guest_access.h>
#include <xen/hypercall.h>
#include <xen/virtio/virtio-msg-bus.h>
#include <xen/virtio/virtio-msg-bus-xen.h>

int virtio_msg_bus_dm_op(struct xen_dm_op *op, struct domain *d, bool *const_op)
{
    struct xen_dm_op_virtio_msg_bus *data = &op->u.virtio_msg_bus;
    int rc = 0;

    /*
     * const_op == true  - No need to copy back *op to guest memory (default).
     * const_op == false - Modified *op and we need to copy it back.
     */
    *const_op = true;

    switch ( data->op )
    {
#ifdef CONFIG_VIRTIO_MSG_BUS_XEN
    case XEN_DMOP_VIRTIO_MSG_BUS_XEN_CONNECT:
        rc = virtio_msg_bus_xen_dm_connect(d, data->bus_id,
                                           data->u.xen.shm_fifo_gfn,
                                           &data->u.xen.port);
        *const_op = false;
        break;
#endif
    default:
        return -EINVAL;
    }
    return rc;
}
