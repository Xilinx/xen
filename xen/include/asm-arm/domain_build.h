#ifndef __ASM_DOMAIN_BUILD_H__
#define __ASM_DOMAIN_BUILD_H__

#include <xen/sched.h>
#include <asm/kernel.h>

struct map_range_data
{
    struct domain *d;
    p2m_type_t p2mt;
};

int map_irq_to_domain(struct domain *d, unsigned int irq,
                      bool need_mapping, const char *devname);
int make_chosen_node(const struct kernel_info *kinfo);
void evtchn_allocate(struct domain *d);
int handle_device_interrupts(struct domain *d, struct dt_device_node *dev,
                             bool need_mapping);
int map_range_to_domain(const struct dt_device_node *dev, u64 addr, u64 len,
                        void *data);

#ifndef CONFIG_ACPI
static inline int prepare_acpi(struct domain *d, struct kernel_info *kinfo)
{
    /* Only booting with ACPI will hit here */
    BUG();
    return -EINVAL;
}
#else
int prepare_acpi(struct domain *d, struct kernel_info *kinfo);
#endif
#endif

/*
 * Local variables:
 * mode: C
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * End:
 */
