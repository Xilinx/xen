/*
 * xen/arch/arm/platforms/xilinx-versal-eemi.c
 *
 * Xilinx Versal EEMI API mediator.
 *
 * Copyright (c) 2019 Xilinx Inc.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <asm/regs.h>
#include <xen/iocap.h>
#include <xen/sched.h>
#include <asm/smccc.h>
#include <asm/platforms/xilinx-eemi.h>
#include <asm/platforms/xilinx-versal-mm.h>


/*
 * This table maps a node into a memory address.
 * If a guest has access to the address, it has enough control
 * over the node to grant it access to EEMI calls for that node.
 */
#define PM_NODE_IDX(Id) ((Id) & 0x3FFF)

static const struct pm_access pm_node_access[] = {
    [PM_NODE_IDX(VERSAL_PM_DEV_PLD_0)] = { .hwdom_access = true },

    [PM_NODE_IDX(VERSAL_PM_DEV_ACPU_0)] = { 0 },
    [PM_NODE_IDX(VERSAL_PM_DEV_ACPU_1)] = { 0 },
    [PM_NODE_IDX(VERSAL_PM_DEV_RPU0_0)] = { MM_DEV_RPU0_0 },
    [PM_NODE_IDX(VERSAL_PM_DEV_RPU0_1)] = { MM_DEV_RPU0_1 },

    [PM_NODE_IDX(VERSAL_PM_DEV_OCM_0)] = { MM_DEV_OCM_0 },
    [PM_NODE_IDX(VERSAL_PM_DEV_OCM_1)] = { MM_DEV_OCM_1 },
    [PM_NODE_IDX(VERSAL_PM_DEV_OCM_2)] = { MM_DEV_OCM_2 },
    [PM_NODE_IDX(VERSAL_PM_DEV_OCM_3)] = { MM_DEV_OCM_3 },
    [PM_NODE_IDX(VERSAL_PM_DEV_TCM_0_A)] = { MM_DEV_TCM_0_A },
    [PM_NODE_IDX(VERSAL_PM_DEV_TCM_0_B)] = { MM_DEV_TCM_0_B },
    [PM_NODE_IDX(VERSAL_PM_DEV_TCM_1_A)] = { MM_DEV_TCM_1_A },
    [PM_NODE_IDX(VERSAL_PM_DEV_TCM_1_B)] = { MM_DEV_TCM_1_B },

    [PM_NODE_IDX(VERSAL_PM_DEV_L2_BANK_0)] = { .hwdom_access = true },

    /* Should Dom0 have access to this? */
    [PM_NODE_IDX(VERSAL_PM_DEV_DDR_0)] = { .hwdom_access = true },

    [PM_NODE_IDX(VERSAL_PM_DEV_USB_0)] = { MM_DEV_USB_0 },
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
    [PM_NODE_IDX(VERSAL_PM_DEV_SWDT_LPD)] = { MM_DEV_SWDT_LPD },
    [PM_NODE_IDX(VERSAL_PM_DEV_SWDT_FPD)] = { MM_DEV_SWDT_FPD },
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

    [PM_NODE_IDX(VERSAL_PM_DEV_IPI_0)] = { .hwdom_access = true },
    [PM_NODE_IDX(VERSAL_PM_DEV_IPI_1)] = { MM_DEV_IPI_1 },
    [PM_NODE_IDX(VERSAL_PM_DEV_IPI_2)] = { MM_DEV_IPI_2 },
    [PM_NODE_IDX(VERSAL_PM_DEV_IPI_3)] = { MM_DEV_IPI_3 },
    [PM_NODE_IDX(VERSAL_PM_DEV_IPI_4)] = { MM_DEV_IPI_4 },
    [PM_NODE_IDX(VERSAL_PM_DEV_IPI_5)] = { MM_DEV_IPI_5 },
    [PM_NODE_IDX(VERSAL_PM_DEV_IPI_6)] = { MM_DEV_IPI_6 },

    /* Should Dom0 have access to this? */
    [PM_NODE_IDX(VERSAL_PM_DEV_DDRMC_0)] = { .hwdom_access = true },
    [PM_NODE_IDX(VERSAL_PM_DEV_DDRMC_1)] = { .hwdom_access = true },
    [PM_NODE_IDX(VERSAL_PM_DEV_DDRMC_2)] = { .hwdom_access = true },
    [PM_NODE_IDX(VERSAL_PM_DEV_DDRMC_3)] = { .hwdom_access = true },

    [PM_NODE_IDX(VERSAL_PM_DEV_GT_0)] = { MM_DEV_GT_0 },
    [PM_NODE_IDX(VERSAL_PM_DEV_GT_1)] = { MM_DEV_GT_1 },
    [PM_NODE_IDX(VERSAL_PM_DEV_GT_2)] = { MM_DEV_GT_2 },
    [PM_NODE_IDX(VERSAL_PM_DEV_GT_3)] = { MM_DEV_GT_3 },
    [PM_NODE_IDX(VERSAL_PM_DEV_GT_4)] = { MM_DEV_GT_4 },
    [PM_NODE_IDX(VERSAL_PM_DEV_GT_5)] = { MM_DEV_GT_5 },
    [PM_NODE_IDX(VERSAL_PM_DEV_GT_6)] = { MM_DEV_GT_6 },
    [PM_NODE_IDX(VERSAL_PM_DEV_GT_7)] = { MM_DEV_GT_7 },
    [PM_NODE_IDX(VERSAL_PM_DEV_GT_8)] = { MM_DEV_GT_8 },
    [PM_NODE_IDX(VERSAL_PM_DEV_GT_9)] = { MM_DEV_GT_9 },

    [PM_NODE_IDX(VERSAL_PM_DEV_GT_10)] = { MM_DEV_GT_10 },
    [PM_NODE_IDX(VERSAL_PM_DEV_EFUSE_CACHE)] = { MM_DEV_EFUSE_CACHE },
    [PM_NODE_IDX(VERSAL_PM_DEV_AMS_ROOT)] = { MM_DEV_AMS_ROOT },

    [PM_NODE_IDX(VERSAL_PM_DEV_AIE)] = { MM_DEV_AIE },
};

/*
 * This table maps a reset node into its corresponding device node.
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

    [PM_NODE_IDX(VERSAL_PM_RST_OCM2_POR)] = { .hwdom_access = true },
    [PM_NODE_IDX(VERSAL_PM_RST_PS_SRST)] = { .hwdom_access = true },
    [PM_NODE_IDX(VERSAL_PM_RST_PL_SRST)] = { .hwdom_access = true },
    [PM_NODE_IDX(VERSAL_PM_RST_NOC)] = { .hwdom_access = true },
    [PM_NODE_IDX(VERSAL_PM_RST_NPI)] = { .hwdom_access = true },
    [PM_NODE_IDX(VERSAL_PM_RST_SYS_RST_1)] = { .hwdom_access = true },
    [PM_NODE_IDX(VERSAL_PM_RST_SYS_RST_2)] = { .hwdom_access = true },
    [PM_NODE_IDX(VERSAL_PM_RST_SYS_RST_3)] = { .hwdom_access = true },
    [PM_NODE_IDX(VERSAL_PM_RST_FPD)] = { .hwdom_access = true },

    [PM_NODE_IDX(VERSAL_PM_RST_PL0)] = { PM_NODE_IDX(VERSAL_PM_DEV_PLD_0) },
    [PM_NODE_IDX(VERSAL_PM_RST_PL1)] = { PM_NODE_IDX(VERSAL_PM_DEV_PLD_0) },
    [PM_NODE_IDX(VERSAL_PM_RST_PL2)] = { PM_NODE_IDX(VERSAL_PM_DEV_PLD_0) },
    [PM_NODE_IDX(VERSAL_PM_RST_PL3)] = { PM_NODE_IDX(VERSAL_PM_DEV_PLD_0) },

    [PM_NODE_IDX(VERSAL_PM_RST_APU)] = { 0 },
    [PM_NODE_IDX(VERSAL_PM_RST_ACPU_0)] = { 0 },
    [PM_NODE_IDX(VERSAL_PM_RST_ACPU_1)] = { 0 },
    [PM_NODE_IDX(VERSAL_PM_RST_ACPU_L2)] = { 0 },
    [PM_NODE_IDX(VERSAL_PM_RST_ACPU_GIC)] = { 0 },

    [PM_NODE_IDX(VERSAL_PM_RST_RPU_ISLAND)] = { PM_NODE_IDX(VERSAL_PM_DEV_RPU0_0) },
    [PM_NODE_IDX(VERSAL_PM_RST_RPU_AMBA)] = { PM_NODE_IDX(VERSAL_PM_DEV_RPU0_0) },
    [PM_NODE_IDX(VERSAL_PM_RST_R5_0)] = { PM_NODE_IDX(VERSAL_PM_DEV_RPU0_0) },
    [PM_NODE_IDX(VERSAL_PM_RST_R5_1)] = { PM_NODE_IDX(VERSAL_PM_DEV_RPU0_1) },

    [PM_NODE_IDX(VERSAL_PM_RST_SYSMON_PMC_SEQ_RST)] = { .hwdom_access = true },
    [PM_NODE_IDX(VERSAL_PM_RST_SYSMON_PMC_CFG_RST)] = { .hwdom_access = true },
    [PM_NODE_IDX(VERSAL_PM_RST_SYSMON_FPD_CFG_RST)] = { .hwdom_access = true },
    [PM_NODE_IDX(VERSAL_PM_RST_SYSMON_FPD_SEQ_RST)] = { .hwdom_access = true },
    [PM_NODE_IDX(VERSAL_PM_RST_SYSMON_LPD)] = { .hwdom_access = true },
    [PM_NODE_IDX(VERSAL_PM_RST_PDMA_RST1)] = { .hwdom_access = true },
    [PM_NODE_IDX(VERSAL_PM_RST_PDMA_RST0)] = { .hwdom_access = true },

    /* ADMA Channel 0 grants access to pull the reset signal.  */
    [PM_NODE_IDX(VERSAL_PM_RST_ADMA)] = { PM_NODE_IDX(VERSAL_PM_DEV_ADMA_0) },
    [PM_NODE_IDX(VERSAL_PM_RST_TIMESTAMP)] = { .hwdom_access = true },
    [PM_NODE_IDX(VERSAL_PM_RST_OCM)] = { PM_NODE_IDX(VERSAL_PM_DEV_OCM_0) },
    [PM_NODE_IDX(VERSAL_PM_RST_OCM2_RST)] = { .hwdom_access = true },
    [PM_NODE_IDX(VERSAL_PM_RST_IPI)] = { .hwdom_access = true },
    [PM_NODE_IDX(VERSAL_PM_RST_SBI)] = { .hwdom_access = true },

    /* No ops on LPD */
    [PM_NODE_IDX(VERSAL_PM_RST_LPD)] = { 0 },

    [PM_NODE_IDX(VERSAL_PM_RST_QSPI)] = { PM_NODE_IDX(VERSAL_PM_DEV_QSPI) },
    [PM_NODE_IDX(VERSAL_PM_RST_OSPI)] = { PM_NODE_IDX(VERSAL_PM_DEV_OSPI) },
    [PM_NODE_IDX(VERSAL_PM_RST_SDIO_0)] = { PM_NODE_IDX(VERSAL_PM_DEV_SDIO_0) },
    [PM_NODE_IDX(VERSAL_PM_RST_SDIO_1)] = { PM_NODE_IDX(VERSAL_PM_DEV_SDIO_1) },
    [PM_NODE_IDX(VERSAL_PM_RST_I2C_PMC)] = { PM_NODE_IDX(VERSAL_PM_DEV_I2C_PMC) },
    [PM_NODE_IDX(VERSAL_PM_RST_GPIO_PMC)] = { PM_NODE_IDX(VERSAL_PM_DEV_GPIO_PMC) },
    [PM_NODE_IDX(VERSAL_PM_RST_GEM_0)] = { PM_NODE_IDX(VERSAL_PM_DEV_GEM_0) },
    [PM_NODE_IDX(VERSAL_PM_RST_GEM_1)] = { PM_NODE_IDX(VERSAL_PM_DEV_GEM_1) },

    [PM_NODE_IDX(VERSAL_PM_RST_SPARE)] = { .hwdom_access = true },
    [PM_NODE_IDX(VERSAL_PM_RST_USB_0)] = { PM_NODE_IDX(VERSAL_PM_DEV_USB_0) },

    [PM_NODE_IDX(VERSAL_PM_RST_UART_0)] = { PM_NODE_IDX(VERSAL_PM_DEV_UART_0) },
    [PM_NODE_IDX(VERSAL_PM_RST_UART_1)] = { PM_NODE_IDX(VERSAL_PM_DEV_UART_1) },
    [PM_NODE_IDX(VERSAL_PM_RST_SPI_0)] = { PM_NODE_IDX(VERSAL_PM_DEV_SPI_0) },
    [PM_NODE_IDX(VERSAL_PM_RST_SPI_1)] = { PM_NODE_IDX(VERSAL_PM_DEV_SPI_1) },
    [PM_NODE_IDX(VERSAL_PM_RST_CAN_FD_0)] = { PM_NODE_IDX(VERSAL_PM_DEV_CAN_FD_0) },
    [PM_NODE_IDX(VERSAL_PM_RST_CAN_FD_1)] = { PM_NODE_IDX(VERSAL_PM_DEV_CAN_FD_1) },
    [PM_NODE_IDX(VERSAL_PM_RST_I2C_0)] = { PM_NODE_IDX(VERSAL_PM_DEV_I2C_0) },
    [PM_NODE_IDX(VERSAL_PM_RST_I2C_1)] = { PM_NODE_IDX(VERSAL_PM_DEV_I2C_1) },

    [PM_NODE_IDX(VERSAL_PM_RST_GPIO_LPD)] = { PM_NODE_IDX(VERSAL_PM_DEV_GPIO) },

    [PM_NODE_IDX(VERSAL_PM_RST_TTC_0)] = { PM_NODE_IDX(VERSAL_PM_DEV_TTC_0) },
    [PM_NODE_IDX(VERSAL_PM_RST_TTC_1)] = { PM_NODE_IDX(VERSAL_PM_DEV_TTC_1) },
    [PM_NODE_IDX(VERSAL_PM_RST_TTC_2)] = { PM_NODE_IDX(VERSAL_PM_DEV_TTC_2) },
    [PM_NODE_IDX(VERSAL_PM_RST_TTC_3)] = { PM_NODE_IDX(VERSAL_PM_DEV_TTC_3) },

    [PM_NODE_IDX(VERSAL_PM_RST_SWDT_FPD)] = { PM_NODE_IDX(VERSAL_PM_DEV_SWDT_FPD) },
    [PM_NODE_IDX(VERSAL_PM_RST_SWDT_LPD)] = { PM_NODE_IDX(VERSAL_PM_DEV_SWDT_LPD) },

    [PM_NODE_IDX(VERSAL_PM_RST_USB)] = { VERSAL_PM_DEV_USB_0 },
    [PM_NODE_IDX(VERSAL_PM_RST_DPC)] = { .hwdom_access = true },
    [PM_NODE_IDX(VERSAL_PM_RST_PMCDBG)] = { .hwdom_access = true },
    [PM_NODE_IDX(VERSAL_PM_RST_DBG_TRACE)] = { .hwdom_access = true },
    [PM_NODE_IDX(VERSAL_PM_RST_DBG_FPD)] = { .hwdom_access = true },
    [PM_NODE_IDX(VERSAL_PM_RST_DBG_TSTMP)] = { .hwdom_access = true },
    [PM_NODE_IDX(VERSAL_PM_RST_RPU0_DBG)] = { .hwdom_access = true },
    [PM_NODE_IDX(VERSAL_PM_RST_RPU1_DBG)] = { .hwdom_access = true },
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
    [PM_NODE_IDX(VERSAL_PM_RST_L2_0)] = { .hwdom_access = true },
    [PM_NODE_IDX(VERSAL_PM_RST_L2_1)] = { .hwdom_access = true },
    [PM_NODE_IDX(VERSAL_PM_RST_ADDR_REMAP)] = { .hwdom_access = true },
    [PM_NODE_IDX(VERSAL_PM_RST_CPI0)] = { .hwdom_access = true },
    [PM_NODE_IDX(VERSAL_PM_RST_CPI1)] = { .hwdom_access = true },

    [PM_NODE_IDX(VERSAL_PM_RST_AIE_ARRAY)] = { PM_NODE_IDX(VERSAL_PM_DEV_AIE) },
    [PM_NODE_IDX(VERSAL_PM_RST_AIE_SHIM)] = { PM_NODE_IDX(VERSAL_PM_DEV_AIE) },
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
    PM_CLK2NODE(PM_NODE_IDX(VERSAL_PM_CLK_PMC_PL0_REF), PM_NODE_IDX(VERSAL_PM_DEV_PLD_0)),
    PM_CLK2NODE(PM_NODE_IDX(VERSAL_PM_CLK_PMC_PL1_REF), PM_NODE_IDX(VERSAL_PM_DEV_PLD_0)),
    PM_CLK2NODE(PM_NODE_IDX(VERSAL_PM_CLK_PMC_PL2_REF), PM_NODE_IDX(VERSAL_PM_DEV_PLD_0)),
    PM_CLK2NODE(PM_NODE_IDX(VERSAL_PM_CLK_PMC_PL3_REF), PM_NODE_IDX(VERSAL_PM_DEV_PLD_0)),
    PM_CLK2NODE(PM_NODE_IDX(VERSAL_PM_CLK_ACPU), PM_NODE_IDX(VERSAL_PM_DEV_ACPU_0)),
    PM_CLK2NODE(PM_NODE_IDX(VERSAL_PM_CLK_ACPU), PM_NODE_IDX(VERSAL_PM_DEV_ACPU_1)),
    PM_CLK2NODE(PM_NODE_IDX(VERSAL_PM_CLK_ADMA), PM_NODE_IDX(VERSAL_PM_DEV_ADMA_0)),
    PM_CLK2NODE(PM_NODE_IDX(VERSAL_PM_CLK_ADMA), PM_NODE_IDX(VERSAL_PM_DEV_ADMA_1)),
    PM_CLK2NODE(PM_NODE_IDX(VERSAL_PM_CLK_ADMA), PM_NODE_IDX(VERSAL_PM_DEV_ADMA_2)),
    PM_CLK2NODE(PM_NODE_IDX(VERSAL_PM_CLK_ADMA), PM_NODE_IDX(VERSAL_PM_DEV_ADMA_3)),
    PM_CLK2NODE(PM_NODE_IDX(VERSAL_PM_CLK_ADMA), PM_NODE_IDX(VERSAL_PM_DEV_ADMA_4)),
    PM_CLK2NODE(PM_NODE_IDX(VERSAL_PM_CLK_ADMA), PM_NODE_IDX(VERSAL_PM_DEV_ADMA_5)),
    PM_CLK2NODE(PM_NODE_IDX(VERSAL_PM_CLK_ADMA), PM_NODE_IDX(VERSAL_PM_DEV_ADMA_6)),
    PM_CLK2NODE(PM_NODE_IDX(VERSAL_PM_CLK_ADMA), PM_NODE_IDX(VERSAL_PM_DEV_ADMA_7)),
    PM_CLK2NODE(PM_NODE_IDX(VERSAL_PM_CLK_CPU_R5_CORE), PM_NODE_IDX(VERSAL_PM_DEV_RPU0_0)),
    PM_CLK2NODE(PM_NODE_IDX(VERSAL_PM_CLK_CPU_R5_CORE), PM_NODE_IDX(VERSAL_PM_DEV_RPU0_1)),
    PM_CLK2NODE(PM_NODE_IDX(VERSAL_PM_CLK_CPU_R5_OCM), PM_NODE_IDX(VERSAL_PM_DEV_OCM_0)),
    PM_CLK2NODE(PM_NODE_IDX(VERSAL_PM_CLK_CPU_R5_OCM), PM_NODE_IDX(VERSAL_PM_DEV_OCM_1)),
    PM_CLK2NODE(PM_NODE_IDX(VERSAL_PM_CLK_CPU_R5_OCM), PM_NODE_IDX(VERSAL_PM_DEV_OCM_2)),
    PM_CLK2NODE(PM_NODE_IDX(VERSAL_PM_CLK_CPU_R5_OCM), PM_NODE_IDX(VERSAL_PM_DEV_OCM_3)),
    PM_CLK2NODE(PM_NODE_IDX(VERSAL_PM_CLK_GEM0_REF), PM_NODE_IDX(VERSAL_PM_DEV_GEM_0)),
    PM_CLK2NODE(PM_NODE_IDX(VERSAL_PM_CLK_GEM1_REF), PM_NODE_IDX(VERSAL_PM_DEV_GEM_1)),
    PM_CLK2NODE(PM_NODE_IDX(VERSAL_PM_CLK_GEM_TSU_REF), PM_NODE_IDX(VERSAL_PM_DEV_GEM_0)),
    PM_CLK2NODE(PM_NODE_IDX(VERSAL_PM_CLK_GEM_TSU_REF), PM_NODE_IDX(VERSAL_PM_DEV_GEM_1)),
    PM_CLK2NODE(PM_NODE_IDX(VERSAL_PM_CLK_USB0_BUS_REF), PM_NODE_IDX(VERSAL_PM_DEV_USB_0)),
    PM_CLK2NODE(PM_NODE_IDX(VERSAL_PM_CLK_UART0_REF), PM_NODE_IDX(VERSAL_PM_DEV_UART_0)),
    PM_CLK2NODE(PM_NODE_IDX(VERSAL_PM_CLK_UART1_REF), PM_NODE_IDX(VERSAL_PM_DEV_UART_1)),
    PM_CLK2NODE(PM_NODE_IDX(VERSAL_PM_CLK_SPI0_REF), PM_NODE_IDX(VERSAL_PM_DEV_SPI_0)),
    PM_CLK2NODE(PM_NODE_IDX(VERSAL_PM_CLK_SPI1_REF), PM_NODE_IDX(VERSAL_PM_DEV_SPI_1)),
    PM_CLK2NODE(PM_NODE_IDX(VERSAL_PM_CLK_CAN0_REF), PM_NODE_IDX(VERSAL_PM_DEV_CAN_FD_0)),
    PM_CLK2NODE(PM_NODE_IDX(VERSAL_PM_CLK_CAN1_REF), PM_NODE_IDX(VERSAL_PM_DEV_CAN_FD_1)),
    PM_CLK2NODE(PM_NODE_IDX(VERSAL_PM_CLK_I2C0_REF), PM_NODE_IDX(VERSAL_PM_DEV_I2C_0)),
    PM_CLK2NODE(PM_NODE_IDX(VERSAL_PM_CLK_I2C1_REF), PM_NODE_IDX(VERSAL_PM_DEV_I2C_1)),
    PM_CLK2NODE(PM_NODE_IDX(VERSAL_PM_CLK_USB3_DUAL_REF), PM_NODE_IDX(VERSAL_PM_DEV_USB_0)),
};

/* bound check to match ZynqMP EEMI handling */
static inline bool pll_in_bounds(u32 nodeid)
{
    return nodeid & 0x8000000U;
}

/* Last clock node index */
#define VERSAL_PM_CLK_END_IDX  PM_NODE_IDX(VERSAL_PM_CLK_XRAM_APB)

bool versal_eemi(struct cpu_user_regs *regs)
{
    struct arm_smccc_res res;
    uint32_t fid = get_user_reg(regs, 0);
    uint32_t nodeid = get_user_reg(regs, 1);
    uint32_t pm_fn = EEMI_PM_FID(fid);
    enum pm_ret_status ret;

    switch (fid)
    {
    /* These calls are safe and always allowed.  */
    case EEMI_FID(PM_FEATURE_CHECK):
        goto forward_to_fw;

    /* Mediated MMIO access.  */
    case EEMI_FID(PM_MMIO_WRITE):
    case EEMI_FID(PM_MMIO_READ):
        /* TBD */
        ret = XST_PM_NOTSUPPORTED;
        goto done;


    case EEMI_FID(PM_PLL_GET_PARAMETER):
    case EEMI_FID(PM_PLL_GET_MODE):
        if ( !pll_in_bounds(nodeid) )
        {
            gprintk(XENLOG_WARNING, "versal-pm: fn=%u Invalid pll node %u\n",
                    pm_fn, nodeid);
            ret = XST_PM_INVALID_PARAM;
            goto done;
        }
        else
            goto forward_to_fw;

    case EEMI_FID(PM_PLL_SET_PARAMETER):
    case EEMI_FID(PM_PLL_SET_MODE):
        if ( !pll_in_bounds(nodeid) )
        {
            gprintk(XENLOG_WARNING, "versal-pm: fn=%u Invalid pll node %u\n",
                    pm_fn, nodeid);
            ret = XST_PM_INVALID_PARAM;
            goto done;
        }
        if ( !domain_has_node_access(current->domain, PM_NODE_IDX(nodeid),
                                     pm_node_access,
                                     ARRAY_SIZE(pm_node_access)) )
        {
            gprintk(XENLOG_WARNING, "versal-pm: fn=0x%04x No access to pll=0x%08x\n",
                    pm_fn, nodeid);
            ret = XST_PM_NO_ACCESS;
            goto done;
        }
        goto forward_to_fw;

    default:
        return xilinx_eemi(regs, fid, PM_NODE_IDX(nodeid), pm_fn,
                           pm_node_access,
                           ARRAY_SIZE(pm_node_access),
                           pm_rst_access,
                           ARRAY_SIZE(pm_rst_access),
                           pm_clk_node_map,
                           ARRAY_SIZE(pm_clk_node_map),
                           VERSAL_PM_CLK_END_IDX);
    }

forward_to_fw:
    /* Re-encode pm args.  */
    arm_smccc_1_1_smc(get_user_reg(regs, 0),
                      get_user_reg(regs, 1),
                      get_user_reg(regs, 2),
                      get_user_reg(regs, 3),
                      get_user_reg(regs, 4),
                      get_user_reg(regs, 5),
                      get_user_reg(regs, 6),
                      get_user_reg(regs, 7),
                      &res);

    set_user_reg(regs, 0, res.a0);
    set_user_reg(regs, 1, res.a1);
    set_user_reg(regs, 2, res.a2);
    set_user_reg(regs, 3, res.a3);
    return true;

done:
    set_user_reg(regs, 0, ret);
    return true;
}
