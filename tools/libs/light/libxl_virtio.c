/*
 * Setup VirtIO backend. This is intended to interact with a VirtIO
 * backend that is watching xenstore, and create new VirtIO devices
 * with the parameter found in xenstore (VirtIO frontend don't
 * interact with xenstore.)
 *
 * Copyright (C) 2022 Linaro Ltd.
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

#include "libxl_internal.h"

static int libxl__device_virtio_setdefault(libxl__gc *gc, uint32_t domid,
                                           libxl_device_virtio *virtio,
                                           bool hotplug)
{
    int rc;

    rc = libxl__resolve_domid(gc, virtio->backend_domname,
                              &virtio->backend_domid);
    if (rc < 0) return rc;

    libxl_defbool_setdefault(&virtio->grant_usage,
                             virtio->backend_domid != LIBXL_TOOLSTACK_DOMID);

    return 0;
}

static int libxl__device_from_virtio(libxl__gc *gc, uint32_t domid,
                                     libxl_device_virtio *virtio,
                                     libxl__device *device)
{
    device->backend_devid   = virtio->devid;
    device->backend_domid   = virtio->backend_domid;
    device->devid           = virtio->devid;
    device->domid           = domid;

    device->backend_kind    = LIBXL__DEVICE_KIND_VIRTIO;
    device->kind            = LIBXL__DEVICE_KIND_VIRTIO;

    return 0;
}

static int libxl__set_xenstore_virtio(libxl__gc *gc, uint32_t domid,
                                      libxl_device_virtio *virtio,
                                      flexarray_t *back, flexarray_t *front,
                                      flexarray_t *ro_front)
{
    const char *transport = libxl_virtio_transport_to_string(virtio->transport);

    if (virtio->transport == LIBXL_VIRTIO_TRANSPORT_MMIO) {
        flexarray_append_pair(back, "irq", GCSPRINTF("%u", virtio->u.mmio.irq));
        flexarray_append_pair(back, "base", GCSPRINTF("%#"PRIx64, virtio->u.mmio.base));
    } else {
        /*
         * TODO:
         * Probably we will also need to store PCI Host bridge details (irq and
         * mem ranges) this particular PCI device belongs to if emulator cannot
         * or should not rely on what is described at include/public/arch-arm.h
         */
        flexarray_append_pair(back, "bdf", GCSPRINTF("%04x:%02x:%02x.%01x",
                              virtio->u.pci.domain, virtio->u.pci.bus,
                              virtio->u.pci.dev, virtio->u.pci.func));
        flexarray_append_pair(back, "host_id", GCSPRINTF("%u", virtio->u.pci.host_id));
    }
    flexarray_append_pair(back, "type", GCSPRINTF("%s", virtio->type));
    flexarray_append_pair(back, "transport", GCSPRINTF("%s", transport));
    flexarray_append_pair(back, "grant_usage",
                          libxl_defbool_val(virtio->grant_usage) ? "1" : "0");

    return 0;
}

static int libxl__virtio_from_xenstore(libxl__gc *gc, const char *libxl_path,
                                       libxl_devid devid,
                                       libxl_device_virtio *virtio)
{
    const char *be_path, *tmp = NULL;
    int rc;

    virtio->devid = devid;

    rc = libxl__xs_read_mandatory(gc, XBT_NULL,
                                  GCSPRINTF("%s/backend", libxl_path),
                                  &be_path);
    if (rc) goto out;

    rc = libxl__backendpath_parse_domid(gc, be_path, &virtio->backend_domid);
    if (rc) goto out;

    tmp = libxl__xs_read(gc, XBT_NULL, GCSPRINTF("%s/transport", be_path));
    if (!tmp) {
        LOG(ERROR, "Missing xenstore node %s/transport", be_path);
        rc = ERROR_INVAL;
        goto out;
    }

    rc = libxl_virtio_transport_from_string(tmp, &virtio->transport);
    if (rc) {
        LOG(ERROR, "Unable to parse xenstore node %s/transport", be_path);
        goto out;
    }

    if (virtio->transport != LIBXL_VIRTIO_TRANSPORT_MMIO &&
        virtio->transport != LIBXL_VIRTIO_TRANSPORT_PCI) {
        LOG(ERROR, "Unexpected transport for virtio");
        rc = ERROR_INVAL;
        goto out;
    }

    if (virtio->transport == LIBXL_VIRTIO_TRANSPORT_MMIO) {
        tmp = NULL;
        rc = libxl__xs_read_checked(gc, XBT_NULL,
                                    GCSPRINTF("%s/irq", be_path), &tmp);
        if (rc) goto out;

        if (tmp) {
            virtio->u.mmio.irq = strtoul(tmp, NULL, 0);
        }

        tmp = NULL;
        rc = libxl__xs_read_checked(gc, XBT_NULL,
                                    GCSPRINTF("%s/base", be_path), &tmp);
        if (rc) goto out;

        if (tmp) {
            virtio->u.mmio.base = strtoul(tmp, NULL, 0);
        }
    } else {
        unsigned int domain, bus, dev, func;

        tmp = NULL;
        rc = libxl__xs_read_checked(gc, XBT_NULL,
                                    GCSPRINTF("%s/bdf", be_path), &tmp);
        if (rc) goto out;

        if (tmp) {
            if (sscanf(tmp, "%04x:%02x:%02x.%01x",
                &domain, &bus, &dev, &func) != 4) {
                rc = ERROR_INVAL;
                goto out;
            }

            virtio->u.pci.domain = domain;
            virtio->u.pci.bus = bus;
            virtio->u.pci.dev = dev;
            virtio->u.pci.func = func;
        }

        tmp = NULL;
        rc = libxl__xs_read_checked(gc, XBT_NULL,
                                    GCSPRINTF("%s/host_id", be_path), &tmp);
        if (rc) goto out;

        if (tmp) {
            virtio->u.pci.host_id = strtoul(tmp, NULL, 0);
        }
    }

    tmp = NULL;
    rc = libxl__xs_read_checked(gc, XBT_NULL,
                                GCSPRINTF("%s/grant_usage", be_path), &tmp);
    if (rc) goto out;

    if (tmp) {
        libxl_defbool_set(&virtio->grant_usage, strtoul(tmp, NULL, 0));
    }

    tmp = NULL;
    rc = libxl__xs_read_checked(gc, XBT_NULL,
				GCSPRINTF("%s/type", be_path), &tmp);
    if (rc) goto out;

    if (tmp) {
        int len = sizeof(VIRTIO_DEVICE_TYPE_GENERIC) - 1;

        if (!strncmp(tmp, VIRTIO_DEVICE_TYPE_GENERIC, len)) {
            virtio->type = libxl__strdup(NOGC, tmp);
        } else {
            return ERROR_INVAL;
        }
    }

out:
    return rc;
}

static LIBXL_DEFINE_UPDATE_DEVID(virtio)

#define libxl__add_virtios NULL
#define libxl_device_virtio_compare NULL

DEFINE_DEVICE_TYPE_STRUCT(virtio, VIRTIO, virtios,
    .set_xenstore_config = (device_set_xenstore_config_fn_t)
                           libxl__set_xenstore_virtio,
    .from_xenstore = (device_from_xenstore_fn_t)libxl__virtio_from_xenstore,
    .skip_attach = 1
);

/*
 * Local variables:
 * mode: C
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * End:
 */
