/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * xen/arch/arm/domain-reset.c: Domain reset implementation for arm
 *
 * Copyright (C) 2025, Advanced Micro Devices, Inc. All Rights Reserved.
 *
 */

#include <xen/domain_page.h>
#include <xen/event.h>
#include <xen/grant_table.h>
#include <xen/libfdt/libfdt.h>
#include <xen/sizes.h>
#include <xen/vmap.h>
#include <public/io/xs_wire.h>
#include <asm/dom0less-build.h>
#include <asm/div64.h>
#include <asm/domain_build.h>
#include <asm/guest_access.h>
#include <asm/vtimer.h>

struct reset_info {
    struct domain *d;

    void* kernel_start;
    size_t kernel_size;
    void* initrd_start;
    size_t initrd_size;
    void* fdt;
    size_t fdt_size;

    paddr_t entry;
    paddr_t initrd_paddr;
    paddr_t dtb_paddr;

    uint16_t dom0less_feature;

    struct list_head list;
};
/*
 * XXX: reset_data is accessed without locking. This is currently safe because
 * the list is only populated during boot and remains read-only afterwards.
 * If future changes require modifying the list at runtime, appropriate locking
 * must be introduced to ensure safety during concurrent accesses.
 */
static LIST_HEAD(reset_data);

static void* __init save_binary(paddr_t start, paddr_t size)
{
    void *bin_mapped;
    void *relocated_address;

    relocated_address = xzalloc_bytes(size);
    if ( !relocated_address )
        panic("reset: Cannot allocate memory for module\n");

    bin_mapped = ioremap_wc(start, size);
    if ( !bin_mapped )
        panic("reset: Cannot map module memory\n");

    memcpy(relocated_address, bin_mapped, size);

    iounmap(bin_mapped);

    return relocated_address;
}

void __init domain_reset_add_data(struct kernel_info *kinfo)
{
    struct reset_info *rinfo;

    if ( !kinfo->d->reset_info.is_resettable )
        return;

    rinfo = xzalloc(struct reset_info);
    if ( !rinfo )
        panic("reset: Cannot allocate memory for reset info\n");

    rinfo->d = kinfo->d;
    rinfo->dom0less_feature = kinfo->dom0less_feature;

    if ( kinfo->kernel_bootmodule )
    {
        rinfo->kernel_start = save_binary(kinfo->kernel_bootmodule->start,
                                          kinfo->kernel_bootmodule->size);
        rinfo->entry = kinfo->entry;
        rinfo->kernel_size = kinfo->kernel_bootmodule->size;
        printk(XENLOG_DEBUG
               "%pd: reset: saved Kernel to 0x%p-0x%p\n",
               rinfo->d, rinfo->kernel_start,
               rinfo->kernel_start + rinfo->kernel_size);
    }
    else
        panic("%pd: reset: kernel bootmodule is null\n", rinfo->d);

    if ( kinfo->fdt )
    {
        /*
        * FDT will be freed later during boot stage in dtb_load. Create
        * a copy in the rinfo info structure and set the size information.
        * During reset hypercall there is no access to the fdt library.
        */
        rinfo->fdt_size = fdt_totalsize(kinfo->fdt);
        rinfo->fdt = xzalloc_bytes(rinfo->fdt_size);
        if ( !rinfo->fdt )
            panic("(%pd): reset: cannot alloc memory for fdt\n", rinfo->d);
        memcpy(rinfo->fdt, kinfo->fdt, rinfo->fdt_size);

        rinfo->dtb_paddr = kinfo->dtb_paddr;
        printk(XENLOG_DEBUG
            "%pd: reset: saved FDT to 0x%p-0x%p\n",
            rinfo->d, rinfo->fdt, rinfo->fdt + rinfo->fdt_size);
    }
    else
       panic("%pd: reset: fdt is null\n", rinfo->d);

    if ( kinfo->initrd_bootmodule )
    {
        rinfo->initrd_start = save_binary(kinfo->initrd_bootmodule->start,
                                          kinfo->initrd_bootmodule->size);
        rinfo->initrd_paddr = kinfo->initrd_paddr;
        rinfo->initrd_size = kinfo->initrd_bootmodule->size;
        printk(XENLOG_DEBUG
               "%pd: reset: saved Initrd to 0x%p-0x%p\n",
               rinfo->d, rinfo->initrd_start,
               rinfo->initrd_start + rinfo->initrd_size);
    }
    else
        printk(XENLOG_DEBUG "%pd: reset: initrd bootmodule is null\n", rinfo->d);

    list_add_tail(&rinfo->list, &reset_data);

    return;
}

static struct reset_info* find_rinfo_from_domain(domid_t id)
{
    struct reset_info *data = NULL;

    list_for_each_entry(data, &reset_data, list)
    {
        if ( data->d->domain_id == id )
            return data;
    }

    return NULL;
}

static int copy_binary(struct domain *d, paddr_t dest, void *src, size_t size)
{
    int rc;
    size_t real_size = size - d->arch.reset.size;
    paddr_t real_dest = dest + d->arch.reset.size;
    void *real_src = src + d->arch.reset.size;

    while ( real_size > 0 )
    {
        unsigned int chunk_size =
            (real_size > SZ_64K) ? SZ_64K : real_size;

        rc = copy_to_guest_phys_flush_dcache(d, real_dest, real_src, chunk_size);
        if ( rc )
           return rc;

        real_size -= chunk_size;
        real_dest += chunk_size;
        real_src = (void *)(real_src + chunk_size);
        d->arch.reset.size += chunk_size;

        if ( hypercall_preempt_check() && d->vcpu[0] != current )
            return -ERESTART;
    }
    return 0;
}


static int reload_binaries(struct reset_info *rinfo)
{
    struct domain *d = rinfo->d;
    int rc;

    switch ( d->arch.reset.stage )
    {
#define PROGRESS(x)                             \
        d->arch.reset.stage = PROG_ ## x;       \
        fallthrough;                            \
    case PROG_ ## x

    enum {
            PROG_none,
            PROG_kernel,
            PROG_fdt,
            PROG_initrd,
            PROG_done,
        };

    case PROG_none:
        BUILD_BUG_ON(PROG_none != 0);

    PROGRESS(kernel):
        rc = copy_binary(d, rinfo->entry, rinfo->kernel_start,
                         rinfo->kernel_size);
        if ( rc )
            return rc;
        printk(XENLOG_DEBUG
            "(%pd) Reloaded Kernel from 0x%p to 0x%"PRIpaddr"-0x%"PRIpaddr"\n",
            d, rinfo->kernel_start, rinfo->entry,
            rinfo->entry + rinfo->kernel_size);
        d->arch.reset.size = 0;

    PROGRESS(fdt):
        rc = copy_binary(d, rinfo->dtb_paddr, rinfo->fdt, rinfo->fdt_size);
        if ( rc )
            return rc;
        printk(XENLOG_DEBUG
              "(%pd) Reloaded FDT from 0x%p to 0x%"PRIpaddr"-0x%"PRIpaddr"\n",
               d, rinfo->fdt, rinfo->dtb_paddr,
               rinfo->dtb_paddr + rinfo->fdt_size);
        d->arch.reset.size = 0;

    PROGRESS(initrd):
        if ( rinfo->initrd_start )
        {
            rc = copy_binary(d, rinfo->initrd_paddr, rinfo->initrd_start,
                             rinfo->initrd_size);
            if ( rc )
                return rc;
            printk(XENLOG_DEBUG
               "(%pd) Reloaded Initrd from 0x%p to 0x%"PRIpaddr"-0x%"PRIpaddr"\n",
               d, rinfo->initrd_start, rinfo->initrd_paddr,
               rinfo->initrd_paddr + rinfo->initrd_size);
            d->arch.reset.size = 0;
        }

    PROGRESS(done):
        break;

#undef PROGRESS

    default:
        BUG();
    }

    /* Clean up stage for next reset request */
    d->arch.reset.stage = 0;

    return 0;
}

/* Should be called with lock on domain d held */
long arch_domain_reset(struct domain *d)
{
    struct vcpu *v = NULL;
    struct reset_info *rinfo = NULL;
    struct cpu_user_regs *regs;
    int ret;

    rinfo = find_rinfo_from_domain(d->domain_id);
    if ( rinfo == NULL )
    {
        printk(XENLOG_ERR "Error: no domain data found: %u\n", d->domain_id);
        return -EINVAL;
    }

    /* Reload kernel, fdt and initrd */
    ret = reload_binaries(rinfo);
    if ( ret )
        return ret;

    /* Remove any shared info mapping */
    page_set_xenheap_gfn(mfn_to_page(_mfn(virt_to_mfn(d->shared_info))),
                         INVALID_GFN);
    clear_page(d->shared_info);

    if ( rinfo->dom0less_feature == DOM0LESS_ENHANCED ||
        rinfo->dom0less_feature == DOM0LESS_ENHANCED_LEGACY )
    {
        struct xenstore_domain_interface *interface;
        mfn_t mfn;
        struct page_info *page;

        initialize_domU_xenstore(d);

        mfn = gfn_to_mfn(d,_gfn(d->arch.hvm.params[HVM_PARAM_STORE_PFN]));
        page = mfn_to_page(mfn);

        if ( !mfn_valid(mfn) || !get_page(page, d) )
            return -EINVAL;

        interface = __map_domain_page(page);
        if ( interface == NULL )
        {
            put_page(page);
            return -EINVAL;
        }

        interface->connection = XENSTORE_RECONNECT;
        unmap_domain_page(interface);
        put_page(page);
    }

    d->arch.virt_timer_base.offset = get_cycles();
    d->arch.virt_timer_base.nanoseconds =
        ticks_to_ns(d->arch.virt_timer_base.offset - boot_count);
    d->time_offset.seconds = d->arch.virt_timer_base.nanoseconds;
    do_div(d->time_offset.seconds, 1000000000);

    for_each_vcpu ( d, v )
    {
        regs = &v->arch.cpu_info->guest_cpu_user_regs;
        memset(regs, 0, sizeof(*regs));

        v->arch.sctlr = SCTLR_GUEST_INIT;
        /*  Reset MMU registers */
        v->arch.ttbr0 = 0x0;
        v->arch.ttbr1 = 0x0;
        v->arch.ttbcr = 0x0;

        vcpu_timer_destroy(v);
        vgic_clear_pending_irqs(v);

        if ( is_32bit_domain(d) )
            regs->cpsr = PSR_GUEST32_INIT;
#ifdef CONFIG_ARM_64
        else
            regs->cpsr = PSR_GUEST64_INIT;
#endif
        ret = vcpu_vtimer_init(v);
        if ( ret )
            return ret;

        sync_vcpu_execstate(v);
    }

    /* Reset boot CPU (first vcpu of domain) */
    v = d->vcpu[0];
    regs = &v->arch.cpu_info->guest_cpu_user_regs;
    regs->pc = (register_t)rinfo->entry;

    if ( is_32bit_domain(d) )
    {
        regs->r0 = 0U; /* SBZ */
        regs->r1 = 0xffffffffU; /* We use DTB therefore no machine id */
        regs->r2 = rinfo->dtb_paddr;
    }
#ifdef CONFIG_ARM_64
    else
    {
        /* From linux/Documentation/arm64/booting.txt */
        regs->x0 = rinfo->dtb_paddr;
        regs->x1 = 0; /* Reserved for future use */
        regs->x2 = 0; /* Reserved for future use */
        regs->x3 = 0; /* Reserved for future use */
    }
#endif

    /* Commit the update on pCPU in self reset case */
    if ( v == current )
    {
        WRITE_SYSREG(current->arch.ttbcr, TCR_EL1);
        WRITE_SYSREG64(current->arch.ttbr0, TTBR0_EL1);
        WRITE_SYSREG64(current->arch.ttbr1, TTBR1_EL1);
        p2m_restore_state(current);
        WRITE_SYSREG(current->arch.cntkctl, CNTKCTL_EL1);
        virt_timer_restore(current);
    }

#ifdef CONFIG_ARM_64
    /*
     * In a self-reset scenario where the guest issues a reset hypercall with
     * its boot CPU, we need to return the GPA of the FDT
     * so that the guest can reboot correctly. This is because the x0 register
     * will be filled with the return value of the hypercall.
     * We assume that this function is the last one called in the main
     * reset function and that its return code is used.
     */
    if ( v == current )
        return rinfo->dtb_paddr;
#endif
    return 0;
}

void arch_domain_reset_info(const struct domain *d)
{
    struct reset_info *rinfo;

    rinfo = find_rinfo_from_domain(d->domain_id);
    if ( !rinfo )
    {
        printk(XENLOG_ERR "No reset data found for Domain %d\n", d->domain_id);
        return;
    }

    printk("  Kernel: [0x%p-0x%p]\n",
           rinfo->kernel_start,
           (void *)(rinfo->kernel_start + rinfo->kernel_size));
    if ( rinfo->initrd_start )
        printk("  Initrd: [0x%p-0x%p]\n",
               rinfo->initrd_start,
               (void *)(rinfo->initrd_start + rinfo->initrd_size));

    printk("  FDT: [%p - %p]\n", rinfo->fdt,
           (void *)(rinfo->fdt + rinfo->fdt_size));
}

/*
 * Local variables:
 * mode: C
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * End:
 */
