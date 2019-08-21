/*
 * xen/arch/arm/color_alloc.c
 *
 * Colored allocator
 *
 * Copyright (C) 2019 Xilinx Inc.
 *
 * Authors:
 *  Luca Miccio <lucmiccio@gmail.com> (Università di Modena e Reggio Emilia)
 *  Marco Solieri <ms@xt3.it> (Università di Modena e Reggio Emilia)
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */
#include <asm/coloring.h>
#include <asm/page.h>
#include <xen/keyhandler.h>

/*
 * Pages are stored by their color in separated lists. Each list defines a color
 * and it is initialized during end_boot_allocator, where each page's color
 * is calculated and the page itself is put in the correct list.
 * After initialization we have N list where N is the number of maximum
 * available colors on the platform.
 * All the lists' heads are stored as element in an array with size N-1 using
 * the following schema:
 * array[X] = head of color X, where X goes from 0 to N-1
 */
typedef struct page_list_head color_list;
static color_list *color_heap;
static long total_avail_col_pages;
static u64 col_num_max;
static bool color_init_state = true;

static DEFINE_SPINLOCK(heap_lock);

#define page_to_head(pg) (&color_heap[color_from_page(pg)])
#define color_to_head(col) (&color_heap[col])

/* Add page in list in order depending on its physical address. */
static void page_list_add_order(struct page_info *pg, struct list_head *head)
{
    struct page_info *pos;

    /* Add first page after head */
    if ( page_list_empty(head) )
    {
        page_list_add(pg, head);
        return;
    }

    /* Add non-first page in list in ascending order */
    page_list_for_each_reverse(pos, head)
    {
        /* Get pg position */
        if ( page_to_maddr(pos) <= page_to_maddr(pg) )
        {
            /* Insert pg between pos and pos->list.next */
            page_list_add(pg, &pos->list);
            break;
        }

        /*
         * If pos is the first element it means that pg <= pos so we have
         * to insert pg after head.
         */
        if ( page_list_first(head) == pos )
        {
            page_list_add(pg, head);
            break;
        }
    }
}

/* Alloc one page based on domain color configuration */
static struct page_info *alloc_col_heap_page(
    unsigned int memflags, struct domain *d)
{
    struct page_info *pg, *tmp;
    bool need_tlbflush = false;
    uint32_t cur_color;
    uint32_t tlbflush_timestamp = 0;
    uint32_t *colors = 0;
    int max_colors;
    int i;

    colors = d->colors;
    max_colors = d->max_colors;

    spin_lock(&heap_lock);

    tmp = pg = NULL;

    /* Check for the first pg on non-empty list */
    for ( i = 0; i < max_colors; i++ )
    {
        if ( !page_list_empty(color_to_head(colors[i])) )
        {
            tmp = pg = page_list_last(color_to_head(colors[i]));
            cur_color = d->colors[i];
            break;
        }
    }

    /* If all lists are empty, no requests can be satisfied */
    if ( !pg )
    {
        spin_unlock(&heap_lock);
        return NULL;
    }

    /* Get the highest page from the lists compliant to the domain color(s) */
    for ( i += 1; i < max_colors; i++ )
    {
        if ( page_list_empty(color_to_head(colors[i])) )
        {
            C_DEBUG("List empty\n");
            continue;
        }
        tmp = page_list_last(color_to_head(colors[i]));
        if ( page_to_maddr(tmp) > page_to_maddr(pg) )
        {
            pg = tmp;
            cur_color = colors[i];
        }
    }

    if ( !pg )
    {
        spin_unlock(&heap_lock);
        return NULL;
    }

    pg->count_info = PGC_state_inuse;

    if ( !(memflags & MEMF_no_tlbflush) )
        accumulate_tlbflush(&need_tlbflush, pg,
                            &tlbflush_timestamp);

    /* Initialise fields which have other uses for free pages. */
    pg->u.inuse.type_info = 0;
    page_set_owner(pg, NULL);

    flush_page_to_ram(mfn_x(page_to_mfn(pg)),
                      !(memflags & MEMF_no_icache_flush));

    page_list_del(pg, page_to_head(pg));
    total_avail_col_pages--;

    spin_unlock(&heap_lock);

    if ( need_tlbflush )
        filtered_flush_tlb_mask(tlbflush_timestamp);

    return pg;
}

struct page_info *alloc_col_domheap_page(
    struct domain *d, unsigned int memflags)
{
    struct page_info *pg;

    ASSERT(!in_irq());

    /* Get page based on color selection */
    pg = alloc_col_heap_page(memflags, d);

    if ( !pg )
    {
        C_DEBUG("ERROR: Colored Page is null\n");
        return NULL;
    }

    /* Assign page to domain */
    if ( d && !(memflags & MEMF_no_owner) &&
        assign_pages(d, pg, 0, memflags) )
    {
        free_col_heap_page(pg);
        return NULL;
    }

    return pg;
}

void free_col_heap_page(struct page_info *pg)
{
    /* This page is not a guest frame any more. */
    pg->count_info = PGC_state_free;

    page_set_owner(pg, NULL);
    total_avail_col_pages++;
    page_list_add_order( pg, page_to_head(pg) );
}

bool init_col_heap_pages(struct page_info *pg, unsigned long nr_pages)
{
    int i;

    if ( color_init_state )
    {
        col_num_max = get_max_colors();
        color_heap = xmalloc_array(color_list, col_num_max);
        BUG_ON(!color_heap);

        for ( i = 0; i < col_num_max; i++ )
        {
            C_DEBUG("Init list for color: %u\n", i);
            INIT_PAGE_LIST_HEAD(&color_heap[i]);
        }

        color_init_state = false;
    }

    C_DEBUG("Init color heap pages with %lu pages for a given size of 0x%lx\n",
            nr_pages, nr_pages * PAGE_SIZE);
    C_DEBUG("Paging starting from: 0x%lx\n", page_to_maddr(pg));
    total_avail_col_pages += nr_pages;

    for ( i = 0; i < nr_pages; i++ )
    {
        pg->colored = true;
        page_list_add_order(pg, page_to_head(pg));
        pg++;
    }

    return true;
}

static void dump_col_heap(unsigned char key)
{
    struct page_info *pg;
    unsigned long size;
    unsigned int i;

    printk("Colored heap info\n");
    for ( i = 0; i < col_num_max; i++ )
    {
        printk("Heap[%u]: ", i);
        size = 0;
        page_list_for_each( pg, color_to_head(i) )
        {
            BUG_ON(!(color_from_page(pg) == i));
            size++;
        }
        printk("%lu pages -> %lukB free\n", size, size << (PAGE_SHIFT - 10));
    }

    printk("Total number of pages: %lu\n", total_avail_col_pages);
}

static __init int register_heap_trigger(void)
{
    register_keyhandler('c', dump_col_heap, "dump coloring heap info", 1);
    return 0;
}
__initcall(register_heap_trigger);

/*
 * Local variables:
 * mode: C
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * tab-width: 4
 * indent-tabs-mode: nil
 * End:
 */
