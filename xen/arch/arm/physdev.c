/******************************************************************************
 * Arch-specific physdev.c
 *
 * Copyright (c) 2012, Citrix Systems
 */

#include <xen/config.h>
#include <xen/types.h>
#include <xen/lib.h>
#include <xen/errno.h>
#include <xen/iocap.h>
#include <xen/guest_access.h>
#include <xsm/xsm.h>
#include <asm/current.h>
#include <asm/hypercall.h>
#include <public/physdev.h>

static int physdev_map_pirq(domid_t domid, int type, int index, int *pirq_p)
{
    struct domain *d;
    int ret;
    int irq = index;
    int virq;

    d = rcu_lock_domain_by_any_id(domid);
    if ( d == NULL )
        return -ESRCH;

    ret = xsm_map_domain_pirq(XSM_TARGET, d);
    if ( ret )
        goto free_domain;

    /* For now we only suport GSI */
    if ( type != MAP_PIRQ_TYPE_GSI )
    {
        ret = -EINVAL;
        dprintk(XENLOG_G_ERR,
                "dom%u: wrong map_pirq type 0x%x, only MAP_PIRQ_TYPE_GSI is supported.\n",
                d->domain_id, type);
        goto free_domain;
    }

    if ( !is_assignable_irq(irq) )
    {
        ret = -EINVAL;
        dprintk(XENLOG_G_ERR, "IRQ%u is not routable to a guest\n", irq);
        goto free_domain;
    }

    ret = -EPERM;
    if ( !irq_access_permitted(current->domain, irq) )
        goto free_domain;

    if ( *pirq_p < 0 )
    {
        BUG_ON(irq < 16);   /* is_assignable_irq already denies SGIs */
        virq = vgic_allocate_virq(d, (irq >= 32));

        ret = -ENOSPC;
        if ( virq < 0 )
            goto free_domain;
    }
    else
    {
        ret = -EBUSY;
        virq = *pirq_p;

        if ( !vgic_reserve_virq(d, virq) )
            goto free_domain;
    }

    gdprintk(XENLOG_DEBUG, "irq = %u virq = %u\n", irq, virq);

    ret = route_irq_to_guest(d, virq, irq, "routed IRQ");

    if ( !ret )
        *pirq_p = virq;
    else
        vgic_free_virq(d, virq);

free_domain:
    rcu_unlock_domain(d);

    return ret;
}

int physdev_unmap_pirq(domid_t domid, int pirq)
{
    struct domain *d;
    int ret;

    d = rcu_lock_domain_by_any_id(domid);
    if ( d == NULL )
        return -ESRCH;

    ret = xsm_unmap_domain_pirq(XSM_TARGET, d);
    if ( ret )
        goto free_domain;

    ret = release_guest_irq(d, pirq);
    if ( ret )
        goto free_domain;

    vgic_free_virq(d, pirq);

free_domain:
    rcu_unlock_domain(d);

    return ret;
}

int do_physdev_op(int cmd, XEN_GUEST_HANDLE_PARAM(void) arg)
{
    int ret;

    switch ( cmd )
    {
    case PHYSDEVOP_map_pirq:
        {
            physdev_map_pirq_t map;

            ret = -EFAULT;
            if ( copy_from_guest(&map, arg, 1) != 0 )
                break;

            ret = physdev_map_pirq(map.domid, map.type, map.index, &map.pirq);

            if ( __copy_to_guest(arg, &map, 1) )
                ret = -EFAULT;
        }
        break;

    case PHYSDEVOP_unmap_pirq:
        {
            physdev_unmap_pirq_t unmap;

            ret = -EFAULT;
            if ( copy_from_guest(&unmap, arg, 1) != 0 )
                break;

            ret = physdev_unmap_pirq(unmap.domid, unmap.pirq);
        }

    default:
        ret = -ENOSYS;
        break;
    }

    return ret;
}

/*
 * Local variables:
 * mode: C
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * End:
 */
