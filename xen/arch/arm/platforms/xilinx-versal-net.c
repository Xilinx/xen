/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * xen/arch/arm/platforms/xilinx-versal-net.c
 *
 * Xilinx Versal-net setup
 *
 * Copyright (C) 2023, Advanced Micro Devices, Inc. All Rights Reserved.
 *
 */

#include <asm/platform.h>
#include <asm/platforms/xilinx-eemi.h>
#include <asm/platforms/xilinx-versal-net-eemi.h>
#include <asm/smccc.h>

static const char * const versal_net_dt_compat[] __initconst =
{
    "xlnx,versal-net",
    NULL
};

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

static int versal_net_init(void)
{
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

    return 0;
}

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

PLATFORM_START(xilinx_versal_net, "Xilinx Versal-net")
    .compatible = versal_net_dt_compat,
    .init = versal_net_init,
    .smc = versal_net_smc,
    .sgi = versal_net_sgi,
PLATFORM_END

/*
 * Local variables:
 * mode: C
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * End:
 */
