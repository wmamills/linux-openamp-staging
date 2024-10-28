// SPDX-License-Identifier: GPL-2.0
/*
 * Virtio-msg-amp common code
 *
 * Copyright (c) Linaro Ltd, 2024
 *
 */

#include <linux/module.h>
#include <linux/init.h>
#include <linux/delay.h>

#include "virtio_msg_amp.h"

#define to_virtio_msg_amp_device(_vmdev) \
	container_of(_vmdev, struct virtio_msg_amp_device, this_dev)

#define MK_RESP(type, msg_id) (((u16) type) << 8 | (u16) (msg_id))

static void tx_msg(struct virtio_msg_amp *amp_dev, void* msg_buf, size_t size);

/* wait for completion with timeout */
static bool wait_for_it(struct completion* p_it, u32 msec)
{
	long consumed;

	consumed = wait_for_completion_timeout(p_it, msecs_to_jiffies(msec));
	return (consumed > 0);
}

static int virtio_msg_amp_send(struct virtio_msg_device *vmdev,
				struct virtio_msg *request,
				struct virtio_msg *response)
{
	struct virtio_msg_amp_device *vmadev = to_virtio_msg_amp_device(vmdev);
	struct virtio_msg_amp *amp_dev = vmadev->amp_dev;
	int len = sizeof(*request);
	u8 type;

	if (response) {
		/* init a bad response in case we fail or timeout */
		response->type = 0;
		response->id = 0;
		type = request->type | VIRTIO_MSG_TYPE_RESPONSE;
		vmadev->expected_response = MK_RESP(type, request->id);
		vmadev->response = response;
		reinit_completion(&vmadev->response_done);
	}

	tx_msg(amp_dev, request, len);

	if (response) {
		struct device *pdev = amp_dev->ops->get_device(amp_dev);
		if (!wait_for_it(&vmadev->response_done, 5000)) {
			dev_err(pdev,
			  "Timeout waiting for responce dev_id=%x, type/id=%x\n",
			  vmadev->dev_id, vmadev->expected_response);
			return 2;
		} else {
			dev_info(pdev,
			  "send_response complete dev_id=%x, type/id=%x\n",
			  vmadev->dev_id, vmadev->expected_response);
			return 2;

		}
	}

	return 0;
}

static const char *virtio_msg_amp_bus_name(struct virtio_msg_device *vmdev)
{
	struct virtio_msg_amp_device *vmadev = to_virtio_msg_amp_device(vmdev);
	struct virtio_msg_amp *amp_dev = vmadev->amp_dev;
	struct device *pdev = amp_dev->ops->get_device(amp_dev);

	dev_info(pdev, "get bus name for dev_id=%d\n",  vmadev->dev_id);

	return dev_name(pdev);
}

static void virtio_msg_amp_synchronize_cbs(struct virtio_msg_device *vmdev)
{
	struct virtio_msg_amp_device *vmadev = to_virtio_msg_amp_device(vmdev);
	struct virtio_msg_amp *amp_dev = vmadev->amp_dev;
	struct device *pdev = amp_dev->ops->get_device(amp_dev);

	dev_info(pdev, "sync cbs for dev_id=%d\n",  vmadev->dev_id);

	/* hope for the best */
}

static void virtio_msg_amp_release(struct virtio_msg_device *vmdev)
{
	struct virtio_msg_amp_device *vmadev = to_virtio_msg_amp_device(vmdev);
	struct virtio_msg_amp *amp_dev = vmadev->amp_dev;
	struct device *pdev = amp_dev->ops->get_device(amp_dev);

	vmadev->response = NULL;
	vmadev->expected_response = 0;
	complete_all(&vmadev->response_done);
	vmadev->in_use = true;

	dev_info(pdev, "release for dev_id=%d\n", vmadev->dev_id);
}

static int virtio_msg_amp_vqs_prepare(struct virtio_msg_device *vmdev)
{
	struct virtio_msg_amp_device *vmadev = to_virtio_msg_amp_device(vmdev);
	struct virtio_msg_amp *amp_dev = vmadev->amp_dev;
	struct device *pdev = amp_dev->ops->get_device(amp_dev);

	dev_info(pdev, "prep vqs for dev_id=%d\n", vmadev->dev_id);
	return 0;
}

static void virtio_msg_amp_vqs_release(struct virtio_msg_device *vmdev)
{
	struct virtio_msg_amp_device *vmadev = to_virtio_msg_amp_device(vmdev);
	struct virtio_msg_amp *amp_dev = vmadev->amp_dev;
	struct device *pdev = amp_dev->ops->get_device(amp_dev);

	dev_info(pdev, "release vqs for dev_id=%d\n", vmadev->dev_id);
}

static struct virtio_msg_ops amp_msg_device_ops = {
	.send = virtio_msg_amp_send,
	.bus_name = virtio_msg_amp_bus_name,
	.synchronize_cbs = virtio_msg_amp_synchronize_cbs,
	.release = virtio_msg_amp_release,
	.prepare_vqs = virtio_msg_amp_vqs_prepare,
	.release_vqs = virtio_msg_amp_vqs_release,
};

static void init_vmadev(struct virtio_msg_amp_device* vmadev,
	struct virtio_msg_amp* amp_dev, u16 dev_id)
{
	struct device* parent_dev = amp_dev->ops->get_device(amp_dev);
	vmadev->this_dev.ops = &amp_msg_device_ops;
	vmadev->this_dev.data = NULL;
	vmadev->this_dev.dev_id = dev_id;
	vmadev->this_dev.vdev.dev.parent = parent_dev;

	vmadev->amp_dev = amp_dev;
	vmadev->in_use = true;
	vmadev->dev_id = dev_id;
	vmadev->expected_response = 0;
	vmadev->response = NULL;
	init_completion(&vmadev->response_done);
}

/* this one is temporary as the v0 layout is not self describing */
int virtio_msg_amp_register_v0(struct virtio_msg_amp *amp_dev) {
	return 0;
}

static struct virtio_msg_amp_device *amp_find_dev(
	struct virtio_msg_amp 	*amp_dev,
	u16			dev_id)
{
	if (amp_dev->one_dev.dev_id == dev_id)
		return &amp_dev->one_dev;

	return NULL;
}

static bool vmadev_check_rx_match(
	struct virtio_msg_amp_device	*vmadev,
	struct virtio_msg 		*msg)
{
	u16 match;

	match = MK_RESP(msg->type, msg->id);
	if (vmadev->expected_response == match ) {
		memcpy(vmadev->response, msg, sizeof(*msg));
		vmadev->expected_response = 0;
		complete(&vmadev->response_done);
		return true;
	}
	return false;
}

static void rx_proc_all(struct virtio_msg_amp *amp_dev) {
	char buf[64];
	struct device *pdev = amp_dev->ops->get_device(amp_dev);
	struct virtio_msg_amp_device *vmadev;
	struct virtio_msg *msg;
	bool expected = false;
	u16 dev_id;

	while (spsc_recv(&amp_dev->dev2drv, buf, 64)) {
		dev_info(pdev, "RX MSG: %16ph \n", buf);
		msg = (struct virtio_msg*) buf;
		dev_id =  le16_to_cpu(msg->dev_id);
		if ((vmadev = amp_find_dev(amp_dev, dev_id))) {
			if (vmadev_check_rx_match(vmadev, msg)) {
				expected = true;
			}
		}
		if (!expected) {
			dev_err(pdev,
				"Unexpected msg dev_id=%d, type/id=%x/%x\n",
				msg->dev_id, msg->type, msg->id);
		}
	}
}

static void tx_msg(struct virtio_msg_amp *amp_dev, void* msg_buf,
	size_t msg_len) {
	struct device *pdev = amp_dev->ops->get_device(amp_dev);

	dev_info(pdev, "TX MSG: %16ph \n", msg_buf);

	/* queue a message */
	while ( ! spsc_send(&amp_dev->drv2dev, msg_buf, sizeof msg_len) ) {
		dev_info(pdev, "out of tx space, sleep");
		mdelay(10);
	}

	/* Notify the peer */
	amp_dev->ops->tx_notify(amp_dev, 0);
}

/* normal API */
int  virtio_msg_amp_register(struct virtio_msg_amp *amp_dev) {
	size_t page_size = 4096;
	char* mem = amp_dev->shmem;
	void* page0 = &mem[0 * page_size];
	void* page1 = &mem[1 * page_size];
	int err;

	/* create the first (and only) device */
	init_vmadev(&amp_dev->one_dev, amp_dev, 0);

	/* create the structures that point to the message FIFOs in memory */
	spsc_open(&amp_dev->drv2dev, "drv2dev", page0, page_size);
	spsc_open(&amp_dev->dev2drv, "dev2drv", page1, page_size);

	/* empty the rx queue */
	rx_proc_all(amp_dev);

	/* register with the virtio-msg common code */
	err = virtio_msg_register(&amp_dev->one_dev.this_dev);

	return err;
}

static void virtio_msg_amp_device_unregister(
	struct virtio_msg_amp_device *vmadev) {
	if (vmadev->in_use)
		virtio_msg_unregister(&vmadev->this_dev);
}

void virtio_msg_amp_unregister(struct virtio_msg_amp *amp_dev) {
	/* destroy all devices */
	virtio_msg_amp_device_unregister(&amp_dev->one_dev);
}

int  virtio_msg_amp_notify_rx(struct virtio_msg_amp *amp_dev, u32 notify_idx) {
	rx_proc_all(amp_dev);
	return 0;
}

static int __init virtio_msg_amp_init(void)
{
	return 0;
}
module_init(virtio_msg_amp_init);

static void __exit virtio_msg_amp_exit(void) {}
module_exit(virtio_msg_amp_exit);

EXPORT_SYMBOL_GPL(virtio_msg_amp_register_v0);
EXPORT_SYMBOL_GPL(virtio_msg_amp_register);
EXPORT_SYMBOL_GPL(virtio_msg_amp_unregister);
EXPORT_SYMBOL_GPL(virtio_msg_amp_notify_rx);

MODULE_DESCRIPTION("Virtio-msg for AMP systems");
MODULE_AUTHOR("Bill Mills <bill.mills@linaro.org>");
MODULE_LICENSE("GPL v2");
