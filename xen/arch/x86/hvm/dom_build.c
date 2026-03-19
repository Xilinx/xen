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
#include <xen/event.h>
#include <xen/grant_table.h>
#include <xen/iocap.h>
#include <xen/iommu.h>
#include <xen/init.h>
#include <xen/softirq.h>
#include <xen/types.h>
#include <xen/unaligned.h>
#include <xen/fdt-virtio.h>

#include <acpi/actables.h>

#include <public/arch-x86/hvm/start_info.h>
#include <public/hvm/e820.h>
#include <public/hvm/hvm_vcpu.h>
#include <public/hvm/params.h>
#include <public/io/console.h>
#include <public/io/xs_wire.h>

#include <asm/bootinfo.h>
#include <asm/bzimage.h>
#include <asm/dom0_build.h>
#include <asm/domain-builder.h>
#include <asm/hvm/io.h>
#include <asm/hvm/support.h>
#include <asm/p2m.h>
#include <asm/paging.h>
#include <asm/pci.h>

#define ACPI_OEM_ID             "XenIn"
#define ACPI_OEM_TABLE_ID       "PVH"
#define ACPI_OEM_REVISION       0

#define ACPI_CREATOR_ID         "INTL"
#define ACPI_CREATOR_REVISION   0

#define ACPI_2_0_RSDP_REVISION 0x02
#define ACPI_2_0_MADT_REVISION 0x02
#define ACPI_2_0_XSDT_REVISION 0x01
#define ACPI_2_0_FACS_VERSION 0x01

static const struct acpi_table_header xsdt_hdr = {
    .signature = ACPI_SIG_XSDT,
    .length = sizeof(struct acpi_table_header),
    .revision = ACPI_2_0_XSDT_REVISION,
    .oem_id = ACPI_OEM_ID,
    .oem_table_id = ACPI_OEM_TABLE_ID,
    .oem_revision = ACPI_OEM_REVISION,
    .asl_compiler_id = ACPI_CREATOR_ID,
    .asl_compiler_revision = ACPI_CREATOR_REVISION,
};

static const struct acpi_table_header madt_hdr = {
    .signature = ACPI_SIG_MADT,
    .length = sizeof(struct acpi_table_header),
    .revision = ACPI_2_0_MADT_REVISION,
    .oem_id = ACPI_OEM_ID,
    .oem_table_id = ACPI_OEM_TABLE_ID,
    .oem_revision = ACPI_OEM_REVISION,
    .asl_compiler_id = ACPI_CREATOR_ID,
    .asl_compiler_revision = ACPI_CREATOR_REVISION,
};

static const struct acpi_table_header mcfg_hdr = {
    .signature = ACPI_SIG_MCFG,
    .length = 0,
    .revision = 1,
    .oem_id = ACPI_OEM_ID,
    .oem_table_id = ACPI_OEM_TABLE_ID,
    .oem_revision = ACPI_OEM_REVISION,
    .asl_compiler_id = ACPI_CREATOR_ID,
    .asl_compiler_revision = ACPI_CREATOR_REVISION,
};

static const struct acpi_table_header fadt_hdr = {
    .signature = ACPI_SIG_FADT,
    .revision = 5,
    .oem_id = ACPI_OEM_ID,
    .oem_table_id = ACPI_OEM_TABLE_ID,
    .oem_revision = ACPI_OEM_REVISION,
    .asl_compiler_id = ACPI_CREATOR_ID,
    .asl_compiler_revision = ACPI_CREATOR_REVISION,
};

static const struct acpi_table_rsdp rsdp_xen = {
    .signature = ACPI_SIG_RSDP,
    .revision = ACPI_2_0_RSDP_REVISION,
    .length = sizeof(struct acpi_table_rsdp),
    .oem_id = ACPI_OEM_ID,
};

static const struct acpi_table_facs facs_xen = {
    .signature = ACPI_SIG_FACS,
    .length    = sizeof(struct acpi_table_facs),
    .version   = ACPI_2_0_FACS_VERSION
};

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

    if ( has_vioapic(d) )
    {
        size += sizeof(struct acpi_madt_io_apic) * d->arch.hvm.nr_vioapics;
        size += sizeof(struct acpi_madt_interrupt_override);
    }

    return size;
}

extern unsigned char dsdt_pvh[];
extern int dsdt_pvh_len;

static unsigned long __init hvm_size_acpi_dsdt(struct boot_domain *bd)
{
    if ( bd->acpi_dsdt )
        return bd->acpi_dsdt->size;

    return dsdt_pvh_len;
}

static unsigned long __init hvm_size_acpi_facs(struct domain *d)
{
    return sizeof(struct acpi_table_facs);
}

static unsigned long __init hvm_size_acpi_fadt(struct domain *d)
{
    return sizeof(struct acpi_table_fadt);
}

static unsigned long __init hvm_size_acpi_mcfg(struct domain *d)
{
    unsigned long size = sizeof(struct acpi_table_mcfg);

    size += sizeof(struct acpi_mcfg_allocation);

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

    extra_tables++; /* FADT */
    extra_tables++; /* MCFG */

    /*
     * No need to add or subtract anything because struct acpi_table_xsdt
     * includes one array slot already.
     */
    size += extra_tables * sizeof(uint64_t);

    return size;
}

static unsigned long __init hvm_size_acpi_region(struct boot_domain *bd)
{
    struct domain *d = bd->d;
    /* First page is used for ACPI info */
    unsigned long size = PAGE_SIZE;

    size += sizeof(struct acpi_table_rsdp);
    size += hvm_size_acpi_xsdt(d);
    size += hvm_size_acpi_madt(d);
    size += hvm_size_acpi_dsdt(bd);
    size += hvm_size_acpi_facs(d);
    size += hvm_size_acpi_fadt(d);
    size += hvm_size_acpi_mcfg(d);

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
static void __init hvm_setup_e820(struct boot_domain *bd,
                                  unsigned long nr_pages)
{
    struct domain *d = bd->d;
    const uint32_t lowmem_reserved_base = 0xA0000;
    unsigned long low_pages, ext_pages, acpi_pages;
    unsigned long page_count = 0, high_pages = 0;
    unsigned long max_ext_pages, mmio_start = HVM_BELOW_4G_MMIO_START;
    unsigned nr = 0, e820_entries = 5;

    /* lowmem is bound by PCI Segment 1 Root bridge ECAM base */
    mmio_start = min_t(unsigned long, mmio_start, PCI1_ECAM_BASE);
    e820_entries++;

    /* low pages: below 1MB */
    low_pages = lowmem_reserved_base >> PAGE_SHIFT;
    if ( low_pages > nr_pages )
        panic("Insufficient memory for HVM/PVH domain (%pd)\n", d);

    acpi_pages = hvm_size_acpi_region(bd) >> PAGE_SHIFT;

    /* ext pages: from 1MB to mmio hole */
    ext_pages = nr_pages - PFN_DOWN(MB(1));
    max_ext_pages = PFN_DOWN(mmio_start - MB(1));
    if ( ext_pages > max_ext_pages )
        ext_pages = max_ext_pages;

    /* high pages: above 4GB */
    if ( nr_pages > (PFN_DOWN(MB(1)) + ext_pages) )
        high_pages = nr_pages - (PFN_DOWN(MB(1)) + ext_pages);

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

    /* reserved: PCI Segment 1 Root Bridge ECAM range */
    d->arch.e820[nr].addr = PCI1_ECAM_BASE;
    d->arch.e820[nr].size = PCI1_ECAM_SIZE;
    d->arch.e820[nr].type = E820_RESERVED;
    nr++;

    /* reserved: ACPI entry, ACPI_INFO_PHYSICAL_ADDRESS */
    d->arch.e820[nr].addr = 0xFC000000U;
    d->arch.e820[nr].size = acpi_pages << PAGE_SHIFT;
    d->arch.e820[nr].type = E820_ACPI;
    nr++;

    /* reserved: HVM special pages, X86_HVM_END_SPECIAL_REGION */
    d->arch.e820[nr].addr = START_SPECIAL_REGION << PAGE_SHIFT;
    d->arch.e820[nr].size = NR_SPECIAL_PAGES << PAGE_SHIFT;
    d->arch.e820[nr].type = E820_RESERVED;
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
        hvm_setup_e820(bd, nr_pages);

    do {
        preempted = false;
        paging_set_allocation(bd->d, paging_pages, &preempted);
        process_pending_softirqs();
    } while ( preempted );
}

static int __init pvh_setup_cpus(struct boot_domain *bd, paddr_t entry,
                                 paddr_t start_info)
{
    struct domain *d = bd->d;
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

    alloc_dom_vcpus(d, bd->hard_affinity);

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

static int __init hvm_populate_p2m(struct domain *d)
{
    unsigned int i;

    /* Populate memory map. */
    for ( i = 0; i < d->arch.nr_e820; i++ )
    {
        int rc;
        unsigned long addr, size;

        /*
         * Leave any any reserved region that's not the 0xA0000-1M (populated
         * for OVMF) or the special pages as p2m holes
         */
        if ( d->arch.e820[i].type != E820_RAM &&
             d->arch.e820[i].type != E820_ACPI &&
             d->arch.e820[i].addr != 0xA0000 &&
             PFN_DOWN(d->arch.e820[i].addr) != START_SPECIAL_REGION )
                continue;

        addr = PFN_DOWN(d->arch.e820[i].addr);
        size = PFN_DOWN(d->arch.e820[i].size);

        rc = pvh_populate_memory_range(d, addr, size);
        if ( rc )
            return rc;

        if ( d->arch.hvm.reset_info )
        {
            rc = rangeset_add_range(d->arch.hvm.reset_info->mem,
                                    addr, addr + size - 1);
            if ( rc )
                return rc;
        }
    }

    return 0;
}

static paddr_t __init find_memory(
    const struct domain *d, const struct elf_binary *elf, size_t size)
{
    paddr_t kernel_start = (paddr_t)elf->dest_base & PAGE_MASK;
    paddr_t kernel_end = ROUNDUP((paddr_t)elf->dest_base + elf->dest_size,
                                 PAGE_SIZE);
    unsigned int i;

    /*
     * The memory map is sorted and all RAM regions starts and sizes are
     * aligned to page boundaries.
     */
    for ( i = 0; i < d->arch.nr_e820; i++ )
    {
        paddr_t start, end = d->arch.e820[i].addr + d->arch.e820[i].size;

        /* Don't use memory below 1MB, as it could overwrite BDA/EBDA/IBFT. */
        if ( end <= MB(1) || d->arch.e820[i].type != E820_RAM )
            continue;

        start = MAX(ROUNDUP(d->arch.e820[i].addr, PAGE_SIZE), MB(1));

        ASSERT(IS_ALIGNED(start, PAGE_SIZE) && IS_ALIGNED(end, PAGE_SIZE));

        /*
         * NB: Even better would be to use rangesets to determine a suitable
         * range, in particular in case a kernel requests multiple heavily
         * discontiguous regions (which right now we fold all into one big
         * region).
         */
        if ( end <= kernel_start || start >= kernel_end )
        {
            /* No overlap, just check whether the region is large enough. */
            if ( end - start >= size )
                return start;
        }
        /* Deal with the kernel already being loaded in the region. */
        else if ( kernel_start > start && kernel_start - start >= size )
            return start;
        else if ( kernel_end < end && end - kernel_end >= size )
            return kernel_end;
    }

    return INVALID_PADDR;
}

static int __init hvm_setup_acpi_madt(
    struct domain *d, struct acpi_table_madt *madtp)
{
    struct acpi_table_madt madt = {};
    struct acpi_madt_local_apic lapic = {};
    unsigned long size = hvm_size_acpi_madt(d);
    unsigned long offs = 0;

    madt.header = madt_hdr;
    madt.address = APIC_DEFAULT_PHYS_BASE;

    offs += sizeof(madt);

    for ( unsigned int i = 0; i < d->max_vcpus; i++ )
    {
        lapic.header.type = ACPI_MADT_TYPE_LOCAL_APIC;
        lapic.header.length = sizeof(lapic);
        lapic.id = i * 2;
        lapic.processor_id = i;
        lapic.lapic_flags = ACPI_MADT_ENABLED;
        memcpy((void *)madtp + offs, &lapic, sizeof(lapic));
        offs += sizeof(lapic);
    }

    if ( has_vioapic(d) )
    {
        struct acpi_madt_io_apic io_apic = {};
        struct acpi_madt_interrupt_override intr_ovr = {};

        for ( unsigned int i = 0; i < d->arch.hvm.nr_vioapics; i++ )
        {
            io_apic.header.type = ACPI_MADT_TYPE_IO_APIC;
            io_apic.header.length = sizeof(io_apic);
            io_apic.id = domain_vioapic(d, i)->id;
            io_apic.address = domain_vioapic(d, i)->base_address;
            io_apic.global_irq_base = domain_vioapic(d, i)->base_gsi;
            memcpy((void *)madtp + offs, &io_apic, sizeof(io_apic));
            offs += sizeof(io_apic);
        }

        /* ISA IRQ0 routed to IOAPIC GSI 2. */
        intr_ovr.header.type = ACPI_MADT_TYPE_INTERRUPT_OVERRIDE;
        intr_ovr.header.length = sizeof(intr_ovr);
        intr_ovr.source_irq = 0;
        intr_ovr.global_irq = 2;
        intr_ovr.inti_flags = 0x0;
        memcpy((void *)madtp + offs, &intr_ovr, sizeof(intr_ovr));
        offs += sizeof(intr_ovr);
    }

    madt.header.length = size;
    memcpy(madtp, &madt, sizeof(madt));

    /*
     * Calling acpi_tb_checksum here is a layering violation, but
     * introducing a wrapper for such simple usage seems overkill.
     */
    madt.header.checksum -= acpi_tb_checksum(ACPI_CAST_PTR(u8, madtp), size);
    put_unaligned(madt.header.checksum, &madtp->header.checksum);

    return 0;
}

static int __init hvm_setup_acpi_xsdt(
    struct domain *d, struct acpi_table_xsdt *xsdt, paddr_t madt_addr,
    paddr_t fadt_addr, paddr_t mcfg_addr)
{
    unsigned long size = hvm_size_acpi_xsdt(d);

    xsdt->header = xsdt_hdr;

    /* Add the custom MADT. */
    xsdt->table_offset_entry[0] = madt_addr;
    /* Add the custom FADT. */
    xsdt->table_offset_entry[1] = fadt_addr;
    /* Add the custom MCFG. */
    xsdt->table_offset_entry[2] = mcfg_addr;

    xsdt->header.revision = 1;
    xsdt->header.length = size;
    /*
     * Calling acpi_tb_checksum here is a layering violation, but
     * introducing a wrapper for such simple usage seems overkill.
     */
    xsdt->header.checksum -= acpi_tb_checksum(ACPI_CAST_PTR(u8, xsdt), size);

    return 0;
}

static void __init hvm_setup_acpi_dsdt(struct boot_domain *bd, void *dsdt)
{
    if ( bd->acpi_dsdt )
        memcpy(dsdt, __va(bd->acpi_dsdt->start), bd->acpi_dsdt->size);
    else
        memcpy(dsdt, dsdt_pvh, dsdt_pvh_len);
}

static void __init hvm_setup_acpi_facs(struct domain *d,
    struct acpi_table_facs *facs)
{
    *facs = facs_xen;
}

static void __init hvm_setup_acpi_fadt(struct domain *d,
    struct acpi_table_fadt *fadtp, paddr_t facs_paddr, paddr_t dsdt_paddr)
{
    struct acpi_table_fadt fadt = {};
    unsigned long size = sizeof(fadt);

    fadt.header = fadt_hdr;

    fadt.sci_interrupt = 9;
    fadt.pm1a_event_block = ACPI_PM1A_EVT_BLK_ADDRESS_V1;
    fadt.pm1a_control_block = ACPI_PM1A_CNT_BLK_ADDRESS_V1;
#define ACPI_PM1A_EVT_BLK_BIT_WIDTH         0x20
#define ACPI_PM1A_CNT_BLK_BIT_WIDTH         0x10
    fadt.pm1_event_length = ACPI_PM1A_EVT_BLK_BIT_WIDTH/8;
    fadt.pm1_control_length = ACPI_PM1A_CNT_BLK_BIT_WIDTH/8;
    fadt.boot_flags = ACPI_FADT_NO_VGA | ACPI_FADT_NO_CMOS_RTC;

    fadt.dsdt = dsdt_paddr;
    fadt.facs = facs_paddr;

    fadt.header.length = size;
    /*
     * Calling acpi_tb_checksum here is a layering violation, but
     * introducing a wrapper for such simple usage seems overkill.
     */
    fadt.header.checksum -= acpi_tb_checksum(ACPI_CAST_PTR(u8, &fadt), size);

    memcpy(fadtp, &fadt, sizeof(fadt));
}

static void __init hvm_setup_acpi_mcfg(struct domain *d,
    struct acpi_table_mcfg *mcfgp)
{
    struct acpi_table_mcfg mcfg = {};
    struct acpi_mcfg_allocation mmcfg = {};
    unsigned long size = hvm_size_acpi_mcfg(d);

    mcfg.header = mcfg_hdr;

    mmcfg.address = PCI1_ECAM_BASE;
    mmcfg.pci_segment = 1;
    mmcfg.start_bus_number = 0;
    mmcfg.end_bus_number = PCI1_NR_BUS - 1;

    mcfg.header.length = size;

    memcpy(mcfgp, &mcfg, sizeof(mcfg));
    memcpy(mcfgp + 1, &mmcfg, sizeof(mmcfg));

    /*
     * Calling acpi_tb_checksum here is a layering violation, but
     * introducing a wrapper for such simple usage seems overkill.
     */
    mcfg.header.checksum -= acpi_tb_checksum(ACPI_CAST_PTR(u8, mcfgp), size);
    put_unaligned(mcfg.header.checksum, &mcfgp->header.checksum);
}

static paddr_t __init hvm_find_acpi_region(struct domain *d, unsigned long size)
{
    for ( unsigned int i = 0; i < d->arch.nr_e820; i++ )
    {
        if ( d->arch.e820[i].type != E820_ACPI )
            continue;

        BUG_ON(d->arch.e820[i].size != size);

        return d->arch.e820[i].addr;
    }

    panic("acpi region missing in e820 for %pd\n", d);
}

static int __init hvm_setup_acpi(struct boot_domain *bd, paddr_t start_info)
{
    struct domain *d = bd->d;
    paddr_t rsdp_paddr, xsdt_paddr, madt_paddr;
    paddr_t dsdt_paddr, facs_paddr, fadt_paddr, mcfg_paddr;
    paddr_t acpi_info_paddr;
    struct acpi_info *acpi_info;
    struct acpi_table_rsdp *rsdp;
    unsigned long size = hvm_size_acpi_region(bd);
    void *table;
    int rc;
    struct reset_info *rinfo = d->arch.hvm.reset_info;

    table = xzalloc_bytes(size);
    if ( !table )
        return -ENOMEM;

    /* First ACPI page is used for ACPI info */
    acpi_info = table;
    acpi_info->pci1_min = PCI1_MMIO_BASE;
    acpi_info->pci1_len = PCI1_MMIO_SIZE;
    acpi_info->pci1_hi_min = PCI1_64BIT_MMIO_BASE;
    acpi_info->pci1_hi_len = PCI1_64BIT_MMIO_SIZE;
    acpi_info->pci1_ecam = PCI1_ECAM_BASE;
    acpi_info->pci1_max_bus = PCI1_NR_BUS - 1;
    acpi_info->pci1_intx = PCI1_INTX_BASE;
    acpi_info_paddr = hvm_find_acpi_region(d, size);

    /* FACS
     * According to ACPI version 5.0, in contrast to the rest of the
     * ACPI tables that have no alignment requirements, FACS table
     * is expected to be aligned on a 64-byte boundary. Thus, place
     * FACS right after the ACPI info page.
     */
    table += PAGE_SIZE;
    facs_paddr = acpi_info_paddr + PAGE_SIZE;
    hvm_setup_acpi_facs(d, table);

    /* RSDP */
    table += hvm_size_acpi_facs(d);
    rsdp = table;
    rsdp_paddr = facs_paddr + hvm_size_acpi_facs(d);
    xsdt_paddr = rsdp_paddr + sizeof(struct acpi_table_rsdp);

    *rsdp = rsdp_xen;
    rsdp->xsdt_physical_address = xsdt_paddr,

    rsdp->checksum -= acpi_tb_checksum(ACPI_CAST_PTR(u8, rsdp),
                                       ACPI_RSDP_REV0_SIZE);
    rsdp->extended_checksum -= acpi_tb_checksum(ACPI_CAST_PTR(u8, rsdp),
                                                sizeof(*rsdp));

    /* XSDT */
    table += sizeof(struct acpi_table_rsdp);
    madt_paddr = xsdt_paddr + hvm_size_acpi_xsdt(d);
    dsdt_paddr = madt_paddr + hvm_size_acpi_madt(d);
    fadt_paddr = dsdt_paddr + hvm_size_acpi_dsdt(bd);
    mcfg_paddr = fadt_paddr + hvm_size_acpi_fadt(d);

    rc = hvm_setup_acpi_xsdt(d, table, madt_paddr, fadt_paddr, mcfg_paddr);
    if ( rc )
    {
        printk("Unable to construct XSDT\n");
        goto out;
    }

    /* MADT */
    table += hvm_size_acpi_xsdt(d);
    rc = hvm_setup_acpi_madt(d, table);
    if ( rc )
    {
        printk("Unable to construct MADT\n");
        goto out;
    }

    acpi_info->nr_cpus = d->max_vcpus;
    acpi_info->madt_csum_addr = madt_paddr +
        offsetof(struct acpi_table_header, checksum);
    acpi_info->madt_lapic0_addr = madt_paddr +
        sizeof(struct acpi_table_madt);

    /* DSDT */
    table += hvm_size_acpi_madt(d);
    hvm_setup_acpi_dsdt(bd, table);

    /* FADT */
    table += hvm_size_acpi_dsdt(bd);
    hvm_setup_acpi_fadt(d, table, facs_paddr, dsdt_paddr);

    /* MCFG */
    table += hvm_size_acpi_fadt(d);
    hvm_setup_acpi_mcfg(d, table);

    /* Copy ACPI region into guest memory. */
    rc = hvm_copy_to_guest_phys(acpi_info_paddr, acpi_info, size, d->vcpu[0]);
    if ( rc != HVMTRANS_okay )
    {
        printk("Unable to copy RSDP into guest memory (rc=%d)\n", rc);
        goto out;
    }

    /* Copy RSDP address to start_info. */
    rc = hvm_copy_to_guest_phys(
        start_info + offsetof(struct hvm_start_info, rsdp_paddr), &rsdp_paddr,
        sizeof(((struct hvm_start_info *) NULL)->rsdp_paddr), d->vcpu[0]);
    if ( rc != HVMTRANS_okay )
        printk("Unable to copy RSDP address to start info (rc=%d)\n", rc);

    if ( rinfo )
    {
        rinfo->acpi_sz = size;
        rinfo->acpi = xzalloc_bytes(rinfo->acpi_sz);
        if ( !rinfo->acpi )
        {
            rc = -ENOMEM;
            goto out;
        }
        memcpy(rinfo->acpi, acpi_info, rinfo->acpi_sz);
        rinfo->start_info->rsdp_paddr = rsdp_paddr;
    }

 out:
    if ( acpi_info )
        xfree(acpi_info);

    return rc;
}

static bool __init check_load_address(
    const struct domain *d, const struct elf_binary *elf)
{
    paddr_t kernel_start = (uintptr_t)elf->dest_base;
    paddr_t kernel_end = kernel_start + elf->dest_size;
    unsigned int i;

    /* Relies on a sorted memory map with adjacent entries merged. */
    for ( i = 0; i < d->arch.nr_e820; i++ )
    {
        paddr_t start = d->arch.e820[i].addr;
        paddr_t end = start + d->arch.e820[i].size;

        if ( start >= kernel_end )
            return false;

        if ( d->arch.e820[i].type == E820_RAM &&
             start <= kernel_start &&
             end >= kernel_end )
            return true;
    }

    return false;
}

/* Find an e820 RAM region that fits the kernel at a suitable alignment. */
static paddr_t __init find_kernel_memory(
    const struct domain *d, struct elf_binary *elf,
    const struct elf_dom_parms *parms)
{
    paddr_t kernel_size = elf->dest_size;
    unsigned int align;
    unsigned int i;

    if ( parms->phys_align != UNSET_ADDR32 )
        align = parms->phys_align;
    else if ( elf->palign >= PAGE_SIZE )
        align = elf->palign;
    else
        align = MB(2);

    /* Search backwards to find the highest address. */
    for ( i = d->arch.nr_e820; i--; )
    {
        paddr_t start = d->arch.e820[i].addr;
        paddr_t end = start + d->arch.e820[i].size;
        paddr_t kstart, kend;

        if ( d->arch.e820[i].type != E820_RAM ||
             d->arch.e820[i].size < kernel_size )
            continue;

        if ( start > parms->phys_max )
            continue;

        if ( end - 1 > parms->phys_max )
            end = parms->phys_max + 1;

        kstart = (end - kernel_size) & ~(align - 1);
        kend = kstart + kernel_size;

        if ( kstart < parms->phys_min )
            return 0;

        if ( kstart >= start && kend <= end )
            return kstart;
    }

    return 0;
}

/* Check the kernel load address, and adjust if necessary and possible. */
static bool __init check_and_adjust_load_address(
    const struct domain *d, struct elf_binary *elf, struct elf_dom_parms *parms)
{
    paddr_t reloc_base;

    if ( check_load_address(d, elf) )
        return true;

    if ( !parms->phys_reloc )
    {
        printk("%pd kernel: Address conflict and not relocatable\n", d);
        return false;
    }

    reloc_base = find_kernel_memory(d, elf, parms);
    if ( !reloc_base )
    {
        printk("%pd kernel: Failed find a load address\n", d);
        return false;
    }

    if ( opt_dom0_verbose )
        printk("%pd kernel: Moving [%p, %p] -> [%"PRIpaddr", %"PRIpaddr"]\n", d,
               elf->dest_base, elf->dest_base + elf->dest_size - 1,
               reloc_base, reloc_base + elf->dest_size - 1);

    parms->phys_entry =
        reloc_base + (parms->phys_entry - (uintptr_t)elf->dest_base);
    elf->dest_base = (char *)reloc_base;

    return true;
}

static int __init pvh_load_kernel(
    const struct boot_domain *bd, paddr_t *entry, paddr_t *start_info_addr)
{
    struct domain *d = bd->d;
    struct boot_module *image = bd->kernel;
    struct boot_module *initrd = bd->initrd;
    void *image_base = bootstrap_map_bm(image);
    void *image_start = image_base + image->arch.headroom;
    unsigned long image_len = image->size;
    unsigned long initrd_len = initrd ? initrd->size : 0;
    size_t cmdline_len = bd->cmdline ? strlen(bd->cmdline) + 1 : 0;
    struct elf_binary elf;
    struct elf_dom_parms parms;
    size_t extra_space;
    paddr_t last_addr;
    struct hvm_start_info start_info = { 0 };
    struct hvm_modlist_entry mod = { 0 };
    struct vcpu *v = d->vcpu[0];
    int rc;
    struct reset_info *rinfo = d->arch.hvm.reset_info;

    if ( (rc = bzimage_parse(image_base, &image_start, image->arch.headroom,
                             &image_len)) != 0 )
    {
        printk("Error trying to detect bz compressed kernel\n");
        return rc;
    }

    if ( rinfo )
    {
        rinfo->kernel_sz = image_len;
        rinfo->kernel = xmalloc_bytes(rinfo->kernel_sz);
        if ( !rinfo->kernel )
            return -ENOMEM;
        memcpy(rinfo->kernel, image_start, rinfo->kernel_sz);
        image_start = rinfo->kernel;
    }

    if ( (rc = elf_init(&elf, image_start, image_len)) != 0 )
    {
        printk("Unable to init ELF\n");
        return rc;
    }
    if ( opt_dom0_verbose )
        elf_set_verbose(&elf);
    elf_parse_binary(&elf);
    if ( (rc = elf_xen_parse(&elf, &parms, true)) != 0 )
    {
        printk("Unable to parse kernel for ELFNOTES\n");
        if ( elf_check_broken(&elf) )
            printk("%pd kernel: broken ELF: %s\n", d, elf_check_broken(&elf));
        return rc;
    }

    if ( parms.phys_entry == UNSET_ADDR32 )
    {
        printk("Unable to find XEN_ELFNOTE_PHYS32_ENTRY address\n");
        return -EINVAL;
    }

    /* Copy the OS image and free temporary buffer. */
    elf.dest_base = (void *)(parms.virt_kstart - parms.virt_base);
    elf.dest_size = parms.virt_kend - parms.virt_kstart;

    if ( !check_and_adjust_load_address(d, &elf, &parms) )
        return -ENOSPC;

    elf_set_vcpu(&elf, v);
    rc = elf_load_binary(&elf);
    if ( rc < 0 )
    {
        printk("Failed to load kernel: %d\n", rc);
        if ( elf_check_broken(&elf) )
            printk("%pd kernel: broken ELF: %s\n", d, elf_check_broken(&elf));
        return rc;
    }

    if ( rinfo )
    {
        rinfo->elf = xmalloc_bytes(sizeof(struct elf_binary));
        if ( !rinfo->elf )
            return -ENOMEM;
        *rinfo->elf = elf;
    }

    /*
     * Find a RAM region big enough (and that doesn't overlap with the loaded
     * kernel) in order to load the initrd and the metadata. Note it could be
     * split into smaller allocations, done as a single region in order to
     * simplify it.
     */
    extra_space = sizeof(start_info);

    if ( initrd )
    {
        size_t initrd_space = elf_round_up(&elf, initrd_len);

        if ( initrd_space )
            extra_space += ROUNDUP(initrd_space, PAGE_SIZE) + sizeof(mod);
        else
            initrd = NULL;
    }

    extra_space += elf_round_up(&elf, cmdline_len);

    last_addr = find_memory(d, &elf, extra_space);
    if ( last_addr == INVALID_PADDR )
    {
        printk("Unable to find a memory region to load initrd and metadata\n");
        return -ENOMEM;
    }

    if ( initrd != NULL )
    {
        rc = hvm_copy_to_guest_phys(last_addr, __va(initrd->start),
                                    initrd_len, v);
        if ( rc )
        {
            printk("Unable to copy initrd to guest\n");
            return rc;
        }

        mod.paddr = last_addr;
        mod.size = initrd_len;

        if ( rinfo )
        {
            rinfo->initrd_gpa = last_addr;
            rinfo->initrd_sz = initrd_len;
            rinfo->initrd = xmalloc_bytes(rinfo->initrd_sz);
            if ( !rinfo->initrd )
                return -ENOMEM;
            memcpy(rinfo->initrd, __va(initrd->start), rinfo->initrd_sz);
        }

        last_addr += elf_round_up(&elf, initrd_len);
        last_addr = ROUNDUP(last_addr, PAGE_SIZE);
    }

    rc = hvm_copy_to_guest_phys(last_addr, bd->cmdline, cmdline_len, v);
    if ( rc )
    {
        printk("Unable to copy guest command line\n");
        return rc;
    }

    start_info.cmdline_paddr = cmdline_len ? last_addr : 0;
    if ( rinfo && cmdline_len )
    {
        rinfo->kernel_cmd_gpa = start_info.cmdline_paddr;
        rinfo->kernel_cmd_sz = cmdline_len;
        rinfo->kernel_cmd = xmalloc_bytes(rinfo->kernel_cmd_sz);
        if ( !rinfo->kernel_cmd )
            return -ENOMEM;
        memcpy(rinfo->kernel_cmd, bd->cmdline, rinfo->kernel_cmd_sz);
    }

    /*
     * Round up to 32/64 bits (depending on the guest kernel bitness) so
     * the modlist/start_info is aligned.
     */
    last_addr += elf_round_up(&elf, cmdline_len);

    if ( initrd != NULL )
    {
        rc = hvm_copy_to_guest_phys(last_addr, &mod, sizeof(mod), v);
        if ( rc )
        {
            printk("Unable to copy guest modules\n");
            return rc;
        }
        start_info.modlist_paddr = last_addr;
        start_info.nr_modules = 1;
        last_addr += sizeof(mod);
    }

    start_info.magic = XEN_HVM_START_MAGIC_VALUE;
    if ( is_control_domain(d) )
        start_info.flags = SIF_PRIVILEGED;
    if ( is_hardware_domain(d) )
        start_info.flags |= SIF_INITDOMAIN;
    rc = hvm_copy_to_guest_phys(last_addr, &start_info, sizeof(start_info), v);
    if ( rc )
    {
        printk("Unable to copy start info to guest\n");
        return rc;
    }

    *entry = parms.phys_entry;
    *start_info_addr = last_addr;

    if ( rinfo )
    {
        rinfo->start_info = xmalloc_bytes(sizeof(struct hvm_start_info));
        if ( !rinfo->start_info )
            return -ENOMEM;
        *rinfo->start_info = start_info;
        rinfo->start_info_gpa = *start_info_addr;
        rinfo->entry_gpa = *entry;
    }

    return 0;
}

static int __init alloc_console_page(struct boot_domain *bd)
{
    paddr_t con_addr = special_pfn(SPECIALPAGE_CONSOLE) << PAGE_SHIFT;
    uint8_t fields[(sizeof(struct xencons_interface) -
                    offsetof(struct xencons_interface, in_cons))] = {};
    fields[(offsetof(struct xencons_interface, connection) -
            offsetof(struct xencons_interface, in_cons))] =
                XENCONSOLE_DISCONNECTED;

    if ( !port_is_valid(bd->d, bd->console.evtchn) )
    {
        printk("No event channel available for %pd console\n", bd->d);
        return -EINVAL;
    }

    /*
     * Clear the xencons_interface fields that are located after a 1024 rx and
     * a 2048 tx buffer, 3072 bytes.
     */
    if ( hvm_copy_to_guest_phys(
             con_addr + offsetof(struct xencons_interface, in_cons), fields,
             sizeof(fields), bd->d->vcpu[0]) != HVMTRANS_okay )
    {
        printk("Unable to set console connection state\n");
        return -EFAULT;
    }

    bd->console.gfn = gfn_x(gaddr_to_gfn(con_addr));
    bd->d->arch.hvm.params[HVM_PARAM_CONSOLE_PFN] = bd->console.gfn;
    bd->d->arch.hvm.params[HVM_PARAM_CONSOLE_EVTCHN] = bd->console.evtchn;

    if ( IS_ENABLED(CONFIG_GRANT_TABLE) )
        gnttab_seed_entry(bd->d, GNTTAB_RESERVED_CONSOLE,
                          bd->console.be_domid, bd->console.gfn);

    return 0;
}

typedef struct xenstore_domain_interface xsdom_if;

static int __init alloc_xenstore_page(struct boot_domain *bd)
{
    enum hvm_translation_result rc;
    paddr_t xs_addr = special_pfn(SPECIALPAGE_XENSTORE) << PAGE_SHIFT;
    /*
     * This is essentially hard-coding initialisation of xsdom_if
     *
     * Written in this convoluted fashion because we can't map the page here
     * and allocating a full interface in the stack (>2KiB) is dubious.
     */
    uint32_t fields[(sizeof(xsdom_if) - offsetof(xsdom_if, req_cons)) / 4]
        = { 0, 0, 0, 0, 0, XENSTORE_RECONNECT, 0, bd->xenstore.evtchn };

    BUILD_BUG_ON(sizeof(xsdom_if) != 2 * XENSTORE_RING_SIZE + sizeof(fields));

    if ( !port_is_valid(bd->d, bd->xenstore.evtchn) )
    {
        printk("No event channel available for %pd xenstore\n", bd->d);
        return -EINVAL;
    }

    BUG_ON(is_hardware_domain(bd->d));

    rc = hvm_copy_to_guest_phys(xs_addr + offsetof(xsdom_if, req_cons),
                                fields, sizeof(fields), bd->d->vcpu[0]);
    if ( rc != HVMTRANS_okay )
    {
        printk("Unable to set xenstore connection state (rc=%d)\n", rc);
        return -EFAULT;
    }

    bd->xenstore.gfn = gfn_x(gaddr_to_gfn(xs_addr));
    bd->d->arch.hvm.params[HVM_PARAM_STORE_PFN] = bd->xenstore.gfn;
    bd->d->arch.hvm.params[HVM_PARAM_STORE_EVTCHN] = bd->xenstore.evtchn;

    if ( IS_ENABLED(CONFIG_GRANT_TABLE) )
        gnttab_seed_entry(bd->d, GNTTAB_RESERVED_XENSTORE,
                          bd->xenstore.be_domid, bd->xenstore.gfn);

    return 0;
}

static int __init map_iomem(struct boot_domain *bd)
{
    struct domain *d = bd->d;
    unsigned int i;
    int ret = 0;

    for ( i = 0; i < bd->arch.nr_iomem; i++ )
    {
        unsigned long nr_mfns;
        mfn_t mfn, mfn_end;
        gfn_t gfn;

        mfn = bd->arch.iomem[i].start;
        nr_mfns = bd->arch.iomem[i].number;
        gfn = bd->arch.iomem[i].gfn;

        mfn_end = _mfn(mfn_x(mfn) + nr_mfns - 1);
        ret = iomem_permit_access(d, mfn_x(mfn), mfn_x(mfn_end));
        if ( ret )
        {
            printk(XENLOG_ERR
                   "%pd: Failed to permit access to %"PRI_mfn"-%"PRI_mfn"\n",
                   d, mfn_x(mfn), mfn_x(mfn_end));
            break;
        }

        ret = map_mmio_regions(d, gfn, nr_mfns, mfn);
        if ( ret < 0 )
        {
            printk(XENLOG_ERR
                   "%pd: Failed to map mfns %"PRI_mfn"-%"PRI_mfn" to %"PRI_gfn" ret %d\n",
                   d, mfn_x(mfn), mfn_x(mfn_end), gfn_x(gfn), ret);
            break;
        }
        else if ( ret > 0 )
        {
            printk(XENLOG_ERR
                   "%pd: Only mapped %d/%lu mfns at %"PRI_mfn" to %"PRI_gfn"\n",
                   d, ret, nr_mfns, mfn_x(mfn), gfn_x(gfn));
            ret = -ENOMEM;
            break;
        }
    }

    if ( d->arch.hvm.reset_info )
    {
        d->arch.hvm.reset_info->arch.nr_iomem = bd->arch.nr_iomem;
        SWAP(d->arch.hvm.reset_info->arch.iomem, bd->arch.iomem);
    }

    XFREE(bd->arch.iomem);
    bd->arch.nr_iomem = 0;

    return ret;
}

static int __init map_irqs(struct boot_domain *bd)
{
    struct domain *d = bd->d;
    unsigned int i;
    int ret = 0;

    for ( i = 0; i < bd->arch.nr_irqs; i++ )
    {
        struct xen_domctl_bind_pt_irq pt_irq = {};
        int hw_irq = bd->arch.irqs[i].hw_irq;
        int pirq = bd->arch.irqs[i].guest_irq;

        if ( bd->arch.irqs[i].hw_irq > INT_MAX ||
             bd->arch.irqs[i].guest_irq > INT_MAX )
        {
            printk(XENLOG_ERR "%pd: hw_irq %u or guest_irq %u out of range\n",
                   d, bd->arch.irqs[i].hw_irq, bd->arch.irqs[i].guest_irq);
            ret = -EOVERFLOW;
            break;
        }

        printk(XENLOG_INFO "%pd: hw irq %d -> guest irq %d\n", d, hw_irq, pirq);
        ret = irq_permit_access(d, hw_irq);
        if ( ret )
        {
            printk(XENLOG_ERR "%pd: Failed to permit access to irq %d\n", d,
                   hw_irq);
            break;
        }

        ret = allocate_and_map_gsi_pirq(d, hw_irq, &pirq);
        if ( ret )
        {
            printk(XENLOG_ERR "%pd: Failed allocate_and_map_gsi_pirq %d\n", d,
                   ret);
            break;
        }

        pt_irq.irq_type = PT_IRQ_TYPE_ISA;
        pt_irq.machine_irq = pirq;
        pt_irq.u.isa.isa_irq = pirq;

        ret = pt_irq_create_bind(d, &pt_irq);
        if ( ret )
        {
            printk(XENLOG_ERR "%pd: bind failed %d\n", d, ret);
            break;
        }

        printk(XENLOG_INFO "%pd: Success irq %d pirq %d\n", d, hw_irq, pirq);
    }

    if ( d->arch.hvm.reset_info )
    {
        d->arch.hvm.reset_info->arch.nr_irqs = bd->arch.nr_irqs;
        SWAP(d->arch.hvm.reset_info->arch.irqs, bd->arch.irqs);
    }

    XFREE(bd->arch.irqs);
    bd->arch.nr_irqs = 0;

    return ret;
}

int __init dom_construct_pvh(struct boot_domain *bd)
{
    paddr_t entry, start_info;
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

    if ( is_hardware_domain(bd->d) )
        rc = dom0_pvh_populate_p2m(bd->d);
    else
        rc = hvm_populate_p2m(bd->d);
    if ( rc )
    {
#ifdef CONFIG_COVERAGE_XEN
        {
            struct page_info *page, *tmp;

            /*
             * If we are here, then all available memory has been exhausted by
             * this domain's allocation, and it will not be possible to allocate
             * a buffer for coverage data. Release the memory for the failing
             * domain.
             */
            rspin_lock(&d->page_alloc_lock);
            page_list_for_each_safe(page, tmp, &d->page_list)
                put_page(page);
            rspin_unlock(&d->page_alloc_lock);
        }
#endif
        printk("Failed to setup HVM/PVH %pd physical memory map\n", bd->d);
        return rc;
    }

#ifdef CONFIG_VIRTIO_MMIO_NON_BLOCKING
    rc = dom_construct_virtio(bd);
    if ( rc )
        return rc;
#endif

    rc = pvh_load_kernel(bd, &entry, &start_info);
    if ( rc )
    {
        printk("Failed to load Dom%u kernel\n", d->domain_id);
        return rc;
    }

    rc = pvh_setup_cpus(bd, entry, start_info);
    if ( rc )
    {
        printk("Failed to setup Dom%u CPUs: %d\n", d->domain_id, rc);
        return rc;
    }

    if ( is_hardware_domain(bd->d) )
        rc = hwdom_pvh_setup_acpi(bd->d, start_info);
    else
        rc = hvm_setup_acpi(bd, start_info);

    if ( rc )
    {
        printk("Failed to setup Dom%u ACPI tables: %d\n", d->domain_id, rc);
        return rc;
    }

    if ( IS_ENABLED(CONFIG_DOM0LESS_BOOT) )
    {
        /* Allow console_io. */
        d->is_console = true;
        d->console.input_allowed = true;

        /* Also setup console page. */
        if ( !is_hardware_domain(bd->d) )
            alloc_console_page(bd);

        if ( !is_xenstore_domain(bd->d) )
            alloc_xenstore_page(bd);
    }

    if ( !is_hardware_domain(bd->d) )
    {
        rc = map_iomem(bd);
        if ( rc )
            return rc;

        rc = map_irqs(bd);
        if ( rc )
            return rc;
    }

    if ( opt_dom0_verbose )
    {
        printk("Dom%u memory map:\n", d->domain_id);
        print_e820_memory_map(d->arch.e820, d->arch.nr_e820);
    }

    printk("WARNING: PVH is an experimental mode with limited functionality\n");
    return 0;
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
