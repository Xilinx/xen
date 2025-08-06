/* SPDX-License-Identifier: GPL-2.0-only */

#include <xen/fdt-kernel.h>
#include <xen/sched.h>

#include <asm/domain.h>

#ifdef CONFIG_DOM0LESS_BOOT
void __init set_domain_type(struct domain *d, struct kernel_info *kinfo)
{
    /* Nothing to do */
}
#endif

/*
 * Local variables:
 * mode: C
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * End:
 */
