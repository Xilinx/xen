#ifndef __ARCH_ARM_MM_MMU__
#define __ARCH_ARM_MM_MMU__

#define frame_table ((struct page_info *)FRAMETABLE_VIRT_START)

/* Boot-time pagetable setup */
extern void setup_pagetables(unsigned long boot_phys_offset, paddr_t xen_paddr);
/* Allocate and initialise pagetables for a secondary CPU. Sets init_ttbr to the
 * new page table */
extern int init_secondary_pagetables(int cpu);
/* Switch secondary CPUS to its own pagetables and finalise MMU setup */
extern void mmu_init_secondary_cpu(void);
/* Non-boot CPUs use this to find the correct pagetables. */
extern uint64_t init_ttbr;

static inline paddr_t __virt_to_maddr(vaddr_t va)
{
    uint64_t par = va_to_par(va);
    return (par & PADDR_MASK & PAGE_MASK) | (va & ~PAGE_MASK);
}
#define virt_to_maddr(va)   __virt_to_maddr((vaddr_t)(va))

#ifdef CONFIG_ARM_32
static inline void *maddr_to_virt(paddr_t ma)
{
    ASSERT(is_xen_heap_mfn(maddr_to_mfn(ma)));
    ma -= mfn_to_maddr(directmap_mfn_start);
    return (void *)(unsigned long) ma + XENHEAP_VIRT_START;
}
#else
static inline void *maddr_to_virt(paddr_t ma)
{
    ASSERT((mfn_to_pdx(maddr_to_mfn(ma)) - directmap_base_pdx) <
           (DIRECTMAP_SIZE >> PAGE_SHIFT));
    return (void *)(XENHEAP_VIRT_START -
                    (directmap_base_pdx << PAGE_SHIFT) +
                    ((ma & ma_va_bottom_mask) |
                     ((ma & ma_top_mask) >> pfn_pdx_hole_shift)));
}
#endif

/* Convert between Xen-heap virtual addresses and page-info structures. */
static inline struct page_info *virt_to_page(const void *v)
{
    unsigned long va = (unsigned long)v;
    unsigned long pdx;

    ASSERT(va >= XENHEAP_VIRT_START);
    ASSERT(va < directmap_virt_end);

    pdx = (va - XENHEAP_VIRT_START) >> PAGE_SHIFT;
    pdx += mfn_to_pdx(directmap_mfn_start);
    return frame_table + pdx - frametable_base_pdx;
}

#endif /* __ARCH_ARM_MM_MMU__ */
