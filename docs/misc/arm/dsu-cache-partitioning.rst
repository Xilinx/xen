.. SPDX-License-Identifier: CC-BY-4.0

Arm DSU-600AE L3 Cache Partitioning
====================================

Overview
--------

The Arm DynamIQ Shared Unit (DSU-600AE) manages the shared L3 cache
for a DynamIQ cluster. The DSU supports hardware-assisted cache
partitioning through two mechanisms:

- Way-groups (WG): the L3 cache ways are divided into up to 4
  way-groups (WG0-WG3). Each way-group represents a fixed fraction
  (1/4) of the total L3 associativity.

- Schemes (S0-S7): a scheme is a named partition that aggregates
  one or more way-groups. The mapping between way-groups and schemes
  is programmed in the ``CLUSTERPARTCR_EL1`` register.

At runtime, each CPU core is assigned to a scheme via
``CLUSTERTHREADSID_EL1``. Cache allocations from that core are
restricted to the way-groups belonging to its active scheme. Lookups
(hits) are not restricted: a core can hit on any cache line regardless
of scheme, but new allocations (fills) go only into the assigned
way-groups.

This implementation exposes DSU cache partitioning to Xen domains:
the hypervisor programs the way-group to scheme mapping at boot via
``CLUSTERPARTCR_EL1``, and switches ``CLUSTERTHREADSID_EL1``,
``CLUSTERACPSID_EL1`` and ``CLUSTERSTASHSID_EL1`` on every context
switch so that each domain allocates into its assigned cache
partition. Scheme 0 and way-group 0 are reserved for Xen itself.

Each domain is assigned a single scheme that applies to all its pCPUs.
Xen resolves the cluster membership internally.

Hardware and implementation limits
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

The DSU-600AE provides 4 way-groups (WG0-WG3) and 8 schemes (S0-S7) per
cluster. The current implementation supports up to 8 physical CPUs and
up to 4 clusters. Cluster IDs derived from MPIDR must be contiguous and
0-based.

Scheduler requirements
----------------------

The active scheme (``CLUSTERTHREADSID_EL1``) is per-core, so each core
can use a different scheme within the same cluster. At context switch,
Xen writes the domain scheme into ``CLUSTERTHREADSID_EL1``. This only
works correctly when each vCPU is permanently bound to a specific pCPU,
because migrating a vCPU to a different pCPU would change which core
runs the domain.

Xen must therefore run with the null scheduler so that vCPUs remain
pinned to specific physical CPUs (with the null scheduler each vCPU
corresponds to exactly one pCPU). Any scheduler that can migrate vCPUs
between cores (e.g. credit) is incompatible with the current
implementation.

Boot-time configuration
-----------------------

Use ``dsu_partitioning=1`` on Xen cmdline to enable DSU cache
partitioning. By default, it is disabled.

When no explicit way-group to scheme mapping is provided, a default
mapping is applied: WG0 and WG3 are assigned to Xen (S0), while WG1
maps to S1 and WG2 maps to S2. With the current implementation limits
(8 pCPUs, 4 clusters) each cluster has at most 2 cores, so at most 2
domain schemes plus Xen can be active per cluster. This default avoids
wasting a way-group by giving Xen two way-groups (50% of the L3) and
leaving two for domains. Users who need a different distribution can
override this with ``dsu_part_config``.

Use ``dsu_part_config=`` on Xen cmdline to override the default mapping.
Grammar::

  dsu_part_config=C<cluster>:(W<range>:S<range>[,W<range>:S<range>...])[;C<cluster>:...]

Where:

- WG0 and Scheme 0 are reserved for Xen (cannot be reassigned).
- Unassigned WGs default to scheme 0.

Examples::

  C0:(W1:S1)                     # WG1->S1, WG0->S0
  C0:(W1:S1,W2:S2)               # WG1->S1, WG2->S2
  C1:(W1-3:S1-3)                 # WG1->S1, WG2->S2, WG3->S3
  C2:(W1-3:S2)                   # WG1,WG2,WG3->S2

By default, multiple domains on the same cluster cannot share a scheme.
To allow scheme sharing between domains on the same cluster, set
``dsu_part_sharing=1`` on the Xen command line.

Dom0 configuration
------------------

When DSU partitioning is enabled, dom0 must be assigned a scheme via
the ``dom0-dsu-part`` parameter on the Xen command line::

  dom0-dsu-part=3

Domain configuration (xl)
-------------------------

In the domain .cfg file, use ``dsu_part`` to assign the domain to a
scheme::

  dsu_part = 3

Scheme 0 is reserved for Xen. Scheme IDs range from 1 to 7. When DSU
partitioning is enabled, every domain must have a scheme explicitly
assigned. Domain creation will fail if no scheme is specified.

Dom0less (Device Tree)
----------------------

On ``/chosen/<domain>`` node (``compatible = "xen,domain"``), add::

  dsu-part = <3>;

The property contains a single cell specifying the scheme number.

Runtime debugging
-----------------

When DSU partitioning is enabled, press the ``D`` key on the Xen
console (keyhandler) to dump the current partition layout for all
clusters, including way-group mappings, scheme masks and domain
assignments.

Limitations
-----------

- Requires ``CONFIG_DSU_CACHE_PARTITIONING`` at build time.
- Only supported using the null scheduler.
- Up to 8 physical CPUs (IDs 0-7) and up to 4 clusters. The DSU-600AE
  hardware provides 4 way-groups and 8 schemes per cluster.
- Cluster IDs from MPIDR must be contiguous and 0-based. Sparse IDs
  are not currently supported.
- Only tested on Arm A78AE with DSU-600AE.
- Cannot change scheme/way-group assignment at runtime.
- FEAT_CCIDX (extended CCSIDR_EL1 format) is not supported. The L3
  geometry parsing assumes the non-CCIDX field layout; probing will
  fail on cores that implement this feature.

References
----------

.. [Arm TRM] Arm DSU-AE Technical Reference Manual: AArch64 control register
   summary; L3 cache partitioning. (CLUSTERPARTCR_EL1, CLUSTERTHREADSID_EL1).
   https://developer.arm.com/documentation/101322/0101
