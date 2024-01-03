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
#include <xen/mm-frame.h>

/**
 * Iterate over each Xen mfn in the colored space.
 * @mfn:    the current mfn. The first non colored mfn must be provided as the
 *          starting point.
 * @i:      loop index.
 */
#define for_each_xen_colored_mfn(mfn, i)        \
    for ( i = 0, mfn = xen_colored_mfn(mfn);    \
          i < (_end - _start) >> PAGE_SHIFT;    \
          i++, mfn = xen_colored_mfn(mfn_add(mfn, 1)) )

bool __init llc_coloring_init(void);
int dom0_set_llc_colors(struct domain *d);
int domain_set_llc_colors_from_str(struct domain *d, const char *str);

paddr_t xen_colored_map_size(paddr_t size);
mfn_t xen_colored_mfn(mfn_t mfn);
void *xen_remap_colored(mfn_t xen_fn, paddr_t xen_size);

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
