/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * xen/arch/arm/domain-reset.c: Domain reset implementation for arm
 *
 * Copyright (C) 2025, Advanced Micro Devices, Inc. All Rights Reserved.
 *
 */

#include <xen/dom0less-build.h>
#include <xen/domain_page.h>
#include <xen/event.h>
#include <xen/grant_table.h>
#include <xen/libfdt/libfdt.h>
#include <xen/sizes.h>
#include <xen/vmap.h>
#include <public/io/xs_wire.h>
#include <asm/div64.h>
#include <asm/domain_build.h>
#include <asm/guest_access.h>
#include <asm/vtimer.h>

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
    struct domain *d = kinfo->bd.d;

    if ( !is_domain_resettable(d) )
        return;

    rinfo = xzalloc(struct reset_info);
    if ( !rinfo )
        panic("reset: Cannot allocate memory for reset info\n");

    if ( kinfo->bd.kernel )
    {
        rinfo->kernel_start = save_binary(kinfo->bd.kernel->start,
                                          kinfo->bd.kernel->size);
        rinfo->entry = kinfo->entry;
        rinfo->kernel_size = kinfo->bd.kernel->size;
        printk(XENLOG_DEBUG
               "%pd: reset: saved Kernel to 0x%p-0x%p\n",
               d, rinfo->kernel_start,
               rinfo->kernel_start + rinfo->kernel_size);
    }
    else
        panic("%pd: reset: kernel bootmodule is null\n", d);

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
            panic("(%pd): reset: cannot alloc memory for fdt\n", d);
        memcpy(rinfo->fdt, kinfo->fdt, rinfo->fdt_size);

        rinfo->dtb_paddr = kinfo->dtb_paddr;
        printk(XENLOG_DEBUG
            "%pd: reset: saved FDT to 0x%p-0x%p\n",
            d, rinfo->fdt, rinfo->fdt + rinfo->fdt_size);
    }
    else
       panic("%pd: reset: fdt is null\n", d);

    if ( kinfo->bd.initrd )
    {
        rinfo->initrd_start = save_binary(kinfo->bd.initrd->start,
                                          kinfo->bd.initrd->size);
        rinfo->initrd_paddr = kinfo->initrd_paddr;
        rinfo->initrd_size = kinfo->bd.initrd->size;
        printk(XENLOG_DEBUG
               "%pd: reset: saved Initrd to 0x%p-0x%p\n",
               d, rinfo->initrd_start,
               rinfo->initrd_start + rinfo->initrd_size);
    }
    else
        printk(XENLOG_DEBUG "%pd: reset: initrd bootmodule is null\n", d);

    /* Save original memory layout (GFN ranges) */
    rinfo->mem = rangeset_new(d, "reset-memory", 0);
    if ( !rinfo->mem )
        panic("%pd: reset: cannot allocate rangeset for memory\n", d);

    /*
     * Populate rangeset with all GFN ranges from memory banks.
     * This tracks which GFNs were allocated at domain creation, so we can
     * restore the original layout during reset.
     */
    {
        const struct membanks *mem = kernel_info_get_mem_const(kinfo);
        unsigned int i;
        int rc;

        for ( i = 0; i < mem->nr_banks; i++ )
        {
            paddr_t bank_start = mem->bank[i].start;
            paddr_t bank_size = mem->bank[i].size;
            unsigned long start_gfn = paddr_to_pfn(bank_start);
            unsigned long end_gfn = paddr_to_pfn(bank_start + bank_size - 1);

            rc = rangeset_add_range(rinfo->mem, start_gfn, end_gfn);
            if ( rc )
                panic("%pd: reset: failed to add GFN range [%lx-%lx] to rangeset: %d\n",
                      d, start_gfn, end_gfn, rc);

            printk(XENLOG_DEBUG "%pd: reset: saved memory bank %u: GFN %lx-%lx\n",
                   d, i, start_gfn, end_gfn);
        }
    }

    /* Save reset_info in domain structure */
    d->arch.reset_info = rinfo;

    return;
}


static int copy_binary(struct domain *d, paddr_t dest, void *src, size_t size)
{
    struct reset_info *rinfo = d->arch.reset_info;
    int rc;
    size_t real_size = size - rinfo->size;
    paddr_t real_dest = dest + rinfo->size;
    void *real_src = src + rinfo->size;

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
        rinfo->size += chunk_size;

        if ( hypercall_preempt_check() && d->vcpu[0] != current )
            return -ERESTART;
    }
    return 0;
}


static int reload_binaries(struct domain *d, struct reset_info *rinfo)
{
    int rc;

    switch ( rinfo->stage )
    {
#define PROGRESS(x)                             \
        rinfo->stage = PROG_ ## x;              \
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
        rinfo->size = 0;

    PROGRESS(fdt):
        rc = copy_binary(d, rinfo->dtb_paddr, rinfo->fdt, rinfo->fdt_size);
        if ( rc )
            return rc;
        printk(XENLOG_DEBUG
              "(%pd) Reloaded FDT from 0x%p to 0x%"PRIpaddr"-0x%"PRIpaddr"\n",
               d, rinfo->fdt, rinfo->dtb_paddr,
               rinfo->dtb_paddr + rinfo->fdt_size);
        rinfo->size = 0;

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
            rinfo->size = 0;
        }

    PROGRESS(done):
        break;

#undef PROGRESS

    default:
        BUG();
    }

    /* Clean up stage for next reset request */
    rinfo->stage = 0;

    return 0;
}

/* Should be called with lock on domain d held */
long arch_domain_full_reset(struct domain *d)
{
    struct vcpu *v = NULL;
    struct reset_info *rinfo;
    struct cpu_user_regs *regs;
    int ret;

    rinfo = d->arch.reset_info;
    if ( !rinfo )
    {
        printk(XENLOG_ERR "%pd: no reset_info found\n", d);
        return -EINVAL;
    }

    /*
     * Restore original P2M layout first, filling any holes (e.g., ballooned
     * pages). This must be done before reload_binaries() because the guest
     * may have ballooned out pages even in regions where kernel/fdt/initrd
     * are located, and we need valid mappings to copy the binaries.
     */
    ret = p2m_reset(d);
    if ( ret )
        return ret;

    /* Reload kernel, fdt and initrd */
    ret = reload_binaries(d, rinfo);
    if ( ret )
        return ret;

    d->arch.virt_timer_base.offset = get_cycles();
    d->arch.virt_timer_base.nanoseconds =
        ticks_to_ns(d->arch.virt_timer_base.offset - boot_count);
    d->time_offset.seconds = d->arch.virt_timer_base.nanoseconds;
    do_div(d->time_offset.seconds, 1000000000);

    for_each_vcpu ( d, v )
    {
        /*
         * vcpu_state_reset() takes domain_lock internally, which we already
         * hold here. This works because domain_lock is recursive (rspin_lock),
         * but relies on this implementation detail.
         */
        vcpu_state_reset(v);
        /*
         * Secondary vCPUs must remain down. Only vCPU0 will be started.
         * arch_vcpu_state_reset() clears all state but doesn't set the
         * pause flag, so we must ensure secondary vCPUs stay paused.
         */
        if ( v->vcpu_id != 0 )
        {
            v->is_initialised = 0;
            set_bit(_VPF_down, &v->pause_flags);
        }
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

    update_vcpu_system_time(v);
    update_domain_wallclock_time(d);
    domain_update_node_affinity(d);

    v->is_initialised = 1;
    clear_bit(_VPF_down, &v->pause_flags);

    /* Commit the update on pCPU in self reset case */
    if ( v == current )
    {
        WRITE_SYSREG(current->arch.ttbcr, TCR_EL1);
        WRITE_SYSREG64(current->arch.ttbr0, TTBR0_EL1);
        WRITE_SYSREG64(current->arch.ttbr1, TTBR1_EL1);
        p2m_restore_state(current);
        WRITE_SYSREG(current->arch.cntkctl, CNTKCTL_EL1);
        virt_timer_restore(current);
        current->runstate.state = RUNSTATE_running;
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

/*
 * Local variables:
 * mode: C
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * End:
 */
