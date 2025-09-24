/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Mali G78AE GPU PTM GPU Subinstance (GSI) handling
 *
 * Copyright (C) 2025, Advanced Micro Devices, Inc.
 */
#include "gpu-subinstance.h"

int mali_gsi_create(struct mali_arb_gsi **gsi, unsigned int idx,
                  struct mali_arbiter *arbiter)
{
    struct mali_arb_gsi *gsi_instance;

    gsi_instance = xvzalloc(struct mali_arb_gsi);
    if ( !gsi_instance )
        return -ENOMEM;

    gsi_instance->arbiter = arbiter;
    gsi_instance->idx = idx;
    gsi_instance->flags = 0;
    gsi_instance->state = STARTED;
    gsi_instance->part_cfg = arbiter->rg->cfg[idx];
    gsi_instance->part_ctrl = arbiter->rg->ctrl[idx];

    if ( gsi_instance->part_cfg == NULL || gsi_instance->part_ctrl == NULL )
    {
        printk(XENLOG_ERR "GSI%d: Failed to get part_cfg or part_ctrl\n", idx);
        xvfree(gsi_instance);
        return -EINVAL;
    }

    register_gsi_scheduler(gsi_instance);
    *gsi = gsi_instance;
    return 0;
}

void mali_gsi_destroy(struct mali_arb_gsi *gsi)
{
    if ( !gsi )
        return;

    xvfree(gsi);
    return;
}

void mali_gsi_start(struct mali_arb_gsi *gsi)
{
    if ( gsi->state != STOPPED )
    {
        printk(XENLOG_ERR "GPU subinstance is in invalid state %d", gsi->state);
        return;
    }
    if ( !gsi->sched_ptr ) {
        printk(XENLOG_ERR "No scheduler defined for GSI %d", gsi->idx);
        return;
    }

    gsi->state = STARTING;
    gsi->sched_ops->sched_start(gsi->sched_ptr);
    gsi->state = STARTED;

    return;
}

void mali_gsi_stop(struct mali_arb_gsi *gsi)
{
    if ( gsi->state != STARTED )
    {
        printk(XENLOG_ERR "GPU subinstance is in invalid state %d", gsi->state);
        return;
    }

    gsi->state = STOPPING;
    gsi->sched_ops->sched_stop(gsi->sched_ptr);
    gsi->state = STOPPED;

    return;
}

void mali_gsi_get_utilisation(struct mali_arb_gsi *gsi,
                              uint32_t *gsi_busytime, uint32_t *gsi_totaltime)
{
    if ( gsi_busytime )
        *gsi_busytime = 0;
    if ( gsi_totaltime )
        *gsi_totaltime = 0;

    return;
}

void mali_gsi_update_freq(struct mali_arb_gsi *gsi,
                          uint32_t new_freq)
{
    printk(XENLOG_WARNING "Unimplemented: mali_gsi_update_freq called\n");
    return;
}

void mali_gsi_flag_set(struct mali_arb_gsi *gsi,
                       enum mali_arb_flags flag)
{
    if ( flag >= GSI_FLAG_MAX )
        return;

    gsi->flags |= (1U << flag);

    return;
}

void mali_gsi_flag_clear(struct mali_arb_gsi *gsi,
                         enum mali_arb_flags flag)
{
    if ( flag >= GSI_FLAG_MAX )
        return;

    gsi->flags &= ~(1U << flag);

    return;
}

bool mali_arbiter_gsi_remove_vm(struct mali_arb_gsi *gsi,
                                struct mali_vm_data *rem_vm)
{
    return false;
}

void mali_gsi_on_gpu_stopped(struct mali_vm_data *arb_vm,
                             struct mali_arb_gsi *gsi, bool req_again)
{
    gsi->sched_ops->sched_remove_vm(gsi->sched_ptr, arb_vm, req_again);

    return;
}

void mali_gsi_on_gpu_request(struct mali_vm_data *arb_vm,
                             struct mali_arb_gsi *gsi)
{
    gsi->sched_ops->sched_add_vm(gsi->sched_ptr, arb_vm);

    return;
}

void mali_gsi_on_gpu_active(struct mali_vm_data *arb_vm,
                            struct mali_arb_gsi *gsi)
{
    return;
}

void mali_gsi_on_gpu_idle(struct mali_vm_data *arb_vm,
                          struct mali_arb_gsi *gsi)
{
    gsi->sched_ops->sched_stop_idle_vm(gsi->sched_ptr, arb_vm);

    return;
}

int mali_gsi_handle_gpu_stop(struct mali_vm_data *arb_vm)
{
    return mali_arbif_gpu_stop(arb_vm);
}

int mali_gsi_handle_gpu_granted(struct mali_arb_gsi *gsi,
                            struct mali_vm_data *arb_vm)
{
    if ( gsi->state != STARTED )
    {
        printk(XENLOG_ERR "GSI %d: Cannot grant GPU to VM %d, GSI not started\n",
               gsi->idx, arb_vm->aw);
        return -EINVAL;
    }

    if ( !arb_vm )
    {
        printk(XENLOG_ERR "GSI %d: Cannot grant GPU to NULL VM\n", gsi->idx);
        return -EINVAL;
    }

    if ( gsi->flags & (1U << GSI_FLAG_SLICE_ASSIGNED) )
        ctrlif_assign_partition_to_aw(gsi->part_ctrl, arb_vm->aw);
    else
    {
        printk(XENLOG_ERR "GSI %d: Cannot grant GPU to VM %d, no slice assigned\n",
               gsi->idx, arb_vm->aw);
        return -EINVAL;
    }

    /*
     * Frequency calculation not implemented yet.
     * Granting access with default frequency.
     */
    return mali_arbif_gpu_granted(arb_vm, GSI_DEFAULT_FREQ);
}

int mali_gsi_handle_gpu_lost(struct mali_vm_data *arb_vm)
{
    return mali_arbif_gpu_lost(arb_vm);
}

/*
 * Local variables:
 * mode: C
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * End:
 */
