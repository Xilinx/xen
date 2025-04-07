/*
 * virtio-mmio to virtio-msg proxy, x86 arch specifics.
 *
 * Copyright (c) 2025 Advanced Micro Devices, Inc.
 * Written by Edgar E. Iglesias <edgar.iglesias@amd.com>
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include <xen/errno.h>
#include <xen/event.h>
#include <asm/hvm/io.h>
#include <asm/hvm/irq.h>
#include <xen/virtio/vmp.h>

static struct vmp *vmp_from_domain_addr(struct domain *d, uint64_t addr)
{
    unsigned int i;

    /*
     * This helper is only safe when d == current->domain (x86 MMIO handlers).
     * If called for a foreign domain it may race with domain destruction and
     * return a pointer to an object that may be freed.
     *
     * For foreign-domain callers, we'd need to hold rcu_lock_domain(d) across
     * the entire lookup and use of the returned pointer, for example:
     *
     *   rcu_lock_domain(d);
     *   s = vmp_from_domain_addr(d, addr);
     *   if (s) { use s safely while RCU is held }
     *   rcu_unlock_domain(d);
     */
    BUG_ON(d != current->domain);

    for ( i = 0; i < ARRAY_SIZE(d->virtio_msg_bus); i++ )
    {
        struct virtio_msg_bus *bd = rcu_dereference(d->virtio_msg_bus[i]);
        struct vmp *s;

        if ( !bd || virtio_msg_bus_get_state(bd) == STATE_DISABLED )
            continue;

        s = bd->client_opaque;

        /*
         * TODO: When we add multiple kinds of clients, we need a way
         * to tell the type, if it's VMP or something else, e.g virtio-pci.
         */
        if ( addr >= s->base_addr && (addr - s->base_addr) < VMP_MMIO_SIZE )
        {
            return s;
        }
    }

    return NULL;
}

static int mmio_check(struct vcpu *v, unsigned long addr)
{
    return vmp_from_domain_addr(v->domain, addr) != NULL;
}

static int mmio_read(struct vcpu *v, unsigned long addr, unsigned int length,
                     unsigned long *val)
{
    struct vmp *s = vmp_from_domain_addr(v->domain, addr);
    uint32_t data = 0;
    int rc;

    if ( !s )
        return X86EMUL_UNHANDLEABLE;

    rc = vmp_mmio_read(s, addr, length, &data);
    if ( !rc )
        return X86EMUL_UNHANDLEABLE;

    *val = data;
    return X86EMUL_OKAY;
}

static int mmio_write(struct vcpu *v, unsigned long addr, unsigned int length,
                      unsigned long val)
{
    struct vmp *s = vmp_from_domain_addr(v->domain, addr);
    int rc;

    if ( !s )
        return X86EMUL_UNHANDLEABLE;

    rc = vmp_mmio_write(s, addr, length, val);
    return rc ? X86EMUL_OKAY : X86EMUL_UNHANDLEABLE;
}

static const struct hvm_mmio_ops vmp_mmio_handler = {
    .check = mmio_check,
    .read = mmio_read,
    .write = mmio_write,
};

void vmp_arch_set_irq(struct vmp *s, bool level)
{
    if ( level )
        hvm_ioapic_assert(s->domain, s->virq, level);
    else
        hvm_ioapic_deassert(s->domain, s->virq);
}

int vmp_arch_init(struct vmp *s, unsigned int virq)
{
    register_mmio_handler(s->domain, &vmp_mmio_handler);
    return 0;
}

void vmp_arch_deinit(struct vmp *s)
{
    /* TODO: Unregister mmio handlers? */
}
