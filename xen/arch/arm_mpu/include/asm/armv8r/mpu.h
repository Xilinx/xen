// SPDX-License-Identifier: GPL-2.0

#ifndef __ARM_MPU_H__
#define __ARM_MPU_H__

#define MPU_REGION_SHIFT  6
#define MPU_REGION_ALIGN  (_AC(1, UL) << MPU_REGION_SHIFT)
#define MPU_REGION_MASK   (~(MPU_REGION_ALIGN - 1))

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

#endif /* __ARM_MPU_H__ */
