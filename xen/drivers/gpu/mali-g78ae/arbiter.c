/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Mali G78AE GPU Arbiter Implementation
 *
 * Copyright (C) 2025, Advanced Micro Devices, Inc.
 */
#include <xen/sched.h>

#include "arbiter.h"
#include "arb-vm-protocol.h"
#include "partition-control.h"
#include "ptm-msg.h"

/* Must be called with arbiter lock held */
static bool find_vm_by_aw(struct mali_arbiter *arb, unsigned int aw,
                          struct list_head *list, struct mali_vm_data **vm_data)
{
    struct mali_vm_data *curr_vm;

    list_for_each_entry(curr_vm, list, entry)
    {
        if ( curr_vm->aw == aw )
        {
            if ( vm_data )
                *vm_data = curr_vm;
            return true;
        }
    }

    return false;
}

/* Must be called with arbiter lock held */
static bool vm_is_in_wait_list(struct mali_arbiter *arb,
                              struct mali_vm_data *vm_data)
{
    return find_vm_by_aw(arb, vm_data->aw, &arb->wait_list, NULL);
}

/* Must be called with arbiter lock held */
static bool vm_is_in_reg_list(struct mali_arbiter *arb,
                             struct mali_vm_data *vm_data)
{
    return find_vm_by_aw(arb, vm_data->aw, &arb->reg_vms_list, NULL);;
}

static int gsi_idx_from_aw_locked(struct mali_arbiter *arb, unsigned int aw)
{
    unsigned int i;

    for ( i = 0; i < MALI_PTM_PARTITION_COUNT; i++ )
    {
        if ( !arb->gsi_info[i].enabled )
            continue;

        spin_lock(&arb->gsi_info[i].lock);
        if ( arb->gsi_info[i].aw_mask & (1U << aw) )
            return i;
        spin_unlock(&arb->gsi_info[i].lock);
    }

    return -1;
}

static int gsi_idx_from_aw(struct mali_arbiter *arb, unsigned int aw)
{
    unsigned int i;

    for ( i = 0; i < MALI_PTM_PARTITION_COUNT; i++ )
    {
        if ( !arb->gsi_info[i].enabled )
            continue;

        if ( arb->gsi_info[i].aw_mask & (1U << aw) )
            return i;
    }

    return -1;
}

/**
 * Calculate max_l2_slices value which is considered the number of L2 slices
 * allocated to the resource group which this arbiter is associated with
 * (1 to 1 relationship between arbiter and resource group)
 *
 * Return: 0 on success, standard linux error code otherwise
 */
static int get_max_l2_slices(struct mali_ptm_rg *rg, uint32_t *max_l2_slices)
{
    uint32_t slice_mask = 0;
    uint32_t block_l2_num = 0;
    int err;

    if ( !max_l2_slices )
        return -EINVAL;

    err = rgif_get_slice_mask(rg, &slice_mask);
    if ( err )
        return err;

    *max_l2_slices = 0;

    while ( slice_mask )
    {
        if ( slice_mask & 0x1 )
            block_l2_num++;
        else
        {
            if ( block_l2_num > *max_l2_slices )
                *max_l2_slices = block_l2_num;
            block_l2_num = 0;
        }
        slice_mask >>= 1;
    }

    if ( block_l2_num > *max_l2_slices )
        *max_l2_slices = block_l2_num;

    return 0;
}

/**
 * get_max_core_mask() - Calculates max core mask data
 * @dev: Resource Group module data
 * @max_core_mask: pointer to variable to receive max core mask data
 *
 * Calculate max_core_mask value which is considered to be all the l2 slices
 * assigned to the resource group and the shader_cores on each of them and we
 * calculate the max_core_mask bitmap as the smallest superset that would
 * represent all of them. So any possible combination of contiguous
 * l2 slices word have a core_mask that would be a subgroup of
 * the max_core_mask.
 *
 * Return: 0 on success, standard linux error code otherwise
 */
static int get_max_core_mask(struct mali_ptm_rg *rg, uint32_t *max_core_mask)
{
    uint32_t slice_mask = 0;
    uint8_t core_mask_stride = 0;
    uint32_t block_core_mask = 0;
    uint32_t core_mask = 0;
    uint64_t slices_core_mask = 0;
    int err;
    unsigned int slice_count = 0;

    if ( !max_core_mask )
        return -EINVAL;

    err = rgif_get_slice_mask(rg, &slice_mask);
    if ( err )
        return err;

    rgif_get_slices_core_mask(rg, &slices_core_mask, &core_mask_stride);
    *max_core_mask = 0;

    while ( slice_mask )
    {
        core_mask = slices_core_mask & ((0x1ll << core_mask_stride) - 1);
        slices_core_mask >>= core_mask_stride;

        if ( slice_mask & 0x1 )
        {
            core_mask <<= core_mask_stride * slice_count;
            block_core_mask |= core_mask;
            slice_count++;
        }
        else
        {
            *max_core_mask |= block_core_mask;
            block_core_mask = 0;
            slice_count = 0;
        }
        slice_mask >>= 1;
    }
    *max_core_mask |= block_core_mask;

    return 0;
}

int mali_arbiter_create(struct mali_arbiter **arbiter, struct mali_ptm_rg *rg)
{
    struct mali_arbiter *arb;
    unsigned int i;
    int err = 0;

    /* One unique arbiter per resource group */
    ASSERT(rg->arbiter == NULL);

    arb = xvzalloc(struct mali_arbiter);
    if ( !arb )
        return -ENOMEM;

    arb->rg = rg;

    for ( i = 0; i < MALI_PTM_PARTITION_COUNT; i++)
    {
        if ( !(rg->partition_mask & (1U << i)) )
        {
            arb->gsi_info[i].enabled = false;
            continue;
        }
        if ( arb->gsi_info[i].enabled )
        {
            err = -EFAULT;
            goto clean_instances;
        }

        arb->gsi_info[i].enabled = true;

        err = mali_gsi_create(&arb->gsi_info[i].gsi, i, arb);
        if ( err )
            goto clean_instances;


        err = rgif_get_aw_mask(arb->rg, &arb->gsi_info[i].aw_mask);
        if ( err )
            goto clean_instances;

        printk(XENLOG_DEBUG "P%u: Enabled with AW mask 0x%x\n",
            i, arb->gsi_info[i].aw_mask);

        err = rgif_get_slice_mask(arb->rg, &arb->gsi_info[i].slice_mask);
        if ( err )
            goto clean_instances;

        printk(XENLOG_DEBUG "P%u: Slices mask 0x%x\n",
            i,arb->gsi_info[i].slice_mask);

        if ( arb->gsi_info[i].slice_mask != 0)
            mali_gsi_flag_set(arb->gsi_info[i].gsi,
                                GSI_FLAG_SLICE_ASSIGNED);

        spin_lock_init(&arb->gsi_info[i].lock);
    }

    INIT_LIST_HEAD(&arb->wait_list);
    INIT_LIST_HEAD(&arb->reg_vms_list);
    spin_lock_init(&arb->lock);

    /* Init l2 and core slice values */
    get_max_l2_slices(arb->rg, &arb->l2_slices);
    get_max_core_mask(arb->rg, &arb->core_mask);

    *arbiter = arb;
    return 0;

clean_instances:
    mali_arbiter_destroy(arb);
    return err;
}

void mali_arbiter_destroy(struct mali_arbiter *arb)
{
    unsigned int i;

    if ( !arb )
        return;

    for ( i = 0; i < MALI_PTM_PARTITION_COUNT; i++ )
    {
        if ( arb->gsi_info[i].enabled )
        {
            mali_gsi_destroy(arb->gsi_info[i].gsi);
            arb->gsi_info[i].enabled = false;
            arb->gsi_info[i].gsi = NULL;
        }
        arb->gsi_info[i].enabled = false;
    }

    xvfree(arb);
    return;
}

int mali_arbif_register_vm(struct mali_arbiter *arb, struct mali_vm_data *vm_data)
{
    int err = 0;

    spin_lock(&arb->lock);
    if ( vm_is_in_reg_list(arb, vm_data) )
    {
        printk(XENLOG_ERR "VM %u already registered.\n", vm_data->aw);
        spin_unlock(&arb->lock);
        return -EBUSY;
    }
    printk(XENLOG_DEBUG "Registering AW: %u\n", vm_data->aw);

    vm_data->gpu_lost = false;
    vm_data->arb = arb;
    /* GSI index is assigned only later on mali_arbif_assign_domain */
    vm_data->gsi_idx = -1;
    vm_data->domain = NULL;
    list_add_tail(&vm_data->entry, &arb->reg_vms_list);

    spin_unlock(&arb->lock);
    return err;
}

int mali_arbif_assign_domain(struct mali_arbiter *arb, struct domain *d)
{
    int err = 0;
    struct mali_vm_data *vm_data = NULL;
    uint16_t aw_mask = 0;
    int gsi_idx;

    if ( !arb || !d )
        return -EINVAL;

    spin_lock(&arb->lock);
    if ( !find_vm_by_aw(arb, d->arch.mali_aw, &arb->reg_vms_list, &vm_data) )
    {
        printk(XENLOG_ERR "Domain %u (AW: %u) is not registered. Cannot assign\n",
               d->domain_id, d->arch.mali_aw);
        spin_unlock(&arb->lock);
        return -EINVAL;
    }
    spin_unlock(&arb->lock);

    if ( vm_data->domain )
    {
        printk(XENLOG_ERR "AW %u is already assigned to Domain %pd.\n",
               d->arch.mali_aw, vm_data->domain);
        return -EBUSY;
    }

    printk(XENLOG_INFO "Assigning domain %pd to AW %u.\n", d, vm_data->aw);
    vm_data->domain = d;

    gsi_idx = gsi_idx_from_aw(arb, vm_data->aw);
    if ( gsi_idx < 0 )
    {
        printk(XENLOG_ERR "Failed to find GSI instance for AW %u.\n",
              vm_data->aw);
        return gsi_idx;
    }
    vm_data->gsi_idx = gsi_idx;

    /* Check if the AW is already assigned to the GSI instance */
    err = mali_arbif_get_aw_assignment(arb, gsi_idx, &aw_mask);
    if ( err )
    {
        printk(XENLOG_ERR "Failed to get AW assignment for AW %u.\n",
               vm_data->aw);
        return err;
    }

    err = mali_arbif_set_aw_assignment(arb, gsi_idx,
                                       aw_mask | (1U << d->arch.mali_aw));
    if ( err )
    {
        printk(XENLOG_ERR "Failed to set AW assignment for AW %u.\n",
               vm_data->aw);
        return err;
    }
    printk(XENLOG_DEBUG "AW mask %x set for AW %u on GSI %d.\n",
           aw_mask | (1U << d->arch.mali_aw), vm_data->aw, gsi_idx);

    return 0;
}

void mali_arbif_unregister_vm(struct mali_vm_data *vm_data)
{
    if ( !vm_data )
    {
        printk(XENLOG_ERR "Cannot unregister: VM data is NULL\n");
        return;
    }

    spin_lock(&vm_data->arb->lock);
    if ( vm_is_in_wait_list(vm_data->arb, vm_data) )
        list_del_init(&vm_data->entry);

    if ( vm_is_in_reg_list(vm_data->arb, vm_data) )
        list_del_init(&vm_data->entry);
    spin_unlock(&vm_data->arb->lock);

    return;
}

int mali_arbif_unassign_domain(struct mali_arbiter *arb, struct domain *d)
{
    struct mali_vm_data *vm_data = NULL;

    if ( !arb || !d )
        return -EINVAL;

    spin_lock(&arb->lock);
    if ( !find_vm_by_aw(arb, d->arch.mali_aw, &arb->reg_vms_list, &vm_data) )
    {
        printk(XENLOG_ERR "Domain %u (AW: %u) is not registered.\n",
               d->domain_id, d->arch.mali_aw);
        spin_unlock(&arb->lock);
        return -EINVAL;
    }
    spin_unlock(&arb->lock);

    if ( !vm_data->domain )
    {
        printk(XENLOG_ERR "AW %u is not assigned to any domain.\n",
               d->arch.mali_aw);
        return -ENOENT;
    }
    printk(XENLOG_DEBUG "Unassigning domain %u from AW %u.\n",
           d->domain_id, vm_data->aw);

    vm_data->domain = NULL;

    return 0;
}

void mali_arbif_on_gpu_request(struct mali_vm_data *vm_data)
{
    int gsi_idx;
    struct mali_arbiter *arb = vm_data->arb;

    gsi_idx = gsi_idx_from_aw_locked(arb, vm_data->aw);
    if ( gsi_idx < 0 )
    {
        printk(XENLOG_DEBUG "Failed to find GSI instance for AW %u\n",
               vm_data->aw);
        spin_lock(&arb->lock);
        if ( vm_is_in_wait_list(arb, vm_data) )
        {
            spin_unlock(&arb->lock);
            return;
        }
        list_add_tail(&vm_data->entry, &arb->wait_list);
        spin_unlock(&arb->lock);
        printk(XENLOG_DEBUG "Added AW %u to wait list\n", vm_data->aw);
        return;
    }

    mali_gsi_on_gpu_request(vm_data, arb->gsi_info[gsi_idx].gsi);

    spin_unlock(&arb->gsi_info[gsi_idx].lock);
    return;
}

void mali_arbif_on_gpu_active(struct mali_vm_data *vm_data)
{
    int gsi_idx;
    struct mali_arbiter *arb = vm_data->arb;

    gsi_idx = gsi_idx_from_aw_locked(arb, vm_data->aw);
    if ( gsi_idx < 0 )
    {
        printk(XENLOG_ERR "Failed to find GSI instance for AW %u.\n",
               vm_data->aw);
        return;
    }

    mali_gsi_on_gpu_active(vm_data, arb->gsi_info[gsi_idx].gsi);

    spin_unlock(&arb->gsi_info[gsi_idx].lock);
    return;
}

void mali_arbif_on_gpu_idle(struct mali_vm_data *vm_data)
{
    int gsi_idx;
    struct mali_vm_data *cur_vm, *tmp_vm;
    struct mali_arbiter *arb = vm_data->arb;

    gsi_idx = gsi_idx_from_aw_locked(arb, vm_data->aw);
    if ( gsi_idx < 0 )
    {
        printk(XENLOG_DEBUG "Failed to find GSI instance for VM %u.\n",
               vm_data->aw);
        printk(XENLOG_DEBUG "Remove it from arbiter wait list\n");
        spin_lock(&arb->lock);
        list_for_each_entry_safe(cur_vm, tmp_vm, &arb->wait_list, entry)
        {
            if ( cur_vm->aw == vm_data->aw )
            {
                printk(XENLOG_DEBUG "Removing VM %u from wait list.\n",
                       vm_data->aw);
                list_del_init(&cur_vm->entry);
                break;
            }
        }
        spin_unlock(&arb->lock);
        return;
    }

    mali_gsi_on_gpu_idle(vm_data, arb->gsi_info[gsi_idx].gsi);

    spin_unlock(&arb->gsi_info[gsi_idx].lock);

    return;
}

void mali_arbif_gpu_stopped(struct mali_vm_data *vm_data, bool req_again)
{
    struct mali_arb_gsi *gsi = NULL;

    if ( !vm_data || !vm_data->arb )
    {
        printk(XENLOG_ERR
               "Invalid AW data or arbiter in mali_arbif_gpu_stopped.\n");
        return;
    }

    gsi = vm_data->arb->gsi_info[vm_data->gsi_idx].gsi;
    BUG_ON(!gsi);

    mali_gsi_on_gpu_stopped(vm_data, gsi, req_again);

    return;
}

int mali_arbif_get_max_config(struct mali_vm_data *vm_data,
                              uint32_t *max_l2_slices, uint32_t *max_core_mask)
{
    struct mali_arbiter *arb = vm_data->arb;

    if ( !arb || !max_l2_slices || !max_core_mask )
        return -EINVAL;

    get_max_l2_slices(vm_data->arb->rg, &arb->l2_slices);
    get_max_core_mask(vm_data->arb->rg, &arb->core_mask);

    *max_l2_slices = arb->l2_slices;
    *max_core_mask = arb->core_mask;

    return 0;
}

int mali_arbif_get_aw_assignment(struct mali_arbiter *arb, unsigned int gsi_idx,
                                 uint16_t *aw_mask)
{
    if ( !arb || !aw_mask || gsi_idx >= MALI_PTM_PARTITION_COUNT )
        return -EINVAL;

    if ( arb->gsi_info[gsi_idx].enabled )
    {
        spin_lock(&arb->gsi_info[gsi_idx].lock);
        *aw_mask = arb->gsi_info[gsi_idx].aw_mask;
        spin_unlock(&arb->gsi_info[gsi_idx].lock);
    }

    return 0;
}

int mali_arbif_set_aw_assignment(struct mali_arbiter *arb, unsigned int gsi_idx,
                                 uint16_t new_aw_mask)
{
    struct mali_arb_gsi *gsi_ptr;
    int ret = 0;
    unsigned int i, j;
    uint16_t current_aws, available_aws;
    struct mali_vm_data *cur_vm, *tmp_vm;

    if ( new_aw_mask > MAX_AW_MASK )
        return -EINVAL;

    if ( gsi_idx >= MALI_PTM_PARTITION_COUNT )
        return -EINVAL;

    if ( !arb->gsi_info[gsi_idx].enabled )
        return -EINVAL;

    /*
     * We first acquire the GSI info structure, followed by the arbiter
     * wait list lock, which we only take when managing the wait list.
     * Although there is no strict intrinsic locking order required,
     * it is important to note that we always take the GSI lock first.
     * This is because the GSI structure is the main data we aim to
     * protect when calling this function. The wait list lock is taken
     * only if necessary.
     */
    spin_lock(&arb->gsi_info[gsi_idx].lock);

    gsi_ptr = arb->gsi_info[gsi_idx].gsi;
    current_aws = arb->gsi_info[gsi_idx].aw_mask;

    if ( current_aws == new_aw_mask )
        goto exit;

    ret = rgif_get_aw_mask(arb->rg, &available_aws);
    if ( ret )
    {
        printk(XENLOG_ERR "Get Access Window Mask failed.\n");
        ret = -EIO;
        goto exit;
    }

    /* Check if the request is allowed */
    for ( i = 0; i < MALI_PTM_ACCESS_WINDOW_COUNT; i++ )
    {
        uint16_t cur_aw_bit = 1U << i;

        if ( !(new_aw_mask & cur_aw_bit) )
            continue;

        /* Check that the requested AW is in this RG */
        if ( !(available_aws & cur_aw_bit) )
        {
            ret = -EINVAL;
            goto exit;
        }

        /* Check that the requested AW is not in another gpu-subinstance */
        for ( j = 0; j < MALI_PTM_PARTITION_COUNT; j++ )
        {
            if ( !arb->gsi_info[j].enabled )
                continue;

            if ( j != gsi_idx &&
                (arb->gsi_info[j].aw_mask & cur_aw_bit) )
            {
                printk(XENLOG_ERR "AW%u already assigned to GSI%u\n", i, j);
                ret = -EINVAL;
                goto exit;
            }
        }
    }

    /* Stop gpu-subinstance */
    mali_gsi_stop(gsi_ptr);

    /* Update gsi_access_window */
    arb->gsi_info[gsi_idx].aw_mask = new_aw_mask;

    /*
     * Check arbiter wait list with new AW_MASK and add them to
     * new gpu-subinstance
     */
    spin_lock(&arb->lock);
    list_for_each_entry_safe(cur_vm, tmp_vm, &arb->wait_list, entry)
    {
        if ( (1U << cur_vm->aw) & new_aw_mask )
        {
            /* Remove the VM from Arbiter wait-queue */
            list_del_init(&cur_vm->entry);
            /* Add the VM to new gpu-subinstance */
            mali_gsi_on_gpu_request(cur_vm, gsi_ptr);
        }
    }
    spin_unlock(&arb->lock);

    /* Move requested VMs in AWs being unassigned into the arbiter wait_list */
    for ( i = 0; i < MALI_PTM_ACCESS_WINDOW_COUNT; i++ )
    {
        uint32_t cur_aw_bit = 1U << i;

        if ( (current_aws & cur_aw_bit) && !(new_aw_mask & cur_aw_bit) )
        {
            struct mali_vm_data *reg_vm;

            spin_lock(&arb->lock);
            list_for_each_entry(reg_vm, &arb->reg_vms_list, entry)
            {
                if ( reg_vm->aw == i )
                {
                    bool requested;

                    /* Remove the VM from the gpu-subinstance */
                    requested = mali_arbiter_gsi_remove_vm(gsi_ptr, reg_vm);

                    /* Add the VM to the wait-list if it was in req-list */
                    if ( requested )
                    {
                        list_del_init(&reg_vm->entry);
                        list_add_tail(&reg_vm->entry, &arb->wait_list);
                    }
                    break;
                }
            }
            spin_unlock(&arb->lock);
        }
    }

    /* Start gpu-subinstance */
    mali_gsi_start(gsi_ptr);
exit:
    spin_unlock(&arb->gsi_info[gsi_idx].lock);
    printk(XENLOG_DEBUG "AW mask 0x%x set for GSI%d\n", new_aw_mask, gsi_idx);
    return ret;
}

int mali_arbif_gpu_stop(struct mali_vm_data *vm_data)
{
    uint64_t message = 0;
    struct mali_arbiter *arb = vm_data->arb;
    int ret = 0;

    ret = arb_vm_gpu_stop_build_msg(&message);
    if ( ret )
        return ret;

    ret = ptm_msg_buff_write(&arb->rg->msg_handler.send_msgs,vm_data->aw, message);
    if ( ret )
        return ret;

    return ptm_msg_send(&arb->rg->msg_handler, vm_data->aw);
}

int mali_arbif_gpu_granted(struct mali_vm_data *vm_data, uint32_t freq)
{
    uint64_t message = 0;
    struct mali_arbiter *arb = vm_data->arb;
    int ret = 0;

    if ( !arb || !vm_data || freq == 0 )
        return -EINVAL;

    ret = arb_vm_gpu_granted_build_msg(freq, &message);
    if ( ret )
        return ret;

    ret = ptm_msg_buff_write(&arb->rg->msg_handler.send_msgs,vm_data->aw, message);
    if ( ret )
        return ret;

    return ptm_msg_send(&arb->rg->msg_handler, vm_data->aw);
}

int mali_arbif_gpu_lost(struct mali_vm_data *vm_data)
{
    uint64_t message = 0;
    struct mali_arbiter *arb = vm_data->arb;
    int ret = 0;

    ret = arb_vm_gpu_lost_build_msg(&message);
    if ( ret )
        return ret;

    printk(XENLOG_INFO "P%u: GPU lost for VM %u.\n", vm_data->aw, vm_data->aw);
    ret = ptm_msg_buff_write(&arb->rg->msg_handler.send_msgs,vm_data->aw, message);
    if ( ret )
        return ret;
    printk(XENLOG_DEBUG "P%u: Sending GPU lost message to VM %u.\n",
           vm_data->aw, vm_data->aw);

    return ptm_msg_send(&arb->rg->msg_handler, vm_data->aw);
}

/*
 * Local variables:
 * mode: C
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * End:
 */
