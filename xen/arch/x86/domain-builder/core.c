/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (C) 2024, Apertus Solutions, LLC
 */
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

/*
 * Local variables:
 * mode: C
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * tab-width: 4
 * indent-tabs-mode: nil
 * End:
 */
