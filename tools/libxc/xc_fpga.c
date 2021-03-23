/*
 *
 * FPGA control functions.
 * Copyright (C) 2021 Xilinx Inc.
 * Author Vikram Garhwal <fnu.vikram@xilinx.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation;
 * version 2.1 of the License.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; If not, see <http://www.gnu.org/licenses/>.
 */

#include "xc_bitops.h"
#include "xc_private.h"
#include <xen/hvm/hvm_op.h>
#include <libfdt.h>

int xc_domain_add_fpga(xc_interface *xch, void *pfdt, int pfdt_size)
{
    int err;
    DECLARE_DOMCTL;

    DECLARE_HYPERCALL_BOUNCE(pfdt, pfdt_size, XC_HYPERCALL_BUFFER_BOUNCE_IN);

    if ( (err = xc_hypercall_bounce_pre(xch, pfdt)) )
        goto err;

    domctl.cmd = XEN_DOMCTL_addfpga;
    /* Adding the device to hardware domain by default. */
    domctl.domain = 0;
    domctl.u.fpga_add_dt.pfdt_size = pfdt_size;

    set_xen_guest_handle(domctl.u.fpga_add_dt.pfdt, pfdt);

    if ( (err = do_domctl(xch, &domctl)) != 0 )
        PERROR("%s failed\n", __func__);

err:
    xc_hypercall_bounce_post(xch, pfdt);

    return err;
}

int xc_domain_del_fpga(xc_interface *xch, char *full_dt_node_path)
{
    int err;
    DECLARE_DOMCTL;
    size_t size = strlen(full_dt_node_path) + 1;

    DECLARE_HYPERCALL_BOUNCE(full_dt_node_path, size,
                             XC_HYPERCALL_BUFFER_BOUNCE_IN);

    if ( (err = xc_hypercall_bounce_pre(xch, full_dt_node_path)) )
        goto err;

    domctl.cmd = XEN_DOMCTL_delfpga;
    /*
     * Remove the device from the dt_host, setting hardware domain by
     * default.
     */
    domctl.domain = 0;
    domctl.u.fpga_del_dt.size = size;

    set_xen_guest_handle(domctl.u.fpga_del_dt.full_dt_node_path,
                         full_dt_node_path);

    if ( (err = do_domctl(xch, &domctl)) != 0 )
        PERROR("%s failed\n", __func__);

err:
    xc_hypercall_bounce_post(xch, full_dt_node_path);

    return err;
}
