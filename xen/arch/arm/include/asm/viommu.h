/* SPDX-License-Identifier: (GPL-2.0-or-later OR BSD-2-Clause) */
#ifndef __ARCH_ARM_VIOMMU_H__
#define __ARCH_ARM_VIOMMU_H__

#ifdef CONFIG_VIRTUAL_IOMMU

#include <xen/lib.h>
#include <xen/list.h>
#include <xen/types.h>
#include <public/xen.h>

extern struct list_head host_iommu_list;
extern bool viommu_enabled;

/* data structure for each hardware IOMMU */
struct host_iommu {
    struct list_head entry;
    const struct dt_device_node *dt_node;
    paddr_t addr;
    paddr_t size;
    uint32_t irq;
    bool hwdom_node_created;
};

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
    uint8_t viommu_type;
};

int domain_viommu_init(struct domain *d, uint8_t viommu_type);
int viommu_relinquish_resources(struct domain *d);
uint8_t viommu_get_type(void);
void add_to_host_iommu_list(paddr_t addr, paddr_t size,
                            const struct dt_device_node *node);

static always_inline bool is_viommu_enabled(void)
{
    return viommu_enabled;
}

#else

static inline uint8_t viommu_get_type(void)
{
    return XEN_DOMCTL_CONFIG_VIOMMU_NONE;
}

static inline int domain_viommu_init(struct domain *d, uint8_t viommu_type)
{
    if ( likely(viommu_type == XEN_DOMCTL_CONFIG_VIOMMU_NONE) )
        return 0;

    return -ENODEV;
}

static inline int viommu_relinquish_resources(struct domain *d)
{
    return 0;
}

static inline void add_to_host_iommu_list(paddr_t addr, paddr_t size,
                                          const struct dt_device_node *node)
{
    return;
}

static always_inline bool is_viommu_enabled(void)
{
    return false;
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
