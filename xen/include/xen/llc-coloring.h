/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Last Level Cache (LLC) coloring common header
 *
 * Copyright (C) 2022 Xilinx Inc.
 *
 * Authors:
 *    Carlo Nonato <carlo.nonato@minervasys.tech>
 */
#ifndef __COLORING_H__
#define __COLORING_H__

#include <xen/sched.h>
#include <public/domctl.h>

#ifdef CONFIG_HAS_LLC_COLORING

#include <asm/llc-coloring.h>

#ifdef CONFIG_LLC_COLORING
extern bool llc_coloring_enabled;
#define llc_coloring_enabled (llc_coloring_enabled)
#endif

#endif

#ifndef llc_coloring_enabled
#define llc_coloring_enabled (false)
#endif

#define is_domain_llc_colored(d) (llc_coloring_enabled)

void domain_llc_coloring_free(struct domain *d);
void domain_dump_llc_colors(struct domain *d);

int domain_set_llc_colors_domctl(struct domain *d,
                                 const struct xen_domctl_set_llc_colors *config);

struct page_info;
unsigned int page_to_llc_color(const struct page_info *pg);
unsigned int get_nr_llc_colors(void);

#endif /* __COLORING_H__ */

/*
 * Local variables:
 * mode: C
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * tab-width: 4
 * indent-tabs-mode: nil
 * End:
 */
