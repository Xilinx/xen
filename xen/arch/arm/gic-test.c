/* SPDX-License-Identifier: GPL-2.0-only */

#include <xen/delay.h>
#include <xen/init.h>
#include <xen/param.h>
#include <xen/shutdown.h>
#include <xen/irq.h>
#include <xen/const.h>
#include <asm/gic.h>

#define GIC_TEST_SGI  BIT(0, U)
#define GIC_TEST_SPI  BIT(1, U)

/*
 * gic_test: Specifies the gic test to be executed.
 * 0 = no tests are executed
 * 1 = SGI tests are executed
 * 2 = SPI routing test is executed
 */
static unsigned int __initdata gic_test = 0;
integer_param("gic-test", gic_test);

/* Chosen SPI for routing/enable test; adjust if the platform uses it. */
#define GIC_TEST_SPI_IRQ   96

static void __init gic_test_irq(int irq, void *dev_id)
{
    struct irq_desc *desc = irq_to_desc(GIC_TEST_SPI_IRQ);

    printk("CPU%u: GIC test IRQ %d received\n", smp_processor_id(), irq);
    gic_set_pending_state(desc, false);
}

/* [XenVerTestCode~~1->XenVerTestCase~arm64_gicv3_sgi~1>>XenVerTestJob] */
/*
 * CPU0: GIC_SGI_DUMP_STATE to self
 * CPU{0-N}: GIC_SGI_TEST to self
 * CPU{1-N}: GIC_SGI_TEST to CPU0
 * CPU{N}: GIC_SGI_TEST to all but self
 */
static int __init gic_self_sgi_test(void)
{
    if ( !(gic_test & GIC_TEST_SGI) )
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

/* [XenVerTestCode~~1->XenVerTestCase~arm64_gicv3_spi~1>>XenVerTestJob] */
static int __init gic_spi_routing_test(void)
{
    int rc = 0;
    struct irq_desc *desc = irq_to_desc(GIC_TEST_SPI_IRQ);

    if ( !(gic_test & GIC_TEST_SPI) )
        return 0;

    if ( smp_processor_id() != 0 )
        return 0;

    if ( !desc )
        return 0;

    rc = request_irq(GIC_TEST_SPI_IRQ, 0, gic_test_irq, "gic-test", NULL);
    if ( rc )
    {
        printk("GIC test: request_irq(%u) failed (%d)\n", GIC_TEST_SPI_IRQ, rc);
        return rc;
    }

    desc = irq_to_desc(GIC_TEST_SPI_IRQ);
    if ( !desc )
        return 0;

    printk("GIC test: Setting pending state for SPI%u on CPU%u\n",
           GIC_TEST_SPI_IRQ, smp_processor_id());
    gic_set_pending_state(desc, true);

    return 0;
}
__initcallboottest(gic_spi_routing_test);
