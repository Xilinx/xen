/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Mali G78AE GPU hardware virtualization support
 *
 * Copyright (C) 2025, Advanced Micro Devices, Inc.
 */

#ifndef __ARCH_ARM_MALI_G78AE_H__
#define __ARCH_ARM_MALI_G78AE_H__

#define INVALID_AW UINT8_MAX
/* Address Window (AW) range for Mali G78AE */
#define AW_MIN (0)
#define AW_MAX (15)

#ifdef CONFIG_MALI_G78AE
extern int opt_dom0_mali_aw;

int mali_g78ae_register_domain(struct domain *d, unsigned int aw);
int mali_g78ae_unregister_domain(struct domain *d);

#else
#define opt_dom0_mali_aw INVALID_AW

static inline int mali_g78ae_register_domain(struct domain *d, unsigned int aw)
{
    return 0;
}
static inline int mali_g78ae_unregister_domain(struct domain *d)
{
    return 0;
}
#endif /* CONFIG_MALI_G78AE */

#endif /* __ARCH_ARM_MALI_G78AE_H__ */

/*
 * Local variables:
 * mode: C
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * End:
 */