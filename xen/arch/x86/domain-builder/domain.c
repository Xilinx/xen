/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (C) 2024, Apertus Solutions, LLC
 */

#include <xen/domain.h>
#include <xen/sched.h>
#include <xen/err.h>
#include <xen/event.h>
#include <xen/grant_table.h>
#include <xen/init.h>
#include <xen/libelf.h>
#include <xen/nodemask.h>
#include <xen/param.h>
#include <xen/sched.h>
#include <xen/sizes.h>

#include <asm/bootinfo.h>
#include <asm/cpu-policy.h>
#include <asm/dom0_build.h>
#include <asm/domain-builder.h>
#include <asm/io_apic.h>
#include <asm/paging.h>
#include <asm/pv/shim.h>
#include <asm/spec_ctrl.h>

#include <public/bootfdt.h>
#include <public/domctl.h>

bool __initdata acpi_force;
static char __initdata acpi_param[10] = "";

static int __init cf_check parse_acpi_param(const char *s)
{
    /* Interpret the parameter for use within Xen. */
    if ( !parse_bool(s, NULL) )
    {
        disable_acpi();
    }
    else if ( !strcmp(s, "force") )
    {
        acpi_force = true;
        acpi_ht = 1;
        acpi_disabled = false;
    }
    else if ( !strcmp(s, "ht") )
    {
        if ( !acpi_force )
            disable_acpi();
        acpi_ht = 1;
    }
    else if ( !strcmp(s, "noirq") )
    {
        acpi_noirq_set();
    }
    else if ( !strcmp(s, "verbose") )
    {
        opt_acpi_verbose = true;
        return 0;
    }
    else
        return -EINVAL;

    /* Save the parameter so it can be propagated to domain0. */
    safe_strcpy(acpi_param, s);

    return 0;
}
custom_param("acpi", parse_acpi_param);

void __init alloc_dom_vcpus(struct domain *d, const cpumask_t *aff)
{
    for ( unsigned int i = 1; i < d->max_vcpus; i++ )
        vcpu_create(d, i);

    /* Set vcpu hard affinity after vcpu initialization */
    domain_vcpu_affinity(d, aff);

    domain_update_node_affinity(d);
}

static int __init alloc_dom_evtchn(
    const struct boot_domain *bd, domid_t remote_domid,
    evtchn_alloc_unbound_t *ec)
{
    int rc;

    ec->dom = bd->domid;
    ec->remote_dom = remote_domid;

    rc = evtchn_alloc_unbound(ec, 0);
    if ( rc )
    {
        printk(XENLOG_WARNING "Failed allocating event channel for %pd\n",
               bd->d);
        return rc;
    }

    return 0;
}

static int __init alloc_console_evtchn(
    struct boot_info *bi, struct boot_domain *bd)
{
    evtchn_alloc_unbound_t evtchn_req;
    int rc;

    if ( bd->console.be_domid == DOMID_INVALID )
    {
        printk(XENLOG_WARNING
               "backend for %pd console not constructed\n", bd->d);
        return -EINVAL;
    }

    if ( (rc = alloc_dom_evtchn(bd, bd->console.be_domid, &evtchn_req)) < 0 )
        return rc;

    bd->console.evtchn = evtchn_req.port;

    return 0;
}

static int __init alloc_xenstore_evtchn(struct boot_info *bi,
                                        struct boot_domain *bd)
{
    evtchn_alloc_unbound_t evtchn_req;
    int rc;

    if ( (rc = alloc_dom_evtchn(bd, bd->xenstore.be_domid, &evtchn_req)) < 0 )
        return rc;

    bd->xenstore.evtchn = evtchn_req.port;

    return 0;
}

/*
 * Calculate the maximum possible size of the dom0 cmdline.  Pieces of the
 * dom0 cmdline optionally come from the bootloader directly, from Xen's
 * cmdline (following " -- "), and individual Xen parameters are forwarded
 * too.
 */
static size_t __init domain_cmdline_size(const struct boot_info *bi,
                                         const struct boot_domain *bd)
{
    size_t s = 0;

    if ( bd->kernel->arch.cmdline_pa )
        s += strlen(__va(bd->kernel->arch.cmdline_pa));

    if ( bi->kextra )
        s += strlen(bi->kextra);

    /*
     * Certain parameters from the Xen command line may be added to the dom0
     * command line. Add additional space for the possible cases along with one
     * extra char to hold \0.
     */
    s += strlen(" noapic") + strlen(" acpi=") + sizeof(acpi_param) + 1;

    return s;
}

struct domain *__init arch_create_dom(struct boot_info *bi,
                                      struct boot_domain *bd)
{

    char *cmdline = NULL;
    size_t cmdline_size;
    struct domain *d;

    d = domain_create(bd->domid, &bd->create_cfg, bd->create_flags);
    if ( IS_ERR(d) )
        panic("Error creating d%u: %ld\n", bd->domid, PTR_ERR(d));

    init_dom_cpuid_policy(d);

    if ( bd->create_flags & CDF_hardware )
        dom0_set_affinity(d);

    if ( !vcpu_create(d, 0) )
        panic("Error creating %pdv0\n", d);

    cmdline_size = domain_cmdline_size(bi, bd);
    if ( cmdline_size )
    {
        if ( !(cmdline = xzalloc_array(char, cmdline_size)) )
            panic("Error allocating cmdline buffer for %pd\n", d);

        if ( bd->kernel->arch.cmdline_pa )
            strlcpy(cmdline, __va(bd->kernel->arch.cmdline_pa), cmdline_size);

        /* Params from Xen cmd line apply only to control/hardware doms */
        if ( bd->create_flags & CDF_hardware )
        {
            if ( bi->kextra )
                /* kextra always includes exactly one leading space. */
                strlcat(cmdline, bi->kextra, cmdline_size);

            /* Append any extra parameters. */
            if ( skip_ioapic_setup && !strstr(cmdline, "noapic") )
                strlcat(cmdline, " noapic", cmdline_size);

            if ( (strlen(acpi_param) == 0) && acpi_disabled )
            {
                printk("ACPI is disabled, notifying Domain 0 (acpi=off)\n");
                safe_strcpy(acpi_param, "off");
            }

            if ( (strlen(acpi_param) != 0) && !strstr(cmdline, "acpi=") )
            {
                strlcat(cmdline, " acpi=", cmdline_size);
                strlcat(cmdline, acpi_param, cmdline_size);
            }
        }

        bd->kernel->arch.cmdline_pa = 0;
        bd->cmdline = cmdline;
    }

    bd->d = d;
    if ( !(bd->create_cfg.flags & XEN_DOMCTL_CDF_xs_domain) )
        alloc_xenstore_evtchn(bi, bd);

    if ( !(bd->create_flags & CDF_hardware) )
        alloc_console_evtchn(bi, bd);

    if ( construct_dom(bd) != 0 )
        panic("Could not construct domain 0\n");

    bd->cmdline = NULL;
    xfree(cmdline);

    return d;
}

/*
 * Local variables:
 * mode: C
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * tab-width: 4
 * indent-tabs-mode: nil
 * End:
 */
