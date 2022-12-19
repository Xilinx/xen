/* SPDX-License-Identifier: (GPL-2.0-or-later OR BSD-2-Clause) */

#include <xen/errno.h>
#include <xen/init.h>
#include <xen/irq.h>
#include <xen/param.h>
#include <xen/types.h>

#include <asm/viommu.h>

/* List of all host IOMMUs */
LIST_HEAD(host_iommu_list);

const struct viommu_desc __read_mostly *cur_viommu;

/* Common function for adding to host_iommu_list */
void add_to_host_iommu_list(paddr_t addr, paddr_t size,
                            const struct dt_device_node *node)
{
    struct host_iommu *iommu_data;

    iommu_data = xzalloc(struct host_iommu);
    if ( !iommu_data )
        panic("vIOMMU: Cannot allocate memory for host IOMMU data\n");

    iommu_data->addr = addr;
    iommu_data->size = size;
    iommu_data->dt_node = node;
    iommu_data->irq = platform_get_irq(node, 0);
    if ( iommu_data->irq < 0 )
    {
        gdprintk(XENLOG_ERR,
                 "vIOMMU: Cannot find a valid IOMMU irq\n");
        return;
    }

    printk("vIOMMU: Found IOMMU @0x%"PRIx64"\n", addr);

    list_add_tail(&iommu_data->entry, &host_iommu_list);
}

/* By default viommu is disabled. */
bool __read_mostly viommu_enabled;
boolean_param("viommu", viommu_enabled);

int domain_viommu_init(struct domain *d, uint8_t viommu_type)
{
    /* Enable viommu when it has been enabled explicitly (viommu=on). */
    if ( !viommu_enabled )
        return 0;

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

uint8_t viommu_get_type(void)
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
