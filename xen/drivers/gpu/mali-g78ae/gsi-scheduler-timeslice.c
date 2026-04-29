/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Mali G78AE GPU PTM GSI Time-Slice Scheduler
 *
 * Copyright (C) 2026, Advanced Micro Devices, Inc.
 */

/*
 * Design
 * ======
 *
 * The timeslice scheduler multiplexes a single GPU partition (GSI) across
 * multiple VMs using cooperative time-sharing with forced preemption as a
 * fallback.
 *
 * Scheduling policy
 * -----------------
 *
 * VMs that request GPU access are placed in a FIFO queue (vm_req_list).
 * The first VM to request an idle partition gets it immediately and
 * becomes the "current VM" (current_vm). As long as only one VM wants
 * the GPU, no timers are armed and the VM holds the partition
 * indefinitely, so there is zero overhead in the single-VM case.
 *
 * When a second VM requests GPU access the scheduler enters the
 * time-slicing rotation:
 *
 *   1. A request timer fires after the quantum expires (default 7 ms).
 *   2. The scheduler sends GPU_STOP to the current VM.
 *   3. A yield timer starts (default 100 ms) as a safety net.
 *   4. The VM responds with GPU_STOPPED and is moved to the back of
 *      the queue. The next VM in the queue gets GPU_GRANTED.
 *   5. If the VM does not respond before the yield timer fires, the
 *      scheduler sends GPU_LOST and forcibly grants the partition to
 *      the next VM. The evicted VM must go through kbase recovery
 *      before it can request GPU access again.
 *
 * If a VM re-requests GPU access while it is being stopped
 * (GPU_STOPPING state), it is added to the back of the queue so it
 * will be rescheduled after it yields.
 *
 * State machine
 * -------------
 *
 *   TS_NO_REQ      - No VM wants the GPU. Idle.
 *   TS_SINGLE_REQ  - Exactly one VM holds the GPU. No timers running.
 *   TS_RUNNING     - Multiple VMs competing. Request timer armed.
 *   TS_GPU_STOPPING- GPU_STOP sent, waiting for GPU_STOPPED or yield
 *                    timeout. Yield timer armed.
 *   TS_STOPPED     - Scheduler stopped (GSI teardown / reconfiguration).
 *
 * Transitions:
 *
 *   NO_REQ -----> SINGLE_REQ      first VM requests GPU and gets it
 *   SINGLE_REQ -> RUNNING         second VM requests GPU, request timer armed
 *   SINGLE_REQ -> NO_REQ          current VM removed (domain destroyed)
 *   RUNNING ----> GPU_STOPPING    request timer fires, GPU_STOP sent
 *   RUNNING ----> SINGLE_REQ     a queued VM removed, only one left
 *   RUNNING ----> NO_REQ          all VMs removed
 *   GPU_STOPPING > SINGLE_REQ     VM yields (GPU_STOPPED), next VM granted,
 *                                  no others in queue
 *   GPU_STOPPING > RUNNING        VM yields, next VM granted, queue not empty
 *   GPU_STOPPING > NO_REQ         VM yields or evicted, queue empty
 *   any ---------> STOPPED        sched_stop called (GSI teardown)
 *
 * Locking
 * -------
 *
 * All scheduler callbacks are invoked with gsi_info[].lock held,
 * except sched_destroy and sched_get_utilisation. The two timer
 * callbacks (request_timer_fn, yield_timer_fn) acquire gsi_lock
 * themselves.
 *
 * sched_stop temporarily drops gsi_lock to call kill_timer(), which
 * waits for any in-flight timer callback to complete. During this
 * window the state is TS_STOPPED, so concurrent sched_add_vm calls
 * only enqueue without granting.
 *
 * Utilization tracking
 * --------------------
 *
 * GSI-level busy time and total time are tracked in nanoseconds and
 * reported in milliseconds through the debug dump (key '7').
 *
 * Boot parameters
 * ---------------
 *
 *   mali_ts_quantum_ms  - Per-VM time slice in ms (default 7, range 1-1000)
 *   mali_ts_yield_ms    - Yield timeout in ms (default 100, range 1-10000)
 */

#include <xen/init.h>
#include <xen/param.h>
#include <xen/timer.h>

#include "gsi-scheduler-if.h"
#include "ptm-msg.h"

#define TS_QUANTUM_MS_DEFAULT  7
#define TS_QUANTUM_MS_MIN      1
#define TS_QUANTUM_MS_MAX      1000

#define TS_YIELD_MS_DEFAULT    100
#define TS_YIELD_MS_MIN        1
#define TS_YIELD_MS_MAX        10000

static unsigned int __initdata mali_ts_quantum_ms = TS_QUANTUM_MS_DEFAULT;
integer_param("mali_ts_quantum_ms", mali_ts_quantum_ms);

static unsigned int __initdata mali_ts_yield_ms = TS_YIELD_MS_DEFAULT;
integer_param("mali_ts_yield_ms", mali_ts_yield_ms);

enum ts_sched_state {
    TS_NO_REQ,
    TS_SINGLE_REQ,
    TS_RUNNING,
    TS_GPU_STOPPING,
    TS_STOPPED,
};

static const char *ts_state_name[] = {
    [TS_NO_REQ]       = "NO_REQ",
    [TS_SINGLE_REQ]   = "SINGLE_REQ",
    [TS_RUNNING]      = "RUNNING",
    [TS_GPU_STOPPING] = "GPU_STOPPING",
    [TS_STOPPED]      = "STOPPED",
};

struct gsi_scheduler_ts {
    struct mali_arb_gsi *gsi;
    struct list_head vm_req_list;
    struct mali_vm_data *current_vm;
    /*
     * Invariant: req_count == (current_vm ? 1 : 0) + len(vm_req_list).
     *
     * A VM that re-requests during GPU_STOPPING is added to vm_req_list
     * while still being current_vm, so it is counted TWICE (once for
     * current, once for queue).yield_timer_fn and ts_sched_remove_vm
     * handle this by decrementing once per slot the VM occupies.
     */
    unsigned int req_count;

    spinlock_t *gsi_lock;

    enum ts_sched_state state;

    struct timer request_timer;
    struct timer yield_timer;

    s_time_t request_timeout_ns;
    s_time_t yield_timeout_ns;

    /* Utilization tracking */
    s_time_t busy_start;
    s_time_t total_busy;
    s_time_t total_time;
    s_time_t last_grant_time;
};

static bool grant_gpu_to_vm(struct gsi_scheduler_ts *sched,
                             struct mali_vm_data *vm)
{
    s_time_t now = NOW();
    int err;

    err = mali_gsi_handle_gpu_granted(sched->gsi, vm);
    if ( err )
    {
        printk(XENLOG_ERR "GSI%u: grant to AW%u failed (err=%d)\n",
               sched->gsi->idx, vm->aw, err);
        return false;
    }

    sched->current_vm = vm;
    sched->last_grant_time = now;

    return true;
}

static void update_busy_time(struct gsi_scheduler_ts *sched)
{
    if ( sched->busy_start )
    {
        sched->total_busy += NOW() - sched->busy_start;
        sched->busy_start = 0;
    }
}

static void update_total_time(struct gsi_scheduler_ts *sched)
{
    if ( sched->last_grant_time )
    {
        sched->total_time += NOW() - sched->last_grant_time;
        sched->last_grant_time = 0;
    }
}

/* Flush GSI-level stats for the current VM */
static void flush_current_vm_stats(struct gsi_scheduler_ts *sched)
{
    update_busy_time(sched);
    update_total_time(sched);
}

/*
 * Try to grant the GPU to the next queued VM. Loops through the
 * queue until a grant succeeds or the queue is empty. VMs whose
 * grant fails receive GPU_LOST so they can enter recovery.
 */
static void try_grant_next_vm(struct gsi_scheduler_ts *sched)
{
    struct mali_vm_data *next_vm;

    while ( !list_empty(&sched->vm_req_list) )
    {
        next_vm = list_first_entry(&sched->vm_req_list,
                                   struct mali_vm_data, sched_entry);
        list_del_init(&next_vm->sched_entry);

        if ( grant_gpu_to_vm(sched, next_vm) )
            return;

        mali_gsi_handle_gpu_lost(next_vm);
        next_vm->gpu_lost = true;
        if ( sched->req_count > 0 )
            sched->req_count--;
    }
}

/*
 * Transition the scheduler state based on req_count.
 * Arms or disarms the request timer as appropriate.
 *
 * Must not overwrite TS_GPU_STOPPING: that transient state is resolved
 * by the yield timer or by remove_vm(current_vm). A non-current VM
 * removal that changes req_count must not disrupt the in-progress stop.
 */
static void update_sched_state(struct gsi_scheduler_ts *sched)
{
    if ( sched->state == TS_GPU_STOPPING )
        return;

    if ( sched->req_count == 0 )
    {
        stop_timer(&sched->request_timer);
        sched->state = TS_NO_REQ;
    }
    else if ( sched->req_count == 1 )
    {
        stop_timer(&sched->request_timer);
        sched->state = TS_SINGLE_REQ;
    }
    else
    {
        sched->state = TS_RUNNING;
        /*
         * Do not arm the request timer here.  After a fresh grant
         * the VM has not yet processed GPU_GRANTED; starting the
         * quantum immediately would expire before the VM can even
         * begin using the GPU, leading to spurious GPU_LOST evictions.
         *
         * ts_sched_gpu_active() arms the timer when the VM actually
         * starts GPU work.  As a safety net, arm with yield_timeout
         * so a completely unresponsive VM still gets preempted.
         */
        set_timer(&sched->request_timer,
                  NOW() + sched->yield_timeout_ns);
    }
}

static void request_timer_fn(void *data)
{
    struct gsi_scheduler_ts *sched = data;
    int err;

    spin_lock(sched->gsi_lock);

    if ( sched->state != TS_RUNNING || !sched->current_vm )
    {
        spin_unlock(sched->gsi_lock);
        return;
    }

    err = mali_gsi_handle_gpu_stop(sched->current_vm);
    if ( err )
        printk(XENLOG_ERR "GSI%u: GPU_STOP to AW%u failed (err=%d)\n",
               sched->gsi->idx, sched->current_vm->aw, err);

    sched->state = TS_GPU_STOPPING;
    set_timer(&sched->yield_timer, NOW() + sched->yield_timeout_ns);

    spin_unlock(sched->gsi_lock);
}

static void yield_timer_fn(void *data)
{
    struct gsi_scheduler_ts *sched = data;

    spin_lock(sched->gsi_lock);

    if ( sched->state != TS_GPU_STOPPING || !sched->current_vm )
    {
        spin_unlock(sched->gsi_lock);
        return;
    }

    /* VM did not respond to GPU_STOP in time, force evict */
    {
        struct ptm_msg_handler *mh = &sched->gsi->arbiter->rg->msg_handler;
        uint32_t aw = sched->current_vm->aw;
        uint32_t status = readl(mh->base_addr +
                                PTM_MESSAGE_OFFSET(aw) +
                                PTM_OUTGOING_MESSAGE_STATUS);

        printk(XENLOG_WARNING
               "GSI%u: AW%u did not stop in time (PTM outgoing_status=0x%x), "
               "sending GPU_LOST\n",
               sched->gsi->idx, aw, status);
    }

    flush_current_vm_stats(sched);

    /*
     * Unassign the partition BEFORE sending GPU_LOST.  This resets
     * the GPU hardware and aborts any in-flight transactions,
     * preventing the guest driver's GPU_LOST recovery code from
     * issuing new accesses through a still-assigned partition
     * (which would trigger IRQ_AWx_INVALID_ACCESS).
     */
    if ( ctrlif_unassign_partition(sched->gsi->part_ctrl) )
        printk(XENLOG_ERR "GSI%u: ctrlif_unassign_partition failed\n",
               sched->gsi->idx);

    mali_gsi_handle_gpu_lost(sched->current_vm);
    sched->current_vm->gpu_lost = true;

    /*
     * Drop the timed-out VM. If it re-requested during GPU_STOPPING,
     * remove it from the queue too since GPU_LOST invalidates it.
     */
    if ( !list_empty(&sched->current_vm->sched_entry) )
    {
        list_del_init(&sched->current_vm->sched_entry);
        if ( sched->req_count > 0 )
            sched->req_count--;
    }
    sched->current_vm = NULL;
    if ( sched->req_count > 0 )
        sched->req_count--;
    else
        printk(XENLOG_ERR "GSI%u: req_count underflow in yield_timer_fn\n",
               sched->gsi->idx);

    /* GPU_STOPPING is resolved — clear it so update_sched_state works */
    sched->state = TS_NO_REQ;
    try_grant_next_vm(sched);
    update_sched_state(sched);

    spin_unlock(sched->gsi_lock);
}

static bool ts_sched_remove_vm(void *sched_ptr, struct mali_vm_data *rem_vm,
                               bool req_again);

static void ts_sched_add_vm(void *sched_ptr, struct mali_vm_data *add_vm)
{
    struct gsi_scheduler_ts *sched = sched_ptr;
    struct mali_vm_data *vm;

    /* The VM is requesting GPU access; clear any stale gpu_lost flag */
    add_vm->gpu_lost = false;

    /*
     * Duplicate check: if the VM is already queued in the wait list,
     * it is normally a harmless duplicate that can be ignored.
     *
     * Exception: if the VM is also current_vm during GPU_STOPPING,
     * it had already re-requested and is now sending another
     * GPU_REQUEST without having sent GPU_STOPPED first (the
     * GPU_STOPPED may have been lost in transit).  Treat this as
     * an implicit GPU_STOPPED with req_again=true so the scheduler
     * proceeds immediately instead of waiting for the yield timer.
     */
    list_for_each_entry(vm, &sched->vm_req_list, sched_entry)
    {
        if ( vm == add_vm )
        {
            if ( sched->current_vm == add_vm &&
                 sched->state == TS_GPU_STOPPING )
                ts_sched_remove_vm(sched, add_vm, true);
            return;
        }
    }

    /*
     * If the VM is current_vm AND we are in GPU_STOPPING (we sent
     * GPU_STOP, waiting for GPU_STOPPED), the VM is expressing intent
     * to continue using the GPU. Queue it so it gets rescheduled
     * after it yields. In all other states current_vm already holds
     * the GPU and the request is a harmless duplicate.
     */
    if ( sched->current_vm == add_vm )
    {
        if ( sched->state == TS_GPU_STOPPING )
        {
            list_add_tail(&add_vm->sched_entry, &sched->vm_req_list);
            sched->req_count++;
        }
        return;
    }

    switch ( sched->state )
    {
    case TS_NO_REQ:
        if ( grant_gpu_to_vm(sched, add_vm) )
        {
            sched->req_count = 1;
            sched->state = TS_SINGLE_REQ;
        }
        else
        {
            mali_gsi_handle_gpu_lost(add_vm);
            add_vm->gpu_lost = true;
        }
        break;

    case TS_SINGLE_REQ:
        list_add_tail(&add_vm->sched_entry, &sched->vm_req_list);
        sched->req_count++;
        set_timer(&sched->request_timer,
                  NOW() + sched->request_timeout_ns);
        sched->state = TS_RUNNING;
        break;

    case TS_RUNNING:
    case TS_GPU_STOPPING:
    case TS_STOPPED:
        list_add_tail(&add_vm->sched_entry, &sched->vm_req_list);
        sched->req_count++;
        break;
    }
}

static bool ts_sched_remove_vm(void *sched_ptr, struct mali_vm_data *rem_vm,
                                bool req_again)
{
    struct gsi_scheduler_ts *sched = sched_ptr;

    /*
     * Guard against late GPU_STOPPED arriving after yield_timer_fn
     * already sent GPU_LOST and dropped the VM from the scheduler.
     * The VM is not current and its sched_entry is empty (not in
     * vm_req_list). If req_again is set (e.g. VM driver re-init),
     * treat it as a fresh request instead of ignoring it.
     */
    if ( sched->current_vm != rem_vm && list_empty(&rem_vm->sched_entry) )
    {
        if ( !req_again )
            return true;
        rem_vm->gpu_lost = false;
        sched->req_count++;
        list_add_tail(&rem_vm->sched_entry, &sched->vm_req_list);
        if ( !sched->current_vm )
            try_grant_next_vm(sched);
        update_sched_state(sched);
        return true;
    }

    flush_current_vm_stats(sched);

    if ( sched->current_vm == rem_vm )
    {
        bool already_queued = !list_empty(&rem_vm->sched_entry);

        stop_timer(&sched->yield_timer);
        if ( ctrlif_unassign_partition(sched->gsi->part_ctrl) )
            printk(XENLOG_ERR "GSI%u: ctrlif_unassign_partition failed\n",
                   sched->gsi->idx);
        sched->current_vm = NULL;

        if ( req_again && !already_queued )
            list_add_tail(&rem_vm->sched_entry, &sched->vm_req_list);
        else if ( !req_again && already_queued )
        {
            /*
             * VM was queued by GPU_REQUEST during GPU_STOPPING but
             * now says it doesn't want GPU anymore. Remove it.
             * Decrement twice: once for current_vm, once for queue.
             */
            list_del_init(&rem_vm->sched_entry);
            if ( sched->req_count > 1 )
                sched->req_count -= 2;
            else
                sched->req_count = 0;
        }
        else if ( !req_again && !already_queued )
        {
            if ( sched->req_count > 0 )
                sched->req_count--;
        }
        else
        {
            /*
             * req_again && already_queued: the VM is already queued
             * for re-scheduling so it stays on the list. Decrement
             * once for the vacated current_vm slot.
             */
            if ( sched->req_count > 0 )
                sched->req_count--;
        }
    }
    else
    {
        /*
         * VM is in the wait queue (not current). Remove it first,
         * then re-add at tail if it still wants GPU time.
         */
        list_del_init(&rem_vm->sched_entry);

        if ( req_again )
            list_add_tail(&rem_vm->sched_entry, &sched->vm_req_list);
        else if ( sched->req_count > 0 )
            sched->req_count--;
    }

    /* Grant GPU to next VM in queue if no VM currently holds it */
    if ( !sched->current_vm )
    {
        /*
         * GPU_STOPPING is resolved (current_vm was removed).
         * Clear it so update_sched_state can set the correct state.
         */
        if ( sched->state == TS_GPU_STOPPING )
            sched->state = TS_NO_REQ;
        try_grant_next_vm(sched);
    }

    update_sched_state(sched);

    return true;
}

static void ts_sched_stop_idle_vm(void *sched_ptr,
                                   struct mali_vm_data *arb_vm)
{
    struct gsi_scheduler_ts *sched = sched_ptr;
    int err;

    if ( sched->current_vm == NULL || sched->current_vm != arb_vm )
    {
        /*
         * The VM is not the current GPU holder.  This can happen
         * when a timeslice switch already moved the GPU to another
         * VM and the idle notification arrived late.  Ignore it.
         */
        return;
    }

    if ( sched->state == TS_SINGLE_REQ )
    {
        /*
         * Only one VM is requesting the GPU, no contention.
         * Let the VM keep the partition; it will re-request
         * when it has new work.
         */
        return;
    }

    if ( sched->state != TS_RUNNING )
        return;

    err = mali_gsi_handle_gpu_stop(sched->current_vm);
    if ( err )
        printk(XENLOG_ERR "GSI%u: GPU_STOP to AW%u failed (err=%d)\n",
               sched->gsi->idx, sched->current_vm->aw, err);

    sched->state = TS_GPU_STOPPING;
    set_timer(&sched->yield_timer, NOW() + sched->yield_timeout_ns);
}

static struct mali_vm_data *ts_sched_get_active_vm(void *sched_ptr)
{
    return ((struct gsi_scheduler_ts *)sched_ptr)->current_vm;
}

static void ts_sched_stop(void *sched_ptr)
{
    struct gsi_scheduler_ts *sched = sched_ptr;
    struct mali_vm_data *vm, *tmp;

    /*
     * Phase 1 (lock held by caller): disarm timers non-blockingly and
     * update state so any in-flight callback will bail out when it
     * eventually acquires the lock.
     */
    stop_timer(&sched->request_timer);
    stop_timer(&sched->yield_timer);

    if ( sched->current_vm )
    {
        mali_gsi_handle_gpu_stop(sched->current_vm);
        if ( ctrlif_unassign_partition(sched->gsi->part_ctrl) )
            printk(XENLOG_ERR "GSI%u: ctrlif_unassign_partition failed\n",
                   sched->gsi->idx);
        flush_current_vm_stats(sched);
        sched->current_vm = NULL;
    }

    /*
     * Drain queued VMs: they were waiting for GPU_GRANTED that will
     * never arrive. Send GPU_LOST so each VM can enter its recovery
     * path instead of hanging indefinitely.
     */
    list_for_each_entry_safe(vm, tmp, &sched->vm_req_list, sched_entry)
    {
        mali_gsi_handle_gpu_lost(vm);
        vm->gpu_lost = true;
        list_del_init(&vm->sched_entry);
    }
    sched->req_count = 0;

    sched->state = TS_STOPPED;

    /*
     * Phase 2: release the lock so kill_timer() can wait for any
     * in-flight timer callback to complete without deadlock.
     * The callback will see TS_STOPPED and return immediately.
     *
     * Any concurrent sched_add_vm() that acquires the lock during
     * this window will see TS_STOPPED and only enqueue (not grant),
     * keeping state consistent for the subsequent sched_start().
     */
    spin_unlock(sched->gsi_lock);
    kill_timer(&sched->request_timer);
    kill_timer(&sched->yield_timer);
    spin_lock(sched->gsi_lock);

    /*
     * Phase 3: drain any VMs enqueued by concurrent sched_add_vm()
     * during the phase-2 window.  Without this, mali_gsi_destroy()
     * would free sched_ptr while those VMs still have sched_entry
     * linked to vm_req_list, causing use-after-free.
     */
    list_for_each_entry_safe(vm, tmp, &sched->vm_req_list, sched_entry)
    {
        mali_gsi_handle_gpu_lost(vm);
        vm->gpu_lost = true;
        list_del_init(&vm->sched_entry);
    }
    sched->req_count = 0;
}

static void ts_sched_start(void *sched_ptr)
{
    struct gsi_scheduler_ts *sched = sched_ptr;

    try_grant_next_vm(sched);
    update_sched_state(sched);
}

static void ts_sched_resync_vm(void *sched_ptr, struct mali_vm_data *vm)
{
    struct gsi_scheduler_ts *sched = sched_ptr;

    if ( sched->current_vm == vm )
    {
        mali_gsi_handle_gpu_stop(vm);
        return;
    }

    /* Remove VM from the queue if present */
    if ( !list_empty(&vm->sched_entry) )
    {
        list_del_init(&vm->sched_entry);
        if ( sched->req_count > 0 )
            sched->req_count--;
        update_sched_state(sched);
    }
}

static void ts_sched_get_utilisation(void *sched_ptr,
                                      uint32_t *gsi_busytime,
                                      uint32_t *gsi_totaltime)
{
    struct gsi_scheduler_ts *sched = sched_ptr;
    s_time_t busy, total;
    s_time_t now = NOW();

    /* Read-only snapshot: do not modify scheduler state */
    busy = sched->total_busy;
    total = sched->total_time;
    if ( sched->busy_start )
        busy += now - sched->busy_start;
    if ( sched->last_grant_time )
        total += now - sched->last_grant_time;

    /* Report in milliseconds to avoid uint32_t overflow (~49 days range) */
    *gsi_busytime = (uint32_t)(busy / MILLISECS(1));
    *gsi_totaltime = (uint32_t)(total / MILLISECS(1));
}

static void ts_sched_gpu_active(void *sched_ptr, struct mali_vm_data *arb_vm)
{
    struct gsi_scheduler_ts *sched = sched_ptr;

    if ( sched->current_vm == arb_vm )
    {
        s_time_t now = NOW();

        sched->busy_start = now;

        /*
         * The VM has actually started using the GPU.  Re-arm the
         * request timer with the real quantum so the timeslice is
         * measured from this point, not from the grant time.
         */
        if ( sched->state == TS_RUNNING )
            set_timer(&sched->request_timer,
                      now + sched->request_timeout_ns);
    }
}

static void ts_sched_print_stats(void *sched_ptr, const char *tab)
{
    struct gsi_scheduler_ts *sched = sched_ptr;
    s_time_t busy, total;

    if ( sched->current_vm )
        printk("%sScheduler: timeslice  state=%s  queued=%u  "
               "current=AW%u\n",
               tab,
               (sched->state < ARRAY_SIZE(ts_state_name)) ?
                   ts_state_name[sched->state] : "UNKNOWN",
               sched->req_count,
               sched->current_vm->aw);
    else
        printk("%sScheduler: timeslice  state=%s  queued=%u  "
               "current=none\n",
               tab,
               (sched->state < ARRAY_SIZE(ts_state_name)) ?
                   ts_state_name[sched->state] : "UNKNOWN",
               sched->req_count);

    printk("%s  request_timeout=%"PRI_stime"ms  yield_timeout=%"PRI_stime"ms\n",
           tab,
           sched->request_timeout_ns / MILLISECS(1),
           sched->yield_timeout_ns / MILLISECS(1));

    /* Snapshot GSI-level utilization without resetting counters */
    {
        s_time_t now = NOW();

        busy = sched->total_busy;
        total = sched->total_time;
        if ( sched->busy_start )
            busy += now - sched->busy_start;
        if ( sched->last_grant_time )
            total += now - sched->last_grant_time;
    }

    if ( total > MILLISECS(1) )
    {
        /* Divide before multiplying to avoid s_time_t overflow */
        unsigned int pct = (unsigned int)(busy / (total / 100));

        printk("%sUtilization: busy=%"PRI_stime"ms / total=%"PRI_stime"ms (%u%%)\n",
               tab, busy / MILLISECS(1), total / MILLISECS(1), pct);
    }
    else
        printk("%sUtilization: idle (no data)\n", tab);
}

static void ts_sched_destroy(void *sched_ptr)
{
    struct gsi_scheduler_ts *sched = sched_ptr;

    kill_timer(&sched->request_timer);
    kill_timer(&sched->yield_timer);
}

static const struct mali_arb_gsi_sched_ops gsi_scheduler_timeslice = {
    .sched_get_utilisation = ts_sched_get_utilisation,
    .sched_stop_idle_vm    = ts_sched_stop_idle_vm,
    .sched_get_active_vm   = ts_sched_get_active_vm,
    .sched_stop            = ts_sched_stop,
    .sched_start           = ts_sched_start,
    .sched_add_vm          = ts_sched_add_vm,
    .sched_remove_vm       = ts_sched_remove_vm,
    .sched_resync_vm       = ts_sched_resync_vm,
    .sched_print_stats     = ts_sched_print_stats,
    .sched_gpu_active      = ts_sched_gpu_active,
    .sched_destroy         = ts_sched_destroy,
};

int __init register_gsi_timeslice_scheduler(struct mali_arb_gsi *gsi,
                                             spinlock_t *gsi_lock)
{
    struct gsi_scheduler_ts *sched;

    if ( mali_ts_quantum_ms < TS_QUANTUM_MS_MIN ||
         mali_ts_quantum_ms > TS_QUANTUM_MS_MAX )
    {
        printk(XENLOG_WARNING
               "mali_ts_quantum_ms=%u outside valid range [%u,%u]. "
               "Resetting to default %u\n",
               mali_ts_quantum_ms, TS_QUANTUM_MS_MIN, TS_QUANTUM_MS_MAX,
               TS_QUANTUM_MS_DEFAULT);
        mali_ts_quantum_ms = TS_QUANTUM_MS_DEFAULT;
    }

    if ( mali_ts_yield_ms < TS_YIELD_MS_MIN ||
         mali_ts_yield_ms > TS_YIELD_MS_MAX )
    {
        printk(XENLOG_WARNING
               "mali_ts_yield_ms=%u outside valid range [%u,%u]. "
               "Resetting to default %u\n",
               mali_ts_yield_ms, TS_YIELD_MS_MIN, TS_YIELD_MS_MAX,
               TS_YIELD_MS_DEFAULT);
        mali_ts_yield_ms = TS_YIELD_MS_DEFAULT;
    }

    sched = xzalloc(struct gsi_scheduler_ts);
    if ( !sched )
    {
        printk(XENLOG_ERR "Failed to allocate timeslice scheduler\n");
        return -ENOMEM;
    }

    sched->gsi = gsi;
    sched->gsi_lock = gsi_lock;
    sched->current_vm = NULL;
    sched->req_count = 0;
    sched->state = TS_NO_REQ;

    INIT_LIST_HEAD(&sched->vm_req_list);

    sched->request_timeout_ns = MILLISECS(mali_ts_quantum_ms);
    sched->yield_timeout_ns = MILLISECS(mali_ts_yield_ms);

    sched->busy_start = 0;
    sched->total_busy = 0;
    sched->total_time = 0;
    sched->last_grant_time = 0;

    init_timer(&sched->request_timer, request_timer_fn, sched, 0);
    init_timer(&sched->yield_timer, yield_timer_fn, sched, 0);

    gsi->sched_ptr = sched;
    gsi->sched_ops = &gsi_scheduler_timeslice;

    printk(XENLOG_INFO "GSI%u: Using timeslice scheduler "
           "(quantum=%ums yield=%ums)\n", gsi->idx,
           mali_ts_quantum_ms, mali_ts_yield_ms);
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
