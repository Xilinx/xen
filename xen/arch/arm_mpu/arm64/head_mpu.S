// SPDX-License-Identifier: GPL-2.0-only
/*
 * Start-of-day code for an Armv8-R MPU system.
 */

#include <asm/arm64/mpu.h>
#include <asm/page.h>
#include <asm/early_printk.h>

/*
 * From the requirements of head.S we know that Xen image should
 * be linked at XEN_START_ADDRESS, and all of text + data + bss
 * must fit in 2MB. On MPU systems, XEN_START_ADDRESS is also the
 * address that Xen image should be loaded at. So for initial MPU
 * regions setup, we use 2MB for Xen data memory to setup boot
 * region, or the create boot regions code below will need adjustment.
 */
#define XEN_START_MEM_SIZE      0x200000

/*
 * In boot stage, we will use 1 MPU region:
 * Region#0: Normal memory for Xen text + data + bss (2MB)
 */
#define BOOT_NORMAL_REGION_IDX  0x0

/* MPU normal memory attributes. */
#define PRBAR_NORMAL_MEM        0x30    /* SH=11 AP=00 XN=00 */
#define PRLAR_NORMAL_MEM        0x0f    /* NS=0 ATTR=111 EN=1 */

.macro write_pr, sel, prbar, prlar
    msr   PRSELR_EL2, \sel
    dsb   sy
    msr   PRBAR_EL2, \prbar
    msr   PRLAR_EL2, \prlar
    dsb   sy
    isb
.endm

.section .text.header, "ax", %progbits

/*
 * Static start-of-day EL2 MPU memory layout.
 *
 * It has a very simple structure, including:
 *  - 2MB normal memory mappings of xen at XEN_START_ADDRESS, which
 * is the address where Xen was loaded by the bootloader.
 */
ENTRY(prepare_early_mappings)
    /* Map Xen start memory to a normal memory region. */
    mov x0, #BOOT_NORMAL_REGION_IDX
    ldr x1, =XEN_START_ADDRESS
    and x1, x1, #MPU_REGION_MASK
    mov x3, #PRBAR_NORMAL_MEM
    orr x1, x1, x3

    ldr x2, =XEN_START_ADDRESS
    mov x3, #(XEN_START_MEM_SIZE - 1)
    add x2, x2, x3
    and x2, x2, #MPU_REGION_MASK
    mov x3, #PRLAR_NORMAL_MEM
    orr x2, x2, x3

    /*
     * Write to MPU protection region:
     * x0 for pr_sel, x1 for prbar x2 for prlar
     */
    write_pr x0, x1, x2

    ret
ENDPROC(prepare_early_mappings)

/*
 * Enable EL2 MPU and data cache. Because we will disable cache
 * with MPU at the same time, in accordance with that, we have
 * to enable cache with MPU at the same time in this function.
 * When MPU is disabled, the MPU background attributes will
 * be used. On some platform, the background will treat all
 * memory as IO memory. The access to IO memory will bypass
 * the cache, even you have enabled the C bit in SCTLR.
 * To avoid this implicit behavior, we disable cache with MPU
 * explicitly to tell user that when MPU is disabled, the memory
 * access is uncacheable.
 */
ENTRY(enable_mm)
    mrs   x0, SCTLR_EL2
    mov   x1, #(SCTLR_Axx_ELx_M | SCTLR_Axx_ELx_C)
    /* Enable EL2 MPU and D-cache */
    orr   x0, x0, x1
    dsb   sy
    msr   SCTLR_EL2, x0
    isb
    ret
ENDPROC(enable_mm)

/* Disable MPU system, including data cache. */
ENTRY(disable_mm)
    mrs   x0, SCTLR_EL2
    mov   x1, #~(SCTLR_Axx_ELx_M | SCTLR_Axx_ELx_C)
    /* Disable EL2 MPU and D-cache */
    and   x0, x0, x1
    dsb   sy
    msr   SCTLR_EL2, x0
    isb
    ret
ENDPROC(disable_mm)
