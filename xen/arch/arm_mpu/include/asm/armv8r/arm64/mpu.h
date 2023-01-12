// SPDX-License-Identifier: GPL-2.0-only
/*
 * mpu.h: Arm Memory Protection Unit definitions.
 */

#ifndef __ARM64_MPU_H__
#define __ARM64_MPU_H__

#define MPUIR_REGION_MASK _AC(0xFF, UL)
#define MPU_PRENR_BITS    32

/* Access permission attributes. */
/* Read/Write at EL2, No Access at EL1/EL0. */
#define AP_RW_EL2 0x0
/* Read/Write at EL2/EL1/EL0 all levels. */
#define AP_RW_ALL 0x1
/* Read-only at EL2, No Access at EL1/EL0. */
#define AP_RO_EL2 0x2
/* Read-only at EL2/EL1/EL0 all levels. */
#define AP_RO_ALL 0x3

/*
 * Excute never.
 * Stage 1 EL2 translation regime.
 * XN[1] determines whether execution of the instruction fetched from the MPU
 * memory region is permitted.
 * Stage 2 EL1/EL0 translation regime.
 * XN[0] determines whether execution of the instruction fetched from the MPU
 * memory region is permitted.
 */
#define XN_DISABLED    0x0
#define XN_P2M_ENABLED 0x1
#define XN_ENABLED     0x2

/* For Armv8-R, the MPU protection regions can reach to 256. */
#define MAX_MPU_PROTECTION_REGIONS 256

/*
 * 16 as default size of Arm MPU Protetcion Regions is enough
 * and necessary for initializing mpu map table in boot stage.
 */
#define ARM_DEFAULT_MPU_PROTECTION_REGIONS 16

#define _REGION_XN_BIT      0
#define _REGION_RO_BIT      1
#define _REGION_XN          (1U << _REGION_XN_BIT)
#define _REGION_RO          (1U << _REGION_RO)
#define REGION_XN_MASK(x)   (((x) >> _REGION_XN_BIT) & 0x1U)
#define REGION_RO_MASK(x)   (((x) >> _REGION_RO_BIT) & 0x1U)

#define REGION_HYPERVISOR_RW    _REGION_XN

#ifndef __ASSEMBLY__

/* Protection Region Base Address Register */
typedef union {
    struct __packed {
        unsigned long xn:2;       /* Execute-Never */
        unsigned long ap:2;       /* Acess Permission */
        unsigned long sh:2;       /* Sharebility */
        unsigned long base:42;    /* Base Address */
        unsigned long pad:12;
        unsigned long p2m_type:4; /*
                                   * Ignore by hardware.
                                   * Used to store p2m types.
                                   */
    } reg;
    uint64_t bits;
} prbar_t;

/* Protection Region Limit Address Register */
typedef union {
    struct __packed {
        unsigned long en:1;     /* Region enable */
        unsigned long ai:3;     /* Memory Attribute Index */
        unsigned long ns:1;     /* Not-Secure */
        unsigned long res:1;    /* Reserved 0 by hardware */
        unsigned long base:42;  /* Limit Address */
        unsigned long pad:16;
    } reg;
    uint64_t bits;
} prlar_t;

/* Protection Region */
typedef struct {
    prbar_t base;
    prlar_t limit;
} pr_t;

/* Access to set base address of MPU protection region(pr_t). */
#define pr_set_base(pr, paddr) ({                           \
    pr_t* _pr = pr;                                         \
    _pr->base.reg.base = (paddr >> MPU_REGION_SHIFT);       \
})

/* Access to set limit address of MPU protection region(pr_t). */
#define pr_set_limit(pr, paddr) ({                          \
    pr_t* _pr = pr;                                         \
    _pr->limit.reg.base = (paddr >> MPU_REGION_SHIFT);      \
})

#define IS_PR_ENABLED(pr) ({                                \
    pr_t* _pr = pr;                                         \
    _pr->limit.reg.en;                                      \
})

/*
 * Access to get base address of MPU protection region.
 * The base address shall be zero extended.
 */
#define pr_get_base(pr) ({                                  \
    pr_t* _pr = pr;                                         \
    (uint64_t)_pr->base.reg.base << MPU_REGION_SHIFT;       \
})

/*
 * Access to get limit address of MPU protection region.
 * The limit address shall be concatenated with 0x3f.
 */
#define pr_get_limit(pr) ({                                       \
    pr_t* _pr = pr;                                               \
    (uint64_t)((_pr->limit.reg.base << MPU_REGION_SHIFT) | 0x3f); \
})

static inline bool region_is_valid(pr_t *region)
{
    return region->limit.reg.en;
}

#endif /* __ASSEMBLY__ */

#endif /* __ARM64_MPU_H__ */
