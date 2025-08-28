/*
 * Virtio MSG - Message packing/unpacking functions.
 *
 * Copyright (c) 2024 Advanced Micro Devices, Inc.
 * Written by Edgar E. Iglesias <edgar.iglesias@amd.com>.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef VIRTIO_MSG_H
#define VIRTIO_MSG_H

#include <xen/lib.h>
#include <xen/ctype.h>
#include <xen/stringify.h>
#include <xen/bug.h>
#include <xen/virtio/std/virtio_config.h>

enum {
    VIRTIO_MSG_DEVICE_INFO       = 0x02,
    VIRTIO_MSG_GET_FEATURES      = 0x03,
    VIRTIO_MSG_SET_FEATURES      = 0x04,
    VIRTIO_MSG_GET_CONFIG        = 0x05,
    VIRTIO_MSG_SET_CONFIG        = 0x06,
    VIRTIO_MSG_GET_DEVICE_STATUS = 0x07,
    VIRTIO_MSG_SET_DEVICE_STATUS = 0x08,
    VIRTIO_MSG_GET_VQUEUE        = 0x09,
    VIRTIO_MSG_SET_VQUEUE        = 0x0a,
    VIRTIO_MSG_RESET_VQUEUE      = 0x0b,
    VIRTIO_MSG_GET_SHM           = 0x0c, /* Not yet supported */
    VIRTIO_MSG_EVENT_CONFIG      = 0x40,
    VIRTIO_MSG_EVENT_AVAIL       = 0x41,
    VIRTIO_MSG_EVENT_USED        = 0x42,

    VIRTIO_MSG_MAX = VIRTIO_MSG_EVENT_USED,
};

#define VIRTIO_MSG_MAX_SIZE 44

#define VIRTIO_MSG_TYPE_RESPONSE  (1 << 0)
#define VIRTIO_MSG_TYPE_BUS       (1 << 1)

typedef struct VirtIOMSG {
    uint8_t type;
    uint8_t msg_id;
    uint16_t dev_num;
    uint16_t msg_size;

    union {
        uint8_t payload_u8[38];

        struct {
            uint32_t device_id;
            uint32_t vendor_id;
            uint32_t num_feature_bits;
            uint32_t config_size;
            uint32_t max_vqs;
            uint16_t admin_vq_idx;
            uint16_t admin_vq_count;
        } __packed get_device_info_resp;
        struct {
            uint32_t index;
            uint32_t num;
        } __packed get_features;
        struct {
            uint32_t index;
            uint32_t num;
            uint32_t b32[];
        } __packed get_features_resp;
        struct {
            uint32_t index;
            uint32_t num;
            uint32_t b32[];
        } __packed set_features;
        struct {
            uint32_t offset;
            uint32_t size;
        } __packed get_config;
        struct {
            uint32_t generation;
            uint32_t offset;
            uint32_t size;
            uint8_t data[];
        } __packed get_config_resp;
        struct {
            uint32_t generation;
            uint32_t offset;
            uint32_t size;
            uint8_t data[];
        } __packed set_config;
        struct {
            uint32_t generation;
            uint32_t offset;
            uint32_t size;
            uint8_t data[];
        } __packed set_config_resp;
        struct {
            uint32_t status;
        } __packed get_device_status_resp;
        struct {
            uint32_t status;
        } __packed set_device_status;
        struct {
            uint32_t status;
        } __packed set_device_status_resp;
        struct {
            uint32_t index;
        } __packed get_vqueue;
        struct {
            uint32_t index;
            uint32_t max_size;
            uint32_t size;
            uint64_t descriptor_addr;
            uint64_t driver_addr;
            uint64_t device_addr;
        } __packed get_vqueue_resp;
        struct {
            uint32_t index;
            uint32_t unused;
            uint32_t size;
            uint64_t descriptor_addr;
            uint64_t driver_addr;
            uint64_t device_addr;
        } __packed set_vqueue;
        struct {
            uint32_t index;
        } __packed reset_vqueue;
        struct {
            uint32_t status;
            uint32_t generation;
            uint32_t offset;
            uint32_t size;
            uint8_t config_value[];
        } __packed event_config;
        struct {
            uint32_t index;
            uint32_t next_offset_wrap;
        } __packed event_avail;
        struct {
            uint32_t index;
        } __packed event_used;
    };
} __packed VirtIOMSG;

/* Maximum number of 32b feature-blocks in a single message.  */
#define VIRTIO_MSG_MAX_FEATURE_NUM \
    ((VIRTIO_MSG_MAX_SIZE - offsetof(VirtIOMSG, set_features.b32)) / 4)

/* Maximum amount of config-data in a single message, in bytes.  */
#define VIRTIO_MSG_MAX_CONFIG_BYTES \
    (VIRTIO_MSG_MAX_SIZE - offsetof(VirtIOMSG, set_config.data))

static inline void virtio_msg_unpack(VirtIOMSG *msg)
{
    /* Xen is little endian only, so there's nothing to do here.  */
}

static inline size_t virtio_msg_header_size(void)
{
    return offsetof(VirtIOMSG, payload_u8);
}

static inline void virtio_msg_pack_header(VirtIOMSG *msg,
                                          uint8_t msg_id,
                                          uint8_t type,
                                          uint16_t dev_num,
                                          uint16_t payload_size)
{
    uint16_t msg_size = virtio_msg_header_size() + payload_size;

    msg->type = type;
    msg->msg_id = msg_id;
    msg->dev_num = dev_num;
    msg->msg_size = msg_size;

    /* Keep things predictable.  */
    memset(msg->payload_u8, 0, sizeof msg->payload_u8);
}

static inline void virtio_msg_pack_get_device_info(VirtIOMSG *msg)
{
    virtio_msg_pack_header(msg, VIRTIO_MSG_DEVICE_INFO, 0, 0, 0);
}

static inline void virtio_msg_pack_get_features(VirtIOMSG *msg,
                                                uint32_t index,
                                                uint32_t num)
{
    virtio_msg_pack_header(msg, VIRTIO_MSG_GET_FEATURES, 0, 0,
                           sizeof msg->get_features);

    msg->get_features.index = index;
    msg->get_features.num = num;
}

static inline void virtio_msg_pack_set_features(VirtIOMSG *msg,
                                                uint32_t index,
                                                uint32_t num,
                                                uint32_t *f)
{
    unsigned int i;

    BUG_ON(num > VIRTIO_MSG_MAX_FEATURE_NUM);

    virtio_msg_pack_header(msg, VIRTIO_MSG_SET_FEATURES, 0, 0,
                           sizeof msg->set_features);

    msg->set_features.index = index;
    msg->set_features.num = num;

    for ( i = 0; i < num && i < VIRTIO_MSG_MAX_FEATURE_NUM; i++ )
        msg->set_features.b32[i] = f[i];
}

static inline void virtio_msg_pack_set_device_status(VirtIOMSG *msg,
                                                     uint32_t status)
{
    virtio_msg_pack_header(msg, VIRTIO_MSG_SET_DEVICE_STATUS, 0, 0,
                           sizeof msg->set_device_status);

    msg->set_device_status.status = status;
}

static inline void virtio_msg_pack_get_device_status(VirtIOMSG *msg)
{
    virtio_msg_pack_header(msg, VIRTIO_MSG_GET_DEVICE_STATUS, 0, 0, 0);
}

static inline void virtio_msg_pack_get_config(VirtIOMSG *msg,
                                              uint32_t size,
                                              uint32_t offset)
{
    virtio_msg_pack_header(msg, VIRTIO_MSG_GET_CONFIG, 0, 0,
                           sizeof msg->get_config);

    msg->get_config.offset = offset;
    msg->get_config.size = size;
}

static inline void virtio_msg_pack_set_config(VirtIOMSG *msg,
                                              uint32_t size,
                                              uint32_t offset,
                                              uint32_t generation,
                                              void *data)
{
    BUG_ON(size > VIRTIO_MSG_MAX_CONFIG_BYTES);

    virtio_msg_pack_header(msg, VIRTIO_MSG_SET_CONFIG, 0, 0,
                           sizeof msg->set_config);

    msg->set_config.offset = offset;
    msg->set_config.size = size;
    msg->set_config.generation = generation;

    memcpy(&msg->set_config.data, data, size);
}

static inline void virtio_msg_pack_get_vqueue(VirtIOMSG *msg, uint32_t index)
{
    virtio_msg_pack_header(msg, VIRTIO_MSG_GET_VQUEUE, 0, 0,
                           sizeof msg->get_vqueue);

    msg->get_vqueue.index = index;
}

static inline void virtio_msg_pack_reset_vqueue(VirtIOMSG *msg, uint32_t index)
{
    virtio_msg_pack_header(msg, VIRTIO_MSG_RESET_VQUEUE, 0, 0,
                           sizeof msg->reset_vqueue);

    msg->reset_vqueue.index = index;
}

static inline void virtio_msg_pack_set_vqueue(VirtIOMSG *msg,
                                              uint32_t index,
                                              uint32_t size,
                                              uint64_t descriptor_addr,
                                              uint64_t driver_addr,
                                              uint64_t device_addr)
{
    virtio_msg_pack_header(msg, VIRTIO_MSG_SET_VQUEUE, 0, 0,
                           sizeof msg->set_vqueue);

    msg->set_vqueue.index = index;
    msg->set_vqueue.unused = 0;
    msg->set_vqueue.size = size;
    msg->set_vqueue.descriptor_addr = descriptor_addr;
    msg->set_vqueue.driver_addr = driver_addr;
    msg->set_vqueue.device_addr = device_addr;
}

static inline void virtio_msg_pack_event_avail(VirtIOMSG *msg,
                                               uint32_t index,
                                               uint32_t next_offset,
                                               bool next_wrap)
{
    uint32_t next_ow;

    virtio_msg_pack_header(msg, VIRTIO_MSG_EVENT_AVAIL, 0, 0,
                           sizeof msg->event_avail);

    /* next_offset is 31b wide.  */
    ASSERT((next_offset & 0x80000000U) == 0);

    /* Pack the next_offset_wrap field. */
    next_ow = next_wrap ? 0x80000000U : 0;
    next_ow |= next_offset;

    msg->event_avail.index = index;
    msg->event_avail.next_offset_wrap = next_ow;
}

static inline const char *virtio_msg_id_to_str(unsigned int type)
{
#define VIRTIO_MSG_TYPE2STR(x) [ VIRTIO_MSG_ ## x ] = __stringify(x)
    static const char *type2str[VIRTIO_MSG_MAX + 1] = {
        VIRTIO_MSG_TYPE2STR(DEVICE_INFO),
        VIRTIO_MSG_TYPE2STR(GET_FEATURES),
        VIRTIO_MSG_TYPE2STR(SET_FEATURES),
        VIRTIO_MSG_TYPE2STR(GET_CONFIG),
        VIRTIO_MSG_TYPE2STR(SET_CONFIG),
        VIRTIO_MSG_TYPE2STR(GET_DEVICE_STATUS),
        VIRTIO_MSG_TYPE2STR(SET_DEVICE_STATUS),
        VIRTIO_MSG_TYPE2STR(GET_VQUEUE),
        VIRTIO_MSG_TYPE2STR(SET_VQUEUE),
        VIRTIO_MSG_TYPE2STR(RESET_VQUEUE),
        VIRTIO_MSG_TYPE2STR(EVENT_CONFIG),
        VIRTIO_MSG_TYPE2STR(EVENT_AVAIL),
        VIRTIO_MSG_TYPE2STR(EVENT_USED),
    };
    const char *s = NULL;

    if ( type < ARRAY_SIZE(type2str) )
        s = type2str[type];

    return s;
}

static inline void virtio_msg_print_status(uint32_t status)
{
    printk("status %x", status);

    if ( status & VIRTIO_CONFIG_S_ACKNOWLEDGE )
        printk(" ACKNOWLEDGE");
    if ( status & VIRTIO_CONFIG_S_DRIVER )
        printk(" DRIVER");
    if ( status & VIRTIO_CONFIG_S_DRIVER_OK )
        printk(" DRIVER_OK");
    if ( status & VIRTIO_CONFIG_S_FEATURES_OK )
        printk(" FEATURES_OK");
    if ( status & VIRTIO_CONFIG_S_NEEDS_RESET )
        printk(" NEEDS_RESET");
    if ( status & VIRTIO_CONFIG_S_FAILED )
        printk(" FAILED");

    printk("\n");
}

static inline void virtio_msg_print(VirtIOMSG *msg)
{
    bool resp = msg->type & VIRTIO_MSG_TYPE_RESPONSE;
    size_t payload_size;
    unsigned int i;

    BUG_ON(!msg);
    printk("virtio-msg: id %s 0x%x type 0x%x dev_num 0x%x msg_size 0x%x\n",
           virtio_msg_id_to_str(msg->msg_id), msg->msg_id, msg->type,
           msg->dev_num, msg->msg_size);

    payload_size = msg->msg_size - offsetof(VirtIOMSG, payload_u8);
    if ( payload_size > ARRAY_SIZE(msg->payload_u8) )
    {
        printk("Size overflow! %zu > %zu\n", payload_size,
               ARRAY_SIZE(msg->payload_u8));
        payload_size = ARRAY_SIZE(msg->payload_u8);
    }

    for ( i = 0; i < payload_size; i++ )
    {
        printk("%2.2x ", msg->payload_u8[i]);
        if ( ((i + 1) % 16) == 0 )
            printk("\n");
    }

    switch ( msg->msg_id )
    {
    case VIRTIO_MSG_GET_DEVICE_STATUS:
        if ( resp )
            virtio_msg_print_status(msg->get_device_status_resp.status);
        break;
    case VIRTIO_MSG_SET_DEVICE_STATUS:
        virtio_msg_print_status(msg->set_device_status.status);
        break;
    case VIRTIO_MSG_SET_VQUEUE:
        printk("set-vqueue: index=%d size=%d desc-addr=%lx driver-addr=%lx "
               "device-addr=%lx\n",
               msg->set_vqueue.index, msg->set_vqueue.size,
               msg->set_vqueue.descriptor_addr, msg->set_vqueue.driver_addr,
               msg->set_vqueue.device_addr);
        break;
    }
    printk("\n");
}
#endif /* VIRTIO_MSG_H */
