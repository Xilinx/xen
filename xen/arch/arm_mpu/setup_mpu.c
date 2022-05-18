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
#include <xen/mm.h>
#include <xen/pfn.h>
#include <asm/page.h>
#include <asm/setup.h>

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
