/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * hvm/dom_build.c
 *
 * Dom builder for PVH guest.
 *
 * Copyright (C) 2017 Citrix Systems R&D
 * Copyright (C) 2024 Apertus Solutions, LLC
 */

#include <xen/init.h>

#include <asm/bootinfo.h>
#include <asm/dom0_build.h>

int __init dom_construct_pvh(struct boot_domain *bd)
{
    struct domain *d = bd->d;

    printk(XENLOG_INFO "*** Building a PVH Dom%u ***\n", d->domain_id);

    return dom0_construct_pvh(bd);
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
