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

bool __init coloring_init(void);

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
#else /* !CONFIG_COLORING */
static inline bool __init coloring_init(void)
{
    return true;
}

static inline void coloring_dump_info(struct domain *d)
{
    return;
}
#endif /* CONFIG_COLORING */
#endif /* !__ASM_ARM_COLORING_H__ */
