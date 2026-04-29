/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Mali G78AE GPU PTM GSI Null Scheduler
 *
 * Copyright (C) 2025, Advanced Micro Devices, Inc.
 */
#include "gsi-scheduler-if.h"

struct gsi_scheduler_null {
    struct mali_vm_data *current_vm;
    struct mali_arb_gsi *gsi;
};

static inline int remove_active_vm(struct gsi_scheduler_null *sched_ptr_null)
{
    if ( sched_ptr_null->current_vm == NULL )
    {
        printk(XENLOG_ERR "remove_active_vm: current_vm is NULL\n");
        return -ENOENT;
    }

    if ( ctrlif_unassign_partition(sched_ptr_null->gsi->part_ctrl) )
    {
        printk(XENLOG_ERR "remove_active_vm: "
               "ctrlif_unassign_partition failed\n");
        return -EIO;
    }

    sched_ptr_null->current_vm = NULL;

    return 0;
}

static void gsi_scheduler_null_get_utilisation(void *sched_ptr,
                                               uint32_t *gsi_busytime,
                                               uint32_t *gsi_totaltime)
{
    /*
     * The NULL scheduler does not calculate usage times,
     * as there is no real scheduling involved.
     * A VM has exclusive access to the GPU when assigned.
     */
    *gsi_busytime = 0;
    *gsi_totaltime = 0;
    return;
}

static void gsi_scheduler_null_stop_idle_vm(void *sched_ptr,
                                            struct mali_vm_data *arb_vm)
{
    (void)sched_ptr;
    (void)arb_vm;

    /*
     * The null scheduler has a single VM with exclusive GPU access.
     * When the VM reports idle there is no contention, so there is
     * nothing to preempt. Do not send GPU_STOP; the VM will
     * re-request the GPU when it has new work.
     */
}

static struct mali_vm_data *gsi_scheduler_null_get_active_vm(void *sched_ptr)
{
    return ((struct gsi_scheduler_null *)sched_ptr)->current_vm;
}

static void gsi_scheduler_null_stop(void *sched_ptr)
{
    struct gsi_scheduler_null *sched_ptr_null =
        (struct gsi_scheduler_null *)sched_ptr;

    /* No VM active, nothing to stop */
    if ( sched_ptr_null->current_vm == NULL )
        return;

    if ( mali_gsi_handle_gpu_stop(sched_ptr_null->current_vm) )
    {
        printk(XENLOG_ERR "mali_gsi_handle_gpu_stop failed\n");
        return;
    }
    remove_active_vm(sched_ptr_null);

    return;
}

static void gsi_scheduler_null_start(void *sched_ptr)
{
    /* Nothing to do here for the null scheduler */
    return;
}

static void gsi_scheduler_null_add_vm(void *sched_ptr, struct mali_vm_data *add_vm)
{
    struct gsi_scheduler_null *sched_ptr_null =
        (struct gsi_scheduler_null *)sched_ptr;

    if ( sched_ptr_null->current_vm != NULL &&
         sched_ptr_null->current_vm != add_vm )
    {
        printk(XENLOG_ERR "current_vm is not NULL, cannot add new VM\n");
        return;
    }

    sched_ptr_null->current_vm = add_vm;
    if ( mali_gsi_handle_gpu_granted(sched_ptr_null->gsi, add_vm) )
    {
        printk(XENLOG_ERR "GSI%u: Failed to grant GPU to AW%u\n",
               sched_ptr_null->gsi->idx, add_vm->aw);
        sched_ptr_null->current_vm = NULL;
    }

    return;
}

static bool gsi_scheduler_null_remove_vm(void *sched_ptr,
                    struct mali_vm_data *remove_vm, bool req_again)
{
    struct gsi_scheduler_null *sched_ptr_null =
        (struct gsi_scheduler_null *)sched_ptr;

    if ( sched_ptr_null->current_vm == NULL )
    {
        printk(XENLOG_ERR "current_vm is NULL, cannot remove\n");
        return false;
    }
    if ( sched_ptr_null->current_vm != remove_vm )
    {
        printk(XENLOG_ERR "current_vm != remove_vm\n");
        return false;
    }

    if ( req_again )
    {
        /*
         * If a new request is needed, the VM is re-added to the scheduler.
         * This ensures that the GPU grant process is triggered again for the VM.
         */
        gsi_scheduler_null_add_vm(sched_ptr, remove_vm);
    }
    else
    {
        if ( remove_active_vm(sched_ptr_null) )
        {
            printk(XENLOG_ERR "Failed to remove active VM\n");
            return false;
        }
    }

    return true;
}

static void gsi_scheduler_null_resync_vm(void *sched_ptr,
                                         struct mali_vm_data *vm)
{
    struct gsi_scheduler_null *sched_ptr_null =
        (struct gsi_scheduler_null *)sched_ptr;

    if ( sched_ptr_null->current_vm == vm )
        mali_gsi_handle_gpu_stop(vm);
}

static void gsi_scheduler_print_stats(void *sched_ptr, const char *tab)
{
    struct gsi_scheduler_null *sched_ptr_null =
                                (struct gsi_scheduler_null *)sched_ptr;
    if ( sched_ptr_null->current_vm )
        printk("%sScheduler: null  current=AW%u\n",
               tab, sched_ptr_null->current_vm->aw);
    else
        printk("%sScheduler: null  current=none\n", tab);
}

static const struct mali_arb_gsi_sched_ops gsi_scheduler_null = {
    .sched_get_utilisation = gsi_scheduler_null_get_utilisation,
    .sched_stop_idle_vm    = gsi_scheduler_null_stop_idle_vm,
    .sched_get_active_vm   = gsi_scheduler_null_get_active_vm,
    .sched_stop            = gsi_scheduler_null_stop,
    .sched_start           = gsi_scheduler_null_start,
    .sched_add_vm          = gsi_scheduler_null_add_vm,
    .sched_remove_vm       = gsi_scheduler_null_remove_vm,
    .sched_resync_vm       = gsi_scheduler_null_resync_vm,
    .sched_print_stats     = gsi_scheduler_print_stats,
    .sched_gpu_active      = NULL,
    .sched_destroy         = NULL,
};

int register_gsi_scheduler(struct mali_arb_gsi *gsi)
{
    struct gsi_scheduler_null *sched_ptr_null;

    sched_ptr_null = xzalloc(struct gsi_scheduler_null);
    if ( !sched_ptr_null )
    {
        printk(XENLOG_ERR "Failed to allocate memory for GSI scheduler\n");
        return -ENOMEM;
    }

    sched_ptr_null->gsi = gsi;
    sched_ptr_null->current_vm = NULL;

    gsi->sched_ptr = sched_ptr_null;
    gsi->sched_ops = &gsi_scheduler_null;

    printk(XENLOG_INFO "GSI%u: Using null scheduler\n", gsi->idx);
    return 0;
}

/*
 * Local variables:
 * mode: C
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * End:
 */