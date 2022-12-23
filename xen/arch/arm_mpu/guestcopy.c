#include <xen/domain_page.h>
#include <xen/guest_access.h>
#include <xen/lib.h>
#include <xen/mm.h>
#include <xen/sched.h>

#include <asm/current.h>

#define COPY_flush_dcache   (1U << 0)
#define COPY_from_guest     (0U << 1)
#define COPY_to_guest       (1U << 1)
#define COPY_ipa            (0U << 2)
#define COPY_linear         (1U << 2)

typedef union
{
    struct
    {
        struct vcpu *v;
    } gva;

    struct
    {
        struct domain *d;
    } gpa;
} copy_info_t;

#define GVA_INFO(vcpu) ((copy_info_t) { .gva = { vcpu } })
#define GPA_INFO(domain) ((copy_info_t) { .gpa = { domain } })

static struct page_info *translate_get_page(copy_info_t info, uint64_t addr,
                                            bool linear, bool write)
{
    p2m_type_t p2mt;
    struct page_info *page;

    if ( linear )
        return get_page_from_gva(info.gva.v, addr,
                                 write ? GV2M_WRITE : GV2M_READ);

    page = get_page_from_gfn(info.gpa.d, paddr_to_pfn(addr), &p2mt, P2M_ALLOC);

    if ( !page )
        return NULL;

    if ( !p2m_is_ram(p2mt) )
    {
        put_page(page);
        return NULL;
    }

    return page;
}

#ifdef CONFIG_HAS_MPU
static struct page_info *translate_get_region(copy_info_t info, uint64_t addr,
                                              unsigned int len)
{
    p2m_type_t p2mt;
    struct page_info *page = NULL;

    /* Base address and length shall be correctly aligned to PAGE_SIZE. */
    ASSERT(!(addr & ~PAGE_MASK) && !(len & ~PAGE_MASK));

    page = get_region_from_gfns(info.gpa.d, paddr_to_pfn(addr), len >> PAGE_SHIFT, &p2mt);
    if ( !page )
        return NULL;

    if ( !p2m_is_ram(p2mt) )
        return NULL;

    return page;
}
#endif

static void mem_copy_to_guest(void *buf, void *p, unsigned int size,
                              unsigned int flags)
{
    if ( flags & COPY_to_guest )
    {
        /*
         * buf will be NULL when the caller request to zero the
         * guest memory.
         */
        if ( buf )
            memcpy(p, buf, size);
        else
            memset(p, 0, size);
    }
    else
        memcpy(buf, p, size);

    if ( flags & COPY_flush_dcache )
        clean_dcache_va_range(p, size);
}

static unsigned long copy_guest(void *buf, uint64_t addr, unsigned int len,
                                copy_info_t info, unsigned int flags)
{
    /* XXX needs to handle faults */
    unsigned int offset = addr & ~PAGE_MASK;

    BUILD_BUG_ON((sizeof(addr)) < sizeof(vaddr_t));
    BUILD_BUG_ON((sizeof(addr)) < sizeof(paddr_t));

    while ( len )
    {
        void *p;
        unsigned int size = min(len, (unsigned int)PAGE_SIZE - offset);
        struct page_info *page;

#ifdef CONFIG_HAS_MPU
        /*
         * On MPU system, due to 1:1 direct-map feature(GFN == MFN), user
         * could copy into/from guest memory in size of a memory region with
         * multiple pages.
         */
        if ( !offset && (len > PAGE_SIZE) )
        {
            size = round_pgdown(len);

            page = translate_get_region(info, addr, size);
            if ( page == NULL )
                return len;

            p = __map_domain_page(page);

            mem_copy_to_guest(buf, p, size, flags);

            unmap_domain_page(p);

            for ( unsigned int i = 0; i < (size >> PAGE_SHIFT); i++ )
                put_page(page + i);
            len -= size;
            buf += size;
            addr += size;

            continue;
        }
#endif

        page = translate_get_page(info, addr, flags & COPY_linear,
                                  flags & COPY_to_guest);
        if ( page == NULL )
            return len;

        p = __map_domain_page(page);
        p += offset;

        mem_copy_to_guest(buf, p, size, flags);

        unmap_domain_page(p - offset);
        put_page(page);
        len -= size;
        buf += size;
        addr += size;
        /*
         * After the first iteration, guest virtual address is correctly
         * aligned to PAGE_SIZE.
         */
        offset = 0;
    }

    return 0;
}

unsigned long raw_copy_to_guest(void *to, const void *from, unsigned int len)
{
    return copy_guest((void *)from, (vaddr_t)to, len,
                      GVA_INFO(current), COPY_to_guest | COPY_linear);
}

unsigned long raw_copy_to_guest_flush_dcache(void *to, const void *from,
                                             unsigned int len)
{
    return copy_guest((void *)from, (vaddr_t)to, len, GVA_INFO(current),
                      COPY_to_guest | COPY_flush_dcache | COPY_linear);
}

unsigned long raw_clear_guest(void *to, unsigned int len)
{
    return copy_guest(NULL, (vaddr_t)to, len, GVA_INFO(current),
                      COPY_to_guest | COPY_linear);
}

unsigned long raw_copy_from_guest(void *to, const void __user *from,
                                  unsigned int len)
{
    return copy_guest(to, (vaddr_t)from, len, GVA_INFO(current),
                      COPY_from_guest | COPY_linear);
}

unsigned long copy_to_guest_phys_flush_dcache(struct domain *d,
                                              paddr_t gpa,
                                              void *buf,
                                              unsigned int len)
{
    return copy_guest(buf, gpa, len, GPA_INFO(d),
                      COPY_to_guest | COPY_ipa | COPY_flush_dcache);
}

int access_guest_memory_by_ipa(struct domain *d, paddr_t gpa, void *buf,
                               uint32_t size, bool is_write)
{
    unsigned long left;
    int flags = COPY_ipa;

    flags |= is_write ? COPY_to_guest : COPY_from_guest;

    left = copy_guest(buf, gpa, size, GPA_INFO(d), flags);

    return (!left) ? 0 : -EINVAL;
}

/*
 * Local variables:
 * mode: C
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * End:
 */
