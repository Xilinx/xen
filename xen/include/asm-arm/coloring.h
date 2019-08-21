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

#include <xen/lib.h>
#include <xen/sched.h>

/* Logging utilities */
#if defined(CONFIG_COLORING) && defined(CONFIG_COLORING_DEBUG)
#define C_DEBUG(fmt, args...) \
	printk(fmt, ##args)
#else
#define C_DEBUG(fmt, args...) { }
#endif
#ifdef CONFIG_COLORING
bool __init coloring_init(void);

/* 
 * Compute the color of the given page address.
 * This function should change depending on the cache architecture
 * specifications. Currently only ARMv8 is supported and implemented.
 */
unsigned long color_from_page(struct page_info *pg);

/* Return the maximum available number of colors supported by the hardware */
uint64_t get_max_colors(void);

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
uint32_t *setup_default_colors(unsigned int *col_num);

/* Colored allocator functions */
bool init_col_heap_pages(struct page_info *pg, unsigned long nr_pages);
struct page_info *alloc_col_domheap_page(
	struct domain *d, unsigned int memflags);
void free_col_heap_page(struct page_info *pg);

#else /* !CONFIG_COLORING */

static bool inline __init coloring_init(void)
{
    return true;
}

static inline bool init_col_heap_pages(
	struct page_info *pg, unsigned long nr_pages)
{
	return false;
}

static inline struct page_info *alloc_col_domheap_page(
	struct domain *d, unsigned int memflags)
{
	return NULL;
}

static inline uint64_t get_max_colors(void)
{
    return 0;
}

static inline void free_col_heap_page(struct page_info *pg)
{
	return;
}
#endif /* CONFIG_COLORING */

#endif /* !__ASM_ARM_COLORING_H__ */
