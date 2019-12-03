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
