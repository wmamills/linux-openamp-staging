/* SPDX-License-Identifier: ((GPL-2.0 WITH Linux-syscall-note) OR BSD-3-Clause) */
/*
 * Virtio message FF-A (Arm Firmware Framework) transport header.
 *
 * Copyright (C) 2025 Google LLC and Linaro.
 * Viresh Kumar <viresh.kumar@linaro.org>
 */

#ifndef _LINUX_VIRTIO_MSG_FFA_H
#define _LINUX_VIRTIO_MSG_FFA_H

#include <linux/types.h>

/* Message types */
#define VIRTIO_MSG_FFA_BUS_VERSION		0x80
#define VIRTIO_MSG_FFA_BUS_AREA_SHARE		0x81
#define VIRTIO_MSG_FFA_BUS_AREA_UNSHARE		0x82
#define VIRTIO_MSG_FFA_BUS_AREA_RELEASE		0x83
#define VIRTIO_MSG_FFA_BUS_EVENT_POLL		0x84

#define VIRTIO_MSG_FFA_BUS_VERSION_1_0		0x1
#define VIRTIO_MSG_FFA_BUS_MSG_SIZE		VIRTIO_MSG_MIN_SIZE

#define VIRTIO_MSG_FFA_FEATURE_INDIRECT_MSG_SUPP	(1 << 0)
#define VIRTIO_MSG_FFA_FEATURE_DIRECT_MSG_SUPP		(1 << 1)
#define VIRTIO_MSG_FFA_FEATURE_NUM_SHM			(0xFF << 8)

#define VIRTIO_MSG_FFA_AREA_ID_OFFSET			56
#define VIRTIO_MSG_FFA_OFFSET_MASK			((ULL(1) << VIRTIO_MSG_FFA_AREA_ID_OFFSET) - 1)

/* Message payload format */

struct bus_ffa_version {
	__le32 driver_version;
} __attribute__((packed));

struct bus_ffa_version_resp {
	__le32 device_version;
	__le32 features;
	__le16 num;
} __attribute__((packed));

struct bus_area_share {
	__le16 area_id;
	__le64 mem_handle;
	__le64 tag;
	__le32 count;
	__le32 attr;
} __attribute__((packed));

struct bus_area_share_resp {
	__le16 area_id;
} __attribute__((packed));

struct bus_area_unshare {
	__le16 area_id;
} __attribute__((packed));

struct bus_area_release {
	__le16 area_id;
} __attribute__((packed));

#endif /* _LINUX_VIRTIO_MSG_FFA_H */
