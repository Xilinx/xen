/******************************************************************************
 * arch/x86/irq.c
 * 
 * Portions of this file are:
 *  Copyright (C) 1992, 1998 Linus Torvalds, Ingo Molnar
 */

#include <xen/init.h>
#include <xen/delay.h>
#include <xen/errno.h>
#include <xen/event.h>
#include <xen/irq.h>
#include <xen/param.h>
#include <xen/perfc.h>
#include <xen/sched.h>
#include <xen/keyhandler.h>
#include <xen/compat.h>
#include <xen/iocap.h>
#include <xen/iommu.h>
#include <xen/symbols.h>
#include <xen/trace.h>
#include <xen/softirq.h>
#include <xsm/xsm.h>
#include <asm/msi.h>
#include <asm/current.h>
#include <asm/flushtlb.h>
#include <asm/mach-generic/mach_apic.h>
#include <irq_vectors.h>
#include <public/physdev.h>

/* opt_noirqbalance: If true, software IRQ balancing/affinity is disabled. */
bool __read_mostly opt_noirqbalance;
boolean_param("noirqbalance", opt_noirqbalance);

unsigned int __read_mostly nr_irqs_gsi = 16;
unsigned int __read_mostly nr_irqs;
integer_param("nr_irqs", nr_irqs);

/* This default may be changed by the AMD IOMMU code */
int __read_mostly opt_irq_vector_map = OPT_IRQ_VECTOR_MAP_DEFAULT;

/* Max number of guests IRQ could be shared with */
static unsigned char __read_mostly irq_max_guests;
integer_param("irq-max-guests", irq_max_guests);

vmask_t global_used_vector_map;

struct irq_desc __read_mostly *irq_desc = NULL;

static DECLARE_BITMAP(used_vectors, X86_NR_VECTORS);

static DEFINE_SPINLOCK(vector_lock);

DEFINE_PER_CPU(vector_irq_t, vector_irq);

DEFINE_PER_CPU(struct cpu_user_regs *, __irq_regs);

static LIST_HEAD(irq_ratelimit_list);
static DEFINE_SPINLOCK(irq_ratelimit_lock);
static struct timer irq_ratelimit_timer;

/* irq_ratelimit: the max irq rate allowed in every 10ms, set 0 to disable */
static unsigned int __read_mostly irq_ratelimit_threshold = 10000;
integer_param("irq_ratelimit", irq_ratelimit_threshold);

static int __init cf_check parse_irq_vector_map_param(const char *s)
{
    const char *ss;
    int rc = 0;

    do {
        ss = strchr(s, ',');
        if ( !ss )
            ss = strchr(s, '\0');

        if ( !cmdline_strcmp(s, "none") )
            opt_irq_vector_map = OPT_IRQ_VECTOR_MAP_NONE;
        else if ( !cmdline_strcmp(s, "global") )
            opt_irq_vector_map = OPT_IRQ_VECTOR_MAP_GLOBAL;
        else if ( !cmdline_strcmp(s, "per-device") )
            opt_irq_vector_map = OPT_IRQ_VECTOR_MAP_PERDEV;
        else
            rc = -EINVAL;

        s = ss + 1;
    } while ( *ss );

    return rc;
}
custom_param("irq_vector_map", parse_irq_vector_map_param);

/* Must be called when irq disabled */
void lock_vector_lock(void)
{
    /* Used to the online set of cpus does not change
     * during assign_irq_vector.
     */
    spin_lock(&vector_lock);
}

void unlock_vector_lock(void)
{
    spin_unlock(&vector_lock);
}

static inline bool valid_irq_vector(unsigned int vector)
{
    return vector >= FIRST_IRQ_VECTOR && vector <= LAST_IRQ_VECTOR;
}

static void release_old_vec(struct irq_desc *desc)
{
    unsigned int vector = desc->arch.old_vector;

    desc->arch.old_vector = IRQ_VECTOR_UNASSIGNED;
    cpumask_clear(desc->arch.old_cpu_mask);

    if ( !valid_irq_vector(vector) )
        ASSERT_UNREACHABLE();
    else if ( desc->arch.used_vectors )
    {
        ASSERT(test_bit(vector, desc->arch.used_vectors));
        clear_bit(vector, desc->arch.used_vectors);
    }
}

static void _trace_irq_mask(uint32_t event, int irq, int vector,
                            const cpumask_t *mask)
{
    struct {
        uint16_t irq, vec;
        uint32_t mask[6];
    } d = {
       .irq = irq,
       .vec = vector,
    };

    memcpy(d.mask, mask,
           min(sizeof(d.mask), BITS_TO_LONGS(nr_cpu_ids) * sizeof(long)));
    trace_var(event, 1, sizeof(d), &d);
}

static void trace_irq_mask(uint32_t event, int irq, int vector,
                           const cpumask_t *mask)
{
    if ( unlikely(tb_init_done) )
        _trace_irq_mask(event, irq, vector, mask);
}

static int __init _bind_irq_vector(struct irq_desc *desc, int vector,
                                   const cpumask_t *cpu_mask)
{
    cpumask_t online_mask;
    int cpu;

    BUG_ON((unsigned)vector >= X86_NR_VECTORS);

    cpumask_and(&online_mask, cpu_mask, &cpu_online_map);
    if (cpumask_empty(&online_mask))
        return -EINVAL;
    if ( (desc->arch.vector == vector) &&
         cpumask_equal(desc->arch.cpu_mask, &online_mask) )
        return 0;
    if ( desc->arch.vector != IRQ_VECTOR_UNASSIGNED )
        return -EBUSY;
    trace_irq_mask(TRC_HW_IRQ_BIND_VECTOR, desc->irq, vector, &online_mask);
    for_each_cpu(cpu, &online_mask)
        per_cpu(vector_irq, cpu)[vector] = desc->irq;
    desc->arch.vector = vector;
    cpumask_copy(desc->arch.cpu_mask, &online_mask);
    if ( desc->arch.used_vectors )
    {
        ASSERT(!test_bit(vector, desc->arch.used_vectors));
        set_bit(vector, desc->arch.used_vectors);
    }
    desc->arch.used = IRQ_USED;
    return 0;
}

int __init bind_irq_vector(int irq, int vector, const cpumask_t *cpu_mask)
{
    struct irq_desc *desc = irq_to_desc(irq);
    unsigned long flags;
    int ret;

    BUG_ON((unsigned)irq >= nr_irqs);

    spin_lock_irqsave(&desc->lock, flags);
    spin_lock(&vector_lock);
    ret = _bind_irq_vector(desc, vector, cpu_mask);
    spin_unlock(&vector_lock);
    spin_unlock_irqrestore(&desc->lock, flags);

    return ret;
}

static void _clear_irq_vector(struct irq_desc *desc)
{
    unsigned int cpu, old_vector, irq = desc->irq;
    unsigned int vector = desc->arch.vector;
    cpumask_t *tmp_mask = this_cpu(scratch_cpumask);

    BUG_ON(!valid_irq_vector(vector));

    /* Always clear desc->arch.vector */
    cpumask_and(tmp_mask, desc->arch.cpu_mask, &cpu_online_map);

    for_each_cpu(cpu, tmp_mask)
    {
        ASSERT(per_cpu(vector_irq, cpu)[vector] == irq);
        per_cpu(vector_irq, cpu)[vector] = ~irq;
    }

    desc->arch.vector = IRQ_VECTOR_UNASSIGNED;
    cpumask_clear(desc->arch.cpu_mask);

    if ( desc->arch.used_vectors )
    {
        ASSERT(test_bit(vector, desc->arch.used_vectors));
        clear_bit(vector, desc->arch.used_vectors);
    }

    desc->arch.used = IRQ_UNUSED;

    trace_irq_mask(TRC_HW_IRQ_CLEAR_VECTOR, irq, vector, tmp_mask);

    if ( likely(!desc->arch.move_in_progress) )
        return;

    /* If we were in motion, also clear desc->arch.old_vector */
    old_vector = desc->arch.old_vector;
    cpumask_and(tmp_mask, desc->arch.old_cpu_mask, &cpu_online_map);

    for_each_cpu(cpu, tmp_mask)
    {
        ASSERT(per_cpu(vector_irq, cpu)[old_vector] == irq);
        TRACE_3D(TRC_HW_IRQ_MOVE_FINISH, irq, old_vector, cpu);
        per_cpu(vector_irq, cpu)[old_vector] = ~irq;
    }

    release_old_vec(desc);

    desc->arch.move_in_progress = 0;
}

void __init clear_irq_vector(int irq)
{
    struct irq_desc *desc = irq_to_desc(irq);
    unsigned long flags;

    spin_lock_irqsave(&desc->lock, flags);
    spin_lock(&vector_lock);
    _clear_irq_vector(desc);
    spin_unlock(&vector_lock);
    spin_unlock_irqrestore(&desc->lock, flags);
}

/*
 * Dynamic irq allocate and deallocation for MSI
 */

int create_irq(nodeid_t node, bool grant_access)
{
    int irq, ret;
    struct irq_desc *desc;

    for (irq = nr_irqs_gsi; irq < nr_irqs; irq++)
    {
        desc = irq_to_desc(irq);
        if (cmpxchg(&desc->arch.used, IRQ_UNUSED, IRQ_RESERVED) == IRQ_UNUSED)
           break;
    }

    if (irq >= nr_irqs)
         return -ENOSPC;

    ret = init_one_irq_desc(desc);
    if (!ret)
    {
        cpumask_t *mask = NULL;

        if ( node != NUMA_NO_NODE )
        {
            mask = &node_to_cpumask(node);
            if (cpumask_empty(mask))
                mask = NULL;
        }
        ret = assign_irq_vector(irq, mask);
    }

    ASSERT(desc->arch.creator_domid == DOMID_INVALID);

    if (ret < 0)
    {
        desc->arch.used = IRQ_UNUSED;
        irq = ret;
    }
    else if ( grant_access )
    {
        struct domain *currd = current->domain;

        ret = irq_permit_access(currd, irq);
        if ( ret )
            printk(XENLOG_G_ERR
                   "Could not grant %pd access to IRQ%d (error %d)\n",
                   currd, irq, ret);
        else
            desc->arch.creator_domid = currd->domain_id;
    }

    return irq;
}

void destroy_irq(unsigned int irq)
{
    struct irq_desc *desc = irq_to_desc(irq);
    unsigned long flags;
    struct irqaction *action;

    BUG_ON(!MSI_IRQ(irq));

    if ( desc->arch.creator_domid != DOMID_INVALID )
    {
        struct domain *d = rcu_lock_domain_by_id(desc->arch.creator_domid);

        if ( d )
        {
            int err = irq_deny_access(d, irq);

            if ( err )
                printk(XENLOG_G_ERR
                       "Could not revoke %pd access to IRQ%u (error %d)\n",
                       d, irq, err);

            rcu_unlock_domain(d);
        }

        desc->arch.creator_domid = DOMID_INVALID;
    }

    spin_lock_irqsave(&desc->lock, flags);
    desc->status  &= ~IRQ_GUEST;
    desc->handler->shutdown(desc);
    desc->status |= IRQ_DISABLED;
    action = desc->action;
    desc->action  = NULL;
    desc->msi_desc = NULL;
    cpumask_setall(desc->affinity);
    spin_unlock_irqrestore(&desc->lock, flags);

    /* Wait to make sure it's not being used on another CPU */
    do { smp_mb(); } while ( desc->status & IRQ_INPROGRESS );

    spin_lock_irqsave(&desc->lock, flags);
    desc->handler = &no_irq_type;
    spin_lock(&vector_lock);
    _clear_irq_vector(desc);
    spin_unlock(&vector_lock);
    desc->arch.used_vectors = NULL;
    spin_unlock_irqrestore(&desc->lock, flags);

    xfree(action);
}

int irq_to_vector(int irq)
{
    int vector = IRQ_VECTOR_UNASSIGNED;
    const struct irq_desc *desc;

    BUG_ON(irq >= nr_irqs || irq < 0);
    desc = irq_to_desc(irq);

    if (IO_APIC_IRQ(irq))
    {
        vector = desc->arch.vector;
        /*
         * Both parts of the condition are needed here during early boot, as
         * at that time IRQ0 in particular may still have the 8259A chip set,
         * but has already got its special IRQ0_VECTOR.
         */
        if ( desc->handler->enable == enable_8259A_irq &&
             vector >= FIRST_LEGACY_VECTOR && vector <= LAST_LEGACY_VECTOR )
            vector = 0;
    }
    else if (MSI_IRQ(irq))
        vector = desc->arch.vector;
    else
        vector = LEGACY_VECTOR(irq);

    return vector;
}

int arch_init_one_irq_desc(struct irq_desc *desc)
{
    if ( !zalloc_cpumask_var(&desc->arch.cpu_mask) )
        return -ENOMEM;

    if ( !alloc_cpumask_var(&desc->arch.old_cpu_mask) )
    {
        free_cpumask_var(desc->arch.cpu_mask);
        return -ENOMEM;
    }

    if ( !alloc_cpumask_var(&desc->arch.pending_mask) )
    {
        free_cpumask_var(desc->arch.old_cpu_mask);
        free_cpumask_var(desc->arch.cpu_mask);
        return -ENOMEM;
    }

    desc->arch.vector = IRQ_VECTOR_UNASSIGNED;
    desc->arch.old_vector = IRQ_VECTOR_UNASSIGNED;
    desc->arch.creator_domid = DOMID_INVALID;

    return 0;
}

int __init init_irq_data(void)
{
    struct irq_desc *desc;
    int irq, vector;

    for ( vector = 0; vector < X86_NR_VECTORS; ++vector )
        this_cpu(vector_irq)[vector] = INT_MIN;

    irq_desc = xzalloc_array(struct irq_desc, nr_irqs);
    
    if ( !irq_desc )
        return -ENOMEM;

    for ( irq = 0; irq < nr_irqs_gsi; irq++ )
    {
        int rc;

        desc = irq_to_desc(irq);
        desc->irq = irq;

        rc = init_one_irq_desc(desc);
        if ( rc )
            return rc;
    }
    for ( ; irq < nr_irqs; irq++ )
        irq_to_desc(irq)->irq = irq;

    if ( !irq_max_guests )
        irq_max_guests = 32;

#ifdef CONFIG_PV
    /* Never allocate the Linux/BSD fast-trap vector. */
    set_bit(LEGACY_SYSCALL_VECTOR, used_vectors);
#endif

#ifdef CONFIG_PV32
    /* Never allocate the hypercall vector. */
    set_bit(HYPERCALL_VECTOR, used_vectors);
#endif
    
    /*
     * Mark vectors up to the cleanup one as used, to prevent an infinite loop
     * invoking irq_move_cleanup_interrupt.
     */
    BUILD_BUG_ON(IRQ_MOVE_CLEANUP_VECTOR < FIRST_DYNAMIC_VECTOR);
    for ( vector = FIRST_DYNAMIC_VECTOR;
          vector <= IRQ_MOVE_CLEANUP_VECTOR;
          vector++ )
        __set_bit(vector, used_vectors);

    return 0;
}

static void cf_check ack_none(struct irq_desc *desc)
{
    ack_bad_irq(desc->irq);
}

hw_irq_controller no_irq_type = {
    "none",
    irq_startup_none,
    irq_shutdown_none,
    irq_enable_none,
    irq_disable_none,
    ack_none,
};

static vmask_t *irq_get_used_vector_mask(int irq)
{
    vmask_t *ret = NULL;

    if ( opt_irq_vector_map == OPT_IRQ_VECTOR_MAP_GLOBAL )
    {
        struct irq_desc *desc = irq_to_desc(irq);

        ret = &global_used_vector_map;

        if ( desc->arch.used_vectors )
            printk(XENLOG_INFO "Unassigned IRQ %d already has used_vectors\n",
                   irq);
        else
        {
            int vector;
            
            vector = irq_to_vector(irq);
            if ( valid_irq_vector(vector) )
            {
                printk(XENLOG_INFO "IRQ%d already assigned vector %02x\n",
                       irq, vector);
                
                ASSERT(!test_bit(vector, ret));

                set_bit(vector, ret);
            }
            else if ( vector != IRQ_VECTOR_UNASSIGNED )
                printk(XENLOG_WARNING "IRQ%d mapped to bogus vector %02x\n",
                       irq, vector);
        }
    }
    else if ( IO_APIC_IRQ(irq) &&
              opt_irq_vector_map != OPT_IRQ_VECTOR_MAP_NONE )
    {
        ret = io_apic_get_used_vector_map(irq);
    }

    return ret;
}

static int _assign_irq_vector(struct irq_desc *desc, const cpumask_t *mask)
{
    /*
     * NOTE! The local APIC isn't very good at handling
     * multiple interrupts at the same interrupt level.
     * As the interrupt level is determined by taking the
     * vector number and shifting that right by 4, we
     * want to spread these out a bit so that they don't
     * all fall in the same interrupt level.
     *
     * Also, we've got to be careful not to trash gate
     * 0x80, because int 0x80 is hm, kind of importantish. ;)
     */
    static int current_vector = FIRST_DYNAMIC_VECTOR, current_offset = 0;
    unsigned int cpu;
    int err, old_vector, irq = desc->irq;
    vmask_t *irq_used_vectors = NULL;

    old_vector = irq_to_vector(irq);
    if ( valid_irq_vector(old_vector) )
    {
        cpumask_t tmp_mask;

        cpumask_and(&tmp_mask, mask, &cpu_online_map);
        if (cpumask_intersects(&tmp_mask, desc->arch.cpu_mask)) {
            desc->arch.vector = old_vector;
            return 0;
        }
    }

    if ( desc->arch.move_in_progress || desc->arch.move_cleanup_count )
        return -EAGAIN;

    err = -ENOSPC;

    /* This is the only place normal IRQs are ever marked
     * as "in use".  If they're not in use yet, check to see
     * if we need to assign a global vector mask. */
    if ( desc->arch.used == IRQ_USED )
    {
        irq_used_vectors = desc->arch.used_vectors;
    }
    else
        irq_used_vectors = irq_get_used_vector_mask(irq);

    for_each_cpu(cpu, mask)
    {
        const cpumask_t *vec_mask;
        int new_cpu;
        int vector, offset;

        /* Only try and allocate irqs on cpus that are present. */
        if (!cpu_online(cpu))
            continue;

        vec_mask = vector_allocation_cpumask(cpu);

        vector = current_vector;
        offset = current_offset;
next:
        vector += 8;
        if (vector > LAST_DYNAMIC_VECTOR) {
            /* If out of vectors on large boxen, must share them. */
            offset = (offset + 1) % 8;
            vector = FIRST_DYNAMIC_VECTOR + offset;
        }
        if (unlikely(current_vector == vector))
            continue;

        if (test_bit(vector, used_vectors))
            goto next;

        if (irq_used_vectors
            && test_bit(vector, irq_used_vectors) )
            goto next;

        if ( cpumask_test_cpu(0, vec_mask) &&
             vector >= FIRST_LEGACY_VECTOR && vector <= LAST_LEGACY_VECTOR )
            goto next;

        for_each_cpu(new_cpu, vec_mask)
            if (per_cpu(vector_irq, new_cpu)[vector] >= 0)
                goto next;
        /* Found one! */
        current_vector = vector;
        current_offset = offset;

        if ( valid_irq_vector(old_vector) )
        {
            cpumask_and(desc->arch.old_cpu_mask, desc->arch.cpu_mask,
                        &cpu_online_map);
            desc->arch.old_vector = desc->arch.vector;
            if ( !cpumask_empty(desc->arch.old_cpu_mask) )
                desc->arch.move_in_progress = 1;
            else
                /* This can happen while offlining a CPU. */
                release_old_vec(desc);
        }

        trace_irq_mask(TRC_HW_IRQ_ASSIGN_VECTOR, irq, vector, vec_mask);

        for_each_cpu(new_cpu, vec_mask)
            per_cpu(vector_irq, new_cpu)[vector] = irq;
        desc->arch.vector = vector;
        cpumask_copy(desc->arch.cpu_mask, vec_mask);

        desc->arch.used = IRQ_USED;
        ASSERT((desc->arch.used_vectors == NULL)
               || (desc->arch.used_vectors == irq_used_vectors));
        desc->arch.used_vectors = irq_used_vectors;

        if ( desc->arch.used_vectors )
        {
            ASSERT(!test_bit(vector, desc->arch.used_vectors));

            set_bit(vector, desc->arch.used_vectors);
        }

        err = 0;
        break;
    }
    return err;
}

int assign_irq_vector(int irq, const cpumask_t *mask)
{
    int ret;
    unsigned long flags;
    struct irq_desc *desc = irq_to_desc(irq);
    
    BUG_ON(irq >= nr_irqs || irq <0);

    spin_lock_irqsave(&desc->lock, flags);

    spin_lock(&vector_lock);
    ret = _assign_irq_vector(desc, mask ?: TARGET_CPUS);
    spin_unlock(&vector_lock);

    if ( !ret )
    {
        ret = desc->arch.vector;
        if ( mask )
            cpumask_copy(desc->affinity, mask);
        else
            cpumask_setall(desc->affinity);
    }

    spin_unlock_irqrestore(&desc->lock, flags);

    return ret;
}

/*
 * Initialize vector_irq on a new cpu. This function must be called
 * with vector_lock held.  For this reason it may not itself acquire
 * the IRQ descriptor locks, as lock nesting is the other way around.
 */
void setup_vector_irq(unsigned int cpu)
{
    unsigned int irq, vector;

    /* Clear vector_irq */
    for ( vector = 0; vector < X86_NR_VECTORS; ++vector )
        per_cpu(vector_irq, cpu)[vector] = INT_MIN;
    /* Mark the inuse vectors */
    for ( irq = 0; irq < nr_irqs; ++irq )
    {
        struct irq_desc *desc = irq_to_desc(irq);

        if ( !irq_desc_initialized(desc) )
            continue;
        vector = irq_to_vector(irq);
        if ( vector >= FIRST_HIPRIORITY_VECTOR &&
             vector <= LAST_HIPRIORITY_VECTOR )
            cpumask_set_cpu(cpu, desc->arch.cpu_mask);
        else if ( !cpumask_test_cpu(cpu, desc->arch.cpu_mask) )
            continue;
        per_cpu(vector_irq, cpu)[vector] = irq;
    }
}

void move_masked_irq(struct irq_desc *desc)
{
    cpumask_t *pending_mask = desc->arch.pending_mask;

    if (likely(!(desc->status & IRQ_MOVE_PENDING)))
        return;
    
    desc->status &= ~IRQ_MOVE_PENDING;

    if (!desc->handler->set_affinity)
        return;

    /*
     * If there was a valid mask to work with, please do the disable, 
     * re-program, enable sequence. This is *not* particularly important for 
     * level triggered but in a edge trigger case, we might be setting rte when 
     * an active trigger is comming in. This could cause some ioapics to 
     * mal-function. Being paranoid i guess!
     *
     * For correct operation this depends on the caller masking the irqs.
     */
    if ( likely(cpumask_intersects(pending_mask, &cpu_online_map)) )
        desc->handler->set_affinity(desc, pending_mask);

    cpumask_clear(pending_mask);
}

void move_native_irq(struct irq_desc *desc)
{
    if (likely(!(desc->status & IRQ_MOVE_PENDING)))
        return;

    if (unlikely(desc->status & IRQ_DISABLED))
        return;

    desc->handler->disable(desc);
    move_masked_irq(desc);
    desc->handler->enable(desc);
}

void cf_check irq_move_cleanup_interrupt(struct cpu_user_regs *regs)
{
    unsigned vector, me;

    ack_APIC_irq();

    me = smp_processor_id();
    if ( !cpu_online(me) )
        return;

    for ( vector = FIRST_DYNAMIC_VECTOR;
          vector <= LAST_HIPRIORITY_VECTOR; vector++)
    {
        unsigned int irq;
        unsigned int irr;
        struct irq_desc *desc;
        irq = per_cpu(vector_irq, me)[vector];

        if ((int)irq < 0)
            continue;

        desc = irq_to_desc(irq);
        if (!desc)
            continue;

        spin_lock(&desc->lock);

        if (desc->handler->enable == enable_8259A_irq)
            goto unlock;

        if (!desc->arch.move_cleanup_count)
            goto unlock;

        if ( vector == desc->arch.vector &&
             cpumask_test_cpu(me, desc->arch.cpu_mask) )
            goto unlock;

        irr = apic_read(APIC_IRR + (vector / 32 * 0x10));
        /*
         * Check if the vector that needs to be cleanedup is
         * registered at the cpu's IRR. If so, then this is not
         * the best time to clean it up. Lets clean it up in the
         * next attempt by sending another IRQ_MOVE_CLEANUP_VECTOR
         * to myself.
         */
        if ( irr & (1u << (vector % 32)) )
        {
            if ( vector < IRQ_MOVE_CLEANUP_VECTOR )
            {
                ASSERT_UNREACHABLE();
                goto unlock;
            }
            send_IPI_self(IRQ_MOVE_CLEANUP_VECTOR);
            TRACE_3D(TRC_HW_IRQ_MOVE_CLEANUP_DELAY,
                     irq, vector, smp_processor_id());
            goto unlock;
        }

        TRACE_3D(TRC_HW_IRQ_MOVE_CLEANUP,
                 irq, vector, smp_processor_id());

        per_cpu(vector_irq, me)[vector] = ~irq;
        desc->arch.move_cleanup_count--;

        if ( desc->arch.move_cleanup_count == 0 )
        {
            ASSERT(vector == desc->arch.old_vector);
            release_old_vec(desc);
        }
unlock:
        spin_unlock(&desc->lock);
    }
}

static void send_cleanup_vector(struct irq_desc *desc)
{
    cpumask_and(desc->arch.old_cpu_mask, desc->arch.old_cpu_mask,
                &cpu_online_map);
    desc->arch.move_cleanup_count = cpumask_weight(desc->arch.old_cpu_mask);

    if ( desc->arch.move_cleanup_count )
        send_IPI_mask(desc->arch.old_cpu_mask, IRQ_MOVE_CLEANUP_VECTOR);
    else
        release_old_vec(desc);

    desc->arch.move_in_progress = 0;
}

void cf_check irq_complete_move(struct irq_desc *desc)
{
    unsigned vector, me;

    if (likely(!desc->arch.move_in_progress))
        return;

    vector = (u8)get_irq_regs()->entry_vector;
    me = smp_processor_id();

    if ( vector == desc->arch.vector &&
         cpumask_test_cpu(me, desc->arch.cpu_mask) )
        send_cleanup_vector(desc);
}

unsigned int set_desc_affinity(struct irq_desc *desc, const cpumask_t *mask)
{
    int ret;
    unsigned long flags;
    cpumask_t dest_mask;

    if ( mask && !cpumask_intersects(mask, &cpu_online_map) )
        return BAD_APICID;

    spin_lock_irqsave(&vector_lock, flags);
    ret = _assign_irq_vector(desc, mask ?: TARGET_CPUS);
    spin_unlock_irqrestore(&vector_lock, flags);

    if ( ret < 0 )
        return BAD_APICID;

    if ( mask )
    {
        cpumask_copy(desc->affinity, mask);
        cpumask_and(&dest_mask, mask, desc->arch.cpu_mask);
    }
    else
    {
        cpumask_setall(desc->affinity);
        cpumask_copy(&dest_mask, desc->arch.cpu_mask);
    }
    cpumask_and(&dest_mask, &dest_mask, &cpu_online_map);

    return cpu_mask_to_apicid(&dest_mask);
}

/* For re-setting irq interrupt affinity for specific irq */
void irq_set_affinity(struct irq_desc *desc, const cpumask_t *mask)
{
    if (!desc->handler->set_affinity)
        return;
    
    ASSERT(spin_is_locked(&desc->lock));
    desc->status &= ~IRQ_MOVE_PENDING;
    smp_wmb();
    cpumask_copy(desc->arch.pending_mask, mask);
    smp_wmb();
    desc->status |= IRQ_MOVE_PENDING;
}

void pirq_set_affinity(struct domain *d, int pirq, const cpumask_t *mask)
{
    unsigned long flags;
    struct irq_desc *desc = domain_spin_lock_irq_desc(d, pirq, &flags);

    if ( !desc )
        return;
    irq_set_affinity(desc, mask);
    spin_unlock_irqrestore(&desc->lock, flags);
}

DEFINE_PER_CPU(unsigned int, irq_count);
static DEFINE_PER_CPU(bool, check_eoi_deferral);

uint8_t alloc_hipriority_vector(void)
{
    static uint8_t next = FIRST_HIPRIORITY_VECTOR;
    BUG_ON(next < FIRST_HIPRIORITY_VECTOR);
    BUG_ON(next > LAST_HIPRIORITY_VECTOR);
    return next++;
}

static void (*direct_apic_vector[X86_NR_VECTORS])(struct cpu_user_regs *);
void set_direct_apic_vector(
    uint8_t vector, void (*handler)(struct cpu_user_regs *))
{
    BUG_ON(direct_apic_vector[vector] != NULL);
    direct_apic_vector[vector] = handler;
}

void alloc_direct_apic_vector(
    uint8_t *vector, void (*handler)(struct cpu_user_regs *))
{
    static DEFINE_SPINLOCK(lock);

    spin_lock(&lock);
    if (*vector == 0) {
        *vector = alloc_hipriority_vector();
        set_direct_apic_vector(*vector, handler);
    }
    spin_unlock(&lock);
}

static void cf_check irq_ratelimit_timer_fn(void *data)
{
    struct irq_desc *desc, *tmp;
    unsigned long flags;

    spin_lock_irqsave(&irq_ratelimit_lock, flags);

    list_for_each_entry_safe ( desc, tmp, &irq_ratelimit_list, rl_link )
    {
        spin_lock(&desc->lock);
        desc->handler->enable(desc);
        list_del(&desc->rl_link);
        INIT_LIST_HEAD(&desc->rl_link);
        spin_unlock(&desc->lock);
    }

    spin_unlock_irqrestore(&irq_ratelimit_lock, flags);
}

static int __init cf_check irq_ratelimit_init(void)
{
    if ( irq_ratelimit_threshold )
        init_timer(&irq_ratelimit_timer, irq_ratelimit_timer_fn, NULL, 0);
    return 0;
}
__initcall(irq_ratelimit_init);

int __init request_irq(unsigned int irq, unsigned int irqflags,
        void (*handler)(int, void *, struct cpu_user_regs *),
        const char * devname, void *dev_id)
{
    struct irqaction * action;
    int retval;

    /*
     * Sanity-check: shared interrupts must pass in a real dev-ID,
     * otherwise we'll have trouble later trying to figure out
     * which interrupt is which (messes up the interrupt freeing
     * logic etc).
     */
    if (irq >= nr_irqs)
        return -EINVAL;
    if (!handler)
        return -EINVAL;

    action = xmalloc(struct irqaction);
    if (!action)
        return -ENOMEM;

    action->handler = handler;
    action->name = devname;
    action->dev_id = dev_id;
    action->free_on_release = 1;

    retval = setup_irq(irq, irqflags, action);
    if (retval)
        xfree(action);

    return retval;
}

void __init release_irq(unsigned int irq, const void *dev_id)
{
    struct irq_desc *desc;
    unsigned long flags;
    struct irqaction *action;

    desc = irq_to_desc(irq);

    spin_lock_irqsave(&desc->lock,flags);
    action = desc->action;
    desc->action  = NULL;
    desc->handler->shutdown(desc);
    desc->status |= IRQ_DISABLED;
    spin_unlock_irqrestore(&desc->lock,flags);

    /* Wait to make sure it's not being used on another CPU */
    do { smp_mb(); } while ( desc->status & IRQ_INPROGRESS );

    if (action && action->free_on_release)
        xfree(action);
}

int __init setup_irq(unsigned int irq, unsigned int irqflags,
                     struct irqaction *new)
{
    struct irq_desc *desc;
    unsigned long flags;

    ASSERT(irqflags == 0);

    desc = irq_to_desc(irq);
 
    spin_lock_irqsave(&desc->lock,flags);

    if ( desc->action != NULL )
    {
        spin_unlock_irqrestore(&desc->lock,flags);
        return -EBUSY;
    }

    desc->action  = new;
    desc->status &= ~IRQ_DISABLED;
    desc->handler->startup(desc);

    spin_unlock_irqrestore(&desc->lock,flags);

    return 0;
}


/*
 * HANDLING OF GUEST-BOUND PHYSICAL IRQS
 */

typedef struct {
    u8 nr_guests;
    u8 in_flight;
    u8 shareable;
    u8 ack_type;
#define ACKTYPE_NONE   0     /* No final acknowledgement is required */
#define ACKTYPE_UNMASK 1     /* Unmask PIC hardware (from any CPU)   */
#define ACKTYPE_EOI    2     /* EOI on the CPU that was interrupted  */
    cpumask_var_t cpu_eoi_map; /* CPUs that need to EOI this interrupt */
    struct timer eoi_timer;
    struct domain *guest[];
} irq_guest_action_t;

static irq_guest_action_t *guest_action(const struct irq_desc *desc)
{
    return desc->status & IRQ_GUEST ? (void *)desc->action : NULL;
}

/*
 * Stack of interrupts awaiting EOI on each CPU. These must be popped in
 * order, as only the current highest-priority pending irq can be EOIed.
 */
struct pending_eoi {
    u32 ready:1;  /* Ready for EOI now?  */
    u32 irq:23;   /* irq of the vector */
    u32 vector:8; /* vector awaiting EOI */
};

static DEFINE_PER_CPU(struct pending_eoi, pending_eoi[NR_DYNAMIC_VECTORS]);
#define pending_eoi_sp(p) ((p)[NR_DYNAMIC_VECTORS-1].vector)

bool cpu_has_pending_apic_eoi(void)
{
    return pending_eoi_sp(this_cpu(pending_eoi)) != 0;
}

void cf_check end_nonmaskable_irq(struct irq_desc *desc, uint8_t vector)
{
    struct pending_eoi *peoi = this_cpu(pending_eoi);
    unsigned int sp = pending_eoi_sp(peoi);

    if ( !this_cpu(check_eoi_deferral) || !sp || peoi[sp - 1].vector < vector )
    {
        ack_APIC_irq();
        return;
    }

    /* Defer this vector's EOI until all higher ones have been EOI-ed. */
    pending_eoi_sp(peoi) = sp + 1;
    do {
        peoi[sp] = peoi[sp - 1];
    } while ( --sp && peoi[sp - 1].vector > vector );
    ASSERT(!sp || peoi[sp - 1].vector < vector);

    peoi[sp].irq = desc->irq;
    peoi[sp].vector = vector;
    peoi[sp].ready = 1;
}

static inline void set_pirq_eoi(struct domain *d, unsigned int irq)
{
    if ( d->arch.pirq_eoi_map )
    {
        ASSERT(irq < PAGE_SIZE * BITS_PER_BYTE);
        set_bit(irq, d->arch.pirq_eoi_map);
    }
}

static inline void clear_pirq_eoi(struct domain *d, unsigned int irq)
{
    if ( d->arch.pirq_eoi_map )
    {
        ASSERT(irq < PAGE_SIZE * BITS_PER_BYTE);
        clear_bit(irq, d->arch.pirq_eoi_map);
    }
}

static void cf_check set_eoi_ready(void *data);

static void cf_check irq_guest_eoi_timer_fn(void *data)
{
    struct irq_desc *desc = data;
    unsigned int i, irq = desc - irq_desc;
    irq_guest_action_t *action;

    spin_lock_irq(&desc->lock);
    
    if ( !(action = guest_action(desc)) )
        goto out;

    ASSERT(action->ack_type != ACKTYPE_NONE);

    /*
     * Is no IRQ in flight at all, or another instance of this timer already
     * running? Skip everything to avoid forcing an EOI early.
     */
    if ( !action->in_flight || timer_is_active(&action->eoi_timer) )
        goto out;

    for ( i = 0; i < action->nr_guests; i++ )
    {
        struct domain *d = action->guest[i];
        unsigned int pirq = domain_irq_to_pirq(d, irq);

        if ( test_and_clear_bool(pirq_info(d, pirq)->masked) )
            action->in_flight--;
    }

    if ( action->in_flight )
    {
        printk(XENLOG_G_WARNING
               "IRQ%u: %d/%d handler(s) still in flight at forced EOI\n",
               irq, action->in_flight, action->nr_guests);
        ASSERT_UNREACHABLE();
    }

    switch ( action->ack_type )
    {
        cpumask_t *cpu_eoi_map;

    case ACKTYPE_UNMASK:
        if ( desc->handler->end )
            desc->handler->end(desc, 0);
        break;

    case ACKTYPE_EOI:
        cpu_eoi_map = this_cpu(scratch_cpumask);
        cpumask_copy(cpu_eoi_map, action->cpu_eoi_map);
        spin_unlock_irq(&desc->lock);
        on_selected_cpus(cpu_eoi_map, set_eoi_ready, desc, 0);
        return;
    }

 out:
    spin_unlock_irq(&desc->lock);
}

/*
 * Retrieve Xen irq-descriptor corresponding to a domain-specific irq.
 * The descriptor is returned locked. This function is safe against changes
 * to the per-domain irq-to-vector mapping.
 */
struct irq_desc *domain_spin_lock_irq_desc(
    struct domain *d, int pirq, unsigned long *pflags)
{
    const struct pirq *info = pirq_info(d, pirq);

    return info ? pirq_spin_lock_irq_desc(info, pflags) : NULL;
}

/*
 * Same with struct pirq already looked up.
 */
struct irq_desc *pirq_spin_lock_irq_desc(
    const struct pirq *pirq, unsigned long *pflags)
{
    struct irq_desc *desc;
    unsigned long flags;

    for ( ; ; )
    {
        int irq = pirq->arch.irq;

        if ( irq <= 0 )
            return NULL;

        desc = irq_to_desc(irq);
        spin_lock_irqsave(&desc->lock, flags);
        if ( irq == pirq->arch.irq )
            break;
        spin_unlock_irqrestore(&desc->lock, flags);
    }

    if ( pflags )
        *pflags = flags;

    return desc;
}

static int prepare_domain_irq_pirq(struct domain *d, int irq, int pirq,
                                struct pirq **pinfo)
{
    int err = radix_tree_insert(&d->arch.irq_pirq, irq,
                                radix_tree_int_to_ptr(0));
    struct pirq *info;

    if ( err && err != -EEXIST )
        return err;
    info = pirq_get_info(d, pirq);
    if ( !info )
    {
        if ( !err )
            radix_tree_delete(&d->arch.irq_pirq, irq);
        return -ENOMEM;
    }
    *pinfo = info;

    return !!err;
}

static void set_domain_irq_pirq(struct domain *d, int irq, struct pirq *pirq)
{
    radix_tree_replace_slot(
        radix_tree_lookup_slot(&d->arch.irq_pirq, irq),
        radix_tree_int_to_ptr(pirq->pirq));
    pirq->arch.irq = irq;
}

static void clear_domain_irq_pirq(struct domain *d, int irq, struct pirq *pirq)
{
    pirq->arch.irq = 0;
    radix_tree_replace_slot(
        radix_tree_lookup_slot(&d->arch.irq_pirq, irq),
        radix_tree_int_to_ptr(0));
}

static void cleanup_domain_irq_pirq(struct domain *d, int irq,
                                    struct pirq *pirq)
{
    pirq_cleanup_check(pirq, d);
    radix_tree_delete(&d->arch.irq_pirq, irq);
}

int init_domain_irq_mapping(struct domain *d)
{
    unsigned int i;
    int err = 0;

    radix_tree_init(&d->arch.irq_pirq);
    if ( is_hvm_domain(d) )
        radix_tree_init(&d->arch.hvm.emuirq_pirq);

    for ( i = 1; platform_legacy_irq(i); ++i )
    {
        struct pirq *info;

        if ( IO_APIC_IRQ(i) )
            continue;
        err = prepare_domain_irq_pirq(d, i, i, &info);
        if ( err )
        {
            ASSERT(err < 0);
            break;
        }
        set_domain_irq_pirq(d, i, info);
    }

    if ( err )
        cleanup_domain_irq_mapping(d);
    return err;
}

void cleanup_domain_irq_mapping(struct domain *d)
{
    radix_tree_destroy(&d->arch.irq_pirq, NULL);
    if ( is_hvm_domain(d) )
        radix_tree_destroy(&d->arch.hvm.emuirq_pirq, NULL);
}

struct pirq *alloc_pirq_struct(struct domain *d)
{
    size_t sz = is_hvm_domain(d) ? sizeof(struct pirq) :
                                   offsetof(struct pirq, arch.hvm);
    struct pirq *pirq = xzalloc_bytes(sz);

    if ( pirq )
    {
        if ( is_hvm_domain(d) )
        {
            pirq->arch.hvm.emuirq = IRQ_UNBOUND;
            pt_pirq_init(d, &pirq->arch.hvm.dpci);
        }
    }

    return pirq;
}

void (pirq_cleanup_check)(struct pirq *pirq, struct domain *d)
{
    /*
     * Check whether all fields have their default values, and delete
     * the entry from the tree if so.
     *
     * NB: Common parts were already checked.
     */
    if ( pirq->arch.irq )
        return;

    if ( is_hvm_domain(d) )
    {
        if ( pirq->arch.hvm.emuirq != IRQ_UNBOUND )
            return;
        if ( !pt_pirq_cleanup_check(&pirq->arch.hvm.dpci) )
            return;
    }

    if ( radix_tree_delete(&d->pirq_tree, pirq->pirq) != pirq )
        BUG();
}

/* Flush all ready EOIs from the top of this CPU's pending-EOI stack. */
static void flush_ready_eoi(void)
{
    struct pending_eoi *peoi = this_cpu(pending_eoi);
    struct irq_desc         *desc;
    int                irq, sp;

    ASSERT(!local_irq_is_enabled());

    sp = pending_eoi_sp(peoi);

    while ( (--sp >= 0) && peoi[sp].ready )
    {
        irq = peoi[sp].irq;
        ASSERT(irq > 0);
        desc = irq_to_desc(irq);
        spin_lock(&desc->lock);
        if ( desc->handler->end )
            desc->handler->end(desc, peoi[sp].vector);
        spin_unlock(&desc->lock);
    }

    pending_eoi_sp(peoi) = sp+1;
}

static void __set_eoi_ready(const struct irq_desc *desc)
{
    irq_guest_action_t *action = guest_action(desc);
    struct pending_eoi *peoi = this_cpu(pending_eoi);
    int                 irq, sp;

    irq = desc - irq_desc;

    if ( !action || action->in_flight ||
         !cpumask_test_and_clear_cpu(smp_processor_id(),
                                     action->cpu_eoi_map) )
        return;

    sp = pending_eoi_sp(peoi);

    do {
        ASSERT(sp > 0);
    } while ( peoi[--sp].irq != irq );
    ASSERT(!peoi[sp].ready);
    peoi[sp].ready = 1;
}

/* Mark specified IRQ as ready-for-EOI (if it really is) and attempt to EOI. */
static void cf_check set_eoi_ready(void *data)
{
    struct irq_desc *desc = data;

    ASSERT(!local_irq_is_enabled());

    spin_lock(&desc->lock);
    __set_eoi_ready(desc);
    spin_unlock(&desc->lock);

    flush_ready_eoi();
}

void pirq_guest_eoi(struct pirq *pirq)
{
    struct irq_desc *desc;

    ASSERT(local_irq_is_enabled());
    desc = pirq_spin_lock_irq_desc(pirq, NULL);
    if ( desc )
        desc_guest_eoi(desc, pirq);
}

void desc_guest_eoi(struct irq_desc *desc, struct pirq *pirq)
{
    irq_guest_action_t *action = guest_action(desc);
    cpumask_t           cpu_eoi_map;

    if ( unlikely(!action) ||
         unlikely(!test_and_clear_bool(pirq->masked)) ||
         unlikely(--action->in_flight != 0) )
    {
        spin_unlock_irq(&desc->lock);
        return;
    }

    stop_timer(&action->eoi_timer);

    if ( action->ack_type == ACKTYPE_UNMASK )
    {
        ASSERT(cpumask_empty(action->cpu_eoi_map));
        if ( desc->handler->end )
            desc->handler->end(desc, 0);
        spin_unlock_irq(&desc->lock);
        return;
    }

    ASSERT(action->ack_type == ACKTYPE_EOI);
        
    cpumask_copy(&cpu_eoi_map, action->cpu_eoi_map);

    if ( __cpumask_test_and_clear_cpu(smp_processor_id(), &cpu_eoi_map) )
    {
        __set_eoi_ready(desc);
        spin_unlock(&desc->lock);
        flush_ready_eoi();
        local_irq_enable();
    }
    else
    {
        spin_unlock_irq(&desc->lock);
    }

    if ( !cpumask_empty(&cpu_eoi_map) )
        on_selected_cpus(&cpu_eoi_map, set_eoi_ready, desc, 0);
}

int pirq_guest_unmask(struct domain *d)
{
    unsigned int pirq = 0, n, i;
    struct pirq *pirqs[16];

    do {
        n = radix_tree_gang_lookup(&d->pirq_tree, (void **)pirqs, pirq,
                                   ARRAY_SIZE(pirqs));
        for ( i = 0; i < n; ++i )
        {
            pirq = pirqs[i]->pirq;
            if ( pirqs[i]->masked &&
                 !evtchn_port_is_masked(d, pirqs[i]->evtchn) )
                pirq_guest_eoi(pirqs[i]);
        }
    } while ( ++pirq < d->nr_pirqs && n == ARRAY_SIZE(pirqs) );

    return 0;
}

static int irq_acktype(const struct irq_desc *desc)
{
    if ( desc->handler == &no_irq_type )
        return ACKTYPE_NONE;

    /*
     * Edge-triggered IO-APIC and LAPIC interrupts need no final
     * acknowledgement: we ACK early during interrupt processing.
     */
    if ( !strcmp(desc->handler->typename, "IO-APIC-edge") ||
         !strcmp(desc->handler->typename, "local-APIC-edge") )
        return ACKTYPE_NONE;

    /*
     * MSIs are treated as edge-triggered interrupts, except
     * when there is no proper way to mask them.
     */
    if ( desc->msi_desc )
        return msi_maskable_irq(desc->msi_desc) ? ACKTYPE_NONE : ACKTYPE_EOI;

    /*
     * Level-triggered IO-APIC interrupts need to be acknowledged on the CPU
     * on which they were received. This is because we tickle the LAPIC to EOI.
     */
    if ( !strcmp(desc->handler->typename, "IO-APIC-level") )
        return desc->handler->ack == irq_complete_move ?
               ACKTYPE_EOI : ACKTYPE_UNMASK;

    /* Legacy PIC interrupts can be acknowledged from any CPU. */
    if ( !strcmp(desc->handler->typename, "XT-PIC") )
        return ACKTYPE_UNMASK;

    printk("Unknown PIC type '%s' for IRQ%d\n",
           desc->handler->typename, desc->irq);
    BUG();

    return 0;
}

int pirq_shared(struct domain *d, int pirq)
{
    struct irq_desc    *desc;
    const irq_guest_action_t *action;
    unsigned long       flags;
    int                 shared;

    desc = domain_spin_lock_irq_desc(d, pirq, &flags);
    if ( desc == NULL )
        return 0;

    action = guest_action(desc);
    shared = (action && (action->nr_guests > 1));

    spin_unlock_irqrestore(&desc->lock, flags);

    return shared;
}

int pirq_guest_bind(struct vcpu *v, struct pirq *pirq, int will_share)
{
    struct irq_desc         *desc;
    irq_guest_action_t *action, *newaction = NULL;
    unsigned int        max_nr_guests = will_share ? irq_max_guests : 1;
    int                 rc = 0;

    WARN_ON(!rw_is_write_locked(&v->domain->event_lock));
    BUG_ON(!local_irq_is_enabled());

 retry:
    desc = pirq_spin_lock_irq_desc(pirq, NULL);
    if ( desc == NULL )
    {
        rc = -EINVAL;
        goto out;
    }

    if ( !(action = guest_action(desc)) )
    {
        if ( desc->action != NULL )
        {
            printk(XENLOG_G_INFO
                   "Cannot bind IRQ%d to dom%d. In use by '%s'.\n",
                   pirq->pirq, v->domain->domain_id, desc->action->name);
            rc = -EBUSY;
            goto unlock_out;
        }

        if ( newaction == NULL )
        {
            spin_unlock_irq(&desc->lock);
            if ( (newaction = xmalloc_flex_struct(irq_guest_action_t, guest,
                                                  max_nr_guests)) != NULL &&
                 zalloc_cpumask_var(&newaction->cpu_eoi_map) )
                goto retry;
            xfree(newaction);
            printk(XENLOG_G_INFO
                   "Cannot bind IRQ%d to dom%d. Out of memory.\n",
                   pirq->pirq, v->domain->domain_id);
            return -ENOMEM;
        }

        action = newaction;
        desc->action = (struct irqaction *)action;
        newaction = NULL;

        action->nr_guests   = 0;
        action->in_flight   = 0;
        action->shareable   = will_share;
        action->ack_type    = irq_acktype(desc);
        init_timer(&action->eoi_timer, irq_guest_eoi_timer_fn, desc, 0);

        desc->status |= IRQ_GUEST;

        /*
         * Attempt to bind the interrupt target to the correct (or at least
         * some online) CPU.
         */
        if ( desc->handler->set_affinity )
        {
            const cpumask_t *affinity = NULL;

            if ( !opt_noirqbalance )
                affinity = cpumask_of(v->processor);
            else if ( !cpumask_intersects(desc->affinity, &cpu_online_map) )
            {
                cpumask_setall(desc->affinity);
                affinity = &cpumask_all;
            }
            else if ( !cpumask_intersects(desc->arch.cpu_mask,
                                          &cpu_online_map) )
                affinity = desc->affinity;
            if ( affinity )
                desc->handler->set_affinity(desc, affinity);
        }

        desc->status &= ~IRQ_DISABLED;
        desc->handler->startup(desc);
    }
    else if ( !will_share || !action->shareable )
    {
        printk(XENLOG_G_INFO "Cannot bind IRQ%d to dom%d. %s.\n",
               pirq->pirq, v->domain->domain_id,
               will_share ? "Others do not share"
                          : "Will not share with others");
        rc = -EBUSY;
        goto unlock_out;
    }
    else if ( action->nr_guests == 0 )
    {
        /*
         * Indicates that an ACKTYPE_EOI interrupt is being released.
         * Wait for that to happen before continuing.
         */
        ASSERT(action->ack_type == ACKTYPE_EOI);
        ASSERT(desc->status & IRQ_DISABLED);
        spin_unlock_irq(&desc->lock);
        cpu_relax();
        goto retry;
    }

    if ( action->nr_guests >= max_nr_guests )
    {
        printk(XENLOG_G_INFO
               "Cannot bind IRQ%d to %pd: already at max share %u"
               " (increase with irq-max-guests= option)\n",
               pirq->pirq, v->domain, irq_max_guests);
        rc = -EBUSY;
        goto unlock_out;
    }

    action->guest[action->nr_guests++] = v->domain;

    if ( action->ack_type != ACKTYPE_NONE )
        set_pirq_eoi(v->domain, pirq->pirq);
    else
        clear_pirq_eoi(v->domain, pirq->pirq);

 unlock_out:
    spin_unlock_irq(&desc->lock);
 out:
    if ( newaction != NULL )
    {
        free_cpumask_var(newaction->cpu_eoi_map);
        xfree(newaction);
    }
    return rc;
}

static irq_guest_action_t *__pirq_guest_unbind(
    struct domain *d, struct pirq *pirq, struct irq_desc *desc)
{
    irq_guest_action_t *action = guest_action(desc);
    cpumask_t           cpu_eoi_map;
    int                 i;

    if ( unlikely(action == NULL) )
    {
        dprintk(XENLOG_G_WARNING, "dom%d: pirq %d: desc->action is NULL!\n",
                d->domain_id, pirq->pirq);
        BUG_ON(!(desc->status & IRQ_GUEST));
        return NULL;
    }

    for ( i = 0; (i < action->nr_guests) && (action->guest[i] != d); i++ )
        continue;
    BUG_ON(i == action->nr_guests);
    memmove(&action->guest[i], &action->guest[i+1],
            (action->nr_guests-i-1) * sizeof(action->guest[0]));
    action->nr_guests--;

    switch ( action->ack_type )
    {
    case ACKTYPE_UNMASK:
        if ( test_and_clear_bool(pirq->masked) &&
             (--action->in_flight == 0) &&
             desc->handler->end )
                desc->handler->end(desc, 0);
        break;
    case ACKTYPE_EOI:
        /* NB. If #guests == 0 then we clear the eoi_map later on. */
        if ( test_and_clear_bool(pirq->masked) &&
             (--action->in_flight == 0) &&
             (action->nr_guests != 0) )
        {
            cpumask_copy(&cpu_eoi_map, action->cpu_eoi_map);
            spin_unlock_irq(&desc->lock);
            on_selected_cpus(&cpu_eoi_map, set_eoi_ready, desc, 0);
            spin_lock_irq(&desc->lock);
        }
        break;
    }

    /*
     * The guest cannot re-bind to this IRQ until this function returns. So,
     * when we have flushed this IRQ from ->masked, it should remain flushed.
     */
    BUG_ON(pirq->masked);

    if ( action->nr_guests != 0 )
        return NULL;

    BUG_ON(action->in_flight != 0);

    /* Disabling IRQ before releasing the desc_lock avoids an IRQ storm. */
    desc->handler->disable(desc);
    desc->status |= IRQ_DISABLED;

    /*
     * Mark any remaining pending EOIs as ready to flush.
     * NOTE: We will need to make this a stronger barrier if in future we allow
     * an interrupt vectors to be re-bound to a different PIC. In that case we
     * would need to flush all ready EOIs before returning as otherwise the
     * desc->handler could change and we would call the wrong 'end' hook.
     */
    cpumask_copy(&cpu_eoi_map, action->cpu_eoi_map);
    if ( !cpumask_empty(&cpu_eoi_map) )
    {
        BUG_ON(action->ack_type != ACKTYPE_EOI);
        spin_unlock_irq(&desc->lock);
        on_selected_cpus(&cpu_eoi_map, set_eoi_ready, desc, 1);
        spin_lock_irq(&desc->lock);
    }

    BUG_ON(!cpumask_empty(action->cpu_eoi_map));

    desc->action = NULL;
    desc->status &= ~(IRQ_GUEST|IRQ_INPROGRESS);
    desc->handler->shutdown(desc);

    /* Caller frees the old guest descriptor block. */
    return action;
}

void pirq_guest_unbind(struct domain *d, struct pirq *pirq)
{
    irq_guest_action_t *oldaction = NULL;
    struct irq_desc *desc;
    int irq = 0;

    WARN_ON(!rw_is_write_locked(&d->event_lock));

    BUG_ON(!local_irq_is_enabled());
    desc = pirq_spin_lock_irq_desc(pirq, NULL);

    if ( desc == NULL )
    {
        irq = -pirq->arch.irq;
        BUG_ON(irq <= 0);
        desc = irq_to_desc(irq);
        spin_lock_irq(&desc->lock);
        clear_domain_irq_pirq(d, irq, pirq);
    }
    else
    {
        oldaction = __pirq_guest_unbind(d, pirq, desc);
    }

    spin_unlock_irq(&desc->lock);

    if ( oldaction != NULL )
    {
        kill_timer(&oldaction->eoi_timer);
        free_cpumask_var(oldaction->cpu_eoi_map);
        xfree(oldaction);
    }
    else if ( irq > 0 )
        cleanup_domain_irq_pirq(d, irq, pirq);
}

static bool pirq_guest_force_unbind(struct domain *d, struct pirq *pirq)
{
    struct irq_desc *desc;
    irq_guest_action_t *action, *oldaction = NULL;
    unsigned int i;
    bool bound = false;

    WARN_ON(!rw_is_write_locked(&d->event_lock));

    BUG_ON(!local_irq_is_enabled());
    desc = pirq_spin_lock_irq_desc(pirq, NULL);
    BUG_ON(desc == NULL);

    action = guest_action(desc);
    if ( unlikely(action == NULL) )
    {
        if ( desc->status & IRQ_GUEST )
            dprintk(XENLOG_G_WARNING, "%pd: pirq %d: desc->action is NULL!\n",
                    d, pirq->pirq);
        goto out;
    }

    for ( i = 0; (i < action->nr_guests) && (action->guest[i] != d); i++ )
        continue;
    if ( i == action->nr_guests )
        goto out;

    bound = true;
    oldaction = __pirq_guest_unbind(d, pirq, desc);

 out:
    spin_unlock_irq(&desc->lock);

    if ( oldaction != NULL )
    {
        kill_timer(&oldaction->eoi_timer);
        free_cpumask_var(oldaction->cpu_eoi_map);
        xfree(oldaction);
    }

    return bound;
}

static void do_IRQ_guest(struct irq_desc *desc, unsigned int vector)
{
    irq_guest_action_t *action = guest_action(desc);
    unsigned int        i;
    struct pending_eoi *peoi = this_cpu(pending_eoi);

    if ( unlikely(!action->nr_guests) )
    {
        /* An interrupt may slip through while freeing an ACKTYPE_EOI irq. */
        ASSERT(action->ack_type == ACKTYPE_EOI);
        ASSERT(desc->status & IRQ_DISABLED);
        if ( desc->handler->end )
            desc->handler->end(desc, vector);
        return;
    }

    /*
     * Stop the timer as soon as we're certain we'll set it again further down,
     * to prevent the current timeout (if any) to needlessly expire.
     */
    if ( action->ack_type != ACKTYPE_NONE )
        stop_timer(&action->eoi_timer);

    if ( action->ack_type == ACKTYPE_EOI )
    {
        unsigned int sp = pending_eoi_sp(peoi);

        ASSERT(sp < (NR_DYNAMIC_VECTORS - 1));
        ASSERT(!sp || (peoi[sp - 1].vector < vector));
        peoi[sp].irq = desc->irq;
        peoi[sp].vector = vector;
        peoi[sp].ready = 0;
        pending_eoi_sp(peoi) = sp + 1;
        cpumask_set_cpu(smp_processor_id(), action->cpu_eoi_map);
    }

    for ( i = 0; i < action->nr_guests; i++ )
    {
        struct domain *d = action->guest[i];
        struct pirq *pirq = pirq_info(d, domain_irq_to_pirq(d, desc->irq));;

        if ( (action->ack_type != ACKTYPE_NONE) &&
             !test_and_set_bool(pirq->masked) )
            action->in_flight++;
        if ( !is_hvm_domain(d) || !hvm_do_IRQ_dpci(d, pirq) )
            send_guest_pirq(d, pirq);
    }

    if ( action->ack_type != ACKTYPE_NONE )
    {
        migrate_timer(&action->eoi_timer, smp_processor_id());
        set_timer(&action->eoi_timer, NOW() + MILLISECS(1));
    }
}

void do_IRQ(struct cpu_user_regs *regs)
{
    struct irqaction *action;
    uint32_t          tsc_in;
    struct irq_desc  *desc;
    unsigned int      vector = (uint8_t)regs->entry_vector;
    int               irq = this_cpu(vector_irq)[vector];
    struct cpu_user_regs *old_regs = set_irq_regs(regs);

    perfc_incr(irqs);
    this_cpu(irq_count)++;
    irq_enter();

    if ( irq < 0 )
    {
        if ( direct_apic_vector[vector] )
            direct_apic_vector[vector](regs);
        else
        {
            const char *kind = ", LAPIC";

            if ( apic_isr_read(vector) )
                ack_APIC_irq();
            else
                kind = "";
            if ( !(vector >= FIRST_LEGACY_VECTOR &&
                   vector <= LAST_LEGACY_VECTOR &&
                   !smp_processor_id() &&
                   bogus_8259A_irq(vector - FIRST_LEGACY_VECTOR)) )
            {
                printk("CPU%u: No irq handler for vector %02x (IRQ %d%s)\n",
                       smp_processor_id(), vector, irq, kind);
                desc = irq_to_desc(~irq);
                if ( ~irq < nr_irqs && irq_desc_initialized(desc) )
                {
                    spin_lock(&desc->lock);
                    printk("IRQ%d a=%04lx[%04lx,%04lx] v=%02x[%02x] t=%s s=%08x\n",
                           ~irq, *cpumask_bits(desc->affinity),
                           *cpumask_bits(desc->arch.cpu_mask),
                           *cpumask_bits(desc->arch.old_cpu_mask),
                           desc->arch.vector, desc->arch.old_vector,
                           desc->handler->typename, desc->status);
                    spin_unlock(&desc->lock);
                }
            }
            TRACE_1D(TRC_HW_IRQ_UNMAPPED_VECTOR, vector);
        }
        goto out_no_unlock;
    }

    desc = irq_to_desc(irq);

    spin_lock(&desc->lock);
    desc->handler->ack(desc);

    if ( likely(desc->status & IRQ_GUEST) )
    {
        if ( irq_ratelimit_timer.function && /* irq rate limiting enabled? */
             unlikely(desc->rl_cnt++ >= irq_ratelimit_threshold) )
        {
            s_time_t now = NOW();

            if ( now < (desc->rl_quantum_start + MILLISECS(10)) )
            {
                desc->handler->disable(desc);
                /*
                 * If handler->disable doesn't actually mask the interrupt, a
                 * disabled irq still can fire. This check also avoids possible
                 * deadlocks if ratelimit_timer_fn runs at the same time.
                 */
                if ( likely(list_empty(&desc->rl_link)) )
                {
                    spin_lock(&irq_ratelimit_lock);
                    if ( list_empty(&irq_ratelimit_list) )
                        set_timer(&irq_ratelimit_timer, now + MILLISECS(10));
                    list_add(&desc->rl_link, &irq_ratelimit_list);
                    spin_unlock(&irq_ratelimit_lock);
                }
                goto out;
            }
            desc->rl_cnt = 0;
            desc->rl_quantum_start = now;
        }

        tsc_in = tb_init_done ? get_cycles() : 0;
        do_IRQ_guest(desc, vector);
        TRACE_3D(TRC_HW_IRQ_HANDLED, irq, tsc_in, get_cycles());
        goto out_no_end;
    }

    desc->status &= ~IRQ_REPLAY;
    desc->status |= IRQ_PENDING;

    /*
     * Since we set PENDING, if another processor is handling a different
     * instance of this same irq, the other processor will take care of it.
     */
    if ( desc->status & (IRQ_DISABLED | IRQ_INPROGRESS) )
        goto out;

    desc->status |= IRQ_INPROGRESS;

    action = desc->action;
    while ( desc->status & IRQ_PENDING )
    {
        desc->status &= ~IRQ_PENDING;
        spin_unlock_irq(&desc->lock);

        tsc_in = tb_init_done ? get_cycles() : 0;
        action->handler(irq, action->dev_id, regs);
        TRACE_3D(TRC_HW_IRQ_HANDLED, irq, tsc_in, get_cycles());

        spin_lock_irq(&desc->lock);
    }

    desc->status &= ~IRQ_INPROGRESS;

 out:
    if ( desc->handler->end )
    {
        /*
         * If higher priority vectors still have their EOIs pending, we may
         * not issue an EOI here, as this would EOI the highest priority one.
         */
        this_cpu(check_eoi_deferral) = true;
        desc->handler->end(desc, vector);
        this_cpu(check_eoi_deferral) = false;

        spin_unlock(&desc->lock);
        flush_ready_eoi();
        goto out_no_unlock;
    }

 out_no_end:
    spin_unlock(&desc->lock);
 out_no_unlock:
    irq_exit();
    set_irq_regs(old_regs);
}

static inline bool is_free_pirq(const struct domain *d,
                                const struct pirq *pirq)
{
    return !pirq || (!pirq->arch.irq && (!is_hvm_domain(d) ||
        pirq->arch.hvm.emuirq == IRQ_UNBOUND));
}

int get_free_pirq(struct domain *d, int type)
{
    int i;

    ASSERT(rw_is_write_locked(&d->event_lock));

    if ( type == MAP_PIRQ_TYPE_GSI )
    {
        for ( i = 16; i < nr_irqs_gsi; i++ )
            if ( is_free_pirq(d, pirq_info(d, i)) )
            {
                pirq_get_info(d, i);
                return i;
            }
    }
    for ( i = d->nr_pirqs - 1; i >= nr_irqs_gsi; i-- )
        if ( is_free_pirq(d, pirq_info(d, i)) )
        {
            pirq_get_info(d, i);
            return i;
        }

    return -ENOSPC;
}

int get_free_pirqs(struct domain *d, unsigned int nr)
{
    unsigned int i, found = 0;

    ASSERT(rw_is_write_locked(&d->event_lock));

    for ( i = d->nr_pirqs - 1; i >= nr_irqs_gsi; --i )
        if ( is_free_pirq(d, pirq_info(d, i)) )
        {
            pirq_get_info(d, i);
            if ( ++found == nr )
                return i;
        }
        else
            found = 0;

    return -ENOSPC;
}

#define MAX_MSI_IRQS 32 /* limited by MSI capability struct properties */

int map_domain_pirq(
    struct domain *d, int pirq, int irq, int type, void *data)
{
    int ret = 0;
    int old_irq, old_pirq;
    struct pirq *info;
    struct irq_desc *desc;
    unsigned long flags;
    DECLARE_BITMAP(prepared, MAX_MSI_IRQS) = {};
    DECLARE_BITMAP(granted, MAX_MSI_IRQS) = {};

    ASSERT(rw_is_write_locked(&d->event_lock));

    if ( !irq_access_permitted(current->domain, irq))
        return -EPERM;

    if ( pirq < 0 || pirq >= d->nr_pirqs || irq <= 0 || irq >= nr_irqs )
    {
        dprintk(XENLOG_G_ERR, "dom%d: invalid pirq %d or irq %d\n",
                d->domain_id, pirq, irq);
        return -EINVAL;
    }

    old_irq = domain_pirq_to_irq(d, pirq);
    old_pirq = domain_irq_to_pirq(d, irq);

    if ( (old_irq > 0 && (old_irq != irq) ) ||
         (old_pirq && (old_pirq != pirq)) )
    {
        dprintk(XENLOG_G_WARNING,
                "dom%d: pirq %d or irq %d already mapped (%d,%d)\n",
                d->domain_id, pirq, irq, old_pirq, old_irq);
        return 0;
    }

    ret = xsm_map_domain_irq(XSM_HOOK, d, irq, data);
    if ( ret )
    {
        dprintk(XENLOG_G_ERR, "dom%d: could not permit access to irq %d mapping to pirq %d\n",
                d->domain_id, irq, pirq);
        return ret;
    }

    if ( likely(!irq_access_permitted(d, irq)) )
    {
        ret = irq_permit_access(d, irq);
        if ( ret )
        {
            printk(XENLOG_G_ERR
                   "dom%d: could not permit access to IRQ%d (pirq %d)\n",
                  d->domain_id, irq, pirq);
            return ret;
        }
        __set_bit(0, granted);
    }

    ret = prepare_domain_irq_pirq(d, irq, pirq, &info);
    if ( ret < 0 )
        goto revoke;
    if ( !ret )
        __set_bit(0, prepared);

    desc = irq_to_desc(irq);

    if ( type == MAP_PIRQ_TYPE_MSI || type == MAP_PIRQ_TYPE_MULTI_MSI )
    {
        struct msi_info *msi = (struct msi_info *)data;
        struct msi_desc *msi_desc;
        struct pci_dev *pdev;
        unsigned int nr = 0;

        ASSERT(pcidevs_read_locked());

        ret = -ENODEV;
        if ( !cpu_has_apic )
            goto done;

        pdev = pci_get_pdev(d, msi->sbdf);
        if ( !pdev )
            goto done;

        ret = pci_enable_msi(msi, &msi_desc);
        if ( ret )
        {
            if ( ret > 0 )
            {
                msi->entry_nr = ret;
                ret = -ENFILE;
            }
            goto done;
        }

        spin_lock_irqsave(&desc->lock, flags);

        if ( desc->handler != &no_irq_type )
        {
            spin_unlock_irqrestore(&desc->lock, flags);
            dprintk(XENLOG_G_ERR, "dom%d: irq %d in use\n",
                    d->domain_id, irq);
            pci_disable_msi(msi_desc);
            msi_desc->irq = -1;
            msi_free_irq(msi_desc);
            ret = -EBUSY;
            goto done;
        }

        while ( !(ret = setup_msi_irq(desc, msi_desc + nr)) )
        {
            if ( opt_irq_vector_map == OPT_IRQ_VECTOR_MAP_PERDEV &&
                 !desc->arch.used_vectors )
            {
                desc->arch.used_vectors = &pdev->arch.used_vectors;
                if ( desc->arch.vector != IRQ_VECTOR_UNASSIGNED )
                {
                    int vector = desc->arch.vector;

                    ASSERT(!test_bit(vector, desc->arch.used_vectors));
                    set_bit(vector, desc->arch.used_vectors);
                }
            }
            if ( type == MAP_PIRQ_TYPE_MSI ||
                 msi_desc->msi_attrib.type != PCI_CAP_ID_MSI ||
                 ++nr == msi->entry_nr )
                break;

            set_domain_irq_pirq(d, irq, info);
            spin_unlock_irqrestore(&desc->lock, flags);

            info = NULL;
            irq = create_irq(NUMA_NO_NODE, true);
            ret = irq >= 0 ? prepare_domain_irq_pirq(d, irq, pirq + nr, &info)
                           : irq;
            if ( ret < 0 )
                break;
            if ( !ret )
                __set_bit(nr, prepared);
            msi_desc[nr].irq = irq;

            if ( likely(!irq_access_permitted(d, irq)) )
            {
                if ( irq_permit_access(d, irq) )
                    printk(XENLOG_G_WARNING
                           "dom%d: could not permit access to IRQ%d (pirq %d)\n",
                           d->domain_id, irq, pirq);
                else
                    __set_bit(nr, granted);
            }

            desc = irq_to_desc(irq);
            spin_lock_irqsave(&desc->lock, flags);

            if ( desc->handler != &no_irq_type )
            {
                dprintk(XENLOG_G_ERR, "dom%d: irq %d (pirq %u) in use (%s)\n",
                        d->domain_id, irq, pirq + nr, desc->handler->typename);
                ret = -EBUSY;
                break;
            }
        }

        if ( ret )
        {
            spin_unlock_irqrestore(&desc->lock, flags);
            pci_disable_msi(msi_desc);
            if ( nr )
            {
                ASSERT(msi_desc->irq >= 0);
                desc = irq_to_desc(msi_desc->irq);
                spin_lock_irqsave(&desc->lock, flags);
                desc->handler = &no_irq_type;
                desc->msi_desc = NULL;
                spin_unlock_irqrestore(&desc->lock, flags);
            }
            while ( nr )
            {
                if ( irq >= 0 && test_bit(nr, granted) &&
                     irq_deny_access(d, irq) )
                    printk(XENLOG_G_ERR
                           "dom%d: could not revoke access to IRQ%d (pirq %d)\n",
                           d->domain_id, irq, pirq);
                if ( info && test_bit(nr, prepared) )
                    cleanup_domain_irq_pirq(d, irq, info);
                info = pirq_info(d, pirq + --nr);
                irq = info->arch.irq;
            }
            msi_desc->irq = -1;
            msi_free_irq(msi_desc);
            goto done;
        }

        set_domain_irq_pirq(d, irq, info);
        spin_unlock_irqrestore(&desc->lock, flags);
    }
    else
    {
        spin_lock_irqsave(&desc->lock, flags);
        set_domain_irq_pirq(d, irq, info);
        spin_unlock_irqrestore(&desc->lock, flags);
        ret = 0;
    }

done:
    if ( ret )
    {
        if ( test_bit(0, prepared) )
            cleanup_domain_irq_pirq(d, irq, info);
 revoke:
        if ( test_bit(0, granted) && irq_deny_access(d, irq) )
            printk(XENLOG_G_ERR
                   "dom%d: could not revoke access to IRQ%d (pirq %d)\n",
                   d->domain_id, irq, pirq);
    }
    return ret;
}

/* The pirq should have been unbound before this call. */
int unmap_domain_pirq(struct domain *d, int pirq)
{
    struct irq_desc *desc;
    int irq, ret = 0, rc;
    unsigned int i, nr = 1;
    bool forced_unbind;
    struct pirq *info;
    struct msi_desc *msi_desc = NULL;

    if ( (pirq < 0) || (pirq >= d->nr_pirqs) )
        return -EINVAL;

    ASSERT(pcidevs_read_locked());
    ASSERT(rw_is_write_locked(&d->event_lock));

    info = pirq_info(d, pirq);
    if ( !info || (irq = info->arch.irq) <= 0 )
    {
        dprintk(XENLOG_G_ERR, "dom%d: pirq %d not mapped\n",
                d->domain_id, pirq);
        ret = -EINVAL;
        goto done;
    }

    desc = irq_to_desc(irq);
    msi_desc = desc->msi_desc;
    if ( msi_desc && msi_desc->msi_attrib.type == PCI_CAP_ID_MSI )
    {
        if ( msi_desc->msi_attrib.entry_nr )
        {
            printk(XENLOG_G_ERR
                   "dom%d: trying to unmap secondary MSI pirq %d\n",
                   d->domain_id, pirq);
            ret = -EBUSY;
            goto done;
        }
        nr = msi_desc->msi.nvec;
    }

    /*
     * When called by complete_domain_destroy via RCU, current is a random
     * domain.  Skip the XSM check since this is a Xen-initiated action.
     */
    if ( !d->is_dying )
        ret = xsm_unmap_domain_irq(XSM_HOOK, d, irq,
                                   msi_desc ? msi_desc->dev : NULL);

    if ( ret )
        goto done;

    forced_unbind = pirq_guest_force_unbind(d, info);
    if ( forced_unbind )
        dprintk(XENLOG_G_WARNING, "dom%d: forcing unbind of pirq %d\n",
                d->domain_id, pirq);

    if ( msi_desc != NULL )
        pci_disable_msi(msi_desc);

    for ( i = 0; i < nr; i++, info = pirq_info(d, pirq + i) )
    {
        unsigned long flags;

        if ( !info || info->arch.irq <= 0 )
        {
            printk(XENLOG_G_ERR "%pd: MSI pirq %d not mapped\n",
                   d, pirq + i);
            continue;
        }
        irq = info->arch.irq;
        desc = irq_to_desc(irq);

        spin_lock_irqsave(&desc->lock, flags);

        BUG_ON(irq != domain_pirq_to_irq(d, pirq + i));
        BUG_ON(desc->msi_desc != msi_desc + i);

        if ( !forced_unbind )
            clear_domain_irq_pirq(d, irq, info);
        else
        {
            info->arch.irq = -irq;
            radix_tree_replace_slot(
                radix_tree_lookup_slot(&d->arch.irq_pirq, irq),
                radix_tree_int_to_ptr(-pirq));
        }

        if ( msi_desc )
        {
            desc->handler = &no_irq_type;
            desc->msi_desc = NULL;
        }

        spin_unlock_irqrestore(&desc->lock, flags);

        if ( !forced_unbind )
            cleanup_domain_irq_pirq(d, irq, info);

        rc = irq_deny_access(d, irq);
        if ( rc )
        {
            printk(XENLOG_G_ERR
                   "dom%d: could not deny access to IRQ%d (pirq %d)\n",
                   d->domain_id, irq, pirq + i);
            ret = rc;
        }
    }

    if (msi_desc)
        msi_free_irq(msi_desc);

 done:
    return ret;
}

void free_domain_pirqs(struct domain *d)
{
    int i;

    pcidevs_lock();
    write_lock(&d->event_lock);

    for ( i = 0; i < d->nr_pirqs; i++ )
        if ( domain_pirq_to_irq(d, i) > 0 )
            unmap_domain_pirq(d, i);

    write_unlock(&d->event_lock);
    pcidevs_unlock();
}

static void cf_check dump_irqs(unsigned char key)
{
    int i, irq, pirq;
    struct irq_desc *desc;
    struct domain *d;
    const struct pirq *info;
    unsigned long flags;
    char *ssid;

    printk("IRQ information:\n");

    for ( irq = 0; irq < nr_irqs; irq++ )
    {
        const irq_guest_action_t *action;

        if ( !(irq & 0x1f) )
            process_pending_softirqs();

        desc = irq_to_desc(irq);

        if ( !irq_desc_initialized(desc) || desc->handler == &no_irq_type )
            continue;

        ssid = in_irq() ? NULL : xsm_show_irq_sid(irq);

        spin_lock_irqsave(&desc->lock, flags);

        printk("   IRQ:%4d vec:%02x %-15s status=%03x aff:{%*pbl}/{%*pbl} ",
               irq, desc->arch.vector, desc->handler->typename, desc->status,
               CPUMASK_PR(desc->affinity), CPUMASK_PR(desc->arch.cpu_mask));

        if ( ssid )
            printk("Z=%-25s ", ssid);

        action = guest_action(desc);
        if ( action )
        {
            printk("in-flight=%d%c",
                   action->in_flight, action->nr_guests ? ' ' : '\n');

            for ( i = 0; i < action->nr_guests; )
            {
                struct evtchn *evtchn;
                unsigned int pending = 2, masked = 2;

                d = action->guest[i++];
                pirq = domain_irq_to_pirq(d, irq);
                info = pirq_info(d, pirq);
                evtchn = evtchn_from_port(d, info->evtchn);
                if ( evtchn_read_trylock(evtchn) )
                {
                    pending = evtchn_is_pending(d, evtchn);
                    masked = evtchn_is_masked(d, evtchn);
                    evtchn_read_unlock(evtchn);
                }
                printk("d%d:%3d(%c%c%c)%c",
                       d->domain_id, pirq, "-P?"[pending],
                       "-M?"[masked], info->masked ? 'M' : '-',
                       i < action->nr_guests ? ',' : '\n');
            }
        }
        else if ( desc->action )
            printk("%ps()\n", desc->action->handler);
        else
            printk("mapped, unbound\n");

        spin_unlock_irqrestore(&desc->lock, flags);

        xfree(ssid);
    }

    process_pending_softirqs();
    printk("Direct vector information:\n");
    for ( i = FIRST_DYNAMIC_VECTOR; i < X86_NR_VECTORS; ++i )
        if ( direct_apic_vector[i] )
            printk("   %#02x -> %ps()\n", i, direct_apic_vector[i]);

    dump_ioapic_irq_info();
}

static int __init cf_check setup_dump_irqs(void)
{
    /* In lieu of being able to live in init_irq_data(). */
    BUILD_BUG_ON(sizeof(irq_max_guests) >
                 sizeof_field(irq_guest_action_t, nr_guests));

    register_keyhandler('i', dump_irqs, "dump interrupt bindings", 1);
    return 0;
}
__initcall(setup_dump_irqs);

/* Reset irq affinities to match the given CPU mask. */
void fixup_irqs(const cpumask_t *mask, bool verbose)
{
    unsigned int irq;
    static int warned;
    struct irq_desc *desc;

    for ( irq = 0; irq < nr_irqs; irq++ )
    {
        bool break_affinity = false, set_affinity = true;
        unsigned int vector;
        cpumask_t *affinity = this_cpu(scratch_cpumask);

        if ( irq == 2 )
            continue;

        desc = irq_to_desc(irq);
        if ( !irq_desc_initialized(desc) )
            continue;

        spin_lock(&desc->lock);

        vector = irq_to_vector(irq);
        if ( vector >= FIRST_HIPRIORITY_VECTOR &&
             vector <= LAST_HIPRIORITY_VECTOR )
        {
            cpumask_and(desc->arch.cpu_mask, desc->arch.cpu_mask, mask);

            /*
             * This can in particular happen when parking secondary threads
             * during boot and when the serial console wants to use a PCI IRQ.
             */
            if ( desc->handler == &no_irq_type )
            {
                spin_unlock(&desc->lock);
                continue;
            }
        }

        if ( desc->arch.move_cleanup_count )
        {
            /* The cleanup IPI may have got sent while we were still online. */
            cpumask_andnot(affinity, desc->arch.old_cpu_mask,
                           &cpu_online_map);
            desc->arch.move_cleanup_count -= cpumask_weight(affinity);
            if ( !desc->arch.move_cleanup_count )
                release_old_vec(desc);
        }

        if ( !desc->action || cpumask_subset(desc->affinity, mask) )
        {
            spin_unlock(&desc->lock);
            continue;
        }

        /*
         * In order for the affinity adjustment below to be successful, we
         * need _assign_irq_vector() to succeed. This in particular means
         * clearing desc->arch.move_in_progress if this would otherwise
         * prevent the function from succeeding. Since there's no way for the
         * flag to get cleared anymore when there's no possible destination
         * left (the only possibility then would be the IRQs enabled window
         * after this loop), there's then also no race with us doing it here.
         *
         * Therefore the logic here and there need to remain in sync.
         */
        if ( desc->arch.move_in_progress &&
             !cpumask_intersects(mask, desc->arch.cpu_mask) )
        {
            unsigned int cpu;

            cpumask_and(affinity, desc->arch.old_cpu_mask, &cpu_online_map);

            spin_lock(&vector_lock);
            for_each_cpu(cpu, affinity)
                per_cpu(vector_irq, cpu)[desc->arch.old_vector] = ~irq;
            spin_unlock(&vector_lock);

            release_old_vec(desc);
            desc->arch.move_in_progress = 0;
        }

        if ( !cpumask_intersects(mask, desc->affinity) )
        {
            break_affinity = true;
            cpumask_setall(affinity);
        }
        else
            cpumask_copy(affinity, desc->affinity);

        if ( desc->handler->disable )
            desc->handler->disable(desc);

        if ( desc->handler->set_affinity )
            desc->handler->set_affinity(desc, affinity);
        else if ( !(warned++) )
            set_affinity = false;

        if ( desc->handler->enable )
            desc->handler->enable(desc);

        cpumask_copy(affinity, desc->affinity);

        spin_unlock(&desc->lock);

        if ( !verbose )
            continue;

        if ( !set_affinity )
            printk("Cannot set affinity for IRQ%u\n", irq);
        else if ( break_affinity )
            printk("Broke affinity for IRQ%u, new: %*pb\n",
                   irq, CPUMASK_PR(affinity));
    }

    /* That doesn't seem sufficient.  Give it 1ms. */
    local_irq_enable();
    mdelay(1);
    local_irq_disable();
}

void fixup_eoi(void)
{
    unsigned int irq, sp;
    struct pending_eoi *peoi;

    /* Clean up cpu_eoi_map of every interrupt to exclude this CPU. */
    for ( irq = 0; irq < nr_irqs; irq++ )
    {
        irq_guest_action_t *action = guest_action(irq_to_desc(irq));

        if ( !action )
            continue;
        cpumask_clear_cpu(smp_processor_id(), action->cpu_eoi_map);
    }

    /* Flush the interrupt EOI stack. */
    peoi = this_cpu(pending_eoi);
    for ( sp = 0; sp < pending_eoi_sp(peoi); sp++ )
        peoi[sp].ready = 1;
    flush_ready_eoi();
}

int map_domain_emuirq_pirq(struct domain *d, int pirq, int emuirq)
{
    int old_emuirq = IRQ_UNBOUND, old_pirq = IRQ_UNBOUND;
    struct pirq *info;

    ASSERT(rw_is_write_locked(&d->event_lock));

    if ( !is_hvm_domain(d) )
        return -EINVAL;

    if ( pirq < 0 || pirq >= d->nr_pirqs ||
            emuirq == IRQ_UNBOUND || emuirq >= (int) nr_irqs )
    {
        dprintk(XENLOG_G_ERR, "dom%d: invalid pirq %d or emuirq %d\n",
                d->domain_id, pirq, emuirq);
        return -EINVAL;
    }

    old_emuirq = domain_pirq_to_emuirq(d, pirq);
    if ( emuirq != IRQ_PT )
        old_pirq = domain_emuirq_to_pirq(d, emuirq);

    if ( (old_emuirq != IRQ_UNBOUND && (old_emuirq != emuirq) ) ||
         (old_pirq != IRQ_UNBOUND && (old_pirq != pirq)) )
    {
        dprintk(XENLOG_G_WARNING, "dom%d: pirq %d or emuirq %d already mapped\n",
                d->domain_id, pirq, emuirq);
        return 0;
    }

    info = pirq_get_info(d, pirq);
    if ( !info )
        return -ENOMEM;

    /* do not store emuirq mappings for pt devices */
    if ( emuirq != IRQ_PT )
    {
        int err = radix_tree_insert(&d->arch.hvm.emuirq_pirq, emuirq,
                                    radix_tree_int_to_ptr(pirq));

        switch ( err )
        {
        case 0:
            break;
        case -EEXIST:
            radix_tree_replace_slot(
                radix_tree_lookup_slot(
                    &d->arch.hvm.emuirq_pirq, emuirq),
                radix_tree_int_to_ptr(pirq));
            break;
        default:
            pirq_cleanup_check(info, d);
            return err;
        }
    }
    info->arch.hvm.emuirq = emuirq;

    return 0;
}

int unmap_domain_pirq_emuirq(struct domain *d, int pirq)
{
    int emuirq, ret = 0;
    struct pirq *info;

    if ( !is_hvm_domain(d) )
        return -EINVAL;

    if ( (pirq < 0) || (pirq >= d->nr_pirqs) )
        return -EINVAL;

    ASSERT(rw_is_write_locked(&d->event_lock));

    emuirq = domain_pirq_to_emuirq(d, pirq);
    if ( emuirq == IRQ_UNBOUND )
    {
        dprintk(XENLOG_G_ERR, "dom%d: pirq %d not mapped\n",
                d->domain_id, pirq);
        ret = -EINVAL;
        goto done;
    }

    info = pirq_info(d, pirq);
    if ( info )
    {
        info->arch.hvm.emuirq = IRQ_UNBOUND;
        pirq_cleanup_check(info, d);
    }
    if ( emuirq != IRQ_PT )
        radix_tree_delete(&d->arch.hvm.emuirq_pirq, emuirq);

 done:
    return ret;
}

void arch_evtchn_bind_pirq(struct domain *d, int pirq)
{
    int irq = domain_pirq_to_irq(d, pirq);
    struct irq_desc *desc;
    unsigned long flags;

    if ( irq <= 0 )
        return;

    if ( is_hvm_domain(d) )
        map_domain_emuirq_pirq(d, pirq, IRQ_PT);

    desc = irq_to_desc(irq);
    spin_lock_irqsave(&desc->lock, flags);
    if ( desc->msi_desc )
        guest_mask_msi_irq(desc, 0);
    spin_unlock_irqrestore(&desc->lock, flags);
}

static int allocate_pirq(struct domain *d, int index, int pirq, int irq,
                         int type, int *nr)
{
    int current_pirq;

    ASSERT(rw_is_write_locked(&d->event_lock));
    current_pirq = domain_irq_to_pirq(d, irq);
    if ( pirq < 0 )
    {
        if ( current_pirq )
        {
            dprintk(XENLOG_G_ERR, "dom%d: %d:%d already mapped to %d\n",
                    d->domain_id, index, pirq, current_pirq);
            if ( current_pirq < 0 )
                return -EBUSY;
        }
        else if ( type == MAP_PIRQ_TYPE_MULTI_MSI )
        {
            if ( *nr <= 0 || *nr > MAX_MSI_IRQS )
                return -EDOM;
            if ( *nr != 1 && !iommu_intremap )
                return -EOPNOTSUPP;

            while ( *nr & (*nr - 1) )
                *nr += *nr & -*nr;
            pirq = get_free_pirqs(d, *nr);
            if ( pirq < 0 )
            {
                while ( (*nr >>= 1) > 1 )
                    if ( get_free_pirqs(d, *nr) > 0 )
                        break;
                dprintk(XENLOG_G_ERR, "dom%d: no block of %d free pirqs\n",
                        d->domain_id, *nr << 1);
            }
        }
        else
        {
            pirq = get_free_pirq(d, type);
            if ( pirq < 0 )
                dprintk(XENLOG_G_ERR, "dom%d: no free pirq\n", d->domain_id);
        }
    }
    else if ( current_pirq && pirq != current_pirq )
    {
        dprintk(XENLOG_G_ERR, "dom%d: irq %d already mapped to pirq %d\n",
                d->domain_id, irq, current_pirq);
        return -EEXIST;
    }

    return pirq;
}

int allocate_and_map_gsi_pirq(struct domain *d, int index, int *pirq_p)
{
    int irq, pirq, ret;

    if ( index < 0 || index >= nr_irqs_gsi )
    {
        dprintk(XENLOG_G_ERR, "dom%d: map invalid irq %d\n", d->domain_id,
                index);
        return -EINVAL;
    }

    irq = domain_pirq_to_irq(current->domain, index);
    if ( irq <= 0 )
    {
        if ( is_hardware_domain(current->domain) )
            irq = index;
        else
        {
            dprintk(XENLOG_G_ERR, "dom%d: map pirq with incorrect irq!\n",
                    d->domain_id);
            return -EINVAL;
        }
    }

    /* Verify or get pirq. */
    write_lock(&d->event_lock);
    pirq = allocate_pirq(d, index, *pirq_p, irq, MAP_PIRQ_TYPE_GSI, NULL);
    if ( pirq < 0 )
    {
        ret = pirq;
        goto done;
    }

    ret = map_domain_pirq(d, pirq, irq, MAP_PIRQ_TYPE_GSI, NULL);
    if ( !ret )
        *pirq_p = pirq;

 done:
    write_unlock(&d->event_lock);

    return ret;
}

int allocate_and_map_msi_pirq(struct domain *d, int index, int *pirq_p,
                              int type, struct msi_info *msi)
{
    int irq, pirq, ret;

    switch ( type )
    {
    case MAP_PIRQ_TYPE_MSI:
        if ( !msi->table_base )
            msi->entry_nr = 1;
        irq = index;
        if ( irq == -1 )
        {
    case MAP_PIRQ_TYPE_MULTI_MSI:
            irq = create_irq(NUMA_NO_NODE, true);
        }

        if ( irq < nr_irqs_gsi || irq >= nr_irqs )
        {
            dprintk(XENLOG_G_ERR, "dom%d: can't create irq for msi!\n",
                    d->domain_id);
            return -EINVAL;
        }
        break;

    default:
        dprintk(XENLOG_G_ERR, "dom%d: wrong pirq type %x\n",
                d->domain_id, type);
        ASSERT_UNREACHABLE();
        return -EINVAL;
    }

    msi->irq = irq;

    pcidevs_read_lock();
    /* Verify or get pirq. */
    write_lock(&d->event_lock);
    pirq = allocate_pirq(d, index, *pirq_p, irq, type, &msi->entry_nr);
    if ( pirq < 0 )
    {
        ret = pirq;
        goto done;
    }

    ret = map_domain_pirq(d, pirq, irq, type, msi);
    if ( !ret )
        *pirq_p = pirq;

 done:
    write_unlock(&d->event_lock);
    pcidevs_read_unlock();
    if ( ret )
    {
        switch ( type )
        {
        case MAP_PIRQ_TYPE_MSI:
            if ( index == -1 )
        case MAP_PIRQ_TYPE_MULTI_MSI:
                destroy_irq(irq);
            break;
        }
    }

    return ret;
}
