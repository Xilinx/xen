/*
 * xen/arch/arm/seattle.c
 *
 * AMD Seattle specific settings
 *
 * Suravee Suthikulpanit <suravee.suthikulpanit@amd.com>
 * Copyright (c) 2014 Advance Micro Devices Inc.
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
#include <asm/psci.h>

static const char * const seattle_dt_compat[] __initconst =
{
    "amd,seattle",
    NULL
};

/* Seattle firmware only implements PSCI handler for
 * system off and system reset at this point.
 * This is temporary until full PSCI-0.2 is supported.
 * Then, these function will be removed.
 */
static void seattle_system_reset(void)
{
    arm_smccc_smc(PSCI_0_2_FN32_SYSTEM_RESET, NULL);
}

static void seattle_system_off(void)
{
    arm_smccc_smc(PSCI_0_2_FN32_SYSTEM_OFF, NULL);
}

PLATFORM_START(seattle, "SEATTLE")
    .compatible = seattle_dt_compat,
    .reset      = seattle_system_reset,
    .poweroff   = seattle_system_off,
PLATFORM_END

/*
 * Local variables:
 * mode: C
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * End:
 */
