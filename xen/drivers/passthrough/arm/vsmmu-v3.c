/* SPDX-License-Identifier: (GPL-2.0-or-later OR BSD-2-Clause) */

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
    uint64_t    gerror_irq_cfg0;
    uint64_t    evtq_irq_cfg0;
    struct      arm_vsmmu_queue evtq, cmdq;
};

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
        reg32 = smmu->cmdq.prod;
        vreg_reg32_update(&reg32, r, info);
        smmu->cmdq.prod = reg32;
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

static int vsmmuv3_init_single(struct domain *d, paddr_t addr, paddr_t size)
{
    struct virt_smmu *smmu;

    smmu = xzalloc(struct virt_smmu);
    if ( !smmu )
        return -ENOMEM;

    smmu->d = d;
    smmu->cmdq.q_base = FIELD_PREP(Q_BASE_LOG2SIZE, SMMU_CMDQS);
    smmu->cmdq.ent_size = CMDQ_ENT_DWORDS * DWORDS_BYTES;
    smmu->evtq.q_base = FIELD_PREP(Q_BASE_LOG2SIZE, SMMU_EVTQS);
    smmu->evtq.ent_size = EVTQ_ENT_DWORDS * DWORDS_BYTES;

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
            ret = vsmmuv3_init_single(d, hw_iommu->addr, hw_iommu->size);
            if ( ret )
                return ret;
        }
    }
    else
    {
        ret = vsmmuv3_init_single(d, GUEST_VSMMUV3_BASE, GUEST_VSMMUV3_SIZE);
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
