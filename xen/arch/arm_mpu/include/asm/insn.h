#ifndef __ARCH_ARM_INSN
#define __ARCH_ARM_INSN

#ifndef __ASSEMBLY__

#include <xen/types.h>

#if defined(CONFIG_ARM_64)
# include <asm/arm64/insn.h>
#elif defined(CONFIG_ARM_32)
# include <asm/arm32/insn.h>
#else
# error "unknown ARM variant"
#endif

#endif /* __ASSEMBLY__ */

/* On ARM32,64 instructions are always 4 bytes long. */
#define ARCH_PATCH_INSN_SIZE 4

#endif /* !__ARCH_ARM_INSN */
/*
 * Local variables:
 * mode: C
 * c-file-style: "BSD"
 * c-basic-offset: 8
 * indent-tabs-mode: t
 * End:
 */
