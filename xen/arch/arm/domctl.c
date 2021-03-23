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
/* Included for FPGA dt add. */
#include <xen/libfdt/libfdt.h>
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

static int check_pfdt(void *pfdt, uint32_t pfdt_size)
{
    if ( fdt_totalsize(pfdt) != pfdt_size )
    {
        printk(XENLOG_ERR "Partial FDT is not a valid Flat Device Tree\n");
        return -EFAULT;
    }

    if ( fdt_check_header(pfdt) )
    {
        printk(XENLOG_ERR "Partial FDT is not a valid Flat Device Tree\n");
        return -EFAULT;
    }

    return 0;
}

static void overlay_get_node_info(void *fdto, char *node_full_path)
{
    int fragment;

    /*
     * Handle overlay nodes. But for now we are just handling one node.
     */
    fdt_for_each_subnode(fragment, fdto, 0)
    {
        int target;
        int overlay;
        int subnode;
        const char *target_path;

        target = overlay_get_target(device_tree_flattened, fdto, fragment,
                                    &target_path);
        overlay = fdt_subnode_offset(fdto, fragment, "__overlay__");

        fdt_for_each_subnode(subnode, fdto, overlay)
        {
            const char *node_name = fdt_get_name(fdto, subnode, NULL);
            int node_name_len = strlen(node_name);
            int target_path_len = strlen(target_path);

            memcpy(node_full_path, target_path, target_path_len);

            node_full_path[target_path_len] = '/';

            memcpy(node_full_path + target_path_len + 1, node_name,
                   node_name_len);

            node_full_path[target_path_len + 1 + node_name_len] = '\0';

            return;
        }
    }
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

/*
 * Adds only one device node at a time under target node.
 * We use dt_host_new to unflatten the updated device_tree_flattened. This is
 * done to avoid the removal of device_tree generation, iomem regions mapping to
 * DOM0 done by handle_node().
 */
static long handle_add_fpga_overlay(void *pfdt, uint32_t pfdt_size)
{
    int rc = 0;
    struct dt_device_node *fpga_node;
    char node_full_path[128];
    void *fdt = xmalloc_bytes(fdt_totalsize(device_tree_flattened));
    struct dt_device_node *dt_host_new;
    struct domain *d = hardware_domain;
    struct overlay_track *tr = NULL;
    int node_full_path_namelen;
    unsigned int naddr;
    unsigned int i;
    u64 addr, size;

    if ( fdt == NULL )
        return ENOMEM;

    spin_lock(&overlay_lock);

    memcpy(fdt, device_tree_flattened, fdt_totalsize(device_tree_flattened));

    rc = check_pfdt(pfdt, pfdt_size);
    if ( rc )
        goto err;

    overlay_get_node_info(pfdt, node_full_path);

    rc = fdt_overlay_apply(fdt, pfdt);
    if ( rc )
    {
        printk(XENLOG_ERR "Adding overlay node %s failed with error %d\n",
               node_full_path, rc);
        goto err;
    }

    /* Check if node already exists in dt_host. */
    fpga_node = dt_find_node_by_path(node_full_path);
    if ( fpga_node != NULL )
    {
        printk(XENLOG_ERR "node %s exists in device tree\n", node_full_path);
        rc = -EINVAL;
        goto err;
    }

    /* Unflatten the fdt into a new dt_host. */
    unflatten_device_tree(fdt, &dt_host_new);

    /* Find the newly added node in dt_host_new by it's full path. */
    fpga_node = _dt_find_node_by_path(dt_host_new, node_full_path);
    if ( fpga_node == NULL )
    {
        dt_dprintk("%s node not found\n", node_full_path);
        rc = -EFAULT;
        xfree(dt_host_new);
        goto err;
    }

    /* Just keep the node we intend to add. Remove every other node in list. */
    fpga_node->allnext = NULL;
    fpga_node->sibling = NULL;

    /* Add the node to dt_host. */
    rc = fpga_add_node(fpga_node, fpga_node->parent->full_name);
    if ( rc )
    {
        /* Node not added in dt_host. Safe to free dt_host_new. */
        xfree(dt_host_new);
        goto err;
    }

    /* Get the node from dt_host and add interrupt and IOMMUs. */
    fpga_node = dt_find_node_by_path(fpga_node->full_name);
    if ( fpga_node == NULL )
    {
        /* Sanity check. But code will never come in this loop. */
        printk(XENLOG_ERR "Cannot find %s node under updated dt_host\n",
               fpga_node->name);
        goto remove_node;
    }

    /* First let's handle the interrupts. */
    rc = handle_device_interrupts(d, fpga_node, false);
    if ( rc )
    {
        printk(XENLOG_G_ERR "Interrupt failed\n");
        goto remove_node;
    }

    /* Add device to IOMMUs */
    rc = iommu_add_dt_device(fpga_node);
    if ( rc < 0 )
    {
        printk(XENLOG_G_ERR "Failed to add %s to the IOMMU\n",
               dt_node_full_name(fpga_node));
        goto remove_node;
    }

    /* Set permissions. */
    naddr = dt_number_of_address(fpga_node);

    dt_dprintk("%s passthrough = %d naddr = %u\n",
               dt_node_full_name(fpga_node), false, naddr);

    /* Give permission and map MMIOs */
    for ( i = 0; i < naddr; i++ )
    {
        struct map_range_data mr_data = { .d = d, .p2mt = p2m_mmio_direct_c };
        rc = dt_device_get_address(fpga_node, i, &addr, &size);
        if ( rc )
        {
            printk(XENLOG_ERR "Unable to retrieve address %u for %s\n",
                   i, dt_node_full_name(fpga_node));
            goto remove_node;
        }

        rc = map_range_to_domain(fpga_node, addr, size, &mr_data);
        if ( rc )
            goto remove_node;
    }

    /* This will happen if everything above goes right. */
    tr = xzalloc(struct overlay_track);
    tr->dt_host_new = dt_host_new;
    node_full_path_namelen = strlen(node_full_path);
    tr->node_fullname = xmalloc_bytes(node_full_path_namelen + 1);

    if ( tr->node_fullname == NULL )
    {
        rc = -ENOMEM;
        goto remove_node;
    }

    memcpy(tr->node_fullname, node_full_path, node_full_path_namelen);
    tr->node_fullname[node_full_path_namelen] = '\0';

    INIT_LIST_HEAD(&tr->entry);
    list_add_tail(&tr->entry, &overlay_tracker);

err:
    spin_unlock(&overlay_lock);
    xfree(fdt);
    return rc;

/*
 * Failure case. We need to remove the node, free tracker(if tr exists) and
 * dt_host_new. As the tracker is not in list yet so it doesn't get freed in
 * handle_del_fpga_nodes() and due to that dt_host_new will not get freed so we
 * we free tracker and dt_host_new here.
 */
remove_node:
    spin_unlock(&overlay_lock);
    handle_del_fpga_nodes(node_full_path);
    xfree(dt_host_new);

    if ( tr )
        xfree(tr);

    xfree(fdt);
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

    case XEN_DOMCTL_addfpga:
    {
        void *pfdt;
        int rc;

        if ( domctl->u.fpga_add_dt.pfdt_size > 0 )
            pfdt = xmalloc_bytes(domctl->u.fpga_add_dt.pfdt_size);
        else
            return -EINVAL;

        if ( pfdt == NULL )
            return -ENOMEM;

        rc = copy_from_guest(pfdt, domctl->u.fpga_add_dt.pfdt,
                             domctl->u.fpga_add_dt.pfdt_size);
        if ( rc )
        {
            gprintk(XENLOG_ERR, "copy from guest failed\n");
            xfree(pfdt);

            return -EFAULT;
        }

        rc = handle_add_fpga_overlay(pfdt, domctl->u.fpga_add_dt.pfdt_size);

        xfree(pfdt);

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
