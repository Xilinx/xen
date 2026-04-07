/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Mali G78AE GPU PTM Partition control driver header
 *
 * Copyright (C) 2025, Advanced Micro Devices, Inc.
 */
#ifndef DRIVERS__GPU_MALI_G78AE_PARTITION_CONTROL_H
#define DRIVERS__GPU_MALI_G78AE_PARTITION_CONTROL_H

#include <xen/device_tree.h>

struct mali_ptm_part_ctrl
{
    paddr_t base;
    paddr_t size;
    void __iomem *mem;
};

int mali_ptm_part_ctrl_init(struct dt_device_node *mali_gpu_node,
			                struct mali_ptm_part_ctrl *part_ctrls);
int mali_ptm_part_ctrl_get_id(struct dt_device_node *part_ctrl_node);

/* Partition control interface */
/**
 * Assign the specified partition to the given Address Window ID
 * @ctrl: Pointer to the partition control structure.
 * @aw:   Address Window identifier to assign the partition to.
 *
 * Return: 0 on success, or a negative error code on failure.
 */
int ctrlif_assign_partition_to_aw(struct mali_ptm_part_ctrl *ctrl,
                                  unsigned int aw);

/**
 * Get the Address Window ID assigned to the partition.
 * @ctrl: Pointer to the partition control structure.
 * @aw:   Pointer to store the Address Window ID after retrieval.
 *
 * Return: 0 on success, or a negative error code on failure.
 */
int ctrlif_get_assigned_aw(struct mali_ptm_part_ctrl *ctrl,
                           unsigned int *aw);

/**
 * Unassign the partition from its current Address Window.
 * @ctrl: Pointer to the partition control structure.
 *
 * Return: 0 on success, or a negative error code on failure.
 */
int ctrlif_unassign_partition(struct mali_ptm_part_ctrl *ctrl);
#endif

/*
 * Local variables:
 * mode: C
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * End:
 */