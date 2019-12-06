#include <xen/init.h>
#include <xen/compile.h>
#include <xen/lib.h>
#include <xen/mm.h>
#include <xen/domain_page.h>
#include <xen/sched.h>
#include <xen/sizes.h>
#include <asm/irq.h>
#include <asm/regs.h>
#include <xen/errno.h>
#include <xen/err.h>
#include <xen/device_tree.h>
#include <xen/libfdt/libfdt.h>
#include <xen/guest_access.h>
#include <xen/iocap.h>
#include <xen/acpi.h>
#include <xen/vmap.h>
#include <xen/warning.h>
#include <acpi/actables.h>
#include <asm/device.h>
#include <asm/kernel.h>
#include <asm/setup.h>
#include <asm/platform.h>
#include <asm/psci.h>
#include <asm/setup.h>
#include <asm/cpufeature.h>
#include <asm/domain_build.h>
#include <asm/coloring.h>

#include <xen/irq.h>
#include <xen/grant_table.h>

static unsigned int __initdata opt_dom0_max_vcpus;
integer_param("dom0_max_vcpus", opt_dom0_max_vcpus);

static u64 __initdata dom0_mem;

static int __init parse_dom0_mem(const char *s)
{
    dom0_mem = parse_size_and_unit(s, &s);

    return *s ? -EINVAL : 0;
}
custom_param("dom0_mem", parse_dom0_mem);

struct map_range_data
{
    struct domain *d;
    p2m_type_t p2mt;
};

/* Override macros from asm/page.h to make them work with mfn_t */
#undef virt_to_mfn
#define virt_to_mfn(va) _mfn(__virt_to_mfn(va))

//#define DEBUG_11_ALLOCATION
#ifdef DEBUG_11_ALLOCATION
# define D11PRINT(fmt, args...) printk(XENLOG_DEBUG fmt, ##args)
#else
# define D11PRINT(fmt, args...) do {} while ( 0 )
#endif

/*
 * Amount of extra space required to dom0's device tree.  No new nodes
 * are added (yet) but one terminating reserve map entry (16 bytes) is
 * added.
 */
#define DOM0_FDT_EXTRA_SIZE (128 + sizeof(struct fdt_reserve_entry))

struct vcpu *__init alloc_dom0_vcpu0(struct domain *dom0)
{
    if ( opt_dom0_max_vcpus == 0 )
        opt_dom0_max_vcpus = num_online_cpus();
    if ( opt_dom0_max_vcpus > MAX_VIRT_CPUS )
        opt_dom0_max_vcpus = MAX_VIRT_CPUS;

    dom0->vcpu = xzalloc_array(struct vcpu *, opt_dom0_max_vcpus);
    if ( !dom0->vcpu )
        return NULL;
    dom0->max_vcpus = opt_dom0_max_vcpus;

    return alloc_vcpu(dom0, 0, 0);
}

static unsigned int __init get_allocation_size(paddr_t size)
{
    /*
     * get_order_from_bytes returns the order greater than or equal to
     * the given size, but we need less than or equal. Adding one to
     * the size pushes an evenly aligned size into the next order, so
     * we can then unconditionally subtract 1 from the order which is
     * returned.
     */
    return get_order_from_bytes(size + 1) - 1;
}

/*
 * Insert the given pages into a memory bank, banks are ordered by address.
 *
 * Returns false if the memory would be below bank 0 or we have run
 * out of banks. In this case it will free the pages.
 */
static bool __init insert_11_bank(struct domain *d,
                                  struct kernel_info *kinfo,
                                  struct page_info *pg,
                                  unsigned int order)
{
    int res, i;
    mfn_t smfn;
    paddr_t start, size;

    smfn = page_to_mfn(pg);
    start = mfn_to_maddr(smfn);
    size = pfn_to_paddr(1UL << order);

    D11PRINT("Allocated %#"PRIpaddr"-%#"PRIpaddr" (%ldMB/%ldMB, order %d)\n",
             start, start + size,
             1UL << (order + PAGE_SHIFT - 20),
             /* Don't want format this as PRIpaddr (16 digit hex) */
             (unsigned long)(kinfo->unassigned_mem >> 20),
             order);

    if ( kinfo->mem.nr_banks > 0 &&
         size < MB(128) &&
         start + size < kinfo->mem.bank[0].start )
    {
        D11PRINT("Allocation below bank 0 is too small, not using\n");
        goto fail;
    }

    res = guest_physmap_add_page(d, _gfn(mfn_x(smfn)), smfn, order);
    if ( res )
        panic("Failed map pages to DOM0: %d", res);

    kinfo->unassigned_mem -= size;

    if ( kinfo->mem.nr_banks == 0 )
    {
        kinfo->mem.bank[0].start = start;
        kinfo->mem.bank[0].size = size;
        kinfo->mem.nr_banks = 1;
        return true;
    }

    for( i = 0; i < kinfo->mem.nr_banks; i++ )
    {
        struct membank *bank = &kinfo->mem.bank[i];

        /* If possible merge new memory into the start of the bank */
        if ( bank->start == start+size )
        {
            bank->start = start;
            bank->size += size;
            return true;
        }

        /* If possible merge new memory onto the end of the bank */
        if ( start == bank->start + bank->size )
        {
            bank->size += size;
            return true;
        }

        /*
         * Otherwise if it is below this bank insert new memory in a
         * new bank before this one. If there was a lower bank we
         * could have inserted the memory into/before we would already
         * have done so, so this must be the right place.
         */
        if ( start + size < bank->start && kinfo->mem.nr_banks < NR_MEM_BANKS )
        {
            memmove(bank + 1, bank,
                    sizeof(*bank) * (kinfo->mem.nr_banks - i));
            kinfo->mem.nr_banks++;
            bank->start = start;
            bank->size = size;
            return true;
        }
    }

    if ( i == kinfo->mem.nr_banks && kinfo->mem.nr_banks < NR_MEM_BANKS )
    {
        struct membank *bank = &kinfo->mem.bank[kinfo->mem.nr_banks];

        bank->start = start;
        bank->size = size;
        kinfo->mem.nr_banks++;
        return true;
    }

    /* If we get here then there are no more banks to fill. */

fail:
    free_domheap_pages(pg, order);
    return false;
}

/*
 * This is all pretty horrible.
 *
 * Requirements:
 *
 * 1. The dom0 kernel should be loaded within the first 128MB of RAM. This
 *    is necessary at least for Linux zImage kernels, which are all we
 *    support today.
 * 2. We want to put the dom0 kernel, ramdisk and DTB in the same
 *    bank. Partly this is just easier for us to deal with, but also
 *    the ramdisk and DTB must be placed within a certain proximity of
 *    the kernel within RAM.
 * 3. For dom0 we want to place as much of the RAM as we reasonably can
 *    below 4GB, so that it can be used by non-LPAE enabled kernels (32-bit)
 *    or when a device assigned to dom0 can only do 32-bit DMA access.
 * 4. For 32-bit dom0 the kernel must be located below 4GB.
 * 5. We want to have a few largers banks rather than many smaller ones.
 *
 * For the first two requirements we need to make sure that the lowest
 * bank is sufficiently large.
 *
 * For convenience we also sort the banks by physical address.
 *
 * The memory allocator does not really give us the flexibility to
 * meet these requirements directly. So instead of proceed as follows:
 *
 * We first allocate the largest allocation we can as low as we
 * can. This then becomes the first bank. This bank must be at least
 * 128MB (or dom0_mem if that is smaller).
 *
 * Then we start allocating more memory, trying to allocate the
 * largest possible size and trying smaller sizes until we
 * successfully allocate something.
 *
 * We then try and insert this memory in to the list of banks. If it
 * can be merged into an existing bank then this is trivial.
 *
 * If the new memory is before the first bank (and cannot be merged into it)
 * and is at least 128M then we allow it, otherwise we give up. Since the
 * allocator prefers to allocate high addresses first and the first bank has
 * already been allocated to be as low as possible this likely means we
 * wouldn't have been able to allocate much more memory anyway.
 *
 * Otherwise we insert a new bank. If we've reached MAX_NR_BANKS then
 * we give up.
 *
 * For 32-bit domain we require that the initial allocation for the
 * first bank is under 4G. For 64-bit domain, the first bank is preferred
 * to be allocated under 4G. Then for the subsequent allocations we
 * initially allocate memory only from below 4GB. Once that runs out
 * (as described above) we allow higher allocations and continue until
 * that runs out (or we have allocated sufficient dom0 memory).
 */
static void __init allocate_memory_11(struct domain *d,
                                      struct kernel_info *kinfo)
{
    const unsigned int min_low_order =
        get_order_from_bytes(min_t(paddr_t, dom0_mem, MB(128)));
    const unsigned int min_order = get_order_from_bytes(MB(4));
    struct page_info *pg;
    unsigned int order = get_allocation_size(kinfo->unassigned_mem);
    int i;

    bool lowmem = true;
    unsigned int bits;

    /*
     * TODO: Implement memory bank allocation when DOM0 is not direct
     * mapped
     */
    BUG_ON(!is_domain_direct_mapped(d));

    printk("Allocating 1:1 mappings totalling %ldMB for dom0:\n",
           /* Don't want format this as PRIpaddr (16 digit hex) */
           (unsigned long)(kinfo->unassigned_mem >> 20));

    kinfo->mem.nr_banks = 0;

    /*
     * First try and allocate the largest thing we can as low as
     * possible to be bank 0.
     */
    while ( order >= min_low_order )
    {
        for ( bits = order ; bits <= (lowmem ? 32 : PADDR_BITS); bits++ )
        {
            pg = alloc_domheap_pages(d, order, MEMF_bits(bits));
            if ( pg != NULL )
            {
                if ( !insert_11_bank(d, kinfo, pg, order) )
                    BUG(); /* Cannot fail for first bank */

                goto got_bank0;
            }
        }
        order--;
    }

    /* Failed to allocate bank0 under 4GB */
    if ( is_32bit_domain(d) )
        panic("Unable to allocate first memory bank.");

    /* Try to allocate memory from above 4GB */
    printk(XENLOG_INFO "No bank has been allocated below 4GB.\n");
    lowmem = false;

 got_bank0:

    /*
     * If we failed to allocate bank0 under 4GB, continue allocating
     * memory from above 4GB and fill in banks.
     */
    order = get_allocation_size(kinfo->unassigned_mem);
    while ( kinfo->unassigned_mem && kinfo->mem.nr_banks < NR_MEM_BANKS )
    {
        pg = alloc_domheap_pages(d, order, lowmem ? MEMF_bits(32) : 0);
        if ( !pg )
        {
            order --;

            if ( lowmem && order < min_low_order)
            {
                D11PRINT("Failed at min_low_order, allow high allocations\n");
                order = get_allocation_size(kinfo->unassigned_mem);
                lowmem = false;
                continue;
            }
            if ( order >= min_order )
                continue;

            /* No more we can do */
            break;
        }

        if ( !insert_11_bank(d, kinfo, pg, order) )
        {
            if ( kinfo->mem.nr_banks == NR_MEM_BANKS )
                /* Nothing more we can do. */
                break;

            if ( lowmem )
            {
                D11PRINT("Allocation below bank 0, allow high allocations\n");
                order = get_allocation_size(kinfo->unassigned_mem);
                lowmem = false;
                continue;
            }
            else
            {
                D11PRINT("Allocation below bank 0\n");
                break;
            }
        }

        /*
         * Success, next time around try again to get the largest order
         * allocation possible.
         */
        order = get_allocation_size(kinfo->unassigned_mem);
    }

    if ( kinfo->unassigned_mem )
        printk("WARNING: Failed to allocate requested dom0 memory."
               /* Don't want format this as PRIpaddr (16 digit hex) */
               " %ldMB unallocated\n",
               (unsigned long)kinfo->unassigned_mem >> 20);

    for( i = 0; i < kinfo->mem.nr_banks; i++ )
    {
        printk("BANK[%d] %#"PRIpaddr"-%#"PRIpaddr" (%ldMB)\n",
               i,
               kinfo->mem.bank[i].start,
               kinfo->mem.bank[i].start + kinfo->mem.bank[i].size,
               /* Don't want format this as PRIpaddr (16 digit hex) */
               (unsigned long)(kinfo->mem.bank[i].size >> 20));
    }
}

static bool __init allocate_bank_memory(struct domain *d,
                                       struct kernel_info *kinfo,
                                       gfn_t sgfn,
                                       unsigned long tot_size)
{
    int res;
    struct page_info *pg;
    struct membank *bank;
    unsigned int max_order = ~0;

    bank = &kinfo->mem.bank[kinfo->mem.nr_banks];
    bank->start = gfn_to_gaddr(sgfn);
    bank->size = tot_size;

    while ( tot_size > 0 )
    {
        unsigned int order = get_allocation_size(tot_size);

        order = min(max_order, order);

        pg = alloc_domheap_pages(d, order, 0);
        if ( !pg )
        {
            /*
             * If we can't allocate one page, then it is unlikely to
             * succeed in the next iteration. So bail out.
             */
            if ( !order )
                return false;

            /*
             * If we can't allocate memory with order, then it is
             * unlikely to succeed in the next iteration.
             * Record the order - 1 to avoid re-trying.
             */
            max_order = order - 1;
            continue;
        }

        res = guest_physmap_add_page(d, sgfn, page_to_mfn(pg), order);
        if ( res )
        {
            dprintk(XENLOG_ERR, "Failed map pages to DOMU: %d", res);
            return false;
        }

        sgfn = gfn_add(sgfn, 1UL << order);
        tot_size -= (1ULL << (PAGE_SHIFT + order));
    }

    kinfo->mem.nr_banks++;
    kinfo->unassigned_mem -= bank->size;

    return true;
}

static void __init allocate_memory(struct domain *d, struct kernel_info *kinfo)
{
    unsigned int i;
    unsigned long bank_size;

    printk(XENLOG_INFO "Allocating mappings totalling %ldMB for %pd:\n",
           /* Don't want format this as PRIpaddr (16 digit hex) */
           (unsigned long)(kinfo->unassigned_mem >> 20), d);

    kinfo->mem.nr_banks = 0;
    bank_size = MIN(GUEST_RAM0_SIZE, kinfo->unassigned_mem);
    if ( !allocate_bank_memory(d, kinfo, gaddr_to_gfn(GUEST_RAM0_BASE),
                               bank_size) )
        goto fail;

    bank_size = MIN(GUEST_RAM1_SIZE, kinfo->unassigned_mem);
    if ( !allocate_bank_memory(d, kinfo, gaddr_to_gfn(GUEST_RAM1_BASE),
                               bank_size) )
        goto fail;

    if ( kinfo->unassigned_mem )
        goto fail;

    for( i = 0; i < kinfo->mem.nr_banks; i++ )
    {
        printk(XENLOG_INFO "%pd BANK[%d] %#"PRIpaddr"-%#"PRIpaddr" (%ldMB)\n",
               d,
               i,
               kinfo->mem.bank[i].start,
               kinfo->mem.bank[i].start + kinfo->mem.bank[i].size,
               /* Don't want format this as PRIpaddr (16 digit hex) */
               (unsigned long)(kinfo->mem.bank[i].size >> 20));
    }

    return;

fail:
    panic("Failed to allocate requested domain memory."
          /* Don't want format this as PRIpaddr (16 digit hex) */
          " %ldKB unallocated. Fix the VMs configurations.\n",
          (unsigned long)kinfo->unassigned_mem >> 10);
}
#ifdef CONFIG_COLORING
static void __init allocate_colored_memory(
    struct domain *d, struct kernel_info *kinfo)
{
    int res;
    struct page_info *pg;
    struct membank *bank;
    gfn_t sgfn = gaddr_to_gfn(GUEST_RAM0_BASE);

    bank = &kinfo->mem.bank[kinfo->mem.nr_banks];
    bank->size = kinfo->unassigned_mem;
    bank->start = GUEST_RAM0_BASE;
    printk("Allocating colored mappings totalling %ldMB for DOM %d:\n",
           (unsigned long)(kinfo->unassigned_mem >> 20), d->domain_id);

    while ( kinfo->unassigned_mem > 0 )
    {
        pg = alloc_col_domheap_page(d, 0);
        if ( !pg )
            panic("ERROR: Failed alloc pages to DOM: %d\n", d->domain_id);

        res = guest_physmap_add_page(d, sgfn, page_to_mfn(pg), 0);

        if ( res )
        {
            printk("ERROR: Failed map pages to DOM: %d", d->domain_id);
            BUG();
        }
        sgfn = gfn_add(sgfn,1);
        kinfo->unassigned_mem -= PAGE_SIZE;
    }

    kinfo->mem.nr_banks++;
}
#endif
static int __init write_properties(struct domain *d, struct kernel_info *kinfo,
                                   const struct dt_device_node *node)
{
    const char *bootargs = NULL;
    const struct dt_property *prop, *status = NULL;
    int res = 0;
    int had_dom0_bootargs = 0;

    if ( kinfo->cmdline && kinfo->cmdline[0] )
        bootargs = &kinfo->cmdline[0];

    dt_for_each_property_node (node, prop)
    {
        const void *prop_data = prop->value;
        u32 prop_len = prop->length;

        /*
         * In chosen node:
         *
         * * remember xen,dom0-bootargs if we don't already have
         *   bootargs (from module #1, above).
         * * remove bootargs,  xen,dom0-bootargs, xen,xen-bootargs,
         *   linux,initrd-start and linux,initrd-end.
         * * remove stdout-path.
         * * remove bootargs, linux,uefi-system-table,
         *   linux,uefi-mmap-start, linux,uefi-mmap-size,
         *   linux,uefi-mmap-desc-size, and linux,uefi-mmap-desc-ver
         *   (since EFI boot is not currently supported in dom0).
         */
        if ( dt_node_path_is_equal(node, "/chosen") )
        {
            if ( dt_property_name_is_equal(prop, "xen,xen-bootargs") ||
                 dt_property_name_is_equal(prop, "linux,initrd-start") ||
                 dt_property_name_is_equal(prop, "linux,initrd-end") ||
                 dt_property_name_is_equal(prop, "stdout-path") ||
                 dt_property_name_is_equal(prop, "linux,uefi-system-table") ||
                 dt_property_name_is_equal(prop, "linux,uefi-mmap-start") ||
                 dt_property_name_is_equal(prop, "linux,uefi-mmap-size") ||
                 dt_property_name_is_equal(prop, "linux,uefi-mmap-desc-size") ||
                 dt_property_name_is_equal(prop, "linux,uefi-mmap-desc-ver"))
                continue;

            if ( dt_property_name_is_equal(prop, "xen,dom0-bootargs") )
            {
                had_dom0_bootargs = 1;
                bootargs = prop->value;
                continue;
            }
            if ( dt_property_name_is_equal(prop, "bootargs") )
            {
                if ( !bootargs  && !had_dom0_bootargs )
                    bootargs = prop->value;
                continue;
            }
        }

        /* Don't expose the property "xen,passthrough" to the guest */
        if ( dt_property_name_is_equal(prop, "xen,passthrough") )
            continue;

        /* Remember and skip the status property as Xen may modify it later */
        if ( dt_property_name_is_equal(prop, "status") )
        {
            status = prop;
            continue;
        }

        res = fdt_property(kinfo->fdt, prop->name, prop_data, prop_len);

        if ( res )
            return res;
    }

    /*
     * Override the property "status" to disable the device when it's
     * marked for passthrough.
     */
    if ( dt_device_for_passthrough(node) )
        res = fdt_property_string(kinfo->fdt, "status", "disabled");
    else if ( status )
        res = fdt_property(kinfo->fdt, "status", status->value,
                           status->length);

    if ( res )
        return res;

    if ( dt_node_path_is_equal(node, "/chosen") )
    {
        const struct bootmodule *initrd = kinfo->initrd_bootmodule;

        if ( bootargs )
        {
            res = fdt_property(kinfo->fdt, "bootargs", bootargs,
                               strlen(bootargs) + 1);
            if ( res )
                return res;
        }

        /*
         * If the bootloader provides an initrd, we must create a placeholder
         * for the initrd properties. The values will be replaced later.
         */
        if ( initrd && initrd->size )
        {
            u64 a = 0;
            res = fdt_property(kinfo->fdt, "linux,initrd-start", &a, sizeof(a));
            if ( res )
                return res;

            res = fdt_property(kinfo->fdt, "linux,initrd-end", &a, sizeof(a));
            if ( res )
                return res;
        }
    }

    return 0;
}

/*
 * Helper to write an interrupts with the GIC format
 * This code is assuming the irq is an PPI.
 */

typedef __be32 gic_interrupt_t[3];

static void __init set_interrupt(gic_interrupt_t interrupt,
                                 unsigned int irq,
                                 unsigned int cpumask,
                                 unsigned int level)
{
    __be32 *cells = interrupt;
    bool is_ppi = !!(irq < 32);

    BUG_ON(irq < 16);
    irq -= (is_ppi) ? 16: 32; /* PPIs start at 16, SPIs at 32 */

    /* See linux Documentation/devicetree/bindings/interrupt-controller/arm,gic.txt */
    dt_set_cell(&cells, 1, is_ppi); /* is a PPI? */
    dt_set_cell(&cells, 1, irq);
    dt_set_cell(&cells, 1, (cpumask << 8) | level);
}

/*
 * Helper to set interrupts for a node in the flat device tree.
 * It needs 2 property:
 *  "interrupts": contains the list of interrupts
 *  "interrupt-parent": link to the GIC
 */
static int __init fdt_property_interrupts(void *fdt, gic_interrupt_t *intr,
                                          unsigned num_irq)
{
    int res;

    res = fdt_property(fdt, "interrupts", intr, sizeof (intr[0]) * num_irq);
    if ( res )
        return res;

    res = fdt_property_cell(fdt, "interrupt-parent",
                            dt_interrupt_controller->phandle);

    return res;
}

static int __init make_memory_node(const struct domain *d,
                                   void *fdt,
                                   int addrcells, int sizecells,
                                   const struct kernel_info *kinfo)
{
    int res, i;
    int reg_size = addrcells + sizecells;
    int nr_cells = reg_size * (kinfo->mem.nr_banks + (is_hardware_domain(d) ?
                               bootinfo.reserved_mem.nr_banks : 0));
    __be32 reg[nr_cells];
    __be32 *cells;

    dt_dprintk("Create memory node (reg size %d, nr cells %d)\n",
               reg_size, nr_cells);

    /* ePAPR 3.4 */
    res = fdt_begin_node(fdt, "memory");
    if ( res )
        return res;

    res = fdt_property_string(fdt, "device_type", "memory");
    if ( res )
        return res;

    cells = &reg[0];
    for ( i = 0 ; i < kinfo->mem.nr_banks; i++ )
    {
        u64 start = kinfo->mem.bank[i].start;
        u64 size = kinfo->mem.bank[i].size;

        dt_dprintk("  Bank %d: %#"PRIx64"->%#"PRIx64"\n",
                   i, start, start + size);

        dt_child_set_range(&cells, addrcells, sizecells, start, size);
    }

    if ( is_hardware_domain(d) )
    {
        for ( i = 0; i < bootinfo.reserved_mem.nr_banks; i++ )
        {
            u64 start = bootinfo.reserved_mem.bank[i].start;
            u64 size = bootinfo.reserved_mem.bank[i].size;

            dt_dprintk("  Bank %d: %#"PRIx64"->%#"PRIx64"\n",
                    i, start, start + size);

            dt_child_set_range(&cells, addrcells, sizecells, start, size);
        }
    }

    res = fdt_property(fdt, "reg", reg, sizeof(reg));
    if ( res )
        return res;

    res = fdt_end_node(fdt);

    return res;
}

static int __init make_hypervisor_node(struct domain *d,
                                       const struct kernel_info *kinfo,
                                       int addrcells, int sizecells)
{
    const char compat[] =
        "xen,xen-"__stringify(XEN_VERSION)"."__stringify(XEN_SUBVERSION)"\0"
        "xen,xen";
    __be32 reg[4];
    gic_interrupt_t intr;
    __be32 *cells;
    int res;
    void *fdt = kinfo->fdt;

    dt_dprintk("Create hypervisor node\n");

    /*
     * Sanity-check address sizes, since addresses and sizes which do
     * not take up exactly 4 or 8 bytes are not supported.
     */
    if ((addrcells != 1 && addrcells != 2) ||
        (sizecells != 1 && sizecells != 2))
        panic("Cannot cope with this size");

    /* See linux Documentation/devicetree/bindings/arm/xen.txt */
    res = fdt_begin_node(fdt, "hypervisor");
    if ( res )
        return res;

    /* Cannot use fdt_property_string due to embedded nulls */
    res = fdt_property(fdt, "compatible", compat, sizeof(compat));
    if ( res )
        return res;

    /* reg 0 is grant table space */
    cells = &reg[0];
    dt_child_set_range(&cells, addrcells, sizecells,
                       kinfo->gnttab_start, kinfo->gnttab_size);
    res = fdt_property(fdt, "reg", reg,
                       dt_cells_to_size(addrcells + sizecells));
    if ( res )
        return res;

    BUG_ON(d->arch.evtchn_irq == 0);

    /*
     * Interrupt event channel upcall:
     *  - Active-low level-sensitive
     *  - All CPUs
     *  TODO: Handle properly the cpumask;
     */
    set_interrupt(intr, d->arch.evtchn_irq, 0xf, DT_IRQ_TYPE_LEVEL_LOW);
    res = fdt_property_interrupts(fdt, &intr, 1);
    if ( res )
        return res;

    res = fdt_end_node(fdt);

    return res;
}

static int __init make_psci_node(void *fdt,
                                 const struct dt_device_node *parent)
{
    int res;
    const char compat[] =
        "arm,psci-1.0""\0"
        "arm,psci-0.2""\0"
        "arm,psci";

    dt_dprintk("Create PSCI node\n");

    /* See linux Documentation/devicetree/bindings/arm/psci.txt */
    res = fdt_begin_node(fdt, "psci");
    if ( res )
        return res;

    res = fdt_property(fdt, "compatible", compat, sizeof(compat));
    if ( res )
        return res;

    res = fdt_property_string(fdt, "method", "hvc");
    if ( res )
        return res;

    res = fdt_property_cell(fdt, "cpu_off", PSCI_cpu_off);
    if ( res )
        return res;

    res = fdt_property_cell(fdt, "cpu_on", PSCI_cpu_on);
    if ( res )
        return res;

    res = fdt_end_node(fdt);

    return res;
}

static int __init make_cpus_node(const struct domain *d, void *fdt,
                                 const struct dt_device_node *parent)
{
    int res;
    const struct dt_device_node *cpus = dt_find_node_by_path("/cpus");
    const struct dt_device_node *npcpu;
    unsigned int cpu;
    const void *compatible = NULL;
    u32 len;
    /* Placeholder for cpu@ + a 32-bit number + \0 */
    char buf[15];
    u32 clock_frequency;
    bool clock_valid;
    uint64_t mpidr_aff;

    dt_dprintk("Create cpus node\n");

    if ( !cpus )
    {
        dprintk(XENLOG_ERR, "Missing /cpus node in the device tree?\n");
        return -ENOENT;
    }

    /*
     * Get the compatible property of CPUs from the device tree.
     * We are assuming that all CPUs are the same so we are just look
     * for the first one.
     * TODO: Handle compatible per VCPU
     */
    dt_for_each_child_node(cpus, npcpu)
    {
        if ( dt_device_type_is_equal(npcpu, "cpu") )
        {
            compatible = dt_get_property(npcpu, "compatible", &len);
            clock_valid = dt_property_read_u32(npcpu, "clock-frequency",
                                            &clock_frequency);
            break;
        }
    }

    if ( !compatible )
    {
        dprintk(XENLOG_ERR, "Can't find cpu in the device tree?\n");
        return -ENOENT;
    }

    /* See Linux Documentation/devicetree/booting-without-of.txt
     * section III.5.b
     */
    res = fdt_begin_node(fdt, "cpus");
    if ( res )
        return res;

    res = fdt_property_cell(fdt, "#address-cells", 1);
    if ( res )
        return res;

    res = fdt_property_cell(fdt, "#size-cells", 0);
    if ( res )
        return res;

    for ( cpu = 0; cpu < d->max_vcpus; cpu++ )
    {
        /*
         * According to ARM CPUs bindings, the reg field should match
         * the MPIDR's affinity bits. We will use AFF0 and AFF1 when
         * constructing the reg value of the guest at the moment, for it
         * is enough for the current max vcpu number.
         */
        mpidr_aff = vcpuid_to_vaffinity(cpu);
        dt_dprintk("Create cpu@%"PRIx64" (logical CPUID: %d) node\n",
                   mpidr_aff, cpu);

        snprintf(buf, sizeof(buf), "cpu@%"PRIx64, mpidr_aff);
        res = fdt_begin_node(fdt, buf);
        if ( res )
            return res;

        res = fdt_property(fdt, "compatible", compatible, len);
        if ( res )
            return res;

        res = fdt_property_string(fdt, "device_type", "cpu");
        if ( res )
            return res;

        res = fdt_property_cell(fdt, "reg", mpidr_aff);
        if ( res )
            return res;

        if ( clock_valid )
        {
            res = fdt_property_cell(fdt, "clock-frequency", clock_frequency);
            if ( res )
                return res;
        }

        if ( is_64bit_domain(d) )
        {
            res = fdt_property_string(fdt, "enable-method", "psci");
            if ( res )
                return res;
        }

        res = fdt_end_node(fdt);
        if ( res )
            return res;
    }

    res = fdt_end_node(fdt);

    return res;
}

static int __init make_gic_node(const struct domain *d, void *fdt,
                                const struct dt_device_node *node)
{
    const struct dt_device_node *gic = dt_interrupt_controller;
    int res = 0;
    const void *addrcells, *sizecells;
    u32 addrcells_len, sizecells_len;

    /*
     * Xen currently supports only a single GIC. Discard any secondary
     * GIC entries.
     */
    if ( node != dt_interrupt_controller )
    {
        dt_dprintk("  Skipping (secondary GIC)\n");
        return 0;
    }

    dt_dprintk("Create gic node\n");

    res = fdt_begin_node(fdt, "interrupt-controller");
    if ( res )
        return res;

    /*
     * The value of the property "phandle" in the property "interrupts"
     * to know on which interrupt controller the interrupt is wired.
     */
    if ( gic->phandle )
    {
        dt_dprintk("  Set phandle = 0x%x\n", gic->phandle);
        res = fdt_property_cell(fdt, "phandle", gic->phandle);
        if ( res )
            return res;
    }

    addrcells = dt_get_property(gic, "#address-cells", &addrcells_len);
    if ( addrcells )
    {
        res = fdt_property(fdt, "#address-cells", addrcells, addrcells_len);
        if ( res )
            return res;
    }

    sizecells = dt_get_property(gic, "#size-cells", &sizecells_len);
    if ( sizecells )
    {
        res = fdt_property(fdt, "#size-cells", sizecells, sizecells_len);
        if ( res )
            return res;
    }

    res = fdt_property_cell(fdt, "#interrupt-cells", 3);
    if ( res )
        return res;

    res = fdt_property(fdt, "interrupt-controller", NULL, 0);
    if ( res )
        return res;

    res = gic_make_hwdom_dt_node(d, node, fdt);
    if ( res )
        return res;

    res = fdt_end_node(fdt);

    return res;
}

static int __init make_timer_node(const struct domain *d, void *fdt,
                                  const struct dt_device_node *node)
{
    static const struct dt_device_match timer_ids[] __initconst =
    {
        DT_MATCH_COMPATIBLE("arm,armv7-timer"),
        DT_MATCH_COMPATIBLE("arm,armv8-timer"),
        { /* sentinel */ },
    };
    struct dt_device_node *dev;
    u32 len;
    const void *compatible;
    int res;
    unsigned int irq;
    gic_interrupt_t intrs[3];
    u32 clock_frequency;
    bool clock_valid;

    dt_dprintk("Create timer node\n");

    dev = dt_find_matching_node(NULL, timer_ids);
    if ( !dev )
    {
        dprintk(XENLOG_ERR, "Missing timer node in the device tree?\n");
        return -FDT_ERR_XEN(ENOENT);
    }

    compatible = dt_get_property(dev, "compatible", &len);
    if ( !compatible )
    {
        dprintk(XENLOG_ERR, "Can't find compatible property for timer node\n");
        return -FDT_ERR_XEN(ENOENT);
    }

    res = fdt_begin_node(fdt, "timer");
    if ( res )
        return res;

    res = fdt_property(fdt, "compatible", compatible, len);
    if ( res )
        return res;

    /* The timer IRQ is emulated by Xen. It always exposes an active-low
     * level-sensitive interrupt */

    irq = timer_get_irq(TIMER_PHYS_SECURE_PPI);
    dt_dprintk("  Secure interrupt %u\n", irq);
    set_interrupt(intrs[0], irq, 0xf, DT_IRQ_TYPE_LEVEL_LOW);

    irq = timer_get_irq(TIMER_PHYS_NONSECURE_PPI);
    dt_dprintk("  Non secure interrupt %u\n", irq);
    set_interrupt(intrs[1], irq, 0xf, DT_IRQ_TYPE_LEVEL_LOW);

    irq = timer_get_irq(TIMER_VIRT_PPI);
    dt_dprintk("  Virt interrupt %u\n", irq);
    set_interrupt(intrs[2], irq, 0xf, DT_IRQ_TYPE_LEVEL_LOW);

    res = fdt_property_interrupts(fdt, intrs, 3);
    if ( res )
        return res;

    clock_valid = dt_property_read_u32(dev, "clock-frequency",
                                       &clock_frequency);
    if ( clock_valid )
    {
        res = fdt_property_cell(fdt, "clock-frequency", clock_frequency);
        if ( res )
            return res;
    }

    res = fdt_end_node(fdt);

    return res;
}

/*
 * This function is used as part of the device tree generation for Dom0
 * on ACPI systems, and DomUs started directly from Xen based on device
 * tree information.
 */
int __init make_chosen_node(const struct kernel_info *kinfo)
{
    int res;
    const char *bootargs = NULL;
    const struct bootmodule *mod = kinfo->kernel_bootmodule;
    void *fdt = kinfo->fdt;

    dt_dprintk("Create chosen node\n");
    res = fdt_begin_node(fdt, "chosen");
    if ( res )
        return res;

    if ( kinfo->cmdline && kinfo->cmdline[0] )
    {
        bootargs = &kinfo->cmdline[0];
        res = fdt_property(fdt, "bootargs", bootargs, strlen(bootargs) + 1);
        if ( res )
           return res;
    }

    /*
     * If the bootloader provides an initrd, we must create a placeholder
     * for the initrd properties. The values will be replaced later.
     */
    if ( mod && mod->size )
    {
        u64 a = 0;
        res = fdt_property(kinfo->fdt, "linux,initrd-start", &a, sizeof(a));
        if ( res )
            return res;

        res = fdt_property(kinfo->fdt, "linux,initrd-end", &a, sizeof(a));
        if ( res )
            return res;
    }

    res = fdt_end_node(fdt);

    return res;
}

int __init map_irq_to_domain(struct domain *d, unsigned int irq,
                             bool need_mapping, const char *devname)
{
    int res;

    res = irq_permit_access(d, irq);
    if ( res )
    {
        printk(XENLOG_ERR "Unable to permit to dom%u access to IRQ %u\n",
               d->domain_id, irq);
        return res;
    }

    if ( need_mapping )
    {
        /*
         * Checking the return of vgic_reserve_virq is not
         * necessary. It should not fail except when we try to map
         * the IRQ twice. This can legitimately happen if the IRQ is shared
         */
        vgic_reserve_virq(d, irq);

        res = route_irq_to_guest(d, irq, irq, devname);
        if ( res < 0 )
        {
            printk(XENLOG_ERR "Unable to map IRQ%"PRId32" to dom%d\n",
                   irq, d->domain_id);
            return res;
        }
    }

    dt_dprintk("  - IRQ: %u\n", irq);
    return 0;
}

static int __init map_dt_irq_to_domain(const struct dt_device_node *dev,
                                       const struct dt_irq *dt_irq,
                                       void *data)
{
    struct domain *d = data;
    unsigned int irq = dt_irq->irq;
    int res;
    bool need_mapping = !dt_device_for_passthrough(dev);

    if ( irq < NR_LOCAL_IRQS )
    {
        printk(XENLOG_ERR "%s: IRQ%"PRId32" is not a SPI\n",
               dt_node_name(dev), irq);
        return -EINVAL;
    }

    /* Setup the IRQ type */
    res = irq_set_spi_type(irq, dt_irq->type);
    if ( res )
    {
        printk(XENLOG_ERR
               "%s: Unable to setup IRQ%"PRId32" to dom%d\n",
               dt_node_name(dev), irq, d->domain_id);
        return res;
    }

    res = map_irq_to_domain(d, irq, need_mapping, dt_node_name(dev));

    return 0;
}

static int __init map_range_to_domain(const struct dt_device_node *dev,
                                      u64 addr, u64 len,
                                      void *data)
{
    struct map_range_data *mr_data = data;
    struct domain *d = mr_data->d;
    bool need_mapping = !dt_device_for_passthrough(dev);
    int res;

    res = iomem_permit_access(d, paddr_to_pfn(addr),
                              paddr_to_pfn(PAGE_ALIGN(addr + len - 1)));
    if ( res )
    {
        printk(XENLOG_ERR "Unable to permit to dom%d access to"
               " 0x%"PRIx64" - 0x%"PRIx64"\n",
               d->domain_id,
               addr & PAGE_MASK, PAGE_ALIGN(addr + len) - 1);
        return res;
    }

    if ( need_mapping )
    {
        res = map_regions_p2mt(d,
                               gaddr_to_gfn(addr),
                               PFN_UP(len),
                               maddr_to_mfn(addr),
                               mr_data->p2mt);

        if ( res < 0 )
        {
            printk(XENLOG_ERR "Unable to map 0x%"PRIx64
                   " - 0x%"PRIx64" in domain %d\n",
                   addr & PAGE_MASK, PAGE_ALIGN(addr + len) - 1,
                   d->domain_id);
            return res;
        }
    }

    dt_dprintk("  - MMIO: %010"PRIx64" - %010"PRIx64" P2MType=%x\n",
               addr, addr + len, mr_data->p2mt);

    return 0;
}

/*
 * For a node which describes a discoverable bus (such as a PCI bus)
 * then we may need to perform additional mappings in order to make
 * the child resources available to domain 0.
 */
static int __init map_device_children(struct domain *d,
                                      const struct dt_device_node *dev,
                                      p2m_type_t p2mt)
{
    struct map_range_data mr_data = { .d = d, .p2mt = p2mt };
    int ret;

    if ( dt_device_type_is_equal(dev, "pci") )
    {
        dt_dprintk("Mapping children of %s to guest\n",
                   dt_node_full_name(dev));

        ret = dt_for_each_irq_map(dev, &map_dt_irq_to_domain, d);
        if ( ret < 0 )
            return ret;

        ret = dt_for_each_range(dev, &map_range_to_domain, &mr_data);
        if ( ret < 0 )
            return ret;
    }

    return 0;
}

/*
 * For a given device node:
 *  - Give permission to the guest to manage IRQ and MMIO range
 *  - Retrieve the IRQ configuration (i.e edge/level) from device tree
 * When the device is not marked for guest passthrough:
 *  - Assign the device to the guest if it's protected by an IOMMU
 *  - Map the IRQs and iomem regions to DOM0
 */
static int __init handle_device(struct domain *d, struct dt_device_node *dev,
                                p2m_type_t p2mt)
{
    unsigned int nirq;
    unsigned int naddr;
    unsigned int i;
    int res;
    struct dt_raw_irq rirq;
    u64 addr, size;
    bool need_mapping = !dt_device_for_passthrough(dev);

    nirq = dt_number_of_irq(dev);
    naddr = dt_number_of_address(dev);

    dt_dprintk("%s passthrough = %d nirq = %d naddr = %u\n",
               dt_node_full_name(dev), need_mapping, nirq, naddr);

    if ( dt_device_is_protected(dev) && need_mapping )
    {
        dt_dprintk("%s setup iommu\n", dt_node_full_name(dev));
        res = iommu_assign_dt_device(d, dev);
        if ( res )
        {
            printk(XENLOG_ERR "Failed to setup the IOMMU for %s\n",
                   dt_node_full_name(dev));
            return res;
        }
    }

    /* Give permission and map IRQs */
    for ( i = 0; i < nirq; i++ )
    {
        res = dt_device_get_raw_irq(dev, i, &rirq);
        if ( res )
        {
            printk(XENLOG_ERR "Unable to retrieve irq %u for %s\n",
                   i, dt_node_full_name(dev));
            return res;
        }

        /*
         * Don't map IRQ that have no physical meaning
         * ie: IRQ whose controller is not the GIC
         */
        if ( rirq.controller != dt_interrupt_controller )
        {
            dt_dprintk("irq %u not connected to primary controller. Connected to %s\n",
                      i, dt_node_full_name(rirq.controller));
            continue;
        }

        res = platform_get_irq(dev, i);
        if ( res < 0 )
        {
            printk(XENLOG_ERR "Unable to get irq %u for %s\n",
                   i, dt_node_full_name(dev));
            return res;
        }

        res = map_irq_to_domain(d, res, need_mapping, dt_node_name(dev));
        if ( res )
            return res;
    }

    /* Give permission and map MMIOs */
    for ( i = 0; i < naddr; i++ )
    {
        struct map_range_data mr_data = { .d = d, .p2mt = p2mt };
        res = dt_device_get_address(dev, i, &addr, &size);
        if ( res )
        {
            printk(XENLOG_ERR "Unable to retrieve address %u for %s\n",
                   i, dt_node_full_name(dev));
            return res;
        }

        res = map_range_to_domain(dev, addr, size, &mr_data);
        if ( res )
            return res;
    }

    res = map_device_children(d, dev, p2mt);
    if ( res )
        return res;

    return 0;
}

static int __init handle_node(struct domain *d, struct kernel_info *kinfo,
                              struct dt_device_node *node,
                              p2m_type_t p2mt)
{
    static const struct dt_device_match skip_matches[] __initconst =
    {
        DT_MATCH_COMPATIBLE("xen,xen"),
        DT_MATCH_COMPATIBLE("xen,multiboot-module"),
        DT_MATCH_COMPATIBLE("multiboot,module"),
        DT_MATCH_COMPATIBLE("arm,psci"),
        DT_MATCH_COMPATIBLE("arm,psci-0.2"),
        DT_MATCH_COMPATIBLE("arm,psci-1.0"),
        DT_MATCH_COMPATIBLE("arm,cortex-a7-pmu"),
        DT_MATCH_COMPATIBLE("arm,cortex-a15-pmu"),
        DT_MATCH_COMPATIBLE("arm,cortex-a53-edac"),
        DT_MATCH_COMPATIBLE("arm,armv8-pmuv3"),
        DT_MATCH_PATH("/cpus"),
        DT_MATCH_TYPE("memory"),
        /* The memory mapped timer is not supported by Xen. */
        DT_MATCH_COMPATIBLE("arm,armv7-timer-mem"),
        { /* sentinel */ },
    };
    static const struct dt_device_match timer_matches[] __initconst =
    {
        DT_MATCH_TIMER,
        { /* sentinel */ },
    };
    static const struct dt_device_match reserved_matches[] __initconst =
    {
        DT_MATCH_PATH("/psci"),
        DT_MATCH_PATH("/memory"),
        DT_MATCH_PATH("/hypervisor"),
        { /* sentinel */ },
    };
    struct dt_device_node *child;
    int res;
    const char *name;
    const char *path;

    path = dt_node_full_name(node);

    dt_dprintk("handle %s\n", path);

    /* Skip theses nodes and the sub-nodes */
    if ( dt_match_node(skip_matches, node) )
    {
        dt_dprintk("  Skip it (matched)\n");
        return 0;
    }
    if ( platform_device_is_blacklisted(node) )
    {
        dt_dprintk("  Skip it (blacklisted)\n");
        return 0;
    }

    /*
     * Replace these nodes with our own. Note that the original may be
     * used_by DOMID_XEN so this check comes first.
     */
    if ( device_get_class(node) == DEVICE_GIC )
        return make_gic_node(d, kinfo->fdt, node);
    if ( dt_match_node(timer_matches, node) )
        return make_timer_node(d, kinfo->fdt, node);

    /* Skip nodes used by Xen */
    if ( dt_device_used_by(node) == DOMID_XEN )
    {
        dt_dprintk("  Skip it (used by Xen)\n");
        return 0;
    }

    /*
     * Even if the IOMMU device is not used by Xen, it should not be
     * passthrough to DOM0
     */
    if ( device_get_class(node) == DEVICE_IOMMU )
    {
        dt_dprintk(" IOMMU, skip it\n");
        return 0;
    }

    /*
     * Xen is using some path for its own purpose. Warn if a node
     * already exists with the same path.
     */
    if ( dt_match_node(reserved_matches, node) )
        printk(XENLOG_WARNING
               "WARNING: Path %s is reserved, skip the node as we may re-use the path.\n",
               path);

    /*
     * reserved-memory ranges should be mapped as normal memory in the
     * p2m.
     */
    if ( !strcmp(dt_node_name(node), "reserved-memory") )
        p2mt = p2m_mmio_direct_c;

    res = handle_device(d, node, p2mt);
    if ( res)
        return res;

    /*
     * The property "name" is used to have a different name on older FDT
     * version. We want to keep the name retrieved during the tree
     * structure creation, that is store in the node path.
     */
    name = strrchr(path, '/');
    name = name ? name + 1 : path;

    res = fdt_begin_node(kinfo->fdt, name);
    if ( res )
        return res;

    res = write_properties(d, kinfo, node);
    if ( res )
        return res;

    for ( child = node->child; child != NULL; child = child->sibling )
    {
        res = handle_node(d, kinfo, child, p2mt);
        if ( res )
            return res;
    }

    if ( node == dt_host )
    {
        int addrcells = dt_child_n_addr_cells(node);
        int sizecells = dt_child_n_size_cells(node);

        /*
         * It is safe to allocate the event channel here because all the
         * PPIs used by the hardware domain have been registered.
         */
        evtchn_allocate(d);

        /*
         * The hypervisor node should always be created after all nodes
         * from the host DT have been parsed.
         */
        res = make_hypervisor_node(d, kinfo, addrcells, sizecells);
        if ( res )
            return res;

        res = make_psci_node(kinfo->fdt, node);
        if ( res )
            return res;

        res = make_cpus_node(d, kinfo->fdt, node);
        if ( res )
            return res;

        res = make_memory_node(d, kinfo->fdt, addrcells, sizecells, kinfo);
        if ( res )
            return res;

    }

    res = fdt_end_node(kinfo->fdt);

    return res;
}

static int __init make_gicv2_domU_node(const struct domain *d, void *fdt)
{
    int res = 0;
    __be32 reg[(GUEST_ROOT_ADDRESS_CELLS + GUEST_ROOT_SIZE_CELLS) * 2];
    __be32 *cells;

    res = fdt_begin_node(fdt, "interrupt-controller@"__stringify(GUEST_GICD_BASE));
    if ( res )
        return res;

    res = fdt_property_cell(fdt, "#address-cells", 0);
    if ( res )
        return res;

    res = fdt_property_cell(fdt, "#interrupt-cells", 3);
    if ( res )
        return res;

    res = fdt_property(fdt, "interrupt-controller", NULL, 0);
    if ( res )
        return res;

    res = fdt_property_string(fdt, "compatible", "arm,gic-400");
    if ( res )
        return res;

    cells = &reg[0];
    dt_child_set_range(&cells, GUEST_ROOT_ADDRESS_CELLS, GUEST_ROOT_SIZE_CELLS,
                       GUEST_GICD_BASE, GUEST_GICD_SIZE);
    dt_child_set_range(&cells, GUEST_ROOT_ADDRESS_CELLS, GUEST_ROOT_SIZE_CELLS,
                       GUEST_GICC_BASE, GUEST_GICC_SIZE);

    res = fdt_property(fdt, "reg", reg, sizeof(reg));
    if (res)
        return res;

    res = fdt_property_cell(fdt, "linux,phandle", GUEST_PHANDLE_GIC);
    if (res)
        return res;

    res = fdt_property_cell(fdt, "phandle", GUEST_PHANDLE_GIC);
    if (res)
        return res;

    res = fdt_end_node(fdt);

    return res;
}

static int __init make_gicv3_domU_node(const struct domain *d, void *fdt)
{
    int res = 0;
    __be32 reg[(GUEST_ROOT_ADDRESS_CELLS + GUEST_ROOT_SIZE_CELLS) * 2];
    __be32 *cells;

    res = fdt_begin_node(fdt, "interrupt-controller@"__stringify(GUEST_GICV3_GICD_BASE));
    if ( res )
        return res;

    res = fdt_property_cell(fdt, "#address-cells", 0);
    if ( res )
        return res;

    res = fdt_property_cell(fdt, "#interrupt-cells", 3);
    if ( res )
        return res;

    res = fdt_property(fdt, "interrupt-controller", NULL, 0);
    if ( res )
        return res;

    res = fdt_property_string(fdt, "compatible", "arm,gic-v3");
    if ( res )
        return res;

    cells = &reg[0];
    dt_child_set_range(&cells, GUEST_ROOT_ADDRESS_CELLS, GUEST_ROOT_SIZE_CELLS,
                       GUEST_GICV3_GICD_BASE, GUEST_GICV3_GICD_SIZE);
    dt_child_set_range(&cells, GUEST_ROOT_ADDRESS_CELLS, GUEST_ROOT_SIZE_CELLS,
                       GUEST_GICV3_GICR0_BASE, GUEST_GICV3_GICR0_SIZE);

    res = fdt_property(fdt, "reg", reg, sizeof(reg));
    if (res)
        return res;

    res = fdt_property_cell(fdt, "linux,phandle", GUEST_PHANDLE_GIC);
    if (res)
        return res;

    res = fdt_property_cell(fdt, "phandle", GUEST_PHANDLE_GIC);
    if (res)
        return res;

    res = fdt_end_node(fdt);

    return res;
}

static int __init make_gic_domU_node(const struct domain *d, void *fdt)
{
    switch ( d->arch.vgic.version )
    {
    case GIC_V3:
        return make_gicv3_domU_node(d, fdt);
    case GIC_V2:
        return make_gicv2_domU_node(d, fdt);
    default:
        panic("Unsupported GIC version");
    }
}

static int __init make_timer_domU_node(const struct domain *d, void *fdt)
{
    int res;
    gic_interrupt_t intrs[3];

    res = fdt_begin_node(fdt, "timer");
    if ( res )
        return res;

    if ( !is_64bit_domain(d) )
    {
        res = fdt_property_string(fdt, "compatible", "arm,armv7-timer");
        if ( res )
            return res;
    }
    else
    {
        res = fdt_property_string(fdt, "compatible", "arm,armv8-timer");
        if ( res )
            return res;
    }

    set_interrupt(intrs[0], GUEST_TIMER_PHYS_S_PPI, 0xf, DT_IRQ_TYPE_LEVEL_LOW);
    set_interrupt(intrs[1], GUEST_TIMER_PHYS_NS_PPI, 0xf, DT_IRQ_TYPE_LEVEL_LOW);
    set_interrupt(intrs[2], GUEST_TIMER_VIRT_PPI, 0xf, DT_IRQ_TYPE_LEVEL_LOW);

    res = fdt_property(fdt, "interrupts", intrs, sizeof (intrs[0]) * 3);
    if ( res )
        return res;

    res = fdt_property_cell(fdt, "interrupt-parent",
                            GUEST_PHANDLE_GIC);
    if (res)
        return res;

    res = fdt_end_node(fdt);

    return res;
}

#ifdef CONFIG_SBSA_VUART_CONSOLE
static int __init make_vpl011_uart_node(const struct domain *d, void *fdt)
{
    int res;
    gic_interrupt_t intr;
    __be32 reg[GUEST_ROOT_ADDRESS_CELLS + GUEST_ROOT_SIZE_CELLS];
    __be32 *cells;

    res = fdt_begin_node(fdt, "sbsa-uart@"__stringify(GUEST_PL011_BASE));
    if ( res )
        return res;

    res = fdt_property_string(fdt, "compatible", "arm,sbsa-uart");
    if ( res )
        return res;

    cells = &reg[0];
    dt_child_set_range(&cells, GUEST_ROOT_ADDRESS_CELLS,
                       GUEST_ROOT_SIZE_CELLS, GUEST_PL011_BASE,
                       GUEST_PL011_SIZE);
    if ( res )
        return res;
    res = fdt_property(fdt, "reg", reg, sizeof(reg));
    if ( res )
        return res;

    set_interrupt(intr, GUEST_VPL011_SPI, 0xf, DT_IRQ_TYPE_LEVEL_HIGH);

    res = fdt_property(fdt, "interrupts", intr, sizeof (intr));
    if ( res )
        return res;

    res = fdt_property_cell(fdt, "interrupt-parent",
                            GUEST_PHANDLE_GIC);
    if ( res )
        return res;

    /* Use a default baud rate of 115200. */
    fdt_property_u32(fdt, "current-speed", 115200);

    res = fdt_end_node(fdt);
    if ( res )
        return res;

    return 0;
}
#endif

static int __init copy_properties(void *fdt, void *pfdt, int nodeoff)
{
    int propoff, nameoff, r;
    const struct fdt_property *prop;

    for ( propoff = fdt_first_property_offset(pfdt, nodeoff);
          propoff >= 0;
          propoff = fdt_next_property_offset(pfdt, propoff) )
    {

        if ( !(prop = fdt_get_property_by_offset(pfdt, propoff, NULL)) )
            return -FDT_ERR_INTERNAL;

        nameoff = fdt32_to_cpu(prop->nameoff);
        r = fdt_property(fdt, fdt_string(pfdt, nameoff),
                         prop->data, fdt32_to_cpu(prop->len));
        if ( r )
            return r;
    }

    /* FDT_ERR_NOTFOUND => There is no more properties for this node */
    return ( propoff != -FDT_ERR_NOTFOUND ) ? propoff : 0;
}

static int __init copy_node(void *fdt, void *pfdt, int nodeoff, int depth)
{
    int r;

    r = fdt_begin_node(fdt, fdt_get_name(pfdt, nodeoff, NULL));
    if ( r )
        return r;

    r = copy_properties(fdt, pfdt, nodeoff);
    if ( r )
        return r;

    for ( nodeoff = fdt_first_subnode(pfdt, nodeoff);
          nodeoff >= 0;
          nodeoff = fdt_next_subnode(pfdt, nodeoff) )
    {
        r = copy_node(fdt, pfdt, nodeoff, depth + 1);
        if ( r )
            return r;
    }

    if ( nodeoff != -FDT_ERR_NOTFOUND )
        return nodeoff;

    r = fdt_end_node(fdt);
    if ( r )
        return r;

    return 0;
}

/*
 * The max size for DT is 2MB. However, the generated DT is small, 4KB
 * are enough for now, but we might have to increase it in the future.
 */
#define DOMU_DTB_SIZE 4096
static int __init prepare_dtb_domU(struct domain *d, struct kernel_info *kinfo)
{
    int addrcells, sizecells;
    int ret;

    addrcells = GUEST_ROOT_ADDRESS_CELLS;
    sizecells = GUEST_ROOT_SIZE_CELLS;

    kinfo->fdt = xmalloc_bytes(DOMU_DTB_SIZE);
    if ( kinfo->fdt == NULL )
        return -ENOMEM;

    ret = fdt_create(kinfo->fdt, DOMU_DTB_SIZE);
    if ( ret < 0 )
        goto err;

    ret = fdt_finish_reservemap(kinfo->fdt);
    if ( ret < 0 )
        goto err;

    ret = fdt_begin_node(kinfo->fdt, "/");
    if ( ret < 0 )
        goto err;

    ret = fdt_property_cell(kinfo->fdt, "#address-cells", addrcells);
    if ( ret )
        goto err;

    ret = fdt_property_cell(kinfo->fdt, "#size-cells", sizecells);
    if ( ret )
        goto err;

    ret = make_chosen_node(kinfo);
    if ( ret )
        goto err;

    ret = make_psci_node(kinfo->fdt, NULL);
    if ( ret )
        goto err;

    ret = make_cpus_node(d, kinfo->fdt, NULL);
    if ( ret )
        goto err;

    ret = make_memory_node(d, kinfo->fdt, addrcells, sizecells, kinfo);
    if ( ret )
        goto err;

    ret = make_gic_domU_node(d, kinfo->fdt);
    if ( ret )
        goto err;

    ret = make_timer_domU_node(d, kinfo->fdt);
    if ( ret )
        goto err;

    if ( kinfo->vpl011 )
    {
        ret = -EINVAL;
#ifdef CONFIG_SBSA_VUART_CONSOLE
        ret = make_vpl011_uart_node(d, kinfo->fdt);
#endif
        if ( ret )
            goto err;
    }

    if ( kinfo->dtb_bootmodule ) {
        int nodeoff, res;
        void *pfdt;

        pfdt = ioremap_cache(kinfo->dtb_bootmodule->start,
                             kinfo->dtb_bootmodule->size);
        if ( pfdt == NULL )
            return -EFAULT;

        if ( fdt_magic(pfdt) != FDT_MAGIC )
            return -EINVAL;

        nodeoff = fdt_path_offset(pfdt, "/passthrough");
        if (nodeoff < 0)
            return nodeoff;

        res = copy_node(kinfo->fdt, pfdt, nodeoff, 0);
        if ( res )
            return res;

        nodeoff = fdt_path_offset(pfdt, "/aliases");
        if ( nodeoff >= 0 )
        {
            res = copy_node(kinfo->fdt, pfdt, nodeoff, 0);
            if ( res )
                return res;
        }


        iounmap(pfdt);
    }

    ret = fdt_end_node(kinfo->fdt);
    if ( ret < 0 )
        goto err;

    ret = fdt_finish(kinfo->fdt);
    if ( ret < 0 )
        goto err;

    return 0;

  err:
    printk("Device tree generation failed (%d).\n", ret);
    xfree(kinfo->fdt);

    return -EINVAL;
}

static int __init prepare_dtb_hwdom(struct domain *d, struct kernel_info *kinfo)
{
    const p2m_type_t default_p2mt = p2m_mmio_direct_c;
    const void *fdt;
    int new_size;
    int ret;

    ASSERT(dt_host && (dt_host->sibling == NULL));

    fdt = device_tree_flattened;

    new_size = fdt_totalsize(fdt) + DOM0_FDT_EXTRA_SIZE;
    kinfo->fdt = xmalloc_bytes(new_size);
    if ( kinfo->fdt == NULL )
        return -ENOMEM;

    ret = fdt_create(kinfo->fdt, new_size);
    if ( ret < 0 )
        goto err;

    fdt_finish_reservemap(kinfo->fdt);

    ret = handle_node(d, kinfo, dt_host, default_p2mt);
    if ( ret )
        goto err;

    ret = fdt_finish(kinfo->fdt);
    if ( ret < 0 )
        goto err;

    return 0;

  err:
    printk("Device tree generation failed (%d).\n", ret);
    xfree(kinfo->fdt);
    return -EINVAL;
}

static void __init dtb_load(struct kernel_info *kinfo)
{
    unsigned long left;

    printk("Loading dom0 DTB to 0x%"PRIpaddr"-0x%"PRIpaddr"\n",
           kinfo->dtb_paddr, kinfo->dtb_paddr + fdt_totalsize(kinfo->fdt));

    left = copy_to_guest_phys_flush_dcache(kinfo->d, kinfo->dtb_paddr,
                                           kinfo->fdt,
                                           fdt_totalsize(kinfo->fdt));

    if ( left != 0 )
        panic("Unable to copy the DTB to dom0 memory (left = %lu bytes)", left);
    xfree(kinfo->fdt);
}

static void __init initrd_load(struct kernel_info *kinfo)
{
    const struct bootmodule *mod = kinfo->initrd_bootmodule;
    paddr_t load_addr = kinfo->initrd_paddr;
    paddr_t paddr, len;
    int node;
    int res;
    __be32 val[2];
    __be32 *cellp;
    void __iomem *initrd;

    if ( !mod || !mod->size )
        return;

    paddr = mod->start;
    len = mod->size;

    printk("Loading dom0 initrd from %"PRIpaddr" to 0x%"PRIpaddr"-0x%"PRIpaddr"\n",
           paddr, load_addr, load_addr + len);

    /* Fix up linux,initrd-start and linux,initrd-end in /chosen */
    node = fdt_path_offset(kinfo->fdt, "/chosen");
    if ( node < 0 )
        panic("Cannot find the /chosen node");

    cellp = (__be32 *)val;
    dt_set_cell(&cellp, ARRAY_SIZE(val), load_addr);
    res = fdt_setprop_inplace(kinfo->fdt, node, "linux,initrd-start",
                              val, sizeof(val));
    if ( res )
        panic("Cannot fix up \"linux,initrd-start\" property");

    cellp = (__be32 *)val;
    dt_set_cell(&cellp, ARRAY_SIZE(val), load_addr + len);
    res = fdt_setprop_inplace(kinfo->fdt, node, "linux,initrd-end",
                              val, sizeof(val));
    if ( res )
        panic("Cannot fix up \"linux,initrd-end\" property");

    initrd = ioremap_wc(paddr, len);
    if ( !initrd )
        panic("Unable to map the hwdom initrd");

    res = copy_to_guest_phys_flush_dcache(kinfo->d, load_addr,
                                          initrd, len);
    if ( res != 0 )
        panic("Unable to copy the initrd in the hwdom memory");
}

/*
 * Allocate the event channel PPIs and setup the HVM_PARAM_CALLBACK_IRQ.
 * The allocated IRQ will be found in d->arch.evtchn_irq.
 *
 * Note that this should only be called once all PPIs used by the
 * hardware domain have been registered.
 */
void __init evtchn_allocate(struct domain *d)
{
    int res;
    u64 val;

    res = vgic_allocate_ppi(d);
    if ( res < 0 )
        panic("Unable to allocate a PPI for the event channel interrupt\n");

    d->arch.evtchn_irq = res;

    printk("Allocating PPI %u for event channel interrupt\n",
           d->arch.evtchn_irq);

    /* Set the value of domain param HVM_PARAM_CALLBACK_IRQ */
    val = MASK_INSR(HVM_PARAM_CALLBACK_TYPE_PPI,
                    HVM_PARAM_CALLBACK_IRQ_TYPE_MASK);
    /* Active-low level-sensitive  */
    val |= MASK_INSR(HVM_PARAM_CALLBACK_TYPE_PPI_FLAG_LOW_LEVEL,
                     HVM_PARAM_CALLBACK_TYPE_PPI_FLAG_MASK);
    val |= d->arch.evtchn_irq;
    d->arch.hvm_domain.params[HVM_PARAM_CALLBACK_IRQ] = val;
}

static void __init find_gnttab_region(struct domain *d,
                                      struct kernel_info *kinfo)
{
    /*
     * The region used by Xen on the memory will never be mapped in DOM0
     * memory layout. Therefore it can be used for the grant table.
     *
     * Only use the text section as it's always present and will contain
     * enough space for a large grant table
     */
    kinfo->gnttab_start = __pa(_stext);
    kinfo->gnttab_size = gnttab_dom0_frames() << PAGE_SHIFT;

#ifdef CONFIG_ARM_32
    /*
     * The gnttab region must be under 4GB in order to work with DOM0
     * using short page table.
     * In practice it's always the case because Xen is always located
     * below 4GB, but be safe.
     */
    BUG_ON((kinfo->gnttab_start + kinfo->gnttab_size) > GB(4));
#endif

    printk("Grant table range: %#"PRIpaddr"-%#"PRIpaddr"\n",
           kinfo->gnttab_start, kinfo->gnttab_start + kinfo->gnttab_size);
}

static int __init construct_domain(struct domain *d, struct kernel_info *kinfo)
{
    struct vcpu *saved_current;
    int i, cpu;
    struct vcpu *v = d->vcpu[0];
    struct cpu_user_regs *regs = &v->arch.cpu_info->guest_cpu_user_regs;

    BUG_ON(d->vcpu[0] == NULL);
    BUG_ON(v->is_initialised);

#ifdef CONFIG_ARM_64
    /* if aarch32 mode is not supported at EL1 do not allow 32-bit domain */
    if ( !(cpu_has_el1_32) && kinfo->type == DOMAIN_32BIT )
    {
        printk("Platform does not support 32-bit domain\n");
        return -EINVAL;
    }

    if ( is_64bit_domain(d) )
        vcpu_switch_to_aarch64_mode(v);

#endif

    /*
     * The following loads use the domain's p2m and require current to
     * be a vcpu of the domain, temporarily switch
     */
    saved_current = current;
    p2m_restore_state(v);
    set_current(v);

    /*
     * kernel_load will determine the placement of the kernel as well
     * as the initrd & fdt in RAM, so call it first.
     */
    kernel_load(kinfo);
    /* initrd_load will fix up the fdt, so call it before dtb_load */
    initrd_load(kinfo);
    dtb_load(kinfo);

    /* Now that we are done restore the original p2m and current. */
    set_current(saved_current);
    p2m_restore_state(saved_current);

    memset(regs, 0, sizeof(*regs));

    regs->pc = (register_t)kinfo->entry;

    if ( is_32bit_domain(d) )
    {
        regs->cpsr = PSR_GUEST32_INIT;

        /* FROM LINUX head.S
         *
         * Kernel startup entry point.
         * ---------------------------
         *
         * This is normally called from the decompressor code.  The requirements
         * are: MMU = off, D-cache = off, I-cache = dont care, r0 = 0,
         * r1 = machine nr, r2 = atags or dtb pointer.
         *...
         */
        regs->r0 = 0; /* SBZ */
        regs->r1 = 0xffffffff; /* We use DTB therefore no machine id */
        regs->r2 = kinfo->dtb_paddr;
    }
#ifdef CONFIG_ARM_64
    else
    {
        regs->cpsr = PSR_GUEST64_INIT;
        /* From linux/Documentation/arm64/booting.txt */
        regs->x0 = kinfo->dtb_paddr;
        regs->x1 = 0; /* Reserved for future use */
        regs->x2 = 0; /* Reserved for future use */
        regs->x3 = 0; /* Reserved for future use */
    }
#endif

    for ( i = 1, cpu = 0; i < d->max_vcpus; i++ )
    {
        cpu = cpumask_cycle(cpu, &cpu_online_map);
        if ( alloc_vcpu(d, i, cpu) == NULL )
        {
            printk("Failed to allocate dom0 vcpu %d on pcpu %d\n", i, cpu);
            break;
        }

        if ( is_64bit_domain(d) )
            vcpu_switch_to_aarch64_mode(d->vcpu[i]);
    }

    v->is_initialised = 1;
    clear_bit(_VPF_down, &v->pause_flags);

    return 0;
}

static int __init scan_pt_node(const void *pfdt,
                               int nodeoff, const char *name, int depth,
                               u32 address_cells, u32 size_cells,
                               void *data)
{
    int rc;
    struct dt_device_node *node;
    int len, i;
    const struct fdt_property *prop;
    struct kernel_info *kinfo = data;
    struct domain *d = kinfo->d;
    const __be32 *cell;

    if ( depth > 2 )
        return 0;

    prop = fdt_get_property_namelen(pfdt, nodeoff, "xen,reg",
                                    strlen("xen,reg"), &len);
    if ( prop != NULL )
    {
        paddr_t mstart, size, gstart;
        cell = (const __be32 *)prop->data;
        len = fdt32_to_cpu(prop->len) /
              ((address_cells*2 + size_cells) * sizeof (u32));

        for ( i = 0; i < len; i++ )
        {
            mstart = dt_next_cell(address_cells, &cell);
            size = dt_next_cell(size_cells, &cell);
            gstart = dt_next_cell(address_cells, &cell);

            rc = guest_physmap_add_entry(d, gaddr_to_gfn(gstart),
                                         maddr_to_mfn(mstart),
                                         get_order_from_bytes(size),
                                         p2m_mmio_direct_dev);
            if ( rc < 0 )
            {
                dprintk(XENLOG_ERR,
                        "Failed to map %"PRIpaddr" to the guest at%"PRIpaddr"\n",
                        mstart, gstart);
                return -EFAULT;
            }
        }
    }

    prop = fdt_get_property_namelen(pfdt, nodeoff, "xen,path",
                                    strlen("xen,path"), &len);
    if ( prop != NULL ) {
        node = dt_find_node_by_path((char *)prop->data);
        if ( node != NULL )
            rc = iommu_assign_dt_device(d, node);
        else
            dprintk(XENLOG_ERR, "Couldn't find node %s in host_dt!\n",
                    (char *)prop->data);
    }

    prop = fdt_get_property_namelen(pfdt, nodeoff, "interrupts",
                                    strlen("interrupts"), &len);
    if ( prop != NULL )
    {
        int pt_irq;
        fdt32_t *u = (fdt32_t*)prop->data;
        /* The GIC interrupt format is 3 cells per interrupt */
        len = fdt32_to_cpu(prop->len) / (3 * sizeof (u32));

        for ( i = 0; i < len; i++, u += 3 )
        {
            pt_irq = fdt32_to_cpu(*(u + 1)) + 32;

            vgic_reserve_virq(d, pt_irq);
            rc = route_irq_to_guest(d, pt_irq, pt_irq, "routed IRQ");
            if ( rc < 0 )
                return rc;
        }
    }

    return 0;
}

static int __init domain_adding_devices(struct domain *d,
                                        struct kernel_info *kinfo)
{
    void *pfdt;

    pfdt = ioremap_cache(kinfo->dtb_bootmodule->start,
            kinfo->dtb_bootmodule->size);
    if ( pfdt == NULL )
        return -EFAULT;

    device_tree_for_each_node(pfdt, scan_pt_node, kinfo);

    iounmap(pfdt);
    return 0;
}

static int __init construct_domU(struct domain *d,
                                 const struct dt_device_node *node)
{
    struct kernel_info kinfo = {};
    int rc;
    u64 mem;

    rc = dt_property_read_u64(node, "memory", &mem);
    if ( !rc )
    {
        printk("Error building DomU: cannot read \"memory\" property\n");
        return -EINVAL;
    }
    kinfo.unassigned_mem = (paddr_t)mem * SZ_1K;

    printk("*** LOADING DOMU cpus=%u memory=%"PRIx64"KB ***\n", d->max_vcpus, mem);

    kinfo.vpl011 = dt_property_read_bool(node, "vpl011");

    d->vcpu = xzalloc_array(struct vcpu *, d->max_vcpus);
    if ( !d->vcpu )
        return -ENOMEM;;
    if ( alloc_vcpu(d, 0, 0) == NULL )
        return -ENOMEM;
    d->max_pages = ~0U;

    kinfo.d = d;

    rc = kernel_probe(&kinfo, node);
    if ( rc < 0 )
        return rc;

#ifdef CONFIG_ARM_64
    /* type must be set before allocate memory */
    d->arch.type = kinfo.type;
#endif
    allocate_memory(d, &kinfo);

    rc = prepare_dtb_domU(d, &kinfo);
    if ( rc < 0 )
        return rc;

    rc = construct_domain(d, &kinfo);
    if ( rc < 0 )
        return rc;

    if ( kinfo.vpl011 )
        rc = domain_vpl011_init(d, NULL);

    if ( kinfo.dtb_bootmodule )
        rc = domain_adding_devices(d, &kinfo);

    return rc;
}

void __init create_domUs(void)
{
    struct dt_device_node *node;
    const struct dt_device_node *chosen = dt_find_node_by_path("/chosen");

    BUG_ON(chosen == NULL);
    dt_for_each_child_node(chosen, node)
    {
        struct domain *d;
        struct xen_domctl_createdomain d_cfg = {
            .arch.gic_version = XEN_DOMCTL_CONFIG_GIC_NATIVE,
            .arch.nr_spis = gic_number_lines() - 32,
            .flags = XEN_DOMCTL_CDF_hvm_guest,
        };

        if ( !dt_device_is_compatible(node, "xen,domain") )
            continue;

        d = domain_create(++max_init_domid, &d_cfg);
        if ( IS_ERR(d) )
            panic("Error creating domain %s", dt_node_name(node));

        d->max_vcpus = 1;
        dt_property_read_u32(node, "cpus", &d->max_vcpus);
        d->is_console = true;

        if ( construct_domU(d, node) != 0 )
            panic("Could not set up domain %s", dt_node_name(node));

        domain_unpause_by_systemcontroller(d);
    }
}

int __init construct_dom0(struct domain *d)
{
    struct kernel_info kinfo = {};
    int rc;

    /* Sanity! */
    BUG_ON(d->domain_id != 0);

    printk("*** LOADING DOMAIN 0 ***\n");
    if ( dom0_mem <= 0 )
    {
        warning_add("PLEASE SPECIFY dom0_mem PARAMETER - USING 512M FOR NOW\n");
        dom0_mem = MB(512);
    }

    iommu_hwdom_init(d);

    d->max_pages = ~0U;

    kinfo.unassigned_mem = dom0_mem;
    kinfo.d = d;

    rc = kernel_probe(&kinfo, NULL);
    if ( rc < 0 )
        return rc;

#ifdef CONFIG_ARM_64
    /* type must be set before allocate_memory */
    d->arch.type = kinfo.type;
#endif
#ifdef CONFIG_COLORING
    if ( d->max_colors )
        allocate_colored_memory(d, &kinfo);
    else
#endif
        allocate_memory_11(d, &kinfo);
    find_gnttab_region(d, &kinfo);

    /* Map extra GIC MMIO, irqs and other hw stuffs to dom0. */
    rc = gic_map_hwdom_extra_mappings(d);
    if ( rc < 0 )
        return rc;

    rc = platform_specific_mapping(d);
    if ( rc < 0 )
        return rc;

    if ( acpi_disabled )
        rc = prepare_dtb_hwdom(d, &kinfo);
    else
        rc = prepare_acpi(d, &kinfo);

    if ( rc < 0 )
        return rc;

    return construct_domain(d, &kinfo);
}

/*
 * Local variables:
 * mode: C
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * End:
 */
