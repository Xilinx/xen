/* SPDX-License-Identifier: GPL-2.0
 *
 * xen/arch/arm_mpu/proc-v8.S
 *
 *
 * Copyright (C) 2022, Advanced Micro Devices, Inc. All Rights Reserved.
 */

#include <asm/arm32/processor.h>
#include <asm/sysregs.h>

cr52_init:
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
