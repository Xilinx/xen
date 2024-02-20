/*
 * This program is free software; you can redistribute it and/or
 * modify it under the terms and conditions of the GNU General Public
 * License, version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public
 * License along with this program; If not, see <http://www.gnu.org/licenses/>.
 */

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
void vpci_msix_arch_init_entry(struct vpci_msix_entry *entry) {}
void vpci_msi_arch_update(struct vpci_msi *msi, const struct pci_dev *pdev) {}
int vpci_msix_arch_print(const struct vpci_msix *msix)
{
    return 0;
}

static int vpci_get_msi_base(const struct pci_dev *pdev, uint64_t *msi_base)
{
    if ( domain_use_host_layout(pdev->domain) )
    {
        struct pci_host_bridge *bridge;

        bridge = pci_find_host_bridge(pdev->seg, pdev->bus);
        if ( unlikely(!bridge) )
        {
            gprintk(XENLOG_ERR, "Unable to find PCI bridge for %pp\n",
                    &pdev->sbdf);
            return -ENODEV;
        }

        *msi_base = bridge->its_msi_base + ITS_DOORBELL_OFFSET;
    }
    else
        *msi_base = GUEST_GICV3_ITS_BASE + ITS_DOORBELL_OFFSET;

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

    pci_conf_write32(pdev->sbdf, msi->mask,
                     msi_mask_bits_reg(pos,pdev->vpci->msi->address64));
}

int vpci_msix_arch_disable_entry(struct vpci_msix_entry *entry,
                                 const struct pci_dev *pdev)
{
    vpci_msix_arch_mask_entry(entry, pdev, true);

    return 0;
}

void vpci_msix_arch_mask_entry(struct vpci_msix_entry *entry,
                               const struct pci_dev *pdev, bool mask)
{
    uint32_t mask_bits;
    paddr_t phys_addr = vmsix_table_addr(pdev->vpci, VPCI_MSIX_TABLE);
    uint32_t entry_nr = vmsix_entry_nr(pdev->vpci->msix, entry);
    void __iomem *desc_addr = ioremap_nocache(phys_addr +
                                              entry_nr * PCI_MSIX_ENTRY_SIZE,
                                              PCI_MSIX_ENTRY_SIZE);

    mask_bits = readl(desc_addr + PCI_MSIX_ENTRY_VECTOR_CTRL_OFFSET);
    mask_bits &= ~PCI_MSIX_VECTOR_BITMASK;
    if ( mask )
        mask_bits |= PCI_MSIX_VECTOR_BITMASK;
    writel(mask_bits, desc_addr + PCI_MSIX_ENTRY_VECTOR_CTRL_OFFSET);

    readl(desc_addr + PCI_MSIX_ENTRY_VECTOR_CTRL_OFFSET);

    iounmap(desc_addr);
}

int vpci_msix_arch_enable_entry(struct vpci_msix_entry *entry,
                                const struct pci_dev *pdev, paddr_t table_base)
{
    int ret;
    uint64_t msi_base;
    paddr_t phys_addr = vmsix_table_addr(pdev->vpci, VPCI_MSIX_TABLE);
    uint32_t entry_nr = vmsix_entry_nr(pdev->vpci->msix, entry);
    void __iomem *desc_addr = ioremap_nocache(phys_addr +
                                              entry_nr * PCI_MSIX_ENTRY_SIZE,
                                              PCI_MSIX_ENTRY_SIZE);

    ret = vpci_get_msi_base(pdev, &msi_base);
    if ( ret )
    {
        iounmap(desc_addr);
        return ret;
    }

    writel(lower_32_bits(msi_base),
           desc_addr + PCI_MSIX_ENTRY_LOWER_ADDR_OFFSET);
    writel(upper_32_bits(msi_base),
           desc_addr + PCI_MSIX_ENTRY_UPPER_ADDR_OFFSET);
    writel(entry->data, desc_addr + PCI_MSIX_ENTRY_DATA_OFFSET);

    iounmap(desc_addr);

    vpci_msix_arch_mask_entry(entry, pdev, false);

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
