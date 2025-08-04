/* SPDX-License-Identifier: GPL-2.0-only */

#include <xen/bootfdt.h>
#include <xen/device_tree.h>
#include <xen/dom0less-build.h>
#include <xen/domain.h>
#include <xen/grant_table.h>
#include <xen/llc-coloring.h>
#include <xen/fdt-virtio.h>
#include <xen/sched.h>
#include <asm/setup.h>

#include <public/bootfdt.h>
#include <public/domctl.h>

static enum {
    NONE,
    EXPLICIT,
    DEDUCED,
} domid_policy __initdata = NONE;

static void __init apply_dom0less_domid_policy(struct boot_domain *bd,
                                               unsigned int flags,
                                               struct dt_device_node *node)
{
    if ( bd->domid == DOMID_INVALID && domid_policy != EXPLICIT )
    {
        domid_policy = DEDUCED;
        if ( flags & CDF_hardware )
            bd->domid = 0;
        else
            bd->domid = ++max_init_domid;
    }
    else if ( bd->domid != DOMID_INVALID && domid_policy != DEDUCED )
        domid_policy = EXPLICIT;
    else
        panic("can't mix domains with and without domid properties. domain node %s\n",
              dt_node_name(node));

    /*
     * At this moment policy is verified and domid could have valid value or
     * DOMID_INVALID. In both cases, domid_alloc() has to be called to
     * - verify and store assigned domain ID value
     * - or allocate domain ID
     */
    bd->domid = domid_alloc(bd->domid);
    if ( bd->domid == DOMID_INVALID )
        panic("Error allocating ID for domain node %s\n", dt_node_name(node));
}

static void __init parse_cpu_affinity_node(const struct dt_device_node *node,
                                           struct boot_domain *bd)
{
    struct dt_device_node *np;

    dt_for_each_child_node(node, np)
    {
        const char *hard_affinity_str = NULL;
        uint32_t max_vcpus = bd->create_cfg.max_vcpus, vcpu_id;
        int rc;

        if ( !dt_device_is_compatible(np, "xen,vcpu") )
            continue;

        if ( !dt_property_read_u32(np, "id", &vcpu_id) )
            panic("Invalid xen,vcpu node for domain %s\n", dt_node_name(node));

        if ( vcpu_id >= max_vcpus )
            panic("Invalid vcpu_id %u for domain %s, max_vcpus=%u\n", vcpu_id,
                  dt_node_name(node), max_vcpus);

        if ( !bd->hard_affinity )
        {
            bd->hard_affinity = xzalloc_array(cpumask_t, max_vcpus);
            if ( !bd->hard_affinity )
                panic("Error allocating hard_affinity buffer for %s\n",
                      dt_node_name(node));
        }

        rc = dt_property_read_string(np, "hard-affinity", &hard_affinity_str);
        if ( rc < 0 )
            continue;

        cpumask_clear(&bd->hard_affinity[vcpu_id]);
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
                cpumask_set_cpu(start, &bd->hard_affinity[vcpu_id]);

            if ( *hard_affinity_str == ',' )
                hard_affinity_str++;
            else if ( *hard_affinity_str != '\0' )
                break;
        }
    }
}

int __init parse_dom0less_node(struct dt_device_node *node,
                               struct boot_domain *bd)
{
    struct xen_domctl_createdomain *d_cfg = &bd->create_cfg;
    unsigned int *flags = &bd->create_flags;
    struct dt_device_node *cpupool_node;
    uint32_t val;
    bool has_dtb = false;
    bool iommu = false;
    const char *dom0less_iommu = NULL;

    if ( !dt_device_is_compatible(node, "xen,domain") )
        return -ENOENT;

    *flags = 0;
    *d_cfg = (struct xen_domctl_createdomain){
        .max_evtchn_port = 1023,
        .max_grant_frames = -1,
        .max_maptrack_frames = -1,
        .grant_opts = XEN_DOMCTL_GRANT_version(opt_gnttab_max_version),
    };

    if ( dt_property_read_u32(node, "capabilities", &val) )
    {
        if ( val & ~DOMAIN_CAPS_MASK )
            panic("Invalid capabilities (%"PRIx32")\n", val);

        if ( val & DOMAIN_CAPS_CONTROL )
            *flags |= CDF_privileged;

        if ( val & DOMAIN_CAPS_HARDWARE )
        {
            if ( hardware_domain )
                panic("Only 1 hardware domain can be specified! (%pd)\n",
                        hardware_domain);

#ifdef CONFIG_GRANT_TABLE
            d_cfg->max_grant_frames = gnttab_dom0_frames();
#endif
            d_cfg->max_evtchn_port = -1;
            *flags |= CDF_hardware;
            iommu = true;
        }

        if ( val & DOMAIN_CAPS_XENSTORE )
        {
            d_cfg->flags |= XEN_DOMCTL_CDF_xs_domain;
            d_cfg->max_evtchn_port = -1;
        }

        if ( val & DOMAIN_CAPS_DEVICE_MODEL )
            d_cfg->flags |= XEN_DOMCTL_CDF_device_model;

        if ( val & DOMAIN_CAPS_NOT_HYPERCALL_TARGET )
            d_cfg->flags |= XEN_DOMCTL_CDF_not_hypercall_target;
    }

    if ( dt_find_property(node, "xen,static-mem", NULL) )
    {
        if ( llc_coloring_enabled )
            panic("LLC coloring and static memory are incompatible\n");

        *flags |= CDF_staticmem;
    }

    if ( dt_property_read_bool(node, "direct-map") )
    {
        if ( !(*flags & CDF_hardware) && !(*flags & CDF_staticmem) )
            panic("direct-map is not valid for domain %s without static allocation.\n",
                  dt_node_name(node));

        *flags |= CDF_directmap;
    }

    if ( !dt_property_read_u32(node, "cpus", &d_cfg->max_vcpus) )
        panic("Missing property 'cpus' for domain %s\n",
              dt_node_name(node));

    parse_cpu_affinity_node(node, bd);

    if ( !dt_property_read_string(node, "passthrough", &dom0less_iommu) )
    {
        if ( *flags & CDF_hardware )
            panic("Don't specify passthrough for hardware domain\n");

        if ( !strcmp(dom0less_iommu, "enabled") )
            iommu = true;
    }

    if ( *flags & CDF_hardware )
    {
        if ( !llc_coloring_enabled )
            *flags |= CDF_directmap;

        if ( !(*flags & CDF_directmap) && !iommu_enabled )
            panic("non-direct mapped hardware domain requires iommu\n");
    }

    if ( dt_find_compatible_child_node(node, NULL, "multiboot,device-tree") )
    {
        if ( *flags & CDF_hardware )
            panic("\"multiboot,device-tree\" incompatible with hardware domain\n");

        has_dtb = true;
    }

    if ( iommu_enabled && (iommu || has_dtb) )
        d_cfg->flags |= XEN_DOMCTL_CDF_iommu;

    /* Get the optional property domain-cpupool */
    cpupool_node = dt_parse_phandle(node, "domain-cpupool", 0);
    if ( cpupool_node )
    {
        int pool_id = btcpupools_get_domain_pool_id(cpupool_node);
        if ( pool_id < 0 )
            panic("Error getting cpupool id from domain-cpupool (%d)\n",
                  pool_id);
        d_cfg->cpupool_id = pool_id;
    }

    if ( dt_property_read_u32(node, "max_grant_version", &val) )
        d_cfg->grant_opts = XEN_DOMCTL_GRANT_version(val);

    if ( dt_property_read_u32(node, "max_grant_frames", &val) )
    {
        if ( val > INT32_MAX )
            panic("max_grant_frames (%"PRIu32") overflow\n", val);
        d_cfg->max_grant_frames = val;
    }

    if ( dt_property_read_u32(node, "max_maptrack_frames", &val) )
    {
        if ( val > INT32_MAX )
            panic("max_maptrack_frames (%"PRIu32") overflow\n", val);
        d_cfg->max_maptrack_frames = val;
    }

#ifdef CONFIG_HAS_LLC_COLORING
    dt_property_read_string(node, "llc-colors", &bd->llc_colors_str);
    if ( !llc_coloring_enabled && bd->llc_colors_str )
        panic("'llc-colors' found, but LLC coloring is disabled\n");
#endif

#ifdef CONFIG_VIRTIO_MMIO_NON_BLOCKING
    if ( parse_virtio(node, bd) )
        panic("Failed to parse virtio\n");
#endif

    bd->domid = DOMID_INVALID;
    if ( dt_property_read_u32(node, "domid", &val) )
    {
        if ( val >= DOMID_FIRST_RESERVED )
            panic("bad domid for node=%s domid=%u\n", dt_node_name(node), val);
        if ( val && (*flags & CDF_hardware) )
            panic("hwdom is d%u, but must be d0\n", val);
        if ( !val && !(*flags & CDF_hardware) )
            panic("can't create non-hwdom d0\n");

        bd->domid = val;
    }

    apply_dom0less_domid_policy(bd, *flags, node);

    /* Default to PVH, if available */
    if ( IS_ENABLED(CONFIG_HVM) )
        bd->create_cfg.flags |= XEN_DOMCTL_CDF_hvm;

    if ( !dt_property_read_u64(node, "memory", &bd->memory) )
        panic("missing memory binding for %s.\n", dt_node_name(node));

    return arch_parse_dom0less_node(node, bd);
}
