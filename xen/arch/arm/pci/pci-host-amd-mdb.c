/* SPDX-License-Identifier: GPL-2.0-only
 *
 * Based on Linux drivers/pci/controller/pci-host-common.c
 * Based on Linux drivers/pci/controller/pci-host-generic.c
 * Based on xen/arch/arm/pci/pci-host-generic.c
 */

#include <xen/err.h>
#include <xen/init.h>
#include <xen/pci.h>

#include <asm/device.h>
#include <asm/io.h>
#include <asm/pci.h>

#include "pci-designware.h"

/*
 * PCI host bridges often have different ways to access the root and child
 * bus config spaces:
 *   "dbi"   : the aperture where root port's own configuration registers
 *             are available.
 *   "config": child's configuration space
 *   "atu"   : iATU registers for DWC version 4.80 or later
 */
static int __init amd_mdb_cfg_reg_index(struct dt_device_node *np)
{
    return dt_property_match_string(np, "reg-names", "dbi");
}

static int __init amd_mdb_child_cfg_reg_index(struct dt_device_node *np)
{
    return dt_property_match_string(np, "reg-names", "config");
}

/* ECAM ops */
static const struct pci_ecam_ops amd_mdb_pcie_ops = {
    .bus_shift  = 20,
    .cfg_reg_index = amd_mdb_cfg_reg_index,
    .pci_ops    = {
        .map_bus                = pci_ecam_map_bus,
        .read                   = pci_generic_config_read,
        .write                  = pci_generic_config_write,
        .need_p2m_hwdom_mapping = pci_ecam_need_p2m_hwdom_mapping,
        .init_bus_range         = pci_generic_init_bus_range,
    }
};

static const struct pci_ecam_ops amd_mdb_pcie_child_ops = {
    .bus_shift  = 20,
    .cfg_reg_index = amd_mdb_child_cfg_reg_index,
    .pci_ops    = {
        .map_bus                = dw_pcie_child_map_bus,
        .read                   = dw_pcie_child_config_read,
        .write                  = dw_pcie_child_config_write,
        .need_p2m_hwdom_mapping = dw_pcie_child_need_p2m_hwdom_mapping,
        .init_bus_range         = pci_generic_init_bus_range_child,
    }
};

static const struct dt_device_match __initconstrel amd_mdb_pcie_dt_match[] = {
    { .compatible = "amd,versal2-mdb-host" },
    {},
};

static int __init pci_host_amd_mdb_probe(struct dt_device_node *dev,
                                         const void *data)
{
    return PTR_RET(dw_pcie_host_probe(dev, data, &amd_mdb_pcie_ops,
                                      &amd_mdb_pcie_child_ops));
}

DT_DEVICE_START(pci_gen, "AMD MDB PCIe HOST", DEVICE_PCI_HOSTBRIDGE)
.dt_match = amd_mdb_pcie_dt_match,
.init = pci_host_amd_mdb_probe,
DT_DEVICE_END

/*
 * Local variables:
 * mode: C
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * tab-width: 4
 * indent-tabs-mode: nil
 * End:
 */
