// SPDX-License-Identifier: GPL-2.0-only
/*
 * config_mpu.h: A Linux-style configuration list for Arm MPU systems,
 *               only can be included by config.h
 */

#ifndef __ARM_CONFIG_MPU_H__
#define __ARM_CONFIG_MPU_H__

#ifdef CONFIG_FVP_BASER
#include <asm/platforms/fvp_baser.h>
#endif

/*
 * All MPU platforms need to provide a XEN_START_ADDRESS for linker. This
 * address indicates where Xen image will be loaded and run from. This
 * address must be aligned to a PAGE_SIZE.
 */
#if (XEN_START_ADDRESS % PAGE_SIZE) != 0
#error "XEN_START_ADDRESS must be aligned to PAGE_SIZE"
#endif

#define XEN_VIRT_START         _AT(paddr_t, XEN_START_ADDRESS)

#define HYPERVISOR_VIRT_START  XEN_VIRT_START

#define FRAMETABLE_SIZE        GB(32)
#define FRAMETABLE_NR          (FRAMETABLE_SIZE / sizeof(*frame_table))

#define FIXMAP_ADDR(n)         (_AT(paddr_t, n) & (PAGE_MASK))

#endif /* __ARM_CONFIG_MPU_H__ */
