/* SPDX-License-Identifier: GPL-2.0-only */

#ifndef ARM_ARM32_DOMAIN_H
#define ARM_ARM32_DOMAIN_H

struct kernel_info;

/* Arm32 always runs guests in AArch32 mode */

#define is_32bit_domain(d) ((void)(d), 1)
#define is_64bit_domain(d) ((void)(d), 0)

static inline bool is_aarch32_enabled(void)
{
    return true;
}

static inline int arm64_set_domain_type(struct kernel_info *kinfo)
{
    return 0;
}

#endif /* ARM_ARM32_DOMAIN_H */

/*
 * Local variables:
 * mode: C
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * End:
 */
