/* SPDX-License-Identifier: GPL-2.0
 *
 * xen/arch/arm_mpu/proc-v8.S
 *
 *
 * Copyright (C) 2022, Advanced Micro Devices, Inc. All Rights Reserved.
 */

#include <asm/arm32/processor.h>
#include <asm/sysregs.h>

#define XPAR_CPU_CORTEXR52_0_TIMESTAMP_CLK_FREQ  800000U

cr52_init:
        /*
         * Set counter frequency  800 KHZ
         *
         * Set counter frequency, CNTFRQ
         */
        ldr     r0,=XPAR_CPU_CORTEXR52_0_TIMESTAMP_CLK_FREQ
        mrc     15,0,r1,c14,c0,0  /* Read CNTFRQ */
        cmp     r1,#0
        /* Set only if the frequency read is 0 */
        mcreq   15,0,r0,c14,c0,0  /* Write CNTFRQ */

        mov pc, lr

        .section ".proc.info", #alloc
        .type __v8_cr52_proc_info, #object
__v8_cr52_proc_info:
        .long 0x410F0000
        .long 0xFF0F0000             /* Mask */
        .long cr52_init
        .long caxx_processor
        .size __v8_cr52_proc_info, . - __v8_cr52_proc_info

        .section ".proc.info", #alloc
        .type __v8_cr52_proc_info, #object

/*
 * Local variables:
 * mode: ASM
 * indent-tabs-mode: nil
 * End:
 */
