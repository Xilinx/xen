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

#define FID_MASK      0xf000u
#define IPI_FID_VALUE 0x1000u

#define is_ipi_fid(_fid) (((_fid) & FID_MASK) == IPI_FID_VALUE)

static const char * const versal_dt_compat[] __initconst =
{
    "xlnx,versal",
    NULL
};

bool versal_hvc(struct cpu_user_regs *regs)
{
    register_t ret[4] = { 0 };

    if ( is_ipi_fid(regs->x0) ) {
        if ( !is_hardware_domain(current->domain) )
            return false;

        call_smcc64(regs->x0, regs->x1, regs->x2, regs->x3,
                    regs->x4, regs->x5, regs->x6, ret);
    } else {
        if ( !versal_eemi_mediate(regs->x0, regs->x1, regs->x2, regs->x3,
                                  regs->x4, regs->x5, regs->x6, ret) )
            return false;
    }

    regs->x0 = ret[0];
    regs->x1 = ret[1];
    regs->x2 = ret[2];
    regs->x3 = ret[3];
    return true;
}

PLATFORM_START(xilinx_versal, "Xilinx Versal")
    .compatible = versal_dt_compat,
    .hvc = versal_hvc,
PLATFORM_END

/*
 * Local variables:
 * mode: C
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * End:
 */
