/* SPDX-License-Identifier: MIT */
/*
 * Xen Device Tree boot information
 *
 * Information for configuring Xen domains created at boot time.
 */

#ifndef __XEN_PUBLIC_BOOTFDT_H__
#define __XEN_PUBLIC_BOOTFDT_H__

/*
 * Domain Capabilities specified in the "capabilities" property.  Use of
 * this property allows splitting up the monolithic dom0 into separate,
 * less privileged components.  A regular domU has no capabilities
 * (which is the default if nothing is specified).  A traditional dom0
 * has all three capabilities.
 */

/* Control/Privileged domain capable of affecting other domains. */
#define DOMAIN_CAPS_CONTROL  (1U << 0)
/*
 * Hardware domain controlling physical hardware.  Typically providing
 * backends to other domains.
 */
#define DOMAIN_CAPS_HARDWARE (1U << 1)
/* Xenstore domain. */
#define DOMAIN_CAPS_XENSTORE (1U << 2)
/*
 * Device model capability allows the use of the dm_op hypercalls to provide
 * the device model emulation (run QEMU) for other domains.  This is a
 * subset of the Control capability which can be granted to the
 * Hardware domain for running QEMU.
 */
#define DOMAIN_CAPS_DEVICE_MODEL (1U << 3)
/*
 * Domain cannot be the target of hypercalls.  This provides the domain
 * freedom from interference from other domains.
 */
#define DOMAIN_CAPS_NOT_HYPERCALL_TARGET (1U << 4)

#define DOMAIN_CAPS_MASK    (DOMAIN_CAPS_CONTROL  | DOMAIN_CAPS_HARDWARE | \
                             DOMAIN_CAPS_XENSTORE | DOMAIN_CAPS_DEVICE_MODEL | \
                             DOMAIN_CAPS_NOT_HYPERCALL_TARGET)

#endif /* __XEN_PUBLIC_BOOTFDT_H__ */
