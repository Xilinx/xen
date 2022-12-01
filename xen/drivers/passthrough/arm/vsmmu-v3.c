/* SPDX-License-Identifier: (GPL-2.0-or-later OR BSD-2-Clause) */

#include <xen/param.h>
#include <xen/sched.h>
#include <asm/mmio.h>
#include <asm/viommu.h>

/* Struct to hold the vIOMMU ops and vIOMMU type */
extern const struct viommu_desc __read_mostly *cur_viommu;

struct virt_smmu {
    struct      domain *d;
    struct      list_head viommu_list;
};

static int vsmmuv3_mmio_write(struct vcpu *v, mmio_info_t *info,
                              register_t r, void *priv)
{
    return IO_HANDLED;
}

static int vsmmuv3_mmio_read(struct vcpu *v, mmio_info_t *info,
                             register_t *r, void *priv)
{
    return IO_HANDLED;
}

static const struct mmio_handler_ops vsmmuv3_mmio_handler = {
    .read  = vsmmuv3_mmio_read,
    .write = vsmmuv3_mmio_write,
};

static int vsmmuv3_init_single(struct domain *d, paddr_t addr, paddr_t size)
{
    struct virt_smmu *smmu;

    smmu = xzalloc(struct virt_smmu);
    if ( !smmu )
        return -ENOMEM;

    smmu->d = d;

    register_mmio_handler(d, &vsmmuv3_mmio_handler, addr, size, smmu);

    /* Register the vIOMMU to be able to clean it up later. */
    list_add_tail(&smmu->viommu_list, &d->arch.viommu_list);

    return 0;
}

int domain_vsmmuv3_init(struct domain *d)
{
    int ret;
    INIT_LIST_HEAD(&d->arch.viommu_list);

    if ( is_hardware_domain(d) )
    {
        struct host_iommu *hw_iommu;

        list_for_each_entry(hw_iommu, &host_iommu_list, entry)
        {
            ret = vsmmuv3_init_single(d, hw_iommu->addr, hw_iommu->size);
            if ( ret )
                return ret;
        }
    }
    else
    {
        ret = vsmmuv3_init_single(d, GUEST_VSMMUV3_BASE, GUEST_VSMMUV3_SIZE);
        if ( ret )
            return ret;
    }

    return 0;
}

int vsmmuv3_relinquish_resources(struct domain *d)
{
    struct virt_smmu *pos, *temp;

    /* Cope with unitialized vIOMMU */
    if ( list_head_is_null(&d->arch.viommu_list) )
        return 0;

    list_for_each_entry_safe(pos, temp, &d->arch.viommu_list, viommu_list )
    {
        list_del(&pos->viommu_list);
        xfree(pos);
    }

    return 0;
}

static const struct viommu_ops vsmmuv3_ops = {
    .domain_init = domain_vsmmuv3_init,
    .relinquish_resources = vsmmuv3_relinquish_resources,
};

static const struct viommu_desc vsmmuv3_desc = {
    .ops = &vsmmuv3_ops,
    .viommu_type = XEN_DOMCTL_CONFIG_VIOMMU_SMMUV3,
};

void __init vsmmuv3_set_type(void)
{
    const struct viommu_desc *desc = &vsmmuv3_desc;

    if ( !is_viommu_enabled() )
        return;

    if ( cur_viommu && (cur_viommu != desc) )
    {
        printk("WARNING: Cannot set vIOMMU, already set to a different value\n");
        return;
    }

    cur_viommu = desc;
}

/*
 * Local variables:
 * mode: C
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * End:
 */
