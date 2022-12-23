/*
 * xen/arch/arm/mm.c
 *
 * Memory management common code for MMU and MPU sytem
 *
 * Tim Deegan <tim@xen.org>
 * Copyright (c) 2011 Citrix Systems.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <xen/domain_page.h>
#include <xen/sched.h>
#include <xen/types.h>

#include <asm/coloring.h>

#ifdef CONFIG_CACHE_COLORING
static DEFINE_PAGE_TABLE(xen_colored_temp);
#endif

/* Non-boot CPUs use this to find the correct pagetables. */
uint64_t init_ttbr;

unsigned long frametable_base_pdx __read_mostly;

unsigned long max_page;
unsigned long total_pages;

void flush_page_to_ram(unsigned long mfn, bool sync_icache)
{
    void *v = map_domain_page(_mfn(mfn));

    clean_and_invalidate_dcache_va_range(v, PAGE_SIZE);
    unmap_domain_page(v);

    /*
     * For some of the instruction cache (such as VIPT), the entire I-Cache
     * needs to be flushed to guarantee that all the aliases of a given
     * physical address will be removed from the cache.
     * Invalidating the I-Cache by VA highly depends on the behavior of the
     * I-Cache (See D4.9.2 in ARM DDI 0487A.k_iss10775). Instead of using flush
     * by VA on select platforms, we just flush the entire cache here.
     */
    if ( sync_icache )
        invalidate_icache();
}

extern void relocate_xen(uint64_t ttbr, void *src, void *dst, size_t len);

void arch_dump_shared_mem_info(void)
{
}

int steal_page(
    struct domain *d, struct page_info *page, unsigned int memflags)
{
    return -EOPNOTSUPP;
}

int page_is_ram_type(unsigned long mfn, unsigned long mem_type)
{
    ASSERT_UNREACHABLE();
    return 0;
}

unsigned long domain_get_maximum_gpfn(struct domain *d)
{
    return gfn_x(d->arch.p2m.max_mapped_gfn);
}

void share_xen_page_with_guest(struct page_info *page, struct domain *d,
                               enum XENSHARE_flags flags)
{
    if ( page_get_owner(page) == d )
        return;

    spin_lock(&d->page_alloc_lock);

    /*
     * The incremented type count pins as writable or read-only.
     *
     * Please note, the update of type_info field here is not atomic as
     * we use Read-Modify-Write operation on it. But currently it is fine
     * because the caller of page_set_xenheap_gfn() (which is another place
     * where type_info is updated) would need to acquire a reference on
     * the page. This is only possible after the count_info is updated *and*
     * there is a barrier between the type_info and count_info. So there is
     * no immediate need to use cmpxchg() here.
     */
    page->u.inuse.type_info &= ~(PGT_type_mask | PGT_count_mask);
    page->u.inuse.type_info |= (flags == SHARE_ro ? PGT_none
                                                  : PGT_writable_page) |
                                MASK_INSR(1, PGT_count_mask);

    page_set_owner(page, d);
    smp_wmb(); /* install valid domain ptr before updating refcnt. */
    ASSERT((page->count_info & ~PGC_xen_heap) == 0);

    /* Only add to the allocation list if the domain isn't dying. */
    if ( !d->is_dying )
    {
        page->count_info |= PGC_allocated | 1;
        if ( unlikely(d->xenheap_pages++ == 0) )
            get_knownalive_domain(d);
        page_list_add_tail(page, &d->xenpage_list);
    }

    spin_unlock(&d->page_alloc_lock);
}

static struct domain *page_get_owner_and_nr_reference(struct page_info *page,
                                                      unsigned long nr)
{
    unsigned long x, y = page->count_info;
    struct domain *owner;

    /* Restrict nr to avoid "double" overflow */
    if ( nr >= PGC_count_mask )
    {
        ASSERT_UNREACHABLE();
        return NULL;
    }

    do {
        x = y;
        /*
         * Count ==  0: Page is not allocated, so we cannot take a reference.
         * Count == -1: Reference count would wrap, which is invalid.
         */
        if ( unlikely(((x + nr) & PGC_count_mask) <= nr) )
            return NULL;
    }
    while ( (y = cmpxchg(&page->count_info, x, x + nr)) != x );

    owner = page_get_owner(page);
    ASSERT(owner);

    return owner;
}

struct domain *page_get_owner_and_reference(struct page_info *page)
{
    return page_get_owner_and_nr_reference(page, 1);
}

void put_page_nr(struct page_info *page, unsigned long nr)
{
    unsigned long nx, x, y = page->count_info;

    do {
        ASSERT((y & PGC_count_mask) >= nr);
        x  = y;
        nx = x - nr;
    }
    while ( unlikely((y = cmpxchg(&page->count_info, x, nx)) != x) );

    if ( unlikely((nx & PGC_count_mask) == 0) )
    {
        if ( unlikely(nx & PGC_static) )
            free_domstatic_page(page);
        else
            free_domheap_page(page);
    }
}

void put_page(struct page_info *page)
{
    put_page_nr(page, 1);
}

bool get_page_nr(struct page_info *page, const struct domain *domain,
                 unsigned long nr)
{
    const struct domain *owner = page_get_owner_and_nr_reference(page, nr);

    if ( likely(owner == domain) )
        return true;

    if ( owner != NULL )
        put_page_nr(page, nr);

    return false;
}

bool get_page(struct page_info *page, const struct domain *domain)
{
    return get_page_nr(page, domain, 1);
}

/* Common code requires get_page_type and put_page_type.
 * We don't care about typecounts so we just do the minimum to make it
 * happy. */
int get_page_type(struct page_info *page, unsigned long type)
{
    return 1;
}

void put_page_type(struct page_info *page)
{
    return;
}

bool is_iomem_page(mfn_t mfn)
{
    return !mfn_valid(mfn);
}

void clear_and_clean_page(struct page_info *page)
{
    void *p = __map_domain_page(page);

    clear_page(p);
    clean_dcache_va_range(p, PAGE_SIZE);
    unmap_domain_page(p);
}

unsigned long get_upper_mfn_bound(void)
{
    /* No memory hotplug yet, so current memory limit is the final one. */
    return max_page - 1;
}

/*
 * Local variables:
 * mode: C
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * End:
 */
