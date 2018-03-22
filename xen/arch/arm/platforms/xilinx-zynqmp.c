/*
 * xen/arch/arm/platforms/xilinx-zynqmp.c
 *
 * Xilinx ZynqMP setup
 *
 * Copyright (c) 2016 Xilinx Inc.
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

#include <asm/platform.h>
#include <asm/platforms/xilinx-zynqmp-eemi.h>

#define FID_MASK      0xf000u
#define IPI_FID_VALUE 0x1000u

#define is_ipi_fid(_fid) (((_fid) & FID_MASK) == IPI_FID_VALUE)

static const char * const zynqmp_dt_compat[] __initconst =
{
    "xlnx,zynqmp",
    NULL
};

bool zynqmp_hvc(struct cpu_user_regs *regs)
{
    register_t ret[4] = { 0 };

    if ( is_ipi_fid(regs->x0) ) {
        /*
         * The IPI FW API is currently only available to dom0.
         * FIXME: Virtualize the IPI firmware API so that
         * unprivileged guests can use IPIs aswell.
         */
        if ( !is_hardware_domain(current->domain) )
            return false;

        call_smcc64(regs->x0, regs->x1, regs->x2, regs->x3,
                    regs->x4, regs->x5, regs->x6, ret);
    } else {
        if ( !zynqmp_eemi_mediate(regs->x0, regs->x1, regs->x2, regs->x3,
                                  regs->x4, regs->x5, regs->x6, ret) )
            return false;
    }

    /* Transfer return values into guest registers.  */
    regs->x0 = ret[0];
    regs->x1 = ret[1];
    regs->x2 = ret[2];
    regs->x3 = ret[3];
    return true;
}

PLATFORM_START(xilinx_zynqmp, "Xilinx ZynqMP")
    .compatible = zynqmp_dt_compat,
    .hvc = zynqmp_hvc,
PLATFORM_END

/*
 * Local variables:
 * mode: C
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * End:
 */
