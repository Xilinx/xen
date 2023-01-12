#ifndef __ASM_ARM_ARMV8R_SYSREGS_H__
#define __ASM_ARM_ARMV8R_SYSREGS_H__

#ifndef __ASSEMBLY__

/* Write a protection region */
#define WRITE_PROTECTION_REGION(sel, pr, prbar, prlar) ({               \
    uint64_t _sel = sel;                                                \
    const pr_t *_pr = pr;                                               \
    asm volatile(                                                       \
        "msr "__stringify(PRSELR_EL2)", %0;" /* Selects the region */   \
        "dsb sy;"                                                       \
        "msr "__stringify(prbar)", %1;" /* Write PRBAR<n>_EL2 */        \
        "msr "__stringify(prlar)", %2;" /* Write PRLAR<n>_EL2 */        \
        "dsb sy;"                                                       \
        : : "r" (_sel), "r" (_pr->base.bits), "r" (_pr->limit.bits));   \
})

/* Read a protection region */
#define READ_PROTECTION_REGION(sel, prbar, prlar) ({                    \
    uint64_t _sel = sel;                                                \
    pr_t _pr;                                                           \
    asm volatile(                                                       \
        "msr "__stringify(PRSELR_EL2)", %2;" /* Selects the region */   \
        "dsb sy;"                                                       \
        "mrs %0, "__stringify(prbar)";" /* Read PRBAR<n>_EL2 */         \
        "mrs %1, "__stringify(prlar)";" /* Read PRLAR<n>_EL2 */         \
        "dsb sy;"                                                       \
        : "=r" (_pr.base.bits), "=r" (_pr.limit.bits) : "r" (_sel));    \
    _pr;                                                                \
})

#endif /* __ASSEMBLY__ */

#endif /* _ASM_ARM_ARMV8R_SYSREGS_H__ */

/*
 * Local variables:
 * mode: C
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * End:
 */
