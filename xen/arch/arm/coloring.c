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

#include <asm/sysregs.h>
#include <asm/coloring.h>
#include <asm/io.h>

/* Number of color(s) assigned to Xen */
static uint32_t xen_col_num;
/* Coloring configuration of Xen as bitmask */
static uint32_t xen_col_mask[MAX_COLORS_CELLS];

/* Number of color(s) assigned to Dom0 */
static uint32_t dom0_col_num;
/* Coloring configuration of Dom0 as bitmask */
static uint32_t dom0_col_mask[MAX_COLORS_CELLS];
/* Maximum number of available color(s) */
static uint32_t max_col_num;
/* Maximum available coloring configuration as bitmask */
static uint32_t max_col_mask[MAX_COLORS_CELLS];

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

    printk(XENLOG_INFO "Get information on LLC\n");
    printk(XENLOG_INFO "Cache CLIDR_EL1: 0x%"PRIx32"\n", cache_global_info);

    /* Check if at least L2 is implemented */
    if ( ((cache_global_info >> CTR_LOC_SHIFT) & CTR_LOC_MASK)
        == CTR_LOC_NOT_IMPLEMENTED )
    {
        printk(XENLOG_ERR "ERROR: L2 Cache not implemented\n");
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

    printk(XENLOG_INFO "LLC selection: %u\n", cache_sel);
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

    printk(XENLOG_INFO "Cache line size: %u bytes\n", cache_line_size);
    printk(XENLOG_INFO "Cache sets num: %u\n", cache_set_num);

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
static __init uint64_t calculate_addr_col_mask(uint64_t llc_way_size)
{
    uint64_t addr_col_mask = 0;
    unsigned int i;
    unsigned int low_idx, high_idx;

    low_idx = PAGE_SHIFT;
    high_idx = get_count_order(llc_way_size) - 1;

    for ( i = low_idx; i <= high_idx; i++ )
        addr_col_mask |= (1 << i);

    return addr_col_mask;
}

static int copy_mask_to_list(
    uint32_t *col_mask, uint32_t *col_list, uint64_t col_num)
{
    unsigned int i, k, c;

    if ( !col_list )
        return -EINVAL;

    for ( i = 0, k = 0; i < MAX_COLORS_CELLS; i++ )
        for ( c = 0; k < col_num && c < 32; c++ )
            if ( col_mask[i] & (1 << (c + (i*32))) )
                col_list[k++] = c + (i * 32);

    return 0;
}

uint32_t *setup_default_colors(uint32_t *col_num)
{
    uint32_t *col_list;

    if ( dom0_col_num )
    {
        *col_num = dom0_col_num;
        col_list = xzalloc_array(uint32_t, dom0_col_num);
        if ( !col_list )
        {
            printk(XENLOG_ERR "setup_default_colors: Alloc failed\n");
            return NULL;
        }
        copy_mask_to_list(dom0_col_mask, col_list, dom0_col_num);
        return col_list;
    }

    return NULL;
}

bool __init coloring_init(void)
{
    int i;

    printk(XENLOG_INFO "Initialize XEN coloring: \n");
    /*
     * If the way size is not provided by the configuration, try to get
     * this information from hardware.
     */
    if ( !way_size )
    {
        way_size = get_llc_way_size();

        if ( !way_size )
        {
            printk(XENLOG_ERR "ERROR: way size is null\n");
            return false;
        }
    }

    addr_col_mask = calculate_addr_col_mask(way_size);
    if ( !addr_col_mask )
    {
        printk(XENLOG_ERR "ERROR: addr_col_mask is null\n");
        return false;
    }

    max_col_num = ((addr_col_mask >> PAGE_SHIFT) + 1);

   /*
    * If the user or the platform itself provide a way_size
    * configuration that corresponds to a number of max.
    * colors greater than the one we support, we cannot
    * continue. So the check on offset value is necessary.
    */
    if ( max_col_num > 32 * MAX_COLORS_CELLS )
    {
        printk(XENLOG_ERR "ERROR: max. color value not supported\n");
        return false;
    }

    for ( i = 0; i < max_col_num; i++ )
    {
        unsigned int offset = i / 32;

        max_col_mask[offset] |= (1 << i % 32);
    }

    printk(XENLOG_INFO "Way size: 0x%"PRIx64"\n", way_size);
    printk(XENLOG_INFO "Color bits in address: 0x%"PRIx64"\n", addr_col_mask);
    printk(XENLOG_INFO "Max number of colors: %u\n", max_col_num);

    return true;
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
    const char *buf, uint32_t *col_mask, uint32_t *col_num)
{
    int start, end, i;
    const char* s = buf;
    unsigned int offset;

    if ( !col_mask || !col_num )
        return -EINVAL;

    *col_num = 0;
    for ( i = 0; i < MAX_COLORS_CELLS; i++ )
        col_mask[i] = 0;

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
                offset = i / 32;
                if ( offset > MAX_COLORS_CELLS )
                    goto fail;

                if ( !(col_mask[offset] & (1 << i % 32)) )
                    *col_num += 1;
                col_mask[offset] |= (1 << i % 32);
            }
        }
        else
            s++;
    }

    return *s ? -EINVAL : 0;
fail:
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
    return parse_color_config(s, dom0_col_mask, &dom0_col_num);
}
custom_param("dom0_colors", parse_dom0_colors);

static int __init parse_xen_colors(const char *s)
{
    return parse_color_config(s, xen_col_mask, &xen_col_num);
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
