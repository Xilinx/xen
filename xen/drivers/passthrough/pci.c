/*
 * Copyright (C) 2008,  Netronome Systems, Inc.
 *                
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program; If not, see <http://www.gnu.org/licenses/>.
 */

#include <xen/sched.h>
#include <xen/pci.h>
#include <xen/pci_regs.h>
#include <xen/pci_ids.h>
#include <xen/list.h>
#include <xen/prefetch.h>
#include <xen/iommu.h>
#include <xen/irq.h>
#include <xen/param.h>
#include <xen/delay.h>
#include <xen/keyhandler.h>
#include <xen/event.h>
#include <xen/guest_access.h>
#include <xen/paging.h>
#include <xen/radix-tree.h>
#include <xen/softirq.h>
#include <xen/tasklet.h>
#include <xen/vpci.h>
#include <xen/msi.h>
#include <xsm/xsm.h>
#include "ats.h"

struct pci_seg {
    struct list_head alldevs_list;
    u16 nr;
    unsigned long *ro_map;
    /* bus2bridge_lock protects bus2bridge array */
    spinlock_t bus2bridge_lock;
#define MAX_BUSES 256
    struct {
        u8 map;
        u8 bus;
        u8 devfn;
    } bus2bridge[MAX_BUSES];
};

static DEFINE_RWLOCK(_pcidevs_rwlock);
static DEFINE_PER_CPU(unsigned int, pcidevs_read_cnt);
static DEFINE_PER_CPU(unsigned int, pcidevs_write_cnt);

void pcidevs_lock(void)
{
    pcidevs_write_lock();
}

void pcidevs_unlock(void)
{
    pcidevs_write_unlock();
}

bool pcidevs_locked(void)
{
    return pcidevs_write_locked();
}

void pcidevs_read_lock(void)
{
    if ( this_cpu(pcidevs_read_cnt)++ == 0 )
        read_lock(&_pcidevs_rwlock);
}

int pcidevs_read_trylock(void)
{
    int ret = 1;

    if ( this_cpu(pcidevs_read_cnt) == 0 )
        ret = read_trylock(&_pcidevs_rwlock);
    if ( ret )
        this_cpu(pcidevs_read_cnt)++;

    return ret;
}

void pcidevs_read_unlock(void)
{
    ASSERT(this_cpu(pcidevs_read_cnt));

    if ( --this_cpu(pcidevs_read_cnt) == 0 )
        read_unlock(&_pcidevs_rwlock);
}

bool pcidevs_read_locked(void)
{
    /* Write lock implies read lock */
    return this_cpu(pcidevs_read_cnt) || this_cpu(pcidevs_write_cnt);
}

void pcidevs_write_lock(void)
{
    if ( this_cpu(pcidevs_write_cnt)++ == 0 )
        write_lock(&_pcidevs_rwlock);
}

void pcidevs_write_unlock(void)
{
    ASSERT(this_cpu(pcidevs_write_cnt));

    if ( --this_cpu(pcidevs_write_cnt) == 0 )
        write_unlock(&_pcidevs_rwlock);
}

bool pcidevs_write_locked(void)
{
    return rw_is_write_locked(&_pcidevs_rwlock);
}

static struct radix_tree_root pci_segments;

static inline struct pci_seg *get_pseg(u16 seg)
{
    return radix_tree_lookup(&pci_segments, seg);
}

bool_t pci_known_segment(u16 seg)
{
    return get_pseg(seg) != NULL;
}

static struct pci_seg *alloc_pseg(u16 seg)
{
    struct pci_seg *pseg = get_pseg(seg);

    if ( pseg )
        return pseg;

    pseg = xzalloc(struct pci_seg);
    if ( !pseg )
        return NULL;

    pseg->nr = seg;
    INIT_LIST_HEAD(&pseg->alldevs_list);
    spin_lock_init(&pseg->bus2bridge_lock);

    if ( radix_tree_insert(&pci_segments, seg, pseg) )
    {
        xfree(pseg);
        pseg = NULL;
    }

    return pseg;
}

static int pci_segments_iterate(
    int (*handler)(struct pci_seg *, void *), void *arg)
{
    u16 seg = 0;
    int rc = 0;

    do {
        struct pci_seg *pseg;

        if ( !radix_tree_gang_lookup(&pci_segments, (void **)&pseg, seg, 1) )
            break;
        rc = handler(pseg, arg);
        seg = pseg->nr + 1;
    } while (!rc && seg);

    return rc;
}

void __init pci_segments_init(void)
{
    radix_tree_init(&pci_segments);
    if ( !alloc_pseg(0) )
        panic("Could not initialize PCI segment 0\n");
}

int __init pci_add_segment(u16 seg)
{
    return alloc_pseg(seg) ? 0 : -ENOMEM;
}

const unsigned long *pci_get_ro_map(u16 seg)
{
    struct pci_seg *pseg = get_pseg(seg);

    return pseg ? pseg->ro_map : NULL;
}

static struct phantom_dev {
    u16 seg;
    u8 bus, slot, stride;
} phantom_devs[8];
static unsigned int nr_phantom_devs;

static int __init cf_check parse_phantom_dev(const char *str)
{
    const char *s;
    unsigned int seg, bus, slot;
    struct phantom_dev phantom;

    if ( !*str )
        return -EINVAL;
    if ( nr_phantom_devs >= ARRAY_SIZE(phantom_devs) )
        return -E2BIG;

    s = parse_pci(str, &seg, &bus, &slot, NULL);
    if ( !s || *s != ',' )
        return -EINVAL;

    phantom.seg = seg;
    phantom.bus = bus;
    phantom.slot = slot;

    switch ( phantom.stride = simple_strtol(s + 1, &s, 0) )
    {
    case 1: case 2: case 4:
        if ( *s )
    default:
            return -EINVAL;
    }

    phantom_devs[nr_phantom_devs++] = phantom;

    return 0;
}
custom_param("pci-phantom", parse_phantom_dev);

static u16 __read_mostly command_mask;
static u16 __read_mostly bridge_ctl_mask;

static int __init cf_check parse_pci_param(const char *s)
{
    const char *ss;
    int rc = 0;

    do {
        int val;
        u16 cmd_mask = 0, brctl_mask = 0;

        ss = strchr(s, ',');
        if ( !ss )
            ss = strchr(s, '\0');

        if ( (val = parse_boolean("serr", s, ss)) >= 0 )
        {
            cmd_mask = PCI_COMMAND_SERR;
            brctl_mask = PCI_BRIDGE_CTL_SERR | PCI_BRIDGE_CTL_DTMR_SERR;
        }
        else if ( (val = parse_boolean("perr", s, ss)) >= 0 )
        {
            cmd_mask = PCI_COMMAND_PARITY;
            brctl_mask = PCI_BRIDGE_CTL_PARITY;
        }
        else
            rc = -EINVAL;

        if ( val )
        {
            command_mask &= ~cmd_mask;
            bridge_ctl_mask &= ~brctl_mask;
        }
        else
        {
            command_mask |= cmd_mask;
            bridge_ctl_mask |= brctl_mask;
        }

        s = ss + 1;
    } while ( *ss );

    return rc;
}
custom_param("pci", parse_pci_param);

static void check_pdev(const struct pci_dev *pdev)
{
#define PCI_STATUS_CHECK \
    (PCI_STATUS_PARITY | PCI_STATUS_SIG_TARGET_ABORT | \
     PCI_STATUS_REC_TARGET_ABORT | PCI_STATUS_REC_MASTER_ABORT | \
     PCI_STATUS_SIG_SYSTEM_ERROR | PCI_STATUS_DETECTED_PARITY)
    u16 val;

    if ( command_mask )
    {
        val = pci_conf_read16(pdev->sbdf, PCI_COMMAND);
        if ( val & command_mask )
            pci_conf_write16(pdev->sbdf, PCI_COMMAND, val & ~command_mask);
        val = pci_conf_read16(pdev->sbdf, PCI_STATUS);
        if ( val & PCI_STATUS_CHECK )
        {
            printk(XENLOG_INFO "%pp status %04x -> %04x\n",
                   &pdev->sbdf, val, val & ~PCI_STATUS_CHECK);
            pci_conf_write16(pdev->sbdf, PCI_STATUS, val & PCI_STATUS_CHECK);
        }
    }

    switch ( pci_conf_read8(pdev->sbdf, PCI_HEADER_TYPE) & 0x7f )
    {
    case PCI_HEADER_TYPE_BRIDGE:
        if ( !bridge_ctl_mask )
            break;
        val = pci_conf_read16(pdev->sbdf, PCI_BRIDGE_CONTROL);
        if ( val & bridge_ctl_mask )
            pci_conf_write16(pdev->sbdf, PCI_BRIDGE_CONTROL,
                             val & ~bridge_ctl_mask);
        val = pci_conf_read16(pdev->sbdf, PCI_SEC_STATUS);
        if ( val & PCI_STATUS_CHECK )
        {
            printk(XENLOG_INFO "%pp secondary status %04x -> %04x\n",
                   &pdev->sbdf, val, val & ~PCI_STATUS_CHECK);
            pci_conf_write16(pdev->sbdf, PCI_SEC_STATUS,
                             val & PCI_STATUS_CHECK);
        }
        break;

    case PCI_HEADER_TYPE_CARDBUS:
        /* TODO */
        break;
    }
#undef PCI_STATUS_CHECK
}

static void apply_quirks(struct pci_dev *pdev)
{
    uint16_t vendor = pci_conf_read16(pdev->sbdf, PCI_VENDOR_ID);
    uint16_t device = pci_conf_read16(pdev->sbdf, PCI_DEVICE_ID);
    static const struct {
        uint16_t vendor, device;
    } ignore_bars[] = {
        /*
         * Device [8086:2fc0]
         * Erratum HSE43
         * CONFIG_TDP_NOMINAL CSR Implemented at Incorrect Offset
         * http://www.intel.com/content/www/us/en/processors/xeon/xeon-e5-v3-spec-update.html 
         */
        { PCI_VENDOR_ID_INTEL, 0x2fc0 },
        /*
         * Devices [8086:6f60,6fa0,6fc0]
         * Errata BDF2 / BDX2
         * PCI BARs in the Home Agent Will Return Non-Zero Values During Enumeration
         * http://www.intel.com/content/www/us/en/processors/xeon/xeon-e5-v4-spec-update.html 
        */
        { PCI_VENDOR_ID_INTEL, 0x6f60 },
        { PCI_VENDOR_ID_INTEL, 0x6fa0 },
        { PCI_VENDOR_ID_INTEL, 0x6fc0 },
    };
    unsigned int i;

    for ( i = 0; i < ARRAY_SIZE(ignore_bars); i++)
        if ( vendor == ignore_bars[i].vendor &&
             device == ignore_bars[i].device )
            /*
             * For these errata force ignoring the BARs, which prevents vPCI
             * from trying to size the BARs or add handlers to trap accesses.
             */
            pdev->ignore_bars = true;
}

static struct pci_dev *alloc_pdev(struct pci_seg *pseg, u8 bus, u8 devfn)
{
    struct pci_dev *pdev;
    unsigned int pos;
    int rc;

    list_for_each_entry ( pdev, &pseg->alldevs_list, alldevs_list )
        if ( pdev->bus == bus && pdev->devfn == devfn )
            return pdev;

    pdev = xzalloc(struct pci_dev);
    if ( !pdev )
        return NULL;

    *(u16*) &pdev->seg = pseg->nr;
    *((u8*) &pdev->bus) = bus;
    *((u8*) &pdev->devfn) = devfn;
    pdev->domain = NULL;

    arch_pci_init_pdev(pdev);

    rc = pdev_msi_init(pdev);
    if ( rc )
    {
        xfree(pdev);
        return NULL;
    }

    list_add(&pdev->alldevs_list, &pseg->alldevs_list);

    /* update bus2bridge */
    switch ( pdev->type = pdev_type(pseg->nr, bus, devfn) )
    {
        unsigned int cap, sec_bus, sub_bus;

        case DEV_TYPE_PCIe2PCI_BRIDGE:
        case DEV_TYPE_LEGACY_PCI_BRIDGE:
            sec_bus = pci_conf_read8(pdev->sbdf, PCI_SECONDARY_BUS);
            sub_bus = pci_conf_read8(pdev->sbdf, PCI_SUBORDINATE_BUS);

            spin_lock(&pseg->bus2bridge_lock);
            for ( ; sec_bus <= sub_bus; sec_bus++ )
            {
                pseg->bus2bridge[sec_bus].map = 1;
                pseg->bus2bridge[sec_bus].bus = bus;
                pseg->bus2bridge[sec_bus].devfn = devfn;
            }
            spin_unlock(&pseg->bus2bridge_lock);
            break;

        case DEV_TYPE_PCIe_ENDPOINT:
            pos = pci_find_cap_offset(pseg->nr, bus, PCI_SLOT(devfn),
                                      PCI_FUNC(devfn), PCI_CAP_ID_EXP);
            BUG_ON(!pos);
            cap = pci_conf_read16(pdev->sbdf, pos + PCI_EXP_DEVCAP);
            if ( cap & PCI_EXP_DEVCAP_PHANTOM )
            {
                pdev->phantom_stride = 8 >> MASK_EXTR(cap,
                                                      PCI_EXP_DEVCAP_PHANTOM);
                if ( PCI_FUNC(devfn) >= pdev->phantom_stride )
                    pdev->phantom_stride = 0;
            }
            else
            {
                unsigned int i;

                for ( i = 0; i < nr_phantom_devs; ++i )
                    if ( phantom_devs[i].seg == pseg->nr &&
                         phantom_devs[i].bus == bus &&
                         phantom_devs[i].slot == PCI_SLOT(devfn) &&
                         phantom_devs[i].stride > PCI_FUNC(devfn) )
                    {
                        pci_sbdf_t sbdf = pdev->sbdf;
                        unsigned int stride = phantom_devs[i].stride;

                        while ( (sbdf.fn += stride) > PCI_FUNC(devfn) )
                        {
                            if ( pci_conf_read16(sbdf, PCI_VENDOR_ID) == 0xffff &&
                                 pci_conf_read16(sbdf, PCI_DEVICE_ID) == 0xffff )
                                continue;
                            stride <<= 1;
                            printk(XENLOG_WARNING
                                   "%pp looks to be a real device; bumping %04x:%02x:%02x stride to %u\n",
                                   &sbdf, phantom_devs[i].seg,
                                   phantom_devs[i].bus, phantom_devs[i].slot,
                                   stride);
                            sbdf = pdev->sbdf;
                        }
                        if ( PCI_FUNC(stride) )
                           pdev->phantom_stride = stride;
                        break;
                    }
            }
            break;

        case DEV_TYPE_PCI:
        case DEV_TYPE_PCIe_BRIDGE:
        case DEV_TYPE_PCI_HOST_BRIDGE:
            break;

        default:
            printk(XENLOG_WARNING "%pp: unknown type %d\n",
                   &pdev->sbdf, pdev->type);
            break;
    }

    apply_quirks(pdev);
    check_pdev(pdev);

    return pdev;
}

static void free_pdev(struct pci_seg *pseg, struct pci_dev *pdev)
{
    /* update bus2bridge */
    switch ( pdev->type )
    {
        unsigned int sec_bus, sub_bus;

        case DEV_TYPE_PCIe2PCI_BRIDGE:
        case DEV_TYPE_LEGACY_PCI_BRIDGE:
            sec_bus = pci_conf_read8(pdev->sbdf, PCI_SECONDARY_BUS);
            sub_bus = pci_conf_read8(pdev->sbdf, PCI_SUBORDINATE_BUS);

            spin_lock(&pseg->bus2bridge_lock);
            for ( ; sec_bus <= sub_bus; sec_bus++ )
                pseg->bus2bridge[sec_bus] = pseg->bus2bridge[pdev->bus];
            spin_unlock(&pseg->bus2bridge_lock);
            break;

        default:
            break;
    }

    list_del(&pdev->alldevs_list);
    pdev_msi_deinit(pdev);
    xfree(pdev);
}

static void __init _pci_hide_device(struct pci_dev *pdev)
{
    if ( pdev->domain )
        return;
    pdev->domain = dom_xen;
    list_add(&pdev->domain_list, &dom_xen->pdev_list);
}

int __init pci_hide_device(unsigned int seg, unsigned int bus,
                           unsigned int devfn)
{
    struct pci_dev *pdev;
    struct pci_seg *pseg;
    int rc = -ENOMEM;

    pcidevs_lock();
    pseg = alloc_pseg(seg);
    if ( pseg )
    {
        pdev = alloc_pdev(pseg, bus, devfn);
        if ( pdev )
        {
            _pci_hide_device(pdev);
            rc = 0;
        }
    }
    pcidevs_unlock();

    return rc;
}

int __init pci_ro_device(int seg, int bus, int devfn)
{
    struct pci_seg *pseg = alloc_pseg(seg);
    struct pci_dev *pdev;

    if ( !pseg )
        return -ENOMEM;
    pdev = alloc_pdev(pseg, bus, devfn);
    if ( !pdev )
        return -ENOMEM;

    if ( !pseg->ro_map )
    {
        size_t sz = BITS_TO_LONGS(PCI_BDF(-1, -1, -1) + 1) * sizeof(long);

        pseg->ro_map = alloc_xenheap_pages(get_order_from_bytes(sz), 0);
        if ( !pseg->ro_map )
            return -ENOMEM;
        memset(pseg->ro_map, 0, sz);
    }

    __set_bit(PCI_BDF(bus, devfn), pseg->ro_map);
    _pci_hide_device(pdev);

    return 0;
}

struct pci_dev *pci_get_real_pdev(pci_sbdf_t sbdf)
{
    struct pci_dev *pdev;
    int stride;

    for ( pdev = pci_get_pdev(NULL, sbdf), stride = 4;
          !pdev && stride; stride >>= 1 )
    {
        if ( !(sbdf.devfn & stride) )
            continue;
        sbdf.devfn &= ~stride;
        pdev = pci_get_pdev(NULL, sbdf);
        if ( pdev && stride != pdev->phantom_stride )
            pdev = NULL;
    }

    return pdev;
}

struct pci_dev *pci_get_pdev(const struct domain *d, pci_sbdf_t sbdf)
{
    struct pci_dev *pdev;

    ASSERT(d || pcidevs_read_locked());

    /*
     * The hardware domain owns the majority of the devices in the system.
     * When there are multiple segments, traversing the per-segment list is
     * likely going to be faster, whereas for a single segment the difference
     * shouldn't be that large.
     */
    if ( !d || is_hardware_domain(d) )
    {
        const struct pci_seg *pseg = get_pseg(sbdf.seg);

        if ( !pseg )
            return NULL;

        list_for_each_entry ( pdev, &pseg->alldevs_list, alldevs_list )
            if ( pdev->sbdf.bdf == sbdf.bdf &&
                 (!d || pdev->domain == d) )
                return pdev;
    }
    else
        list_for_each_entry ( pdev, &d->pdev_list, domain_list )
            if ( pdev->sbdf.bdf == sbdf.bdf )
                return pdev;

    return NULL;
}

/**
 * pci_enable_acs - enable ACS if hardware support it
 * @dev: the PCI device
 */
static void pci_enable_acs(struct pci_dev *pdev)
{
    int pos;
    u16 cap, ctrl, seg = pdev->seg;
    u8 bus = pdev->bus;

    if ( !is_iommu_enabled(pdev->domain) )
        return;

    pos = pci_find_ext_capability(seg, bus, pdev->devfn, PCI_EXT_CAP_ID_ACS);
    if (!pos)
        return;

    cap = pci_conf_read16(pdev->sbdf, pos + PCI_ACS_CAP);
    ctrl = pci_conf_read16(pdev->sbdf, pos + PCI_ACS_CTRL);

    /* Source Validation */
    ctrl |= (cap & PCI_ACS_SV);

    /* P2P Request Redirect */
    ctrl |= (cap & PCI_ACS_RR);

    /* P2P Completion Redirect */
    ctrl |= (cap & PCI_ACS_CR);

    /* Upstream Forwarding */
    ctrl |= (cap & PCI_ACS_UF);

    pci_conf_write16(pdev->sbdf, pos + PCI_ACS_CTRL, ctrl);
}

static int iommu_add_device(struct pci_dev *pdev);
static int iommu_enable_device(struct pci_dev *pdev);
static int iommu_remove_device(struct pci_dev *pdev);

unsigned int pci_size_mem_bar(pci_sbdf_t sbdf, unsigned int pos,
                              uint64_t *paddr, uint64_t *psize,
                              unsigned int flags)
{
    uint32_t hi = 0, bar = pci_conf_read32(sbdf, pos);
    uint64_t size;
    bool is64bits = !(flags & PCI_BAR_ROM) &&
        (bar & PCI_BASE_ADDRESS_MEM_TYPE_MASK) == PCI_BASE_ADDRESS_MEM_TYPE_64;
    uint32_t mask = (flags & PCI_BAR_ROM) ? (uint32_t)PCI_ROM_ADDRESS_MASK
                                          : (uint32_t)PCI_BASE_ADDRESS_MEM_MASK;

    ASSERT(!((flags & PCI_BAR_VF) && (flags & PCI_BAR_ROM)));
    ASSERT((flags & PCI_BAR_ROM) ||
           (bar & PCI_BASE_ADDRESS_SPACE) == PCI_BASE_ADDRESS_SPACE_MEMORY);
    pci_conf_write32(sbdf, pos, ~0);
    if ( is64bits )
    {
        if ( flags & PCI_BAR_LAST )
        {
            printk(XENLOG_WARNING
                   "%sdevice %pp with 64-bit %sBAR in last slot\n",
                   (flags & PCI_BAR_VF) ? "SR-IOV " : "", &sbdf,
                   (flags & PCI_BAR_VF) ? "vf " : "");
            *psize = 0;
            return 1;
        }
        hi = pci_conf_read32(sbdf, pos + 4);
        pci_conf_write32(sbdf, pos + 4, ~0);
    }
    size = pci_conf_read32(sbdf, pos) & mask;
    if ( is64bits )
    {
        size |= (uint64_t)pci_conf_read32(sbdf, pos + 4) << 32;
        pci_conf_write32(sbdf, pos + 4, hi);
    }
    else if ( size )
        size |= (uint64_t)~0 << 32;
    pci_conf_write32(sbdf, pos, bar);
    size = -size;

    if ( paddr )
        *paddr = (bar & mask) | ((uint64_t)hi << 32);
    *psize = size;

    return is64bits ? 2 : 1;
}

int pci_add_device(struct domain *d, u16 seg, u8 bus, u8 devfn,
                   const struct pci_dev_info *info, nodeid_t node)
{
    struct pci_seg *pseg;
    struct pci_dev *pdev;
    unsigned int slot = PCI_SLOT(devfn), func = PCI_FUNC(devfn);
    const char *pdev_type;
    int ret;
    bool pf_is_extfn = false;

    if ( !info )
        pdev_type = "device";
    else if ( info->is_virtfn )
    {
        pcidevs_lock();
        pdev = pci_get_pdev(NULL,
                            PCI_SBDF(seg, info->physfn.bus,
                                     info->physfn.devfn));
        if ( pdev )
            pf_is_extfn = pdev->info.is_extfn;
        pcidevs_unlock();
        if ( !pdev )
            pci_add_device(d, seg, info->physfn.bus, info->physfn.devfn,
                           NULL, node);
        pdev_type = "virtual function";
    }
    else if ( info->is_extfn )
        pdev_type = "extended function";
    else
        pdev_type = "device";

    if ( d != dom_io )
    {
        ret = xsm_resource_plug_pci(XSM_PRIV, (seg << 16) | (bus << 8) | devfn);
        if ( ret )
            return ret;
    }

    ret = -ENOMEM;

    pcidevs_lock();
    pseg = alloc_pseg(seg);
    if ( !pseg )
        goto out;
    pdev = alloc_pdev(pseg, bus, devfn);
    if ( !pdev )
        goto out;

    pdev->node = node;

    if ( info )
    {
        pdev->info = *info;
        /*
         * VF's 'is_extfn' field is used to indicate whether its PF is an
         * extended function.
         */
        if ( pdev->info.is_virtfn )
            pdev->info.is_extfn = pf_is_extfn;
    }

    if ( !pdev->info.is_virtfn && !pdev->vf_rlen[0] )
    {
        unsigned int pos = pci_find_ext_capability(seg, bus, devfn,
                                                   PCI_EXT_CAP_ID_SRIOV);
        uint16_t ctrl = pci_conf_read16(pdev->sbdf, pos + PCI_SRIOV_CTRL);

        if ( !pos )
            /* Nothing */;
        else if ( !(ctrl & (PCI_SRIOV_CTRL_VFE | PCI_SRIOV_CTRL_MSE)) )
        {
            unsigned int i;

            BUILD_BUG_ON(ARRAY_SIZE(pdev->vf_rlen) != PCI_SRIOV_NUM_BARS);
            for ( i = 0; i < PCI_SRIOV_NUM_BARS; )
            {
                unsigned int idx = pos + PCI_SRIOV_BAR + i * 4;
                uint32_t bar = pci_conf_read32(pdev->sbdf, idx);

                if ( (bar & PCI_BASE_ADDRESS_SPACE) ==
                     PCI_BASE_ADDRESS_SPACE_IO )
                {
                    printk(XENLOG_WARNING
                           "SR-IOV device %pp with vf BAR%u in IO space\n",
                           &pdev->sbdf, i);
                    continue;
                }
                ret = pci_size_mem_bar(pdev->sbdf, idx, NULL,
                                       &pdev->vf_rlen[i],
                                       PCI_BAR_VF |
                                       ((i == PCI_SRIOV_NUM_BARS - 1) ?
                                        PCI_BAR_LAST : 0));
                ASSERT(ret);
                i += ret;
            }
        }
        else
            printk(XENLOG_WARNING "SR-IOV device %pp has its virtual"
                   " functions already enabled (%04x)\n", &pdev->sbdf, ctrl);
    }

    check_pdev(pdev);

    ret = 0;
    if ( !pdev->domain )
    {
        pdev->domain = d;
        list_add(&pdev->domain_list, &pdev->domain->pdev_list);

        /*
         * For devices not discovered by Xen during boot, add vPCI handlers
         * when Dom0 first informs Xen about such devices.
         */
        ret = vpci_add_handlers(pdev);
        if ( ret )
        {
            printk(XENLOG_ERR "Setup of vPCI failed: %d\n", ret);
            list_del(&pdev->domain_list);
            pdev->domain = NULL;
            goto out;
        }
        ret = iommu_add_device(pdev);
        if ( ret )
        {
            vpci_remove_device(pdev);
            list_del(&pdev->domain_list);
            pdev->domain = NULL;
            goto out;
        }
    }
    else if ( pdev->domain == d )
        iommu_enable_device(pdev);
    else
    {
        ret = -EINVAL;
        goto out;
    }

    pci_enable_acs(pdev);

out:
    pcidevs_unlock();
    if ( !ret )
    {
        printk(XENLOG_DEBUG "PCI add %s %pp\n", pdev_type,  &pdev->sbdf);
        while ( pdev->phantom_stride )
        {
            func += pdev->phantom_stride;
            if ( PCI_SLOT(func) )
                break;
            printk(XENLOG_DEBUG "PCI phantom %pp\n",
                   &PCI_SBDF(seg, bus, slot, func));
        }
    }
    return ret;
}

int pci_remove_device(u16 seg, u8 bus, u8 devfn)
{
    struct pci_seg *pseg = get_pseg(seg);
    struct pci_dev *pdev;
    int ret;

    ret = xsm_resource_unplug_pci(XSM_PRIV, (seg << 16) | (bus << 8) | devfn);
    if ( ret )
        return ret;

    ret = -ENODEV;

    if ( !pseg )
        return -ENODEV;

    pcidevs_lock();
    list_for_each_entry ( pdev, &pseg->alldevs_list, alldevs_list )
        if ( pdev->bus == bus && pdev->devfn == devfn )
        {
            vpci_remove_device(pdev);
            pci_cleanup_msi(pdev);
            ret = iommu_remove_device(pdev);
            if ( pdev->domain )
                list_del(&pdev->domain_list);
            printk(XENLOG_DEBUG "PCI remove device %pp\n", &pdev->sbdf);
            free_pdev(pseg, pdev);
            break;
        }

    pcidevs_unlock();
    return ret;
}

/* Caller should hold the pcidevs_lock */
static int deassign_device(struct domain *d, uint16_t seg, uint8_t bus,
                           uint8_t devfn)
{
    const struct domain_iommu *hd = dom_iommu(d);
    struct pci_dev *pdev;
    struct domain *target;
    int ret = 0;

    if ( !is_iommu_enabled(d) )
        return -EINVAL;

    ASSERT(pcidevs_locked());
    pdev = pci_get_pdev(d, PCI_SBDF(seg, bus, devfn));
    if ( !pdev )
        return -ENODEV;

    /* De-assignment from dom_io should de-quarantine the device */
    if ( (pdev->quarantine || iommu_quarantine) && pdev->domain != dom_io )
    {
        ret = iommu_quarantine_dev_init(pci_to_dev(pdev));
        if ( ret )
           return ret;

        target = dom_io;
    }
    else
        target = hardware_domain;

    while ( pdev->phantom_stride )
    {
        devfn += pdev->phantom_stride;
        if ( PCI_SLOT(devfn) != PCI_SLOT(pdev->devfn) )
            break;
        ret = iommu_call(hd->platform_ops, reassign_device, d, target, devfn,
                         pci_to_dev(pdev));
        if ( ret )
            goto out;
    }

    devfn = pdev->devfn;
    ret = iommu_call(hd->platform_ops, reassign_device, d, target, devfn,
                     pci_to_dev(pdev));
    if ( ret )
        goto out;

    vpci_deassign_device(pdev);

    if ( pdev->domain == hardware_domain  )
        pdev->quarantine = false;

    pdev->fault.count = 0;

 out:
    if ( ret )
        printk(XENLOG_G_ERR "%pd: deassign (%pp) failed (%d)\n",
               d, &PCI_SBDF(seg, bus, devfn), ret);

    return ret;
}

int pci_release_devices(struct domain *d)
{
    struct pci_dev *pdev, *tmp;
    u8 bus, devfn;
    int ret;

    pcidevs_lock();
    ret = arch_pci_clean_pirqs(d);
    if ( ret )
    {
        pcidevs_unlock();
        return ret;
    }
    list_for_each_entry_safe ( pdev, tmp, &d->pdev_list, domain_list )
    {
        bus = pdev->bus;
        devfn = pdev->devfn;
        ret = deassign_device(d, pdev->seg, bus, devfn) ?: ret;
    }
    pcidevs_unlock();

    return ret;
}

#define PCI_CLASS_BRIDGE_HOST    0x0600
#define PCI_CLASS_BRIDGE_PCI     0x0604

enum pdev_type pdev_type(u16 seg, u8 bus, u8 devfn)
{
    u16 class_device, creg;
    u8 d = PCI_SLOT(devfn), f = PCI_FUNC(devfn);
    int pos = pci_find_cap_offset(seg, bus, d, f, PCI_CAP_ID_EXP);

    class_device = pci_conf_read16(PCI_SBDF(seg, bus, d, f), PCI_CLASS_DEVICE);
    switch ( class_device )
    {
    case PCI_CLASS_BRIDGE_PCI:
        if ( !pos )
            return DEV_TYPE_LEGACY_PCI_BRIDGE;
        creg = pci_conf_read16(PCI_SBDF(seg, bus, d, f), pos + PCI_EXP_FLAGS);
        switch ( (creg & PCI_EXP_FLAGS_TYPE) >> 4 )
        {
        case PCI_EXP_TYPE_PCI_BRIDGE:
            return DEV_TYPE_PCIe2PCI_BRIDGE;
        case PCI_EXP_TYPE_PCIE_BRIDGE:
            return DEV_TYPE_PCI2PCIe_BRIDGE;
        }
        return DEV_TYPE_PCIe_BRIDGE;
    case PCI_CLASS_BRIDGE_HOST:
        return DEV_TYPE_PCI_HOST_BRIDGE;

    case 0xffff:
        return DEV_TYPE_PCI_UNKNOWN;
    }

    /* NB: treat legacy pre PCI 2.0 devices (class_device == 0) as endpoints. */
    return pos ? DEV_TYPE_PCIe_ENDPOINT : DEV_TYPE_PCI;
}

/*
 * find the upstream PCIe-to-PCI/PCIX bridge or PCI legacy bridge
 * return 0: the device is integrated PCI device or PCIe
 * return 1: find PCIe-to-PCI/PCIX bridge or PCI legacy bridge
 * return -1: fail
 */
int find_upstream_bridge(u16 seg, u8 *bus, u8 *devfn, u8 *secbus)
{
    struct pci_seg *pseg = get_pseg(seg);
    int ret = 0;
    int cnt = 0;

    if ( *bus == 0 )
        return 0;

    if ( !pseg )
        return -1;

    if ( !pseg->bus2bridge[*bus].map )
        return 0;

    ret = 1;
    spin_lock(&pseg->bus2bridge_lock);
    while ( pseg->bus2bridge[*bus].map )
    {
        *secbus = *bus;
        *devfn = pseg->bus2bridge[*bus].devfn;
        *bus = pseg->bus2bridge[*bus].bus;
        if ( cnt++ >= MAX_BUSES )
        {
            ret = -1;
            goto out;
        }
    }

out:
    spin_unlock(&pseg->bus2bridge_lock);
    return ret;
}

bool_t __init pci_device_detect(u16 seg, u8 bus, u8 dev, u8 func)
{
    u32 vendor;

    vendor = pci_conf_read32(PCI_SBDF(seg, bus, dev, func), PCI_VENDOR_ID);
    /* some broken boards return 0 or ~0 if a slot is empty: */
    if ( (vendor == 0xffffffff) || (vendor == 0x00000000) ||
         (vendor == 0x0000ffff) || (vendor == 0xffff0000) )
        return 0;
    return 1;
}

void pci_check_disable_device(u16 seg, u8 bus, u8 devfn)
{
    struct pci_dev *pdev;
    s_time_t now = NOW();
    u16 cword;

    pcidevs_lock();
    pdev = pci_get_real_pdev(PCI_SBDF(seg, bus, devfn));
    if ( pdev )
    {
        if ( now < pdev->fault.time ||
             now - pdev->fault.time > MILLISECS(10) )
            pdev->fault.count >>= 1;
        pdev->fault.time = now;
        if ( ++pdev->fault.count < PT_FAULT_THRESHOLD )
            pdev = NULL;
    }
    pcidevs_unlock();

    if ( !pdev )
        return;

    /* Tell the device to stop DMAing; we can't rely on the guest to
     * control it for us. */
    cword = pci_conf_read16(pdev->sbdf, PCI_COMMAND);
    pci_conf_write16(pdev->sbdf, PCI_COMMAND, cword & ~PCI_COMMAND_MASTER);
}

/*
 * scan pci devices to add all existed PCI devices to alldevs_list,
 * and setup pci hierarchy in array bus2bridge.
 */
static int __init cf_check _scan_pci_devices(struct pci_seg *pseg, void *arg)
{
    struct pci_dev *pdev;
    int bus, dev, func;

    for ( bus = 0; bus < 256; bus++ )
    {
        for ( dev = 0; dev < 32; dev++ )
        {
            for ( func = 0; func < 8; func++ )
            {
                if ( !pci_device_detect(pseg->nr, bus, dev, func) )
                {
                    if ( !func )
                        break;
                    continue;
                }

                pdev = alloc_pdev(pseg, bus, PCI_DEVFN(dev, func));
                if ( !pdev )
                {
                    printk(XENLOG_WARNING "%pp: alloc_pdev failed\n",
                           &PCI_SBDF(pseg->nr, bus, dev, func));
                    return -ENOMEM;
                }

                if ( !func && !(pci_conf_read8(PCI_SBDF(pseg->nr, bus, dev,
                                                        func),
                                               PCI_HEADER_TYPE) & 0x80) )
                    break;
            }
        }
    }

    return 0;
}

int __init scan_pci_devices(void)
{
    int ret;

    pcidevs_lock();
    ret = pci_segments_iterate(_scan_pci_devices, NULL);
    pcidevs_unlock();

    return ret;
}

static int __init _add_discovered_pci_devices(struct pci_seg *pseg, void *arg)
{
    struct pci_dev *pdev;
    int ret = 0;

    list_for_each_entry ( pdev, &pseg->alldevs_list, alldevs_list )
    {
        ret = pci_add_device(dom_io, pdev->seg, pdev->bus, pdev->devfn, NULL,
                             NUMA_NO_NODE);
        if ( ret < 0 )
        {
            printk(XENLOG_ERR
                   "%pp: Failure adding the discovered pci device (Error %d)\n",
                   &pdev->sbdf, ret);
            break;
        }
    }

    return ret;
}

void __init add_discovered_pci_devices(void)
{
    pcidevs_lock();
    pci_segments_iterate(_add_discovered_pci_devices, NULL);
    pcidevs_unlock();
}

struct setup_hwdom {
    struct domain *d;
    int (*handler)(u8 devfn, struct pci_dev *);
};

static void __hwdom_init setup_one_hwdom_device(const struct setup_hwdom *ctxt,
                                                struct pci_dev *pdev)
{
    u8 devfn = pdev->devfn;
    int err;

    do {
        err = ctxt->handler(devfn, pdev);
        if ( err )
        {
            printk(XENLOG_ERR "setup %pp for d%d failed (%d)\n",
                   &pdev->sbdf, ctxt->d->domain_id, err);
            if ( devfn == pdev->devfn )
                return;
        }
        devfn += pdev->phantom_stride;
    } while ( devfn != pdev->devfn &&
              PCI_SLOT(devfn) == PCI_SLOT(pdev->devfn) );

    err = vpci_add_handlers(pdev);
    if ( err )
        printk(XENLOG_ERR "setup of vPCI for d%d failed: %d\n",
               ctxt->d->domain_id, err);
}

static int __hwdom_init cf_check _setup_hwdom_pci_devices(
    struct pci_seg *pseg, void *arg)
{
    struct setup_hwdom *ctxt = arg;
    int bus, devfn;

    for ( bus = 0; bus < 256; bus++ )
    {
        for ( devfn = 0; devfn < 256; devfn++ )
        {
            struct pci_dev *pdev = pci_get_pdev(NULL,
                                                PCI_SBDF(pseg->nr, bus, devfn));

            if ( !pdev )
                continue;

            if ( !pdev->domain )
            {
                pdev->domain = ctxt->d;
                list_add(&pdev->domain_list, &ctxt->d->pdev_list);
                setup_one_hwdom_device(ctxt, pdev);
            }
            else if ( pdev->domain == dom_xen )
            {
                pdev->domain = ctxt->d;
                setup_one_hwdom_device(ctxt, pdev);
                pdev->domain = dom_xen;
            }
            else if ( pdev->domain != ctxt->d )
                printk(XENLOG_WARNING "Dom%d owning %pp?\n",
                       pdev->domain->domain_id, &pdev->sbdf);

            if ( iommu_verbose )
            {
                pcidevs_unlock();
                process_pending_softirqs();
                pcidevs_lock();
            }
        }

        if ( !iommu_verbose )
        {
            pcidevs_unlock();
            process_pending_softirqs();
            pcidevs_lock();
        }
    }

    return 0;
}

void __hwdom_init setup_hwdom_pci_devices(
    struct domain *d, int (*handler)(u8 devfn, struct pci_dev *))
{
    struct setup_hwdom ctxt = { .d = d, .handler = handler };

    pcidevs_lock();
    pci_segments_iterate(_setup_hwdom_pci_devices, &ctxt);
    pcidevs_unlock();
}

/* APEI not supported on ARM yet. */
#if defined(CONFIG_ACPI) && defined(CONFIG_X86)
#include <acpi/acpi.h>
#include <acpi/apei.h>

static int hest_match_pci(const struct acpi_hest_aer_common *p,
                          const struct pci_dev *pdev)
{
    return ACPI_HEST_SEGMENT(p->bus) == pdev->seg &&
           ACPI_HEST_BUS(p->bus)     == pdev->bus &&
           p->device                 == PCI_SLOT(pdev->devfn) &&
           p->function               == PCI_FUNC(pdev->devfn);
}

static bool_t hest_match_type(const struct acpi_hest_header *hest_hdr,
                              const struct pci_dev *pdev)
{
    unsigned int pos = pci_find_cap_offset(pdev->seg, pdev->bus,
                                           PCI_SLOT(pdev->devfn),
                                           PCI_FUNC(pdev->devfn),
                                           PCI_CAP_ID_EXP);
    u8 pcie = MASK_EXTR(pci_conf_read16(pdev->sbdf, pos + PCI_EXP_FLAGS),
                        PCI_EXP_FLAGS_TYPE);

    switch ( hest_hdr->type )
    {
    case ACPI_HEST_TYPE_AER_ROOT_PORT:
        return pcie == PCI_EXP_TYPE_ROOT_PORT;
    case ACPI_HEST_TYPE_AER_ENDPOINT:
        return pcie == PCI_EXP_TYPE_ENDPOINT;
    case ACPI_HEST_TYPE_AER_BRIDGE:
        return pci_conf_read16(pdev->sbdf, PCI_CLASS_DEVICE) ==
               PCI_CLASS_BRIDGE_PCI;
    }

    return 0;
}

struct aer_hest_parse_info {
    const struct pci_dev *pdev;
    bool_t firmware_first;
};

static bool_t hest_source_is_pcie_aer(const struct acpi_hest_header *hest_hdr)
{
    if ( hest_hdr->type == ACPI_HEST_TYPE_AER_ROOT_PORT ||
         hest_hdr->type == ACPI_HEST_TYPE_AER_ENDPOINT ||
         hest_hdr->type == ACPI_HEST_TYPE_AER_BRIDGE )
        return 1;
    return 0;
}

static int cf_check aer_hest_parse(
    const struct acpi_hest_header *hest_hdr, void *data)
{
    struct aer_hest_parse_info *info = data;
    const struct acpi_hest_aer_common *p;
    bool_t ff;

    if ( !hest_source_is_pcie_aer(hest_hdr) )
        return 0;

    p = (const struct acpi_hest_aer_common *)(hest_hdr + 1);
    ff = !!(p->flags & ACPI_HEST_FIRMWARE_FIRST);

    /*
     * If no specific device is supplied, determine whether
     * FIRMWARE_FIRST is set for *any* PCIe device.
     */
    if ( !info->pdev )
    {
        info->firmware_first |= ff;
        return 0;
    }

    /* Otherwise, check the specific device */
    if ( p->flags & ACPI_HEST_GLOBAL ?
         hest_match_type(hest_hdr, info->pdev) :
         hest_match_pci(p, info->pdev) )
    {
        info->firmware_first = ff;
        return 1;
    }

    return 0;
}

bool_t pcie_aer_get_firmware_first(const struct pci_dev *pdev)
{
    struct aer_hest_parse_info info = { .pdev = pdev };

    return pci_find_cap_offset(pdev->seg, pdev->bus, PCI_SLOT(pdev->devfn),
                               PCI_FUNC(pdev->devfn), PCI_CAP_ID_EXP) &&
           apei_hest_parse(aer_hest_parse, &info) >= 0 &&
           info.firmware_first;
}
#endif

static int cf_check _dump_pci_devices(struct pci_seg *pseg, void *arg)
{
    struct pci_dev *pdev;

    printk("==== segment %04x ====\n", pseg->nr);

    list_for_each_entry ( pdev, &pseg->alldevs_list, alldevs_list )
    {
        printk("%pp - ", &pdev->sbdf);
#ifdef CONFIG_X86
        if ( pdev->domain == dom_io )
            printk("DomIO:%x", pdev->arch.pseudo_domid);
        else
#endif
            printk("%pd", pdev->domain);
        printk(" - node %-3d", (pdev->node != NUMA_NO_NODE) ? pdev->node : -1);
        pdev_dump_msi(pdev);
        printk("\n");
    }

    return 0;
}

void cf_check dump_pci_devices(unsigned char ch)
{
    printk("==== PCI devices ====\n");
    pcidevs_lock();
    pci_segments_iterate(_dump_pci_devices, NULL);
    pcidevs_unlock();
}

static int __init cf_check setup_dump_pcidevs(void)
{
    register_keyhandler('Q', dump_pci_devices, "dump PCI devices", 1);
    return 0;
}
__initcall(setup_dump_pcidevs);

static int iommu_add_device(struct pci_dev *pdev)
{
    const struct domain_iommu *hd __maybe_unused;
    int rc;
    unsigned int devfn = pdev->devfn;

    if ( !pdev->domain )
        return -EINVAL;

    ASSERT(pcidevs_locked());

    hd = dom_iommu(pdev->domain);
    if ( !is_iommu_enabled(pdev->domain) )
        return 0;

#ifdef CONFIG_HAS_DEVICE_TREE
    rc = iommu_add_dt_pci_device(devfn, pdev);
#else
    rc = iommu_call(hd->platform_ops, add_device, devfn, pci_to_dev(pdev));
#endif
    if ( rc < 0 || !pdev->phantom_stride )
    {
        if ( rc < 0 )
            printk(XENLOG_WARNING "IOMMU: add %pp failed (%d)\n",
                   &pdev->sbdf, rc);
        return rc;
    }

    for ( ; ; )
    {
        devfn += pdev->phantom_stride;
        if ( PCI_SLOT(devfn) != PCI_SLOT(pdev->devfn) )
            return 0;
#ifdef CONFIG_HAS_DEVICE_TREE
        rc = iommu_add_dt_pci_device(devfn, pdev);
#else
        rc = iommu_call(hd->platform_ops, add_device, devfn, pci_to_dev(pdev));
#endif
        if ( rc < 0 )
            printk(XENLOG_WARNING "IOMMU: add %pp failed (%d)\n",
                   &PCI_SBDF(pdev->seg, pdev->bus, devfn), rc);
    }
}

static int iommu_enable_device(struct pci_dev *pdev)
{
    const struct domain_iommu *hd;

    if ( !pdev->domain )
        return -EINVAL;

    ASSERT(pcidevs_locked());

    hd = dom_iommu(pdev->domain);
    if ( !is_iommu_enabled(pdev->domain) ||
         !hd->platform_ops->enable_device )
        return 0;

    return iommu_call(hd->platform_ops, enable_device, pci_to_dev(pdev));
}

static int iommu_remove_device(struct pci_dev *pdev)
{
    const struct domain_iommu *hd;
    u8 devfn;

    if ( !pdev->domain )
        return -EINVAL;

    hd = dom_iommu(pdev->domain);
    if ( !is_iommu_enabled(pdev->domain) )
        return 0;

    for ( devfn = pdev->devfn ; pdev->phantom_stride; )
    {
        int rc;

        devfn += pdev->phantom_stride;
        if ( PCI_SLOT(devfn) != PCI_SLOT(pdev->devfn) )
            break;
        rc = iommu_call(hd->platform_ops, remove_device, devfn,
                        pci_to_dev(pdev));
        if ( !rc )
            continue;

        printk(XENLOG_ERR "IOMMU: remove %pp failed (%d)\n",
               &PCI_SBDF(pdev->seg, pdev->bus, devfn), rc);
        return rc;
    }

    devfn = pdev->devfn;

    return iommu_call(hd->platform_ops, remove_device, devfn, pci_to_dev(pdev));
}

static int device_assigned(u16 seg, u8 bus, u8 devfn)
{
    struct pci_dev *pdev;
    int rc = 0;

    ASSERT(pcidevs_locked());
    pdev = pci_get_pdev(NULL, PCI_SBDF(seg, bus, devfn));

    if ( !pdev )
        rc = -ENODEV;
    /*
     * If the device exists and it is not owned by either the hardware
     * domain or dom_io then it must be assigned to a guest, or be
     * hidden (owned by dom_xen).
     */
    else if ( pdev->domain != hardware_domain &&
              pdev->domain != dom_io )
        rc = -EBUSY;

    return rc;
}

/* Caller should hold the pcidevs_lock */
static int assign_device(struct domain *d, u16 seg, u8 bus, u8 devfn, u32 flag)
{
    const struct domain_iommu *hd = dom_iommu(d);
    struct pci_dev *pdev;
    int rc = 0;

    if ( !is_iommu_enabled(d) )
        return 0;

    if ( !arch_iommu_use_permitted(d) )
        return -EXDEV;

    /* device_assigned() should already have cleared the device for assignment */
    ASSERT(pcidevs_locked());
    pdev = pci_get_pdev(NULL, PCI_SBDF(seg, bus, devfn));
    ASSERT(pdev && (pdev->domain == hardware_domain ||
                    pdev->domain == dom_io));

    /* Do not allow broken devices to be assigned to guests. */
    rc = -EBADF;
    if ( pdev->broken && d != hardware_domain && d != dom_io )
        goto done;

    vpci_deassign_device(pdev);

    rc = pdev_msix_assign(d, pdev);
    if ( rc )
        goto done;

    if ( pdev->domain != dom_io )
    {
        rc = iommu_quarantine_dev_init(pci_to_dev(pdev));
        if ( rc )
            goto done;
    }

    pdev->fault.count = 0;

    if ( (rc = iommu_call(hd->platform_ops, assign_device, d, devfn,
                          pci_to_dev(pdev), flag)) )
        goto done;

    for ( ; pdev->phantom_stride; rc = 0 )
    {
        devfn += pdev->phantom_stride;
        if ( PCI_SLOT(devfn) != PCI_SLOT(pdev->devfn) )
            break;
        rc = iommu_call(hd->platform_ops, assign_device, d, devfn,
                        pci_to_dev(pdev), flag);
    }
    if ( rc )
        goto done;

    devfn = pdev->devfn;
    rc = vpci_assign_device(pdev);
    if ( rc && deassign_device(d, seg, bus, devfn) )
    {
        printk(XENLOG_ERR "%pd: %pp was left partially assigned\n",
               d, &PCI_SBDF(seg, bus, devfn));
        domain_crash(d);
    }

 done:
    if ( rc )
        printk(XENLOG_G_WARNING "%pd: assign (%pp) failed (%d)\n",
               d, &PCI_SBDF(seg, bus, devfn), rc);
    /* The device is assigned to dom_io so mark it as quarantined */
    else if ( d == dom_io )
        pdev->quarantine = true;

    return rc;
}

static int __init _assign_hwdom_pci_devices(struct pci_seg *pseg, void *arg)
{
    struct pci_dev *pdev;
    int ret = 0;

    list_for_each_entry ( pdev, &pseg->alldevs_list, alldevs_list )
    {
        enum pdev_type pci_dev_type = pdev_type(pdev->seg, pdev->bus,
                                                pdev->devfn);
        bool is_pci_endpoint = (pci_dev_type == DEV_TYPE_PCIe_ENDPOINT) ||
                               (pci_dev_type == DEV_TYPE_PCI);

        if ( is_pci_endpoint && (pdev->domain == dom_io) )
        {
            ret = assign_device(hardware_domain, pdev->seg, pdev->bus,
                                pdev->devfn, 0);
            if ( ret < 0 )
            {
                printk(XENLOG_ERR
                       "%pp: Failure assigning the discovered pci device "
                       "(Error %d)\n", &pdev->sbdf, ret);
                break;
            }
        }
    }

    return ret;
}

void __init assign_hwdom_pci_devices(void)
{
    if ( !hardware_domain || !hwdom_uses_vpci() )
        return;

    pcidevs_lock();
    pci_segments_iterate(_assign_hwdom_pci_devices, NULL);
    pcidevs_unlock();
}

int pci_assign_device(struct domain *d, u16 seg, u8 bus, u8 devfn, u32 flag)
{
    int ret;

    pcidevs_lock();
    ret = assign_device(d, seg, bus, devfn, flag);
    pcidevs_unlock();

    return ret;
}

static int iommu_get_device_group(
    struct domain *d, u16 seg, u8 bus, u8 devfn,
    XEN_GUEST_HANDLE_64(uint32) buf, int max_sdevs)
{
    const struct domain_iommu *hd = dom_iommu(d);
    struct pci_dev *pdev;
    int group_id, sdev_id;
    u32 bdf;
    int i = 0;
    const struct iommu_ops *ops = hd->platform_ops;

    if ( !is_iommu_enabled(d) || !ops->get_device_group_id )
        return 0;

    group_id = iommu_call(ops, get_device_group_id, seg, bus, devfn);
    if ( group_id < 0 )
        return group_id;

    pcidevs_lock();
    for_each_pdev( d, pdev )
    {
        unsigned int b = pdev->bus;
        unsigned int df = pdev->devfn;

        if ( (pdev->seg != seg) || ((b == bus) && (df == devfn)) )
            continue;

        if ( xsm_get_device_group(XSM_HOOK, (seg << 16) | (b << 8) | df) )
            continue;

        sdev_id = iommu_call(ops, get_device_group_id, seg, b, df);
        if ( sdev_id < 0 )
        {
            pcidevs_unlock();
            return sdev_id;
        }

        if ( (sdev_id == group_id) && (i < max_sdevs) )
        {
            bdf = (b << 16) | (df << 8);

            if ( unlikely(copy_to_guest_offset(buf, i, &bdf, 1)) )
            {
                pcidevs_unlock();
                return -EFAULT;
            }
            i++;
        }
    }

    pcidevs_unlock();

    return i;
}

void iommu_dev_iotlb_flush_timeout(struct domain *d, struct pci_dev *pdev)
{
    pcidevs_lock();

    disable_ats_device(pdev);

    ASSERT(pdev->domain);
    if ( d != pdev->domain )
    {
        pcidevs_unlock();
        return;
    }

    pdev->broken = true;

    if ( !d->is_shutting_down && printk_ratelimit() )
        printk(XENLOG_ERR "dom%d: ATS device %pp flush failed\n",
               d->domain_id, &pdev->sbdf);
    if ( !is_hardware_domain(d) )
        domain_crash(d);

    pcidevs_unlock();
}

int iommu_do_pci_domctl(
    struct xen_domctl *domctl, struct domain *d,
    XEN_GUEST_HANDLE_PARAM(xen_domctl_t) u_domctl)
{
    u16 seg;
    u8 bus, devfn;
    int ret = 0;
    uint32_t machine_sbdf;

    switch ( domctl->cmd )
    {
        unsigned int flags;

    case XEN_DOMCTL_get_device_group:
    {
        u32 max_sdevs;
        XEN_GUEST_HANDLE_64(uint32) sdevs;

        ret = xsm_get_device_group(XSM_HOOK, domctl->u.get_device_group.machine_sbdf);
        if ( ret )
            break;

        seg = domctl->u.get_device_group.machine_sbdf >> 16;
        bus = PCI_BUS(domctl->u.get_device_group.machine_sbdf);
        devfn = PCI_DEVFN(domctl->u.get_device_group.machine_sbdf);
        max_sdevs = domctl->u.get_device_group.max_sdevs;
        sdevs = domctl->u.get_device_group.sdev_array;

        ret = iommu_get_device_group(d, seg, bus, devfn, sdevs, max_sdevs);
        if ( ret < 0 )
        {
            dprintk(XENLOG_ERR, "iommu_get_device_group() failed: %d\n", ret);
            domctl->u.get_device_group.num_sdevs = 0;
        }
        else
        {
            domctl->u.get_device_group.num_sdevs = ret;
            ret = 0;
        }
        if ( __copy_field_to_guest(u_domctl, domctl, u.get_device_group) )
            ret = -EFAULT;
    }
    break;

    case XEN_DOMCTL_assign_device:
        ASSERT(d);
        /* fall through */
    case XEN_DOMCTL_test_assign_device:
        /* Don't support self-assignment of devices. */
        if ( d == current->domain )
        {
            ret = -EINVAL;
            break;
        }

        ret = -ENODEV;
        if ( domctl->u.assign_device.dev != XEN_DOMCTL_DEV_PCI )
            break;

        ret = -EINVAL;
        flags = domctl->u.assign_device.flags;
        if ( domctl->cmd == XEN_DOMCTL_assign_device
             ? d->is_dying || (flags & ~XEN_DOMCTL_DEV_RDM_RELAXED)
             : flags )
            break;

        machine_sbdf = domctl->u.assign_device.u.pci.machine_sbdf;

        ret = xsm_assign_device(XSM_HOOK, d, machine_sbdf);
        if ( ret )
            break;

        seg = machine_sbdf >> 16;
        bus = PCI_BUS(machine_sbdf);
        devfn = PCI_DEVFN(machine_sbdf);

        pcidevs_lock();
        ret = device_assigned(seg, bus, devfn);
        if ( domctl->cmd == XEN_DOMCTL_test_assign_device )
        {
            if ( ret )
            {
                printk(XENLOG_G_INFO "%pp already assigned, or non-existent\n",
                       &PCI_SBDF(seg, bus, devfn));
                ret = -EINVAL;
            }
        }
        else if ( !ret )
            ret = assign_device(d, seg, bus, devfn, flags);
        pcidevs_unlock();
        if ( ret == -ERESTART )
            ret = hypercall_create_continuation(__HYPERVISOR_domctl,
                                                "h", u_domctl);
        break;

    case XEN_DOMCTL_deassign_device:
        /* Don't support self-deassignment of devices. */
        if ( d == current->domain )
        {
            ret = -EINVAL;
            break;
        }

        ret = -ENODEV;
        if ( domctl->u.assign_device.dev != XEN_DOMCTL_DEV_PCI )
            break;

        ret = -EINVAL;
        if ( domctl->u.assign_device.flags )
            break;

        machine_sbdf = domctl->u.assign_device.u.pci.machine_sbdf;

        ret = xsm_deassign_device(XSM_HOOK, d, machine_sbdf);
        if ( ret )
            break;

        seg = machine_sbdf >> 16;
        bus = PCI_BUS(machine_sbdf);
        devfn = PCI_DEVFN(machine_sbdf);

        pcidevs_lock();
        ret = deassign_device(d, seg, bus, devfn);
        pcidevs_unlock();
        break;

    default:
        ret = -ENOSYS;
        break;
    }

    return ret;
}

/*
 * Local variables:
 * mode: C
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * End:
 */
