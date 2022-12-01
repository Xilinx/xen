/* SPDX-License-Identifier: GPL-2.0-only */
#include <xen/device_tree.h>
#include <xen/domain_page.h>
#include <xen/err.h>
#include <xen/event.h>
#include <xen/grant_table.h>
#include <xen/iocap.h>
#include <xen/libfdt/libfdt.h>
#include <xen/llc-coloring.h>
#include <xen/sched.h>
#include <xen/serial.h>
#include <xen/sizes.h>
#include <xen/vmap.h>

#include <public/bootfdt.h>
#include <public/io/xs_wire.h>

#include <asm/arm64/sve.h>
#include <asm/dom0less-build.h>
#include <asm/domain_build.h>
#include <asm/gic_v3_its.h>
#include <asm/grant_table.h>
#include <asm/pci.h>
#include <asm/static-memory.h>
#include <asm/static-shmem.h>
#include <asm/platform.h>
#include <asm/viommu.h>

static domid_t __initdata xs_domid = DOMID_INVALID;
static bool __initdata need_xenstore;

void __init set_xs_domid(domid_t domid)
{
    xs_domid = domid;
}

bool __init is_dom0less_mode(void)
{
    struct bootmodules *mods = &bootinfo.modules;
    struct bootmodule *mod;
    unsigned int i;
    bool dom0found = false;
    bool domUfound = false;

    /* Look into the bootmodules */
    for ( i = 0 ; i < mods->nr_mods ; i++ )
    {
        mod = &mods->module[i];
        /* Find if dom0 and domU kernels are present */
        if ( mod->kind == BOOTMOD_KERNEL )
        {
            if ( mod->domU == false )
            {
                dom0found = true;
                break;
            }
            else
                domUfound = true;
        }
    }

    /*
     * If there is no dom0 kernel but at least one domU, then we are in
     * dom0less mode
     */
    return ( !dom0found && domUfound );
}

#ifdef CONFIG_VGICV2
static int __init make_gicv2_domU_node(struct kernel_info *kinfo)
{
    void *fdt = kinfo->fdt;
    int res = 0;
    __be32 reg[(GUEST_ROOT_ADDRESS_CELLS + GUEST_ROOT_SIZE_CELLS) * 2];
    __be32 *cells;
    const struct domain *d = kinfo->d;

    res = domain_fdt_begin_node(fdt, "interrupt-controller",
                                vgic_dist_base(&d->arch.vgic));
    if ( res )
        return res;

    res = fdt_property_cell(fdt, "#address-cells", GUEST_ROOT_ADDRESS_CELLS);
    if ( res )
        return res;

    res = fdt_property_cell(fdt, "#interrupt-cells", 3);
    if ( res )
        return res;

    res = fdt_property(fdt, "interrupt-controller", NULL, 0);
    if ( res )
        return res;

    res = fdt_property_string(fdt, "compatible", "arm,gic-400");
    if ( res )
        return res;

    cells = &reg[0];
    dt_child_set_range(&cells, GUEST_ROOT_ADDRESS_CELLS, GUEST_ROOT_SIZE_CELLS,
                       vgic_dist_base(&d->arch.vgic), GUEST_GICD_SIZE);
    dt_child_set_range(&cells, GUEST_ROOT_ADDRESS_CELLS, GUEST_ROOT_SIZE_CELLS,
                       vgic_cpu_base(&d->arch.vgic), GUEST_GICC_SIZE);

    res = fdt_property(fdt, "reg", reg, sizeof(reg));
    if (res)
        return res;

    res = fdt_property_cell(fdt, "linux,phandle", kinfo->phandle_gic);
    if (res)
        return res;

    res = fdt_property_cell(fdt, "phandle", kinfo->phandle_gic);
    if (res)
        return res;

    res = fdt_end_node(fdt);

    return res;
}
#endif

#ifdef CONFIG_GICV3
static int __init make_gicv3_domU_node(struct kernel_info *kinfo)
{
    void *fdt = kinfo->fdt;
    int res = 0;
    __be32 *reg, *cells;
    const struct domain *d = kinfo->d;
    unsigned int i, len = 0;

    res = domain_fdt_begin_node(fdt, "interrupt-controller",
                                vgic_dist_base(&d->arch.vgic));
    if ( res )
        return res;

    res = fdt_property_cell(fdt, "#address-cells", GUEST_ROOT_ADDRESS_CELLS);
    if ( res )
        return res;

    res = fdt_property_cell(fdt, "#size-cells", GUEST_ROOT_SIZE_CELLS);
    if ( res )
        return res;

    res = fdt_property(fdt, "ranges", NULL, 0);
    if ( res )
        return res;

    res = fdt_property_cell(fdt, "#interrupt-cells", 3);
    if ( res )
        return res;

    res = fdt_property(fdt, "interrupt-controller", NULL, 0);
    if ( res )
        return res;

    res = fdt_property_string(fdt, "compatible", "arm,gic-v3");
    if ( res )
        return res;

    res = fdt_property_cell(fdt, "#redistributor-regions",
                            d->arch.vgic.nr_regions);
    if ( res )
        return res;

    /* reg specifies all re-distributors and Distributor. */
    len = (GUEST_ROOT_ADDRESS_CELLS + GUEST_ROOT_SIZE_CELLS) *
          (d->arch.vgic.nr_regions + 1) * sizeof(__be32);
    reg = xmalloc_bytes(len);
    if ( reg == NULL )
        return -ENOMEM;
    cells = reg;

    dt_child_set_range(&cells, GUEST_ROOT_ADDRESS_CELLS, GUEST_ROOT_SIZE_CELLS,
                       vgic_dist_base(&d->arch.vgic), GUEST_GICV3_GICD_SIZE);

    for ( i = 0; i < d->arch.vgic.nr_regions; i++ )
        dt_child_set_range(&cells,
                           GUEST_ROOT_ADDRESS_CELLS, GUEST_ROOT_SIZE_CELLS,
                           d->arch.vgic.rdist_regions[i].base,
                           d->arch.vgic.rdist_regions[i].size);

    res = fdt_property(fdt, "reg", reg, len);
    xfree(reg);
    if (res)
        return res;

    res = fdt_property_cell(fdt, "linux,phandle", kinfo->phandle_gic);
    if (res)
        return res;

    res = fdt_property_cell(fdt, "phandle", kinfo->phandle_gic);
    if (res)
        return res;

    /* Add ITS node only if domain will use vpci */
    if ( is_pci_scan_enabled() )
    {
        res = gicv3_its_make_emulated_dt_node(d, fdt);
        if ( res )
            return res;
    }

    res = fdt_end_node(fdt);

    return res;
}
#endif

static int __init make_gic_domU_node(struct kernel_info *kinfo)
{
    switch ( kinfo->d->arch.vgic.version )
    {
#ifdef CONFIG_GICV3
    case GIC_V3:
        return make_gicv3_domU_node(kinfo);
#endif
#ifdef CONFIG_VGICV2
    case GIC_V2:
        return make_gicv2_domU_node(kinfo);
#endif
    default:
        panic("Unsupported GIC version\n");
    }
}

#ifdef CONFIG_VPL011_CONSOLE
static int __init make_vpl011_clk_node(struct kernel_info *kinfo)
{
    void *fdt = kinfo->fdt;
    int res;

    res = fdt_begin_node(fdt, "pl011-clk");
    if ( res )
        return res;

    res = fdt_property_cell(fdt, "#clock-cells", 0);
    if ( res )
        return res;

    res = fdt_property_string(fdt, "compatible", "fixed-clock");
    if ( res )
        return res;

    /*
     * This clock is used as both UARTCLK and PCLK. 7.3728MHz was selected
     * to make the divisor calculations simpler for the guest (i.e. FBRD
     * register is 0 for most of the common bit rates).
     */
    res = fdt_property_u32(fdt, "clock-frequency", 7372800);
    if ( res )
        return res;

    res = fdt_property_cell(fdt, "phandle", GUEST_PHANDLE_VPL011_CLK);
    if ( res )
        return res;

    res = fdt_end_node(fdt);
    if ( res )
        return res;

    return 0;
}

static int __init make_vpl011_uart_node(struct kernel_info *kinfo)
{
    void *fdt = kinfo->fdt;
    int res;
    gic_interrupt_t intr;
    __be32 reg[GUEST_ROOT_ADDRESS_CELLS + GUEST_ROOT_SIZE_CELLS];
    __be32 *cells;
    struct domain *d = kinfo->d;
    bool sbsa = (kinfo->vpl011 == VUART_TYPE_SBSA);

    res = domain_fdt_begin_node(fdt, sbsa ? "sbsa-uart" : "pl011",
                                d->arch.vpl011.base_addr);
    if ( res )
        return res;

    if ( sbsa )
        res = fdt_property_string(fdt, "compatible", "arm,sbsa-uart");
    else
        res = fdt_property(fdt, "compatible", "arm,pl011\0arm,primecell", 23);

    if ( res )
        return res;

    cells = &reg[0];
    dt_child_set_range(&cells, GUEST_ROOT_ADDRESS_CELLS,
                       GUEST_ROOT_SIZE_CELLS, d->arch.vpl011.base_addr,
                       GUEST_PL011_SIZE);

    res = fdt_property(fdt, "reg", reg, sizeof(reg));
    if ( res )
        return res;

    set_interrupt(intr, d->arch.vpl011.virq, 0xf, DT_IRQ_TYPE_LEVEL_HIGH);

    res = fdt_property(fdt, "interrupts", intr, sizeof (intr));
    if ( res )
        return res;

    if ( !sbsa )
    {
        /*
         * Two phandles (for UARTCLK and PCLK) need to be present under clocks
         * property but they can refer to the same clock.
         */
        __be32 clocks[] = {
            cpu_to_fdt32(GUEST_PHANDLE_VPL011_CLK),
            cpu_to_fdt32(GUEST_PHANDLE_VPL011_CLK),
        };

        res = fdt_property(fdt, "clocks", clocks, sizeof(clocks));
        if ( res )
            return res;

        res = fdt_property(fdt, "clock-names", "uartclk\0apb_pclk", 17);
        if ( res )
            return res;
    }

    res = fdt_property_cell(fdt, "interrupt-parent",
                            kinfo->phandle_gic);
    if ( res )
        return res;

    if ( sbsa )
    {
        /* Use a default baud rate of 115200. */
        fdt_property_u32(fdt, "current-speed", 115200);
    }

    res = fdt_end_node(fdt);
    if ( res )
        return res;

    return 0;
}
#endif

#ifdef CONFIG_HAS_VPCI_GUEST_SUPPORT
static int __init domU_assign_pci_device(struct domain *d,
                                         struct kernel_info *kinfo,
                                         const void *pfdt,
                                         int nodeoffset)
{
    const struct fdt_property *prop;
    const __be32 *cell;
    unsigned int count;
    int rc;

    if ( !is_pci_passthrough_enabled() || !is_pci_scan_enabled() )
        return 0;

    prop = fdt_get_property(pfdt, nodeoffset, "xen,pci-assigned", NULL);
    /* If the property is not found, return without errors */
    if ( !prop )
        return 0;

    if ( !prop->len )
    {
        printk(XENLOG_ERR "xen,pci-assigned property cannot be empty.\n");
        return -EINVAL;
    }

    cell = (const __be32 *)prop->data;
    count = fdt32_to_cpu(prop->len) / sizeof(u32);

    if ( (count % 3) != 0 )
    {
        printk(XENLOG_ERR
               "xen,pci-assigned values count must be multiple of 3.\n");
        return -EINVAL;
    }

    while ( count > 0 )
    {
        u32 seg, bus, devfn;
        pci_sbdf_t sbdf;
        unsigned int i, ret;

        seg = dt_next_cell(1, &cell);
        bus = dt_next_cell(1, &cell);
        devfn = dt_next_cell(1, &cell);

        if ( (seg & ~0xFFFF) || (bus & ~0xFF) || (devfn & ~0xFF) )
        {
            printk(XENLOG_ERR
                   "Invalid device identifier in xen,pci-assigned property: "
                   "<%04x %02x %02x> \n", seg, bus, devfn);
            return -EINVAL;
        }

        sbdf = PCI_SBDF(seg, bus, devfn);

        if ( (pci_conf_read8(sbdf, PCI_HEADER_TYPE) & 0x7f) !=
             PCI_HEADER_TYPE_NORMAL )
            return -EINVAL;

        printk(XENLOG_INFO
               "Assign PCI device %04x:%02x:%02x.%x to domain domU%d\n", seg,
               bus, PCI_SLOT(devfn), PCI_FUNC(devfn), d->domain_id);

        rc = pci_assign_device(d, seg, bus, devfn, 0);
        if ( rc < 0 )
        {
            printk(XENLOG_ERR
                   "Failure assigning the pci device (Error %d) \n", rc);
            return rc;
        }
        count -= 3;

        for ( i = 0; i < PCI_HEADER_NORMAL_NR_BARS; i += ret )
        {
            uint64_t addr, size;
            uint8_t reg = PCI_BASE_ADDRESS_0 + i * 4;

            ret = pci_size_mem_bar(sbdf, reg, &addr, &size,
                                  (i == PCI_HEADER_NORMAL_NR_BARS - 1)
                                      ? PCI_BAR_LAST : 0);

            if ( !size )
                continue;

            rc = iomem_permit_access(d, paddr_to_pfn(addr),
                                     paddr_to_pfn(PAGE_ALIGN(addr + size - 1)));
            if ( rc )
                return rc;
        }
    }

    rc = make_vpci_node(d, kinfo->fdt);
    if ( rc )
        return rc;

    return 0;
}
#else
static inline int __init domU_assign_pci_device(struct domain *d,
                                                struct kernel_info *kinfo,
                                                const void *pfdt,
                                                int nodeoffset)
{
    return 0;
}
#endif

/*
 * Scan device tree properties for passthrough specific information.
 * Returns < 0 on error
 *         0 on success
 */
static int __init handle_passthrough_prop(struct kernel_info *kinfo,
                                          const struct fdt_property *xen_reg,
                                          const struct fdt_property *xen_path,
                                          bool xen_force, bool cacheable,
                                          uint32_t address_cells,
                                          uint32_t size_cells)
{
    const __be32 *cell;
    unsigned int i, len;
    struct dt_device_node *node;
    int res;
    paddr_t mstart, size, gstart;

    /* xen,reg specifies where to map the MMIO region */
    cell = (const __be32 *)xen_reg->data;
    len = fdt32_to_cpu(xen_reg->len) / ((address_cells * 2 + size_cells) *
                                        sizeof(uint32_t));

    for ( i = 0; i < len; i++ )
    {
        device_tree_get_reg(&cell, address_cells, size_cells,
                            &mstart, &size);
        gstart = dt_next_cell(address_cells, &cell);

        if ( gstart & ~PAGE_MASK || mstart & ~PAGE_MASK || size & ~PAGE_MASK )
        {
            printk(XENLOG_ERR
                   "DomU passthrough config has not page aligned addresses/sizes\n");
            return -EINVAL;
        }

        res = iomem_permit_access(kinfo->d, paddr_to_pfn(mstart),
                                  paddr_to_pfn(PAGE_ALIGN(mstart + size - 1)));
        if ( res )
        {
            printk(XENLOG_ERR "Unable to permit to dom%d access to"
                   " 0x%"PRIpaddr" - 0x%"PRIpaddr"\n",
                   kinfo->d->domain_id,
                   mstart & PAGE_MASK, PAGE_ALIGN(mstart + size) - 1);
            return res;
        }

        res = map_regions_p2mt(kinfo->d,
                               gaddr_to_gfn(gstart),
                               PFN_DOWN(size),
                               maddr_to_mfn(mstart),
                               cacheable ? p2m_mmio_direct_c : p2m_mmio_direct_dev);
        if ( res < 0 )
        {
            printk(XENLOG_ERR
                   "Failed to map %"PRIpaddr" to the guest at%"PRIpaddr"\n",
                   mstart, gstart);
            return -EFAULT;
        }
    }

    /*
     * If xen_force, we let the user assign a MMIO region with no
     * associated path.
     */
    if ( xen_path == NULL )
        return xen_force ? 0 : -EINVAL;

    /*
     * xen,path specifies the corresponding node in the host DT.
     * Both interrupt mappings and IOMMU settings are based on it,
     * as they are done based on the corresponding host DT node.
     */
    node = dt_find_node_by_path(xen_path->data);
    if ( node == NULL )
    {
        printk(XENLOG_ERR "Couldn't find node %s in host_dt!\n",
               xen_path->data);
        return -EINVAL;
    }

    res = map_device_irqs_to_domain(kinfo->d, node, true, NULL);
    if ( res < 0 )
        return res;

    res = iommu_add_dt_device(node);
    if ( res < 0 )
        return res;

    if ( (xen_force || is_domain_direct_mapped(kinfo->d)) &&
         !dt_device_is_protected(node) )
        return 0;

    return iommu_assign_dt_device(kinfo->d, node);
}

static int __init handle_prop_pfdt(struct kernel_info *kinfo,
                                   const void *pfdt, int nodeoff,
                                   uint32_t address_cells, uint32_t size_cells,
                                   bool scan_passthrough_prop)
{
    void *fdt = kinfo->fdt;
    int propoff, nameoff, res;
    const struct fdt_property *prop, *xen_reg = NULL, *xen_path = NULL;
    const char *name;
    bool found, xen_force = false, cacheable = false;

    for ( propoff = fdt_first_property_offset(pfdt, nodeoff);
          propoff >= 0;
          propoff = fdt_next_property_offset(pfdt, propoff) )
    {
        if ( !(prop = fdt_get_property_by_offset(pfdt, propoff, NULL)) )
            return -FDT_ERR_INTERNAL;

        found = false;
        nameoff = fdt32_to_cpu(prop->nameoff);
        name = fdt_string(pfdt, nameoff);

        if ( scan_passthrough_prop )
        {
            if ( dt_prop_cmp("xen,reg", name) == 0 )
            {
                xen_reg = prop;
                found = true;
            }
            else if ( dt_prop_cmp("xen,reg-cacheable", name) == 0 )
            {
                xen_reg = prop;
                cacheable = true;
                found = true;
            }
            else if ( dt_prop_cmp("xen,path", name) == 0 )
            {
                xen_path = prop;
                found = true;
            }
            else if ( dt_prop_cmp("xen,force-assign-without-iommu",
                                  name) == 0 )
            {
                xen_force = true;
                found = true;
            }
        }

        /*
         * Copy properties other than the ones above: xen,reg, xen,path,
         * and xen,force-assign-without-iommu.
         */
        if ( !found )
        {
            res = fdt_property(fdt, name, prop->data, fdt32_to_cpu(prop->len));
            if ( res )
                return res;
        }
    }

    /*
     * Only handle passthrough properties if both xen,reg and xen,path
     * are present, or if xen,force-assign-without-iommu is specified.
     */
    if ( xen_reg != NULL && (xen_path != NULL || xen_force) )
    {
        res = handle_passthrough_prop(kinfo, xen_reg, xen_path, xen_force,
                                      cacheable, address_cells, size_cells);
        if ( res < 0 )
        {
            printk(XENLOG_ERR "Failed to assign device to %pd\n", kinfo->d);
            return res;
        }
    }
    else if ( (xen_path && !xen_reg) || (xen_reg && !xen_path && !xen_force) )
    {
        printk(XENLOG_ERR "xen,reg or xen,path missing for %pd\n",
               kinfo->d);
        return -EINVAL;
    }

    /* FDT_ERR_NOTFOUND => There is no more properties for this node */
    return ( propoff != -FDT_ERR_NOTFOUND ) ? propoff : 0;
}

static int __init scan_pfdt_node(struct kernel_info *kinfo, const void *pfdt,
                                 int nodeoff,
                                 uint32_t address_cells, uint32_t size_cells,
                                 bool scan_passthrough_prop)
{
    int rc = 0;
    void *fdt = kinfo->fdt;
    int node_next;

    rc = fdt_begin_node(fdt, fdt_get_name(pfdt, nodeoff, NULL));
    if ( rc )
        return rc;

    rc = handle_prop_pfdt(kinfo, pfdt, nodeoff, address_cells, size_cells,
                          scan_passthrough_prop);
    if ( rc )
        return rc;

    address_cells = device_tree_get_u32(pfdt, nodeoff, "#address-cells",
                                        DT_ROOT_NODE_ADDR_CELLS_DEFAULT);
    size_cells = device_tree_get_u32(pfdt, nodeoff, "#size-cells",
                                     DT_ROOT_NODE_SIZE_CELLS_DEFAULT);

    node_next = fdt_first_subnode(pfdt, nodeoff);
    while ( node_next > 0 )
    {
        rc = scan_pfdt_node(kinfo, pfdt, node_next, address_cells, size_cells,
                            scan_passthrough_prop);
        if ( rc )
            return rc;

        node_next = fdt_next_subnode(pfdt, node_next);
    }

    return fdt_end_node(fdt);
}

static int __init check_partial_fdt(void *pfdt, size_t size)
{
    int res;

    if ( fdt_magic(pfdt) != FDT_MAGIC )
    {
        dprintk(XENLOG_ERR, "Partial FDT is not a valid Flat Device Tree");
        return -EINVAL;
    }

    res = fdt_check_header(pfdt);
    if ( res )
    {
        dprintk(XENLOG_ERR, "Failed to check the partial FDT (%d)", res);
        return -EINVAL;
    }

    if ( fdt_totalsize(pfdt) > size )
    {
        dprintk(XENLOG_ERR, "Partial FDT totalsize is too big");
        return -EINVAL;
    }

    return 0;
}

static int __init domain_handle_dtb_bootmodule(struct domain *d,
                                               struct kernel_info *kinfo)
{
    void *pfdt;
    int res, node_next;

    pfdt = ioremap_cache(kinfo->dtb_bootmodule->start,
                         kinfo->dtb_bootmodule->size);
    if ( pfdt == NULL )
        return -EFAULT;

    res = check_partial_fdt(pfdt, kinfo->dtb_bootmodule->size);
    if ( res < 0 )
        goto out;

    for ( node_next = fdt_first_subnode(pfdt, 0);
          node_next > 0;
          node_next = fdt_next_subnode(pfdt, node_next) )
    {
        const char *name = fdt_get_name(pfdt, node_next, NULL);

        if ( name == NULL )
            continue;

        /*
         * Only scan /gic /aliases /passthrough, ignore the rest.
         * They don't have to be parsed in order.
         *
         * Take the GIC phandle value from the special /gic node in the
         * DTB fragment.
         */
        if ( dt_node_cmp(name, "gic") == 0 )
        {
            uint32_t phandle_gic = fdt_get_phandle(pfdt, node_next);

            if ( phandle_gic != 0 )
                kinfo->phandle_gic = phandle_gic;
            continue;
        }

        if ( dt_node_cmp(name, "aliases") == 0 )
        {
            res = scan_pfdt_node(kinfo, pfdt, node_next,
                                 DT_ROOT_NODE_ADDR_CELLS_DEFAULT,
                                 DT_ROOT_NODE_SIZE_CELLS_DEFAULT,
                                 false);
            if ( res )
                goto out;
            continue;
        }
        if ( dt_node_cmp(name, "passthrough") == 0 )
        {
            res = domU_assign_pci_device(d, kinfo, pfdt, node_next);
            if ( res )
                return res;

            res = scan_pfdt_node(kinfo, pfdt, node_next,
                                 DT_ROOT_NODE_ADDR_CELLS_DEFAULT,
                                 DT_ROOT_NODE_SIZE_CELLS_DEFAULT,
                                 true);
            if ( res )
                goto out;
            continue;
        }
    }

 out:
    iounmap(pfdt);

    return res;
}

static int __init make_virtio_pci_domU_node(const struct kernel_info *kinfo)
{
    void *fdt = kinfo->fdt;
    /* reg is sized to be used for all the needed properties below */
    __be32 reg[(1 + (GUEST_ROOT_ADDRESS_CELLS * 2) + GUEST_ROOT_SIZE_CELLS)
               * 2];
    __be32 irq_map[4 * 4 * 10];
    __be32 *cells;
    char buf[22]; /* pcie@ + max 16 char address + '\0' */
    int res;
    int devfn, intx_pin;
    static const char compat[] = "pci-host-ecam-generic";
    static const char reg_names[] = "ecam";

    snprintf(buf, sizeof(buf), "pcie@%" PRIx64, kinfo->virtio_pci.ecam.base);
    dt_dprintk("Create virtio-pci node\n");
    res = fdt_begin_node(fdt, buf);
    if ( res )
        return res;

    res = fdt_property(fdt, "compatible", compat, sizeof(compat));
    if ( res )
        return res;

    res = fdt_property_string(fdt, "device_type", "pci");
    if ( res )
        return res;

    /* Create reg property */
    cells = &reg[0];
    dt_child_set_range(&cells, GUEST_ROOT_ADDRESS_CELLS, GUEST_ROOT_SIZE_CELLS,
                       kinfo->virtio_pci.ecam.base, GUEST_VIRTIO_PCI_ECAM_SIZE);

    res = fdt_property(fdt, "reg", reg,
                       (GUEST_ROOT_ADDRESS_CELLS +
                        GUEST_ROOT_SIZE_CELLS) * sizeof(*reg));
    if ( res )
        return res;

    res = fdt_property(fdt, "reg-names", reg_names, sizeof(reg_names));
    if ( res )
        return res;

    /* Create bus-range property */
    cells = &reg[0];
    dt_set_cell(&cells, 1, 0);
    dt_set_cell(&cells, 1, 0xff);
    res = fdt_property(fdt, "bus-range", reg, 2 * sizeof(*reg));
    if ( res )
        return res;

    res = fdt_property_cell(fdt, "#address-cells", 3);
    if ( res )
        return res;

    res = fdt_property_cell(fdt, "#size-cells", 2);
    if ( res )
        return res;

    res = fdt_property_string(fdt, "status", "okay");
    if ( res )
        return res;

    /*
     * Create ranges property as:
     * <(PCI bitfield) (PCI address) (CPU address) (Size)>
     */
    cells = &reg[0];
    dt_set_cell(&cells, 1, GUEST_VIRTIO_PCI_MEM_TYPE);
    dt_set_cell(&cells, GUEST_ROOT_ADDRESS_CELLS, kinfo->virtio_pci.mem.base);
    dt_set_cell(&cells, GUEST_ROOT_ADDRESS_CELLS, kinfo->virtio_pci.mem.base);
    dt_set_cell(&cells, GUEST_ROOT_SIZE_CELLS, GUEST_VIRTIO_PCI_MEM_SIZE);
    dt_set_cell(&cells, 1, GUEST_VIRTIO_PCI_PREFETCH_MEM_TYPE);
    dt_set_cell(&cells, GUEST_ROOT_ADDRESS_CELLS, kinfo->virtio_pci.pf_mem.base);
    dt_set_cell(&cells, GUEST_ROOT_ADDRESS_CELLS, kinfo->virtio_pci.pf_mem.base);
    dt_set_cell(&cells, GUEST_ROOT_SIZE_CELLS, GUEST_VIRTIO_PCI_PREFETCH_MEM_SIZE);
    res = fdt_property(fdt, "ranges", reg, 14 * sizeof(*reg));
    if ( res )
        return res;

    res = fdt_property(fdt, "dma-coherent", "", 0);
    if ( res )
        return res;

    res = fdt_property_cell(fdt, "#interrupt-cells", 1);
    if ( res )
        return res;

    /*
     * PCI-to-PCI bridge specification
     * 9.1: Interrupt routing. Table 9-1
     *
     * the PCI Express Base Specification, Revision 2.1
     * 2.2.8.1: INTx interrupt signaling - Rules
     *          the Implementation Note
     *          Table 2-20
     */
    cells = &irq_map[0];
    for (devfn = 0; devfn <= 0x18; devfn += 8) {
        for (intx_pin = 0; intx_pin < 4; intx_pin++) {
            int irq = GUEST_VIRTIO_PCI_SPI_FIRST - 32;
            irq += ((intx_pin + PCI_SLOT(devfn)) % 4);

            dt_set_cell(&cells, 1, devfn << 8);
            dt_set_cell(&cells, 1, 0);
            dt_set_cell(&cells, 1, 0);
            dt_set_cell(&cells, 1, intx_pin + 1);
            dt_set_cell(&cells, 1, kinfo->phandle_gic);

            /*
             * Parent unit address (GIC unit address). The number of cells
             * is determined by the vGIC node's #address-cells. See section
             * 2.4.3 in [1].
             *
             * [1] https://github.com/devicetree-org/devicetree-specification/releases/download/v0.4/devicetree-specification-v0.4.pdf
             */
            dt_set_cell(&cells, 1, 0);
            dt_set_cell(&cells, 1, 0);

            /* 3 GIC cells.  */
            dt_set_cell(&cells, 1, 0);
            dt_set_cell(&cells, 1, irq);
            dt_set_cell(&cells, 1, DT_IRQ_TYPE_LEVEL_HIGH);
        }
    }

    /* Assert we've sized irq_map correctly.  */
    BUG_ON(cells - &irq_map[0] != ARRAY_SIZE(irq_map));

    res = fdt_property(fdt, "interrupt-map", irq_map, sizeof(irq_map));
    if ( res )
        return res;

    cells = &reg[0];
    dt_set_cell(&cells, 1, cpu_to_be16(PCI_DEVFN(3, 0)));
    dt_set_cell(&cells, 1, 0x0);
    dt_set_cell(&cells, 1, 0x0);
    dt_set_cell(&cells, 1, 0x7);
    res = fdt_property(fdt, "interrupt-map-mask", reg, 4 * sizeof(*reg));
    if ( res )
        return res;

    if ( kinfo->virtio_pci.mode == VIRTIO_PCI_GRANTS )
    {
        cells = &reg[0];
        dt_set_cell(&cells, 1, 0x0);
        dt_set_cell(&cells, 1, GUEST_PHANDLE_IOMMU);
        dt_set_cell(&cells, 1, 0x0);
        dt_set_cell(&cells, 1, 0x10000);
        res = fdt_property(fdt, "iommu-map", reg, 4 * sizeof(*reg));
        if ( res )
            return res;
    }

    res = fdt_property_cell(fdt, "linux,pci-domain", 1);
    if ( res )
        return res;

    res = fdt_end_node(fdt);
    if ( res )
        return res;

    if ( kinfo->virtio_pci.mode == VIRTIO_PCI_GRANTS )
    {
        snprintf(buf, sizeof(buf), "xen_iommu");

        res = fdt_begin_node(fdt, buf);
        if ( res )
            return res;

        res = fdt_property_string(fdt, "compatible", "xen,grant-dma");
        if ( res )
            return res;

        res = fdt_property_cell(fdt, "#iommu-cells", 1);
        if ( res )
            return res;

        res = fdt_property_cell(fdt, "phandle", GUEST_PHANDLE_IOMMU);
        if ( res )
            return res;

        res = fdt_end_node(fdt);
    }

    return res;
}

/*
 * The max size for DT is 2MB. However, the generated DT is small (not including
 * domU passthrough DT nodes whose size we account separately), 16KB are enough
 * for now, but we might have to increase it in the future.
 */
#define DOMU_DTB_SIZE 16384
static int __init prepare_dtb_domU(struct domain *d, struct kernel_info *kinfo)
{
    int addrcells, sizecells;
    int ret, fdt_size = DOMU_DTB_SIZE;

    kinfo->phandle_gic = GUEST_PHANDLE_GIC;

    addrcells = GUEST_ROOT_ADDRESS_CELLS;
    sizecells = GUEST_ROOT_SIZE_CELLS;

    /* Account for domU passthrough DT size */
    if ( kinfo->dtb_bootmodule )
        fdt_size += kinfo->dtb_bootmodule->size;

    /* Cap to max DT size if needed */
    fdt_size = min(fdt_size, SZ_2M);

    kinfo->fdt = xmalloc_bytes(fdt_size);
    if ( kinfo->fdt == NULL )
        return -ENOMEM;

    ret = fdt_create(kinfo->fdt, fdt_size);
    if ( ret < 0 )
        goto err;

    ret = fdt_finish_reservemap(kinfo->fdt);
    if ( ret < 0 )
        goto err;

    ret = fdt_begin_node(kinfo->fdt, "");
    if ( ret < 0 )
        goto err;

    ret = fdt_property_cell(kinfo->fdt, "#address-cells", addrcells);
    if ( ret )
        goto err;

    ret = fdt_property_cell(kinfo->fdt, "#size-cells", sizecells);
    if ( ret )
        goto err;

    ret = make_chosen_node(kinfo);
    if ( ret )
        goto err;

    ret = make_psci_node(kinfo->fdt);
    if ( ret )
        goto err;

    ret = make_cpus_node(d, kinfo->fdt);
    if ( ret )
        goto err;

    ret = make_memory_node(kinfo, addrcells, sizecells,
                           kernel_info_get_mem(kinfo));
    if ( ret )
        goto err;

    ret = make_resv_memory_node(kinfo, addrcells, sizecells);
    if ( ret )
        goto err;

    /*
     * domain_handle_dtb_bootmodule has to be called before the rest of
     * the device tree is generated because it depends on the value of
     * the field phandle_gic.
     */
    if ( kinfo->dtb_bootmodule )
    {
        ret = domain_handle_dtb_bootmodule(d, kinfo);
        if ( ret )
            goto err;
    }

    ret = make_gic_domU_node(kinfo);
    if ( ret )
        goto err;

    ret = make_timer_node(kinfo);
    if ( ret )
        goto err;

    if ( kinfo->vpl011 )
    {
        ret = -EINVAL;
#ifdef CONFIG_VPL011_CONSOLE
        if ( kinfo->vpl011 == VUART_TYPE_PL011 )
        {
            ret = make_vpl011_clk_node(kinfo);
            if ( ret )
                goto err;
        }
        ret = make_vpl011_uart_node(kinfo);
#endif
        if ( ret )
            goto err;
    }

    if ( kinfo->dom0less_feature & DOM0LESS_ENHANCED_NO_XS )
    {
        ret = make_hypervisor_node(d, kinfo, addrcells, sizecells);
        if ( ret )
            goto err;
    }

    if ( kinfo->virtio_pci.mode )
    {
        ret = make_virtio_pci_domU_node(kinfo);
        if ( ret )
            goto err;
    }

    ret = fdt_end_node(kinfo->fdt);
    if ( ret < 0 )
        goto err;

    ret = fdt_finish(kinfo->fdt);
    if ( ret < 0 )
        goto err;

    return 0;

  err:
    printk("Device tree generation failed (%d).\n", ret);
    xfree(kinfo->fdt);

    return -EINVAL;
}

static unsigned long __init domain_p2m_pages(unsigned long maxmem_kb,
                                             unsigned int smp_cpus)
{
    /*
     * Keep in sync with libxl__get_required_paging_memory().
     * 256 pages (1MB) per vcpu, plus 1 page per MiB of RAM for the P2M map,
     * plus 128 pages to cover extended regions.
     */
    unsigned long memkb = 4 * (256 * smp_cpus + (maxmem_kb / 1024) + 128);

    BUILD_BUG_ON(PAGE_SIZE != SZ_4K);

    return DIV_ROUND_UP(memkb, 1024) << (20 - PAGE_SHIFT);
}

static int __init alloc_xenstore_evtchn(struct domain *d)
{
    evtchn_alloc_unbound_t alloc;
    int rc;

    alloc.dom = d->domain_id;
    alloc.remote_dom = xs_domid;
    rc = evtchn_alloc_unbound(&alloc, 0);
    if ( rc )
    {
        printk("Failed allocating event channel for domain\n");
        return rc;
    }

    d->arch.hvm.params[HVM_PARAM_STORE_EVTCHN] = alloc.port;

    return 0;
}

#define XENSTORE_PFN_OFFSET 1
static int __init alloc_xenstore_page(struct domain *d)
{
    struct page_info *xenstore_pg;
    struct xenstore_domain_interface *interface;
    mfn_t mfn;
    gfn_t gfn;
    int rc;

    if ( (UINT_MAX - d->max_pages) < 1 )
    {
        printk(XENLOG_ERR "%pd: Over-allocation for d->max_pages by 1 page.\n",
               d);
        return -EINVAL;
    }
    d->max_pages += 1;
    xenstore_pg = alloc_domheap_page(d, MEMF_bits(32));
    if ( xenstore_pg == NULL && is_64bit_domain(d) )
        xenstore_pg = alloc_domheap_page(d, 0);
    if ( xenstore_pg == NULL )
        return -ENOMEM;

    mfn = page_to_mfn(xenstore_pg);
    if ( !mfn_x(mfn) )
    {
        free_domheap_page(xenstore_pg);
        return -ENOMEM;
    }

    if ( !is_domain_direct_mapped(d) )
        gfn = gaddr_to_gfn(GUEST_MAGIC_BASE +
                           (XENSTORE_PFN_OFFSET << PAGE_SHIFT));
    else
        gfn = gaddr_to_gfn(mfn_to_maddr(mfn));

    rc = guest_physmap_add_page(d, gfn, mfn, 0);
    if ( rc )
    {
        free_domheap_page(xenstore_pg);
        return rc;
    }

    d->arch.hvm.params[HVM_PARAM_STORE_PFN] = gfn_x(gfn);
    interface = map_domain_page(mfn);
    interface->connection = XENSTORE_RECONNECT;
    unmap_domain_page(interface);

    return 0;
}

static u64 __init combine_u64(u32 v[2])
{
    u64 v64;

    v64 = v[0];
    v64 <<= 32;
    v64 |= v[1];
    return v64;
}

static int __init alloc_xenstore_params(struct kernel_info *kinfo)
{
    struct domain *d = kinfo->d;
    int rc = 0;

    if ( (kinfo->dom0less_feature & (DOM0LESS_XENSTORE | DOM0LESS_XS_LEGACY))
                                 == (DOM0LESS_XENSTORE | DOM0LESS_XS_LEGACY) )
        d->arch.hvm.params[HVM_PARAM_STORE_PFN] = ~0ULL;
    else if ( kinfo->dom0less_feature & DOM0LESS_XENSTORE )
    {
        rc = alloc_xenstore_page(d);
        if ( rc < 0 )
            return rc;
    }

    return rc;
}

static void __init initialize_domU_xenstore(void)
{
    struct domain *d;

    if ( xs_domid == DOMID_INVALID )
        return;

    for_each_domain( d )
    {
        unsigned long gfn = d->arch.hvm.params[HVM_PARAM_STORE_PFN];
        int rc;

        if ( gfn == 0 )
            continue;

        if ( is_xenstore_domain(d) )
            continue;

        rc = alloc_xenstore_evtchn(d);
        if ( rc < 0 )
            panic("%pd: Failed to allocate xenstore_evtchn\n", d);

        if ( gfn != ~0ULL )
            gnttab_seed_entry(d, GNTTAB_RESERVED_XENSTORE, xs_domid,
                              gfn, GTF_permit_access);
    }
}

static void __init domain_vcpu_affinity(struct domain *d,
                                        const struct dt_device_node *node)
{
    struct dt_device_node *np;

    dt_for_each_child_node(node, np)
    {
        const char *hard_affinity_str = NULL;
        uint32_t val;
        int rc;
        struct vcpu *v;
        cpumask_t affinity;

        if ( !dt_device_is_compatible(np, "xen,vcpu") )
            continue;

        if ( !dt_property_read_u32(np, "id", &val) )
            panic("Invalid xen,vcpu node for domain %s\n", dt_node_name(node));

        if ( val >= d->max_vcpus )
            panic("Invalid vcpu_id %u for domain %s, max_vcpus=%u\n", val,
                  dt_node_name(node), d->max_vcpus);

        v = d->vcpu[val];
        rc = dt_property_read_string(np, "hard-affinity", &hard_affinity_str);
        if ( rc < 0 )
            continue;

        cpumask_clear(&affinity);
        while ( *hard_affinity_str != '\0' )
        {
            unsigned int start, end;

            start = simple_strtoul(hard_affinity_str, &hard_affinity_str, 0);

            if ( *hard_affinity_str == '-' )    /* Range */
            {
                hard_affinity_str++;
                end = simple_strtoul(hard_affinity_str, &hard_affinity_str, 0);
            }
            else                /* Single value */
                end = start;

            if ( end >= nr_cpu_ids )
                panic("Invalid pCPU %u for domain %s\n", end, dt_node_name(node));

            for ( ; start <= end; start++ )
                cpumask_set_cpu(start, &affinity);

            if ( *hard_affinity_str == ',' )
                hard_affinity_str++;
            else if ( *hard_affinity_str != '\0' )
                break;
        }

        rc = vcpu_set_hard_affinity(v, &affinity);
        if ( rc )
            panic("vcpu%d: failed (rc=%d) to set hard affinity for domain %s\n",
                  v->vcpu_id, rc, dt_node_name(node));
    }
}

static int __init construct_domU(struct domain *d,
                                 const struct dt_device_node *node)
{
    struct kernel_info kinfo = KERNEL_INFO_INIT;
    const char *dom0less_enhanced;
    const char *virtio_pci;
    /* virtio-pci ECAM, MEM, PF-MEM each carrying 2 x Address cells.  */
    u32 virtio_pci_ranges[3 * 2];
    const char *vpl011;
    int rc;
    u64 mem;
    u32 p2m_mem_mb;
    unsigned long p2m_pages;

    rc = dt_property_read_u64(node, "memory", &mem);
    if ( !rc )
    {
        printk("Error building DomU: cannot read \"memory\" property\n");
        return -EINVAL;
    }
    kinfo.unassigned_mem = (paddr_t)mem * SZ_1K;

    rc = dt_property_read_u32(node, "xen,domain-p2m-mem-mb", &p2m_mem_mb);
    /* If xen,domain-p2m-mem-mb is not specified, use the default value. */
    p2m_pages = rc ?
                p2m_mem_mb << (20 - PAGE_SHIFT) :
                domain_p2m_pages(mem, d->max_vcpus);

    spin_lock(&d->arch.paging.lock);
    rc = p2m_set_allocation(d, p2m_pages, NULL);
    spin_unlock(&d->arch.paging.lock);
    if ( rc != 0 )
        return rc;

    printk("*** LOADING DOMU cpus=%u memory=%#"PRIx64"KB ***\n",
           d->max_vcpus, mem);

    rc = dt_property_read_string(node, "vpl011", &vpl011);
    if ( !rc )
    {
        if ( !strcmp(vpl011, "sbsa_uart") )
            kinfo.vpl011 = VUART_TYPE_SBSA;
        else if ( !strcmp(vpl011, "pl011") )
            kinfo.vpl011 = VUART_TYPE_PL011;
        else
        {
            printk("Invalid \"vpl011\" property value (%s)\n", vpl011);
            return -EINVAL;
        }
    }
    else if ( rc == -ENODATA )
    {
        /* Handle missing property value */
        kinfo.vpl011 = dt_property_read_bool(node, "vpl011");
    }

    rc = dt_property_read_string(node, "virtio-pci", &virtio_pci);
    if ( !rc )
    {
        if ( !strcmp(virtio_pci, "enabled") )
            kinfo.virtio_pci.mode = VIRTIO_PCI;
        else if ( !strcmp(virtio_pci, "grants") )
            kinfo.virtio_pci.mode = VIRTIO_PCI_GRANTS;
        else
        {
            printk("Invalid \"virtio-pci\" property value (%s)\n", virtio_pci);
            return -EINVAL;
        }
    }
    else if ( rc == -ENODATA )
    {
        /* Handle missing property value */
        kinfo.virtio_pci.mode = dt_property_read_bool(node, "virtio-pci");
    }

    if ( kinfo.virtio_pci.mode != VIRTIO_PCI_NONE )
    {
        rc = dt_property_read_u32_array(node, "virtio-pci-ranges",
                                        virtio_pci_ranges,
                                        ARRAY_SIZE(virtio_pci_ranges));
        if ( rc == 0 ) {
            kinfo.virtio_pci.ecam.base = combine_u64(&virtio_pci_ranges[0]);
            kinfo.virtio_pci.mem.base = combine_u64(&virtio_pci_ranges[2]);
            kinfo.virtio_pci.pf_mem.base = combine_u64(&virtio_pci_ranges[4]);
        } else {
            kinfo.virtio_pci.ecam.base = GUEST_VIRTIO_PCI_ECAM_BASE;
            kinfo.virtio_pci.mem.base = GUEST_VIRTIO_PCI_MEM_BASE;
            kinfo.virtio_pci.pf_mem.base = GUEST_VIRTIO_PCI_PREFETCH_MEM_BASE;
        }

        /*
         * Register a background PCI ECAM region returning ~0. This indicates
         * to the OS that there are no PCI devices on the bus. Once an IOREQ
         * client connects, the OS can rescan the bus and find devices.
         */
        register_mmio_bg_handler(d, true, &mmio_read_const_writes_ignored,
                                 kinfo.virtio_pci.ecam.base,
                                 GUEST_VIRTIO_PCI_ECAM_SIZE,
                                 (void *) ULONG_MAX);
    }

    rc = dt_property_read_string(node, "xen,enhanced", &dom0less_enhanced);
    if ( rc == -EILSEQ ||
         rc == -ENODATA ||
         (rc == 0 && !strcmp(dom0less_enhanced, "enabled")) )
    {
        need_xenstore = true;
        kinfo.dom0less_feature = DOM0LESS_ENHANCED;
    }
    else if ( rc == 0 && !strcmp(dom0less_enhanced, "legacy") )
    {
        need_xenstore = true;
        kinfo.dom0less_feature = DOM0LESS_ENHANCED_LEGACY;
    }
    else if ( rc == 0 && !strcmp(dom0less_enhanced, "no-xenstore") )
        kinfo.dom0less_feature = DOM0LESS_ENHANCED_NO_XS;

    if ( vcpu_create(d, 0) == NULL )
        return -ENOMEM;

    d->max_pages = ((paddr_t)mem * SZ_1K) >> PAGE_SHIFT;

    kinfo.d = d;

    rc = kernel_probe(&kinfo, node);
    if ( rc < 0 )
        return rc;

#ifdef CONFIG_ARM_64
    /* type must be set before allocate memory */
    d->arch.type = kinfo.type;
#endif
    if ( is_hardware_domain(d) )
    {
        rc = construct_hwdom(&kinfo, node);
        if ( rc < 0 )
            return rc;
    }
    else
    {
        if ( !dt_find_property(node, "xen,static-mem", NULL) )
            allocate_memory(d, &kinfo);
        else if ( !is_domain_direct_mapped(d) )
            allocate_static_memory(d, &kinfo, node);
        else
            assign_static_memory_11(d, &kinfo, node);

        rc = process_shm(d, &kinfo, node);
        if ( rc < 0 )
            return rc;

        /*
         * Base address and irq number are needed when creating vpl011 device
         * tree node in prepare_dtb_domU, so initialization on related variables
         * shall be done first.
         */
        if ( kinfo.vpl011 )
        {
            rc = domain_vpl011_init(d, NULL, kinfo.vpl011 == VUART_TYPE_SBSA);
            if ( rc < 0 )
                return rc;
        }

        rc = prepare_dtb_domU(d, &kinfo);
        if ( rc < 0 )
            return rc;

        rc = construct_domain(d, &kinfo);
        if ( rc < 0 )
            return rc;

        domain_vcpu_affinity(d, node);
    }

    return alloc_xenstore_params(&kinfo);
}

void __init create_domUs(void)
{
    struct dt_device_node *node;
    const char *dom0less_iommu;
    bool iommu = false;
    const struct dt_device_node *cpupool_node,
                                *chosen = dt_find_node_by_path("/chosen");
    const char *llc_colors_str = NULL;

    BUG_ON(chosen == NULL);
    dt_for_each_child_node(chosen, node)
    {
        struct domain *d;
        domid_t domid;
        struct xen_domctl_createdomain d_cfg = {
            .arch.gic_version = XEN_DOMCTL_CONFIG_GIC_NATIVE,
            .arch.viommu_type = viommu_get_type(),
            .flags = XEN_DOMCTL_CDF_hvm | XEN_DOMCTL_CDF_hap,
            /*
             * The default of 1023 should be sufficient for guests because
             * on ARM we don't bind physical interrupts to event channels.
             * The only use of the evtchn port is inter-domain communications.
             * 1023 is also the default value used in libxl.
             */
            .max_evtchn_port = 1023,
            .max_grant_frames = -1,
            .max_maptrack_frames = -1,
            .grant_opts = XEN_DOMCTL_GRANT_version(opt_gnttab_max_version),
        };
        unsigned int flags = 0U;
        uint32_t val;
        int rc;

        if ( !dt_device_is_compatible(node, "xen,domain") )
            continue;

        if ( (max_init_domid + 1) >= DOMID_FIRST_RESERVED )
            panic("No more domain IDs available\n");

        if ( dt_property_read_u32(node, "capabilities", &val) )
        {
            if ( val & ~DOMAIN_CAPS_MASK )
                panic("Invalid capabilities (%"PRIx32")\n", val);

            if ( val & DOMAIN_CAPS_CONTROL )
                flags |= CDF_privileged;

            if ( val & DOMAIN_CAPS_HARDWARE )
            {
                if ( hardware_domain )
                    panic("Only 1 hardware domain can be specified! (%pd)\n",
                           hardware_domain);

                d_cfg.max_grant_frames = gnttab_dom0_frames();
                d_cfg.max_evtchn_port = -1;
                flags |= CDF_hardware;
                flags |= CDF_directmap;
                iommu = true;
            }

            if ( val & DOMAIN_CAPS_XENSTORE )
            {
                d_cfg.flags |= XEN_DOMCTL_CDF_xs_domain;
                d_cfg.max_evtchn_port = -1;
            }
        }

        if ( dt_find_property(node, "xen,static-mem", NULL) )
        {
            if ( llc_coloring_enabled )
                panic("LLC coloring and static memory are incompatible\n");

            flags |= CDF_staticmem;
        }

        if ( dt_property_read_bool(node, "direct-map") )
        {
            if ( !(flags & CDF_staticmem) )
                panic("direct-map is not valid for domain %s without static allocation.\n",
                      dt_node_name(node));

            flags |= CDF_directmap;
        }

        if ( !dt_property_read_u32(node, "cpus", &d_cfg.max_vcpus) )
            panic("Missing property 'cpus' for domain %s\n",
                  dt_node_name(node));

        if ( !dt_property_read_string(node, "passthrough", &dom0less_iommu) &&
             !strcmp(dom0less_iommu, "enabled") )
            iommu = true;

        if ( iommu_enabled &&
             (iommu || dt_find_compatible_node(node, NULL,
                                               "multiboot,device-tree")) )
            d_cfg.flags |= XEN_DOMCTL_CDF_iommu;

        if ( !dt_property_read_u32(node, "nr_spis", &d_cfg.arch.nr_spis) )
        {
            int vpl011_virq = GUEST_VPL011_SPI;

            d_cfg.arch.nr_spis = gic_number_lines() - 32;

            /*
             * The VPL011 virq is GUEST_VPL011_SPI, unless direct-map is
             * set, in which case it'll match the hardware.
             *
             * Since the domain is not yet created, we can't use
             * d->arch.vpl011.irq. So the logic to find the vIRQ has to
             * be hardcoded.
             * The logic here shall be consistent with the one in
             * domain_vpl011_init().
             */
            if ( flags & CDF_directmap )
            {
                vpl011_virq = serial_irq(SERHND_DTUART);
                if ( vpl011_virq < 0 )
                    panic("Error getting IRQ number for this serial port %d\n",
                          SERHND_DTUART);
            }

            /*
             * vpl011 uses one emulated SPI. If vpl011 is requested, make
             * sure that we allocate enough SPIs for it.
             */
            if ( dt_property_read_bool(node, "vpl011") )
                d_cfg.arch.nr_spis = MAX(d_cfg.arch.nr_spis,
                                         vpl011_virq - 32 + 1);
        }

        /* Get the optional property domain-cpupool */
        cpupool_node = dt_parse_phandle(node, "domain-cpupool", 0);
        if ( cpupool_node )
        {
            int pool_id = btcpupools_get_domain_pool_id(cpupool_node);
            if ( pool_id < 0 )
                panic("Error getting cpupool id from domain-cpupool (%d)\n",
                      pool_id);
            d_cfg.cpupool_id = pool_id;
        }

        if ( dt_property_read_u32(node, "max_grant_version", &val) )
            d_cfg.grant_opts = XEN_DOMCTL_GRANT_version(val);

        if ( dt_property_read_u32(node, "max_grant_frames", &val) )
        {
            if ( val > INT32_MAX )
                panic("max_grant_frames (%"PRIu32") overflow\n", val);
            d_cfg.max_grant_frames = val;
        }

        if ( dt_property_read_u32(node, "max_maptrack_frames", &val) )
        {
            if ( val > INT32_MAX )
                panic("max_maptrack_frames (%"PRIu32") overflow\n", val);
            d_cfg.max_maptrack_frames = val;
        }

        if ( dt_get_property(node, "sve", &val) )
        {
#ifdef CONFIG_ARM64_SVE
            unsigned int sve_vl_bits;
            bool ret = false;

            if ( !val )
            {
                /* Property found with no value, means max HW VL supported */
                ret = sve_domctl_vl_param(-1, &sve_vl_bits);
            }
            else
            {
                if ( dt_property_read_u32(node, "sve", &val) )
                    ret = sve_domctl_vl_param(val, &sve_vl_bits);
                else
                    panic("Error reading 'sve' property\n");
            }

            if ( ret )
                d_cfg.arch.sve_vl = sve_encode_vl(sve_vl_bits);
            else
                panic("SVE vector length error\n");
#else
            panic("'sve' property found, but CONFIG_ARM64_SVE not selected\n");
#endif
        }

        dt_property_read_string(node, "llc-colors", &llc_colors_str);
        if ( !llc_coloring_enabled && llc_colors_str )
            panic("'llc-colors' found, but LLC coloring is disabled\n");

        if ( is_pci_scan_enabled() )
            d_cfg.flags |= XEN_DOMCTL_CDF_vpci;

        /*
         * The variable max_init_domid is initialized with zero, so here it's
         * very important to use the pre-increment operator to call
         * domain_create() with a domid > 0. (domid == 0 is reserved for Dom0)
         */
        if ( flags & CDF_hardware )
            domid = 0;
        else
            domid = ++max_init_domid;

        d = domain_create(domid, &d_cfg, flags);
        if ( IS_ERR(d) )
            panic("Error creating domain %s (rc = %ld)\n",
                  dt_node_name(node), PTR_ERR(d));

        if ( llc_coloring_enabled &&
             (rc = domain_set_llc_colors_from_str(d, llc_colors_str)) )
            panic("Error initializing LLC coloring for domain %s (rc = %d)\n",
                  dt_node_name(node), rc);

        d->is_console = true;
        dt_device_set_used_by(node, d->domain_id);

        rc = construct_domU(d, node);
        if ( rc )
            panic("Could not set up domain %s (rc = %d)\n",
                  dt_node_name(node), rc);

        if ( d_cfg.flags & XEN_DOMCTL_CDF_xs_domain )
            set_xs_domid(d->domain_id);
    }

    if ( need_xenstore && xs_domid == DOMID_INVALID )
        panic("xenstore requested, but xenstore domain not present\n");

    initialize_domU_xenstore();
}

/*
 * Local variables:
 * mode: C
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * tab-width: 4
 * indent-tabs-mode: nil
 * End:
 */
