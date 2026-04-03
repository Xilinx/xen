/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * DSU (DynamIQ Shared Unit) L3 Cache Partitioning support for ARM
 *
 * Copyright (C) 2026 Advanced Micro Devices, Inc.
 */
#include <xen/errno.h>
#include <xen/keyhandler.h>
#include <xen/lib.h>
#include <xen/param.h>
#include <xen/sched.h>
#include <xen/stop_machine.h>

#include <asm/dsu-cache.h>
#include <asm/sysregs.h>
#include <asm/system.h>


/*
 * DSU Partitioning cmdline format:
 *   C<cluster>:(W<range>:S<range>[,W<range>:S<range>...])[;C<cluster>:...]
 * Examples:
 *   C0:(W1:S1)                     # WG1->S1, WG0->S0
 *   C0:(W1:S1,W2:S2)               # WG1->S1, WG2->S2
 *   C1:(W1-3:S1-3)                 # WG1->S1, WG2->S2, WG3->S3
 *   C2:(W1-3:S2)                   # WG1,WG2,WG3->S2
 *
 * When omitted, a default mapping is applied: WG0,WG3->S0, WG1->S1, WG2->S2.
 *
 * Corner cases:
 *   C0:()                          # All WG default Xen
 *   C0:(W0:S1)                     # ERROR: WG0 reserved
 *   C0:(W1:S0)                     # ERROR: scheme 0 reserved
 *
 * Considering the limits the longest valid cmdline is about 304 chars.
 */
static char __initdata opt_dsu_config[320] = "";
string_param("dsu_part_config", opt_dsu_config);

static bool __initdata opt_dsu = false;
boolean_param("dsu_partitioning", opt_dsu);

static bool __initdata opt_dsu_allow_sharing = false;
boolean_param("dsu_part_sharing", opt_dsu_allow_sharing);

static struct dsu_info dsu;
bool __read_mostly dsu_enabled = false;
static bool __read_mostly dsu_allow_sharing = false;

static void __init dsu_init_cluster_config(struct dsu_cluster_info *cinfo)
{
    unsigned int w, i;

    for ( w = 0; w < DSU_WAY_GROUPS; w++ )
        cinfo->wg_scheme[w] = DSU_SCHEME_XEN;

    for ( i = 0; i < DSU_MAX_SCHEMES; i++ )
        cinfo->nr_users[i] = 0;
}

static unsigned int get_pcpu_cluster_id(unsigned int pcpu)
{
    return MPIDR_AFFINITY_LEVEL(cpu_logical_map(pcpu), 2);
}

static void cf_check dsu_dump_clusters_info(unsigned char key)
{
    unsigned int c, i, w;
    struct dsu_cluster_info *cinfo;
    struct domain *d;
    unsigned int cache_size, mask;
    char buf[256];
    int pos;

    printk(XENLOG_INFO "=== DSU Cluster Configuration Dump ===\n");

    for ( c = 0; c < dsu.num_clusters; c++ )
    {
        cinfo = &dsu.clusters[c];

        pos = scnprintf(buf, sizeof(buf), "Cluster %u: L3 size=%uKB, pCPUs=",
                        c, cinfo->l3_cache_size / 1024);
        for_each_online_cpu(i)
            if ( get_pcpu_cluster_id(i) == c )
                pos += scnprintf(buf + pos, sizeof(buf) - pos, "%u ", i);
        printk(XENLOG_INFO "%s\n", buf);

        pos = scnprintf(buf, sizeof(buf), "  Way-group mapping:");
        for ( w = 0; w < DSU_WAY_GROUPS; w++ )
            pos += scnprintf(buf + pos, sizeof(buf) - pos,
                             " %u", cinfo->wg_scheme[w]);
        printk(XENLOG_INFO "%s\n", buf);

        for ( i = 0; i < DSU_MAX_SCHEMES; i++ )
        {
            /* Compute mask from wg_scheme for this scheme */
            mask = 0;
            for ( w = 0; w < DSU_WAY_GROUPS; w++ )
                if ( cinfo->wg_scheme[w] == i )
                    mask |= (1U << w);

            if ( mask == 0 )
                continue;

            cache_size =
                (cinfo->l3_cache_size / DSU_WAY_GROUPS) * hweightl(mask);

            pos = scnprintf(buf, sizeof(buf),
                            "  Scheme %u: mask=0x%x size=%uKB users=",
                            i, mask, cache_size / 1024);

            if ( i == DSU_SCHEME_XEN )
                pos += scnprintf(buf + pos, sizeof(buf) - pos, "Xen ");

            rcu_read_lock(&domlist_read_lock);
            for_each_domain(d)
            {
                if ( d->arch.dsu_scheme == (int8_t)i )
                    pos += scnprintf(buf + pos, sizeof(buf) - pos,
                                     "d%u ", d->domain_id);
            }
            rcu_read_unlock(&domlist_read_lock);

            printk(XENLOG_INFO "%s\n", buf);
        }
    }

    printk(XENLOG_INFO "=== End of DSU Dump ===\n");
}

static int __init parse_dsu_partition_cmdline(void)
{
    const char *p = opt_dsu_config;
    unsigned int cluster;
    int wg_start, wg_end, s_start, s_end;
    int wg_count, s_count;
    unsigned int i;

    while ( *p )
    {
        if ( *p != 'C' )
        {
            printk(XENLOG_ERR
                   "DSU: expected 'C' at start of cluster, got '%c'\n",
                   *p);
            return -EINVAL;
        }
        p++;

        cluster = simple_strtoul(p, &p, 10);
        if ( cluster >= dsu.num_clusters )
        {
            printk(XENLOG_ERR
                   "DSU: cluster %u out of range (max: %u)\n",
                   cluster, dsu.num_clusters - 1);
            return -EINVAL;
        }

        if ( *p != ':' )
        {
            printk(XENLOG_ERR "DSU: expected ':', got '%c'\n", *p);
            return -EINVAL;
        }
        p++;

        if ( *p != '(' )
        {
            printk(XENLOG_ERR "DSU: expected '(', got '%c'\n", *p);
            return -EINVAL;
        }
        p++;

        while ( *p && *p != ')' )
        {
            if ( *p != 'W' )
            {
                printk(XENLOG_ERR
                       "DSU: invalid token '%c' inside WG (expected 'W')\n",
                       *p);
                return -EINVAL;
            }
            p++;

            wg_start = simple_strtoul(p, &p, 10);
            wg_end = wg_start;
            if ( *p == '-' )
            {
                p++;
                wg_end = simple_strtoul(p, &p, 10);
            }

            if ( wg_start <= 0 || wg_end <= 0 )
            {
                printk(XENLOG_ERR
                       "DSU: WG0 reserved for Xen (attempted %d-%d)\n",
                       wg_start, wg_end);
                return -EINVAL;
            }
            if ( wg_start > wg_end )
            {
                printk(XENLOG_ERR
                       "DSU: reversed WG range (%d-%d)\n",
                       wg_start, wg_end);
                return -EINVAL;
            }
            if ( wg_start >= DSU_WAY_GROUPS || wg_end >= DSU_WAY_GROUPS )
            {
                printk(XENLOG_ERR
                       "DSU: WG out of range (%d-%d, max %u)\n",
                       wg_start, wg_end, DSU_WAY_GROUPS - 1);
                return -EINVAL;
            }

            if ( *p != ':' )
            {
                printk(XENLOG_ERR "DSU: expected ':', got '%c'\n", *p);
                return -EINVAL;
            }
            p++;

            if ( *p != 'S' )
            {
                printk(XENLOG_ERR "DSU: expected 'S', got '%c'\n", *p);
                return -EINVAL;
            }
            p++;

            s_start = simple_strtoul(p, &p, 10);
            s_end = s_start;
            if ( *p == '-' )
            {
                p++;
                s_end = simple_strtoul(p, &p, 10);
            }

            if ( s_start == 0 || s_end == 0 )
            {
                printk(XENLOG_ERR
                       "DSU: scheme 0 reserved for Xen (attempted %d-%d)\n",
                       s_start, s_end);
                return -EINVAL;
            }
            if ( s_start > s_end )
            {
                printk(XENLOG_ERR
                       "DSU: reversed scheme range (%d-%d)\n",
                       s_start, s_end);
                return -EINVAL;
            }
            if ( s_start >= DSU_MAX_SCHEMES || s_end >= DSU_MAX_SCHEMES )
            {
                printk(XENLOG_ERR
                       "DSU: scheme out of range (%d-%d, max %u)\n",
                       s_start, s_end, DSU_MAX_SCHEMES - 1);
                return -EINVAL;
            }

            wg_count = wg_end - wg_start + 1;
            s_count = s_end - s_start + 1;
            if ( s_count != 1 && s_count != wg_count )
            {
                printk(XENLOG_ERR
                       "DSU: scheme range mismatch (wg_count=%d, s_count=%d)\n",
                       wg_count, s_count);
                return -EINVAL;
            }

            for ( i = 0; i < wg_count; i++ )
            {
                int wg = wg_start + i;
                int scheme = (s_count == 1) ? s_start : (s_start + i);

                if ( dsu.clusters[cluster].wg_scheme[wg] != DSU_SCHEME_XEN )
                {
                    printk(XENLOG_ERR
                           "DSU: WG%d already assigned to S%d (cluster %u)\n",
                           wg, dsu.clusters[cluster].wg_scheme[wg], cluster);
                    return -EINVAL;
                }
                dsu.clusters[cluster].wg_scheme[wg] = scheme;
            }

            if ( *p == ',' )
            {
                p++;
                continue;
            }
            else if ( *p != ')' && *p != '\0' )
            {
                printk(XENLOG_ERR
                       "DSU: invalid separator '%c' (expected ',' or ')')\n",
                       *p);
                return -EINVAL;
            }
        }

        if ( *p != ')' )
        {
            printk(XENLOG_ERR "DSU: expected ')', got '%c'\n", *p);
            return -EINVAL;
        }
        p++;

        if ( *p == ';' )
        {
            p++;
            continue;
        }
        else if ( *p != '\0' && *p != 'C' )
        {
            printk(XENLOG_ERR "DSU: invalid token '%c' after cluster\n", *p);
            return -EINVAL;
        }
    }

    return 0;
}

/*
 * Apply a default mapping for clusters that have no explicit configuration
 * from the command line. WG0 and WG3 are assigned to Xen (S0), WG1 to S1
 * and WG2 to S2. With the current limits (max 8 pCPUs, 4 clusters) each
 * cluster has at most 2 cores, so at most 2 domain schemes plus Xen can
 * be active per cluster.
 */
static void __init dsu_apply_default_mapping(void)
{
    unsigned int i, w;
    bool all_xen;

    for ( i = 0; i < dsu.num_clusters; i++ )
    {
        all_xen = true;
        for ( w = 1; w < DSU_WAY_GROUPS; w++ )
        {
            if ( dsu.clusters[i].wg_scheme[w] != DSU_SCHEME_XEN )
            {
                all_xen = false;
                break;
            }
        }

        if ( all_xen )
        {
            dsu.clusters[i].wg_scheme[0] = DSU_SCHEME_XEN;
            dsu.clusters[i].wg_scheme[1] = 1;
            dsu.clusters[i].wg_scheme[2] = 2;
            dsu.clusters[i].wg_scheme[3] = DSU_SCHEME_XEN;

            printk(XENLOG_INFO
                   "DSU: cluster %u using default mapping (WG0,WG3->S0, WG1->S1, WG2->S2)\n",
                   i);
        }
    }
}

static int __init dsu_probe(void)
{
    unsigned int max_cluster_id = 0;
    unsigned int num_clusters_seen = 0;
    bool cluster_seen[DSU_MAX_CLUSTERS] = { false };
    unsigned int cpu, i;
    uint64_t csselr_save, ccsidr, aa64mmfr2;
    unsigned int line_size, ways, sets, l3_cache_size;

    /*
     * Cluster IDs from MPIDR are used directly as array indices, so they
     * must be contiguous and 0-based. Sparse IDs (as permitted by the
     * ARM ARM) are detected and rejected after this loop.
     */
    for_each_online_cpu(cpu)
    {
        unsigned long mpidr = cpu_logical_map(cpu);
        unsigned int cluster_id = MPIDR_AFFINITY_LEVEL(mpidr, 2);

        if ( cpu >= DSU_MAX_CPUS )
        {
            printk(XENLOG_ERR "DSU: pCPU %u exceeds max supported %u\n",
                   cpu, DSU_MAX_CPUS - 1);
            return -EINVAL;
        }

        if ( cluster_id >= DSU_MAX_CLUSTERS )
        {
            printk(XENLOG_ERR "DSU: cluster ID %u exceeds max supported %u\n",
                   cluster_id, DSU_MAX_CLUSTERS - 1);
            return -EINVAL;
        }

        if ( !cluster_seen[cluster_id] )
        {
            cluster_seen[cluster_id] = true;
            num_clusters_seen++;
        }

        if ( cluster_id > max_cluster_id )
            max_cluster_id = cluster_id;
    }

    dsu.num_clusters = max_cluster_id + 1;

    if ( num_clusters_seen != dsu.num_clusters )
    {
        printk(XENLOG_ERR "DSU: sparse cluster IDs detected (%u seen, expected %u contiguous)\n",
               num_clusters_seen, dsu.num_clusters);
        return -EINVAL;
    }

    /*
     * Check for FEAT_CCIDX. When present, CCSIDR_EL1 uses a different
     * field layout with wider associativity and set count fields. This
     * implementation does not handle the extended format.
     */
    aa64mmfr2 = READ_SYSREG(ID_AA64MMFR2_EL1);
    if ( (aa64mmfr2 >> ID_AA64MMFR2_CCIDX_SHIFT) & 0xF )
    {
        printk(XENLOG_ERR
               "DSU: FEAT_CCIDX detected, not supported by DSU partitioning\n");
        return -ENODEV;
    }

    /*
     * Select L3 in CSSELR_EL1 to read its geometry from CCSIDR_EL1.
     * This runs on CPU0 during init, so all clusters get the same value.
     * To read per-cluster geometry we would need to execute the read on
     * a CPU belonging to each cluster, but on the target platform all
     * clusters share the same DSU configuration.
     */
    csselr_save = READ_SYSREG(CSSELR_EL1);
    WRITE_SYSREG(2 << 1, CSSELR_EL1);
    isb();

    ccsidr = READ_SYSREG(CCSIDR_EL1);
    line_size = 1 << ((ccsidr & 0x7) + 4);
    ways = ((ccsidr >> 3) & 0x3FF) + 1;
    sets = ((ccsidr >> 13) & 0x7FFF) + 1;
    l3_cache_size = ways * sets * line_size;

    WRITE_SYSREG(csselr_save, CSSELR_EL1);
    isb();

    for ( i = 0; i < dsu.num_clusters; i++ )
    {
        dsu.clusters[i].l3_cache_size = l3_cache_size;
        dsu_init_cluster_config(&dsu.clusters[i]);
    }

    spin_lock_init(&dsu.lock);

    return 0;
}

static int __init cf_check dsu_part_program_cpu(void *data)
{
    unsigned int cluster_id;
    struct dsu_cluster_info *cinfo;
    uint32_t partcr = 0;
    unsigned int wg, sc;

    cluster_id = get_pcpu_cluster_id(smp_processor_id());
    cinfo = &dsu.clusters[cluster_id];

    for ( wg = 0; wg < DSU_WAY_GROUPS; wg++ )
    {
        sc = cinfo->wg_scheme[wg];
        if ( sc < DSU_MAX_SCHEMES )
            partcr |= (1U << (sc * DSU_WAY_GROUPS + wg));
    }

    /*
     * Program Cluster Partition Control Register and switch to Xen scheme.
     * Only CLUSTERTHREADSID_EL1 (CPU core traffic) is set here.
     * CLUSTERACPSID_EL1 (ACP) and CLUSTERSTASHSID_EL1 (stash) default to
     * scheme 0 at reset, which is the Xen scheme, so no explicit
     * programming is needed at boot.
     */
    WRITE_SYSREG(partcr, CLUSTERPARTCR_EL1);
    WRITE_SYSREG(DSU_SCHEME_XEN & DSU_SCHEME_MASK, CLUSTERTHREADSID_EL1);
    isb();

    return 0;
}

void dsu_deconfigure_domain(struct domain *d)
{
    unsigned int i;
    int8_t scheme;

    if ( !dsu_enabled )
        return;

    scheme = d->arch.dsu_scheme;
    if ( scheme <= DSU_SCHEME_XEN )
        return;

    spin_lock(&dsu.lock);

    for ( i = 0; i < dsu.num_clusters; i++ )
    {
        if ( dsu.clusters[i].nr_users[scheme] > 0 )
            dsu.clusters[i].nr_users[scheme]--;
    }

    d->arch.dsu_scheme = DSU_SCHEME_XEN;

    spin_unlock(&dsu.lock);
}

int dsu_configure_domain(struct domain *d, int8_t scheme)
{
    unsigned int i;
    int rc = 0;

    if ( !dsu_enabled )
        return 0;

    if ( scheme == DSU_SCHEME_INVALID )
    {
        printk(XENLOG_ERR "DSU: no scheme specified for d%d\n",
               d->domain_id);
        return -EINVAL;
    }

    if ( scheme < DSU_SCHEME_MIN || scheme > DSU_SCHEME_MAX )
    {
        printk(XENLOG_ERR
               "DSU: invalid scheme %d for d%d\n",
               (int)scheme, d->domain_id);
        return -EINVAL;
    }

    spin_lock(&dsu.lock);

    for ( i = 0; i < dsu.num_clusters; i++ )
    {
        if ( dsu.clusters[i].nr_users[scheme] > 0 && !dsu_allow_sharing )
        {
            printk(XENLOG_ERR
                   "DSU: scheme %d on cluster %u in use, cannot assign to d%d\n",
                   scheme, i, d->domain_id);
            rc = -EBUSY;
            goto out;
        }
    }

    for ( i = 0; i < dsu.num_clusters; i++ )
        dsu.clusters[i].nr_users[scheme]++;

    d->arch.dsu_scheme = scheme;

    spin_unlock(&dsu.lock);

    printk(XENLOG_DEBUG "DSU: d%d configured with scheme %d\n",
           d->domain_id, scheme);

    return 0;

out:
    spin_unlock(&dsu.lock);
    return rc;
}

void dsu_switch_dom_scheme(struct domain *d)
{
    int8_t scheme;

    if ( !dsu_enabled )
        return;

    scheme = d->arch.dsu_scheme;

    if ( scheme <= DSU_SCHEME_XEN )
        return;

    /*
     * DSB ST drains the store buffer before switching the cache partition
     * scheme, preventing pending stores from allocating dirty L3 lines
     * under the new partition. A full DSB SY would also cover outstanding
     * loads, but load misses only produce clean lines that are naturally
     * evicted, while DSB SY introduces measurable latency spikes.
     */
    dsb(st);
    WRITE_SYSREG(scheme & DSU_SCHEME_MASK, CLUSTERTHREADSID_EL1);
    WRITE_SYSREG(scheme & DSU_SCHEME_MASK, CLUSTERACPSID_EL1);
    WRITE_SYSREG(scheme & DSU_SCHEME_MASK, CLUSTERSTASHSID_EL1);
    isb();
}

/*
 * Verify that the domain's assigned scheme has at least one way-group
 * on every cluster where the domain has a vCPU pinned.  This must run
 * after vCPU creation, when v->processor is meaningful (i.e. from
 * arch_domain_creation_finished).
 */
int dsu_check_domain(struct domain *d)
{
    int8_t scheme;
    struct vcpu *v;
    unsigned int cluster_id, w;
    bool has_wg;

    if ( !dsu_enabled )
        return 0;

    scheme = d->arch.dsu_scheme;
    if ( scheme <= DSU_SCHEME_XEN )
        return 0;

    for_each_vcpu ( d, v )
    {
        cluster_id = get_pcpu_cluster_id(v->processor);

        has_wg = false;
        for ( w = 0; w < DSU_WAY_GROUPS; w++ )
        {
            if ( dsu.clusters[cluster_id].wg_scheme[w] == scheme )
            {
                has_wg = true;
                break;
            }
        }

        if ( !has_wg )
        {
            printk(XENLOG_ERR
                   "DSU: scheme %d has no way-group on cluster %u (vCPU %u, pCPU %u) for d%d\n",
                   scheme, cluster_id, v->vcpu_id, v->processor, d->domain_id);
            return -EINVAL;
        }
    }

    return 0;
}

int __init dsu_init(void)
{
    int ret;

    dsu_enabled = false;
    if ( !opt_dsu )
    {
        printk(XENLOG_DEBUG
               "DSU: L3 cache partitioning disabled via cmdline\n");
        return 0;
    }

    if ( sched_get_id_by_name("null") != scheduler_id() )
    {
        printk(XENLOG_ERR
               "DSU: L3 partitioning requires null scheduler\n");
        return -ENOSYS;
    }

    /* Verify that an L3 cache exists (CLIDR_EL1 Ctype3) */
    if ( ((READ_SYSREG(CLIDR_EL1) >> CLIDR_CTYPEn_SHIFT(3)) &
          CLIDR_CTYPEn_MASK) == 0 )
    {
        printk(XENLOG_WARNING
               "DSU: no L3 cache detected, disabling cache partitioning\n");
        return -ENODEV;
    }

    ret = dsu_probe();
    if ( ret )
    {
        printk(XENLOG_ERR "DSU: failed to populate info (%d)\n", ret);
        return ret;
    }

    if ( opt_dsu_config[0] )
    {
        ret = parse_dsu_partition_cmdline();
        if ( ret )
        {
            printk(XENLOG_ERR "DSU: failed to parse partition cmdline (%d)\n",
                   ret);
            return ret;
        }
    }

    dsu_apply_default_mapping();

    ret = stop_machine_run(dsu_part_program_cpu, NULL, NR_CPUS);
    if ( ret )
    {
        printk(XENLOG_ERR
               "DSU: failed to program clusters partitions (%d)\n", ret);
        return ret;
    }

    register_keyhandler('D', dsu_dump_clusters_info, "Dump DSU cache partitions", 0);

    dsu_allow_sharing = opt_dsu_allow_sharing;
    dsu_enabled = true;

    printk(XENLOG_WARNING
           "DSU: L3 cache partitioning is experimental\n");
    printk(XENLOG_INFO "DSU: L3 cache partitioning enabled (%s sharing)\n",
           dsu_allow_sharing ? "with" : "without");
    return 0;
}

/*
 * Local variables:
 * mode: C
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * End:
 */
