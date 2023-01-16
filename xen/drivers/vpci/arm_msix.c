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

#include <xen/sched.h>
#include <xen/vpci.h>

#include <asm/io.h>

void vpci_msix_arch_register(struct vpci_msix *msix, struct domain *d) { }

static int arm_msix_read(struct vcpu *v, mmio_info_t *info,
                         register_t *data, void *priv)
{
    struct vpci *vpci = (struct vpci *)priv;
    struct vpci_msix *msix = vpci->msix;
    unsigned int len = 1U << info->dabt.size;
    unsigned long addr = info->gpa;

    return vpci_msix_read(msix, addr, len, data);
}

static int arm_msix_write(struct vcpu *v, mmio_info_t *info,
                          register_t data, void *priv)
{
    struct vpci *vpci = (struct vpci *)priv;
    struct vpci_msix *msix = vpci->msix;
    unsigned int len = 1U << info->dabt.size;
    unsigned long addr = info->gpa;

    return vpci_msix_write(msix, addr, len, data);
}

static const struct mmio_handler_ops vpci_msi_mmio_handler = {
    .read  = arm_msix_read,
    .write = arm_msix_write,
};

int vpci_make_msix_hole(const struct pci_dev *pdev)
{
    struct vpci_msix *msix = pdev->vpci->msix;
    paddr_t addr,size;

    for ( int i = 0; msix && i < ARRAY_SIZE(msix->tables); i++ )
    {
        if ( is_hardware_pci_domain(pdev->domain) )
            addr = vmsix_table_addr(pdev->vpci, VPCI_MSIX_TABLE);
        else
            addr = vmsix_guest_table_addr(pdev->vpci, VPCI_MSIX_TABLE);

        size = vmsix_table_size(pdev->vpci, VPCI_MSIX_TABLE) - 1;

        register_mmio_handler(pdev->domain, &vpci_msi_mmio_handler,
                              addr, size, pdev->vpci);
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

