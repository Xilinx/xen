/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Mali G78AE GPU Arbiter Header
 *
 * Copyright (C) 2025, Advanced Micro Devices, Inc.
 */
#ifndef DRIVERS__GPU_MALI_G78AE_ARBITER_H
#define DRIVERS__GPU_MALI_G78AE_ARBITER_H

#include <xen/domain.h>
#include <xen/list.h>
#include <xen/spinlock.h>
#include <xen/types.h>

#include "common.h"
#include "gpu-subinstance.h"
#include "resource-group.h"

struct mali_arbiter;

struct mali_vm_data {
    struct list_head sched_entry;
    struct list_head entry;
    struct list_head wait_entry;
    unsigned int aw;
    struct mali_arbiter *arb;
    bool gpu_lost;
    struct domain *domain;
    int gsi_idx;
};

struct gsi_info {
    bool enabled;
    struct mali_arb_gsi *gsi;

    spinlock_t lock;
    /* AW mask assigned to this GSI */
    uint16_t aw_mask;
    /* Slices assigned to this GSI */
    uint32_t slice_mask;
};

struct mali_arbiter {
    /* gpu-subinstance information */
    struct gsi_info gsi_info[MALI_PTM_PARTITION_COUNT];

    /* Resource group instance managed by this arbiter */
    struct mali_ptm_rg *rg;

    /* slice information */
    uint32_t l2_slices;
    uint32_t core_mask;

    /* vm information */
    struct list_head wait_list;
    struct list_head reg_vms_list;

    /* Protect lists */
    spinlock_t lock;
};

int mali_arbiter_create(struct mali_arbiter **arb, struct mali_ptm_rg *rg);
void mali_arbiter_destroy(struct mali_arbiter *arb);

/* Arbiter interface */
int mali_arbif_register_vm(struct mali_arbiter *arb,
                           struct mali_vm_data *vm_data);
void mali_arbif_unregister_vm(struct mali_vm_data *vm_data);
int mali_arbif_assign_domain(struct mali_arbiter *arb, struct domain *d);
int mali_arbif_unassign_domain(struct mali_arbiter *arb, struct domain *d);

void mali_arbif_on_gpu_request(struct mali_vm_data *vm_data);
void mali_arbif_on_gpu_active(struct mali_vm_data *vm_data);
void mali_arbif_on_gpu_idle(struct mali_vm_data *vm_data);
void mali_arbif_gpu_stopped(struct mali_vm_data *vm_data,bool req_again);

int mali_arbif_get_max_config(struct mali_vm_data *vm_data,
                              uint32_t *max_l2_slices, uint32_t *max_core_mask);
int mali_arbif_get_aw_assignment(struct mali_arbiter *arb, unsigned int gsi_idx,
                                 uint16_t *aw_mask);
int mali_arbif_set_aw_assignment(struct mali_arbiter *arb, unsigned int gsi_idx,
                                 uint16_t new_aw_mask);
int mali_arbif_gpu_stop(struct mali_vm_data *vm_data);
int mali_arbif_gpu_granted(struct mali_vm_data *vm_data, uint32_t freq);
int mali_arbif_gpu_lost(struct mali_vm_data *vm_data);

#endif /* DRIVERS__GPU_MALI_G78AE_ARBITER_H */

/*
 * Local variables:
 * mode: C
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * End:
 */