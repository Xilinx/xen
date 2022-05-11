#ifndef __ASM_ARM_PLATFORMS_FVP_BASER_H__
#define __ASM_ARM_PLATFORMS_FVP_BASER_H__

/*
 * 0xFFFFFFFF indicates users haven't customized XEN_START_ADDRESS,
 * we will use platform defined default address.
 */
#if CONFIG_XEN_START_ADDRESS == 0xFFFFFFFF
#define XEN_START_ADDRESS 0x200000
#else
#define XEN_START_ADDRESS CONFIG_XEN_START_ADDRESS
#endif

#endif /* __ASM_ARM_PLATFORMS_FVP_BASER_H__ */
