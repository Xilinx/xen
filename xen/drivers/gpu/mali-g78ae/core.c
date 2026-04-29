/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Mali G78AE GPU PTM core driver
 *
 * Copyright (C) 2025, Advanced Micro Devices, Inc.
 */
#include <xen/init.h>
#include <xen/param.h>
#include <xen/device_tree.h>
#include <xen/libfdt/libfdt.h>
#include <xen/sched.h>
#include <xen/errno.h>
#include <xen/keyhandler.h>
#include <xen/list.h>

#include <asm/mali-g78ae.h>

#include "common.h"
#include "assign.h"
#include "system.h"
#include "device-tree.h"
#include "partition-control.h"
#include "partition-config.h"
#include "resource-group.h"
#include "gsi-scheduler-if.h"
#include "versal2-gpu.h"

static struct {
    struct mali_ptm_assign assign;
    struct mali_ptm_system system;
    struct mali_ptm_part_ctrl partition_control[MALI_PTM_PARTITION_COUNT];
    struct mali_ptm_part_cfg partition_config[MALI_PTM_PARTITION_COUNT];
    struct mali_ptm_rg resource_group[MALI_PTM_PARTITION_COUNT];
    bool initialized;
} mali_g78ae;

int __initdata opt_dom0_mali_aw = MAX_AW_NUM;

int mali_g78ae_register_domain(struct domain *d, unsigned int aw)
{
    unsigned int i;
    int err;

    if ( !mali_g78ae.initialized )
    {
        printk(XENLOG_ERR "Mali GPU not initialized\n");
        return -ENODEV;
    }

    if ( aw >= MALI_PTM_ACCESS_WINDOW_COUNT )
    {
        printk(XENLOG_ERR "Invalid AW ID %u for d%d\n", aw, d->domain_id);
        return -EINVAL;
    }

    d->arch.mali_aw = aw;

    for ( i = 0; i < MALI_PTM_PARTITION_COUNT; i++ )
    {
        err = mali_arbif_assign_domain(mali_g78ae.resource_group[i].arbiter, d);
        if ( !err )
            return 0;
    }
    printk(XENLOG_ERR "gpu: Failed to assign d%u AW%u to any RG\n",
           d->domain_id, aw);
    d->arch.mali_aw = MAX_AW_NUM;
    return err;
}

int mali_g78ae_unregister_domain(struct domain *d)
{
    unsigned int i;

    if ( !mali_g78ae.initialized )
    {
        printk(XENLOG_ERR "Mali GPU not initialized\n");
        return -ENODEV;
    }

    for ( i = 0; i < MALI_PTM_PARTITION_COUNT; i++ )
        mali_arbif_unassign_domain(mali_g78ae.resource_group[i].arbiter,d);

    d->arch.mali_aw = MAX_AW_NUM;
    return 0;
}

void dump_mali_info(unsigned char key)
{
    const struct mali_ptm_rg *rg = NULL;
    const struct gsi_info *gpu_info = NULL;
    static const char buses[] = { 'A', 'B' };
    int bus_id;
    unsigned int i, j;

    printk("MALI G78AE GPU info:\n");
    printk("- System interface: 0x%lx\n", mali_g78ae.system.base);
    printk("- Assign interface: 0x%lx\n", mali_g78ae.assign.base);
    mali_assign_print_config(&mali_g78ae.assign);
    printk("Partition manager status:\n");
    for ( i = 0; i < MALI_PTM_PARTITION_COUNT; i++ )
    {
        rg = &mali_g78ae.resource_group[i];
        printk("- RG%d: ", i);
        if ( !rg || !rg->base )
        {
            printk("DISABLED\n");
            continue;
        }
        bus_id = bus_from_rg_id(i, &mali_g78ae.assign);
        printk("ENABLED on bus %c | Addr: 0x%lx\n",
                (bus_id < 0 || (unsigned int)bus_id >= ARRAY_SIZE(buses))
                    ? '?' : buses[bus_id],
                rg->base);
        printk("  - Partitions mask: 0x%x\n", rg->partition_mask);
        for ( j = 0; j < MALI_PTM_PARTITION_COUNT; j++ )
        {
            if ( !(rg->partition_mask & (1U << j)) )
                continue;
            printk("    P%d: Config: 0x%lx | Control: 0x%lx\n",
                    j, rg->cfg[j]->base, rg->ctrl[j]->base);
        }
        printk("  - Arbiter: L2 slices=%d | Core Mask=0x%x\n",
                        rg->arbiter->l2_slices, rg->arbiter->core_mask);
        for ( j = 0; j < MALI_PTM_PARTITION_COUNT; j++ )
        {
            gpu_info = &rg->arbiter->gsi_info[j];

            if ( gpu_info->enabled )
            {
                struct mali_vm_data *vm_data = NULL;
                struct mali_arb_gsi *gsi = gpu_info->gsi;
                struct domain *dom;

                if ( !gsi )
                    continue;
                printk("    GSI%u: Enabled (AW mask=0x%x)\n",
                        gsi->idx, gpu_info->aw_mask);
                if ( !gpu_info->aw_mask )
                    continue;
                spin_lock(&rg->arbiter->lock);
                list_for_each_entry(vm_data, &rg->arbiter->reg_vms_list, entry)
                {
                    if ( vm_data->gsi_idx != (int)gsi->idx
                        || vm_data->gsi_idx < 0
                        || vm_data->gsi_idx >= MALI_PTM_PARTITION_COUNT )
                        continue;
                    dom = vm_data->domain;
                    if ( !dom )
                        continue;
                    printk("    - AW%u, Domain: %u\n",
                           vm_data->aw, dom->domain_id);
                }
                spin_unlock(&rg->arbiter->lock);
                if ( gpu_info->gsi->sched_ops &&
                    gpu_info->gsi->sched_ops->sched_print_stats )
                    gpu_info->gsi->sched_ops->sched_print_stats(
                        gpu_info->gsi->sched_ptr, "    ");
            }
            else
                printk("    GSI%u: Disabled\n", j);
        }
    }
}

static int __init mali_gpu_init(void)
{
    int ret;
    struct dt_device_node *gpu;

    gpu = dt_find_compatible_node(NULL, NULL, MALI_GPU_PTM_DT_NAME);
    if ( !gpu )
    {
        printk(XENLOG_ERR "gpu: No GPU node found in device tree\n");
        return -ENOENT;
    }

    /* XXX: this should be removed in final release */
    ret = versal2_mali_gpu_init();
    if ( ret )
    {
        printk(XENLOG_ERR "gpu: Failed to initialize Versal2 MALI GPU\n");
        return ret;
    }

    printk(XENLOG_WARNING "############################################################\n");
    printk(XENLOG_WARNING "# WARNING: You are using an EXPERIMENTAL Mali-G78AE driver #\n");
    printk(XENLOG_WARNING "# Functionality may be incomplete or unstable.             #\n");
    printk(XENLOG_WARNING "############################################################\n");

    ret = mali_ptm_system_init(&mali_g78ae.system);
    if ( ret )
    {
        printk(XENLOG_ERR "gpu: Failed to initialize system interface\n");
        goto err;
    }

    ret = mali_ptm_assign_init(&mali_g78ae.assign);
    if ( ret )
    {
        printk(XENLOG_ERR "gpu: Failed to initialize assign interface\n");
        goto err;
    }

    ret = mali_ptm_part_ctrl_init(gpu, mali_g78ae.partition_control);
    if ( ret )
    {
        printk(XENLOG_ERR
               "gpu: Failed to initialize partition control interface\n");
        goto err;
    }

    ret = mali_ptm_part_cfg_init(gpu, mali_g78ae.partition_config);
    if ( ret )
    {
        printk(XENLOG_ERR
               "gpu: Failed to initialize partition config interface\n");
        goto err;
    }
    ret = mali_ptm_rg_init(gpu,
                           mali_g78ae.partition_control,
                           mali_g78ae.partition_config,
                           mali_g78ae.resource_group);
    if ( ret )
    {
        printk(XENLOG_ERR
               "gpu: Failed to initialize resource group interface\n");
        goto err;
    }
    printk(XENLOG_INFO "gpu: Mali G78AE initialized\n");

    register_keyhandler('7', dump_mali_info, "Dump MALI G78AE info", 0);
    mali_g78ae.initialized = true;
    return 0;

 err:
    printk(XENLOG_ERR "gpu: Initialization failed (ret = %d)\n", ret);
    return ret;
}

__initcall(mali_gpu_init);

/*
 * Local variables:
 * mode: C
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * End:
 */
