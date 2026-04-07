/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Mali G78AE GPU PTM Assign Driver header
 *
 * Copyright (C) 2025, Advanced Micro Devices, Inc.
 */
#ifndef DRIVERS__GPU_MALI_G78AE_ASSIGN_H
#define DRIVERS__GPU_MALI_G78AE_ASSIGN_H

#include <xen/types.h>
#include "common.h"

enum PTM_BUS_ENUM {
    BUS_A = 0,
    BUS_B = 1,
    BUS_C = 2,
    BUS_COUNT = 3
};

struct resource_group {
	bool is_set;
	uint32_t bus;
};

struct access_window {
	bool is_set;
	uint32_t rg;
};

struct partition {
	bool is_set;
	uint32_t rg;
};

struct slice {
	bool is_set;
	uint32_t rg;
	uint32_t isolation_set;
};

struct mali_ptm_assign {
    paddr_t base;
    paddr_t size;
	struct resource_group res_grps[MALI_PTM_PARTITION_COUNT];
	struct access_window access_windows[MALI_PTM_ACCESS_WINDOW_COUNT];
	struct partition partitions[MALI_PTM_PARTITION_COUNT];
	struct slice slices[MALI_PTM_SLICES_COUNT];
	unsigned int partition_count, slice_count, aw_count, rg_count;
};

int mali_ptm_assign_init(struct mali_ptm_assign *assign);
int bus_from_rg_id(unsigned int rg_id, struct mali_ptm_assign *assign);
void mali_assign_print_config(struct mali_ptm_assign *assign);
#endif /* DRIVERS__GPU_MALI_G78AE_ASSIGN_H */

/*
 * Local variables:
 * mode: C
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * End:
 */