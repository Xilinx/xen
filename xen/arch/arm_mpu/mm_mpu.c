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

/* Maximum number of supported MPU protection regions by the EL2 MPU. */
unsigned long max_xen_mpumap;

/* Xen stage 1 MPU memory region configuration. */
pr_t *xen_mpumap;

struct page_info* frame_table;

/*
 * It stores statically configured system resource, through "mpu,xxx" device
 * tree property.
 */
struct mpuinfo mpuinfo;

/*
 * Number of MPU protection regions which need to be unmapped when context
 * switching to guest mode, from idle VCPU in hypervisor mode.
*/
unsigned long nr_unmapped_xen_mpumap;

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

static pr_t *get_mpu_region(paddr_t addr, size_t len)
{
    unsigned int i = 0;
    paddr_t start, end;

    for ( ; i < next_xen_mpumap_index; i++ )
    {
        start = pr_get_base(&xen_mpumap[i]);
        end = pr_get_limit(&xen_mpumap[i]);

        if ( start <= addr && addr+len-1 <= end)
            break;
    }

    if ( i == next_xen_mpumap_index )
        return NULL;
    return &xen_mpumap[i];
}

static bool is_mpu_attribute_match(pr_t *pr, unsigned attributes)
{
    bool ret = false;

    switch (attributes)
    {
    case PAGE_HYPERVISOR:
        /* PAGE_HYPERVISOR: MT_NORMAL|_PAGE_PRESENT|_PAGE_XN */
        if ( pr->base.reg.xn == XN_ENABLED && region_is_valid(pr)
             && pr->limit.reg.ai == MT_NORMAL )
            ret = true;
        else
            printk("pr->limit.reg.ai(%d) != MT_NORMAL(%d)\n",
                   pr->limit.reg.ai, MT_NORMAL);
        break;
    case PAGE_HYPERVISOR_NOCACHE:
        /* PAGE_HYPERVISOR_NOCACHE: _PAGE_XN|_PAGE_PRESENT|MT_DEVICE_nGnRE */
        if ( pr->base.reg.xn == XN_ENABLED && region_is_valid(pr)
             && pr->limit.reg.ai == MT_DEVICE_nGnRE )
            ret = true;
        else
            printk("pr->limit.reg.ai(%d) != MT_DEVICE_nGnRE(%d)\n",
                   pr->limit.reg.ai, MT_DEVICE_nGnRE);
        break;
    case PAGE_HYPERVISOR_WC:
        /* PAGE_HYPERVISOR_WC: _PAGE_XN|_PAGE_PRESENT|MT_NORMAL_NC */
        if ( pr->base.reg.xn == XN_ENABLED && region_is_valid(pr)
             && pr->limit.reg.ai == MT_NORMAL_NC )
            ret = true;
        else
            printk("pr->limit.reg.ai(%d) != MT_NORMAL_NC(%d)\n",
                   pr->limit.reg.ai, MT_NORMAL_NC);
        break;
    default:
        printk(XENLOG_ERR
               "Unrecognized attributes %04x.\n", attributes);
        break;
    }

    return ret;
}

/*
 * In MPU System, device memory shall be statically configured in
 * the very beginning, no ioremap needed.
 * But for compatibility, here prints ingoing physical address.
 */
void *ioremap_attr(paddr_t pa, size_t len, unsigned int attributes)
{
    pr_t *pr;

    pr = get_mpu_region(pa, len);
    if ( pr == NULL )
    {
        printk(XENLOG_ERR
               "IOREMAP: %#"PRIpaddr" has not been mapped in MPU!\n", pa);
        /*
         * Trigger ASSERTION to notify users that the caller IOREMAP
         * is not suitbale for it in MPU system.
         */
        ASSERT(0);
        return NULL;
    }

    if ( !is_mpu_attribute_match(pr, attributes) )
    {
        printk(XENLOG_ERR
               "IOREMAP: %#"PRIpaddr" attributes mis-matched!\n", pa);
        ASSERT(0);
        return NULL;
    }

    return (void *)pa;
}

void disable_mpu_region_from_index(unsigned int index)
{
    pr_t pr = {};

    /* Read the according MPU memory region based on index. */
    access_protection_region(true, &pr, NULL, index);
    if ( !IS_PR_ENABLED(&pr) )
    {
        printk(XENLOG_WARNING
               "mpu: MPU protection region %u is already disabled.\n", index);
        return;
    }

    /*
     * ARM64v8R provides PRENR_EL2 to disable the EL2 MPU Protection
     * region(0 - 31), but only for the previous 32 ones.
     */
    if ( index < MPU_PRENR_BITS )
    {
        register_t orig, after;

        orig = READ_SYSREG(PRENR_EL2);
        /* Set respective bit 0 to disable. */
        after = orig & (~(1UL << index));
        WRITE_SYSREG(after, PRENR_EL2);
    }
    else
    {
        pr.limit.reg.en = 0;
        access_protection_region(false, NULL, (const pr_t*)&pr, index);
    }
}

/*
 * This helper is only for disabling MPU Protection Region accountable
 * for XEN itself stage 1 MPU memory region mapping.
 */
static int disable_xen_mpu_region(paddr_t s, paddr_t e)
{
    unsigned int i = 0;

    /*
     * Find requested MPU Protection Region based on base and
     * limit address.
     */
    for ( ; i < next_xen_mpumap_index; i++ )
    {
        if ( (pr_get_base(&xen_mpumap[i]) == s) &&
             (pr_get_limit(&xen_mpumap[i]) == e) )
            break;
    }

    if ( i == next_xen_mpumap_index )
    {
        printk(XENLOG_ERR
               "mpu: can't find requested MPU Protection Region %#"PRIpaddr"-%#"PRIpaddr".\n",
               s, e);
        return -ENOENT;
    }

    disable_mpu_region_from_index(i);

    /*
     * Do not forget to clear the according MPU memory region
     * in xen_mpumap, and also according bitfield in xen_mpumap_mask.
     */
    memset(&xen_mpumap[i], 0, sizeof(pr_t));
    clear_bit(i, xen_mpumap_mask);
    nr_xen_mpumap--;

    return i;
}

int destroy_xen_mappings(unsigned long s, unsigned long e)
{
    ASSERT(s <= e);

    return disable_xen_mpu_region(s, e);
}

void __init map_boot_module_section(void)
{
    unsigned int i = 0;
    for ( ; i < mpuinfo.sections[MSINFO_BOOT].nr_banks; i++ )
    {
        paddr_t start = round_pgup(
                        mpuinfo.sections[MSINFO_BOOT].bank[i].start);
        paddr_t size = mpuinfo.sections[MSINFO_BOOT].bank[i].size;
        paddr_t end = round_pgdown(start + size) - 1;

        /* Normal memory with only EL2 Read/Write. */
        ASSERT(next_xen_mpumap_index < max_xen_mpumap);
        xen_mpumap[next_xen_mpumap_index] = pr_of_xenaddr(start, end,
                                                          MT_NORMAL);
        access_protection_region(false, NULL,
                            (const pr_t*)(&xen_mpumap[next_xen_mpumap_index]),
                            next_xen_mpumap_index);
        set_bit(next_xen_mpumap_index, xen_mpumap_mask);
        next_xen_mpumap_index++;
        nr_xen_mpumap++;
    }
}

/*
 * In MPU system, device memory shall be statically configured through
 * "mpu,device-memory-section" in device tree.
 * Instead of providing a MPU protection region each time parsing a device
 * in the system, this method helps us use as few MPU protection regions as
 * possible.
 */
static void map_device_memory_section(pr_t *mpu, unsigned long *mpu_index)
{
    unsigned int i = 0;

    for ( ; i < mpuinfo.sections[MSINFO_DEVICE].nr_banks; i++ )
    {
        paddr_t start = round_pgup(
                        mpuinfo.sections[MSINFO_DEVICE].bank[i].start);
        paddr_t size = mpuinfo.sections[MSINFO_DEVICE].bank[i].size;
        paddr_t end = round_pgdown(start + size) - 1;

        ASSERT(*mpu_index < max_xen_mpumap);
        mpu[*mpu_index] = pr_of_xenaddr(start, end, MT_DEVICE_nGnRE);
        access_protection_region(false, NULL,
                                 (const pr_t*)(&mpu[*mpu_index]),
                                 *mpu_index);
        (*mpu_index)++;
    }
}

/* Map device memory during system boot-up. */
static void __init map_device_memory_section_on_boot(void)
{
    unsigned int i = 0, tail;

#ifdef CONFIG_EARLY_PRINTK
    /* Destroy device memory mapping at early boot. */
    destroy_xen_mappings(CONFIG_EARLY_UART_BASE_ADDRESS,
                         CONFIG_EARLY_UART_BASE_ADDRESS + EARLY_UART_SIZE - 1);
#endif

    map_device_memory_section(xen_mpumap, &next_xen_mpumap_index);

    /*
     * Set recording bit in xen_mpumap_mask.
     * Device memory section need to be reordered to the tail of xen_mpumap
     * at the end of boot-up.
     */
    tail = next_xen_mpumap_index - 1;
    for ( ; i < mpuinfo.sections[MSINFO_DEVICE].nr_banks; i++ )
        set_bit(tail - i, xen_mpumap_mask);

    nr_xen_mpumap += mpuinfo.sections[MSINFO_DEVICE].nr_banks;

    /*
     * Unmap device memory section when context switching from
     * hypervisor mode of idle VCPU.
     */
    nr_unmapped_xen_mpumap += mpuinfo.sections[MSINFO_DEVICE].nr_banks;
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

void __init setup_frametable_mappings(paddr_t ps, paddr_t pe)
{
    mfn_t base_mfn;
    unsigned long nr_pdxs = mfn_to_pdx(mfn_add(maddr_to_mfn(pe), -1)) -
                            mfn_to_pdx(maddr_to_mfn(ps)) + 1;
    unsigned long frametable_size = nr_pdxs * sizeof(struct page_info);

    /* Calculate base pdx from physical start address */
    frametable_base_pdx = mfn_to_pdx(maddr_to_mfn(ps));
    frametable_size = ROUNDUP(frametable_size, PAGE_SIZE);
    base_mfn = alloc_boot_pages(frametable_size >> PAGE_SHIFT, 1);
    /* VA == PA */
    frame_table = (struct page_info*)(mfn_x(base_mfn) << PAGE_SHIFT);

    memset(&frame_table[0], 0, nr_pdxs * sizeof(struct page_info));
    memset(&frame_table[nr_pdxs], -1,
           frametable_size - (nr_pdxs * sizeof(struct page_info)));
}

/* In MPU system, Xen heap must be statically allocated. */
void __init setup_directmap_mappings(unsigned long base_mfn,
                                     unsigned long nr_mfns)
{
    /* No directmap mapping on MPU system */
    BUG();
}

void __init setup_staticheap_mappings(void)
{
    unsigned int bank = 0;

    for ( ; bank < bootinfo.reserved_mem.nr_banks; bank++ )
    {
        if ( bootinfo.reserved_mem.bank[bank].type == MEMBANK_STATIC_HEAP )
        {
            paddr_t bank_start = round_pgup(
                                 bootinfo.reserved_mem.bank[bank].start);
            paddr_t bank_size = bootinfo.reserved_mem.bank[bank].size;
            paddr_t bank_end = round_pgdown(bank_start + bank_size);

            boot_mpumap[next_xen_mpumap_index] = pr_of_xenaddr(bank_start,
                                                 bank_end - 1, MT_NORMAL);
            access_protection_region(false, NULL,
                                     (const pr_t*)(&boot_mpumap[next_xen_mpumap_index]),
                                     next_xen_mpumap_index);
            set_bit(next_xen_mpumap_index, xen_mpumap_mask);
            next_xen_mpumap_index++;
            nr_xen_mpumap++;
        }
    }
}

/* Standard entry to dynamically allocate Xen MPU memory region map. */
pr_t *alloc_mpumap(void)
{
    pr_t *map;

    /*
     * One pr_t structure takes 16 bytes, even with maximum supported MPU
     * protection regions, 256, the whole EL2 MPU map at most takes up
     * 4KB(one page-size).
     */
    map = alloc_xenheap_pages(0, 0);
    if ( map == NULL )
        return NULL;

    clear_page(map);
    return map;
}

/*
 * Relocate Xen MPU map in XEN heap based on the maximum supported
 * MPU protection regions in EL2, which is read from MPUIR_EL2 register.
 */
static int __init relocate_xen_mpumap(void)
{
    /*
     * MPUIR_EL2 identifies the maximum supported MPU protection regions by
     * the EL2 MPU.
     */
    max_xen_mpumap = READ_SYSREG(MPUIR_EL2);
    ASSERT(max_xen_mpumap <= 256);

    xen_mpumap = alloc_mpumap();
    if ( !xen_mpumap )
        return -EINVAL;

    copy_from_paddr(xen_mpumap, (paddr_t)(pr_t *)boot_mpumap,
                    sizeof(pr_t) * next_xen_mpumap_index);

    return 0;
}

void __init update_mm(void)
{
    if ( relocate_xen_mpumap() )
        panic("Failed to relocate MPU configuration map from heap!\n");

    map_device_memory_section_on_boot();

    map_boot_module_section();
}
