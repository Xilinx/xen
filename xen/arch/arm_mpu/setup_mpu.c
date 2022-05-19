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
};

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
