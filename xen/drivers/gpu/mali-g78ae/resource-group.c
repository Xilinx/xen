/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Mali G78AE GPU PTM Resource Group driver
 *
 * Copyright (C) 2025, Advanced Micro Devices, Inc.
 */
#include <xen/bootfdt.h>
#include <xen/irq.h>
#include <xen/init.h>
#include <xen/mm.h>
#include <xen/vmap.h>
#include <xen/sched.h>
#include <asm/io.h>
#include <asm/system.h>
#include <public/device_tree_defs.h>
#include "resource-group.h"
#include "device-tree.h"
#include "arb-vm-protocol.h"

/* Slice specific defs of the Partition Manager HW  */
#define PTM_SLICE_FEATURES 0x0008
#define PTM_SLICE_CORES0 0x000C
#define PTM_SLICE_CORES1 0x0010
#define PTM_SLICE_FEATURES_TILER_MASK 0x1
#define PTM_SLICE_FEATURES_STRIDE_MASK 0xFF
#define PTM_SLICE_CORE_COUNT_MASK 0xFF
#define PTM_SLICE_CORE_BITS_PER_SLICE 0x8

/* Resource Group specific defs of the Partition Manager HW */
#define PTM_RESOURCE_SLICE_MASK 0x0014
#define PTM_RESOURCE_PARTITION_MASK 0x0018
#define PTM_RESOURCE_AW_MASK 0x001C
#define PTM_RESOURCE_IRQ_RAWSTAT 0x0080
#define PTM_RESOURCE_IRQ_CLEAR 0x0084
#define PTM_RESOURCE_IRQ_MASK 0x0088
#define PTM_RESOURCE_IRQ_STATUS 0x008C
#define PTM_RESOURCE_SLICE_POWER_STATE 0x0098
#define PTM_RESOURCE_SLICE_POWER_SET 0x009C
#define PTM_RESOURCE_SLICE_RESET_STATE 0x00A8
#define PTM_RESOURCE_SLICE_CLOCK_SET 0x00A4
#define PTM_RESOURCE_SLICE_RESET_SET 0x00AC
#define PTM_RESOURCE_AW0_MESSAGE 0x0100

#define MAX_SLICE_MASK 0xFF
#define MAX_PARTITION_MASK 0xF

#define SLICE_CLOCK_MASK_BIT 0x03
#define CLOCK_REG_BITS_PER_SLICE 0x2
#define CLOCK_SET_AUTOMATIC 0x2
#define CLOCK_SET_DISABLE 0x0

#define SLICE_RESET_MASK_BIT 0x01
#define RESET_REG_BITS_PER_SLICE 0x1
#define RESET_SET_RESET 0x1

/* Register map layout values, used for determining hardware indices */
#define PTM_PT_OFFSET 0x20000
#define PTM_PT_STRIDE 0x20000

static int res_group_get_state(struct mali_ptm_rg *rg, uint32_t index,
                               enum ptm_rg_state *curr_state)
{
    if ( !rg || index > MALI_PTM_ACCESS_WINDOW_COUNT - 1 )
        return -EINVAL;

    if ( !spin_is_locked(&rg->prot_data[index].lock) )
        return -EBUSY;

    *curr_state = rg->prot_data[index].state;

    return 0;
}

static int res_group_set_state(struct mali_ptm_rg *rg,
                                enum ptm_rg_state new_state, uint32_t index)
{
    if ( !rg || index > MALI_PTM_ACCESS_WINDOW_COUNT - 1 )
        return -EINVAL;

    if ( !spin_is_locked(&rg->prot_data[index].lock) )
        return -EBUSY;

    rg->prot_data[index].state = new_state;

    return 0;
}

static void rg_isr(int irq, void *data)
{
    struct mali_ptm_rg *rg = data;
    uint32_t irq_status;
    uint8_t aw;
    uint64_t payload;
    int ret;

    /* Check if the IRQ in known. */
    if ( irq == rg->irq.line )
        irq_status = readl(rg->mem + PTM_RESOURCE_IRQ_STATUS) & MAX_AW_MASK;
    else
        return;

    if ( irq_status == 0 )
        return;

    aw = 0;
    do {
        if ( irq_status & 0x1 )
        {
            /**
             * Reads the message from the AW and schedules work to process it.
             *
             * Notes:
             * - Unlike the reference implementation, which uses
             *   `res_group_read_incoming_msg`, this implementation handles the
             *   message directly without a separate function.
             * - This is safe because initialization is always performed by the VM,
             *   and this code runs within Xen, which starts before any VMs.
             */
            ptm_msg_read(&rg->msg_handler, aw, &payload);
            ret = ptm_msg_buff_write(&rg->msg_handler.recv_msgs, aw, payload);
            if ( ret < 0 )
            {
                printk(XENLOG_ERR "RG message buffer write failed\n");
                return;
            }

            writel(1U << aw, rg->mem + PTM_RESOURCE_IRQ_CLEAR);
        }
        /* Check next AW. */
        ++aw;
        irq_status >>= 1;
    } while ( irq_status );

    /* Schedule the work to process the messages received. */
    tasklet_schedule_on_rnd(&rg->recv_task);

    return;
}

static int res_group_respond_init(struct mali_ptm_rg *rg, uint8_t aw,
                                  uint8_t recv_version)
{
    uint8_t sending_ack;
    uint8_t sending_version;
    uint32_t max_l2_slices = 0;
    uint32_t max_core_mask = 0;
    struct mali_vm_data *vm;
    int ret = 0;
    uint64_t message;

    if ( !rg->ptm_rg_vm[aw] )
        return -EINVAL;

    vm = rg->ptm_rg_vm[aw];

    /* Get maximum configuration from the Arbiter */
    mali_arbif_get_max_config(vm, &max_l2_slices, &max_core_mask);

    /* Init response should always be with ack = 1 */
    sending_ack = 1;

    /* If the received version is not supported the handshake fails */
    if ( recv_version < MIN_SUPPORTED_VERSION )
    {
        /* Send higher version to make sure the other side also fails */
        ret = res_group_set_state(rg, HANDSHAKE_FAILED, aw);
        if ( ret )
            return ret;
        sending_version = MIN_SUPPORTED_VERSION;
        printk(XENLOG_ERR "Protocol handshake failed with AW%d\n", aw);
        ret = -EPERM;
    }
    else
    {
        /* Send minimum of the maximum versions */
        ret = res_group_set_state(rg, HANDSHAKE_DONE, aw);
        if ( ret )
            return ret;
        sending_version = (recv_version < CURRENT_VERSION) ? recv_version : CURRENT_VERSION;
        /* Set protocol in use */
        rg->prot_data[aw].version_in_use = sending_version;
    }

    arb_vm_init_build_msg(max_l2_slices, max_core_mask, sending_ack,
                          sending_version, &message);
    ptm_msg_buff_write(&rg->msg_handler.send_msgs,aw, message);
    ptm_msg_send(&rg->msg_handler, aw);

    return ret;
}

static int res_group_validate_init(struct mali_ptm_rg *rg, uint8_t aw,
                                   uint8_t recv_version)
{
    int ret = 0;

    if ( !rg || aw > MALI_PTM_ACCESS_WINDOW_COUNT - 1 )
        return -EINVAL;

    /* If the received version is not supported the handshake fails */
    ret = res_group_set_state(rg, HANDSHAKE_FAILED, aw);
    if ( ret )
        return ret;
    if ( recv_version > CURRENT_VERSION ||
         recv_version < MIN_SUPPORTED_VERSION )
    {
        printk(XENLOG_ERR "Protocol handshake failed with AW %d.\n", aw);
        return -EPERM;
    }

    ret = res_group_set_state(rg, HANDSHAKE_DONE, aw);
    if ( ret )
        return ret;

    /* If supported the received version should be used */
    rg->prot_data[aw].version_in_use = recv_version;

    return 0;
}

static int process_protocol_handshake(struct mali_ptm_rg *rg, enum ptm_rg_state rg_state,
                      unsigned int aw, uint64_t *message)
{
    uint8_t recv_ack;
    uint8_t recv_version;
    uint8_t message_id;
    int ret = 0;

    printk(XENLOG_DEBUG "Processing protocol handshake with AW %d\n", aw);
    /* Decode the rest of the message. */
    if ( get_msg_id(message, &message_id) ||
         get_msg_protocol_version(message, &recv_version) ||
         get_msg_init_ack(message, &recv_ack) )
    {
        printk(XENLOG_ERR "Failed to decode message from AW %u\n", aw);
        return -EPERM;
    }

    if ( message_id != VM_ARB_INIT )
    {
        printk(XENLOG_ERR "Invalid message id %u from AW %u\n", message_id, aw);
        return -EPERM;
    }

    switch ( rg_state )
    {
        case HANDSHAKE_INIT:
        case HANDSHAKE_DONE:
            if ( recv_ack == 0 )
                /* If the other side initialized the handshake */
                ret = res_group_respond_init(rg, aw, recv_version);
            else
                ret = -EINVAL;
            break;
        case HANDSHAKE_IN_PROGRESS:
            if ( recv_ack == 1 )
                /* If the other side has responded the init. */
                ret = res_group_validate_init(rg, aw, recv_version);
            else
            {
                printk(XENLOG_DEBUG
                       "Protocol handshake with AW %u started by RG.\n", aw);
                ret = -EPERM;
            }
            break;
        case HANDSHAKE_FAILED:
            printk(XENLOG_ERR
                   "Unexpected message from AW %u. Handshake failed.\n", aw);
            ret = -EPERM;
            break;
        default:
            break;
    }

    return ret;
}

static void recv_msg_worker(struct msg_worker_params *params)
{
    uint64_t message;
    struct mali_ptm_rg *rg = (struct mali_ptm_rg *)params->data;
    int ret;
    uint8_t message_id;
    uint8_t gpu_request = 0;
    bool stop_restart = false;
    enum ptm_rg_state rg_state = HANDSHAKE_INIT;
    uint8_t aw = params->aw;
    struct mali_arbiter* arbiter;
    struct mali_vm_data* arb_vm;

    /* Read the incoming message, overwriting if necessary */
    ret = ptm_msg_buff_read(&rg->msg_handler.recv_msgs, aw, &message);
    if ( ret )
    {
        printk(XENLOG_ERR "Failed to get message (%d)\n", ret);
        return;
    }

    /* Process the message */
    ret = get_msg_id(&message, &message_id);
    if ( ret )
    {
        printk(XENLOG_ERR "Failed to get message id (%d)\n", ret);
        return;
    }

    spin_lock(&rg->prot_data[aw].lock);

    /* Get information about the VM which the message was received from. */
    arb_vm = rg->ptm_rg_vm[aw];
    if ( !arb_vm )
    {
        printk(XENLOG_ERR "No VM data for AW %d\n", aw);
        goto cleanup_lock;
    }
    ret = res_group_get_state(rg, aw, &rg_state);
    if ( ret )
    {
        printk(XENLOG_ERR "Failed to get RG state (%d)\n", ret);
        goto cleanup_lock;
    }

    /* Deal with the protocol handshake if VM_ARB_INIT */
    if ( message_id == VM_ARB_INIT )
    {
        if ( get_msg_init_request(&message, &gpu_request) )
        {
            printk(XENLOG_ERR "Failed to get gpu_request from message\n");
            goto cleanup_lock;
        }
        /* If gpu_request = 1, there is no reason to send stop */
        if ( gpu_request && atomic_read(&rg->prot_data[aw].posthandshake_stop) )
        {
            stop_restart = true;
            atomic_set(&rg->prot_data[aw].posthandshake_stop, 0);
        }

        /* First process the handshake on INIT message */
        ret = process_protocol_handshake(rg, rg_state, aw, &message);
        if ( ret )
        {
            printk(XENLOG_ERR "Failed to process handshake (%d)\n", ret);
            goto cleanup_lock;
        }
    }
    else if ( rg_state != HANDSHAKE_DONE )
    {
        printk(XENLOG_ERR "Handshake not completed, MSG from AW %d ignored\n",
               aw);
        ret = -EPERM;
        goto cleanup_lock;
    }

    spin_unlock(&rg->prot_data[aw].lock);

    arbiter = rg->arbiter;
    if ( !arbiter )
    {
        printk(XENLOG_ERR "No arbiter data for AW %d\n", aw);
        return;
    }

    /* Now that handshake is done, process the msg using arbiter interface */
    switch ( message_id )
    {
        case VM_ARB_INIT:
            /* If gpu_request = 1, gpu_request must be called for that VM.*/
            if ( gpu_request && stop_restart )
                mali_arbif_gpu_stopped(arb_vm, true);
            else if ( gpu_request )
                mali_arbif_on_gpu_request(arb_vm);
            break;
        case VM_ARB_GPU_IDLE:
            mali_arbif_on_gpu_idle(arb_vm);
            break;
        case VM_ARB_GPU_ACTIVE:
            mali_arbif_on_gpu_active(arb_vm);
            break;
        case VM_ARB_GPU_REQUEST:
            mali_arbif_on_gpu_request(arb_vm);
            break;
        case VM_ARB_GPU_STOPPED:
            {
                uint8_t priority;

                ret = get_msg_priority(&message, &priority);
                if ( ret )
                    return;

                /*
                * While priority mechanism is not implemented by the protocol,
                * we will use the priority field to pass the req_again param
                * for the GPU_STOPPED message.
                */
                mali_arbif_gpu_stopped(arb_vm, (priority != 0));
            }
            break;
        default:
            break;
    }

    return;

cleanup_lock:
    spin_unlock(&rg->prot_data[aw].lock);
    return;
}

/**
 * res_group_process_message() - Worker thread for processing received messages
 * @data: Work contained within the device data.
 *
 * Process the receiving commands and schedule the required work to handle them
 */
static void res_group_process_message(void *data)
{
    struct mali_ptm_rg *ptm_rg = (struct mali_ptm_rg *)data;

    /* Process pending incoming messages */
    if ( ptm_rg->msg_handler.recv_msgs.mask )
        ptm_msg_process_msgs(&ptm_rg->msg_handler.recv_msgs, ptm_rg, recv_msg_worker);
    /* If there are still messages in the buffer, schedule the work again */
    if ( ptm_rg->msg_handler.recv_msgs.mask )
        tasklet_schedule_on_rnd(&ptm_rg->recv_task);

    return;
}


void rgif_get_slices_core_mask(struct mali_ptm_rg *rg, uint64_t *core_mask,
                              uint8_t *core_mask_stride)
{
    uint64_t slice_cores;
    uint32_t slice_features;
    uint32_t slice_count = 0;
    uint64_t slice_core_mask = 0;

    /* Read the number of cores per slice */
    slice_cores = readl(rg->mem + PTM_SLICE_CORES1);
    slice_cores <<= 32;
    slice_cores |= readl(rg->mem + PTM_SLICE_CORES0);

    /* Read the core mask stride of the cores in the shader_present. */
    slice_features = readl(rg->mem + PTM_SLICE_FEATURES);
    *core_mask_stride = slice_features & PTM_SLICE_FEATURES_STRIDE_MASK;

    *core_mask = 0;
    /* Build the correct bitmap with the overall mask. */
    for ( slice_count = 0; slice_count < MALI_PTM_SLICES_COUNT; slice_count++ )
    {
        slice_core_mask = (1ULL << (slice_cores & PTM_SLICE_CORE_COUNT_MASK)) - 1;
        *core_mask |= slice_core_mask << (*core_mask_stride * slice_count);
        slice_cores >>= hweight32(PTM_SLICE_CORE_COUNT_MASK);
    }

    return;
}

int rgif_get_slice_mask(struct mali_ptm_rg *rg, uint32_t *slice_mask)
{
    uint32_t slice_mask_reg;
    uint32_t slice_mask_val = 0;
    uint64_t slice_cores;
    unsigned int i = 0;

    slice_mask_reg = readl(rg->mem + PTM_RESOURCE_SLICE_MASK);
    if ( slice_mask_reg > MAX_SLICE_MASK )
    {
        printk(XENLOG_ERR
               "RG%u: Hardware reports out of range slice mask.\n", rg->id);
        return -EIO;
    }

    slice_cores = readl(rg->mem + PTM_SLICE_CORES1);
    slice_cores <<= 32;
    slice_cores |= readl(rg->mem + PTM_SLICE_CORES0);

    while ( (slice_mask_reg != 0) &&
            (slice_cores != 0) && (i < MALI_PTM_SLICES_COUNT) )
    {
        if ( (slice_mask_reg & 0x1) &&
                (slice_cores & PTM_SLICE_CORE_COUNT_MASK) )
            slice_mask_val |= (1U << i);
        slice_cores >>= PTM_SLICE_CORE_BITS_PER_SLICE;
        slice_mask_reg >>= 1;
        i++;
    }

    *slice_mask = slice_mask_val;

    return 0;
}

int rgif_get_partition_mask(struct mali_ptm_rg *rg, uint32_t *partition_mask)
{
    uint32_t partition_mask_val;

    partition_mask_val = readl(rg->mem + PTM_RESOURCE_PARTITION_MASK);
    if ( partition_mask_val > MAX_PARTITION_MASK )
    {
        printk(XENLOG_ERR
            "RG%u: Hardware reports out of range partition mask.\n", rg->id);
        return -EIO;
    }

    *partition_mask = partition_mask_val;

    return 0;
}

int rgif_get_aw_mask(struct mali_ptm_rg *rg, uint16_t *aw_mask)
{
    uint16_t cur_aw_mask;

    /* No need for locking as these are read-only */
    cur_aw_mask = readl(rg->mem + PTM_RESOURCE_AW_MASK);

    /* The only possible check in this case is to the range expected */
    if ( cur_aw_mask > MAX_AW_MASK )
        BUG_ON("Hardware reports out of range aw mask.\n");

    *aw_mask = cur_aw_mask;

    return 0;
}

int rgif_poweron_slices(struct mali_ptm_rg *rg, uint32_t slice_mask)
{
    uint32_t slice_power_set_val;
    uint32_t slice_power_status_read;
    int err = 0;

    /* If the slice mask is zero, there is no work to do */
    if ( slice_mask == 0 )
        return 0;

    printk(XENLOG_DEBUG "Powering on slices %08X\n", slice_mask);

    /* Read both power and clock set registers */
    spin_lock(&rg->lock);
    slice_power_set_val = readl(rg->mem + PTM_RESOURCE_SLICE_POWER_SET);

    /*
     * The mask represents just the slices to power on so we can't power off
     * other slices that might be already on but not in the mask
     */
    slice_power_set_val |= slice_mask;

    /* Write the result to both power and clock set registers */
    writel(slice_power_set_val, rg->mem + PTM_RESOURCE_SLICE_POWER_SET);

    /*
     * Read back the power status register to check that the new setting
     * was applied
     */
    err = readx_poll_timeout(readl, rg->mem + PTM_RESOURCE_SLICE_POWER_STATE,
                 slice_power_status_read,
                 ((slice_power_status_read & slice_mask) == slice_mask),
                 REG_POLL_SLEEP_US, REG_POLL_TIMEOUT_US);
    if ( err )
    {
        printk(XENLOG_ERR "Power state of slices not expected %08X != %08X\n",
            slice_power_status_read, slice_power_set_val);
        err = -EIO;
    }

    spin_unlock(&rg->lock);
    return err;
}

int rgif_poweroff_slices(struct mali_ptm_rg *rg, uint32_t slice_mask)
{
    uint32_t slice_power_set_val;
    uint32_t slice_power_status_read;
    int err = 0;

    /* If the slice mask is zero, there is no work to do */
    if ( slice_mask == 0 )
        return 0;

    /* Read both power and clock set registers */
    spin_lock(&rg->lock);
    slice_power_set_val = readl(rg->mem + PTM_RESOURCE_SLICE_POWER_SET);

    /*
     * The mask represents just the slices to power off so we can't power
     * off other slices that might be on but not in the mask
     */
    slice_power_set_val &= ~slice_mask;

    /* Write the result to power set register */
    writel(slice_power_set_val, rg->mem + PTM_RESOURCE_SLICE_POWER_SET);

    /*
     * Read back the power status register to check that the new setting
     * was applied
     */
    err = readx_poll_timeout(readl, rg->mem + PTM_RESOURCE_SLICE_POWER_STATE,
                 slice_power_status_read,
                 ((slice_power_status_read & slice_mask) == 0), REG_POLL_SLEEP_US,
                 REG_POLL_TIMEOUT_US);
    if ( err )
    {
        printk(XENLOG_ERR "Power state of slices not expected %08X != %08X\n",
                slice_power_status_read, slice_power_set_val);
        err = -EIO;
    }
    spin_unlock(&rg->lock);
    return err;
}

uint32_t rgif_get_powered_slices_mask(struct mali_ptm_rg *rg)
{
    uint32_t mask = 0;
    spin_lock(&rg->lock);
    mask = ~readl(rg->mem + PTM_RESOURCE_SLICE_POWER_STATE) & MAX_SLICE_MASK;
    spin_unlock(&rg->lock);
    return mask;
}

int rgif_enable_slices(struct mali_ptm_rg *rg, uint32_t slice_mask)
{
    uint32_t slice_reset_set_val;
    uint32_t slice_clock_set_val;
    uint32_t slice_reset_status_read;
    uint32_t reset_bit_val;
    uint32_t clock_bit_val;
    unsigned int i;
    int err = 0;

    printk(XENLOG_DEBUG "Enabling slices %08X\n", slice_mask);
    /* If the slice mask is zero, there is no work to do */
    if ( slice_mask == 0 )
        return 0;

    spin_lock(&rg->lock);
    slice_reset_set_val = readl(rg->mem + PTM_RESOURCE_SLICE_RESET_STATE);
    slice_clock_set_val = readl(rg->mem + PTM_RESOURCE_SLICE_CLOCK_SET);

    /*
     * The mask represents just the slices to release so we can't reset
     * other slices that might be on released already but not in the mask
     */
    for ( i = 0; (i < MALI_PTM_SLICES_COUNT) && (slice_mask != 0); ++i )
    {
        if ( slice_mask & 0x1 )
        {
            printk(XENLOG_DEBUG "Processing slice %d\n", i);
            reset_bit_val = SLICE_RESET_MASK_BIT;
            slice_reset_set_val &= ~(reset_bit_val << (i * RESET_REG_BITS_PER_SLICE));

            /* Just turn ON slices currently disabled */
            clock_bit_val = (slice_clock_set_val >> (i * CLOCK_REG_BITS_PER_SLICE)) &
                            SLICE_CLOCK_MASK_BIT;

            if ( clock_bit_val == CLOCK_SET_DISABLE )
            {
                clock_bit_val = CLOCK_SET_AUTOMATIC;
                slice_clock_set_val |= clock_bit_val
                                       << (i * CLOCK_REG_BITS_PER_SLICE);
            }
        }
        slice_mask >>= 1;
    }

    /* Write the result to the reset and clock set registers */
    writel(slice_reset_set_val, rg->mem + PTM_RESOURCE_SLICE_RESET_SET);
    err = readx_poll_timeout(readl, rg->mem + PTM_RESOURCE_SLICE_RESET_STATE,
                 slice_reset_status_read,
                 (slice_reset_status_read == slice_reset_set_val),
                 REG_POLL_SLEEP_US, REG_POLL_RESET_TIMEOUT_US);
    if ( err )
    {
        printk(XENLOG_ERR
               "Reset state of slices is in a wrong state %08X != %08X\n",
               slice_reset_status_read, slice_reset_set_val);
        err = -EIO;
        goto fail;
    }

    writel(slice_clock_set_val, rg->mem + PTM_RESOURCE_SLICE_CLOCK_SET);

    err = 0;
fail:
    spin_unlock(&rg->lock);
    return err;
}

uint32_t rgif_get_enabled_slices_mask(struct mali_ptm_rg *rg)
{
    uint32_t mask;

    spin_lock(&rg->lock);
    mask = (~readl(rg->mem + PTM_RESOURCE_SLICE_RESET_STATE)) & MAX_SLICE_MASK;
    spin_unlock(&rg->lock);
    return mask;
}

int rgif_reset_slices(struct mali_ptm_rg *rg, uint32_t slice_mask)
{
    uint32_t slice_reset_set_val;
    uint32_t slice_reset_status_read;
    uint32_t slice_clock_set_val;
    uint32_t reset_bit_val;
    uint32_t clock_bit_val;
    unsigned int i;
    int err = 0;

    /* If the slice mask is zero, there is no work to do */
    if ( slice_mask == 0 )
        return 0;

    /* Read the current clock and reset set registers. */
    spin_lock(&rg->lock);
    slice_reset_set_val = readl(rg->mem + PTM_RESOURCE_SLICE_RESET_STATE);
    slice_clock_set_val = readl(rg->mem + PTM_RESOURCE_SLICE_CLOCK_SET);

    /*
     * The mask represents just the slices to reset so we can't release
     * other slices that might be on reset already but not in the mask.
     */
    for ( i = 0; (i < MALI_PTM_SLICES_COUNT) && (slice_mask != 0); ++i )
    {
        /* Turn OFF slices with clock enabled (1) or automatic (2). */
        if ( slice_mask & 0x1 )
        {
            clock_bit_val = SLICE_CLOCK_MASK_BIT;
            slice_clock_set_val &=
                    ~(clock_bit_val << (i * CLOCK_REG_BITS_PER_SLICE));
            reset_bit_val = RESET_SET_RESET;
            slice_reset_set_val |=
                    reset_bit_val << (i * RESET_REG_BITS_PER_SLICE);
        }
        slice_mask >>= 1;
    }

    /* Write the result to the clock and reset set registers. */
    writel(slice_reset_set_val, rg->mem + PTM_RESOURCE_SLICE_RESET_SET);
    err = readx_poll_timeout(readl, rg->mem + PTM_RESOURCE_SLICE_RESET_STATE,
                 slice_reset_status_read,
                 (slice_reset_status_read == slice_reset_set_val),
                 REG_POLL_SLEEP_US, REG_POLL_RESET_TIMEOUT_US);
    if ( err )
    {
        printk(XENLOG_ERR
               "Reset state of slices is in a wrong state %08X != %08X\n",
               slice_reset_status_read, slice_reset_set_val);
        err = -EIO;
        goto fail;
    }

    /* clock needs to be deassert after reset to allow a correct reset
     * state transition
     */
    writel(slice_clock_set_val, rg->mem + PTM_RESOURCE_SLICE_CLOCK_SET);

    err = 0;
fail:
    spin_unlock(&rg->lock);
    return err;
}

static int __init mali_ptm_rg_get_id(struct dt_device_node *rg_node)
{
    const __be32 *prop;
    paddr_t addr;
    uint32_t reg_len;

    prop = dt_get_property(rg_node, "reg", &reg_len);
    if ( !prop )
        return -EINVAL;

    addr = dt_read_paddr(prop, dt_n_addr_cells(rg_node));
    return MALI_NODE_PTM_RESOURCE_GROUP_ID_SAFE(addr);
}

/* Register all VMs managed by this resource group */
static int __init mali_ptm_rg_register_vms(struct mali_ptm_rg *rg)
{
    uint16_t aw_mask;
    unsigned int aw = 0;
    int err;

    /* Get the assigned AWs to this RG */
    err = rgif_get_aw_mask(rg, &aw_mask);
    if ( err )
        return err;

    /* Register the AWs */
    while ( aw_mask )
    {
        if ( (aw_mask & 0x1) )
        {
            rg->ptm_rg_vm[aw] = xvzalloc(struct mali_vm_data);
            if ( !rg->ptm_rg_vm[aw] )
                return -ENOMEM;

            INIT_LIST_HEAD(&rg->ptm_rg_vm[aw]->sched_entry);
            INIT_LIST_HEAD(&rg->ptm_rg_vm[aw]->entry);
            INIT_LIST_HEAD(&rg->ptm_rg_vm[aw]->wait_entry);
            rg->ptm_rg_vm[aw]->aw = aw;
            rg->ptm_rg_vm[aw]->gsi_idx = rg->id;
            rg->ptm_rg_vm[aw]->arb = rg->arbiter;

            mali_arbif_register_vm(rg->arbiter, rg->ptm_rg_vm[aw]);
        }
        aw++;
        aw_mask >>= 1;
    }

    return 0;
}

static int __init mali_ptm_rg_unregister_vms(struct mali_ptm_rg *rg)
{
    uint16_t aw_mask;
    unsigned int aw = 0;
    int err;

    /* Get the assigned AWs to this RG */
    err = rgif_get_aw_mask(rg, &aw_mask);
    if ( err )
        return err;

    /* Unregister the AWs */
    while ( aw_mask )
    {
        if ( (aw_mask & 0x1) && rg->ptm_rg_vm[aw] )
        {
            mali_arbif_unregister_vm(rg->ptm_rg_vm[aw]);
            xfree(rg->ptm_rg_vm[aw]);
            rg->ptm_rg_vm[aw] = NULL;
        }
        aw++;
        aw_mask >>= 1;
    }

    return 0;
}

static int __init scan_interface(struct dt_device_node *node,
                struct mali_ptm_rg *rgs,
                struct mali_ptm_part_ctrl *partition_controls,
                struct mali_ptm_part_cfg *partition_configs)
{
    paddr_t base, size;
    struct dt_device_node *part, *child;
    int ret = 0;
    int child_idx=-1;
    int part_nodes;
    uint32_t part_nodes_array[MALI_PARTITION_COUNT_MAX];
    uint32_t part_ctrl_mask = 0;
    uint32_t slices = 0;
    struct mali_ptm_rg *rg;
    unsigned int i;

    dt_for_each_child_node(node, child)
    {
        if ( !dt_device_is_compatible(child, MALI_GPU_RG_PTM_DT_NAME) )
           continue;

        ret = dt_device_get_paddr(child, 0, &base, &size);
        if ( ret )
        {
            printk(XENLOG_ERR "gpu: Failed to get res group base address\n");
            return -ENXIO;
        }

        ret = mali_ptm_rg_get_id(child);
        if ( ret < 0 )
        {
            printk(XENLOG_ERR "Error: Invalid index for resource group: %s\n",
                   child->full_name);
            return -EINVAL;
        }
        if ( rgs[ret].base != 0 )
        {
            printk(XENLOG_ERR "Error: Found duplicate resource group: %s\n",
                   child->full_name);
            return -EINVAL;
        }

        rg = &rgs[ret];
        rg->id = ret;

        rg->base = base;
        rg->size = size;
        rg->mem = ioremap_nocache(base, size);
        rg->irq.line = platform_get_irq(child, 0);
        if ( rg->irq.line < 0 )
        {
            printk(XENLOG_ERR "gpu: Failed to get res group IRQ\n");
            ret = -ENXIO;
            goto out_release_rg;
        }
        printk(XENLOG_DEBUG "RG%d: base=0x%"PRIpaddr" size=0x%"PRIpaddr" IRQ=%d\n",
               ret, rg->base, rg->size, rg->irq.line);

        /* Get the partitions config nodes associated with this RG */
        part_nodes = dt_property_read_variable_u32_array(child,
                "partition-config", part_nodes_array, 1,
                MALI_PARTITION_COUNT_MAX);
        if ( part_nodes < 0 )
        {
            printk(XENLOG_ERR
                   "Error: Failed to read partition-config property\n");
            ret = -EINVAL;
            goto out_release_rg;
        }

        if ( part_nodes > 1 )
            printk(XENLOG_WARNING "Warning: Found more than one partition-config node, using the first one\n");

        for ( i = 0; i < part_nodes; i++ )
        {
            part = dt_find_node_by_phandle(part_nodes_array[i]);
            if ( !part )
            {
                printk(XENLOG_ERR "Error: Failed to get partition-config node with phandle %u\n",
                       part_nodes_array[i]);
                ret = -EINVAL;
                goto out_release_rg;
            }
            child_idx = mali_ptm_part_cfg_get_id(part);
            if ( child_idx < 0 )
            {
                printk(XENLOG_ERR
                       "PTM: Failed to get partition config ID for %s\n",
                        part->full_name);
                ret = -EINVAL;
                goto out_release_rg;
            }
            rg->cfg[child_idx] = &partition_configs[child_idx];
            rg->partition_mask |= (1U << child_idx);
        }

        /*
         * By default, this section powers on all GPU slices and assigns them to
         * the first partition. The condition `part_nodes < 0` indicates that at
         * least one partition configuration node was found.
         */
        /* Initialize rg->lock before any function that acquires it */
        spin_lock_init(&rg->lock);

        /* Power on and enable all slices assigned to this RG */
        rgif_get_slice_mask(rg, &slices);
        rgif_reset_slices(rg, slices);
        rgif_poweroff_slices(rg, slices);
        cfgif_assign_slices(rg->cfg[child_idx], slices);
        rgif_poweron_slices(rg, slices);
        rgif_enable_slices(rg, slices);

        /* Get the partition controls node associated with this RG */
        part_nodes = dt_property_read_variable_u32_array(child,
                        "partition-control", part_nodes_array, 1,
                        MALI_PARTITION_COUNT_MAX);
        if ( part_nodes < 0 )
        {
            printk(XENLOG_ERR "Error: Failed to read partition-control property\n");
            ret = -EINVAL;
            goto out_release_rg;
        }

        for ( i = 0; i < part_nodes; i++ )
        {
            part = dt_find_node_by_phandle(part_nodes_array[i]);
            if ( !part )
            {
                ret = -EINVAL;
                goto out_release_rg;
            }

            child_idx = mali_ptm_part_ctrl_get_id(part);
            if ( child_idx < 0 )
            {
                printk(XENLOG_ERR
                       "PTM: Failed to get partition control ID for %s\n",
                        part->full_name);
                ret = -EINVAL;
                goto out_release_rg;
            }
            rg->ctrl[child_idx] = &partition_controls[child_idx];
            part_ctrl_mask |= (1U << child_idx);
        }

        /*
         * There should be a matching between the partition control and
         * partition config nodes. For example, if there are 2 partition
         * control nodes, there should be 2 partition config nodes with the
         * same IDs.
         */
        if ( part_ctrl_mask != rg->partition_mask )
        {
            printk(XENLOG_ERR "Error: Partition control and partition config nodes mismatch\n");
            ret = -EINVAL;
            goto out_release_rg;
        }

        ret = mali_arbiter_create(&rg->arbiter, rg);
        if ( ret )
        {
            printk(XENLOG_ERR "Failed to create arbiter for resource group %d\n",
                   rg->id);
            goto out_release_rg;
        }

        ret = mali_ptm_rg_register_vms(rg);
        if ( ret )
        {
            printk(XENLOG_ERR "Failed to register VMs for resource group %d\n",
                   rg->id);
            goto out_arb_destroy;
        }

        softirq_tasklet_init(&rg->recv_task, res_group_process_message, rg);

        for ( i = 0; i < MALI_PTM_ACCESS_WINDOW_COUNT; i++ )
        {
            rg->prot_data[i].version_in_use = 0;
            rg->prot_data[i].state = HANDSHAKE_INIT;
            spin_lock_init(&rg->prot_data[i].lock);
            atomic_set(&rg->prot_data[i].posthandshake_stop, 0);
        }

        /* request IRQ. */
        ret = request_irq(rg->irq.line,
                            IRQF_SHARED, rg_isr, "ptm_rg", rg);
        if ( ret )
        {
            printk(XENLOG_ERR "Can't request interrupt. Err: %d\n", ret);
            goto out_arb_destroy;
        }

        /* Initialize the msg_handler */
        ptm_msg_handler_init(&rg->msg_handler,
                 rg->mem + PTM_RESOURCE_AW0_MESSAGE,
                 MALI_PTM_ACCESS_WINDOW_COUNT,
                 ARB_TO_VM_BUFF_SIZE, VM_TO_ARB_BUFF_SIZE);

        /* Enable all IRQs */
        writel(MAX_AW_MASK, rg->mem + PTM_RESOURCE_IRQ_MASK);
    }

    return 0;

out_arb_destroy:
    mali_ptm_rg_unregister_vms(rg);
    if ( rg->arbiter )
        mali_arbiter_destroy(rg->arbiter);
out_release_rg:
    if ( rg->mem )
        iounmap(rg->mem);
    return ret;
}

int __init mali_ptm_rg_init(struct dt_device_node *mali_gpu_node,
                    struct mali_ptm_part_ctrl *partition_controls,
                    struct mali_ptm_part_cfg *partition_configs,
                    struct mali_ptm_rg *rg)
{
    unsigned int idx;
    int ret = 0;
    struct dt_device_node *ptm_interface;

    for ( idx = 0; idx < MALI_PTM_PARTITION_COUNT; idx++ )
    {
        rg[idx].base = 0;
        rg[idx].size = 0;
    }

    dt_for_each_child_node(mali_gpu_node, ptm_interface)
    {
        if ( !dt_device_is_compatible(ptm_interface, MALI_GPU_IF_PTM_DT_NAME) )
            continue;
        ret = scan_interface(ptm_interface, rg,
                             partition_controls, partition_configs);
        if ( ret )
        {
            printk(XENLOG_ERR
                   "PTM: Failed to scan ptm_interface for part. control: %s\n",
                ptm_interface->full_name);
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