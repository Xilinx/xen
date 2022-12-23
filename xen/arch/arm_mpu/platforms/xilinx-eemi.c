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

/*
 * Check if a domain has access to a clock control.
 * Note: domain has access to clock control if it has access to all the nodes
 * the are driven by the target clock.
 */
bool domain_has_clock_access(struct domain *d, u32 clk_id,
                 const struct pm_access *pm_node_access,
                 const uint32_t pm_node_access_size,
                 const struct pm_clk2node *pm_clk_node_map,
                 const uint32_t table_size)
{
   uint32_t i;
   bool access = is_hardware_domain(d);

   for ( i = 0; i < table_size && pm_clk_node_map[i].clk_idx <= clk_id; i++ )
   {
       if ( pm_clk_node_map[i].clk_idx == clk_id )
       {
           if ( !domain_has_node_access(d,
                                        pm_clk_node_map[i].dev_idx,
                                        pm_node_access,
                                        pm_node_access_size) )
               return false;

           access = true;
       }
   }

   return access;
}


/* Check if a clock id is valid */
bool clock_id_is_valid(u32 clk_id, u32 clk_end)
{
    if ( clk_id > clk_end )
        return false;

    return true;
}

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

#define VERSAL_PM_CLKNODE_PLL_MASK (0x80 << 20)
#define VERSAL_PM_CLK_SBCL_MASK    (0x3F << 20)    /* Clock subclass mask */
#define VERSAL_PM_CLK_SBCL_PLL     (0x01 << 20)    /* PLL subclass value */

static bool pll_in_bounds(u32 nodeid, u32 clk_end)
{
    /* ZynqMP */
    if ( clk_end == ZYNQMP_PM_CLK_END_IDX )
    {
        return ( (nodeid >= ZYNQMP_PM_DEV_APLL) &&
                 (nodeid <= ZYNQMP_PM_DEV_IOPLL) );
    }
    /* Versal */
    else
    {
        /* Check if node is PM clock node for PLL */
        return nodeid & VERSAL_PM_CLKNODE_PLL_MASK;
    }

    return false;
}

/* Check if a clock id belongs to pll type */
static bool clock_id_is_pll(u32 clk_id, u32 clk_end)
{
    /* ZynqMP */
    if ( clk_end == ZYNQMP_PM_CLK_END_IDX )
    {
        for ( int i = 0;
              ZYNQMP_PM_CLK_END_IDX != zynqmp_clock_id_plls[i];
              i++ )
        {
            if ( clk_id == zynqmp_clock_id_plls[i] )
                return true;
        }
    }
    /* Versal */
    else
    {
        if ( (clk_id & VERSAL_PM_CLK_SBCL_MASK ) == VERSAL_PM_CLK_SBCL_PLL )
            return true;
    }

    return false;
}

static bool is_clock_enabled(struct cpu_user_regs *regs)
{
    struct arm_smccc_res res1;

    arm_smccc_1_1_smc(EEMI_FID(PM_CLOCK_GETSTATE),
            get_user_reg(regs, 1),
            0,
            0,
            0,
            0,
            0,
            0,
            &res1);
    if ( (res1.a0 & 0xfff) != XST_PM_SUCCESS )
        return false;
    return !!(res1.a0 >> 32);
}

bool xilinx_eemi(struct cpu_user_regs *regs, const uint32_t fid,
                 uint32_t nodeid,
                 uint32_t pm_fn,
                 const struct pm_access *pm_node_access,
                 const uint32_t pm_node_access_size,
                 const struct pm_access *pm_rst_access,
                 const uint32_t pm_rst_access_size,
                 const struct pm_clk2node *pm_clock_node_map,
                 const uint32_t pm_clock_node_map_size,
                 const uint32_t clk_end)
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
    case EEMI_FID(PM_QUERY_DATA):
    case EEMI_FID(PM_REGISTER_NOTIFIER):
    case EEMI_FID(PM_GET_CALLBACK_DATA):
        goto forward_to_fw;

    case EEMI_FID(PM_CLOCK_GETSTATE):
    case EEMI_FID(PM_CLOCK_GETDIVIDER):
    case EEMI_FID(PM_CLOCK_GETPARENT):
        if ( !clock_id_is_valid(nodeid, clk_end) )
        {
            gprintk(XENLOG_WARNING, "xilinx-pm: fn=%u Invalid clock=%u\n",
                    pm_fn, nodeid);
            ret = XST_PM_INVALID_PARAM;
            goto done;
        }
        else
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
        if ( !is_hardware_domain(current->domain) )
        {
            gprintk(XENLOG_WARNING, "eemi: fn=%u No access\n", pm_fn);
            ret = XST_PM_NO_ACCESS;
            goto done;
        }
        goto forward_to_fw;

    /*
     * Based on the zynqmp_pmufw/src/pm_core.c, PM_IOCTL is implemented only
     * for Versal, not ZynqMP. However, linux on ZynqMP still issues PM_IOCTL.
     * As it is harmless, we have allowed it to go to the firmware. The return
     * payload of the firmware is ignored by linux.
     */
    case EEMI_FID(PM_IOCTL):
    {
        enum pm_ioctl_id id = get_user_reg(regs, 1) >> 32;

        if ( id == IOCTL_REGISTER_SGI )
        {
            ret = XST_PM_NOTSUPPORTED;
            goto done;
        }

        if ( !is_hardware_domain(current->domain) )
        {
            /*
             * This is allowed for domU as it tries to fetch some pll values
             * to configure the clocks.
             */
            if ( id == IOCTL_GET_PLL_FRAC_MODE )
            {
                goto forward_to_fw;
            }
            /*
             * This is allowed as domU tries to set them for configuring
             * mmc device. We check if domU has access to the mmc node.
             */
            else if ( ((id == IOCTL_SET_SD_TAPDELAY) ||
                       (id == IOCTL_SD_DLL_RESET)) &&
                      domain_has_node_access(current->domain, nodeid,
                                             pm_node_access,
                                             pm_node_access_size) )
            {
                goto forward_to_fw;
            }
            else
            {
                gprintk(XENLOG_WARNING, "eemi: fn=%u No access id = %d\n", pm_fn, id);
                ret = XST_PM_NO_ACCESS;
                goto done;
            }
        }
        goto forward_to_fw;
    }

    case EEMI_FID(PM_PLL_GET_PARAMETER):
    case EEMI_FID(PM_PLL_GET_MODE):
        if ( !pll_in_bounds(get_user_reg(regs, 1), clk_end) )
        {
            gprintk(XENLOG_WARNING, "xilinx-pm: fn=%u Invalid pll node %u\n",
                    pm_fn, nodeid);
            ret = XST_PM_INVALID_PARAM;
            goto done;
        }
        else
            goto forward_to_fw;

    case EEMI_FID(PM_PLL_SET_PARAMETER):
    case EEMI_FID(PM_PLL_SET_MODE):
        if ( !pll_in_bounds(get_user_reg(regs, 1), clk_end) )
        {
            gprintk(XENLOG_WARNING, "xilinx-pm: fn=%u Invalid pll node %u\n",
                    pm_fn, nodeid);
            ret = XST_PM_INVALID_PARAM;
            goto done;
        }
        if ( !domain_has_node_access(current->domain, nodeid,
                                     pm_node_access,
                                     pm_node_access_size) )
        {
            gprintk(XENLOG_WARNING, "xilinx-pm: fn=%u No access to pll=%u\n",
                    pm_fn, nodeid);
            ret = XST_PM_NO_ACCESS;
            goto done;
        }
        goto forward_to_fw;

    case EEMI_FID(PM_CLOCK_ENABLE):
        /*
         * First, check if the Clock is already enabled.
         *
         * Certain critical clocks are already enabled but the guest
         * might still request to enable them again, even if they are
         * related to devices they are not visible from the guest.
         */
        if ( is_clock_enabled(regs) )
        {
            ret = XST_PM_SUCCESS;
            goto done;
        }
    case EEMI_FID(PM_CLOCK_DISABLE):
    case EEMI_FID(PM_CLOCK_SETDIVIDER):
    case EEMI_FID(PM_CLOCK_SETPARENT):
        if ( !clock_id_is_valid(nodeid, clk_end) )
        {
            gprintk(XENLOG_WARNING, "xilinx-pm: fn=%u Invalid clock=%u\n",
                    pm_fn, nodeid);
            ret = XST_PM_INVALID_PARAM;
            goto done;
        }
        /*
         * Allow pll clock nodes to passthrough since there is no device binded to them
         */
        if ( clock_id_is_pll(nodeid, clk_end) )
        {
            goto forward_to_fw;
        }
        if ( !domain_has_clock_access(current->domain, nodeid,
                                      pm_node_access,
                                      pm_node_access_size,
                                      pm_clock_node_map,
                                      pm_clock_node_map_size) )

        {
            gprintk(XENLOG_WARNING, "xilinx-pm: fn=%u No access to clock=%u\n",
                    pm_fn, nodeid);
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
            gprintk(XENLOG_WARNING, "IPI mailbox: fn=%u No access\n", pm_fn);
            ret = XST_PM_NO_ACCESS;
            goto done;
        }
        goto forward_to_fw;

    default:
        if ( is_hardware_domain(current->domain) )
            goto forward_to_fw;
        gprintk(XENLOG_WARNING, "xilinx-pm: Unhandled PM Call: %u, domid=%u\n",
                fid, current->domain->domain_id);
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
