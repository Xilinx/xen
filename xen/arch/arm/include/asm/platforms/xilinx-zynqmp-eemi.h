/*
 * Copyright (c) 2018 Xilinx Inc.
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

#ifndef __ASM_ARM_PLATFORMS_ZYNQMP_H
#define __ASM_ARM_PLATFORMS_ZYNQMP_H

#include <asm/processor.h>
#include <asm/smccc.h>

/* SMC function IDs for SiP Service queries */
#define ZYNQMP_SIP_SVC_CALL_COUNT       0xff00
#define ZYNQMP_SIP_SVC_UID              0xff01
#define ZYNQMP_SIP_SVC_VERSION          0xff03

#define EEMI_FID(fid) ARM_SMCCC_CALL_VAL(ARM_SMCCC_FAST_CALL, \
                                         ARM_SMCCC_CONV_64,   \
                                         ARM_SMCCC_OWNER_SIP, \
                                         fid)
enum pm_api_id {
    /* Miscellaneous API functions: */
    PM_GET_API_VERSION = 1, /* Do not change or move */
    PM_SET_CONFIGURATION,
    PM_GET_NODE_STATUS,
    PM_GET_OP_CHARACTERISTIC,
    PM_REGISTER_NOTIFIER,
    /* API for suspending of PUs: */
    PM_REQ_SUSPEND,
    PM_SELF_SUSPEND,
    PM_FORCE_POWERDOWN,
    PM_ABORT_SUSPEND,
    PM_REQ_WAKEUP,
    PM_SET_WAKEUP_SOURCE,
    PM_SYSTEM_SHUTDOWN,
    /* API for managing PM slaves: */
    PM_REQ_NODE,
    PM_RELEASE_NODE,
    PM_SET_REQUIREMENT,
    PM_SET_MAX_LATENCY,
    /* Direct control API functions: */
    PM_RESET_ASSERT,
    PM_RESET_GET_STATUS,
    PM_MMIO_WRITE,
    PM_MMIO_READ,
    PM_INIT,
    PM_FPGA_LOAD,
    PM_FPGA_GET_STATUS,
    PM_GET_CHIPID,
    /* ID 25 is been used by U-boot to process secure boot images */
    /* Secure library generic API functions */
    PM_SECURE_SHA = 26,
    PM_SECURE_RSA,
    /* Pin control API functions */
    PM_PINCTRL_REQUEST,
    PM_PINCTRL_RELEASE,
    PM_PINCTRL_GET_FUNCTION,
    PM_PINCTRL_SET_FUNCTION,
    PM_PINCTRL_CONFIG_PARAM_GET,
    PM_PINCTRL_CONFIG_PARAM_SET,
    /* PM IOCTL API */
    PM_IOCTL,
    /* API to query information from firmware */
    PM_QUERY_DATA,
    /* Clock control API functions */
    PM_CLOCK_ENABLE,
    PM_CLOCK_DISABLE,
    PM_CLOCK_GETSTATE,
    PM_CLOCK_SETDIVIDER,
    PM_CLOCK_GETDIVIDER,
    PM_CLOCK_SETRATE,
    PM_CLOCK_GETRATE,
    PM_CLOCK_SETPARENT,
    PM_CLOCK_GETPARENT,
    PM_GET_TRUSTZONE_VERSION = 2563,
    PM_API_MAX
};

/**
 * @XST_PM_SUCCESS:		Success
 * @XST_PM_ARGS:		illegal arguments provided (deprecated)
 * @XST_PM_NOTSUPPORTED:	feature not supported  (deprecated)
 * @XST_PM_INVALID_PARAM:	invalid argument
 * @XST_PM_INTERNAL:	Unexpected error
 * @XST_PM_CONFLICT:	Conflicting requirements
 * @XST_PM_NO_ACCESS:	Access rights violation
 * @XST_PM_INVALID_NODE:	Does not apply to node passed as argument
 * @XST_PM_DOUBLE_REQ:	Duplicate request
 * @XST_PM_ABORT_SUSPEND:	Target has aborted suspend
 * @XST_PM_TIMEOUT:		timeout in communication with PMU
 * @XST_PM_NODE_USED:		node is already in use
 */
enum pm_ret_status {
    XST_PM_SUCCESS = 0,
    XST_PM_ARGS = 1,
    XST_PM_NOTSUPPORTED = 4,
    XST_PM_INVALID_PARAM = 15,
    XST_PM_INTERNAL = 2000,
    XST_PM_CONFLICT,
    XST_PM_NO_ACCESS,
    XST_PM_INVALID_NODE,
    XST_PM_DOUBLE_REQ,
    XST_PM_ABORT_SUSPEND,
    XST_PM_TIMEOUT,
    XST_PM_NODE_USED
};

/* IPI SMC function numbers enum definition and fids */
#define IPI_MAILBOX_FID(fid) ARM_SMCCC_CALL_VAL(ARM_SMCCC_FAST_CALL, \
                                                ARM_SMCCC_CONV_32,   \
                                                ARM_SMCCC_OWNER_SIP, \
                                                fid)
enum ipi_api_id {
    IPI_MAILBOX_OPEN = 0x1000,
    IPI_MAILBOX_RELEASE,
    IPI_MAILBOX_STATUS_ENQUIRY,
    IPI_MAILBOX_NOTIFY,
    IPI_MAILBOX_ACK,
    IPI_MAILBOX_ENABLE_IRQ,
    IPI_MAILBOX_DISABLE_IRQ,
};

enum pm_node_id {
	NODE_UNKNOWN = 0,
	NODE_APU,
	NODE_APU_0,
	NODE_APU_1,
	NODE_APU_2,
	NODE_APU_3,
	NODE_RPU,
	NODE_RPU_0,
	NODE_RPU_1,
	NODE_PLD,
	NODE_FPD,
	NODE_OCM_BANK_0,
	NODE_OCM_BANK_1,
	NODE_OCM_BANK_2,
	NODE_OCM_BANK_3,
	NODE_TCM_0_A,
	NODE_TCM_0_B,
	NODE_TCM_1_A,
	NODE_TCM_1_B,
	NODE_L2,
	NODE_GPU_PP_0,
	NODE_GPU_PP_1,
	NODE_USB_0,
	NODE_USB_1,
	NODE_TTC_0,
	NODE_TTC_1,
	NODE_TTC_2,
	NODE_TTC_3,
	NODE_SATA,
	NODE_ETH_0,
	NODE_ETH_1,
	NODE_ETH_2,
	NODE_ETH_3,
	NODE_UART_0,
	NODE_UART_1,
	NODE_SPI_0,
	NODE_SPI_1,
	NODE_I2C_0,
	NODE_I2C_1,
	NODE_SD_0,
	NODE_SD_1,
	NODE_DP,
	NODE_GDMA,
	NODE_ADMA,
	NODE_NAND,
	NODE_QSPI,
	NODE_GPIO,
	NODE_CAN_0,
	NODE_CAN_1,
	NODE_AFI,
	NODE_APLL,
	NODE_VPLL,
	NODE_DPLL,
	NODE_RPLL,
	NODE_IOPLL,
	NODE_DDR,
	NODE_IPI_APU,
	NODE_IPI_RPU_0,
	NODE_GPU,
	NODE_PCIE,
	NODE_PCAP,
	NODE_RTC,
	NODE_LPD,
	NODE_VCU,
	NODE_IPI_RPU_1,
	NODE_IPI_PL_0,
	NODE_IPI_PL_1,
	NODE_IPI_PL_2,
	NODE_IPI_PL_3,
	NODE_PL,
	NODE_MAX
};

enum pm_reset {
	PM_RESET_START = 999,
	PM_RESET_PCIE_CFG,
	PM_RESET_PCIE_BRIDGE,
	PM_RESET_PCIE_CTRL,
	PM_RESET_DP,
	PM_RESET_SWDT_CRF,
	PM_RESET_AFI_FM5,
	PM_RESET_AFI_FM4,
	PM_RESET_AFI_FM3,
	PM_RESET_AFI_FM2,
	PM_RESET_AFI_FM1,
	PM_RESET_AFI_FM0,
	PM_RESET_GDMA,
	PM_RESET_GPU_PP1,
	PM_RESET_GPU_PP0,
	PM_RESET_GPU,
	PM_RESET_GT,
	PM_RESET_SATA,
	PM_RESET_ACPU3_PWRON,
	PM_RESET_ACPU2_PWRON,
	PM_RESET_ACPU1_PWRON,
	PM_RESET_ACPU0_PWRON,
	PM_RESET_APU_L2,
	PM_RESET_ACPU3,
	PM_RESET_ACPU2,
	PM_RESET_ACPU1,
	PM_RESET_ACPU0,
	PM_RESET_DDR,
	PM_RESET_APM_FPD,
	PM_RESET_SOFT,
	PM_RESET_GEM0,
	PM_RESET_GEM1,
	PM_RESET_GEM2,
	PM_RESET_GEM3,
	PM_RESET_QSPI,
	PM_RESET_UART0,
	PM_RESET_UART1,
	PM_RESET_SPI0,
	PM_RESET_SPI1,
	PM_RESET_SDIO0,
	PM_RESET_SDIO1,
	PM_RESET_CAN0,
	PM_RESET_CAN1,
	PM_RESET_I2C0,
	PM_RESET_I2C1,
	PM_RESET_TTC0,
	PM_RESET_TTC1,
	PM_RESET_TTC2,
	PM_RESET_TTC3,
	PM_RESET_SWDT_CRL,
	PM_RESET_NAND,
	PM_RESET_ADMA,
	PM_RESET_GPIO,
	PM_RESET_IOU_CC,
	PM_RESET_TIMESTAMP,
	PM_RESET_RPU_R50,
	PM_RESET_RPU_R51,
	PM_RESET_RPU_AMBA,
	PM_RESET_OCM,
	PM_RESET_RPU_PGE,
	PM_RESET_USB0_CORERESET,
	PM_RESET_USB1_CORERESET,
	PM_RESET_USB0_HIBERRESET,
	PM_RESET_USB1_HIBERRESET,
	PM_RESET_USB0_APB,
	PM_RESET_USB1_APB,
	PM_RESET_IPI,
	PM_RESET_APM_LPD,
	PM_RESET_RTC,
	PM_RESET_SYSMON,
	PM_RESET_AFI_FM6,
	PM_RESET_LPD_SWDT,
	PM_RESET_FPD,
	PM_RESET_RPU_DBG1,
	PM_RESET_RPU_DBG0,
	PM_RESET_DBG_LPD,
	PM_RESET_DBG_FPD,
	PM_RESET_APLL,
	PM_RESET_DPLL,
	PM_RESET_VPLL,
	PM_RESET_IOPLL,
	PM_RESET_RPLL,
	PM_RESET_GPO3_PL_0,
	PM_RESET_GPO3_PL_1,
	PM_RESET_GPO3_PL_2,
	PM_RESET_GPO3_PL_3,
	PM_RESET_GPO3_PL_4,
	PM_RESET_GPO3_PL_5,
	PM_RESET_GPO3_PL_6,
	PM_RESET_GPO3_PL_7,
	PM_RESET_GPO3_PL_8,
	PM_RESET_GPO3_PL_9,
	PM_RESET_GPO3_PL_10,
	PM_RESET_GPO3_PL_11,
	PM_RESET_GPO3_PL_12,
	PM_RESET_GPO3_PL_13,
	PM_RESET_GPO3_PL_14,
	PM_RESET_GPO3_PL_15,
	PM_RESET_GPO3_PL_16,
	PM_RESET_GPO3_PL_17,
	PM_RESET_GPO3_PL_18,
	PM_RESET_GPO3_PL_19,
	PM_RESET_GPO3_PL_20,
	PM_RESET_GPO3_PL_21,
	PM_RESET_GPO3_PL_22,
	PM_RESET_GPO3_PL_23,
	PM_RESET_GPO3_PL_24,
	PM_RESET_GPO3_PL_25,
	PM_RESET_GPO3_PL_26,
	PM_RESET_GPO3_PL_27,
	PM_RESET_GPO3_PL_28,
	PM_RESET_GPO3_PL_29,
	PM_RESET_GPO3_PL_30,
	PM_RESET_GPO3_PL_31,
	PM_RESET_RPU_LS,
	PM_RESET_PS_ONLY,
	PM_RESET_PL,
	PM_RESET_END
};

extern bool zynqmp_eemi(struct cpu_user_regs *regs);

#endif /* __ASM_ARM_PLATFORMS_ZYNQMP_H */

/*
 * Local variables:
 * mode: C
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * End:
 */
