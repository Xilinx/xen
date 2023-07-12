/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * xen/arch/arm_mpu/platforms/xilinx-versal-net.c
 *
 * Xilinx Versal-net setup
 *
 * Copyright (C) 2023, Advanced Micro Devices, Inc. All Rights Reserved.
 */

#include <asm/platform.h>
#include <asm/io.h>
#ifndef CONFIG_HAS_MPU
#include <asm/platforms/xilinx-eemi.h>
#include <asm/platforms/xilinx-versal-net-eemi.h>
#include <asm/smccc.h>
#endif /* !CONFIG_HAS_MPU */

#define LPD_RST_TIMESTAMP                       0xEB5E035CU
#define XIOU_SCNTRS_BASEADDR                    0xEB5B0000U
#define XIOU_SCNTRS_CNT_CNTRL_REG_OFFSET        0x0U
#define XIOU_SCNTRS_CNT_CNTRL_REG_EN_MASK       0x1U
#define XIOU_SCNTRS_CNT_CNTRL_REG_EN            0x1U
#define XIOU_SCNTRS_FREQ_REG_OFFSET             0x20U

#define XPAR_PSU_CORTEXR52_0_TIMESTAMP_CLK_FREQ         100000000U

static const char * const versal_net_dt_compat[] __initconst =
{
    "xlnx,versal-net",
    NULL
};

#ifndef CONFIG_HAS_MPU
static bool versal_net_smc(struct cpu_user_regs *regs)
{
    if ( !cpus_have_const_cap(ARM_SMCCC_1_1) )
    {
        printk_once(XENLOG_WARNING
                    "Versal-net firmware Error: no SMCCC 1.1 support. Disabling firmware calls\n");

        return false;
    }

	return versal_net_eemi(regs);
}
#endif /* !CONFIG_HAS_MPU */

static int versal_net_init(void)
{
#ifndef CONFIG_HAS_MPU
    struct arm_smccc_res res;

    /*
     * Assuming the SGI number is on the second register
     */
    arm_smccc_1_1_smc(EEMI_FID(TF_A_PM_REGISTER_SGI),
                      GIC_SGI_FIRMWARE,
                      0,
                      0,
                      0,
                      0,
                      0,
                      0,
                      &res);
    if ( res.a0 != XST_PM_SUCCESS )
    {
        printk("Versal-net firmware Error registering SGI\n");
        return res.a0;
    }
#else /* CONFIG_HAS_MPU */

    WRITE_SYSREG(XPAR_PSU_CORTEXR52_0_TIMESTAMP_CLK_FREQ, CNTFRQ_EL0);

#endif /* CONFIG_HAS_MPU */

    return 0;
}

#ifndef CONFIG_HAS_MPU
static bool versal_net_sgi(void)
{
    struct domain *d;

    for_each_domain( d )
    {
        if ( d->arch.firmware_sgi != 0 )
            vgic_inject_irq(d, d->vcpu[0], d->arch.firmware_sgi, true);
    }
    return true;
}
#else /* CONFIG_HAS_MPU */

static __init int versal_net_init_time(void)
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
#endif /* CONFIG_HAS_MPU */

PLATFORM_START(xilinx_versal_net, "Xilinx Versal-net")
    .compatible = versal_net_dt_compat,
    .init = versal_net_init,
#ifdef CONFIG_HAS_MPU
    .init_time = versal_net_init_time,
#else /* !CONFIG_HAS_MPU */
    .smc = versal_net_smc,
    .sgi = versal_net_sgi,
#endif /* !CONFIG_HAS_MPU */
PLATFORM_END

/*
 * Local variables:
 * mode: C
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * End:
 */
