/*
 * Based on xen/arch/arm/pci/pci-host-zynqmp.c
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
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <xen/init.h>
#include <xen/pci.h>
#include <asm/device.h>
#include <asm/pci.h>

static int __init cpm_cfg_reg_index(struct dt_device_node *np)
{
    return dt_property_match_string(np, "reg-names", "cfg");
}

/* ECAM ops */
static const struct pci_ecam_ops cpm_pcie_ops = {
    .bus_shift = 20,
    .cfg_reg_index = cpm_cfg_reg_index,
    .pci_ops = {
        .map_bus                = pci_ecam_map_bus,
        .read                   = pci_generic_config_read,
        .write                  = pci_generic_config_write,
        .need_p2m_hwdom_mapping = pci_ecam_need_p2m_hwdom_mapping,
    },
};

static const struct dt_device_match __initconstrel cpm_pcie_dt_match[] =
{
    { .compatible = "xlnx,versal-cpm-host-1.00" },
    { }
};

static int __init pci_host_versal_probe(struct dt_device_node *dev,
                                        const void *data)
{
    return pci_host_common_probe(dev, &cpm_pcie_ops);
}

DT_DEVICE_START(pci_gen, "PCI HOST VERSAL", DEVICE_PCI_HOSTBRIDGE)
.dt_match = cpm_pcie_dt_match,
.init = pci_host_versal_probe,
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
