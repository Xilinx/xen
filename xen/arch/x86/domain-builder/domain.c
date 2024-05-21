/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (C) 2024, Apertus Solutions, LLC
 */

#include <xen/domain.h>
#include <xen/sched.h>

void __init alloc_dom_vcpus(struct domain *d)
{
    for ( unsigned int i = 1; i < d->max_vcpus; i++ )
        vcpu_create(d, i);

    domain_update_node_affinity(d);
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
