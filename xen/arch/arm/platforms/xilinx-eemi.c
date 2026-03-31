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
#include <xen/llc-coloring.h>
#include <xen/sched.h>
#include <asm/smccc.h>
#include <asm/platforms/xilinx-eemi.h>
#include <xen/mm.h>
#include <asm/page.h>
#include <asm/guest_access.h>

static DEFINE_SPINLOCK(eemi_bounce_lock);

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

/*
 * Pass EEMI call to firmware with GPA translated to MA. This is because when
 * LLC coloring is enabled, hwdom is no longer 1:1 mapped.
 *
 * EEMI calls passing addresses (for now only the ones tested on ZynqMP):
 * PM_SECURE_SHA - illformed
 * PM_SECURE_AES - illformed
 * PM_SECURE_RSA - illformed
 * PM_FPGA_LOAD
 * PM_FPGA_READ
 *
 * PM_FPGA_LOAD passes bitstram size, so we need to create a bounce buffer.
 *
 * Illformed - upper 32-bits stored in lower 32-bits of the register.
 */
static bool eemi_translate(struct cpu_user_regs *regs, enum pm_api_id api_id)
{
    struct domain *d = current->domain;
    register_t addr, guest_addr;
    paddr_t maddr = 0;
    struct arm_smccc_res res;
    bool illformed = false;
    size_t buffer_size = 0;
    void *bounce_vaddr = NULL;
    unsigned int bounce_order = 0;
    int ret;
    mfn_t mfn;

    if ( !is_hardware_domain(d) || !llc_coloring_enabled )
        return false;

    switch ( api_id )
    {
    case PM_SECURE_AES:
    case PM_SECURE_RSA:
    case PM_SECURE_SHA:
        illformed = true;
        break;
    case PM_FPGA_LOAD:
        buffer_size = (uint32_t)get_user_reg(regs, 2);
        break;
    case PM_FPGA_READ:
        break;
    default:
        return false;
    }

    if ( !illformed )
        guest_addr = get_user_reg(regs, 1);
    else
        guest_addr = ((register_t)get_user_reg(regs, 1) << 32) |
                     (get_user_reg(regs, 1) >> 32);

    /* Some calls pass 0 as addr to denote something else, don't translate */
    if ( !guest_addr )
    {
        addr = guest_addr;
        goto done;
    }

    /*
     * Use bounce buffer for multi-page FPGA_LOAD.
     * For LLC domains, GPA != MA and the contiguous buffers in guest address
     * space are not contiguous in PA (with LLC max order for domheap is 0).
     */
    if ( (api_id == PM_FPGA_LOAD) && (buffer_size > PAGE_SIZE) )
    {
        unsigned long nr_pages = PFN_UP(buffer_size);

        spin_lock(&eemi_bounce_lock);

        bounce_order = get_order_from_pages(nr_pages);
        bounce_vaddr = alloc_xenheap_pages(bounce_order, MEMF_bits(32));
        if ( !bounce_vaddr )
        {
            spin_unlock(&eemi_bounce_lock);
            printk(XENLOG_ERR "EEMI: Failed to allocate %zu byte bounce buffer\n",
                   buffer_size);
            return false;
        }

        maddr = virt_to_maddr(bounce_vaddr);

        /* Copy guest data to bounce buffer */
        ret = access_guest_memory_by_gpa(d, guest_addr, bounce_vaddr,
                                         buffer_size, false);
        if ( ret )
        {
            free_xenheap_pages(bounce_vaddr, bounce_order);
            spin_unlock(&eemi_bounce_lock);
            printk(XENLOG_ERR "EEMI: Failed to copy from guest buffer: %d\n",
                   ret);
            return false;
        }

        clean_dcache_va_range(bounce_vaddr, buffer_size);
    }
    else
    {
        /* Single page or small buffer - direct translation */
        mfn = gfn_to_mfn(d, gaddr_to_gfn(guest_addr));
        if ( !mfn_valid(mfn) )
            return false;

        maddr = mfn_to_maddr(mfn) + (guest_addr & ~PAGE_MASK);
    }

    if ( illformed )
        addr = ((register_t)maddr << 32) | (maddr >> 32);
    else
        addr = maddr;

done:
    arm_smccc_1_1_smc(get_user_reg(regs, 0),
                      addr,
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

    if ( bounce_vaddr )
    {
        free_xenheap_pages(bounce_vaddr, bounce_order);
        spin_unlock(&eemi_bounce_lock);
    }

    return true;
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

    if ( IS_ENABLED(CONFIG_LLC_COLORING) && eemi_translate(regs, pm_fn) )
        return true;

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
    case EEMI_FID(PM_FEATURE_CHECK):
    case EEMI_FID(TF_A_PM_FEATURE_CHECK):
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
    case EEMI_FID(PM_FPGA_READ):
    case EEMI_FID(PM_SECURE_SHA):
    case EEMI_FID(PM_SECURE_RSA):
    case EEMI_FID(PM_PINCTRL_SET_FUNCTION):
    case EEMI_FID(PM_PINCTRL_REQUEST):
    case EEMI_FID(PM_PINCTRL_RELEASE):
    case EEMI_FID(PM_PINCTRL_GET_FUNCTION):
    case EEMI_FID(PM_PINCTRL_CONFIG_PARAM_GET):
    case EEMI_FID(PM_PINCTRL_CONFIG_PARAM_SET):
    case EEMI_FID(PM_SECURE_AES):
    case EEMI_FID(PM_FPGA_GET_VERSION):
    case EEMI_FID(PM_FPGA_GET_FEATURE_LIST):
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

    /*
     * To avoid the need for guests to specify the clk_ignore_unused property,
     * we ignore requests to disable the clock. This prevents one domain from
     * disabling clocks that might be in use by another domain, potentially
     * causing issues if the latter is already utilizing a device or if the
     * firmware + clock controller nodes have not been passthroughed (in such
     * case there would be no PM_CLOCK_ENABLE call).
     */
    case EEMI_FID(PM_CLOCK_DISABLE):
        ret = XST_PM_SUCCESS;
        goto done;

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
        if ( clock_id_is_pll(get_user_reg(regs, 1), clk_end) )
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
        if ( !domain_has_node_access(current->domain,
                                     ZYNQMP_PM_DEV_IPI_APU,
                                     pm_node_access,
                                     pm_node_access_size) )
        {
            gprintk(XENLOG_WARNING, "IPI mailbox: fn=%u No access\n", pm_fn);
            ret = XST_PM_NO_ACCESS;
            goto done;
        }
        goto forward_to_fw;

    default:
        gprintk(XENLOG_WARNING, "xilinx-pm: Unhandled PM Call: %u, domid=%u\n",
                fid, current->domain->domain_id);

        if ( is_hardware_domain(current->domain) )
            goto forward_to_fw;

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
