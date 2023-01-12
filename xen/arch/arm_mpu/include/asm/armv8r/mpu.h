// SPDX-License-Identifier: GPL-2.0

#ifndef __ARM_MPU_H__
#define __ARM_MPU_H__

#define MPU_REGION_SHIFT  6
#define MPU_REGION_ALIGN  (_AC(1, UL) << MPU_REGION_SHIFT)
#define MPU_REGION_MASK   (~(MPU_REGION_ALIGN - 1))

#if defined(CONFIG_ARM_64)
# include <asm/armv8r/arm64/mpu.h>
#endif

#endif /* __ARM_MPU_H__ */
