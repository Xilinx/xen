/*
 * xen/arch/arm_mpu/mm_mpu.c
 *
 * MPU based memory managment code for an Armv8-R AArch64.
 *
 * Copyright (C) 2022 Arm Ltd.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#include <xen/init.h>
#include <xen/mm.h>
#include <asm/kernel.h>

/*
 *  A boot time MPU protetction region map that is used in assembly code
 *  before BSS is zeroed.
 */
pr_t __aligned(PAGE_SIZE) __section(".data.page_aligned")
     boot_mpumap[ARM_DEFAULT_MPU_PROTECTION_REGIONS];

/* Number of MPU protection regions that XEN can see at EL2. */
unsigned long nr_xen_mpumap;

/*
 * Next index used in xen_mpumap.
 * We always add new entry in xen_mpumap in the ascending order.
 * Be aware that nr_xen_mpumap is not always equal to next_xen_mpumap_index.
 */
unsigned long next_xen_mpumap_index = 0UL;

/*
 * Using a bitmap here to record the status of MPU protection region,
 * which is used for XEN stage 1 memory mapping.
 * Bit 0 represents MPU protection region 0, bit 1 represents MPU protection
 * region 1, ..., and so on.
 * If a MPU protection region gets enabled, set the according bit to 1.
 * Be aware that AArch64-v8R supports at most 256 MPU protection regions.
 */
static DECLARE_BITMAP(xen_mpumap_mask, MAX_MPU_PROTECTION_REGIONS);

/*
 * Standard entry that will be used to build Xen's own MPU memory region
 * configuration.
 * It is equivalent to mfn_to_xen_entry from MMU system.
 */
static inline pr_t pr_of_xenaddr(paddr_t baddr, paddr_t eaddr, unsigned attr)
{
    prbar_t base;
    prlar_t limit;
    pr_t region;

    /* Build up prbar (Protection Region Base Address Register) register. */
    base = (prbar_t) {
        .reg = {
            .ap = AP_RW_EL2,  /* Read/Write at EL2, no access at EL1/EL0. */
            .xn = XN_ENABLED, /* No need to execute outside .text */
        }};

    switch ( attr )
    {
    case MT_NORMAL_NC:
        /*
         * ARM ARM: Overlaying the shareability attribute (DDI
         * 0406C.b B3-1376 to 1377)
         *
         * A memory region with a resultant memory type attribute of normal,
         * and a resultant cacheability attribute of Inner non-cacheable,
         * outer non-cacheable, must have a resultant shareability attribute
         * of outer shareable, otherwise shareability is UNPREDICTABLE.
         *
         * On ARMv8 sharability is ignored and explicitly treated as outer
         * shareable for normal inner non-cacheable, outer non-cacheable.
         */
        base.reg.sh = LPAE_SH_OUTER;
        break;
    case MT_DEVICE_nGnRnE:
    case MT_DEVICE_nGnRE:
        /*
         * Shareability is ignored for non-normal memory, Outer is as
         * good as anything.
         *
         * On ARMv8 sharability is ignored and explicitly treated as outer
         * shareable for any device memory type.
         */
        base.reg.sh = LPAE_SH_OUTER;
        break;
    default:
        base.reg.sh = LPAE_SH_INNER;  /* Xen mappings are SMP coherent */
        break;
    }

    /* Build up prlar (Protection Region Limit Address Register) register. */
    limit = (prlar_t) {
        .reg = {
            .ns = 0,        /* Hyp mode is in secure world */
            .ai = attr,
            .en = 1,        /* Region enabled */
        }};

    /* Build up MPU protection region. */
    region = (pr_t) {
        .base = base,
        .limit = limit,
    };

    /* Set base address and limit address. */
    pr_set_base(&region, baddr);
    pr_set_limit(&region, eaddr);

    return region;
}

/*
 * After boot, Xen memory mapping should not contain mapping that are
 * both writable and executable.
 *
 * This should be called on each CPU to enforce the policy like MMU
 * system. But the different is that, for MPU systems, EL2 stage 1
 * PMSAv8-64 attributes will not be cached by TLB (ARM DDI 0600A.c
 * D1.6.2 TLB maintenance instructions). So this MPU version function
 * does not need Xen local TLB flush.
 */
static void xen_mpu_enforce_wnx(void)
{
    WRITE_SYSREG(READ_SYSREG(SCTLR_EL2) | SCTLR_Axx_ELx_WXN, SCTLR_EL2);
}

/*
 * At boot-time, there are only two MPU memory regions defined: normal memory
 * and device memory, which are now insecure and coarse-grained.
 * Split Xen kernel into six sections based on memory attributes. Since
 * adjacent kernel section has different memory attributes, like text and RO
 * data, one MPU protection region is needed for each section.
 * Below is the XEN MPU memory region layout:
 *     boot_mpumap[0] : kernel text
 *     boot_mpumap[1] : kernel read-only data
 *     boot_mpumap[2] : kernel read-write data
 *     boot_mpumap[3] : kernel init text
 *     boot_mpumap[4] : kernel init data
 *     boot_mpumap[5] : kernel BSS
 */
static void __init map_xen_to_protection_regions(void)
{
    unsigned long start, end;

    /* Kernel text section. */
    start = get_kernel_text_start();
    /* In xen.lds.S, section is page-aligned. */
    end = round_pgup(get_kernel_text_end()) - 1;
    boot_mpumap[next_xen_mpumap_index] = pr_of_xenaddr(start, end, MT_NORMAL);
    /* Read-only, and executable. */
    boot_mpumap[next_xen_mpumap_index].base.reg.xn = XN_DISABLED;
    boot_mpumap[next_xen_mpumap_index].base.reg.ap = AP_RO_EL2;
    set_bit(next_xen_mpumap_index++, xen_mpumap_mask);

    /* Kernel read-only data section. */
    start = get_kernel_rodata_start();
    /* In xen.lds.S, section is page-aligned. */
    end = round_pgup(get_kernel_rodata_end()) - 1;
    boot_mpumap[next_xen_mpumap_index] = pr_of_xenaddr(start, end, MT_NORMAL);
    /* Read-only. */
    boot_mpumap[next_xen_mpumap_index].base.reg.ap = AP_RO_EL2;
    set_bit(next_xen_mpumap_index++, xen_mpumap_mask);

    /* Kernel read-write data section. */
    start = get_kernel_data_start();
    /* In xen.lds.S, section is page-aligned. */
    end = round_pgup(get_kernel_data_end()) - 1;
    boot_mpumap[next_xen_mpumap_index] = pr_of_xenaddr(start, end, MT_NORMAL);
    set_bit(next_xen_mpumap_index++, xen_mpumap_mask);

    /* Kernel init text section. */
    start = get_kernel_inittext_start();
    /* In xen.lds.S, section is page-aligned. */
    end = round_pgup(get_kernel_inittext_end()) - 1;
    boot_mpumap[next_xen_mpumap_index] = pr_of_xenaddr(start, end, MT_NORMAL);
    /* Read-only, and executable. */
    boot_mpumap[next_xen_mpumap_index].base.reg.xn = XN_DISABLED;
    boot_mpumap[next_xen_mpumap_index].base.reg.ap = AP_RO_EL2;
    set_bit(next_xen_mpumap_index++, xen_mpumap_mask);

    /* Kernel init data. */
    start = get_kernel_initdata_start();
    /* In xen.lds.S, section is page-aligned. */
    end = round_pgup(get_kernel_initdata_end()) - 1;
    boot_mpumap[next_xen_mpumap_index] = pr_of_xenaddr(start, end, MT_NORMAL);
    set_bit(next_xen_mpumap_index++, xen_mpumap_mask);

    /* Kernel BSS */
    start = get_kernel_bss_start();
    /* In xen.lds.S, section is page-aligned. */
    end = round_pgup(get_kernel_bss_end()) - 1;
    boot_mpumap[next_xen_mpumap_index] = pr_of_xenaddr(start, end, MT_NORMAL);
    set_bit(next_xen_mpumap_index++, xen_mpumap_mask);

#ifdef CONFIG_EARLY_PRINTK
    /*
     * Before getting complete device memory mappings in device tree,
     * only early printk uart is considered during early boot.
     */
    start = CONFIG_EARLY_UART_BASE_ADDRESS;
    end = CONFIG_EARLY_UART_BASE_ADDRESS + EARLY_UART_SIZE - 1;
    boot_mpumap[next_xen_mpumap_index] = pr_of_xenaddr(start, end,
                                                       MT_DEVICE_nGnRE);
    set_bit(next_xen_mpumap_index++, xen_mpumap_mask);
#endif

    nr_xen_mpumap = next_xen_mpumap_index;
}

void __init setup_protection_regions()
{
    map_xen_to_protection_regions();

    /*
     * MPU must be disabled to switch to new mpu memory region configuration.
     * Once the MPU is disabled, cache should also be disabled, because if MPU
     * is disabled, some system will treat the memory access as the IO memory
     * access. The cache data should be flushed to RAM before disabling MPU.
     */
    clean_dcache_va_range((void *)&next_xen_mpumap_index,
                          sizeof(unsigned long));
    clean_dcache_va_range((void *)(pr_t *)boot_mpumap,
                          sizeof(pr_t) * next_xen_mpumap_index);

    /*
     * Since it is the MPU protection region which holds the XEN kernel that
     * needs updating.
     * The whole MPU system must be disabled for the update.
     */
    disable_mm();

    /*
     * Set new MPU memory region configuration.
     * To avoid the mismatch between nr_xen_mpumap and nr_xen_mpumap
     * after the relocation of some MPU regions later, here
     * next_xen_mpumap_index is used.
     * To avoid unexpected unaligment access fault during MPU disabled,
     * set_boot_mpumap shall be written in assembly code.
     */
    set_boot_mpumap(next_xen_mpumap_index, (pr_t *)boot_mpumap);

    enable_mm();

    xen_mpu_enforce_wnx();
}
