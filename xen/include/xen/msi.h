#ifndef __XEN_MSI_H_
#define __XEN_MSI_H_

#include <xen/pci.h>

#define msi_control_reg(base)		((base) + PCI_MSI_FLAGS)
#define msi_lower_address_reg(base)	((base) + PCI_MSI_ADDRESS_LO)
#define msi_upper_address_reg(base)	((base) + PCI_MSI_ADDRESS_HI)
#define msi_data_reg(base, is64bit)	\
	((base) + ((is64bit) ? PCI_MSI_DATA_64 : PCI_MSI_DATA_32))
#define msi_mask_bits_reg(base, is64bit) \
	((base) + PCI_MSI_MASK_BIT - ((is64bit) ? 0 : 4))
#define msi_pending_bits_reg(base, is64bit) \
	((base) + PCI_MSI_MASK_BIT + ((is64bit) ? 4 : 0))
#define multi_msi_capable(control) \
	(1U << MASK_EXTR(control, PCI_MSI_FLAGS_QMASK))
#define multi_msi_enable(control, num) \
	((control) |= MASK_INSR(fls(num) - 1, PCI_MSI_FLAGS_QSIZE))
#define is_64bit_address(control)	(!!((control) & PCI_MSI_FLAGS_64BIT))
#define is_mask_bit_support(control)	(!!((control) & PCI_MSI_FLAGS_MASKBIT))

#define msix_control_reg(base)		((base) + PCI_MSIX_FLAGS)
#define msix_table_offset_reg(base)	((base) + PCI_MSIX_TABLE)
#define msix_pba_offset_reg(base)	((base) + PCI_MSIX_PBA)
#define msix_table_size(control) 	(((control) & PCI_MSIX_FLAGS_QSIZE) + 1)


#ifdef CONFIG_HAS_PCI_MSI

#include <asm/msi.h>

int pdev_msix_assign(struct domain *d, struct pci_dev *pdev);
int pdev_msi_init(struct pci_dev *pdev);
void pdev_msi_deinit(struct pci_dev *pdev);
void pdev_dump_msi(const struct pci_dev *pdev);

#else /* !CONFIG_HAS_PCI_MSI */

static inline int pdev_msix_assign(struct domain *d, struct pci_dev *pdev)
{
    return 0;
}

static inline int pdev_msi_init(struct pci_dev *pdev)
{
    return 0;
}

static inline void pdev_msi_deinit(struct pci_dev *pdev) {}
static inline void pci_cleanup_msi(struct pci_dev *pdev) {}
static inline void pdev_dump_msi(const struct pci_dev *pdev) {}

#endif /* CONFIG_HAS_PCI_MSI */

#endif /* __XEN_MSI_H */

/*
 * Local variables:
 * mode: C
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * tab-width: 4
 * indent-tabs-mode: nil
 * End:
 */
