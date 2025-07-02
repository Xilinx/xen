/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (C) 2024, Apertus Solutions, LLC
 */
#include <xen/device_tree.h>
#include <xen/err.h>
#include <xen/init.h>
#include <xen/lib.h>
#include <xen/bootfdt.h>
#include <xen/libfdt/libfdt.h>

#include <asm/bootinfo.h>
#include <asm/domain-builder.h>
#include <asm/setup.h>

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
    const struct boot_module *bm = &bi->mods[0];
    unsigned int initrdidx;

    if ( IS_ENABLED(CONFIG_DOM0LESS_BOOT) && bm->kind == BOOTMOD_FDT)
    {
        dt_parse_domains(bi);
        return;
    }

    /* Not Hyperlaunch. Fall back to dom0-based booting. */
    bi->nr_domains = 1;
    bi->domains[0].kernel = &bi->mods[0];

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
