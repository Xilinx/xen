Xen cache coloring user guide
=============================

The cache coloring support in Xen allows to reserve Last Level Cache (LLC)
partitions for Dom0, DomUs and Xen itself. Currently only ARM64 is supported.

To compile LLC coloring support set ``CONFIG_LLC_COLORING=y``.

If needed, change the maximum number of colors with
``CONFIG_NR_LLC_COLORS=<n>``.

Compile Xen and the toolstack and then configure it via
`Command line parameters`_.

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

::

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
cache size by the number of its ways. The second parameter is the minimum
amount of memory that can be mapped by the hypervisor, thus dividing the way
size by the page size, the number of total cache partitions is found. So for
example, an Arm Cortex-A53 with a 16-ways associative 1 MiB LLC, can isolate up
to 16 colors when pages are 4 KiB in size.

Cache layout is probed automatically by Xen itself, but a possibility to
manually set the way size it's left for the user to overcome failing situations
or for debugging/testing purposes. See `Command line parameters`_ for more
information on that.

Command line parameters
***********************

More specific documentation is available at `docs/misc/xen-command-line.pandoc`.

+----------------------+-------------------------------+
| **Parameter**        | **Description**               |
+----------------------+-------------------------------+
| ``llc-coloring``     | enable coloring at runtime    |
+----------------------+-------------------------------+
| ``llc-way-size``     | set the LLC way size          |
+----------------------+-------------------------------+
| ``dom0-llc-colors``  | Dom0 color configuration      |
+----------------------+-------------------------------+

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

+-------------------+-----------------------------+
| **Configuration** | **Actual selection**        |
+-------------------+-----------------------------+
| 1-2,5-8           | [1, 2, 5, 6, 7, 8]          |
+-------------------+-----------------------------+
| 4-8,10,11,12      | [4, 5, 6, 7, 8, 10, 11, 12] |
+-------------------+-----------------------------+
| 0                 | [0]                         |
+-------------------+-----------------------------+

Known issues and limitations
****************************

"xen,static-mem" isn't supported when coloring is enabled
#########################################################

In the domain configuration, "xen,static-mem" allows memory to be statically
allocated to the domain. This isn't possibile when LLC coloring is enabled,
because that memory can't be guaranteed to use only colors assigned to the
domain.
