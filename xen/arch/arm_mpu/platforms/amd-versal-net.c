/*
 * SPDX-License-Identifier: GPL-2.0
 *
 * xen/arch/arm_mpu/platforms/amd-versal-net.c
 *
 * Copyright (C) 2023, Advanced Micro Devices, Inc. All Rights Reserved.
 */

#include <asm/platform.h>
#include <asm/io.h>

#define LPD_RST_TIMESTAMP                       0xEB5E035CU
#define XIOU_SCNTRS_BASEADDR                    0xEB5B0000U
#define XIOU_SCNTRS_CNT_CNTRL_REG_OFFSET        0x0U
#define XIOU_SCNTRS_CNT_CNTRL_REG_EN_MASK       0x1U
#define XIOU_SCNTRS_CNT_CNTRL_REG_EN            0x1U
#define XIOU_SCNTRS_FREQ_REG_OFFSET             0x20U

#define XPAR_PSU_CORTEXR52_0_TIMESTAMP_CLK_FREQ         100000000U

static int versal_net_init_time(void)
{
    /* Take LPD_TIMESTAMP out of reset, TODO: remove this once FW flow is up */
    writel(0, (volatile void __iomem *)LPD_RST_TIMESTAMP);

    /* Write frequency to System Time Stamp Generator Register */
    writel(XPAR_PSU_CORTEXR52_0_TIMESTAMP_CLK_FREQ,
           (volatile void __iomem *)
           (XIOU_SCNTRS_BASEADDR + XIOU_SCNTRS_FREQ_REG_OFFSET));

    /* Enable the timer/counter */
    writel(XIOU_SCNTRS_CNT_CNTRL_REG_EN,
           (volatile void __iomem *)
           (XIOU_SCNTRS_BASEADDR + XIOU_SCNTRS_CNT_CNTRL_REG_OFFSET));

    return 0;
}

static const char * const versal_net_dt_compat[] __initconst =
{
    "xlnx,versal-net",
    NULL
};

PLATFORM_START(versal_net, "XILINX VERSAL-NET")
    .compatible = versal_net_dt_compat,
    .init_time = versal_net_init_time,
PLATFORM_END
