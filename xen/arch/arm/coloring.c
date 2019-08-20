/*
 * xen/arch/arm/coloring.c
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
#include <xen/init.h>
#include <xen/types.h>
#include <xen/lib.h>
#include <xen/errno.h>

/* Number of color(s) assigned to Xen */
static uint64_t xen_col_num;
/* Coloring configuration of Xen as bitmask */
static uint64_t xen_col_mask;

/* Number of color(s) assigned to Dom0 */
static uint64_t dom0_colors_num;
/* Coloring configuration of Dom0 as bitmask */
static uint64_t dom0_colors_mask;

static uint64_t way_size;

/*************************
 * PARSING COLORING BOOTARGS
 */

/*
 * Parse the coloring configuration given in the buf string, following the
 * syntax below, and store the number of colors and a corresponding mask in
 * the last two given pointers.
 *
 * COLOR_CONFIGURATION ::= RANGE,...,RANGE
 * RANGE               ::= COLOR-COLOR
 *
 * Example: "2-6,15-16" represents the set of colors: 2,3,4,5,6,15,16.
 */
static int parse_color_config(
    const char *buf, uint64_t *mask, uint64_t *color_num)
{
    int start, end, i;
    const char* s = buf;

    if ( !mask || !color_num )
        return -EINVAL;

    *mask = *color_num = 0;

    while ( *s != '\0' )
    {
        if ( *s != ',' )
        {
            start = simple_strtoul(s, &s, 0);

            /* Ranges are hyphen-separated */
            if ( *s != '-' )
                goto fail;
            s++;

            end = simple_strtoul(s, &s, 0);

            for ( i = start; i <= end; i++ )
            {
                if ( !(*mask & ((1 << i))) )
                    *color_num += 1;
                *mask |= (1 << i);
            }
        }
        else
            s++;
    }

    return *s ? -EINVAL : 0;
fail:
    *mask = *color_num = 0;
    return -EINVAL;
}

static int __init parse_way_size(const char *s)
{
    way_size = simple_strtoull(s, &s, 0);

    return *s ? -EINVAL : 0;
}
custom_param("way_size", parse_way_size);

static int __init parse_dom0_colors(const char *s)
{
    return parse_color_config(s, &dom0_colors_mask, &dom0_colors_num);
}
custom_param("dom0_colors", parse_dom0_colors);

static int __init parse_xen_colors(const char *s)
{
    return parse_color_config(s, &xen_col_mask, &xen_col_num);
}
custom_param("xen_colors", parse_xen_colors);

/*
 * Local variables:
 * mode: C
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * tab-width: 4
 * indent-tabs-mode: nil
 * End:
 */
