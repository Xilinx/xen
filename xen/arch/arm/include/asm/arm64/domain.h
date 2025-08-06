/* SPDX-License-Identifier: GPL-2.0-only */

#ifndef ARM_ARM64_DOMAIN_H
#define ARM_ARM64_DOMAIN_H

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

#endif /* ARM_ARM64_DOMAIN_H */

/*
 * Local variables:
 * mode: C
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * End:
 */
