/* SPDX-License-Identifier: GPL-2.0-only */

#include <xen/types.h>
#include <xen/device_tree.h>
#include <xen/libfdt/libfdt.h>

#include <asm/bootinfo.h>

static int __init cf_check process_module(const void *fdt, int node,
                                          const char *name, int depth,
                                          uint32_t address_cells,
                                          uint32_t size_cells, void *data)
{
    struct boot_info *bi = data;
    uint32_t idx;

    if ( fdt_node_check_compatible(fdt, node, "multiboot,module") )
        return 0;

    idx = device_tree_get_u32(fdt, node, "module-index", MAX_NR_BOOTMODS);
    if ( idx >= MAX_NR_BOOTMODS )
        panic("module-index=%u of %s overflows MAX_NR_BOOTMODS=%u.\n",
              idx, name, MAX_NR_BOOTMODS);

    bi->mods[idx].kind = fdt_node_to_kind(fdt, node);

    printk(XENLOG_DEBUG " %s kind=%s module-index=%u\n",
           name, boot_module_kind_as_string(bi->mods[idx].kind), idx);

    return 0;
}

void __init fdt_identify_module_kinds(struct boot_info *bi)
{
    void *fdt = bootstrap_map_bm(&bi->mods[0]);

    if ( fdt && !fdt_check_header(fdt) )
    {
        int node = fdt_path_offset(fdt, "/chosen/hypervisor");

        if ( node < 0 )
        {
            node = fdt_path_offset(fdt, "/chosen");
            if ( node < 0 )
                panic("Malformed DTB in mod0: /chosen not found");
        }

        bi->mods[0].kind = BOOTMOD_FDT;
        device_tree_for_each_node(fdt, node, process_module, bi);
    }

    if ( fdt )
        bootstrap_unmap();
}
