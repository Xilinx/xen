Xen cache coloring user guide
=============================

The cache coloring support in Xen allows to reserve Last Level Cache (LLC)
partition for Dom0, DomUs and Xen itself. Currently only ARM64 is supported.

In order to enable and use it, few steps are needed.

- Enable expert mode in Xen configuration file.

        CONFIG_EXPERT=y
- Enable cache coloring in Xen configuration file.

        CONFIG_CACHE_COLORING=y
- If needed, change the maximum number of colors in Xen configuration file
  (refer to menuconfig help for value meaning and when it should be changed).

        CONFIG_MAX_CACHE_COLORS=<n>
- If needed, change the amount of memory reserved for the buddy allocator either
  from the Xen configuration file, via the CONFIG_BUDDY_ALLOCATOR_SIZE value,
  or with the command line option. See `Colored allocator and buddy allocator`.
- Assign colors to each memory pool (Xen, Dom0/DomUs) using the
  `Color selection format`_ for `Coloring parameters`_ configuration.

Background
**********

Cache hierarchy of a modern multi-core CPU typically has first levels dedicated
to each core (hence using multiple cache units), while the last level is shared
among all of them. Such configuration implies that memory operations on one
core (e.g. running a DomU) are able to generate interference on another core
(e.g .hosting another DomU). Cache coloring allows eliminating this
mutual interference, and thus guaranteeing higher and more predictable
performances for memory accesses.
The key concept underlying cache coloring is a fragmentation of the memory
space into a set of sub-spaces called colors that are mapped to disjoint cache
partitions. Technically, the whole memory space is first divided into a number
of subsequent regions. Then each region is in turn divided into a number of
subsequent sub-colors. The generic i-th color is then obtained by all the
i-th sub-colors in each region.

.. raw:: html

    <pre>
                            Region j            Region j+1
                .....................   ............
                .                     . .
                .                       .
            _ _ _______________ _ _____________________ _ _
                |     |     |     |     |     |     |
                | c_0 | c_1 |     | c_n | c_0 | c_1 |
           _ _ _|_____|_____|_ _ _|_____|_____|_____|_ _ _
                    :                       :
                    :                       :...         ... .
                    :                            color 0
                    :...........................         ... .
                                                :
          . . ..................................:
    </pre>

There are two pragmatic lesson to be learnt.

1. If one wants to avoid cache interference between two domains, different
   colors needs to be used for their memory.

2. Color assignment must privilege contiguity in the partitioning. E.g.,
   assigning colors (0,1) to domain I  and (2,3) to domain  J is better than
   assigning colors (0,2) to I and (1,3) to J.

How to compute the number of colors
***********************************

To compute the number of available colors for a specific platform, the size of
an LLC way and the page size used by Xen must be known. The first parameter can
be found in the processor manual or can be also computed dividing the total
cache size by the number of its ways. The second parameter is the minimum amount
of memory that can be mapped by the hypervisor, thus dividing the way size by
the page size, the number of total cache partitions is found. So for example,
an Arm Cortex-A53 with a 16-ways associative 1 MiB LLC, can isolate up to 16
colors when pages are 4 KiB in size.

Cache layout is probed automatically by Xen itself, but a possibility to
manually set the way size it's left for the user to overcome failing situations
or for debugging/testing purposes. See `Coloring parameters`_ section for more
information on that.

Colors selection format
***********************

Regardless of the memory pool that has to be colored (Xen, Dom0/DomUs),
the color selection can be expressed using the same syntax. In particular a
comma-separated list of colors or ranges of colors is used.
Ranges are hyphen-separated intervals (such as `0-4`) and are inclusive on both
sides.

Note that:
 - no spaces are allowed between values.
 - no overlapping ranges or duplicated colors are allowed.
 - values must be written in ascending order.

Examples:

+---------------------+-----------------------------------+
|**Configuration**    |**Actual selection**               |
+---------------------+-----------------------------------+
|  1-2,5-8            | [1, 2, 5, 6, 7, 8]                |
+---------------------+-----------------------------------+
|  4-8,10,11,12       | [4, 5, 6, 7, 8, 10, 11, 12]       |
+---------------------+-----------------------------------+
|  0                  | [0]                               |
+---------------------+-----------------------------------+

Coloring parameters
*******************

LLC way size (as previously discussed), Xen colors and Dom0 colors can be set
using the appropriate command line parameters. See the relevant documentation in
"docs/misc/xen-command-line.pandoc".

DomUs colors can be set either in the xl configuration file (relative
documentation at "docs/man/xl.cfg.pod.5.in") or via Device Tree, also for
Dom0less configurations, as in the following example:

.. raw:: html

    <pre>
        xen,xen-bootargs = "console=dtuart dtuart=serial0 dom0_mem=1G dom0_max_vcpus=1 sched=null llc-way-size=64K xen-colors=0-1 dom0-colors=2-6";
        xen,dom0-bootargs "console=hvc0 earlycon=xen earlyprintk=xen root=/dev/ram0"

        dom0 {
            compatible = "xen,linux-zimage" "xen,multiboot-module";
            reg = <0x0 0x1000000 0x0 15858176>;
        };

        dom0-ramdisk {
            compatible = "xen,linux-initrd" "xen,multiboot-module";
            reg = <0x0 0x2000000 0x0 20638062>;
        };

        domU0 {
            #address-cells = <0x1>;
            #size-cells = <0x1>;
            compatible = "xen,domain";
            memory = <0x0 0x40000>;
            colors = "4-8,10,11,12";
            cpus = <0x1>;
            vpl011 = <0x1>;

            module@2000000 {
                compatible = "multiboot,kernel", "multiboot,module";
                reg = <0x2000000 0xffffff>;
                bootargs = "console=ttyAMA0";
            };

            module@30000000 {
                compatible = "multiboot,ramdisk", "multiboot,module";
                reg = <0x3000000 0xffffff>;
            };
        };
    </pre>

Please refer to the relative documentation in
"docs/misc/arm/device-tree/booting.txt".

Note that if no color configuration is provided for domains, they fallback to
the default one, which corresponds simply to all available colors.

Colored allocator and buddy allocator
*************************************

The colored allocator distributes pages based on color configurations of
domains so that each domains only gets pages of its own colors.
The colored allocator is meant as an alternative to the buddy allocator because
its allocation policy is by definition incompatible with the generic one. Since
the Xen heap is not colored yet, we need to support the coexistence of the two
allocators and some memory must be left for the buddy one.
The buddy allocator memory can be reserved from the Xen configuration file or
with the help of a command-line option.

Known issues and limitations
****************************

Cache coloring is intended only for embedded systems
####################################################

The current implementation aims to satisfy the need of predictability in
embedded systems with small amount of memory to be managed in a colored way.
Given that, some shortcuts are taken in the development. Expect worse
performances on larger systems.

The maximum number of colors supported is 32768
###############################################

The upper bound of the CONFIG_MAX_CACHE_COLORS range (which is an upper bound
too) is set to 2^15 = 32768 colors because of some limitation on the domain
configuration structure size used in domain creation. "uint16_t" is the biggest
integer type that fit the constraint and 2^15 is the biggest power of 2 it can
easily represent. This value is big enough for the generic case, though.

"xen,static-mem" isn't supported when coloring is enabled
#########################################################

In the domain configuration, "xen,static-mem" allows memory to be statically
allocated to the domain. This isn't possibile when cache coloring is enabled,
because that memory can't be guaranteed to be of the same colors assigned to
that domain.

Colored allocator can only make use of order-0 pages
####################################################

The cache coloring technique relies on memory mappings and on the smallest
amount of memory that can be mapped to achieve the maximum number of colors
(cache partitions) possible. This amount is what is normally called a page and,
in Xen terminology, the order-0 page is the smallest one. The fairly simple
colored allocator currently implemented, makes use only of such pages.
It must be said that a more complex one could, in theory, adopt higher order
pages if the colors selection contained adjacent colors. Two subsequent colors,
for example, can be represented by an order-1 page, four colors correspond to
an order-2 page, etc.

Fail to boot colored DomUs with large memory size
#################################################

If the Linux kernel used for Dom0 does not contain the upstream commit
3941552aec1e04d63999988a057ae09a1c56ebeb and uses the hypercall buffer device,
colored DomUs with memory size larger then 127 MB cannot be created. This is
caused by the default limit of this buffer of 64 pages. The solution is to
manually apply the above patch, or to check if there is an updated version of
the kernel in use for Dom0 that contains this change.
