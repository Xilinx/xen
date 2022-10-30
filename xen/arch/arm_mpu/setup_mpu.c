/*
 * xen/arch/arm_mpu/setup_mpu.c
 *
 * Early bringup code for an Armv8-R with virt extensions.
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

#include <xen/init.h>
#include <xen/libfdt/libfdt.h>
#include <xen/mm.h>
#include <xen/pfn.h>
#include <asm/page.h>
#include <asm/setup.h>

static const char *mpu_section_info_str[MSINFO_MAX] = {
    "mpu,device-memory-section",
    "mpu,boot-module-section",
    "mpu,guest-memory-section",
};

static
unsigned long initial_module_mask[BITS_TO_LONGS(MAX_MPU_PROTECTION_REGIONS)];

void arch_init_finialize(void)
{
    unsigned int i = 0;

    reorder_xen_mpumap();
    /*
     * Initialize PER-PCPU runtime XEN MPU memory region configuration(
     * cpu_mpumap) with reordered xen_mpumap.
     */
    for_each_online_cpu( i )
    {
        pr_t* new_mpu;

        /* Boot cpu still holds the xen_mpumap. */
        if ( i == 0 )
        {
            per_cpu(cpu_mpumap, 0) = xen_mpumap;
            per_cpu(nr_cpu_mpumap, 0) = nr_xen_mpumap;
            continue;
        }

        new_mpu = alloc_mpumap();
        if ( !new_mpu )
            panic("Not enough space to allocate space for CPU%u MPU memory region configuration.\n", i);

        memcpy(new_mpu, xen_mpumap, nr_xen_mpumap * sizeof(pr_t));
        per_cpu(cpu_mpumap, i) = new_mpu;
        per_cpu(nr_cpu_mpumap, i) = nr_xen_mpumap;
    }
}

/*
 * In MPU system, due to limited MPU protection regions, we do not let user
 * scatter guest boot module(e.g. kernel image) wherever they want.
 *
 * "mpu,boot-module-section" is requested to define limited boot module
 * sections, then later all guest boot modules shall be placed inside boot
 * module sections, including kernel image boot module(BOOTMOD_KERNEL),
 * device tree passthrough binary(BOOTMOD_GUEST_DTB), ramdisk boot
 * module(BOOTMOD_RAMDISK).
 */
bool __init check_boot_module(bootmodule_kind kind,
                              paddr_t mod_start, paddr_t mod_size)
{
    paddr_t mod_end = (mod_start + mod_size) - 1;
    unsigned int i = 0;

    /*
     * Only boot modules of guest kernel image, guest ramdisk,
     * device assignment binary need to be checked.
     */
    if ( kind != BOOTMOD_KERNEL && kind != BOOTMOD_RAMDISK &&
         kind != BOOTMOD_GUEST_DTB )
        return true;

    for ( ; i < mpuinfo.sections[MSINFO_BOOT].nr_banks; i++ )
    {
        paddr_t section_start = mpuinfo.sections[MSINFO_BOOT].bank[i].start;
        paddr_t section_size = mpuinfo.sections[MSINFO_BOOT].bank[i].size;
        paddr_t section_end = section_start + section_size;

        /* guest boot module inclusive */
        if ( mod_start >= section_start && mod_end <= section_end )
            return true;
    }

    printk(XENLOG_ERR
           "guest boot module address invalid, and it shall be placed inside mpu boot module section\n");

    return false;
}

static void __init discard_initial_modules_one(void* data)
{
    unsigned int i;

    for_each_set_bit( i, (const unsigned long *)&initial_module_mask,
                      MAX_MPU_PROTECTION_REGIONS )
        disable_mpu_region_from_index(i);
}

void __init discard_initial_modules(void)
{
    unsigned int i = 0;

    /*
     * All boot modules on MPU system. except XEN module, must be located
     * inside boot module section, which was defined by device tree property
     * "mpu,boot-module-section".
     * Disable according MPU protection region, since it will be in
     * no use after boot.
     */
    for ( ; i < mpuinfo.sections[MSINFO_BOOT].nr_banks; i++ )
    {
        paddr_t start = round_pgup(
                        mpuinfo.sections[MSINFO_BOOT].bank[i].start);
        paddr_t size = mpuinfo.sections[MSINFO_BOOT].bank[i].size;
        paddr_t end = round_pgdown(start + size) - 1;
        int rc;

        rc = destroy_xen_mappings(start, end);
        if ( rc < 0 )
            panic("Unable to destroy boot module section %"PRIpaddr"-%"PRIpaddr".\n",
                  start, end);
        set_bit(rc, initial_module_mask);
    }

    smp_call_function(discard_initial_modules_one, NULL, 1);
}

void __init setup_mm(void)
{
    paddr_t ram_start = ~0;
    paddr_t ram_end = 0;
    paddr_t ram_size = 0;
    unsigned int bank;

    init_pdx();

    populate_boot_allocator();

    total_pages = 0;
    for ( bank = 0 ; bank < bootinfo.mem.nr_banks; bank++ )
    {
        paddr_t bank_start = round_pgup(bootinfo.mem.bank[bank].start);
        paddr_t bank_size = bootinfo.mem.bank[bank].size;
        paddr_t bank_end = round_pgdown(bank_start + bank_size);

        ram_size = ram_size + bank_size;
        ram_start = min(ram_start, bank_start);
        ram_end = max(ram_end, bank_end);
    }

    /*
     * RAM usasge on MPU system must be statically configured in Device Tree,
     * so we will set up MPU memory regions components by components, not
     * a whole directmap mapping, like we did in MMU system.
     */
    setup_staticheap_mappings();

    total_pages += ram_size >> PAGE_SHIFT;
    max_page = PFN_DOWN(ram_end);

    setup_frametable_mappings(ram_start, ram_end);

    /* Map guest memory section before initialization */
    map_guest_memory_section_on_boot();
    init_staticmem_pages();
}

/*
 * In MPU system, due to limited MPU protection regions and predictable
 * static behavior, we prefer statically configured system resource
 * through device tree.
 * "mpu,boot-module-section": limited boot module section in which guest
 * boot module section (e.g. kernel image boot module) shall be placed.
 * "mpu,guest-memory-section": limited guest memory section in which statically
 * configured guest RAM shall be placed.
 * "mpu,device-memory-section": limited device memory section in which all
 * system device shall be included.
 */
static int __init process_mpu_section(const void *fdt, int node,
                                      const char *name, void *data,
                                      uint32_t address_cells,
                                      uint32_t size_cells)
{
    if ( !fdt_get_property(fdt, node, name, NULL) )
        return -EINVAL;

    return device_tree_get_meminfo(fdt, node, name, address_cells, size_cells,
                                   data, MEMBANK_DEFAULT);
}

int __init arch_process_chosen_node(const void *fdt, int node)

{
    uint32_t address_cells, size_cells;
    uint8_t idx;
    const char *prop_name;

    address_cells = device_tree_get_u32(fdt, node, "#mpu,address-cells", 0);
    size_cells = device_tree_get_u32(fdt, node, "#mpu,size-cells", 0);
    if ( (address_cells == 0) || (size_cells == 0) )
    {
         printk("Missing \"#mpu,address-cells\" or \"#mpu,size-cells\".\n");
         return -EINVAL;
    }

    for ( idx = 0; idx < MSINFO_MAX; idx++ )
    {
        prop_name = mpu_section_info_str[idx];
        printk(XENLOG_DEBUG "Checking for %s in /chosen\n", prop_name);
        if ( process_mpu_section(fdt, node, prop_name, &mpuinfo.sections[idx],
                                 address_cells, size_cells) )
        {
            printk(XENLOG_ERR "%s not present.\n", prop_name);
            return -EINVAL;
        }
    }

    return 0;
}

/*
 * CPU errata depends on Xen alternative framework, and alternative
 * framework depends on VMAP. As VMAP could not be support for MPU
 * systems, we disable CPU errata for MPU systems currently.
 *
 * Stub check_local_cpu_errata to make common code flow unchanged.
 */
void check_local_cpu_errata(void)
{

}
