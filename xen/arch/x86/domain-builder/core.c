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
        dom0_max_vcpus();

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

/*
 * Local variables:
 * mode: C
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * tab-width: 4
 * indent-tabs-mode: nil
 * End:
 */
