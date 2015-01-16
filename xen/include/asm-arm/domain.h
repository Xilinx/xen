#ifndef __ASM_DOMAIN_H__
#define __ASM_DOMAIN_H__

#include <xen/config.h>
#include <xen/cache.h>
#include <xen/sched.h>
#include <asm/page.h>
#include <asm/p2m.h>
#include <asm/vfp.h>
#include <asm/mmio.h>
#include <asm/gic.h>
#include <public/hvm/params.h>
#include <xen/serial.h>
#include <xen/hvm/iommu.h>

struct hvm_domain
{
    uint64_t              params[HVM_NR_PARAMS];
    struct hvm_iommu      iommu;
}  __cacheline_aligned;

#ifdef CONFIG_ARM_64
enum domain_type {
    DOMAIN_32BIT,
    DOMAIN_64BIT,
};
#define is_32bit_domain(d) ((d)->arch.type == DOMAIN_32BIT)
#define is_64bit_domain(d) ((d)->arch.type == DOMAIN_64BIT)
#else
#define is_32bit_domain(d) (1)
#define is_64bit_domain(d) (0)
#endif

extern int dom0_11_mapping;
#define is_domain_direct_mapped(d) ((d) == hardware_domain && dom0_11_mapping)

struct vtimer {
        struct vcpu *v;
        int irq;
        struct timer timer;
        uint32_t ctl;
        uint64_t cval;
};

struct arch_domain
{
#ifdef CONFIG_ARM_64
    enum domain_type type;
#endif

    /* Virtual MMU */
    struct p2m_domain p2m;
    uint64_t vttbr;

    struct hvm_domain hvm_domain;
    xen_pfn_t *grant_table_gpfn;

    struct io_handler io_handlers;
    /* Continuable domain_relinquish_resources(). */
    enum {
        RELMEM_not_started,
        RELMEM_xen,
        RELMEM_page,
        RELMEM_mapping,
        RELMEM_done,
    } relmem;

    /* Virtual CPUID */
    uint32_t vpidr;

    struct {
        uint64_t offset;
    } phys_timer_base;
    struct {
        uint64_t offset;
    } virt_timer_base;

    struct {
        /* GIC HW version specific vGIC driver handler */
        const struct vgic_ops *handler;
        /*
         * Covers access to other members of this struct _except_ for
         * shared_irqs where each member contains its own locking.
         *
         * If both class of lock is required then this lock must be
         * taken first. If multiple rank locks are required (including
         * the per-vcpu private_irqs rank) then they must be taken in
         * rank order.
         */
        spinlock_t lock;
        int ctlr;
        int nr_spis; /* Number of SPIs */
        struct vgic_irq_rank *shared_irqs;
        /*
         * SPIs are domain global, SGIs and PPIs are per-VCPU and stored in
         * struct arch_vcpu.
         */
        struct pending_irq *pending_irqs;
        /* Base address for guest GIC */
        paddr_t dbase; /* Distributor base address */
        paddr_t cbase; /* CPU base address */
#ifdef CONFIG_ARM_64
        /* GIC V3 addressing */
        paddr_t dbase_size; /* Distributor base size */
        paddr_t rbase[MAX_RDIST_COUNT];      /* Re-Distributor base address */
        paddr_t rbase_size[MAX_RDIST_COUNT]; /* Re-Distributor size */
        uint32_t rdist_stride;               /* Re-Distributor stride */
        int rdist_count;                     /* No. of Re-Distributors */
#endif
    } vgic;

    struct vuart {
#define VUART_BUF_SIZE 128
        char                        *buf;
        int                         idx;
        const struct vuart_info     *info;
        spinlock_t                  lock;
    } vuart;

    unsigned int evtchn_irq;
}  __cacheline_aligned;

struct arch_vcpu
{
    struct {
#ifdef CONFIG_ARM_32
        register_t r4;
        register_t r5;
        register_t r6;
        register_t r7;
        register_t r8;
        register_t r9;
        register_t sl;
#else
        register_t x19;
        register_t x20;
        register_t x21;
        register_t x22;
        register_t x23;
        register_t x24;
        register_t x25;
        register_t x26;
        register_t x27;
        register_t x28;
#endif
        register_t fp;
        register_t sp;
        register_t pc;
    } saved_context;

    void *stack;

    /*
     * Points into ->stack, more convenient than doing pointer arith
     * all the time.
     */
    struct cpu_info *cpu_info;

    /* Fault Status */
#ifdef CONFIG_ARM_32
    uint32_t dfsr;
    uint32_t dfar, ifar;
#else
    uint64_t far;
    uint32_t esr;
#endif

    uint32_t ifsr; /* 32-bit guests only */
    uint32_t afsr0, afsr1;

    /* MMU */
    register_t vbar;
    register_t ttbcr;
    uint64_t ttbr0, ttbr1;

    uint32_t dacr; /* 32-bit guests only */
    uint64_t par;
#ifdef CONFIG_ARM_32
    uint32_t mair0, mair1;
    uint32_t amair0, amair1;
#else
    uint64_t mair;
    uint64_t amair;
#endif

    /* Control Registers */
    uint32_t actlr, sctlr;
    uint32_t cpacr;

    uint32_t contextidr;
    register_t tpidr_el0;
    register_t tpidr_el1;
    register_t tpidrro_el0;

    uint32_t teecr, teehbr; /* ThumbEE, 32-bit guests only */
#ifdef CONFIG_ARM_32
    /*
     * ARMv8 only supports a trivial implementation on Jazelle when in AArch32
     * mode and therefore has no extended control registers.
     */
    uint32_t joscr, jmcr;
#endif

    /* Float-pointer */
    struct vfp_state vfp;

    /* CP 15 */
    uint32_t csselr;
    register_t vmpidr;

    /* Holds gic context data */
    union gic_state_data gic;
    uint64_t lr_mask;

    struct {
        /*
         * SGIs and PPIs are per-VCPU, SPIs are domain global and in
         * struct arch_domain.
         */
        struct pending_irq pending_irqs[32];
        struct vgic_irq_rank *private_irqs;

        /* This list is ordered by IRQ priority and it is used to keep
         * track of the IRQs that the VGIC injected into the guest.
         * Depending on the availability of LR registers, the IRQs might
         * actually be in an LR, and therefore injected into the guest,
         * or queued in gic.lr_pending.
         * As soon as an IRQ is EOI'd by the guest and removed from the
         * corresponding LR it is also removed from this list. */
        struct list_head inflight_irqs;
        /* lr_pending is used to queue IRQs (struct pending_irq) that the
         * vgic tried to inject in the guest (calling gic_set_guest_irq) but
         * no LRs were available at the time.
         * As soon as an LR is freed we remove the first IRQ from this
         * list and write it to the LR register.
         * lr_pending is a subset of vgic.inflight_irqs. */
        struct list_head lr_pending;
        spinlock_t lock;
    } vgic;

    /* Timer registers  */
    uint32_t cntkctl;

    struct vtimer phys_timer;
    struct vtimer virt_timer;
    bool_t vtimer_initialized;
}  __cacheline_aligned;

void vcpu_show_execution_state(struct vcpu *);
void vcpu_show_registers(const struct vcpu *);

#endif /* __ASM_DOMAIN_H__ */

/*
 * Local variables:
 * mode: C
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * tab-width: 4
 * indent-tabs-mode: nil
 * End:
 */
