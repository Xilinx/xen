/* SPDX-License-Identifier: (GPL-2.0-or-later OR BSD-2-Clause) */

#include <xen/guest_access.h>
#include <xen/param.h>
#include <xen/sched.h>
#include <asm/mmio.h>
#include <asm/vgic-emul.h>
#include <asm/viommu.h>
#include <asm/vreg.h>

#include "smmu-v3.h"

/* Register Definition */
#define ARM_SMMU_IDR2       0x8
#define ARM_SMMU_IDR3       0xc
#define ARM_SMMU_IDR4       0x10
#define IDR0_TERM_MODEL     (1 << 26)
#define IDR3_RIL            (1 << 10)
#define CR0_RESERVED        0xFFFFFC20
#define SMMU_IDR1_SIDSIZE   16
#define SMMU_CMDQS          19
#define SMMU_EVTQS          19
#define DWORDS_BYTES        8

/* Struct to hold the vIOMMU ops and vIOMMU type */
extern const struct viommu_desc __read_mostly *cur_viommu;

/* SMMUv3 command definitions */
#define CMDQ_OP_PREFETCH_CFG    0x1
#define CMDQ_OP_CFGI_STE        0x3
#define CMDQ_OP_CFGI_ALL        0x4
#define CMDQ_OP_CFGI_CD         0x5
#define CMDQ_OP_CFGI_CD_ALL     0x6
#define CMDQ_OP_TLBI_NH_ASID    0x11
#define CMDQ_OP_TLBI_NH_VA      0x12
#define CMDQ_OP_TLBI_NSNH_ALL   0x30
#define CMDQ_OP_CMD_SYNC        0x46

/* Queue Handling */
#define Q_BASE(q)       ((q)->q_base & Q_BASE_ADDR_MASK)
#define Q_CONS_ENT(q)   (Q_BASE(q) + Q_IDX(q, (q)->cons) * (q)->ent_size)
#define Q_PROD_ENT(q)   (Q_BASE(q) + Q_IDX(q, (q)->prod) * (q)->ent_size)

/* Helper Macros */
#define smmu_get_cmdq_enabled(x)    FIELD_GET(CR0_CMDQEN, x)
#define smmu_get_evtq_enabled(x)    FIELD_GET(CR0_EVTQEN, x)
#define smmu_cmd_get_command(x)     FIELD_GET(CMDQ_0_OP, x)
#define smmu_cmd_get_sid(x)         FIELD_GET(CMDQ_PREFETCH_0_SID, x)
#define smmu_get_ste_s1cdmax(x)     FIELD_GET(STRTAB_STE_0_S1CDMAX, x)
#define smmu_get_ste_s1fmt(x)       FIELD_GET(STRTAB_STE_0_S1FMT, x)
#define smmu_get_ste_s1stalld(x)    FIELD_GET(STRTAB_STE_1_S1STALLD, x)
#define smmu_get_ste_s1ctxptr(x)    FIELD_PREP(STRTAB_STE_0_S1CTXPTR_MASK, \
                                    FIELD_GET(STRTAB_STE_0_S1CTXPTR_MASK, x))

/* event queue entry */
struct arm_smmu_evtq_ent {
    /* Common fields */
    uint8_t     opcode;
    uint32_t    sid;

    /* Event-specific fields */
    union {
        struct {
            uint32_t ssid;
            bool ssv;
        } c_bad_ste_streamid;

        struct {
            bool stall;
            uint16_t stag;
            uint32_t ssid;
            bool ssv;
            bool s2;
            uint64_t addr;
            bool rnw;
            bool pnu;
            bool ind;
            uint8_t class;
            uint64_t addr2;
        } f_translation;
    };
};

/* stage-1 translation configuration */
struct arm_vsmmu_s1_trans_cfg {
    paddr_t s1ctxptr;
    uint8_t s1fmt;
    uint8_t s1cdmax;
    bool    bypassed;             /* translation is bypassed */
    bool    aborted;              /* translation is aborted */
};

/* virtual smmu queue */
struct arm_vsmmu_queue {
    uint64_t    q_base; /* base register */
    uint32_t    prod;
    uint32_t    cons;
    uint8_t     ent_size;
    uint8_t     max_n_shift;
};

struct virt_smmu {
    struct      domain *d;
    struct      list_head viommu_list;
    uint8_t     sid_split;
    uint32_t    features;
    uint32_t    cr[3];
    uint32_t    cr0ack;
    uint32_t    gerror;
    uint32_t    gerrorn;
    uint32_t    strtab_base_cfg;
    uint64_t    strtab_base;
    uint32_t    irq_ctrl;
    uint32_t    virq;
    uint64_t    gerror_irq_cfg0;
    uint64_t    evtq_irq_cfg0;
    struct      arm_vsmmu_queue evtq, cmdq;
    spinlock_t  cmd_queue_lock;
};

/* Queue manipulation functions */
static bool queue_full(struct arm_vsmmu_queue *q)
{
    return Q_IDX(q, q->prod) == Q_IDX(q, q->cons) &&
           Q_WRP(q, q->prod) != Q_WRP(q, q->cons);
}

static bool queue_empty(struct arm_vsmmu_queue *q)
{
    return Q_IDX(q, q->prod) == Q_IDX(q, q->cons) &&
           Q_WRP(q, q->prod) == Q_WRP(q, q->cons);
}

static void queue_inc_cons(struct arm_vsmmu_queue *q)
{
    uint32_t cons = (Q_WRP(q, q->cons) | Q_IDX(q, q->cons)) + 1;
    q->cons = Q_OVF(q->cons) | Q_WRP(q, cons) | Q_IDX(q, cons);
}

static void queue_inc_prod(struct arm_vsmmu_queue *q)
{
    u32 prod = (Q_WRP(q, q->prod) | Q_IDX(q, q->prod)) + 1;
    q->prod = Q_OVF(q->prod) | Q_WRP(q, prod) | Q_IDX(q, prod);
}

static void dump_smmu_command(uint64_t *command)
{
    gdprintk(XENLOG_ERR, "cmd 0x%02llx: %016lx %016lx\n",
             smmu_cmd_get_command(command[0]), command[0], command[1]);
}

static void arm_vsmmu_inject_irq(struct virt_smmu *smmu, bool is_gerror,
                                uint32_t gerror_err)
{
    uint32_t new_gerrors, pending;

    if ( is_gerror )
    {
        /* trigger global error irq to guest */
        pending = smmu->gerror ^ smmu->gerrorn;
        new_gerrors = ~pending & gerror_err;

        /* only toggle non pending errors */
        if (!new_gerrors)
            return;

        smmu->gerror ^= new_gerrors;
    }

    vgic_inject_irq(smmu->d, NULL, smmu->virq, true);
}

static int arm_vsmmu_write_evtq(struct virt_smmu *smmu, uint64_t *evt)
{
    struct arm_vsmmu_queue *q = &smmu->evtq;
    struct domain *d = smmu->d;
    paddr_t addr;
    int ret;

    if ( !smmu_get_evtq_enabled(smmu->cr[0]) )
        return -EINVAL;

    if ( queue_full(q) )
        return -EINVAL;

    addr = Q_PROD_ENT(q);
    ret = access_guest_memory_by_gpa(d, addr, evt,
                                     sizeof(*evt) * EVTQ_ENT_DWORDS, true);
    if ( ret )
        return ret;

    queue_inc_prod(q);

    /* trigger eventq irq to guest */
    if ( !queue_empty(q) )
        arm_vsmmu_inject_irq(smmu, false, 0);

    return 0;
}

void arm_vsmmu_send_event(struct virt_smmu *smmu,
                          struct arm_smmu_evtq_ent *ent)
{
    uint64_t evt[EVTQ_ENT_DWORDS];
    int ret;

    memset(evt, 0, 1 << EVTQ_ENT_SZ_SHIFT);

    if ( !smmu_get_evtq_enabled(smmu->cr[0]) )
        return;

    evt[0] |= FIELD_PREP(EVTQ_0_ID, ent->opcode);
    evt[0] |= FIELD_PREP(EVTQ_0_SID, ent->sid);

    switch (ent->opcode)
    {
    case EVT_ID_BAD_STREAMID:
    case EVT_ID_BAD_STE:
        evt[0] |= FIELD_PREP(EVTQ_0_SSID, ent->c_bad_ste_streamid.ssid);
        evt[0] |= FIELD_PREP(EVTQ_0_SSV, ent->c_bad_ste_streamid.ssv);
        break;
    case EVT_ID_TRANSLATION_FAULT:
    case EVT_ID_ADDR_SIZE_FAULT:
    case EVT_ID_ACCESS_FAULT:
    case EVT_ID_PERMISSION_FAULT:
        break;
    default:
        gdprintk(XENLOG_WARNING, "vSMMUv3: event opcode is bad\n");
        break;
    }

    ret = arm_vsmmu_write_evtq(smmu, evt);
    if ( ret )
        arm_vsmmu_inject_irq(smmu, true, GERROR_EVTQ_ABT_ERR);

    return;
}

static int arm_vsmmu_find_ste(struct virt_smmu *smmu, uint32_t sid,
                              uint64_t *ste)
{
    paddr_t addr, strtab_base;
    struct domain *d = smmu->d;
    uint32_t log2size;
    int strtab_size_shift;
    int ret;
    struct arm_smmu_evtq_ent ent = {
        .sid = sid,
        .c_bad_ste_streamid = {
            .ssid = 0,
            .ssv = false,
        },
    };

    log2size = FIELD_GET(STRTAB_BASE_CFG_LOG2SIZE, smmu->strtab_base_cfg);

    if ( sid >= (1 << MIN(log2size, SMMU_IDR1_SIDSIZE)) )
    {
        ent.opcode = EVT_ID_BAD_STE;
        arm_vsmmu_send_event(smmu, &ent);
        return -EINVAL;
    }

    if ( smmu->features & STRTAB_BASE_CFG_FMT_2LVL )
    {
        int idx, max_l2_ste, span;
        paddr_t l1ptr, l2ptr;
        uint64_t l1std;

        strtab_size_shift = MAX(5, (int)log2size - smmu->sid_split - 1 + 3);
        strtab_base = smmu->strtab_base & STRTAB_BASE_ADDR_MASK &
                        ~GENMASK_ULL(strtab_size_shift, 0);
        idx = (sid >> STRTAB_SPLIT) * STRTAB_L1_DESC_DWORDS;
        l1ptr = (paddr_t)(strtab_base + idx * sizeof(l1std));

        ret = access_guest_memory_by_gpa(d, l1ptr, &l1std,
                                         sizeof(l1std), false);
        if ( ret )
        {
            gdprintk(XENLOG_ERR,
                     "Could not read L1PTR at 0X%"PRIx64"\n", l1ptr);
            return ret;
        }

        span = FIELD_GET(STRTAB_L1_DESC_SPAN, l1std);
        if ( !span )
        {
            gdprintk(XENLOG_ERR, "Bad StreamID span\n");
            return -EINVAL;
        }

        max_l2_ste = (1 << span) - 1;
        l2ptr = FIELD_PREP(STRTAB_L1_DESC_L2PTR_MASK,
                    FIELD_GET(STRTAB_L1_DESC_L2PTR_MASK, l1std));
        idx = sid & ((1 << smmu->sid_split) - 1);
        if ( idx > max_l2_ste )
        {
            gdprintk(XENLOG_ERR, "idx=%d > max_l2_ste=%d\n",
                     idx, max_l2_ste);
            ent.opcode = EVT_ID_BAD_STREAMID;
            arm_vsmmu_send_event(smmu, &ent);
            return -EINVAL;
        }
        addr = l2ptr + idx * sizeof(*ste) * STRTAB_STE_DWORDS;
    }
    else
    {
        strtab_size_shift = log2size + 5;
        strtab_base = smmu->strtab_base & STRTAB_BASE_ADDR_MASK &
                      ~GENMASK_ULL(strtab_size_shift, 0);
        addr = strtab_base + sid * sizeof(*ste) * STRTAB_STE_DWORDS;
    }
    ret = access_guest_memory_by_gpa(d, addr, ste, sizeof(*ste), false);
    if ( ret )
    {
        gdprintk(XENLOG_ERR,
                "Cannot fetch pte at address=0x%"PRIx64"\n", addr);
        return -EINVAL;
    }

    return 0;
}

static int arm_vsmmu_decode_ste(struct virt_smmu *smmu, uint32_t sid,
                                struct arm_vsmmu_s1_trans_cfg *cfg,
                                uint64_t *ste)
{
    uint64_t val = ste[0];
    struct arm_smmu_evtq_ent ent = {
        .opcode = EVT_ID_BAD_STE,
        .sid = sid,
        .c_bad_ste_streamid = {
            .ssid = 0,
            .ssv = false,
        },
    };

    if ( !(val & STRTAB_STE_0_V) )
        return -EAGAIN;

    switch ( FIELD_GET(STRTAB_STE_0_CFG, val) )
    {
    case STRTAB_STE_0_CFG_BYPASS:
        cfg->bypassed = true;
        return 0;
    case STRTAB_STE_0_CFG_ABORT:
        cfg->aborted = true;
        return 0;
    case STRTAB_STE_0_CFG_S1_TRANS:
        break;
    case STRTAB_STE_0_CFG_S2_TRANS:
        gdprintk(XENLOG_ERR, "vSMMUv3 does not support stage 2 yet\n");
        goto bad_ste;
    default:
        BUG(); /* STE corruption */
    }

    cfg->s1ctxptr = smmu_get_ste_s1ctxptr(val);
    cfg->s1fmt = smmu_get_ste_s1fmt(val);
    cfg->s1cdmax = smmu_get_ste_s1cdmax(val);
    if ( cfg->s1cdmax != 0 )
    {
        gdprintk(XENLOG_ERR,
                 "vSMMUv3 does not support multiple context descriptors\n");
        goto bad_ste;
    }

    return 0;

bad_ste:
    arm_vsmmu_send_event(smmu, &ent);
    return -EINVAL;
}

static int arm_vsmmu_handle_cfgi_ste(struct virt_smmu *smmu, uint64_t *cmdptr)
{
    int ret;
    uint64_t ste[STRTAB_STE_DWORDS];
    struct domain *d = smmu->d;
    struct domain_iommu *hd = dom_iommu(d);
    struct arm_vsmmu_s1_trans_cfg s1_cfg = {0};
    uint32_t sid = smmu_cmd_get_sid(cmdptr[0]);
    struct iommu_guest_config guest_cfg = {0};

    ret = arm_vsmmu_find_ste(smmu, sid, ste);
    if ( ret )
        return ret;

    ret = arm_vsmmu_decode_ste(smmu, sid, &s1_cfg, ste);
    if ( ret )
        return (ret == -EAGAIN ) ? 0 : ret;

    guest_cfg.s1ctxptr = s1_cfg.s1ctxptr;
    guest_cfg.s1fmt = s1_cfg.s1fmt;
    guest_cfg.s1cdmax = s1_cfg.s1cdmax;

    if ( s1_cfg.bypassed )
        guest_cfg.config = ARM_SMMU_DOMAIN_BYPASS;
    else if ( s1_cfg.aborted )
        guest_cfg.config = ARM_SMMU_DOMAIN_ABORT;
    else
        guest_cfg.config = ARM_SMMU_DOMAIN_NESTED;

    ret = hd->platform_ops->attach_guest_config(d, sid, &guest_cfg);
    if ( ret )
        return ret;

    return 0;
}

static int arm_vsmmu_handle_cmds(struct virt_smmu *smmu)
{
    struct arm_vsmmu_queue *q = &smmu->cmdq;
    struct domain *d = smmu->d;
    uint64_t command[CMDQ_ENT_DWORDS];
    paddr_t addr;

    if ( !smmu_get_cmdq_enabled(smmu->cr[0]) )
        return 0;

    while ( !queue_empty(q) )
    {
        int ret;

        addr = Q_CONS_ENT(q);
        ret = access_guest_memory_by_gpa(d, addr, command,
                                         sizeof(command), false);
        if ( ret )
            return ret;

        switch ( smmu_cmd_get_command(command[0]) )
        {
        case CMDQ_OP_CFGI_STE:
            ret = arm_vsmmu_handle_cfgi_ste(smmu, command);
            break;
        case CMDQ_OP_PREFETCH_CFG:
        case CMDQ_OP_CFGI_CD:
        case CMDQ_OP_CFGI_CD_ALL:
        case CMDQ_OP_CFGI_ALL:
        case CMDQ_OP_CMD_SYNC:
            break;
        case CMDQ_OP_TLBI_NH_ASID:
        case CMDQ_OP_TLBI_NSNH_ALL:
        case CMDQ_OP_TLBI_NH_VA:
            if ( !iommu_iotlb_flush_all(smmu->d, 1) )
                break;
        default:
            gdprintk(XENLOG_ERR, "vSMMUv3: unhandled command\n");
            dump_smmu_command(command);
            break;
        }

        if ( ret )
        {
            gdprintk(XENLOG_ERR,
                     "vSMMUv3: command error %d while handling command\n",
                     ret);
            dump_smmu_command(command);
        }
        queue_inc_cons(q);
    }
    return 0;
}

static int vsmmuv3_mmio_write(struct vcpu *v, mmio_info_t *info,
                              register_t r, void *priv)
{
    struct virt_smmu *smmu = priv;
    uint64_t reg;
    uint32_t reg32;

    switch ( info->gpa & 0xffff )
    {
    case VREG32(ARM_SMMU_CR0):
        reg32 = smmu->cr[0];
        vreg_reg32_update(&reg32, r, info);
        smmu->cr[0] = reg32;
        smmu->cr0ack = reg32 & ~CR0_RESERVED;
        break;

    case VREG32(ARM_SMMU_CR1):
        reg32 = smmu->cr[1];
        vreg_reg32_update(&reg32, r, info);
        smmu->cr[1] = reg32;
        break;

    case VREG32(ARM_SMMU_CR2):
        reg32 = smmu->cr[2];
        vreg_reg32_update(&reg32, r, info);
        smmu->cr[2] = reg32;
        break;

    case VREG64(ARM_SMMU_STRTAB_BASE):
        reg = smmu->strtab_base;
        vreg_reg64_update(&reg, r, info);
        smmu->strtab_base = reg;
        break;

    case VREG32(ARM_SMMU_STRTAB_BASE_CFG):
        reg32 = smmu->strtab_base_cfg;
        vreg_reg32_update(&reg32, r, info);
        smmu->strtab_base_cfg = reg32;

        smmu->sid_split = FIELD_GET(STRTAB_BASE_CFG_SPLIT, reg32);
        smmu->features |= STRTAB_BASE_CFG_FMT_2LVL;
        break;

    case VREG32(ARM_SMMU_CMDQ_BASE):
        reg = smmu->cmdq.q_base;
        vreg_reg64_update(&reg, r, info);
        smmu->cmdq.q_base = reg;
        smmu->cmdq.max_n_shift = FIELD_GET(Q_BASE_LOG2SIZE, smmu->cmdq.q_base);
        if ( smmu->cmdq.max_n_shift > SMMU_CMDQS )
            smmu->cmdq.max_n_shift = SMMU_CMDQS;
        break;

    case VREG32(ARM_SMMU_CMDQ_PROD):
        spin_lock(&smmu->cmd_queue_lock);
        reg32 = smmu->cmdq.prod;
        vreg_reg32_update(&reg32, r, info);
        smmu->cmdq.prod = reg32;

        if ( arm_vsmmu_handle_cmds(smmu) )
            gdprintk(XENLOG_ERR, "error handling vSMMUv3 commands\n");

        spin_unlock(&smmu->cmd_queue_lock);
        break;

    case VREG32(ARM_SMMU_CMDQ_CONS):
        reg32 = smmu->cmdq.cons;
        vreg_reg32_update(&reg32, r, info);
        smmu->cmdq.cons = reg32;
        break;

    case VREG32(ARM_SMMU_EVTQ_BASE):
        reg = smmu->evtq.q_base;
        vreg_reg64_update(&reg, r, info);
        smmu->evtq.q_base = reg;
        smmu->evtq.max_n_shift = FIELD_GET(Q_BASE_LOG2SIZE, smmu->evtq.q_base);
        if ( smmu->cmdq.max_n_shift > SMMU_EVTQS )
            smmu->cmdq.max_n_shift = SMMU_EVTQS;
        break;

    case VREG32(ARM_SMMU_EVTQ_PROD):
        reg32 = smmu->evtq.prod;
        vreg_reg32_update(&reg32, r, info);
        smmu->evtq.prod = reg32;
        break;

    case VREG32(ARM_SMMU_EVTQ_CONS):
        reg32 = smmu->evtq.cons;
        vreg_reg32_update(&reg32, r, info);
        smmu->evtq.cons = reg32;
        break;

    case VREG32(ARM_SMMU_IRQ_CTRL):
        reg32 = smmu->irq_ctrl;
        vreg_reg32_update(&reg32, r, info);
        smmu->irq_ctrl = reg32;
        break;

    case VREG64(ARM_SMMU_GERROR_IRQ_CFG0):
        reg = smmu->gerror_irq_cfg0;
        vreg_reg64_update(&reg, r, info);
        smmu->gerror_irq_cfg0 = reg;
        break;

    case VREG64(ARM_SMMU_EVTQ_IRQ_CFG0):
        reg = smmu->evtq_irq_cfg0;
        vreg_reg64_update(&reg, r, info);
        smmu->evtq_irq_cfg0 = reg;
        break;

    case VREG32(ARM_SMMU_GERRORN):
        reg = smmu->gerrorn;
        vreg_reg64_update(&reg, r, info);
        smmu->gerrorn = reg;
        break;

    default:
        printk(XENLOG_G_ERR
               "%pv: vSMMUv3: unhandled write r%d offset %"PRIpaddr"\n",
               v, info->dabt.reg, (unsigned long)info->gpa & 0xffff);
        return IO_ABORT;
    }

    return IO_HANDLED;
}

static int vsmmuv3_mmio_read(struct vcpu *v, mmio_info_t *info,
                             register_t *r, void *priv)
{
    struct virt_smmu *smmu = priv;
    uint64_t reg;

    switch ( info->gpa & 0xffff )
    {
    case VREG32(ARM_SMMU_IDR0):
        reg  = FIELD_PREP(IDR0_S1P, 1) | FIELD_PREP(IDR0_TTF, 2) |
            FIELD_PREP(IDR0_COHACC, 0) | FIELD_PREP(IDR0_ASID16, 1) |
            FIELD_PREP(IDR0_TTENDIAN, 0) | FIELD_PREP(IDR0_STALL_MODEL, 1) |
            FIELD_PREP(IDR0_ST_LVL, 1) | FIELD_PREP(IDR0_TERM_MODEL, 1);
        *r = vreg_reg32_extract(reg, info);
        break;

    case VREG32(ARM_SMMU_IDR1):
        reg  = FIELD_PREP(IDR1_SIDSIZE, SMMU_IDR1_SIDSIZE) |
            FIELD_PREP(IDR1_CMDQS, SMMU_CMDQS) |
            FIELD_PREP(IDR1_EVTQS, SMMU_EVTQS);
        *r = vreg_reg32_extract(reg, info);
        break;

    case VREG32(ARM_SMMU_IDR2):
        goto read_reserved;

    case VREG32(ARM_SMMU_IDR3):
        reg  = FIELD_PREP(IDR3_RIL, 0);
        *r = vreg_reg32_extract(reg, info);
        break;

    case VREG32(ARM_SMMU_IDR4):
        goto read_impl_defined;

    case VREG32(ARM_SMMU_IDR5):
        reg  = FIELD_PREP(IDR5_GRAN4K, 1) | FIELD_PREP(IDR5_GRAN16K, 1) |
            FIELD_PREP(IDR5_GRAN64K, 1) | FIELD_PREP(IDR5_OAS, IDR5_OAS_48_BIT);
        *r = vreg_reg32_extract(reg, info);
        break;

    case VREG32(ARM_SMMU_CR0):
        *r = vreg_reg32_extract(smmu->cr[0], info);
        break;

    case VREG32(ARM_SMMU_CR0ACK):
        *r = vreg_reg32_extract(smmu->cr0ack, info);
        break;

    case VREG32(ARM_SMMU_CR1):
        *r = vreg_reg32_extract(smmu->cr[1], info);
        break;

    case VREG32(ARM_SMMU_CR2):
        *r = vreg_reg32_extract(smmu->cr[2], info);
        break;

    case VREG32(ARM_SMMU_STRTAB_BASE):
        *r = vreg_reg64_extract(smmu->strtab_base, info);
        break;

    case VREG32(ARM_SMMU_STRTAB_BASE_CFG):
        *r = vreg_reg32_extract(smmu->strtab_base_cfg, info);
        break;

    case VREG32(ARM_SMMU_CMDQ_BASE):
        *r = vreg_reg64_extract(smmu->cmdq.q_base, info);
        break;

    case VREG32(ARM_SMMU_CMDQ_PROD):
        *r = vreg_reg32_extract(smmu->cmdq.prod, info);
        break;

    case VREG32(ARM_SMMU_CMDQ_CONS):
        *r = vreg_reg32_extract(smmu->cmdq.cons, info);
        break;

    case VREG32(ARM_SMMU_EVTQ_BASE):
        *r = vreg_reg64_extract(smmu->evtq.q_base, info);
        break;

    case VREG32(ARM_SMMU_EVTQ_PROD):
        *r = vreg_reg32_extract(smmu->evtq.prod, info);
        break;

    case VREG32(ARM_SMMU_EVTQ_CONS):
        *r = vreg_reg32_extract(smmu->evtq.cons, info);
        break;

    case VREG32(ARM_SMMU_IRQ_CTRL):
    case VREG32(ARM_SMMU_IRQ_CTRLACK):
        *r = vreg_reg32_extract(smmu->irq_ctrl, info);
        break;

    case VREG64(ARM_SMMU_GERROR_IRQ_CFG0):
        *r = vreg_reg64_extract(smmu->gerror_irq_cfg0, info);
        break;

    case VREG64(ARM_SMMU_EVTQ_IRQ_CFG0):
        *r = vreg_reg64_extract(smmu->evtq_irq_cfg0, info);
        break;

    case VREG32(ARM_SMMU_GERROR):
        *r = vreg_reg64_extract(smmu->gerror, info);
        break;

    case VREG32(ARM_SMMU_GERRORN):
        *r = vreg_reg64_extract(smmu->gerrorn, info);
        break;

    default:
        printk(XENLOG_G_ERR
               "%pv: vSMMUv3: unhandled read r%d offset %"PRIpaddr"\n",
               v, info->dabt.reg, (unsigned long)info->gpa & 0xffff);
        return IO_ABORT;
    }

    return IO_HANDLED;

 read_impl_defined:
    printk(XENLOG_G_DEBUG
           "%pv: vSMMUv3: RAZ on implementation defined register offset %"PRIpaddr"\n",
           v, info->gpa & 0xffff);
    *r = 0;
    return IO_HANDLED;

 read_reserved:
    printk(XENLOG_G_DEBUG
           "%pv: vSMMUv3: RAZ on reserved register offset %"PRIpaddr"\n",
           v, info->gpa & 0xffff);
    *r = 0;
    return IO_HANDLED;
}

static const struct mmio_handler_ops vsmmuv3_mmio_handler = {
    .read  = vsmmuv3_mmio_read,
    .write = vsmmuv3_mmio_write,
};

static int vsmmuv3_init_single(struct domain *d, paddr_t addr,
                               paddr_t size, uint32_t virq)
{
    struct virt_smmu *smmu;

    smmu = xzalloc(struct virt_smmu);
    if ( !smmu )
        return -ENOMEM;

    smmu->d = d;
    smmu->virq = virq;
    smmu->cmdq.q_base = FIELD_PREP(Q_BASE_LOG2SIZE, SMMU_CMDQS);
    smmu->cmdq.ent_size = CMDQ_ENT_DWORDS * DWORDS_BYTES;
    smmu->evtq.q_base = FIELD_PREP(Q_BASE_LOG2SIZE, SMMU_EVTQS);
    smmu->evtq.ent_size = EVTQ_ENT_DWORDS * DWORDS_BYTES;

    spin_lock_init(&smmu->cmd_queue_lock);

    register_mmio_handler(d, &vsmmuv3_mmio_handler, addr, size, smmu);

    /* Register the vIOMMU to be able to clean it up later. */
    list_add_tail(&smmu->viommu_list, &d->arch.viommu_list);

    return 0;
}

int domain_vsmmuv3_init(struct domain *d)
{
    int ret;
    INIT_LIST_HEAD(&d->arch.viommu_list);

    if ( is_hardware_domain(d) )
    {
        struct host_iommu *hw_iommu;

        list_for_each_entry(hw_iommu, &host_iommu_list, entry)
        {
            ret = vsmmuv3_init_single(d, hw_iommu->addr, hw_iommu->size,
                                      hw_iommu->irq);
            if ( ret )
                return ret;
        }
    }
    else
    {
        ret = vsmmuv3_init_single(d, GUEST_VSMMUV3_BASE, GUEST_VSMMUV3_SIZE,
                                  GUEST_VSMMU_SPI);
        if ( ret )
            return ret;
    }

    return 0;
}

int vsmmuv3_relinquish_resources(struct domain *d)
{
    struct virt_smmu *pos, *temp;

    /* Cope with unitialized vIOMMU */
    if ( list_head_is_null(&d->arch.viommu_list) )
        return 0;

    list_for_each_entry_safe(pos, temp, &d->arch.viommu_list, viommu_list )
    {
        list_del(&pos->viommu_list);
        xfree(pos);
    }

    return 0;
}

static const struct viommu_ops vsmmuv3_ops = {
    .domain_init = domain_vsmmuv3_init,
    .relinquish_resources = vsmmuv3_relinquish_resources,
};

static const struct viommu_desc vsmmuv3_desc = {
    .ops = &vsmmuv3_ops,
    .viommu_type = XEN_DOMCTL_CONFIG_VIOMMU_SMMUV3,
};

void __init vsmmuv3_set_type(void)
{
    const struct viommu_desc *desc = &vsmmuv3_desc;

    if ( !is_viommu_enabled() )
        return;

    if ( cur_viommu && (cur_viommu != desc) )
    {
        printk("WARNING: Cannot set vIOMMU, already set to a different value\n");
        return;
    }

    cur_viommu = desc;
}

/*
 * Local variables:
 * mode: C
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * End:
 */
