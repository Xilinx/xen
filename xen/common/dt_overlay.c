/*
 * xen/common/dt_overlay.c
 *
 * Device tree overlay support in Xen.
 *
 * Copyright (c) 2021 Xilinx Inc.
 * Written by Vikram Garhwal <fnu.vikram@xilinx.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms and conditions of the GNU General Public
 * License, version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */
#include <xen/iocap.h>
#include <xen/xmalloc.h>
#include <asm/domain_build.h>
#include <xen/dt_overlay.h>
#include <xen/guest_access.h>

static LIST_HEAD(overlay_tracker);
static DEFINE_SPINLOCK(overlay_lock);

static int dt_overlay_remove_node(struct dt_device_node *device_node)
{
    struct dt_device_node *np;
    struct dt_device_node *parent_node;
    struct dt_device_node *current_node;

    parent_node = device_node->parent;

    current_node = parent_node;

    if ( parent_node == NULL )
    {
        dt_dprintk("%s's parent node not found\n", device_node->name);
        return -EFAULT;
    }

    np = parent_node->child;

    if ( np == NULL )
    {
        dt_dprintk("parent node %s's not found\n", parent_node->name);
        return -EFAULT;
    }

    /* If node to be removed is only child node or first child. */
    if ( !dt_node_cmp(np->full_name, device_node->full_name) )
    {
        current_node->allnext = np->allnext;

        /* If node is first child but not the only child. */
        if ( np->sibling != NULL )
            current_node->child = np->sibling;
        else
            /* If node is only child. */
            current_node->child = NULL;
        return 0;
    }

    for ( np = parent_node->child; np->sibling != NULL; np = np->sibling )
    {
        current_node = np;
        if ( !dt_node_cmp(np->sibling->full_name, device_node->full_name) )
        {
            /* Found the node. Now we remove it. */
            current_node->allnext = np->allnext->allnext;

            if ( np->sibling->sibling )
                current_node->sibling = np->sibling->sibling;
            else
                current_node->sibling = NULL;

            break;
        }
    }

    return 0;
}

/* Basic sanity check for the dtbo tool stack provided to Xen. */
static int check_overlay_fdt(const void *overlay_fdt, uint32_t overlay_fdt_size)
{
    if ( (fdt_totalsize(overlay_fdt) != overlay_fdt_size) ||
          fdt_check_header(overlay_fdt) )
    {
        printk(XENLOG_ERR "The overlay FDT is not a valid Flat Device Tree\n");
        return -EINVAL;
    }

    return 0;
}

static unsigned int overlay_node_count(void *fdto)
{
    unsigned int num_overlay_nodes = 0;
    int fragment;

    fdt_for_each_subnode(fragment, fdto, 0)
    {

        int subnode;
        int overlay;

        overlay = fdt_subnode_offset(fdto, fragment, "__overlay__");

        /*
         * Overlay value can be < 0. But fdt_for_each_subnode() loop checks for
         * overlay >= 0. So, no need for a overlay>=0 check here.
         */

        fdt_for_each_subnode(subnode, fdto, overlay)
        {
            num_overlay_nodes++;
        }
    }

    return num_overlay_nodes;
}

/*
 * overlay_get_nodes_info will get the all node's full name with path. This is
 * useful when checking node for duplication i.e. dtbo tries to add nodes which
 * already exists in device tree.
 */
static int overlay_get_nodes_info(const void *fdto, char ***nodes_full_path,
                                  unsigned int num_overlay_nodes)
{
    int fragment;
    unsigned int node_num = 0;

    *nodes_full_path = xmalloc_bytes(num_overlay_nodes * sizeof(char *));

    if ( *nodes_full_path == NULL )
        return -ENOMEM;
    memset(*nodes_full_path, 0x0, num_overlay_nodes * sizeof(char *));

    fdt_for_each_subnode(fragment, fdto, 0)
    {
        int target;
        int overlay;
        int subnode;
        const char *target_path;

        target = fdt_overlay_target_offset(device_tree_flattened, fdto,
                                           fragment, &target_path);
        if ( target < 0 )
            return target;

        overlay = fdt_subnode_offset(fdto, fragment, "__overlay__");

        /*
         * Overlay value can be < 0. But fdt_for_each_subnode() loop checks for
         * overlay >= 0. So, no need for a overlay>=0 check here.
         */
        fdt_for_each_subnode(subnode, fdto, overlay)
        {
            const char *node_name = fdt_get_name(fdto, subnode, NULL);
            int node_name_len = strlen(node_name);
            int target_path_len = strlen(target_path);
            int node_full_name_len = target_path_len + node_name_len + 2;

            (*nodes_full_path)[node_num] = xmalloc_bytes(node_full_name_len);

            if ( (*nodes_full_path)[node_num] == NULL )
                return -ENOMEM;

            memcpy((*nodes_full_path)[node_num], target_path, target_path_len);

            (*nodes_full_path)[node_num][target_path_len] = '/';

            memcpy((*nodes_full_path)[node_num] + target_path_len + 1, node_name,
                   node_name_len);

            (*nodes_full_path)[node_num][node_full_name_len - 1] = '\0';

            node_num++;
        }
    }

    return 0;
}

/* Remove nodes from dt_host. */
static int remove_nodes(char **full_dt_node_path, int **nodes_irq,
                        int *node_num_irq, unsigned int num_nodes)
{
    struct domain *d = hardware_domain;
    int rc = 0;
    struct dt_device_node *overlay_node;
    unsigned int naddr;
    unsigned int i, j, nirq;
    u64 addr, size;
    domid_t domid = 0;

    for ( j = 0; j < num_nodes; j++ )
    {
        dt_dprintk("Finding node %s in the dt_host\n", full_dt_node_path[j]);

        overlay_node = dt_find_node_by_path(full_dt_node_path[j]);

        if ( overlay_node == NULL )
        {
            printk(XENLOG_ERR "Device %s is not present in the tree. Removing nodes failed\n",
                   full_dt_node_path[j]);
            return -EINVAL;
        }

        domid = dt_device_used_by(overlay_node);

        dt_dprintk("Checking if node %s is used by any domain\n",
                   full_dt_node_path[j]);

        /*
         * TODO: Below check will changed when node assigning to running VM will
         * be introduced.
         */
        if ( domid != 0 && domid != DOMID_IO )
        {
            printk(XENLOG_ERR "Device %s as it is being used by domain %d. Removing nodes failed\n",
                   full_dt_node_path[j], domid);
            return -EINVAL;
        }

        dt_dprintk("Removing node: %s\n", full_dt_node_path[j]);

        nirq = node_num_irq[j];

        /* Remove IRQ permission */
        for ( i = 0; i < nirq; i++ )
        {
            rc = nodes_irq[j][i];
            /*
             * TODO: We don't handle shared IRQs for now. So, it is assumed that
             * the IRQs was not shared with another domain.
             */
            rc = irq_deny_access(d, rc);
            if ( rc )
            {
                printk(XENLOG_ERR "unable to revoke access for irq %u for %s\n",
                       i, dt_node_full_name(overlay_node));
                return rc;
            }
        }

        rc = iommu_remove_dt_device(overlay_node);
        if ( rc != 0 && rc != -ENXIO )
            return rc;

        naddr = dt_number_of_address(overlay_node);

        /* Remove mmio access. */
        for ( i = 0; i < naddr; i++ )
        {
            rc = dt_device_get_address(overlay_node, i, &addr, &size);
            if ( rc )
            {
                printk(XENLOG_ERR "Unable to retrieve address %u for %s\n",
                       i, dt_node_full_name(overlay_node));
                return rc;
            }

            rc = iomem_deny_access(d, paddr_to_pfn(addr),
                                   paddr_to_pfn(PAGE_ALIGN(addr + size - 1)));
            if ( rc )
            {
                printk(XENLOG_ERR "Unable to remove dom%d access to"
                        " 0x%"PRIx64" - 0x%"PRIx64"\n",
                        d->domain_id,
                        addr & PAGE_MASK, PAGE_ALIGN(addr + size) - 1);
                return rc;
            }
        }

        rc = dt_overlay_remove_node(overlay_node);
        if ( rc )
            return rc;
    }

    return rc;
}

/*
 * First finds the device node to remove. Check if the device is being used by
 * any dom and finally remove it from dt_host. IOMMU is already being taken care
 * while destroying the domain.
 */
static long handle_remove_overlay_nodes(char **full_dt_nodes_path,
                                        unsigned int num_nodes)
{
    int rc = 0;
    struct overlay_track *entry, *temp, *track;
    bool found_entry = false;
    unsigned int i;

    spin_lock(&overlay_lock);

    /*
     * First check if dtbo is correct i.e. it should one of the dtbo which was
     * used when dynamically adding the node.
     * Limitation: Cases with same node names but different property are not
     * supported currently. We are relying on user to provide the same dtbo
     * as it was used when adding the nodes.
     */
    list_for_each_entry_safe( entry, temp, &overlay_tracker, entry )
    {
        /* Checking the num of nodes first. If not same skip to next entry. */
        if ( num_nodes == entry->num_nodes )
        {
            for ( i = 0; i < num_nodes; i++ )
            {
                if ( strcmp(full_dt_nodes_path[i], entry->nodes_fullname[i]) )
                {
                    /* Node name didn't match. Skip to next entry. */
                    break;
                }
            }

            /* Found one tracker with all node name matching. */
            track = entry;
            found_entry = true;
            break;
        }
    }

    if ( found_entry == false )
    {
        rc = -EINVAL;

        printk(XENLOG_ERR "Cannot find any matching tracker with input dtbo."
               " Removing nodes is supported for only prior added dtbo. Please"
               " provide a valid dtbo which was used to add the nodes.\n");
        goto out;

    }

    rc = remove_nodes(full_dt_nodes_path, entry->nodes_irq, entry->node_num_irq,
                      num_nodes);

    if ( rc )
    {
        printk(XENLOG_ERR "Removing node failed\n");
        goto out;
    }

    list_del(&entry->entry);

    for ( i = 0; i < entry->num_nodes && entry->nodes_fullname[i] != NULL; i++ )
    {
        xfree(entry->nodes_fullname[i]);
    }
    xfree(entry->nodes_fullname);
    for ( i = 0; i < entry->num_nodes && entry->nodes_irq[i] != NULL; i++ )
    {
        xfree(entry->nodes_irq[i]);
    }
    xfree(entry->nodes_irq);
    xfree(entry->node_num_irq);
    xfree(entry->dt_host_new);
    xfree(entry->fdt);
    xfree(entry);

out:
    spin_unlock(&overlay_lock);
    return rc;
}

long dt_sysctl(struct xen_sysctl *op)
{
    long ret = 0;
    void *overlay_fdt;
    char **nodes_full_path = NULL;
    unsigned int num_overlay_nodes = 0;

    if ( op->u.dt_overlay.overlay_fdt_size <= 0 )
        return -EINVAL;

    overlay_fdt = xmalloc_bytes(op->u.dt_overlay.overlay_fdt_size);

    if ( overlay_fdt == NULL )
        return -ENOMEM;

    ret = copy_from_guest(overlay_fdt, op->u.dt_overlay.overlay_fdt,
                         op->u.dt_overlay.overlay_fdt_size);
    if ( ret )
    {
        gprintk(XENLOG_ERR, "copy from guest failed\n");
        xfree(overlay_fdt);

        return -EFAULT;
    }

    switch ( op->u.dt_overlay.overlay_op )
    {
    case XEN_SYSCTL_DT_OVERLAY_REMOVE:
        ret = check_overlay_fdt(overlay_fdt,
                                op->u.dt_overlay.overlay_fdt_size);
        if ( ret )
        {
            ret = -EFAULT;
            break;
        }

        num_overlay_nodes = overlay_node_count(overlay_fdt);
        if ( num_overlay_nodes == 0 )
        {
            ret = -ENOMEM;
            break;
        }

        ret = overlay_get_nodes_info(overlay_fdt, &nodes_full_path,
                                     num_overlay_nodes);
        if ( ret )
             break;

        ret = handle_remove_overlay_nodes(nodes_full_path,
                                          num_overlay_nodes);
        break;

    default:
        break;
    }

    if ( nodes_full_path != NULL )
    {
        int i;
        for ( i = 0; i < num_overlay_nodes && nodes_full_path[i] != NULL; i++ )
        {
            xfree(nodes_full_path[i]);
        }
        xfree(nodes_full_path);
    }

    return ret;
}
