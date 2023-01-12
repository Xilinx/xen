#ifndef __ARCH_ARM_V8R__
#define __ARCH_ARM_V8R__

#if defined(CONFIG_ARM_32)
# include <asm/armv8r/arm32/sysregs.h>
#elif defined(CONFIG_ARM_64)
# include <asm/armv8r/arm64/sysregs.h>
#else
# error "unknown ARM variant"
#endif

#endif /* __ARCH_ARM_V8R__ */
