// SPDX-License-Identifier: GPL-2.0-only
/*
 * mpu.h: Arm Memory Protection Unit definitions.
 */

#ifndef __ARM32_MPU_H__
#define __ARM32_MPU_H__

#ifndef __ASSEMBLY__

/* Hypervisor Protection Region Base Address Register */
typedef union {
    struct {
        unsigned int xn:1;       /* Execute-Never */
        unsigned int ap:2;       /* Acess Permission */
        unsigned int sh:2;       /* Sharebility */
        unsigned int res0:1;     /* Reserved as 0 */
        unsigned int base:26;    /* Base Address */
    } reg;
    uint32_t bits;
} prbar_t;

/* Hypervisor Protection Region Limit Address Register */
typedef union {
    struct {
        unsigned int en:1;     /* Region enable */
        unsigned int ai:3;     /* Memory Attribute Index */
        /*
         * There is no actual ns bit in hardware. It is used here for
         * compatibility with Armr64 code. Thus, we are reusing a res0 bit for ns.
         */
        unsigned int ns:1;     /* Reserved 0 by hardware */
        unsigned int res0:1;   /* Reserved 0 by hardware */
        unsigned int base:26;  /* Limit Address */
    } reg;
    uint32_t bits;
} prlar_t;

/* Protection Region */
typedef struct {
    prbar_t base;
    prlar_t limit;
    uint64_t p2m_type;          /* Used to store p2m types. */
} pr_t;

#define XN_P2M_ENABLED XN_ENABLED

static inline uint64_t p2m_get_region_type(pr_t *region)
{
    return region->p2m_type;
}

static inline void p2m_set_region_type(pr_t *region, uint64_t type)
{
    region->p2m_type = type;
}

static inline uint32_t generate_vsctlr(uint16_t vmid)
{
    return ((uint32_t)vmid << 16);
}

extern void set_boot_mpumap(u32 len, pr_t *table);

#endif /* __ASSEMBLY__ */
#endif /* __ARM32_MPU_H__ */
