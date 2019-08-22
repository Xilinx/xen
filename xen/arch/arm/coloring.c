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

/* By default Xen uses the lowestmost color */
#define XEN_COLOR_DEFAULT_MASK 0x0001
#define XEN_COLOR_DEFAULT_NUM 1
/* Current maximum useful colors */
#define MAX_XEN_COLOR   128

/* Minimum size required for buddy allocator to work with colored one */
unsigned long buddy_required_size __read_mostly = MB(64);

/* Number of color(s) assigned to Xen */
static uint64_t xen_col_num;
/* Coloring configuration of Xen as bitmask */
static uint64_t xen_col_mask;
/* Xen colors IDs */
static uint32_t xen_col_list[MAX_XEN_COLOR];

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

static int copy_mask_to_list(
    uint64_t col_val, uint32_t *col_list, uint64_t col_num)
{
    unsigned int i, k;

    if ( !col_list )
        return -EINVAL;

    for ( i = 0, k = 0; k < col_num; i++ )
        if (col_val & (1 << i))
            col_list[k++] = i;

    return 0;
}

bool __init coloring_init(void)
{
    int i, rc;

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

    /* Clean Xen color array with default color value */
    memset(xen_col_list, 0, sizeof(uint32_t) *  MAX_XEN_COLOR);

    if ( !xen_col_num )
    {
        xen_col_mask = XEN_COLOR_DEFAULT_MASK;
        xen_col_num = XEN_COLOR_DEFAULT_NUM;
        C_DEBUG("Xen color configuration not found. Using default\n");
    }

    C_DEBUG("Xen color configuration: 0x%lx\n", xen_col_mask);
    rc = copy_mask_to_list(xen_col_mask, xen_col_list, xen_col_num);

    if ( rc )
        return false;

    return true;
}

bool check_domain_colors(struct domain *d)
{
    int i;
    bool ret = false;
	
	if ( !d )
		return ret;

    if ( d->max_colors > col_num_max )
        return ret;

    for ( i = 0; i < d->max_colors; i++ )
        ret |= (d->colors[i] > (col_num_max - 1));

    return !ret;
}

uint32_t *setup_default_colors(unsigned int *col_num)
{
    uint32_t *col_list;

    if ( dom0_colors_num )
    {
        *col_num = dom0_colors_num;
        col_list = xzalloc_array(uint32_t, dom0_colors_num);
        if ( !col_list )
        {
            C_DEBUG("setup_default_colors: Alloc failed\n");
            return NULL;
        }
        copy_mask_to_list(dom0_colors_mask, col_list, dom0_colors_num);
        return col_list;
    }

    return NULL;
}

paddr_t next_xen_colored(paddr_t phys)
{
    unsigned int i;
    unsigned int col_next_number = 0;
    unsigned int col_cur_number = (phys & addr_col_mask) >> PAGE_SHIFT;
    int overrun = 0;
    paddr_t ret;

    /*
     * Check if address color conforms to Xen selection. If it does, return
     * the address as is.
     */
    for( i = 0; i < xen_col_num; i++)
        if ( col_cur_number == xen_col_list[i] )
            return phys;

    /* Find next col */
    for( i = xen_col_num -1 ; i >= 0; i--)
    {
        if ( col_cur_number > xen_col_list[i])
        {
            /* Need to start to first element and add a way_size */
            if ( i == (xen_col_num - 1) )
            {
                col_next_number = xen_col_list[0];
                overrun = 1;
            }
            else
            {
                col_next_number = xen_col_list[i+1];
                overrun = 0;
            }
            break;
        }
    }

    /* Align phys to way_size */
    ret = phys - (PAGE_SIZE * col_cur_number);
    /* Add the offset based on color selection*/
    ret += (PAGE_SIZE * (col_next_number)) + (way_size*overrun);
    return ret;
}

/**
 * Compute color id from the page @param pg.
 * Page size determines the lowest available bit, while add_col_mask is used to
 * select the rest.
 *
 * @param pg              Page address
 * @return unsigned long  Color id
 */
unsigned long color_from_page(struct page_info *pg)
{
  return ((addr_col_mask & page_to_maddr(pg)) >> PAGE_SHIFT);
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

void coloring_dump_info(struct domain *d)
{
    int i;

    printk("Domain %d has %u color(s) [ ", d->domain_id, d->max_colors);
    for ( i = 0; i < d->max_colors; i++ )
    {
        printk("%"PRIu32" ", d->colors[i]);
    }
    printk("]\n");
}

/*
 * Local variables:
 * mode: C
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * tab-width: 4
 * indent-tabs-mode: nil
 * End:
 */
