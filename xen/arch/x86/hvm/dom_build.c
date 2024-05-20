/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * hvm/dom_build.c
 *
 * Dom builder for PVH guest.
 *
 * Copyright (C) 2017 Citrix Systems R&D
 * Copyright (C) 2024 Apertus Solutions, LLC
 */

#include <xen/acpi.h>
#include <xen/init.h>
#include <xen/types.h>

#include <acpi/actables.h>

#include <asm/bootinfo.h>
#include <asm/dom0_build.h>
#include <asm/hvm/io.h>
#include <asm/pci.h>

static void __hwdom_init pvh_setup_mmcfg(struct domain *d)
{
    unsigned int i;
    int rc;

    for ( i = 0; i < pci_mmcfg_config_num; i++ )
    {
        rc = register_vpci_mmcfg_handler(d, pci_mmcfg_config[i].address,
                                         pci_mmcfg_config[i].start_bus_number,
                                         pci_mmcfg_config[i].end_bus_number,
                                         pci_mmcfg_config[i].pci_segment);
        if ( rc )
            printk("Unable to setup MMCFG handler at %#lx for segment %u\n",
                   pci_mmcfg_config[i].address,
                   pci_mmcfg_config[i].pci_segment);
    }
}

int __init dom_construct_pvh(struct boot_domain *bd)
{
    int rc;
    struct domain *d = bd->d;

    printk(XENLOG_INFO "*** Building a PVH Dom%u ***\n", d->domain_id);

    if ( bd->kernel == NULL )
        panic("Missing kernel boot module for %pd construction\n", d);

    if ( is_hardware_domain(d) )
    {
        /*
         * MMCFG initialization must be performed before setting domain
         * permissions, as the MCFG areas must not be part of the domain IOMEM
         * accessible regions.
         */
        pvh_setup_mmcfg(d);

        /*
         * Setup permissions early so that calls to add MMIO regions to the
         * p2m as part of vPCI setup don't fail due to permission checks.
         */
        rc = dom0_setup_permissions(d);
        if ( rc )
        {
            printk("%pd unable to setup permissions: %d\n", d, rc);
            return rc;
        }
    }

    return dom0_construct_pvh(bd);
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
