/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Arm DSU-600AE L3 cache partitioning support
 */
#ifndef __ASM_ARM_DSU_CACHE_H__
#define __ASM_ARM_DSU_CACHE_H__

#ifdef CONFIG_DSU_CACHE_PARTITIONING

#define DSU_MAX_SCHEMES             8
#define DSU_WAY_GROUPS              4
#define DSU_SCHEME_XEN              0
#define DSU_SCHEME_MIN              1
#define DSU_SCHEME_MAX              7
#define DSU_SCHEME_INVALID          (-1)
#define DSU_SCHEME_MASK             0x7
#define DSU_MAX_CLUSTERS            4
#define DSU_MAX_CPUS                8

/* DSU registers (Arm TRM) */
#define CLUSTERTHREADSID_EL1        S3_0_C15_C4_0
#define CLUSTERACPSID_EL1           S3_0_C15_C4_1
#define CLUSTERSTASHSID_EL1         S3_0_C15_C4_2
#define CLUSTERPARTCR_EL1           S3_0_C15_C4_3

#ifndef __ASSEMBLY__

#include <xen/types.h>
#include <xen/smp.h>
#include <xen/spinlock.h>
#include <asm/cpregs.h>

struct domain;

extern bool dsu_enabled;

struct dsu_cluster_info
{
    /* Total L3 cache size in bytes, used for diagnostic output. */
    unsigned int l3_cache_size;
    /* Mapping from way-group index to scheme number. */
    uint8_t wg_scheme[DSU_WAY_GROUPS];
    /* Number of domains assigned to each scheme on this cluster. */
    unsigned int nr_users[DSU_MAX_SCHEMES];
};

struct dsu_info
{
    unsigned int num_clusters;
    struct dsu_cluster_info clusters[DSU_MAX_CLUSTERS];
    spinlock_t lock;
};

int dsu_init(void);
int dsu_configure_domain(struct domain *d, int8_t scheme);
void dsu_deconfigure_domain(struct domain *d);
int dsu_check_domain(struct domain *d);
void dsu_switch_dom_scheme(struct domain *d);

#else /* __ASSEMBLY__ */

/*
 * CLUSTERACPSID_EL1 is not programmed here because Xen does not
 * generate ACP transactions. If ACP support is added in the future,
 * this register must be switched as well (see dsu_switch_dom_scheme).
 */
.macro dsu_switch_xen_scheme
    adrp    x0, dsu_enabled
    ldrb    w0, [x0, :lo12:dsu_enabled]
    cbz     w0, 1f

    mrs     x0, CLUSTERTHREADSID_EL1
    and     x0, x0, #DSU_SCHEME_MASK
    cmp     x0, #DSU_SCHEME_XEN
    b.eq    1f

    mov     x0, #DSU_SCHEME_XEN
    msr     CLUSTERTHREADSID_EL1, x0
    msr     CLUSTERSTASHSID_EL1, x0
    isb
1:
.endm

#endif /* __ASSEMBLY__ */

#else /* !CONFIG_DSU_CACHE_PARTITIONING */

#ifndef __ASSEMBLY__

#define DSU_SCHEME_INVALID (-1)

struct domain;

static inline int dsu_init(void) { return 0; }
static inline int dsu_configure_domain(struct domain *d, int8_t scheme)
{
    return 0;
}
static inline void dsu_deconfigure_domain(struct domain *d) {}
static inline int dsu_check_domain(struct domain *d) { return 0; }
static inline void dsu_switch_dom_scheme(struct domain *d) {}
#endif /* __ASSEMBLY__ */

#endif /* CONFIG_DSU_CACHE_PARTITIONING */

#endif /* __ASM_ARM_DSU_CACHE_H__ */

/*
 * Local variables:
 * mode: C
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * End:
 */
