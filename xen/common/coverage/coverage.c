/*
 * Generic functionality for coverage analysis.
 *
 * Copyright (C) 2017 Citrix Systems R&D
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms and conditions of the GNU General Public
 * License, version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public
 * License along with this program; If not, see <http://www.gnu.org/licenses/>.
 */

#include <xen/errno.h>
#include <xen/guest_access.h>
#include <xen/types.h>
#include <xen/coverage.h>

#ifdef CONFIG_COVERAGE_XEN
    #include <xen/init.h>
    #include <xen/keyhandler.h>
    #include <xen/lib.h>
    #include <xen/xmalloc.h>
#endif

#include <public/sysctl.h>

#include "coverage.h"

int sysctl_cov_op(struct xen_sysctl_coverage_op *op)
{
    int ret;

    switch ( op->cmd )
    {
    case XEN_SYSCTL_COVERAGE_get_size:
        op->size = cov_ops.get_size();
        ret = 0;
        break;

    case XEN_SYSCTL_COVERAGE_read:
    {
        XEN_GUEST_HANDLE_PARAM(char) buf;
        uint32_t size = op->size;

        buf = guest_handle_cast(op->buffer, char);

        ret = cov_ops.dump(buf, &size);
        op->size = size;

        break;
    }

    case XEN_SYSCTL_COVERAGE_reset:
        cov_ops.reset_counters();
        ret = 0;
        break;

    default:
        ret = -EOPNOTSUPP;
        break;
    }

    return ret;
}

#ifdef CONFIG_COVERAGE_XEN
static char code[] =
{
    'A', 'B', 'C', 'D', 'E', 'F', 'G', 'H', 'I', 'J', 'K', 'L', 'M', 'N', 'O',
    'P', 'Q', 'R', 'S', 'T', 'U', 'V', 'W', 'X', 'Y', 'Z', 'a', 'b', 'c', 'd',
    'e', 'f', 'g', 'h', 'i', 'j', 'k', 'l', 'm', 'n', 'o', 'p', 'q', 'r', 's',
    't', 'u', 'v', 'w', 'x', 'y', 'z', '0', '1', '2', '3', '4', '5', '6', '7',
    '8', '9', '+', '/',
};

/*
 * key handler for coverage data collection:
 *      B   collect coverage data and print address + size, no return
 *      C   collect coverage data and print data in base64
 */
void coverage_collect(unsigned char key)
{
    int ret;
    unsigned int block;
    uint32_t size;
    struct domain *d;
    char * chars = NULL;

    if ( cov_ops.dump_xen == NULL )
        return;

    rcu_read_lock(&domlist_read_lock);
    for_each_domain(d)
        domain_pause(d);
    rcu_read_unlock(&domlist_read_lock);

    size = cov_ops.get_size();
    if ( (chars = xmalloc_bytes(size)) == NULL )
        goto back;

    if ( (ret = cov_ops.dump_xen(chars, &size)) != 0 )
    {
        printk("unable to collect coverage\n");
        goto back;
    }

    printk("coverage: address %" PRIpaddr " bytes %x\n", (paddr_t)chars, size);

    /* for QEMU testing */
    if ( key == 'B' )
        startup_cpu_idle_loop();

    printk("XEN COVERAGE DATA BEGIN");

    /* convert blocks of 3 chars to base64 */
    block = 0;
    for (unsigned int i = 0, point = 0; i < (size / 3) * 3;
         i += 1, point = i % 3)
    {
        block |= ((unsigned int)(unsigned char)(chars[i]))
                 << (8 * (2 - point));

        if ( point == 2 )
        {
            for (unsigned int place = 0; place < 4; place += 1)
            {
                unsigned int ind = (block >> (6 * (3 - place))) & 0x3F;
                printk("%c", code[ind]);
            }
            block = 0;
        }
    }

    /* convert remainder chars to base64 and add required padding */
    block = 0;
    for (unsigned int i = (size / 3) * 3, point = 0; i < size;
         i += 1, point = i % 3)
    {
        block |= ((unsigned int)(unsigned char)(chars[i]))
                 << (8 * (2 - point));

        if ( i == size - 1 )
            /* convert place if contains any bits from chars */
            for (unsigned int place = 0; place < 4; place += 1)
            {
                unsigned int point_low = 8 * (2 - point);
                unsigned int place_high = 6 * (1 + (3 - place)) - 1;

                if ( place_high >= point_low )
                {
                    unsigned int ind = (block >> (6 * (3 - place))) & 0x3F;
                    printk("%c", code[ind]);
                }
                else
                {
                    printk("=");
                }
            }
    }

    printk("XEN COVERAGE DATA END\n");

 back:
    if ( chars != NULL )
        xfree(chars);

    rcu_read_lock(&domlist_read_lock);
    for_each_domain(d)
        domain_unpause(d);
    rcu_read_unlock(&domlist_read_lock);
}

static int __init setup_coverage_keyhandlers(void)
{
    register_keyhandler('B', coverage_collect,
                        "collect coverage data and idle", 0);
    register_keyhandler('C', coverage_collect,
                        "collect coverage data and print", 0);

    return 0;
}
presmp_initcall(setup_coverage_keyhandlers);
#endif

/*
 * Local variables:
 * mode: C
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * tab-width: 4
 * indent-tabs-mode: nil
 * End:
 */
