/*
 * xen/arch/arm/platforms/xilinx-zynqmp.c
 *
 * Xilinx ZynqMP setup
 *
 * Copyright (c) 2015 Xilinx Inc.
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

#include <xen/config.h>
#include <asm/platform.h>
#include <xen/stdbool.h>
#include <xen/vmap.h>
#include <asm/io.h>
#include <asm/gic.h>

static uint32_t zynqmp_quirks(void)
{
    return PLATFORM_QUIRK_GIC_64K_STRIDE;
}

static const char * const zynqmp_dt_compat[] __initconst =
{
    "xlnx,zynqmp",
    NULL
};

PLATFORM_START(xgene_storm, "Xilinx ZynqMP")
    .compatible = zynqmp_dt_compat,
    .quirks = zynqmp_quirks,
PLATFORM_END

/*
 * Local variables:
 * mode: C
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * End:
 */
