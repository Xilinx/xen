#ifndef __ASM_ARM_ARM64_MACROS_H
#define __ASM_ARM_ARM64_MACROS_H

    /*
     * @dst: Result of get_cpu_info()
     */
    .macro  adr_cpu_info, dst
    add     \dst, sp, #STACK_SIZE
    and     \dst, \dst, #~(STACK_SIZE - 1)
    sub     \dst, \dst, #CPUINFO_sizeof
    .endm

    /*
     * @dst: Result of READ_ONCE(per_cpu(sym, smp_processor_id()))
     * @sym: The name of the per-cpu variable
     * @tmp: scratch register
     */
    .macro  ldr_this_cpu, dst, sym, tmp
    ldr     \dst, =per_cpu__\sym
    mrs     \tmp, tpidr_el2
    ldr     \dst, [\dst, \tmp]
    .endm

    .macro  ret
        /* ret opcode */
        .inst 0xd65f03c0
        sb
    .endm

    /* clearbhb instruction clearing the branch history */
    .macro clearbhb
        hint    #22
    .endm

    /*
     * Pseudo-op for PC relative adr <reg>, <symbol> where <symbol> is
     * within the range +/- 4GB of the PC.
     *
     * @dst: destination register (64 bit wide)
     * @sym: name of the symbol
     */
    .macro  adr_l, dst, sym
        adrp \dst, \sym
        add  \dst, \dst, :lo12:\sym
    .endm

    /* Load the physical address of a symbol into xb */
    .macro load_paddr xb, sym
        ldr \xb, =\sym
        add \xb, \xb, x20
    .endm

/*
 * Register aliases.
 */
lr      .req    x30             /* link register */

#ifdef CONFIG_EARLY_PRINTK
/*
 * Macro to print a string to the UART, if there is one.
 *
 * Clobbers x0 - x3
 */
#define PRINT(_s)          \
        mov   x3, lr ;     \
        adr   x0, 98f ;    \
        bl    puts    ;    \
        mov   lr, x3 ;     \
        RODATA_STR(98, _s)

    /*
     * Macro to print the value of register \xb
     *
     * Clobbers x0 - x4
     */
    .macro print_reg xb
    mov   x0, \xb
    mov   x4, lr
    bl    putn
    mov   lr, x4
    .endm

#else /* CONFIG_EARLY_PRINTK */
#define PRINT(s)

.macro print_reg xb
.endm

#endif /* !CONFIG_EARLY_PRINTK */

#endif /* __ASM_ARM_ARM64_MACROS_H */
