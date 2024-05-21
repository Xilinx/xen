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
#include <xen/config.h>
#include <xen/iommu.h>
#include <xen/init.h>
#include <xen/softirq.h>
#include <xen/types.h>

#include <acpi/actables.h>

#include <public/hvm/e820.h>
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

static unsigned long __init hvm_size_acpi_madt(struct domain *d)
{
    unsigned long size = sizeof(struct acpi_table_madt);

    size += sizeof(struct acpi_madt_local_apic) * d->max_vcpus;

    return size;
}

static unsigned long __init hvm_size_acpi_xsdt(struct domain *d)
{
    unsigned long size = sizeof(struct acpi_table_xsdt);
    /*
     * Start count at 0 because struct acpi_table_xsdt has one array
     * slot already.
     */
    unsigned int extra_tables = 0; /* MADT */

    /*
     * No need to add or subtract anything because struct acpi_table_xsdt
     * includes one array slot already.
     */
    size += extra_tables * sizeof(uint64_t);

    return size;
}

static unsigned long __init hvm_size_acpi_region(struct domain *d)
{
    unsigned long size = sizeof(struct acpi_table_rsdp);

    size += hvm_size_acpi_xsdt(d);
    size += hvm_size_acpi_madt(d);

    return ROUNDUP(size, PAGE_SIZE);
}

/* From xenguest lib */
#define END_SPECIAL_REGION   0xff000U
#define NR_SPECIAL_PAGES     8
#define START_SPECIAL_REGION (END_SPECIAL_REGION - NR_SPECIAL_PAGES)

#define SPECIALPAGE_PAGING   0
#define SPECIALPAGE_ACCESS   1
#define SPECIALPAGE_SHARING  2
#define SPECIALPAGE_BUFIOREQ 3
#define SPECIALPAGE_XENSTORE 4
#define SPECIALPAGE_IOREQ    5
#define SPECIALPAGE_IDENT_PT 6
#define SPECIALPAGE_CONSOLE  7
#define special_pfn(x)       (START_SPECIAL_REGION + (x))

/*
 * Allocation scheme, derived from xenlight/xenguest:
 *
 *                                  |  <4G MMIO Hole  |
 * [ Low Mem ][ RDM Mem ][ >1M Mem ][ ACPI ][ Special ][ High Mem ]
 *
 */
static void __init hvm_setup_e820(struct domain *d, unsigned long nr_pages)
{
    const uint32_t lowmem_reserved_base = 0xA0000;
    unsigned long low_pages, ext_pages, mmio_pages, acpi_pages;
    unsigned long page_count = 0, high_pages = 0;
    unsigned long max_ext_pages = PFN_DOWN(HVM_BELOW_4G_MMIO_START - MB(1));
    unsigned nr = 0, e820_entries = 5;

    /* low pages: below 1MB */
    low_pages = lowmem_reserved_base >> PAGE_SHIFT;
    if ( low_pages > nr_pages )
        panic("Insufficient memory for HVM/PVH domain (%pd)\n", d);

    acpi_pages = hvm_size_acpi_region(d) >> PAGE_SHIFT;
    mmio_pages = acpi_pages + NR_SPECIAL_PAGES;

    /* ext pages: from 1MB to mmio hole */
    ext_pages = nr_pages - (PFN_DOWN(MB(1)) + mmio_pages);
    if ( ext_pages > max_ext_pages )
        ext_pages = max_ext_pages;

    /* high pages: above 4GB */
    if ( nr_pages > (PFN_DOWN(MB(1)) + mmio_pages + ext_pages) )
        high_pages = nr_pages - (PFN_DOWN(MB(1)) + mmio_pages + ext_pages);

    /* If we should have a highmem range, add one more e820 entry */
    if ( high_pages )
        e820_entries++;

    ASSERT(e820_entries < E820MAX);

    d->arch.e820 = xzalloc_array(struct e820entry, e820_entries);
    if ( !d->arch.e820 )
        panic("Unable to allocate memory for boot domain e820 map\n");

    /* usable: Low memory */
    d->arch.e820[nr].addr = 0;
    d->arch.e820[nr].size = lowmem_reserved_base;
    d->arch.e820[nr].type = E820_RAM;
    page_count += d->arch.e820[nr].size >> PAGE_SHIFT;
    nr++;

    /* reserved: lowmem reserved device memory */
    d->arch.e820[nr].addr = lowmem_reserved_base;
    d->arch.e820[nr].size = MB(1) - lowmem_reserved_base;
    d->arch.e820[nr].type = E820_RESERVED;
    page_count += d->arch.e820[nr].size >> PAGE_SHIFT; /* populated for OVMF */
    nr++;

    /* usable: extended memory from 1MB */
    d->arch.e820[nr].addr = MB(1);
    d->arch.e820[nr].size = ext_pages << PAGE_SHIFT;
    d->arch.e820[nr].type = E820_RAM;
    page_count += d->arch.e820[nr].size >> PAGE_SHIFT;
    nr++;

    /* reserved: ACPI entry, ACPI_INFO_PHYSICAL_ADDRESS */
    d->arch.e820[nr].addr = 0xFC000000U;
    d->arch.e820[nr].size = acpi_pages << PAGE_SHIFT;
    d->arch.e820[nr].type = E820_ACPI;
    page_count += d->arch.e820[nr].size >> PAGE_SHIFT;
    nr++;

    /* reserved: HVM special pages, X86_HVM_END_SPECIAL_REGION */
    d->arch.e820[nr].addr = START_SPECIAL_REGION << PAGE_SHIFT;
    d->arch.e820[nr].size = NR_SPECIAL_PAGES << PAGE_SHIFT;
    d->arch.e820[nr].type = E820_RESERVED;
    page_count += d->arch.e820[nr].size >> PAGE_SHIFT;
    nr++;

    /* usable: highmem */
    if ( high_pages )
    {
        d->arch.e820[nr].addr = GB(4);
        d->arch.e820[nr].size = high_pages << PAGE_SHIFT;
        d->arch.e820[nr].type = E820_RAM;
        page_count += d->arch.e820[nr].size >> PAGE_SHIFT;
        nr++;
    }

    d->arch.nr_e820 = nr;

    ASSERT(nr == e820_entries);
    ASSERT(nr_pages == page_count);
}

static void __init pvh_init_p2m(struct boot_domain *bd)
{
    unsigned long nr_pages = dom_compute_nr_pages(bd, NULL);
    unsigned long paging_pages = dom_paging_pages(bd->d, nr_pages);
    bool preempted;

    if ( bd->create_flags & CDF_hardware )
        dom0_pvh_setup_e820(bd->d, nr_pages);
    else
        hvm_setup_e820(bd->d, nr_pages);

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
