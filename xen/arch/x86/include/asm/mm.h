
#ifndef __ASM_X86_MM_H__
#define __ASM_X86_MM_H__

#include <xen/list.h>
#include <xen/spinlock.h>
#include <xen/rwlock.h>
#include <asm/io.h>
#include <asm/page.h>
#include <asm/uaccess.h>
#include <asm/x86_emulate.h>

/*
 * Per-page-frame information.
 *
 * Every architecture must ensure the following:
 *  1. 'struct page_info' contains a 'struct page_list_entry list'.
 *  2. Provide a PFN_ORDER() macro for accessing the order of a free page.
 */
#define PFN_ORDER(_pfn) ((_pfn)->v.free.order)

#define PG_shift(idx)   (BITS_PER_LONG - (idx))
#define PG_mask(x, idx) (x ## UL << PG_shift(idx))

 /* The following page types are MUTUALLY EXCLUSIVE. */
#define PGT_none          PG_mask(0, 3)  /* no special uses of this page   */
#define PGT_l1_page_table PG_mask(1, 3)  /* using as an L1 page table?     */
#define PGT_l2_page_table PG_mask(2, 3)  /* using as an L2 page table?     */
#define PGT_l3_page_table PG_mask(3, 3)  /* using as an L3 page table?     */
#define PGT_l4_page_table PG_mask(4, 3)  /* using as an L4 page table?     */
#define PGT_seg_desc_page PG_mask(5, 3)  /* using this page in a GDT/LDT?  */
#define PGT_shared_page   PG_mask(6, 3)  /* CoW sharable page              */
#define PGT_writable_page PG_mask(7, 3)  /* has writable mappings?         */
#define PGT_type_mask     PG_mask(7, 3)  /* Bits 61-63.                    */

 /* Page is locked? */
#define _PGT_locked       PG_shift(4)
#define PGT_locked        PG_mask(1, 4)
 /* Owning guest has pinned this page to its current type? */
#define _PGT_pinned       PG_shift(5)
#define PGT_pinned        PG_mask(1, 5)
 /* Has this page been validated for use as its current type? */
#define _PGT_validated    PG_shift(6)
#define PGT_validated     PG_mask(1, 6)
 /* PAE only: is this an L2 page directory containing Xen-private mappings? */
#ifdef CONFIG_PV32
#define _PGT_pae_xen_l2   PG_shift(7)
#define PGT_pae_xen_l2    PG_mask(1, 7)
#else
#define PGT_pae_xen_l2    0
#endif
/* Has this page been *partially* validated for use as its current type? */
#define _PGT_partial      PG_shift(8)
#define PGT_partial       PG_mask(1, 8)

/* Has this page been mapped writeable with a non-coherent memory type? */
#define _PGT_non_coherent PG_shift(9)
#define PGT_non_coherent  PG_mask(1, 9)

 /* Count of uses of this frame as its current type. */
#define PGT_count_width   PG_shift(9)
#define PGT_count_mask    ((1UL<<PGT_count_width)-1)

/* Are the 'type mask' bits identical? */
#define PGT_type_equal(x, y) (!(((x) ^ (y)) & PGT_type_mask))

 /* Cleared when the owning guest 'frees' this page. */
#define _PGC_allocated    PG_shift(1)
#define PGC_allocated     PG_mask(1, 1)
 /* Page is Xen heap? */
#define _PGC_xen_heap     PG_shift(2)
#define PGC_xen_heap      PG_mask(1, 2)
 /* Set when is using a page as a page table */
#define _PGC_page_table   PG_shift(3)
#define PGC_page_table    PG_mask(1, 3)
/* Page is cache colored */
#define _PGC_colored      PG_shift(4)
#define PGC_colored       PG_mask(1, 4)
 /* Page is broken? */
#define _PGC_broken       PG_shift(5)
#define PGC_broken        PG_mask(1, 5)
 /* Mutually-exclusive page states: { inuse, offlining, offlined, free }. */
#define PGC_state           PG_mask(3, 6)
#define PGC_state_inuse     PG_mask(0, 6)
#define PGC_state_offlining PG_mask(1, 6)
#define PGC_state_offlined  PG_mask(2, 6)
#define PGC_state_free      PG_mask(3, 6)
#define page_state_is(pg, st) (((pg)->count_info&PGC_state) == PGC_state_##st)
/* Page is not reference counted */
#define _PGC_extra        PG_shift(7)
#define PGC_extra         PG_mask(1, 7)

/* Count of references to this frame. */
#define PGC_count_width   PG_shift(7)
#define PGC_count_mask    ((1UL<<PGC_count_width)-1)

/*
 * Page needs to be scrubbed. Since this bit can only be set on a page that is
 * free (i.e. in PGC_state_free) we can reuse PGC_allocated bit.
 */
#define _PGC_need_scrub   _PGC_allocated
#define PGC_need_scrub    PGC_allocated

#ifndef CONFIG_BIGMEM
/*
 * This definition is solely for the use in struct page_info (and
 * struct page_list_head), intended to allow easy adjustment once x86-64
 * wants to support more than 16TB.
 * 'unsigned long' should be used for MFNs everywhere else.
 */
#define __pdx_t unsigned int

#undef page_list_entry
struct page_list_entry
{
    __pdx_t next, prev;
};
#else
#define __pdx_t unsigned long
#endif

struct page_sharing_info;

struct page_info
{
    union {
        /* Each frame can be threaded onto a doubly-linked list.
         *
         * For unused shadow pages, a list of free shadow pages;
         * for multi-page shadows, links to the other pages in this shadow;
         * for pinnable shadows, if pinned, a list of all pinned shadows
         * (see sh_type_is_pinnable() for the definition of "pinnable"
         * shadow types).  N.B. a shadow may be both pinnable and multi-page.
         * In that case the pages are inserted in order in the list of
         * pinned shadows and walkers of that list must be prepared
         * to keep them all together during updates.
         */
        struct page_list_entry list;
        /* For non-pinnable single-page shadows, a higher entry that points
         * at us. */
        paddr_t up;

#ifdef CONFIG_MEM_SHARING
        /* For shared/sharable pages, we use a doubly-linked list
         * of all the {pfn,domain} pairs that map this page. We also include
         * an opaque handle, which is effectively a version, so that clients
         * of sharing share the version they expect to.
         * This list is allocated and freed when a page is shared/unshared.
         */
        struct page_sharing_info *sharing;
#endif
    };

    /* Reference count and various PGC_xxx flags and fields. */
    unsigned long count_info;

    /* Context-dependent fields follow... */
    union {

        /* Page is in use: ((count_info & PGC_count_mask) != 0). */
        struct {
            /* Type reference count and various PGT_xxx flags and fields. */
            unsigned long type_info;
        } inuse;

        /* Page is in use as a shadow: count_info == 0. */
        struct {
            unsigned long type:5;   /* What kind of shadow is this? */
            unsigned long pinned:1; /* Is the shadow pinned? */
            unsigned long head:1;   /* Is this the first page of the shadow? */
#define PAGE_SH_REFCOUNT_WIDTH (PGT_count_width - 7)
            unsigned long count:PAGE_SH_REFCOUNT_WIDTH; /* Reference count */
        } sh;

        /* Page is on a free list: ((count_info & PGC_count_mask) == 0). */
        union {
            struct {
                /*
                 * Index of the first *possibly* unscrubbed page in the buddy.
                 * One more bit than maximum possible order to accommodate
                 * INVALID_DIRTY_IDX.
                 */
#define INVALID_DIRTY_IDX ((1UL << (MAX_ORDER + 1)) - 1)
                unsigned int first_dirty;

                /* Do TLBs need flushing for safety before next page use? */
                bool need_tlbflush;

#define BUDDY_NOT_SCRUBBING    0
#define BUDDY_SCRUBBING        1
#define BUDDY_SCRUB_ABORT      2
                uint8_t  scrub_state;
            };

            unsigned long val;
        } free;

    } u;

    union {

        /* Page is in use, but not as a shadow. */
        struct {
            /* Owner of this page (zero if page is anonymous). */
            __pdx_t _domain;
        } inuse;

        /* Page is in use as a shadow. */
        struct {
            /* GMFN of guest page we're a shadow of. */
            __pdx_t back;
        } sh;

        /* Page is on a free list. */
        struct {
            /* Order-size of the free chunk this page is the head of. */
            unsigned int order;
        } free;

    } v;

    union {
        /*
         * Timestamp from 'TLB clock', used to avoid extra safety flushes.
         * Only valid for: a) free pages, and b) pages with zero type count
         * (except page table pages when the guest is in shadow mode).
         */
        u32 tlbflush_timestamp;

        /*
         * When PGT_partial is true then the first two fields are valid and
         * indicate that PTEs in the range [0, @nr_validated_ptes) have been
         * validated. An extra page reference must be acquired (or not dropped)
         * whenever PGT_partial gets set, and it must be dropped when the flag
         * gets cleared. This is so that a get() leaving a page in partially
         * validated state (where the caller would drop the reference acquired
         * due to the getting of the type [apparently] failing [-ERESTART])
         * would not accidentally result in a page left with zero general
         * reference count, but non-zero type reference count (possible when
         * the partial get() is followed immediately by domain destruction).
         * Likewise, the ownership of the single type reference for partially
         * (in-)validated pages is tied to this flag, i.e. the instance
         * setting the flag must not drop that reference, whereas the instance
         * clearing it will have to.
         *
         * If partial_flags & PTF_partial_set is set, then the page at
         * at @nr_validated_ptes had PGT_partial set as a result of an
         * operation on the current page.  (That page may or may not
         * still have PGT_partial set.)
         *
         * Additionally, if PTF_partial_set is set, then the PTE at
         * @nr_validated_ptef holds a general reference count for the
         * page.
         *
         * This happens:
         * - During validation or de-validation, if the operation was
         *   interrupted
         * - During validation, if an invalid entry is encountered and
         *   validation is preemptible
         * - During validation, if PTF_partial_set was set on this
         *   entry to begin with (perhaps because it picked up a
         *   previous operation)
         *
         * When resuming validation, if PTF_partial_set is clear, then
         * a general reference must be re-acquired; if it is set, no
         * reference should be acquired.
         *
         * When resuming de-validation, if PTF_partial_set is clear,
         * no reference should be dropped; if it is set, a reference
         * should be dropped.
         *
         * NB that PTF_partial_set is defined in mm.c, the only place
         * where it is used.
         *
         * The 3rd field, @linear_pt_count, indicates
         * - by a positive value, how many same-level page table entries a page
         *   table has,
         * - by a negative value, in how many same-level page tables a page is
         *   in use.
         */
        struct {
            u16 nr_validated_ptes:PAGETABLE_ORDER + 1;
            u16 :16 - PAGETABLE_ORDER - 1 - 1;
            u16 partial_flags:1;
            s16 linear_pt_count;
        };

        /*
         * Guest pages with a shadow.  This does not conflict with
         * tlbflush_timestamp since page table pages are explicitly not
         * tracked for TLB-flush avoidance when a guest runs in shadow mode.
         *
         * pagetable_dying is used for HVM domains only. The layout here has
         * to avoid re-use of the space used by linear_pt_count, which (only)
         * PV guests use.
         */
        struct {
            uint16_t shadow_flags;
#ifdef CONFIG_HVM
            bool pagetable_dying;
#endif
        };

        /* When in use as a shadow, next shadow in this hash chain. */
        __pdx_t next_shadow;
    };
};

#undef __pdx_t

#define is_xen_heap_page(page) ((page)->count_info & PGC_xen_heap)
#define is_xen_heap_mfn(mfn) \
    (mfn_valid(mfn) && is_xen_heap_page(mfn_to_page(mfn)))
#define is_xen_fixed_mfn(mfn)                     \
    (((mfn_to_maddr(mfn)) >= virt_to_maddr((unsigned long)_stext)) && \
     ((mfn_to_maddr(mfn)) <= virt_to_maddr((unsigned long)__2M_rwdata_end - 1)))

#define PRtype_info "016lx"/* should only be used for printk's */

/* The number of out-of-sync shadows we allow per vcpu (prime, please) */
#define SHADOW_OOS_PAGES 3

/* OOS fixup entries */
#define SHADOW_OOS_FIXUPS 2

#define page_get_owner(_p)                                              \
    ((struct domain *)((_p)->v.inuse._domain ?                          \
                       pdx_to_virt((_p)->v.inuse._domain) : NULL))
#define page_set_owner(_p,_d)                                           \
    ((_p)->v.inuse._domain = (_d) ? virt_to_pdx(_d) : 0)

#define maddr_get_owner(ma)   (page_get_owner(maddr_to_page((ma))))

#define frame_table ((struct page_info *)FRAMETABLE_VIRT_START)
extern unsigned long max_page;
extern unsigned long total_pages;
void init_frametable(void);

#define PDX_GROUP_SHIFT L2_PAGETABLE_SHIFT

/* Convert between Xen-heap virtual addresses and page-info structures. */
static inline struct page_info *__virt_to_page(const void *v)
{
    unsigned long va = (unsigned long)v;

    ASSERT(va >= XEN_VIRT_START);
    ASSERT(va < DIRECTMAP_VIRT_END);
    if ( va < XEN_VIRT_END )
        va += DIRECTMAP_VIRT_START - XEN_VIRT_START + xen_phys_start;
    else
        ASSERT(va >= DIRECTMAP_VIRT_START);
    return frame_table + ((va - DIRECTMAP_VIRT_START) >> PAGE_SHIFT);
}

static inline void *__page_to_virt(const struct page_info *pg)
{
    ASSERT((unsigned long)pg - FRAMETABLE_VIRT_START < FRAMETABLE_SIZE);
    /*
     * (sizeof(*pg) & -sizeof(*pg)) selects the LS bit of sizeof(*pg). The
     * division and re-multiplication avoids one shift when sizeof(*pg) is a
     * power of two (otherwise there would be a right shift followed by a
     * left shift, which the compiler can't know it can fold into one).
     */
    return (void *)(DIRECTMAP_VIRT_START +
                    ((unsigned long)pg - FRAMETABLE_VIRT_START) /
                    (sizeof(*pg) / (sizeof(*pg) & -sizeof(*pg))) *
                    (PAGE_SIZE / (sizeof(*pg) & -sizeof(*pg))));
}

int devalidate_page(struct page_info *page, unsigned long type,
                         int preemptible);

void init_xen_pae_l2_slots(l2_pgentry_t *l2t, const struct domain *d);
void init_xen_l4_slots(l4_pgentry_t *l4t, mfn_t l4mfn,
                       const struct domain *d, mfn_t sl4mfn, bool ro_mpt);
bool fill_ro_mpt(mfn_t mfn);
void zap_ro_mpt(mfn_t mfn);

bool is_iomem_page(mfn_t mfn);

struct platform_bad_page {
    unsigned long mfn;
    unsigned int order;
};

const struct platform_bad_page *get_platform_badpages(unsigned int *array_size);

/* Per page locks:
 * page_lock() is used for pte serialization.
 *
 * All users of page lock for pte serialization live in mm.c, use it
 * to lock a page table page during pte updates, do not take other locks within
 * the critical section delimited by page_lock/unlock, and perform no
 * nesting.
 *
 * The use of PGT_locked in mem_sharing does not collide, since mem_sharing is
 * only supported for hvm guests, which do not have PV PTEs updated.
 */
int page_lock(struct page_info *page);
void page_unlock(struct page_info *page);

void put_page_type(struct page_info *page);
int  get_page_type(struct page_info *page, unsigned long type);
int  put_page_type_preemptible(struct page_info *page);
int  get_page_type_preemptible(struct page_info *page, unsigned long type);
int  put_old_guest_table(struct vcpu *);
int  get_page_from_l1e(
    l1_pgentry_t l1e, struct domain *l1e_owner, struct domain *pg_owner);
void put_page_from_l1e(l1_pgentry_t l1e, struct domain *l1e_owner);

static inline struct page_info *get_page_from_mfn(mfn_t mfn, struct domain *d)
{
    struct page_info *page = mfn_to_page(mfn);

    if ( unlikely(!mfn_valid(mfn)) || unlikely(!get_page(page, d)) )
    {
        gdprintk(XENLOG_WARNING,
                 "Could not get page ref for mfn %"PRI_mfn"\n", mfn_x(mfn));
        return NULL;
    }

    return page;
}

static inline void put_page_and_type(struct page_info *page)
{
    put_page_type(page);
    put_page(page);
}

static inline int put_page_and_type_preemptible(struct page_info *page)
{
    int rc = put_page_type_preemptible(page);

    if ( likely(rc == 0) )
        put_page(page);
    return rc;
}

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

#define ASSERT_PAGE_IS_TYPE(_p, _t)                            \
    ASSERT(((_p)->u.inuse.type_info & PGT_type_mask) == (_t)); \
    ASSERT(((_p)->u.inuse.type_info & PGT_count_mask) != 0)
#define ASSERT_PAGE_IS_DOMAIN(_p, _d)                          \
    ASSERT(((_p)->count_info & PGC_count_mask) != 0);          \
    ASSERT(page_get_owner(_p) == (_d))

extern paddr_t mem_hotplug;

/******************************************************************************
 * With shadow pagetables, the different kinds of address start
 * to get get confusing.
 *
 * Virtual addresses are what they usually are: the addresses that are used
 * to accessing memory while the guest is running.  The MMU translates from
 * virtual addresses to machine addresses.
 *
 * (Pseudo-)physical addresses are the abstraction of physical memory the
 * guest uses for allocation and so forth.  For the purposes of this code,
 * we can largely ignore them.
 *
 * Guest frame numbers (gfns) are the entries that the guest puts in its
 * pagetables.  For normal paravirtual guests, they are actual frame numbers,
 * with the translation done by the guest.
 *
 * Machine frame numbers (mfns) are the entries that the hypervisor puts
 * in the shadow page tables.
 *
 * Elsewhere in the xen code base, the name "gmfn" is generally used to refer
 * to a "machine frame number, from the guest's perspective", or in other
 * words, pseudo-physical frame numbers.  However, in the shadow code, the
 * term "gmfn" means "the mfn of a guest page"; this combines naturally with
 * other terms such as "smfn" (the mfn of a shadow page), gl2mfn (the mfn of a
 * guest L2 page), etc...
 */

/*
 * The MPT (machine->physical mapping table) is an array of word-sized
 * values, indexed on machine frame number. It is expected that guest OSes
 * will use it to store a "physical" frame number to give the appearance of
 * contiguous (or near contiguous) physical memory.
 */
#undef  machine_to_phys_mapping
#define machine_to_phys_mapping  ((unsigned long *)RDWR_MPT_VIRT_START)
#define INVALID_M2P_ENTRY        (~0UL)
#define VALID_M2P(_e)            (!((_e) & (1UL<<(BITS_PER_LONG-1))))
#define SHARED_M2P_ENTRY         (~0UL - 1UL)
#define SHARED_M2P(_e)           ((_e) == SHARED_M2P_ENTRY)

/*
 * Disable some users of set_gpfn_from_mfn() (e.g., free_heap_pages()) until
 * the machine_to_phys_mapping is actually set up.
 */
extern bool machine_to_phys_mapping_valid;

void set_gpfn_from_mfn(unsigned long mfn, unsigned long pfn);

extern struct rangeset *mmio_ro_ranges;

#define get_gpfn_from_mfn(mfn)      (machine_to_phys_mapping[(mfn)])

#define compat_pfn_to_cr3(pfn) (((unsigned)(pfn) << 12) | ((unsigned)(pfn) >> 20))
#define compat_cr3_to_pfn(cr3) (((unsigned)(cr3) >> 12) | ((unsigned)(cr3) << 20))

void memguard_guard_stack(void *p);
void memguard_unguard_stack(void *p);

struct mmio_ro_emulate_ctxt {
        unsigned long cr2;
        unsigned int seg, bdf;
};

int cf_check mmio_ro_emulated_write(
    enum x86_segment seg, unsigned long offset, void *p_data,
    unsigned int bytes, struct x86_emulate_ctxt *ctxt);
int cf_check mmcfg_intercept_write(
    enum x86_segment seg, unsigned long offset, void *p_data,
    unsigned int bytes, struct x86_emulate_ctxt *ctxt);

int audit_adjust_pgtables(struct domain *d, int dir, int noisy);

extern int pagefault_by_memadd(unsigned long addr, struct cpu_user_regs *regs);
extern int handle_memadd_fault(unsigned long addr, struct cpu_user_regs *regs);

#ifndef NDEBUG

#define AUDIT_SHADOW_ALREADY_LOCKED ( 1u << 0 )
#define AUDIT_ERRORS_OK             ( 1u << 1 )
#define AUDIT_QUIET                 ( 1u << 2 )

void _audit_domain(struct domain *d, int flags);
#define audit_domain(_d) _audit_domain((_d), AUDIT_ERRORS_OK)
void audit_domains(void);

#else

#define _audit_domain(_d, _f) ((void)0)
#define audit_domain(_d)      ((void)0)
#define audit_domains()       ((void)0)

#endif

void make_cr3(struct vcpu *v, mfn_t mfn);
void update_cr3(struct vcpu *v);
int vcpu_destroy_pagetables(struct vcpu *);
void *do_page_walk(struct vcpu *v, unsigned long addr);

/* Allocator functions for Xen pagetables. */
mfn_t alloc_xen_pagetable(void);
void free_xen_pagetable(mfn_t mfn);
void *alloc_mapped_pagetable(mfn_t *pmfn);

l1_pgentry_t *virt_to_xen_l1e(unsigned long v);

int __sync_local_execstate(void);

/* Arch-specific portion of memory_op hypercall. */
long arch_memory_op(unsigned long cmd, XEN_GUEST_HANDLE_PARAM(void) arg);
long subarch_memory_op(unsigned long cmd, XEN_GUEST_HANDLE_PARAM(void) arg);
int compat_arch_memory_op(unsigned long cmd, XEN_GUEST_HANDLE_PARAM(void));
int compat_subarch_memory_op(int op, XEN_GUEST_HANDLE_PARAM(void));

#define NIL(type) ((type *)-sizeof(type))
#define IS_NIL(ptr) (!((uintptr_t)(ptr) + sizeof(*(ptr))))

int create_perdomain_mapping(struct domain *, unsigned long va,
                             unsigned int nr, l1_pgentry_t **,
                             struct page_info **);
void destroy_perdomain_mapping(struct domain *, unsigned long va,
                               unsigned int nr);
void free_perdomain_mappings(struct domain *);

void __iomem *ioremap_wc(paddr_t, size_t);

extern int memory_add(unsigned long spfn, unsigned long epfn, unsigned int pxm);

void domain_set_alloc_bitsize(struct domain *d);
unsigned int domain_clamp_alloc_bitsize(struct domain *d, unsigned int bits);

unsigned long domain_get_maximum_gpfn(struct domain *d);

/* Definition of an mm lock: spinlock with extra fields for debugging */
typedef struct mm_lock {
    spinlock_t         lock;
    int                unlock_level;
    int                locker;          /* processor which holds the lock */
    const char        *locker_function; /* func that took it */
} mm_lock_t;

typedef struct mm_rwlock {
    percpu_rwlock_t    lock;
    int                unlock_level;
    int                recurse_count;
    int                locker; /* CPU that holds the write lock */
    const char        *locker_function; /* func that took it */
} mm_rwlock_t;

#define arch_free_heap_page(d, pg) \
    page_list_del2(pg, page_to_list(d, pg), &(d)->arch.relmem_list)

extern const char zero_page[];

/* Build a 32bit PSE page table using 4MB pages. */
void write_32bit_pse_identmap(uint32_t *l2);

/*
 * x86 maps part of physical memory via the directmap region.
 * Return whether the range of MFN falls in the directmap region.
 */
static inline bool arch_mfns_in_directmap(unsigned long mfn, unsigned long nr)
{
    unsigned long eva = min(DIRECTMAP_VIRT_END, HYPERVISOR_VIRT_END);

    return (mfn + nr) <= (virt_to_mfn(eva - 1) + 1);
}

#endif /* __ASM_X86_MM_H__ */
