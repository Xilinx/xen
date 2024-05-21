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
#include <xen/iommu.h>
#include <xen/init.h>
#include <xen/softirq.h>
#include <xen/types.h>

#include <acpi/actables.h>

#include <public/hvm/hvm_vcpu.h>

#include <asm/bootinfo.h>
#include <asm/dom0_build.h>
#include <asm/domain-builder.h>
#include <asm/hvm/io.h>
#include <asm/paging.h>
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

static void __init pvh_init_p2m(struct boot_domain *bd)
{
    unsigned long nr_pages = dom_compute_nr_pages(bd, NULL);
    unsigned long paging_pages = dom_paging_pages(bd->d, nr_pages);
    bool preempted;

    dom0_pvh_setup_e820(bd->d, nr_pages);
    do {
        preempted = false;
        paging_set_allocation(bd->d, paging_pages, &preempted);
        process_pending_softirqs();
    } while ( preempted );
}

int __init pvh_setup_cpus(struct domain *d, paddr_t entry, paddr_t start_info)
{
    struct vcpu *v = d->vcpu[0];
    int rc;
    /*
     * This sets the vCPU state according to the state described in
     * docs/misc/pvh.pandoc.
     */
    vcpu_hvm_context_t cpu_ctx = {
        .mode = VCPU_HVM_MODE_32B,
        .cpu_regs.x86_32.ebx = start_info,
        .cpu_regs.x86_32.eip = entry,
        .cpu_regs.x86_32.cr0 = X86_CR0_PE | X86_CR0_ET,
        .cpu_regs.x86_32.cs_limit = ~0u,
        .cpu_regs.x86_32.ds_limit = ~0u,
        .cpu_regs.x86_32.es_limit = ~0u,
        .cpu_regs.x86_32.ss_limit = ~0u,
        .cpu_regs.x86_32.tr_limit = 0x67,
        .cpu_regs.x86_32.cs_ar = 0xc9b,
        .cpu_regs.x86_32.ds_ar = 0xc93,
        .cpu_regs.x86_32.es_ar = 0xc93,
        .cpu_regs.x86_32.ss_ar = 0xc93,
        .cpu_regs.x86_32.tr_ar = 0x8b,
    };

    alloc_dom_vcpus(d);

    rc = arch_set_info_hvm_guest(v, &cpu_ctx);
    if ( rc )
    {
        printk("Unable to setup Dom%u BSP context: %d\n", d->domain_id, rc);
        return rc;
    }

    update_domain_wallclock_time(d);

    v->is_initialised = 1;
    clear_bit(_VPF_down, &v->pause_flags);

    return 0;
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

    /*
     * Craft domain physical memory map and set the paging allocation. This
     * must be done before the iommu initializion, since iommu initialization
     * code will likely add mappings required by devices to the p2m (ie:
     * RMRRs).
     */
    pvh_init_p2m(bd);

    if ( is_hardware_domain(bd->d) )
        iommu_hwdom_init(bd->d);

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
