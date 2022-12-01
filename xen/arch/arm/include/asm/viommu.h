/* SPDX-License-Identifier: (GPL-2.0-or-later OR BSD-2-Clause) */
#ifndef __ARCH_ARM_VIOMMU_H__
#define __ARCH_ARM_VIOMMU_H__

#ifdef CONFIG_VIRTUAL_IOMMU

#include <xen/lib.h>
#include <xen/types.h>
#include <public/xen.h>

struct viommu_ops {
    /*
     * Called during domain construction if toolstack requests to enable
     * vIOMMU support.
     */
    int (*domain_init)(struct domain *d);

    /*
     * Called during domain destruction to free resources used by vIOMMU.
     */
    int (*relinquish_resources)(struct domain *d);
};

struct viommu_desc {
    /* vIOMMU domains init/free operations described above. */
    const struct viommu_ops *ops;

    /*
     * ID of vIOMMU. Corresponds to xen_arch_domainconfig.viommu_type.
     * Should be one of XEN_DOMCTL_CONFIG_VIOMMU_xxx
     */
    uint16_t viommu_type;
};

int domain_viommu_init(struct domain *d, uint16_t viommu_type);
int viommu_relinquish_resources(struct domain *d);
uint16_t viommu_get_type(void);

#else

static inline uint8_t viommu_get_type(void)
{
    return XEN_DOMCTL_CONFIG_VIOMMU_NONE;
}

static inline int domain_viommu_init(struct domain *d, uint16_t viommu_type)
{
    if ( likely(viommu_type == XEN_DOMCTL_CONFIG_VIOMMU_NONE) )
        return 0;

    return -ENODEV;
}

static inline int viommu_relinquish_resources(struct domain *d)
{
    return 0;
}

#endif /* CONFIG_VIRTUAL_IOMMU */

#endif /* __ARCH_ARM_VIOMMU_H__ */

/*
 * Local variables:
 * mode: C
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * End:
 */
