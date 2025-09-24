/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Mali G78AE GPU PTM System driver
 *
 * Copyright (C) 2025, Advanced Micro Devices, Inc.
 */
#include <xen/init.h>
#include <xen/irq.h>
#include <xen/mm.h>
#include <xen/vmap.h>
#include <asm/io.h>

#include "common.h"
#include "device-tree.h"
#include "system.h"

#define PTM_IRQ_CLEAR (0x004C)
#define PTM_UNCORRECTED_ERROR_IRQ_MASK (0x0058)
#define PTM_UNCORRECTED_ERROR_IRQ_STATUS (0x0070)
#define PTM_AW0_STREAM_ID (0x1000)
#define PTM_AW0_PROTECTED_STREAM_ID (0x1004)
#define PTM_AW_STREAM_ID_STRIDE (0x08)
#define PTM_ERROR_FINGER_PRINT (0x0094)
#define PTM_GROUP_RESET_STATE (0x0098)
#define PTM_GROUP_RESET_SET (0x009C)
#define PTM_GROUP_SOFT_RESET (0x1)
#define PTM_GROUP_RESET_SET_BITS (0x2)
#define PTM_GROUP_RESET_STATE_BITS (0x4)
#define PTM_SYSTEM_NUM_GROUPS 4
#define OFFSET_4B 4
/* Two 96-bit of PTM_UNCORRECTED_ERROR_IRQ_MASK and PTM_DEFERRED_ERROR_IRQ_MASK */
#define NUM_IRQ_REGISTERS_PER_TYPE 3
/* note: mask doesn't have GENERAL_HARDWARE bit */
#define IRQ_ENABLE_MASK_LOW (0x1FFFFFF7)
#define IRQ_ENABLE_MASK_MID (0xFFFF)
#define IRQ_ENABLE_MASK_HIGH (0xFFFF)
#define LOW_IRQS 0
#define MID_IRQS 1

/* IRQ names, bits 0-47 */
static const char *const irq_name_low[] = {
    "IRQ_AXI_A_PARITY", "IRQ_AXI_B_PARITY", "IRQ_AXI_C_PARITY",
    "IRQ_GENERAL_HARDWARE", "IRQ_SLICE0_PARITY", "IRQ_SLICE1_PARITY",
    "IRQ_SLICE2_PARITY", "IRQ_SLICE3_PARITY", "IRQ_SLICE4_PARITY",
    "IRQ_SLICE5_PARITY", "IRQ_SLICE6_PARITY","IRQ_SLICE7_PARITY",
    "IRQ_SLICE0_ISOLATION", "IRQ_SLICE1_ISOLATION", "IRQ_SLICE2_ISOLATION",
    "IRQ_SLICE3_ISOLATION", "IRQ_SLICE4_ISOLATION", "IRQ_SLICE5_ISOLATION",
    "IRQ_SLICE6_ISOLATION", "IRQ_SLICE7_ISOLATION", "IRQ_SLICE0_BIST",
    "IRQ_SLICE1_BIST", "IRQ_SLICE2_BIST", "IRQ_SLICE3_BIST",
    "IRQ_SLICE4_BIST", "IRQ_SLICE5_BIST", "IRQ_SLICE6_BIST",
    "IRQ_SLICE7_BIST", "IRQ_INVALID_BUS"
};

/* IRQ names, bits 32-47 */
static const char *const irq_name_mid[] = {
    "IRQ_GROUP0_WATCHDOG", "IRQ_GROUP1_WATCHDOG", "IRQ_GROUP2_WATCHDOG",
    "IRQ_GROUP3_WATCHDOG", "IRQ_PARTITION0_LOCKED_ACCESS",
    "IRQ_PARTITION1_LOCKED_ACCESS", "IRQ_PARTITION2_LOCKED_ACCESS",
    "IRQ_PARTITION3_LOCKED_ACCESS", "IRQ_PARTITION0_LOCKED_BIST",
    "IRQ_PARTITION1_LOCKED_BIST", "IRQ_PARTITION2_LOCKED_BIST",
    "IRQ_PARTITION3_LOCKED_BIST", "IRQ_PARTITION0_MTCRC",
    "IRQ_PARTITION1_MTCRC","IRQ_PARTITION2_MTCRC", "IRQ_PARTITION3_MTCRC"
};

/* IRQ names, bits 64-79 */
static const char *const irq_name_high[] = {
    "IRQ_AW0_INVALID_ACCESS", "IRQ_AW1_INVALID_ACCESS",
    "IRQ_AW2_INVALID_ACCESS", "IRQ_AW3_INVALID_ACCESS",
    "IRQ_AW4_INVALID_ACCESS", "IRQ_AW5_INVALID_ACCESS",
    "IRQ_AW6_INVALID_ACCESS", "IRQ_AW7_INVALID_ACCESS",
    "IRQ_AW8_INVALID_ACCESS", "IRQ_AW9_INVALID_ACCESS",
    "IRQ_AW10_INVALID_ACCESS", "IRQ_AW11_INVALID_ACCESS",
    "IRQ_AW12_INVALID_ACCESS", "IRQ_AW13_INVALID_ACCESS",
    "IRQ_AW14_INVALID_ACCESS", "IRQ_AW15_INVALID_ACCESS"
};

enum irq_type { UNCORRECTED, DEFERRED };

/**
 * set_system_irq_masks() - Writes the IRQ masks to the registers
 * @system: Internal system device data
 */
static void set_system_irq_masks(struct mali_ptm_system *system)
{
    unsigned int i;

    if ( WARN_ON(!system) )
        return;

    for ( i = 0; i < NUM_IRQ_REGISTERS; ++i )
        writel(system->irq.mask[i],
              system->mem + PTM_UNCORRECTED_ERROR_IRQ_MASK + (OFFSET_4B * i));
}

/**
 * process_irq() - Process IRQ error
 * @status:     status word read by ISR
 * @reg_ind:    IRQ register index
 * @system:     Internal system device data
 *
 * Return: 0 if success, or an error code.
 */
static int process_irq(uint32_t status, unsigned int reg_ind)
{
    unsigned int i = 0;
    unsigned int irq_level;
    enum irq_type type;
    unsigned int arr_size;

    irq_level = reg_ind % NUM_IRQ_REGISTERS_PER_TYPE;
    type = reg_ind / NUM_IRQ_REGISTERS_PER_TYPE;

    if ( irq_level == LOW_IRQS )
        arr_size = ARRAY_SIZE(irq_name_low);
    else if ( irq_level == MID_IRQS )
        arr_size = ARRAY_SIZE(irq_name_mid);
    else
        arr_size = ARRAY_SIZE(irq_name_high);

    if ( type == UNCORRECTED )
        printk(XENLOG_ERR "PTM system: Detected UNCORRECTED_");
    else if ( type == DEFERRED )
        printk(XENLOG_ERR "PTM system: Detected DEFERRED_");
    else
    {
        printk(XENLOG_ERR "PTM system: IRQ type not known\n");
        return -EINVAL;
    }

    while ( status )
    {
        if ( i >= arr_size )
        {
            printk(XENLOG_ERR "PTM system: Max IRQ number exceeded\n");
            return -EINVAL;
        }
        if ( status & 0x1 )
        {
            if ( irq_level == LOW_IRQS )
                printk(XENLOG_ERR "%d %s\n", type, irq_name_low[i]);
            else if ( irq_level == MID_IRQS )
                printk(XENLOG_ERR "%d %s\n", type, irq_name_mid[i]);
            else
                printk(XENLOG_ERR "%d %s\n", type, irq_name_high[i]);
        }
        status >>= 1;
        i++;
    }

    return 0;
}

static void clear_system_irqs(struct mali_ptm_system *system,
                              uint32_t *status_bef_clear)
{
    uint32_t clear_irqs;
    bool set_irq_masks = false;
    unsigned int i;

    if ( WARN_ON(!system) )
        return;

    /* Clear all the IRQs */
    for ( i = 0; i < NUM_IRQ_REGISTERS_PER_TYPE; ++i )
    {
        unsigned int def_i = i + NUM_IRQ_REGISTERS_PER_TYPE;
        /*
         * Clear uncorrected or deferred errors as they are cleared from
         * the same register.
         */
        clear_irqs = status_bef_clear[i] | status_bef_clear[def_i];
        writel(clear_irqs, system->mem + PTM_IRQ_CLEAR + (OFFSET_4B * i));
        /* Mask any uncorrelated IRQs */
        if ( status_bef_clear[i] )
        {
            system->irq.mask[i] &= ~(status_bef_clear[i]);
            set_irq_masks = true;
        }
        /* Mask any deferred IRQs */
        if ( status_bef_clear[def_i] )
        {
            system->irq.mask[def_i] &= ~(status_bef_clear[def_i]);
            set_irq_masks = true;
        }
    }

    /* Set again the IRQ masks if needed to mask persistent IRQs */
    if ( set_irq_masks )
        set_system_irq_masks(system);

    return;
}

static void ptm_system_isr(int irq, void *data)
{
    uint32_t error_finger_print = 0;
    struct mali_ptm_system *system = data;
    uint32_t irq_status[NUM_IRQ_REGISTERS];
    bool irq_found = false;
    unsigned int i;
    int err;

    if ( WARN_ON(!system) )
        return;

    if ( irq != system->irq.line )
    {
        printk(XENLOG_ERR "PTM: Unknown system irq %d\n", irq);
        return;
    }

    for ( i = 0; i < NUM_IRQ_REGISTERS; ++i )
    {
        irq_status[i] = readl(system->mem + PTM_UNCORRECTED_ERROR_IRQ_STATUS +
                        (OFFSET_4B * i));

        if ( !irq_status[i] )
            continue;

        irq_found = true;
        err = process_irq(irq_status[i], i);
        if ( err )
        {
            printk(XENLOG_ERR "Error processing IRQ %d\n", i);
            return;
        }
    }

    if ( !irq_found )
        return;

    /* In case we have any errors the fingerprint is very important to data
     * to be printed as well.
     */
    error_finger_print = readl(system->mem + PTM_ERROR_FINGER_PRINT);
    printk(XENLOG_DEBUG "PTM: Finger print of the error: 0x%x\n",
                        error_finger_print);

    clear_system_irqs(system, irq_status);
    return;
}

/**
 * set_aw_stream_id() - Set stream ID for access windows
 * @base_addr: Base address of system partition register page.
 *
 * Set StreamIDs to hard coded sequential values.
 * The LSB of the protected stream is always '1', and is used
 * to set the PROTMODE signal in hardware.
 */
static void __init set_aw_stream_id(void __iomem *base_addr)
{
    unsigned int aw;

    for ( aw = 0; aw < MALI_PTM_ACCESS_WINDOW_COUNT; ++aw ) {
        const uint32_t offset = aw * PTM_AW_STREAM_ID_STRIDE;

        writel(2 * aw, base_addr + PTM_AW0_STREAM_ID + offset);
        writel((2 * aw) + 1, base_addr + PTM_AW0_PROTECTED_STREAM_ID + offset);
    }
}

static void __init initialize_irq_masks(struct mali_ptm_system *system)
{
    unsigned int i;
    unsigned int reg_level;

    if ( WARN_ON(!system) )
        return;

    for ( i = 0; i < NUM_IRQ_REGISTERS; ++i )
    {
        reg_level = i % NUM_IRQ_REGISTERS_PER_TYPE;
        if ( reg_level == 0 )
            system->irq.mask[i] = IRQ_ENABLE_MASK_LOW;
        else if ( reg_level == 1 )
            system->irq.mask[i] = IRQ_ENABLE_MASK_MID;
        else
            system->irq.mask[i] = IRQ_ENABLE_MASK_HIGH;
    }
    return;
}


int __init mali_ptm_system_init(struct mali_ptm_system *system)
{
    uint32_t value, reset_status;
    unsigned int group;
    int ret = 0;
    struct dt_device_node *node;

    node = dt_find_compatible_node(NULL, NULL, MALI_GPU_SYSTEM_PTM_DT_NAME);
    if ( !node )
    {
        printk(XENLOG_ERR "PTM: No system node found in device tree\n");
        return -ENOENT;
    }
    ret = dt_device_get_paddr(node, 0, &system->base, &system->size);
    if ( ret )
    {
        printk(XENLOG_ERR "PTM: Failed to get system base address\n");
        return -ENXIO;
    }

    system->mem = ioremap_nocache(system->base, system->size);
    if ( !system->mem )
    {
        printk(XENLOG_ERR "PTM: Failed to map system memory\n");
        return -ENOMEM;
    }

    value = readl(system->mem + PTM_DEVICE_ID);
    if ( check_ptm_version(value) )
    {
        printk(XENLOG_ERR "PTM: Unsupported PTM version 0x%x\n", value);
        ret = -ENODEV;
        goto out_err;
    }

    system->irq.line = platform_get_irq(node, 0);
    if ( system->irq.line < 0 )
    {
        printk(XENLOG_ERR "gpu: Failed to get system IRQ\n");
        ret = -ENXIO;
        goto out_err;
    }

    printk(XENLOG_DEBUG "PTM: Found system base address: 0x%"PRIpaddr"(0x%p)\n",
           system->base, system->mem);
    printk(XENLOG_DEBUG "PTM: Found system size: 0x%"PRIpaddr"\n", system->size);

    value = 0;
    reset_status = 0;
    /* Reset the GPU before performing any operation */
    for ( group = 0; group < PTM_SYSTEM_NUM_GROUPS; group++ )
    {
        /* perform a soft reset */
        value |= PTM_GROUP_SOFT_RESET << (group * PTM_GROUP_RESET_SET_BITS);
        reset_status |=
            PTM_GROUP_SOFT_RESET << (group * PTM_GROUP_RESET_STATE_BITS);
    }

    writel(value, system->mem + PTM_GROUP_RESET_SET);
    ret = readl_relaxed_poll_timeout(system->mem + PTM_GROUP_RESET_STATE,
                value, (value == reset_status),
                REG_POLL_SLEEP_US, REG_POLL_RESET_TIMEOUT_US);
    if ( ret )
    {
        printk(XENLOG_ERR "PTM: Failed GPU soft reset\n");
        ret = -EIO;
        goto out_err;
    }

    /* release reset */
    writel(0, system->mem + PTM_GROUP_RESET_SET);
    ret = readl_relaxed_poll_timeout(system->mem + PTM_GROUP_RESET_STATE,
                value, (value == 0),
                REG_POLL_SLEEP_US, REG_POLL_RESET_TIMEOUT_US);
    if ( ret )
    {
        printk(XENLOG_ERR "PTM: Failed to release GPU reset\n");
        ret = -EIO;
        goto out_err;
    }

    set_aw_stream_id(system->mem);
    initialize_irq_masks(system);

    ret = request_irq(system->irq.line,
                IRQF_SHARED, ptm_system_isr, "mali_system", system);
    if ( ret )
    {
        printk(XENLOG_ERR "PTM: Can't request system interrupt.\n");
        goto out_err;
    }

    /* Enable all uncorrected and deferred IRQs */
    set_system_irq_masks(system);

    return 0;

out_err:
    iounmap(system->mem);
    return ret;
}

/*
 * Local variables:
 * mode: C
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * End:
 */