/*
 * Kernel image loading.
 *
 * Copyright (C) 2011 Citrix Systems, Inc.
 */
#ifndef __ARCH_ARM_KERNEL_H__
#define __ARCH_ARM_KERNEL_H__

#include <asm/domain.h>

/* Supported vpl011 types */
enum vpl011_type {
    VUART_TYPE_NONE,
    VUART_TYPE_SBSA,     /* Expose SBSA UART (subset of PL011) */
    VUART_TYPE_PL011,    /* Expose PL011 */
};

struct arch_kernel_info
{
#ifdef CONFIG_ARM_64
    enum domain_type type;
#endif

    /* Enable pl011 emulation */
    enum vpl011_type vpl011;
};

#endif /* #ifdef __ARCH_ARM_KERNEL_H__ */

/*
 * Local variables:
 * mode: C
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * End:
 */
