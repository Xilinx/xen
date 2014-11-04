#include <xen/types.h>
#include <xen/device.h>

void device_initialize(struct device *dev, enum device_type type)
{
    dev->type = type;

#ifdef HAS_DEVICE_TREE
    if ( type == DEV_DT )
        dev->of_node = dev_to_dt(dev);
#endif
}

/*
 * Local variables:
 * mode: C
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * End:
 */
