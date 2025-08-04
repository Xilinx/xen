/* SPDX-License-Identifier: GPL-2.0-only */

#ifndef XEN_FDT_VIRTIO_H
#define XEN_FDT_VIRTIO_H

#include <xen/fdt-kernel.h>
#include <xen/types.h>

int dom_construct_virtio(struct boot_domain *bd);
int parse_virtio(struct dt_device_node *node, struct boot_domain *bd);

#endif /* XEN_FDT_VIRTIO_H */
