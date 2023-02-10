/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Last Level Cache (LLC) coloring support for ARM
 *
 * Copyright (C) 2022 Xilinx Inc.
 *
 * Authors:
 *    Luca Miccio <lucmiccio@gmail.com>
 *    Carlo Nonato <carlo.nonato@minervasys.tech>
 */
#include <xen/bitops.h>
#include <xen/errno.h>
#include <xen/guest_access.h>
#include <xen/keyhandler.h>
#include <xen/llc_coloring.h>
#include <xen/param.h>
#include <xen/types.h>
#include <xen/vmap.h>

#include <asm/processor.h>
#include <asm/sysregs.h>

/* By default Xen uses the lowest color */
#define XEN_DEFAULT_COLOR       0
#define XEN_DEFAULT_NUM_COLORS  1

bool llc_coloring_enabled;
boolean_param("llc-coloring", llc_coloring_enabled);

/* Size of an LLC way */
static unsigned int __ro_after_init llc_way_size, way_size;
size_param("llc-way-size", llc_way_size);
integer_param("way_size", way_size);
/* Number of colors available in the LLC */
static unsigned int __ro_after_init nr_colors = CONFIG_NR_LLC_COLORS;
/* Mask to extract coloring relevant bits */
static paddr_t __ro_after_init addr_col_mask;

static unsigned int __ro_after_init dom0_colors[CONFIG_NR_LLC_COLORS];
static unsigned int __ro_after_init dom0_num_colors;

static unsigned int __ro_after_init xen_colors[CONFIG_NR_LLC_COLORS];
static unsigned int __ro_after_init xen_num_colors;

/* Legacy configuration parameters for cache coloring */
bool __ro_after_init coloring_legacy;

#define addr_to_color(addr) (((addr) & addr_col_mask) >> PAGE_SHIFT)
#define addr_set_color(addr, color) (((addr) & ~addr_col_mask) \
                                     | ((color) << PAGE_SHIFT))

/*
 * Parse the coloring configuration given in the buf string, following the
 * syntax below.
 *
 * COLOR_CONFIGURATION ::= COLOR | RANGE,...,COLOR | RANGE
 * RANGE               ::= COLOR-COLOR
 *
 * Example: "0,2-6,15-16" represents the set of colors: 0,2,3,4,5,6,15,16.
 */
static int parse_color_config(const char *buf, unsigned int *colors,
                              unsigned int *num_colors)
{
    const char *s = buf;

    if ( !colors || !num_colors )
        return -EINVAL;

    *num_colors = 0;

    while ( *s != '\0' )
    {
        if ( *s != ',' )
        {
            unsigned int color, start, end;

            start = simple_strtoul(s, &s, 0);

            if ( *s == '-' )    /* Range */
            {
                s++;
                end = simple_strtoul(s, &s, 0);
            }
            else                /* Single value */
                end = start;

            if ( start > end || (end - start) > UINT_MAX - *num_colors ||
                 *num_colors + (end - start) >= nr_colors )
                return -EINVAL;
            for ( color = start; color <= end; color++ )
                colors[(*num_colors)++] = color;
        }
        else
            s++;
    }

    return *s ? -EINVAL : 0;
}

static int __init parse_xen_colors(const char *s)
{
    return parse_color_config(s, xen_colors, &xen_num_colors);
}
custom_param("xen-llc-colors", parse_xen_colors);

static int __init parse_xen_colors_legacy(const char *s)
{
    /* For legacy coloring, enable LLC by default */
    llc_coloring_enabled = true;
    coloring_legacy = true;
    return parse_color_config(s, xen_colors, &xen_num_colors);
}
custom_param("xen_colors", parse_xen_colors_legacy);

static int __init parse_dom0_colors(const char *s)
{
    return parse_color_config(s, dom0_colors, &dom0_num_colors);
}
custom_param("dom0-llc-colors", parse_dom0_colors);

static int __init parse_dom0_colors_legacy(const char *s)
{
    /* For legacy coloring, enable LLC by default */
    llc_coloring_enabled = true;
    coloring_legacy = true;
    return parse_color_config(s, dom0_colors, &dom0_num_colors);
}
custom_param("dom0_colors", parse_dom0_colors_legacy);

/* Return the LLC way size by probing the hardware */
static unsigned int __init get_llc_way_size(void)
{
    register_t ccsidr_el1;
    register_t clidr_el1 = READ_SYSREG(CLIDR_EL1);
    register_t csselr_el1 = READ_SYSREG(CSSELR_EL1);
    register_t id_aa64mmfr2_el1 = READ_SYSREG(ID_AA64MMFR2_EL1);
    uint32_t ccsidr_numsets_shift = CCSIDR_NUMSETS_SHIFT;
    uint32_t ccsidr_numsets_mask = CCSIDR_NUMSETS_MASK;
    unsigned int n, line_size, num_sets;

    for ( n = CLIDR_CTYPEn_LEVELS;
          n != 0 && !((clidr_el1 >> CLIDR_CTYPEn_SHIFT(n)) & CLIDR_CTYPEn_MASK);
          n-- );

    if ( n == 0 )
        return 0;

    WRITE_SYSREG(((n - 1) & CCSELR_LEVEL_MASK) << CCSELR_LEVEL_SHIFT,
                 CSSELR_EL1);
    isb();

    ccsidr_el1 = READ_SYSREG(CCSIDR_EL1);

    /* Arm ARM: (Log2(Number of bytes in cache line)) - 4 */
    line_size = 1 << ((ccsidr_el1 & CCSIDR_LINESIZE_MASK) + 4);

    /* If FEAT_CCIDX is enabled, CCSIDR_EL1 has a different bit layout */
    if ( (id_aa64mmfr2_el1 >> ID_AA64MMFR2_CCIDX_SHIFT) & 0x7 )
    {
        ccsidr_numsets_shift = CCSIDR_NUMSETS_SHIFT_FEAT_CCIDX;
        ccsidr_numsets_mask = CCSIDR_NUMSETS_MASK_FEAT_CCIDX;
    }
    /* Arm ARM: (Number of sets in cache) - 1 */
    num_sets = ((ccsidr_el1 >> ccsidr_numsets_shift) & ccsidr_numsets_mask) + 1;

    printk(XENLOG_INFO "LLC found: L%u (line size: %u bytes, sets num: %u)\n",
           n, line_size, num_sets);

    /* Restore value in CSSELR_EL1 */
    WRITE_SYSREG(csselr_el1, CSSELR_EL1);
    isb();

    return line_size * num_sets;
}

static bool check_colors(unsigned int *colors, unsigned int num_colors)
{
    unsigned int i;

    if ( num_colors > nr_colors )
        return false;

    for ( i = 0; i < num_colors; i++ )
        if ( colors[i] >= nr_colors ||
             (i != num_colors - 1 && colors[i] >= colors[i + 1]) )
            return false;

    return true;
}

static void print_colors(unsigned int *colors, unsigned int num_colors)
{
    unsigned int i;

    printk("[ ");
    for ( i = 0; i < num_colors; i++ )
        printk("%u ", colors[i]);
    printk("]\n");
}

static void dump_coloring_info(unsigned char key)
{
    printk("'%c' pressed -> dumping LLC coloring general info\n", key);
    printk("LLC way size: %u KiB\n", llc_way_size >> 10);
    printk("Number of LLC colors supported: %u\n", nr_colors);
    printk("Address to LLC color mask: 0x%lx\n", addr_col_mask);
    printk("Legacy LLC params: %s\n", coloring_legacy ? "true" : "false");
    printk("Xen LLC colors: ");
    print_colors(xen_colors, xen_num_colors);
}

bool __init llc_coloring_init(void)
{
    if ( way_size != 0 )
        llc_way_size = way_size + 1;

    if ( !llc_way_size && !(llc_way_size = get_llc_way_size()) )
    {
        printk(XENLOG_ERR
               "Probed LLC way size is 0 and no custom value provided\n");
        return false;
    }

    nr_colors = llc_way_size >> PAGE_SHIFT;

    if ( nr_colors < 2 || nr_colors > CONFIG_NR_LLC_COLORS )
    {
        printk(XENLOG_ERR "Number of LLC colors (%u) not in range [2, %u]\n",
               nr_colors, CONFIG_NR_LLC_COLORS);
        return false;
    }

    addr_col_mask = (nr_colors - 1) << PAGE_SHIFT;

    if ( !xen_num_colors )
    {
        printk(XENLOG_WARNING
               "Xen LLC color config not found. Using default color: %u\n",
               XEN_DEFAULT_COLOR);
        xen_colors[0] = XEN_DEFAULT_COLOR;
        xen_num_colors = XEN_DEFAULT_NUM_COLORS;
    }

    if ( !check_colors(xen_colors, xen_num_colors) )
    {
        printk(XENLOG_ERR "Bad LLC color config for Xen\n");
        return false;
    }

    register_keyhandler('K', dump_coloring_info, "dump LLC coloring info", 1);

    /* Print general coloring info at start of day */
    dump_coloring_info('K');

    return true;
}

int domain_llc_coloring_init(struct domain *d, unsigned int *colors,
                             unsigned int num_colors)
{
    unsigned int i;

    if ( is_domain_direct_mapped(d) )
    {
        printk(XENLOG_ERR
               "LLC coloring and direct mapping are incompatible (%pd)\n", d);
        return -EINVAL;
    }

    if ( !colors || num_colors == 0 )
    {
        printk(XENLOG_WARNING
               "LLC color config not found for %pd. Using default\n", d);
        colors = xmalloc_array(unsigned int, nr_colors);

        if ( !colors )
        {
            printk(XENLOG_ERR "Can't allocate LLC colors for domain %pd\n", d);
            return -ENOMEM;
        }

        num_colors = nr_colors;
        for ( i = 0; i < nr_colors; i++ )
            colors[i] = i;
    }

    d->llc_colors = colors;
    d->num_llc_colors = num_colors;

    /* Print domain coloring info at domain creation */
    domain_dump_llc_colors(d);

    if ( !check_colors(d->llc_colors, d->num_llc_colors) )
    {
        /* d->llc_colors will be freed in domain_destroy() */
        printk(XENLOG_ERR "Bad LLC color config for %pd\n", d);
        print_colors(d->llc_colors, d->num_llc_colors);
        return -EINVAL;
    }

    return 0;
}

void domain_llc_coloring_free(struct domain *d)
{
    xfree(d->llc_colors);
}

void domain_dump_llc_colors(struct domain *d)
{
    printk("Domain %pd has %u LLC colors: ", d, d->num_llc_colors);
    print_colors(d->llc_colors, d->num_llc_colors);
}

unsigned int *dom0_llc_colors(unsigned int *num_colors)
{
    unsigned int *colors = xmalloc_array(unsigned int, dom0_num_colors);

    if ( !dom0_num_colors || !colors )
        return NULL;

    memcpy(colors, dom0_colors, sizeof(unsigned int) * dom0_num_colors);
    *num_colors = dom0_num_colors;

    return colors;
}

unsigned int *llc_colors_from_guest(const struct xen_domctl_createdomain *config)
{
    unsigned int *colors = xmalloc_array(unsigned int, config->num_llc_colors);

    if ( !config->num_llc_colors || !colors )
        return NULL;

    copy_from_guest(colors, config->llc_colors, config->num_llc_colors);

    return colors;
}

unsigned int *llc_colors_from_str(const char *str, unsigned int *num_colors)
{
    unsigned int *colors = xmalloc_array(unsigned int, nr_colors);

    if ( !colors )
        panic("Can't allocate LLC colors\n");

    if ( parse_color_config(str, colors, num_colors) )
        panic("Error parsing LLC color configuration\n");

    return colors;
}

unsigned int *llc_colors_from_legacy_bitmask(struct dt_device_node *node,
                                             unsigned int *num_colors)
{
    uint32_t len, col_val;
    const uint32_t *cells;
    int cell, k, i;
    unsigned int *colors = NULL;

    *num_colors = 0;

    cells = dt_get_property(node, "colors", &len);
    if ( cells != NULL && len > 0 )
    {
        if ( !get_nr_llc_colors() )
            panic("Coloring requested but no colors configuration found!\n");

        colors = xmalloc_array(unsigned int, nr_colors);
        if ( !colors )
            panic("Unable to allocate cache colors\n");

        *num_colors = 0;
        for ( k = 0, cell = len/4 - 1; cell >= 0; cell--, k++ )
        {
            col_val = be32_to_cpup(&cells[cell]);
            if ( col_val )
            {
                /* Calculate number of bit set */
                for ( i = 0; i < 32; i++)
                {
                    if ( col_val & (1 << i) )
                        colors[(*num_colors)++] = i;
                }
            }
        }
    }

    return colors;
}

unsigned int page_to_llc_color(const struct page_info *pg)
{
    return addr_to_color(page_to_maddr(pg));
}

unsigned int get_nr_llc_colors(void)
{
    return nr_colors;
}

paddr_t xen_colored_map_size(paddr_t size)
{
    return ROUNDUP(size * nr_colors, XEN_PADDR_ALIGN);
}

mfn_t xen_colored_mfn(mfn_t mfn)
{
    paddr_t maddr = mfn_to_maddr(mfn);
    unsigned int i, color = addr_to_color(maddr);

    for( i = 0; i < xen_num_colors; i++ )
    {
        if ( color == xen_colors[i] )
            return mfn;
        else if ( color < xen_colors[i] )
            return maddr_to_mfn(addr_set_color(maddr, xen_colors[i]));
    }

    /* Jump to next color space (llc_way_size bytes) and use the first color */
    return maddr_to_mfn(addr_set_color(maddr + llc_way_size, xen_colors[0]));
}

void *xen_remap_colored(mfn_t xen_mfn, paddr_t xen_size)
{
    unsigned int i;
    void *xenmap;
    mfn_t *xen_colored_mfns = xmalloc_array(mfn_t, xen_size >> PAGE_SHIFT);

    if ( !xen_colored_mfns )
        panic("Can't allocate LLC colored MFNs\n");

    for_each_xen_colored_mfn( xen_mfn, i )
    {
        xen_colored_mfns[i] = xen_mfn;
    }

    xenmap = vmap(xen_colored_mfns, xen_size >> PAGE_SHIFT);
    xfree(xen_colored_mfns);

    return xenmap;
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
