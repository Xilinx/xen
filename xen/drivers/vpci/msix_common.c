/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Handlers for accesses to the MSI-X capability structure and the memory
 * region.
 *
 * Copyright (C) 2017 Citrix Systems R&D
 */

#include <xen/io.h>
#include <xen/lib.h>
#include <xen/msi.h>
#include <xen/sched.h>
#include <xen/vpci.h>

#include <asm/p2m.h>

static uint32_t cf_check control_read(
    const struct pci_dev *pdev, unsigned int reg, void *data)
{
    const struct vpci_msix *msix = data;

    return (msix->max_entries - 1) |
           (msix->enabled ? PCI_MSIX_FLAGS_ENABLE : 0) |
           (msix->masked ? PCI_MSIX_FLAGS_MASKALL : 0);
}

static void cf_check control_write(
    const struct pci_dev *pdev, unsigned int reg, uint32_t val, void *data)
{
    struct vpci_msix *msix = data;
    bool new_masked = val & PCI_MSIX_FLAGS_MASKALL;
    bool new_enabled = val & PCI_MSIX_FLAGS_ENABLE;

    if ( new_masked == msix->masked && new_enabled == msix->enabled )
        return;

    vpci_msix_arch_control_write(pdev, reg, val, data);

    /* Make sure domU doesn't enable INTx while enabling MSI-X. */
    if ( new_enabled && !msix->enabled && !is_hardware_domain(pdev->domain) )
    {
        pci_intx(pdev, false);
        pdev->vpci->header.guest_cmd |= PCI_COMMAND_INTX_DISABLE;
    }

    msix->masked = new_masked;
    msix->enabled = new_enabled;

    val = control_read(pdev, reg, data);
    if ( pci_msi_conf_write_intercept(msix->pdev, reg, 2, &val) >= 0 )
        pci_conf_write16(pdev->sbdf, reg, val);
}

static int cf_check cleanup_msix(const struct pci_dev *pdev, bool hide)
{
    int rc;
    struct vpci *vpci = pdev->vpci;
    const unsigned int msix_pos = pdev->msix_pos;

    vpci_msix_arch_cleanup(vpci);

    if ( vpci->msix )
        XFREE(vpci->msix);

    if ( !hide )
        return 0;

    rc = vpci_remove_registers(vpci, msix_pba_offset_reg(msix_pos), 4);
    if ( rc )
    {
        printk(XENLOG_ERR "%pd: %pp remove msix_pba_offset failed rc=%d\n",
               pdev->domain, &pdev->sbdf, rc);
        ASSERT_UNREACHABLE();
        return rc;
    }

    rc = vpci_remove_registers(vpci, msix_table_offset_reg(msix_pos), 4);
    if ( rc )
    {
        printk(XENLOG_ERR "%pd: %pp remove msix_table_offset failed rc=%d\n",
               pdev->domain, &pdev->sbdf, rc);
        ASSERT_UNREACHABLE();
        return rc;
    }

    rc = vpci_remove_registers(vpci, msix_control_reg(msix_pos), 2);
    if ( rc )
    {
        printk(XENLOG_ERR "%pd %pp: fail to remove MSIX handlers rc=%d\n",
               pdev->domain, &pdev->sbdf, rc);
        ASSERT_UNREACHABLE();
        return rc;
    }

    /*
     * Unprivileged domains have a deny by default register access policy, no
     * need to add any further handlers for them.
     */
    if ( !is_hardware_domain(pdev->domain) )
        return 0;

    /*
     * The driver may not traverse the capability list and think device
     * supports MSIX by default. So here let the control register of MSIX
     * be Read-Only is to ensure MSIX disabled.
     */
    rc = vpci_add_register(vpci, vpci_hw_read16, NULL,
                           msix_control_reg(msix_pos), 2, NULL);
    if ( rc )
        printk(XENLOG_ERR "%pd %pp: fail to add MSIX ctrl handler rc=%d\n",
               pdev->domain, &pdev->sbdf, rc);

    return rc;
}

static int cf_check init_msix(struct pci_dev *pdev)
{
    struct domain *d = pdev->domain;
    unsigned int msix_offset, i, max_entries;
    uint16_t control;
    struct vpci_msix *msix;
    int rc;

    msix_offset = pdev->msix_pos;
    if ( !msix_offset )
        return 0;

    control = pci_conf_read16(pdev->sbdf, msix_control_reg(msix_offset));

    max_entries = msix_table_size(control);

    msix = xzalloc_flex_struct(struct vpci_msix, entries, max_entries);
    if ( !msix )
        return -ENOMEM;

    msix->tables[VPCI_MSIX_TABLE] =
        pci_conf_read32(pdev->sbdf, msix_table_offset_reg(msix_offset));
    msix->tables[VPCI_MSIX_PBA] =
        pci_conf_read32(pdev->sbdf, msix_pba_offset_reg(msix_offset));

    /* Check that the referenced BAR(s) regions are valid. */
    for ( i = 0; i < ARRAY_SIZE(msix->tables); i++ )
    {
        const char *name = (i == VPCI_MSIX_TABLE) ? "vector" : "PBA";
        const struct vpci_bar *bars = pdev->vpci->header.bars;
        unsigned int bir = msix->tables[i] & PCI_MSIX_BIRMASK;
        unsigned int type;
        unsigned int offset = msix->tables[i] & ~PCI_MSIX_BIRMASK;
        unsigned int size =
            (i == VPCI_MSIX_TABLE) ? max_entries * PCI_MSIX_ENTRY_SIZE
                                   : ROUNDUP(DIV_ROUND_UP(max_entries, 8), 8);

        if ( bir >= ARRAY_SIZE(pdev->vpci->header.bars) )
        {
            printk(XENLOG_ERR DEV_BUG_PREFIX
                   "%pp: MSI-X %s table with out of range BIR %u\n",
                   &pdev->sbdf, name, bir);
 invalid:
            xfree(msix);
            return -ENODEV;
        }

        type = bars[bir].type;
        if ( type != VPCI_BAR_MEM32 && type != VPCI_BAR_MEM64_LO )
        {
            printk(XENLOG_ERR DEV_BUG_PREFIX
                   "%pp: MSI-X %s table at invalid BAR%u with type %u\n",
                   &pdev->sbdf, name, bir, type);
            goto invalid;
        }

        if ( (uint64_t)offset + size > bars[bir].size )
        {
            printk(XENLOG_ERR DEV_BUG_PREFIX
                   "%pp: MSI-X %s table offset %#x size %#x outside of BAR%u size %#lx\n",
                   &pdev->sbdf, name, offset, size, bir, bars[bir].size);
            goto invalid;
        }
    }

    rc = vpci_add_register(pdev->vpci, control_read, control_write,
                           msix_control_reg(msix_offset), 2, msix);
    if ( rc )
        goto out;

    if ( !is_hardware_domain(d) )
    {
        unsigned long val;

        val = pci_conf_read32(pdev->sbdf, msix_table_offset_reg(msix_offset));
        rc = vpci_add_register(pdev->vpci, vpci_read_val, NULL,
                               msix_table_offset_reg(msix_offset), 4,
                               (void *)(uintptr_t)val);
        if ( rc )
        {
            printk("%pd: %pp register msix_table_offset_reg failed\n",
                   d, &pdev->sbdf);
            goto out;
        }

        val = pci_conf_read32(pdev->sbdf, msix_pba_offset_reg(msix_offset));
        rc = vpci_add_register(pdev->vpci, vpci_read_val, NULL,
                               msix_pba_offset_reg(msix_offset), 4,
                               (void *)(uintptr_t)val);
        if ( rc )
        {
            printk("%pd: %pp register msix_pba_offset failed\n",
                   d, &pdev->sbdf);
            goto out;
        }
    }

    msix->max_entries = max_entries;
    msix->pdev = pdev;

    for ( i = 0; i < max_entries; i++)
    {
        msix->entries[i].masked = true;
        vpci_msix_arch_init_entry(&msix->entries[i]);
    }

    pdev->vpci->msix = msix;

    vpci_msix_arch_register(msix, d);

    /*
     * vPCI header initialization will have mapped the whole BAR into the
     * p2m, as MSI-X capability was not yet initialized.  Crave a hole for
     * the MSI-X table here, so that Xen can trap accesses.
     */
    return vpci_make_msix_hole(pdev);

 out:
    xfree(msix);
    return rc;
}
REGISTER_VPCI_CAP(MSIX, init_msix, cleanup_msix);

/*
 * Local variables:
 * mode: C
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * tab-width: 4
 * indent-tabs-mode: nil
 * End:
 */
