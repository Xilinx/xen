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

struct mali_vm_data;
struct mali_arb_gsi;

/**
 * struct mali_arb_gsi_sched_ops - scheduler callback interface
 *
 * These callbacks are invoked by the GSI layer (gpu-subinstance.c) to
 * delegate scheduling decisions to the active scheduler backend.
 *
 * Locking contract:
 *
 *   Called WITH gsi_info[].lock held:
 *     sched_start, sched_stop, sched_add_vm, sched_remove_vm,
 *     sched_stop_idle_vm, sched_gpu_active, sched_resync_vm,
 *     sched_get_active_vm
 *
 *   Called WITHOUT gsi_info[].lock:
 *     sched_destroy, sched_get_utilisation, sched_print_stats
 *
 * All callbacks run in atomic context and must not sleep.
 *
 * @sched_start: Start the scheduler. May immediately grant the GPU to
 *     a queued VM.
 *
 * @sched_stop: Stop an active AW on a gpu-subinstance.
 *     If the gpu-subinstance has an access window assigned, we need to
 *     get into a state where it is not using the GPU.
 *     If the gpu-subinstance state machine is:
 *         - RUNNING: Tell the using KBase to stop, and wait for it to
 *           stop or a GPU_LOST state.
 *         - SINGLE_REQ: Same as RUNNING, but there is only one AW
 *           requesting GPU time.
 *         - STOPPING: The using KBase was already stopping, so just
 *           wait for it, or hit a GPU_LOST.
 *         - Anything else: No KBase instance using the GPU, so just do
 *           the reassignment.
 *     Queued VMs receive GPU_LOST. The implementation may temporarily
 *     drop and reacquire gsi_info[].lock (e.g. to cancel synchronous
 *     timers whose callbacks also take this lock). Callers must not
 *     assume atomicity across the call but may assume the lock is held
 *     again on return.
 *
 * @sched_add_vm: Enqueue a VM that is requesting GPU access.
 *
 * @sched_remove_vm: Remove a VM from the scheduler. If @req_again is
 *     set, the VM still wants the GPU and is re-enqueued after yielding
 *     its current timeslice. Returns true if the VM was found.
 *
 * @sched_stop_idle_vm: Preempt an idle VM that is not actively using
 *     the GPU, so the next queued VM can be granted.
 *
 * @sched_gpu_active: Notify the scheduler that a VM has started using
 *     the GPU (optional, may be NULL).
 *
 * @sched_resync_vm: Soft-stop a VM during arbiter restart.
 *
 * @sched_get_active_vm: Return the VM currently assigned to this GSI,
 *     or NULL if none.
 *
 * @sched_get_utilisation: Return busy/total time counters. Read-only,
 *     must not modify scheduler state.
 *
 * @sched_destroy: Release scheduler-private resources. Called once
 *     during GSI teardown after the scheduler has been stopped.
 *
 * @sched_print_stats: Print scheduler state for the debug key handler.
 *     Must be safe to call concurrently with other callbacks.
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
int register_gsi_timeslice_scheduler(struct mali_arb_gsi *gsi,
                                     spinlock_t *gsi_lock);

#endif /* DRIVERS__GPU_MALI_G78AE_GSI_SCHEDULER_IF_H */

/*
 * Local variables:
 * mode: C
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * End:
 */