// SPDX-License-Identifier: GPL-2.0+
/*
 * FF-A based bus implementation for Virtio message transport.
 *
 * Copyright (C) 2025 Google LLC and Linaro.
 * Viresh Kumar <viresh.kumar@linaro.org>
 *
 * This implements the FF-A (Arm Firmware Framework) bus for Virtio msg
 * transport.
 */

#define pr_fmt(fmt) "virtio-msg-ffa: " fmt

#include <linux/arm_ffa.h>
#include <linux/err.h>
#include <linux/idr.h>
#include <linux/list.h>
#include <linux/module.h>
#include <linux/scatterlist.h>
#include <linux/slab.h>
#include <linux/types.h>
#include <linux/virtio.h>
#include <uapi/linux/virtio_msg_ffa.h>

#include "virtio_msg.h"

/* Represents channel bus corresponding to a partition */
struct virtio_msg_ffa_device {
	struct ffa_device *ffa_dev;
	struct virtio_msg_device *vmdevs;
	int vmdev_count;
	u16 msg_size;
	bool indirect;
	struct virtio_msg_async async;
	void *response;
};

#define to_vmfdev(_vmdev) ((struct virtio_msg_ffa_device *) vmdev->priv)

static struct virtio_msg *ffa_msg_alloc(struct virtio_msg_ffa_device *vmfdev)
{
	return kzalloc(vmfdev->msg_size, GFP_KERNEL);
}

static void ffa_msg_prepare(struct virtio_msg_ffa_device *vmfdev,
			    struct virtio_msg *vmsg, u8 msg_id, u16 size)
{
	virtio_msg_prepare(vmsg, msg_id, size);
}

static int vmsg_ffa_send_direct(struct virtio_msg_ffa_device *vmfdev,
		struct virtio_msg *request, struct virtio_msg *response)
{
	struct ffa_device *ffa_dev = vmfdev->ffa_dev;
	struct ffa_send_direct_data2 ffa_data;
	int ret;

	memcpy(&ffa_data, request, vmfdev->msg_size);

	ret = ffa_dev->ops->msg_ops->sync_send_receive2(ffa_dev, &ffa_data);
	if (ret) {
		dev_err(&ffa_dev->dev, "Unable to send direct FF-A message: %d\n", ret);
		return ret;
	}

	if (response)
		memcpy(response, &ffa_data, vmfdev->msg_size);

	return 0;
}

static int vmsg_ffa_send_indirect(struct virtio_msg_ffa_device *vmfdev,
		struct virtio_msg_async *async, struct virtio_msg *request,
		struct virtio_msg *response)
{
	struct ffa_device *ffa_dev = vmfdev->ffa_dev;
	int ret;

	/* Save buffer so it can be filled by vmsg_ffa_notifier_cb() */
	vmfdev->response = response;

	ret = ffa_dev->ops->msg_ops->indirect_send(ffa_dev, request,
						   vmfdev->msg_size);
	if (ret) {
		dev_err(&ffa_dev->dev, "Unable to send in-direct FF-A message: %d\n", ret);
		return ret;
	}

	/*
	 * Always wait for the operation to finish, otherwise we may start
	 * another operation while the previous one is still ongoing.
	 */
	ret = virtio_msg_async_wait(async, &ffa_dev->dev, 1000);
	vmfdev->response = NULL;

	return ret;
}

static int vmsg_ffa_send(struct virtio_msg_ffa_device *vmfdev,
		struct virtio_msg_async *async, struct virtio_msg *request,
		struct virtio_msg *response)
{
	int ret;

	/* Try direct messaging first, fallback to indirect */
	if (!vmfdev->indirect) {
		ret = vmsg_ffa_send_direct(vmfdev, request, response);
		if (!ret)
			return 0;

		/* Fallback to indirect messaging */
		vmfdev->indirect = true;
	}

	return vmsg_ffa_send_indirect(vmfdev, async, request, response);
}

static struct virtio_msg_device *
find_vmdev(struct virtio_msg_ffa_device *vmfdev, u16 dev_id)
{
	int i;

	/* Find the device corresponding to the message */
	for (i = 0; i < vmfdev->vmdev_count; i++) {
		if (vmfdev->vmdevs[i].dev_id == dev_id)
			return &vmfdev->vmdevs[i];
	}

	dev_err(&vmfdev->ffa_dev->dev, "Couldn't find matching vmdev: %d\n",
		dev_id);
	return NULL;
}

static void handle_async_event(struct virtio_msg_ffa_device *vmfdev,
			       struct virtio_msg *vmsg)
{
	struct ffa_device *ffa_dev = vmfdev->ffa_dev;
	struct virtio_msg_device *vmdev;

	/*
	 * We can either receive a response message (to a previously sent
	 * request), or an EVENT_USED request message.
	 */
	if (vmsg->type & VIRTIO_MSG_TYPE_RESPONSE) {
		if (vmfdev->response)
			memcpy(vmfdev->response, vmsg, vmfdev->msg_size);

		if (vmsg->type & VIRTIO_MSG_TYPE_BUS) {
			virtio_msg_async_complete(&vmfdev->async);
		} else {
			vmdev = find_vmdev(vmfdev, le16_to_cpu(vmsg->dev_id));
			if (!vmdev)
				return;

			virtio_msg_async_complete(&vmdev->async);
		}

		return;
	}

	/* Only support EVENT_USED virtio request messages */
	if (vmsg->type & VIRTIO_MSG_TYPE_BUS || vmsg->msg_id != VIRTIO_MSG_EVENT_USED) {
		dev_err(&ffa_dev->dev, "Unsupported message received\n");
		return;
	}

	vmdev = find_vmdev(vmfdev, le16_to_cpu(vmsg->dev_id));
	if (!vmdev)
		return;

	if (virtio_msg_event(vmdev, vmsg)) {
		/* Interrupt should belong to one of the virtqueues at least */
		dev_err(&ffa_dev->dev,
				"Failed to find virtqueue for EVENT_USED message\n");
	}
}

static void vmsg_ffa_notifier_cb(int notify_id, void *cb_data, void *buf)
{
	struct virtio_msg_ffa_device *vmfdev = cb_data;

	handle_async_event(vmfdev, buf);
}

static int vmsg_ffa_indirect_notify_setup(struct virtio_msg_ffa_device *vmfdev)
{
	struct ffa_device *ffa_dev = vmfdev->ffa_dev;
	int ret;

	ret = ffa_dev->ops->notifier_ops->fwk_notify_request(ffa_dev,
			&vmsg_ffa_notifier_cb, vmfdev, 0);
	if (ret)
		dev_err(&ffa_dev->dev, "Unable to request notifier: %d\n", ret);

	return ret;
}

static void vmsg_ffa_indirect_notify_cleanup(struct virtio_msg_ffa_device *vmfdev)
{
	struct ffa_device *ffa_dev = vmfdev->ffa_dev;
	int ret;

	ret = ffa_dev->ops->notifier_ops->fwk_notify_relinquish(ffa_dev, 0);
	if (ret)
		dev_err(&ffa_dev->dev, "Unable to relinquish notifier: %d\n", ret);
}

static int virtio_msg_ffa_transfer(struct virtio_msg_device *vmdev,
				   struct virtio_msg *request,
				   struct virtio_msg *response)
{
	struct virtio_msg_ffa_device *vmfdev = to_vmfdev(vmdev);

	return vmsg_ffa_send(vmfdev, &vmdev->async, request, response);
}

static int vmsg_ffa_bus_version(struct virtio_msg_ffa_device *vmfdev,
				u32 *features, u16 *devices)
{
	struct virtio_msg *request __free(kfree) = NULL;
	struct virtio_msg *response __free(kfree) = NULL;
	struct bus_ffa_version *req_payload;
	struct bus_ffa_version_resp *res_payload;
	int ret;

	request = ffa_msg_alloc(vmfdev);
	response = ffa_msg_alloc(vmfdev);
	if (!request || !response)
		return -ENOMEM;
	req_payload = virtio_msg_payload(request);
	res_payload = virtio_msg_payload(response);

	ffa_msg_prepare(vmfdev, request, VIRTIO_MSG_FFA_BUS_VERSION,
			sizeof(*req_payload));
	req_payload->driver_version = cpu_to_le32(VIRTIO_MSG_FFA_BUS_VERSION_1_0);

	ret = vmsg_ffa_send(vmfdev, &vmfdev->async, request, response);
	if (ret < 0)
		return ret;

	if (le32_to_cpu(res_payload->device_version) != VIRTIO_MSG_FFA_BUS_VERSION_1_0)
		return -EINVAL;

	if (features)
		*features = le32_to_cpu(res_payload->features);
	if (devices)
		*devices = le16_to_cpu(res_payload->num);

	return 0;
}

static const char *virtio_msg_ffa_bus_info(struct virtio_msg_device *vmdev,
					   u16 *msg_size, u32 *rev)
{
	struct virtio_msg_ffa_device *vmfdev = to_vmfdev(vmdev);

	*msg_size = vmfdev->msg_size;
	*rev = VIRTIO_MSG_REVISION_1;

	return dev_name(&vmfdev->ffa_dev->dev);
}

static struct virtio_msg_ops vmf_ops = {
	.transfer = virtio_msg_ffa_transfer,
	.bus_info = virtio_msg_ffa_bus_info,
};

static void remove_vmdevs(struct virtio_msg_ffa_device *vmfdev, int count)
{
	while (count--)
		virtio_msg_unregister(&vmfdev->vmdevs[count]);
}

static int virtio_msg_ffa_probe(struct ffa_device *ffa_dev)
{
	struct virtio_msg_ffa_device *vmfdev;
	struct device *dev = &ffa_dev->dev;
	struct virtio_msg_device *vmdev;
	u32 features;
	u16 count;
	int ret, i;

	vmfdev = devm_kzalloc(dev, sizeof(*vmfdev), GFP_KERNEL);
	if (!vmfdev)
		return -ENOMEM;

	/* Try direct message first */
	vmfdev->indirect = false;

	vmfdev->ffa_dev = ffa_dev;
	vmfdev->msg_size = VIRTIO_MSG_FFA_BUS_MSG_SIZE;
	ffa_dev_set_drvdata(ffa_dev, vmfdev);
	virtio_msg_async_init(&vmfdev->async);

	ret = dma_set_mask_and_coherent(dev, DMA_BIT_MASK(64));
	if (ret)
		ret = dma_set_mask_and_coherent(dev, DMA_BIT_MASK(32));
	if (ret)
		dev_warn(dev, "Failed to enable 64-bit or 32-bit DMA\n");

	ret = vmsg_ffa_indirect_notify_setup(vmfdev);
	if (ret)
		return ret;

	ret = vmsg_ffa_bus_version(vmfdev, &features, &count);
	if (ret)
		goto notify_cleanup;

	if (!count) {
		dev_err(dev, "Device count can't be zero\n");
		ret = -ENODEV;
		goto notify_cleanup;
	}

	/*
	 * - Direct message must be supported if it already worked.
	 * - In-direct message must be supported if it already worked
	 * - And direct message must not be supported since it didn't work.
	 */
	if ((!vmfdev->indirect &&
	     !(features & VIRTIO_MSG_FFA_FEATURE_DIRECT_MSG_SUPP)) ||
	    (vmfdev->indirect &&
	     (!(features & VIRTIO_MSG_FFA_FEATURE_INDIRECT_MSG_SUPP) ||
	      (features & VIRTIO_MSG_FFA_FEATURE_DIRECT_MSG_SUPP)))) {
		dev_err(dev, "Invalid features\n");
		ret = -EINVAL;
		goto notify_cleanup;
	}

	vmfdev->vmdevs = devm_kcalloc(dev, count, sizeof(*vmfdev->vmdevs), GFP_KERNEL);
	if (!vmfdev->vmdevs) {
		ret = -ENOMEM;
		goto notify_cleanup;
	}
	vmfdev->vmdev_count = count;

	for (i = 0; i < count; i++) {
		vmdev = &vmfdev->vmdevs[i];
		vmdev->dev_id = i;
		vmdev->ops = &vmf_ops;
		vmdev->vdev.dev.parent = dev;
		vmdev->priv = vmfdev;

		ret = virtio_msg_register(vmdev);
		if (ret) {
			dev_err(dev, "Failed to register virtio msg device (%d)\n", ret);
			goto unregister;
		}
	}

	return 0;

unregister:
	remove_vmdevs(vmfdev, i);
notify_cleanup:
	vmsg_ffa_indirect_notify_cleanup(vmfdev);
	return ret;
}

static void virtio_msg_ffa_remove(struct ffa_device *ffa_dev)
{
	struct virtio_msg_ffa_device *vmfdev = ffa_dev->dev.driver_data;

	remove_vmdevs(vmfdev, vmfdev->vmdev_count);
	vmsg_ffa_indirect_notify_cleanup(vmfdev);
}

static const struct ffa_device_id virtio_msg_ffa_device_ids[] = {
	/* c66028b5-2498-4aa1-9de7-77da6122abf0 */
	{ UUID_INIT(0xc66028b5, 0x2498, 0x4aa1,
		    0x9d, 0xe7, 0x77, 0xda, 0x61, 0x22, 0xab, 0xf0) },
#if 0
	/* bd7fd089-6795-472b-b47f-db0c5d9a719d */
	{ UUID_INIT(0xbd7fd089, 0x6795, 0x472b,
		    0xb4, 0x7f, 0xdb, 0x0c, 0x5d, 0x9a, 0x71, 0x9d) },
#endif
	{}
};

static struct ffa_driver virtio_msg_ffa_driver = {
	.name = "virtio-msg-ffa",
	.probe = virtio_msg_ffa_probe,
	.remove = virtio_msg_ffa_remove,
	.id_table = virtio_msg_ffa_device_ids,
};

static int virtio_msg_ffa_init(void)
{
	if (IS_REACHABLE(CONFIG_ARM_FFA_TRANSPORT))
		return ffa_register(&virtio_msg_ffa_driver);
	else
		return -EOPNOTSUPP;
}
module_init(virtio_msg_ffa_init);

static void virtio_msg_ffa_exit(void)
{
	if (IS_REACHABLE(CONFIG_ARM_FFA_TRANSPORT))
		ffa_unregister(&virtio_msg_ffa_driver);
}
module_exit(virtio_msg_ffa_exit);

MODULE_AUTHOR("Viresh Kumar <viresh.kumar@linaro.org>");
MODULE_DESCRIPTION("Virtio message FF-A bus driver");
MODULE_LICENSE("GPL");
