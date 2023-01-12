// SPDX-License-Identifier: GPL-2.0
/*
 * xen/arch/arm_mpu/arm32/mpu_v8r.S
 *
 * Helper function and macros for Armv8R AArch32.
 *
 * Copyright (C) 2022, Advanced Micro Devices, Inc. All Rights Reserved.
 */

#include <asm/armv8r/mpu.h>
#include <asm/cpregs.h>
#include <asm/early_printk.h>
#include <asm/sysregs.h>

/* Normal and device MPU protection region selector. */
#define MPU_REGION_NORMAL_SEL       0x0
#define MPU_REGION_DEVICE_SEL       0x1

#define MPU_NORMAL_MEM_SIZE         0x001fffff    /* 2MB - 1 */

/*
 * One pr_t entry in boot_mpumap takes up 16 bytes.
 * Normal and device MPU protection region table index.
 */
#define MPU_REGION_NORMAL_INDEX     0x0
#define MPU_REGION_DEVICE_INDEX     0x10

/*
 * Refer Hypervisor Protection Region Base Address Register for the
 * definition of the below registers.
 */

/* Normal memory attributes. */
#define PRBAR_NORMAL_MEM            0x30    /* SH=11 AP=00 XN=00 */
#define PRLAR_NORMAL_MEM            0x0f    /* NS=0 ATTR=111 EN=1 */

/* Device memory attributes. */
#define PRBAR_DEV_MEM               0x20    /* SH=10 AP=00 XN=00 */
#define PRLAR_DEV_MEM               0x09    /* NS=0 ATTR=100 EN=1 */


.text

/*
 * Macro to create a mpu protection region entry in \prmap.
 *
 * prmap:   mpu protection region map table symbol
 * index:   table index
 * tmp1:    scratch register
 * tmp2:    scratch register
 *
 * Preserve prbar register value in x1 and prlar register value in x2
 *
 */
.macro create_mpu_entry, prmap, index, tmp1, tmp2
    ldr   \tmp1, =\prmap
    mov   \tmp2, #\index
    str   r1, [\tmp1, \tmp2]  /* r1: prbar */

    add   \tmp2, \tmp2, #4
    str   r2, [\tmp1, \tmp2]  /* r2: prlar */
.endm

.macro write_pr, sel, prbar, prlar
    mcr   p15, 4, \sel, c6, c2, 1
    dsb   sy
    mcr   p15, 4, \prbar, c6, c3, 0
    mcr   p15, 4, \prlar, c6, c3, 1
    dsb   sy
    isb
.endm

/*
 * Static start-of-day EL2 MPU memory region configuration that we use
 * before the allocators are up.
 *
 * It has a very simple structure, including:
 *  - 2MB normal memory mappings of xen at CONFIG_XEN_START_ADDRESS, which
 * is the address where Xen was loaded by the bootloader.
 *  - Map the UART if EARLY_PRINTK is enabled.
 */
ENTRY(mpu_init)
    /* Normal memory region. */
    mov   r0, #MPU_REGION_NORMAL_SEL
    ldr   r1, =CONFIG_XEN_START_ADDRESS
    lsr   r1, r1, #MPU_REGION_SHIFT
    lsl   r1, r1, #MPU_REGION_SHIFT
    mov   r3, #PRBAR_NORMAL_MEM
    orr   r1, r1, r3    /* x1: prbar */
    ldr   r2, =CONFIG_XEN_START_ADDRESS
    ldr   r3, =#MPU_NORMAL_MEM_SIZE
    add   r2, r2, r3
    lsr   r2, r2, #MPU_REGION_SHIFT
    lsl   r2, r2, #MPU_REGION_SHIFT
    mov   r3, #PRLAR_NORMAL_MEM
    orr   r2, r2, r3    /* x2: prlar */

    /* Create an entry for normal memory region in boot_mpumap. */
    create_mpu_entry boot_mpumap, MPU_REGION_NORMAL_INDEX, r4, r5
    isb
    write_pr r0, r1, r2

#ifdef CONFIG_EARLY_PRINTK
    /* Device memory region of UART. */
    mov   r0, #MPU_REGION_DEVICE_SEL
    ldr   r1, =CONFIG_EARLY_UART_BASE_ADDRESS
    lsr   r1, r1, #MPU_REGION_SHIFT
    lsl   r1, r1, #MPU_REGION_SHIFT
    mov   r3, #PRBAR_DEV_MEM
    orr   r1, r1, r3    /* x1: prbar */
    ldr   r2, =CONFIG_EARLY_UART_BASE_ADDRESS
    mov   r3, #EARLY_UART_SIZE
    add   r2, r2, r3
    lsr   r2, r2, #MPU_REGION_SHIFT
    lsl   r2, r2, #MPU_REGION_SHIFT
    mov   r3, #PRLAR_DEV_MEM
    orr   r2, r2, r3    /* x2: prlar */

    /* Create an entry for device memory region in boot_mpumap. */
    create_mpu_entry boot_mpumap, MPU_REGION_DEVICE_INDEX, r4, r5
    isb
    write_pr r0, r1, r2
#endif
    ret
ENDPROC(mpu_init)

/* Enable EL2 MPU and data cache. */
ENTRY(enable_mpu)
    mrc   CP32(r0, HSCTLR)
    mov   r1, #(SCTLR_Axx_ELx_M | SCTLR_Axx_ELx_C)
    orr   r0, r0, r1  /* Enable EL2 MPU and D-cache */
    dsb   sy
    mcr   CP32(r0, HSCTLR)
    isb
    ret
ENDPROC(enable_mpu)

/* Disable EL2 MPU and data cache. */
ENTRY(disable_mpu)
    mrc   CP32(r0, HSCTLR)
    mov   r1, #~(SCTLR_Axx_ELx_M | SCTLR_Axx_ELx_C)
    and   r0, r0, r1  /* Disable EL2 MPU and D-cache */
    dsb   sy
    mcr   CP32(r0, HSCTLR)
    isb
    ret
ENDPROC(disable_mpu)

/*
 * Iterate MPU memory region configuration, #mpu_table, to properly
 * set according MPU protection region registers.
 *
 * r0: len refers to table length (count of entries in mpu table).
 * r1: mpu_table. Every entry(pr_t) takes up 16 bytes, the previous 4 bytes
 * shall be stored in prbar register, and the latter 4 are for prlar register.
 * The last 8 bytes are left unused as of now. They shall be used later to
 * store p2m_type.
 */
ENTRY(set_boot_mpumap)
    push {r4}
    mov   r2, #0               /* table index */
1:  ldr   r3, [r1], #4         /* r3: prbar */
    ldr   r4, [r1], #12        /* r4: prlar */
    write_pr r2, r3, r4
    add   r2, r2, #1           /* table index ++ */
    cmp   r2, r0
    blt  1b
    pop {r4}
    ret
ENDPROC(set_boot_mpumap)

/*
 * Clear stale MPU protection regions before setting new reordered ones.
 *
 * r0: len refers to length of stale mpu table (count of entries in mpu table).
 */
ENTRY(clear_xen_mpumap)
    mov   r1, #0              /* sel */
    mov   r2, #0              /* use zero value to clean up. */
1:  write_pr r1, r2, r2
    add   r1, r1, #1
    cmp   r1, r0
    blt  1b
    ret
ENDPROC(clear_xen_mpumap)
