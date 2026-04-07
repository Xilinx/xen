/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Mali G78AE GPU PTM common header
 *
 * Copyright (C) 2025, Advanced Micro Devices, Inc.
 */
#ifndef DRIVERS__GPU_MALI_G78AE_COMMON_H
#define DRIVERS__GPU_MALI_G78AE_COMMON_H

#include <xen/delay.h>
#include <xen/errno.h>
#include <xen/time.h>
#include <xen/types.h>
#include <asm/io.h>

#define PTM_DEVICE_ID (0x0000)
#define PTM_UNIT_FEATURES (0x0004)
#define PTM_SLICE_CORES ((uint64_t)(0x000C))
#define MAX_AW_MASK 0xFFFF
/* This is the total number of AW IDs permitted. They go from 0 to
 * MAX_AW_NUM - 1. Therefore MAX_AW_NUM is considered as an invalid AW_ID.
 */
#define MAX_AW_NUM 16
#define IS_VALID_AW(aw) ((aw) >= 0 && (aw) < MAX_AW_NUM)

#define MALI_PTM_RESOURCE_GROUP_COUNT (4)
#define MALI_PTM_ACCESS_WINDOW_COUNT (16)
#define MALI_PTM_PARTITION_COUNT (4)
#define MALI_PTM_SLICES_COUNT (8)

#define REG_POLL_SLEEP_US 1
#define REG_POLL_TIMEOUT_US 40
#define REG_POLL_RESET_TIMEOUT_US 20000

/* We check PTM_DEVICE_ID against this value to determine compatibility */
#define PTM_SUPPORTED_VER 0x9e550000
/* Required and masked fields: */
/* Bits [3:0] VERSION_STATUS - masked */
/* Bits [11:4] VERSION_MINOR - masked */
/* Bits [15:12] VERSION_MAJOR - masked */
/* Bits [19:16] PRODUCT_MAJOR - required */
/* Bits [23:20] ARCH_REV - masked */
/* Bits [27:24] ARCH_MINOR - required */
/* Bits [31:28] ARCH_MAJOR - required */
#define PTM_VER_MASK 0xFF0F0000

static inline int check_ptm_version(uint32_t ptm_device_id)
{
    if ( ptm_device_id == 0 )
    {
        printk(XENLOG_ERR "Read zero from PTM ID register.\n");
        return -EIO;
    }

    /* Mask the status, minor, major versions and arch_rev. */
    ptm_device_id &= PTM_VER_MASK;

    if ( ptm_device_id != (PTM_SUPPORTED_VER & PTM_VER_MASK) )
    {
        printk(XENLOG_ERR "Unsupported PTM version (DRV: 0x%X HW: 0x%X)\n",
            PTM_SUPPORTED_VER & PTM_VER_MASK, ptm_device_id);
        return -ENODEV;
    }

    return 0;
}
#endif /* DRIVERS__GPU_MALI_G78AE_COMMON_H */

/*
 * Local variables:
 * mode: C
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * End:
 */