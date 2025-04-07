/*
 * Non-blocking virtio-mmio to virtio-msg proxy.
 *
 * Copyright (c) 2025 Advanced Micro Devices, Inc.
 * Written by Edgar E. Iglesias <edgar.iglesias@amd.com>
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */
#ifndef _VMP_H_
#define _VMP_H_

#include <xen/event.h>
#include <xen/virtio/virtio-msg-bus.h>

#define VMP_MMIO_SIZE 0x1000

#define VMP_STATS 0
#define VIRTIO_MSG_MAX_QUEUES 1024

enum vmp_state {
        VMP_STATE_IDLE = 0,
        VMP_STATE_DEVICE_INFO,
        VMP_STATE_GET_FEATURES,
        VMP_STATE_SET_FEATURES,
        VMP_STATE_GET_CONFIG,
        VMP_STATE_SET_CONFIG,
        VMP_STATE_GET_DEVICE_STATUS,
        VMP_STATE_SET_DEVICE_STATUS,
        VMP_STATE_GET_VQUEUE,
        VMP_STATE_SET_VQUEUE,
        VMP_STATE_RESET_VQUEUE,
        VMP_STATE_MAX,
};

struct vmp {
    struct virtio_msg_bus *bd;
    struct domain *domain;

    /*
     * VMP runs in two contexts.
     *
     * 1. MMIO traps into the virtio-mmio space.
     * 2. Event-channel notifications from the device_domain.
     *
     * These contexts may run on different guest vcpus, so we need locking.
     * We don't yet use interrupts so we don't need to disable IRQs when
     * taking our spinlock.
     */
    spinlock_t lock;

    enum vmp_state state;
    enum vmp_state next_state;

    /*
     * We need to keep track of the generation nr in order
     * to put our latest read into set_config.
     */
    uint32_t generation;

    struct {
        uint32_t access;
        uint32_t access_data;
        uint32_t device_features_sel;
        uint32_t driver_features_sel;
        uint32_t driver_features[VIRTIO_MSG_MAX_FEATURE_NUM];
        uint32_t queue_sel;
        uint32_t queue_max_size;
        uint32_t interrupt_status;
        uint32_t status;
    } regs;

    struct {
        uint32_t size;
        uint64_t descriptor_addr;
        uint64_t driver_addr;
        uint64_t device_addr;
        unsigned long enabled_bitmap[VIRTIO_MSG_MAX_QUEUES / BITS_PER_LONG];
    } vq;

#if VMP_STATS
    struct {
        uint16_t state[VMP_STATE_MAX];

        /* A 16bit counter per 32bit register up to VIRTIO_MMIO_CONFIG. */
        uint16_t regs[VIRTIO_MMIO_CONFIG / 4];
        uint16_t ind_regs[VIRTIO_MMIO_CONFIG / 4];
    } stats;
#endif

    paddr_t     base_addr;
    unsigned int virq;
};

int vmp_mmio_read(struct vmp *s, uint64_t addr, int size, uint32_t *data);
int vmp_mmio_write(struct vmp *s, uint64_t addr, int size, uint32_t data);
void vmp_arch_set_irq(struct vmp *s, bool level);

int vmp_init(struct vmp *s, struct virtio_msg_bus *bd,
             struct domain *d, paddr_t base_addr, unsigned int virq);

int vmp_arch_init(struct vmp *s, unsigned int virq);

void vmp_arch_deinit(struct vmp *s);
#endif  /* _VMP_H_ */
