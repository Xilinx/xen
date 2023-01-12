// SPDX-License-Identifier: GPL-2.0

#ifndef __ARM_MPU_H__
#define __ARM_MPU_H__

#define MPU_REGION_SHIFT  6
#define MPU_REGION_ALIGN  (_AC(1, UL) << MPU_REGION_SHIFT)
#define MPU_REGION_MASK   (~(MPU_REGION_ALIGN - 1))

#define _REGION_XN_BIT      0
#define _REGION_RO_BIT      1
#define _REGION_XN          (1U << _REGION_XN_BIT)
#define _REGION_RO          (1U << _REGION_RO)
#define REGION_XN_MASK(x)   (((x) >> _REGION_XN_BIT) & 0x1U)
#define REGION_RO_MASK(x)   (((x) >> _REGION_RO_BIT) & 0x1U)

#define REGION_HYPERVISOR_RW    _REGION_XN

#if defined(CONFIG_ARM_32)
# include <asm/armv8r/arm32/mpu.h>
#elif defined(CONFIG_ARM_64)
# include <asm/armv8r/arm64/mpu.h>
#else
# error "unknown ARM variant"
#endif

/* For Armv8-R, the MPU protection regions can reach to 256. */
#define MAX_MPU_PROTECTION_REGIONS   256
#define MPU_PRENR_BITS               32

/* Access permission attributes. */
/* Read/Write at EL2, No Access at EL1/EL0. */
#define AP_RW_EL2 0x0
/* Read/Write at EL2/EL1/EL0 all levels. */
#define AP_RW_ALL 0x1
/* Read-only at EL2, No Access at EL1/EL0. */
#define AP_RO_EL2 0x2
/* Read-only at EL2/EL1/EL0 all levels. */
#define AP_RO_ALL 0x3

/*
 * Excute never.
 * Stage 1 EL2 translation regime.
 * XN[1] determines whether execution of the instruction fetched from the MPU
 * memory region is permitted.
 * Stage 2 EL1/EL0 translation regime.
 * XN[0] determines whether execution of the instruction fetched from the MPU
 * memory region is permitted.
 */
#define XN_DISABLED    0x0
#define XN_ENABLED     0x1

#endif /* __ARM_MPU_H__ */
