/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Mali G78AE GPU GPU Subinstance (GSI) arbiter interface
 *
 * Copyright (C) 2025, Advanced Micro Devices, Inc.
 */
#ifndef DRIVERS__GPU_MALI_G78AE_ARBITER_GSI_H
#define DRIVERS__GPU_MALI_G78AE_ARBITER_GSI_H

#include <xen/types.h>

#include "arbiter.h"
#include "gsi-scheduler-if.h"
#include "partition-config.h"
#include "partition-control.h"

struct mali_vm_data;
struct mali_arbiter;

/* Default frequency to use when granting GPU access to a VM */
#define GSI_DEFAULT_FREQ (999)

/**
 * enum mali_arb_flags - gpu-subinstance flags
 * @GSI_FLAG_SLICE_ASSIGNED: Set when slice is assigned to the gpu-subinstance.
 * @GSI_FLAG_MAX: Maximum valid index.
 *
 * Runtime flags that are set by arbiter-core submodule.
 */
enum mali_arb_flags { GSI_FLAG_SLICE_ASSIGNED, GSI_FLAG_MAX };

/**
 * enum mali_arb_state - gpu-subinstance states
 * @STARTING: A gpu-subinstance start request is in progress
 * @STARTED: The gpu-subinstance is started
 * @STOPPING: A gpu-subinstance stop request is in progress
 * @STOPPED: The gpu-subinstance is stopped
 *
 * Definition of the possible arbiter's states
 */
enum mali_arb_state {
    STARTING,
    STARTED,
    STOPPING,
    STOPPED
};
struct mali_arb_gsi {
    unsigned int idx;
    unsigned int flags;
    /* Assign interfaces come from partition control and config */
    struct mali_ptm_part_cfg *part_cfg;
    struct mali_ptm_part_ctrl *part_ctrl;
    /* Callbacks */
    struct mali_arbiter *arbiter;
    /* Scheduling */
    void *sched_ptr;
    const struct mali_arb_gsi_sched_ops *sched_ops;

    enum mali_arb_state state;
    uint32_t curr_gpu_freq;
};

/* GSI interface */
int mali_gsi_create(struct mali_arb_gsi **gsi, unsigned int idx,
                  struct mali_arbiter *arbiter);
void mali_gsi_destroy(struct mali_arb_gsi *gsi);
void mali_gsi_start(struct mali_arb_gsi *gsi);
void mali_gsi_stop(struct mali_arb_gsi *gsi);
void mali_gsi_get_utilisation(struct mali_arb_gsi *gsi,
                        uint32_t *gsi_busytime, uint32_t *gsi_totaltime);
void mali_gsi_update_freq(struct mali_arb_gsi *gsi, uint32_t new_freq);
void mali_gsi_flag_set(struct mali_arb_gsi *gsi, enum mali_arb_flags flag);
void mali_gsi_flag_clear(struct mali_arb_gsi *gsi, enum mali_arb_flags flag);
bool mali_arbiter_gsi_remove_vm(struct mali_arb_gsi *gsi,
                struct mali_vm_data *rem_vm);
/* GSI VM ARB handlers (see arb-vm-protocol.h VM_ARB_*) */
void mali_gsi_on_gpu_stopped(struct mali_vm_data *arb_vm,
                  struct mali_arb_gsi *gsi, bool req_again);
void mali_gsi_on_gpu_request(struct mali_vm_data *arb_vm,
                  struct mali_arb_gsi *gsi);
void mali_gsi_on_gpu_active(struct mali_vm_data *arb_vm,
                  struct mali_arb_gsi *gsi);
void mali_gsi_on_gpu_idle(struct mali_vm_data *arb_vm,
                  struct mali_arb_gsi *gsi);

/* GSI ARB VM handlers (see arb-vm-protocol.h GSI_ARB_VM_*) */
/* Send a gpu stop message to the VM */
int mali_gsi_handle_gpu_stop(struct mali_vm_data *arb_vm);
/* Send a gpu granted message to the VM */
int mali_gsi_handle_gpu_granted(struct mali_arb_gsi *gsi,
                            struct mali_vm_data *arb_vm);
/* Send a gpu lost message to the VM */
int mali_gsi_handle_gpu_lost(struct mali_vm_data *arb_vm);
#endif

/*
 * Local variables:
 * mode: C
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * End:
 */