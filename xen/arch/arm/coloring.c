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

#include <asm/coloring.h>
#include <asm/io.h>

/* Minimum size required for buddy allocator to work with colored one */
unsigned long buddy_required_size __read_mostly = MB(512);

/* Number of color(s) assigned to Xen */
static uint64_t xen_col_num;
/* Coloring configuration of Xen as bitmask */
static uint64_t xen_col_mask;

/* Number of color(s) assigned to Dom0 */
static uint64_t dom0_colors_num;
/* Coloring configuration of Dom0 as bitmask */
static uint64_t dom0_colors_mask;
/* Maximum number of available color(s) */
static uint64_t col_num_max;
/* Maximum available coloring configuration as bitmask */
static uint64_t col_val_max;

static uint64_t way_size;
static uint64_t addr_col_mask;

#define CTR_LINESIZE_MASK 0x7
#define CTR_SIZE_SHIFT 13
#define CTR_SIZE_MASK 0x3FFF
#define CTR_SELECT_L2 1 << 1
#define CTR_SELECT_L3 1 << 2
#define CTR_CTYPEn_MASK 0x7
#define CTR_CTYPE2_SHIFT 3
#define CTR_CTYPE3_SHIFT 6
#define CTR_LLC_ON 1 << 2
#define CTR_LOC_SHIFT 24
#define CTR_LOC_MASK 0x7
#define CTR_LOC_L2 1 << 1
#define CTR_LOC_NOT_IMPLEMENTED 1 << 0


/* Return the way size of last level cache by asking the hardware */
static uint64_t get_llc_way_size(void)
{
    uint32_t cache_sel = READ_SYSREG64(CSSELR_EL1);
    uint32_t cache_global_info = READ_SYSREG64(CLIDR_EL1);
    uint32_t cache_info;
    uint32_t cache_line_size;
    uint32_t cache_set_num;
    uint32_t cache_sel_tmp;

    C_DEBUG("Get information on LLC\n");
    C_DEBUG("Cache CLIDR_EL1: 0x%x\n", cache_global_info);

    /* Check if at least L2 is implemented */
    if ( ((cache_global_info >> CTR_LOC_SHIFT) & CTR_LOC_MASK)
        == CTR_LOC_NOT_IMPLEMENTED )
    {
        C_DEBUG("ERROR: L2 Cache not implemented\n");
        return 0;
    }

    /* Save old value of CSSELR_EL1 */
    cache_sel_tmp = cache_sel;

    /* Get LLC index */
    if ( ((cache_global_info >> CTR_CTYPE2_SHIFT) & CTR_CTYPEn_MASK)
        == CTR_LLC_ON )
        cache_sel = CTR_SELECT_L2;
    else
        cache_sel = CTR_SELECT_L3;

    C_DEBUG("LLC selection: %u\n", cache_sel);
    /* Select the correct LLC in CSSELR_EL1 */
    WRITE_SYSREG64(cache_sel, CSSELR_EL1);

    /* Ensure write */
    isb();

    /* Get info about the LLC */
    cache_info = READ_SYSREG64(CCSIDR_EL1);

    /* ARM TRM: (Log2(Number of bytes in cache line)) - 4. */
    cache_line_size = 1 << ((cache_info & CTR_LINESIZE_MASK) + 4);
    /* ARM TRM: (Number of sets in cache) - 1 */
    cache_set_num = ((cache_info >> CTR_SIZE_SHIFT) & CTR_SIZE_MASK) + 1;

    C_DEBUG("Cache line size: %u bytes\n", cache_line_size);
    C_DEBUG("Cache sets num: %u\n", cache_set_num);

    /* Restore value in CSSELR_EL1 */
    WRITE_SYSREG64(cache_sel_tmp, CSSELR_EL1);

    /* Ensure write */
    isb();

    return (cache_line_size * cache_set_num);
}

/*
 * Return the coloring mask based on the value of @param llc_way_size.
 * This mask represents the bits in the address that can be used
 * for defining available colors.
 *
 * @param llc_way_size		Last level cache way size.
 * @return unsigned long	The coloring bitmask.
 */
static __init unsigned long calculate_addr_col_mask(unsigned int llc_way_size)
{
    unsigned long addr_col_mask = 0;
    unsigned int i;
    unsigned int low_idx, high_idx;

    low_idx = PAGE_SHIFT;
    high_idx = get_count_order(llc_way_size) - 1;

    for ( i = low_idx; i <= high_idx; i++ )
        addr_col_mask |= (1 << i);

    return addr_col_mask;
}

bool __init coloring_init(void)
{
    int i;

    C_DEBUG("Initialize XEN coloring: \n");
    /*
     * If the way size is not provided by the configuration, try to get
     * this information from hardware.
     */
    if ( !way_size )
    {
        way_size = get_llc_way_size();

        if ( !way_size )
        {
            C_DEBUG("ERROR: way size is null\n");
            return false;
        }
    }

    addr_col_mask = calculate_addr_col_mask(way_size);
    if ( !addr_col_mask )
    {
        C_DEBUG("ERROR: addr_col_mask is null\n");
        return false;
    }

    col_num_max = ((addr_col_mask >> PAGE_SHIFT) + 1);
    for ( i = 0; i < col_num_max; i++ )
        col_val_max |= (1 << i);

    C_DEBUG("Way size: 0x%lx\n", way_size);
    C_DEBUG("Color bits in address: 0x%lx\n", addr_col_mask);
    C_DEBUG("Max number of colors: %lu (0x%lx)\n", col_num_max, col_val_max);

    return true;
}

uint64_t get_max_colors(void)
{
    return col_num_max;
}

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

static int __init parse_buddy_required_size(const char *s)
{
    buddy_required_size = simple_strtoull(s, &s, 0);

    return *s ? -EINVAL : 0;
}
custom_param("buddy_size", parse_buddy_required_size);

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
