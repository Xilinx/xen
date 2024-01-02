/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Last Level Cache (LLC) coloring support for ARM
 *
 * Copyright (C) 2022 Xilinx Inc.
 *
 * Authors:
 *    Luca Miccio <lucmiccio@gmail.com>
 *    Carlo Nonato <carlo.nonato@minervasys.tech>
 */
#ifndef __ASM_ARM_COLORING_H__
#define __ASM_ARM_COLORING_H__

#include <xen/init.h>

bool __init llc_coloring_init(void);
int dom0_set_llc_colors(struct domain *d);
int domain_set_llc_colors_from_str(struct domain *d, const char *str);

#endif /* __ASM_ARM_COLORING_H__ */

/*
 * Local variables:
 * mode: C
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * tab-width: 4
 * indent-tabs-mode: nil
 * End:
 */
