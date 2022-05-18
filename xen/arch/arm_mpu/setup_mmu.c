/*
 * xen/arch/arm_mpu/setup_mmu.c
 *
 * Early bringup code for an ARMv7-A with virt extensions.
 *
 * Tim Deegan <tim@xen.org>
 * Copyright (c) 2011 Citrix Systems.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <xen/init.h>
#include <xen/libfdt/libfdt.h>
#include <xen/mm.h>
#include <xen/param.h>
#include <xen/pfn.h>
#include <asm/page.h>
#include <asm/setup.h>

#if defined (CONFIG_ARM_32) || (CONFIG_CACHE_COLORING)
static unsigned long opt_xenheap_megabytes __initdata;
integer_param("xenheap_megabytes", opt_xenheap_megabytes);

/*
 * Returns the end address of the highest region in the range s..e
 * with required size and alignment that does not conflict with the
 * modules from first_mod to nr_modules.
 *
 * For non-recursive callers first_mod should normally be 0 (all
 * modules and Xen itself) or 1 (all modules but not Xen).
 */
static paddr_t __init consider_modules(paddr_t s, paddr_t e,
                                       uint32_t size, paddr_t align,
                                       int first_mod)
{
    const struct bootmodules *mi = &bootinfo.modules;
    int i;
    int nr;

    s = (s+align-1) & ~(align-1);
    e = e & ~(align-1);

    if ( s > e ||  e - s < size )
        return 0;

    /* First check the boot modules */
    for ( i = first_mod; i < mi->nr_mods; i++ )
    {
        paddr_t mod_s = mi->module[i].start;
        paddr_t mod_e = mod_s + mi->module[i].size;

        if ( s < mod_e && mod_s < e )
        {
            mod_e = consider_modules(mod_e, e, size, align, i+1);
            if ( mod_e )
                return mod_e;

            return consider_modules(s, mod_s, size, align, i+1);
        }
    }

    /* Now check any fdt reserved areas. */

    nr = fdt_num_mem_rsv(device_tree_flattened);

    for ( ; i < mi->nr_mods + nr; i++ )
    {
        paddr_t mod_s, mod_e;

        if ( fdt_get_mem_rsv(device_tree_flattened,
                             i - mi->nr_mods,
                             &mod_s, &mod_e ) < 0 )
            /* If we can't read it, pretend it doesn't exist... */
            continue;

        /* fdt_get_mem_rsv returns length */
        mod_e += mod_s;

        if ( s < mod_e && mod_s < e )
        {
            mod_e = consider_modules(mod_e, e, size, align, i+1);
            if ( mod_e )
                return mod_e;

            return consider_modules(s, mod_s, size, align, i+1);
        }
    }

    /*
     * i is the current bootmodule we are evaluating, across all
     * possible kinds of bootmodules.
     *
     * When retrieving the corresponding reserved-memory addresses, we
     * need to index the bootinfo.reserved_mem bank starting from 0, and
     * only counting the reserved-memory modules. Hence, we need to use
     * i - nr.
     */
    nr += mi->nr_mods;
    for ( ; i - nr < bootinfo.reserved_mem.nr_banks; i++ )
    {
        paddr_t r_s = bootinfo.reserved_mem.bank[i - nr].start;
        paddr_t r_e = r_s + bootinfo.reserved_mem.bank[i - nr].size;

        if ( s < r_e && r_s < e )
        {
            r_e = consider_modules(r_e, e, size, align, i + 1);
            if ( r_e )
                return r_e;

            return consider_modules(s, r_s, size, align, i + 1);
        }
    }
    return e;
}

/*
 * Find a contiguous region that fits in the static heap region with
 * required size and alignment, and return the end address of the region
 * if found otherwise 0.
 */
static paddr_t __init fit_xenheap_in_static_heap(uint32_t size, paddr_t align)
{
    unsigned int i;
    paddr_t end = 0, aligned_start, aligned_end;
    paddr_t bank_start, bank_size, bank_end;

    for ( i = 0 ; i < bootinfo.reserved_mem.nr_banks; i++ )
    {
        if ( bootinfo.reserved_mem.bank[i].type != MEMBANK_STATIC_HEAP )
            continue;

        bank_start = bootinfo.reserved_mem.bank[i].start;
        bank_size = bootinfo.reserved_mem.bank[i].size;
        bank_end = bank_start + bank_size;

        if ( bank_size < size )
            continue;

        aligned_end = bank_end & ~(align - 1);
        aligned_start = (aligned_end - size) & ~(align - 1);

        if ( aligned_start > bank_start )
            /*
             * Allocate the xenheap as high as possible to keep low-memory
             * available (assuming the admin supplied region below 4GB)
             * for other use (e.g. domain memory allocation).
             */
            end = max(end, aligned_end);
    }

    return end;
}
#endif

void __init discard_initial_modules(void)
{
    struct bootmodules *mi = &bootinfo.modules;
    int i;

    for ( i = 0; i < mi->nr_mods; i++ )
    {
        paddr_t s = mi->module[i].start;
        paddr_t e = s + PAGE_ALIGN(mi->module[i].size);

        if ( mi->module[i].kind == BOOTMOD_XEN )
            continue;

        if ( !mfn_valid(maddr_to_mfn(s)) ||
             !mfn_valid(maddr_to_mfn(e)) )
            continue;

        fw_unreserved_regions(s, e, init_domheap_pages, 0);
    }

    mi->nr_mods = 0;

    remove_early_mappings();
}

#ifdef CONFIG_ARM_32
void __init setup_mm(void)
{
    paddr_t ram_start, ram_end, ram_size, e, bank_start, bank_end, bank_size;
    paddr_t static_heap_end = 0, static_heap_size = 0;
    unsigned long heap_pages, xenheap_pages, domheap_pages;
    unsigned int i;
    const uint32_t ctr = READ_CP32(CTR);

    if ( !bootinfo.mem.nr_banks )
        panic("No memory bank\n");

    /* We only supports instruction caches implementing the IVIPT extension. */
    if ( ((ctr >> CTR_L1IP_SHIFT) & CTR_L1IP_MASK) == ICACHE_POLICY_AIVIVT )
        panic("AIVIVT instruction cache not supported\n");

    init_pdx();

    ram_start = bootinfo.mem.bank[0].start;
    ram_size  = bootinfo.mem.bank[0].size;
    ram_end   = ram_start + ram_size;

    for ( i = 1; i < bootinfo.mem.nr_banks; i++ )
    {
        bank_start = bootinfo.mem.bank[i].start;
        bank_size = bootinfo.mem.bank[i].size;
        bank_end = bank_start + bank_size;

        ram_size  = ram_size + bank_size;
        ram_start = min(ram_start,bank_start);
        ram_end   = max(ram_end,bank_end);
    }

    total_pages = ram_size >> PAGE_SHIFT;

    if ( bootinfo.static_heap )
    {
        for ( i = 0 ; i < bootinfo.reserved_mem.nr_banks; i++ )
        {
            if ( bootinfo.reserved_mem.bank[i].type != MEMBANK_STATIC_HEAP )
                continue;

            bank_start = bootinfo.reserved_mem.bank[i].start;
            bank_size = bootinfo.reserved_mem.bank[i].size;
            bank_end = bank_start + bank_size;

            static_heap_size += bank_size;
            static_heap_end = max(static_heap_end, bank_end);
        }

        heap_pages = static_heap_size >> PAGE_SHIFT;
    }
    else
        heap_pages = total_pages;

    /*
     * If the user has not requested otherwise via the command line
     * then locate the xenheap using these constraints:
     *
     *  - must be contiguous
     *  - must be 32 MiB aligned
     *  - must not include Xen itself or the boot modules
     *  - must be at most 1GB or 1/32 the total RAM in the system (or static
          heap if enabled) if less
     *  - must be at least 32M
     *
     * We try to allocate the largest xenheap possible within these
     * constraints.
     */
    if ( opt_xenheap_megabytes )
        xenheap_pages = opt_xenheap_megabytes << (20-PAGE_SHIFT);
    else
    {
        xenheap_pages = (heap_pages/32 + 0x1fffUL) & ~0x1fffUL;
        xenheap_pages = max(xenheap_pages, 32UL<<(20-PAGE_SHIFT));
        xenheap_pages = min(xenheap_pages, 1UL<<(30-PAGE_SHIFT));
    }

    do
    {
        e = bootinfo.static_heap ?
            fit_xenheap_in_static_heap(pfn_to_paddr(xenheap_pages), MB(32)) :
            consider_modules(ram_start, ram_end,
                             pfn_to_paddr(xenheap_pages),
                             32<<20, 0);
        if ( e )
            break;

        xenheap_pages >>= 1;
    } while ( !opt_xenheap_megabytes && xenheap_pages > 32<<(20-PAGE_SHIFT) );

    if ( ! e )
        panic("Not enough space for xenheap\n");

    domheap_pages = heap_pages - xenheap_pages;

    printk("Xen heap: %"PRIpaddr"-%"PRIpaddr" (%lu pages%s)\n",
           e - (pfn_to_paddr(xenheap_pages)), e, xenheap_pages,
           opt_xenheap_megabytes ? ", from command-line" : "");
    printk("Dom heap: %lu pages\n", domheap_pages);

    /*
     * We need some memory to allocate the page-tables used for the
     * directmap mappings. So populate the boot allocator first.
     *
     * This requires us to set directmap_mfn_{start, end} first so the
     * direct-mapped Xenheap region can be avoided.
     */
    directmap_mfn_start = _mfn((e >> PAGE_SHIFT) - xenheap_pages);
    directmap_mfn_end = mfn_add(directmap_mfn_start, xenheap_pages);

    populate_boot_allocator();

    setup_directmap_mappings(mfn_x(directmap_mfn_start), xenheap_pages);

    /* Frame table covers all of RAM region, including holes */
    setup_frametable_mappings(ram_start, ram_end);
    max_page = PFN_DOWN(ram_end);

    /*
     * The allocators may need to use map_domain_page() (such as for
     * scrubbing pages). So we need to prepare the domheap area first.
     */
    if ( !init_domheap_mappings(smp_processor_id()) )
        panic("CPU%u: Unable to prepare the domheap page-tables\n",
              smp_processor_id());

    /* Add xenheap memory that was not already added to the boot allocator. */
    init_xenheap_pages(mfn_to_maddr(directmap_mfn_start),
                       mfn_to_maddr(directmap_mfn_end));

    init_staticmem_pages();
}
#else /* CONFIG_ARM_64 */
void __init setup_mm(void)
{
    const struct meminfo *banks = &bootinfo.mem;
    paddr_t ram_start = INVALID_PADDR;
    paddr_t ram_end = 0;
    paddr_t ram_size = 0;
    unsigned int i;

    init_pdx();

    /*
     * We need some memory to allocate the page-tables used for the directmap
     * mappings. But some regions may contain memory already allocated
     * for other uses (e.g. modules, reserved-memory...).
     *
     * For simplicity, add all the free regions in the boot allocator.
     */
    populate_boot_allocator();

    total_pages = 0;

    for ( i = 0; i < banks->nr_banks; i++ )
    {
        const struct membank *bank = &banks->bank[i];
        paddr_t bank_end = bank->start + bank->size;

        ram_size = ram_size + bank->size;
        ram_start = min(ram_start, bank->start);
        ram_end = max(ram_end, bank_end);

        setup_directmap_mappings(PFN_DOWN(bank->start),
                                 PFN_DOWN(bank->size));
    }

    total_pages += ram_size >> PAGE_SHIFT;

    directmap_virt_end = XENHEAP_VIRT_START + ram_end - ram_start;
    directmap_mfn_start = maddr_to_mfn(ram_start);
    directmap_mfn_end = maddr_to_mfn(ram_end);

    setup_frametable_mappings(ram_start, ram_end);
    max_page = PFN_DOWN(ram_end);

    init_staticmem_pages();
}
#endif
