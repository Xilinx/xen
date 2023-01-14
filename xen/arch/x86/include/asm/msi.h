#ifndef __ASM_MSI_H
#define __ASM_MSI_H

#include <xen/cpumask.h>
#include <xen/pci.h>
#include <asm/byteorder.h>
#include <asm/hvm/vmx/vmcs.h>

/*
 * Constants for Intel APIC based MSI messages.
 */

/*
 * Shifts for MSI data
 */

#define MSI_DATA_VECTOR_SHIFT		0
#define  MSI_DATA_VECTOR_MASK		0x000000ff
#define	 MSI_DATA_VECTOR(v)		(((v) << MSI_DATA_VECTOR_SHIFT) & MSI_DATA_VECTOR_MASK)

#define MSI_DATA_DELIVERY_MODE_SHIFT	8
#define  MSI_DATA_DELIVERY_FIXED	(0 << MSI_DATA_DELIVERY_MODE_SHIFT)
#define  MSI_DATA_DELIVERY_LOWPRI	(1 << MSI_DATA_DELIVERY_MODE_SHIFT)
#define  MSI_DATA_DELIVERY_MODE_MASK    0x00000700

#define MSI_DATA_LEVEL_SHIFT		14
#define	 MSI_DATA_LEVEL_DEASSERT	(0 << MSI_DATA_LEVEL_SHIFT)
#define	 MSI_DATA_LEVEL_ASSERT		(1 << MSI_DATA_LEVEL_SHIFT)

#define MSI_DATA_TRIGGER_SHIFT		15
#define  MSI_DATA_TRIGGER_EDGE		(0 << MSI_DATA_TRIGGER_SHIFT)
#define  MSI_DATA_TRIGGER_LEVEL		(1 << MSI_DATA_TRIGGER_SHIFT)
#define  MSI_DATA_TRIGGER_MASK          0x00008000

/*
 * Shift/mask fields for msi address
 */

#define MSI_ADDR_BASE_HI            0
#define MSI_ADDR_BASE_LO            0xfee00000
#define MSI_ADDR_BASE_MASK          (~0xfffff)
#define MSI_ADDR_HEADER             MSI_ADDR_BASE_LO

#define MSI_ADDR_DESTMODE_SHIFT     2
#define MSI_ADDR_DESTMODE_PHYS      (0 << MSI_ADDR_DESTMODE_SHIFT)
#define MSI_ADDR_DESTMODE_LOGIC     (1 << MSI_ADDR_DESTMODE_SHIFT)
#define MSI_ADDR_DESTMODE_MASK      0x4

#define MSI_ADDR_REDIRECTION_SHIFT  3
#define MSI_ADDR_REDIRECTION_CPU    (0 << MSI_ADDR_REDIRECTION_SHIFT)
#define MSI_ADDR_REDIRECTION_LOWPRI (1 << MSI_ADDR_REDIRECTION_SHIFT)
#define MSI_ADDR_REDIRECTION_MASK   (1 << MSI_ADDR_REDIRECTION_SHIFT)

#define MSI_ADDR_DEST_ID_SHIFT		12
#define	 MSI_ADDR_DEST_ID_MASK		0x00ff000
#define  MSI_ADDR_DEST_ID(dest)		(((dest) << MSI_ADDR_DEST_ID_SHIFT) & MSI_ADDR_DEST_ID_MASK)

/* MAX fixed pages reserved for mapping MSIX tables. */
#define FIX_MSIX_MAX_PAGES              512

struct msi_info {
    pci_sbdf_t sbdf;
    int irq;
    int entry_nr;
    uint64_t table_base;
};

struct msi_msg {
	union {
		u64	address; /* message address */
		struct {
			u32	address_lo; /* message address low 32 bits */
			u32	address_hi; /* message address high 32 bits */
		};
	};
	u32	data;		/* 16 bits of msi message data */
	u32	dest32;		/* used when Interrupt Remapping with EIM is enabled */
};

struct irq_desc;
struct hw_interrupt_type;
struct msi_desc;
/* Helper functions */
extern int pci_enable_msi(struct msi_info *msi, struct msi_desc **desc);
extern void pci_disable_msi(struct msi_desc *desc);
extern int pci_prepare_msix(u16 seg, u8 bus, u8 devfn, bool off);
extern void pci_cleanup_msi(struct pci_dev *pdev);
extern int setup_msi_irq(struct irq_desc *, struct msi_desc *);
extern int __setup_msi_irq(struct irq_desc *, struct msi_desc *,
                           const struct hw_interrupt_type *);
extern void teardown_msi_irq(int irq);
extern int msi_free_vector(struct msi_desc *entry);
extern int pci_restore_msi_state(struct pci_dev *pdev);
extern int pci_reset_msix_state(struct pci_dev *pdev);

struct msi_desc {
	struct msi_attrib {
		__u8	type;		/* {0: unused, 5h:MSI, 11h:MSI-X} */
		__u8	pos;		/* Location of the MSI capability */
		__u8	maskbit	: 1;	/* mask/pending bit supported ?   */
		__u8	is_64	: 1;	/* Address size: 0=32bit 1=64bit  */
		__u8	host_masked : 1;
		__u8	guest_masked : 1;
		__u16	entry_nr;	/* specific enabled entry 	  */
	} msi_attrib;

	bool irte_initialized;
	uint8_t gvec;			/* guest vector. valid when pi_desc isn't NULL */
	const struct pi_desc *pi_desc;	/* pointer to posted descriptor */

	struct list_head list;

	union {
		void __iomem *mask_base;/* va for the entry in mask table */
		struct {
			unsigned int nvec;/* number of vectors            */
			unsigned int mpos;/* location of mask register    */
		} msi;
		unsigned int hpet_id;   /* HPET (dev is NULL)             */
	};
	struct pci_dev *dev;
	int irq;
	int remap_index;		/* index in interrupt remapping table */

	struct msi_msg msg;		/* Last set MSI message */
};

/*
 * Values stored into msi_desc.msi_attrib.pos for non-PCI devices
 * (msi_desc.msi_attrib.type is zero):
 */
#define MSI_TYPE_UNKNOWN 0
#define MSI_TYPE_HPET    1
#define MSI_TYPE_IOMMU   2

int msi_maskable_irq(const struct msi_desc *);
int msi_free_irq(struct msi_desc *entry);

/*
 * Assume the maximum number of hot plug slots supported by the system is about
 * ten. The worstcase is that each of these slots is hot-added with a device,
 * which has two MSI/MSI-X capable functions. To avoid any MSI-X driver, which
 * attempts to request all available vectors, NR_HP_RESERVED_VECTORS is defined
 * as below to ensure at least one message is assigned to each detected MSI/
 * MSI-X device function.
 */
#define NR_HP_RESERVED_VECTORS 	20

/*
 * MSI Defined Data Structures
 */

struct __packed msg_data {
#if defined(__LITTLE_ENDIAN_BITFIELD)
	__u32	vector		:  8;
	__u32	delivery_mode	:  3;	/* 000b: FIXED | 001b: lowest prior */
	__u32	reserved_1	:  3;
	__u32	level		:  1;	/* 0: deassert | 1: assert */
	__u32	trigger		:  1;	/* 0: edge | 1: level */
	__u32	reserved_2	: 16;
#elif defined(__BIG_ENDIAN_BITFIELD)
	__u32	reserved_2	: 16;
	__u32	trigger		:  1;	/* 0: edge | 1: level */
	__u32	level		:  1;	/* 0: deassert | 1: assert */
	__u32	reserved_1	:  3;
	__u32	delivery_mode	:  3;	/* 000b: FIXED | 001b: lowest prior */
	__u32	vector		:  8;
#else
#error "Bitfield endianness not defined! Check your byteorder.h"
#endif
};

struct __packed msg_address {
	union {
		struct {
#if defined(__LITTLE_ENDIAN_BITFIELD)
			__u32	reserved_1	:  2;
			__u32	dest_mode	:  1;	/*0:physic | 1:logic */
			__u32	redirection_hint:  1;  	/*0: dedicated CPU
							  1: lowest priority */
			__u32	reserved_2	:  4;
 			__u32	dest_id		: 24;	/* Destination ID */
#elif defined(__BIG_ENDIAN_BITFIELD)
 			__u32	dest_id		: 24;	/* Destination ID */
			__u32	reserved_2	:  4;
			__u32	redirection_hint:  1;  	/*0: dedicated CPU
							  1: lowest priority */
			__u32	dest_mode	:  1;	/*0:physic | 1:logic */
			__u32	reserved_1	:  2;
#else
#error "Bitfield endianness not defined! Check your byteorder.h"
#endif
      		}u;
       		__u32  value;
	}lo_address;
	__u32 	hi_address;
};

#define MAX_MSIX_TABLE_ENTRIES  (PCI_MSIX_FLAGS_QSIZE + 1)
#define MAX_MSIX_TABLE_PAGES    PFN_UP(MAX_MSIX_TABLE_ENTRIES * \
                                       PCI_MSIX_ENTRY_SIZE + \
                                       (~PCI_MSIX_BIRMASK & (PAGE_SIZE - 1)))

struct arch_msix {
    unsigned int nr_entries, used_entries;
    struct {
        unsigned long first, last;
    } table, pba;
    int table_refcnt[MAX_MSIX_TABLE_PAGES];
    int table_idx[MAX_MSIX_TABLE_PAGES];
    spinlock_t table_lock;
    bool host_maskall, guest_maskall;
    domid_t warned;
};

void early_msi_init(void);
void msi_compose_msg(unsigned vector, const cpumask_t *mask,
                     struct msi_msg *msg);
void __msi_set_enable(u16 seg, u8 bus, u8 slot, u8 func, int pos, int enable);
void cf_check mask_msi_irq(struct irq_desc *);
void cf_check unmask_msi_irq(struct irq_desc *);
void guest_mask_msi_irq(struct irq_desc *, bool mask);
void cf_check ack_nonmaskable_msi_irq(struct irq_desc *);
void cf_check set_msi_affinity(struct irq_desc *, const cpumask_t *);

#endif /* __ASM_MSI_H */
