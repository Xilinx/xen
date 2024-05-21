/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef X86_DOMAIN_BUILDER_H
#define X86_DOMAIN_BUILDER_H

#include <xen/types.h>

struct boot_info;
struct boot_domain;
struct domain;

/*
 * For classic dom0 boots, module 0 is assumed to be a kernel and subsystems
 * initialised later must probe unknown modules themselves. When DTB-booting,
 * the function identifies the kinds of every boot module discovered on the DTB.
 *
 * This identification is important to know which modules are kernels that may
 * need decompressing during relocation.
 */
void builder_init(struct boot_info *bi);
void builder_late_init(struct boot_info *bi);

int pvh_setup_cpus(struct domain *d, paddr_t entry, paddr_t start_info);

void alloc_dom_vcpus(struct domain *d);

int dom_construct_pvh(struct boot_domain *bd);

#endif /* X86_DOMAIN_BUILDER_H */
