/*
 *
 * Overlay control functions.
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

int xc_dt_overlay(xc_interface *xch, void *overlay_fdt, int overlay_fdt_size,
                  uint8_t op)
{
    int err;
    DECLARE_SYSCTL;

    DECLARE_HYPERCALL_BOUNCE(overlay_fdt, overlay_fdt_size,
                        XC_HYPERCALL_BUFFER_BOUNCE_IN);

    if ( (err = xc_hypercall_bounce_pre(xch, overlay_fdt)) )
        goto err;

    sysctl.cmd = XEN_SYSCTL_overlay;
    sysctl.u.dt_overlay.overlay_op= op;
    sysctl.u.dt_overlay.overlay_fdt_size = overlay_fdt_size;

    set_xen_guest_handle(sysctl.u.dt_overlay.overlay_fdt, overlay_fdt);

    if ( (err = do_sysctl(xch, &sysctl)) != 0 )
        PERROR("%s failed\n", __func__);

err:
    xc_hypercall_bounce_post(xch, overlay_fdt);

    return err;
}
