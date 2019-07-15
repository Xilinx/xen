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
#include <asm/platforms/xilinx-zynqmp-eemi.h>

static const char * const versal_dt_compat[] __initconst =
{
    "xlnx,versal",
    NULL
};

static bool versal_smc(struct cpu_user_regs *regs)
{
    struct arm_smccc_res res;

    if ( !cpus_have_const_cap(ARM_SMCCC_1_1) )
    {
        printk_once(XENLOG_WARNING
                    "ZynqMP firmware Error: no SMCCC 1.1 support. Disabling firmware calls\n");

        return false;
    }

    if ( !is_hardware_domain(current->domain) )
        return false;

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
}

PLATFORM_START(xilinx_versal, "Xilinx Versal")
    .compatible = versal_dt_compat,
    .smc = versal_smc,
PLATFORM_END

/*
 * Local variables:
 * mode: C
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * End:
 */
