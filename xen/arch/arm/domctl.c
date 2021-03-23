/******************************************************************************
 * Arch-specific domctl.c
 *
 * Copyright (c) 2012, Citrix Systems
 */

#include <xen/errno.h>
#include <xen/guest_access.h>
#include <xen/hypercall.h>
#include <xen/iocap.h>
#include <xen/lib.h>
#include <xen/list.h>
#include <xen/mm.h>
#include <xen/sched.h>
#include <xen/types.h>
#include <xsm/xsm.h>
#include <public/domctl.h>
#include <xen/xmalloc.h>
#include <xen/device_tree.h>
#include <asm/domain_build.h>

/*
 * overlay_node_track describes information about added nodes through dtbo.
 * @dt_host_new: Pointer to the updated dt_host_new unflattened 'updated fdt'.
 * @node_fullname: Store the name of nodes.
 * @entry: List pointer.
 */
struct overlay_track {
    struct list_head entry;
    struct dt_device_node *dt_host_new;
    /*
     * TODO: We keep max nodes to 10 in an overlay. But for now we will be
     * adding one node only.
     */
    char *node_fullname;
};

static LIST_HEAD(overlay_tracker);
static DEFINE_SPINLOCK(overlay_lock);

void arch_get_domain_info(const struct domain *d,
                          struct xen_domctl_getdomaininfo *info)
{
    /* All ARM domains use hardware assisted paging. */
    info->flags |= XEN_DOMINF_hap;
}

static int handle_vuart_init(struct domain *d, 
                             struct xen_domctl_vuart_op *vuart_op)
{
    int rc;
    struct vpl011_init_info info;

    info.console_domid = vuart_op->console_domid;
    info.gfn = _gfn(vuart_op->gfn);

    if ( d->creation_finished )
        return -EPERM;

    if ( vuart_op->type != XEN_DOMCTL_VUART_TYPE_VPL011 )
        return -EOPNOTSUPP;

    rc = domain_vpl011_init(d, &info);

    if ( !rc )
        vuart_op->evtchn = info.evtchn;

    return rc;
}

/*
 * First finds the device node to remove. Check if the device is being used by
 * any dom and finally remove it from dt_host. IOMMU is already being taken care
 * while destroying the domain.
 */
static long handle_del_fpga_nodes(char *full_dt_node_path)
{
    struct domain *d = hardware_domain;
    int rc = 0;
    uint32_t ret = 0;
    struct dt_device_node *fpga_device;
    struct overlay_track *entry, *temp;
    unsigned int naddr;
    unsigned int i, nirq;
    struct dt_raw_irq rirq;
    u64 addr, size;

    fpga_device = dt_find_node_by_path(full_dt_node_path);

    if ( fpga_device == NULL )
    {
        printk(XENLOG_G_ERR "Device %s is not present in the tree\n",
               full_dt_node_path);
        return -EINVAL;
    }

    ret = dt_device_used_by(fpga_device);

    if ( ret != 0 && ret != DOMID_IO )
    {
        printk(XENLOG_G_ERR "Cannot remove the device as it is being used by"
               "domain %d\n", ret);
        return -EPERM;
    }

    spin_lock(&overlay_lock);

    nirq = dt_number_of_irq(fpga_device);

    /* Remove IRQ permission */
    for ( i = 0; i < nirq; i++ )
    {
        rc = dt_device_get_raw_irq(fpga_device, i, &rirq);
        if ( rc )
        {
            printk(XENLOG_ERR "Unable to retrieve irq %u for %s\n",
                   i, dt_node_full_name(fpga_device));
            goto out;
        }

        rc = platform_get_irq(fpga_device, i);
        if ( rc < 0 )
        {
            printk(XENLOG_ERR "Unable to get irq %u for %s\n",
                   i, dt_node_full_name(fpga_device));
            goto out;
        }

        rc = irq_deny_access(d, rc);
        if ( rc )
        {
            printk(XENLOG_ERR "unable to revoke access for irq %u for %s\n",
                   i, dt_node_full_name(fpga_device));
            goto out;
        }
    }

    rc = iommu_remove_dt_device(fpga_device);
    if ( rc )
        goto out;

    naddr = dt_number_of_address(fpga_device);

    /* Remove mmio access. */
    for ( i = 0; i < naddr; i++ )
    {
        rc = dt_device_get_address(fpga_device, i, &addr, &size);
        if ( rc )
        {
            printk(XENLOG_ERR "Unable to retrieve address %u for %s\n",
                   i, dt_node_full_name(fpga_device));
            goto out;
        }

        rc = iomem_deny_access(d, paddr_to_pfn(addr),
                               paddr_to_pfn(PAGE_ALIGN(addr + size - 1)));
        if ( rc )
        {
            printk(XENLOG_ERR "Unable to remove dom%d access to"
                    " 0x%"PRIx64" - 0x%"PRIx64"\n",
                    d->domain_id,
                    addr & PAGE_MASK, PAGE_ALIGN(addr + size) - 1);
            goto out;
        }
    }

    rc = fpga_del_node(fpga_device);
    if ( rc )
        goto out;

    list_for_each_entry_safe( entry, temp, &overlay_tracker, entry )
    {
        if ( (strcmp(full_dt_node_path, entry->node_fullname) == 0) )
        {
            list_del(&entry->entry);
            xfree(entry->node_fullname);
            xfree(entry->dt_host_new);
            xfree(entry);
            goto out;
        }
    }

    printk(XENLOG_G_ERR "Cannot find the node in tracker. Memory will not"
           "be freed\n");
    rc = -ENOENT;

out:
    spin_unlock(&overlay_lock);
    return rc;
}

long arch_do_domctl(struct xen_domctl *domctl, struct domain *d,
                    XEN_GUEST_HANDLE_PARAM(xen_domctl_t) u_domctl)
{
    switch ( domctl->cmd )
    {
    case XEN_DOMCTL_cacheflush:
    {
        gfn_t s = _gfn(domctl->u.cacheflush.start_pfn);
        gfn_t e = gfn_add(s, domctl->u.cacheflush.nr_pfns);
        int rc;

        if ( domctl->u.cacheflush.nr_pfns > (1U<<MAX_ORDER) )
            return -EINVAL;

        if ( gfn_x(e) < gfn_x(s) )
            return -EINVAL;

        /* XXX: Handle preemption */
        do
            rc = p2m_cache_flush_range(d, &s, e);
        while ( rc == -ERESTART );

        return rc;
    }
    case XEN_DOMCTL_bind_pt_irq:
    {
        int rc;
        struct xen_domctl_bind_pt_irq *bind = &domctl->u.bind_pt_irq;
        uint32_t irq = bind->u.spi.spi;
        uint32_t virq = bind->machine_irq;

        /* We only support PT_IRQ_TYPE_SPI */
        if ( bind->irq_type != PT_IRQ_TYPE_SPI )
            return -EOPNOTSUPP;

        /*
         * XXX: For now map the interrupt 1:1. Other support will require to
         * modify domain_pirq_to_irq macro.
         */
        if ( irq != virq )
            return -EINVAL;

        /*
         * ARM doesn't require separating IRQ assignation into 2
         * hypercalls (PHYSDEVOP_map_pirq and DOMCTL_bind_pt_irq).
         *
         * Call xsm_map_domain_irq in order to keep the same XSM checks
         * done by the 2 hypercalls for consistency with other
         * architectures.
         */
        rc = xsm_map_domain_irq(XSM_HOOK, d, irq, NULL);
        if ( rc )
            return rc;

        rc = xsm_bind_pt_irq(XSM_HOOK, d, bind);
        if ( rc )
            return rc;

        if ( !irq_access_permitted(current->domain, irq) )
            return -EPERM;

        if ( !vgic_reserve_virq(d, virq) )
            return -EBUSY;

        rc = route_irq_to_guest(d, virq, irq, "routed IRQ");
        if ( rc )
            vgic_free_virq(d, virq);

        return rc;
    }
    case XEN_DOMCTL_unbind_pt_irq:
    {
        int rc;
        struct xen_domctl_bind_pt_irq *bind = &domctl->u.bind_pt_irq;
        uint32_t irq = bind->u.spi.spi;
        uint32_t virq = bind->machine_irq;

        /* We only support PT_IRQ_TYPE_SPI */
        if ( bind->irq_type != PT_IRQ_TYPE_SPI )
            return -EOPNOTSUPP;

        /* For now map the interrupt 1:1 */
        if ( irq != virq )
            return -EINVAL;

        rc = xsm_unbind_pt_irq(XSM_HOOK, d, bind);
        if ( rc )
            return rc;

        if ( !irq_access_permitted(current->domain, irq) )
            return -EPERM;

        rc = release_guest_irq(d, virq);
        if ( rc )
            return rc;

        vgic_free_virq(d, virq);

        return 0;
    }

    case XEN_DOMCTL_disable_migrate:
        d->disable_migrate = domctl->u.disable_migrate.disable;
        return 0;

    case XEN_DOMCTL_vuart_op:
    {
        int rc;
        unsigned int i;
        struct xen_domctl_vuart_op *vuart_op = &domctl->u.vuart_op;

        /* check that structure padding must be 0. */
        for ( i = 0; i < sizeof(vuart_op->pad); i++ )
            if ( vuart_op->pad[i] )
                return -EINVAL;

        switch( vuart_op->cmd )
        {
        case XEN_DOMCTL_VUART_OP_INIT:
            rc = handle_vuart_init(d, vuart_op);
            break;

        default:
            rc = -EINVAL;
            break;
        }

        if ( !rc )
            rc = copy_to_guest(u_domctl, domctl, 1);

        return rc;
    }

    case XEN_DOMCTL_delfpga:
    {
        char *full_dt_node_path;
        int rc;

        if ( domctl->u.fpga_del_dt.size > 0 )
            full_dt_node_path = xmalloc_bytes(domctl->u.fpga_del_dt.size);
        else
            return -EINVAL;

        if ( full_dt_node_path == NULL )
            return -ENOMEM;

        rc = copy_from_guest(full_dt_node_path,
                             domctl->u.fpga_del_dt.full_dt_node_path,
                             domctl->u.fpga_del_dt.size);
        if ( rc )
        {
            gprintk(XENLOG_ERR, "copy from guest failed\n");
            xfree(full_dt_node_path);

            return -EFAULT;
        }

        full_dt_node_path[domctl->u.fpga_del_dt.size - 1] = '\0';

        rc = handle_del_fpga_nodes(full_dt_node_path);

        xfree(full_dt_node_path);

        return rc;
    }

    default:
    {
        int rc;

        rc = subarch_do_domctl(domctl, d, u_domctl);

        if ( rc == -ENOSYS )
            rc = iommu_do_domctl(domctl, d, u_domctl);

        return rc;
    }
    }
}

void arch_get_info_guest(struct vcpu *v, vcpu_guest_context_u c)
{
    struct vcpu_guest_context *ctxt = c.nat;
    struct vcpu_guest_core_regs *regs = &c.nat->user_regs;

    vcpu_regs_hyp_to_user(v, regs);

    ctxt->sctlr = v->arch.sctlr;
    ctxt->ttbr0 = v->arch.ttbr0;
    ctxt->ttbr1 = v->arch.ttbr1;
    ctxt->ttbcr = v->arch.ttbcr;

    if ( !test_bit(_VPF_down, &v->pause_flags) )
        ctxt->flags |= VGCF_online;
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
