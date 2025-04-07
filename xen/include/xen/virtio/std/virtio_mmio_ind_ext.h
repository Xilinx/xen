/*
 * virtio-mmio indirect access extension.
 *
 * Copyright (c) 2024 Advanced Micro Devices, Inc.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#ifndef __VIRTIO_MMIO_IND_EXT_H__
#define __VIRTIO_MMIO_IND_EXT_H__

/*
 * To allow domU to do non-blocking virtio-mmio, 2 new registers are introduced.
 * The ACCESS and the DATA register. These registers allow domUs to indirectly
 * access any existing virtio-mmio register.
 *
 * Writing to the ACCESS register triggers an indirect register access.
 * If ACCESS.RW is set, a write is issued and data is expected to already
 * be in the DATA register.
 *
 * If ACCESS.RW is not set, a read is issued.
 *
 * An access is complete when reading ACCESS.BUSY=0. If ACCESS.ERROR=1,
 * something failed and the access should be retried.
 * Accesses may fail because the system is out of resources and needs
 * to apply back pressure to slow things down. DomU can, but is not
 * required to for example schedule some other tasks or simply wait for
 * a while before retrying.
 *
 * For reads, once the access is ready without errors, the read data
 * can be found in the DATA register.
 *
 * Only a single in-flight access is allowed at any given time.
 *
 * It is possible to access all registers with indirect accesses but a
 * subset of them can also be accessed directly. This subset does not
 * require processing by the virtio device and can be handled in Xen
 * in a fast and deterministic manner. The registers that can be read
 * directly are:
 *
 * - VIRTIO_MMIO_MAGIC_VALUE
 * - VIRTIO_MMIO_VERSION
 * - VIRTIO_MMIO_VENDOR_ID
 * - VIRTIO_MMIO_INTERRUPT_STATUS
 *
 * The ones that can be written directly are:
 *
 * - VIRTIO_MMIO_INTERRUPT_ACK
 */


/*
 * Writing to the VIRTIO_MMIO_ACCESS register will trigger an indirect MMIO
 * access to the virtio-mmio registers.
 *
 * Write layout:
 * [31]    - rw:     READ=0, WRITE=1
 * [29:28] - size:   8-bit=0, 16-bit=1, 32-bit=2
 * [16:0]  - offset: Virtio-mmio offset.
 *
 * When reading from this register, you can poll for status. No new transaction
 * will be triggered.
 *
 * Read layout:
 * [31]    - rw:     READ=0, WRITE=1
 * [30]    - status: IDLE=0, BUSY=1
 * [29:28] - size:   8-bit=0, 16-bit=1, 32-bit=2
 * [27]    - error:  RETRY=1
 *                   If the device signals error retry, it means some of its
 *                   internal resources are temporarily fully used and it needs
 *                   to push back (back pressure). The driver should retry
 *                   the access.
 *
 * [16:0]  - offset: Virtio-mmio offset.
  */
#define VIRTIO_MMIO_ACCESS 0x18
#define VIRTIO_MMIO_ACCESS_WRITE       (1UL << 31)
#define VIRTIO_MMIO_ACCESS_BUSY        (1UL << 30)
#define VIRTIO_MMIO_ACCESS_SIZE_SHIFT  28
#define VIRTIO_MMIO_ACCESS_ERROR_RETRY (1UL << 27)

/*
 * Transaction data.
 *
 * For reads, only valid after ACCESS.BUSY = 0.
 * For writes, must be written to before writing to ACCESS to initiate
 * the write.
 */
#define VIRTIO_MMIO_ACCESS_DATA 0x1c

/*
 * Since the in-Xen implementation only implements the non-blocking version
 * of virtio-mmio and not the standard one, it is important for guests that
 * do not know about non-blocking virtio-mmio to avoid using the proxy.
 * Therefore a new MAGIC ID has been used to allow for existing device-tree
 * and ACPI bindings to be used while providing a run-time way to distinguish
 * the device.
 */
#define VIRT_MAGIC_NON_BLOCKING 0x626e6d76 /* 'vmnb' */
#endif /*__VIRTIO_MMIO_IND_EXT_H__ */
