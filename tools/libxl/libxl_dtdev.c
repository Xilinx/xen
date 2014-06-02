/*
 * Copyright (C) 2014      Linaro Limited.
 * Author Julien Grall <julien.grall@linaro.org>
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

#include "libxl_osdeps.h" /* Must come before other headers */

#include "libxl_internal.h"

int libxl__device_dt_add(libxl__gc *gc, uint32_t domid,
                         const libxl_device_dtdev *dtdev)
{
    LOG(DEBUG, "Assign device \"%s\" to dom%u", dtdev->path, domid);

    return xc_assign_dt_device(CTX->xch, domid, dtdev->path);
}

/*
 * Local variables:
 * mode: C
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * End:
 */
