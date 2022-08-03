/*
 * xen/arch/arm/platforms/xilinx-versal.c
 *
 * Xilinx Versal setup
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

#include <asm/platform.h>
#include <asm/platforms/xilinx-eemi.h>
#include <asm/platforms/xilinx-versal-eemi.h>
#include <asm/smccc.h>

static const char * const versal_dt_compat[] __initconst =
{
    "xlnx,versal",
    NULL
};

static bool versal_smc(struct cpu_user_regs *regs)
{
    if ( !cpus_have_const_cap(ARM_SMCCC_1_1) )
    {
        printk_once(XENLOG_WARNING
                    "ZynqMP firmware Error: no SMCCC 1.1 support. Disabling firmware calls\n");

        return false;
    }

	return versal_eemi(regs);
}

static int versal_init(void)
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
        printk("Versal firmware Error registering SGI\n");
        return res.a0;
    }

    return 0;
}

static bool versal_sgi(void)
{
    struct domain *d;

    for_each_domain( d )
    {
        if ( d->arch.firmware_sgi != 0 )
            vgic_inject_irq(d, d->vcpu[0], d->arch.firmware_sgi, true);
    }
    return true;
}

PLATFORM_START(xilinx_versal, "Xilinx Versal")
    .compatible = versal_dt_compat,
    .init = versal_init,
    .smc = versal_smc,
    .sgi = versal_sgi,
PLATFORM_END

/*
 * Local variables:
 * mode: C
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * End:
 */
