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
bool __init coloring_init(void);

/*
 * Return an array with default colors selection and store the number of
 * colors in @param col_num. The array selection will be equal to the dom0
 * color configuration.
 */
uint32_t *setup_default_colors(uint32_t *col_num);
#else /* !CONFIG_COLORING */
static inline bool __init coloring_init(void)
{
    return true;
}
#endif /* CONFIG_COLORING */
#endif /* !__ASM_ARM_COLORING_H__ */
