/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * xen/arch/arm/platforms/xilinx-versal-net-eemi.c
 *
 * Xilinx Versal-net EEMI API mediator.
 *
 * Copyright (C) 2023, Advanced Micro Devices, Inc. All Rights Reserved.
 *
 */

#include <asm/regs.h>
#include <xen/iocap.h>
#include <xen/sched.h>
#include <asm/smccc.h>
#include <asm/platforms/xilinx-eemi.h>
#include <asm/platforms/xilinx-versal-net-mm.h>

/*
 * This table maps a node into a memory address.
 * If a guest has access to the address, it has enough control
 * over the node to grant it access to EEMI calls for that node.
 */
#define PM_NODE_IDX(Id) ((Id) & 0x3fff)

static const struct pm_access pm_node_access[] = {
    [PM_NODE_IDX(VERSAL_PM_DEV_PLD_0)] = { .hwdom_access = true },

    [PM_NODE_IDX(VERSAL_NET_PM_DEV_ACPU_0_0)] = { 0 },
    [PM_NODE_IDX(VERSAL_NET_PM_DEV_ACPU_0_1)] = { 0 },
    [PM_NODE_IDX(VERSAL_NET_PM_DEV_ACPU_0_2)] = { 0 },
    [PM_NODE_IDX(VERSAL_NET_PM_DEV_ACPU_0_3)] = { 0 },
    [PM_NODE_IDX(VERSAL_NET_PM_DEV_ACPU_1_0)] = { 0 },
    [PM_NODE_IDX(VERSAL_NET_PM_DEV_ACPU_1_1)] = { 0 },
    [PM_NODE_IDX(VERSAL_NET_PM_DEV_ACPU_1_2)] = { 0 },
    [PM_NODE_IDX(VERSAL_NET_PM_DEV_ACPU_1_3)] = { 0 },
    [PM_NODE_IDX(VERSAL_NET_PM_DEV_ACPU_2_0)] = { 0 },
    [PM_NODE_IDX(VERSAL_NET_PM_DEV_ACPU_2_1)] = { 0 },
    [PM_NODE_IDX(VERSAL_NET_PM_DEV_ACPU_2_2)] = { 0 },
    [PM_NODE_IDX(VERSAL_NET_PM_DEV_ACPU_2_3)] = { 0 },
    [PM_NODE_IDX(VERSAL_NET_PM_DEV_ACPU_3_0)] = { 0 },
    [PM_NODE_IDX(VERSAL_NET_PM_DEV_ACPU_3_1)] = { 0 },
    [PM_NODE_IDX(VERSAL_NET_PM_DEV_ACPU_3_2)] = { 0 },
    [PM_NODE_IDX(VERSAL_NET_PM_DEV_ACPU_3_3)] = { 0 },

    [PM_NODE_IDX(VERSAL_PM_DEV_L2_BANK_0)] = { 0 },

    /* Should Dom0 have access to this? */
    [PM_NODE_IDX(VERSAL_PM_DEV_DDR_0)] = { .hwdom_access = true },

    [PM_NODE_IDX(VERSAL_PM_DEV_USB_0)] = { MM_DEV_USB_0 },
    [PM_NODE_IDX(VERSAL_NET_PM_DEV_USB_1)] = { MM_DEV_USB_1 },

    [PM_NODE_IDX(VERSAL_PM_DEV_GEM_0)] = { MM_DEV_GEM_0 },
    [PM_NODE_IDX(VERSAL_PM_DEV_GEM_1)] = { MM_DEV_GEM_1 },

    [PM_NODE_IDX(VERSAL_PM_DEV_SPI_0)] = { MM_DEV_SPI_0 },
    [PM_NODE_IDX(VERSAL_PM_DEV_SPI_1)] = { MM_DEV_SPI_1 },

    [PM_NODE_IDX(VERSAL_PM_DEV_I2C_0)] = { MM_DEV_I2C_0 },
    [PM_NODE_IDX(VERSAL_PM_DEV_I2C_1)] = { MM_DEV_I2C_1 },

    [PM_NODE_IDX(VERSAL_PM_DEV_CAN_FD_0)] = { MM_DEV_CAN_FD_0 },
    [PM_NODE_IDX(VERSAL_PM_DEV_CAN_FD_1)] = { MM_DEV_CAN_FD_1 },

    [PM_NODE_IDX(VERSAL_PM_DEV_UART_0)] = { MM_DEV_UART_0 },
    [PM_NODE_IDX(VERSAL_PM_DEV_UART_1)] = { MM_DEV_UART_1 },

    [PM_NODE_IDX(VERSAL_PM_DEV_GPIO)] = { MM_DEV_GPIO },

    [PM_NODE_IDX(VERSAL_PM_DEV_TTC_0)] = { MM_DEV_TTC_0 },
    [PM_NODE_IDX(VERSAL_PM_DEV_TTC_1)] = { MM_DEV_TTC_1 },
    [PM_NODE_IDX(VERSAL_PM_DEV_TTC_2)] = { MM_DEV_TTC_2 },
    [PM_NODE_IDX(VERSAL_PM_DEV_TTC_3)] = { MM_DEV_TTC_3 },

    /* Versal-net WDT nodes. */
    [PM_NODE_IDX(VERSAL_NET_PM_DEV_LPD_SWDT_0)] = { MM_DEV_SWDT_LPD_0 },
    [PM_NODE_IDX(VERSAL_NET_PM_DEV_LPD_SWDT_1)] = { MM_DEV_SWDT_LPD_1 },
    [PM_NODE_IDX(VERSAL_NET_PM_DEV_FPD_SWDT_0)] = { MM_DEV_SWDT_FPD_0 },
    [PM_NODE_IDX(VERSAL_NET_PM_DEV_FPD_SWDT_1)] = { MM_DEV_SWDT_FPD_1 },
    [PM_NODE_IDX(VERSAL_NET_PM_DEV_FPD_SWDT_2)] = { MM_DEV_SWDT_FPD_2 },
    [PM_NODE_IDX(VERSAL_NET_PM_DEV_FPD_SWDT_3)] = { MM_DEV_SWDT_FPD_3 },

    [PM_NODE_IDX(VERSAL_PM_DEV_OSPI)] = { MM_DEV_OSPI },
    [PM_NODE_IDX(VERSAL_PM_DEV_QSPI)] = { MM_DEV_QSPI },
    [PM_NODE_IDX(VERSAL_PM_DEV_GPIO_PMC)] = { MM_DEV_GPIO_PMC },
    [PM_NODE_IDX(VERSAL_PM_DEV_I2C_PMC)] = { MM_DEV_I2C_PMC },

    [PM_NODE_IDX(VERSAL_PM_DEV_SDIO_0)] = { MM_DEV_SDIO_0 },
    [PM_NODE_IDX(VERSAL_PM_DEV_SDIO_1)] = { MM_DEV_SDIO_1 },

    [PM_NODE_IDX(VERSAL_PM_DEV_RTC)] = { MM_DEV_RTC },

    [PM_NODE_IDX(VERSAL_PM_DEV_ADMA_0)] = { MM_DEV_ADMA_0 },
    [PM_NODE_IDX(VERSAL_PM_DEV_ADMA_1)] = { MM_DEV_ADMA_1 },
    [PM_NODE_IDX(VERSAL_PM_DEV_ADMA_2)] = { MM_DEV_ADMA_2 },
    [PM_NODE_IDX(VERSAL_PM_DEV_ADMA_3)] = { MM_DEV_ADMA_3 },
    [PM_NODE_IDX(VERSAL_PM_DEV_ADMA_4)] = { MM_DEV_ADMA_4 },
    [PM_NODE_IDX(VERSAL_PM_DEV_ADMA_5)] = { MM_DEV_ADMA_5 },
    [PM_NODE_IDX(VERSAL_PM_DEV_ADMA_6)] = { MM_DEV_ADMA_6 },
    [PM_NODE_IDX(VERSAL_PM_DEV_ADMA_7)] = { MM_DEV_ADMA_7 },

    [PM_NODE_IDX(VERSAL_PM_DEV_IPI_0)] = { MM_DEV_IPI_0 },
    [PM_NODE_IDX(VERSAL_PM_DEV_IPI_1)] = { MM_DEV_IPI_1 },
    [PM_NODE_IDX(VERSAL_PM_DEV_IPI_2)] = { MM_DEV_IPI_2 },
    [PM_NODE_IDX(VERSAL_PM_DEV_IPI_3)] = { MM_DEV_IPI_3 },
    [PM_NODE_IDX(VERSAL_PM_DEV_IPI_4)] = { MM_DEV_IPI_4 },
    [PM_NODE_IDX(VERSAL_PM_DEV_IPI_5)] = { MM_DEV_IPI_5 },
    [PM_NODE_IDX(VERSAL_PM_DEV_IPI_6)] = { MM_DEV_IPI_6 },

    /* Should Dom0 have access to this? */
    [PM_NODE_IDX(VERSAL_PM_DEV_AMS_ROOT)] = { MM_DEV_AMS_ROOT },

    /* hwdom get access to remaining nodes by default. */
    [PM_NODE_IDX(VERSAL_NET_PM_DEV_GTM_5)] = { .hwdom_access = true },
    [PM_NODE_IDX(VERSAL_NET_PM_DEV_GTM_6)] = { .hwdom_access = true },
    [PM_NODE_IDX(VERSAL_NET_PM_DEV_GTM_7)] = { .hwdom_access = true },
    [PM_NODE_IDX(VERSAL_NET_PM_DEV_GTM_8)] = { .hwdom_access = true },
    [PM_NODE_IDX(VERSAL_NET_PM_DEV_GTM_9)] = { .hwdom_access = true },
    [PM_NODE_IDX(VERSAL_NET_PM_DEV_GTM_10)] = { .hwdom_access = true },
    [PM_NODE_IDX(VERSAL_NET_PM_DEV_GTM_11)] = { .hwdom_access = true },
    [PM_NODE_IDX(VERSAL_NET_PM_DEV_GTM_12)] = { .hwdom_access = true },
    [PM_NODE_IDX(VERSAL_NET_PM_DEV_GTM_13)] = { .hwdom_access = true },
    [PM_NODE_IDX(VERSAL_NET_PM_DEV_GTM_14)] = { .hwdom_access = true },
    [PM_NODE_IDX(VERSAL_NET_PM_DEV_GTM_15)] = { .hwdom_access = true },
    [PM_NODE_IDX(VERSAL_NET_PM_DEV_GTM_16)] = { .hwdom_access = true },
    [PM_NODE_IDX(VERSAL_NET_PM_DEV_GTM_17)] = { .hwdom_access = true },
    [PM_NODE_IDX(VERSAL_NET_PM_DEV_GTM_18)] = { .hwdom_access = true },
    [PM_NODE_IDX(VERSAL_NET_PM_DEV_GTM_19)] = { .hwdom_access = true },
    [PM_NODE_IDX(VERSAL_NET_PM_DEV_GTM_20)] = { .hwdom_access = true },
    [PM_NODE_IDX(VERSAL_NET_PM_DEV_GTM_21)] = { .hwdom_access = true },
    [PM_NODE_IDX(VERSAL_NET_PM_DEV_GTM_22)] = { .hwdom_access = true },
    [PM_NODE_IDX(VERSAL_NET_PM_DEV_GTM_23)] = { .hwdom_access = true },

    [PM_NODE_IDX(VERSAL_NET_PM_DEV_HBMMC_0)] = { .hwdom_access = true },
    [PM_NODE_IDX(VERSAL_NET_PM_DEV_HBMMC_1)] = { .hwdom_access = true },
    [PM_NODE_IDX(VERSAL_NET_PM_DEV_HBMMC_2)] = { .hwdom_access = true },
    [PM_NODE_IDX(VERSAL_NET_PM_DEV_HBMMC_3)] = { .hwdom_access = true },
    [PM_NODE_IDX(VERSAL_NET_PM_DEV_HBMMC_4)] = { .hwdom_access = true },
    [PM_NODE_IDX(VERSAL_NET_PM_DEV_HBMMC_5)] = { .hwdom_access = true },
    [PM_NODE_IDX(VERSAL_NET_PM_DEV_HBMMC_6)] = { .hwdom_access = true },
    [PM_NODE_IDX(VERSAL_NET_PM_DEV_HBMMC_7)] = { .hwdom_access = true },
    [PM_NODE_IDX(VERSAL_NET_PM_DEV_HBMMC_8)] = { .hwdom_access = true },
    [PM_NODE_IDX(VERSAL_NET_PM_DEV_HBMMC_9)] = { .hwdom_access = true },
    [PM_NODE_IDX(VERSAL_NET_PM_DEV_HBMMC_10)] = { .hwdom_access = true },
    [PM_NODE_IDX(VERSAL_NET_PM_DEV_HBMMC_11)] = { .hwdom_access = true },
    [PM_NODE_IDX(VERSAL_NET_PM_DEV_HBMMC_12)] = { .hwdom_access = true },
    [PM_NODE_IDX(VERSAL_NET_PM_DEV_HBMMC_13)] = { .hwdom_access = true },
    [PM_NODE_IDX(VERSAL_NET_PM_DEV_HBMMC_14)] = { .hwdom_access = true },
    [PM_NODE_IDX(VERSAL_NET_PM_DEV_HBMMC_15)] = { .hwdom_access = true },

    [PM_NODE_IDX(VERSAL_NET_PM_DEV_GTYP_3)] = { .hwdom_access = true },
    [PM_NODE_IDX(VERSAL_NET_PM_DEV_GTYP_4)] = { .hwdom_access = true },
    [PM_NODE_IDX(VERSAL_NET_PM_DEV_GTYP_5)] = { .hwdom_access = true },
    [PM_NODE_IDX(VERSAL_NET_PM_DEV_GTYP_6)] = { .hwdom_access = true },
    [PM_NODE_IDX(VERSAL_NET_PM_DEV_GTYP_7)] = { .hwdom_access = true },
    [PM_NODE_IDX(VERSAL_NET_PM_DEV_GTYP_8)] = { .hwdom_access = true },
    [PM_NODE_IDX(VERSAL_NET_PM_DEV_GTYP_9)] = { .hwdom_access = true },

    [PM_NODE_IDX(VERSAL_NET_PM_DEV_HBM_0)] = { .hwdom_access = true },

    [PM_NODE_IDX(VERSAL_NET_PM_DEV_VDU_0)] = { .hwdom_access = true },
    [PM_NODE_IDX(VERSAL_NET_PM_DEV_VDU_1)] = { .hwdom_access = true },
    [PM_NODE_IDX(VERSAL_NET_PM_DEV_VDU_2)] = { .hwdom_access = true },
    [PM_NODE_IDX(VERSAL_NET_PM_DEV_VDU_3)] = { .hwdom_access = true },

    [PM_NODE_IDX(VERSAL_NET_PM_DEV_BFRB_0)] = { .hwdom_access = true },
    [PM_NODE_IDX(VERSAL_NET_PM_DEV_BFRB_1)] = { .hwdom_access = true },
    [PM_NODE_IDX(VERSAL_NET_PM_DEV_BFRB_2)] = { .hwdom_access = true },
    [PM_NODE_IDX(VERSAL_NET_PM_DEV_BFRB_3)] = { .hwdom_access = true },
    [PM_NODE_IDX(VERSAL_NET_PM_DEV_BFRB_4)] = { .hwdom_access = true },
    [PM_NODE_IDX(VERSAL_NET_PM_DEV_BFRB_5)] = { .hwdom_access = true },
    [PM_NODE_IDX(VERSAL_NET_PM_DEV_BFRB_6)] = { .hwdom_access = true },
    [PM_NODE_IDX(VERSAL_NET_PM_DEV_BFRB_7)] = { .hwdom_access = true },
    [PM_NODE_IDX(VERSAL_NET_PM_DEV_BFRB_8)] = { .hwdom_access = true },
    [PM_NODE_IDX(VERSAL_NET_PM_DEV_BFRB_9)] = { .hwdom_access = true },
    [PM_NODE_IDX(VERSAL_NET_PM_DEV_BFRB_10)] = { .hwdom_access = true },
    [PM_NODE_IDX(VERSAL_NET_PM_DEV_BFRB_11)] = { .hwdom_access = true },

    [PM_NODE_IDX(VERSAL_NET_PM_DEV_RPU_A_0)] = { .hwdom_access = true },
    [PM_NODE_IDX(VERSAL_NET_PM_DEV_RPU_A_1)] = { .hwdom_access = true },
    [PM_NODE_IDX(VERSAL_NET_PM_DEV_RPU_B_0)] = { .hwdom_access = true },
    [PM_NODE_IDX(VERSAL_NET_PM_DEV_RPU_B_1)] = { .hwdom_access = true },

    [PM_NODE_IDX(VERSAL_NET_PM_DEV_OCM_0_0)] = { .hwdom_access = true },
    [PM_NODE_IDX(VERSAL_NET_PM_DEV_OCM_0_1)] = { .hwdom_access = true },
    [PM_NODE_IDX(VERSAL_NET_PM_DEV_OCM_0_2)] = { .hwdom_access = true },
    [PM_NODE_IDX(VERSAL_NET_PM_DEV_OCM_0_3)] = { .hwdom_access = true },
    [PM_NODE_IDX(VERSAL_NET_PM_DEV_OCM_1_0)] = { .hwdom_access = true },
    [PM_NODE_IDX(VERSAL_NET_PM_DEV_OCM_1_1)] = { .hwdom_access = true },
    [PM_NODE_IDX(VERSAL_NET_PM_DEV_OCM_1_2)] = { .hwdom_access = true },
    [PM_NODE_IDX(VERSAL_NET_PM_DEV_OCM_1_3)] = { .hwdom_access = true },

    [PM_NODE_IDX(VERSAL_NET_PM_DEV_TCM_A_0A)] = { .hwdom_access = true },
    [PM_NODE_IDX(VERSAL_NET_PM_DEV_TCM_A_0B)] = { .hwdom_access = true },
    [PM_NODE_IDX(VERSAL_NET_PM_DEV_TCM_A_0C)] = { .hwdom_access = true },
    [PM_NODE_IDX(VERSAL_NET_PM_DEV_TCM_A_1A)] = { .hwdom_access = true },
    [PM_NODE_IDX(VERSAL_NET_PM_DEV_TCM_A_1B)] = { .hwdom_access = true },
    [PM_NODE_IDX(VERSAL_NET_PM_DEV_TCM_A_1C)] = { .hwdom_access = true },
    [PM_NODE_IDX(VERSAL_NET_PM_DEV_TCM_B_0A)] = { .hwdom_access = true },
    [PM_NODE_IDX(VERSAL_NET_PM_DEV_TCM_B_0B)] = { .hwdom_access = true },
    [PM_NODE_IDX(VERSAL_NET_PM_DEV_TCM_B_0C)] = { .hwdom_access = true },
    [PM_NODE_IDX(VERSAL_NET_PM_DEV_TCM_B_1A)] = { .hwdom_access = true },
    [PM_NODE_IDX(VERSAL_NET_PM_DEV_TCM_B_1B)] = { .hwdom_access = true },
    [PM_NODE_IDX(VERSAL_NET_PM_DEV_TCM_B_1C)] = { .hwdom_access = true },

    [PM_NODE_IDX(VERSAL_NET_PM_DEV_PMC_WWDT)] = { .hwdom_access = true },

    [PM_NODE_IDX(VERSAL_PM_DEV_DDRMC_0)] = { .hwdom_access = true },
    [PM_NODE_IDX(VERSAL_PM_DEV_DDRMC_1)] = { .hwdom_access = true },
    [PM_NODE_IDX(VERSAL_PM_DEV_DDRMC_2)] = { .hwdom_access = true },
    [PM_NODE_IDX(VERSAL_PM_DEV_DDRMC_3)] = { .hwdom_access = true },
    [PM_NODE_IDX(VERSAL_NET_PM_DEV_DDRMC_4)] = { .hwdom_access = true },
    [PM_NODE_IDX(VERSAL_NET_PM_DEV_DDRMC_5)] = { .hwdom_access = true },
    [PM_NODE_IDX(VERSAL_NET_PM_DEV_DDRMC_6)] = { .hwdom_access = true },
    [PM_NODE_IDX(VERSAL_NET_PM_DEV_DDRMC_7)] = { .hwdom_access = true },
};

/*
 * This table maps a reset node into its corresponding memory address.
 *
 * Note: reset nodes must be in ascending order!
 */
static const struct pm_access pm_rst_access[] = {
    [PM_NODE_IDX(VERSAL_PM_RST_PMC_POR)] = { .hwdom_access = true },
    [PM_NODE_IDX(VERSAL_PM_RST_PMC)] = { .hwdom_access = true },
    [PM_NODE_IDX(VERSAL_PM_RST_PS_POR)] = { .hwdom_access = true },
    [PM_NODE_IDX(VERSAL_PM_RST_PL_POR)] = { .hwdom_access = true },
    [PM_NODE_IDX(VERSAL_PM_RST_NOC_POR)] = { .hwdom_access = true },
    [PM_NODE_IDX(VERSAL_PM_RST_FPD_POR)] = { .hwdom_access = true },

    /* We don't allow anyone to turn on/off the ACPUs.  */
    [PM_NODE_IDX(VERSAL_PM_RST_ACPU_0_POR)] = { 0 },
    [PM_NODE_IDX(VERSAL_PM_RST_ACPU_1_POR)] = { 0 },

    [PM_NODE_IDX(VERSAL_PM_RST_PS_SRST)] = { .hwdom_access = true },
    [PM_NODE_IDX(VERSAL_PM_RST_PL_SRST)] = { .hwdom_access = true },
    [PM_NODE_IDX(VERSAL_PM_RST_NOC)] = { .hwdom_access = true },
    [PM_NODE_IDX(VERSAL_PM_RST_NPI)] = { .hwdom_access = true },
    [PM_NODE_IDX(VERSAL_PM_RST_SYS_RST_1)] = { .hwdom_access = true },
    [PM_NODE_IDX(VERSAL_PM_RST_SYS_RST_2)] = { .hwdom_access = true },
    [PM_NODE_IDX(VERSAL_PM_RST_SYS_RST_3)] = { .hwdom_access = true },
    [PM_NODE_IDX(VERSAL_PM_RST_FPD)] = { .hwdom_access = true },

    [PM_NODE_IDX(VERSAL_PM_RST_PL0)] = { .hwdom_access = true },
    [PM_NODE_IDX(VERSAL_PM_RST_PL1)] = { .hwdom_access = true },
    [PM_NODE_IDX(VERSAL_PM_RST_PL2)] = { .hwdom_access = true },
    [PM_NODE_IDX(VERSAL_PM_RST_PL3)] = { .hwdom_access = true },

    [PM_NODE_IDX(VERSAL_PM_RST_APU)] = { 0 },
    [PM_NODE_IDX(VERSAL_PM_RST_ACPU_0)] = { 0 },
    [PM_NODE_IDX(VERSAL_PM_RST_ACPU_1)] = { 0 },
    [PM_NODE_IDX(VERSAL_PM_RST_ACPU_L2)] = { 0 },
    [PM_NODE_IDX(VERSAL_PM_RST_ACPU_GIC)] = { 0 },

    [PM_NODE_IDX(VERSAL_PM_RST_SYSMON_PMC_SEQ_RST)] = { .hwdom_access = true },
    [PM_NODE_IDX(VERSAL_PM_RST_SYSMON_PMC_CFG_RST)] = { .hwdom_access = true },
    [PM_NODE_IDX(VERSAL_PM_RST_SYSMON_FPD_CFG_RST)] = { .hwdom_access = true },
    [PM_NODE_IDX(VERSAL_PM_RST_SYSMON_FPD_SEQ_RST)] = { .hwdom_access = true },
    [PM_NODE_IDX(VERSAL_PM_RST_SYSMON_LPD)] = { .hwdom_access = true },
    [PM_NODE_IDX(VERSAL_PM_RST_PDMA_RST1)] = { .hwdom_access = true },
    [PM_NODE_IDX(VERSAL_PM_RST_PDMA_RST0)] = { .hwdom_access = true },

    /* ADMA Channel 0 grants access to pull the reset signal.  */
    [PM_NODE_IDX(VERSAL_PM_RST_ADMA)] = { MM_DEV_ADMA_0 },
    [PM_NODE_IDX(VERSAL_PM_RST_TIMESTAMP)] = { .hwdom_access = true },
    [PM_NODE_IDX(VERSAL_PM_RST_IPI)] = { .hwdom_access = true },
    [PM_NODE_IDX(VERSAL_PM_RST_SBI)] = { .hwdom_access = true },

    [PM_NODE_IDX(VERSAL_PM_RST_LPD)] = { .hwdom_access = true },

    [PM_NODE_IDX(VERSAL_PM_RST_QSPI)] = { MM_DEV_QSPI },
    [PM_NODE_IDX(VERSAL_PM_RST_OSPI)] = { MM_DEV_OSPI },
    [PM_NODE_IDX(VERSAL_PM_RST_SDIO_0)] = { MM_DEV_SDIO_0 },
    [PM_NODE_IDX(VERSAL_PM_RST_SDIO_1)] = { MM_DEV_SDIO_1 },
    [PM_NODE_IDX(VERSAL_PM_RST_I2C_PMC)] = { MM_DEV_I2C_PMC },
    [PM_NODE_IDX(VERSAL_NET_PM_RST_I2C)] = { MM_DEV_I2C_0 },

    [PM_NODE_IDX(VERSAL_PM_RST_GPIO_PMC)] = { MM_DEV_GPIO_PMC },
    [PM_NODE_IDX(VERSAL_PM_RST_GEM_0)] = { MM_DEV_GEM_0 },
    [PM_NODE_IDX(VERSAL_PM_RST_GEM_1)] = { MM_DEV_GEM_1 },

    [PM_NODE_IDX(VERSAL_PM_RST_SPARE)] = { .hwdom_access = true },
    [PM_NODE_IDX(VERSAL_PM_RST_USB_0)] = { MM_DEV_USB_0 },
    [PM_NODE_IDX(VERSAL_NET_PM_RST_USB_1)] = { MM_DEV_USB_1 },
    [PM_NODE_IDX(VERSAL_NET_PM_RST_USB0_PHY)] = { MM_DEV_USB_0 },
    [PM_NODE_IDX(VERSAL_NET_PM_RST_USB1_PHY)] = { MM_DEV_USB_1 },

    [PM_NODE_IDX(VERSAL_PM_RST_UART_0)] = { MM_DEV_UART_0 },
    [PM_NODE_IDX(VERSAL_PM_RST_UART_1)] = { MM_DEV_UART_1 },
    [PM_NODE_IDX(VERSAL_PM_RST_SPI_0)] = { MM_DEV_SPI_0 },
    [PM_NODE_IDX(VERSAL_PM_RST_SPI_1)] = { MM_DEV_SPI_1 },
    [PM_NODE_IDX(VERSAL_PM_RST_CAN_FD_0)] = { MM_DEV_CAN_FD_0 },
    [PM_NODE_IDX(VERSAL_PM_RST_CAN_FD_1)] = { MM_DEV_CAN_FD_1 },

    [PM_NODE_IDX(VERSAL_PM_RST_I2C_0)] = { MM_DEV_I2C_0 },
    [PM_NODE_IDX(VERSAL_PM_RST_I2C_1)] = { MM_DEV_I2C_1 },
    [PM_NODE_IDX(VERSAL_NET_PM_RST_I3C_0)] = { MM_DEV_I3C_0 },
    [PM_NODE_IDX(VERSAL_NET_PM_RST_I3C_1)] = { MM_DEV_I3C_1 },

    [PM_NODE_IDX(VERSAL_PM_RST_GPIO_LPD)] = { MM_DEV_GPIO },

    [PM_NODE_IDX(VERSAL_PM_RST_TTC_0)] = { MM_DEV_TTC_0 },
    [PM_NODE_IDX(VERSAL_PM_RST_TTC_1)] = { MM_DEV_TTC_1 },
    [PM_NODE_IDX(VERSAL_PM_RST_TTC_2)] = { MM_DEV_TTC_2 },
    [PM_NODE_IDX(VERSAL_PM_RST_TTC_3)] = { MM_DEV_TTC_3 },

    [PM_NODE_IDX(VERSAL_NET_PM_RST_SWDT_0)] = { MM_DEV_SWDT_LPD_0 },
    [PM_NODE_IDX(VERSAL_NET_PM_RST_SWDT_1)] = { MM_DEV_SWDT_LPD_1 },

    [PM_NODE_IDX(VERSAL_PM_RST_DPC)] = { .hwdom_access = true },
    [PM_NODE_IDX(VERSAL_PM_RST_PMCDBG)] = { .hwdom_access = true },
    [PM_NODE_IDX(VERSAL_PM_RST_DBG_TRACE)] = { .hwdom_access = true },
    [PM_NODE_IDX(VERSAL_PM_RST_DBG_FPD)] = { .hwdom_access = true },
    [PM_NODE_IDX(VERSAL_PM_RST_DBG_TSTMP)] = { .hwdom_access = true },
    [PM_NODE_IDX(VERSAL_PM_RST_HSDP)] = { .hwdom_access = true },
    [PM_NODE_IDX(VERSAL_PM_RST_DBG_LPD)] = { .hwdom_access = true },
    [PM_NODE_IDX(VERSAL_PM_RST_CPM_POR)] = { .hwdom_access = true },
    [PM_NODE_IDX(VERSAL_PM_RST_CPM)] = { .hwdom_access = true },
    [PM_NODE_IDX(VERSAL_PM_RST_CPMDBG)] = { .hwdom_access = true },
    [PM_NODE_IDX(VERSAL_PM_RST_PCIE_CFG)] = { .hwdom_access = true },
    [PM_NODE_IDX(VERSAL_PM_RST_PCIE_CORE0)] = { .hwdom_access = true },
    [PM_NODE_IDX(VERSAL_PM_RST_PCIE_CORE1)] = { .hwdom_access = true },
    [PM_NODE_IDX(VERSAL_PM_RST_PCIE_DMA)] = { .hwdom_access = true },
    [PM_NODE_IDX(VERSAL_PM_RST_CMN)] = { .hwdom_access = true },
    [PM_NODE_IDX(VERSAL_PM_RST_ADDR_REMAP)] = { .hwdom_access = true },
    [PM_NODE_IDX(VERSAL_PM_RST_CPI0)] = { .hwdom_access = true },
    [PM_NODE_IDX(VERSAL_PM_RST_CPI1)] = { .hwdom_access = true },

    [PM_NODE_IDX(VERSAL_NET_PM_RST_FPD_SWDT_0)] = { MM_DEV_SWDT_FPD_0 },
    [PM_NODE_IDX(VERSAL_NET_PM_RST_FPD_SWDT_1)] = { MM_DEV_SWDT_FPD_1 },
    [PM_NODE_IDX(VERSAL_NET_PM_RST_FPD_SWDT_2)] = { MM_DEV_SWDT_FPD_2 },
    [PM_NODE_IDX(VERSAL_NET_PM_RST_FPD_SWDT_3)] = { MM_DEV_SWDT_FPD_3 },

    [PM_NODE_IDX(VERSAL_PM_RST_L2_0)] = { 0 },
    [PM_NODE_IDX(VERSAL_PM_RST_L2_1)] = { 0 },

    [PM_NODE_IDX(VERSAL_NET_PM_RST_RAM_0)] = { 0 },
    [PM_NODE_IDX(VERSAL_NET_PM_RST_RAM_1)] = { 0 },
    [PM_NODE_IDX(VERSAL_NET_PM_RST_MMU_TBU_3)] = { 0 },
    [PM_NODE_IDX(VERSAL_NET_PM_RST_MMU_GLOBAL)] = { 0 },
    [PM_NODE_IDX(VERSAL_NET_PM_RST_MMU_TBU_4)] = { 0 },
    [PM_NODE_IDX(VERSAL_NET_PM_RST_MMU_TBU_9)] = { 0 },
    [PM_NODE_IDX(VERSAL_NET_PM_RST_MMU_TBU_5)] = { 0 },
    [PM_NODE_IDX(VERSAL_NET_PM_RST_MMU_TBU_0)] = { 0 },
    [PM_NODE_IDX(VERSAL_NET_PM_RST_MMU_TBU_7)] = { 0 },
    [PM_NODE_IDX(VERSAL_NET_PM_RST_MMU_TBU_1)] = { 0 },
    [PM_NODE_IDX(VERSAL_NET_PM_RST_MMU_TBU_8)] = { 0 },
    [PM_NODE_IDX(VERSAL_NET_PM_RST_MMU_TBU_6)] = { 0 },
    [PM_NODE_IDX(VERSAL_NET_PM_RST_MMU_TBU_10)] = { 0 },
    [PM_NODE_IDX(VERSAL_NET_PM_RST_MMU_TBU_2)] = { 0 },

    [PM_NODE_IDX(VERSAL_NET_PM_RST_APU3_CORE1_WARM)] = { 0 },
    [PM_NODE_IDX(VERSAL_NET_PM_RST_APU3_CORE3_COLD)] = { 0 },
    [PM_NODE_IDX(VERSAL_NET_PM_RST_APU3_CORE0_COLD)] = { 0 },
    [PM_NODE_IDX(VERSAL_NET_PM_RST_APU3_CORE1_COLD)] = { 0 },
    [PM_NODE_IDX(VERSAL_NET_PM_RST_APU3_CLUSTER_COLD)] = { 0 },
    [PM_NODE_IDX(VERSAL_NET_PM_RST_APU3_CORE0_WARM)] = { 0 },
    [PM_NODE_IDX(VERSAL_NET_PM_RST_APU3_CORE2_COLD)] = { 0 },
    [PM_NODE_IDX(VERSAL_NET_PM_RST_APU3_CORE2_WARM)] = { 0 },
    [PM_NODE_IDX(VERSAL_NET_PM_RST_APU3_CORE3_WARM)] = { 0 },
    [PM_NODE_IDX(VERSAL_NET_PM_RST_APU3_CLUSTER_WARM)] = { 0 },
    [PM_NODE_IDX(VERSAL_NET_PM_RST_APU1_CORE1_WARM)] = { 0 },
    [PM_NODE_IDX(VERSAL_NET_PM_RST_APU1_CORE3_COLD)] = { 0 },
    [PM_NODE_IDX(VERSAL_NET_PM_RST_APU1_CORE0_COLD)] = { 0 },
    [PM_NODE_IDX(VERSAL_NET_PM_RST_APU1_CORE1_COLD)] = { 0 },
    [PM_NODE_IDX(VERSAL_NET_PM_RST_APU1_CLUSTER_COLD)] = { 0 },
    [PM_NODE_IDX(VERSAL_NET_PM_RST_APU1_CORE0_WARM)] = { 0 },
    [PM_NODE_IDX(VERSAL_NET_PM_RST_APU1_CORE2_COLD)] = { 0 },
    [PM_NODE_IDX(VERSAL_NET_PM_RST_APU1_CORE2_WARM)] = { 0 },
    [PM_NODE_IDX(VERSAL_NET_PM_RST_APU1_CORE3_WARM)] = { 0 },
    [PM_NODE_IDX(VERSAL_NET_PM_RST_APU1_CLUSTER_WARM)] = { 0 },
    [PM_NODE_IDX(VERSAL_NET_PM_RST_APU0_CORE1_WARM)] = { 0 },
    [PM_NODE_IDX(VERSAL_NET_PM_RST_APU0_CORE3_COLD)] = { 0 },
    [PM_NODE_IDX(VERSAL_NET_PM_RST_APU0_CORE0_COLD)] = { 0 },
    [PM_NODE_IDX(VERSAL_NET_PM_RST_APU0_CORE1_COLD)] = { 0 },
    [PM_NODE_IDX(VERSAL_NET_PM_RST_APU0_CLUSTER_COLD)] = { 0 },
    [PM_NODE_IDX(VERSAL_NET_PM_RST_APU0_CORE0_WARM)] = { 0 },
    [PM_NODE_IDX(VERSAL_NET_PM_RST_APU0_CORE2_COLD)] = { 0 },
    [PM_NODE_IDX(VERSAL_NET_PM_RST_APU0_CORE2_WARM)] = { 0 },
    [PM_NODE_IDX(VERSAL_NET_PM_RST_APU0_CORE3_WARM)] = { 0 },
    [PM_NODE_IDX(VERSAL_NET_PM_RST_APU0_CLUSTER_WARM)] = { 0 },
    [PM_NODE_IDX(VERSAL_NET_PM_RST_APU2_CORE1_WARM)] = { 0 },
    [PM_NODE_IDX(VERSAL_NET_PM_RST_APU2_CORE3_COLD)] = { 0 },
    [PM_NODE_IDX(VERSAL_NET_PM_RST_APU2_CORE0_COLD)] = { 0 },
    [PM_NODE_IDX(VERSAL_NET_PM_RST_APU2_CORE1_COLD)] = { 0 },
    [PM_NODE_IDX(VERSAL_NET_PM_RST_APU2_CLUSTER_COLD)] = { 0 },
    [PM_NODE_IDX(VERSAL_NET_PM_RST_APU2_CORE0_WARM)] = { 0 },
    [PM_NODE_IDX(VERSAL_NET_PM_RST_APU2_CORE2_COLD)] = { 0 },
    [PM_NODE_IDX(VERSAL_NET_PM_RST_APU2_CORE2_WARM)] = { 0 },
    [PM_NODE_IDX(VERSAL_NET_PM_RST_APU2_CORE3_WARM)] = { 0 },
    [PM_NODE_IDX(VERSAL_NET_PM_RST_APU2_CLUSTER_WARM)] = { 0 },

    /* hwdom get access to remaining nodes by default. */
    [PM_NODE_IDX(VERSAL_NET_PM_RST_PCIE_DMA1)] = { .hwdom_access = true },
    [PM_NODE_IDX(VERSAL_NET_PM_RST_GTY_1)] = { .hwdom_access = true },
    [PM_NODE_IDX(VERSAL_NET_PM_RST_GTY_3)] = { .hwdom_access = true },
    [PM_NODE_IDX(VERSAL_NET_PM_RST_GTY_2)] = { .hwdom_access = true },

    [PM_NODE_IDX(VERSAL_NET_PM_RST_CDX)] = { .hwdom_access = true },

    [PM_NODE_IDX(VERSAL_NET_PM_RST_DPU)] = { .hwdom_access = true },
    [PM_NODE_IDX(VERSAL_NET_PM_RST_DPU_CONFIG)] = { .hwdom_access = true },

    [PM_NODE_IDX(VERSAL_NET_PM_RST_CDX_CONFIG)] = { .hwdom_access = true },

    [PM_NODE_IDX(VERSAL_NET_PM_RST_PCIE0_CONFIG)] = { .hwdom_access = true },
    [PM_NODE_IDX(VERSAL_NET_PM_RST_PCIE_CONFIG_POR)] = { .hwdom_access = true },
    [PM_NODE_IDX(VERSAL_NET_PM_RST_PCIE_1)] = { .hwdom_access = true },
    [PM_NODE_IDX(VERSAL_NET_PM_RST_PCIE_0)] = { .hwdom_access = true },
    [PM_NODE_IDX(VERSAL_NET_PM_RST_PCIE_3)] = { .hwdom_access = true },
    [PM_NODE_IDX(VERSAL_NET_PM_RST_PCIE_2)] = { .hwdom_access = true },
    [PM_NODE_IDX(VERSAL_NET_PM_RST_PCIE1_CONFIG)] = { .hwdom_access = true },
    [PM_NODE_IDX(VERSAL_NET_PM_RST_PCIE3_CONFIG)] = { .hwdom_access = true },
    [PM_NODE_IDX(VERSAL_NET_PM_RST_PCIE2_CONFIG)] = { .hwdom_access = true },

    [PM_NODE_IDX(VERSAL_NET_PM_RST_WWDT)] = { .hwdom_access = true },

    [PM_NODE_IDX(VERSAL_NET_PM_RST_SYS_1)] = { .hwdom_access = true },
    [PM_NODE_IDX(VERSAL_NET_PM_RST_SYS_3)] = { .hwdom_access = true },
    [PM_NODE_IDX(VERSAL_NET_PM_RST_SYS_2)] = { .hwdom_access = true },

    [PM_NODE_IDX(VERSAL_NET_PM_RST_SYSMON_CFG_CPM5N)] = { .hwdom_access = true },
    [PM_NODE_IDX(VERSAL_NET_PM_RST_SYSMON_CFG_PMC)] = { .hwdom_access = true },
    [PM_NODE_IDX(VERSAL_NET_PM_RST_SYSMON_SEQ_CPM5N)] = { .hwdom_access = true },
    [PM_NODE_IDX(VERSAL_NET_PM_RST_SYSMON_SEQ_PMC)] = { .hwdom_access = true },

    [PM_NODE_IDX(VERSAL_NET_PM_RST_DMA_CONFIG_POR)] = { .hwdom_access = true },
    [PM_NODE_IDX(VERSAL_NET_PM_RST_PDMA_1)] = { .hwdom_access = true },
    [PM_NODE_IDX(VERSAL_NET_PM_RST_PDMA_0)] = { .hwdom_access = true },

    [PM_NODE_IDX(VERSAL_NET_PM_RST_DBG_CPM5N)] = { .hwdom_access = true },
    [PM_NODE_IDX(VERSAL_NET_PM_RST_DBG_PMC)] = { .hwdom_access = true },
    [PM_NODE_IDX(VERSAL_NET_PM_RST_DBG_DPC)] = { .hwdom_access = true },

    [PM_NODE_IDX(VERSAL_NET_PM_RST_PKI)] = { .hwdom_access = true },
    [PM_NODE_IDX(VERSAL_NET_PM_RST_FMU)] = { .hwdom_access = true },

    [PM_NODE_IDX(VERSAL_NET_PM_RST_CMN_CXS)] = { .hwdom_access = true },
    [PM_NODE_IDX(VERSAL_NET_PM_RST_CMN_CGL)] = { .hwdom_access = true },

    [PM_NODE_IDX(VERSAL_NET_PM_RST_RPU_A_GD)] = { .hwdom_access = true },
    [PM_NODE_IDX(VERSAL_NET_PM_RST_RPU_B_GD)] = { .hwdom_access = true },
    [PM_NODE_IDX(VERSAL_NET_PM_RST_RPU_CORE0A)] = { .hwdom_access = true },
    [PM_NODE_IDX(VERSAL_NET_PM_RST_RPU_CORE0A_POR)] = { .hwdom_access = true },
    [PM_NODE_IDX(VERSAL_NET_PM_RST_RPU_CORE0B_POR)] = { .hwdom_access = true },
    [PM_NODE_IDX(VERSAL_NET_PM_RST_RPU_A_GD_TOP)] = { .hwdom_access = true },
    [PM_NODE_IDX(VERSAL_NET_PM_RST_RPU_CORE1B)] = { .hwdom_access = true },
    [PM_NODE_IDX(VERSAL_NET_PM_RST_RPU_B_TOPRESET)] = { .hwdom_access = true },
    [PM_NODE_IDX(VERSAL_NET_PM_RST_RPU_CORE1B_POR)] = { .hwdom_access = true },
    [PM_NODE_IDX(VERSAL_NET_PM_RST_RPU_CORE1A)] = { .hwdom_access = true },
    [PM_NODE_IDX(VERSAL_NET_PM_RST_RPU_B_GD_TOP)] = { .hwdom_access = true },
    [PM_NODE_IDX(VERSAL_NET_PM_RST_RPU_A_TOPRESET)] = { .hwdom_access = true },
    [PM_NODE_IDX(VERSAL_NET_PM_RST_RPU_B_DBGRST)] = { .hwdom_access = true },
    [PM_NODE_IDX(VERSAL_NET_PM_RST_RPU_A_DCLS_TOPRESET)] = { .hwdom_access = true },
    [PM_NODE_IDX(VERSAL_NET_PM_RST_RPU_CORE1A_POR)] = { .hwdom_access = true },
    [PM_NODE_IDX(VERSAL_NET_PM_RST_RPU_B_DCLS_TOPRESET)] = { .hwdom_access = true },
    [PM_NODE_IDX(VERSAL_NET_PM_RST_RPU_A_DBGRST)] = { .hwdom_access = true },
    [PM_NODE_IDX(VERSAL_NET_PM_RST_RPU_CORE0B)] = { .hwdom_access = true },

    [PM_NODE_IDX(VERSAL_NET_PM_RST_SYSMON_CFG_FPD)] = { .hwdom_access = true },
    [PM_NODE_IDX(VERSAL_NET_PM_RST_SYSMON_CFG_LPD)] = { .hwdom_access = true },
    [PM_NODE_IDX(VERSAL_NET_PM_RST_SYSMON_SEQ_FPD)] = { .hwdom_access = true },
    [PM_NODE_IDX(VERSAL_NET_PM_RST_SYSMON_SEQ_LPD)] = { .hwdom_access = true },

    [PM_NODE_IDX(VERSAL_NET_PM_RST_FPD_SRST)] = { .hwdom_access = true },

    [PM_NODE_IDX(VERSAL_NET_PM_RST_DBG_LPD_HSDP)] = { .hwdom_access = true },

    [PM_NODE_IDX(VERSAL_NET_PM_RST_PSM_MODE_WAKEUP)] = { .hwdom_access = true },
    [PM_NODE_IDX(VERSAL_NET_PM_RST_PSM_MODE_MODE)] = { .hwdom_access = true },

    [PM_NODE_IDX(VERSAL_NET_PM_RST_TIMESTAMP_FPD)] = { .hwdom_access = true },
    [PM_NODE_IDX(VERSAL_NET_PM_RST_TIMESTAMP_LPD)] = { .hwdom_access = true },
    [PM_NODE_IDX(VERSAL_NET_PM_RST_CPI)] = { .hwdom_access = true },
};

/*
 * This table maps a clk node into a device node.
 */
static const struct pm_clk2node pm_clk_node_map[] = {
    PM_CLK2NODE(PM_NODE_IDX(VERSAL_PM_CLK_SYSMON_REF), PM_NODE_IDX(VERSAL_PM_DEV_AMS_ROOT)),

    PM_CLK2NODE(PM_NODE_IDX(VERSAL_PM_CLK_TTC0), PM_NODE_IDX(VERSAL_PM_DEV_TTC_0)),
    PM_CLK2NODE(PM_NODE_IDX(VERSAL_PM_CLK_TTC1), PM_NODE_IDX(VERSAL_PM_DEV_TTC_1)),
    PM_CLK2NODE(PM_NODE_IDX(VERSAL_PM_CLK_TTC2), PM_NODE_IDX(VERSAL_PM_DEV_TTC_2)),
    PM_CLK2NODE(PM_NODE_IDX(VERSAL_PM_CLK_TTC3), PM_NODE_IDX(VERSAL_PM_DEV_TTC_3)),

    PM_CLK2NODE(PM_NODE_IDX(VERSAL_PM_CLK_GEM_TSU), PM_NODE_IDX(VERSAL_PM_DEV_GEM_0)),
    PM_CLK2NODE(PM_NODE_IDX(VERSAL_PM_CLK_GEM_TSU), PM_NODE_IDX(VERSAL_PM_DEV_GEM_1)),
    PM_CLK2NODE(PM_NODE_IDX(VERSAL_PM_CLK_GEM0_RX), PM_NODE_IDX(VERSAL_PM_DEV_GEM_0)),
    PM_CLK2NODE(PM_NODE_IDX(VERSAL_PM_CLK_GEM0_TX), PM_NODE_IDX(VERSAL_PM_DEV_GEM_0)),
    PM_CLK2NODE(PM_NODE_IDX(VERSAL_PM_CLK_GEM1_RX), PM_NODE_IDX(VERSAL_PM_DEV_GEM_1)),
    PM_CLK2NODE(PM_NODE_IDX(VERSAL_PM_CLK_GEM1_TX), PM_NODE_IDX(VERSAL_PM_DEV_GEM_1)),

    PM_CLK2NODE(PM_NODE_IDX(VERSAL_PM_CLK_QSPI_REF), PM_NODE_IDX(VERSAL_PM_DEV_QSPI)),
    PM_CLK2NODE(PM_NODE_IDX(VERSAL_PM_CLK_OSPI_REF), PM_NODE_IDX(VERSAL_PM_DEV_OSPI)),

    PM_CLK2NODE(PM_NODE_IDX(VERSAL_PM_CLK_SDIO0_REF), PM_NODE_IDX(VERSAL_PM_DEV_SDIO_0)),
    PM_CLK2NODE(PM_NODE_IDX(VERSAL_PM_CLK_SDIO1_REF), PM_NODE_IDX(VERSAL_PM_DEV_SDIO_1)),

    PM_CLK2NODE(PM_NODE_IDX(VERSAL_PM_CLK_I2C_REF), PM_NODE_IDX(VERSAL_PM_DEV_I2C_PMC)),

    PM_CLK2NODE(PM_NODE_IDX(VERSAL_PM_CLK_TEST_PATTERN_REF), PM_NODE_IDX(VERSAL_PM_DEV_PLD_0)),

    PM_CLK2NODE(PM_NODE_IDX(VERSAL_PM_CLK_PMC_PL0_REF), PM_NODE_IDX(VERSAL_PM_DEV_PLD_0)),
    PM_CLK2NODE(PM_NODE_IDX(VERSAL_PM_CLK_PMC_PL1_REF), PM_NODE_IDX(VERSAL_PM_DEV_PLD_0)),
    PM_CLK2NODE(PM_NODE_IDX(VERSAL_PM_CLK_PMC_PL2_REF), PM_NODE_IDX(VERSAL_PM_DEV_PLD_0)),
    PM_CLK2NODE(PM_NODE_IDX(VERSAL_PM_CLK_PMC_PL3_REF), PM_NODE_IDX(VERSAL_PM_DEV_PLD_0)),

    PM_CLK2NODE(PM_NODE_IDX(VERSAL_NET_PM_CLK_ACPU0), PM_NODE_IDX(VERSAL_NET_PM_DEV_ACPU_0_0)),
    PM_CLK2NODE(PM_NODE_IDX(VERSAL_NET_PM_CLK_ACPU0), PM_NODE_IDX(VERSAL_NET_PM_DEV_ACPU_0_1)),
    PM_CLK2NODE(PM_NODE_IDX(VERSAL_NET_PM_CLK_ACPU0), PM_NODE_IDX(VERSAL_NET_PM_DEV_ACPU_0_2)),
    PM_CLK2NODE(PM_NODE_IDX(VERSAL_NET_PM_CLK_ACPU0), PM_NODE_IDX(VERSAL_NET_PM_DEV_ACPU_0_3)),
    PM_CLK2NODE(PM_NODE_IDX(VERSAL_NET_PM_CLK_ACPU1), PM_NODE_IDX(VERSAL_NET_PM_DEV_ACPU_1_0)),
    PM_CLK2NODE(PM_NODE_IDX(VERSAL_NET_PM_CLK_ACPU1), PM_NODE_IDX(VERSAL_NET_PM_DEV_ACPU_1_1)),
    PM_CLK2NODE(PM_NODE_IDX(VERSAL_NET_PM_CLK_ACPU1), PM_NODE_IDX(VERSAL_NET_PM_DEV_ACPU_1_2)),
    PM_CLK2NODE(PM_NODE_IDX(VERSAL_NET_PM_CLK_ACPU1), PM_NODE_IDX(VERSAL_NET_PM_DEV_ACPU_1_3)),
    PM_CLK2NODE(PM_NODE_IDX(VERSAL_NET_PM_CLK_ACPU2), PM_NODE_IDX(VERSAL_NET_PM_DEV_ACPU_2_0)),
    PM_CLK2NODE(PM_NODE_IDX(VERSAL_NET_PM_CLK_ACPU2), PM_NODE_IDX(VERSAL_NET_PM_DEV_ACPU_2_1)),
    PM_CLK2NODE(PM_NODE_IDX(VERSAL_NET_PM_CLK_ACPU2), PM_NODE_IDX(VERSAL_NET_PM_DEV_ACPU_2_2)),
    PM_CLK2NODE(PM_NODE_IDX(VERSAL_NET_PM_CLK_ACPU2), PM_NODE_IDX(VERSAL_NET_PM_DEV_ACPU_2_3)),
    PM_CLK2NODE(PM_NODE_IDX(VERSAL_NET_PM_CLK_ACPU3), PM_NODE_IDX(VERSAL_NET_PM_DEV_ACPU_3_0)),
    PM_CLK2NODE(PM_NODE_IDX(VERSAL_NET_PM_CLK_ACPU3), PM_NODE_IDX(VERSAL_NET_PM_DEV_ACPU_3_1)),
    PM_CLK2NODE(PM_NODE_IDX(VERSAL_NET_PM_CLK_ACPU3), PM_NODE_IDX(VERSAL_NET_PM_DEV_ACPU_3_2)),
    PM_CLK2NODE(PM_NODE_IDX(VERSAL_NET_PM_CLK_ACPU3), PM_NODE_IDX(VERSAL_NET_PM_DEV_ACPU_3_3)),

    PM_CLK2NODE(PM_NODE_IDX(VERSAL_PM_CLK_ADMA), PM_NODE_IDX(VERSAL_PM_DEV_ADMA_0)),
    PM_CLK2NODE(PM_NODE_IDX(VERSAL_PM_CLK_ADMA), PM_NODE_IDX(VERSAL_PM_DEV_ADMA_1)),
    PM_CLK2NODE(PM_NODE_IDX(VERSAL_PM_CLK_ADMA), PM_NODE_IDX(VERSAL_PM_DEV_ADMA_2)),
    PM_CLK2NODE(PM_NODE_IDX(VERSAL_PM_CLK_ADMA), PM_NODE_IDX(VERSAL_PM_DEV_ADMA_3)),
    PM_CLK2NODE(PM_NODE_IDX(VERSAL_PM_CLK_ADMA), PM_NODE_IDX(VERSAL_PM_DEV_ADMA_4)),
    PM_CLK2NODE(PM_NODE_IDX(VERSAL_PM_CLK_ADMA), PM_NODE_IDX(VERSAL_PM_DEV_ADMA_5)),
    PM_CLK2NODE(PM_NODE_IDX(VERSAL_PM_CLK_ADMA), PM_NODE_IDX(VERSAL_PM_DEV_ADMA_6)),
    PM_CLK2NODE(PM_NODE_IDX(VERSAL_PM_CLK_ADMA), PM_NODE_IDX(VERSAL_PM_DEV_ADMA_7)),

    PM_CLK2NODE(PM_NODE_IDX(VERSAL_PM_CLK_GEM0_REF), PM_NODE_IDX(VERSAL_PM_DEV_GEM_0)),
    PM_CLK2NODE(PM_NODE_IDX(VERSAL_PM_CLK_GEM1_REF), PM_NODE_IDX(VERSAL_PM_DEV_GEM_1)),
    PM_CLK2NODE(PM_NODE_IDX(VERSAL_PM_CLK_GEM_TSU_REF), PM_NODE_IDX(VERSAL_PM_DEV_GEM_0)),
    PM_CLK2NODE(PM_NODE_IDX(VERSAL_PM_CLK_GEM_TSU_REF), PM_NODE_IDX(VERSAL_PM_DEV_GEM_1)),

    PM_CLK2NODE(PM_NODE_IDX(VERSAL_PM_CLK_USB0_BUS_REF), PM_NODE_IDX(VERSAL_PM_DEV_USB_0)),

    PM_CLK2NODE(PM_NODE_IDX(VERSAL_NET_PM_CLK_USB1_BUS_REF), PM_NODE_IDX(VERSAL_NET_PM_DEV_USB_1)),

    PM_CLK2NODE(PM_NODE_IDX(VERSAL_PM_CLK_UART0_REF), PM_NODE_IDX(VERSAL_PM_DEV_UART_0)),
    PM_CLK2NODE(PM_NODE_IDX(VERSAL_PM_CLK_UART1_REF), PM_NODE_IDX(VERSAL_PM_DEV_UART_1)),

    PM_CLK2NODE(PM_NODE_IDX(VERSAL_PM_CLK_SPI0_REF), PM_NODE_IDX(VERSAL_PM_DEV_SPI_0)),
    PM_CLK2NODE(PM_NODE_IDX(VERSAL_PM_CLK_SPI1_REF), PM_NODE_IDX(VERSAL_PM_DEV_SPI_1)),

    PM_CLK2NODE(PM_NODE_IDX(VERSAL_PM_CLK_CAN0_REF), PM_NODE_IDX(VERSAL_PM_DEV_CAN_FD_0)),
    PM_CLK2NODE(PM_NODE_IDX(VERSAL_PM_CLK_CAN1_REF), PM_NODE_IDX(VERSAL_PM_DEV_CAN_FD_1)),

    PM_CLK2NODE(PM_NODE_IDX(VERSAL_PM_CLK_I2C0_REF), PM_NODE_IDX(VERSAL_PM_DEV_I2C_0)),
    PM_CLK2NODE(PM_NODE_IDX(VERSAL_PM_CLK_I2C1_REF), PM_NODE_IDX(VERSAL_PM_DEV_I2C_1)),

    PM_CLK2NODE(PM_NODE_IDX(VERSAL_PM_CLK_TIMESTAMP_REF),PM_NODE_IDX(VERSAL_PM_DEV_TTC_0)),
};

/* Last clock node index */
#define VERSAL_NET_PM_CLK_END_IDX  PM_NODE_IDX(VERSAL_NET_PM_CLK_FLX_PLL)

bool versal_net_eemi(struct cpu_user_regs *regs)
{
    uint32_t fid = get_user_reg(regs, 0);
    uint32_t nodeid;
    uint32_t pm_fn;
    enum pm_ret_status ret;

    if (fid == PASS_THROUGH_SMC_ID) {
        pm_fn = (uint32_t)get_user_reg(regs, 1);
        if (MODULE_ID(pm_fn) == XPM_MODULE_ID)
            pm_fn &= XPM_API_ID_MASK;
        fid = EEMI_FID(pm_fn);
        nodeid = (uint32_t)(get_user_reg(regs, 1) >> 32);
    } else {
        nodeid = (uint32_t)get_user_reg(regs, 1);
        pm_fn = EEMI_PM_FID(fid);
    }

    switch (fid)
    {
    /* Mediated MMIO access.  */
    case EEMI_FID(PM_MMIO_WRITE):
    case EEMI_FID(PM_MMIO_READ):
        /* TBD */
        ret = XST_PM_NOTSUPPORTED;
        goto done;

    /*
     * Get the guest SGI number appropriately, this assumes it is on the
     * second register (x1). The third register is to reset SGI.
     * If it is set to 1, then SGI is unregistered and xen won't
     * send SGI to that guest VM anymore.
     */
    case EEMI_FID(TF_A_PM_REGISTER_SGI):
    {
        bool reset_sgi = get_user_reg(regs, 2);

        /* check for invalid SGI */
        if ( nodeid >= MAX_SGI_VERSAL_NET )
        {
            ret = XST_PM_INVALID_PARAM;
            goto done;
        }

        /* reset sgi number if requested */
        if ( reset_sgi )
        {
            current->domain->arch.firmware_sgi = 0;
            ret = XST_PM_SUCCESS;
            goto done;
        }

        /* already registered */
        if ( current->domain->arch.firmware_sgi != 0 )
        {
            ret = XST_PM_DOUBLE_REQ;
            goto done;
        }

        /* set relative SGI number */
        current->domain->arch.firmware_sgi = nodeid;

        ret = XST_PM_SUCCESS;
        goto done;
    }

    default:
        return xilinx_eemi(regs, fid, PM_NODE_IDX(nodeid), pm_fn,
                           pm_node_access,
                           ARRAY_SIZE(pm_node_access),
                           pm_rst_access,
                           ARRAY_SIZE(pm_rst_access),
                           pm_clk_node_map,
                           ARRAY_SIZE(pm_clk_node_map),
                           VERSAL_NET_PM_CLK_END_IDX);
    }

done:
    set_user_reg(regs, 0, ret);
    return true;
}
