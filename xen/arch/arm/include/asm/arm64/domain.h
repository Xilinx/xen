/* SPDX-License-Identifier: GPL-2.0-only */

#ifndef ARM_ARM64_DOMAIN_H
#define ARM_ARM64_DOMAIN_H

#include <asm/cpufeature.h>

struct kernel_info;

/*
 * Returns true if guest execution state is AArch32
 *
 * @d: pointer to the domain structure
 */
#define is_32bit_domain(d) ((d)->arch.type == DOMAIN_32BIT)

/*
 * Returns true if guest execution state is AArch64
 *
 * @d: pointer to the domain structure
 */
#define is_64bit_domain(d) ((d)->arch.type == DOMAIN_64BIT)

/*
 * Arm64 declares AArch32 (32bit) Execution State support in the
 * Processor Feature Registers (PFR0), but also can be disabled manually.
 */
static inline bool is_aarch32_enabled(void)
{
    return IS_ENABLED(CONFIG_ARM64_AARCH32) && cpu_has_el1_32;
}

/*
 * Set domain type from struct kernel_info which defines guest Execution
 * State AArch32/AArch64 during regular dom0 or predefined (dom0less)
 * domains creation .
 * Type must be set before allocate_memory or create vcpus.
 *
 * @kinfo: pointer to the kinfo structure.
 */
int arm64_set_domain_type(struct kernel_info *kinfo);

#endif /* ARM_ARM64_DOMAIN_H */

/*
 * Local variables:
 * mode: C
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * End:
 */
