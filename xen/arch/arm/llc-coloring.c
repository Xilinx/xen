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
#include <xen/errno.h>
#include <xen/guest_access.h>
#include <xen/keyhandler.h>
#include <xen/llc-coloring.h>
#include <xen/param.h>
#include <xen/types.h>

#include <asm/processor.h>
#include <asm/sysregs.h>

#define XEN_DEFAULT_COLOR       0
#define XEN_DEFAULT_NUM_COLORS  1

bool __ro_after_init llc_coloring_enabled;
boolean_param("llc-coloring", llc_coloring_enabled);

/* Size of an LLC way */
static unsigned int __ro_after_init llc_way_size;
size_param("llc-way-size", llc_way_size);
/* Number of colors available in the LLC */
static unsigned int __ro_after_init nr_colors = CONFIG_NR_LLC_COLORS;

static unsigned int __ro_after_init dom0_colors[CONFIG_NR_LLC_COLORS];
static unsigned int __ro_after_init dom0_num_colors;

static unsigned int __ro_after_init xen_colors[CONFIG_NR_LLC_COLORS];
static unsigned int __ro_after_init xen_num_colors;

#define mfn_color_mask              (nr_colors - 1)
#define mfn_to_color(mfn)           (mfn_x(mfn) & mfn_color_mask)

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

static int __init parse_dom0_colors(const char *s)
{
    return parse_color_config(s, dom0_colors, &dom0_num_colors);
}
custom_param("dom0-llc-colors", parse_dom0_colors);

static int __init parse_xen_colors(const char *s)
{
    return parse_color_config(s, xen_colors, &xen_num_colors);
}
custom_param("xen-llc-colors", parse_xen_colors);

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

static void print_colors(unsigned int *colors, unsigned int num_colors)
{
    unsigned int i;

    printk("{ ");
    for ( i = 0; i < num_colors; i++ ) {
        unsigned int start = colors[i], end = colors[i];

        printk("%u", start);

        for ( ;
              i < num_colors - 1 && colors[i] + 1 == colors[i + 1];
              i++, end++ );

        if ( start != end )
            printk("-%u", end);

        if ( i < num_colors - 1 )
            printk(", ");
    }
    printk(" }\n");
}

static void dump_coloring_info(unsigned char key)
{
    printk("'%c' pressed -> dumping LLC coloring general info\n", key);
    printk("LLC way size: %u KiB\n", llc_way_size >> 10);
    printk("Number of LLC colors supported: %u\n", nr_colors);
    printk("Xen has %u LLC colors: ", xen_num_colors);
    print_colors(xen_colors, xen_num_colors);
}

static bool check_colors(unsigned int *colors, unsigned int num_colors)
{
    unsigned int i;

    if ( num_colors > nr_colors )
    {
        printk(XENLOG_ERR "Number of LLC colors requested > %u\n", nr_colors);
        return false;
    }

    for ( i = 0; i < num_colors; i++ )
    {
        if ( colors[i] >= nr_colors )
        {
            printk(XENLOG_ERR "LLC color %u >= %u\n", colors[i], nr_colors);
            return false;
        }
    }

    return true;
}

bool __init llc_coloring_init(void)
{
    if ( !llc_way_size && !(llc_way_size = get_llc_way_size()) )
    {
        printk(XENLOG_ERR
               "Probed LLC way size is 0 and no custom value provided\n");
        return false;
    }

    /*
     * The maximum number of colors must be a power of 2 in order to correctly
     * map them to bits of an address, so also the LLC way size must be so.
     */
    if ( llc_way_size & (llc_way_size - 1) )
    {
        printk(XENLOG_WARNING "LLC way size (%u) isn't a power of 2.\n",
               llc_way_size);
        llc_way_size = 1U << flsl(llc_way_size);
        printk(XENLOG_WARNING
               "Using %u instead. Performances will be suboptimal\n",
               llc_way_size);
    }

    nr_colors = llc_way_size >> PAGE_SHIFT;

    if ( nr_colors < 2 || nr_colors > CONFIG_NR_LLC_COLORS )
    {
        printk(XENLOG_ERR "Number of LLC colors (%u) not in range [2, %u]\n",
               nr_colors, CONFIG_NR_LLC_COLORS);
        return false;
    }

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

    return true;
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

static int domain_alloc_colors(struct domain *d, unsigned int num_colors)
{
    d->num_llc_colors = num_colors;

    if ( !num_colors )
        return 0;

    d->llc_colors = xmalloc_array(unsigned int, num_colors);
    if ( !d->llc_colors )
    {
        printk("Can't allocate LLC colors for domain %pd\n", d);
        return -1;
    }

    return 0;
}

static int domain_check_colors(struct domain *d)
{
    unsigned int i;

    if ( !d->num_llc_colors )
    {
        printk(XENLOG_WARNING
               "LLC color config not found for %pd. Using default\n", d);
        if ( domain_alloc_colors(d, nr_colors) )
            return -ENOMEM;
        for ( i = 0; i < nr_colors; i++ )
            d->llc_colors[i] = i;
    }
    else if ( !check_colors(d->llc_colors, d->num_llc_colors) )
    {
        printk(XENLOG_ERR "Bad LLC color config for %pd\n", d);
        return -EINVAL;
    }

    return 0;
}

int dom0_set_llc_colors(struct domain *d)
{
    if ( domain_alloc_colors(d, dom0_num_colors) )
        return -ENOMEM;

    memcpy(d->llc_colors, dom0_colors, sizeof(unsigned int) * dom0_num_colors);

    return domain_check_colors(d);
}

int domain_set_llc_colors_domctl(struct domain *d,
                                 const struct xen_domctl_set_llc_colors *config)
{
    if ( d->num_llc_colors )
        return -EEXIST;

    if ( domain_alloc_colors(d, config->num_llc_colors) )
        return -ENOMEM;

    if ( copy_from_guest(d->llc_colors, config->llc_colors,
                         config->num_llc_colors) )
        return -EFAULT;

    return domain_check_colors(d);
}

int domain_set_llc_colors_from_str(struct domain *d, const char *str)
{
    int err;

    if ( domain_alloc_colors(d, nr_colors) )
        return -ENOMEM;

    err = parse_color_config(str, d->llc_colors, &d->num_llc_colors);
    if ( err )
    {
        printk(XENLOG_ERR "Error parsing LLC color configuration.");
        return err;
    }

    return domain_check_colors(d);
}

unsigned int page_to_llc_color(const struct page_info *pg)
{
    return mfn_to_color(page_to_mfn(pg));
}

unsigned int get_nr_llc_colors(void)
{
    return nr_colors;
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
