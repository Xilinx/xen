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

#ifdef CONFIG_ARM
#include <asm/llc_coloring.h>
#endif

#ifdef CONFIG_LLC_COLORING
extern bool llc_coloring_enabled;
#else
#define llc_coloring_enabled (false)
#endif

#define is_domain_llc_colored(d) (llc_coloring_enabled)

struct page_info;

int domain_llc_coloring_init(struct domain *d, unsigned int *colors,
                             unsigned int num_colors);
void domain_llc_coloring_free(struct domain *d);
void domain_dump_llc_colors(struct domain *d);

unsigned int *llc_colors_from_guest(const struct xen_domctl_createdomain *config);

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
