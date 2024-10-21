/* SPDX-License-Identifier: ((GPL-2.0 WITH Linux-syscall-note) OR BSD-3-Clause) */
/*
 * Virtio-msg AMP support header.
 *
 * Copyright (c) 2024 Advanced Micro Devices, Inc.
 * Written by Edgar E. Iglesias <edgar.iglesias@amd.com>
 *
 * Copyright (C) 2024 Google LLC and Linaro.
 * Viresh Kumar <viresh.kumar@linaro.org>
 *
 * The Virtio-msg-amp is a flavor of virtio-msg that can be implemented with 
 * a shared memory and bi-directional notification.  Indivisual drivers
 * map the shared memory and provide the base level notfification methods.
 */

#ifndef _DRIVERS_VIRTIO_VIRTIO_MSG_AMP_H
#define _DRIVERS_VIRTIO_VIRTIO_MSG_AMP_H

#include <linux/device.h>

struct virtio_msg_amp;

struct virtio_msg_amp_ops {
	int (*tx_notify)(struct virtio_msg_amp *amp_dev, u32 notify_idx);
	const char *(*bus_name)(struct virtio_msg_amp *amp_dev);
	void (*release)(struct virtio_msg_amp *amp_dev);
};

/**
 * struct virtio_msg_amp - an abstraction for a base device with 
 * shared memory and notifications
 */
struct virtio_msg_amp {
	struct device *dev;
	struct virtio_msg_amp_ops *ops;
	const void *data;

	/* info about this instance set by lower level */
	void *shmem;		/* pointer to mapped shared memory */
	size_t shmem_size;	/* size of shared memory */
	u32 num_notify_idx;	/* number of lower layer notify indexes
				 * 1 is very typical */

	/* internal state, list of devices on this bus */
	spinlock_t lock;
	struct list_head devices;
};

/* this one is temporary as the v0 layout is not self describing */
int virtio_msg_amp_register_v0(struct virtio_msg_amp *amp_dev);

/* normal API */
int  virtio_msg_amp_register(struct virtio_msg_amp *amp_dev);
void virtio_msg_amp_unregister(struct virtio_msg_amp *amp_dev);
int  virtio_msg_amp_notify_rx(struct virtio_msg_amp *amp_dev, u32 notify_idx);

#endif /* _DRIVERS_VIRTIO_VIRTIO_MSG_H */
