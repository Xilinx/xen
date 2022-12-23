#ifndef __X86_IOCAP_H__
#define __X86_IOCAP_H__

#define cache_flush_permitted(d)                        \
    (!rangeset_is_empty((d)->iomem_caps))

#endif

/*
 * Local variables:
 * mode: C
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * End:
 */
