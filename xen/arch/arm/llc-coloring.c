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
#include <xen/keyhandler.h>
#include <xen/llc-coloring.h>
#include <xen/param.h>
#include <xen/types.h>

#include <asm/processor.h>
#include <asm/sysregs.h>

bool __ro_after_init llc_coloring_enabled;
boolean_param("llc-coloring", llc_coloring_enabled);

/* Size of an LLC way */
static unsigned int __ro_after_init llc_way_size;
size_param("llc-way-size", llc_way_size);
/* Number of colors available in the LLC */
static unsigned int __ro_after_init nr_colors = CONFIG_NR_LLC_COLORS;

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

/*
 * Local variables:
 * mode: C
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * tab-width: 4
 * indent-tabs-mode: nil
 * End:
 */
