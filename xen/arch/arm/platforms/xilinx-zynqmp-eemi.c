/*
 * xen/arch/arm/platforms/xilinx-zynqmp-eemi.c
 *
 * Xilinx ZynqMP EEMI API mediator.
 *
 * Copyright (c) 2017 Xilinx Inc.
 * Written by Edgar E. Iglesias <edgar.iglesias@xilinx.com>
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

/*
 * Mediator for the EEMI Power Mangement API.
 *
 * Refs:
 * https://www.xilinx.com/support/documentation/user_guides/ug1200-eemi-api.pdf
 *
 * Background:
 * The ZynqMP has a subsystem named the PMU with a CPU and special devices
 * dedicated to running Power Management Firmware. Other masters in the
 * system need to send requests to the PMU in order to for example:
 * * Manage power state
 * * Configure clocks
 * * Program bitstreams for the programmable logic
 * * etc
 *
 * Allthough the details of the setup are configurable, in the common case
 * the PMU lives in the Secure world. NS World cannot directly communicate
 * with it and must use proxy services from ARM Trusted Firmware to reach
 * the PMU.
 *
 * Power Management on the ZynqMP is implemented in a layered manner.
 * The PMU knows about various masters and will enforce access controls
 * based on a pre-configured partitioning. This configuration dictates
 * which devices are owned by the various masters and the PMU FW makes sure
 * that a given master cannot turn off a device that it does not own or that
 * is in use by other masters.
 *
 * The PMU is not aware of multiple execution states in masters.
 * For example, it treats the ARMv8 cores as single units and does not
 * distinguish between Secure vs NS OS:s nor does it know about Hypervisors
 * and multiple guests. It is up to software on the ARMv8 cores to present
 * a unified view of its power requirements.
 *
 * To implement this unified view, ARM Trusted Firmware at EL3 provides
 * access to the PM API via SMC calls. ARM Trusted Firmware is responsible
 * for mediating between the Secure and the NS world, rejecting SMC calls
 * that request changes that are not allowed.
 *
 * Xen running above ATF owns the NS world and is responsible for presenting
 * unified PM requests taking all guests and the hypervisor into account.
 *
 * Implementation:
 * The PM API contains different classes of calls.
 * Certain calls are harmless to expose to any guest.
 * These include calls to get the PM API Version, or to read out the version
 * of the chip we're running on.
 *
 * Other calls are more problematic. Here're some:
 * * Power requests for nodes (i.e turn on or off a given device)
 * * Reset line control (i.e reset a given device)
 * * MMIO access (i.e directly access clock and reset registers)
 *
 * Power requests for nodes:
 * In order to correctly mediate these calls, we need to know if
 * guests issuing these calls have ownership of the given device.
 * The approach taken here is to map PM API Nodes identifying
 * a device into base addresses for registers that belong to that
 * same device.
 *
 * If the guest has access to a devices registers, we give the guest
 * access to PM API calls that affect that device. This is implemented
 * by pm_node_access and domain_has_node_access().
 *
 * Reset lines:
 * Reset lines are identified by a list of identifiers that do not relate
 * to device nodes. We can use the same approach here as for nodes, except
 * that we need a separate table mapping reset lines to base addresses.
 * This is implemented by pm_reset_access.
 *
 * MMIO access:
 * These calls allow guests to access certain memory ranges. These ranges
 * are typically protected for secure-world access only and also from
 * certain masters only, so guests cannot access them directly.
 * Registers within the memory regions affect certain nodes. In this case,
 * our imput is an address and we map that address into a node. If the
 * guest has ownership of that node, the access is allowed.
 * Some registers contain bitfields and a single register may contain
 * bits that affect multiple nodes.
 *
 * Some of the calls act on more global state. For example acting on
 * NODE_PS, which affects many a lot of nodes. This higher level
 * orchestrating is left for the hardware-domain only.
 */

#include <xen/iocap.h>
#include <asm/platform.h>
#include <asm/platforms/xilinx-zynqmp-eemi.h>
#include <asm/platforms/xilinx-zynqmp-mm.h>

struct pm_access
{
    uint64_t addr;
    bool hwdom_access;    /* HW domain gets access regardless.  */
};

/*
 * This table maps a node into a memory address.
 * If a guest has access to the address, it has enough control
 * over the node to grant it access to EEMI calls for that node.
 */
static const struct pm_access pm_node_access[] = {
    /* MM_RPU grants access to alll RPU Nodes.  */
    [NODE_RPU] = { MM_RPU },
    [NODE_RPU_0] = { MM_RPU },
    [NODE_RPU_1] = { MM_RPU },
    [NODE_IPI_RPU_0] = { MM_RPU },

    /* GPU nodes.  */
    [NODE_GPU] = { MM_GPU },
    [NODE_GPU_PP_0] = { MM_GPU },
    [NODE_GPU_PP_1] = { MM_GPU },

    [NODE_USB_0] = { MM_USB3_0_XHCI },
    [NODE_USB_1] = { MM_USB3_1_XHCI },
    [NODE_TTC_0] = { MM_TTC0 },
    [NODE_TTC_1] = { MM_TTC1 },
    [NODE_TTC_2] = { MM_TTC2 },
    [NODE_TTC_3] = { MM_TTC3 },
    [NODE_SATA] = { MM_SATA_AHCI_HBA },
    [NODE_ETH_0] = { MM_GEM0 },
    [NODE_ETH_1] = { MM_GEM1 },
    [NODE_ETH_2] = { MM_GEM2 },
    [NODE_ETH_3] = { MM_GEM3 },
    [NODE_UART_0] = { MM_UART0 },
    [NODE_UART_1] = { MM_UART1 },
    [NODE_SPI_0] = { MM_SPI0 },
    [NODE_SPI_1] = { MM_SPI1 },
    [NODE_I2C_0] = { MM_I2C0 },
    [NODE_I2C_1] = { MM_I2C1 },
    [NODE_SD_0] = { MM_SD0 },
    [NODE_SD_1] = { MM_SD1 },
    [NODE_DP] = { MM_DP },

    /* Guest with GDMA Channel 0 gets PM access. Other guests don't.  */
    [NODE_GDMA] = { MM_GDMA_CH0 },
    /* Guest with ADMA Channel 0 gets PM access. Other guests don't.  */
    [NODE_ADMA] = { MM_ADMA_CH0 },

    [NODE_NAND] = { MM_NAND },
    [NODE_QSPI] = { MM_QSPI },
    [NODE_GPIO] = { MM_GPIO },
    [NODE_CAN_0] = { MM_CAN0 },
    [NODE_CAN_1] = { MM_CAN1 },

    /* Only for the hardware domain.  */
    [NODE_AFI] = { .hwdom_access = true },
    [NODE_APLL] = { .hwdom_access = true },
    [NODE_VPLL] = { .hwdom_access = true },
    [NODE_DPLL] = { .hwdom_access = true },
    [NODE_RPLL] = { .hwdom_access = true },
    [NODE_IOPLL] = { .hwdom_access = true },
    [NODE_DDR] = { .hwdom_access = true },
    [NODE_IPI_APU] = { .hwdom_access = true },
    [NODE_PCAP] = { .hwdom_access = true },

    [NODE_PCIE] = { MM_PCIE_ATTRIB },
    [NODE_RTC] = { MM_RTC },
};

/*
 * This table maps reset line IDs into a memory address.
 * If a guest has access to the address, it has enough control
 * over the affected node to grant it access to EEMI calls for
 * resetting that node.
 */
#define PM_RESET_IDX(n) (n - PM_RESET_PCIE_CFG)
static const struct pm_access pm_reset_access[] = {
    [PM_RESET_IDX(PM_RESET_PCIE_CFG)] = { MM_AXIPCIE_MAIN },
    [PM_RESET_IDX(PM_RESET_PCIE_BRIDGE)] = { MM_PCIE_ATTRIB },
    [PM_RESET_IDX(PM_RESET_PCIE_CTRL)] = { MM_PCIE_ATTRIB },

    [PM_RESET_IDX(PM_RESET_DP)] = { MM_DP },
    [PM_RESET_IDX(PM_RESET_SWDT_CRF)] = { MM_SWDT },
    [PM_RESET_IDX(PM_RESET_AFI_FM5)] = { .hwdom_access = true },
    [PM_RESET_IDX(PM_RESET_AFI_FM4)] = { .hwdom_access = true },
    [PM_RESET_IDX(PM_RESET_AFI_FM3)] = { .hwdom_access = true },
    [PM_RESET_IDX(PM_RESET_AFI_FM2)] = { .hwdom_access = true },
    [PM_RESET_IDX(PM_RESET_AFI_FM1)] = { .hwdom_access = true },
    [PM_RESET_IDX(PM_RESET_AFI_FM0)] = { .hwdom_access = true },

    /* Channel 0 grants PM access.  */
    [PM_RESET_IDX(PM_RESET_GDMA)] = { MM_GDMA_CH0 },
    [PM_RESET_IDX(PM_RESET_GPU_PP1)] = { MM_GPU },
    [PM_RESET_IDX(PM_RESET_GPU_PP0)] = { MM_GPU },
    [PM_RESET_IDX(PM_RESET_GT)] = { .hwdom_access = true },
    [PM_RESET_IDX(PM_RESET_SATA)] = { MM_SATA_AHCI_HBA },

    /* We don't allow anyone to turn on/off the ACPUs.  */
    [PM_RESET_IDX(PM_RESET_ACPU3_PWRON)] = { 0 },
    [PM_RESET_IDX(PM_RESET_ACPU2_PWRON)] = { 0 },
    [PM_RESET_IDX(PM_RESET_ACPU1_PWRON)] = { 0 },
    [PM_RESET_IDX(PM_RESET_ACPU0_PWRON)] = { 0 },
    [PM_RESET_IDX(PM_RESET_APU_L2)] = { 0 },
    [PM_RESET_IDX(PM_RESET_ACPU3)] = { 0 },
    [PM_RESET_IDX(PM_RESET_ACPU2)] = { 0 },
    [PM_RESET_IDX(PM_RESET_ACPU1)] = { 0 },
    [PM_RESET_IDX(PM_RESET_ACPU0)] = { 0 },

    [PM_RESET_IDX(PM_RESET_DDR)] = { 0 },

    [PM_RESET_IDX(PM_RESET_APM_FPD)] = { .hwdom_access = true },
    [PM_RESET_IDX(PM_RESET_SOFT)] = { .hwdom_access = true },

    [PM_RESET_IDX(PM_RESET_GEM0)] = { MM_GEM0 },
    [PM_RESET_IDX(PM_RESET_GEM1)] = { MM_GEM1 },
    [PM_RESET_IDX(PM_RESET_GEM2)] = { MM_GEM2 },
    [PM_RESET_IDX(PM_RESET_GEM3)] = { MM_GEM3 },

    [PM_RESET_IDX(PM_RESET_QSPI)] = { MM_QSPI },
    [PM_RESET_IDX(PM_RESET_UART0)] = { MM_UART0 },
    [PM_RESET_IDX(PM_RESET_UART1)] = { MM_UART1 },
    [PM_RESET_IDX(PM_RESET_SPI0)] = { MM_SPI0 },
    [PM_RESET_IDX(PM_RESET_SPI1)] = { MM_SPI1 },
    [PM_RESET_IDX(PM_RESET_SDIO0)] = { MM_SD0 },
    [PM_RESET_IDX(PM_RESET_SDIO1)] = { MM_SD1 },
    [PM_RESET_IDX(PM_RESET_CAN0)] = { MM_CAN0 },
    [PM_RESET_IDX(PM_RESET_CAN1)] = { MM_CAN1 },
    [PM_RESET_IDX(PM_RESET_I2C0)] = { MM_I2C0 },
    [PM_RESET_IDX(PM_RESET_I2C1)] = { MM_I2C1 },
    [PM_RESET_IDX(PM_RESET_TTC0)] = { MM_TTC0 },
    [PM_RESET_IDX(PM_RESET_TTC1)] = { MM_TTC1 },
    [PM_RESET_IDX(PM_RESET_TTC2)] = { MM_TTC2 },
    [PM_RESET_IDX(PM_RESET_TTC3)] = { MM_TTC3 },
    [PM_RESET_IDX(PM_RESET_SWDT_CRL)] = { MM_SWDT },
    [PM_RESET_IDX(PM_RESET_NAND)] = { MM_NAND },
    /* ADMA Channel 0 grants access to pull the reset signal.  */
    [PM_RESET_IDX(PM_RESET_ADMA)] = { MM_ADMA_CH0 },
    [PM_RESET_IDX(PM_RESET_GPIO)] = { MM_GPIO },
    /* FIXME: What is this?  */
    [PM_RESET_IDX(PM_RESET_IOU_CC)] = { .hwdom_access = true },
    [PM_RESET_IDX(PM_RESET_TIMESTAMP)] = { .hwdom_access = true },
    [PM_RESET_IDX(PM_RESET_RPU_R50)] = { MM_RPU },
    [PM_RESET_IDX(PM_RESET_RPU_R51)] = { MM_RPU },
    [PM_RESET_IDX(PM_RESET_RPU_AMBA)] = { MM_RPU },
    [PM_RESET_IDX(PM_RESET_OCM)] = { .hwdom_access = true },
    [PM_RESET_IDX(PM_RESET_RPU_PGE)] = { MM_RPU },

    [PM_RESET_IDX(PM_RESET_USB0_CORERESET)] = { MM_USB3_0_XHCI },
    [PM_RESET_IDX(PM_RESET_USB0_HIBERRESET)] = { MM_USB3_0_XHCI },
    [PM_RESET_IDX(PM_RESET_USB0_APB)] = { MM_USB3_0_XHCI },

    [PM_RESET_IDX(PM_RESET_USB1_CORERESET)] = { MM_USB3_1_XHCI },
    [PM_RESET_IDX(PM_RESET_USB1_HIBERRESET)] = { MM_USB3_1_XHCI },
    [PM_RESET_IDX(PM_RESET_USB1_APB)] = { MM_USB3_1_XHCI },

    [PM_RESET_IDX(PM_RESET_IPI)] = { .hwdom_access = true },
    [PM_RESET_IDX(PM_RESET_APM_LPD)] = { .hwdom_access = true },
    [PM_RESET_IDX(PM_RESET_RTC)] = { MM_RTC },
    [PM_RESET_IDX(PM_RESET_SYSMON)] = { .hwdom_access = true },
    [PM_RESET_IDX(PM_RESET_AFI_FM6)] = { .hwdom_access = true },
    [PM_RESET_IDX(PM_RESET_LPD_SWDT)] = { MM_SWDT },
    [PM_RESET_IDX(PM_RESET_FPD)] = { .hwdom_access = true },
    [PM_RESET_IDX(PM_RESET_RPU_DBG1)] = { MM_RPU },
    [PM_RESET_IDX(PM_RESET_RPU_DBG0)] = { MM_RPU },
    [PM_RESET_IDX(PM_RESET_DBG_LPD)] = { .hwdom_access = true },
    [PM_RESET_IDX(PM_RESET_DBG_FPD)] = { .hwdom_access = true },
    [PM_RESET_IDX(PM_RESET_APLL)] = { .hwdom_access = true },
    [PM_RESET_IDX(PM_RESET_DPLL)] = { .hwdom_access = true },
    [PM_RESET_IDX(PM_RESET_VPLL)] = { .hwdom_access = true },
    [PM_RESET_IDX(PM_RESET_IOPLL)] = { .hwdom_access = true },
    [PM_RESET_IDX(PM_RESET_RPLL)] = { .hwdom_access = true },
    [PM_RESET_IDX(PM_RESET_GPO3_PL_0)] = { .hwdom_access = true },
    [PM_RESET_IDX(PM_RESET_GPO3_PL_1)] = { .hwdom_access = true },
    [PM_RESET_IDX(PM_RESET_GPO3_PL_2)] = { .hwdom_access = true },
    [PM_RESET_IDX(PM_RESET_GPO3_PL_3)] = { .hwdom_access = true },
    [PM_RESET_IDX(PM_RESET_GPO3_PL_4)] = { .hwdom_access = true },
    [PM_RESET_IDX(PM_RESET_GPO3_PL_5)] = { .hwdom_access = true },
    [PM_RESET_IDX(PM_RESET_GPO3_PL_6)] = { .hwdom_access = true },
    [PM_RESET_IDX(PM_RESET_GPO3_PL_7)] = { .hwdom_access = true },
    [PM_RESET_IDX(PM_RESET_GPO3_PL_8)] = { .hwdom_access = true },
    [PM_RESET_IDX(PM_RESET_GPO3_PL_9)] = { .hwdom_access = true },
    [PM_RESET_IDX(PM_RESET_GPO3_PL_10)] = { .hwdom_access = true },
    [PM_RESET_IDX(PM_RESET_GPO3_PL_11)] = { .hwdom_access = true },
    [PM_RESET_IDX(PM_RESET_GPO3_PL_12)] = { .hwdom_access = true },
    [PM_RESET_IDX(PM_RESET_GPO3_PL_13)] = { .hwdom_access = true },
    [PM_RESET_IDX(PM_RESET_GPO3_PL_14)] = { .hwdom_access = true },
    [PM_RESET_IDX(PM_RESET_GPO3_PL_15)] = { .hwdom_access = true },
    [PM_RESET_IDX(PM_RESET_GPO3_PL_16)] = { .hwdom_access = true },
    [PM_RESET_IDX(PM_RESET_GPO3_PL_17)] = { .hwdom_access = true },
    [PM_RESET_IDX(PM_RESET_GPO3_PL_18)] = { .hwdom_access = true },
    [PM_RESET_IDX(PM_RESET_GPO3_PL_19)] = { .hwdom_access = true },
    [PM_RESET_IDX(PM_RESET_GPO3_PL_20)] = { .hwdom_access = true },
    [PM_RESET_IDX(PM_RESET_GPO3_PL_21)] = { .hwdom_access = true },
    [PM_RESET_IDX(PM_RESET_GPO3_PL_22)] = { .hwdom_access = true },
    [PM_RESET_IDX(PM_RESET_GPO3_PL_23)] = { .hwdom_access = true },
    [PM_RESET_IDX(PM_RESET_GPO3_PL_24)] = { .hwdom_access = true },
    [PM_RESET_IDX(PM_RESET_GPO3_PL_25)] = { .hwdom_access = true },
    [PM_RESET_IDX(PM_RESET_GPO3_PL_26)] = { .hwdom_access = true },
    [PM_RESET_IDX(PM_RESET_GPO3_PL_27)] = { .hwdom_access = true },
    [PM_RESET_IDX(PM_RESET_GPO3_PL_28)] = { .hwdom_access = true },
    [PM_RESET_IDX(PM_RESET_GPO3_PL_29)] = { .hwdom_access = true },
    [PM_RESET_IDX(PM_RESET_GPO3_PL_30)] = { .hwdom_access = true },
    [PM_RESET_IDX(PM_RESET_GPO3_PL_31)] = { .hwdom_access = true },
    [PM_RESET_IDX(PM_RESET_RPU_LS)] = { MM_RPU },
    [PM_RESET_IDX(PM_RESET_PS_ONLY)] = { .hwdom_access = true },
    [PM_RESET_IDX(PM_RESET_PL)] = { .hwdom_access = true },
};

/*
 * This table maps reset line IDs into a memory address.
 * If a guest has access to the address, it has enough control
 * over the affected node to grant it access to EEMI calls for
 * resetting that node.
 */
static const struct {
    uint64_t start;
    uint64_t end;    /* Inclusive. If not set, single reg entry.  */
    uint32_t mask;   /* Zero means no mask, i.e all bits.  */
    enum pm_node_id node;
    bool hwdom_access;
    bool readonly;
} pm_mmio_access[] = {
    {
        .start = MM_CRF_APB + R_CRF_APLL_CTRL,
        .end = MM_CRF_APB + R_CRF_ACPU_CTRL,
        .hwdom_access = true
    },
    {
        .start = MM_CRL_APB + R_CRL_IOPLL_CTRL,
        .end = MM_CRL_APB + R_CRL_RPLL_TO_FPD_CTRL,
        .hwdom_access = true
    },
    { .start = MM_CRF_APB + R_CRF_DP_VIDEO_REF_CTRL, .node = NODE_DP },
    { .start = MM_CRF_APB + R_CRF_DP_STC_REF_CTRL, .node = NODE_DP },
    { .start = MM_CRF_APB + R_CRF_GPU_REF_CTRL, .node = NODE_GPU },
    { .start = MM_CRF_APB + R_CRF_SATA_REF_CTRL, .node = NODE_SATA },
    { .start = MM_CRF_APB + R_CRF_PCIE_REF_CTRL, .node = NODE_PCIE },
    { .start = MM_CRF_APB + R_CRF_GDMA_REF_CTRL, .node = NODE_GDMA },
    { .start = MM_CRF_APB + R_CRF_DPDMA_REF_CTRL, .node = NODE_DP },
    { .start = MM_CRL_APB + R_CRL_USB3_DUAL_REF_CTRL, .node = NODE_USB_0 },
    { .start = MM_CRL_APB + R_CRL_USB0_BUS_REF_CTRL, .node = NODE_USB_0 },
    { .start = MM_CRL_APB + R_CRL_USB1_BUS_REF_CTRL, .node = NODE_USB_1 },
    { .start = MM_CRL_APB + R_CRL_USB1_BUS_REF_CTRL, .node = NODE_USB_1 },
    { .start = MM_CRL_APB + R_CRL_GEM0_REF_CTRL, .node = NODE_ETH_0 },
    { .start = MM_CRL_APB + R_CRL_GEM1_REF_CTRL, .node = NODE_ETH_1 },
    { .start = MM_CRL_APB + R_CRL_GEM2_REF_CTRL, .node = NODE_ETH_2 },
    { .start = MM_CRL_APB + R_CRL_GEM3_REF_CTRL, .node = NODE_ETH_3 },
    { .start = MM_CRL_APB + R_CRL_QSPI_REF_CTRL, .node = NODE_QSPI },
    { .start = MM_CRL_APB + R_CRL_SDIO0_REF_CTRL, .node = NODE_SD_0 },
    { .start = MM_CRL_APB + R_CRL_SDIO1_REF_CTRL, .node = NODE_SD_1 },
    { .start = MM_CRL_APB + R_CRL_UART0_REF_CTRL, .node = NODE_UART_0 },
    { .start = MM_CRL_APB + R_CRL_UART1_REF_CTRL, .node = NODE_UART_1 },
    { .start = MM_CRL_APB + R_CRL_SPI0_REF_CTRL, .node = NODE_SPI_0 },
    { .start = MM_CRL_APB + R_CRL_SPI1_REF_CTRL, .node = NODE_SPI_1 },
    { .start = MM_CRL_APB + R_CRL_CAN0_REF_CTRL, .node = NODE_CAN_0 },
    { .start = MM_CRL_APB + R_CRL_CAN1_REF_CTRL, .node = NODE_CAN_1 },
    { .start = MM_CRL_APB + R_CRL_CPU_R5_CTRL, .node = NODE_RPU },
    { .start = MM_CRL_APB + R_CRL_IOU_SWITCH_CTRL, .hwdom_access = true },
    { .start = MM_CRL_APB + R_CRL_CSU_PLL_CTRL, .hwdom_access = true },
    { .start = MM_CRL_APB + R_CRL_PCAP_CTRL, .hwdom_access = true },
    { .start = MM_CRL_APB + R_CRL_LPD_SWITCH_CTRL, .hwdom_access = true },
    { .start = MM_CRL_APB + R_CRL_LPD_LSBUS_CTRL, .hwdom_access = true },
    { .start = MM_CRL_APB + R_CRL_DBG_LPD_CTRL, .hwdom_access = true },
    { .start = MM_CRL_APB + R_CRL_NAND_REF_CTRL, .node = NODE_NAND },
    { .start = MM_CRL_APB + R_CRL_ADMA_REF_CTRL, .node = NODE_ADMA },
    { .start = MM_CRL_APB + R_CRL_PL0_REF_CTRL, .hwdom_access = true },
    { .start = MM_CRL_APB + R_CRL_PL1_REF_CTRL, .hwdom_access = true },
    { .start = MM_CRL_APB + R_CRL_PL2_REF_CTRL, .hwdom_access = true },
    { .start = MM_CRL_APB + R_CRL_PL3_REF_CTRL, .hwdom_access = true },
    { .start = MM_CRL_APB + R_CRL_PL0_THR_CTRL, .hwdom_access = true },
    { .start = MM_CRL_APB + R_CRL_PL1_THR_CTRL, .hwdom_access = true },
    { .start = MM_CRL_APB + R_CRL_PL2_THR_CTRL, .hwdom_access = true },
    { .start = MM_CRL_APB + R_CRL_PL3_THR_CTRL, .hwdom_access = true },
    { .start = MM_CRL_APB + R_CRL_PL0_THR_CNT, .hwdom_access = true },
    { .start = MM_CRL_APB + R_CRL_PL1_THR_CNT, .hwdom_access = true },
    { .start = MM_CRL_APB + R_CRL_PL2_THR_CNT, .hwdom_access = true },
    { .start = MM_CRL_APB + R_CRL_PL3_THR_CNT, .hwdom_access = true },
    { .start = MM_CRL_APB + R_CRL_GEM_TSU_REF_CTRL, .node = NODE_ETH_0 },
    { .start = MM_CRL_APB + R_CRL_DLL_REF_CTRL, .hwdom_access = true },
    { .start = MM_CRL_APB + R_CRL_AMS_REF_CTRL, .hwdom_access = true },
    { .start = MM_CRL_APB + R_CRL_I2C0_REF_CTRL, .node = NODE_I2C_0 },
    { .start = MM_CRL_APB + R_CRL_I2C1_REF_CTRL, .node = NODE_I2C_1 },
    { .start = MM_CRL_APB + R_CRL_TIMESTAMP_REF_CTRL, .hwdom_access = true },
    /* MIOs are controlled by the hardware domain.  */
    {
        .start = MM_IOU_SLCR + R_IOU_SLCR_MIO_PIN_0,
        .end = MM_IOU_SLCR + R_IOU_SLCR_MIO_MST_TRI2,
        .hwdom_access = true
    },
    { .start = MM_IOU_SLCR + R_IOU_SLCR_WDT_CLK_SEL, .hwdom_access = true },
    {
        .start = MM_IOU_SLCR + R_IOU_SLCR_CAN_MIO_CTRL,
        .mask = 0x1ff, .node = NODE_CAN_0
    },
    {
        .start = MM_IOU_SLCR + R_IOU_SLCR_CAN_MIO_CTRL,
        .mask = 0x1ff << 15, .node = NODE_CAN_1
    },
    {
        .start = MM_IOU_SLCR + R_IOU_SLCR_GEM_CLK_CTRL,
        .mask = 0xf, .node = NODE_ETH_0
    },
    {
        .start = MM_IOU_SLCR + R_IOU_SLCR_GEM_CLK_CTRL,
        .mask = 0xf << 5, .node = NODE_ETH_1
    },
    {
        .start = MM_IOU_SLCR + R_IOU_SLCR_GEM_CLK_CTRL,
        .mask = 0xf << 10, .node = NODE_ETH_2
    },
    {
        .start = MM_IOU_SLCR + R_IOU_SLCR_GEM_CLK_CTRL,
        .mask = 0xf << 15, .node = NODE_ETH_3
    },
    {
        .start = MM_IOU_SLCR + R_IOU_SLCR_GEM_CLK_CTRL,
        .mask = 0x7 << 20, .hwdom_access = true
    },
    {
        .start = MM_IOU_SLCR + R_IOU_SLCR_SDIO_CLK_CTRL,
        .mask = 0x7, .node = NODE_SD_0
    },
    {
        .start = MM_IOU_SLCR + R_IOU_SLCR_SDIO_CLK_CTRL,
        .mask = 0x7 << 17, .node = NODE_SD_1
    },

    {
        .start = MM_IOU_SLCR + R_IOU_SLCR_CTRL_REG_SD,
        .mask = 0x1, .node = NODE_SD_0
    },
    {
        .start = MM_IOU_SLCR + R_IOU_SLCR_CTRL_REG_SD,
        .mask = 0x1 << 15, .node = NODE_SD_1
    },
    /* A series of SD regs with the same layout.  */
    {
        .start = MM_IOU_SLCR + R_IOU_SLCR_SD_ITAPDLY,
        .end = MM_IOU_SLCR + R_IOU_SLCR_SD_CDN_CTRL,
        .mask = 0x3ff << 0, .node = NODE_SD_0
    },
    {
        .start = MM_IOU_SLCR + R_IOU_SLCR_SD_ITAPDLY,
        .end = MM_IOU_SLCR + R_IOU_SLCR_SD_CDN_CTRL,
        .mask = 0x3ff << 16, .node = NODE_SD_1
    },
    {
        .start = MM_IOU_SLCR + R_IOU_SLCR_GEM_CTRL,
        .mask = 0x3 << 0, .node = NODE_ETH_0
    },
    {
        .start = MM_IOU_SLCR + R_IOU_SLCR_GEM_CTRL,
        .mask = 0x3 << 2, .node = NODE_ETH_1
    },
    {
        .start = MM_IOU_SLCR + R_IOU_SLCR_GEM_CTRL,
        .mask = 0x3 << 4, .node = NODE_ETH_2
    },
    {
        .start = MM_IOU_SLCR + R_IOU_SLCR_GEM_CTRL,
        .mask = 0x3 << 6, .node = NODE_ETH_3
    },
    {
        .start = MM_IOU_SLCR + R_IOU_SLCR_IOU_TTC_APB_CLK,
        .mask = 0x3 << 0, .node = NODE_TTC_0
    },
    {
        .start = MM_IOU_SLCR + R_IOU_SLCR_IOU_TTC_APB_CLK,
        .mask = 0x3 << 2, .node = NODE_TTC_1
    },
    {
        .start = MM_IOU_SLCR + R_IOU_SLCR_IOU_TTC_APB_CLK,
        .mask = 0x3 << 4, .node = NODE_TTC_2
    },
    {
        .start = MM_IOU_SLCR + R_IOU_SLCR_IOU_TTC_APB_CLK,
        .mask = 0x3 << 6, .node = NODE_TTC_3
    },

    {
        .start = MM_IOU_SLCR + R_IOU_SLCR_IOU_TAPDLY_BYPASS,
        .mask = 0x3, .node = NODE_NAND
    },
    {
        .start = MM_IOU_SLCR + R_IOU_SLCR_IOU_TAPDLY_BYPASS,
        .mask = 0x1 << 2, .node = NODE_QSPI
    },
    {
        .start = MM_IOU_SLCR + R_IOU_SLCR_IOU_COHERENT_CTRL,
        .mask = 0xf << 0, .node = NODE_ETH_0
    },
    {
        .start = MM_IOU_SLCR + R_IOU_SLCR_IOU_COHERENT_CTRL,
        .mask = 0xf << 4, .node = NODE_ETH_1
    },
    {
        .start = MM_IOU_SLCR + R_IOU_SLCR_IOU_COHERENT_CTRL,
        .mask = 0xf << 8, .node = NODE_ETH_2
    },
    {
        .start = MM_IOU_SLCR + R_IOU_SLCR_IOU_COHERENT_CTRL,
        .mask = 0xf << 12, .node = NODE_ETH_3
    },
    {
        .start = MM_IOU_SLCR + R_IOU_SLCR_IOU_COHERENT_CTRL,
        .mask = 0xf << 16, .node = NODE_SD_0
    },
    {
        .start = MM_IOU_SLCR + R_IOU_SLCR_IOU_COHERENT_CTRL,
        .mask = 0xf << 20, .node = NODE_SD_1
    },
    {
        .start = MM_IOU_SLCR + R_IOU_SLCR_IOU_COHERENT_CTRL,
        .mask = 0xf << 24, .node = NODE_NAND
    },
    {
        .start = MM_IOU_SLCR + R_IOU_SLCR_IOU_COHERENT_CTRL,
        .mask = 0xf << 28, .node = NODE_QSPI
    },
    { .start = MM_IOU_SLCR + R_IOU_SLCR_VIDEO_PSS_CLK_SEL, .hwdom_access = true },
    /* No access to R_IOU_SLCR_IOU_INTERCONNECT_ROUTE at all.  */
    { .start = MM_IOU_SLCR + R_IOU_SLCR_IOU_RAM_GEM0, .node = NODE_ETH_0 },
    { .start = MM_IOU_SLCR + R_IOU_SLCR_IOU_RAM_GEM1, .node = NODE_ETH_1 },
    { .start = MM_IOU_SLCR + R_IOU_SLCR_IOU_RAM_GEM2, .node = NODE_ETH_2 },
    { .start = MM_IOU_SLCR + R_IOU_SLCR_IOU_RAM_GEM3, .node = NODE_ETH_3 },
    { .start = MM_IOU_SLCR + R_IOU_SLCR_IOU_RAM_SD0, .node = NODE_SD_0 },
    { .start = MM_IOU_SLCR + R_IOU_SLCR_IOU_RAM_SD1, .node = NODE_SD_1 },
    { .start = MM_IOU_SLCR + R_IOU_SLCR_IOU_RAM_CAN0, .node = NODE_CAN_0 },
    { .start = MM_IOU_SLCR + R_IOU_SLCR_IOU_RAM_CAN1, .node = NODE_CAN_1 },
    { .start = MM_IOU_SLCR + R_IOU_SLCR_IOU_RAM_LQSPI, .node = NODE_QSPI },
    { .start = MM_IOU_SLCR + R_IOU_SLCR_IOU_RAM_NAND, .node = NODE_NAND },
    {
        .start = MM_PMU_GLOBAL + R_PMU_GLOBAL_PWR_STATE,
        .readonly = true,
    },
    {
        .start = MM_PMU_GLOBAL + R_PMU_GLOBAL_GLOBAL_GEN_STORAGE0,
        .end = MM_PMU_GLOBAL + R_PMU_GLOBAL_PERS_GLOB_GEN_STORAGE7,
        .readonly = true,
    },
    {
        /* Universal read-only access to CRF. Linux CCF needs this.  */
        .start = MM_CRF_APB, .end = MM_CRF_APB + 0x104,
        .readonly = true,
    },
    {
        /* Universal read-only access to CRL. Linux CCF needs this.  */
        .start = MM_CRL_APB, .end = MM_CRL_APB + 0x284,
        .readonly = true,
    }
};

static bool pm_check_access(const struct pm_access *acl, struct domain *d, int idx)
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
static bool domain_has_node_access(struct domain *d, enum pm_node_id node)
{
    if ( node < 0 || node > ARRAY_SIZE(pm_node_access) )
        return false;

    /* NODE_UNKNOWN is treated as a wildcard.  */
    if ( node == NODE_UNKNOWN )
        return true;

    return pm_check_access(pm_node_access, d, node);
}

/* Check if a domain has access to a reset line.  */
static bool domain_has_reset_access(struct domain *d, enum pm_reset rst)
{
    int rst_idx = PM_RESET_IDX(rst);

    if ( rst_idx < 0 || rst_idx > ARRAY_SIZE(pm_reset_access) )
        return false;

    return pm_check_access(pm_reset_access, d, rst_idx);
}

/*
 * Check if a given domain has access to perform an indirect
 * MMIO access.
 *
 * If the provided mask is invalid, it will be fixed up.
 */
static bool domain_mediate_mmio_access(struct domain *d,
                                       bool write, uint64_t addr,
                                       uint32_t *mask)
{
    unsigned int i;
    bool ret = false;
    uint32_t prot_mask = 0;

    ASSERT(mask);

    /*
     * The hardware domain gets read access to everything.
     * Lower layers will do further filtering.
     */
    if ( !write && is_hardware_domain(d) )
        return true;

    /* Scan the ACL.  */
    for ( i = 0; i < ARRAY_SIZE(pm_mmio_access); i++ ) {
        bool r;

        /* Check if the address is OK.  */
        if ( pm_mmio_access[i].end ) {
            /* Memory range.  */
            if ( addr < pm_mmio_access[i].start )
                continue;
            if ( addr > pm_mmio_access[i].end )
                continue;
        } else {
            /* Single register.  */
            if ( addr != pm_mmio_access[i].start )
                continue;
        }

        if ( write && pm_mmio_access[i].readonly )
            continue;
        if ( pm_mmio_access[i].hwdom_access && !is_hardware_domain(d) )
            continue;

        /* Unlimited access is represented by a zero mask.  */
        ASSERT( pm_mmio_access[i].mask != 0xFFFFFFFF );

        r = domain_has_node_access(d, pm_mmio_access[i].node);
        if ( r ) {
            /* We've got access to this reg (or parts of it).  */
            ret = true;
            /* Masking only applies to writes, so reads can exit here.  */
            if ( !write )
                break;

            /* Permit write access to selected bits.  */
            prot_mask |= pm_mmio_access[i].mask ? pm_mmio_access[i].mask : ~0;
            if ( prot_mask == 0xFFFFFFFF )
                break; /* Full access, we're done.  */
        } else {
            if ( !pm_mmio_access[i].mask ) {
                /*
                 * The entire reg is tied to a device that we don't have
                 * access to. No point in continuing.
                 */
                return false;
            }
        }
    }

    /* Masking only applies to writes.  */
    if ( write )
        *mask &= prot_mask;
    return ret;
}

bool zynqmp_eemi_mediate(register_t fid,
                         register_t a0,
                         register_t a1,
                         register_t a2,
                         register_t a3,
                         register_t a4,
                         register_t a5,
                         register_t *ret)
{
    bool is_mmio_write = false;
    unsigned int pm_fn = fid & 0xFFFF;
    uint32_t pm_arg[4];

    /* Decode pm args.  */
    pm_arg[0] = a0;
    pm_arg[1] = a0 >> 32;
    pm_arg[2] = a1;
    pm_arg[3] = a1 >> 32;

    ASSERT(ret);

    switch (pm_fn)
    {
    /*
     * We can't allow CPUs to suspend without Xen knowing about it.
     * We accept but ignore the request and wait for the guest to issue
     * a WFI which Xen will trap and act accordingly upon.
     */
    case PM_SELF_SUSPEND:
        ret[0] = PM_RET_SUCCESS;
        goto done;

    case PM_GET_NODE_STATUS:
    /* API for PUs.  */
    case PM_REQ_SUSPEND:
    case PM_FORCE_POWERDOWN:
    case PM_ABORT_SUSPEND:
    case PM_REQ_WAKEUP:
    case PM_SET_WAKEUP_SOURCE:
    /* API for slaves.  */
    case PM_REQ_NODE:
    case PM_RELEASE_NODE:
    case PM_SET_REQUIREMENT:
    case PM_SET_MAX_LATENCY:
        if ( !domain_has_node_access(current->domain, pm_arg[0]) ) {
            printk("zynqmp-pm: fn=%d No access to node %d\n", pm_fn, pm_arg[0]);
            ret[0] = PM_RET_ERROR_ACCESS;
            goto done;
        }
        goto forward_to_fw;

    case PM_RESET_ASSERT:
    case PM_RESET_GET_STATUS:
        if ( !domain_has_reset_access(current->domain, pm_arg[0]) ) {
            printk("zynqmp-pm: fn=%d No access to reset %d\n", pm_fn, pm_arg[0]);
            ret[0] = PM_RET_ERROR_ACCESS;
            goto done;
        }
        goto forward_to_fw;

    /* These calls are safe and always allowed.  */
    case ZYNQMP_SIP_SVC_CALL_COUNT:
    case ZYNQMP_SIP_SVC_UID:
    case ZYNQMP_SIP_SVC_VERSION:
    case PM_GET_API_VERSION:
    case PM_GET_CHIPID:
        goto forward_to_fw;

    /* Mediated MMIO access.  */
    case PM_MMIO_WRITE:
        is_mmio_write = true;
    /* Fallthrough.  */
    case PM_MMIO_READ:
        if ( !domain_mediate_mmio_access(current->domain,
                                         is_mmio_write,
                                         pm_arg[0], &pm_arg[1]) ) {
            printk("eemi: fn=%d No access to MMIO %s %x\n",
                   pm_fn, is_mmio_write ? "write" : "read", pm_arg[0]);
            ret[0] = PM_RET_ERROR_ACCESS;
            goto done;
        }
        goto forward_to_fw;

    /* Exclusive to the hardware domain.  */
    case PM_INIT:
    case PM_SET_CONFIGURATION:
    case PM_FPGA_LOAD:
    case PM_FPGA_GET_STATUS:
        if ( !is_hardware_domain(current->domain) ) {
            printk("eemi: fn=%d No access", pm_fn);
            ret[0] = PM_RET_ERROR_ACCESS;
            goto done;
        }
        goto forward_to_fw;

    /* These calls are never allowed.  */
    case PM_SYSTEM_SHUTDOWN:
        ret[0] = PM_RET_ERROR_ACCESS;
        goto done;

    default:
        printk("zynqmp-pm: Unhandled PM Call: %d\n", (u32)fid);
        return false;
    }

forward_to_fw:
    /* Re-encode pm args.  */
    a0 = ((uint64_t)pm_arg[1] << 32) | pm_arg[0];
    a1 = ((uint64_t)pm_arg[3] << 32) | pm_arg[2];
    call_smcc64(fid, a0, a1, a2, a3, a4, a5, ret);
done:
    return true;
}

/*
 * Local variables:
 * mode: C
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * End:
 */
