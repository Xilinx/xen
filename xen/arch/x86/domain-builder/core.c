/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (C) 2024, Apertus Solutions, LLC
 */
#include <xen/device_tree.h>
#include <xen/dom0less-build.h>
#include <xen/domain.h>
#include <xen/err.h>
#include <xen/grant_table.h>
#include <xen/init.h>
#include <xen/iommu.h>
#include <xen/lib.h>
#include <xen/bootfdt.h>
#include <xen/domain.h>
#include <xen/libfdt/libfdt.h>

#include <asm/bootinfo.h>
#include <asm/dom0_build.h>
#include <asm/domain-builder.h>
#include <asm/pv/shim.h>
#include <asm/setup.h>

#include <public/domctl.h>

#include "dtb.h"

void __init builder_init(struct boot_info *bi)
{
    if ( IS_ENABLED(CONFIG_DOM0LESS_BOOT) )
    {
        fdt_identify_module_kinds(bi);

        if ( bi->mods[0].kind == BOOTMOD_FDT )
            printk(XENLOG_INFO "Boot mode: DTB\n");
    }

    if ( bi->mods[0].kind == BOOTMOD_UNKNOWN )
    {
        /* Not Hyperlaunch. Assume first module is dom0's kernel. */
        bi->mods[0].kind = BOOTMOD_KERNEL;
        printk(XENLOG_INFO "Boot mode: dom0\n");
    }
}

void __init builder_late_init(struct boot_info *bi)
{
    bool pvh_hap = opt_dom0_pvh && !opt_dom0_shadow && hvm_hap_supported();
    const struct boot_module *bm = &bi->mods[0];
    struct boot_domain *dom0;

    unsigned int initrdidx;

    if ( IS_ENABLED(CONFIG_DOM0LESS_BOOT) && bm->kind == BOOTMOD_FDT)
    {
        /* Prevent cmdline from setting CPU/node masks on DTB boots. */
        dom0_disable_cmdline_cpu_node_overrides();
        dt_parse_domains(bi);
        return;
    }

    /* Not Hyperlaunch. Fall back to dom0-based booting. */

    if ( !IS_ENABLED(CONFIG_SHADOW_PAGING) && opt_dom0_pvh && !pvh_hap )
        panic("Neither HAP nor Shadow available for PVH domain\n");

    bi->nr_domains = 1;
    dom0 = &bi->domains[0];

    dom0->kernel = &bi->mods[0];
    dom0->domid = get_initial_domain_id(); /* Not d0 for pvshim */
    dom0->create_flags = pv_shim ? 0 : CDF_privileged | CDF_hardware;
    dom0->create_cfg = (struct xen_domctl_createdomain){
        .flags = (IS_ENABLED(CONFIG_TBOOT) ? XEN_DOMCTL_CDF_s3_integrity : 0) |
                 (iommu_enabled            ? XEN_DOMCTL_CDF_iommu        : 0) |
                 (opt_dom0_pvh             ? XEN_DOMCTL_CDF_hvm          : 0) |
                 (pvh_hap                  ? XEN_DOMCTL_CDF_hap          : 0) |
                 XEN_DOMCTL_CDF_xs_domain,
        .max_evtchn_port= -1,
        .max_grant_frames = -1,
        .max_maptrack_frames = -1,
        .grant_opts = XEN_DOMCTL_GRANT_version(opt_gnttab_max_version),
        .max_vcpus = dom0_max_vcpus(),
        .arch = {
            .misc_flags = opt_dom0_msr_relaxed ? XEN_X86_MSR_RELAXED : 0,
            .emulation_flags = opt_dom0_pvh                               ?
                XEN_X86_EMU_LAPIC | XEN_X86_EMU_IOAPIC | XEN_X86_EMU_VPCI :
                XEN_X86_EMU_PIT,
        },
    };

    /* Not d0 for pvshim */
    bi->domains[0].domid = domid_alloc(get_initial_domain_id());
    if ( bi->domains[0].domid == DOMID_INVALID )
        panic("Error allocating domain ID %u\n", get_initial_domain_id());

    /*
     * At this point all capabilities that consume boot modules should have
     * claimed their boot modules. Find the first unclaimed boot module and
     * claim it as the initrd ramdisk. Do a second search to see if there
     * are any remaining unclaimed boot modules, and report them as unusued
     * initrd candidates.
     */
    initrdidx = first_boot_module_index(bi, BOOTMOD_UNKNOWN);
    if ( initrdidx < MAX_NR_BOOTMODS )
    {
        bi->mods[initrdidx].kind = BOOTMOD_RAMDISK;
        bi->domains[0].initrd = &bi->mods[initrdidx];
        if ( first_boot_module_index(bi, BOOTMOD_UNKNOWN) < MAX_NR_BOOTMODS )
            printk(XENLOG_WARNING
                   "Multiple initrd candidates, picking module #%u\n",
                   initrdidx);
    }
}

static int  __init build_core_domains(struct boot_info *bi,
                                      domid_t *hw_domid,
                                      domid_t *xs_domid)
{
    struct boot_domain *bd;
    unsigned int count = 0;

    *hw_domid = DOMID_INVALID;
    *xs_domid = DOMID_INVALID;

    if ( !(bd = first_boot_domain(bi, XEN_DOMCTL_CDF_xs_domain, 0)) )
        printk(XENLOG_WARNING "No xenstore domain was defined\n");
    else
    {
        *xs_domid = bd->domid;

        if ( !bd->d )
        {
            arch_create_dom(bi, bd);
            if ( bd->d )
            {
                count++;
            }
        }
    }

    if ( !(bd = first_boot_domain(bi, 0, CDF_hardware)) )
        printk(XENLOG_WARNING "No hardware domain was defined\n");
    else
    {
        *hw_domid = bd->domid;
        if ( *xs_domid == DOMID_INVALID )
            *xs_domid = bd->domid;

        if ( !bd->d )
        {
            arch_create_dom(bi, bd);
            if ( bd->d )
            {
                count++;
            }
        }
    }

    if ( !(bd = first_boot_domain(bi, 0, CDF_privileged)) )
        printk(XENLOG_WARNING "No control domain was defined\n");
    else if ( !bd->d )
    {
        arch_create_dom(bi, bd);
        if ( bd->d )
            count++;
    }

    return count;
}

unsigned int __init builder_create_domains(struct boot_info *bi)
{
    unsigned int build_count = 0;
    domid_t hw_domid;
    domid_t xs_domid;

    if ( bi->nr_domains == 0 )
        panic("%s: no domains defined\n", __func__);

    build_count = build_core_domains(bi, &hw_domid, &xs_domid);

    BUG_ON(!IS_ENABLED(CONFIG_DOM0LESS_BOOT) && build_count != bi->nr_domains);

    for ( unsigned int i = 0; i < bi->nr_domains; i++ )
    {
        struct boot_domain *bd = &bi->domains[i];

        if ( bd->d )
            continue;

        if ( !(bd->create_cfg.flags & XEN_DOMCTL_CDF_hvm) )
        {
            printk(XENLOG_WARNING "don't support PV DomU, skipping %u\n", i);
            continue;
        }

        bd->xenstore.be_domid = xs_domid;
        bd->console.be_domid = hw_domid;

        arch_create_dom(bi, bd);
        if ( bd->d )
            build_count++;
        else
            printk(XENLOG_WARNING "failed to construct build domain %u\n", i);
    }

    /* Free temporary buffers. */
    free_boot_modules();

    return build_count;
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
