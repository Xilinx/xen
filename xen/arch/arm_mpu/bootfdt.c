/*
 * Early Device Tree
 *
 * Copyright (C) 2012-2014 Citrix Systems, Inc.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */
#include <xen/types.h>
#include <xen/lib.h>
#include <xen/kernel.h>
#include <xen/init.h>
#include <xen/efi.h>
#include <xen/device_tree.h>
#include <xen/lib.h>
#include <xen/libfdt/libfdt.h>
#include <xen/sort.h>
#include <xsm/xsm.h>
#include <asm/setup.h>

static bool __init device_tree_node_matches(const void *fdt, int node,
                                            const char *match)
{
    const char *name;
    size_t match_len;

    name = fdt_get_name(fdt, node, NULL);
    match_len = strlen(match);

    /* Match both "match" and "match@..." patterns but not
       "match-foo". */
    return strncmp(name, match, match_len) == 0
        && (name[match_len] == '@' || name[match_len] == '\0');
}

static bool __init device_tree_node_compatible(const void *fdt, int node,
                                               const char *match)
{
    int len, l;
    const void *prop;

    prop = fdt_getprop(fdt, node, "compatible", &len);
    if ( prop == NULL )
        return false;

    while ( len > 0 ) {
        if ( !dt_compat_cmp(prop, match) )
            return true;
        l = strlen(prop) + 1;
        prop += l;
        len -= l;
    }

    return false;
}

void __init device_tree_get_reg(const __be32 **cell, u32 address_cells,
                                u32 size_cells, u64 *start, u64 *size)
{
    *start = dt_next_cell(address_cells, cell);
    *size = dt_next_cell(size_cells, cell);
}

int __init device_tree_get_meminfo(const void *fdt, int node,
                                   const char *prop_name,
                                   u32 address_cells, u32 size_cells,
                                   void *data, enum membank_type type)
{
    const struct fdt_property *prop;
    unsigned int i, banks;
    const __be32 *cell;
    u32 reg_cells = address_cells + size_cells;
    paddr_t start, size;
    struct meminfo *mem = data;

    if ( address_cells < 1 || size_cells < 1 )
    {
        printk("fdt: property `%s': invalid #address-cells or #size-cells",
               prop_name);
        return -EINVAL;
    }

    prop = fdt_get_property(fdt, node, prop_name, NULL);
    if ( !prop )
        return -ENOENT;

    cell = (const __be32 *)prop->data;
    banks = fdt32_to_cpu(prop->len) / (reg_cells * sizeof (u32));

    for ( i = 0; i < banks && mem->nr_banks < NR_MEM_BANKS; i++ )
    {
        int j, cont;

        device_tree_get_reg(&cell, address_cells, size_cells, &start, &size);
        /* Some DT may describe empty bank, ignore them */
        if ( !size )
            continue;

        for ( j = 0, cont = 0; j < mem->nr_banks; j++ )
        {
            if ( mem->bank[j].start == start && mem->bank[j].size == size )
            {
                cont = 1;
            }
        }
        if ( cont )
            continue;

        mem->bank[mem->nr_banks].start = start;
        mem->bank[mem->nr_banks].size = size;
        mem->bank[mem->nr_banks].type = type;
        mem->nr_banks++;
    }

    if ( i < banks )
    {
        printk("Warning: Max number of supported memory regions reached.\n");
        return -ENOSPC;
    }

    return 0;
}

u32 __init device_tree_get_u32(const void *fdt, int node,
                               const char *prop_name, u32 dflt)
{
    const struct fdt_property *prop;

    prop = fdt_get_property(fdt, node, prop_name, NULL);
    if ( !prop || prop->len < sizeof(u32) )
        return dflt;

    return fdt32_to_cpu(*(uint32_t*)prop->data);
}

/**
 * device_tree_for_each_node - iterate over all device tree sub-nodes
 * @fdt: flat device tree.
 * @node: parent node to start the search from
 * @func: function to call for each sub-node.
 * @data: data to pass to @func.
 *
 * Any nodes nested at DEVICE_TREE_MAX_DEPTH or deeper are ignored.
 *
 * Returns 0 if all nodes were iterated over successfully.  If @func
 * returns a value different from 0, that value is returned immediately.
 */
int __init device_tree_for_each_node(const void *fdt, int node,
                                     device_tree_node_func func,
                                     void *data)
{
    /*
     * We only care about relative depth increments, assume depth of
     * node is 0 for simplicity.
     */
    int depth = 0;
    const int first_node = node;
    u32 address_cells[DEVICE_TREE_MAX_DEPTH];
    u32 size_cells[DEVICE_TREE_MAX_DEPTH];
    int ret;

    do {
        const char *name = fdt_get_name(fdt, node, NULL);
        u32 as, ss;

        if ( depth >= DEVICE_TREE_MAX_DEPTH )
        {
            printk("Warning: device tree node `%s' is nested too deep\n",
                   name);
            continue;
        }

        as = depth > 0 ? address_cells[depth-1] : DT_ROOT_NODE_ADDR_CELLS_DEFAULT;
        ss = depth > 0 ? size_cells[depth-1] : DT_ROOT_NODE_SIZE_CELLS_DEFAULT;

        address_cells[depth] = device_tree_get_u32(fdt, node,
                                                   "#address-cells", as);
        size_cells[depth] = device_tree_get_u32(fdt, node,
                                                "#size-cells", ss);

        /* skip the first node */
        if ( node != first_node )
        {
            ret = func(fdt, node, name, depth, as, ss, data);
            if ( ret != 0 )
                return ret;
        }

        node = fdt_next_node(fdt, node, &depth);
    } while ( node >= 0 && depth > 0 );

    return 0;
}

static int __init process_memory_node(const void *fdt, int node,
                                      const char *name, int depth,
                                      u32 address_cells, u32 size_cells,
                                      void *data)
{
    return device_tree_get_meminfo(fdt, node, "reg", address_cells, size_cells,
                                   data, MEMBANK_DEFAULT);
}

static int __init process_reserved_memory_node(const void *fdt, int node,
                                               const char *name, int depth,
                                               u32 address_cells,
                                               u32 size_cells,
                                               void *data)
{
    int rc = process_memory_node(fdt, node, name, depth, address_cells,
                                 size_cells, data);

    if ( rc == -ENOSPC )
        panic("Max number of supported reserved-memory regions reached.");
    else if ( rc != -ENOENT )
        return rc;
    return 0;
}

static int __init process_reserved_memory(const void *fdt, int node,
                                          const char *name, int depth,
                                          u32 address_cells, u32 size_cells)
{
    return device_tree_for_each_node(fdt, node,
                                     process_reserved_memory_node,
                                     &bootinfo.reserved_mem);
}

static void __init process_multiboot_node(const void *fdt, int node,
                                          const char *name,
                                          u32 address_cells, u32 size_cells)
{
    static int __initdata kind_guess = 0;
    const struct fdt_property *prop;
    const __be32 *cell;
    bootmodule_kind kind;
    paddr_t start, size;
    int len;
    /* sizeof("/chosen/") + DT_MAX_NAME + '/' + DT_MAX_NAME + '/0' => 92 */
    char path[92];
    int parent_node, ret;
    bool domU;

    parent_node = fdt_parent_offset(fdt, node);
    ASSERT(parent_node >= 0);

    /* Check that the node is under "/chosen" (first 7 chars of path) */
    ret = fdt_get_path(fdt, node, path, sizeof (path));
    if ( ret != 0 || strncmp(path, "/chosen", 7) )
        return;

    prop = fdt_get_property(fdt, node, "reg", &len);
    if ( !prop )
        panic("node %s missing `reg' property\n", name);

    if ( len < dt_cells_to_size(address_cells + size_cells) )
        panic("fdt: node `%s': `reg` property length is too short\n",
                    name);

    cell = (const __be32 *)prop->data;
    device_tree_get_reg(&cell, address_cells, size_cells, &start, &size);

    if ( fdt_node_check_compatible(fdt, node, "xen,linux-zimage") == 0 ||
         fdt_node_check_compatible(fdt, node, "multiboot,kernel") == 0 )
        kind = BOOTMOD_KERNEL;
    else if ( fdt_node_check_compatible(fdt, node, "xen,linux-initrd") == 0 ||
              fdt_node_check_compatible(fdt, node, "multiboot,ramdisk") == 0 )
        kind = BOOTMOD_RAMDISK;
    else if ( fdt_node_check_compatible(fdt, node, "xen,xsm-policy") == 0 )
        kind = BOOTMOD_XSM;
    else if ( fdt_node_check_compatible(fdt, node, "multiboot,device-tree") == 0 )
        kind = BOOTMOD_GUEST_DTB;
    else
        kind = BOOTMOD_UNKNOWN;

    /**
     * Guess the kind of these first two unknowns respectively:
     * (1) The first unknown must be kernel.
     * (2) Detect the XSM Magic from the 2nd unknown:
     *     a. If it's XSM, set the kind as XSM, and that also means we
     *     won't load ramdisk;
     *     b. if it's not XSM, set the kind as ramdisk.
     *     So if user want to load ramdisk, it must be the 2nd unknown.
     * We also detect the XSM Magic for the following unknowns,
     * then set its kind according to the return value of has_xsm_magic.
     */
    if ( kind == BOOTMOD_UNKNOWN )
    {
        switch ( kind_guess++ )
        {
        case 0: kind = BOOTMOD_KERNEL; break;
        case 1: kind = BOOTMOD_RAMDISK; break;
        default: break;
        }
        if ( kind_guess > 1 && has_xsm_magic(start) )
            kind = BOOTMOD_XSM;
    }

    domU = fdt_node_check_compatible(fdt, parent_node, "xen,domain") == 0;
    add_boot_module(kind, start, size, domU);

    prop = fdt_get_property(fdt, node, "bootargs", &len);
    if ( !prop )
        return;
    add_boot_cmdline(fdt_get_name(fdt, parent_node, &len), prop->data,
                     kind, start, domU);
}

static int __init process_chosen_node(const void *fdt, int node,
                                      const char *name,
                                      u32 address_cells, u32 size_cells)
{
    const struct fdt_property *prop;
    paddr_t start, end;
    int len;

    if ( fdt_get_property(fdt, node, "xen,static-heap", NULL) )
    {
        int rc;

        printk("Checking for static heap in /chosen\n");

        rc = device_tree_get_meminfo(fdt, node, "xen,static-heap",
                                     address_cells, size_cells,
                                     &bootinfo.reserved_mem,
                                     MEMBANK_STATIC_HEAP);
        if ( rc )
            return rc;

        bootinfo.static_heap = true;
    }

    if ( arch_process_chosen_node(fdt, node) )
        return -EINVAL;

    printk("Checking for initrd in /chosen\n");

    prop = fdt_get_property(fdt, node, "linux,initrd-start", &len);
    if ( !prop )
        /* No initrd present. */
        return 0;
    if ( len != sizeof(u32) && len != sizeof(u64) )
    {
        printk("linux,initrd-start property has invalid length %d\n", len);
        return -EINVAL;
    }
    start = dt_read_number((void *)&prop->data, dt_size_to_cells(len));

    prop = fdt_get_property(fdt, node, "linux,initrd-end", &len);
    if ( !prop )
    {
        printk("linux,initrd-end not present but -start was\n");
        return -EINVAL;
    }
    if ( len != sizeof(u32) && len != sizeof(u64) )
    {
        printk("linux,initrd-end property has invalid length %d\n", len);
        return -EINVAL;
    }
    end = dt_read_number((void *)&prop->data, dt_size_to_cells(len));

    if ( start >= end )
    {
        printk("linux,initrd limits invalid: %"PRIpaddr" >= %"PRIpaddr"\n",
                  start, end);
        return -EINVAL;
    }

    printk("Initrd %"PRIpaddr"-%"PRIpaddr"\n", start, end);

    add_boot_module(BOOTMOD_RAMDISK, start, end-start, false);

    return 0;
}

static int __init process_domain_node(const void *fdt, int node,
                                      const char *name,
                                      u32 address_cells, u32 size_cells)
{
    const struct fdt_property *prop;

    printk("Checking for \"xen,static-mem\" in domain node\n");

    prop = fdt_get_property(fdt, node, "xen,static-mem", NULL);
    if ( !prop )
        /* No "xen,static-mem" present. */
        return 0;

    return device_tree_get_meminfo(fdt, node, "xen,static-mem", address_cells,
                                   size_cells, &bootinfo.reserved_mem,
                                   MEMBANK_STATIC_DOMAIN);
}

#ifdef CONFIG_STATIC_SHM
static int __init process_shm_node(const void *fdt, int node,
                                   uint32_t address_cells, uint32_t size_cells)
{
    const struct fdt_property *prop, *prop_id, *prop_role;
    const __be32 *cell;
    paddr_t paddr, gaddr, size;
    struct meminfo *mem = &bootinfo.reserved_mem;
    unsigned int i;
    int len;
    bool owner = false;
    const char *shm_id;

    if ( address_cells < 1 || size_cells < 1 )
    {
        printk("fdt: invalid #address-cells or #size-cells for static shared memory node.\n");
        return -EINVAL;
    }

    /*
     * "xen,shm-id" property holds an arbitrary string with a strict limit
     * on the number of characters, MAX_SHM_ID_LENGTH
     */
    prop_id = fdt_get_property(fdt, node, "xen,shm-id", NULL);
    if ( !prop_id )
        return -ENOENT;
    shm_id = (const char *)prop_id->data;
    if ( strnlen(shm_id, MAX_SHM_ID_LENGTH) == MAX_SHM_ID_LENGTH )
    {
        printk("fdt: invalid xen,shm-id %s, it must be limited to %u characters\n",
               shm_id, MAX_SHM_ID_LENGTH);
        return -EINVAL;
    }

    /*
     * "role" property is optional and if it is defined explicitly,
     * it must be either `owner` or `borrower`.
     */
    prop_role = fdt_get_property(fdt, node, "role", NULL);
    if ( prop_role )
    {
        if ( !strcmp(prop_role->data, "owner") )
            owner = true;
        else if ( strcmp(prop_role->data, "borrower") )
        {
            printk("fdt: invalid `role` property for static shared memory node.\n");
            return -EINVAL;
        }
    }

    /*
     * xen,shared-mem = <paddr, gaddr, size>;
     * Memory region starting from physical address #paddr of #size shall
     * be mapped to guest physical address #gaddr as static shared memory
     * region.
     */
    prop = fdt_get_property(fdt, node, "xen,shared-mem", &len);
    if ( !prop )
        return -ENOENT;

    if ( len != dt_cells_to_size(address_cells + size_cells + address_cells) )
    {
        if ( len == dt_cells_to_size(size_cells + address_cells) )
            printk("fdt: host physical address must be chosen by users at the moment.\n");

        printk("fdt: invalid `xen,shared-mem` property.\n");
        return -EINVAL;
    }

    cell = (const __be32 *)prop->data;
    device_tree_get_reg(&cell, address_cells, address_cells, &paddr, &gaddr);
    size = dt_next_cell(size_cells, &cell);

    if ( !size )
    {
        printk("fdt: the size for static shared memory region can not be zero\n");
        return -EINVAL;
    }

    for ( i = 0; i < mem->nr_banks; i++ )
    {
        /*
         * Meet the following check:
         * 1) The shm ID matches and the region exactly match
         * 2) The shm ID doesn't match and the region doesn't overlap
         * with an existing one
         */
        if ( paddr == mem->bank[i].start && size == mem->bank[i].size )
        {
            if ( strncmp(shm_id, mem->bank[i].shm_id, MAX_SHM_ID_LENGTH) == 0 )
                break;
            else
            {
                printk("fdt: xen,shm-id %s does not match for all the nodes using the same region.\n",
                       shm_id);
                return -EINVAL;
            }
        }
        else
        {
            paddr_t end = paddr + size;
            paddr_t bank_end = mem->bank[i].start + mem->bank[i].size;

            if ( (end <= paddr) || (bank_end <= mem->bank[i].start) )
            {
                printk("fdt: static shared memory region %s overflow\n", shm_id);
                return -EINVAL;
            }

            if ( (end <= mem->bank[i].start) || (paddr >= bank_end) )
            {
                if ( strcmp(shm_id, mem->bank[i].shm_id) != 0 )
                    continue;
                else
                {
                    printk("fdt: different shared memory region could not share the same shm ID %s\n",
                           shm_id);
                    return -EINVAL;
                }
            }
            else
            {
                printk("fdt: shared memory region overlap with an existing entry %#"PRIpaddr" - %#"PRIpaddr"\n",
                        mem->bank[i].start, bank_end);
                return -EINVAL;
            }
        }
    }

    if ( i == mem->nr_banks )
    {
        if ( i < NR_MEM_BANKS )
        {
            /* Static shared memory shall be reserved from any other use. */
            safe_strcpy(mem->bank[mem->nr_banks].shm_id, shm_id);
            mem->bank[mem->nr_banks].start = paddr;
            mem->bank[mem->nr_banks].size = size;
            mem->bank[mem->nr_banks].type = MEMBANK_STATIC_DOMAIN;
            mem->nr_banks++;
        }
        else
        {
            printk("Warning: Max number of supported memory regions reached.\n");
            return -ENOSPC;
        }
    }
    /*
     * keep a count of the number of borrowers, which later may be used
     * to calculate the reference count.
     */
    if ( !owner )
        mem->bank[i].nr_shm_borrowers++;

    return 0;
}
#else
static int __init process_shm_node(const void *fdt, int node,
                                   uint32_t address_cells, uint32_t size_cells)
{
    printk("CONFIG_STATIC_SHM must be enabled for parsing static shared memory nodes\n");
    return -EINVAL;
}
#endif

static int __init early_scan_node(const void *fdt,
                                  int node, const char *name, int depth,
                                  u32 address_cells, u32 size_cells,
                                  void *data)
{
    int rc = 0;

    /*
     * If Xen has been booted via UEFI, the memory banks are
     * populated. So we should skip the parsing.
     */
    if ( !efi_enabled(EFI_BOOT) &&
         device_tree_node_matches(fdt, node, "memory") )
        rc = process_memory_node(fdt, node, name, depth,
                                 address_cells, size_cells, &bootinfo.mem);
    else if ( depth == 1 && !dt_node_cmp(name, "reserved-memory") )
        rc = process_reserved_memory(fdt, node, name, depth,
                                     address_cells, size_cells);
    else if ( depth <= 3 && (device_tree_node_compatible(fdt, node, "xen,multiboot-module" ) ||
              device_tree_node_compatible(fdt, node, "multiboot,module" )))
        process_multiboot_node(fdt, node, name, address_cells, size_cells);
    else if ( depth == 1 && device_tree_node_matches(fdt, node, "chosen") )
        rc = process_chosen_node(fdt, node, name, address_cells, size_cells);
    else if ( depth == 2 && device_tree_node_compatible(fdt, node, "xen,domain") )
        rc = process_domain_node(fdt, node, name, address_cells, size_cells);
    else if ( depth <= 3 && device_tree_node_compatible(fdt, node, "xen,domain-shared-memory-v1") )
        rc = process_shm_node(fdt, node, address_cells, size_cells);

    if ( rc < 0 )
        printk("fdt: node `%s': parsing failed\n", name);
    return rc;
}

static void __init early_print_info(void)
{
    struct meminfo *mi = &bootinfo.mem;
    struct meminfo *mem_resv = &bootinfo.reserved_mem;
    struct bootmodules *mods = &bootinfo.modules;
    struct bootcmdlines *cmds = &bootinfo.cmdlines;
    unsigned int i, j, nr_rsvd;

    for ( i = 0; i < mi->nr_banks; i++ )
        printk("RAM: %"PRIpaddr" - %"PRIpaddr"\n",
                mi->bank[i].start,
                mi->bank[i].start + mi->bank[i].size - 1);
    printk("\n");
    for ( i = 0 ; i < mods->nr_mods; i++ )
        printk("MODULE[%d]: %"PRIpaddr" - %"PRIpaddr" %-12s\n",
                i,
                mods->module[i].start,
                mods->module[i].start + mods->module[i].size,
                boot_module_kind_as_string(mods->module[i].kind));

    nr_rsvd = fdt_num_mem_rsv(device_tree_flattened);
    for ( i = 0; i < nr_rsvd; i++ )
    {
        paddr_t s, e;
        if ( fdt_get_mem_rsv(device_tree_flattened, i, &s, &e) < 0 )
            continue;
        /* fdt_get_mem_rsv returns length */
        e += s;
        printk(" RESVD[%u]: %"PRIpaddr" - %"PRIpaddr"\n", i, s, e);
    }
    for ( j = 0; j < mem_resv->nr_banks; j++, i++ )
    {
        printk(" RESVD[%u]: %"PRIpaddr" - %"PRIpaddr"\n", i,
               mem_resv->bank[j].start,
               mem_resv->bank[j].start + mem_resv->bank[j].size - 1);
    }
    printk("\n");
    for ( i = 0 ; i < cmds->nr_mods; i++ )
        printk("CMDLINE[%"PRIpaddr"]:%s %s\n", cmds->cmdline[i].start,
               cmds->cmdline[i].dt_name,
               &cmds->cmdline[i].cmdline[0]);
    printk("\n");
}

/* This function assumes that memory regions are not overlapped */
static int __init cmp_memory_node(const void *key, const void *elem)
{
    const struct membank *handler0 = key;
    const struct membank *handler1 = elem;

    if ( handler0->start < handler1->start )
        return -1;

    if ( handler0->start >= (handler1->start + handler1->size) )
        return 1;

    return 0;
}

static void __init swap_memory_node(void *_a, void *_b, size_t size)
{
    struct membank *a = _a, *b = _b;

    SWAP(*a, *b);
}

/**
 * boot_fdt_info - initialize bootinfo from a DTB
 * @fdt: flattened device tree binary
 *
 * Returns the size of the DTB.
 */
size_t __init boot_fdt_info(const void *fdt, paddr_t paddr)
{
    int ret;

    ret = fdt_check_header(fdt);
    if ( ret < 0 )
        panic("No valid device tree\n");

    add_boot_module(BOOTMOD_FDT, paddr, fdt_totalsize(fdt), false);

    device_tree_for_each_node((void *)fdt, 0, early_scan_node, NULL);

    /*
     * On Arm64 setup_directmap_mappings() expects to be called with the lowest
     * bank in memory first. There is no requirement that the DT will provide
     * the banks sorted in ascending order. So sort them through.
     */
    sort(bootinfo.mem.bank, bootinfo.mem.nr_banks, sizeof(struct membank),
         cmp_memory_node, swap_memory_node);

    early_print_info();

    return fdt_totalsize(fdt);
}

const __init char *boot_fdt_cmdline(const void *fdt)
{
    int node;
    const struct fdt_property *prop;

    node = fdt_path_offset(fdt, "/chosen");
    if ( node < 0 )
        return NULL;

    prop = fdt_get_property(fdt, node, "xen,xen-bootargs", NULL);
    if ( prop == NULL )
    {
        struct bootcmdline *dom0_cmdline =
            boot_cmdline_find_by_kind(BOOTMOD_KERNEL);

        if (fdt_get_property(fdt, node, "xen,dom0-bootargs", NULL) ||
            ( dom0_cmdline && dom0_cmdline->cmdline[0] ) )
            prop = fdt_get_property(fdt, node, "bootargs", NULL);
    }
    if ( prop == NULL )
        return NULL;

    return prop->data;
}

/*
 * Local variables:
 * mode: C
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * End:
 */
