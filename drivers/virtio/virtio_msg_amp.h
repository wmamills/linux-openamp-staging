/* SPDX-License-Identifier: ((GPL-2.0 WITH Linux-syscall-note) OR BSD-3-Clause) */
/*
 * Virtio-msg AMP support header.
 *
 * Copyright (c) 2026 Advanced Micro Devices, Inc.
 * Written by Edgar E. Iglesias <edgar.iglesias@amd.com>
 *
 * The Virtio-msg-amp is a flavor of virtio-msg that can be implemented with a
 * shared memory and bi-directional notification. Individual drivers map the
 * shared memory and provide the base level notification methods.
 */

#ifndef _DRIVERS_VIRTIO_MSG_AMP_H
#define _DRIVERS_VIRTIO_MSG_AMP_H

#include <linux/completion.h>
#include <linux/spinlock.h>
#include <linux/workqueue.h>

#include "virtio_msg_internal.h"
#include "virtio_msg_amp_shmem.h"

#define VIRTIO_MSG_AMP_PAGE_SIZE 4096
#define VIRTIO_MSG_AMP_SIZE 64
#define VMA_MAX_DEVS 32

struct device;
struct virtio_msg_amp;

struct virtio_msg_amp_response_data {
	u16 expected_response;
	struct virtio_msg *response;
	struct completion completion;
};

struct virtio_msg_amp_device {
	struct virtio_msg_device vmdev;
	struct virtio_msg_amp_response_data rdata;
};

struct virtio_msg_amp_ops {
	void (*tx_notify)(struct virtio_msg_amp *vmamp);
};

/* An abstraction for a base device with shared memory and notifications */
struct virtio_msg_amp {
	/* Set by the AMP implementation */
	struct device *dev;
	struct virtio_msg_amp_ops *ops;
	struct vmamp_shmem shmem;
	spinlock_t tx_lock; /* Serialize send operation */

	struct virtio_msg_amp_device devs[VMA_MAX_DEVS];
	struct virtio_msg_amp_response_data rdata;
	struct work_struct work;
	struct work_struct init_work;
	struct delayed_work ping;
	u8 bus_buf[VIRTIO_MSG_AMP_SIZE];
	u8 irq_buf[VIRTIO_MSG_AMP_SIZE];
	bool registered;
	bool broken;
};

int virtio_msg_amp_register(struct virtio_msg_amp *vmamp);
void virtio_msg_amp_unregister(struct virtio_msg_amp *vmamp);
void virtio_msg_amp_event(struct virtio_msg_amp *vmamp);

#endif /* _DRIVERS_VIRTIO_MSG_AMP_H */
