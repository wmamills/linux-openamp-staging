/* SPDX-License-Identifier: ((GPL-2.0 WITH Linux-syscall-note) OR BSD-3-Clause) */
/*
 * Virtio message transport header.
 *
 * Copyright (C) 2025 Google LLC and Linaro.
 * Viresh Kumar <viresh.kumar@linaro.org>
 */

#ifndef _DRIVERS_VIRTIO_VIRTIO_MSG_H
#define _DRIVERS_VIRTIO_VIRTIO_MSG_H

#include <linux/list.h>
#include <linux/pm.h>
#include <linux/virtio.h>
#include <uapi/linux/virtio_msg.h>

struct virtio_msg_device;

/**
 * struct virtio_msg_ops - Virtio message bus operations.
 * @bus_info: Returns bus information.
 * @transfer: Transfers a message.
 * @synchronize_cbs: Synchronize with the virtqueue callbacks (optional).
 * @release: Release the resources corresponding to the device (optional).
 */
struct virtio_msg_ops {
	const char *(*bus_info)(struct virtio_msg_device *vmdev, u16 *msg_size, u32 *rev);
	int (*transfer)(struct virtio_msg_device *vmdev, struct virtio_msg *request,
			struct virtio_msg *response);
	void (*synchronize_cbs)(struct virtio_msg_device *vmdev);
	void (*release)(struct virtio_msg_device *vmdev);
};

/*
 * Synchronization mechanism for asynchronous transfers.
 */
struct virtio_msg_async {
	struct completion completion;
};

/*
 * Representation of a device using virtio message
 * transport.
 */
struct virtio_msg_device {
	struct virtio_device vdev;
	struct virtio_msg_ops *ops;
	const char *bus_name;
	void *priv;
	u16 dev_id;
	u16 msg_size;
	u32 num_feature_bits;
	u32 config_size;
	u32 generation_count;

	struct virtio_msg *request;
	struct virtio_msg *response;
	struct virtio_msg_async async;
};

int virtio_msg_register(struct virtio_msg_device *vmdev);
void virtio_msg_unregister(struct virtio_msg_device *vmdev);

void virtio_msg_prepare(struct virtio_msg *vmsg, u8 msg_id, u16 payload_size);
int virtio_msg_event(struct virtio_msg_device *vmdev, struct virtio_msg *msg);

static inline void virtio_msg_async_init(struct virtio_msg_async *async)
{
	init_completion(&async->completion);
}

static inline int virtio_msg_async_wait(struct virtio_msg_async *async,
		struct device *dev, unsigned long timeout)
{
	int ret;

	if (timeout)
		ret = wait_for_completion_interruptible_timeout(&async->completion, timeout);
	else
		ret = wait_for_completion_interruptible(&async->completion);

	if (ret < 0) {
		dev_err(dev, "Interrupted while waiting for response: %d\n", ret);
	} else if (timeout && !ret) {
		dev_err(dev, "Timed out waiting for response\n");
		ret = -ETIMEDOUT;
	} else {
		ret = 0;
	}

	return ret;
}

static inline void virtio_msg_async_wait_nosleep(struct virtio_msg_async *async)
{
	while (!try_wait_for_completion(&async->completion))
		cpu_relax();
}

static inline void virtio_msg_async_complete(struct virtio_msg_async *async)
{
	complete(&async->completion);
}

#ifdef CONFIG_PM_SLEEP
static inline int virtio_msg_freeze(struct virtio_msg_device *vmdev)
{
	return virtio_device_freeze(&vmdev->vdev);
}

static inline int virtio_msg_restore(struct virtio_msg_device *vmdev)
{
	return virtio_device_restore(&vmdev->vdev);
}
#endif

#endif /* _DRIVERS_VIRTIO_VIRTIO_MSG_H */
