#include <xen/cpu.h>
#include <xen/domain_page.h>
#include <xen/iocap.h>
#include <xen/ioreq.h>
#include <xen/lib.h>
#include <xen/init.h>
#include <xen/sched.h>
#include <xen/softirq.h>

#include <asm/alternative.h>
#include <asm/event.h>
#include <asm/flushtlb.h>
#include <asm/guest_walk.h>
#include <asm/page.h>
#include <asm/traps.h>
#include <xen/init.h>
#include <xen/sched.h>
#include <asm/p2m.h>

#ifdef CONFIG_ARM_64
/* VMID is by default 8 bit width on AArch64. */
unsigned int __read_mostly max_vmid = MAX_VMID_8_BIT;
#endif

/*
 * Set to the maximum configured support for IPA bits, so the number of IPA bits can be
 * restricted by external entity (e.g. IOMMU).
 */
unsigned int __read_mostly p2m_ipa_bits = PADDR_BITS;

/* Return the size of the pool, in bytes. */
int arch_get_paging_mempool_size(struct domain *d, uint64_t *size)
{
    *size = (uint64_t)ACCESS_ONCE(d->arch.paging.p2m_total_pages) << PAGE_SHIFT;
    return 0;
}

int arch_set_paging_mempool_size(struct domain *d, uint64_t size)
{
    unsigned long pages = size >> PAGE_SHIFT;
    bool preempted = false;
    int rc;

    if ( (size & ~PAGE_MASK) ||          /* Non page-sized request? */
         pages != (size >> PAGE_SHIFT) ) /* 32-bit overflow? */
        return -EINVAL;

    spin_lock(&d->arch.paging.lock);
    rc = p2m_set_allocation(d, pages, &preempted);
    spin_unlock(&d->arch.paging.lock);

    ASSERT(preempted == (rc == -ERESTART));

    return rc;
}

struct page_info *_p2m_alloc_page(struct domain *d)
{
    /* If cache coloring is enabled, p2m tables are allocated using the domain
     * coloring configuration to prevent cache interference. */
    if ( IS_ENABLED(CONFIG_CACHE_COLORING) )
        return alloc_domheap_page(d, MEMF_no_owner);
    else
        return alloc_domheap_page(NULL, 0);
}

struct page_info *p2m_alloc_page(struct domain *d)
{
    struct page_info *pg;

    spin_lock(&d->arch.paging.lock);
    /*
     * For hardware domain, there should be no limit in the number of pages that
     * can be allocated, so that the kernel may take advantage of the extended
     * regions. Hence, allocate p2m pages for hardware domains from heap.
     */
    if ( is_hardware_domain(d) )
    {
        pg = _p2m_alloc_page(d);
        if ( pg == NULL )
        {
            printk(XENLOG_G_ERR "Failed to allocate P2M pages for hwdom.\n");
            spin_unlock(&d->arch.paging.lock);
            return NULL;
        }
    }
    else
    {
        pg = page_list_remove_head(&d->arch.paging.p2m_freelist);
        if ( unlikely(!pg) )
        {
            spin_unlock(&d->arch.paging.lock);
            return NULL;
        }
    }
    spin_unlock(&d->arch.paging.lock);

    return pg;
}

void p2m_free_page(struct domain *d, struct page_info *pg)
{
    spin_lock(&d->arch.paging.lock);
    if ( is_hardware_domain(d) )
        free_domheap_page(pg);
    else
        page_list_add_tail(pg, &d->arch.paging.p2m_freelist);
    spin_unlock(&d->arch.paging.lock);
}

mfn_t p2m_lookup(struct domain *d, gfn_t gfn, p2m_type_t *t)
{
    mfn_t mfn;
    struct p2m_domain *p2m = p2m_get_hostp2m(d);

    p2m_read_lock(p2m);
    mfn = p2m_get_entry(p2m, gfn, t, NULL, NULL, NULL);
    p2m_read_unlock(p2m);

    return mfn;
}

struct page_info *p2m_get_page_from_gfn(struct domain *d, gfn_t gfn,
                                        p2m_type_t *t)
{
    struct page_info *page;
    p2m_type_t p2mt;
    mfn_t mfn = p2m_lookup(d, gfn, &p2mt);

    if ( t )
        *t = p2mt;

    if ( !p2m_is_any_ram(p2mt) )
        return NULL;

    if ( !mfn_valid(mfn) )
        return NULL;

    page = mfn_to_page(mfn);

    /*
     * get_page won't work on foreign mapping because the page doesn't
     * belong to the current domain.
     */
    if ( p2m_is_foreign(p2mt) )
    {
        struct domain *fdom = page_get_owner_and_reference(page);
        ASSERT(fdom != NULL);
        ASSERT(fdom != d);
        return page;
    }

    return get_page(page, d) ? page : NULL;
}

int p2m_insert_mapping(struct domain *d, gfn_t start_gfn, unsigned long nr,
                       mfn_t mfn, p2m_type_t t)
{
    struct p2m_domain *p2m = p2m_get_hostp2m(d);
    int rc;

    p2m_write_lock(p2m);
    rc = p2m_set_entry(p2m, start_gfn, nr, mfn, t, p2m->default_access);
    p2m_write_unlock(p2m);

    return rc;
}

int map_regions_p2mt(struct domain *d,
                     gfn_t gfn,
                     unsigned long nr,
                     mfn_t mfn,
                     p2m_type_t p2mt)
{
    return p2m_insert_mapping(d, gfn, nr, mfn, p2mt);
}

spinlock_t vmid_alloc_lock = SPIN_LOCK_UNLOCKED;

/*
 * VTTBR_EL2 VMID field is 8 or 16 bits. AArch64 may support 16-bit VMID.
 * Using a bitmap here limits us to 256 or 65536 (for AArch64) concurrent
 * domains. The bitmap space will be allocated dynamically based on
 * whether 8 or 16 bit VMIDs are supported.
 */
unsigned long *vmid_mask;

void p2m_vmid_allocator_init(void)
{
    /*
     * allocate space for vmid_mask based on MAX_VMID
     */
    vmid_mask = xzalloc_array(unsigned long, BITS_TO_LONGS(MAX_VMID));

    if ( !vmid_mask )
        panic("Could not allocate VMID bitmap space\n");

    set_bit(INVALID_VMID, vmid_mask);
}

int p2m_alloc_vmid(struct domain *d)
{
    struct p2m_domain *p2m = p2m_get_hostp2m(d);

    int rc, nr;

    spin_lock(&vmid_alloc_lock);

    nr = find_first_zero_bit(vmid_mask, MAX_VMID);

    ASSERT(nr != INVALID_VMID);

    if ( nr == MAX_VMID )
    {
        rc = -EBUSY;
        printk(XENLOG_ERR "p2m.c: dom%d: VMID pool exhausted\n", d->domain_id);
        goto out;
    }

    set_bit(nr, vmid_mask);

    p2m->vmid = nr;

    rc = 0;

out:
    spin_unlock(&vmid_alloc_lock);
    return rc;
}

/*
 * Local variables:
 * mode: C
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * End:
 */
