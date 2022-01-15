/*
 * xen/common/dt_overlay.c
 *
 * Device tree overlay suppoert in Xen.
 *
 * Copyright (c) 2021 Xilinx Inc.
 * Written by Vikram Garhwal <fnu.vikram@xilinx.com>
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
#ifndef __XEN_DT_SYSCTL_H__
#define __XEN_DT_SYSCTL_H__

#include <xen/list.h>
#include <xen/libfdt/libfdt.h>
#include <xen/device_tree.h>
#include <public/sysctl.h>

/*
 * overlay_node_track describes information about added nodes through dtbo.
 * @dt_host_new: Pointer to the updated dt_host_new unflattened 'updated fdt'.
 * @node_fullname: Store the name of nodes.
 * @entry: List pointer.
 */
struct overlay_track {
    struct list_head entry;
    struct dt_device_node *dt_host_new;
    void *fdt;
    char **nodes_fullname;
    int **nodes_irq;
    int *node_num_irq;
    unsigned int num_nodes;
};

long dt_sysctl(struct xen_sysctl *op);
#endif
