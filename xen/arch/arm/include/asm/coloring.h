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
#include <xen/sched.h>

#include <public/arch-arm.h>

bool __init coloring_init(void);

int domain_coloring_init(struct domain *d,
                         const struct xen_arch_domainconfig *config);
void domain_coloring_free(struct domain *d);

#else /* !CONFIG_CACHE_COLORING */

static inline bool __init coloring_init(void) { return true; }
static inline int domain_coloring_init(
    struct domain *d, const struct xen_arch_domainconfig *config) { return 0; }
static inline void domain_coloring_free(struct domain *d) {}

#endif /* CONFIG_CACHE_COLORING */
#endif /* __ASM_ARM_COLORING_H__ */
