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
#else
static inline void *xen_remap_colored(mfn_t xen_fn, paddr_t xen_size)
{
    return NULL;
}
static inline int domain_set_llc_colors_from_str(struct domain *d, const char *str)
{
    return -ENOSYS;
}
static inline int dom0_set_llc_colors(struct domain *d)
{
    return 0;
}
static inline bool llc_coloring_init(void)
{
    return false;
}
static inline paddr_t xen_colored_map_size(paddr_t size)
{
    return 0;
}
#endif

#ifndef llc_coloring_enabled
#define llc_coloring_enabled (false)
#endif

#ifndef CONFIG_NR_LLC_COLORS
#define CONFIG_NR_LLC_COLORS 2
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
