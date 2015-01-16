/*
 * xen/arch/arm/platform.c
 *
 * Helpers to execute platform specific code.
 *
 * Julien Grall <julien.grall@linaro.org>
 * Copyright (C) 2013 Linaro Limited.
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
#include <xen/device_tree.h>
#include <xen/init.h>
#include <asm/psci.h>

extern const struct platform_desc _splatform[], _eplatform[];

/* Pointer to the current platform description */
static const struct platform_desc *platform;


static bool_t __init platform_is_compatible(const struct platform_desc *plat)
{
    const char *const *compat;

    if ( !plat->compatible )
        return 0;

    for ( compat = plat->compatible; *compat; compat++ )
    {
        if ( dt_machine_is_compatible(*compat) )
            return 1;
    }

    return 0;
}

/* List of possible platform */
static void dump_platform_table(void)
{
    const struct platform_desc *p;

    printk("Available platform support:\n");

    for ( p = _splatform; p != _eplatform; p++ )
        printk("    - %s\n", p->name);
}

void __init platform_init(void)
{
    int res = 0;

    ASSERT(platform == NULL);

    /* Looking for the platform description */
    for ( platform = _splatform; platform != _eplatform; platform++ )
    {
        if ( platform_is_compatible(platform) )
            break;
    }

    /* We don't have specific operations for this platform */
    if ( platform == _eplatform )
    {
        /* TODO: dump DT machine compatible node */
        printk(XENLOG_WARNING "WARNING: Unrecognized/unsupported device tree "
              "compatible list\n");
        dump_platform_table();
        platform = NULL;
    }
    else
        printk(XENLOG_INFO "Platform: %s\n", platform->name);

    if ( platform && platform->init )
        res = platform->init();

    if ( res )
        panic("Unable to initialize the platform");
}

int __init platform_init_time(void)
{
    int res = 0;

    if ( platform && platform->init_time )
        res = platform->init_time();

    return res;
}

int __init platform_specific_mapping(struct domain *d)
{
    int res = 0;

    if ( platform && platform->specific_mapping )
        res = platform->specific_mapping(d);

    return res;
}

#ifdef CONFIG_ARM_32
int __init platform_cpu_up(int cpu)
{
    if ( psci_ver )
        return call_psci_cpu_on(cpu);

    if ( platform && platform->cpu_up )
        return platform->cpu_up(cpu);

    return -ENODEV;
}

int __init platform_smp_init(void)
{
    if ( platform && platform->smp_init )
        return platform->smp_init();

    return 0;
}
#endif

void platform_reset(void)
{
    if ( platform && platform->reset )
        platform->reset();
}

void platform_poweroff(void)
{
    if ( platform && platform->poweroff )
        platform->poweroff();
}

bool_t platform_has_quirk(uint32_t quirk)
{
    uint32_t quirks = 0;

    if ( platform && platform->quirks )
        quirks = platform->quirks();

    return !!(quirks & quirk);
}

bool_t platform_device_is_blacklisted(const struct dt_device_node *node)
{
    const struct dt_device_match *blacklist = NULL;

    if ( platform && platform->blacklist_dev )
        blacklist = platform->blacklist_dev;

    return (dt_match_node(blacklist, node) != NULL);
}

void platform_dom0_gnttab(paddr_t *start, paddr_t *size)
{
    if ( platform && platform->dom0_gnttab_size )
    {
        *start = platform->dom0_gnttab_start;
        *size = platform->dom0_gnttab_size;
    }
    else
    {
        *start = 0xb0000000;
        *size = 0x20000;
    }
}

/*
 * Local variables:
 * mode: C
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * End:
 */
