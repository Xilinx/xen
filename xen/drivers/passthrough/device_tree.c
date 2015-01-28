/*
 * Code to passthrough a device tree node to a guest
 *
 * Julien Grall <julien.grall@linaro.org>
 * Copyright (c) 2014 Linaro Limited.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <xen/lib.h>
#include <xen/sched.h>
#include <xen/guest_access.h>
#include <xen/iommu.h>
#include <xen/device_tree.h>

static spinlock_t dtdevs_lock = SPIN_LOCK_UNLOCKED;

int iommu_assign_dt_device(struct domain *d, struct dt_device_node *dev)
{
    int rc = -EBUSY;
    struct hvm_iommu *hd = domain_hvm_iommu(d);

    if ( !iommu_enabled || !hd->platform_ops )
        return -EINVAL;

    if ( !dt_device_is_protected(dev) )
        return -EINVAL;

    spin_lock(&dtdevs_lock);

    if ( !list_empty(&dev->domain_list) )
        goto fail;

    rc = iommu_construct(d);
    if ( rc )
        goto fail;

    rc = hd->platform_ops->assign_device(d, 0, dt_to_dev(dev));

    if ( rc )
        goto fail;

    list_add(&dev->domain_list, &hd->dt_devices);
    dt_device_set_used_by(dev, d->domain_id);

fail:
    spin_unlock(&dtdevs_lock);

    return rc;
}

int iommu_deassign_dt_device(struct domain *d, struct dt_device_node *dev)
{
    struct hvm_iommu *hd = domain_hvm_iommu(d);
    int rc;

    if ( !iommu_enabled || !hd->platform_ops )
        return -EINVAL;

    if ( !dt_device_is_protected(dev) )
        return -EINVAL;

    spin_lock(&dtdevs_lock);

    rc = hd->platform_ops->reassign_device(d, NULL, 0, dt_to_dev(dev));
    if ( rc )
        goto fail;

    list_del_init(&dev->domain_list);
    dt_device_set_used_by(dev, DOMID_IO);

fail:
    spin_unlock(&dtdevs_lock);

    return rc;
}

static bool_t iommu_dt_device_is_assigned(const struct dt_device_node *dev)
{
    bool_t assigned = 0;

    if ( !dt_device_is_protected(dev) )
        return 1;

    spin_lock(&dtdevs_lock);
    assigned = !list_empty(&dev->domain_list);
    spin_unlock(&dtdevs_lock);

    return assigned;
}

int iommu_dt_domain_init(struct domain *d)
{
    struct hvm_iommu *hd = domain_hvm_iommu(d);

    INIT_LIST_HEAD(&hd->dt_devices);

    return 0;
}

int iommu_release_dt_devices(struct domain *d)
{
    struct hvm_iommu *hd = domain_hvm_iommu(d);
    struct dt_device_node *dev, *_dev;
    int rc;

    list_for_each_entry_safe(dev, _dev, &hd->dt_devices, domain_list)
    {
        rc = iommu_deassign_dt_device(d, dev);
        if ( rc )
            dprintk(XENLOG_ERR, "Failed to deassign %s in domain %u\n",
                    dt_node_full_name(dev), d->domain_id);
        return rc;
    }

    return 0;
}

int iommu_do_dt_domctl(struct xen_domctl *domctl, struct domain *d,
                       XEN_GUEST_HANDLE_PARAM(xen_domctl_t) u_domctl)
{
    int ret;
    struct dt_device_node *dev;

    /* TODO: How to deal with XSM? */
    /* TODO: Do we need to check is_dying? Mostly to protect against
     * hypercall trying to passthrough a device while we are
     * dying.
     */

    printk("%s: %d\n", __func__, domctl->cmd);
    switch ( domctl->cmd )
    {
    case XEN_DOMCTL_assign_device:
        ret = -ENOSYS;
        printk("%s:%d\n", __func__, __LINE__);
        if ( domctl->u.assign_device.dev != XEN_DOMCTL_DEV_DT )
            break;

        ret = dt_find_node_by_gpath(domctl->u.assign_device.u.dt.path,
                                    domctl->u.assign_device.u.dt.size,
                                    &dev);
        printk("%s:%d\n", __func__, __LINE__);
        if ( ret )
            break;

        ret = iommu_assign_dt_device(d, dev);

        printk("%s:%d\n", __func__, __LINE__);
        if ( ret )
            printk(XENLOG_G_ERR "XEN_DOMCTL_assign_dt_device: assign \"%s\""
                   " to dom%u failed (%d)\n",
                   dt_node_full_name(dev), d->domain_id, ret);
        break;

    case XEN_DOMCTL_deassign_device:
        ret = -ENOSYS;
        printk("%s:%d\n", __func__, __LINE__);
        if ( domctl->u.assign_device.dev != XEN_DOMCTL_DEV_DT )
            break;

        ret = dt_find_node_by_gpath(domctl->u.assign_device.u.dt.path,
                                    domctl->u.assign_device.u.dt.size,
                                    &dev);
        printk("%s:%d\n", __func__, __LINE__);
        if ( ret )
            break;

        ret = iommu_deassign_dt_device(d, dev);

        if ( ret )
            printk(XENLOG_G_ERR "XEN_DOMCTL_assign_dt_device: assign \"%s\""
                   " to dom%u failed (%d)\n",
                   dt_node_full_name(dev), d->domain_id, ret);
        break;

    case XEN_DOMCTL_test_assign_device:
        ret = -ENOSYS;
        printk("%s:%d\n", __func__, __LINE__);
        if ( domctl->u.assign_device.dev != XEN_DOMCTL_DEV_DT )
            break;

        printk("%s:%d\n", __func__, __LINE__);
        ret = dt_find_node_by_gpath(domctl->u.assign_device.u.dt.path,
                                    domctl->u.assign_device.u.dt.size,
                                    &dev);
        if ( ret )
            break;

        if ( iommu_dt_device_is_assigned(dev) )
        {
            printk(XENLOG_G_ERR "%s already assigned, or not protected\n",
                   dt_node_full_name(dev));
            ret = -EINVAL;
        }
        break;

    default:
        ret = -ENOSYS;
        break;
    }

    return ret;
}
