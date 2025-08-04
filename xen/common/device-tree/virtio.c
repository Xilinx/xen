/*
 * virtio configuration and construction.
 *
 * Copyright (c) 2025 Advanced Micro Devices, Inc.
 * Written by Edgar E. Iglesias <edgar.iglesias@amd.com>
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include <xen/device_tree.h>
#include <xen/fdt-domain-build.h>
#include <xen/libfdt/libfdt.h>
#include <xen/sched.h>

#include <xen/virtio/virtio-msg-bus-xen.h>
#include <xen/fdt-virtio.h>
#include <xen/virtio/vmp.h>

#include <asm/setup.h>

int __init dom_construct_virtio(struct boot_domain *bd)
{
    struct virtio_msg_bus_xen *bus;
    struct domain *d = bd->d;
    struct vmp *vmp;
    unsigned int i;
    int rc;

    BUILD_BUG_ON(ARRAY_SIZE(bd->virtio_mmio) != ARRAY_SIZE(d->virtio_msg_bus));

    for ( i = 0; i < ARRAY_SIZE(bd->virtio_mmio); i++ )
    {
        uint64_t addr = bd->virtio_mmio[i].addr;
        uint32_t size = bd->virtio_mmio[i].size;
        uint32_t irq = bd->virtio_mmio[i].irq;
        domid_t domid = bd->virtio_mmio[i].msg_bus.xen.device_domid;

        /* Non-zero size means the bus is enabled.  */
        if ( !size )
            continue;

        vmp = xzalloc(struct vmp);
        bus = xzalloc(struct virtio_msg_bus_xen);

        if ( !vmp || !bus )
        {
            printk(XENLOG_ERR "OOM: failed to allocate virtio objects\n");
            goto fail;
        }

        rc = virtio_msg_bus_xen_init(bus, d, domid);
        if ( rc )
        {
            printk(XENLOG_ERR "Failed to init virtio-msg-bus\n");
            goto fail;
        }

        /*
         * We check for conflicts when configuring (when parsing the fdt for
         * dom0less/hyperlaunch) so we do not expect conflicts here.
         */
        BUG_ON(d->virtio_msg_bus[i]);

        /* Attach the new bus.  */
        rcu_assign_pointer(d->virtio_msg_bus[i], &bus->bd);

        rc = vmp_init(vmp, &bus->bd, d, addr, irq);
        if ( rc )
        {
            /*
             * bus is already registered and will be freed by
             * virtio_msg_bus_domain_destroy() further down.
             */
            bus = NULL;
            goto fail;
        }
    }

    return 0;

fail:
    xfree(bus);
    xfree(vmp);

    /* This destroys and frees up all registered buses and vmp clients.  */
    virtio_msg_bus_domain_destroy(d);
    return rc;
}

static int __init parse_vmp(struct dt_device_node *np, struct boot_domain *bd)
{
    struct dt_device_node *bus_np;
    bool bus_found = false;
    const __be32 *cells;
    uint32_t device_domid;
    uint32_t bus_id;
    uint64_t addr;
    uint64_t size;
    uint32_t irq;
    uint32_t len;

    cells = dt_get_property(np, "reg", &len);
    if ( !cells )
    {
        printk(XENLOG_ERR "%s: Missing virtio reg property\n",
               dt_node_full_name(np));
        return -EINVAL;
    }

    dt_get_range(&cells, np, &addr, &size);
    if ( size != VMP_MMIO_SIZE )
    {
        printk(XENLOG_ERR
               "%s: Bad size of 0x%lx, the only supported size is 0x%x\n",
               dt_node_full_name(np), size, VMP_MMIO_SIZE);
        return -EINVAL;
    }

    if ( addr & (~PAGE_MASK) )
    {
        printk(XENLOG_ERR "%s: Address needs to be page aligned 0x%lx\n",
               dt_node_full_name(np), addr);
        return -EINVAL;
    }

    if ( !dt_property_read_u32(np, "irq", &irq) )
    {
        printk(XENLOG_ERR "%s: Missing irq property\n", dt_node_full_name(np));
        return -EINVAL;
    }

    /* Look for buses.  */
    dt_for_each_child_node(np, bus_np)
    {
        if ( !dt_device_is_compatible(bus_np, "xen,virtio-msg-bus-xen") )
            continue;

        if ( !dt_property_read_u32(bus_np, "device-domid", &device_domid) )
        {
            printk(XENLOG_ERR "%s: Missing device-domid\n",
                    dt_node_full_name(bus_np));
            return -EINVAL;
        }

        if ( !dt_property_read_u32(bus_np, "bus-id", &bus_id) )
        {
            printk(XENLOG_ERR "%s: Missing bus-id\n",
                    dt_node_full_name(bus_np));
            return -EINVAL;
        }

        if ( bus_id >= ARRAY_SIZE(bd->virtio_mmio) )
        {
            printk(XENLOG_ERR "%s: bus-id %d out of bounds (max %ld)\n",
                   dt_node_full_name(bus_np), bus_id,
                   ARRAY_SIZE(bd->virtio_mmio) - 1);
            return -EINVAL;
        }

        if ( bd->virtio_mmio[bus_id].size )
        {
            printk(XENLOG_ERR "%s: bus-id already in use\n",
                   dt_node_full_name(bus_np));
            return -EBUSY;
        }

        bus_found = true;
        break;
    }

    if ( !bus_found )
    {
        printk(XENLOG_ERR "%s: Missing bus\n", dt_node_full_name(np));
        return -EINVAL;
    }

    /* Success.  */
    bd->virtio_mmio[bus_id].addr = addr;
    bd->virtio_mmio[bus_id].size = size;
    bd->virtio_mmio[bus_id].irq = irq;
    bd->virtio_mmio[bus_id].msg_bus.xen.device_domid = device_domid;

    printk(XENLOG_INFO "%s: found virtio@%lx sz=%lx irq%d bus%d d%d\n",
           dt_node_full_name(np), addr, size, irq, bus_id, device_domid);
    return 0;
}

int __init parse_virtio(struct dt_device_node *node, struct boot_domain *bd)
{
    struct dt_device_node *np;
    int rc;

    dt_for_each_child_node(node, np)
    {
        if ( dt_device_is_compatible(np, "xen,virtio-mmio-nonblocking") )
        {
            rc = parse_vmp(np, bd);
            if ( rc )
                return rc;
            continue;
        }
    }

    return 0;
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
