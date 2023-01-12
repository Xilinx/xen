/*
 * xen/arch/arm_mpu/p2m_mpu.c
 *
 * P2M code for MPU systems.
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
#include <xen/cpu.h>
#include <xen/sched.h>
#include <xen/warning.h>
#include <asm/armv8r/mpu.h>
#include <asm/p2m.h>

/* VTCR_EL2 value to be configured for the boot CPU. */
static uint32_t __read_mostly vtcr;

/* Use the p2m type to check whether a region is valid. */
static inline bool p2m_is_valid(pr_t *region)
{
    return p2m_get_region_type(region) != p2m_invalid;
}

/* Return the size of the pool, rounded up to the nearest MB */
unsigned int p2m_get_allocation(struct domain *d)
{
    return 0;
}

int p2m_set_allocation(struct domain *d, unsigned long pages, bool *preempted)
{
    return 0;
}

int p2m_teardown_allocation(struct domain *d)
{
    return 0;
}

void p2m_write_unlock(struct p2m_domain *p2m)
{
    write_unlock(&p2m->lock);
}

void p2m_dump_info(struct domain *d)
{
}

void memory_type_changed(struct domain *d)
{
}

void dump_p2m_lookup(struct domain *d, paddr_t addr)
{
}

static int p2m_mpu_update(struct vcpu *v)
{
    pr_t *p2m_table;
    unsigned int i = 0;
    struct p2m_domain *p2m = p2m_get_hostp2m(v->domain);

    if ( (THIS_CPU_NR_MPUMAP + p2m->nr_regions) > max_xen_mpumap )
    {
        printk(XENLOG_ERR
               "More than maximum supported MPU protection regions!\n");
        return -EINVAL;
    }

    /* Domain MPU P2M table. */
    p2m_table = (pr_t *)page_to_virt(p2m->root);
    if ( !p2m_table )
        return -EINVAL;

    /*
     * During runtime, EL2 MPU protection region layout has a fixed
     * style, that is, Xen itself stage 1 MPU memory region mapping
     * is always in the front, containing THIS_CPU_NR_MPUMAP entries.
     * If on guest mode, domain P2M mapping followed behind.
     */
    for ( ; i < p2m->nr_regions; i++ )
        access_protection_region(false, NULL,
                                 (const pr_t*)(&p2m_table[i]),
                                 (THIS_CPU_NR_MPUMAP + i));

    return 0;
}

/* p2m_save_state and p2m_restore_state work in pair. */
void p2m_save_state(struct vcpu *p)
{
    unsigned int i = 0;
    struct p2m_domain *p2m = p2m_get_hostp2m(p->domain);

    p->arch.sctlr = READ_SYSREG(SCTLR_EL1);
#ifdef CONFIG_ARM_64
    p->arch.vtcr_el2 = READ_SYSREG(VTCR_EL2);
#endif

    /*
     * We keep the system MPU memory region map in a tight and fixed way.
     * So if in the guest mode, all the previous [0 ... nr_xen_mpumap) MPU
     * memory regions belong to XEN itself stage 1 memory mapping, and the
     * latter [nr_xen_mpumap ... nr_xen_mpumap + p2m->nr_regions) belong to
     * domain P2M stage 2 memory mapping.
     */
    for ( ; i < p2m->nr_regions; i++ )
        disable_mpu_region_from_index(THIS_CPU_NR_MPUMAP + i);
}

/* p2m_save_state and p2m_restore_state work in pair. */
void p2m_restore_state(struct vcpu *n)
{
    struct p2m_domain *p2m = p2m_get_hostp2m(n->domain);
    uint8_t *last_vcpu_ran = &p2m->last_vcpu_ran[smp_processor_id()];

    if ( is_idle_vcpu(n) )
        return;

    WRITE_SYSREG(n->arch.sctlr, SCTLR_EL1);
    WRITE_SYSREG(n->arch.hcr_el2, HCR_EL2);
#ifdef CONFIG_ARM_64
    WRITE_SYSREG(n->arch.vtcr_el2, VTCR_EL2);
#endif

    WRITE_SYSREG(p2m->vsctlr, VSCTLR_EL2);

    if ( p2m_mpu_update(n) )
        panic("Failed to update MPU protection region configuration with domain P2M mapping!");

    isb();

    *last_vcpu_ran = n->vcpu_id;
}

/*
 * Get the details of one guest memory range, [gfn, gfn + nr_gfns).
 *
 * If the range is present, only the starting MFN(mfn) will be returned
 * and also p2m type get filled up. Due to GFN == MFN on MPU system, the
 * whole physical memory range could be deduced, that is, [mfn, mfn + nr_gfns).
 *
 * If the range is not present, INVALID_MFN will be returned.
 *
 * The parameter valid will contain the value of bit[0] (e.g enable bit)
 * of the Protection Region Limit Address Register.
 */
static mfn_t p2m_get_region(struct p2m_domain *p2m, gfn_t gfn,
                            unsigned long nr_gfns, p2m_type_t *t, bool *valid)
{
    pr_t *table, *region = NULL;
    p2m_type_t _t;
    unsigned int i = 0;
    gfn_t egfn = gfn_add(gfn, nr_gfns);
    paddr_t base, limit;

    ASSERT(p2m_is_locked(p2m));

    /* Allow t to be NULL. */
    t = t ?: &_t;

    *t = p2m_invalid;

    if ( valid )
        *valid = false;

    /*
     * Check if the ending gfn is higher than the highest the p2m map
     * currently holds, or the starting gfn lower than the lowest it holds
     */
    if ( (gfn_x(egfn) > gfn_x(p2m->max_mapped_gfn)) ||
         (gfn_x(gfn) < gfn_x(p2m->lowest_mapped_gfn)) )
        return INVALID_MFN;

    /* Get base and limit address */
    base = gfn_to_gaddr(gfn);
    limit = gfn_to_gaddr(egfn) - 1;

    /* MPU P2M table. */
    table = (pr_t *)page_to_virt(p2m->root);
    /* The table should always be non-NULL and is always present. */
    if ( !table )
        ASSERT_UNREACHABLE();

    /*
     * Iterate MPU P2M table to find the region which includes this memory
     * range[base, limit].
     */
    for( ; i < p2m->nr_regions; i++ )
    {
         region = &table[i];
         if( (base >= pr_get_base(region)) && (limit <= pr_get_limit(region)) )
             break;
    }

    /* Not Found. */
    if ( i == p2m->nr_regions )
        return INVALID_MFN;

    if ( p2m_is_valid(region) )
    {
        *t = p2m_get_region_type(region);

        if ( valid )
            *valid = region_is_valid(region);
    }

    /* GFN == MFN, 1:1 direct-map in MPU system. */
    return _mfn(gfn_x(gfn));
}

struct page_info *p2m_get_region_from_gfns(struct domain *d, gfn_t gfn,
                                           unsigned long nr_gfns, p2m_type_t *t)
{
    struct page_info *page;
    p2m_type_t p2mt;
    mfn_t mfn;
    struct p2m_domain *p2m = p2m_get_hostp2m(d);
    unsigned int i = 0;

    p2m_read_lock(p2m);
    mfn = p2m_get_region(p2m, gfn, nr_gfns, &p2mt, NULL);
    p2m_read_unlock(p2m);

    if ( t )
        *t = p2mt;

    /* TODO: Add foreign mapping */
    if ( !p2m_is_ram(p2mt) )
        return NULL;

    if ( !mfn_valid(mfn) )
        return NULL;

    page = mfn_to_page(mfn);

    for ( ; i < nr_gfns; i++ )
        if ( !get_page(page + i, d) )
            return NULL;

    return page;
}

/*
 * Get the details of a given gfn.
 *
 * If the entry is present, the associated MFN will be returned and the
 * p2m type get filled up.
 *
 * The page_order is meaningless on MPU system, and keeping it here is only
 * to be compatible with MMU system.
 *
 * If the entry is not present, INVALID_MFN will be returned.
 *
 * The parameter valid will contain the value of bit[0] (e.g enable bit)
 * of the Protection Region Limit Address Register.
 */
mfn_t p2m_get_entry(struct p2m_domain *p2m, gfn_t gfn,
                    p2m_type_t *t, p2m_access_t *a,
                    unsigned int *page_order,
                    bool *valid)
{
    return p2m_get_region(p2m, gfn, 1, t, valid);
}

int guest_physmap_mark_populate_on_demand(struct domain *d,
                                          unsigned long gfn,
                                          unsigned int order)
{
    return -ENOSYS;
}

unsigned long p2m_pod_decrease_reservation(struct domain *d, gfn_t gfn,
                                           unsigned int order)
{
    return 0;
}

static void p2m_set_permission(pr_t *pr, p2m_type_t t, p2m_access_t a)
{
    /* First apply type permissions */
    /*
     * Only the following six kinds p2m_type_t are supported on the
     * mpu system right now, that is, p2m_invalid, p2m_ram_rw, p2m_ram_ro
     * p2m_max_real_type, p2m_dev_rw, p2m_mmio_direct_dev.
     * All the left will be introduced on first usage.
     */
    switch ( t )
    {
    case p2m_ram_rw:
        pr->base.reg.xn = XN_DISABLED;
        pr->base.reg.ap = AP_RW_ALL;
        break;

    case p2m_ram_ro:
        pr->base.reg.xn = XN_DISABLED;
        pr->base.reg.ap = AP_RO_ALL;
        break;

    case p2m_invalid:
        pr->base.reg.xn = XN_P2M_ENABLED;
        pr->base.reg.ap = AP_RO_ALL;
        break;

    case p2m_max_real_type:
        BUG();
        break;

    case p2m_dev_rw:
        pr->base.reg.xn = XN_P2M_ENABLED;
        pr->base.reg.ap = AP_RW_EL2;
        break;

    case p2m_mmio_direct_dev:
        pr->base.reg.xn = XN_P2M_ENABLED;
        pr->base.reg.ap = AP_RW_ALL;
        break;

    case p2m_mmio_direct_nc:
    case p2m_mmio_direct_c:
    case p2m_iommu_map_ro:
    case p2m_iommu_map_rw:
    case p2m_map_foreign_ro:
    case p2m_map_foreign_rw:
    case p2m_grant_map_ro:
    case p2m_grant_map_rw:
        printk("ERROR: UNIMPLEMENTED P2M TYPE PERMISSSON IN MPU!\n");
        BUG();
        break;
    }

    /*
     * Since mem_access is NOT in use for the domain on MPU system,
     * then it must be p2m_access_rwx.
     */
    ASSERT(a == p2m_access_rwx);
}

static inline pr_t region_to_p2m_entry(mfn_t smfn, unsigned long nr_mfn,
                                       p2m_type_t t, p2m_access_t a)
{
    prbar_t base;
    prlar_t limit;
    pr_t region;
    paddr_t base_addr, limit_addr;

    /* Build up prlar (Protection Region Limit Address Register) register. */
    limit = (prlar_t) {
        .reg = {
            .ns = 0,        /* Hyp mode is in secure world */
            .en = 1,        /* Region enabled */
        }};

    BUILD_BUG_ON(p2m_max_real_type > (1 << 4));

    /*
     * Only the following six kinds p2m_type_t are supported on the
     * mpu system right now, that is, p2m_invalid, p2m_ram_rw, p2m_ram_ro
     * p2m_max_real_type, p2m_dev_rw, and p2m_mmio_direct_dev.
     * All the left will be introduced on first usage.
     */
    switch ( t )
    {
    case p2m_invalid:
    case p2m_ram_rw:
    case p2m_ram_ro:
    case p2m_max_real_type:
        base.reg.sh = LPAE_SH_INNER;
        limit.reg.ai = MT_NORMAL;
        break;

    case p2m_dev_rw:
        limit.reg.ai = MT_DEVICE_nGnRE;
        base.reg.sh = LPAE_SH_OUTER;
        break;

    case p2m_mmio_direct_dev:
        limit.reg.ai = MT_DEVICE_nGnRE;
        base.reg.sh = LPAE_SH_OUTER;
        break;

    default:
        printk("ERROR: UNIMPLEMENTED P2M TYPE IN MPU!\n");
        BUG();
        break;
    }

    /* Build up MPU protection region. */
    region = (pr_t) {
        .base = base,
        .limit = limit,
    };

    p2m_set_region_type(&region, t);

    /*
     * xn and ap bit will be defined in the p2m_set_permission
     * based on a and t.
     */
    p2m_set_permission(&region, t, a);

    /* Set base address and limit address */
    base_addr = mfn_to_maddr(smfn);
    limit_addr = mfn_to_maddr(mfn_add(smfn, nr_mfn)) - 1;
    pr_set_base(&region, base_addr);
    pr_set_limit(&region, limit_addr);

    return region;
}

/* TODO: removing mapping (i.e MFN_INVALID). */
int p2m_set_entry(struct p2m_domain *p2m, gfn_t sgfn,
                  unsigned long nr, mfn_t smfn,
                  p2m_type_t t, p2m_access_t a)
{
    pr_t *table;
    mfn_t emfn = mfn_add(smfn, nr);

    /*
     * Other than removing mapping (i.e MFN_INVALID),
     * gfn == mfn in MPU system.
     */
    if ( !mfn_eq(smfn, INVALID_MFN) )
        ASSERT(gfn_x(sgfn) == mfn_x(smfn));

    /* MPU P2M table. */
    table = (pr_t *)page_to_virt(p2m->root);
    if ( !table )
        return -EINVAL;

    /*
     * Build up according MPU protection region and set its
     * memory attributes.
     */
    table[p2m->nr_regions] = region_to_p2m_entry(smfn, nr, t, a);
    p2m->nr_regions++;

    p2m->max_mapped_gfn = gfn_max(p2m->max_mapped_gfn, _gfn(mfn_x(emfn)));
    p2m->lowest_mapped_gfn = gfn_min(p2m->lowest_mapped_gfn, _gfn(mfn_x(smfn)));

    return 0;
}

void p2m_invalidate_root(struct p2m_domain *p2m)
{
}

bool p2m_resolve_translation_fault(struct domain *d, gfn_t gfn)
{
    printk("Unsupported resolve translation fault in MPU P2M!\n");
    return false;
}

int unmap_regions_p2mt(struct domain *d,
                       gfn_t gfn,
                       unsigned long nr,
                       mfn_t mfn)
{
    return -EINVAL;
}

int map_mmio_regions(struct domain *d,
                     gfn_t start_gfn,
                     unsigned long nr,
                     mfn_t mfn,
                     uint32_t cache_policy)
{
    return -EINVAL;
}

int unmap_mmio_regions(struct domain *d,
                       gfn_t start_gfn,
                       unsigned long nr,
                       mfn_t mfn)
{
    return -EINVAL;
}

int map_dev_mmio_page(struct domain *d,
                      gfn_t gfn,
                      mfn_t mfn)
{
    return -EINVAL;
}

int guest_physmap_add_entry(struct domain *d,
                            gfn_t gfn,
                            mfn_t mfn,
                            unsigned long page_order,
                            p2m_type_t t)
{
    return p2m_insert_mapping(d, gfn, (1 << page_order), mfn, t);
}

int guest_physmap_remove_page(struct domain *d, gfn_t gfn, mfn_t mfn,
                              unsigned int page_order)
{
    return -EINVAL;
}

int set_foreign_p2m_entry(struct domain *d, const struct domain *fd,
                          unsigned long gfn, mfn_t mfn)
{
    return -EINVAL;
}

int p2m_teardown(struct domain *d, bool allow_preemption)
{
    return 0;
}

void p2m_final_teardown(struct domain *d)
{
}

static int __init p2m_alloc_table(struct domain *d)
{
    struct p2m_domain *p2m = p2m_get_hostp2m(d);
    pr_t* p2m_map;

    p2m_map = alloc_mpumap();
    if ( !p2m_map )
    {
        printk(XENLOG_G_ERR
               "Unable to allocate P2M MPU table.\n");
        return -ENOMEM;
    }

    p2m->root = virt_to_page((const void *)p2m_map);

    return 0;
}

int p2m_init(struct domain *d)
{
    struct p2m_domain *p2m = p2m_get_hostp2m(d);
    int rc = 0;
    unsigned int cpu;

    rwlock_init(&p2m->lock);
    spin_lock_init(&d->arch.paging.lock);
    INIT_PAGE_LIST_HEAD(&p2m->pages);
    INIT_PAGE_LIST_HEAD(&d->arch.paging.p2m_freelist);

    /* Allocate and set VMID. */
    p2m->vmid = INVALID_VMID;
    p2m->vsctlr = generate_vsctlr(p2m->vmid);

    p2m->max_mapped_gfn = _gfn(0);
    p2m->lowest_mapped_gfn = _gfn(ULONG_MAX);

    /* mem_access is NOT in use for the domain on mpu system. */
    p2m->default_access = p2m_access_rwx;
    p2m->mem_access_enabled = false;

    /*
     * Make sure that the type chosen to is able to store an vCPU ID
     * between 0 and the maximum of virtual CPUS supported as long as
     * the INVALID_VCPU_ID.
     */
    BUILD_BUG_ON((1 << (sizeof(p2m->last_vcpu_ran[0]) * 8)) < MAX_VIRT_CPUS);
    BUILD_BUG_ON((1 << (sizeof(p2m->last_vcpu_ran[0]) * 8)) < INVALID_VCPU_ID);

    for_each_possible_cpu(cpu)
       p2m->last_vcpu_ran[cpu] = INVALID_VCPU_ID;

    /*
     * Besides getting a domain when we only have the p2m in hand,
     * the back pointer to domain is also used in p2m_teardown()
     * as an end-of-initialization indicator.
     */
    p2m->domain = d;

    rc = p2m_alloc_vmid(d);
    if ( rc != 0 )
        return rc;

    /* Allocate MPU P2M table. */
    rc = p2m_alloc_table(d);
    if ( rc != 0 )
        return rc;

    return rc;
}

int relinquish_p2m_mapping(struct domain *d)
{
    return -EINVAL;
}

int p2m_cache_flush_range(struct domain *d, gfn_t *pstart, gfn_t end)
{
    return 0;
}

void p2m_flush_vm(struct vcpu *v)
{
}

void p2m_tlb_flush_sync(struct p2m_domain *p2m)
{
}

void p2m_set_way_flush(struct vcpu *v, struct cpu_user_regs *regs,
                       const union hsr hsr)
{
}

void p2m_toggle_cache(struct vcpu *v, bool was_enabled)
{
}

mfn_t gfn_to_mfn(struct domain *d, gfn_t gfn)
{
    return p2m_lookup(d, gfn, NULL);
}

struct page_info *get_page_from_gva(struct vcpu *v, vaddr_t va,
                                    unsigned long flags)
{
    return NULL;
}

void __init p2m_restrict_ipa_bits(unsigned int ipa_bits)
{

}

register_t get_default_vtcr_flags()
{
    /* Default value will be set during boot-up, in setup_virt_paging(). */
    return vtcr;
}

#ifdef CONFIG_ARM_64
static void setup_virt_paging_one(void *data)
{
    WRITE_SYSREG(vtcr, VTCR_EL2);

    /*
     * All stage 2 translations for the Secure PA space access the
     * Secure PA space, so we keep SA bit as 0.
     *
     * Stage 2 NS configuration is checked against stage 1 NS configuration
     * in EL1&0 translation regime for the given address, and generate a
     * fault if they are different. So we set SC bit as 1.
     */
    WRITE_SYSREG(1 << VSTCR_EL2_RES1_SHIFT | 1 << VSTCR_EL2_SC_SHIFT, VTCR_EL2);
}

void __init setup_virt_paging(void)
{
    unsigned long val = 0;
    bool p2m_vmsa = true;

    /* In ARMV8R, hypervisor in secure EL2. */
    val &= NSA_SEL2;

    /*
     * ARMv8-R AArch64 could have the following memory system
     * configurations:
     * - PMSAv8-64 at EL1 and EL2
     * - PMSAv8-64 or VMSAv8-64 at EL1 and PMSAv8-64 at EL2
     *
     * In ARMv8-R, the only permitted value is
     * 0b1111(MM64_MSA_PMSA_SUPPORT).
     */
    if ( system_cpuinfo.mm64.msa == MM64_MSA_PMSA_SUPPORT )
    {
        if ( system_cpuinfo.mm64.msa_frac == MM64_MSA_FRAC_NONE_SUPPORT )
            goto fault;

        if ( system_cpuinfo.mm64.msa_frac != MM64_MSA_FRAC_VMSA_SUPPORT )
        {
            p2m_vmsa = false;
            warning_add("Be aware of that there is no support for VMSAv8-64 at EL1 on this platform.\n");
        }
    }
    else
        goto fault;

    /*
     * If the platform supports both PMSAv8-64 or VMSAv8-64 at EL1,
     * then it's VTCR_EL2.MSA that determines the EL1 memory system
     * architecture.
     * Normally, we set the initial VTCR_EL2.MSA value VMSAv8-64 support,
     * unless this platform only supports PMSAv8-64.
     */
    if ( !p2m_vmsa )
        val &= VTCR_MSA_PMSA;
    else
        val |= VTCR_MSA_VMSA;

    /*
     * cpuinfo sanitization makes sure we support 16bits VMID only if
     * all cores are supporting it.
     */
    if ( system_cpuinfo.mm64.vmid_bits == MM64_VMID_16_BITS_SUPPORT )
        max_vmid = MAX_VMID_16_BIT;

    /* Set the VS bit only if 16 bit VMID is supported. */
    if ( MAX_VMID == MAX_VMID_16_BIT )
        val |= VTCR_VS;
 
    /*
     * When guest in PMSAv8-64, the guest EL1 MPU regions will be saved on
     * context switch.
     */
    load_mpu_supported_region_el1();

    p2m_vmid_allocator_init();

    vtcr = val;

    setup_virt_paging_one(NULL);
    smp_call_function(setup_virt_paging_one, NULL, 1);

    return;

fault:
    panic("Hardware with no PMSAv8-64 support in any translation regime.\n");
}
#else
void __init setup_virt_paging(void)
{
    /*
     * When guest in PMSAv8-64, the guest EL1 MPU regions will be saved on
     * context switch.
     */
    load_mpu_supported_region_el1();

    p2m_vmid_allocator_init();

    vtcr = 0;

    return;
}
#endif

/*
 * Local variables:
 * mode: C
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * End:
 */
