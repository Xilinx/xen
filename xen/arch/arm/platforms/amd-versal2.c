/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * xen/arch/arm/platforms/amd-versal2.c
 *
 * AMD Versal2 setup
 *
 * Copyright (C) 2025, Advanced Micro Devices, Inc. All Rights Reserved.
 */

#include <asm/platform.h>
#include <asm/platforms/amd-versal2-eemi.h>
#include <asm/platforms/xilinx-eemi.h>
#include <asm/smccc.h>

static const char * const versal2_dt_compat[] __initconst =
{
    "amd,versal2",
    NULL
};

static bool versal2_smc(struct cpu_user_regs *regs)
{
    if ( !cpus_have_const_cap(ARM_SMCCC_1_1) )
    {
        printk_once(XENLOG_WARNING
                    "Versal2 firmware: Error no SMCCC 1.1 support. Disabling firmware calls\n");

        return false;
    }

    return versal2_eemi(regs);
}

static int versal2_init(void)
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
        printk("Versal2 firmware: Error registering SGI\n");
        return res.a0;
    }

    return 0;
}

static bool versal2_sgi(void)
{
    struct domain *d;

    for_each_domain( d )
    {
        if ( d->arch.firmware_sgi != 0 )
            vgic_inject_irq(d, d->vcpu[0], d->arch.firmware_sgi, true);
    }
    return true;
}

PLATFORM_START(xilinx_versal2, "AMD Versal2")
    .compatible = versal2_dt_compat,
    .init = versal2_init,
    .smc = versal2_smc,
    .sgi = versal2_sgi,
PLATFORM_END

/*
 * Local variables:
 * mode: C
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * End:
 */
