/* SPDX-License-Identifier: (GPL-2.0-or-later OR BSD-2-Clause) */
#ifndef __ARCH_ARM_VSMMU_V3_H__
#define __ARCH_ARM_VSMMU_V3_H__

#include <asm/viommu.h>

#ifdef CONFIG_VIRTUAL_ARM_SMMU_V3

void vsmmuv3_set_type(void);

#else

static inline void vsmmuv3_set_type(void)
{
    return;
}

#endif /* CONFIG_VIRTUAL_ARM_SMMU_V3 */

#endif /* __ARCH_ARM_VSMMU_V3_H__ */
