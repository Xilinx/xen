#ifndef __ASM_ARM_ARMV8R_SYSREGS_H__
#define __ASM_ARM_ARMV8R_SYSREGS_H__

#ifndef __ASSEMBLY__

/* Write a protection region */
#define WRITE_PROTECTION_REGION(sel, pr, prbar, prlar) ({                   \
    const pr_t *_pr = pr;                                                   \
    WRITE_CP32(sel, PRSELR_EL2);         /* Selects the region */           \
    asm volatile("dsb sy;");                                                \
    WRITE_CP32(_pr->base.bits, prbar);   /* Write PRBAR<n>_EL2 */           \
    WRITE_CP32(_pr->limit.bits, prlar);  /* Write PRLAR<n>_EL2 */           \
    asm volatile("dsb sy;");                                                \
    _pr;                                                                    \
})


/* Read a protection region */
#define READ_PROTECTION_REGION(sel, prbar, prlar) ({                        \
    pr_t _pr;                                                               \
    WRITE_CP32(sel, PRSELR_EL2);       /* Selects the region */             \
    asm volatile("dsb sy;");                                                \
    _pr.base.bits = READ_CP32(prbar);  /* Read PRBAR<n>_EL2 */              \
    _pr.limit.bits = READ_CP32(prlar); /* Read PRLAR<n>_EL2 */              \
    asm volatile("dsb sy;");                                                \
    _pr;                                                                    \
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
