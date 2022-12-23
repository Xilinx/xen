/*
 * Kernel image loading.
 *
 * Copyright (C) 2011 Citrix Systems, Inc.
 */
#ifndef __ARCH_ARM_KERNEL_H__
#define __ARCH_ARM_KERNEL_H__

#include <xen/device_tree.h>
#include <asm/setup.h>

extern const char __data_begin[];
extern const char __init_begin[], __init_end[];
extern const char __init_data_begin[];
extern const char _sinitdata[], _einitdata[];
extern const char __bss_start[], __bss_end[];

#define get_kernel_text_start()     __pa(_stext)
#define get_kernel_text_end()       __pa(_etext)
#define get_kernel_rodata_start()   __pa(_srodata)
#define get_kernel_rodata_end()     __pa(_erodata)
#define get_kernel_data_start()     __pa(__data_begin)
#define get_kernel_data_end()       __pa(__init_begin)
#define get_kernel_inittext_start() __pa(_sinittext)
#define get_kernel_inittext_end()   __pa(_einittext)
#define get_kernel_initdata_start() __pa(__init_data_begin)
#define get_kernel_initdata_end()   __pa(__init_end)
#define get_kernel_bss_start()      __pa(__bss_start)
#define get_kernel_bss_end()        __pa(__bss_end)

/*
 * List of possible features for dom0less domUs
 *
 * DOM0LESS_ENHANCED_NO_XS: Notify the OS it is running on top of Xen. All the
 *                          default features (excluding Xenstore) will be
 *                          available. Note that an OS *must* not rely on the
 *                          availability of Xen features if this is not set.
 * DOM0LESS_XENSTORE:       Xenstore will be enabled for the VM. This feature
 *                          can't be enabled without the
 *                          DOM0LESS_ENHANCED_NO_XS.
 * DOM0LESS_ENHANCED:       Notify the OS it is running on top of Xen. All the
 *                          default features (including Xenstore) will be
 *                          available. Note that an OS *must* not rely on the
 *                          availability of Xen features if this is not set.
 */
#define DOM0LESS_ENHANCED_NO_XS  BIT(0, U)
#define DOM0LESS_XENSTORE        BIT(1, U)
#define DOM0LESS_ENHANCED        (DOM0LESS_ENHANCED_NO_XS | DOM0LESS_XENSTORE)

struct kernel_info {
#ifdef CONFIG_ARM_64
    enum domain_type type;
#endif

    struct domain *d;

    void *fdt; /* flat device tree */
    paddr_t unassigned_mem; /* RAM not (yet) assigned to a bank */
    struct meminfo mem;
    struct meminfo shm_mem;

    /* kernel entry point */
    paddr_t entry;

    /* grant table region */
    paddr_t gnttab_start;
    paddr_t gnttab_size;

    /* boot blob load addresses */
    const struct bootmodule *kernel_bootmodule, *initrd_bootmodule, *dtb_bootmodule;
    const char* cmdline;
    paddr_t dtb_paddr;
    paddr_t initrd_paddr;

    /* Enable pl011 emulation */
    bool vpl011;

    /* Enable/Disable PV drivers interfaces */
    uint16_t dom0less_feature;

    /* GIC phandle */
    uint32_t phandle_gic;

    /* loader to use for this kernel */
    void (*load)(struct kernel_info *info);
    /* loader specific state */
    union {
        struct {
            paddr_t kernel_addr;
            paddr_t len;
#ifdef CONFIG_ARM_64
            paddr_t text_offset; /* 64-bit Image only */
#endif
            paddr_t start; /* 32-bit zImage only */
        } zimage;
    };
};

/*
 * Probe the kernel to detemine its type and select a loader.
 *
 * Sets in info:
 *  ->type
 *  ->load hook, and sets loader specific variables ->zimage
 */
int kernel_probe(struct kernel_info *info, const struct dt_device_node *domain);

/*
 * Loads the kernel into guest RAM.
 *
 * Expects to be set in info when called:
 *  ->mem
 *  ->fdt
 *
 * Sets in info:
 *  ->entry
 *  ->dtb_paddr
 *  ->initrd_paddr
 */
void kernel_load(struct kernel_info *info);

#endif /* #ifdef __ARCH_ARM_KERNEL_H__ */

/*
 * Local variables:
 * mode: C
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * End:
 */
