#ifndef _XEN_P2M_H
#define _XEN_P2M_H

#include <xen/mm.h>
#include <xen/radix-tree.h>
#include <xen/rwlock.h>
#include <xen/mem_access.h>

#include <asm/current.h>
#include <asm/hsr.h>

#define paddr_bits PADDR_BITS

/* Holds the bit size of IPAs in p2m tables.  */
extern unsigned int p2m_ipa_bits;

#ifdef CONFIG_ARM_64
extern unsigned int p2m_root_order;
extern unsigned int p2m_root_level;
#define P2M_ROOT_ORDER    p2m_root_order
#define P2M_ROOT_LEVEL p2m_root_level
#else
/* First level P2M is always 2 consecutive pages */
#define P2M_ROOT_ORDER    1
#define P2M_ROOT_LEVEL 1
#endif

#define MAX_VMID_8_BIT  (1UL << 8)
#define MAX_VMID_16_BIT (1UL << 16)

#define INVALID_VMID 0 /* VMID 0 is reserved */

#ifdef CONFIG_ARM_64
extern unsigned int __read_mostly max_vmid;
/* VMID is by default 8 bit width on AArch64 */
#define MAX_VMID       max_vmid
#else
/* VMID is always 8 bit width on AArch32 */
#define MAX_VMID       MAX_VMID_8_BIT
#endif

extern unsigned long *vmid_mask;
extern spinlock_t vmid_alloc_lock;

struct domain;

extern void memory_type_changed(struct domain *);

/* Per-p2m-table state */
struct p2m_domain {
    /*
     * Lock that protects updates to the p2m.
     */
    rwlock_t lock;

    /* Pages used to construct the p2m */
    struct page_list_head pages;

    /* The root of the p2m tree. May be concatenated */
    struct page_info *root;

    /* Current VMID in use */
    uint16_t vmid;

    /* Current Translation Table Base Register for the p2m */
    uint64_t vttbr;

#ifdef CONFIG_HAS_MPU
    /* Current Virtualization System Control Register for the p2m */
    uint64_t vsctlr;
#endif

    /* Highest guest frame that's ever been mapped in the p2m */
    gfn_t max_mapped_gfn;

    /*
     * Lowest mapped gfn in the p2m. When releasing mapped gfn's in a
     * preemptible manner this is update to track recall where to
     * resume the search. Apart from during teardown this can only
     * decrease. */
    gfn_t lowest_mapped_gfn;

    /* Indicate if it is required to clean the cache when writing an entry */
    bool clean_pte;

    /*
     * P2M updates may required TLBs to be flushed (invalidated).
     *
     * Flushes may be deferred by setting 'need_flush' and then flushing
     * when the p2m write lock is released.
     *
     * If an immediate flush is required (e.g, if a super page is
     * shattered), call p2m_tlb_flush_sync().
     */
    bool need_flush;

    /* Gather some statistics for information purposes only */
    struct {
        /* Number of mappings at each p2m tree level */
        unsigned long mappings[4];
        /* Number of times we have shattered a mapping
         * at each p2m tree level. */
        unsigned long shattered[4];
    } stats;

    /*
     * If true, and an access fault comes in and there is no vm_event listener,
     * pause domain. Otherwise, remove access restrictions.
     */
    bool access_required;

    /* Defines if mem_access is in use for the domain. */
    bool mem_access_enabled;

    /*
     * Default P2M access type for each page in the the domain: new pages,
     * swapped in pages, cleared pages, and pages that are ambiguously
     * retyped get this access type. See definition of p2m_access_t.
     */
    p2m_access_t default_access;

    /*
     * Radix tree to store the p2m_access_t settings as the pte's don't have
     * enough available bits to store this information.
     */
    struct radix_tree_root mem_access_settings;

    /* back pointer to domain */
    struct domain *domain;

    /* Keeping track on which CPU this p2m was used and for which vCPU */
    uint8_t last_vcpu_ran[NR_CPUS];

#ifdef CONFIG_HAS_MPU
    /* Number of MPU protection regions in P2M MPU memory mapping table. */
    unsigned int nr_regions;
#endif
};

/*
 * List of possible type for each page in the p2m entry.
 * The number of available bit per page in the pte for this purpose is 4 bits.
 * So it's possible to only have 16 fields. If we run out of value in the
 * future, it's possible to use higher value for pseudo-type and don't store
 * them in the p2m entry.
 */
typedef enum {
    p2m_invalid = 0,    /* Nothing mapped here */
    p2m_ram_rw,         /* Normal read/write guest RAM */
    p2m_ram_ro,         /* Read-only; writes are silently dropped */
    p2m_mmio_direct_dev,/* Read/write mapping of genuine Device MMIO area */
    p2m_mmio_direct_nc, /* Read/write mapping of genuine MMIO area non-cacheable */
    p2m_mmio_direct_c,  /* Read/write mapping of genuine MMIO area cacheable */
    p2m_map_foreign_rw, /* Read/write RAM pages from foreign domain */
    p2m_map_foreign_ro, /* Read-only RAM pages from foreign domain */
    p2m_grant_map_rw,   /* Read/write grant mapping */
    p2m_grant_map_ro,   /* Read-only grant mapping */
    /* The types below are only used to decide the page attribute in the P2M */
    p2m_iommu_map_rw,   /* Read/write iommu mapping */
    p2m_iommu_map_ro,   /* Read-only iommu mapping */
#ifdef CONFIG_HAS_MPU
    p2m_dev_rw,         /* Device read/write memory */
#endif
    p2m_max_real_type,  /* Types after this won't be store in the p2m */
} p2m_type_t;

/* We use bitmaps and mask to handle groups of types */
#define p2m_to_mask(_t) (1UL << (_t))

/* RAM types, which map to real machine frames */
#define P2M_RAM_TYPES (p2m_to_mask(p2m_ram_rw) |        \
                       p2m_to_mask(p2m_ram_ro))

/* Grant mapping types, which map to a real frame in another VM */
#define P2M_GRANT_TYPES (p2m_to_mask(p2m_grant_map_rw) |  \
                         p2m_to_mask(p2m_grant_map_ro))

/* Foreign mappings types */
#define P2M_FOREIGN_TYPES (p2m_to_mask(p2m_map_foreign_rw) | \
                           p2m_to_mask(p2m_map_foreign_ro))

/* Useful predicates */
#define p2m_is_ram(_t) (p2m_to_mask(_t) & P2M_RAM_TYPES)
#define p2m_is_foreign(_t) (p2m_to_mask(_t) & P2M_FOREIGN_TYPES)
#define p2m_is_any_ram(_t) (p2m_to_mask(_t) &                   \
                            (P2M_RAM_TYPES | P2M_GRANT_TYPES |  \
                             P2M_FOREIGN_TYPES))

/* All common type definitions should live ahead of this inclusion. */
#ifdef _XEN_P2M_COMMON_H
# error "xen/p2m-common.h should not be included directly"
#endif
#include <xen/p2m-common.h>

static inline bool arch_acquire_resource_check(struct domain *d)
{
    /*
     * The reference counting of foreign entries in set_foreign_p2m_entry()
     * is supported on Arm.
     */
    return true;
}

static inline
void p2m_altp2m_check(struct vcpu *v, uint16_t idx)
{
    /* Not supported on ARM. */
}

/*
 * Helper to restrict "p2m_ipa_bits" according the external entity
 * (e.g. IOMMU) requirements.
 *
 * Each corresponding driver should report the maximum IPA bits
 * (Stage-2 input size) it can support.
 */
void p2m_restrict_ipa_bits(unsigned int ipa_bits);

#ifdef CONFIG_HAS_MPU
register_t get_default_vtcr_flags(void);
#endif

/* Second stage paging setup, to be called on all CPUs */
void setup_virt_paging(void);

/* VMID-related common functions. */
void p2m_vmid_allocator_init(void);
int p2m_alloc_vmid(struct domain *d);

/* Init the datastructures for later use by the p2m code */
int p2m_init(struct domain *d);

/*
 * The P2M resources are freed in two parts:
 *  - p2m_teardown() will be called preemptively when relinquish the
 *    resources, in which case it will free large resources (e.g. intermediate
 *    page-tables) that requires preemption.
 *  - p2m_final_teardown() will be called when domain struct is been
 *    freed. This *cannot* be preempted and therefore one small
 *    resources should be freed here.
 *  Note that p2m_final_teardown() will also call p2m_teardown(), to properly
 *  free the P2M when failures happen in the domain creation with P2M pages
 *  already in use. In this case p2m_teardown() is called non-preemptively and
 *  p2m_teardown() will always return 0.
 */
int p2m_teardown(struct domain *d, bool allow_preemption);
void p2m_final_teardown(struct domain *d);

/*
 * Remove mapping refcount on each mapping page in the p2m
 *
 * TODO: For the moment only foreign mappings are handled
 */
int relinquish_p2m_mapping(struct domain *d);

/* Context switch */
void p2m_save_state(struct vcpu *p);
void p2m_restore_state(struct vcpu *n);

/* Print debugging/statistial info about a domain's p2m */
void p2m_dump_info(struct domain *d);

struct page_info *p2m_alloc_page(struct domain *d);
void p2m_free_page(struct domain *d, struct page_info *pg);
int p2m_set_allocation(struct domain *d, unsigned long pages, bool *preempted);
int p2m_teardown_allocation(struct domain *d);

static inline void p2m_write_lock(struct p2m_domain *p2m)
{
    write_lock(&p2m->lock);
}

void p2m_write_unlock(struct p2m_domain *p2m);

static inline void p2m_read_lock(struct p2m_domain *p2m)
{
    read_lock(&p2m->lock);
}

static inline void p2m_read_unlock(struct p2m_domain *p2m)
{
    read_unlock(&p2m->lock);
}

static inline int p2m_is_locked(struct p2m_domain *p2m)
{
    return rw_is_locked(&p2m->lock);
}

static inline int p2m_is_write_locked(struct p2m_domain *p2m)
{
    return rw_is_write_locked(&p2m->lock);
}

void p2m_tlb_flush_sync(struct p2m_domain *p2m);

/* Look up the MFN corresponding to a domain's GFN. */
mfn_t p2m_lookup(struct domain *d, gfn_t gfn, p2m_type_t *t);

/*
 * Get details of a given gfn.
 * The P2M lock should be taken by the caller.
 */
mfn_t p2m_get_entry(struct p2m_domain *p2m, gfn_t gfn,
                    p2m_type_t *t, p2m_access_t *a,
                    unsigned int *page_order,
                    bool *valid);

/*
 * Direct set a p2m entry: only for use by the P2M code.
 * The P2M write lock should be taken.
 * TODO: Add a check in __p2m_set_entry() to avoid creating a mapping in
 * arch_domain_create() that requires p2m_put_l3_page() to be called.
 */
int p2m_set_entry(struct p2m_domain *p2m,
                  gfn_t sgfn,
                  unsigned long nr,
                  mfn_t smfn,
                  p2m_type_t t,
                  p2m_access_t a);

bool p2m_resolve_translation_fault(struct domain *d, gfn_t gfn);

void p2m_invalidate_root(struct p2m_domain *p2m);

/*
 * Clean & invalidate caches corresponding to a region [start,end) of guest
 * address space.
 *
 * start will get updated if the function is preempted.
 */
int p2m_cache_flush_range(struct domain *d, gfn_t *pstart, gfn_t end);

void p2m_set_way_flush(struct vcpu *v, struct cpu_user_regs *regs,
                       const union hsr hsr);

void p2m_toggle_cache(struct vcpu *v, bool was_enabled);

void p2m_flush_vm(struct vcpu *v);

/*
 * Map a region in the guest p2m with a specific p2m type.
 * The memory attributes will be derived from the p2m type.
 */
int map_regions_p2mt(struct domain *d,
                     gfn_t gfn,
                     unsigned long nr,
                     mfn_t mfn,
                     p2m_type_t p2mt);

int unmap_regions_p2mt(struct domain *d,
                       gfn_t gfn,
                       unsigned long nr,
                       mfn_t mfn);

int map_dev_mmio_page(struct domain *d, gfn_t gfn, mfn_t mfn);

int p2m_insert_mapping(struct domain *d, gfn_t start_gfn, unsigned long nr,
                       mfn_t mfn, p2m_type_t t);

int guest_physmap_add_entry(struct domain *d,
                            gfn_t gfn,
                            mfn_t mfn,
                            unsigned long page_order,
                            p2m_type_t t);

/* Untyped version for RAM only, for compatibility */
static inline int __must_check
guest_physmap_add_page(struct domain *d, gfn_t gfn, mfn_t mfn,
                       unsigned int page_order)
{
    return guest_physmap_add_entry(d, gfn, mfn, page_order, p2m_ram_rw);
}

static inline int guest_physmap_add_pages(struct domain *d,
                                          gfn_t gfn,
                                          mfn_t mfn,
                                          unsigned int nr_pages)
{
    return p2m_insert_mapping(d, gfn, nr_pages, mfn, p2m_ram_rw);
}

mfn_t gfn_to_mfn(struct domain *d, gfn_t gfn);

/* Look up a GFN and take a reference count on the backing page. */
typedef unsigned int p2m_query_t;
#define P2M_ALLOC    (1u<<0)   /* Populate PoD and paged-out entries */
#define P2M_UNSHARE  (1u<<1)   /* Break CoW sharing */

#ifdef CONFIG_HAS_MPU
struct page_info *p2m_get_region_from_gfns(struct domain *d, gfn_t gfn,
                                           unsigned long nr_gfns, p2m_type_t *t);

static inline struct page_info *get_region_from_gfns(struct domain *d,
                                                     unsigned long gfn,
                                                     unsigned long nr_gfns,
                                                     p2m_type_t *t)
{
    /* TODO: Special case for DOMID_XEN. */
    ASSERT(d != dom_xen);

    return p2m_get_region_from_gfns(d, _gfn(gfn), nr_gfns, t);
}
#endif

struct page_info *p2m_get_page_from_gfn(struct domain *d, gfn_t gfn,
                                        p2m_type_t *t);

static inline struct page_info *get_page_from_gfn(
    struct domain *d, unsigned long gfn, p2m_type_t *t, p2m_query_t q)
{
    mfn_t mfn;
    p2m_type_t _t;
    struct page_info *page;

    /*
     * Special case for DOMID_XEN as it is the only domain so far that is
     * not auto-translated.
     */
    if ( likely(d != dom_xen) )
        return p2m_get_page_from_gfn(d, _gfn(gfn), t);

    if ( !t )
        t = &_t;

    *t = p2m_invalid;

    /*
     * DOMID_XEN sees 1-1 RAM. The p2m_type is based on the type of the
     * page.
     */
    mfn = _mfn(gfn);
    page = mfn_to_page(mfn);

    if ( !mfn_valid(mfn) || !get_page(page, d) )
        return NULL;

    if ( page->u.inuse.type_info & PGT_writable_page )
        *t = p2m_ram_rw;
    else
        *t = p2m_ram_ro;

    return page;
}

int get_page_type(struct page_info *page, unsigned long type);
bool is_iomem_page(mfn_t mfn);
static inline int get_page_and_type(struct page_info *page,
                                    struct domain *domain,
                                    unsigned long type)
{
    int rc = get_page(page, domain);

    if ( likely(rc) && unlikely(!get_page_type(page, type)) )
    {
        put_page(page);
        rc = 0;
    }

    return rc;
}

/* get host p2m table */
#define p2m_get_hostp2m(d) (&(d)->arch.p2m)

static inline bool p2m_vm_event_sanity_check(struct domain *d)
{
    return true;
}

/*
 * Return the start of the next mapping based on the order of the
 * current one.
 */
static inline gfn_t gfn_next_boundary(gfn_t gfn, unsigned int order)
{
    /*
     * The order corresponds to the order of the mapping (or invalid
     * range) in the page table. So we need to align the GFN before
     * incrementing.
     */
    gfn = _gfn(gfn_x(gfn) & ~((1UL << order) - 1));

    return gfn_add(gfn, 1UL << order);
}

/*
 * A vCPU has cache enabled only when the MMU is enabled and data cache
 * is enabled.
 */
static inline bool vcpu_has_cache_enabled(struct vcpu *v)
{
    const register_t mask = SCTLR_Axx_ELx_C | SCTLR_Axx_ELx_M;

    /* Only works with the current vCPU */
    ASSERT(current == v);

    return (READ_SYSREG(SCTLR_EL1) & mask) == mask;
}

#endif /* _XEN_P2M_H */

/*
 * Local variables:
 * mode: C
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * End:
 */
