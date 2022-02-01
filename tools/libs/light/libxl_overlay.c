/*
 * Copyright (C) 2021 Xilinx Inc.
 * Author Vikram Garhwal <fnu.vikram@xilinx.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published
 * by the Free Software Foundation; version 2.1 only. with the special
 * exception on linking described in file LICENSE.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 */

#include "libxl_osdeps.h" /* must come before any other headers */
#include "libxl_internal.h"
#include <libfdt.h>
#include <xenguest.h>
#include <xenctrl.h>
#include <xen/device_tree_defs.h>

static int check_overlay_fdt(libxl__gc *gc, void *fdt, size_t size)
{
    int r;

    if (fdt_magic(fdt) != FDT_MAGIC) {
        LOG(ERROR, "Overlay FDT is not a valid Flat Device Tree");
        return ERROR_FAIL;
    }

    r = fdt_check_header(fdt);
    if (r) {
        LOG(ERROR, "Failed to check the overlay FDT (%d)", r);
        return ERROR_FAIL;
    }

    if (fdt_totalsize(fdt) > size) {
        LOG(ERROR, "Overlay FDT totalsize is too big");
        return ERROR_FAIL;
    }

    return 0;
}

static int modify_overlay_for_domU(libxl__gc *gc, void *overlay_dt_domU,
                                   size_t size)
{
    int rc = 0;
    int virtual_interrupt_parent = GUEST_PHANDLE_GIC;
    const struct fdt_property *fdt_prop_node = NULL;
    int overlay;
    int prop_len = 0;
    int subnode = 0;
    int fragment;
    const char *prop_name;
    const char *target_path = "/";

    fdt_for_each_subnode(fragment, overlay_dt_domU, 0) {
        prop_name = fdt_getprop(overlay_dt_domU, fragment, "target-path",
                                &prop_len);
        if (prop_name == NULL) {
            LOG(ERROR, "target-path property not found\n");
            rc = ERROR_FAIL;
            goto err;
        }

        /* Change target path for domU dtb. */
        rc = fdt_setprop_string(overlay_dt_domU, fragment, "target-path",
                                target_path);
        if (rc) {
            LOG(ERROR, "Setting interrupt parent property failed for %s\n",
                prop_name);
            goto err;
        }

        overlay = fdt_subnode_offset(overlay_dt_domU, fragment, "__overlay__");

        fdt_for_each_subnode(subnode, overlay_dt_domU, overlay)
        {
            const char *node_name = fdt_get_name(overlay_dt_domU, subnode,
                                                 NULL);

            fdt_prop_node = fdt_getprop(overlay_dt_domU, subnode,
                                        "interrupt-parent", &prop_len);
            if (fdt_prop_node == NULL) {
                LOG(DETAIL, "%s property not found for %s. Skip to next node\n",
                    "interrupt-parent", node_name);
                continue;
            }

            rc = fdt_setprop_inplace_u32(overlay_dt_domU, subnode,
                                         "interrupt-parent",
                                         virtual_interrupt_parent);
            if (rc) {
                LOG(ERROR, "Setting interrupt parent property failed for %s\n",
                    "interrupt-parent");
                goto err;
            }
        }
    }

return 0;

err:
    return rc;
}

int libxl_dt_overlay(libxl_ctx *ctx, uint32_t domid, void *overlay_dt,
                     int overlay_dt_size, uint8_t op, bool auto_mode,
                     bool domain_mapping)
{
    int rc = 0;
    GC_INIT(ctx);

    if (check_overlay_fdt(gc, overlay_dt, overlay_dt_size)) {
        LOG(ERROR, "Overlay DTB check failed\n");
        return ERROR_FAIL;
    } else
        LOG(DEBUG, "Overlay DTB check passed\n");

    /* Check if user entered a valid domain id. */
    rc = libxl_domain_info(CTX, NULL, domid);
    if (rc == ERROR_DOMAIN_NOTFOUND) {
        LOGD(ERROR, domid, "Non-existant domain.");
        return ERROR_FAIL;
    }

    /* We don't need to do  xc_interface_open here. */
    rc = xc_dt_overlay(ctx->xch, domid, overlay_dt, overlay_dt_size, op,
                       domain_mapping);

    if (rc) {
        LOG(ERROR, "domain%d: Adding/Removing overlay dtb failed.\n", domid);
        rc = ERROR_FAIL;
        goto out;
    }

    /*
     * auto_mode doesn't apply to dom0 as dom0 can get the physical
     * description of the hardware.
     */
    if (domid && auto_mode) {
        if (op == LIBXL_DT_OVERLAY_ADD)
            rc = modify_overlay_for_domU(gc, overlay_dt, overlay_dt_size);
    }

out:
    GC_FREE;

    return rc;
}

