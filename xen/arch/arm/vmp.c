/*
 * virtio-mmio to virtio-msg proxy, ARM arch specifics.
 *
 * Copyright (c) 2025 Advanced Micro Devices, Inc.
 * Written by Edgar E. Iglesias <edgar.iglesias@amd.com>
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include <xen/errno.h>
#include <xen/event.h>
#include <asm/mmio.h>
#include <xen/virtio/vmp.h>

static int mmio_read(struct vcpu *v, mmio_info_t *info, register_t *r,
                     void *priv)
{
    int size = 1 << info->dabt.size;
    uint32_t data = 0;
    int rc;

    rc = vmp_mmio_read(priv, info->gpa, size, &data);
    if ( rc )
    {
        *r = data;
    }
    return rc;
}

static int mmio_write(struct vcpu *v, mmio_info_t *info, register_t r,
                      void *priv)
{
    int size = 1 << info->dabt.size;

    return vmp_mmio_write(priv, info->gpa, size, r);
}

static const struct mmio_handler_ops vmp_mmio_handler = {
    .read = mmio_read,
    .write = mmio_write,
};

void vmp_arch_set_irq(struct vmp *s, bool level)
{
    vgic_inject_irq(s->domain, NULL, s->virq, level);
}

int vmp_arch_init(struct vmp *s, unsigned int virq)
{
    int rc = 0;

    register_mmio_handler(s->domain, &vmp_mmio_handler, s->base_addr,
                          VMP_MMIO_SIZE, s);

    if ( !vgic_reserve_virq(s->domain, virq) )
    {
        printk(XENLOG_ERR "%s: Failed to reserve virq %d\n", __func__, virq);
        return -EINVAL;
    }

    return rc;
}

void vmp_arch_deinit(struct vmp *s)
{
    /* TODO: Unregister mmio handlers? */
    if ( s->virq )
        vgic_free_virq(s->domain, s->virq);
}
