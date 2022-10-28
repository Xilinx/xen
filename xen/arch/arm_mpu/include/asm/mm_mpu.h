/*
 *
 * MPU based memory managment code for an Armv8-R AArch64.
 *
 * Copyright (C) 2022 Arm Ltd.
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
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef __ARCH_ARM_MM_MPU__
#define __ARCH_ARM_MM_MPU__

#include <xen/percpu.h>
#include <asm/arm64/mpu.h>

extern struct page_info* frame_table;
extern bool heap_parsed;

/* Helper to access MPU protection region */
extern void access_protection_region(bool read, pr_t *pr_read,
                                     const pr_t *pr_write, u64 sel);
/* MPU-related varaible */
extern pr_t *xen_mpumap;
extern unsigned long nr_xen_mpumap;
extern unsigned long max_xen_mpumap;

/* Boot-time MPU protection region configuration setup */
extern void setup_protection_regions(void);
extern void setup_staticheap_mappings(void);

#define setup_mm_data(x,y) setup_protection_regions()

DECLARE_PER_CPU(pr_t *, cpu_mpumap);
#define THIS_CPU_MPUMAP this_cpu(cpu_mpumap)
DECLARE_PER_CPU(unsigned long, nr_cpu_mpumap);
#define THIS_CPU_NR_MPUMAP this_cpu(nr_cpu_mpumap)

/* MPU-related functionality */
extern void enable_mm(void);
extern void disable_mm(void);
extern void set_boot_mpumap(u64 len, pr_t *table);
extern pr_t *alloc_mpumap(void);
extern void update_mm(void);
extern void map_guest_memory_section_on_boot(void);
extern void map_boot_module_section(void);
extern void disable_mpu_region_from_index(unsigned int index);
extern int reorder_xen_mpumap(void);
extern void clear_xen_mpumap(unsigned int len);

static inline paddr_t __virt_to_maddr(vaddr_t va)
{
    return (paddr_t)va;
}
#define virt_to_maddr(va)   __virt_to_maddr((vaddr_t)(va))

static inline void *maddr_to_virt(paddr_t ma)
{
    /* In MPU system, VA == PA. */
    return (void *)ma;
}

/*
 * In MPU system, VA == PA. Convert between Xen-heap liner addresses
 * to page-info structures.
 */
static inline struct page_info *virt_to_page(const void *v)
{
    unsigned long va = (unsigned long)v;
    unsigned long pdx;

    pdx = mfn_to_pdx(_mfn(va >> PAGE_SHIFT));

    return frame_table + pdx - frametable_base_pdx;
}

/*
 * We can't support VMAP on MPU systems, these stub-helpres for VMAP
 * will make building pass only.
 */
#define vm_init_type(type, start, end)  do {} while ( 0 )
#define __vmap(mfn, granularity, nr, align, flags, type) (NULL)
#define vunmap(va) do {} while ( 0 )
#define vmalloc(size) (NULL)
#define vmalloc_xen(size) (NULL)
#define vfree(va) do {} while ( 0 )
#define ioremap(pa, len) (NULL)
#define iounmap(va) do {} while ( 0 )
#define vm_init() do {} while ( 0 )

#endif /* __ARCH_ARM_MM_MPU__ */
