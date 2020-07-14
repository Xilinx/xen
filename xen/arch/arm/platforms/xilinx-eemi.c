/*
 * xen/arch/arm/platforms/xilinx-eemi.c
 *
 * Xilinx Common EEMI API
 *
 * Copyright (c) 2020 Xilinx Inc.
 * Written by Ben Levinsky <ben.levinsky@xilinx.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms and conditions of the GNU General Public
 * License, version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <asm/regs.h>
#include <xen/iocap.h>
#include <xen/sched.h>
#include <asm/smccc.h>
#include <asm/platforms/xilinx-eemi.h>

bool pm_check_access(const struct pm_access *acl, struct domain *d, u32 idx)
{
    unsigned long mfn;

    if ( acl[idx].hwdom_access && is_hardware_domain(d) )
        return true;

    mfn = paddr_to_pfn(acl[idx].addr);
    if ( !mfn )
        return false;

    return iomem_access_permitted(d, mfn, mfn);
}

/* Check if a domain has access to a node.  */
bool domain_has_node_access(struct domain *d, const u32 node,
                            const struct pm_access *pm_node_access,
                            const uint32_t table_size)
{
    if ( node >= table_size )
        return false;

    return pm_check_access(pm_node_access, d, node);
}

bool xilinx_eemi(struct cpu_user_regs *regs, const uint32_t fid,
                 uint32_t nodeid,
                 uint32_t pm_fn,
                 const struct pm_access *pm_node_access,
                 const uint32_t pm_node_access_size,
                 const struct pm_access *pm_rst_access,
                 const uint32_t pm_rst_access_size)
{
    struct arm_smccc_res res;
    enum pm_ret_status ret;

    switch ( fid )
    {
    /* Mandatory SMC32 functions. */
    case ARM_SMCCC_CALL_COUNT_FID(SIP):
    case ARM_SMCCC_CALL_UID_FID(SIP):
    case ARM_SMCCC_REVISION_FID(SIP):
        goto forward_to_fw;
    /*
     * We can't allow CPUs to suspend without Xen knowing about it.
     * We accept but ignore the request and wait for the guest to issue
     * a WFI or PSCI call which Xen will trap and act accordingly upon.
     */
    case EEMI_FID(PM_SELF_SUSPEND):
        ret = XST_PM_SUCCESS;
        goto done;

    /* These calls are safe and always allowed.  */
    case EEMI_FID(PM_GET_TRUSTZONE_VERSION):
    case EEMI_FID(PM_GET_API_VERSION):
    case EEMI_FID(PM_GET_CHIPID):
        goto forward_to_fw;

    case EEMI_FID(PM_GET_NODE_STATUS):
    /* API for PUs.  */
    case EEMI_FID(PM_REQ_SUSPEND):
    case EEMI_FID(PM_FORCE_POWERDOWN):
    case EEMI_FID(PM_ABORT_SUSPEND):
    case EEMI_FID(PM_REQ_WAKEUP):
    case EEMI_FID(PM_SET_WAKEUP_SOURCE):
    /* API for slaves.  */
    case EEMI_FID(PM_REQ_NODE):
    case EEMI_FID(PM_RELEASE_NODE):
    case EEMI_FID(PM_SET_REQUIREMENT):
    case EEMI_FID(PM_SET_MAX_LATENCY):
        if ( !domain_has_node_access(current->domain,
                                     nodeid, pm_node_access,
                                     pm_node_access_size) )
        {
            printk("xilinx-pm: fn=0x%04x No access to node 0x%08x\n", pm_fn, nodeid);
            ret = XST_PM_NO_ACCESS;
            goto done;
        }
        goto forward_to_fw;

    /* Exclusive to the hardware domain.  */
    case EEMI_FID(PM_INIT):
    case EEMI_FID(PM_SET_CONFIGURATION):
    case EEMI_FID(PM_FPGA_LOAD):
    case EEMI_FID(PM_FPGA_GET_STATUS):
    case EEMI_FID(PM_SECURE_SHA):
    case EEMI_FID(PM_SECURE_RSA):
    case EEMI_FID(PM_PINCTRL_SET_FUNCTION):
    case EEMI_FID(PM_PINCTRL_REQUEST):
    case EEMI_FID(PM_PINCTRL_RELEASE):
    case EEMI_FID(PM_PINCTRL_GET_FUNCTION):
    case EEMI_FID(PM_PINCTRL_CONFIG_PARAM_GET):
    case EEMI_FID(PM_PINCTRL_CONFIG_PARAM_SET):
    case EEMI_FID(PM_IOCTL):
    case EEMI_FID(PM_QUERY_DATA):
        if ( !is_hardware_domain(current->domain) )
        {
            gprintk(XENLOG_WARNING, "eemi: fn=%u No access", pm_fn);
            ret = XST_PM_NO_ACCESS;
            goto done;
        }
        goto forward_to_fw;

    case EEMI_FID(PM_RESET_ASSERT):
    case EEMI_FID(PM_RESET_GET_STATUS):
        if ( !domain_has_node_access(current->domain,
                                     nodeid, pm_rst_access,
                                     pm_rst_access_size) )
        {
            gprintk(XENLOG_WARNING,
                    "xilinx-pm: fn=%u No access to reset %u\n", pm_fn, nodeid);
            ret = XST_PM_NO_ACCESS;
            goto done;
        }
        goto forward_to_fw;

    case EEMI_FID(PM_CLOCK_SETRATE):
    case EEMI_FID(PM_CLOCK_GETRATE):
        ret = XST_PM_NOTSUPPORTED;
        goto done;

    /* These calls are never allowed.  */
    case EEMI_FID(PM_SYSTEM_SHUTDOWN):
        ret = XST_PM_NO_ACCESS;
        goto done;

    case IPI_MAILBOX_FID(IPI_MAILBOX_OPEN):
    case IPI_MAILBOX_FID(IPI_MAILBOX_RELEASE):
    case IPI_MAILBOX_FID(IPI_MAILBOX_STATUS_ENQUIRY):
    case IPI_MAILBOX_FID(IPI_MAILBOX_NOTIFY):
    case IPI_MAILBOX_FID(IPI_MAILBOX_ACK):
    case IPI_MAILBOX_FID(IPI_MAILBOX_ENABLE_IRQ):
    case IPI_MAILBOX_FID(IPI_MAILBOX_DISABLE_IRQ):
        if ( !is_hardware_domain(current->domain) )
        {
            gprintk(XENLOG_WARNING, "IPI mailbox: fn=%u No access", pm_fn);
            ret = XST_PM_NO_ACCESS;
            goto done;
        }
        goto forward_to_fw;

    default:
        gprintk(XENLOG_WARNING, "xilinx-pm: Unhandled PM Call: %u\n", fid);
        return false;
    }

forward_to_fw:
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

done:
    set_user_reg(regs, 0, ret);
    return true;
}

/*
 * Local variables:
 * mode: C
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * End:
 */
