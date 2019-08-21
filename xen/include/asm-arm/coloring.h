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

/* Colored allocator functions */
struct page_info *alloc_col_domheap_page(
	struct domain *d, unsigned int memflags);
void free_col_heap_page(struct page_info *pg);

#else /* !CONFIG_COLORING */

static bool inline __init coloring_init(void)
{
    return true;
}

static inline struct page_info *alloc_col_domheap_page(
	struct domain *d, unsigned int memflags)
{
	return NULL;
}

static inline void free_col_heap_page(struct page_info *pg)
{
	return;
}
#endif /* CONFIG_COLORING */

#endif /* !__ASM_ARM_COLORING_H__ */
