/*
 * xen/arch/arm/platforms/xilinx-zynqmp-eemi.c
 *
 * Xilinx ZynqMP EEMI API
 *
 * Copyright (c) 2018 Xilinx Inc.
 * Written by Edgar E. Iglesias <edgar.iglesias@xilinx.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms and conditions of the GNU General Public
 * License, version 2, as published by the Free Software Foundation.
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
#include <asm/platforms/xilinx-zynqmp-eemi.h>
#include <asm/platforms/xilinx-zynqmp-mm.h>

/*
 * EEMI firmware API:
 * https://www.xilinx.com/support/documentation/user_guides/ug1200-eemi-api.pdf
 *
 * IPI firmware API:
 * https://github.com/ARM-software/arm-trusted-firmware/blob/master/plat/xilinx/zynqmp/ipi_mailbox_service/ipi_mailbox_svc.h
 *
 * Although the details of the setup are configurable, in the common case
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
 * our input is an address and we map that address into a node. If the
 * guest has ownership of that node, the access is allowed.
 * Some registers contain bitfields and a single register may contain
 * bits that affect multiple nodes.
 *
 * Some of the calls act on more global state. For example acting on
 * ZYNQMP_PM_DEV_PS, which affects many a lot of nodes. This higher level
 * orchestrating is left for the hardware-domain only.
 */

/*
 * This table maps a node into a memory address.
 * If a guest has access to the address, it has enough control
 * over the node to grant it access to EEMI calls for that node.
 */
static const struct pm_access pm_node_access[] = {
    /* MM_RPU grants access to alll RPU Nodes.  */
    [ZYNQMP_PM_DEV_UNKNOWN] = { .hwdom_access = true },
    [ZYNQMP_PM_DEV_RPU] = { MM_RPU },
    [ZYNQMP_PM_DEV_RPU_0] = { MM_RPU },
    [ZYNQMP_PM_DEV_RPU_1] = { MM_RPU },
    [ZYNQMP_PM_DEV_IPI_RPU_0] = { MM_RPU },

    /* GPU nodes.  */
    [ZYNQMP_PM_DEV_GPU] = { MM_GPU },
    [ZYNQMP_PM_DEV_GPU_PP_0] = { MM_GPU },
    [ZYNQMP_PM_DEV_GPU_PP_1] = { MM_GPU },

    [ZYNQMP_PM_DEV_USB_0] = { MM_USB3_0_XHCI },
    [ZYNQMP_PM_DEV_USB_1] = { MM_USB3_1_XHCI },
    [ZYNQMP_PM_DEV_TTC_0] = { MM_TTC0 },
    [ZYNQMP_PM_DEV_TTC_1] = { MM_TTC1 },
    [ZYNQMP_PM_DEV_TTC_2] = { MM_TTC2 },
    [ZYNQMP_PM_DEV_TTC_3] = { MM_TTC3 },
    [ZYNQMP_PM_DEV_SATA] = { MM_SATA_AHCI_HBA },
    [ZYNQMP_PM_DEV_ETH_0] = { MM_GEM0 },
    [ZYNQMP_PM_DEV_ETH_1] = { MM_GEM1 },
    [ZYNQMP_PM_DEV_ETH_2] = { MM_GEM2 },
    [ZYNQMP_PM_DEV_ETH_3] = { MM_GEM3 },
    [ZYNQMP_PM_DEV_UART_0] = { MM_UART0 },
    [ZYNQMP_PM_DEV_UART_1] = { MM_UART1 },
    [ZYNQMP_PM_DEV_SPI_0] = { MM_SPI0 },
    [ZYNQMP_PM_DEV_SPI_1] = { MM_SPI1 },
    [ZYNQMP_PM_DEV_I2C_0] = { MM_I2C0 },
    [ZYNQMP_PM_DEV_I2C_1] = { MM_I2C1 },
    [ZYNQMP_PM_DEV_SD_0] = { MM_SD0 },
    [ZYNQMP_PM_DEV_SD_1] = { MM_SD1 },
    [ZYNQMP_PM_DEV_DP] = { MM_DP },
    [ZYNQMP_PM_DEV_VPLL] = { MM_DP },
    [ZYNQMP_PM_DEV_RPLL] = { MM_DP },

    /* Guest with GDMA Channel 0 gets PM access. Other guests don't.  */
    [ZYNQMP_PM_DEV_GDMA] = { MM_GDMA_CH0 },
    /* Guest with ADMA Channel 0 gets PM access. Other guests don't.  */
    [ZYNQMP_PM_DEV_ADMA] = { MM_ADMA_CH0 },

    [ZYNQMP_PM_DEV_NAND] = { MM_NAND },
    [ZYNQMP_PM_DEV_QSPI] = { MM_QSPI },
    [ZYNQMP_PM_DEV_GPIO] = { MM_GPIO },
    [ZYNQMP_PM_DEV_CAN_0] = { MM_CAN0 },
    [ZYNQMP_PM_DEV_CAN_1] = { MM_CAN1 },

    /* Only for the hardware domain.  */
    [ZYNQMP_PM_DEV_AFI] = { .hwdom_access = true },
    [ZYNQMP_PM_DEV_DDR] = { .hwdom_access = true },
    [ZYNQMP_PM_DEV_IPI_APU] = { .hwdom_access = true },
    [ZYNQMP_PM_DEV_PCAP] = { .hwdom_access = true },
    [ZYNQMP_PM_DEV_LPD] = { .hwdom_access = true },
    [ZYNQMP_PM_DEV_PL] = { .hwdom_access = true },

    [ZYNQMP_PM_DEV_PCIE] = { MM_PCIE_ATTRIB },
    [ZYNQMP_PM_DEV_RTC] = { MM_RTC },

    [ZYNQMP_PM_DEV_TCM_0_A] = { MM_TCM_0_A },
    [ZYNQMP_PM_DEV_TCM_0_B] = { MM_TCM_0_B },
    [ZYNQMP_PM_DEV_TCM_1_A] = { MM_TCM_1_A },
    [ZYNQMP_PM_DEV_TCM_1_B] = { MM_TCM_1_B },
    [ZYNQMP_PM_DEV_SWDT_1] = { MM_SWDT },
};

/*
 * This table maps reset line IDs into a memory address.
 * If a guest has access to the address, it has enough control
 * over the affected node to grant it access to EEMI calls for
 * resetting that node.
 */
#define PM_RESET_IDX(n) (n - ZYNQMP_PM_RST_PCIE_CFG)
static const struct pm_access pm_reset_access[] = {
    [PM_RESET_IDX(ZYNQMP_PM_RST_PCIE_CFG)] = { MM_AXIPCIE_MAIN },
    [PM_RESET_IDX(ZYNQMP_PM_RST_PCIE_BRIDGE)] = { MM_PCIE_ATTRIB },
    [PM_RESET_IDX(ZYNQMP_PM_RST_PCIE_CTRL)] = { MM_PCIE_ATTRIB },

    [PM_RESET_IDX(ZYNQMP_PM_RST_DP)] = { MM_DP },
    [PM_RESET_IDX(ZYNQMP_PM_RST_SWDT_CRF)] = { MM_SWDT },
    [PM_RESET_IDX(ZYNQMP_PM_RST_AFI_FM5)] = { .hwdom_access = true },
    [PM_RESET_IDX(ZYNQMP_PM_RST_AFI_FM4)] = { .hwdom_access = true },
    [PM_RESET_IDX(ZYNQMP_PM_RST_AFI_FM3)] = { .hwdom_access = true },
    [PM_RESET_IDX(ZYNQMP_PM_RST_AFI_FM2)] = { .hwdom_access = true },
    [PM_RESET_IDX(ZYNQMP_PM_RST_AFI_FM1)] = { .hwdom_access = true },
    [PM_RESET_IDX(ZYNQMP_PM_RST_AFI_FM0)] = { .hwdom_access = true },

    /* Channel 0 grants PM access.  */
    [PM_RESET_IDX(ZYNQMP_PM_RST_GDMA)] = { MM_GDMA_CH0 },
    [PM_RESET_IDX(ZYNQMP_PM_RST_GPU_PP1)] = { MM_GPU },
    [PM_RESET_IDX(ZYNQMP_PM_RST_GPU_PP0)] = { MM_GPU },
    [PM_RESET_IDX(ZYNQMP_PM_RST_GT)] = { .hwdom_access = true },
    [PM_RESET_IDX(ZYNQMP_PM_RST_SATA)] = { MM_SATA_AHCI_HBA },

    /* We don't allow anyone to turn on/off the ACPUs.  */
    [PM_RESET_IDX(ZYNQMP_PM_RST_ACPU3_PWRON)] = { 0 },
    [PM_RESET_IDX(ZYNQMP_PM_RST_ACPU2_PWRON)] = { 0 },
    [PM_RESET_IDX(ZYNQMP_PM_RST_ACPU1_PWRON)] = { 0 },
    [PM_RESET_IDX(ZYNQMP_PM_RST_ACPU0_PWRON)] = { 0 },
    [PM_RESET_IDX(ZYNQMP_PM_RST_APU_L2)] = { 0 },
    [PM_RESET_IDX(ZYNQMP_PM_RST_ACPU3)] = { 0 },
    [PM_RESET_IDX(ZYNQMP_PM_RST_ACPU2)] = { 0 },
    [PM_RESET_IDX(ZYNQMP_PM_RST_ACPU1)] = { 0 },
    [PM_RESET_IDX(ZYNQMP_PM_RST_ACPU0)] = { 0 },

    [PM_RESET_IDX(ZYNQMP_PM_RST_DDR)] = { 0 },

    [PM_RESET_IDX(ZYNQMP_PM_RST_APM_FPD)] = { .hwdom_access = true },
    [PM_RESET_IDX(ZYNQMP_PM_RST_SOFT)] = { .hwdom_access = true },

    [PM_RESET_IDX(ZYNQMP_PM_RST_GEM0)] = { MM_GEM0 },
    [PM_RESET_IDX(ZYNQMP_PM_RST_GEM1)] = { MM_GEM1 },
    [PM_RESET_IDX(ZYNQMP_PM_RST_GEM2)] = { MM_GEM2 },
    [PM_RESET_IDX(ZYNQMP_PM_RST_GEM3)] = { MM_GEM3 },

    [PM_RESET_IDX(ZYNQMP_PM_RST_QSPI)] = { MM_QSPI },
    [PM_RESET_IDX(ZYNQMP_PM_RST_UART0)] = { MM_UART0 },
    [PM_RESET_IDX(ZYNQMP_PM_RST_UART1)] = { MM_UART1 },
    [PM_RESET_IDX(ZYNQMP_PM_RST_SPI0)] = { MM_SPI0 },
    [PM_RESET_IDX(ZYNQMP_PM_RST_SPI1)] = { MM_SPI1 },
    [PM_RESET_IDX(ZYNQMP_PM_RST_SDIO0)] = { MM_SD0 },
    [PM_RESET_IDX(ZYNQMP_PM_RST_SDIO1)] = { MM_SD1 },
    [PM_RESET_IDX(ZYNQMP_PM_RST_CAN0)] = { MM_CAN0 },
    [PM_RESET_IDX(ZYNQMP_PM_RST_CAN1)] = { MM_CAN1 },
    [PM_RESET_IDX(ZYNQMP_PM_RST_I2C0)] = { MM_I2C0 },
    [PM_RESET_IDX(ZYNQMP_PM_RST_I2C1)] = { MM_I2C1 },
    [PM_RESET_IDX(ZYNQMP_PM_RST_TTC0)] = { MM_TTC0 },
    [PM_RESET_IDX(ZYNQMP_PM_RST_TTC1)] = { MM_TTC1 },
    [PM_RESET_IDX(ZYNQMP_PM_RST_TTC2)] = { MM_TTC2 },
    [PM_RESET_IDX(ZYNQMP_PM_RST_TTC3)] = { MM_TTC3 },
    [PM_RESET_IDX(ZYNQMP_PM_RST_SWDT_CRL)] = { MM_SWDT },
    [PM_RESET_IDX(ZYNQMP_PM_RST_NAND)] = { MM_NAND },
    /* ADMA Channel 0 grants access to pull the reset signal.  */
    [PM_RESET_IDX(ZYNQMP_PM_RST_ADMA)] = { MM_ADMA_CH0 },
    [PM_RESET_IDX(ZYNQMP_PM_RST_GPIO)] = { MM_GPIO },
    /* FIXME: What is this?  */
    [PM_RESET_IDX(ZYNQMP_PM_RST_IOU_CC)] = { .hwdom_access = true },
    [PM_RESET_IDX(ZYNQMP_PM_RST_TIMESTAMP)] = { .hwdom_access = true },
    [PM_RESET_IDX(ZYNQMP_PM_RST_RPU_R50)] = { MM_RPU },
    [PM_RESET_IDX(ZYNQMP_PM_RST_RPU_R51)] = { MM_RPU },
    [PM_RESET_IDX(ZYNQMP_PM_RST_RPU_AMBA)] = { MM_RPU },
    [PM_RESET_IDX(ZYNQMP_PM_RST_OCM)] = { .hwdom_access = true },
    [PM_RESET_IDX(ZYNQMP_PM_RST_RPU_PGE)] = { MM_RPU },

    [PM_RESET_IDX(ZYNQMP_PM_RST_USB0_CORERESET)] = { MM_USB3_0_XHCI },
    [PM_RESET_IDX(ZYNQMP_PM_RST_USB0_HIBERRESET)] = { MM_USB3_0_XHCI },
    [PM_RESET_IDX(ZYNQMP_PM_RST_USB0_APB)] = { MM_USB3_0_XHCI },

    [PM_RESET_IDX(ZYNQMP_PM_RST_USB1_CORERESET)] = { MM_USB3_1_XHCI },
    [PM_RESET_IDX(ZYNQMP_PM_RST_USB1_HIBERRESET)] = { MM_USB3_1_XHCI },
    [PM_RESET_IDX(ZYNQMP_PM_RST_USB1_APB)] = { MM_USB3_1_XHCI },

    [PM_RESET_IDX(ZYNQMP_PM_RST_IPI)] = { .hwdom_access = true },
    [PM_RESET_IDX(ZYNQMP_PM_RST_APM_LPD)] = { .hwdom_access = true },
    [PM_RESET_IDX(ZYNQMP_PM_RST_RTC)] = { MM_RTC },
    [PM_RESET_IDX(ZYNQMP_PM_RST_SYSMON)] = { .hwdom_access = true },
    [PM_RESET_IDX(ZYNQMP_PM_RST_AFI_FM6)] = { .hwdom_access = true },
    [PM_RESET_IDX(ZYNQMP_PM_RST_LPD_SWDT)] = { MM_SWDT },
    [PM_RESET_IDX(ZYNQMP_PM_RST_FPD)] = { .hwdom_access = true },
    [PM_RESET_IDX(ZYNQMP_PM_RST_RPU_DBG1)] = { MM_RPU },
    [PM_RESET_IDX(ZYNQMP_PM_RST_RPU_DBG0)] = { MM_RPU },
    [PM_RESET_IDX(ZYNQMP_PM_RST_DBG_LPD)] = { .hwdom_access = true },
    [PM_RESET_IDX(ZYNQMP_PM_RST_DBG_FPD)] = { .hwdom_access = true },
    [PM_RESET_IDX(ZYNQMP_PM_RST_GPO3_PL_0)] = { .hwdom_access = true },
    [PM_RESET_IDX(ZYNQMP_PM_RST_GPO3_PL_1)] = { .hwdom_access = true },
    [PM_RESET_IDX(ZYNQMP_PM_RST_GPO3_PL_2)] = { .hwdom_access = true },
    [PM_RESET_IDX(ZYNQMP_PM_RST_GPO3_PL_3)] = { .hwdom_access = true },
    [PM_RESET_IDX(ZYNQMP_PM_RST_GPO3_PL_4)] = { .hwdom_access = true },
    [PM_RESET_IDX(ZYNQMP_PM_RST_GPO3_PL_5)] = { .hwdom_access = true },
    [PM_RESET_IDX(ZYNQMP_PM_RST_GPO3_PL_6)] = { .hwdom_access = true },
    [PM_RESET_IDX(ZYNQMP_PM_RST_GPO3_PL_7)] = { .hwdom_access = true },
    [PM_RESET_IDX(ZYNQMP_PM_RST_GPO3_PL_8)] = { .hwdom_access = true },
    [PM_RESET_IDX(ZYNQMP_PM_RST_GPO3_PL_9)] = { .hwdom_access = true },
    [PM_RESET_IDX(ZYNQMP_PM_RST_GPO3_PL_10)] = { .hwdom_access = true },
    [PM_RESET_IDX(ZYNQMP_PM_RST_GPO3_PL_11)] = { .hwdom_access = true },
    [PM_RESET_IDX(ZYNQMP_PM_RST_GPO3_PL_12)] = { .hwdom_access = true },
    [PM_RESET_IDX(ZYNQMP_PM_RST_GPO3_PL_13)] = { .hwdom_access = true },
    [PM_RESET_IDX(ZYNQMP_PM_RST_GPO3_PL_14)] = { .hwdom_access = true },
    [PM_RESET_IDX(ZYNQMP_PM_RST_GPO3_PL_15)] = { .hwdom_access = true },
    [PM_RESET_IDX(ZYNQMP_PM_RST_GPO3_PL_16)] = { .hwdom_access = true },
    [PM_RESET_IDX(ZYNQMP_PM_RST_GPO3_PL_17)] = { .hwdom_access = true },
    [PM_RESET_IDX(ZYNQMP_PM_RST_GPO3_PL_18)] = { .hwdom_access = true },
    [PM_RESET_IDX(ZYNQMP_PM_RST_GPO3_PL_19)] = { .hwdom_access = true },
    [PM_RESET_IDX(ZYNQMP_PM_RST_GPO3_PL_20)] = { .hwdom_access = true },
    [PM_RESET_IDX(ZYNQMP_PM_RST_GPO3_PL_21)] = { .hwdom_access = true },
    [PM_RESET_IDX(ZYNQMP_PM_RST_GPO3_PL_22)] = { .hwdom_access = true },
    [PM_RESET_IDX(ZYNQMP_PM_RST_GPO3_PL_23)] = { .hwdom_access = true },
    [PM_RESET_IDX(ZYNQMP_PM_RST_GPO3_PL_24)] = { .hwdom_access = true },
    [PM_RESET_IDX(ZYNQMP_PM_RST_GPO3_PL_25)] = { .hwdom_access = true },
    [PM_RESET_IDX(ZYNQMP_PM_RST_GPO3_PL_26)] = { .hwdom_access = true },
    [PM_RESET_IDX(ZYNQMP_PM_RST_GPO3_PL_27)] = { .hwdom_access = true },
    [PM_RESET_IDX(ZYNQMP_PM_RST_GPO3_PL_28)] = { .hwdom_access = true },
    [PM_RESET_IDX(ZYNQMP_PM_RST_GPO3_PL_29)] = { .hwdom_access = true },
    [PM_RESET_IDX(ZYNQMP_PM_RST_GPO3_PL_30)] = { .hwdom_access = true },
    [PM_RESET_IDX(ZYNQMP_PM_RST_GPO3_PL_31)] = { .hwdom_access = true },
    [PM_RESET_IDX(ZYNQMP_PM_RST_RPU_LS)] = { MM_RPU },
    [PM_RESET_IDX(ZYNQMP_PM_RST_PS_ONLY)] = { .hwdom_access = true },
    [PM_RESET_IDX(ZYNQMP_PM_RST_PL)] = { .hwdom_access = true },
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
    uint32_t node;
    bool hwdom_access;
    bool readonly;
} pm_mmio_access[] = {
    { .start = MM_CRL_APB + R_CRL_PL0_THR_CTRL, .hwdom_access = true },
    { .start = MM_CRL_APB + R_CRL_PL1_THR_CTRL, .hwdom_access = true },
    { .start = MM_CRL_APB + R_CRL_PL2_THR_CTRL, .hwdom_access = true },
    { .start = MM_CRL_APB + R_CRL_PL3_THR_CTRL, .hwdom_access = true },
    { .start = MM_CRL_APB + R_CRL_PL0_THR_CNT, .hwdom_access = true },
    { .start = MM_CRL_APB + R_CRL_PL1_THR_CNT, .hwdom_access = true },
    { .start = MM_CRL_APB + R_CRL_PL2_THR_CNT, .hwdom_access = true },
    { .start = MM_CRL_APB + R_CRL_PL3_THR_CNT, .hwdom_access = true },
    /* MIOs are controlled by the hardware domain.  */
    {
        .start = MM_IOU_SLCR + R_IOU_SLCR_MIO_PIN_0,
        .end = MM_IOU_SLCR + R_IOU_SLCR_MIO_MST_TRI2,
        .hwdom_access = true
    },
    {
        .start = MM_IOU_SLCR + R_IOU_SLCR_CTRL_REG_SD,
        .mask = 0x1, .node = ZYNQMP_PM_DEV_SD_0
    },
    {
        .start = MM_IOU_SLCR + R_IOU_SLCR_CTRL_REG_SD,
        .mask = 0x1 << 15, .node = ZYNQMP_PM_DEV_SD_1
    },
    /* A series of SD regs with the same layout.  */
    {
        .start = MM_IOU_SLCR + R_IOU_SLCR_SD_ITAPDLY,
        .end = MM_IOU_SLCR + R_IOU_SLCR_SD_CDN_CTRL,
        .mask = 0x3ff << 0, .node = ZYNQMP_PM_DEV_SD_0
    },
    {
        .start = MM_IOU_SLCR + R_IOU_SLCR_SD_ITAPDLY,
        .end = MM_IOU_SLCR + R_IOU_SLCR_SD_CDN_CTRL,
        .mask = 0x3ff << 16, .node = ZYNQMP_PM_DEV_SD_1
    },
    {
        .start = MM_IOU_SLCR + R_IOU_SLCR_GEM_CTRL,
        .mask = 0x3 << 0, .node = ZYNQMP_PM_DEV_ETH_0
    },
    {
        .start = MM_IOU_SLCR + R_IOU_SLCR_GEM_CTRL,
        .mask = 0x3 << 2, .node = ZYNQMP_PM_DEV_ETH_1
    },
    {
        .start = MM_IOU_SLCR + R_IOU_SLCR_GEM_CTRL,
        .mask = 0x3 << 4, .node = ZYNQMP_PM_DEV_ETH_2
    },
    {
        .start = MM_IOU_SLCR + R_IOU_SLCR_GEM_CTRL,
        .mask = 0x3 << 6, .node = ZYNQMP_PM_DEV_ETH_3
    },
    {
        .start = MM_IOU_SLCR + R_IOU_SLCR_IOU_TAPDLY_BYPASS,
        .mask = 0x3, .node = ZYNQMP_PM_DEV_NAND
    },
    {
        .start = MM_IOU_SLCR + R_IOU_SLCR_IOU_TAPDLY_BYPASS,
        .mask = 0x1 << 2, .node = ZYNQMP_PM_DEV_QSPI
    },
    {
        .start = MM_IOU_SLCR + R_IOU_SLCR_IOU_COHERENT_CTRL,
        .mask = 0xf << 0, .node = ZYNQMP_PM_DEV_ETH_0
    },
    {
        .start = MM_IOU_SLCR + R_IOU_SLCR_IOU_COHERENT_CTRL,
        .mask = 0xf << 4, .node = ZYNQMP_PM_DEV_ETH_1
    },
    {
        .start = MM_IOU_SLCR + R_IOU_SLCR_IOU_COHERENT_CTRL,
        .mask = 0xf << 8, .node = ZYNQMP_PM_DEV_ETH_2
    },
    {
        .start = MM_IOU_SLCR + R_IOU_SLCR_IOU_COHERENT_CTRL,
        .mask = 0xf << 12, .node = ZYNQMP_PM_DEV_ETH_3
    },
    {
        .start = MM_IOU_SLCR + R_IOU_SLCR_IOU_COHERENT_CTRL,
        .mask = 0xf << 16, .node = ZYNQMP_PM_DEV_SD_0
    },
    {
        .start = MM_IOU_SLCR + R_IOU_SLCR_IOU_COHERENT_CTRL,
        .mask = 0xf << 20, .node = ZYNQMP_PM_DEV_SD_1
    },
    {
        .start = MM_IOU_SLCR + R_IOU_SLCR_IOU_COHERENT_CTRL,
        .mask = 0xf << 24, .node = ZYNQMP_PM_DEV_NAND
    },
    {
        .start = MM_IOU_SLCR + R_IOU_SLCR_IOU_COHERENT_CTRL,
        .mask = 0xf << 28, .node = ZYNQMP_PM_DEV_QSPI
    },
    { .start = MM_IOU_SLCR + R_IOU_SLCR_VIDEO_PSS_CLK_SEL, .hwdom_access = true },
    /* No access to R_IOU_SLCR_IOU_INTERCONNECT_ROUTE at all.  */
    { .start = MM_IOU_SLCR + R_IOU_SLCR_IOU_RAM_GEM0, .node = ZYNQMP_PM_DEV_ETH_0 },
    { .start = MM_IOU_SLCR + R_IOU_SLCR_IOU_RAM_GEM1, .node = ZYNQMP_PM_DEV_ETH_1 },
    { .start = MM_IOU_SLCR + R_IOU_SLCR_IOU_RAM_GEM2, .node = ZYNQMP_PM_DEV_ETH_2 },
    { .start = MM_IOU_SLCR + R_IOU_SLCR_IOU_RAM_GEM3, .node = ZYNQMP_PM_DEV_ETH_3 },
    { .start = MM_IOU_SLCR + R_IOU_SLCR_IOU_RAM_SD0, .node = ZYNQMP_PM_DEV_SD_0 },
    { .start = MM_IOU_SLCR + R_IOU_SLCR_IOU_RAM_SD1, .node = ZYNQMP_PM_DEV_SD_1 },
    { .start = MM_IOU_SLCR + R_IOU_SLCR_IOU_RAM_CAN0, .node = ZYNQMP_PM_DEV_CAN_0 },
    { .start = MM_IOU_SLCR + R_IOU_SLCR_IOU_RAM_CAN1, .node = ZYNQMP_PM_DEV_CAN_1 },
    { .start = MM_IOU_SLCR + R_IOU_SLCR_IOU_RAM_LQSPI, .node = ZYNQMP_PM_DEV_QSPI },
    { .start = MM_IOU_SLCR + R_IOU_SLCR_IOU_RAM_NAND, .node = ZYNQMP_PM_DEV_NAND },
    {
        .start = MM_PMU_GLOBAL + R_PMU_GLOBAL_PWR_STATE,
        .readonly = true,
    },
    {
        .start = MM_PMU_GLOBAL + R_PMU_GLOBAL_GLOBAL_GEN_STORAGE0,
        .end = MM_PMU_GLOBAL + R_PMU_GLOBAL_PERS_GLOB_GEN_STORAGE7,
        .readonly = true,
    },
};


/* This array must be ordered by the increasing clock ID values */
static const struct pm_clk2node pm_clock_node_map[] = {
    PM_CLK2NODE(ZYNQMP_PM_CLK_RPLL, ZYNQMP_PM_DEV_DP),
    PM_CLK2NODE(ZYNQMP_PM_CLK_VPLL, ZYNQMP_PM_DEV_DP),
    PM_CLK2NODE(ZYNQMP_PM_CLK_RPLL_TO_FPD, ZYNQMP_PM_DEV_DP),
    PM_CLK2NODE(ZYNQMP_PM_CLK_VPLL_TO_LPD, ZYNQMP_PM_DEV_DP),
    PM_CLK2NODE(ZYNQMP_PM_CLK_DP_VIDEO_REF, ZYNQMP_PM_DEV_DP),
    PM_CLK2NODE(ZYNQMP_PM_CLK_DP_AUDIO_REF, ZYNQMP_PM_DEV_DP),
    PM_CLK2NODE(ZYNQMP_PM_CLK_DP_STC_REF, ZYNQMP_PM_DEV_DP),
    PM_CLK2NODE(ZYNQMP_PM_CLK_GDMA_REF, ZYNQMP_PM_DEV_GDMA),
    PM_CLK2NODE(ZYNQMP_PM_CLK_DPDMA_REF, ZYNQMP_PM_DEV_DP),
    PM_CLK2NODE(ZYNQMP_PM_CLK_SATA_REF, ZYNQMP_PM_DEV_SATA),
    PM_CLK2NODE(ZYNQMP_PM_CLK_PCIE_REF, ZYNQMP_PM_DEV_PCIE),
    PM_CLK2NODE(ZYNQMP_PM_CLK_GPU_REF, ZYNQMP_PM_DEV_GPU),
    PM_CLK2NODE(ZYNQMP_PM_CLK_GPU_PP0_REF, ZYNQMP_PM_DEV_GPU),
    PM_CLK2NODE(ZYNQMP_PM_CLK_GPU_PP1_REF, ZYNQMP_PM_DEV_GPU),
    PM_CLK2NODE(ZYNQMP_PM_CLK_TOPSW_LSBUS, ZYNQMP_PM_DEV_DDR),
    PM_CLK2NODE(ZYNQMP_PM_CLK_LPD_LSBUS, ZYNQMP_PM_DEV_TTC_0),
    PM_CLK2NODE(ZYNQMP_PM_CLK_LPD_LSBUS, ZYNQMP_PM_DEV_TTC_1),
    PM_CLK2NODE(ZYNQMP_PM_CLK_LPD_LSBUS, ZYNQMP_PM_DEV_TTC_2),
    PM_CLK2NODE(ZYNQMP_PM_CLK_LPD_LSBUS, ZYNQMP_PM_DEV_TTC_3),
    PM_CLK2NODE(ZYNQMP_PM_CLK_USB0_BUS_REF, ZYNQMP_PM_DEV_USB_0),
    PM_CLK2NODE(ZYNQMP_PM_CLK_USB1_BUS_REF, ZYNQMP_PM_DEV_USB_1),
    PM_CLK2NODE(ZYNQMP_PM_CLK_USB3_DUAL_REF, ZYNQMP_PM_DEV_USB_0),
    PM_CLK2NODE(ZYNQMP_PM_CLK_USB3_DUAL_REF, ZYNQMP_PM_DEV_USB_1),
    PM_CLK2NODE(ZYNQMP_PM_CLK_CPU_R5, ZYNQMP_PM_DEV_RPU),
    PM_CLK2NODE(ZYNQMP_PM_CLK_CPU_R5_CORE, ZYNQMP_PM_DEV_RPU),
    PM_CLK2NODE(ZYNQMP_PM_CLK_CSU_PLL, ZYNQMP_PM_DEV_PCAP),
    PM_CLK2NODE(ZYNQMP_PM_CLK_PCAP, ZYNQMP_PM_DEV_PCAP),
    PM_CLK2NODE(ZYNQMP_PM_CLK_GEM_TSU_REF, ZYNQMP_PM_DEV_ETH_0),
    PM_CLK2NODE(ZYNQMP_PM_CLK_GEM_TSU_REF, ZYNQMP_PM_DEV_ETH_1),
    PM_CLK2NODE(ZYNQMP_PM_CLK_GEM_TSU_REF, ZYNQMP_PM_DEV_ETH_2),
    PM_CLK2NODE(ZYNQMP_PM_CLK_GEM_TSU_REF, ZYNQMP_PM_DEV_ETH_3),
    PM_CLK2NODE(ZYNQMP_PM_CLK_GEM_TSU, ZYNQMP_PM_DEV_ETH_0),
    PM_CLK2NODE(ZYNQMP_PM_CLK_GEM_TSU, ZYNQMP_PM_DEV_ETH_1),
    PM_CLK2NODE(ZYNQMP_PM_CLK_GEM_TSU, ZYNQMP_PM_DEV_ETH_2),
    PM_CLK2NODE(ZYNQMP_PM_CLK_GEM_TSU, ZYNQMP_PM_DEV_ETH_3),
    PM_CLK2NODE(ZYNQMP_PM_CLK_GEM0_TX, ZYNQMP_PM_DEV_ETH_0),
    PM_CLK2NODE(ZYNQMP_PM_CLK_GEM1_TX, ZYNQMP_PM_DEV_ETH_1),
    PM_CLK2NODE(ZYNQMP_PM_CLK_GEM2_TX, ZYNQMP_PM_DEV_ETH_2),
    PM_CLK2NODE(ZYNQMP_PM_CLK_GEM3_TX, ZYNQMP_PM_DEV_ETH_3),
    PM_CLK2NODE(ZYNQMP_PM_CLK_GEM0_RX, ZYNQMP_PM_DEV_ETH_0),
    PM_CLK2NODE(ZYNQMP_PM_CLK_GEM1_RX, ZYNQMP_PM_DEV_ETH_1),
    PM_CLK2NODE(ZYNQMP_PM_CLK_GEM2_RX, ZYNQMP_PM_DEV_ETH_2),
    PM_CLK2NODE(ZYNQMP_PM_CLK_GEM3_RX, ZYNQMP_PM_DEV_ETH_3),
    PM_CLK2NODE(ZYNQMP_PM_CLK_QSPI_REF, ZYNQMP_PM_DEV_QSPI),
    PM_CLK2NODE(ZYNQMP_PM_CLK_SDIO0_REF, ZYNQMP_PM_DEV_SD_0),
    PM_CLK2NODE(ZYNQMP_PM_CLK_SDIO1_REF, ZYNQMP_PM_DEV_SD_1),
    PM_CLK2NODE(ZYNQMP_PM_CLK_UART0_REF, ZYNQMP_PM_DEV_UART_0),
    PM_CLK2NODE(ZYNQMP_PM_CLK_UART1_REF, ZYNQMP_PM_DEV_UART_1),
    PM_CLK2NODE(ZYNQMP_PM_CLK_SPI0_REF, ZYNQMP_PM_DEV_SPI_0),
    PM_CLK2NODE(ZYNQMP_PM_CLK_SPI1_REF, ZYNQMP_PM_DEV_SPI_1),
    PM_CLK2NODE(ZYNQMP_PM_CLK_NAND_REF, ZYNQMP_PM_DEV_NAND),
    PM_CLK2NODE(ZYNQMP_PM_CLK_I2C0_REF, ZYNQMP_PM_DEV_I2C_0),
    PM_CLK2NODE(ZYNQMP_PM_CLK_I2C1_REF, ZYNQMP_PM_DEV_I2C_1),
    PM_CLK2NODE(ZYNQMP_PM_CLK_CAN0_REF, ZYNQMP_PM_DEV_CAN_0),
    PM_CLK2NODE(ZYNQMP_PM_CLK_CAN1_REF, ZYNQMP_PM_DEV_CAN_1),
    PM_CLK2NODE(ZYNQMP_PM_CLK_CAN0, ZYNQMP_PM_DEV_CAN_0),
    PM_CLK2NODE(ZYNQMP_PM_CLK_CAN1, ZYNQMP_PM_DEV_CAN_1),
    PM_CLK2NODE(ZYNQMP_PM_CLK_DLL_REF, ZYNQMP_PM_DEV_SD_0),
    PM_CLK2NODE(ZYNQMP_PM_CLK_DLL_REF, ZYNQMP_PM_DEV_SD_1),
    PM_CLK2NODE(ZYNQMP_PM_CLK_ADMA_REF, ZYNQMP_PM_DEV_ADMA),
    PM_CLK2NODE(ZYNQMP_PM_CLK_AMS_REF, ZYNQMP_PM_DEV_LPD),
    PM_CLK2NODE(ZYNQMP_PM_CLK_PL0_REF, ZYNQMP_PM_DEV_PL),
    PM_CLK2NODE(ZYNQMP_PM_CLK_PL1_REF, ZYNQMP_PM_DEV_PL),
    PM_CLK2NODE(ZYNQMP_PM_CLK_PL2_REF, ZYNQMP_PM_DEV_PL),
    PM_CLK2NODE(ZYNQMP_PM_CLK_PL3_REF, ZYNQMP_PM_DEV_PL),
    PM_CLK2NODE(ZYNQMP_PM_CLK_IOPLL_INT, ZYNQMP_PM_DEV_PL),
    PM_CLK2NODE(ZYNQMP_PM_CLK_IOPLL_PRE_SRC, ZYNQMP_PM_DEV_PL),
    PM_CLK2NODE(ZYNQMP_PM_CLK_IOPLL_INT_MUX, ZYNQMP_PM_DEV_PL),
    PM_CLK2NODE(ZYNQMP_PM_CLK_IOPLL_POST_SRC, ZYNQMP_PM_DEV_PL),
    PM_CLK2NODE(ZYNQMP_PM_CLK_RPLL_INT, ZYNQMP_PM_DEV_DP),
    PM_CLK2NODE(ZYNQMP_PM_CLK_RPLL_PRE_SRC, ZYNQMP_PM_DEV_DP),
    PM_CLK2NODE(ZYNQMP_PM_CLK_RPLL_INT_MUX, ZYNQMP_PM_DEV_DP),
    PM_CLK2NODE(ZYNQMP_PM_CLK_RPLL_POST_SRC, ZYNQMP_PM_DEV_DP),
    PM_CLK2NODE(ZYNQMP_PM_CLK_VPLL_INT, ZYNQMP_PM_DEV_DP),
    PM_CLK2NODE(ZYNQMP_PM_CLK_VPLL_PRE_SRC, ZYNQMP_PM_DEV_DP),
    PM_CLK2NODE(ZYNQMP_PM_CLK_VPLL_INT_MUX, ZYNQMP_PM_DEV_DP),
    PM_CLK2NODE(ZYNQMP_PM_CLK_VPLL_POST_SRC, ZYNQMP_PM_DEV_DP),
    PM_CLK2NODE(ZYNQMP_PM_CLK_CAN0_MIO, ZYNQMP_PM_DEV_CAN_0),
    PM_CLK2NODE(ZYNQMP_PM_CLK_CAN1_MIO, ZYNQMP_PM_DEV_CAN_1),
    PM_CLK2NODE(ZYNQMP_PM_CLK_GEM0_REF, ZYNQMP_PM_DEV_ETH_0),
    PM_CLK2NODE(ZYNQMP_PM_CLK_GEM1_REF, ZYNQMP_PM_DEV_ETH_1),
    PM_CLK2NODE(ZYNQMP_PM_CLK_GEM2_REF, ZYNQMP_PM_DEV_ETH_2),
    PM_CLK2NODE(ZYNQMP_PM_CLK_GEM3_REF, ZYNQMP_PM_DEV_ETH_3),
    PM_CLK2NODE(ZYNQMP_PM_CLK_GEM0_REF_UNGATED, ZYNQMP_PM_DEV_ETH_0),
    PM_CLK2NODE(ZYNQMP_PM_CLK_GEM1_REF_UNGATED, ZYNQMP_PM_DEV_ETH_1),
    PM_CLK2NODE(ZYNQMP_PM_CLK_GEM2_REF_UNGATED, ZYNQMP_PM_DEV_ETH_2),
    PM_CLK2NODE(ZYNQMP_PM_CLK_GEM3_REF_UNGATED, ZYNQMP_PM_DEV_ETH_3),
    PM_CLK2NODE(ZYNQMP_PM_CLK_LPD_WDT, ZYNQMP_PM_DEV_SWDT_1),
};

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
            if ( addr >= pm_mmio_access[i].end )
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

        r = domain_has_node_access(d, pm_mmio_access[i].node,
                                   pm_node_access,
                                   ARRAY_SIZE(pm_node_access));
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

static u32 clock_id_plls[18] = {
    ZYNQMP_PM_CLK_RPLL,
    ZYNQMP_PM_CLK_VPLL,
    ZYNQMP_PM_CLK_RPLL_TO_FPD,
    ZYNQMP_PM_CLK_VPLL_TO_LPD,
    ZYNQMP_PM_CLK_CSU_PLL,
    ZYNQMP_PM_CLK_IOPLL_INT,
    ZYNQMP_PM_CLK_IOPLL_PRE_SRC,
    ZYNQMP_PM_CLK_IOPLL_INT_MUX,
    ZYNQMP_PM_CLK_IOPLL_POST_SRC,
    ZYNQMP_PM_CLK_RPLL_INT,
    ZYNQMP_PM_CLK_RPLL_PRE_SRC,
    ZYNQMP_PM_CLK_RPLL_INT_MUX,
    ZYNQMP_PM_CLK_RPLL_POST_SRC,
    ZYNQMP_PM_CLK_VPLL_INT,
    ZYNQMP_PM_CLK_VPLL_PRE_SRC,
    ZYNQMP_PM_CLK_VPLL_INT_MUX,
    ZYNQMP_PM_CLK_VPLL_POST_SRC,
    ZYNQMP_PM_CLK_END_IDX,
};

/* Check if a clock id belongs to pll type */
static bool clock_id_is_pll(u32 clk_id)
{
    for ( int i = 0; clock_id_plls[i] != ZYNQMP_PM_CLK_END_IDX; i++ )
    {
        if ( clk_id == clock_id_plls[i] )
            return true;
    }

    return false;
}

bool zynqmp_eemi(struct cpu_user_regs *regs)
{
    struct arm_smccc_res res;
    uint32_t fid = get_user_reg(regs, 0);
    uint32_t nodeid = get_user_reg(regs, 1);
    uint32_t pm_fn = EEMI_PM_FID(fid);
    enum pm_ret_status ret;
    bool is_mmio_write = false;
    uint32_t mmio_mask = get_user_reg(regs, 1) >> 32;

    switch ( fid )
    {
    case ZYNQMP_SIP_SVC_CALL_COUNT:
    case ZYNQMP_SIP_SVC_UID:
    case ZYNQMP_SIP_SVC_VERSION:
        goto forward_to_fw;

    /* No MMIO access is allowed from non-secure domains */
    case EEMI_FID(PM_MMIO_WRITE):
        is_mmio_write = true;
    /* Fallthrough.  */
    case EEMI_FID(PM_MMIO_READ):
        if ( !domain_mediate_mmio_access(current->domain,
                                         is_mmio_write,
                                         nodeid, &mmio_mask) ) {
            printk("eemi: fn=%d No access to MMIO %s %x\n",
                   pm_fn, is_mmio_write ? "write" : "read", nodeid);
            ret = XST_PM_NO_ACCESS;
            goto done;
        }
        goto forward_to_fw;

    case EEMI_FID(PM_CLOCK_ENABLE):
    case EEMI_FID(PM_CLOCK_DISABLE):
    case EEMI_FID(PM_CLOCK_SETDIVIDER):
    case EEMI_FID(PM_CLOCK_SETPARENT):
        if ( !clock_id_is_valid(nodeid, ZYNQMP_PM_CLK_END) )
        {
            gprintk(XENLOG_WARNING, "zynqmp-pm: fn=%u Invalid clock=%u\n",
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
        if ( !domain_has_clock_access(current->domain, nodeid,
                                      pm_node_access,
                                      ARRAY_SIZE(pm_node_access),
                                      pm_clock_node_map,
                                      ARRAY_SIZE(pm_clock_node_map)) )

        {
            gprintk(XENLOG_WARNING, "zynqmp-pm: fn=%u No access to clock=%u\n",
                    pm_fn, nodeid);
            ret = XST_PM_NO_ACCESS;
            goto done;
        }
        goto forward_to_fw;

    case EEMI_FID(PM_PLL_GET_PARAMETER):
    case EEMI_FID(PM_PLL_GET_MODE):
        if ( nodeid < ZYNQMP_PM_DEV_APLL || nodeid > ZYNQMP_PM_DEV_IOPLL )
        {
            gprintk(XENLOG_WARNING, "zynqmp-pm: fn=%u Invalid pll node %u\n",
                    pm_fn, nodeid);
            ret = XST_PM_INVALID_PARAM;
            goto done;
        }
        else
            goto forward_to_fw;

    case EEMI_FID(PM_PLL_SET_PARAMETER):
    case EEMI_FID(PM_PLL_SET_MODE):
        if ( nodeid < ZYNQMP_PM_DEV_APLL || nodeid > ZYNQMP_PM_DEV_IOPLL )
        {
            gprintk(XENLOG_WARNING, "zynqmp-pm: fn=%u Invalid pll node %u\n",
                    pm_fn, nodeid);
            ret = XST_PM_INVALID_PARAM;
            goto done;
        }
        if ( !domain_has_node_access(current->domain, nodeid,
                                     pm_node_access,
                                     ARRAY_SIZE(pm_node_access)) )
        {
            gprintk(XENLOG_WARNING, "zynqmp-pm: fn=%u No access to pll=%u\n",
                    pm_fn, nodeid);
            ret = XST_PM_NO_ACCESS;
            goto done;
        }
        goto forward_to_fw;

    case EEMI_FID(PM_RESET_ASSERT):
    case EEMI_FID(PM_RESET_GET_STATUS):
        nodeid = PM_RESET_IDX(nodeid);
        /* fall through */

    default:
        return xilinx_eemi(regs, fid, nodeid, pm_fn,
                           pm_node_access,
                           ARRAY_SIZE(pm_node_access),
                           pm_reset_access,
                           ARRAY_SIZE(pm_reset_access),
                           ZYNQMP_PM_CLK_END);
    }

forward_to_fw:
    /*
     * ZynqMP firmware calls (EEMI) take an argument that specifies the
     * area of effect of the function called. Specifically, node ids for
     * power management functions and reset ids for reset functions.
     *
     * The code above checks if a virtual machine has access rights over
     * the node id, reset id, etc. Now that the check has been done, we
     * can forward the whole command to firmware without additional
     * parameters checks.
     */
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

/*
 * Local variables:
 * mode: C
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * End:
 */
