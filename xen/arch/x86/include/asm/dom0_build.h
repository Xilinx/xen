#ifndef _DOM0_BUILD_H_
#define _DOM0_BUILD_H_

#include <xen/libelf.h>
#include <xen/sched.h>

#include <asm/setup.h>

extern unsigned int dom0_memflags;

struct boot_domain;

unsigned long dom_compute_nr_pages(struct boot_domain *bd,
                                   struct elf_dom_parms *parms);
int dom0_setup_permissions(struct domain *d);

void dom0_pvh_setup_e820(struct domain *d, unsigned long nr_pages);

int dom0_construct_pv(struct boot_domain *bd);
int dom0_construct_pvh(struct boot_domain *bd);

unsigned long dom_paging_pages(const struct domain *d,
                               unsigned long nr_pages);

void dom0_update_physmap(bool compat, unsigned long pfn,
                         unsigned long mfn, unsigned long vphysmap_s);

void dom0_set_affinity(struct domain *dom0);

/* Forcibly removes the dom0_cpus and dom0_nodes command line overrides */
void dom0_disable_cmdline_cpu_node_overrides(void);

#endif	/* _DOM0_BUILD_H_ */

/*
 * Local variables:
 * mode: C
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * tab-width: 4
 * indent-tabs-mode: nil
 * End:
 */
