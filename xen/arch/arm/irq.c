/*
 * xen/arch/arm/irq.c
 *
 * ARM Interrupt support
 *
 * Ian Campbell <ian.campbell@citrix.com>
 * Copyright (c) 2011 Citrix Systems.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <xen/config.h>
#include <xen/lib.h>
#include <xen/spinlock.h>
#include <xen/irq.h>
#include <xen/init.h>
#include <xen/errno.h>
#include <xen/sched.h>

#include <asm/gic.h>
#include <asm/vgic.h>

static unsigned int local_irqs_type[NR_LOCAL_IRQS];
static DEFINE_SPINLOCK(local_irqs_type_lock);

/* Describe an IRQ assigned to a guest */
struct irq_guest
{
    struct domain *d;
    unsigned int virq;
};

static void ack_none(struct irq_desc *irq)
{
    printk("unexpected IRQ trap at irq %02x\n", irq->irq);
}

static void end_none(struct irq_desc *irq) { }

hw_irq_controller no_irq_type = {
    .typename = "none",
    .startup = irq_startup_none,
    .shutdown = irq_shutdown_none,
    .enable = irq_enable_none,
    .disable = irq_disable_none,
    .ack = ack_none,
    .end = end_none
};

static irq_desc_t irq_desc[NR_IRQS];
static DEFINE_PER_CPU(irq_desc_t[NR_LOCAL_IRQS], local_irq_desc);

irq_desc_t *__irq_to_desc(int irq)
{
    if (irq < NR_LOCAL_IRQS) return &this_cpu(local_irq_desc)[irq];
    return &irq_desc[irq-NR_LOCAL_IRQS];
}

int __init arch_init_one_irq_desc(struct irq_desc *desc)
{
    desc->arch.type = DT_IRQ_TYPE_INVALID;
    return 0;
}


static int __init init_irq_data(void)
{
    int irq;

    for (irq = NR_LOCAL_IRQS; irq < NR_IRQS; irq++) {
        struct irq_desc *desc = irq_to_desc(irq);
        init_one_irq_desc(desc);
        desc->irq = irq;
        desc->action  = NULL;
    }

    return 0;
}

static int __cpuinit init_local_irq_data(void)
{
    int irq;

    spin_lock(&local_irqs_type_lock);

    for (irq = 0; irq < NR_LOCAL_IRQS; irq++) {
        struct irq_desc *desc = irq_to_desc(irq);
        init_one_irq_desc(desc);
        desc->irq = irq;
        desc->action  = NULL;

        /* PPIs are included in local_irqs, we copy the IRQ type from
         * local_irqs_type when bringing up local IRQ for this CPU in
         * order to pick up any configuration done before this CPU came
         * up. For interrupts configured after this point this is done in
         * irq_set_type.
         */
        desc->arch.type = local_irqs_type[irq];
    }

    spin_unlock(&local_irqs_type_lock);

    return 0;
}

void __init init_IRQ(void)
{
    int irq;

    spin_lock(&local_irqs_type_lock);
    for ( irq = 0; irq < NR_LOCAL_IRQS; irq++ )
        local_irqs_type[irq] = DT_IRQ_TYPE_INVALID;
    spin_unlock(&local_irqs_type_lock);

    BUG_ON(init_local_irq_data() < 0);
    BUG_ON(init_irq_data() < 0);
}

void __cpuinit init_secondary_IRQ(void)
{
    BUG_ON(init_local_irq_data() < 0);
}

static inline struct irq_guest *irq_get_guest_info(struct irq_desc *desc)
{
    ASSERT(spin_is_locked(&desc->lock));
    ASSERT(test_bit(_IRQ_GUEST, &desc->status));
    ASSERT(desc->action != NULL);

    return desc->action->dev_id;
}

static inline struct domain *irq_get_domain(struct irq_desc *desc)
{
    return irq_get_guest_info(desc)->d;
}

void irq_set_affinity(struct irq_desc *desc, const cpumask_t *cpu_mask)
{
    if ( desc != NULL )
        desc->handler->set_affinity(desc, cpu_mask);
}

int request_irq(unsigned int irq, unsigned int irqflags,
                void (*handler)(int, void *, struct cpu_user_regs *),
                const char *devname, void *dev_id)
{
    struct irqaction *action;
    int retval;

    /*
     * Sanity-check: shared interrupts must pass in a real dev-ID,
     * otherwise we'll have trouble later trying to figure out
     * which interrupt is which (messes up the interrupt freeing
     * logic etc).
     */
    if ( irq >= nr_irqs )
        return -EINVAL;
    if ( !handler )
        return -EINVAL;

    action = xmalloc(struct irqaction);
    if ( !action )
        return -ENOMEM;

    action->handler = handler;
    action->name = devname;
    action->dev_id = dev_id;
    action->free_on_release = 1;

    retval = setup_irq(irq, irqflags, action);
    if ( retval )
        xfree(action);

    return retval;
}

/* Dispatch an interrupt */
void do_IRQ(struct cpu_user_regs *regs, unsigned int irq, int is_fiq)
{
    struct irq_desc *desc = irq_to_desc(irq);

    perfc_incr(irqs);

    ASSERT(irq >= 16); /* SGIs do not come down this path */

    if (irq < 32)
        perfc_incr(ppis);
    else
        perfc_incr(spis);

    /* TODO: this_cpu(irq_count)++; */

    irq_enter();

    spin_lock(&desc->lock);
    desc->handler->ack(desc);

    if ( !desc->action )
    {
        printk("Unknown %s %#3.3x\n",
               is_fiq ? "FIQ" : "IRQ", irq);
        goto out;
    }

    if ( test_bit(_IRQ_GUEST, &desc->status) )
    {
        struct irq_guest *info = irq_get_guest_info(desc);

        perfc_incr(guest_irqs);
        desc->handler->end(desc);

        set_bit(_IRQ_INPROGRESS, &desc->status);
        desc->arch.eoi_cpu = smp_processor_id();

        /* the irq cannot be a PPI, we only support delivery of SPIs to
         * guests */
        vgic_vcpu_inject_spi(info->d, info->virq);
        goto out_no_end;
    }

    set_bit(_IRQ_PENDING, &desc->status);

    /*
     * Since we set PENDING, if another processor is handling a different
     * instance of this same irq, the other processor will take care of it.
     */
    if ( test_bit(_IRQ_DISABLED, &desc->status) ||
         test_bit(_IRQ_INPROGRESS, &desc->status) )
        goto out;

    set_bit(_IRQ_INPROGRESS, &desc->status);

    while ( test_bit(_IRQ_PENDING, &desc->status) )
    {
        struct irqaction *action;

        clear_bit(_IRQ_PENDING, &desc->status);
        action = desc->action;

        spin_unlock_irq(&desc->lock);

        do
        {
            action->handler(irq, action->dev_id, regs);
            action = action->next;
        } while ( action );

        spin_lock_irq(&desc->lock);
    }

    clear_bit(_IRQ_INPROGRESS, &desc->status);

out:
    desc->handler->end(desc);
out_no_end:
    spin_unlock(&desc->lock);
    irq_exit();
}

void release_irq(unsigned int irq, const void *dev_id)
{
    struct irq_desc *desc;
    unsigned long flags;
    struct irqaction *action, **action_ptr;

    desc = irq_to_desc(irq);

    spin_lock_irqsave(&desc->lock,flags);

    action_ptr = &desc->action;
    for ( ;; )
    {
        action = *action_ptr;
        if ( !action )
        {
            printk(XENLOG_WARNING "Trying to free already-free IRQ %u\n", irq);
            spin_unlock_irqrestore(&desc->lock, flags);
            return;
        }

        if ( action->dev_id == dev_id )
            break;

        action_ptr = &action->next;
    }

    /* Found it - remove it from the action list */
    *action_ptr = action->next;

    /* If this was the last action, shut down the IRQ */
    if ( !desc->action )
    {
        desc->handler->shutdown(desc);
        clear_bit(_IRQ_GUEST, &desc->status);
    }

    spin_unlock_irqrestore(&desc->lock,flags);

    /* Wait to make sure it's not being used on another CPU */
    do { smp_mb(); } while ( test_bit(_IRQ_INPROGRESS, &desc->status) );

    if ( action->free_on_release )
        xfree(action);
}

static int __setup_irq(struct irq_desc *desc, unsigned int irqflags,
                       struct irqaction *new)
{
    bool_t shared = !!(irqflags & IRQF_SHARED);

    ASSERT(new != NULL);

    /* Sanity checks:
     *  - if the IRQ is marked as shared
     *  - dev_id is not NULL when IRQF_SHARED is set
     */
    if ( desc->action != NULL && (!test_bit(_IRQF_SHARED, &desc->status) || !shared) )
        return -EINVAL;
    if ( shared && new->dev_id == NULL )
        return -EINVAL;

    if ( shared )
        set_bit(_IRQF_SHARED, &desc->status);

    new->next = desc->action;
    dsb(ish);
    desc->action = new;
    dsb(ish);

    return 0;
}

int setup_irq(unsigned int irq, unsigned int irqflags, struct irqaction *new)
{
    int rc;
    unsigned long flags;
    struct irq_desc *desc;
    bool_t disabled;

    desc = irq_to_desc(irq);

    spin_lock_irqsave(&desc->lock, flags);

    if ( test_bit(_IRQ_GUEST, &desc->status) )
    {
        struct domain *d = irq_get_domain(desc);

        spin_unlock_irqrestore(&desc->lock, flags);
        printk(XENLOG_ERR "ERROR: IRQ %u is already in use by the domain %u\n",
               irq, d->domain_id);
        return -EBUSY;
    }

    disabled = (desc->action == NULL);

    rc = __setup_irq(desc, irqflags, new);
    if ( rc )
        goto err;

    /* First time the IRQ is setup */
    if ( disabled )
    {
        /* It's fine to use smp_processor_id() because:
         * For PPI: irq_desc is banked
         * For SPI: we don't care for now which CPU will receive the
         * interrupt
         * TODO: Handle case where SPI is setup on different CPU than
         * the targeted CPU and the priority.
         */
        gic_route_irq_to_xen(desc, cpumask_of(smp_processor_id()),
                             GIC_PRI_IRQ);
        desc->handler->startup(desc);
    }

err:
    spin_unlock_irqrestore(&desc->lock, flags);

    return rc;
}

int route_irq_to_guest(struct domain *d, unsigned int virq,
                       unsigned int irq, const char * devname)
{
    struct irqaction *action;
    struct irq_guest *info;
    struct irq_desc *desc;
    unsigned long flags;
    int retval = 0;

    action = xmalloc(struct irqaction);
    if ( !action )
        return -ENOMEM;

    info = xmalloc(struct irq_guest);
    if ( !info )
    {
        xfree(action);
        return -ENOMEM;
    }

    info->d = d;
    info->virq = virq;

    action->dev_id = info;
    action->name = devname;
    action->free_on_release = 1;

    spin_lock_irqsave(&desc->lock, flags);

    /* If the IRQ is already used by someone
     *  - If it's the same domain -> Xen doesn't need to update the IRQ desc
     *  - Otherwise -> For now, don't allow the IRQ to be shared between
     *  Xen and domains.
     */
    if ( desc->action != NULL )
    {
        struct domain *ad = irq_get_domain(desc);

        if ( test_bit(_IRQ_GUEST, &desc->status) && d == ad )
            goto out;

        if ( test_bit(_IRQ_GUEST, &desc->status) )
            printk(XENLOG_ERR "ERROR: IRQ %u is already used by domain %u\n",
                   irq, ad->domain_id);
        else
            printk(XENLOG_ERR "ERROR: IRQ %u is already used by Xen\n", irq);
        retval = -EBUSY;
        goto out;
    }

    retval = __setup_irq(desc, 0, action);
    if ( retval )
        goto out;

    gic_route_irq_to_guest(d, virq, desc, cpumask_of(smp_processor_id()),
                           GIC_PRI_IRQ);
    spin_unlock_irqrestore(&desc->lock, flags);
    return 0;

out:
    spin_unlock_irqrestore(&desc->lock, flags);
    xfree(action);
    xfree(info);

    return retval;
}

/*
 * pirq event channels. We don't use these on ARM, instead we use the
 * features of the GIC to inject virtualised normal interrupts.
 */
struct pirq *alloc_pirq_struct(struct domain *d)
{
    return NULL;
}

/*
 * These are all unreachable given an alloc_pirq_struct
 * which returns NULL, all callers try to lookup struct pirq first
 * which will fail.
 */
int pirq_guest_bind(struct vcpu *v, struct pirq *pirq, int will_share)
{
    BUG();
}

void pirq_guest_unbind(struct domain *d, struct pirq *pirq)
{
    BUG();
}

void pirq_set_affinity(struct domain *d, int pirq, const cpumask_t *mask)
{
    BUG();
}

static bool_t irq_validate_new_type(unsigned int curr, unsigned new)
{
    return (curr == DT_IRQ_TYPE_INVALID || curr == new );
}

int irq_set_spi_type(unsigned int spi, unsigned int type)
{
    unsigned long flags;
    struct irq_desc *desc = irq_to_desc(spi);
    int ret = -EBUSY;

    /* This function should not be used for other than SPIs */
    if ( spi < NR_LOCAL_IRQS )
        return -EINVAL;

    spin_lock_irqsave(&desc->lock, flags);

    if ( !irq_validate_new_type(desc->arch.type, type) )
        goto err;

    desc->arch.type = type;

    ret = 0;

err:
    spin_unlock_irqrestore(&desc->lock, flags);
    return ret;
}

static int irq_local_set_type(unsigned int irq, unsigned int type)
{
    unsigned int cpu;
    unsigned int old_type;
    unsigned long flags;
    int ret = -EBUSY;
    struct irq_desc *desc;

    ASSERT(irq < NR_LOCAL_IRQS);

    spin_lock(&local_irqs_type_lock);

    old_type = local_irqs_type[irq];

    if ( !irq_validate_new_type(old_type, type) )
        goto unlock;

    ret = 0;
    /* We don't need to reconfigure if the type is correctly set */
    if ( old_type == type )
        goto unlock;

    local_irqs_type[irq] = type;

    for_each_cpu( cpu, &cpu_online_map )
    {
        desc = &per_cpu(local_irq_desc, cpu)[irq];
        spin_lock_irqsave(&desc->lock, flags);
        desc->arch.type = type;
        spin_unlock_irqrestore(&desc->lock, flags);
    }

unlock:
    spin_unlock(&local_irqs_type_lock);
    return ret;
}

int platform_get_irq(const struct dt_device_node *device, int index)
{
    struct dt_irq dt_irq;
    unsigned int type, irq;
    int res;

    res = dt_device_get_irq(device, index, &dt_irq);
    if ( res )
        return -1;

    irq = dt_irq.irq;
    type = dt_irq.type;

    /* Setup the IRQ type */
    if ( irq < NR_LOCAL_IRQS )
        res = irq_local_set_type(irq, type);
    else
        res = irq_set_spi_type(irq, type);

    if ( res )
            return -1;

    return irq;
}

/*
 * Local variables:
 * mode: C
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * End:
 */
