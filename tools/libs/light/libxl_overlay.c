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

int libxl_dt_overlay(libxl_ctx *ctx, void *overlay_dt, int overlay_dt_size,
                     uint8_t op)
{
    int rc = 0;
    GC_INIT(ctx);

    if (check_overlay_fdt(gc, overlay_dt, overlay_dt_size)) {
        LOG(ERROR, "Overlay DTB check failed\n");
        return ERROR_FAIL;
    } else
        LOG(DEBUG, "Overlay DTB check passed\n");

    /* We don't need to do  xc_interface_open here. */
    rc = xc_dt_overlay(ctx->xch, overlay_dt, overlay_dt_size, op);

    if (rc)
        LOG(ERROR, "%s: Adding/Removing overlay dtb failed.\n", __func__);

    return rc;
}

