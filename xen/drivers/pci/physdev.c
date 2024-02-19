
#include <xen/guest_access.h>
#include <xen/hypercall.h>
#include <xen/init.h>
#include <xen/vpci.h>

#ifndef COMPAT
typedef long ret_t;
#endif

ret_t pci_physdev_op(int cmd, XEN_GUEST_HANDLE_PARAM(void) arg)
{
    ret_t ret;

    switch ( cmd )
    {
    case PHYSDEVOP_pci_device_add: {
        struct physdev_pci_device_add add;
        struct pci_dev_info pdev_info;
        nodeid_t node = NUMA_NO_NODE;

        if ( hwdom_uses_vpci() )
            return 0;

        ret = -EFAULT;
        if ( copy_from_guest(&add, arg, 1) != 0 )
            break;

        pdev_info.is_extfn = (add.flags & XEN_PCI_DEV_EXTFN);
        if ( add.flags & XEN_PCI_DEV_VIRTFN )
        {
            pdev_info.is_virtfn = true;
            pdev_info.physfn.bus = add.physfn.bus;
            pdev_info.physfn.devfn = add.physfn.devfn;
        }
        else
            pdev_info.is_virtfn = false;

#ifdef CONFIG_NUMA
        if ( add.flags & XEN_PCI_DEV_PXM )
        {
            uint32_t pxm;
            size_t optarr_off = offsetof(struct physdev_pci_device_add, optarr) /
                                sizeof(add.optarr[0]);

            if ( copy_from_guest_offset(&pxm, arg, optarr_off, 1) )
                break;

            node = pxm_to_node(pxm);
        }
#endif

        ret = pci_add_device(hardware_domain, add.seg, add.bus, add.devfn,
                             &pdev_info, node);
        break;
    }

    case PHYSDEVOP_pci_device_remove: {
        struct physdev_pci_device dev;

        if ( hwdom_uses_vpci() )
            return 0;

        ret = -EFAULT;
        if ( copy_from_guest(&dev, arg, 1) != 0 )
            break;

        ret = pci_remove_device(dev.seg, dev.bus, dev.devfn);
        break;
    }

    case PHYSDEVOP_pci_device_state_reset: {
        struct physdev_pci_device dev;
        struct pci_dev *pdev;
        pci_sbdf_t sbdf;

#ifdef CONFIG_ARM
        if ( !is_pci_passthrough_enabled() )
            return -EOPNOTSUPP;
#endif

        ret = -EFAULT;
        if ( copy_from_guest(&dev, arg, 1) != 0 )
            break;
        sbdf = PCI_SBDF(dev.seg, dev.bus, dev.devfn);

        ret = xsm_resource_setup_pci(XSM_PRIV, sbdf.sbdf);
        if ( ret )
            break;

        pcidevs_lock();
        pdev = pci_get_pdev(NULL, sbdf);
        if ( !pdev )
        {
            pcidevs_unlock();
            ret = -ENODEV;
            break;
        }

        write_lock(&pdev->domain->pci_lock);
        ret = vpci_reset_device_state(pdev);
        write_unlock(&pdev->domain->pci_lock);
        pcidevs_unlock();
        if ( ret )
            printk(XENLOG_ERR "%pp: failed to reset PCI device state\n", &sbdf);
        break;
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
 * tab-width: 4
 * indent-tabs-mode: nil
 * End:
 */
