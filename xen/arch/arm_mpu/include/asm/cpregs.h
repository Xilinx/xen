#ifndef __ASM_ARM_CPREGS_H
#define __ASM_ARM_CPREGS_H

/*
 * AArch32 Co-processor registers.
 *
 * Note that AArch64 requires many of these definitions in order to
 * support 32-bit guests.
 */

#define __HSR_CPREG_c0  0
#define __HSR_CPREG_c1  1
#define __HSR_CPREG_c2  2
#define __HSR_CPREG_c3  3
#define __HSR_CPREG_c4  4
#define __HSR_CPREG_c5  5
#define __HSR_CPREG_c6  6
#define __HSR_CPREG_c7  7
#define __HSR_CPREG_c8  8
#define __HSR_CPREG_c9  9
#define __HSR_CPREG_c10 10
#define __HSR_CPREG_c11 11
#define __HSR_CPREG_c12 12
#define __HSR_CPREG_c13 13
#define __HSR_CPREG_c14 14
#define __HSR_CPREG_c15 15

#define __HSR_CPREG_0   0
#define __HSR_CPREG_1   1
#define __HSR_CPREG_2   2
#define __HSR_CPREG_3   3
#define __HSR_CPREG_4   4
#define __HSR_CPREG_5   5
#define __HSR_CPREG_6   6
#define __HSR_CPREG_7   7

#define _HSR_CPREG32(cp,op1,crn,crm,op2) \
    ((__HSR_CPREG_##crn) << HSR_CP32_CRN_SHIFT) | \
    ((__HSR_CPREG_##crm) << HSR_CP32_CRM_SHIFT) | \
    ((__HSR_CPREG_##op1) << HSR_CP32_OP1_SHIFT) | \
    ((__HSR_CPREG_##op2) << HSR_CP32_OP2_SHIFT)

#define _HSR_CPREG64(cp,op1,crm) \
    ((__HSR_CPREG_##crm) << HSR_CP64_CRM_SHIFT) | \
    ((__HSR_CPREG_##op1) << HSR_CP64_OP1_SHIFT)

/* Encode a register as per HSR ISS pattern */
#define HSR_CPREG32(X...) _HSR_CPREG32(X)
#define HSR_CPREG64(X...) _HSR_CPREG64(X)

/*
 * Order registers by Coprocessor-> CRn-> Opcode 1-> CRm-> Opcode 2
 *
 * This matches the ordering used in the ARM as well as the groupings
 * which the CP registers are allocated in.
 *
 * This is slightly different to the form of the instruction
 * arguments, which are cp,opc1,crn,crm,opc2.
 */

/* Coprocessor 10 */

#define FPSID           p10,7,c0,c0,0   /* Floating-Point System ID Register */
#define FPSCR           p10,7,c1,c0,0   /* Floating-Point Status and Control Register */
#define MVFR0           p10,7,c7,c0,0   /* Media and VFP Feature Register 0 */
#define MVFR1           p10,7,c6,c0,0   /* Media and VFP Feature Register 1 */
#define MVFR2           p10,7,c5,c0,0   /* Media and VFP Feature Register 2 */
#define FPEXC           p10,7,c8,c0,0   /* Floating-Point Exception Control Register */
#define FPINST          p10,7,c9,c0,0   /* Floating-Point Instruction Register */
#define FPINST2         p10,7,c10,c0,0  /* Floating-point Instruction Register 2 */

/* Coprocessor 14 */

/* CP14 0: Debug Register interface */
#define DBGDIDR         p14,0,c0,c0,0   /* Debug ID Register */
#define DBGDSCRINT      p14,0,c0,c1,0   /* Debug Status and Control Internal */
#define DBGDSCREXT      p14,0,c0,c2,2   /* Debug Status and Control External */
#define DBGVCR          p14,0,c0,c7,0   /* Vector Catch */
#define DBGBVR0         p14,0,c0,c0,4   /* Breakpoint Value 0 */
#define DBGBCR0         p14,0,c0,c0,5   /* Breakpoint Control 0 */
#define DBGWVR0         p14,0,c0,c0,6   /* Watchpoint Value 0 */
#define DBGWCR0         p14,0,c0,c0,7   /* Watchpoint Control 0 */
#define DBGBVR1         p14,0,c0,c1,4   /* Breakpoint Value 1 */
#define DBGBCR1         p14,0,c0,c1,5   /* Breakpoint Control 1 */
#define DBGOSLAR        p14,0,c1,c0,4   /* OS Lock Access */
#define DBGOSLSR        p14,0,c1,c1,4   /* OS Lock Status Register */
#define DBGOSDLR        p14,0,c1,c3,4   /* OS Double Lock */
#define DBGPRCR         p14,0,c1,c4,4   /* Debug Power Control Register */

/* CP14 CR0: */
#define TEECR           p14,6,c0,c0,0   /* ThumbEE Configuration Register */

/* CP14 CR1: */
#define DBGDRAR64       p14,0,c1        /* Debug ROM Address Register (64-bit access) */
#define DBGDRAR         p14,0,c1,c0,0   /* Debug ROM Address Register (32-bit access) */
#define TEEHBR          p14,6,c1,c0,0   /* ThumbEE Handler Base Register */
#define JOSCR           p14,7,c1,c0,0   /* Jazelle OS Control Register */

/* CP14 CR2: */
#define DBGDSAR64       p14,0,c2        /* Debug Self Address Offset Register (64-bit access) */
#define DBGDSAR         p14,0,c2,c0,0   /* Debug Self Address Offset Register (32-bit access) */
#define JMCR            p14,7,c2,c0,0   /* Jazelle Main Configuration Register */


/* Coprocessor 15 */

/* CP15 CR0: CPUID and Cache Type Registers */
#define MIDR            p15,0,c0,c0,0   /* Main ID Register */
#define CTR             p15,0,c0,c0,1   /* Cache Type Register */
#define MPUIR           p15,0,c0,c0,4   /* number of regions supported by the EL1 MPU */
#define MPIDR           p15,0,c0,c0,5   /* Multiprocessor Affinity Register */
#define ID_PFR0         p15,0,c0,c1,0   /* Processor Feature Register 0 */
#define ID_PFR1         p15,0,c0,c1,1   /* Processor Feature Register 1 */
#define ID_PFR2         p15,0,c0,c3,4   /* Processor Feature Register 2 */
#define ID_DFR0         p15,0,c0,c1,2   /* Debug Feature Register 0 */
#define ID_DFR1         p15,0,c0,c3,5   /* Debug Feature Register 1 */
#define ID_AFR0         p15,0,c0,c1,3   /* Auxiliary Feature Register 0 */
#define ID_MMFR0        p15,0,c0,c1,4   /* Memory Model Feature Register 0 */
#define ID_MMFR1        p15,0,c0,c1,5   /* Memory Model Feature Register 1 */
#define ID_MMFR2        p15,0,c0,c1,6   /* Memory Model Feature Register 2 */
#define ID_MMFR3        p15,0,c0,c1,7   /* Memory Model Feature Register 3 */
#define ID_MMFR4        p15,0,c0,c2,6   /* Memory Model Feature Register 4 */
#define ID_MMFR5        p15,0,c0,c3,6   /* Memory Model Feature Register 5 */
#define ID_ISAR0        p15,0,c0,c2,0   /* ISA Feature Register 0 */
#define ID_ISAR1        p15,0,c0,c2,1   /* ISA Feature Register 1 */
#define ID_ISAR2        p15,0,c0,c2,2   /* ISA Feature Register 2 */
#define ID_ISAR3        p15,0,c0,c2,3   /* ISA Feature Register 3 */
#define ID_ISAR4        p15,0,c0,c2,4   /* ISA Feature Register 4 */
#define ID_ISAR5        p15,0,c0,c2,5   /* ISA Feature Register 5 */
#define ID_ISAR6        p15,0,c0,c2,7   /* ISA Feature Register 6 */
#define CCSIDR          p15,1,c0,c0,0   /* Cache Size ID Registers */
#define CLIDR           p15,1,c0,c0,1   /* Cache Level ID Register */
#define CSSELR          p15,2,c0,c0,0   /* Cache Size Selection Register */
#define VPIDR           p15,4,c0,c0,0   /* Virtualization Processor ID Register */
#define HMPUIR          p15,4,c0,c0,4   /* Hypervisor MPU Type Register */
#define VMPIDR          p15,4,c0,c0,5   /* Virtualization Multiprocessor ID Register */

/* CP15 CR1: System Control Registers */
#define SCTLR           p15,0,c1,c0,0   /* System Control Register */
#define ACTLR           p15,0,c1,c0,1   /* Auxiliary Control Register */
#define CPACR           p15,0,c1,c0,2   /* Coprocessor Access Control Register */
#define SCR             p15,0,c1,c1,0   /* Secure Configuration Register */
#define NSACR           p15,0,c1,c1,2   /* Non-Secure Access Control Register */
#define HSCTLR          p15,4,c1,c0,0   /* Hyp. System Control Register */
#define HCR             p15,4,c1,c1,0   /* Hyp. Configuration Register */
#define HDCR            p15,4,c1,c1,1   /* Hyp. Debug Configuration Register */
#define HCPTR           p15,4,c1,c1,2   /* Hyp. Coprocessor Trap Register */
#define HSTR            p15,4,c1,c1,3   /* Hyp. System Trap Register */

/* CP15 CR2: Translation Table Base and Control Registers */
#define TTBCR           p15,0,c2,c0,2   /* Translation Table Base Control Register */
#define TTBCR2          p15,0,c2,c0,3   /* Translation Table Base Control Register 2 */
#define TTBR0           p15,0,c2        /* Translation Table Base Reg. 0 */
#define TTBR1           p15,1,c2        /* Translation Table Base Reg. 1 */
#define HTTBR           p15,4,c2        /* Hyp. Translation Table Base Register */
#define TTBR0_32        p15,0,c2,c0,0   /* 32-bit access to TTBR0 */
#define TTBR1_32        p15,0,c2,c0,1   /* 32-bit access to TTBR1 */
#define VSCTLR          p15,4,c2,c0,0   /* Virtualization System Control Register */
#define HTCR            p15,4,c2,c0,2   /* Hyp. Translation Control Register */
#define VTCR            p15,4,c2,c1,2   /* Virtualization Translation Control Register */
#define VTTBR           p15,6,c2        /* Virtualization Translation Table Base Register */

/* CP15 CR3: Domain Access Control Register */
#define DACR            p15,0,c3,c0,0   /* Domain Access Control Register */

/* CP15 CR4: */
#define ICC_PMR         p15,0,c4,c6,0   /* Interrupt Priority Mask Register */

/* CP15 CR5: Fault Status Registers */
#define DFSR            p15,0,c5,c0,0   /* Data Fault Status Register */
#define IFSR            p15,0,c5,c0,1   /* Instruction Fault Status Register */
#define ADFSR           p15,0,c5,c1,0   /* Auxiliary Data Fault Status Register */
#define AIFSR           p15,0,c5,c1,1   /* Auxiliary Instruction Fault Status Register */
#define HSR             p15,4,c5,c2,0   /* Hyp. Syndrome Register */

/* CP15 CR6: Fault Address Registers */
#define DFAR            p15,0,c6,c0,0   /* Data Fault Address Register  */
#define IFAR            p15,0,c6,c0,2   /* Instruction Fault Address Register */
#define PRSELR          p15,0,c6,c2,1   /* MPU Protection Region Selection Register encode */
/* EL1 MPU Protection Region Base/Limit Address Register encode */
#define PRBAR           p15,0,c6,c8,0
#define PRLAR           p15,0,c6,c8,1
#define PRBAR1          p15,0,c6,c8,4
#define PRLAR1          p15,0,c6,c8,5
#define PRBAR2          p15,0,c6,c9,0
#define PRLAR2          p15,0,c6,c9,1
#define PRBAR3          p15,0,c6,c9,4
#define PRLAR3          p15,0,c6,c9,5
#define PRBAR4          p15,0,c6,c10,0
#define PRLAR4          p15,0,c6,c10,1
#define PRBAR5          p15,0,c6,c10,4
#define PRLAR5          p15,0,c6,c10,5
#define PRBAR6          p15,0,c6,c11,0
#define PRLAR6          p15,0,c6,c11,1
#define PRBAR7          p15,0,c6,c11,4
#define PRLAR7          p15,0,c6,c11,5
#define PRBAR8          p15,0,c6,c12,0
#define PRLAR8          p15,0,c6,c12,1
#define PRBAR9          p15,0,c6,c12,4
#define PRLAR9          p15,0,c6,c12,5
#define PRBAR10         p15,0,c6,c13,0
#define PRLAR10         p15,0,c6,c13,1
#define PRBAR11         p15,0,c6,c13,4
#define PRLAR11         p15,0,c6,c13,5
#define PRBAR12         p15,0,c6,c14,0
#define PRLAR12         p15,0,c6,c14,1
#define PRBAR13         p15,0,c6,c14,4
#define PRLAR13         p15,0,c6,c14,5
#define PRBAR14         p15,0,c6,c15,0
#define PRLAR14         p15,0,c6,c15,1
#define PRBAR15         p15,0,c6,c15,4
#define PRLAR15         p15,0,c6,c15,5
#define PRBAR16         p15,1,c6,c8,0
#define PRLAR16         p15,1,c6,c8,1
#define PRBAR17         p15,1,c6,c8,4
#define PRLAR17         p15,1,c6,c8,5
#define PRBAR18         p15,1,c6,c9,0
#define PRLAR18         p15,1,c6,c9,1
#define PRBAR19         p15,1,c6,c9,4
#define PRLAR19         p15,1,c6,c9,5
#define PRBAR20         p15,1,c6,c10,0
#define PRLAR20         p15,1,c6,c10,1
#define PRBAR21         p15,1,c6,c10,4
#define PRLAR21         p15,1,c6,c10,5
#define PRBAR22         p15,1,c6,c11,0
#define PRLAR22         p15,1,c6,c11,1
#define PRBAR23         p15,1,c6,c11,4
#define PRLAR23         p15,1,c6,c11,5
#define PRBAR24         p15,1,c6,c12,0
#define PRLAR24         p15,1,c6,c12,1
#define PRBAR25         p15,1,c6,c12,4
#define PRLAR25         p15,1,c6,c12,5
#define PRBAR26         p15,1,c6,c13,0
#define PRLAR26         p15,1,c6,c13,1
#define PRBAR27         p15,1,c6,c13,4
#define PRLAR27         p15,1,c6,c13,5
#define PRBAR28         p15,1,c6,c14,0
#define PRLAR28         p15,1,c6,c14,1
#define PRBAR29         p15,1,c6,c14,4
#define PRLAR29         p15,1,c6,c14,5
#define PRBAR30         p15,1,c6,c15,0
#define PRLAR30         p15,1,c6,c15,1
#define PRBAR31         p15,1,c6,c15,4
#define PRLAR31         p15,1,c6,c15,5
#define HDFAR           p15,4,c6,c0,0   /* Hyp. Data Fault Address Register */
#define HIFAR           p15,4,c6,c0,2   /* Hyp. Instruction Fault Address Register */
#define HPFAR           p15,4,c6,c0,4   /* Hyp. IPA Fault Address Register */

/* CP15 CR6: Protection Region Enable Register */
#define HPRENR          p15,4,c6,c1,1   /* MPU Protection Region Enable Register */

/* CP15 CR6: MPU Protection Region Base/Limit/Select Address Register */
#define HPRSELR         p15,4,c6,c2,1

/* CP15 CR6: MPU Protection Region Base/Limit Address Register */
#define HPRBAR0         p15,4,c6,c8,0
#define HPRLAR0         p15,4,c6,c8,1
#define HPRBAR1         p15,4,c6,c8,4
#define HPRLAR1         p15,4,c6,c8,5
#define HPRBAR2         p15,4,c6,c9,0
#define HPRLAR2         p15,4,c6,c9,1
#define HPRBAR3         p15,4,c6,c9,4
#define HPRLAR3         p15,4,c6,c9,5
#define HPRBAR4         p15,4,c6,c10,0
#define HPRLAR4         p15,4,c6,c10,1
#define HPRBAR5         p15,4,c6,c10,4
#define HPRLAR5         p15,4,c6,c10,5
#define HPRBAR6         p15,4,c6,c11,0
#define HPRLAR6         p15,4,c6,c11,1
#define HPRBAR7         p15,4,c6,c11,4
#define HPRLAR7         p15,4,c6,c11,5
#define HPRBAR8         p15,4,c6,c12,0
#define HPRLAR8         p15,4,c6,c12,1
#define HPRBAR9         p15,4,c6,c12,4
#define HPRLAR9         p15,4,c6,c12,5
#define HPRBAR10        p15,4,c6,c13,0
#define HPRLAR10        p15,4,c6,c13,1
#define HPRBAR11        p15,4,c6,c13,4
#define HPRLAR11        p15,4,c6,c13,5
#define HPRBAR12        p15,4,c6,c14,0
#define HPRLAR12        p15,4,c6,c14,1
#define HPRBAR13        p15,4,c6,c14,4
#define HPRLAR13        p15,4,c6,c14,5
#define HPRBAR14        p15,4,c6,c15,0
#define HPRLAR14        p15,4,c6,c15,1
#define HPRBAR15        p15,4,c6,c15,4
#define HPRLAR15        p15,4,c6,c15,5
#define HPRBAR16        p15,5,c6,c8,0
#define HPRLAR16        p15,5,c6,c8,1
#define HPRBAR17        p15,5,c6,c8,4
#define HPRLAR17        p15,5,c6,c8,5
#define HPRBAR18        p15,5,c6,c9,0
#define HPRLAR18        p15,5,c6,c9,1
#define HPRBAR19        p15,5,c6,c9,4
#define HPRLAR19        p15,5,c6,c9,5
#define HPRBAR20        p15,5,c6,c10,0
#define HPRLAR20        p15,5,c6,c10,1
#define HPRBAR21        p15,5,c6,c10,4
#define HPRLAR21        p15,5,c6,c10,5
#define HPRBAR22        p15,5,c6,c11,0
#define HPRLAR22        p15,5,c6,c11,1
#define HPRBAR23        p15,5,c6,c11,4
#define HPRLAR23        p15,5,c6,c11,5
#define HPRBAR24        p15,5,c6,c12,0
#define HPRLAR24        p15,5,c6,c12,1
#define HPRBAR25        p15,5,c6,c12,4
#define HPRLAR25        p15,5,c6,c12,5
#define HPRBAR26        p15,5,c6,c13,0
#define HPRLAR26        p15,5,c6,c13,1
#define HPRBAR27        p15,5,c6,c13,4
#define HPRLAR27        p15,5,c6,c13,5
#define HPRBAR28        p15,5,c6,c14,0
#define HPRLAR28        p15,5,c6,c14,1
#define HPRBAR29        p15,5,c6,c14,4
#define HPRLAR29        p15,5,c6,c14,5
#define HPRBAR30        p15,5,c6,c15,0
#define HPRLAR30        p15,5,c6,c15,1
#define HPRBAR31        p15,5,c6,c15,4
#define HPRLAR31        p15,5,c6,c15,5

/* CP15 CR7: Cache and address translation operations */
#define PAR             p15,0,c7        /* Physical Address Register */

#define ICIALLUIS       p15,0,c7,c1,0   /* Invalidate all instruction caches to PoU inner shareable */
#define BPIALLIS        p15,0,c7,c1,6   /* Invalidate entire branch predictor array inner shareable */
#define ICIALLU         p15,0,c7,c5,0   /* Invalidate all instruction caches to PoU */
#define ICIMVAU         p15,0,c7,c5,1   /* Invalidate instruction caches by MVA to PoU */
#define BPIALL          p15,0,c7,c5,6   /* Invalidate entire branch predictor array */
#define BPIMVA          p15,0,c7,c5,7   /* Invalidate MVA from branch predictor array */
#define DCIMVAC         p15,0,c7,c6,1   /* Invalidate data cache line by MVA to PoC */
#define DCISW           p15,0,c7,c6,2   /* Invalidate data cache line by set/way */
#define ATS1CPR         p15,0,c7,c8,0   /* Address Translation Stage 1. Non-Secure Kernel Read */
#define ATS1CPW         p15,0,c7,c8,1   /* Address Translation Stage 1. Non-Secure Kernel Write */
#define ATS1CUR         p15,0,c7,c8,2   /* Address Translation Stage 1. Non-Secure User Read */
#define ATS1CUW         p15,0,c7,c8,3   /* Address Translation Stage 1. Non-Secure User Write */
#define ATS12NSOPR      p15,0,c7,c8,4   /* Address Translation Stage 1+2 Non-Secure Kernel Read */
#define ATS12NSOPW      p15,0,c7,c8,5   /* Address Translation Stage 1+2 Non-Secure Kernel Write */
#define ATS12NSOUR      p15,0,c7,c8,6   /* Address Translation Stage 1+2 Non-Secure User Read */
#define ATS12NSOUW      p15,0,c7,c8,7   /* Address Translation Stage 1+2 Non-Secure User Write */
#define DCCMVAC         p15,0,c7,c10,1  /* Clean data or unified cache line by MVA to PoC */
#define DCCSW           p15,0,c7,c10,2  /* Clean data cache line by set/way */
#define DCCMVAU         p15,0,c7,c11,1  /* Clean data cache line by MVA to PoU */
#define DCCIMVAC        p15,0,c7,c14,1  /* Data cache clean and invalidate by MVA */
#define DCCISW          p15,0,c7,c14,2  /* Clean and invalidate data cache line by set/way */
#define ATS1HR          p15,4,c7,c8,0   /* Address Translation Stage 1 Hyp. Read */
#define ATS1HW          p15,4,c7,c8,1   /* Address Translation Stage 1 Hyp. Write */

/* CP15 CR8: TLB maintenance operations */
#define TLBIALLIS       p15,0,c8,c3,0   /* Invalidate entire TLB innrer shareable */
#define TLBIMVAIS       p15,0,c8,c3,1   /* Invalidate unified TLB entry by MVA inner shareable */
#define TLBIASIDIS      p15,0,c8,c3,2   /* Invalidate unified TLB by ASID match inner shareable */
#define TLBIMVAAIS      p15,0,c8,c3,3   /* Invalidate unified TLB entry by MVA all ASID inner shareable */
#define ITLBIALL        p15,0,c8,c5,0   /* Invalidate instruction TLB */
#define ITLBIMVA        p15,0,c8,c5,1   /* Invalidate instruction TLB entry by MVA */
#define ITLBIASID       p15,0,c8,c5,2   /* Invalidate instruction TLB by ASID match */
#define DTLBIALL        p15,0,c8,c6,0   /* Invalidate data TLB */
#define DTLBIMVA        p15,0,c8,c6,1   /* Invalidate data TLB entry by MVA */
#define DTLBIASID       p15,0,c8,c6,2   /* Invalidate data TLB by ASID match */
#define TLBIALL         p15,0,c8,c7,0   /* invalidate unified TLB */
#define TLBIMVA         p15,0,c8,c7,1   /* invalidate unified TLB entry by MVA */
#define TLBIASID        p15,0,c8,c7,2   /* invalid unified TLB by ASID match */
#define TLBIMVAA        p15,0,c8,c7,3   /* invalidate unified TLB entries by MVA all ASID */
#define TLBIALLHIS      p15,4,c8,c3,0   /* Invalidate Entire Hyp. Unified TLB inner shareable */
#define TLBIMVAHIS      p15,4,c8,c3,1   /* Invalidate Unified Hyp. TLB by MVA inner shareable */
#define TLBIALLNSNHIS   p15,4,c8,c3,4   /* Invalidate Entire Non-Secure Non-Hyp. Unified TLB inner shareable */
#define TLBIALLH        p15,4,c8,c7,0   /* Invalidate Entire Hyp. Unified TLB */
#define TLBIMVAH        p15,4,c8,c7,1   /* Invalidate Unified Hyp. TLB by MVA */
#define TLBIALLNSNH     p15,4,c8,c7,4   /* Invalidate Entire Non-Secure Non-Hyp. Unified TLB */

/* CP15 CR9: Performance monitors */
#define PMCR            p15,0,c9,c12,0  /* Perf. Mon. Control Register */
#define PMCNTENSET      p15,0,c9,c12,1  /* Perf. Mon. Count Enable Set register */
#define PMCNTENCLR      p15,0,c9,c12,2  /* Perf. Mon. Count Enable Clear register */
#define PMOVSR          p15,0,c9,c12,3  /* Perf. Mon. Overflow Flag Status Register */
#define PMSWINC         p15,0,c9,c12,4  /* Perf. Mon. Software Increment register */
#define PMSELR          p15,0,c9,c12,5  /* Perf. Mon. Event Counter Selection Register */
#define PMCEID0         p15,0,c9,c12,6  /* Perf. Mon. Common Event Identification register 0 */
#define PMCEID1         p15,0,c9,c12,7  /* Perf. Mon. Common Event Identification register 1 */
#define PMCCNTR         p15,0,c9,c13,0  /* Perf. Mon. Cycle Count Register */
#define PMXEVTYPER      p15,0,c9,c13,1  /* Perf. Mon. Event Type Select Register */
#define PMXEVCNTR       p15,0,c9,c13,2  /* Perf. Mon. Event Count Register */
#define PMUSERENR       p15,0,c9,c14,0  /* Perf. Mon. User Enable Register */
#define PMINTENSET      p15,0,c9,c14,1  /* Perf. Mon. Interrupt Enable Set Register */
#define PMINTENCLR      p15,0,c9,c14,2  /* Perf. Mon. Interrupt Enable Clear Register */
#define PMOVSSET        p15,0,c9,c14,3  /* Perf. Mon. Overflow Flag Status Set register */

/* CP15 CR10: */
#define MAIR0           p15,0,c10,c2,0  /* Memory Attribute Indirection Register 0 AKA PRRR */
#define MAIR1           p15,0,c10,c2,1  /* Memory Attribute Indirection Register 1 AKA NMRR */
#define HMAIR0          p15,4,c10,c2,0  /* Hyp. Memory Attribute Indirection Register 0 */
#define HMAIR1          p15,4,c10,c2,1  /* Hyp. Memory Attribute Indirection Register 1 */
#define AMAIR0          p15,0,c10,c3,0  /* Aux. Memory Attribute Indirection Register 0 */
#define AMAIR1          p15,0,c10,c3,1  /* Aux. Memory Attribute Indirection Register 1 */

/* CP15 CR11: DMA Operations for TCM Access */

/* CP15 CR12:  */
#define ICC_SGI1R       p15,0,c12       /* Interrupt Controller SGI Group 1 */
#define ICC_ASGI1R      p15,1,c12       /* Interrupt Controller Alias SGI Group 1 Register */
#define ICC_SGI0R       p15,2,c12       /* Interrupt Controller SGI Group 0 */
#define VBAR            p15,0,c12,c0,0  /* Vector Base Address Register */
#define ICC_DIR         p15,0,c12,c11,1 /* Interrupt Controller Deactivate Interrupt Register */
#define HVBAR           p15,4,c12,c0,0  /* Hyp. Vector Base Address Register */

/*
 * CP15 CR12: Interrupt Controller Hyp Active Priorities Group 0 Registers,
 * n = 0 - 3
 */
#define __AP0Rx(x)      p15, 4, c12, c8, x
#define ICH_AP0R0       __AP0Rx(0)
#define ICH_AP0R1       __AP0Rx(1)
#define ICH_AP0R2       __AP0Rx(2)
#define ICH_AP0R3       __AP0Rx(3)

/*
 * CP15 CR12: Interrupt Controller Hyp Active Priorities Group 1 Registers,
 * n = 0 - 3
 */
#define __AP1Rx(x)      p15, 4, c12, c9, x
#define ICH_AP1R0       __AP1Rx(0)
#define ICH_AP1R1       __AP1Rx(1)
#define ICH_AP1R2       __AP1Rx(2)
#define ICH_AP1R3       __AP1Rx(3)

#define ICC_IAR1        p15,0,c12,c12,0  /* Interrupt Controller Interrupt Acknowledge Register 1 */
#define ICC_EOIR1       p15,0,c12,c12,1  /* Interrupt Controller End Of Interrupt Register 1 */
#define ICC_BPR1        p15,0,c12,c12,3  /* Interrupt Controller Binary Point Register 1 */
#define ICC_CTLR        p15,0,c12,c12,4  /* Interrupt Controller Control Register */
#define ICC_SRE         p15,0,c12,c12,5  /* Interrupt Controller System Register Enable register */
#define ICC_IGRPEN1     p15,0,c12,c12,7  /* Interrupt Controller Interrupt Group 1 Enable register */
#define ICC_HSRE        p15,4,c12,c9,5   /* Interrupt Controller Hyp System Register Enable register */
#define ICH_HCR         p15,4,c12,c11,0  /* Interrupt Controller Hyp Control Register */
#define ICH_VTR         p15,4,c12,c11,1  /* Interrupt Controller VGIC Type Register */
#define ICH_MISR        p15,4,c12,c11,2  /* Interrupt Controller Maintenance Interrupt State Register */
#define ICH_EISR        p15,4,c12,c11,3  /* Interrupt Controller End of Interrupt Status Register */
#define ICH_ELRSR       p15,4,c12,c11,5  /* Interrupt Controller Empty List Register Status Register */
#define ICH_VMCR        p15,4,c12,c11,7  /* Interrupt Controller Virtual Machine Control Register */

/* CP15 CR12: Interrupt Controller List Registers, n = 0 - 15 */
#define __LR0(x)        p15, 4, c12, c12, x
#define __LR8(x)        p15, 4, c12, c13, x

#define ICH_LR0         __LR0(0)
#define ICH_LR1         __LR0(1)
#define ICH_LR2         __LR0(2)
#define ICH_LR3         __LR0(3)
#define ICH_LR4         __LR0(4)
#define ICH_LR5         __LR0(5)
#define ICH_LR6         __LR0(6)
#define ICH_LR7         __LR0(7)
#define ICH_LR8         __LR8(0)
#define ICH_LR9         __LR8(1)
#define ICH_LR10        __LR8(2)
#define ICH_LR11        __LR8(3)
#define ICH_LR12        __LR8(4)
#define ICH_LR13        __LR8(5)
#define ICH_LR14        __LR8(6)
#define ICH_LR15        __LR8(7)

/* CP15 CR12: Interrupt Controller List Registers, n = 0 - 15 */
#define __LRC0(x)       p15, 4, c12, c14, x
#define __LRC8(x)       p15, 4, c12, c15, x

#define ICH_LRC0        __LRC0(0)
#define ICH_LRC1        __LRC0(1)
#define ICH_LRC2        __LRC0(2)
#define ICH_LRC3        __LRC0(3)
#define ICH_LRC4        __LRC0(4)
#define ICH_LRC5        __LRC0(5)
#define ICH_LRC6        __LRC0(6)
#define ICH_LRC7        __LRC0(7)
#define ICH_LRC8        __LRC8(0)
#define ICH_LRC9        __LRC8(1)
#define ICH_LRC10       __LRC8(2)
#define ICH_LRC11       __LRC8(3)
#define ICH_LRC12       __LRC8(4)
#define ICH_LRC13       __LRC8(5)
#define ICH_LRC14       __LRC8(6)
#define ICH_LRC15       __LRC8(7)

/* CP15 CR13:  */
#define FCSEIDR         p15,0,c13,c0,0  /* FCSE Process ID Register */
#define CONTEXTIDR      p15,0,c13,c0,1  /* Context ID Register */
#define TPIDRURW        p15,0,c13,c0,2  /* Software Thread ID, User, R/W */
#define TPIDRURO        p15,0,c13,c0,3  /* Software Thread ID, User, R/O */
#define TPIDRPRW        p15,0,c13,c0,4  /* Software Thread ID, Priveleged */
#define HTPIDR          p15,4,c13,c0,2  /* HYp Software Thread Id Register */

/* CP15 CR14:  */
#define CNTPCT          p15,0,c14       /* Time counter value */
#define CNTFRQ          p15,0,c14,c0,0  /* Time counter frequency */
#define CNTKCTL         p15,0,c14,c1,0  /* Time counter kernel control */
#define CNTP_TVAL       p15,0,c14,c2,0  /* Physical Timer value */
#define CNTP_CTL        p15,0,c14,c2,1  /* Physical Timer control register */
#define CNTVCT          p15,1,c14       /* Time counter value + offset */
#define CNTP_CVAL       p15,2,c14       /* Physical Timer comparator */
#define CNTV_CVAL       p15,3,c14       /* Virt. Timer comparator */
#define CNTVOFF         p15,4,c14       /* Time counter offset */
#define CNTHCTL         p15,4,c14,c1,0  /* Time counter hyp. control */
#define CNTHP_TVAL      p15,4,c14,c2,0  /* Hyp. Timer value */
#define CNTHP_CTL       p15,4,c14,c2,1  /* Hyp. Timer control register */
#define CNTV_TVAL       p15,0,c14,c3,0  /* Virt. Timer value */
#define CNTV_CTL        p15,0,c14,c3,1  /* Virt. TImer control register */
#define CNTHP_CVAL      p15,6,c14       /* Hyp. Timer comparator */

/* CP15 CR15: Implementation Defined Registers */

/* Aliases of AArch64 names for use in common code when building for AArch32 */
#ifdef CONFIG_ARM_32
/* Alphabetically... */
#define ACTLR_EL1               ACTLR
#define AFSR0_EL1               ADFSR
#define AFSR1_EL1               AIFSR
#define CCSIDR_EL1              CCSIDR
#define CLIDR_EL1               CLIDR
#define CNTFRQ_EL0              CNTFRQ
#define CNTHCTL_EL2             CNTHCTL
#define CNTHP_CTL_EL2           CNTHP_CTL
#define CNTHP_CVAL_EL2          CNTHP_CVAL
#define CNTKCTL_EL1             CNTKCTL
#define CNTPCT_EL0              CNTPCT
#define CNTP_CTL_EL0            CNTP_CTL
#define CNTP_CVAL_EL0           CNTP_CVAL
#define CNTVCT_EL0              CNTVCT
#define CNTVOFF_EL2             CNTVOFF
#define CNTV_CTL_EL0            CNTV_CTL
#define CNTV_CVAL_EL0           CNTV_CVAL
#define CONTEXTIDR_EL1          CONTEXTIDR
#define CPACR_EL1               CPACR
#define CPTR_EL2                HCPTR
#define CSSELR_EL1              CSSELR
#define CTR_EL0                 CTR
#define DACR32_EL2              DACR
#define ESR_EL1                 DFSR
#define ESR_EL2                 HSR
#define HCR_EL2                 HCR
#define HPFAR_EL2               HPFAR
#define MPUIR_EL1               MPUIR
#define PRENR_EL2               HPRENR
#define PRBAR0_EL1              PRBAR
#define PRBAR_EL1               PRBAR0_EL1
#define PRBAR1_EL1              PRBAR1
#define PRBAR2_EL1              PRBAR2
#define PRBAR3_EL1              PRBAR3
#define PRBAR4_EL1              PRBAR4
#define PRBAR5_EL1              PRBAR5
#define PRBAR6_EL1              PRBAR6
#define PRBAR7_EL1              PRBAR7
#define PRBAR8_EL1              PRBAR8
#define PRBAR9_EL1              PRBAR9
#define PRBAR10_EL1             PRBAR10
#define PRBAR11_EL1             PRBAR11
#define PRBAR12_EL1             PRBAR12
#define PRBAR13_EL1             PRBAR13
#define PRBAR14_EL1             PRBAR14
#define PRBAR15_EL1             PRBAR15
#define PRBAR16_EL1             PRBAR16
#define PRBAR17_EL1             PRBAR17
#define PRBAR18_EL1             PRBAR18
#define PRBAR19_EL1             PRBAR19
#define PRBAR20_EL1             PRBAR20
#define PRBAR21_EL1             PRBAR21
#define PRBAR22_EL1             PRBAR22
#define PRBAR23_EL1             PRBAR23
#define PRBAR24_EL1             PRBAR24
#define PRBAR25_EL1             PRBAR25
#define PRBAR26_EL1             PRBAR26
#define PRBAR27_EL1             PRBAR27
#define PRBAR28_EL1             PRBAR28
#define PRBAR29_EL1             PRBAR29
#define PRBAR30_EL1             PRBAR30
#define PRBAR31_EL1             PRBAR31
#define PRLAR0_EL1              PRLAR
#define PRLAR_EL1               PRLAR0_EL1
#define PRLAR1_EL1              PRLAR1
#define PRLAR2_EL1              PRLAR2
#define PRLAR3_EL1              PRLAR3
#define PRLAR4_EL1              PRLAR4
#define PRLAR5_EL1              PRLAR5
#define PRLAR6_EL1              PRLAR6
#define PRLAR7_EL1              PRLAR7
#define PRLAR8_EL1              PRLAR8
#define PRLAR9_EL1              PRLAR9
#define PRLAR10_EL1             PRLAR10
#define PRLAR11_EL1             PRLAR11
#define PRLAR12_EL1             PRLAR12
#define PRLAR13_EL1             PRLAR13
#define PRLAR14_EL1             PRLAR14
#define PRLAR15_EL1             PRLAR15
#define PRLAR16_EL1             PRLAR16
#define PRLAR17_EL1             PRLAR17
#define PRLAR18_EL1             PRLAR18
#define PRLAR19_EL1             PRLAR19
#define PRLAR20_EL1             PRLAR20
#define PRLAR21_EL1             PRLAR21
#define PRLAR22_EL1             PRLAR22
#define PRLAR23_EL1             PRLAR23
#define PRLAR24_EL1             PRLAR24
#define PRLAR25_EL1             PRLAR25
#define PRLAR26_EL1             PRLAR26
#define PRLAR27_EL1             PRLAR27
#define PRLAR28_EL1             PRLAR28
#define PRLAR29_EL1             PRLAR29
#define PRLAR30_EL1             PRLAR30
#define PRLAR31_EL1             PRLAR31
#define PRBAR0_EL2              HPRBAR0
#define PRBAR1_EL2              HPRBAR1
#define PRBAR2_EL2              HPRBAR2
#define PRBAR3_EL2              HPRBAR3
#define PRBAR4_EL2              HPRBAR4
#define PRBAR5_EL2              HPRBAR5
#define PRBAR6_EL2              HPRBAR6
#define PRBAR7_EL2              HPRBAR7
#define PRBAR8_EL2              HPRBAR8
#define PRBAR9_EL2              HPRBAR9
#define PRBAR10_EL2             HPRBAR10
#define PRBAR11_EL2             HPRBAR11
#define PRBAR12_EL2             HPRBAR12
#define PRBAR13_EL2             HPRBAR13
#define PRBAR14_EL2             HPRBAR14
#define PRBAR15_EL2             HPRBAR15
#define PRBAR16_EL2             HPRBAR16
#define PRBAR17_EL2             HPRBAR17
#define PRBAR18_EL2             HPRBAR18
#define PRBAR19_EL2             HPRBAR19
#define PRBAR20_EL2             HPRBAR20
#define PRBAR21_EL2             HPRBAR21
#define PRBAR22_EL2             HPRBAR22
#define PRBAR23_EL2             HPRBAR23
#define PRBAR24_EL2             HPRBAR24
#define PRBAR25_EL2             HPRBAR25
#define PRBAR26_EL2             HPRBAR26
#define PRBAR27_EL2             HPRBAR27
#define PRBAR28_EL2             HPRBAR28
#define PRBAR29_EL2             HPRBAR29
#define PRBAR30_EL2             HPRBAR30
#define PRBAR31_EL2             HPRBAR31
#define PRLAR0_EL2              HPRLAR0
#define PRLAR1_EL2              HPRLAR1
#define PRLAR2_EL2              HPRLAR2
#define PRLAR3_EL2              HPRLAR3
#define PRLAR4_EL2              HPRLAR4
#define PRLAR5_EL2              HPRLAR5
#define PRLAR6_EL2              HPRLAR6
#define PRLAR7_EL2              HPRLAR7
#define PRLAR8_EL2              HPRLAR8
#define PRLAR9_EL2              HPRLAR9
#define PRLAR10_EL2             HPRLAR10
#define PRLAR11_EL2             HPRLAR11
#define PRLAR12_EL2             HPRLAR12
#define PRLAR13_EL2             HPRLAR13
#define PRLAR14_EL2             HPRLAR14
#define PRLAR15_EL2             HPRLAR15
#define PRLAR16_EL2             HPRLAR16
#define PRLAR17_EL2             HPRLAR17
#define PRLAR18_EL2             HPRLAR18
#define PRLAR19_EL2             HPRLAR19
#define PRLAR20_EL2             HPRLAR20
#define PRLAR21_EL2             HPRLAR21
#define PRLAR22_EL2             HPRLAR22
#define PRLAR23_EL2             HPRLAR23
#define PRLAR24_EL2             HPRLAR24
#define PRLAR25_EL2             HPRLAR25
#define PRLAR26_EL2             HPRLAR26
#define PRLAR27_EL2             HPRLAR27
#define PRLAR28_EL2             HPRLAR28
#define PRLAR29_EL2             HPRLAR29
#define PRLAR30_EL2             HPRLAR30
#define PRLAR31_EL2             HPRLAR31
#define PRSELR_EL1              PRSELR
#define PRSELR_EL2              HPRSELR
#define HSTR_EL2                HSTR
#define ICC_BPR1_EL1            ICC_BPR1
#define ICC_CTLR_EL1            ICC_CTLR
#define ICC_DIR_EL1             ICC_DIR
#define ICC_EOIR1_EL1           ICC_EOIR1
#define ICC_IGRPEN1_EL1         ICC_IGRPEN1
#define ICC_PMR_EL1             ICC_PMR
#define ICC_SGI1R_EL1           ICC_SGI1R
#define ICC_SRE_EL1             ICC_SRE
#define ICC_SRE_EL2             ICC_HSRE
#define ICH_AP0R0_EL2           ICH_AP0R0
#define ICH_AP0R1_EL2           ICH_AP0R1
#define ICH_AP0R2_EL2           ICH_AP0R2
#define ICH_AP0R3_EL2           ICH_AP0R3
#define ICH_AP1R0_EL2           ICH_AP1R0
#define ICH_AP1R1_EL2           ICH_AP1R1
#define ICH_AP1R2_EL2           ICH_AP1R2
#define ICH_AP1R3_EL2           ICH_AP1R3
#define ICH_EISR_EL2            ICH_EISR
#define ICH_ELRSR_EL2           ICH_ELRSR
#define ICH_HCR_EL2             ICH_HCR
#define ICC_IAR1_EL1            ICC_IAR1
#define ICH_LR0_EL2             ICH_LR0
#define ICH_LR1_EL2             ICH_LR1
#define ICH_LR2_EL2             ICH_LR2
#define ICH_LR3_EL2             ICH_LR3
#define ICH_LR4_EL2             ICH_LR4
#define ICH_LR5_EL2             ICH_LR5
#define ICH_LR6_EL2             ICH_LR6
#define ICH_LR7_EL2             ICH_LR7
#define ICH_LR8_EL2             ICH_LR8
#define ICH_LR9_EL2             ICH_LR9
#define ICH_LR10_EL2            ICH_LR10
#define ICH_LR11_EL2            ICH_LR11
#define ICH_LR12_EL2            ICH_LR12
#define ICH_LR13_EL2            ICH_LR13
#define ICH_LR14_EL2            ICH_LR14
#define ICH_LR15_EL2            ICH_LR15
#define ICH_LRC0_EL2            ICH_LRC0
#define ICH_LRC1_EL2            ICH_LRC1
#define ICH_LRC2_EL2            ICH_LRC2
#define ICH_LRC3_EL2            ICH_LRC3
#define ICH_LRC4_EL2            ICH_LRC4
#define ICH_LRC5_EL2            ICH_LRC5
#define ICH_LRC6_EL2            ICH_LRC6
#define ICH_LRC7_EL2            ICH_LRC7
#define ICH_LRC8_EL2            ICH_LRC8
#define ICH_LRC9_EL2            ICH_LRC9
#define ICH_LRC10_EL2           ICH_LRC10
#define ICH_LRC11_EL2           ICH_LRC11
#define ICH_LRC12_EL2           ICH_LRC12
#define ICH_LRC13_EL2           ICH_LRC13
#define ICH_LRC14_EL2           ICH_LRC14
#define ICH_LRC15_EL2           ICH_LRC15
#define ICH_MISR_EL2            ICH_MISR
#define ICH_VMCR_EL2            ICH_VMCR
#define ICH_VTR_EL2             ICH_VTR
#define ID_AFR0_EL1             ID_AFR0
#define ID_DFR0_EL1             ID_DFR0
#define ID_DFR1_EL1             ID_DFR1
#define ID_ISAR0_EL1            ID_ISAR0
#define ID_ISAR1_EL1            ID_ISAR1
#define ID_ISAR2_EL1            ID_ISAR2
#define ID_ISAR3_EL1            ID_ISAR3
#define ID_ISAR4_EL1            ID_ISAR4
#define ID_ISAR5_EL1            ID_ISAR5
#define ID_ISAR6_EL1            ID_ISAR6
#define ID_MMFR0_EL1            ID_MMFR0
#define ID_MMFR1_EL1            ID_MMFR1
#define ID_MMFR2_EL1            ID_MMFR2
#define ID_MMFR3_EL1            ID_MMFR3
#define ID_MMFR4_EL1            ID_MMFR4
#define ID_MMFR5_EL1            ID_MMFR5
#define ID_PFR0_EL1             ID_PFR0
#define ID_PFR1_EL1             ID_PFR1
#define ID_PFR2_EL1             ID_PFR2
#define IFSR32_EL2              IFSR
#define MDCR_EL2                HDCR
#define MIDR_EL1                MIDR
#define MPIDR_EL1               MPIDR
#define MPUIR_EL2               HMPUIR
#define PAR_EL1                 PAR
#define SCTLR_EL1               SCTLR
#define SCTLR_EL2               HSCTLR
#define TCR_EL1                 TTBCR
#define TEECR32_EL1             TEECR
#define TEEHBR32_EL1            TEEHBR
#define TPIDRRO_EL0             TPIDRURO
#define TPIDR_EL0               TPIDRURW
#define TPIDR_EL1               TPIDRPRW
#define TPIDR_EL2               HTPIDR
#define TTBR0_EL1               TTBR0
#define TTBR0_EL2               HTTBR
#define TTBR1_EL1               TTBR1
#define VBAR_EL1                VBAR
#define VBAR_EL2                HVBAR
#define VMPIDR_EL2              VMPIDR
#define VPIDR_EL2               VPIDR
#define VSCTLR_EL2              VSCTLR
#define VTCR_EL2                VTCR
#define VTTBR_EL2               VTTBR
#define MVFR0_EL1               MVFR0
#define MVFR1_EL1               MVFR1
#define MVFR2_EL1               MVFR2
#endif

/* Hypervisor timer registers for Secure/Non-Secure EL2 */
#ifndef CONFIG_ARM_SECURE_STATE
#define CNTHPx_TVAL_EL2  CNTHP_TVAL_EL2
#define CNTHPx_CTL_EL2   CNTHP_CTL_EL2
#define CNTHPx_CVAL_EL2  CNTHP_CVAL_EL2
#else
#define CNTHPx_TVAL_EL2  CNTHPS_TVAL_EL2
#define CNTHPx_CTL_EL2   CNTHPS_CTL_EL2
#define CNTHPx_CVAL_EL2  CNTHPS_CVAL_EL2
#endif

#endif
/*
 * Local variables:
 * mode: C
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * End:
 */
