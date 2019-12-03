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
#include <asm/platforms/xilinx-versal-eemi.h>
#include <asm/platforms/xilinx-versal-mm.h>

/*
 * Selected set of memory mapped definitions of device nodes.
 */
struct pm_access
{
    uint32_t addr;
    bool hwdom_access;    /* HW domain gets access regardless. */
};

/*
 * This table maps a node into a memory address.
 * If a guest has access to the address, it has enough control
 * over the node to grant it access to EEMI calls for that node.
 */
#define PM_NODE_IDX(Id) ((Id) & 0x3FFF)

static const struct pm_access pm_node_access[] = {
    [PM_NODE_IDX(PM_DEV_ACPU_0)] = { 0 },
    [PM_NODE_IDX(PM_DEV_ACPU_1)] = { 0 },
    [PM_NODE_IDX(PM_DEV_RPU0_0)] = { MM_DEV_RPU0_0 },
    [PM_NODE_IDX(PM_DEV_RPU0_1)] = { MM_DEV_RPU0_1 },

    [PM_NODE_IDX(PM_DEV_OCM_0)] = { MM_DEV_OCM_0 },
    [PM_NODE_IDX(PM_DEV_OCM_1)] = { MM_DEV_OCM_1 },
    [PM_NODE_IDX(PM_DEV_OCM_2)] = { MM_DEV_OCM_2 },
    [PM_NODE_IDX(PM_DEV_OCM_3)] = { MM_DEV_OCM_3 },
    [PM_NODE_IDX(PM_DEV_TCM_0_A)] = { MM_DEV_TCM_0_A },
    [PM_NODE_IDX(PM_DEV_TCM_0_B)] = { MM_DEV_TCM_0_B },
    [PM_NODE_IDX(PM_DEV_TCM_1_A)] = { MM_DEV_TCM_1_A },
    [PM_NODE_IDX(PM_DEV_TCM_1_B)] = { MM_DEV_TCM_1_B },

    [PM_NODE_IDX(PM_DEV_L2_BANK_0)] = { .hwdom_access = true },

    /* Should Dom0 have access to this? */
    [PM_NODE_IDX(PM_DEV_DDR_0)] = { .hwdom_access = true },

    [PM_NODE_IDX(PM_DEV_USB_0)] = { MM_DEV_USB_0 },
    [PM_NODE_IDX(PM_DEV_GEM_0)] = { MM_DEV_GEM_0 },
    [PM_NODE_IDX(PM_DEV_GEM_1)] = { MM_DEV_GEM_1 },
    [PM_NODE_IDX(PM_DEV_SPI_0)] = { MM_DEV_SPI_0 },
    [PM_NODE_IDX(PM_DEV_SPI_1)] = { MM_DEV_SPI_1 },
    [PM_NODE_IDX(PM_DEV_I2C_0)] = { MM_DEV_I2C_0 },
    [PM_NODE_IDX(PM_DEV_I2C_1)] = { MM_DEV_I2C_1 },
    [PM_NODE_IDX(PM_DEV_CAN_FD_0)] = { MM_DEV_CAN_FD_0 },
    [PM_NODE_IDX(PM_DEV_CAN_FD_1)] = { MM_DEV_CAN_FD_1 },
    [PM_NODE_IDX(PM_DEV_UART_0)] = { MM_DEV_UART_0 },
    [PM_NODE_IDX(PM_DEV_UART_1)] = { MM_DEV_UART_1 },
    [PM_NODE_IDX(PM_DEV_GPIO)] = { MM_DEV_GPIO },
    [PM_NODE_IDX(PM_DEV_TTC_0)] = { MM_DEV_TTC_0 },
    [PM_NODE_IDX(PM_DEV_TTC_1)] = { MM_DEV_TTC_1 },
    [PM_NODE_IDX(PM_DEV_TTC_2)] = { MM_DEV_TTC_2 },
    [PM_NODE_IDX(PM_DEV_TTC_3)] = { MM_DEV_TTC_3 },
    [PM_NODE_IDX(PM_DEV_SWDT_LPD)] = { MM_DEV_SWDT_LPD },
    [PM_NODE_IDX(PM_DEV_SWDT_FPD)] = { MM_DEV_SWDT_FPD },
    [PM_NODE_IDX(PM_DEV_OSPI)] = { MM_DEV_OSPI },
    [PM_NODE_IDX(PM_DEV_QSPI)] = { MM_DEV_QSPI },
    [PM_NODE_IDX(PM_DEV_GPIO_PMC)] = { MM_DEV_GPIO_PMC },
    [PM_NODE_IDX(PM_DEV_I2C_PMC)] = { MM_DEV_I2C_PMC },
    [PM_NODE_IDX(PM_DEV_SDIO_0)] = { MM_DEV_SDIO_0 },
    [PM_NODE_IDX(PM_DEV_SDIO_1)] = { MM_DEV_SDIO_1 },

    [PM_NODE_IDX(PM_DEV_PL_0)] = { .hwdom_access = true },
    [PM_NODE_IDX(PM_DEV_PL_1)] = { .hwdom_access = true },
    [PM_NODE_IDX(PM_DEV_PL_2)] = { .hwdom_access = true },
    [PM_NODE_IDX(PM_DEV_PL_3)] = { .hwdom_access = true },

    [PM_NODE_IDX(PM_DEV_RTC)] = { MM_DEV_RTC },
    [PM_NODE_IDX(PM_DEV_ADMA_0)] = { MM_DEV_ADMA_0 },
    [PM_NODE_IDX(PM_DEV_ADMA_1)] = { MM_DEV_ADMA_1 },
    [PM_NODE_IDX(PM_DEV_ADMA_2)] = { MM_DEV_ADMA_2 },
    [PM_NODE_IDX(PM_DEV_ADMA_3)] = { MM_DEV_ADMA_3 },
    [PM_NODE_IDX(PM_DEV_ADMA_4)] = { MM_DEV_ADMA_4 },
    [PM_NODE_IDX(PM_DEV_ADMA_5)] = { MM_DEV_ADMA_5 },
    [PM_NODE_IDX(PM_DEV_ADMA_6)] = { MM_DEV_ADMA_6 },
    [PM_NODE_IDX(PM_DEV_ADMA_7)] = { MM_DEV_ADMA_7 },

    [PM_NODE_IDX(PM_DEV_IPI_0)] = { .hwdom_access = true },
    [PM_NODE_IDX(PM_DEV_IPI_1)] = { MM_DEV_IPI_1 },
    [PM_NODE_IDX(PM_DEV_IPI_2)] = { MM_DEV_IPI_2 },
    [PM_NODE_IDX(PM_DEV_IPI_3)] = { MM_DEV_IPI_3 },
    [PM_NODE_IDX(PM_DEV_IPI_4)] = { MM_DEV_IPI_4 },
    [PM_NODE_IDX(PM_DEV_IPI_5)] = { MM_DEV_IPI_5 },
    [PM_NODE_IDX(PM_DEV_IPI_6)] = { MM_DEV_IPI_6 },

    /* Should Dom0 have access to this? */
    [PM_NODE_IDX(PM_DEV_DDRMC_0)] = { .hwdom_access = true },
    [PM_NODE_IDX(PM_DEV_DDRMC_1)] = { .hwdom_access = true },
    [PM_NODE_IDX(PM_DEV_DDRMC_2)] = { .hwdom_access = true },
    [PM_NODE_IDX(PM_DEV_DDRMC_3)] = { .hwdom_access = true },

    [PM_NODE_IDX(PM_DEV_GT_0)] = { MM_DEV_GT_0 },
    [PM_NODE_IDX(PM_DEV_GT_1)] = { MM_DEV_GT_1 },
    [PM_NODE_IDX(PM_DEV_GT_2)] = { MM_DEV_GT_2 },
    [PM_NODE_IDX(PM_DEV_GT_3)] = { MM_DEV_GT_3 },
    [PM_NODE_IDX(PM_DEV_GT_4)] = { MM_DEV_GT_4 },
    [PM_NODE_IDX(PM_DEV_GT_5)] = { MM_DEV_GT_5 },
    [PM_NODE_IDX(PM_DEV_GT_6)] = { MM_DEV_GT_6 },
    [PM_NODE_IDX(PM_DEV_GT_7)] = { MM_DEV_GT_7 },
    [PM_NODE_IDX(PM_DEV_GT_8)] = { MM_DEV_GT_8 },
    [PM_NODE_IDX(PM_DEV_GT_9)] = { MM_DEV_GT_9 },

    [PM_NODE_IDX(PM_DEV_GT_10)] = { MM_DEV_GT_10 },
    [PM_NODE_IDX(PM_DEV_EFUSE_CACHE)] = { MM_DEV_EFUSE_CACHE },
    [PM_NODE_IDX(PM_DEV_AMS_ROOT)] = { MM_DEV_AMS_ROOT },
};

/*
 * This table maps a reset node into its corresponding device node.
 *
 * Note: reset nodes must be in ascending order!
 */
static const struct pm_access pm_rst_access[] = {
    [PM_NODE_IDX(PM_RST_PMC_POR)] = { .hwdom_access = true },
    [PM_NODE_IDX(PM_RST_PMC)] = { .hwdom_access = true },
    [PM_NODE_IDX(PM_RST_PS_POR)] = { .hwdom_access = true },
    [PM_NODE_IDX(PM_RST_PL_POR)] = { .hwdom_access = true },
    [PM_NODE_IDX(PM_RST_NOC_POR)] = { .hwdom_access = true },
    [PM_NODE_IDX(PM_RST_FPD_POR)] = { .hwdom_access = true },

    /* We don't allow anyone to turn on/off the ACPUs.  */
    [PM_NODE_IDX(PM_RST_ACPU_0_POR)] = { 0 },
    [PM_NODE_IDX(PM_RST_ACPU_1_POR)] = { 0 },

    [PM_NODE_IDX(PM_RST_OCM2_POR)] = { .hwdom_access = true },
    [PM_NODE_IDX(PM_RST_PS_SRST)] = { .hwdom_access = true },
    [PM_NODE_IDX(PM_RST_PL_SRST)] = { .hwdom_access = true },
    [PM_NODE_IDX(PM_RST_NOC)] = { .hwdom_access = true },
    [PM_NODE_IDX(PM_RST_NPI)] = { .hwdom_access = true },
    [PM_NODE_IDX(PM_RST_SYS_RST_1)] = { .hwdom_access = true },
    [PM_NODE_IDX(PM_RST_SYS_RST_2)] = { .hwdom_access = true },
    [PM_NODE_IDX(PM_RST_SYS_RST_3)] = { .hwdom_access = true },
    [PM_NODE_IDX(PM_RST_FPD)] = { .hwdom_access = true },

    [PM_NODE_IDX(PM_RST_PL0)] = { PM_NODE_IDX(PM_DEV_PL_0) },
    [PM_NODE_IDX(PM_RST_PL1)] = { PM_NODE_IDX(PM_DEV_PL_1) },
    [PM_NODE_IDX(PM_RST_PL2)] = { PM_NODE_IDX(PM_DEV_PL_2) },
    [PM_NODE_IDX(PM_RST_PL3)] = { PM_NODE_IDX(PM_DEV_PL_3) },

    [PM_NODE_IDX(PM_RST_APU)] = { 0 },
    [PM_NODE_IDX(PM_RST_ACPU_0)] = { 0 },
    [PM_NODE_IDX(PM_RST_ACPU_1)] = { 0 },
    [PM_NODE_IDX(PM_RST_ACPU_L2)] = { 0 },
    [PM_NODE_IDX(PM_RST_ACPU_GIC)] = { 0 },

    [PM_NODE_IDX(PM_RST_RPU_ISLAND)] = { PM_NODE_IDX(PM_DEV_RPU0_0) },
    [PM_NODE_IDX(PM_RST_RPU_AMBA)] = { PM_NODE_IDX(PM_DEV_RPU0_0) },
    [PM_NODE_IDX(PM_RST_R5_0)] = { PM_NODE_IDX(PM_DEV_RPU0_0) },
    [PM_NODE_IDX(PM_RST_R5_1)] = { PM_NODE_IDX(PM_DEV_RPU0_1) },

    [PM_NODE_IDX(PM_RST_SYSMON_PMC_SEQ_RST)] = { .hwdom_access = true },
    [PM_NODE_IDX(PM_RST_SYSMON_PMC_CFG_RST)] = { .hwdom_access = true },
    [PM_NODE_IDX(PM_RST_SYSMON_FPD_CFG_RST)] = { .hwdom_access = true },
    [PM_NODE_IDX(PM_RST_SYSMON_FPD_SEQ_RST)] = { .hwdom_access = true },
    [PM_NODE_IDX(PM_RST_SYSMON_LPD)] = { .hwdom_access = true },
    [PM_NODE_IDX(PM_RST_PDMA_RST1)] = { .hwdom_access = true },
    [PM_NODE_IDX(PM_RST_PDMA_RST0)] = { .hwdom_access = true },

    /* ADMA Channel 0 grants access to pull the reset signal.  */
    [PM_NODE_IDX(PM_RST_ADMA)] = { PM_NODE_IDX(PM_DEV_ADMA_0) },
    [PM_NODE_IDX(PM_RST_TIMESTAMP)] = { .hwdom_access = true },
    [PM_NODE_IDX(PM_RST_OCM)] = { PM_NODE_IDX(PM_DEV_OCM_0) },
    [PM_NODE_IDX(PM_RST_OCM2_RST)] = { .hwdom_access = true },
    [PM_NODE_IDX(PM_RST_IPI)] = { .hwdom_access = true },
    [PM_NODE_IDX(PM_RST_SBI)] = { .hwdom_access = true },

    /* No ops on LPD */
    [PM_NODE_IDX(PM_RST_LPD)] = { 0 },

    [PM_NODE_IDX(PM_RST_QSPI)] = { PM_NODE_IDX(PM_DEV_QSPI) },
    [PM_NODE_IDX(PM_RST_OSPI)] = { PM_NODE_IDX(PM_DEV_OSPI) },
    [PM_NODE_IDX(PM_RST_SDIO_0)] = { PM_NODE_IDX(PM_DEV_SDIO_0) },
    [PM_NODE_IDX(PM_RST_SDIO_1)] = { PM_NODE_IDX(PM_DEV_SDIO_1) },
    [PM_NODE_IDX(PM_RST_I2C_PMC)] = { PM_NODE_IDX(PM_DEV_I2C_PMC) },
    [PM_NODE_IDX(PM_RST_GPIO_PMC)] = { PM_NODE_IDX(PM_DEV_GPIO_PMC) },
    [PM_NODE_IDX(PM_RST_GEM_0)] = { PM_NODE_IDX(PM_DEV_GEM_0) },
    [PM_NODE_IDX(PM_RST_GEM_1)] = { PM_NODE_IDX(PM_DEV_GEM_1) },

    [PM_NODE_IDX(PM_RST_SPARE)] = { .hwdom_access = true },
    [PM_NODE_IDX(PM_RST_USB_0)] = { PM_NODE_IDX(PM_DEV_USB_0) },

    [PM_NODE_IDX(PM_RST_UART_0)] = { PM_NODE_IDX(PM_DEV_UART_0) },
    [PM_NODE_IDX(PM_RST_UART_1)] = { PM_NODE_IDX(PM_DEV_UART_1) },
    [PM_NODE_IDX(PM_RST_SPI_0)] = { PM_NODE_IDX(PM_DEV_SPI_0) },
    [PM_NODE_IDX(PM_RST_SPI_1)] = { PM_NODE_IDX(PM_DEV_SPI_1) },
    [PM_NODE_IDX(PM_RST_CAN_FD_0)] = { PM_NODE_IDX(PM_DEV_CAN_FD_0) },
    [PM_NODE_IDX(PM_RST_CAN_FD_1)] = { PM_NODE_IDX(PM_DEV_CAN_FD_1) },
    [PM_NODE_IDX(PM_RST_I2C_0)] = { PM_NODE_IDX(PM_DEV_I2C_0) },
    [PM_NODE_IDX(PM_RST_I2C_1)] = { PM_NODE_IDX(PM_DEV_I2C_1) },

    [PM_NODE_IDX(PM_RST_GPIO_LPD)] = { PM_NODE_IDX(PM_DEV_GPIO) },

    [PM_NODE_IDX(PM_RST_TTC_0)] = { PM_NODE_IDX(PM_DEV_TTC_0) },
    [PM_NODE_IDX(PM_RST_TTC_1)] = { PM_NODE_IDX(PM_DEV_TTC_1) },
    [PM_NODE_IDX(PM_RST_TTC_2)] = { PM_NODE_IDX(PM_DEV_TTC_2) },
    [PM_NODE_IDX(PM_RST_TTC_3)] = { PM_NODE_IDX(PM_DEV_TTC_3) },

    [PM_NODE_IDX(PM_RST_SWDT_FPD)] = { PM_NODE_IDX(PM_DEV_SWDT_FPD) },
    [PM_NODE_IDX(PM_RST_SWDT_LPD)] = { PM_NODE_IDX(PM_DEV_SWDT_LPD) },

    [PM_NODE_IDX(PM_RST_USB)] = { PM_DEV_USB_0 },
    [PM_NODE_IDX(PM_RST_DPC)] = { .hwdom_access = true },
    [PM_NODE_IDX(PM_RST_PMCDBG)] = { .hwdom_access = true },
    [PM_NODE_IDX(PM_RST_DBG_TRACE)] = { .hwdom_access = true },
    [PM_NODE_IDX(PM_RST_DBG_FPD)] = { .hwdom_access = true },
    [PM_NODE_IDX(PM_RST_DBG_TSTMP)] = { .hwdom_access = true },
    [PM_NODE_IDX(PM_RST_RPU0_DBG)] = { .hwdom_access = true },
    [PM_NODE_IDX(PM_RST_RPU1_DBG)] = { .hwdom_access = true },
    [PM_NODE_IDX(PM_RST_HSDP)] = { .hwdom_access = true },
    [PM_NODE_IDX(PM_RST_DBG_LPD)] = { .hwdom_access = true },
    [PM_NODE_IDX(PM_RST_CPM_POR)] = { .hwdom_access = true },
    [PM_NODE_IDX(PM_RST_CPM)] = { .hwdom_access = true },
    [PM_NODE_IDX(PM_RST_CPMDBG)] = { .hwdom_access = true },
    [PM_NODE_IDX(PM_RST_PCIE_CFG)] = { .hwdom_access = true },
    [PM_NODE_IDX(PM_RST_PCIE_CORE0)] = { .hwdom_access = true },
    [PM_NODE_IDX(PM_RST_PCIE_CORE1)] = { .hwdom_access = true },
    [PM_NODE_IDX(PM_RST_PCIE_DMA)] = { .hwdom_access = true },
    [PM_NODE_IDX(PM_RST_CMN)] = { .hwdom_access = true },
    [PM_NODE_IDX(PM_RST_L2_0)] = { .hwdom_access = true },
    [PM_NODE_IDX(PM_RST_L2_1)] = { .hwdom_access = true },
    [PM_NODE_IDX(PM_RST_ADDR_REMAP)] = { .hwdom_access = true },
    [PM_NODE_IDX(PM_RST_CPI0)] = { .hwdom_access = true },
    [PM_NODE_IDX(PM_RST_CPI1)] = { .hwdom_access = true },
};

struct pm_clk2node
{
    uint32_t clk_idx;
    uint32_t dev_idx;
};

/*
 * This table maps a clk node into a device node.
 */
#define PM_CLK2NODE(clk, dev)   { .clk_idx = clk, .dev_idx = dev }

static const struct pm_clk2node pm_clk_node_map[] = {
    PM_CLK2NODE(PM_NODE_IDX(PM_CLK_SYSMON_REF), PM_NODE_IDX(PM_DEV_AMS_ROOT)),
    PM_CLK2NODE(PM_NODE_IDX(PM_CLK_TTC0), PM_NODE_IDX(PM_DEV_TTC_0)),
    PM_CLK2NODE(PM_NODE_IDX(PM_CLK_TTC1), PM_NODE_IDX(PM_DEV_TTC_1)),
    PM_CLK2NODE(PM_NODE_IDX(PM_CLK_TTC2), PM_NODE_IDX(PM_DEV_TTC_2)),
    PM_CLK2NODE(PM_NODE_IDX(PM_CLK_TTC3), PM_NODE_IDX(PM_DEV_TTC_3)),
    PM_CLK2NODE(PM_NODE_IDX(PM_CLK_GEM_TSU), PM_NODE_IDX(PM_DEV_GEM_0)),
    PM_CLK2NODE(PM_NODE_IDX(PM_CLK_GEM_TSU), PM_NODE_IDX(PM_DEV_GEM_1)),
    PM_CLK2NODE(PM_NODE_IDX(PM_CLK_GEM0_RX), PM_NODE_IDX(PM_DEV_GEM_0)),
    PM_CLK2NODE(PM_NODE_IDX(PM_CLK_GEM0_TX), PM_NODE_IDX(PM_DEV_GEM_0)),
    PM_CLK2NODE(PM_NODE_IDX(PM_CLK_GEM1_RX), PM_NODE_IDX(PM_DEV_GEM_1)),
    PM_CLK2NODE(PM_NODE_IDX(PM_CLK_GEM1_TX), PM_NODE_IDX(PM_DEV_GEM_1)),
    PM_CLK2NODE(PM_NODE_IDX(PM_CLK_QSPI_REF), PM_NODE_IDX(PM_DEV_QSPI)),
    PM_CLK2NODE(PM_NODE_IDX(PM_CLK_OSPI_REF), PM_NODE_IDX(PM_DEV_OSPI)),
    PM_CLK2NODE(PM_NODE_IDX(PM_CLK_SDIO0_REF), PM_NODE_IDX(PM_DEV_SDIO_0)),
    PM_CLK2NODE(PM_NODE_IDX(PM_CLK_SDIO1_REF), PM_NODE_IDX(PM_DEV_SDIO_1)),
    PM_CLK2NODE(PM_NODE_IDX(PM_CLK_I2C_REF), PM_NODE_IDX(PM_DEV_I2C_PMC)),
    PM_CLK2NODE(PM_NODE_IDX(PM_CLK_PMC_PL0_REF), PM_NODE_IDX(PM_DEV_PL_0)),
    PM_CLK2NODE(PM_NODE_IDX(PM_CLK_PMC_PL1_REF), PM_NODE_IDX(PM_DEV_PL_1)),
    PM_CLK2NODE(PM_NODE_IDX(PM_CLK_PMC_PL2_REF), PM_NODE_IDX(PM_DEV_PL_2)),
    PM_CLK2NODE(PM_NODE_IDX(PM_CLK_PMC_PL3_REF), PM_NODE_IDX(PM_DEV_PL_3)),
    PM_CLK2NODE(PM_NODE_IDX(PM_CLK_ACPU), PM_NODE_IDX(PM_DEV_ACPU_0)),
    PM_CLK2NODE(PM_NODE_IDX(PM_CLK_ACPU), PM_NODE_IDX(PM_DEV_ACPU_1)),
    PM_CLK2NODE(PM_NODE_IDX(PM_CLK_ADMA), PM_NODE_IDX(PM_DEV_ADMA_0)),
    PM_CLK2NODE(PM_NODE_IDX(PM_CLK_ADMA), PM_NODE_IDX(PM_DEV_ADMA_1)),
    PM_CLK2NODE(PM_NODE_IDX(PM_CLK_ADMA), PM_NODE_IDX(PM_DEV_ADMA_2)),
    PM_CLK2NODE(PM_NODE_IDX(PM_CLK_ADMA), PM_NODE_IDX(PM_DEV_ADMA_3)),
    PM_CLK2NODE(PM_NODE_IDX(PM_CLK_ADMA), PM_NODE_IDX(PM_DEV_ADMA_4)),
    PM_CLK2NODE(PM_NODE_IDX(PM_CLK_ADMA), PM_NODE_IDX(PM_DEV_ADMA_5)),
    PM_CLK2NODE(PM_NODE_IDX(PM_CLK_ADMA), PM_NODE_IDX(PM_DEV_ADMA_6)),
    PM_CLK2NODE(PM_NODE_IDX(PM_CLK_ADMA), PM_NODE_IDX(PM_DEV_ADMA_7)),
    PM_CLK2NODE(PM_NODE_IDX(PM_CLK_GEM0_REF), PM_NODE_IDX(PM_DEV_GEM_0)),
    PM_CLK2NODE(PM_NODE_IDX(PM_CLK_GEM1_REF), PM_NODE_IDX(PM_DEV_GEM_1)),
    PM_CLK2NODE(PM_NODE_IDX(PM_CLK_GEM_TSU_REF), PM_NODE_IDX(PM_DEV_GEM_0)),
    PM_CLK2NODE(PM_NODE_IDX(PM_CLK_GEM_TSU_REF), PM_NODE_IDX(PM_DEV_GEM_1)),
    PM_CLK2NODE(PM_NODE_IDX(PM_CLK_USB0_BUS_REF), PM_NODE_IDX(PM_DEV_USB_0)),
    PM_CLK2NODE(PM_NODE_IDX(PM_CLK_UART0_REF), PM_NODE_IDX(PM_DEV_UART_0)),
    PM_CLK2NODE(PM_NODE_IDX(PM_CLK_UART1_REF), PM_NODE_IDX(PM_DEV_UART_1)),
    PM_CLK2NODE(PM_NODE_IDX(PM_CLK_SPI0_REF), PM_NODE_IDX(PM_DEV_SPI_0)),
    PM_CLK2NODE(PM_NODE_IDX(PM_CLK_SPI1_REF), PM_NODE_IDX(PM_DEV_SPI_1)),
    PM_CLK2NODE(PM_NODE_IDX(PM_CLK_CAN0_REF), PM_NODE_IDX(PM_DEV_CAN_FD_0)),
    PM_CLK2NODE(PM_NODE_IDX(PM_CLK_CAN1_REF), PM_NODE_IDX(PM_DEV_CAN_FD_1)),
    PM_CLK2NODE(PM_NODE_IDX(PM_CLK_I2C0_REF), PM_NODE_IDX(PM_DEV_I2C_0)),
    PM_CLK2NODE(PM_NODE_IDX(PM_CLK_I2C1_REF), PM_NODE_IDX(PM_DEV_I2C_1)),
    PM_CLK2NODE(PM_NODE_IDX(PM_CLK_USB3_DUAL_REF), PM_NODE_IDX(PM_DEV_USB_0)),
};

/* Last clock node index */
#define PM_CLK_END_IDX  PM_NODE_IDX(PM_CLK_MIO)

#define PM_CLK_SBCL_MASK    (0x3F << 20)    /* Clock subclass mask */
#define PM_CLK_SBCL_PLL     (0x01 << 20)    /* PLL subclass value */

static bool pm_check_access(const struct pm_access *acl, struct domain *d, u32 idx)
{
    unsigned long mfn;

    if ( acl[idx].hwdom_access && is_hardware_domain(d) )
        return true;

    mfn = paddr_to_pfn(acl[idx].addr);
    if ( !mfn )
        return false;

    return iomem_access_permitted(d, mfn, mfn);
}

/* Check if a domain has access to a node.  */
static bool domain_has_node_access(struct domain *d, u32 node)
{
    u32 nd_idx = PM_NODE_IDX(node);

    if ( nd_idx >= ARRAY_SIZE(pm_node_access) )
        return false;

    return pm_check_access(pm_node_access, d, nd_idx);
}

/* Check if a domain has access to a reset line.  */
static bool domain_has_reset_access(struct domain *d, u32 rst)
{
    u32 rst_idx = PM_NODE_IDX(rst);

    if ( rst_idx >= ARRAY_SIZE(pm_rst_access) )
        return false;

    return pm_check_access(pm_rst_access, d, rst_idx);
}

/* Check if a clock id is valid */
static bool clock_id_is_valid(u32 clk_id)
{
    u32 clk_idx = PM_NODE_IDX(clk_id);

    if ( clk_idx > PM_CLK_END_IDX )
        return false;

    return true;
}

/* Check if a clock id belongs to pll type */
static bool clock_id_is_pll(u32 clk_id)
{
    if ( ( clk_id & PM_CLK_SBCL_MASK ) == PM_CLK_SBCL_PLL )
        return true;

    return false;
}

/*
 * Check if a domain has access to a clock control.
 * Note: domain has access to clock control if it has access to all the nodes
 * the are driven by the target clock.
 */
static bool domain_has_clock_access(struct domain *d, u32 clk_id)
{
    uint32_t i;
    bool access = false;
    clk_id = PM_NODE_IDX(clk_id);

    for ( i = 0; i < ARRAY_SIZE(pm_clk_node_map) &&
          pm_clk_node_map[i].clk_idx <= clk_id; i++ )
    {
        if ( pm_clk_node_map[i].clk_idx == clk_id )
        {
            if ( !domain_has_node_access(d, pm_clk_node_map[i].dev_idx) )
                return false;

            access = true;
        }
    }

    return access;
}

bool versal_eemi(struct cpu_user_regs *regs)
{
    struct arm_smccc_res res;
    uint32_t fid = get_user_reg(regs, 0);
    uint32_t nodeid = get_user_reg(regs, 1);
    unsigned int pm_fn = fid & 0xFFFF;
    enum pm_ret_status ret;

    switch (pm_fn)
    {
    /* Mandatory SMC32 functions. */
    case ARM_SMCCC_CALL_COUNT_FID(SIP):
    case ARM_SMCCC_CALL_UID_FID(SIP):
    case ARM_SMCCC_REVISION_FID(SIP):
        goto forward_to_fw;
    /*
     * We can't allow CPUs to suspend without Xen knowing about it.
     * We accept but ignore the request and wait for the guest to issue
     * a WFI which Xen will trap and act accordingly upon.
     */
    case EEMI_FID(PM_SELF_SUSPEND):
        ret = XST_PM_SUCCESS;
        goto done;

    case EEMI_FID(PM_GET_NODE_STATUS):
    /* API for PUs.  */
    case EEMI_FID(PM_REQ_SUSPEND):
    case EEMI_FID(PM_FORCE_POWERDOWN):
    case EEMI_FID(PM_ABORT_SUSPEND):
    case EEMI_FID(PM_REQ_WAKEUP):
    case EEMI_FID(PM_SET_WAKEUP_SOURCE):
    /* API for slaves.  */
    case EEMI_FID(PM_REQ_NODE):
    case EEMI_FID(PM_RELEASE_NODE):
    case EEMI_FID(PM_SET_REQUIREMENT):
    case EEMI_FID(PM_SET_MAX_LATENCY):
        if ( !domain_has_node_access(current->domain, nodeid) ) {
            printk("versal-pm: fn=%d No access to node %d\n", pm_fn, nodeid);
            ret = XST_PM_NO_ACCESS;
            goto done;
        }
        goto forward_to_fw;

    case EEMI_FID(PM_RESET_ASSERT):
    case EEMI_FID(PM_RESET_GET_STATUS):
        if ( !domain_has_reset_access(current->domain, nodeid) ) {
            printk("versal-pm: fn=%d No access to reset %d\n", pm_fn, nodeid);
            ret = XST_PM_NO_ACCESS;
            goto done;
        }
        goto forward_to_fw;

    /* These calls are safe and always allowed.  */
    case EEMI_FID(PM_GET_TRUSTZONE_VERSION):
    case EEMI_FID(PM_GET_API_VERSION):
    case EEMI_FID(PM_GET_CHIPID):
    case EEMI_FID(PM_FEATURE_CHECK):
        goto forward_to_fw;

    /* Mediated MMIO access.  */
    case EEMI_FID(PM_MMIO_WRITE):
    case EEMI_FID(PM_MMIO_READ):
        /* TBD */
        ret = XST_PM_NOTSUPPORTED;
        goto done;

    /* Exclusive to the hardware domain.  */
    case EEMI_FID(PM_INIT):
    case EEMI_FID(PM_SET_CONFIGURATION):
    case EEMI_FID(PM_FPGA_LOAD):
    case EEMI_FID(PM_FPGA_GET_STATUS):
    case EEMI_FID(PM_SECURE_SHA):
    case EEMI_FID(PM_SECURE_RSA):
    case EEMI_FID(PM_PINCTRL_SET_FUNCTION):
    case EEMI_FID(PM_PINCTRL_REQUEST):
    case EEMI_FID(PM_PINCTRL_RELEASE):
    case EEMI_FID(PM_PINCTRL_GET_FUNCTION):
    case EEMI_FID(PM_PINCTRL_CONFIG_PARAM_GET):
    case EEMI_FID(PM_PINCTRL_CONFIG_PARAM_SET):
    case EEMI_FID(PM_IOCTL):
    case EEMI_FID(PM_QUERY_DATA):
        if ( !is_hardware_domain(current->domain) ) {
            printk("eemi: fn=%d No access", pm_fn);
            ret = XST_PM_NO_ACCESS;
            goto done;
        }
        goto forward_to_fw;

    case EEMI_FID(PM_CLOCK_SETRATE):
    case EEMI_FID(PM_CLOCK_GETRATE):
        ret = XST_PM_NOTSUPPORTED;
        goto done;

    case EEMI_FID(PM_CLOCK_GETSTATE):
    case EEMI_FID(PM_CLOCK_GETDIVIDER):
    case EEMI_FID(PM_CLOCK_GETPARENT):
        if ( !clock_id_is_valid(nodeid) )
        {
            gprintk(XENLOG_WARNING, "versal-pm: fn=0x%08x Invalid clock=0x%08x\n",
                    pm_fn, nodeid);
            ret = XST_PM_INVALID_PARAM;
            goto done;
        }
        else
            goto forward_to_fw;

    case EEMI_FID(PM_CLOCK_ENABLE):
    case EEMI_FID(PM_CLOCK_DISABLE):
    case EEMI_FID(PM_CLOCK_SETDIVIDER):
    case EEMI_FID(PM_CLOCK_SETPARENT):
        if ( !clock_id_is_valid(nodeid) )
        {
            gprintk(XENLOG_WARNING, "versal-pm: fn=0x%08x Invalid clock=0x%08x\n",
                    pm_fn, nodeid);
            ret = XST_PM_INVALID_PARAM;
            goto done;
        }
        /*
         * Allow pll clock nodes to passthrough since there is no device binded to them
         */
        if ( clock_id_is_pll(nodeid) )
        {
            goto forward_to_fw;
        }
        if ( !domain_has_clock_access(current->domain, nodeid) )
        {
            gprintk(XENLOG_WARNING, "versal-pm: fn=0x%08x No access to clock=0x%08x\n",
                    pm_fn, nodeid);
            ret = XST_PM_NO_ACCESS;
            goto done;
        }
        goto forward_to_fw;

    case EEMI_FID(PM_PLL_GET_PARAMETER):
    case EEMI_FID(PM_PLL_GET_MODE):
        goto forward_to_fw;

    case EEMI_FID(PM_PLL_SET_PARAMETER):
    case EEMI_FID(PM_PLL_SET_MODE):
        if ( !domain_has_node_access(current->domain, nodeid) )
        {
            gprintk(XENLOG_WARNING, "versal-pm: fn=0x%08x No access to pll=0x%08x\n",
                    pm_fn, nodeid);
            ret = XST_PM_NO_ACCESS;
            goto done;
        }
        goto forward_to_fw;

    /* These calls are never allowed.  */
    case EEMI_FID(PM_SYSTEM_SHUTDOWN):
        ret = XST_PM_NO_ACCESS;
        goto done;

    case IPI_MAILBOX_FID(IPI_MAILBOX_OPEN):
    case IPI_MAILBOX_FID(IPI_MAILBOX_RELEASE):
    case IPI_MAILBOX_FID(IPI_MAILBOX_STATUS_ENQUIRY):
    case IPI_MAILBOX_FID(IPI_MAILBOX_NOTIFY):
    case IPI_MAILBOX_FID(IPI_MAILBOX_ACK):
    case IPI_MAILBOX_FID(IPI_MAILBOX_ENABLE_IRQ):
    case IPI_MAILBOX_FID(IPI_MAILBOX_DISABLE_IRQ):
        if ( !is_hardware_domain(current->domain) )
        {
            gprintk(XENLOG_WARNING, "IPI mailbox: fn=%u No access", pm_fn);
            ret = XST_PM_NO_ACCESS;
            goto done;
        }
        goto forward_to_fw;

    default:
        printk("versal-pm: Unhandled PM Call: %d\n", (u32)fid);
        return false;
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
