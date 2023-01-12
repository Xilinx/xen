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
#include <asm/armv8r/mpu.h>
#include <asm/armv8r/sysregs.h>

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
 * Using a bitmap here to record these MPU protection regions,
 * which are needed to be reordered to the tail of xen_mpumap.
 */
static DECLARE_BITMAP(reordered_mask, MAX_MPU_PROTECTION_REGIONS);

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

/* Per-PCPU runtime Xen itself stage 1 MPU memory region configuration. */
DEFINE_PER_CPU(pr_t *, cpu_mpumap);
DEFINE_PER_CPU(unsigned long, nr_cpu_mpumap);

/* Number of EL1 MPU regions supported by the hardware */
uint8_t __read_mostly mpu_regions_count_el1;

/*
 * Access MPU protection region, including both read/write operations.
 * AArch64-v8R/AArch32-v8R at most supports 256 MPU protection regions.
 * As explained from section G1.3.18 of the reference manual for AArch64-v8R
 * and from section E2.2.10 of the reference manual for AArch32-v8R,
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

static void clear_boot_mpumap(void)
{
    /*
     * Clear the copy of the boot mpu mapping. Each secondary CPU
     * rebuilds these itself (see head.S).
     */
    memset((void *)(boot_mpumap), 0,
           sizeof(pr_t) * ARM_DEFAULT_MPU_PROTECTION_REGIONS);
    clean_and_invalidate_dcache_va_range((void *)(boot_mpumap),
                                         sizeof(pr_t) *
                                         ARM_DEFAULT_MPU_PROTECTION_REGIONS);
}

int init_secondary_protection_regions(int cpu)
{
    clear_boot_mpumap();

    /* All CPUs share a single Xen stage 1 MPU memory region configuration. */
    clean_dcache_va_range((void *)&next_xen_mpumap_index,
                          sizeof(unsigned long));
    clean_dcache_va_range((void *)(pr_t *)xen_mpumap,
                          sizeof(pr_t) * next_xen_mpumap_index);
    return 0;
}

/*
 * Below functions need MPU-specific implementation.
 * TODO: Implementation on first usage.
 */
void __init remove_early_mappings(void)
{
}

int map_pages_to_xen(unsigned long virt,
                     mfn_t mfn,
                     unsigned long nr_mfns,
                     unsigned int flags)
{
    return 0;
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

/* Only support modifying permission on the existing XEN MPU Memory Region. */
int modify_xen_mappings(unsigned long s, unsigned long e, unsigned int flags)
{
    unsigned int i = 0;

    /*
     * Find requested MPU protection region based on base and
     * limit address.
     */
    for ( ; i < nr_xen_mpumap; i++ )
    {
        if ( (pr_get_base(&xen_mpumap[i]) == s) &&
             (pr_get_limit(&xen_mpumap[i]) == e) )
            break;
    }

    if ( i == nr_xen_mpumap )
    {
        printk("Error: can't find requested mpu protection region\n");
        return -ENOENT;
    }

    if ( region_is_valid(&xen_mpumap[i]) )
    {
        /* Set permission */
        if ( REGION_RO_MASK(flags) )
            xen_mpumap[i].base.reg.ap = AP_RO_EL2;
        else
            xen_mpumap[i].base.reg.ap = AP_RW_EL2;

        if ( REGION_XN_MASK(flags) )
            xen_mpumap[i].base.reg.xn = XN_ENABLED;
        else
            xen_mpumap[i].base.reg.xn = XN_DISABLED;

        access_protection_region(false, NULL,
                                 (const pr_t*)(&xen_mpumap[i]), (u64)i);
    }
    else
        return -EINVAL;

    return 0;
}

/*
 * Firstly, all domains on MPU system must be statically allocated.
 * Then, due to limited MPU protection regions, we do not let user scatter
 * statically-configured guest RAM anywhere they want.
 * "mpu,guest-memory-section" is requested to describe limited guest memory
 * sections, and later all statically-configured guest RAM must be placed
 * inside guest memory section.
 */
static void map_guest_memory_section(pr_t* mpu, unsigned long nr_max,
                                     unsigned long *mpu_index)
{
    unsigned int i = 0;

    for ( ; i < mpuinfo.sections[MSINFO_GUEST].nr_banks; i++ )
    {
        paddr_t start = round_pgup(
                        mpuinfo.sections[MSINFO_GUEST].bank[i].start);
        paddr_t size = mpuinfo.sections[MSINFO_GUEST].bank[i].size;
        paddr_t end = round_pgdown(start + size) - 1;

        /* Normal memory with only EL2 Read/Write. */
        ASSERT(*mpu_index < nr_max);
        mpu[*mpu_index] = pr_of_xenaddr(start, end, MT_NORMAL);
        access_protection_region(false, NULL,
                                 (const pr_t*)(&mpu[*mpu_index]),
                                 *mpu_index);
        (*mpu_index)++;
    }
}

void __init map_guest_memory_section_on_boot(void)
{
    unsigned long tail, i = 0;

    map_guest_memory_section(boot_mpumap,
                             ARM_DEFAULT_MPU_PROTECTION_REGIONS,
                             &next_xen_mpumap_index);

    /*
     * Set recording bit in xen_mpumap_mask.
     * Guest memory section need to be reordered to the tail of xen_mpumap
     * at the end of boot-up.
     */
    tail = next_xen_mpumap_index - 1;
    for ( ; i < mpuinfo.sections[MSINFO_GUEST].nr_banks; i++ )
    {
        set_bit(tail - i, xen_mpumap_mask);
        set_bit(tail - i, reordered_mask);
    }
    nr_xen_mpumap += mpuinfo.sections[MSINFO_GUEST].nr_banks;

    /*
     * Unmap guest memory section when context switching from
     * hypervisor mode of idle VCPU.
     */
    nr_unmapped_xen_mpumap += mpuinfo.sections[MSINFO_GUEST].nr_banks;
}

static void map_guest_memory_section_on_ctxt(void)
{
    map_guest_memory_section(THIS_CPU_MPUMAP,
                             max_xen_mpumap, &(THIS_CPU_NR_MPUMAP));
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
    {
        set_bit(tail - i, xen_mpumap_mask);
        set_bit(tail - i, reordered_mask);
    }
    nr_xen_mpumap += mpuinfo.sections[MSINFO_DEVICE].nr_banks;

    /*
     * Unmap device memory section when context switching from
     * hypervisor mode of idle VCPU.
     */
    nr_unmapped_xen_mpumap += mpuinfo.sections[MSINFO_DEVICE].nr_banks;
}

/* Map device memory on context switch. */
static void map_device_memory_section_on_ctxt(void)
{
    map_device_memory_section(THIS_CPU_MPUMAP, &(THIS_CPU_NR_MPUMAP));
}

/*
 * When context switching from hypervisor mode of idle VCPU, some MPU
 * Protection Region need to be unmapped, to avoid overlapping, such as
 * if the target is VCPU of a domain, P2M mapping of domain RAM will overlap
 * with the guest memory section, which contains all guest RAM mapping in
 * EL2.
 *
 * Right now, only guest memory section and device memory dection are
 * taken into consideration
 * .
 * Be aware that all need-to-unmap MPU Protection Regions are added to the
 * tail.
 */
void unmap_xen_mpumap_on_ctxt(void)
{
    unsigned int tail = THIS_CPU_NR_MPUMAP - 1;
    unsigned int i = 0;

    for ( ; i < nr_unmapped_xen_mpumap; i++ )
    {
        disable_mpu_region_from_index(tail - i);
        THIS_CPU_NR_MPUMAP--;
    }
}

void map_xen_mpumap_on_ctxt(void)
{
    map_guest_memory_section_on_ctxt();
    map_device_memory_section_on_ctxt();
}

void free_mpumap(pr_t *mpu)
{
    free_xenheap_page(mpu);
}

/*
 * reordered_mpu value needs to be configured by all CPUs.
 * Set only once by the boot CPU.
 */
static pr_t *reordered_mpu;
static unsigned long reordered_mpu_index = 0;

/*
 * MPU must be disabled to swap in the new MPU memory region
 * configuration.
 * Helper clear_xen_mpumap is to help totally flush the stale
 * config, through setting zero value to original #next_xen_mpumap_index
 * MPU protection Region.
 */
void reorder_xen_mpumap_one(void *data)
{
    disable_mpu();
    clear_xen_mpumap(next_xen_mpumap_index);
    set_boot_mpumap(reordered_mpu_index, (pr_t *)reordered_mpu);
    enable_mpu();

    /*
     * When users disable DEBUG option, some compilers' optimization will
     * use 'ret' in enable_mpu for reorder_xen_mpumap_one directly.
     * The side affect is that, LR will be populated from stack before
     * calling enable_mpu. But LR had been pushed to stack before
     * calling disable_mpu. In this case, LR push and pop behaviors
     * handle with different cache state stack, so the data might be corrupt.
     *
     * The isb will force compiler to generate a 'ret' for
     * reorder_xen_mpumap_one, and the LR pop will happen after calling
     * enable_mpu.
     */
    isb();
}

/*
 * A few MPU memory regions need unmapping on context switch, so it's
 * better to put unchaging ones in the front tier of xen_mpumap, and
 * changing ones, like guest memory section and device memory section in
 * the rear, for the sake of saving trouble and cost in time sensitive context
 * switch.
 */
int reorder_xen_mpumap(void)
{
    unsigned int i, j;

    /* Allocate space for new reordered_mpu. */
    reordered_mpu = alloc_mpumap();
    if ( !reordered_mpu )
        return -ENOMEM;

    /* Firstly, copy the unchaging ones in the front. */
    for_each_set_bit( i, (const unsigned long *)&xen_mpumap_mask,
                      MAX_MPU_PROTECTION_REGIONS )
    {
        /*
         * If current one needs to be reordered to the rear,
         * neglect here.
         */
        if ( test_bit(i, reordered_mask) )
            continue;

        reordered_mpu[reordered_mpu_index++] = xen_mpumap[i];
    }

    /* Add the ones which need to be reordered in the tail. */
    for_each_set_bit( j, (const unsigned long *)&reordered_mask,
                      MAX_MPU_PROTECTION_REGIONS )
        reordered_mpu[reordered_mpu_index++] = xen_mpumap[j];

    clean_dcache_va_range((void *)&reordered_mpu_index,
                          sizeof(unsigned long));
    clean_dcache_va_range((void *)&next_xen_mpumap_index,
                          sizeof(unsigned long));
    clean_dcache_va_range((void *)(pr_t *)reordered_mpu,
                          sizeof(pr_t) * reordered_mpu_index);

    reorder_xen_mpumap_one(NULL);
    smp_call_function(reorder_xen_mpumap_one, NULL, 1);

    /* Now, xen_mpumap acts in a tight way, absolutely no holes in there,
     * then always next_xen_mpumap_index = nr_xen_mpumap for later on.
     */
    next_xen_mpumap_index = nr_xen_mpumap = reordered_mpu_index;
    free_mpumap(xen_mpumap);
    xen_mpumap = reordered_mpu;

    printk(XENLOG_DEBUG
           "Xen Stage 1 MPU memory region mapping in EL2.\n");
    for ( i = 0; i < nr_xen_mpumap; i++ )
    {
        pr_t region;
        access_protection_region(true, &region, NULL, i);
        printk(XENLOG_DEBUG
               "MPU protection region #%u : 0x%"PRIregister" - 0x%"PRIregister".\n",
               i, pr_get_base(&region), pr_get_limit(&region));
    }

    return 0;
}

int xenmem_add_to_physmap_one(struct domain *d, unsigned int space,
                              union add_to_physmap_extra extra,
                              unsigned long idx, gfn_t gfn)
{
    return 0;
}

long arch_memory_op(int op, XEN_GUEST_HANDLE_PARAM(void) arg)
{
    return -ENOSYS;
}

void dump_hyp_walk(vaddr_t addr)
{
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
#ifdef CONFIG_ARM_64
    WRITE_SYSREG(READ_SYSREG(SCTLR_EL2) | SCTLR_Axx_ELx_WXN, SCTLR_EL2);
#endif
}

void mpu_init_secondary_cpu(void)
{
    xen_mpu_enforce_wnx();
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
    disable_mpu();

    /*
     * Set new MPU memory region configuration.
     * To avoid the mismatch between nr_xen_mpumap and nr_xen_mpumap
     * after the relocation of some MPU regions later, here
     * next_xen_mpumap_index is used.
     * To avoid unexpected unaligment access fault during MPU disabled,
     * set_boot_mpumap shall be written in assembly code.
     */
    set_boot_mpumap(next_xen_mpumap_index, (pr_t *)boot_mpumap);

    enable_mpu();

    xen_mpu_enforce_wnx();

    if ( IS_ENABLED(CONFIG_DEBUG) )
        for ( unsigned int i = 0; i < nr_xen_mpumap; i++ )
        {
            pr_t region;
            access_protection_region(true, &region, NULL, i);
            printk("Boot-time Xen MPU memory configuration. #%u : 0x%"PRIregister" - 0x%"PRIregister".\n",
                   i, pr_get_base(&region), pr_get_limit(&region));
        }
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

    clear_boot_mpumap();

    return 0;
}

void __init update_mm(void)
{
    if ( relocate_xen_mpumap() )
        panic("Failed to relocate MPU configuration map from heap!\n");

    map_device_memory_section_on_boot();

    map_boot_module_section();
}

static DECLARE_BITMAP(initial_section_mask, MAX_MPU_PROTECTION_REGIONS);
static void free_init_memory_one(void *data)
{
    unsigned int i;

    for_each_set_bit( i, (const unsigned long *)&initial_section_mask,
                      MAX_MPU_PROTECTION_REGIONS )
        disable_mpu_region_from_index(i);
}

void free_init_memory(void)
{
    /* Kernel init text section. */
    paddr_t init_text = get_kernel_inittext_start();
    /* In xen.lds.S, section is page-aligned. */
    paddr_t init_text_end = round_pgup(get_kernel_inittext_end()) - 1;

    /* Kernel init data. */
    paddr_t init_data = get_kernel_initdata_start();
    /* In xen.lds.S, section is page-aligned. */
    paddr_t init_data_end = round_pgup(get_kernel_initdata_end()) - 1;

    unsigned long init_section[4] = {init_text, init_text_end, init_data, init_data_end};
    unsigned int nr_init = 2;
    uint32_t insn = AARCH64_BREAK_FAULT;
    unsigned int i = 0, j = 0;

    /* Change memory attribute of kernel init text section to RW. */
    modify_xen_mappings(init_text, init_text_end, REGION_HYPERVISOR_RW);

    /*
     * From now on, init will not be used for execution anymore,
     * so nuke the instruction cache to remove entries related to init.
     */
    invalidate_icache_local();

    /* Remove both two init sections: init code and init data. */
    for ( ; i < nr_init; i++ )
    {
        uint32_t *p;
        unsigned int nr;
        int rc;

        i = 2 * i;
        p = (uint32_t *)init_section[i];
        nr = ((init_section[i + 1] + 1) - init_section[i]) / sizeof(uint32_t);

        for ( ; j < nr ; j++ )
            *(p + j) = insn;

        rc = destroy_xen_mappings(init_section[i], init_section[i + 1]);
        if ( rc < 0 )
            panic("Unable to remove the init section (rc = %d)\n", rc);
        set_bit(rc, initial_section_mask);
    }

    smp_call_function(free_init_memory_one, NULL, 1);
}

/* Loads and returns the number of EL1 MPU supported regions by the hardware */
uint8_t __init load_mpu_supported_region_el1(void)
{
    uint8_t reg_value = (uint8_t)(READ_SYSREG(MPUIR_EL1) & MPUIR_REGION_MASK);
    /*
     * Returns the number of supported MPU regions for EL1 from MPUIR_EL1
     * register and also writes the mpu_regions_count_el1 variable value.
     */
    mpu_regions_count_el1 = reg_value;

    return reg_value;
}

/*
 * Save EL1 MPU base registers and limit registers.
 * As explained from section G1.3.18 of the reference manual for Armv8-R,
 * PRBAR<n>_ELx and PRLAR<n>_ELx provides access to the MPU region
 * determined by the 4 most significant bit written on PRSELR_ELx.REGION and
 * the <n> number from 1 to 15, when n == 0 then PRBAR_ELx should be used.
 * So for example to access regions from 16 to 31 (0b10000 to 0b11111):
 *  - Set PRSELR_ELx to 0b10000
 *  - Region 16 configuration is accessible through PRBAR_ELx and PRLAR_ELx
 *  - Region 17 configuration is accessible through PRBAR1_ELx and PRLAR1_ELx
 *  - Region 18 configuration is accessible through PRBAR2_ELx and PRLAR2_ELx
 *  - ...
 *  - Region 31 configuration is accessible through PRBAR15_ELx and PRLAR15_ELx
 */
void save_el1_mpu_regions(pr_t *pr)
{
    int sel = mpu_regions_count_el1 - 1;

    if ( mpu_regions_count_el1 == 0 )
        return;

    while (sel > 0)
    {
        WRITE_SYSREG( (sel & 0xF0), PRSELR_EL1);
        isb();
        switch (sel & 0xF) {
            case 15:
                pr[sel].base.bits = READ_SYSREG(PRBAR15_EL1);
                pr[sel].limit.bits = READ_SYSREG(PRLAR15_EL1);
                sel--;
                fallthrough;
            case 14:
                pr[sel].base.bits = READ_SYSREG(PRBAR14_EL1);
                pr[sel].limit.bits = READ_SYSREG(PRLAR14_EL1);
                sel--;
                fallthrough;
            case 13:
                pr[sel].base.bits = READ_SYSREG(PRBAR13_EL1);
                pr[sel].limit.bits = READ_SYSREG(PRLAR13_EL1);
                sel--;
                fallthrough;
            case 12:
                pr[sel].base.bits = READ_SYSREG(PRBAR12_EL1);
                pr[sel].limit.bits = READ_SYSREG(PRLAR12_EL1);
                sel--;
                fallthrough;
            case 11:
                pr[sel].base.bits = READ_SYSREG(PRBAR11_EL1);
                pr[sel].limit.bits = READ_SYSREG(PRLAR11_EL1);
                sel--;
                fallthrough;
            case 10:
                pr[sel].base.bits = READ_SYSREG(PRBAR10_EL1);
                pr[sel].limit.bits = READ_SYSREG(PRLAR10_EL1);
                sel--;
                fallthrough;
            case 9:
                pr[sel].base.bits = READ_SYSREG(PRBAR9_EL1);
                pr[sel].limit.bits = READ_SYSREG(PRLAR9_EL1);
                sel--;
                fallthrough;
            case 8:
                pr[sel].base.bits = READ_SYSREG(PRBAR8_EL1);
                pr[sel].limit.bits = READ_SYSREG(PRLAR8_EL1);
                sel--;
                fallthrough;
            case 7:
                pr[sel].base.bits = READ_SYSREG(PRBAR7_EL1);
                pr[sel].limit.bits = READ_SYSREG(PRLAR7_EL1);
                sel--;
                fallthrough;
            case 6:
                pr[sel].base.bits = READ_SYSREG(PRBAR6_EL1);
                pr[sel].limit.bits = READ_SYSREG(PRLAR6_EL1);
                sel--;
                fallthrough;
            case 5:
                pr[sel].base.bits = READ_SYSREG(PRBAR5_EL1);
                pr[sel].limit.bits = READ_SYSREG(PRLAR5_EL1);
                sel--;
                fallthrough;
            case 4:
                pr[sel].base.bits = READ_SYSREG(PRBAR4_EL1);
                pr[sel].limit.bits = READ_SYSREG(PRLAR4_EL1);
                sel--;
                fallthrough;
            case 3:
                pr[sel].base.bits = READ_SYSREG(PRBAR3_EL1);
                pr[sel].limit.bits = READ_SYSREG(PRLAR3_EL1);
                sel--;
                fallthrough;
            case 2:
                pr[sel].base.bits = READ_SYSREG(PRBAR2_EL1);
                pr[sel].limit.bits = READ_SYSREG(PRLAR2_EL1);
                sel--;
                fallthrough;
            case 1:
                pr[sel].base.bits = READ_SYSREG(PRBAR1_EL1);
                pr[sel].limit.bits = READ_SYSREG(PRLAR1_EL1);
                sel--;
                fallthrough;
            case 0:
                pr[sel].base.bits = READ_SYSREG(PRBAR_EL1);
                pr[sel].limit.bits = READ_SYSREG(PRLAR_EL1);
                sel--;
        }
        isb();
    }
}

/* Restore EL1 MPU base registers and limit registers. */
void restore_el1_mpu_regions(pr_t *pr)
{
    int sel = mpu_regions_count_el1 - 1;

    if ( mpu_regions_count_el1 == 0 )
        return;

    while (sel > 0)
    {
        dsb(sy);
        WRITE_SYSREG( (sel & 0xF0), PRSELR_EL1);
        isb();
        switch (sel & 0xF) {
            case 15:
                WRITE_SYSREG(pr[sel].base.bits, PRBAR15_EL1);
                WRITE_SYSREG(pr[sel].limit.bits, PRLAR15_EL1);
                sel--;
                fallthrough;
            case 14:
                WRITE_SYSREG(pr[sel].base.bits, PRBAR14_EL1);
                WRITE_SYSREG(pr[sel].limit.bits, PRLAR14_EL1);
                sel--;
                fallthrough;
            case 13:
                WRITE_SYSREG(pr[sel].base.bits, PRBAR13_EL1);
                WRITE_SYSREG(pr[sel].limit.bits, PRLAR13_EL1);
                sel--;
                fallthrough;
            case 12:
                WRITE_SYSREG(pr[sel].base.bits, PRBAR12_EL1);
                WRITE_SYSREG(pr[sel].limit.bits, PRLAR12_EL1);
                sel--;
                fallthrough;
            case 11:
                WRITE_SYSREG(pr[sel].base.bits, PRBAR11_EL1);
                WRITE_SYSREG(pr[sel].limit.bits, PRLAR11_EL1);
                sel--;
                fallthrough;
            case 10:
                WRITE_SYSREG(pr[sel].base.bits, PRBAR10_EL1);
                WRITE_SYSREG(pr[sel].limit.bits, PRLAR10_EL1);
                sel--;
                fallthrough;
            case 9:
                WRITE_SYSREG(pr[sel].base.bits, PRBAR9_EL1);
                WRITE_SYSREG(pr[sel].limit.bits, PRLAR9_EL1);
                sel--;
                fallthrough;
            case 8:
                WRITE_SYSREG(pr[sel].base.bits, PRBAR8_EL1);
                WRITE_SYSREG(pr[sel].limit.bits, PRLAR8_EL1);
                sel--;
                fallthrough;
            case 7:
                WRITE_SYSREG(pr[sel].base.bits, PRBAR7_EL1);
                WRITE_SYSREG(pr[sel].limit.bits, PRLAR7_EL1);
                sel--;
                fallthrough;
            case 6:
                WRITE_SYSREG(pr[sel].base.bits, PRBAR6_EL1);
                WRITE_SYSREG(pr[sel].limit.bits, PRLAR6_EL1);
                sel--;
                fallthrough;
            case 5:
                WRITE_SYSREG(pr[sel].base.bits, PRBAR5_EL1);
                WRITE_SYSREG(pr[sel].limit.bits, PRLAR5_EL1);
                sel--;
                fallthrough;
            case 4:
                WRITE_SYSREG(pr[sel].base.bits, PRBAR4_EL1);
                WRITE_SYSREG(pr[sel].limit.bits, PRLAR4_EL1);
                sel--;
                fallthrough;
            case 3:
                WRITE_SYSREG(pr[sel].base.bits, PRBAR3_EL1);
                WRITE_SYSREG(pr[sel].limit.bits, PRLAR3_EL1);
                sel--;
                fallthrough;
            case 2:
                WRITE_SYSREG(pr[sel].base.bits, PRBAR2_EL1);
                WRITE_SYSREG(pr[sel].limit.bits, PRLAR2_EL1);
                sel--;
                fallthrough;
            case 1:
                WRITE_SYSREG(pr[sel].base.bits, PRBAR1_EL1);
                WRITE_SYSREG(pr[sel].limit.bits, PRLAR1_EL1);
                sel--;
                fallthrough;
            case 0:
                WRITE_SYSREG(pr[sel].base.bits, PRBAR_EL1);
                WRITE_SYSREG(pr[sel].limit.bits, PRLAR_EL1);
                sel--;
        }
        isb();
    }
}
