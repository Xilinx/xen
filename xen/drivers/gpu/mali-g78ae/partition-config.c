/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Mali G78AE GPU PTM Partition Configuration driver
 *
 * Copyright (C) 2025, Advanced Micro Devices, Inc.
 */
#include <xen/bootfdt.h>
#include <xen/init.h>
#include <xen/mm.h>
#include <xen/vmap.h>

#include "common.h"
#include "device-tree.h"
#include "partition-config.h"

#define PTM_SLICE_MODE 0x0014
#define PTM_SLICE_MODE_NEW_OFFSET 0x0018
#define PTM_SLICE_MODE_UPDATE_OFFSET 0x001C
#define PTM_SLICE_MODE_ACK_OFFSET 0x0020
#define PTM_SLICE_MODE_ACK_STATUS_MASK 0x01
#define PTM_SLICE_MODE_ACK_ERROR_MASK 0x02
#define PTM_SLICE_MODE_DISABLED 0x0
#define PTM_SLICE_MODE_PRIMARY 0x1
#define PTM_SLICE_MODE_SECONDARY 0x2
#define PTM_SLICE_BITS_PER_MODE 0x2
#define PTM_SLICE_MODE_UPDATE_TRIGGER_VALUE 0x1

static bool is_slice_mask_valid(uint32_t slices)
{
    bool ones_start = false;
    bool ones_end = false;
    unsigned int i;

    for ( i = 0; i < MALI_PTM_SLICES_COUNT; i++ )
    {
        if ( slices & (1U << i) )
        {
            if ( ones_end )
                return false;
            if ( !ones_start )
                ones_start = true;
        } else {
            if ( ones_start )
            {
                ones_start = false;
                ones_end = true;
            }
        }
    }
    return true;
}

int cfgif_assign_slices(struct mali_ptm_part_cfg *part_cfg,
                        uint32_t slices_mask)
{
    int error = 0;
    uint32_t slice_mode_ack;
    uint32_t slice_mode_wr = 0;
    unsigned int i = 0;
    uint8_t slice_mode;
    bool first_slice = true;

    if ( !part_cfg || !part_cfg->mem )
        return -ENODEV;

    if ( !is_slice_mask_valid(slices_mask) )
        return -EINVAL;

    for ( i = 0; i < MALI_PTM_SLICES_COUNT; i++ )
    {
        slice_mode = PTM_SLICE_MODE_DISABLED;
        if ( slices_mask & (1 << i) )
        {
            if ( first_slice )
            {
                slice_mode = PTM_SLICE_MODE_PRIMARY;
                first_slice = false;
            }
            else
                slice_mode = PTM_SLICE_MODE_SECONDARY;
        }
        slice_mode_wr |= (slice_mode << (i * PTM_SLICE_BITS_PER_MODE));
    }

    writel(slice_mode_wr, part_cfg->mem + PTM_SLICE_MODE_NEW_OFFSET);

    writel(PTM_SLICE_MODE_UPDATE_TRIGGER_VALUE,
          part_cfg->mem + PTM_SLICE_MODE_UPDATE_OFFSET);

    error = readx_poll_timeout(readl, part_cfg->mem + PTM_SLICE_MODE_ACK_OFFSET,
                   slice_mode_ack,
                   !(slice_mode_ack & PTM_SLICE_MODE_ACK_STATUS_MASK),
                   REG_POLL_SLEEP_US, REG_POLL_TIMEOUT_US);

    if ( error )
        return error;

    if ( slice_mode_ack & PTM_SLICE_MODE_ACK_ERROR_MASK )
    {
        printk(XENLOG_ERR "cfgif: Slice mode update failed (0x%x)\n",
               slice_mode_ack);
        return -EIO;
    }

    return 0;
}

int __init mali_ptm_part_cfg_get_id(struct dt_device_node *part_cfg)
{
    const __be32 *prop;
    paddr_t addr;
    uint32_t reg_len;

    prop = dt_get_property(part_cfg, "reg", &reg_len);
    if ( !prop )
        return -ENODEV;
    addr = dt_read_paddr(prop, dt_n_addr_cells(part_cfg));
    return MALI_NODE_PTM_CONFIG_ID_SAFE(addr);
}

static int __init scan_ptm_interface(struct dt_device_node *node,
             struct mali_ptm_part_cfg *part_cfgs)
{
    paddr_t base, size;
    int ret;
    struct dt_device_node *child;

    dt_for_each_child_node(node, child)
    {
        if ( !dt_device_is_compatible(child, MALI_GPU_PT_CFG_PTM_DT_NAME) )
           continue;

        ret = dt_device_get_paddr(child, 0, &base, &size);
        if ( ret )
        {
            printk(XENLOG_ERR
                   "PTM: Failed to get paddr for partition config node\n");
            return ret;
        }

        ret = mali_ptm_part_cfg_get_id(child);
        if ( ret < 0 )
        {
            printk(XENLOG_ERR
                   "PTM: Failed to get partition config ID for %s\n",
                    child->full_name);
            return ret;
        }

        if ( part_cfgs[ret].base != 0 )
        {
            printk(XENLOG_ERR
                   "PTM: Found duplicate partition config node: %s\n",
                    child->full_name);
            return -EINVAL;
        }

        part_cfgs[ret].base = base;
        part_cfgs[ret].size = size;
        part_cfgs[ret].mem = ioremap_nocache(base, size);
        if ( !part_cfgs[ret].mem )
        {
            printk(XENLOG_ERR
                   "PTM: Failed to map partition config memory for %s\n",
                    child->full_name);
            return -ENOMEM;
        }

    }

    return 0;
}

int __init mali_ptm_part_cfg_init(struct dt_device_node *mali_gpu_node,
                   struct mali_ptm_part_cfg *part_cfgs)
{
    struct dt_device_node* ptm_interface;
    int ret;
    unsigned int i;

    memset(part_cfgs, 0,
           sizeof(struct mali_ptm_part_cfg) * MALI_PTM_PARTITION_COUNT);

    dt_for_each_child_node(mali_gpu_node, ptm_interface)
    {
        if ( !dt_device_is_compatible(ptm_interface, MALI_GPU_IF_PTM_DT_NAME) )
            continue;
        ret = scan_ptm_interface(ptm_interface, part_cfgs);
        if ( ret )
        {
            printk(XENLOG_ERR
                   "PTM: Failed to scan ptm_interface for part. config: %s\n",
                    ptm_interface->full_name);
            for ( i = 0; i < MALI_PTM_PARTITION_COUNT; i++ )
                if ( part_cfgs[i].mem )
                    iounmap(part_cfgs[i].mem);
            return ret;
        }
    }

    return 0;
}

/*
 * Local variables:
 * mode: C
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * End:
 */