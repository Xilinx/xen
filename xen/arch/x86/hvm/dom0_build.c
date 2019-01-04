/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * hvm/dom0_build.c
 *
 * Dom0 builder for PVH guest.
 *
 * Copyright (C) 2017 Citrix Systems R&D
 */

#include <xen/acpi.h>
#include <xen/init.h>
#include <xen/libelf.h>
#include <xen/multiboot.h>
#include <xen/pci.h>
#include <xen/softirq.h>

#include <acpi/actables.h>

#include <asm/bootinfo.h>
#include <asm/bzimage.h>
#include <asm/dom0_build.h>
#include <asm/domain-builder.h>
#include <asm/hvm/support.h>
#include <asm/io_apic.h>
#include <asm/p2m.h>
#include <asm/paging.h>
#include <asm/setup.h>

#include <public/arch-x86/hvm/start_info.h>
#include <public/hvm/hvm_info_table.h>
#include <public/hvm/hvm_vcpu.h>
#include <public/hvm/params.h>

/*
 * Have the TSS cover the ISA port range, which makes it
 * - 104 bytes base structure
 * - 32 bytes interrupt redirection bitmap
 * - 128 bytes I/O bitmap
 * - one trailing byte
 * or a total of 265 bytes.
 *
 * NB: as PVHv2 Dom0 doesn't have legacy devices (ISA), it shouldn't have any
 * business in accessing the ISA port range, much less in real mode, and due to
 * the lack of firmware it shouldn't also execute any INT instruction. This is
 * done just for consistency with what hvmloader does.
 */
#define HVM_VM86_TSS_SIZE 265

static unsigned int __initdata acpi_intr_overrides;
static struct acpi_madt_interrupt_override __initdata *intsrcovr;

static unsigned int __initdata order_stats[MAX_ORDER + 1];

static void __init print_order_stats(const struct domain *d)
{
    unsigned int i;

    printk("Dom%u memory allocation stats:\n", d->domain_id);
    for ( i = 0; i < ARRAY_SIZE(order_stats); i++ )
        if ( order_stats[i] )
            printk("order %2u allocations: %u\n", i, order_stats[i]);
}

static int __init modify_identity_mmio(struct domain *d, unsigned long pfn,
                                       unsigned long nr_pages, const bool map)
{
    int rc;

    for ( ; ; )
    {
        if ( map )
            rc = map_mmio_regions(d, _gfn(pfn), nr_pages, _mfn(pfn),
                                  CACHEABILITY_DEVMEM);
        else
            rc = unmap_mmio_regions(d, _gfn(pfn), nr_pages, _mfn(pfn));
        if ( rc == 0 )
            break;
        if ( rc < 0 )
        {
            printk(XENLOG_WARNING
                   "Failed to identity %smap [%#lx,%#lx) for d%d: %d\n",
                   map ? "" : "un", pfn, pfn + nr_pages, d->domain_id, rc);
            break;
        }
        nr_pages -= rc;
        pfn += rc;
        process_pending_softirqs();
    }

    return rc;
}

/* Populate a HVM memory range using the biggest possible order. */
int __init pvh_populate_memory_range(struct domain *d, unsigned long start,
                                     unsigned long nr_pages)
{
    static const struct {
        unsigned long align;
        unsigned int order;
    } orders[] __initconst = {
        /* NB: must be sorted by decreasing size. */
        { .align = PFN_DOWN(GB(1)), .order = PAGE_ORDER_1G },
        { .align = PFN_DOWN(MB(2)), .order = PAGE_ORDER_2M },
        { .align = PFN_DOWN(KB(4)), .order = PAGE_ORDER_4K },
    };
    unsigned int max_order = MAX_ORDER;
    struct page_info *page;
    int rc;

    while ( nr_pages != 0 )
    {
        unsigned int order, j;
        unsigned long end;

        /* Search for the largest page size which can fulfil this request. */
        for ( j = 0; j < ARRAY_SIZE(orders); j++ )
            if ( IS_ALIGNED(start, orders[j].align) &&
                 nr_pages >= (1UL << orders[j].order) )
                break;

        switch ( j )
        {
        case ARRAY_SIZE(orders):
            printk("Unable to find allocation order for [%#lx,%#lx)\n",
                   start, start + nr_pages);
            return -EINVAL;

        case 0:
            /* Highest order, aim to allocate until the end of the region. */
            end = (start + nr_pages) & ~(orders[0].align - 1);
            break;

        default:
            /*
             * Aim to allocate until the higher next order alignment or the
             * end of the region.
             */
            end = min(ROUNDUP(start + 1, orders[j - 1].align),
                      start + nr_pages);
            break;
        }

        order = get_order_from_pages(end - start + 1);
        order = min(order ? order - 1 : 0, max_order);
        /* The order allocated and populated must be aligned to the address. */
        order = min(order, start ? ffsl(start) - 1U : MAX_ORDER + 0U);
        page = alloc_domheap_pages(d, order, dom0_memflags | MEMF_no_scrub);
        if ( page == NULL )
        {
            if ( order == 0 && dom0_memflags )
            {
                /* Try again without any dom0_memflags. */
                dom0_memflags = 0;
                max_order = MAX_ORDER;
                continue;
            }
            if ( order == 0 )
            {
                printk("Unable to allocate memory with order 0!\n");
                return -ENOMEM;
            }
            max_order = order - 1;
            continue;
        }

        rc = p2m_add_page(d, _gfn(start), page_to_mfn(page), order, p2m_ram_rw);
        if ( rc != 0 )
        {
            printk("Failed to populate memory: [%#lx,%#lx): %d\n",
                   start, start + (1UL << order), rc);
            return rc;
        }
        start += 1UL << order;
        nr_pages -= 1UL << order;
        order_stats[order]++;
        /*
         * Process pending softirqs on every successful loop: it's unknown
         * whether the p2m/IOMMU code will have split the page into multiple
         * smaller entries, and thus the time consumed would be much higher
         * than populating a single entry.
         */
        process_pending_softirqs();
    }

    return 0;
}

/* Steal RAM from the end of a memory region. */
static int __init pvh_steal_ram(struct domain *d, unsigned long size,
                                unsigned long align, paddr_t limit,
                                paddr_t *addr)
{
    unsigned int i = d->arch.nr_e820;

    /*
     * Alignment 0 should be set to 1, so it doesn't wrap around in the
     * calculations below.
     */
    align = align ? : 1;
    while ( i-- )
    {
        struct e820entry *entry = &d->arch.e820[i];

        if ( entry->type != E820_RAM || entry->addr + entry->size > limit )
            continue;

        *addr = (entry->addr + entry->size - size) & ~(align - 1);
        if ( *addr < entry->addr ||
             /* Don't steal from the low 1MB due to the copying done there. */
             *addr < MB(1) )
            continue;

        entry->size = *addr - entry->addr;
        return 0;
    }

    return -ENOMEM;
}

/* NB: memory map must be sorted at all times for this to work correctly. */
static int __init pvh_add_mem_range(struct domain *d, uint64_t s, uint64_t e,
                                    unsigned int type)
{
    struct e820entry *map;
    unsigned int i;

    for ( i = 0; i < d->arch.nr_e820; i++ )
    {
        uint64_t rs = d->arch.e820[i].addr;
        uint64_t re = rs + d->arch.e820[i].size;

        if ( rs == e && d->arch.e820[i].type == type )
        {
            d->arch.e820[i].addr = s;
            d->arch.e820[i].size += e - s;
            return 0;
        }

        if ( re == s && d->arch.e820[i].type == type &&
             (i + 1 == d->arch.nr_e820 || d->arch.e820[i + 1].addr >= e) )
        {
            d->arch.e820[i].size += e - s;
            return 0;
        }

        if ( rs >= e )
            break;

        if ( re > s )
            return -EEXIST;
    }

    map = xzalloc_array(struct e820entry, d->arch.nr_e820 + 1);
    if ( !map )
    {
        printk(XENLOG_WARNING "E820: out of memory to add region\n");
        return -ENOMEM;
    }

    memcpy(map, d->arch.e820, i * sizeof(*d->arch.e820));
    memcpy(map + i + 1, d->arch.e820 + i,
           (d->arch.nr_e820 - i) * sizeof(*d->arch.e820));
    map[i].addr = s;
    map[i].size = e - s;
    map[i].type = type;
    xfree(d->arch.e820);
    d->arch.e820 = map;
    d->arch.nr_e820++;

    return 0;
}

static int __init pvh_setup_vmx_realmode_helpers(struct domain *d)
{
    uint32_t rc, *ident_pt;
    mfn_t mfn;
    paddr_t gaddr;
    struct vcpu *v = d->vcpu[0];

    /*
     * Steal some space from the last RAM region below 4GB and use it to
     * store the real-mode TSS. It needs to be aligned to 128 so that the
     * TSS structure (which accounts for the first 104b) doesn't cross
     * a page boundary.
     */
    if ( !pvh_steal_ram(d, HVM_VM86_TSS_SIZE, 128, GB(4), &gaddr) )
    {
        if ( hvm_copy_to_guest_phys(gaddr, NULL, HVM_VM86_TSS_SIZE, v) !=
             HVMTRANS_okay )
            printk("Unable to zero VM86 TSS area\n");
        d->arch.hvm.params[HVM_PARAM_VM86_TSS_SIZED] =
            VM86_TSS_UPDATED | ((uint64_t)HVM_VM86_TSS_SIZE << 32) | gaddr;
        if ( pvh_add_mem_range(d, gaddr, gaddr + HVM_VM86_TSS_SIZE,
                               E820_RESERVED) )
            printk("Unable to set VM86 TSS as reserved in the memory map\n");
    }
    else
        printk("Unable to allocate VM86 TSS area\n");

    /* Steal some more RAM for the identity page tables. */
    if ( pvh_steal_ram(d, PAGE_SIZE, PAGE_SIZE, GB(4), &gaddr) )
    {
        printk("Unable to find memory to stash the identity page tables\n");
        return -ENOMEM;
    }

    /*
     * Identity-map page table is required for running with CR0.PG=0
     * when using Intel EPT. Create a 32-bit non-PAE page directory of
     * superpages.
     */
    ident_pt = map_domain_gfn(p2m_get_hostp2m(d), _gfn(PFN_DOWN(gaddr)),
                              &mfn, 0, &rc);
    if ( ident_pt == NULL )
    {
        printk("Unable to map identity page tables\n");
        return -ENOMEM;
    }
    write_32bit_pse_identmap(ident_pt);
    unmap_domain_page(ident_pt);
    put_page(mfn_to_page(mfn));
    d->arch.hvm.params[HVM_PARAM_IDENT_PT] = gaddr;
    if ( pvh_add_mem_range(d, gaddr, gaddr + PAGE_SIZE, E820_RESERVED) )
            printk("Unable to set identity page tables as reserved in the memory map\n");

    return 0;
}

void __init dom0_pvh_setup_e820(struct domain *d, unsigned long nr_pages)
{
    struct e820entry *entry, *entry_guest;
    unsigned int i;
    unsigned long pages, cur_pages = 0;
    uint64_t start, end;

    /*
     * Craft the e820 memory map for Dom0 based on the hardware e820 map.
     * Add an extra entry in case we have to split a RAM entry into a RAM and a
     * UNUSABLE one in order to truncate it.
     */
    d->arch.e820 = xzalloc_array(struct e820entry, e820.nr_map + 1);
    if ( !d->arch.e820 )
        panic("Unable to allocate memory for Dom%u e820 map\n", d->domain_id);
    entry_guest = d->arch.e820;

    /* Clamp e820 memory map to match the memory assigned to Dom0 */
    for ( i = 0, entry = e820.map; i < e820.nr_map; i++, entry++ )
    {
        *entry_guest = *entry;

        if ( entry->type != E820_RAM )
            goto next;

        if ( nr_pages == cur_pages )
        {
            /*
             * We already have all the requested memory, turn this RAM region
             * into a UNUSABLE region in order to prevent Dom0 from placing
             * BARs in this area.
             */
            entry_guest->type = E820_UNUSABLE;
            goto next;
        }

        /*
         * Make sure the start and length are aligned to PAGE_SIZE, because
         * that's the minimum granularity of the 2nd stage translation. Since
         * the p2m code uses PAGE_ORDER_4K internally, also use it here in
         * order to prevent this code from getting out of sync.
         */
        start = ROUNDUP(entry->addr, PAGE_SIZE << PAGE_ORDER_4K);
        end = (entry->addr + entry->size) &
              ~((PAGE_SIZE << PAGE_ORDER_4K) - 1);
        if ( start >= end )
            continue;

        entry_guest->type = E820_RAM;
        entry_guest->addr = start;
        entry_guest->size = end - start;
        pages = PFN_DOWN(entry_guest->size);
        if ( (cur_pages + pages) > nr_pages )
        {
            /* Truncate region */
            entry_guest->size = (nr_pages - cur_pages) << PAGE_SHIFT;
            /* Add the remaining of the RAM region as UNUSABLE. */
            entry_guest++;
            d->arch.nr_e820++;
            entry_guest->type = E820_UNUSABLE;
            entry_guest->addr = start + ((nr_pages - cur_pages) << PAGE_SHIFT);
            entry_guest->size = end - entry_guest->addr;
            cur_pages = nr_pages;
        }
        else
        {
            cur_pages += pages;
        }
 next:
        d->arch.nr_e820++;
        entry_guest++;
        ASSERT(d->arch.nr_e820 <= e820.nr_map + 1);
    }
    ASSERT(cur_pages == nr_pages);
}

int __init dom0_pvh_populate_p2m(struct domain *d)
{
    struct vcpu *v = d->vcpu[0];
    unsigned int i;
    int rc;
#define MB1_PAGES PFN_DOWN(MB(1))

    /* Populate memory map. */
    for ( i = 0; i < d->arch.nr_e820; i++ )
    {
        unsigned long addr, size;

        if ( d->arch.e820[i].type != E820_RAM )
            continue;

        addr = PFN_DOWN(d->arch.e820[i].addr);
        size = PFN_DOWN(d->arch.e820[i].size);

        rc = pvh_populate_memory_range(d, addr, size);
        if ( rc )
            return rc;

        if ( addr < MB1_PAGES )
        {
            uint64_t end = min_t(uint64_t, MB(1),
                                 d->arch.e820[i].addr + d->arch.e820[i].size);
            enum hvm_translation_result res =
                 hvm_copy_to_guest_phys(mfn_to_maddr(_mfn(addr)),
                                        mfn_to_virt(addr),
                                        end - d->arch.e820[i].addr,
                                        v);

            if ( res != HVMTRANS_okay )
                printk("Failed to copy [%#lx, %#lx): %d\n",
                       addr, addr + size, res);
        }
    }

    /* Identity map everything below 1MB that's not already mapped. */
    for ( i = rc = 0; i < MB1_PAGES; ++i )
    {
        p2m_type_t p2mt;
        mfn_t mfn = get_gfn_query(d, i, &p2mt);

        if ( mfn_eq(mfn, INVALID_MFN) )
            rc = set_mmio_p2m_entry(d, _gfn(i), _mfn(i), PAGE_ORDER_4K);
        else
            /*
             * If the p2m entry is already set it must belong to a reserved
             * region (e.g. RMRR/IVMD) and be identity mapped, or else be a
             * RAM region.
             */
            ASSERT(p2mt == p2m_ram_rw || mfn_eq(mfn, _mfn(i)));
        put_gfn(d, i);
        if ( rc )
        {
            printk("Failed to identity map PFN %x: %d\n", i, rc);
            return rc;
        }
    }

    if ( using_vmx() && paging_mode_hap(d) && !vmx_unrestricted_guest(v) )
    {
        /*
         * Since Dom0 cannot be migrated, we will only setup the
         * unrestricted guest helpers if they are needed by the current
         * hardware we are running on.
         */
        rc = pvh_setup_vmx_realmode_helpers(d);
        if ( rc )
            return rc;
    }

    if ( opt_dom0_verbose )
        print_order_stats(d);

    return 0;
#undef MB1_PAGES
}

static int __init cf_check acpi_count_intr_ovr(
    struct acpi_subtable_header *header, const unsigned long end)
{
    acpi_intr_overrides++;
    return 0;
}

static int __init cf_check acpi_set_intr_ovr(
    struct acpi_subtable_header *header, const unsigned long end)
{
    const struct acpi_madt_interrupt_override *intr =
        container_of(header, struct acpi_madt_interrupt_override, header);

    *intsrcovr = *intr;
    intsrcovr++;

    return 0;
}

static int __init pvh_setup_acpi_madt(struct domain *d, paddr_t *addr)
{
    struct acpi_table_madt *madt;
    struct acpi_table_header *table;
    struct acpi_madt_io_apic *io_apic;
    struct acpi_madt_local_x2apic *x2apic;
    acpi_status status;
    unsigned long size;
    unsigned int i;
    int rc;

    /* Count number of interrupt overrides in the MADT. */
    acpi_table_parse_madt(ACPI_MADT_TYPE_INTERRUPT_OVERRIDE,
                          acpi_count_intr_ovr, UINT_MAX);

    /* Calculate the size of the crafted MADT. */
    size = sizeof(*madt);
    size += sizeof(*io_apic) * nr_ioapics;
    size += sizeof(*intsrcovr) * acpi_intr_overrides;
    size += sizeof(*x2apic) * d->max_vcpus;

    madt = xzalloc_bytes(size);
    if ( !madt )
    {
        printk("Unable to allocate memory for MADT table\n");
        rc = -ENOMEM;
        goto out;
    }

    /* Copy the native MADT table header. */
    status = acpi_get_table(ACPI_SIG_MADT, 0, &table);
    if ( !ACPI_SUCCESS(status) )
    {
        printk("Failed to get MADT ACPI table, aborting.\n");
        rc = -EINVAL;
        goto out;
    }
    madt->header = *table;
    madt->address = APIC_DEFAULT_PHYS_BASE;
    /*
     * NB: this is currently set to 4, which is the revision in the ACPI
     * spec 6.1. Sadly ACPICA doesn't provide revision numbers for the
     * tables described in the headers.
     */
    madt->header.revision = min_t(unsigned char, table->revision, 4);

    /* Setup the IO APIC entries. */
    io_apic = (void *)(madt + 1);
    for ( i = 0; i < nr_ioapics; i++ )
    {
        io_apic->header.type = ACPI_MADT_TYPE_IO_APIC;
        io_apic->header.length = sizeof(*io_apic);
        io_apic->id = domain_vioapic(d, i)->id;
        io_apic->address = domain_vioapic(d, i)->base_address;
        io_apic->global_irq_base = domain_vioapic(d, i)->base_gsi;
        io_apic++;
    }

    x2apic = (void *)io_apic;
    for ( i = 0; i < d->max_vcpus; i++ )
    {
        x2apic->header.type = ACPI_MADT_TYPE_LOCAL_X2APIC;
        x2apic->header.length = sizeof(*x2apic);
        x2apic->uid = i;
        x2apic->local_apic_id = i * 2;
        x2apic->lapic_flags = ACPI_MADT_ENABLED;
        x2apic++;
    }

    /* Setup interrupt overrides. */
    intsrcovr = (void *)x2apic;
    acpi_table_parse_madt(ACPI_MADT_TYPE_INTERRUPT_OVERRIDE, acpi_set_intr_ovr,
                          acpi_intr_overrides);

    ASSERT(((void *)intsrcovr - (void *)madt) == size);
    madt->header.length = size;
    /*
     * Calling acpi_tb_checksum here is a layering violation, but
     * introducing a wrapper for such simple usage seems overkill.
     */
    madt->header.checksum -= acpi_tb_checksum(ACPI_CAST_PTR(u8, madt), size);

    /* Place the new MADT in guest memory space. */
    if ( pvh_steal_ram(d, size, 0, GB(4), addr) )
    {
        printk("Unable to steal guest RAM for MADT\n");
        rc = -ENOMEM;
        goto out;
    }

    /* Mark this region as E820_ACPI. */
    if ( pvh_add_mem_range(d, *addr, *addr + size, E820_ACPI) )
        printk("Unable to add MADT region to memory map\n");

    rc = hvm_copy_to_guest_phys(*addr, madt, size, d->vcpu[0]);
    if ( rc )
    {
        printk("Unable to copy MADT into guest memory\n");
        goto out;
    }

    rc = 0;

 out:
    xfree(madt);

    return rc;
}

static bool __init acpi_memory_banned(unsigned long address,
                                      unsigned long size)
{
    unsigned long mfn = PFN_DOWN(address);
    unsigned long nr_pages = PFN_UP((address & ~PAGE_MASK) + size), i;

    for ( i = 0 ; i < nr_pages; i++ )
        if ( !page_is_ram_type(mfn + i, RAM_TYPE_RESERVED) &&
             !page_is_ram_type(mfn + i, RAM_TYPE_ACPI) )
            return true;

    return false;
}

static bool __init pvh_acpi_table_allowed(const char *sig,
                                          unsigned long address,
                                          unsigned long size)
{
    static const char __initconst allowed_tables[][ACPI_NAME_SIZE] = {
        ACPI_SIG_DSDT, ACPI_SIG_FADT, ACPI_SIG_FACS, ACPI_SIG_PSDT,
        ACPI_SIG_SSDT, ACPI_SIG_SBST, ACPI_SIG_MCFG, ACPI_SIG_SLIC,
        ACPI_SIG_MSDM, ACPI_SIG_WDAT, ACPI_SIG_FPDT, ACPI_SIG_S3PT,
        ACPI_SIG_VFCT,
    };
    unsigned int i;

    for ( i = 0 ; i < ARRAY_SIZE(allowed_tables); i++ )
    {
        if ( strncmp(sig, allowed_tables[i], ACPI_NAME_SIZE) )
            continue;

        if ( !acpi_memory_banned(address, size) )
            return true;
        else
        {
    skip:
            printk("Skipping table %.4s in non-ACPI non-reserved region\n",
                   sig);
            return false;
        }
    }

    if ( !strncmp(sig, "OEM", 3) )
    {
        if ( acpi_memory_banned(address, size) )
            goto skip;
        return true;
    }

    return false;
}

static bool __init pvh_acpi_xsdt_table_allowed(const char *sig,
                                               unsigned long address,
                                               unsigned long size)
{
    /*
     * DSDT and FACS are pointed to from FADT and thus don't belong
     * in XSDT.
     */
    return (pvh_acpi_table_allowed(sig, address, size) &&
            strncmp(sig, ACPI_SIG_DSDT, ACPI_NAME_SIZE) &&
            strncmp(sig, ACPI_SIG_FACS, ACPI_NAME_SIZE));
}

/*
 * Modify FADT table to clear LEGACY_DEVICES flag when VPIC/VPIT are disabled.
 * This prevents dom0 from probing for legacy devices when Xen doesn't provide
 * virtual PIC/PIT emulation.
 */
static int __init pvh_setup_acpi_fadt(struct domain *d, unsigned long native_fadt_addr,
                                      unsigned long *new_fadt_addr)
{
    struct acpi_table_fadt *native_fadt, *new_fadt;
    unsigned long fadt_size;
    int rc;

    if ( IS_ENABLED(CONFIG_VPIT) || IS_ENABLED(CONFIG_VPIC) )
    {
        *new_fadt_addr = native_fadt_addr;
        return 0;
    }

    /* Map the native FADT to determine its size */
    native_fadt = acpi_os_map_memory(native_fadt_addr, sizeof(*native_fadt));
    if ( !native_fadt )
    {
        printk("Unable to map native FADT\n");
        return -EINVAL;
    }

    fadt_size = native_fadt->header.length;
    acpi_os_unmap_memory(native_fadt, sizeof(*native_fadt));

    /* Allocate memory for the new FADT */
    new_fadt = xzalloc_bytes(fadt_size);
    if ( !new_fadt )
    {
        printk("Unable to allocate memory for FADT\n");
        return -ENOMEM;
    }

    /* Map and copy the entire native FADT */
    native_fadt = acpi_os_map_memory(native_fadt_addr, fadt_size);
    if ( !native_fadt )
    {
        printk("Unable to map complete native FADT\n");
        rc = -EINVAL;
        goto cleanup;
    }

    memcpy(new_fadt, native_fadt, fadt_size);
    acpi_os_unmap_memory(native_fadt, fadt_size);

    /*
     * Clear LEGACY_DEVICES flag when both VPIC and VPIT are disabled.
     * This tells dom0 that the system doesn't have legacy ISA/LPC devices
     * and prevents it from probing for PIC/PIT hardware.
     */
    new_fadt->boot_flags &= ~ACPI_FADT_LEGACY_DEVICES;
    printk("dom%u: Cleared ACPI_FADT_LEGACY_DEVICES flag (VPIC/VPIT disabled)\n",
           d->domain_id);

    /* Update checksum */
    new_fadt->header.checksum = 0;
    new_fadt->header.checksum = (uint8_t)(0 - acpi_tb_checksum((uint8_t *)new_fadt, fadt_size));

    /* Allocate guest memory for the new FADT */
    if ( pvh_steal_ram(d, fadt_size, 0, GB(4), new_fadt_addr) )
    {
        printk("Unable to find guest RAM for FADT\n");
        rc = -ENOMEM;
        goto cleanup;
    }

    /* Mark this region as E820_ACPI */
    if ( pvh_add_mem_range(d, *new_fadt_addr, *new_fadt_addr + fadt_size, E820_ACPI) )
        printk("Unable to add FADT region to memory map\n");

    /* Copy the modified FADT to guest memory */
    rc = hvm_copy_to_guest_phys(*new_fadt_addr, new_fadt, fadt_size, d->vcpu[0]);
    if ( rc )
    {
        printk("Unable to copy FADT into guest memory\n");
        goto cleanup;
    }

    rc = 0;

cleanup:
    xfree(new_fadt);
    return rc;
}

static int __init pvh_setup_acpi_xsdt(struct domain *d, paddr_t madt_addr,
                                      paddr_t *addr)
{
    struct acpi_table_xsdt *xsdt;
    struct acpi_table_header *table;
    struct acpi_table_rsdp *rsdp;
    const struct acpi_table_desc *tables = acpi_gbl_root_table_list.tables;
    unsigned long size = sizeof(*xsdt);
    unsigned int i, j, num_tables = 0;
    paddr_t xsdt_paddr;
    int rc;

    /*
     * Restore original DMAR table signature, we are going to filter it from
     * the new XSDT that is presented to the guest, so it is no longer
     * necessary to have it's signature zapped.
     */
    acpi_dmar_reinstate();

    /* Count the number of tables that will be added to the XSDT. */
    for( i = 0; i < acpi_gbl_root_table_list.count; i++ )
    {
        if ( pvh_acpi_xsdt_table_allowed(tables[i].signature.ascii,
                                         tables[i].address, tables[i].length) )
            num_tables++;
    }

    /*
     * No need to add or subtract anything because struct acpi_table_xsdt
     * includes one array slot already, and we have filtered out the original
     * MADT and we are going to add a custom built MADT.
     */
    size += num_tables * sizeof(xsdt->table_offset_entry[0]);

    xsdt = xzalloc_bytes(size);
    if ( !xsdt )
    {
        printk("Unable to allocate memory for XSDT table\n");
        rc = -ENOMEM;
        goto out;
    }

    /* Copy the native XSDT table header. */
    rsdp = acpi_os_map_memory(acpi_os_get_root_pointer(), sizeof(*rsdp));
    if ( !rsdp )
    {
        printk("Unable to map RSDP\n");
        rc = -EINVAL;
        goto out;
    }
    /*
     * Note the header is the same for both RSDT and XSDT, so it's fine to
     * copy the native RSDT header to the Xen crafted XSDT if no native
     * XSDT is available.
     */
    if ( rsdp->revision > 1 && rsdp->xsdt_physical_address )
        xsdt_paddr = rsdp->xsdt_physical_address;
    else
        xsdt_paddr = rsdp->rsdt_physical_address;

    acpi_os_unmap_memory(rsdp, sizeof(*rsdp));
    table = acpi_os_map_memory(xsdt_paddr, sizeof(*table));
    if ( !table )
    {
        printk("Unable to map XSDT\n");
        rc = -EINVAL;
        goto out;
    }
    xsdt->header = *table;
    acpi_os_unmap_memory(table, sizeof(*table));

    /*
     * In case the header is an RSDT copy, unconditionally ensure it has
     * an XSDT sig.
     */
    xsdt->header.signature[0] = 'X';

    /* Add the custom MADT. */
    xsdt->table_offset_entry[0] = madt_addr;

    /* Copy the addresses of the rest of the allowed tables. */
    for( i = 0, j = 1; i < acpi_gbl_root_table_list.count; i++ )
    {
        if ( pvh_acpi_xsdt_table_allowed(tables[i].signature.ascii,
                                         tables[i].address, tables[i].length) )
        {
            /* Handle FADT specially - modify it to clear legacy device flags */
            if ( !strncmp(tables[i].signature.ascii, ACPI_SIG_FADT, ACPI_NAME_SIZE) )
            {
                unsigned long new_fadt_addr;
                rc = pvh_setup_acpi_fadt(d, tables[i].address, &new_fadt_addr);
                if ( rc )
                {
                    printk("Failed to setup modified FADT: %d\n", rc);
                    goto out;
                }
                xsdt->table_offset_entry[j++] = new_fadt_addr;
            }
            else
                xsdt->table_offset_entry[j++] = tables[i].address;
        }
    }

    xsdt->header.revision = 1;
    xsdt->header.length = size;
    /*
     * Calling acpi_tb_checksum here is a layering violation, but
     * introducing a wrapper for such simple usage seems overkill.
     */
    xsdt->header.checksum -= acpi_tb_checksum(ACPI_CAST_PTR(u8, xsdt), size);

    /* Place the new XSDT in guest memory space. */
    if ( pvh_steal_ram(d, size, 0, GB(4), addr) )
    {
        printk("Unable to find guest RAM for XSDT\n");
        rc = -ENOMEM;
        goto out;
    }

    /* Mark this region as E820_ACPI. */
    if ( pvh_add_mem_range(d, *addr, *addr + size, E820_ACPI) )
        printk("Unable to add XSDT region to memory map\n");

    rc = hvm_copy_to_guest_phys(*addr, xsdt, size, d->vcpu[0]);
    if ( rc )
    {
        printk("Unable to copy XSDT into guest memory\n");
        goto out;
    }

    rc = 0;

 out:
    xfree(xsdt);

    return rc;
}

int __init hwdom_pvh_setup_acpi(struct domain *d, paddr_t start_info)
{
    unsigned long pfn, nr_pages;
    paddr_t madt_paddr, xsdt_paddr, rsdp_paddr;
    unsigned int i;
    int rc;
    struct acpi_table_rsdp *native_rsdp, rsdp = {
        .signature = ACPI_SIG_RSDP,
        .revision = 2,
        .length = sizeof(rsdp),
    };


    /* Scan top-level tables and add their regions to the guest memory map. */
    for( i = 0; i < acpi_gbl_root_table_list.count; i++ )
    {
        const char *sig = acpi_gbl_root_table_list.tables[i].signature.ascii;
        unsigned long addr = acpi_gbl_root_table_list.tables[i].address;
        unsigned long size = acpi_gbl_root_table_list.tables[i].length;

        /*
         * Make sure the original MADT is also mapped, so that Dom0 can
         * properly access the data returned by _MAT methods in case it's
         * re-using MADT memory.
         */
        if ( strncmp(sig, ACPI_SIG_MADT, ACPI_NAME_SIZE)
             ? pvh_acpi_table_allowed(sig, addr, size)
             : !acpi_memory_banned(addr, size) )
             pvh_add_mem_range(d, addr, addr + size, E820_ACPI);
    }

    /* Identity map ACPI e820 regions. */
    for ( i = 0; i < d->arch.nr_e820; i++ )
    {
        if ( d->arch.e820[i].type != E820_ACPI &&
             d->arch.e820[i].type != E820_NVS )
            continue;

        pfn = PFN_DOWN(d->arch.e820[i].addr);
        nr_pages = PFN_UP((d->arch.e820[i].addr & ~PAGE_MASK) +
                          d->arch.e820[i].size);

        /* Memory below 1MB has been dealt with by pvh_populate_p2m(). */
        if ( pfn < PFN_DOWN(MB(1)) )
        {
            if ( pfn + nr_pages <= PFN_DOWN(MB(1)) )
                continue;

            /* This shouldn't happen, but is easy to deal with. */
            nr_pages -= PFN_DOWN(MB(1)) - pfn;
            pfn = PFN_DOWN(MB(1));
        }

        rc = modify_identity_mmio(d, pfn, nr_pages, true);
        if ( rc )
        {
            printk("Failed to map ACPI region [%#lx, %#lx) into Dom%u memory map\n",
                   pfn, pfn + nr_pages, d->domain_id);
            return rc;
        }
    }

    rc = pvh_setup_acpi_madt(d, &madt_paddr);
    if ( rc )
        return rc;

    rc = pvh_setup_acpi_xsdt(d, madt_paddr, &xsdt_paddr);
    if ( rc )
        return rc;

    /* Craft a custom RSDP. */
    native_rsdp = acpi_os_map_memory(acpi_os_get_root_pointer(), sizeof(rsdp));
    if ( !native_rsdp )
    {
        printk("Failed to map native RSDP\n");
        return -ENOMEM;
    }
    memcpy(rsdp.oem_id, native_rsdp->oem_id, sizeof(rsdp.oem_id));
    acpi_os_unmap_memory(native_rsdp, sizeof(rsdp));
    rsdp.xsdt_physical_address = xsdt_paddr;
    /*
     * Calling acpi_tb_checksum here is a layering violation, but
     * introducing a wrapper for such simple usage seems overkill.
     */
    rsdp.checksum -= acpi_tb_checksum(ACPI_CAST_PTR(u8, &rsdp),
                                      ACPI_RSDP_REV0_SIZE);
    rsdp.extended_checksum -= acpi_tb_checksum(ACPI_CAST_PTR(u8, &rsdp),
                                               sizeof(rsdp));

    /*
     * Place the new RSDP in guest memory space.
     *
     * NB: this RSDP is not going to replace the original RSDP, which should
     * still be accessible to the guest. However that RSDP is going to point to
     * the native RSDT, and should not be used for the Dom0 kernel's boot
     * purposes (we keep it visible for post boot access).
     */
    if ( pvh_steal_ram(d, sizeof(rsdp), 0, GB(4), &rsdp_paddr) )
    {
        printk("Unable to allocate guest RAM for RSDP\n");
        return -ENOMEM;
    }

    /* Mark this region as E820_ACPI. */
    if ( pvh_add_mem_range(d, rsdp_paddr, rsdp_paddr + sizeof(rsdp),
                           E820_ACPI) )
        printk("Unable to add RSDP region to memory map\n");

    /* Copy RSDP into guest memory. */
    rc = hvm_copy_to_guest_phys(rsdp_paddr, &rsdp, sizeof(rsdp), d->vcpu[0]);
    if ( rc )
    {
        printk("Unable to copy RSDP into guest memory\n");
        return rc;
    }

    /* Copy RSDP address to start_info. */
    rc = hvm_copy_to_guest_phys(start_info +
                                offsetof(struct hvm_start_info, rsdp_paddr),
                                &rsdp_paddr,
                                sizeof_field(struct hvm_start_info, rsdp_paddr),
                                d->vcpu[0]);
    if ( rc )
    {
        printk("Unable to copy RSDP address to start info\n");
        return rc;
    }

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
