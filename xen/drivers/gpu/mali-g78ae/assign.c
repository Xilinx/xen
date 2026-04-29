/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Mali G78AE GPU PTM Assign driver
 *
 * Copyright (C) 2025, Advanced Micro Devices, Inc.
 */

#include <xen/device_tree.h>
#include <xen/errno.h>
#include <xen/init.h>
#include <xen/lib.h>
#include <xen/mm.h>
#include <xen/param.h>
#include <xen/vmap.h>
#include <asm/io.h>

#include "assign.h"
#include "device-tree.h"

#define PTM_ASSIGN_RESOURCE_GROUP_BUS (0x0040)
#define PTM_ASSIGN_PARTITION_RESOURCE_GROUP (0x0044)
#define PTM_ASSIGN_SLICE_RESOURCE_GROUP (0x0048)
#define PTM_ASSIGN_SLICE_ISOLATION_SET (0x0050)
#define PTM_ASSIGN_AW_RESOURCE_GROUP (0x0080) /* 128 bits */

#define PTM_UNIT_FEATURES_PARTITIONS_OFFSET (0)
#define PTM_UNIT_FEATURES_PARTITIONS_MASK (0x0F)
#define PTM_UNIT_FEATURES_AWS_OFFSET (8)
#define PTM_UNIT_FEATURES_AWS_MASK (0x3F)

#define PTM_SLICE_CORES_CORE_BITS (8)
#define PTM_SLICE_CORES_CORE_MASK (0xFF)

#define AWS_PER_DWORD (8)
#define GROUP_BITS (4)
#define GROUP_MASK (0x3)
#define BUS_BITS (4)
#define BUS_MASK (0x1)
#define ISOLATION_BITS (1)
#define ISOLATION_MASK (0x1)

/* Define how big is the dmesg buffer to represent the assign configuration */
#define PRINT_BUFF_SIZE (512)

/*
 * ptm_config: string parsed to assign GPU resources from the command line
 * Example:
 * ptm_config='A:S0:S1:S2:P0:P1:W0:W1,B:S3:P2:W2'
 * This string correspond to the following configuration:
 * - first group assigned to Bus A with slices 0, 1, 2; partition 0, 1
 * and access windows 0, 1
 * - second group assigned to Bus B with slice 3; partition 2 and
 * access window 2
 */
static char __initdata opt_ptm_assign[128] = "";
string_param("ptm_config", opt_ptm_assign);

int bus_from_rg_id(unsigned int rg_id, struct mali_ptm_assign *assign)
{
    if ( rg_id >= assign->rg_count )
        return -EINVAL;

    if ( !assign->res_grps[rg_id].is_set )
        return -EINVAL;

    return assign->res_grps[rg_id].bus;
}

void mali_assign_print_config(struct mali_ptm_assign *assign)
{
    char buff[PRINT_BUFF_SIZE];
    char buses[2] = { 'A', 'B' };
    unsigned int c = 0, rg;

    for ( rg = 0; rg < assign->rg_count; ++rg )
    {
        uint32_t bus = assign->res_grps[rg].bus;
        unsigned int i;

        c += scnprintf(&buff[c], PRINT_BUFF_SIZE - c, "RG%d BUS[%c]", rg,
                   bus < BUS_COUNT ? buses[bus] : '-');

        c += scnprintf(&buff[c], PRINT_BUFF_SIZE - c, " S[");

        for ( i = 0; i < assign->slice_count; ++i )
        {
            if ( i > 0 )
                c += scnprintf(&buff[c], PRINT_BUFF_SIZE - c, " ");

            if ( assign->slices[i].rg == rg )
                c += scnprintf(&buff[c], PRINT_BUFF_SIZE - c, "%d", i);
            else
                c += scnprintf(&buff[c], PRINT_BUFF_SIZE - c, i < 10 ? " " : "  ");
        }

        c += scnprintf(&buff[c], PRINT_BUFF_SIZE - c, "] P[");

        for ( i = 0; i < assign->partition_count; ++i )
        {
            if ( i > 0 )
                c += scnprintf(&buff[c], PRINT_BUFF_SIZE - c, " ");

            if ( assign->partitions[i].rg == rg )
                c += scnprintf(&buff[c], PRINT_BUFF_SIZE - c, "%d", i);
            else
                c += scnprintf(&buff[c], PRINT_BUFF_SIZE - c, i < 10 ? " " : "  ");
        }

        c += scnprintf(&buff[c], PRINT_BUFF_SIZE - c, "] W[");

        for ( i = 0; i < assign->aw_count; ++i )
        {
            if ( i > 0 )
                c += scnprintf(&buff[c], PRINT_BUFF_SIZE - c, " ");

            if ( assign->access_windows[i].rg == rg )
                c += scnprintf(&buff[c], PRINT_BUFF_SIZE - c, "%d", i);
            else
                c += scnprintf(&buff[c], PRINT_BUFF_SIZE - c, i < 10 ? " " : "  ");
        }

        c += scnprintf(&buff[c], PRINT_BUFF_SIZE - c, "]\n");
    }

    printk(XENLOG_INFO "Resource Group Assignment:\n%s", buff);
}

static bool __init set_rg_bus(struct mali_ptm_assign *assign,
                             unsigned int rg, uint32_t bus)
{
    if ( rg >= assign->rg_count )
    {
        printk(XENLOG_ERR
            "PTM: Group %d out of range (max %d)\n", rg, assign->rg_count - 1);
        return false;
    }

    if ( bus >= BUS_COUNT )
    {
        printk(XENLOG_ERR
            "PTM: Group bus %d out of range (max %d)\n", bus, BUS_COUNT - 1);
        return false;
    }

    if (assign->res_grps[rg].is_set && assign->res_grps[rg].bus != bus)
    {
        printk(XENLOG_ERR
            "PTM: Group %d already assigned to bus %d\n",
                rg, assign->res_grps[rg].bus);
        return false;
    }

    assign->res_grps[rg].bus = bus;
    assign->res_grps[rg].is_set = true;

    return true;
}

static bool __init set_aw_rg(struct mali_ptm_assign *assign,
                            unsigned int aw, uint32_t rg)
{
    if ( aw >= assign->aw_count )
    {
        printk(XENLOG_ERR
            "PTM: Window %d out of range (max %d)\n", aw, assign->aw_count - 1);
        return false;
    }

    if ( rg >= assign->rg_count )
    {
        printk(XENLOG_ERR
            "PTM: Window Group %d out of range (max %d)\n",
                rg, assign->rg_count - 1);
        return false;
    }

    if ( assign->access_windows[aw].is_set &&
            assign->access_windows[aw].rg != rg )
    {
        printk(XENLOG_ERR "PTM: Window %d already assigned to group %d\n",
                aw, assign->access_windows[aw].rg);
        return false;
    }

    assign->access_windows[aw].rg = rg;
    assign->access_windows[aw].is_set = true;

    return true;
}

static bool __init set_partition_rg(struct mali_ptm_assign *assign,
                                    unsigned int partition, uint32_t rg)
{
    if ( partition >= assign->partition_count )
    {
        printk(XENLOG_ERR
                "PTM: Partition %d out of range (max %d)\n",
                    partition, assign->partition_count - 1);
        return false;
    }

    if ( rg >= assign->rg_count )
    {
        printk(XENLOG_ERR
                "PTM: Partition Group %d out of range (max %d)\n",
                    rg, assign->rg_count - 1);
        return false;
    }

    if ( assign->partitions[partition].is_set &&
            assign->partitions[partition].rg != rg )
    {
        printk(XENLOG_ERR "PTM: Partition %d already assigned to group %d\n",
                partition, assign->partitions[partition].rg);
        return false;
    }

    assign->partitions[partition].rg = rg;
    assign->partitions[partition].is_set = true;

    return true;
}

static bool __init set_slice_rg(struct mali_ptm_assign *assign,
                                unsigned int slice, uint32_t rg)
{
    if ( slice >= MALI_PTM_SLICES_COUNT )
    {
        printk(XENLOG_ERR "PTM: Slice %d out of driver range (max %d)\n",
                slice, MALI_PTM_SLICES_COUNT);
        return false;
    }

    /*
     * Trying to configure more slices than the hardware allows should not
     * cause probe failure, as the RG module reads the hardware directly and
     * uses that to configure the Arbiter
     */
    if ( slice >= assign->slice_count )
        printk(XENLOG_WARNING "Slice %d out of range (max %d)\n",
               slice, assign->slice_count - 1);

    if ( rg >= assign->rg_count )
    {
        printk(XENLOG_ERR "PTM: Slice Group %d out of range (max %d)\n",
                rg, assign->rg_count - 1);
        return false;
    }

    if ( assign->slices[slice].is_set && assign->slices[slice].rg != rg )
    {
        printk(XENLOG_ERR "PTM: Slice %d already assigned to group %d\n", slice,
            assign->slices[slice].rg);
        return false;
    }

    assign->slices[slice].rg = rg;
    assign->slices[slice].is_set = true;

    return true;
}

static bool __init parse_assign_config(struct mali_ptm_assign *assign,
                                        const char *conf)
{
    unsigned int grp, i = 0;
    char *grp_tok, *next_grp_str, *tok;
    char conf_copy[128];
    enum { ST_BUS, ST_SLICE, ST_PARTITION, ST_AW, ST_END } state;

    grp = 0;
    strlcpy(conf_copy, conf, ARRAY_SIZE(conf_copy));
    conf_copy[sizeof(conf_copy) - 1] = '\0';
    next_grp_str = conf_copy;

    while ( (grp_tok = strsep(&next_grp_str, ",")) )
    {
        if ( *grp_tok == '\0' )
            return false;

        state = ST_BUS;

        while ( (tok = strsep(&grp_tok, ":")) )
        {
            if ( grp >= assign->rg_count )
            {
                printk(XENLOG_ERR "Group %d out of range (max %d)\n",
                       grp, assign->rg_count - 1);
                return false;
            }

            if ( *tok == '\0' )
            {
                printk(XENLOG_ERR "Empty token in group %d\n", grp);
                return false;
            }

            switch (*tok)
            {
                case 'A':
                case 'B':
                    if ( state != ST_BUS )
                    {
                        printk(XENLOG_ERR
                               "PTM: Bus specifier '%c' not allowed here\n",
                               *tok);
                        return false;
                    }
                    if ( *(tok + 1) != ':' && *(tok + 1) != '\0' )
                    {
                        printk(XENLOG_ERR
                               "PTM: Invalid '%c' after bus '%c'\n",
                               *(tok + 1), *tok);
                        return false;
                    }
                    if ( !set_rg_bus(assign, grp, (*tok) - 'A') )
                        return false;
                    state = ST_SLICE;
                    break;

                case 'S':
                    if ( state != ST_SLICE && state != ST_PARTITION )
                    {
                        printk(XENLOG_ERR
                               "PTM: Slice specifier '%c' not allowed here\n",
                               *tok);
                        return false;
                    }
                    ++tok;
                    i = (uint32_t)simple_strtol(tok, (const char**)&tok, 10);
                    if ( !set_slice_rg(assign, i, grp) )
                        return false;
                    if ( state < ST_PARTITION )
                        state = ST_PARTITION;
                    break;

                case 'P':
                    if ( state != ST_PARTITION )
                    {
                        printk(XENLOG_ERR
                               "PTM: Partition specifier '%c' not allowed here\n",
                               *tok);
                        return false;
                    }
                    ++tok;
                    i = (uint32_t)simple_strtol(tok, (const char**)&tok, 10);
                    if ( !set_partition_rg(assign, i, grp) )
                        return false;
                    state = ST_AW;
                    break;

                case 'W':
                    if ( state != ST_AW && state != ST_END )
                    {
                        printk(XENLOG_ERR
                               "PTM: Access window specifier '%c' not allowed here\n",
                               *tok);
                        return false;
                    }
                    ++tok;
                    i = (uint32_t)simple_strtol(tok, (const char**)&tok, 10);
                    if ( !set_aw_rg(assign, i, grp) )
                        return false;
                    if ( state < ST_END )
                        state = ST_END;
                    break;

                default:
                    printk(XENLOG_ERR "Unknown token type: '%s'\n", tok);
                    return false;
            }
        }
        if ( state != ST_END )
        {
            printk(XENLOG_ERR
                   "PTM: Group %d incomplete, expected AW specifier\n", grp);
            return false;
        }
        ++grp;
    }

    return true;
}

static void __init probe_hw_config(struct mali_ptm_assign *assign)
{
    unsigned int i;
    uint32_t v;
    uint64_t cores;
    void __iomem * base_addr =
                    ioremap_nocache(assign->base, assign->size);

    if ( !base_addr )
    {
        printk(XENLOG_ERR "PTM: Failed to map assign registers\n");
        return;
    }

    for ( i = 0; i < MALI_PTM_PARTITION_COUNT; ++i )
    {
        assign->res_grps[i].is_set = false;
        assign->res_grps[i].bus = BUS_A;
    }
    for ( i = 0; i < MALI_PTM_ACCESS_WINDOW_COUNT; ++i )
    {
        assign->access_windows[i].is_set = false;
        assign->access_windows[i].rg = 0;
    }

    for ( i = 0; i < MALI_PTM_PARTITION_COUNT; ++i )
    {
        assign->partitions[i].is_set = false;
        assign->partitions[i].rg = 0;
    }

    for ( i = 0; i < MALI_PTM_SLICES_COUNT; ++i )
    {
        assign->slices[i].is_set = false;
        assign->slices[i].rg = 0;
        assign->slices[i].isolation_set = 0;
    }

    /* Read the number of partitions and access windows the hardware has */
    v = readl(base_addr + PTM_UNIT_FEATURES);
    assign->partition_count = (v >> PTM_UNIT_FEATURES_PARTITIONS_OFFSET) &
                  PTM_UNIT_FEATURES_PARTITIONS_MASK;
    assign->aw_count = (v >> PTM_UNIT_FEATURES_AWS_OFFSET) & PTM_UNIT_FEATURES_AWS_MASK;

    /*
     * Count the number of slices the hardware has by iterating through
     * all the slices until we reach a slice with a core count of zero.
     */
    cores = readl(base_addr + PTM_SLICE_CORES);
    cores |= (uint64_t)readl(base_addr + PTM_SLICE_CORES + 4) << 32;
    for ( i = 0; i < MALI_PTM_SLICES_COUNT; ++i )
        if ( ((cores >> (i * PTM_SLICE_CORES_CORE_BITS)) &
              PTM_SLICE_CORES_CORE_MASK) == 0 )
            break;

    assign->slice_count = i;
    assign->rg_count = MALI_PTM_PARTITION_COUNT;

    /* check these things won't overflow our structure, clamp if they do */
    if ( assign->partition_count > MALI_PTM_PARTITION_COUNT )
        assign->partition_count = MALI_PTM_PARTITION_COUNT;
    if ( assign->aw_count > MALI_PTM_ACCESS_WINDOW_COUNT )
        assign->aw_count = MALI_PTM_ACCESS_WINDOW_COUNT;
    if ( assign->slice_count > MALI_PTM_SLICES_COUNT )
        assign->slice_count = MALI_PTM_SLICES_COUNT;

    iounmap(base_addr);
}

static bool __init validate_data(struct mali_ptm_assign *assign)
{
    unsigned int i;
    uint32_t prev_slice_rg;

    /*
     * Set the isolation flag on each slice whose preceding slice is in a
     * different resource group
     */
    prev_slice_rg = assign->rg_count;
    for ( i = 0; i < assign->slice_count; ++i )
    {
        if ( assign->slices[i].rg != prev_slice_rg )
            assign->slices[i].isolation_set = 1;
        prev_slice_rg = assign->slices[i].rg;
    }

    return true;
}

static void __init apply_assign_config(struct mali_ptm_assign *assign)
{
    unsigned int i;
    uint32_t value;
    void __iomem * base_addr =
            ioremap_nocache(assign->base, assign->size);

    if ( !base_addr )
    {
        printk(XENLOG_ERR "PTM: Failed to map assign registers\n");
        return;
    }

    /* Set the busses for each resource group */
    value = 0;
    for ( i = 0; i < assign->rg_count; ++i )
        value |= (assign->res_grps[i].bus & BUS_MASK) << (i * BUS_BITS);
    writel(value, base_addr + PTM_ASSIGN_RESOURCE_GROUP_BUS);

    /* Set the group for each partition */
    value = 0;
    for ( i = 0; i < assign->partition_count; ++i )
        value |= (assign->partitions[i].rg & GROUP_MASK) << (i * GROUP_BITS);
    writel(value, base_addr + PTM_ASSIGN_PARTITION_RESOURCE_GROUP);

    /* Set the group for each access window (first DWORD) */
    value = 0;
    for ( i = 0; i < assign->aw_count && i < AWS_PER_DWORD; ++i )
        value |= (assign->access_windows[i].rg & GROUP_MASK) << (i * GROUP_BITS);
    writel(value, base_addr + PTM_ASSIGN_AW_RESOURCE_GROUP);

    /* Set the group for each access window (second DWORD) */
    value = 0;
    for ( i = AWS_PER_DWORD; i < assign->aw_count; ++i )
        value |= (assign->access_windows[i].rg & GROUP_MASK)
            << (i - AWS_PER_DWORD) * GROUP_BITS;
    writel(value, base_addr + PTM_ASSIGN_AW_RESOURCE_GROUP + 0x4);

    /* Set the group for each slice */
    value = 0;
    for ( i = 0; i < assign->slice_count; ++i )
        value |= (assign->slices[i].rg & GROUP_MASK) << (i * GROUP_BITS);
    writel(value, base_addr + PTM_ASSIGN_SLICE_RESOURCE_GROUP);

    /* Set the isolation for each slice */
    value = 0;
    for ( i = 0; i < assign->slice_count; ++i )
        value |= (assign->slices[i].isolation_set & ISOLATION_MASK)
                 << (i * ISOLATION_BITS);
    writel(value, base_addr + PTM_ASSIGN_SLICE_ISOLATION_SET);

    iounmap(base_addr);

    return;
}

int __init mali_ptm_assign_init(struct mali_ptm_assign *assign)
{
    struct dt_device_node *node;
    int ret;

    node = dt_find_compatible_node(NULL, NULL, MALI_GPU_ASSIGN_PTM_DT_NAME);
    if ( !node )
    {
        printk(XENLOG_ERR "PTM: No assign node found in device tree\n");
        return -ENOENT;
    }
    ret = dt_device_get_paddr(node, 0, &assign->base, &assign->size);
    if ( ret )
    {
        printk(XENLOG_ERR "PTM: Failed to get assign base address\n");
        return -ENXIO;
    }
    probe_hw_config(assign);
    if ( !parse_assign_config(assign, opt_ptm_assign) )
        return -EINVAL;
    mali_assign_print_config(assign);
    if ( !validate_data(assign) )
        return -EINVAL;

    apply_assign_config(assign);

    return 0;
}

