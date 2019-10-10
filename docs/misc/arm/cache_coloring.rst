Xen coloring support user's guide
=================================

The cache coloring support in Xen allows to reserve last level cache partition
for Dom0, DomUs and Xen itself. Currently only ARM64 is supported.

In order to enable and use it, few steps are needed.

- Enable coloring in XEN configuration file.

        CONFIG_COLORING=y

- Enable/disable debug information (optional).

        CONFIG_COLORING_DEBUG=y/n

Before digging into configuration instructions, configurers should first
understand the basics of cache coloring.

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


Color(s) selection format
**************************

Regardless of the domain that has to be colored (Dom0, DomUs and Xen),
the color selection can be expressed using the same syntax.  In particular,
the latter is expressed as a comma-separated list of hyphen-separated intervals
of color numbers, as in `0-4,5-8,10-15`.  Ranges are always represented using
strings. Note that no spaces are allowed.

The number of available colors depends on the LLC layout of the specific
platform and determines the maximum allowed value.  This number can be either
calculated [#f1]_ or read from the output given by the hypervisor during boot,
if DEBUG logging is enabled.

Examples:

+---------------------+-----------------------------------+
|**Configuration**    |**Actual selection**               |
+---------------------+-----------------------------------+
|  1-2,5-8            | [1, 2, 5, 6, 7, 8]                |
+---------------------+-----------------------------------+
|  0-8,3-8            | [0, 1, 2, 3, 4, 5, 6, 7, 8]       |
+---------------------+-----------------------------------+
|  0-0                | [0]                               |
+---------------------+-----------------------------------+

General coloring parameters
***************************

Four additional parameters in the Xen command line are used to define the
underlying coloring policy, which is not directly configurable otherwise.

Please refer to the relative documentation in docs/man/xl.cfg.pod.5.in.

Dom0less support
****************
Support for the Dom0less experimental features is provided. Color selection for
a virtual machine is defined by the attribute `colors`, whose format is not a
string for ranges list, but a bitmask. It suffices to set all and only the bits
having a position equal to the chosen colors, leaving unset all the others. For
example, if we choose 8 colors out of 16, we can use a bitmask with 8 bits set
and 8 bit unset, like:

- `0xff00` -> `1111 1111 0000 0000`
- `0x0ff0` -> `0000 1111 1111 0000`
- `0x3c3c` -> `0011 1100 0011 1100`

Configuration example:

.. raw:: html

    <pre>
        xen,xen-bootargs = "console=dtuart dtuart=serial0 dom0_mem=1G dom0_max_vcpus=1 sched=null way_size=65536 xen_colors=0-1 dom0_colors=2-6";
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
            colors = <0x0 0x0f00>;
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
docs/misc/arm/device-tree/booting.txt.


Known issues
************

Explicitly define way_size in QEMU
##################################

Currently, QEMU does not have a comprehensive cache model, so the cache coloring
support fails to detect a cache geometry where to operate. In this case, the
boot hangs as soon as the Xen image is loaded. To overcome this issue, it is
enough to specify the way_size parameter in the command line. Any multiple
greater than 1 of the page size allows the coloring mechanism to work, but the
precise behavior on the system that QEMU is emulating can be obtained with its
way_size. For instance, set way_size=65536.


Fail to boot colored DomUs with large memory size
#################################################

If the kernel used for Dom0 does not contain the upstream commit
3941552aec1e04d63999988a057ae09a1c56ebeb and uses the hypercall buffer device,
colored DomUs with memory size larger then 127 MB cannot be created. This is
caused by the default limit of this buffer of 64 pages. The solution is to
manually apply the above patch, or to check if there is an updated version of
the kernel in use for Dom0 that contains this change.

Notes:
******

.. [#f1] To compute the number of available colors on a platform, one can simply
  divide `way_size` by `page_size`, where: `page_size` is the size of the page
  used on the system (usually 4 KiB); `way_size` is size of each LLC way.  For
  example, an Arm Cortex-A53 with a 16-ways associative 1 MiB LLC enable 16
  colors, when pages are 4 KiB.


