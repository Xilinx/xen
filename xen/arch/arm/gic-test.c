/* SPDX-License-Identifier: GPL-2.0-only */

/* [XenVerTestCode~~1->XenVerTestCase~arm64_gicv3_sgi~1>>XenVerTestJob] */
#include <xen/delay.h>
#include <xen/init.h>
#include <xen/param.h>
#include <xen/shutdown.h>
#include <asm/gic.h>

/*
 * gic_test: Specifies the gic test to be executed.
 * 0 = no tests are executed
 * 1 = SGI tests are executed
 */
static unsigned int __initdata gic_test = 0;
integer_param("gic-test", gic_test);

/*
 * CPU0: GIC_SGI_DUMP_STATE to self
 * CPU{0-N}: GIC_SGI_TEST to self
 * CPU{1-N}: GIC_SGI_TEST to CPU0
 * CPU{N}: GIC_SGI_TEST to all but self
 */
static int __init gic_self_sgi_test(void)
{
    if ( !gic_test )
        return 0;

    printk("Sending GIC_SGI_TEST to self CPU%u\n", smp_processor_id());
    send_SGI_self(GIC_SGI_TEST);

    if ( smp_processor_id() == 0 )
    {
        printk("Sending GIC_SGI_DUMP_STATE to CPU0\n");
        smp_send_state_dump(0);

        return 0;
    }

    printk("Sending GIC_SGI_TEST to CPU0 from CPU%u\n", smp_processor_id());
    send_SGI_one(0, GIC_SGI_TEST);

    /* Execute this test only from the last core */
    if ( smp_processor_id() == (smp_get_max_cpus() - 1) )
    {
        printk("Sending GIC_SGI_TEST to all except CPU%u\n", smp_processor_id());
        send_SGI_allbutself(GIC_SGI_TEST);
    }

    return 0;

}
__initcallboottest(gic_self_sgi_test);
