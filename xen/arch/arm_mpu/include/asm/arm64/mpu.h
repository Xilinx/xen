// SPDX-License-Identifier: GPL-2.0-only
/*
 * mpu.h: Arm Memory Protection Unit definitions.
 */

#ifndef __ARM64_MPU_H__
#define __ARM64_MPU_H__

#define MPU_REGION_SHIFT  6
#define MPU_REGION_ALIGN  (_AC(1, UL) << MPU_REGION_SHIFT)
#define MPU_REGION_MASK   (~(MPU_REGION_ALIGN - 1))

#endif /* __ARM64_MPU_H__ */
