/*
 * xen/include/asm-arm/coloring.h
 *
 * Coloring support for ARM
 *
 * Copyright (C) 2019 Xilinx Inc.
 *
 * Authors:
 *    Luca Miccio <lucmiccio@gmail.com>
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

#define MAX_COLORS_CELLS 4

#ifdef CONFIG_COLORING
#include <xen/sched.h>

/*
 * Amount of memory that we need to map in order to color Xen.  The value
 * depends on the maximum number of available colors of the hardware.  The
 * memory size is pessimistically calculated assuming only one color is used,
 * which means that any pages belonging to any other color has to be skipped.
 */
#define XEN_COLOR_MAP_SIZE \
	((((_end - _start) * get_max_colors())\
		+ (XEN_PADDR_ALIGN-1)) & ~(XEN_PADDR_ALIGN-1))
#define XEN_COLOR_MAP_SIZE_M (XEN_COLOR_MAP_SIZE >> 20)

bool __init coloring_init(void);

/*
 * Return physical page address that conforms to the colors selection
 * given in col_selection_mask after @param phys.
 *
 * @param phys         Physical address start.
 * @param addr_col_mask        Mask specifying the bits available for coloring.
 * @param col_selection_mask   Mask asserting the color bits to be used,
 * must not be 0.
 *
 * @return The lowest physical page address being greater or equal than
 * 'phys' and belonging to Xen color selection
 */
paddr_t next_xen_colored(paddr_t phys);

/*
 * Check colors of a given domain.
 * Return true if check passed, false otherwise.
 */
bool check_domain_colors(struct domain *d);

/*
 * Return an array with default colors selection and store the number of
 * colors in @param col_num. The array selection will be equal to the dom0
 * color configuration.
 */
uint32_t *setup_default_colors(uint32_t *col_num);

void coloring_dump_info(struct domain *d);

/*
 * Compute the color of the given page address.
 * This function should change depending on the cache architecture
 * specifications.
 */
unsigned long color_from_page(struct page_info *pg);

/* Return the maximum available number of colors supported by the hardware */
uint32_t get_max_colors(void);
#else /* !CONFIG_COLORING */
#define XEN_COLOR_MAP_SIZE (_end - _start)

static inline bool __init coloring_init(void)
{
    return true;
}

static inline void coloring_dump_info(struct domain *d)
{
    return;
}

static inline uint32_t get_max_colors(void)
{
    return 0;
}
#endif /* CONFIG_COLORING */
#endif /* !__ASM_ARM_COLORING_H__ */
