/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Mali G78AE GPU PTM Resource Group header
 *
 * Copyright (C) 2025, Advanced Micro Devices, Inc.
 */
#ifndef DRIVERS__GPU_MALI_G78AE_RESOURCE_GROUP_H
#define DRIVERS__GPU_MALI_G78AE_RESOURCE_GROUP_H

#include "partition-control.h"
#include "partition-config.h"
#include "ptm-msg.h"
#include "arbiter.h"

struct mali_arbiter;

enum mali_gpu_slice_tiler_type {
    MALI_GPU_TILER_COMPACT,
    MALI_GPU_TILER_HIGH_PERFORMANCE
};

/**
 * enum ptm_rg_state - Resource Group states
 * @HANDSHAKE_INIT:        The protocol handshake is being initialized.
 * @HANDSHAKE_IN_PROGRESS: The protocol handshake is in progress.
 * @HANDSHAKE_DONE:        The protocol handshake was done successfully.
 * @HANDSHAKE_FAILED:      The protocol handshake failed.
 *
 * Definition of the possible Resource Group's states
 */
enum ptm_rg_state {
    HANDSHAKE_INIT,
    HANDSHAKE_IN_PROGRESS,
    HANDSHAKE_DONE,
    HANDSHAKE_FAILED,
};

/**
 * struct ptm_rg_protocol - Resource Group protocol data for an access window
 * @state:              Current Resource Group state.
 * @version_in_use:     Agreed protocol versions used for the communication with
 *                      the Access Windows. Defined during protocol handshake.
 * @posthandshake_stop: keeps track if the VMs needs to receive a gpu_stop
 *                      message when handshaking is complete.
 * @lock:               lock to protect against concurrent API calls accessing
 *                      the protocol data.
 */
struct ptm_rg_protocol {
    enum ptm_rg_state state;
    uint8_t version_in_use;
    atomic_t posthandshake_stop;
    spinlock_t lock;
};

struct mali_ptm_rg {
    struct {
        int line;
        int flags;
    } irq;
    paddr_t base;
    paddr_t size;
    unsigned int id;
    void __iomem *mem;
    struct ptm_msg_handler msg_handler;

    struct mali_ptm_part_ctrl *ctrl[MALI_PTM_PARTITION_COUNT];
    struct mali_ptm_part_cfg *cfg[MALI_PTM_PARTITION_COUNT];
    /* The partitions mask that this RG is controlling */
    uint32_t partition_mask;

    struct ptm_rg_protocol prot_data[MAX_AW_NUM];
    struct mali_vm_data *ptm_rg_vm[MAX_AW_NUM];

    struct tasklet recv_task;

    /* Lock to protect the mali_ptm_rg structure */
    spinlock_t lock;

    /* The arbiter instance of the RG */
    struct mali_arbiter* arbiter;
};

int mali_ptm_rg_init(struct dt_device_node *mali_gpu_node,
                     struct mali_ptm_part_ctrl *partition_controls,
                     struct mali_ptm_part_cfg *partition_cfg,
                     struct mali_ptm_rg *rg);

/* RG interface functions (used by the Arbiter istance of the RG) */
void rgif_get_slices_core_mask(struct mali_ptm_rg *rg, uint64_t *core_mask,
                                uint8_t *core_mask_stride);
int rgif_get_slice_mask(struct mali_ptm_rg *rg, uint32_t *slice_mask);
int rgif_get_partition_mask(struct mali_ptm_rg *rg, uint32_t *partition_mask);
int rgif_get_aw_mask(struct mali_ptm_rg *rg, uint16_t*aw_mask);
int rgif_poweron_slices(struct mali_ptm_rg *rg, uint32_t slice_mask);
int rgif_poweroff_slices(struct mali_ptm_rg *rg, uint32_t slice_mask);
uint32_t rgif_get_powered_slices_mask(struct mali_ptm_rg *rg);
int rgif_enable_slices(struct mali_ptm_rg *rg, uint32_t slice_mask);
uint32_t rgif_get_enabled_slices_mask(struct mali_ptm_rg *rg);
int rgif_reset_slices(struct mali_ptm_rg *rg, uint32_t slice_mask);

#endif /* DRIVERS__GPU_MALI_G78AE_RESOURCE_GROUP_H */

/*
 * Local variables:
 * mode: C
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * End:
 */