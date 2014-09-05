/*
 * xen/arch/arm/arm64/debug-cadence.S
 *
 * Cadence UART specific debug code
 *
 * Copyright (c) 2014 Xilinx Inc.
 * Written by Edgar E. Iglesias.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <asm/asm_defns.h>

/* PL011 UART initialization
 * xb: register which containts the UART base address
 * c: scratch register number */
.macro early_uart_init xb, c
.endm

/* PL011 UART wait UART to be ready to transmit
 * xb: register which contains the UART base address
 * c: scratch register number */
.macro early_uart_ready xb, c
1:
        ldrh  w\c, [\xb, #0x2c]      /* <- UARTFR (Flag register) */
        tst   w\c, #0x10              /* Check BUSY bit */
        b.ne  1b                     /* Wait for the UART to be ready */
.endm

/* PL011 UART transmit character
 * xb: register which contains the UART base address
 * wt: register which contains the character to transmit */
.macro early_uart_transmit xb, wt
        strb  \wt, [\xb, #0x30]             /* -> UARTDR (Data Register) */
.endm

/*
 * Local variables:
 * mode: ASM
 * indent-tabs-mode: nil
 * End:
 */
