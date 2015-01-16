/*
 * xen/arch/arm/gic-v3.c
 *
 * ARM Generic Interrupt Controller support v3 version
 * based on xen/arch/arm/gic-v2.c and kernel GICv3 driver
 *
 * Copyright (C) 2012,2013 - ARM Ltd
 * Marc Zyngier <marc.zyngier@arm.com>
 *
 * Vijaya Kumar K <vijaya.kumar@caviumnetworks.com>, Cavium Inc
 * ported to Xen
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
#include <xen/init.h>
#include <xen/cpu.h>
#include <xen/mm.h>
#include <xen/irq.h>
#include <xen/sched.h>
#include <xen/errno.h>
#include <xen/delay.h>
#include <xen/device_tree.h>
#include <xen/sizes.h>
#include <xen/libfdt/libfdt.h>
#include <asm/p2m.h>
#include <asm/domain.h>
#include <asm/io.h>
#include <asm/device.h>
#include <asm/gic.h>
#include <asm/gic_v3_defs.h>
#include <asm/cpufeature.h>

struct rdist_region {
    paddr_t base;
    paddr_t size;
    void __iomem *map_base;
};

/* Global state */
static struct {
    paddr_t dbase;            /* Address of distributor registers */
    paddr_t dbase_size;
    void __iomem *map_dbase;  /* Mapped address of distributor registers */
    struct rdist_region *rdist_regions;
    uint32_t  rdist_stride;
    unsigned int rdist_count; /* Number of rdist regions count */
    unsigned int nr_priorities;
    spinlock_t lock;
} gicv3;

static struct gic_info gicv3_info;

/* per-cpu re-distributor base */
static DEFINE_PER_CPU(void __iomem*, rbase);

#define GICD                   (gicv3.map_dbase)
#define GICD_RDIST_BASE        (this_cpu(rbase))
#define GICD_RDIST_SGI_BASE    (GICD_RDIST_BASE + SZ_64K)

/*
 * Saves all 16(Max) LR registers. Though number of LRs implemented
 * is implementation specific.
 */
static inline void gicv3_save_lrs(struct vcpu *v)
{
    /* Fall through for all the cases */
    switch ( gicv3_info.nr_lrs )
    {
    case 16:
        v->arch.gic.v3.lr[15] = READ_SYSREG(ICH_LR15_EL2);
    case 15:
        v->arch.gic.v3.lr[14] = READ_SYSREG(ICH_LR14_EL2);
    case 14:
        v->arch.gic.v3.lr[13] = READ_SYSREG(ICH_LR13_EL2);
    case 13:
        v->arch.gic.v3.lr[12] = READ_SYSREG(ICH_LR12_EL2);
    case 12:
        v->arch.gic.v3.lr[11] = READ_SYSREG(ICH_LR11_EL2);
    case 11:
        v->arch.gic.v3.lr[10] = READ_SYSREG(ICH_LR10_EL2);
    case 10:
        v->arch.gic.v3.lr[9] = READ_SYSREG(ICH_LR9_EL2);
    case 9:
        v->arch.gic.v3.lr[8] = READ_SYSREG(ICH_LR8_EL2);
    case 8:
        v->arch.gic.v3.lr[7] = READ_SYSREG(ICH_LR7_EL2);
    case 7:
        v->arch.gic.v3.lr[6] = READ_SYSREG(ICH_LR6_EL2);
    case 6:
        v->arch.gic.v3.lr[5] = READ_SYSREG(ICH_LR5_EL2);
    case 5:
        v->arch.gic.v3.lr[4] = READ_SYSREG(ICH_LR4_EL2);
    case 4:
        v->arch.gic.v3.lr[3] = READ_SYSREG(ICH_LR3_EL2);
    case 3:
        v->arch.gic.v3.lr[2] = READ_SYSREG(ICH_LR2_EL2);
    case 2:
        v->arch.gic.v3.lr[1] = READ_SYSREG(ICH_LR1_EL2);
    case 1:
         v->arch.gic.v3.lr[0] = READ_SYSREG(ICH_LR0_EL2);
         break;
    default:
         BUG();
    }
}

/*
 * Restores all 16(Max) LR registers. Though number of LRs implemented
 * is implementation specific.
 */
static inline void gicv3_restore_lrs(const struct vcpu *v)
{
    /* Fall through for all the cases */
    switch ( gicv3_info.nr_lrs )
    {
    case 16:
        WRITE_SYSREG(v->arch.gic.v3.lr[15], ICH_LR15_EL2);
    case 15:
        WRITE_SYSREG(v->arch.gic.v3.lr[14], ICH_LR14_EL2);
    case 14:
        WRITE_SYSREG(v->arch.gic.v3.lr[13], ICH_LR13_EL2);
    case 13:
        WRITE_SYSREG(v->arch.gic.v3.lr[12], ICH_LR12_EL2);
    case 12:
        WRITE_SYSREG(v->arch.gic.v3.lr[11], ICH_LR11_EL2);
    case 11:
        WRITE_SYSREG(v->arch.gic.v3.lr[10], ICH_LR10_EL2);
    case 10:
        WRITE_SYSREG(v->arch.gic.v3.lr[9], ICH_LR9_EL2);
    case 9:
        WRITE_SYSREG(v->arch.gic.v3.lr[8], ICH_LR8_EL2);
    case 8:
        WRITE_SYSREG(v->arch.gic.v3.lr[7], ICH_LR7_EL2);
    case 7:
        WRITE_SYSREG(v->arch.gic.v3.lr[6], ICH_LR6_EL2);
    case 6:
        WRITE_SYSREG(v->arch.gic.v3.lr[5], ICH_LR5_EL2);
    case 5:
        WRITE_SYSREG(v->arch.gic.v3.lr[4], ICH_LR4_EL2);
    case 4:
        WRITE_SYSREG(v->arch.gic.v3.lr[3], ICH_LR3_EL2);
    case 3:
        WRITE_SYSREG(v->arch.gic.v3.lr[2], ICH_LR2_EL2);
    case 2:
        WRITE_SYSREG(v->arch.gic.v3.lr[1], ICH_LR1_EL2);
    case 1:
        WRITE_SYSREG(v->arch.gic.v3.lr[0], ICH_LR0_EL2);
        break;
    default:
         BUG();
    }
}

static uint64_t gicv3_ich_read_lr(int lr)
{
    switch ( lr )
    {
    case 0: return READ_SYSREG(ICH_LR0_EL2);
    case 1: return READ_SYSREG(ICH_LR1_EL2);
    case 2: return READ_SYSREG(ICH_LR2_EL2);
    case 3: return READ_SYSREG(ICH_LR3_EL2);
    case 4: return READ_SYSREG(ICH_LR4_EL2);
    case 5: return READ_SYSREG(ICH_LR5_EL2);
    case 6: return READ_SYSREG(ICH_LR6_EL2);
    case 7: return READ_SYSREG(ICH_LR7_EL2);
    case 8: return READ_SYSREG(ICH_LR8_EL2);
    case 9: return READ_SYSREG(ICH_LR9_EL2);
    case 10: return READ_SYSREG(ICH_LR10_EL2);
    case 11: return READ_SYSREG(ICH_LR11_EL2);
    case 12: return READ_SYSREG(ICH_LR12_EL2);
    case 13: return READ_SYSREG(ICH_LR13_EL2);
    case 14: return READ_SYSREG(ICH_LR14_EL2);
    case 15: return READ_SYSREG(ICH_LR15_EL2);
    default:
        BUG();
    }
}

static void gicv3_ich_write_lr(int lr, uint64_t val)
{
    switch ( lr )
    {
    case 0:
        WRITE_SYSREG(val, ICH_LR0_EL2);
        break;
    case 1:
        WRITE_SYSREG(val, ICH_LR1_EL2);
        break;
    case 2:
        WRITE_SYSREG(val, ICH_LR2_EL2);
        break;
    case 3:
        WRITE_SYSREG(val, ICH_LR3_EL2);
        break;
    case 4:
        WRITE_SYSREG(val, ICH_LR4_EL2);
        break;
    case 5:
        WRITE_SYSREG(val, ICH_LR5_EL2);
        break;
    case 6:
        WRITE_SYSREG(val, ICH_LR6_EL2);
        break;
    case 7:
        WRITE_SYSREG(val, ICH_LR7_EL2);
        break;
    case 8:
        WRITE_SYSREG(val, ICH_LR8_EL2);
        break;
    case 9:
        WRITE_SYSREG(val, ICH_LR9_EL2);
        break;
    case 10:
        WRITE_SYSREG(val, ICH_LR10_EL2);
        break;
    case 11:
        WRITE_SYSREG(val, ICH_LR11_EL2);
        break;
    case 12:
        WRITE_SYSREG(val, ICH_LR12_EL2);
        break;
    case 13:
        WRITE_SYSREG(val, ICH_LR13_EL2);
        break;
    case 14:
        WRITE_SYSREG(val, ICH_LR14_EL2);
        break;
    case 15:
        WRITE_SYSREG(val, ICH_LR15_EL2);
        break;
    default:
        return;
    }
    isb();
}

/*
 * System Register Enable (SRE). Enable to access CPU & Virtual
 * interface registers as system registers in EL2
 */
static void gicv3_enable_sre(void)
{
    uint32_t val;

    val = READ_SYSREG32(ICC_SRE_EL2);
    val |= GICC_SRE_EL2_SRE | GICC_SRE_EL2_ENEL1;

    WRITE_SYSREG32(val, ICC_SRE_EL2);
    isb();
}

/* Wait for completion of a distributor change */
static void gicv3_do_wait_for_rwp(void __iomem *base)
{
    uint32_t val;
    bool_t timeout = 0;
    s_time_t deadline = NOW() + MILLISECS(1000);

    do {
        val = readl_relaxed(base + GICD_CTLR);
        if ( !(val & GICD_CTLR_RWP) )
            break;
        if ( NOW() > deadline )
        {
            timeout = 1;
            break;
        }
        cpu_relax();
        udelay(1);
    } while ( 1 );

    if ( timeout )
        dprintk(XENLOG_ERR, "RWP timeout\n");
}

static void gicv3_dist_wait_for_rwp(void)
{
    gicv3_do_wait_for_rwp(GICD);
}

static void gicv3_redist_wait_for_rwp(void)
{
    gicv3_do_wait_for_rwp(GICD_RDIST_BASE);
}

static void gicv3_wait_for_rwp(int irq)
{
    if ( irq < NR_LOCAL_IRQS )
         gicv3_redist_wait_for_rwp();
    else
         gicv3_dist_wait_for_rwp();
}

static unsigned int gicv3_get_cpu_from_mask(const cpumask_t *cpumask)
{
    unsigned int cpu;
    cpumask_t possible_mask;

    cpumask_and(&possible_mask, cpumask, &cpu_possible_map);
    cpu = cpumask_any(&possible_mask);

    return cpu;
}

static void restore_aprn_regs(const union gic_state_data *d)
{
    /* Write APRn register based on number of priorities
       platform has implemented */
    switch ( gicv3.nr_priorities )
    {
    case 7:
        WRITE_SYSREG32(d->v3.apr0[2], ICH_AP0R2_EL2);
        WRITE_SYSREG32(d->v3.apr1[2], ICH_AP1R2_EL2);
        /* Fall through */
    case 6:
        WRITE_SYSREG32(d->v3.apr0[1], ICH_AP0R1_EL2);
        WRITE_SYSREG32(d->v3.apr1[1], ICH_AP1R1_EL2);
        /* Fall through */
    case 5:
        WRITE_SYSREG32(d->v3.apr0[0], ICH_AP0R0_EL2);
        WRITE_SYSREG32(d->v3.apr1[0], ICH_AP1R0_EL2);
        break;
    default:
        BUG();
    }
}

static void save_aprn_regs(union gic_state_data *d)
{
    /* Read APRn register based on number of priorities
       platform has implemented */
    switch ( gicv3.nr_priorities )
    {
    case 7:
        d->v3.apr0[2] = READ_SYSREG32(ICH_AP0R2_EL2);
        d->v3.apr1[2] = READ_SYSREG32(ICH_AP1R2_EL2);
        /* Fall through */
    case 6:
        d->v3.apr0[1] = READ_SYSREG32(ICH_AP0R1_EL2);
        d->v3.apr1[1] = READ_SYSREG32(ICH_AP1R1_EL2);
        /* Fall through */
    case 5:
        d->v3.apr0[0] = READ_SYSREG32(ICH_AP0R0_EL2);
        d->v3.apr1[0] = READ_SYSREG32(ICH_AP1R0_EL2);
        break;
    default:
        BUG();
    }
}

/*
 * As per section 4.8.17 of the GICv3 spec following
 * registers are save and restored on guest swap
 */
static void gicv3_save_state(struct vcpu *v)
{

    /* No need for spinlocks here because interrupts are disabled around
     * this call and it only accesses struct vcpu fields that cannot be
     * accessed simultaneously by another pCPU.
     *
     * Make sure all stores to the GIC via the memory mapped interface
     * are now visible to the system register interface
     */
    dsb(sy);
    gicv3_save_lrs(v);
    save_aprn_regs(&v->arch.gic);
    v->arch.gic.v3.vmcr = READ_SYSREG32(ICH_VMCR_EL2);
    v->arch.gic.v3.sre_el1 = READ_SYSREG32(ICC_SRE_EL1);
}

static void gicv3_restore_state(const struct vcpu *v)
{
    WRITE_SYSREG32(v->arch.gic.v3.sre_el1, ICC_SRE_EL1);
    WRITE_SYSREG32(v->arch.gic.v3.vmcr, ICH_VMCR_EL2);
    restore_aprn_regs(&v->arch.gic);
    gicv3_restore_lrs(v);

    /*
     * Make sure all stores are visible the GIC
     */
    dsb(sy);
}

static void gicv3_dump_state(const struct vcpu *v)
{
    int i;

    if ( v == current )
    {
        for ( i = 0; i < gicv3_info.nr_lrs; i++ )
            printk("   HW_LR[%d]=%lx\n", i, gicv3_ich_read_lr(i));
    }
    else
    {
        for ( i = 0; i < gicv3_info.nr_lrs; i++ )
            printk("   VCPU_LR[%d]=%lx\n", i, v->arch.gic.v3.lr[i]);
    }
}

static void gicv3_poke_irq(struct irq_desc *irqd, u32 offset)
{
    u32 mask = 1 << (irqd->irq % 32);
    void __iomem *base;

    if ( irqd->irq < NR_GIC_LOCAL_IRQS )
        base = GICD_RDIST_SGI_BASE;
    else
        base = GICD;

    writel_relaxed(mask, base + offset + (irqd->irq / 32) * 4);
    gicv3_wait_for_rwp(irqd->irq);
}

static void gicv3_unmask_irq(struct irq_desc *irqd)
{
    gicv3_poke_irq(irqd, GICD_ISENABLER);
}

static void gicv3_mask_irq(struct irq_desc *irqd)
{
    gicv3_poke_irq(irqd, GICD_ICENABLER);
}

static void gicv3_eoi_irq(struct irq_desc *irqd)
{
    /* Lower the priority */
    WRITE_SYSREG32(irqd->irq, ICC_EOIR1_EL1);
    isb();
}

static void gicv3_dir_irq(struct irq_desc *irqd)
{
    /* Deactivate */
    WRITE_SYSREG32(irqd->irq, ICC_DIR_EL1);
    isb();
}

static unsigned int gicv3_read_irq(void)
{
    return READ_SYSREG32(ICC_IAR1_EL1);
}

static inline uint64_t gicv3_mpidr_to_affinity(int cpu)
{
     uint64_t mpidr = cpu_logical_map(cpu);
     return (MPIDR_AFFINITY_LEVEL(mpidr, 3) << 32 |
             MPIDR_AFFINITY_LEVEL(mpidr, 2) << 16 |
             MPIDR_AFFINITY_LEVEL(mpidr, 1) << 8  |
             MPIDR_AFFINITY_LEVEL(mpidr, 0));
}

static void gicv3_set_irq_properties(struct irq_desc *desc,
                                     const cpumask_t *cpu_mask,
                                     unsigned int priority)
{
    uint32_t cfg, edgebit;
    uint64_t affinity;
    void __iomem *base;
    unsigned int cpu = gicv3_get_cpu_from_mask(cpu_mask);
    unsigned int irq = desc->irq;
    unsigned int type = desc->arch.type;

    /* SGI's are always edge-triggered not need to call GICD_ICFGR0 */
    ASSERT(irq >= NR_GIC_SGI);

    spin_lock(&gicv3.lock);

    if ( irq >= NR_GIC_LOCAL_IRQS)
        base = GICD + GICD_ICFGR + (irq / 16) * 4;
    else
        base = GICD_RDIST_SGI_BASE + GICR_ICFGR1;

    cfg = readl_relaxed(base);

    edgebit = 2u << (2 * (irq % 16));
    if ( type & DT_IRQ_TYPE_LEVEL_MASK )
        cfg &= ~edgebit;
    else if ( type & DT_IRQ_TYPE_EDGE_BOTH )
        cfg |= edgebit;

    writel_relaxed(cfg, base);

    affinity = gicv3_mpidr_to_affinity(cpu);
    /* Make sure we don't broadcast the interrupt */
    affinity &= ~GICD_IROUTER_SPI_MODE_ANY;

    if ( irq >= NR_GIC_LOCAL_IRQS )
        writeq_relaxed(affinity, (GICD + GICD_IROUTER + irq * 8));

    /* Set priority */
    if ( irq < NR_GIC_LOCAL_IRQS )
        writeb_relaxed(priority, GICD_RDIST_SGI_BASE + GICR_IPRIORITYR0 + irq);
    else
        writeb_relaxed(priority, GICD + GICD_IPRIORITYR + irq);

    spin_unlock(&gicv3.lock);
}

static void __init gicv3_dist_init(void)
{
    uint32_t type;
    uint32_t priority;
    uint64_t affinity;
    int i;

    /* Disable the distributor */
    writel_relaxed(0, GICD + GICD_CTLR);

    type = readl_relaxed(GICD + GICD_TYPER);
    gicv3_info.nr_lines = 32 * ((type & GICD_TYPE_LINES) + 1);

    printk("GICv3: %d lines, (IID %8.8x).\n",
           gicv3_info.nr_lines, readl_relaxed(GICD + GICD_IIDR));

    /* Default all global IRQs to level, active low */
    for ( i = NR_GIC_LOCAL_IRQS; i < gicv3_info.nr_lines; i += 16 )
        writel_relaxed(0, GICD + GICD_ICFGR + (i / 16) * 4);

    /* Default priority for global interrupts */
    for ( i = NR_GIC_LOCAL_IRQS; i < gicv3_info.nr_lines; i += 4 )
    {
        priority = (GIC_PRI_IRQ << 24 | GIC_PRI_IRQ << 16 |
                    GIC_PRI_IRQ << 8 | GIC_PRI_IRQ);
        writel_relaxed(priority, GICD + GICD_IPRIORITYR + (i / 4) * 4);
    }

    /* Disable all global interrupts */
    for ( i = NR_GIC_LOCAL_IRQS; i < gicv3_info.nr_lines; i += 32 )
        writel_relaxed(0xffffffff, GICD + GICD_ICENABLER + (i / 32) * 4);

    gicv3_dist_wait_for_rwp();

    /* Turn on the distributor */
    writel_relaxed(GICD_CTL_ENABLE | GICD_CTLR_ARE_NS |
                GICD_CTLR_ENABLE_G1A | GICD_CTLR_ENABLE_G1, GICD + GICD_CTLR);

    /* Route all global IRQs to this CPU */
    affinity = gicv3_mpidr_to_affinity(smp_processor_id());
    /* Make sure we don't broadcast the interrupt */
    affinity &= ~GICD_IROUTER_SPI_MODE_ANY;

    for ( i = NR_GIC_LOCAL_IRQS; i < gicv3_info.nr_lines; i++ )
        writeq_relaxed(affinity, GICD + GICD_IROUTER + i * 8);
}

static int gicv3_enable_redist(void)
{
    uint32_t val;
    bool_t timeout = 0;
    s_time_t deadline = NOW() + MILLISECS(1000);

    /* Wake up this CPU redistributor */
    val = readl_relaxed(GICD_RDIST_BASE + GICR_WAKER);
    val &= ~GICR_WAKER_ProcessorSleep;
    writel_relaxed(val, GICD_RDIST_BASE + GICR_WAKER);

    do {
        val = readl_relaxed(GICD_RDIST_BASE + GICR_WAKER);
        if ( !(val & GICR_WAKER_ChildrenAsleep) )
            break;
        if ( NOW() > deadline )
        {
            timeout = 1;
            break;
        }
        cpu_relax();
        udelay(1);
    } while ( timeout );

    if ( timeout )
    {
        dprintk(XENLOG_ERR, "GICv3: Redist enable RWP timeout\n");
        return 1;
    }

    return 0;
}

static int __init gicv3_populate_rdist(void)
{
    int i;
    uint32_t aff;
    uint32_t reg;
    uint64_t typer;
    uint64_t mpidr = cpu_logical_map(smp_processor_id());

    /*
     * If we ever get a cluster of more than 16 CPUs, just scream.
     */
    if ( (mpidr & 0xff) >= 16 )
          dprintk(XENLOG_WARNING, "GICv3:Cluster with more than 16's cpus\n");

    /*
     * Convert affinity to a 32bit value that can be matched to GICR_TYPER
     * bits [63:32]
     */
    aff = (MPIDR_AFFINITY_LEVEL(mpidr, 3) << 24 |
           MPIDR_AFFINITY_LEVEL(mpidr, 2) << 16 |
           MPIDR_AFFINITY_LEVEL(mpidr, 1) << 8 |
           MPIDR_AFFINITY_LEVEL(mpidr, 0));

    for ( i = 0; i < gicv3.rdist_count; i++ )
    {
        void __iomem *ptr = gicv3.rdist_regions[i].map_base;

        reg = readl_relaxed(ptr + GICR_PIDR2) & GICR_PIDR2_ARCH_REV_MASK;
        if ( (reg >> GICR_PIDR2_ARCH_REV_SHIFT) != GICR_PIDR2_ARCH_GICV3 )
        {
            dprintk(XENLOG_ERR,
                    "GICv3: No redistributor present @%"PRIpaddr"\n",
                    gicv3.rdist_regions[i].base);
            break;
        }

        do {
            typer = readq_relaxed(ptr + GICR_TYPER);

            if ( (typer >> 32) == aff )
            {
                this_cpu(rbase) = ptr;
                printk("GICv3: CPU%d: Found redistributor in region %d @%p\n",
                        smp_processor_id(), i, ptr);
                return 0;
            }
            if ( gicv3.rdist_stride )
                ptr += gicv3.rdist_stride;
            else
            {
                ptr += SZ_64K * 2;
                if ( typer & GICR_TYPER_VLPIS )
                    ptr += SZ_64K * 2; /* Skip VLPI_base + reserved page */
            }

        } while ( !(typer & GICR_TYPER_LAST) );
    }

    dprintk(XENLOG_ERR, "GICv3: CPU%d: mpidr 0x%x has no re-distributor!\n",
            smp_processor_id(), cpu_logical_map(smp_processor_id()));

    return -ENODEV;
}

static int __cpuinit gicv3_cpu_init(void)
{
    int i;
    uint32_t priority;

    /* Register ourselves with the rest of the world */
    if ( gicv3_populate_rdist() )
        return -ENODEV;

    if ( gicv3_enable_redist() )
        return -ENODEV;

    /* Set priority on PPI and SGI interrupts */
    priority = (GIC_PRI_IPI << 24 | GIC_PRI_IPI << 16 | GIC_PRI_IPI << 8 |
                GIC_PRI_IPI);
    for (i = 0; i < NR_GIC_SGI; i += 4)
        writel_relaxed(priority,
                GICD_RDIST_SGI_BASE + GICR_IPRIORITYR0 + (i / 4) * 4);

    priority = (GIC_PRI_IRQ << 24 | GIC_PRI_IRQ << 16 | GIC_PRI_IRQ << 8 |
                GIC_PRI_IRQ);
    for (i = NR_GIC_SGI; i < NR_GIC_LOCAL_IRQS; i += 4)
        writel_relaxed(priority,
                GICD_RDIST_SGI_BASE + GICR_IPRIORITYR0 + (i / 4) * 4);

    /*
     * Disable all PPI interrupts, ensure all SGI interrupts are
     * enabled.
     */
    writel_relaxed(0xffff0000, GICD_RDIST_SGI_BASE + GICR_ICENABLER0);
    writel_relaxed(0x0000ffff, GICD_RDIST_SGI_BASE + GICR_ISENABLER0);

    gicv3_redist_wait_for_rwp();

    /* Enable system registers */
    gicv3_enable_sre();

    /* No priority grouping */
    WRITE_SYSREG32(0, ICC_BPR1_EL1);

    /* Set priority mask register */
    WRITE_SYSREG32(DEFAULT_PMR_VALUE, ICC_PMR_EL1);

    /* EOI drops priority too (mode 0) */
    WRITE_SYSREG32(GICC_CTLR_EL1_EOImode_drop, ICC_CTLR_EL1);

    /* Enable Group1 interrupts */
    WRITE_SYSREG32(1, ICC_IGRPEN1_EL1);

    /* Sync at once at the end of cpu interface configuration */
    isb();

    return 0;
}

static void gicv3_cpu_disable(void)
{
    WRITE_SYSREG32(0, ICC_CTLR_EL1);
    isb();
}

static void __cpuinit gicv3_hyp_init(void)
{
    uint32_t vtr;

    vtr = READ_SYSREG32(ICH_VTR_EL2);
    gicv3_info.nr_lrs  = (vtr & GICH_VTR_NRLRGS) + 1;
    gicv3.nr_priorities = ((vtr >> GICH_VTR_PRIBITS_SHIFT) &
                          GICH_VTR_PRIBITS_MASK) + 1;

    if ( !((gicv3.nr_priorities > 4) && (gicv3.nr_priorities < 8)) )
        panic("GICv3: Invalid number of priority bits\n");

    WRITE_SYSREG32(GICH_VMCR_EOI | GICH_VMCR_VENG1, ICH_VMCR_EL2);
    WRITE_SYSREG32(GICH_HCR_EN, ICH_HCR_EL2);
}

/* Set up the per-CPU parts of the GIC for a secondary CPU */
static int gicv3_secondary_cpu_init(void)
{
    int res;

    spin_lock(&gicv3.lock);

    res = gicv3_cpu_init();
    gicv3_hyp_init();

    spin_unlock(&gicv3.lock);

    return res;
}

static void __cpuinit gicv3_hyp_disable(void)
{
    uint32_t hcr;

    hcr = READ_SYSREG32(ICH_HCR_EL2);
    hcr &= ~GICH_HCR_EN;
    WRITE_SYSREG32(hcr, ICH_HCR_EL2);
    isb();
}

static u16 gicv3_compute_target_list(int *base_cpu, const struct cpumask *mask,
                                     uint64_t cluster_id)
{
    int cpu = *base_cpu;
    uint64_t mpidr = cpu_logical_map(cpu);
    u16 tlist = 0;

    while ( cpu < nr_cpu_ids )
    {
        /*
         * Assume that each cluster does not have more than 16 CPU's.
         * Check is made during GICv3 initialization (gicv3_populate_rdist())
         * on mpidr value for this. So skip this check here.
         */
        tlist |= 1 << (mpidr & 0xf);

        cpu = cpumask_next(cpu, mask);
        if ( cpu == nr_cpu_ids )
        {
            cpu--;
            goto out;
        }

        mpidr = cpu_logical_map(cpu);
        if ( cluster_id != (mpidr & ~MPIDR_AFF0_MASK) ) {
            cpu--;
            goto out;
        }
    }
out:
    *base_cpu = cpu;

    return tlist;
}

static void gicv3_send_sgi(enum gic_sgi sgi, enum gic_sgi_mode mode,
                           const cpumask_t *cpumask)
{
    int cpu = 0;
    uint64_t val;

    for_each_cpu(cpu, cpumask)
    {
        /* Mask lower 8 bits. It represent cpu in affinity level 0 */
        uint64_t cluster_id = cpu_logical_map(cpu) & ~MPIDR_AFF0_MASK;
        u16 tlist;

        /* Get targetlist for the cluster to send SGI */
        tlist = gicv3_compute_target_list(&cpu, cpumask, cluster_id);

        /*
         * Prepare affinity path of the cluster for which SGI is generated
         * along with SGI number
         */
        val = (MPIDR_AFFINITY_LEVEL(cluster_id, 3) << 48  |
               MPIDR_AFFINITY_LEVEL(cluster_id, 2) << 32  |
               sgi << 24                                  |
               MPIDR_AFFINITY_LEVEL(cluster_id, 1) << 16  |
               tlist);

        WRITE_SYSREG(val, ICC_SGI1R_EL1);
    }
    /* Force above writes to ICC_SGI1R_EL1 */
    isb();
}

/* Shut down the per-CPU GIC interface */
static void gicv3_disable_interface(void)
{
    spin_lock(&gicv3.lock);

    gicv3_cpu_disable();
    gicv3_hyp_disable();

    spin_unlock(&gicv3.lock);
}

static void gicv3_update_lr(int lr, const struct pending_irq *p,
                            unsigned int state)
{
    uint64_t grp = GICH_LR_GRP1;
    uint64_t val = 0;

    BUG_ON(lr >= gicv3_info.nr_lrs);
    BUG_ON(lr < 0);

    val =  (((uint64_t)state & 0x3) << GICH_LR_STATE_SHIFT) | grp;
    val |= ((uint64_t)p->priority & 0xff) << GICH_LR_PRIORITY_SHIFT;
    val |= ((uint64_t)p->irq & GICH_LR_VIRTUAL_MASK) << GICH_LR_VIRTUAL_SHIFT;

   if ( p->desc != NULL )
       val |= GICH_LR_HW | (((uint64_t)p->desc->irq & GICH_LR_PHYSICAL_MASK)
                           << GICH_LR_PHYSICAL_SHIFT);

    gicv3_ich_write_lr(lr, val);
}

static void gicv3_clear_lr(int lr)
{
    gicv3_ich_write_lr(lr, 0);
}

static void gicv3_read_lr(int lr, struct gic_lr *lr_reg)
{
    uint64_t lrv;

    lrv = gicv3_ich_read_lr(lr);

    lr_reg->pirq = (lrv >> GICH_LR_PHYSICAL_SHIFT) & GICH_LR_PHYSICAL_MASK;
    lr_reg->virq = (lrv >> GICH_LR_VIRTUAL_SHIFT) & GICH_LR_VIRTUAL_MASK;

    lr_reg->priority  = (lrv >> GICH_LR_PRIORITY_SHIFT) & GICH_LR_PRIORITY_MASK;
    lr_reg->state     = (lrv >> GICH_LR_STATE_SHIFT) & GICH_LR_STATE_MASK;
    lr_reg->hw_status = (lrv >> GICH_LR_HW_SHIFT) & GICH_LR_HW_MASK;
    lr_reg->grp       = (lrv >> GICH_LR_GRP_SHIFT) & GICH_LR_GRP_MASK;
}

static void gicv3_write_lr(int lr_reg, const struct gic_lr *lr)
{
    uint64_t lrv = 0;

    lrv = ( ((u64)(lr->pirq & GICH_LR_PHYSICAL_MASK) << GICH_LR_PHYSICAL_SHIFT)|
        ((u64)(lr->virq & GICH_LR_VIRTUAL_MASK)  << GICH_LR_VIRTUAL_SHIFT) |
        ((u64)(lr->priority & GICH_LR_PRIORITY_MASK) << GICH_LR_PRIORITY_SHIFT)|
        ((u64)(lr->state & GICH_LR_STATE_MASK) << GICH_LR_STATE_SHIFT) |
        ((u64)(lr->hw_status & GICH_LR_HW_MASK) << GICH_LR_HW_SHIFT)  |
        ((u64)(lr->grp & GICH_LR_GRP_MASK) << GICH_LR_GRP_SHIFT) );

    gicv3_ich_write_lr(lr_reg, lrv);
}

static int gicv_v3_init(struct domain *d)
{
    int i;

    /*
     * Domain 0 gets the hardware address.
     * Guests get the virtual platform layout.
     */
    if ( is_hardware_domain(d) )
    {
        d->arch.vgic.dbase = gicv3.dbase;
        d->arch.vgic.dbase_size = gicv3.dbase_size;
        for ( i = 0; i < gicv3.rdist_count; i++ )
        {
            d->arch.vgic.rbase[i] = gicv3.rdist_regions[i].base;
            d->arch.vgic.rbase_size[i] = gicv3.rdist_regions[i].size;
        }
        d->arch.vgic.rdist_stride = gicv3.rdist_stride;
        d->arch.vgic.rdist_count = gicv3.rdist_count;
    }
    else
    {
        d->arch.vgic.dbase = GUEST_GICV3_GICD_BASE;
        d->arch.vgic.dbase_size = GUEST_GICV3_GICD_SIZE;

        /* XXX: Only one Re-distributor region mapped for the guest */
        BUILD_BUG_ON(GUEST_GICV3_RDIST_REGIONS != 1);

        d->arch.vgic.rdist_count = GUEST_GICV3_RDIST_REGIONS;
        d->arch.vgic.rdist_stride = GUEST_GICV3_RDIST_STRIDE;

        /* The first redistributor should contain enough space for all CPUs */
        BUILD_BUG_ON((GUEST_GICV3_GICR0_SIZE / GUEST_GICV3_RDIST_STRIDE) < MAX_VIRT_CPUS);
        d->arch.vgic.rbase[0] = GUEST_GICV3_GICR0_BASE;
        d->arch.vgic.rbase_size[0] = GUEST_GICV3_GICR0_SIZE;
    }

    return 0;
}

static void gicv3_hcr_status(uint32_t flag, bool_t status)
{
    uint32_t hcr;

    hcr = READ_SYSREG32(ICH_HCR_EL2);
    if ( status )
        WRITE_SYSREG32(hcr | flag, ICH_HCR_EL2);
    else
        WRITE_SYSREG32(hcr & (~flag), ICH_HCR_EL2);
    isb();
}

static unsigned int gicv3_read_vmcr_priority(void)
{
   return ((READ_SYSREG32(ICH_VMCR_EL2) >> GICH_VMCR_PRIORITY_SHIFT) &
            GICH_VMCR_PRIORITY_MASK);
}

/* Only support reading GRP1 APRn registers */
static unsigned int gicv3_read_apr(int apr_reg)
{
    switch ( apr_reg )
    {
    case 0:
        ASSERT(gicv3.nr_priorities > 4 && gicv3.nr_priorities < 8);
        return READ_SYSREG32(ICH_AP1R0_EL2);
    case 1:
        ASSERT(gicv3.nr_priorities > 5 && gicv3.nr_priorities < 8);
        return READ_SYSREG32(ICH_AP1R1_EL2);
    case 2:
        ASSERT(gicv3.nr_priorities > 6 && gicv3.nr_priorities < 8);
        return READ_SYSREG32(ICH_AP1R2_EL2);
    default:
        BUG();
    }
}

static void gicv3_irq_enable(struct irq_desc *desc)
{
    unsigned long flags;

    ASSERT(spin_is_locked(&desc->lock));

    spin_lock_irqsave(&gicv3.lock, flags);
    clear_bit(_IRQ_DISABLED, &desc->status);
    dsb(sy);
    /* Enable routing */
    gicv3_unmask_irq(desc);
    spin_unlock_irqrestore(&gicv3.lock, flags);
}

static void gicv3_irq_disable(struct irq_desc *desc)
{
    unsigned long flags;

    ASSERT(spin_is_locked(&desc->lock));

    spin_lock_irqsave(&gicv3.lock, flags);
    /* Disable routing */
    gicv3_mask_irq(desc);
    set_bit(_IRQ_DISABLED, &desc->status);
    spin_unlock_irqrestore(&gicv3.lock, flags);
}

static unsigned int gicv3_irq_startup(struct irq_desc *desc)
{
    gicv3_irq_enable(desc);

    return 0;
}

static void gicv3_irq_shutdown(struct irq_desc *desc)
{
    gicv3_irq_disable(desc);
}

static void gicv3_irq_ack(struct irq_desc *desc)
{
    /* No ACK -- reading IAR has done this for us */
}

static void gicv3_host_irq_end(struct irq_desc *desc)
{
    /* Lower the priority */
    gicv3_eoi_irq(desc);
    /* Deactivate */
    gicv3_dir_irq(desc);
}

static void gicv3_guest_irq_end(struct irq_desc *desc)
{
    /* Lower the priority of the IRQ */
    gicv3_eoi_irq(desc);
    /* Deactivation happens in maintenance interrupt / via GICV */
}

static void gicv3_irq_set_affinity(struct irq_desc *desc, const cpumask_t *mask)
{
    unsigned int cpu;
    uint64_t affinity;

    ASSERT(!cpumask_empty(mask));

    spin_lock(&gicv3.lock);

    cpu = gicv3_get_cpu_from_mask(mask);
    affinity = gicv3_mpidr_to_affinity(cpu);
    /* Make sure we don't broadcast the interrupt */
    affinity &= ~GICD_IROUTER_SPI_MODE_ANY;

    if ( desc->irq >= NR_GIC_LOCAL_IRQS )
        writeq_relaxed(affinity, (GICD + GICD_IROUTER + desc->irq * 8));

    spin_unlock(&gicv3.lock);
}

static int gicv3_make_dt_node(const struct domain *d,
                              const struct dt_device_node *node, void *fdt)
{
    const struct dt_device_node *gic = dt_interrupt_controller;
    const void *compatible = NULL;
    uint32_t len;
    __be32 *new_cells, *tmp;
    uint32_t rd_stride = 0;
    uint32_t rd_count = 0;

    int i, res = 0;

    compatible = dt_get_property(gic, "compatible", &len);
    if ( !compatible )
    {
        dprintk(XENLOG_ERR, "Can't find compatible property for the gic node\n");
        return -FDT_ERR_XEN(ENOENT);
    }

    res = fdt_begin_node(fdt, "interrupt-controller");
    if ( res )
        return res;

    res = fdt_property(fdt, "compatible", compatible, len);
    if ( res )
        return res;

    res = fdt_property_cell(fdt, "#interrupt-cells", 3);
    if ( res )
        return res;

    res = fdt_property(fdt, "interrupt-controller", NULL, 0);
    if ( res )
        return res;

    res = dt_property_read_u32(gic, "redistributor-stride", &rd_stride);
    if ( !res )
        rd_stride = 0;

    res = dt_property_read_u32(gic, "#redistributor-regions", &rd_count);
    if ( !res )
        rd_count = 1;

    res = fdt_property_cell(fdt, "redistributor-stride", rd_stride);
    if ( res )
        return res;

    res = fdt_property_cell(fdt, "#redistributor-regions", rd_count);
    if ( res )
        return res;

    len = dt_cells_to_size(dt_n_addr_cells(node) + dt_n_size_cells(node));
    /*
     * GIC has two memory regions: Distributor + rdist regions
     * CPU interface and virtual cpu interfaces accessesed as System registers
     * So cells are created only for Distributor and rdist regions
     */
    len = len * (d->arch.vgic.rdist_count + 1);
    new_cells = xzalloc_bytes(len);
    if ( new_cells == NULL )
        return -FDT_ERR_XEN(ENOMEM);

    tmp = new_cells;

    dt_set_range(&tmp, node, d->arch.vgic.dbase, d->arch.vgic.dbase_size);

    for ( i = 0; i < d->arch.vgic.rdist_count; i++ )
        dt_set_range(&tmp, node, d->arch.vgic.rbase[i],
                     d->arch.vgic.rbase_size[i]);

    res = fdt_property(fdt, "reg", new_cells, len);
    xfree(new_cells);

    return res;
}

static const hw_irq_controller gicv3_host_irq_type = {
    .typename     = "gic-v3",
    .startup      = gicv3_irq_startup,
    .shutdown     = gicv3_irq_shutdown,
    .enable       = gicv3_irq_enable,
    .disable      = gicv3_irq_disable,
    .ack          = gicv3_irq_ack,
    .end          = gicv3_host_irq_end,
    .set_affinity = gicv3_irq_set_affinity,
};

static const hw_irq_controller gicv3_guest_irq_type = {
    .typename     = "gic-v3",
    .startup      = gicv3_irq_startup,
    .shutdown     = gicv3_irq_shutdown,
    .enable       = gicv3_irq_enable,
    .disable      = gicv3_irq_disable,
    .ack          = gicv3_irq_ack,
    .end          = gicv3_guest_irq_end,
    .set_affinity = gicv3_irq_set_affinity,
};

static const struct gic_hw_operations gicv3_ops = {
    .info                = &gicv3_info,
    .save_state          = gicv3_save_state,
    .restore_state       = gicv3_restore_state,
    .dump_state          = gicv3_dump_state,
    .gicv_setup          = gicv_v3_init,
    .gic_host_irq_type   = &gicv3_host_irq_type,
    .gic_guest_irq_type  = &gicv3_guest_irq_type,
    .eoi_irq             = gicv3_eoi_irq,
    .deactivate_irq      = gicv3_dir_irq,
    .read_irq            = gicv3_read_irq,
    .set_irq_properties  = gicv3_set_irq_properties,
    .send_SGI            = gicv3_send_sgi,
    .disable_interface   = gicv3_disable_interface,
    .update_lr           = gicv3_update_lr,
    .update_hcr_status   = gicv3_hcr_status,
    .clear_lr            = gicv3_clear_lr,
    .read_lr             = gicv3_read_lr,
    .write_lr            = gicv3_write_lr,
    .read_vmcr_priority  = gicv3_read_vmcr_priority,
    .read_apr            = gicv3_read_apr,
    .secondary_init      = gicv3_secondary_cpu_init,
    .make_dt_node        = gicv3_make_dt_node,
};

/* Set up the GIC */
static int __init gicv3_init(struct dt_device_node *node, const void *data)
{
    struct rdist_region *rdist_regs;
    int res, i;
    uint32_t reg;

    if ( !cpu_has_gicv3 )
    {
        dprintk(XENLOG_ERR, "GICv3: driver requires system register support\n");
        return -ENODEV;
    }

    dt_device_set_used_by(node, DOMID_XEN);

    res = dt_device_get_address(node, 0, &gicv3.dbase, &gicv3.dbase_size);
    if ( res || !gicv3.dbase )
        panic("GICv3: Cannot find a valid distributor address");

    if ( (gicv3.dbase & ~PAGE_MASK) || (gicv3.dbase_size & ~PAGE_MASK) )
        panic("GICv3:  Found unaligned distributor address %"PRIpaddr"",
              gicv3.dbase);

    gicv3.map_dbase = ioremap_nocache(gicv3.dbase, gicv3.dbase_size);
    if ( !gicv3.map_dbase )
        panic("GICv3: Failed to ioremap for GIC distributor\n");

    reg = readl_relaxed(GICD + GICD_PIDR2) & GICD_PIDR2_ARCH_REV_MASK;
    if ( ((reg >> GICD_PIDR2_ARCH_REV_SHIFT) != GICD_PIDR2_ARCH_GICV3) )
         panic("GICv3: no distributor detected\n");

    if ( !dt_property_read_u32(node, "#redistributor-regions",
                &gicv3.rdist_count) )
        gicv3.rdist_count = 1;

    if ( gicv3.rdist_count > MAX_RDIST_COUNT )
        panic("GICv3: Number of redistributor regions is more than"
              "%d (Increase MAX_RDIST_COUNT!!)\n", MAX_RDIST_COUNT);

    rdist_regs = xzalloc_array(struct rdist_region, gicv3.rdist_count);
    if ( !rdist_regs )
        panic("GICv3: Failed to allocate memory for rdist regions\n");

    for ( i = 0; i < gicv3.rdist_count; i++ )
    {
        uint64_t rdist_base, rdist_size;

        res = dt_device_get_address(node, 1 + i, &rdist_base, &rdist_size);
        if ( res || !rdist_base )
            panic("GICv3: No rdist base found for region %d\n", i);

        rdist_regs[i].base = rdist_base;
        rdist_regs[i].size = rdist_size;
    }

    /* If stride is not set in dt. Set default to 2 * SZ_64K */
    if ( !dt_property_read_u32(node, "redistributor-stride", &gicv3.rdist_stride) )
        gicv3.rdist_stride = 0;

    gicv3.rdist_regions= rdist_regs;

    res = platform_get_irq(node, 0);
    if ( res < 0 )
        panic("GICv3: Cannot find the maintenance IRQ");
    gicv3_info.maintenance_irq = res;

    /* Set the GIC as the primary interrupt controller */
    dt_interrupt_controller = node;

    for ( i = 0; i < gicv3.rdist_count; i++ )
    {
        /* map dbase & rdist regions */
        gicv3.rdist_regions[i].map_base =
                ioremap_nocache(gicv3.rdist_regions[i].base,
                                gicv3.rdist_regions[i].size);

        if ( !gicv3.rdist_regions[i].map_base )
            panic("GICv3: Failed to ioremap rdist region for region %d\n", i);
    }

    printk("GICv3 initialization:\n"
           "      gic_dist_addr=%"PRIpaddr"\n"
           "      gic_dist_size=%"PRIpaddr"\n"
           "      gic_dist_mapaddr=%p\n"
           "      gic_rdist_regions=%d\n"
           "      gic_rdist_stride=%x\n"
           "      gic_rdist_base=%"PRIpaddr"\n"
           "      gic_rdist_base_size=%"PRIpaddr"\n"
           "      gic_rdist_base_mapaddr=%p\n"
           "      gic_maintenance_irq=%u\n",
           gicv3.dbase, gicv3.dbase_size, gicv3.map_dbase, gicv3.rdist_count,
           gicv3.rdist_stride, gicv3.rdist_regions[0].base,
           gicv3.rdist_regions[0].size, gicv3.rdist_regions[0].map_base,
           gicv3_info.maintenance_irq);

    spin_lock_init(&gicv3.lock);

    spin_lock(&gicv3.lock);

    gicv3_dist_init();
    res = gicv3_cpu_init();
    gicv3_hyp_init();

    gicv3_info.hw_version = GIC_V3;
    /* Register hw ops*/
    register_gic_ops(&gicv3_ops);

    spin_unlock(&gicv3.lock);

    return res;
}

static const char * const gicv3_dt_compat[] __initconst =
{
    DT_COMPAT_GIC_V3,
    NULL
};

DT_DEVICE_START(gicv3, "GICv3", DEVICE_GIC)
        .compatible = gicv3_dt_compat,
        .init = gicv3_init,
DT_DEVICE_END

/*
 * Local variables:
 * mode: C
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * End:
 */
