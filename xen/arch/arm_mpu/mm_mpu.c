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
#include <xen/libfdt/libfdt.h>
#include <xen/mm.h>
#include <xen/sched.h>
#include <xen/sizes.h>
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

/* Write a protection region */
#define WRITE_PROTECTION_REGION(sel, pr, prbar, prlar) ({               \
    uint64_t _sel = sel;                                                \
    const pr_t *_pr = pr;                                               \
    asm volatile(                                                       \
        "msr "__stringify(PRSELR_EL2)", %0;" /* Selects the region */   \
        "dsb sy;"                                                       \
        "msr "__stringify(prbar)", %1;" /* Write PRBAR<n>_EL2 */        \
        "msr "__stringify(prlar)", %2;" /* Write PRLAR<n>_EL2 */        \
        "dsb sy;"                                                       \
        : : "r" (_sel), "r" (_pr->base.bits), "r" (_pr->limit.bits));   \
})

/* Read a protection region */
#define READ_PROTECTION_REGION(sel, prbar, prlar) ({                    \
    uint64_t _sel = sel;                                                \
    pr_t _pr;                                                           \
    asm volatile(                                                       \
        "msr "__stringify(PRSELR_EL2)", %2;" /* Selects the region */   \
        "dsb sy;"                                                       \
        "mrs %0, "__stringify(prbar)";" /* Read PRBAR<n>_EL2 */         \
        "mrs %1, "__stringify(prlar)";" /* Read PRLAR<n>_EL2 */         \
        "dsb sy;"                                                       \
        : "=r" (_pr.base.bits), "=r" (_pr.limit.bits) : "r" (_sel));    \
    _pr;                                                                \
})

/*
 * Access MPU protection region, including both read/write operations.
 * AArch64-v8R at most supports 256 MPU protection regions.
 * As explained from section G1.3.18 of the reference manual for AArch64-v8R,
 * PRBAR<n>_ELx and PRLAR<n>_ELx provides access to the MPU region
 * determined by the 4 most significant bit written on PRSELR_ELx.REGION and
 * the <n> number from 1 to 15, when n == 0 then PRBAR_ELx should be used.
 * So for example to access regions from 16 to 31 (0b10000 to 0b11111):
 * - Set PRSELR_ELx to 0b10000
 * - Region 16 configuration is accessible through PRBAR_ELx and PRLAR_ELx
 * - Region 17 configuration is accessible through PRBAR1_ELx and PRLAR1_ELx
 * - Region 18 configuration is accessible through PRBAR2_ELx and PRLAR2_ELx
 * - ...
 * - Region 31 configuration is accessible through PRBAR15_ELx and PRLAR15_ELx
 *
 * @read: if it is read operation.
 * @pr_read: mpu protection region returned by read op.
 * @pr_write: mpu protection region passed through write op.
 * @sel: mpu protection region selector
 */
void access_protection_region(bool read, pr_t *pr_read,
                              const pr_t *pr_write, u64 sel)
{
    switch ( sel & 0xf )
    {
    case 0:
        if ( read )
            *pr_read = READ_PROTECTION_REGION(sel, PRBAR0_EL2, PRLAR0_EL2);
        else
            WRITE_PROTECTION_REGION(sel, pr_write, PRBAR0_EL2, PRLAR0_EL2);
        break;
    case 1:
        if ( read )
            *pr_read = READ_PROTECTION_REGION(sel, PRBAR1_EL2, PRLAR1_EL2);
        else
            WRITE_PROTECTION_REGION(sel, pr_write, PRBAR1_EL2, PRLAR1_EL2);
        break;
    case 2:
        if ( read )
            *pr_read = READ_PROTECTION_REGION(sel, PRBAR2_EL2, PRLAR2_EL2);
        else
            WRITE_PROTECTION_REGION(sel, pr_write, PRBAR2_EL2, PRLAR2_EL2);
        break;
    case 3:
        if ( read )
            *pr_read = READ_PROTECTION_REGION(sel, PRBAR3_EL2, PRLAR3_EL2);
        else
            WRITE_PROTECTION_REGION(sel, pr_write, PRBAR3_EL2, PRLAR3_EL2);
        break;
    case 4:
        if ( read )
            *pr_read = READ_PROTECTION_REGION(sel, PRBAR4_EL2, PRLAR4_EL2);
        else
            WRITE_PROTECTION_REGION(sel, pr_write, PRBAR4_EL2, PRLAR4_EL2);
        break;
    case 5:
        if ( read )
            *pr_read = READ_PROTECTION_REGION(sel, PRBAR5_EL2, PRLAR5_EL2);
        else
            WRITE_PROTECTION_REGION(sel, pr_write, PRBAR5_EL2, PRLAR5_EL2);
        break;
    case 6:
        if ( read )
            *pr_read = READ_PROTECTION_REGION(sel, PRBAR6_EL2, PRLAR6_EL2);
        else
            WRITE_PROTECTION_REGION(sel, pr_write, PRBAR6_EL2, PRLAR6_EL2);
        break;
    case 7:
        if ( read )
            *pr_read = READ_PROTECTION_REGION(sel, PRBAR7_EL2, PRLAR7_EL2);
        else
            WRITE_PROTECTION_REGION(sel, pr_write, PRBAR7_EL2, PRLAR7_EL2);
        break;
    case 8:
        if ( read )
            *pr_read = READ_PROTECTION_REGION(sel, PRBAR8_EL2, PRLAR8_EL2);
        else
            WRITE_PROTECTION_REGION(sel, pr_write, PRBAR8_EL2, PRLAR8_EL2);
        break;
    case 9:
        if ( read )
            *pr_read = READ_PROTECTION_REGION(sel, PRBAR9_EL2, PRLAR9_EL2);
        else
            WRITE_PROTECTION_REGION(sel, pr_write, PRBAR9_EL2, PRLAR9_EL2);
        break;
    case 10:
        if ( read )
            *pr_read = READ_PROTECTION_REGION(sel, PRBAR10_EL2, PRLAR10_EL2);
        else
            WRITE_PROTECTION_REGION(sel, pr_write, PRBAR10_EL2, PRLAR10_EL2);
        break;
    case 11:
        if ( read )
            *pr_read = READ_PROTECTION_REGION(sel, PRBAR11_EL2, PRLAR11_EL2);
        else
            WRITE_PROTECTION_REGION(sel, pr_write, PRBAR11_EL2, PRLAR11_EL2);
        break;
    case 12:
        if ( read )
            *pr_read = READ_PROTECTION_REGION(sel, PRBAR12_EL2, PRLAR12_EL2);
        else
            WRITE_PROTECTION_REGION(sel, pr_write, PRBAR12_EL2, PRLAR12_EL2);
        break;
    case 13:
        if ( read )
            *pr_read = READ_PROTECTION_REGION(sel, PRBAR13_EL2, PRLAR13_EL2);
        else
            WRITE_PROTECTION_REGION(sel, pr_write, PRBAR13_EL2, PRLAR13_EL2);
        break;
    case 14:
        if ( read )
            *pr_read = READ_PROTECTION_REGION(sel, PRBAR14_EL2, PRLAR14_EL2);
        else
            WRITE_PROTECTION_REGION(sel, pr_write, PRBAR14_EL2, PRLAR14_EL2);
        break;
    case 15:
        if ( read )
            *pr_read = READ_PROTECTION_REGION(sel, PRBAR15_EL2, PRLAR15_EL2);
        else
            WRITE_PROTECTION_REGION(sel, pr_write, PRBAR15_EL2, PRLAR15_EL2);
        break;
    }

    return;
}

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


void * __init early_fdt_map(paddr_t fdt_paddr)
{
    void *fdt_virt;
    uint32_t size;
    paddr_t fdt_end;

    /*
     * For MPU systems, the physical FDT address must meet two alignment
     * requirements:
     * 1. At least 8 bytes so that we always access the magic and size
     *    fields of the FDT header after mapping the first chunk.
     * 2. Meet the requirement of MPU region address alignment (64 bytes).
     */
    BUILD_BUG_ON( MIN_FDT_ALIGN < 8 || MPU_REGION_ALIGN % MIN_FDT_ALIGN );
    if ( !fdt_paddr || fdt_paddr % MPU_REGION_ALIGN )
        return NULL;

    /*
     * Map FDT with one new MPU protection region of MAX_FDT_SIZE.
     * After that, we can do some magic check.
     */
    fdt_end = round_pgup(fdt_paddr + MAX_FDT_SIZE) - 1;
    boot_mpumap[next_xen_mpumap_index] = pr_of_xenaddr(fdt_paddr, fdt_end,
                                                       MT_NORMAL);
    boot_mpumap[next_xen_mpumap_index].base.reg.ap = AP_RO_EL2;
    access_protection_region(false, NULL,
                             (const pr_t*)(&boot_mpumap[next_xen_mpumap_index]),
                             next_xen_mpumap_index);
    set_bit(next_xen_mpumap_index, xen_mpumap_mask);
    next_xen_mpumap_index++;
    nr_xen_mpumap++;

    /* VA == PA */
    fdt_virt = (void *)fdt_paddr;

    if ( fdt_magic(fdt_virt) != FDT_MAGIC )
        return NULL;

    size = fdt_totalsize(fdt_virt);
    if ( size > MAX_FDT_SIZE )
        return NULL;

    return fdt_virt;
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
