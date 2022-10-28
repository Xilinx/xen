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
