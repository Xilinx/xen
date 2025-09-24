/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Mali G78AE GPU PTM Partition configuration driver header
 *
 * Copyright (C) 2025, Advanced Micro Devices, Inc.
 */
#ifndef DRIVERS__GPU_MALI_G78AE_PARTITION_CONFIG_H
#define DRIVERS__GPU_MALI_G78AE_PARTITION_CONFIG_H

#include <xen/device_tree.h>

struct mali_ptm_part_cfg
{
    paddr_t base;
    paddr_t size;
    void __iomem *mem;
};

int mali_ptm_part_cfg_init(struct dt_device_node *mali_gpu_node,
                           struct mali_ptm_part_cfg *part_cfgs);
int mali_ptm_part_cfg_get_id(struct dt_device_node *part_cfg);

/* Partition configuration interface */
/**
* Updates the partition configuration to enable the slices specified by the
 * bitmask.
 * @part_cfg: Pointer to the partition configuration structure.
 * @slices_mask: Bitmask specifying which slices to enable (1 = enabled,
 *               0 = disabled).
 *
 * Returns 0 on success, or a negative error code on failure.
 */
int cfgif_assign_slices(struct mali_ptm_part_cfg *part_cfg,
                        uint32_t slices_mask);

#endif /* DRIVERS__GPU_MALI_G78AE_PARTITION_CONFIG_H */

/*
 * Local variables:
 * mode: C
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * End:
 */