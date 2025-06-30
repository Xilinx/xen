/* SPDX-License-Identifier: GPL-2.0-or-later */

#include <xen/init.h>
#include <xen/lib.h>
#include <xen/macros.h>
#include <xen/mm.h>
#include <xen/mm-frame.h>
#include <xen/pdx.h>
#include <xen/string.h>

unsigned long frametable_virt_end __read_mostly;

static void __init
init_frametable_chunk(unsigned long pdx_s, unsigned long pdx_e)
{
    unsigned long nr_pdxs = pdx_e - pdx_s;
    unsigned long chunk_size = nr_pdxs * sizeof(struct page_info);
    const unsigned long mapping_size = chunk_size < MB(32) ? MB(2) : MB(32);
    unsigned long virt;
    int rc;
    mfn_t base_mfn;

    /* Round up to 2M or 32M boundary, as appropriate. */
    chunk_size = ROUNDUP(chunk_size, mapping_size);
    base_mfn = alloc_boot_pages(chunk_size >> PAGE_SHIFT, 32 << (20 - 12));

    virt = (unsigned long)pdx_to_page(pdx_s);
    rc = map_pages_to_xen(virt, base_mfn, chunk_size >> PAGE_SHIFT,
                          PAGE_HYPERVISOR_RW | _PAGE_BLOCK);
    if ( rc )
        panic("Unable to setup the frametable mappings\n");

    memset(&frame_table[pdx_s], 0, nr_pdxs * sizeof(struct page_info));
    memset(&frame_table[pdx_e], -1,
           chunk_size - nr_pdxs * sizeof(struct page_info));
}

void __init init_frametable(void)
{
    unsigned int sidx, eidx, nidx;
    unsigned int max_idx;

    /*
     * The size of paddr_t should be sufficient for the complete range of
     * physical address.
     */
    BUILD_BUG_ON((sizeof(paddr_t) * BITS_PER_BYTE) < PADDR_BITS);
    BUILD_BUG_ON(sizeof(struct page_info) != PAGE_INFO_SIZE);

    frametable_base_pdx = mfn_to_pdx(directmap_mfn_start);

    max_pdx = pfn_to_pdx(max_page - 1) + 1;

    if ( max_pdx > FRAMETABLE_NR )
        panic("Frametable too small\n");

    max_idx = DIV_ROUND_UP(max_pdx, PDX_GROUP_COUNT);

    for ( sidx = (frametable_base_pdx / PDX_GROUP_COUNT); ; sidx = nidx )
    {
        eidx = find_next_zero_bit(pdx_group_valid, max_idx, sidx);
        nidx = find_next_bit(pdx_group_valid, max_idx, eidx);

        if ( nidx >= max_idx )
            break;

        init_frametable_chunk(sidx * PDX_GROUP_COUNT, eidx * PDX_GROUP_COUNT);
    }

    init_frametable_chunk(sidx * PDX_GROUP_COUNT, max_pdx);
}

/*
 * Local variables:
 * mode: C
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * End:
 */
