// SPDX-License-Identifier: GPL-2.0+
/*
 * Loopback bus implementation for Virtio message transport.
 *
 * Copyright (C) 2025 Google LLC and Linaro.
 * Viresh Kumar <viresh.kumar@linaro.org>
 *
 * This implements the Loopback bus for Virtio msg transport.
 */

#define pr_fmt(fmt) "virtio-msg-loopback: " fmt

#include <linux/err.h>
#include <linux/list.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/types.h>
#include <linux/virtio.h>

#include <uapi/linux/virtio_msg.h>
#include <uapi/linux/virtio_msg_lb.h>

#include "virtio_msg.h"

struct vmlb_device {
	struct virtio_msg_device vmdev;
	struct list_head list;
};

struct virtio_msg_lb {
	struct mutex mutex;
	struct list_head list;
	struct miscdevice misc;
	struct virtio_msg_user_device vmudev;
	struct virtio_msg *response;
};

static struct virtio_msg_lb *vmlb;

#define to_vmlbdev(_vmdev)	((struct vmlb_device *) vmdev->priv)

static struct vmlb_device *find_vmlbdev(u16 dev_id)
{
	struct vmlb_device *vmlbdev;

	list_for_each_entry(vmlbdev, &vmlb->list, list) {
		if (vmlbdev->vmdev.dev_id == dev_id)
			return vmlbdev;
	}

	return NULL;
}

static const char *virtio_msg_lb_bus_info(struct virtio_msg_device *vmdev,
					  u16 *msg_size, u32 *rev)
{
	*msg_size = VIRTIO_MSG_MIN_SIZE;
	*rev = VIRTIO_MSG_REVISION_1;

	return dev_name(vmlb->misc.this_device);
}

static int virtio_msg_lb_transfer(struct virtio_msg_device *vmdev,
				  struct virtio_msg *request,
				  struct virtio_msg *response)
{
	struct virtio_msg_user_device *vmudev = &vmlb->vmudev;
	int ret;

	/*
	 * Allow only one transaction to progress at once.
	 */
	guard(mutex)(&vmlb->mutex);

	/*
	 * Set `msg` to `request` and finish the completion to wake up the
	 * `read()` thread.
	 */
	vmudev->msg = request;
	vmlb->response = response;
	virtio_msg_async_complete(&vmudev->r_async);

	/*
	 * Wait here for the `write()` thread to finish and not return before
	 * the operation is finished to avoid any potential races.
	 */
	ret = virtio_msg_async_wait(&vmudev->w_async, vmlb->misc.this_device, 1000);

	/* Clear the pointers, just to be safe */
	vmudev->msg = NULL;
	vmlb->response = NULL;

	return ret;
}

static struct virtio_msg_ops virtio_msg_lb_ops = {
	.transfer = virtio_msg_lb_transfer,
	.bus_info = virtio_msg_lb_bus_info,
};

static int virtio_msg_lb_user_send(struct virtio_msg_user_device *vmudev,
				   struct virtio_msg *msg)
{
	struct device *dev = vmlb->misc.this_device;
	struct vmlb_device *vmlbdev;
	struct event_used *payload;
	struct virtqueue *vq;
	u32 index = 0;

	if (msg->type & VIRTIO_MSG_TYPE_RESPONSE) {
		if (vmlb->response)
			memcpy(vmlb->response, msg, VIRTIO_MSG_MIN_SIZE);

		return 0;
	}

	/* Only support EVENT_USED virtio request messages */
	if (msg->type & VIRTIO_MSG_TYPE_BUS || msg->msg_id != VIRTIO_MSG_EVENT_USED) {
		dev_err(dev, "Unsupported message received\n");
		return 0;
	}

	vmlbdev = find_vmlbdev(le16_to_cpu(msg->dev_id));
	if (!vmlbdev)
		return 0;

	payload = virtio_msg_payload(msg);

	/*
	 * Received EVENT_USED request, but the index field can't be really used
	 * as the backend doesn't fill it. Receive the message for each
	 * virtqueue until one accepts it.
	 */
	virtio_device_for_each_vq(&vmlbdev->vmdev.vdev, vq) {
		payload->index = cpu_to_le32(index++);
		if (!virtio_msg_event(&vmlbdev->vmdev, msg))
			return 0;
	}

	/* Interrupt should belong to one of the virtqueues at least */
	dev_err(dev, "Failed to find virtqueue for EVENT_USED message\n");
	return 0;
}

static struct virtio_msg_user_ops vmlb_user_ops = {
	.send = virtio_msg_lb_user_send,
};

static int vmlbdev_add(struct file *file, struct vmsg_lb_dev_info *info)
{
	struct device *dev = vmlb->misc.this_device;
	struct vmlb_device *vmlbdev;
	int ret;

	scoped_guard(mutex, &vmlb->mutex) {
		if (find_vmlbdev(info->dev_id))
			return -EEXIST;

		vmlbdev = kzalloc(sizeof(*vmlbdev), GFP_KERNEL);
		if (!vmlbdev)
			return -ENOMEM;

		vmlbdev->vmdev.dev_id = info->dev_id;
		vmlbdev->vmdev.ops = &virtio_msg_lb_ops ;
		vmlbdev->vmdev.vdev.dev.parent = dev;
		vmlbdev->vmdev.priv = vmlbdev;

		list_add(&vmlbdev->list, &vmlb->list);
	}

	ret = virtio_msg_register(&vmlbdev->vmdev);
	if (ret) {
		dev_err(dev, "Failed to register virtio msg lb device (%d)\n", ret);
		goto out;
	}

	return 0;

out:
	scoped_guard(mutex, &vmlb->mutex)
		list_del(&vmlbdev->list);

	kfree(vmlbdev);
	return ret;
}

static int vmlbdev_remove(struct file *file, struct vmsg_lb_dev_info *info)
{
	struct device *dev = vmlb->misc.this_device;
	struct vmlb_device *vmlbdev;

	scoped_guard(mutex, &vmlb->mutex) {
		vmlbdev = find_vmlbdev(info->dev_id);
		if (vmlbdev) {
			list_del(&vmlbdev->list);
			virtio_msg_unregister(&vmlbdev->vmdev);
			return 0;
		}
	}

	dev_err(dev, "Failed to find virtio msg lb device.\n");
	return -ENODEV;
}

static void vmlbdev_remove_all(void)
{
	struct vmlb_device *vmlbdev, *tvmlbdev;

	guard(mutex)(&vmlb->mutex);

	list_for_each_entry_safe(vmlbdev, tvmlbdev, &vmlb->list, list) {
		list_del(&vmlbdev->list);
		virtio_msg_unregister(&vmlbdev->vmdev);
	}
}

static long vmlb_ioctl(struct file *file, unsigned int cmd, unsigned long data)
{
	struct vmsg_lb_dev_info info;

	if (copy_from_user(&info, (void __user *) data, sizeof(info)))
		return -EFAULT;

	switch (cmd) {
	case IOCTL_VMSG_LB_ADD:
		return vmlbdev_add(file, &info);

	case IOCTL_VMSG_LB_REMOVE:
		return vmlbdev_remove(file, &info);

	default:
		return -ENOTTY;
	}
}

static int vmlb_mmap(struct file *file, struct vm_area_struct *vma)
{
	if (remap_pfn_range(vma, vma->vm_start, 0, vma->vm_end - vma->vm_start,
			    vma->vm_page_prot)) {
		return -EAGAIN;
	}

	return 0;
}

static loff_t vmlb_llseek(struct file *file, loff_t offset, int whence)
{
	return fixed_size_llseek(file, offset, whence, 0x1000000000);
}

static const struct file_operations vmlb_miscdev_fops = {
	.owner = THIS_MODULE,
	.unlocked_ioctl = vmlb_ioctl,
	.mmap = vmlb_mmap,
	.llseek = vmlb_llseek,
};

static int virtio_msg_lb_init(void)
{
	struct device *dev;
	int ret;

	vmlb = kzalloc(sizeof(*vmlb), GFP_KERNEL);
	if (!vmlb)
		return -ENOMEM;

	INIT_LIST_HEAD(&vmlb->list);
	mutex_init(&vmlb->mutex);

	vmlb->misc.name = "virtio-msg-lb";
	vmlb->misc.minor = MISC_DYNAMIC_MINOR;
	vmlb->misc.fops = &vmlb_miscdev_fops;

	ret = misc_register(&vmlb->misc);
	if (ret)
		goto free;

	dev = vmlb->misc.this_device;

	/* Setup dma_mask to allow DMA operations by virtio core */
	dev->dma_mask = &dev->coherent_dma_mask;
	ret = dma_set_mask_and_coherent(dev, DMA_BIT_MASK(64));
	if (ret)
		ret = dma_set_mask_and_coherent(dev, DMA_BIT_MASK(32));
	if (ret)
		dev_warn(dev, "Failed to enable 64-bit or 32-bit DMA\n");

	vmlb->vmudev.ops = &vmlb_user_ops;
	vmlb->vmudev.parent = dev;

	ret = virtio_msg_user_register(&vmlb->vmudev);
	if (ret) {
		dev_err(dev, "Could not register virtio-msg user device\n");
		goto unregister;
	}

	return 0;

unregister:
	misc_deregister(&vmlb->misc);
free:
	kfree(vmlb);
	return ret;
}
module_init(virtio_msg_lb_init);

static void virtio_msg_lb_exit(void)
{
	vmlbdev_remove_all();
	virtio_msg_user_unregister(&vmlb->vmudev);
	misc_deregister(&vmlb->misc);
	kfree(vmlb);
}
module_exit(virtio_msg_lb_exit);

MODULE_AUTHOR("Viresh Kumar <viresh.kumar@linaro.org>");
MODULE_DESCRIPTION("Virtio message Loopback bus driver");
MODULE_LICENSE("GPL");
