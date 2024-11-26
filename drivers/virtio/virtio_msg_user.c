// SPDX-License-Identifier: GPL-2.0+
/*
 * Virtio message transport user API.
 *
 * Copyright (C) 2025 Google LLC and Linaro.
 * Viresh Kumar <viresh.kumar@linaro.org>
 */

#define pr_fmt(fmt) "virtio-msg: " fmt

#include <linux/err.h>
#include <linux/fs.h>
#include <linux/miscdevice.h>
#include <linux/slab.h>
#include <linux/uaccess.h>

#include "virtio_msg.h"

#define to_virtio_msg_user_device(_misc)	container_of(_misc, struct virtio_msg_user_device, misc)

static ssize_t vmsg_miscdev_read(struct file *file, char __user *buf,
				 size_t count, loff_t *pos)
{
	struct miscdevice *misc = file->private_data;
	struct virtio_msg_user_device *vmudev = to_virtio_msg_user_device(misc);
	int ret;

	if (count < VIRTIO_MSG_MIN_SIZE) {
		dev_err(vmudev->parent, "Trying to read message of incorrect size: %zu\n",
			count);
		return 0;
	}

	/* Wait to receive a message from the guest */
	ret = virtio_msg_async_wait(&vmudev->r_async, vmudev->parent, 0);
	if (ret < 0)
		return 0;

	BUG_ON(!vmudev->msg);

	/* The "msg" pointer is filled by the bus driver before waking up */
	if (copy_to_user(buf, vmudev->msg, count) != 0)
		return 0;

	vmudev->msg = NULL;

	return count;
}

static ssize_t vmsg_miscdev_write(struct file *file, const char __user *buf,
				  size_t count, loff_t *pos)
{
	struct miscdevice *misc = file->private_data;
	struct virtio_msg_user_device *vmudev = to_virtio_msg_user_device(misc);
	struct virtio_msg *msg __free(kfree) = NULL;

	if (count < VIRTIO_MSG_MIN_SIZE) {
		dev_err(vmudev->parent, "Trying to write message of incorrect size: %zu\n",
			count);
		return 0;
	}

	msg = kzalloc(count, GFP_KERNEL);
	if (!msg)
		return 0;

	if (copy_from_user(msg, buf, count) != 0)
		return 0;

	vmudev->ops->send(vmudev, msg);

	/* Wake up the handler only for responses */
	if (msg->type & VIRTIO_MSG_TYPE_RESPONSE)
		virtio_msg_async_complete(&vmudev->w_async);

	return count;
}

static const struct file_operations vmsg_miscdev_fops = {
	.owner = THIS_MODULE,
	.read = vmsg_miscdev_read,
	.write = vmsg_miscdev_write,
};

int virtio_msg_user_register(struct virtio_msg_user_device *vmudev)
{
	static u8 vmsg_user_device_count = 0;
	int ret;

	if (!vmudev || !vmudev->ops)
		return -EINVAL;

	virtio_msg_async_init(&vmudev->r_async);
	virtio_msg_async_init(&vmudev->w_async);

	vmudev->misc.parent = vmudev->parent;
	vmudev->misc.minor = MISC_DYNAMIC_MINOR;
	vmudev->misc.fops = &vmsg_miscdev_fops;
	vmudev->misc.name = vmudev->name;
	sprintf(vmudev->name, "virtio-msg-%d", vmsg_user_device_count);

	ret = misc_register(&vmudev->misc);
	if (ret)
		return ret;

	vmsg_user_device_count++;
	return 0;
}
EXPORT_SYMBOL_GPL(virtio_msg_user_register);

void virtio_msg_user_unregister(struct virtio_msg_user_device *vmudev)
{
	misc_deregister(&vmudev->misc);
}
EXPORT_SYMBOL_GPL(virtio_msg_user_unregister);
