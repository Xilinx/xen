/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * xen/arch/arm/vpci.c
 */
#include <xen/sched.h>
#include <xen/vpci.h>

#include <asm/mmio.h>

static bool vpci_sbdf_from_gpa(struct domain *d,
                               const struct pci_host_bridge *bridge,
                               paddr_t gpa, pci_sbdf_t *sbdf)
{
    bool translated = true;

    ASSERT(sbdf);

    if ( bridge )
    {
        sbdf->sbdf = VPCI_ECAM_BDF(gpa - bridge->cfg->phys_addr);
        sbdf->seg = bridge->segment;
        sbdf->bus += bridge->cfg->busn_start;
    }
    else
    {
        /*
         * For the passed through devices we need to map their virtual SBDF
         * to the physical PCI device being passed through.
         */
        sbdf->sbdf = VPCI_ECAM_BDF(gpa - GUEST_VPCI_ECAM_BASE);
        read_lock(&d->pci_lock);
        translated = vpci_translate_virtual_device(d, sbdf);
        read_unlock(&d->pci_lock);
    }

    return translated;
}

static int vpci_mmio_read(struct vcpu *v, mmio_info_t *info,
                          register_t *r, void *p)
{
    struct pci_host_bridge *bridge = p;
    pci_sbdf_t sbdf;
    /* data is needed to prevent a pointer cast on 32bit */
    unsigned long data;

    ASSERT(!bridge == !is_hardware_domain(v->domain));

    if ( !vpci_sbdf_from_gpa(v->domain, bridge, info->gpa, &sbdf) )
    {
        *r = ~0UL;
        return 1;
    }

    if ( vpci_ecam_read(sbdf, ECAM_REG_OFFSET(info->gpa),
                        1U << info->dabt.size, &data) )
    {
        *r = data;
        return 1;
    }

    *r = ~0UL;

    return 0;
}

static int vpci_mmio_write(struct vcpu *v, mmio_info_t *info,
                           register_t r, void *p)
{
    struct pci_host_bridge *bridge = p;
    pci_sbdf_t sbdf;

    ASSERT(!bridge == !is_hardware_domain(v->domain));

    if ( !vpci_sbdf_from_gpa(v->domain, bridge, info->gpa, &sbdf) )
        return 1;

    return vpci_ecam_write(sbdf, ECAM_REG_OFFSET(info->gpa),
                           1U << info->dabt.size, r);
}

static const struct mmio_handler_ops vpci_mmio_handler = {
    .read  = vpci_mmio_read,
    .write = vpci_mmio_write,
};

static int vpci_setup_mmio_handler_cb(struct domain *d,
                                      struct pci_host_bridge *bridge)
{
    struct pci_config_window *cfg = bridge->cfg;

    register_mmio_handler(d, &vpci_mmio_handler,
                          cfg->phys_addr, cfg->size, bridge);

    /* We have registered a single MMIO handler. */
    return 1;
}

int domain_vpci_init(struct domain *d)
{
    if ( !has_vpci(d) )
        return 0;

    /*
     * The hardware domain gets as many MMIOs as required by the
     * physical host bridge.
     * Guests get the virtual platform layout: one virtual host bridge for now.
     */
    if ( is_hardware_domain(d) )
    {
        int ret;

        ret = pci_host_iterate_bridges_and_count(d, vpci_setup_mmio_handler_cb);
        if ( ret < 0 )
            return ret;
    }
    else
        register_mmio_handler(d, &vpci_mmio_handler,
                              GUEST_VPCI_ECAM_BASE, GUEST_VPCI_ECAM_SIZE, NULL);

    return 0;
}

static int vpci_get_num_handlers_cb(struct domain *d,
                                    struct pci_host_bridge *bridge)
{
    /* Each bridge has a single MMIO handler for the configuration space. */
    return 1;
}

unsigned int domain_vpci_get_num_mmio_handlers(struct domain *d)
{
    unsigned int count;

    if ( !has_vpci(d) )
        return 0;

    if ( is_hardware_domain(d) )
    {
        int ret = pci_host_iterate_bridges_and_count(d, vpci_get_num_handlers_cb);

        if ( ret < 0 )
        {
            ASSERT_UNREACHABLE();
            return 0;
        }

        return ret;
    }

    /*
     * For guests each host bridge requires one region to cover the
     * configuration space. At the moment, we only expose a single host bridge.
     */
    count = 1;

    /*
     * There's a single MSI-X MMIO handler that deals with both PBA
     * and MSI-X tables per each PCI device being passed through.
     * Maximum number of emulated virtual devices is VPCI_MAX_VIRT_DEV.
     */
    if ( IS_ENABLED(CONFIG_HAS_PCI_MSI) )
        count += VPCI_MAX_VIRT_DEV;

    return count;
}

/*
 * Local variables:
 * mode: C
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * End:
 */

