/* SPDX-License-Identifier: GPL-2.0-only */

#ifndef ARM_ARM32_DOMAIN_H
#define ARM_ARM32_DOMAIN_H

/* Arm32 always runs guests in AArch32 mode */

#define is_32bit_domain(d) ((void)(d), 1)
#define is_64bit_domain(d) ((void)(d), 0)

#endif /* ARM_ARM32_DOMAIN_H */

/*
 * Local variables:
 * mode: C
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * End:
 */
