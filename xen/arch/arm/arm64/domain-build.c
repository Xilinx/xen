/* SPDX-License-Identifier: GPL-2.0-only */

#include <xen/fdt-kernel.h>
#include <xen/sched.h>

#include <asm/domain.h>
#include <asm/arm64/sve.h>

int __init arm64_set_domain_type(struct kernel_info *kinfo)
{
    struct domain *d = kinfo->bd.d;
    enum domain_type type;

    ASSERT(d);
    ASSERT(kinfo);

    type = kinfo->arch.type;

    if ( !is_aarch32_enabled() )
    {
        ASSERT(d->arch.type == DOMAIN_64BIT);

        if ( type == DOMAIN_32BIT )
        {
            const char *str = "not available";

            if ( !IS_ENABLED(CONFIG_ARM64_AARCH32) )
                str = "disabled";
            printk("aarch32 guests support is %s\n", str);
            return -EINVAL;
        }

        return 0;
    }

    if ( is_sve_domain(d) && type == DOMAIN_32BIT )
    {
        printk("SVE is not available for 32-bit domain\n");
        return -EINVAL;
    }

    d->arch.type = type;

    return 0;
}

#ifdef CONFIG_DOM0LESS_BOOT
/* TODO: make arch.type generic ? */
void __init set_domain_type(struct domain *d, struct kernel_info *kinfo)
{
    int rc;

    rc = arm64_set_domain_type(kinfo);
    if ( rc < 0 )
        panic("Unsupported guest type\n");
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
