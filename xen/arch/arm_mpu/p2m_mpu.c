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
#include <asm/arm64/mpu.h>
#include <asm/p2m.h>

/* VTCR_EL2 value to be configured for the boot CPU. */
static uint32_t __read_mostly vtcr;

static uint64_t generate_vsctlr(uint16_t vmid)
{
    return ((uint64_t)vmid << 48);
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

void p2m_dump_info(struct domain *d)
{
}

void memory_type_changed(struct domain *d)
{
}

void dump_p2m_lookup(struct domain *d, paddr_t addr)
{
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

    return;

fault:
    panic("Hardware with no PMSAv8-64 support in any translation regime.\n");
}

/*
 * Local variables:
 * mode: C
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * End:
 */
