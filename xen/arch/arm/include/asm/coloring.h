/*
 * xen/arm/include/asm/coloring.h
 *
 * Coloring support for ARM
 *
 * Copyright (C) 2019 Xilinx Inc.
 *
 * Authors:
 *    Luca Miccio <lucmiccio@gmail.com>
 *    Carlo Nonato <carlo.nonato@minervasys.tech>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */
#ifndef __ASM_ARM_COLORING_H__
#define __ASM_ARM_COLORING_H__

#ifdef CONFIG_CACHE_COLORING

#include <xen/init.h>
#include <xen/lib.h>
#include <xen/sched.h>

#include <public/arch-arm.h>

/*
 * Amount of memory that we need to map in order to color Xen. The value
 * depends on the maximum number of available colors of the hardware. The
 * memory size is pessimistically calculated assuming only one color is used,
 * which means that any pages belonging to any other color has to be skipped.
 */
#define XEN_COLOR_MAP_SIZE \
    ROUNDUP((_end - _start) * get_max_colors(), XEN_PADDR_ALIGN)

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

struct page_info;

bool __init coloring_init(void);

int domain_coloring_init(struct domain *d,
                         const struct xen_arch_domainconfig *config);
void domain_coloring_free(struct domain *d);
void domain_dump_coloring_info(struct domain *d);

void prepare_color_domain_config(struct xen_arch_domainconfig *config,
                                 const char *colors_str);

unsigned int page_to_color(const struct page_info *pg);

unsigned int get_max_colors(void);

mfn_t xen_colored_mfn(mfn_t mfn);
void *xen_remap_colored(mfn_t xen_fn, paddr_t xen_size);

#else /* !CONFIG_CACHE_COLORING */

#define XEN_COLOR_MAP_SIZE (_end - _start)

static inline bool __init coloring_init(void) { return true; }
static inline int domain_coloring_init(
    struct domain *d, const struct xen_arch_domainconfig *config) { return 0; }
static inline void domain_coloring_free(struct domain *d) {}
static inline void domain_dump_coloring_info(struct domain *d) {}
static inline void prepare_color_domain_config(
    struct xen_arch_domainconfig *config, const char *colors_str) {}
static inline void *xen_remap_colored(mfn_t xen_fn, paddr_t xen_size)
{
    return NULL;
}

#endif /* CONFIG_CACHE_COLORING */
#endif /* __ASM_ARM_COLORING_H__ */
