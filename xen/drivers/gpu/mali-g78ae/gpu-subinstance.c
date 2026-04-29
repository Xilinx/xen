/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Mali G78AE GPU PTM GPU Subinstance (GSI) handling
 *
 * Copyright (C) 2025, Advanced Micro Devices, Inc.
 */
#include <xen/lib.h>
#include <xen/param.h>

#include "gpu-subinstance.h"
#include "gsi-scheduler-if.h"
#include "arbiter.h"
#include "arb-vm-protocol.h"
#include "ptm-msg.h"

/*
 * Global scheduler selection.
 *
 * Boot parameter: mali_sched=<name>
 * Accepted values: "null", "ts"
 *
 * When not specified, the compile-time default from Kconfig is used
 * (CONFIG_MALI_GSI_SCHED_DEFAULT).  All partitions use the same scheduler.
 */
static char __initdata opt_mali_sched[16] = CONFIG_MALI_GSI_SCHED_DEFAULT;
string_param("mali_sched", opt_mali_sched);

enum mali_gsi_sched_type __init mali_gsi_get_sched_type(void)
{
    if ( !opt_mali_sched[0] )
        goto use_default;

    if ( !strcmp(opt_mali_sched, "null") )
        return MALI_GSI_SCHED_NULL;

    if ( !strcmp(opt_mali_sched, "ts") )
        return MALI_GSI_SCHED_TIMESLICE;

    printk(XENLOG_ERR
           "mali_sched: Unknown scheduler '%s', using default\n",
           opt_mali_sched);

use_default:
    if ( !strcmp(CONFIG_MALI_GSI_SCHED_DEFAULT, "ts") )
        return MALI_GSI_SCHED_TIMESLICE;

    return MALI_GSI_SCHED_NULL;
}

int __init mali_gsi_create(struct mali_arb_gsi **gsi, unsigned int idx,
                    struct mali_arbiter *arbiter,
                    spinlock_t *gsi_lock)
{
    struct mali_arb_gsi *gsi_instance;
    enum mali_gsi_sched_type sched_type = mali_gsi_get_sched_type();
    int err;

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
        printk(XENLOG_ERR "GSI%u: Failed to get part_cfg or part_ctrl\n", idx);
        xvfree(gsi_instance);
        return -EINVAL;
    }

    switch ( sched_type )
    {
#ifdef CONFIG_MALI_GSI_SCHED_TIMESLICE
    case MALI_GSI_SCHED_TIMESLICE:
        err = register_gsi_timeslice_scheduler(gsi_instance, gsi_lock);
        if ( err )
        {
            xvfree(gsi_instance);
            return err;
        }
        break;
#endif
#ifdef CONFIG_MALI_GSI_SCHED_NULL
    case MALI_GSI_SCHED_NULL:
        err = register_gsi_scheduler(gsi_instance);
        if ( err )
        {
            xvfree(gsi_instance);
            return err;
        }
        break;
#endif
    default:
        printk(XENLOG_ERR "GSI%u: Unsupported scheduler type %d "
               "(check Kconfig and mali_sched= parameter)\n",
               idx, sched_type);
        xvfree(gsi_instance);
        return -EINVAL;
    }

    *gsi = gsi_instance;
    return 0;
}

void mali_gsi_destroy(struct mali_arb_gsi *gsi)
{
    if ( !gsi )
        return;

    /* Ensure the GSI is stopped before destroying the scheduler */
    if ( gsi->sched_ops && gsi->sched_ops->sched_stop )
    {
        spin_lock(&gsi->arbiter->gsi_info[gsi->idx].lock);
        if ( gsi->state == STARTED )
        {
            gsi->sched_ops->sched_stop(gsi->sched_ptr);
            gsi->state = STOPPED;
        }
        spin_unlock(&gsi->arbiter->gsi_info[gsi->idx].lock);
    }

    if ( gsi->sched_ops && gsi->sched_ops->sched_destroy )
        gsi->sched_ops->sched_destroy(gsi->sched_ptr);

    xfree(gsi->sched_ptr);
    gsi->sched_ptr = NULL;
    gsi->sched_ops = NULL;
    xvfree(gsi);
}

/* Must be called with gsi_info[gsi->idx].lock held */
void mali_gsi_start(struct mali_arb_gsi *gsi)
{
    if ( gsi->state != STOPPED )
    {
        printk(XENLOG_ERR "GSI%u: invalid state %d in start\n",
               gsi->idx, gsi->state);
        return;
    }
    if ( !gsi->sched_ptr || !gsi->sched_ops )
    {
        printk(XENLOG_ERR "GSI%u: no scheduler defined\n", gsi->idx);
        return;
    }

    /*
     * Set STARTED before sched_start() because the scheduler may
     * immediately grant the GPU to a queued VM, which calls
     * mali_gsi_handle_gpu_granted() that requires state == STARTED.
     */
    gsi->state = STARTED;
    gsi->sched_ops->sched_start(gsi->sched_ptr);
}

/* Must be called with gsi_info[gsi->idx].lock held */
void mali_gsi_stop(struct mali_arb_gsi *gsi)
{
    if ( gsi->state != STARTED )
    {
        printk(XENLOG_ERR "GSI%u: invalid state %d in stop\n",
               gsi->idx, gsi->state);
        return;
    }
    if ( !gsi->sched_ptr || !gsi->sched_ops )
    {
        printk(XENLOG_ERR "GSI%u: no scheduler defined\n", gsi->idx);
        return;
    }

    gsi->state = STOPPING;
    gsi->sched_ops->sched_stop(gsi->sched_ptr);
    gsi->state = STOPPED;
}

void mali_gsi_get_utilisation(struct mali_arb_gsi *gsi,
                              uint32_t *gsi_busytime, uint32_t *gsi_totaltime)
{
    if ( !gsi_busytime || !gsi_totaltime )
        return;

    if ( gsi->sched_ops && gsi->sched_ops->sched_get_utilisation )
        gsi->sched_ops->sched_get_utilisation(gsi->sched_ptr,
                                               gsi_busytime, gsi_totaltime);
    else
    {
        *gsi_busytime = 0;
        *gsi_totaltime = 0;
    }
}

void mali_gsi_update_freq(struct mali_arb_gsi *gsi,
                          uint32_t new_freq)
{
    /* TODO: frequency scaling not yet implemented */
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
    if ( !gsi || !gsi->sched_ops || !gsi->sched_ops->sched_remove_vm )
        return false;

    return gsi->sched_ops->sched_remove_vm(gsi->sched_ptr, rem_vm, false);
}

void mali_gsi_on_gpu_stopped(struct mali_vm_data *arb_vm,
                             struct mali_arb_gsi *gsi, bool req_again)
{
    if ( !gsi->sched_ops || !gsi->sched_ops->sched_remove_vm )
    {
        printk(XENLOG_ERR "GSI%u: sched_ops not set in on_gpu_stopped\n",
               gsi->idx);
        return;
    }

    gsi->sched_ops->sched_remove_vm(gsi->sched_ptr, arb_vm, req_again);
}

void mali_gsi_on_gpu_request(struct mali_vm_data *arb_vm,
                             struct mali_arb_gsi *gsi)
{
    if ( !gsi->sched_ops || !gsi->sched_ops->sched_add_vm )
    {
        printk(XENLOG_ERR "GSI%u: sched_ops not set in on_gpu_request\n",
               gsi->idx);
        return;
    }

    gsi->sched_ops->sched_add_vm(gsi->sched_ptr, arb_vm);
}

void mali_gsi_on_gpu_active(struct mali_vm_data *arb_vm,
                            struct mali_arb_gsi *gsi)
{
    if ( !gsi->sched_ops )
        return;

    if ( gsi->sched_ops->sched_gpu_active )
        gsi->sched_ops->sched_gpu_active(gsi->sched_ptr, arb_vm);
}

void mali_gsi_on_gpu_idle(struct mali_vm_data *arb_vm,
                          struct mali_arb_gsi *gsi)
{
    if ( !gsi->sched_ops || !gsi->sched_ops->sched_stop_idle_vm )
    {
        printk(XENLOG_ERR "GSI%u: sched_ops not set in on_gpu_idle\n",
               gsi->idx);
        return;
    }

    gsi->sched_ops->sched_stop_idle_vm(gsi->sched_ptr, arb_vm);
}

int mali_gsi_handle_gpu_stop(struct mali_vm_data *arb_vm)
{
    return mali_arbif_gpu_stop(arb_vm);
}

int mali_gsi_handle_gpu_granted(struct mali_arb_gsi *gsi,
                            struct mali_vm_data *arb_vm)
{
    uint64_t message = 0;
    int ret;

    if ( !arb_vm )
    {
        printk(XENLOG_ERR "GSI%u: Cannot grant GPU, NULL AW context\n", gsi->idx);
        return -EINVAL;
    }

    if ( gsi->state != STARTED )
    {
        printk(XENLOG_ERR "GSI%u: Cannot grant GPU to AW%u, GSI not started\n",
               gsi->idx, arb_vm->aw);
        return -EINVAL;
    }

    if ( gsi->flags & (1U << GSI_FLAG_SLICE_ASSIGNED) )
    {
        int assign_ret = ctrlif_assign_partition_to_aw(gsi->part_ctrl,
                                                       arb_vm->aw);
        if ( assign_ret )
        {
            printk(XENLOG_ERR
                   "GSI%u: partition assign to AW%u failed (%d)\n",
                   gsi->idx, arb_vm->aw, assign_ret);
            return -EIO;
        }

    }
    else
    {
        printk(XENLOG_ERR "GSI%u: Cannot grant GPU to AW%u, no slice assigned\n",
               gsi->idx, arb_vm->aw);
        return -EINVAL;
    }

    /*
     * Try the normal send path first.  If it succeeds (channel free),
     * the message is delivered immediately.  If the channel is busy
     * (stale message from a previous timeslice sitting in the HW
     * register), fall back to force-send which flushes the software
     * buffer and overwrites the hardware register directly.
     */
    ret = mali_arbif_gpu_granted(arb_vm, GSI_DEFAULT_FREQ);
    if ( ret )
    {
        ret = arb_vm_gpu_granted_build_msg(GSI_DEFAULT_FREQ, &message);
        if ( ret )
            return ret;

        ptm_msg_send_force(&gsi->arbiter->rg->msg_handler,
                           arb_vm->aw, &message);
    }
    return 0;
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
