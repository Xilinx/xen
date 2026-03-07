/* SPDX-License-Identifier: GPL-2.0-only */

#include <xen/sched.h>
#include <xen/pci.h>
#include <xen/vpci.h>

void vpci_msix_arch_register(struct vpci_msix *msix, struct domain *d) { }

int vpci_make_msix_hole(const struct pci_dev *pdev) { return 0; }

int vpci_remove_msix_regions(const struct pci_dev *pdev) { return 0; }

void vpci_msix_arch_cleanup(struct vpci *vpci) { }

void vpci_msix_arch_control_write(
    const struct pci_dev *pdev, unsigned int reg, uint32_t val, void *data) { }

void vpci_msix_arch_init_entry(struct vpci_msix_entry *entry) { }

/*
 * Local variables:
 * mode: C
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * tab-width: 4
 * indent-tabs-mode: nil
 * End:
 */

