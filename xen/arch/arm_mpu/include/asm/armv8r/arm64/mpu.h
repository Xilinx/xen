// SPDX-License-Identifier: GPL-2.0-only
/*
 * mpu.h: Arm Memory Protection Unit definitions.
 */

#ifndef __ARM64_MPU_H__
#define __ARM64_MPU_H__

#define MPUIR_REGION_MASK _AC(0xFF, UL)

/*
 * 16 as default size of Arm MPU Protetcion Regions is enough
 * and necessary for initializing mpu map table in boot stage.
 */
#define ARM_DEFAULT_MPU_PROTECTION_REGIONS 16

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

static inline uint64_t generate_vsctlr(uint16_t vmid)
{
    return ((uint64_t)vmid << 48);
}

extern void set_boot_mpumap(u64 len, pr_t *table);

static inline unsigned long p2m_get_region_type(pr_t *region)
{
    return region->base.reg.p2m_type;
}

static inline void p2m_set_region_type(pr_t *region, uint64_t type)
{
    region->base.reg.p2m_type = type;
}

#endif /* __ASSEMBLY__ */

#endif /* __ARM64_MPU_H__ */
