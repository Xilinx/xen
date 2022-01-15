/*
 * Copyright (c) 2006, Intel Corporation.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program; If not, see <http://www.gnu.org/licenses/>.
 *
 * Copyright (C) Allen Kay <allen.m.kay@intel.com>
 */

#ifndef _IOMMU_H_
#define _IOMMU_H_

#include <xen/mm-frame.h>
#include <xen/init.h>
#include <xen/page-defs.h>
#include <xen/pci.h>
#include <xen/spinlock.h>
#include <public/domctl.h>
#include <public/hvm/ioreq.h>
#include <asm/device.h>

TYPE_SAFE(uint64_t, dfn);
#define PRI_dfn     PRIx64
#define INVALID_DFN _dfn(~0ULL)

#ifndef dfn_t
#define dfn_t /* Grep fodder: dfn_t, _dfn() and dfn_x() are defined above */
#define _dfn
#define dfn_x
#undef dfn_t
#undef _dfn
#undef dfn_x
#endif

static inline dfn_t dfn_add(dfn_t dfn, unsigned long i)
{
    return _dfn(dfn_x(dfn) + i);
}

static inline bool_t dfn_eq(dfn_t x, dfn_t y)
{
    return dfn_x(x) == dfn_x(y);
}

#ifdef CONFIG_HAS_PASSTHROUGH
extern bool_t iommu_enable, iommu_enabled;
extern bool force_iommu, iommu_verbose;
/* Boolean except for the specific purposes of drivers/passthrough/iommu.c. */
extern uint8_t iommu_quarantine;
#else
#define iommu_enabled false
#endif

#ifdef CONFIG_X86
extern enum __packed iommu_intremap {
   /*
    * In order to allow traditional boolean uses of the iommu_intremap
    * variable, the "off" value has to come first (yielding a value of zero).
    */
   iommu_intremap_off,
   /*
    * Interrupt remapping enabled, but only able to generate interrupts
    * with an 8-bit APIC ID.
    */
   iommu_intremap_restricted,
   iommu_intremap_full,
} iommu_intremap;
extern bool iommu_igfx, iommu_qinval, iommu_snoop;
#else
# define iommu_intremap false
# define iommu_snoop false
#endif

#if defined(CONFIG_X86) && defined(CONFIG_HVM)
extern bool iommu_intpost;
#else
# define iommu_intpost false
#endif

#if defined(CONFIG_IOMMU_FORCE_PT_SHARE)
#define iommu_hap_pt_share true
#elif defined(CONFIG_HVM)
extern bool iommu_hap_pt_share;
#else
#define iommu_hap_pt_share false
#endif

static inline void clear_iommu_hap_pt_share(void)
{
#ifndef iommu_hap_pt_share
    iommu_hap_pt_share = false;
#elif iommu_hap_pt_share
    ASSERT_UNREACHABLE();
#endif
}

extern bool_t iommu_debug;
extern bool_t amd_iommu_perdev_intremap;

extern bool iommu_hwdom_strict, iommu_hwdom_passthrough, iommu_hwdom_inclusive;
extern int8_t iommu_hwdom_reserved;

extern unsigned int iommu_dev_iotlb_timeout;

int iommu_setup(void);
int iommu_hardware_setup(void);

int iommu_domain_init(struct domain *d, unsigned int opts);
void iommu_hwdom_init(struct domain *d);
void iommu_domain_destroy(struct domain *d);

void arch_iommu_domain_destroy(struct domain *d);
int arch_iommu_domain_init(struct domain *d);
void arch_iommu_check_autotranslated_hwdom(struct domain *d);
void arch_iommu_hwdom_init(struct domain *d);

/*
 * The following flags are passed to map operations and passed by lookup
 * operations.
 */
#define _IOMMUF_readable 0
#define IOMMUF_readable  (1u<<_IOMMUF_readable)
#define _IOMMUF_writable 1
#define IOMMUF_writable  (1u<<_IOMMUF_writable)

/*
 * flush_flags:
 *
 * IOMMU_FLUSHF_added -> A new 'present' PTE has been inserted.
 * IOMMU_FLUSHF_modified -> An existing 'present' PTE has been modified
 *                          (whether the new PTE value is 'present' or not).
 *
 * These flags are passed back from map/unmap operations and passed into
 * flush operations.
 */
enum
{
    _IOMMU_FLUSHF_added,
    _IOMMU_FLUSHF_modified,
};
#define IOMMU_FLUSHF_added (1u << _IOMMU_FLUSHF_added)
#define IOMMU_FLUSHF_modified (1u << _IOMMU_FLUSHF_modified)

int __must_check iommu_map(struct domain *d, dfn_t dfn, mfn_t mfn,
                           unsigned long page_count, unsigned int flags,
                           unsigned int *flush_flags);
int __must_check iommu_unmap(struct domain *d, dfn_t dfn,
                             unsigned long page_count,
                             unsigned int *flush_flags);

int __must_check iommu_legacy_map(struct domain *d, dfn_t dfn, mfn_t mfn,
                                  unsigned long page_count,
                                  unsigned int flags);
int __must_check iommu_legacy_unmap(struct domain *d, dfn_t dfn,
                                    unsigned long page_count);

int __must_check iommu_lookup_page(struct domain *d, dfn_t dfn, mfn_t *mfn,
                                   unsigned int *flags);

int __must_check iommu_iotlb_flush(struct domain *d, dfn_t dfn,
                                   unsigned long page_count,
                                   unsigned int flush_flags);
int __must_check iommu_iotlb_flush_all(struct domain *d,
                                       unsigned int flush_flags);

enum iommu_feature
{
    IOMMU_FEAT_COHERENT_WALK,
    IOMMU_FEAT_count
};

bool_t iommu_has_feature(struct domain *d, enum iommu_feature feature);

#ifdef CONFIG_HAS_PCI
struct pirq;
int hvm_do_IRQ_dpci(struct domain *, struct pirq *);
int pt_irq_create_bind(struct domain *, const struct xen_domctl_bind_pt_irq *);
int pt_irq_destroy_bind(struct domain *, const struct xen_domctl_bind_pt_irq *);

void hvm_dpci_isairq_eoi(struct domain *d, unsigned int isairq);
struct hvm_irq_dpci *domain_get_irq_dpci(const struct domain *);
void free_hvm_irq_dpci(struct hvm_irq_dpci *dpci);

struct msi_desc;
struct msi_msg;

#define PT_IRQ_TIME_OUT MILLISECS(8)
#endif /* HAS_PCI */

#ifdef CONFIG_HAS_DEVICE_TREE
#include <xen/device_tree.h>

int iommu_assign_dt_device(struct domain *d, struct dt_device_node *dev);
int iommu_deassign_dt_device(struct domain *d, struct dt_device_node *dev);
int iommu_dt_domain_init(struct domain *d);
int iommu_release_dt_devices(struct domain *d);

/*
 * Helper to add master device to the IOMMU using generic IOMMU DT bindings.
 *
 * Return values:
 *  0 : device is protected by an IOMMU
 * <0 : device is not protected by an IOMMU, but must be (error condition)
 * >0 : device doesn't need to be protected by an IOMMU
 *      (IOMMU is not enabled/present or device is not connected to it).
 */
int iommu_add_dt_device(struct dt_device_node *np);

int iommu_remove_dt_device(struct dt_device_node *np);

int iommu_do_dt_domctl(struct xen_domctl *, struct domain *,
                       XEN_GUEST_HANDLE_PARAM(xen_domctl_t));

#endif /* HAS_DEVICE_TREE */

struct page_info;

/*
 * Any non-zero value returned from callbacks of this type will cause the
 * function the callback was handed to terminate its iteration. Assigning
 * meaning of these non-zero values is left to the top level caller /
 * callback pair.
 */
typedef int iommu_grdm_t(xen_pfn_t start, xen_ulong_t nr, u32 id, void *ctxt);

struct iommu_ops {
    int (*init)(struct domain *d);
    void (*hwdom_init)(struct domain *d);
    int (*quarantine_init)(struct domain *d);
    int (*add_device)(u8 devfn, device_t *dev);
    int (*enable_device)(device_t *dev);
    int (*remove_device)(u8 devfn, device_t *dev);
    int (*assign_device)(struct domain *, u8 devfn, device_t *dev, u32 flag);
    int (*reassign_device)(struct domain *s, struct domain *t,
                           u8 devfn, device_t *dev);
#ifdef CONFIG_HAS_PCI
    int (*get_device_group_id)(u16 seg, u8 bus, u8 devfn);
#endif /* HAS_PCI */

    void (*teardown)(struct domain *d);

    /*
     * This block of operations must be appropriately locked against each
     * other by the caller in order to have meaningful results.
     */
    int __must_check (*map_page)(struct domain *d, dfn_t dfn, mfn_t mfn,
                                 unsigned int flags,
                                 unsigned int *flush_flags);
    int __must_check (*unmap_page)(struct domain *d, dfn_t dfn,
                                   unsigned int *flush_flags);
    int __must_check (*lookup_page)(struct domain *d, dfn_t dfn, mfn_t *mfn,
                                    unsigned int *flags);

#ifdef CONFIG_X86
    int (*enable_x2apic)(void);
    void (*disable_x2apic)(void);

    void (*update_ire_from_apic)(unsigned int apic, unsigned int reg, unsigned int value);
    unsigned int (*read_apic_from_ire)(unsigned int apic, unsigned int reg);

    int (*setup_hpet_msi)(struct msi_desc *);

    int (*adjust_irq_affinities)(void);
    void (*sync_cache)(const void *addr, unsigned int size);
    void (*clear_root_pgtable)(struct domain *d);
    int (*update_ire_from_msi)(struct msi_desc *msi_desc, struct msi_msg *msg);
#endif /* CONFIG_X86 */

    int __must_check (*suspend)(void);
    void (*resume)(void);
    void (*crash_shutdown)(void);
    int __must_check (*iotlb_flush)(struct domain *d, dfn_t dfn,
                                    unsigned long page_count,
                                    unsigned int flush_flags);
    int __must_check (*iotlb_flush_all)(struct domain *d);
    int (*get_reserved_device_memory)(iommu_grdm_t *, void *);
    void (*dump_page_tables)(struct domain *d);

#ifdef CONFIG_HAS_DEVICE_TREE
    /*
     * All IOMMU drivers which support generic IOMMU DT bindings should use
     * this callback. This is a way for the framework to provide the driver
     * with DT IOMMU specifier which describes the IOMMU master interfaces of
     * that device (device IDs, etc).
     */
    int (*dt_xlate)(device_t *dev, const struct dt_phandle_args *args);
#endif
};

#include <asm/iommu.h>

#ifndef iommu_call
# define iommu_call(ops, fn, args...) ((ops)->fn(args))
# define iommu_vcall iommu_call
#endif

struct domain_iommu {
    struct arch_iommu arch;

    /* iommu_ops */
    const struct iommu_ops *platform_ops;

#ifdef CONFIG_HAS_DEVICE_TREE
    /* List of DT devices assigned to this domain */
    struct list_head dt_devices;
#endif

#ifdef CONFIG_NUMA
    /* NUMA node to do IOMMU related allocations against. */
    nodeid_t node;
#endif

    /* Features supported by the IOMMU */
    DECLARE_BITMAP(features, IOMMU_FEAT_count);

    /* Does the guest share HAP mapping with the IOMMU? */
    bool hap_pt_share;

    /*
     * Does the guest require mappings to be synchronized, to maintain
     * the default dfn == pfn map? (See comment on dfn at the top of
     * include/xen/mm.h). Note that hap_pt_share == false does not
     * necessarily imply this is true.
     */
    bool need_sync;
};

#define dom_iommu(d)              (&(d)->iommu)
#define iommu_set_feature(d, f)   set_bit(f, dom_iommu(d)->features)
#define iommu_clear_feature(d, f) clear_bit(f, dom_iommu(d)->features)

/* Are we using the domain P2M table as its IOMMU pagetable? */
#define iommu_use_hap_pt(d)       (dom_iommu(d)->hap_pt_share)

/* Does the IOMMU pagetable need to be kept synchronized with the P2M */
#ifdef CONFIG_HAS_PASSTHROUGH
#define need_iommu_pt_sync(d)     (dom_iommu(d)->need_sync)
#else
#define need_iommu_pt_sync(d)     ({ (void)(d); false; })
#endif

int __must_check iommu_suspend(void);
void iommu_resume(void);
void iommu_crash_shutdown(void);
int iommu_get_reserved_device_memory(iommu_grdm_t *, void *);

#ifdef CONFIG_HAS_PCI
int iommu_do_pci_domctl(struct xen_domctl *, struct domain *d,
                        XEN_GUEST_HANDLE_PARAM(xen_domctl_t));
#endif

int iommu_do_domctl(struct xen_domctl *, struct domain *d,
                    XEN_GUEST_HANDLE_PARAM(xen_domctl_t));

void iommu_dev_iotlb_flush_timeout(struct domain *d, struct pci_dev *pdev);

/*
 * The purpose of the iommu_dont_flush_iotlb optional cpu flag is to
 * avoid unecessary iotlb_flush in the low level IOMMU code.
 *
 * iommu_map_page/iommu_unmap_page must flush the iotlb but somethimes
 * this operation can be really expensive. This flag will be set by the
 * caller to notify the low level IOMMU code to avoid the iotlb flushes.
 * iommu_iotlb_flush/iommu_iotlb_flush_all will be explicitly called by
 * the caller.
 */
DECLARE_PER_CPU(bool_t, iommu_dont_flush_iotlb);

extern struct spinlock iommu_pt_cleanup_lock;
extern struct page_list_head iommu_pt_cleanup_list;

bool arch_iommu_use_permitted(const struct domain *d);

#ifdef CONFIG_X86
static inline int iommu_update_ire_from_msi(
    struct msi_desc *msi_desc, struct msi_msg *msg)
{
    return iommu_intremap
           ? iommu_call(&iommu_ops, update_ire_from_msi, msi_desc, msg) : 0;
}
#endif

#endif /* _IOMMU_H_ */

/*
 * Local variables:
 * mode: C
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * End:
 */
