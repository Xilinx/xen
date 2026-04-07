/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Mali G78AE GPU PTM System driver header
 *
 * Copyright (C) 2025, Advanced Micro Devices, Inc.
 */
#ifndef DRIVERS__GPU_MALI_G78AE_SYSTEM_H
#define DRIVERS__GPU_MALI_G78AE_SYSTEM_H

#include <xen/types.h>

#define NUM_IRQ_REGISTERS 6

struct mali_ptm_system
{
    paddr_t base;
    paddr_t size;
    void __iomem *mem;
    struct {
        int line;
        int flags;
        uint32_t mask[NUM_IRQ_REGISTERS];
    } irq;
};

int mali_ptm_system_init(struct mali_ptm_system *system);
#endif /* DRIVERS__GPU_MALI_G78AE_SYSTEM_H */

/*
 * Local variables:
 * mode: C
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * End:
 */