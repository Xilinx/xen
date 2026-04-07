/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Mali G78AE GPU PTM GSI Scheduler Interface
 *
 * Copyright (C) 2025, Advanced Micro Devices, Inc.
 */
#ifndef DRIVERS__GPU_MALI_G78AE_GSI_SCHEDULER_IF_H
#define DRIVERS__GPU_MALI_G78AE_GSI_SCHEDULER_IF_H

#include <xen/types.h>

#include "arbiter.h"
#include "gpu-subinstance.h"

struct mali_vm_data;
struct mali_arb_gsi;

/**
 * struct mali_arb_gsi_sched_ops - scheduler operations to schedule the VM
 * requests, these are invoked from the gpu-subinstance to schedule the GPU
 * requests on that gpu-subinstance from the VMs.
 *
 * @sched_get_utilisation: Get gpu-subinstance utilization info
 *     sched_ptr - Pointer to the Scheduler.
 *     gsi_busytime - Out-param that will contain the gpu-subinstance busy time.
 *     gsi_totaltime - Out-param that will contain the gpu-subinstance total
 *         time.
 *     This function provides the GPU busytime and totaltime since last request.
 *
 * @sched_stop_idle_vm: Stop an idle VM.
 *     sched_ptr - Pointer to the Scheduler.
 *     arb_vm - VM to be stopped.
 *     This function sends stop to an idle VM.
 *
 * @sched_get_active_vm: Get the active VM.
 *     sched_ptr - Pointer to the Scheduler.
 *     This function returns the VM which is currently assigned to the GSI.
 *
 * @sched_stop: Stop an active AW on a gpu-subinstance.
 *     sched_ptr - Pointer to the Scheduler.
 *     If the gpu-subinstance has an access window assigned, we need to get into
 *     a state where it is not using the GPU.
 *     If the gpu-subinstance state machine is:
 *         - state_RUNNING: Tell the using KBase to stop, and wait for it to
 *           stop or a GPU_LOST state.
 *         - state_SINGLE_REQ: Same as state_RUNNING, but there is only
 *           AW requesting GPU time.
 *         - STOPPING: The using KBase was already stopping, so just
 *           wait for it, or hit a GPU_LOST.
 *         - Anything else: No KBase instance using the GPU, so just do the
 *           reassignment.
 *
 * @sched_start: Start the scheduler
 *     sched_ptr - Pointer to the Scheduler.
 *     This function starts scheduling the VM requests on the gpu subinstance.
 *
 * @sched_add_vm: Add new VM to the scheduler.
 *     sched_ptr - Pointer to the Scheduler.
 *     add_vm - Pointer to the requested VM's private data.
 *     This function adds a new VM request to the existing requests so it can be
 *     scheduled by the scheduler.
 *
 * @sched_remove_vm: Remove a VM from the scheduler.
 *     sched_ptr - Pointer to the Scheduler.
 *     rem_vm - Pointer to the VM's private data to be removed.
 *     req_again - Flag indicates VM has work pending and still wants the GPU;
 *         if set, the VM is not deleted from the scheduler.
 *     This function un-assigns the VM from the GSI if currently assigned and
 *     deletes it from the scheduler.
 *
 * @sched_resync_vm: Resync VM after the arbiter restart.
 *     sched_ptr - Pointer to the Scheduler.
 *     rem_vm - Pointer to the VM's private data to be removed.
 *     This function soft stops the VM when there is an arbiter restart.
 * @sched_print_stats: Print scheduler stats.
 *     sched_ptr - Pointer to the Scheduler.
 *     tab - Tab string to be used for indentation.
 */
struct mali_arb_gsi_sched_ops {
    void (*sched_get_utilisation)
        (void *sched_ptr, uint32_t *gsi_busytime, uint32_t *gsi_totaltime);
    void (*sched_stop_idle_vm)(void *sched_ptr, struct mali_vm_data *arb_vm);
    struct mali_vm_data *(*sched_get_active_vm)(void *sched_ptr);
    void (*sched_stop)(void *sched_ptr);
    void (*sched_start)(void *sched_ptr);
    void (*sched_add_vm)(void *sched_ptr, struct mali_vm_data *add_vm);
    bool (*sched_remove_vm)
        (void *sched_ptr, struct mali_vm_data *rem_vm, bool req_again);
    void (*sched_resync_vm)(void *sched_ptr, struct mali_vm_data *rem_vm);
    void (*sched_print_stats)(void *sched_ptr, const char *tab);
    void (*sched_gpu_active)(void *sched_ptr, struct mali_vm_data *arb_vm);
    void (*sched_destroy)(void *sched_ptr);
};

int register_gsi_scheduler(struct mali_arb_gsi *gsi);
#endif /* DRIVERS__GPU_MALI_G78AE_GSI_SCHEDULER_IF_H */

/*
 * Local variables:
 * mode: C
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * End:
 */