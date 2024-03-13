#ifndef __ASM_MSI_H
#define __ASM_MSI_H

static inline int pci_reset_msix_state(struct pci_dev *pdev) { return 0; }
static inline void msixtbl_init(struct domain *d) {}
static inline void pci_cleanup_msi(struct pci_dev *pdev) {}

struct arch_msix {
    unsigned int nr_entries;
    spinlock_t table_lock;
};

struct msi_desc {
    struct list_head list;
    int irq;
};

#endif
