/* SPDX-License-Identifier: GPL-2.0-only */

#include <xen/msi.h>
#include <xen/sched.h>
#include <xen/vpci.h>
#include <xen/vmap.h>

#include <asm/gic_v3_its.h>
#include <asm/io.h>

#define upper_32_bits(n) ((uint32_t)(((n) >> 16) >> 16))
#define lower_32_bits(n) ((uint32_t)((n) & 0xffffffffU))

void vpci_msi_arch_init(struct vpci_msi *msi) { }
void vpci_msi_arch_print(const struct vpci_msi *msi) { }
void vpci_msi_arch_disable(struct vpci_msi *msi,
                           const struct pci_dev *pdev) { }
void vpci_msi_arch_update(struct vpci_msi *msi, const struct pci_dev *pdev) {}
int vpci_msix_arch_print(const struct vpci_msix *msix)
{
    return 0;
}

static int vpci_get_msi_base(const struct pci_dev *pdev, uint64_t *msi_base)
{
    struct pci_host_bridge *bridge;

    bridge = pci_find_host_bridge(pdev->seg, pdev->bus);
    if ( unlikely(!bridge) )
    {
        gprintk(XENLOG_ERR, "Unable to find PCI bridge for %pp\n",
                &pdev->sbdf);
        return -ENODEV;
    }

    if ( bridge->its_msi_base && domain_use_host_layout(pdev->domain) )
        *msi_base = bridge->its_msi_base + ITS_DOORBELL_OFFSET;
    else if ( pdev->domain->arch.vgic.version == GIC_V3 &&
              !domain_use_host_layout(pdev->domain) )
        *msi_base = GUEST_GICV3_ITS_BASE + ITS_DOORBELL_OFFSET;
    else
        *msi_base = pdev->vpci->msi->address;

    return 0;
}

int vpci_msi_arch_enable(struct vpci_msi *msi, const struct pci_dev *pdev,
                         unsigned int vectors)
{
    uint64_t msi_base = 0;
    int ret;
    unsigned int pos = pci_find_cap_offset(pdev->sbdf, PCI_CAP_ID_MSI);

    if ( msi->address )
    {
        ret = vpci_get_msi_base(pdev, &msi_base);
        if ( ret )
        {
            return ret;
        }
        pci_conf_write32(pdev->sbdf, msi_lower_address_reg(pos),
                         lower_32_bits(msi_base));
    }

    if ( pdev->vpci->msi->address64 )
    {
        pci_conf_write32(pdev->sbdf, msi_upper_address_reg(pos),
                         upper_32_bits(msi_base));
    }

    if ( msi->data )
    {
        pci_conf_write16(pdev->sbdf, msi_data_reg(pos,
                         pdev->vpci->msi->address64), msi->data);
    }
    return 0;
}

void vpci_msi_arch_mask(struct vpci_msi *msi, const struct pci_dev *pdev,
                        unsigned int entry, bool mask)
{
    unsigned int pos = pci_find_cap_offset(pdev->sbdf, PCI_CAP_ID_MSI);

    if ( mask )
        msi->mask |= 1U << entry;
    else
        msi->mask &= ~(1U << entry);

    pci_conf_write32(pdev->sbdf,
                     msi_mask_bits_reg(pos, pdev->vpci->msi->address64),
                     msi->mask);
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
