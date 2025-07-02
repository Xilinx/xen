/* SPDX-License-Identifier: GPL-2.0-only */

#include <xen/types.h>
#include <xen/device_tree.h>
#include <xen/dom0less-build.h>
#include <xen/libfdt/libfdt.h>

#include <asm/bootinfo.h>
#include <asm/setup.h>

static struct boot_module *__init find_boot_module(
    struct boot_info *bi, struct dt_device_node *dom_node,
    const char *compatible)
{
    uint32_t i;
    struct dt_device_node *node = dt_find_compatible_node(dom_node, NULL,
                                                          compatible);

    if ( !node )
        return NULL;

    if ( !dt_property_read_u32(node, "module-index", &i) )
    {
        /* No module-index. Find out via "reg" */
        const __be32 *prop = dt_get_property(node, "reg", NULL);
        uint64_t addr, size;

        if ( !prop )
        {
            printk(XENLOG_ERR "%s.%s missing both module-index and reg\n",
                   dom_node->name, node->name);

            return NULL;
        }

        dt_get_range(&prop, node, &addr, &size);

        for ( i = 0; i < bi->nr_modules; i++ )
        {
            if ( bi->mods[i].start == addr )
                goto found;
        }


        return NULL;
    }

 found:
    return &bi->mods[i];
}

int __init arch_parse_dom0less_node(struct dt_device_node *node,
                                    struct boot_domain *bd)
{
    return 0;
}

static int __init cf_check process_module(const void *fdt, int node,
                                          const char *name, int depth,
                                          uint32_t address_cells,
                                          uint32_t size_cells, void *data)
{
    struct boot_info *bi = data;
    uint32_t idx;
    const struct fdt_property *prop;
    paddr_t start, size;
    const __be32 *cell;
    int len;

    if ( fdt_node_check_compatible(fdt, node, "multiboot,module") )
        return 0;

    prop = fdt_get_property(fdt, node, "module-index", NULL);
    if ( prop )
    {
        idx = device_tree_get_u32(fdt, node, "module-index", MAX_NR_BOOTMODS);
        if ( idx >= MAX_NR_BOOTMODS )
            panic("module-index=%u of %s overflows MAX_NR_BOOTMODS=%u.\n",
                  idx, name, MAX_NR_BOOTMODS);

        bi->mods[idx].kind = fdt_node_to_kind(fdt, node);

        printk(XENLOG_DEBUG " %s kind=%s module-index=%u\n",
               name, boot_module_kind_as_string(bi->mods[idx].kind), idx);

        return 0;
    }

    /*
     * Module wasn't passed along with multiboot. Needs
     * adding to the list if it isn't already
     */

    prop = fdt_get_property(fdt, node, "reg", &len);
    if ( !prop )
        panic("module \"%s\" missing both module-index and reg.\n", name);

    if ( len < dt_cells_to_size(address_cells + size_cells) )
        panic("malformed reg property of %s", name);

    cell = (const __be32 *)prop->data;
    device_tree_get_reg(&cell, address_cells, size_cells, &start, &size);

    /* Ensure it's not already in the module list */
    for ( idx = 0; idx < bi->nr_modules; idx++ )
        if ( bi->mods[idx].start == start )
            panic("module overflow. can't add reg-provided module %s.\n", name);

    if ( bi->nr_modules >= MAX_NR_BOOTMODS )
        panic("builder: module overflow. can't add reg-provided module %s.\n",
              name);

    bi->mods[idx].start = start;
    bi->mods[idx].size = size;
    bi->mods[idx].kind = fdt_node_to_kind(fdt, node);

    bi->nr_modules++;

    printk(XENLOG_DEBUG " %s kind=%s start=%#08lx sz=%08lx\n",
           name, boot_module_kind_as_string(bi->mods[idx].kind),
           bi->mods[idx].start, bi->mods[idx].size);

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

void __init dt_parse_domains(struct boot_info *bi)
{
    struct boot_module *dtb = &bi->mods[0];
    struct boot_domain *bd = &bi->domains[bi->nr_domains];
    struct dt_device_node *root, *node;

    ASSERT(dtb->kind == BOOTMOD_FDT);

    device_tree_flattened = maddr_to_virt(dtb->start);
    dt_unflatten_host_device_tree();

    root = dt_find_node_by_path("/chosen/hypervisor");
    if ( !root )
        root = dt_find_node_by_path("/chosen");

    dt_for_each_child_node(root, node)
    {
        int rc = parse_dom0less_node(node, bd);

        if ( rc )
        {
            if ( rc != -ENOENT )
                printk(XENLOG_WARNING "builder %s rc=%d: domain ignored\n",
                       dt_node_name(node), rc);
            continue;
        }

        if ( bi->nr_domains >= MAX_NR_BOOTDOMS )
        {
            printk(XENLOG_ERR "builder: only creating first %u domains\n",
                   MAX_NR_BOOTDOMS);
            break;
        }

        bd->initrd = find_boot_module(bi, node, "multiboot,ramdisk");
        bd->kernel = find_boot_module(bi, node, "multiboot,kernel");
        if ( !bd->kernel )
        {
            printk(XENLOG_WARNING "builder %s: missing kernel (ignored)\n",
                   node->name);
            return;
        }

        bi->nr_domains++;
        bd++;
    }
}
