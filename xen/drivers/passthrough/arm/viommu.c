/* SPDX-License-Identifier: (GPL-2.0-or-later OR BSD-2-Clause) */

#include <xen/errno.h>
#include <xen/init.h>
#include <xen/types.h>

#include <asm/viommu.h>

const struct viommu_desc __read_mostly *cur_viommu;

int domain_viommu_init(struct domain *d, uint16_t viommu_type)
{
    if ( viommu_type == XEN_DOMCTL_CONFIG_VIOMMU_NONE )
        return 0;

    if ( !cur_viommu )
        return -ENODEV;

    if ( cur_viommu->viommu_type != viommu_type )
        return -EINVAL;

    return cur_viommu->ops->domain_init(d);
}

int viommu_relinquish_resources(struct domain *d)
{
    if ( !cur_viommu )
        return 0;

    return cur_viommu->ops->relinquish_resources(d);
}

uint16_t viommu_get_type(void)
{
    if ( !cur_viommu )
        return XEN_DOMCTL_CONFIG_VIOMMU_NONE;

    return cur_viommu->viommu_type;
}

/*
 * Local variables:
 * mode: C
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * End:
 */
