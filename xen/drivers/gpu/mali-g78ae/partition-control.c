/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Mali G78AE GPU PTM Partition control driver
 *
 * Copyright (C) 2025, Advanced Micro Devices, Inc.
 */
#include <xen/bootfdt.h>
#include <xen/init.h>
#include <xen/mm.h>
#include <xen/vmap.h>

#include "common.h"
#include "device-tree.h"
#include "partition-control.h"

#define PTM_RESET_STATE_OFFSET 0x0058
#define PTM_RESET_SET_OFFSET 0x005C
#define PTM_PARTITION_STATE_OFFSET 0x0060
#define PTM_AW_SET 0x0064
#define PTM_RESET_STATE_MASK 0x01
#define PTM_PARTITION_STATE_LOCK_MASK 0x3
#define PTM_PARTITION_STATE_AW_READY_MASK 0x4
#define PTM_PARTITION_STATE_AW_ERROR_MASK 0x8
#define PTM_NUM_OF_AW 0x10
#define PTM_RESET_SET_TRIGGER_VALUE 0x1

static int mali_ptm_part_ctrl_reset(struct mali_ptm_part_ctrl *ctrl)
{
    int ret;
    uint32_t reset_state;
    uint32_t part_state;

    if ( !ctrl || !ctrl->mem )
        return -ENODEV;

    writel(PTM_RESET_SET_TRIGGER_VALUE, ctrl->mem + PTM_RESET_SET_OFFSET);
    ret = readx_poll_timeout(readl, ctrl->mem + PTM_RESET_STATE_OFFSET,
                   reset_state, (reset_state & PTM_RESET_STATE_MASK),
                   REG_POLL_SLEEP_US, REG_POLL_RESET_TIMEOUT_US);
    if ( ret )
        return ret;

    writel(0x0, ctrl->mem + PTM_RESET_SET_OFFSET);
    ret = readx_poll_timeout(readl, ctrl->mem + PTM_RESET_STATE_OFFSET,
                   reset_state, ((reset_state & PTM_RESET_STATE_MASK) == 0),
                   REG_POLL_SLEEP_US, REG_POLL_RESET_TIMEOUT_US);
    if ( ret )
        return ret;

    ret = readx_poll_timeout(readl, ctrl->mem + PTM_PARTITION_STATE_OFFSET,
                   part_state,
                   ((part_state & PTM_PARTITION_STATE_LOCK_MASK) == 0),
                   REG_POLL_SLEEP_US, REG_POLL_TIMEOUT_US);

    return ret;
}

int ctrlif_assign_partition_to_aw(struct mali_ptm_part_ctrl *ctrl,
                                  unsigned int aw)
{
    int error = 0;
    uint32_t val;
    uint32_t state;

    if ( !ctrl || !ctrl->mem )
        return -ENODEV;

    if ( aw > PTM_NUM_OF_AW - 1 )
        return -EINVAL;

    /* Reset the partition if necessary */
    state = readl(ctrl->mem + PTM_PARTITION_STATE_OFFSET);
    if ( state & PTM_PARTITION_STATE_LOCK_MASK )
    {
        error = mali_ptm_part_ctrl_reset(ctrl);
        if ( error )
        {
            printk(XENLOG_ERR
                   "ctrlif: Error (%d) while resetting partition\n", error);
            return -EIO;
        }
    }

    val = 0x1U << aw;
    writel(val, ctrl->mem + PTM_AW_SET);

    state = readl(ctrl->mem + PTM_PARTITION_STATE_OFFSET);
    if ( state & PTM_PARTITION_STATE_AW_ERROR_MASK )
    {
        printk(XENLOG_ERR
               "ctrlif: In error state(0x%x) after setting AW\n", state);
        return -EIO;
    }

    error = readx_poll_timeout(readl, ctrl->mem + PTM_PARTITION_STATE_OFFSET,
                   state, (state & PTM_PARTITION_STATE_AW_READY_MASK),
                   REG_POLL_SLEEP_US, REG_POLL_TIMEOUT_US);
    if ( error )
        return error;

    return 0;
}

int ctrlif_get_assigned_aw(struct mali_ptm_part_ctrl *ctrl, unsigned int *aw)
{
    uint32_t val;
    unsigned int i;

    val = readl(ctrl->mem + PTM_AW_SET);
    if ( val )
    {
        for ( i = 0; i < PTM_NUM_OF_AW; i++ )
        {
            val = val >> 1;
            if ( !val )
            {
                *aw = i;
                return 0;
            }
        }

        /* Unexpected result */
        return -EIO;
    }

    *aw = 0;
    return -EIO;
}

int ctrlif_unassign_partition(struct mali_ptm_part_ctrl *ctrl)
{
    int error;
    uint32_t state;

    state = readl(ctrl->mem + PTM_PARTITION_STATE_OFFSET);
    if ( state & PTM_PARTITION_STATE_LOCK_MASK )
    {
        error = mali_ptm_part_ctrl_reset(ctrl);
        if ( error )
            return error;
    }

    return 0;
}

int __init mali_ptm_part_ctrl_get_id(struct dt_device_node *part_ctrl_node)
{
    const __be32 *prop;
    paddr_t addr;
    uint32_t reg_len;

    prop = dt_get_property(part_ctrl_node, "reg", &reg_len);
    if ( !prop )
        return -ENODEV;

    addr = dt_read_paddr(prop, dt_n_addr_cells(part_ctrl_node));
    return MALI_NODE_PTM_CONTROL_ID_SAFE(addr);
}

static int __init scan_ptm_interface(struct dt_device_node *node,
                                    struct mali_ptm_part_ctrl *part_ctrl)
{
    paddr_t base, size;
    int ret;
    struct dt_device_node *child;

    dt_for_each_child_node(node, child)
    {
        if ( !dt_device_is_compatible(child, MALI_GPU_PT_CTRL_PTM_DT_NAME) )
           continue;

        ret = dt_device_get_paddr(child, 0, &base, &size);
        if ( ret )
        {
            printk(XENLOG_ERR
                   "PTM: Failed to get paddr for partition control node\n");
            return ret;
        }

        ret = mali_ptm_part_ctrl_get_id(child);
        if ( ret < 0 )
        {
            printk(XENLOG_ERR
                   "PTM: Failed to get partition control ID for %s\n",
                    child->full_name);
            return ret;
        }

        if ( part_ctrl[ret].base != 0 )
        {
            printk(XENLOG_ERR
                   "PTM: Found duplicate partition control node: %s\n",
                    child->full_name);
            return -EINVAL;
        }

        part_ctrl[ret].base = base;
        part_ctrl[ret].size = size;
        part_ctrl[ret].mem = ioremap_nocache(base, size);
        if ( !part_ctrl[ret].mem )
        {
            printk(XENLOG_ERR
                   "PTM: Failed to map partition control memory for %s\n",
                    child->full_name);
            return -ENOMEM;
        }
    }

    return 0;
}

int __init mali_ptm_part_ctrl_init(struct dt_device_node *mali_gpu_node,
                   struct mali_ptm_part_ctrl *part_ctrls)
{
    struct dt_device_node* ptm_interface;
    int ret;
    unsigned int i;

    memset(part_ctrls, 0,
           sizeof(struct mali_ptm_part_ctrl) * MALI_PTM_PARTITION_COUNT);

    dt_for_each_child_node(mali_gpu_node, ptm_interface)
    {
        if ( !dt_device_is_compatible(ptm_interface, MALI_GPU_IF_PTM_DT_NAME) )
            continue;
        ret = scan_ptm_interface(ptm_interface, part_ctrls);
        if ( ret )
        {
            printk(XENLOG_ERR
                   "PTM: Failed to scan ptm_interface for part. control: %s\n",
                ptm_interface->full_name);
            for ( i = 0; i < MALI_PTM_PARTITION_COUNT; i++ )
                if ( part_ctrls[i].mem )
                    iounmap(part_ctrls[i].mem);
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