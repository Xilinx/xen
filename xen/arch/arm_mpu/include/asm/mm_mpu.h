/*
 *
 * MPU based memory managment code for an Armv8-R AArch64.
 *
 * Copyright (C) 2022 Arm Ltd.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef __ARCH_ARM_MM_MPU__
#define __ARCH_ARM_MM_MPU__

#include <asm/arm64/mpu.h>

/* MPU-related varaible */
extern unsigned long nr_xen_mpumap;

static inline paddr_t __virt_to_maddr(vaddr_t va)
{
    return (paddr_t)va;
}
#define virt_to_maddr(va)   __virt_to_maddr((vaddr_t)(va))

static inline void *maddr_to_virt(paddr_t ma)
{
    /* In MPU system, VA == PA. */
    return (void *)ma;
}

#endif /* __ARCH_ARM_MM_MPU__ */
