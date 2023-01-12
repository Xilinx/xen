// SPDX-License-Identifier: GPL-2.0-only
/*
 * mpu.h: Arm Memory Protection Unit definitions.
 */

#ifndef __ARM32_MPU_H__
#define __ARM32_MPU_H__

#ifndef __ASSEMBLY__

static inline uint32_t generate_vsctlr(uint16_t vmid)
{
    return ((uint32_t)vmid << 16);
}

extern void set_boot_mpumap(u32 len, pr_t *table);

#endif /* __ASSEMBLY__ */
#endif /* __ARM32_MPU_H__ */
