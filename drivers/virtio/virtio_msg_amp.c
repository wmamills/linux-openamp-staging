// SPDX-License-Identifier: GPL-2.0+
/*
 * AMP bus implementation for Virtio message transport.
 *
 * Copyright (C) 2026 Linaro.
 *
 * The Virtio message AMP is a flavor of Virtio message that can be implemented
 * with a shared memory and bi-directional notification. Individual drivers map
 * the shared memory and provide the base level notification methods.
 */

#include <linux/completion.h>
#include <linux/delay.h>
#include <linux/err.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/types.h>
#include <linux/workqueue.h>

#include "virtio_msg_amp.h"

#define to_vmadev(_vmdev) \
	container_of(_vmdev, struct virtio_msg_amp_device, vmdev)
#define to_vmamp(_vmdev) ((struct virtio_msg_amp *)(_vmdev)->bus_data)

#define VMSG_PAGE_SIZE 4096
#define TIMEOUT_JIFFIES msecs_to_jiffies(100000)
#define MK_RESP(type, msg_id) (((u16)type) << 8 | (u16)(msg_id))

/* PING is disabled by default */
static short ping_ms __read_mostly;
module_param(ping_ms, short, 0444);

static struct virtio_msg_amp_device *find_vmadev(struct virtio_msg_amp *vmamp,
						 u16 dev_id)
{
	if (dev_id >= ARRAY_SIZE(vmamp->devs))
		return NULL;

	/* bus_data must be set for an initialized device */
	if (!vmamp->devs[dev_id].vmdev.bus_data)
		return NULL;

	return &vmamp->devs[dev_id];
}

static int tx_msg(struct virtio_msg_amp *vmamp, struct virtio_msg *vmsg)
{
	unsigned long flags;
	int retries = 1000;
	bool sent;

	dev_dbg(vmamp->dev, "TX MSG: %40ph\n", vmsg);

	/* queue a message */
	while (retries--) {
		spin_lock_irqsave(&vmamp->tx_lock, flags);
		sent = spsc_send(&vmamp->drv2dev, vmsg, VIRTIO_MSG_AMP_SIZE);
		spin_unlock_irqrestore(&vmamp->tx_lock, flags);

		if (sent)
			break;

		dev_dbg(vmamp->dev, "out of tx space, sleep");
		mdelay(10);
	};

	/* Notify the peer */
	if (likely(sent)) {
		vmamp->ops->tx_notify(vmamp);
		return 0;
	}

	return -EIO;
}

static bool rx_msg(struct virtio_msg_amp_response_data *rdata,
		   struct virtio_msg *vmsg)
{
	if (rdata->expected_response != MK_RESP(vmsg->type, vmsg->msg_id))
		return false;

	if (WARN_ON(!rdata->response))
		return false;

	memcpy(rdata->response, vmsg, VIRTIO_MSG_AMP_SIZE);
	complete(&rdata->completion);
	return true;
}

static int xfer_msg(struct virtio_msg_amp *vmamp,
		    struct virtio_msg_amp_response_data *rdata,
		    struct virtio_msg *request, struct virtio_msg *response)
{
	int ret;

	dev_dbg(vmamp->dev, "Transfer message, type/id=%x:%x\n", request->type,
		request->msg_id);

	if (unlikely(vmamp->broken))
		return -EIO;

	if (response) {
		rdata->response = response;
		rdata->expected_response = MK_RESP(request->type | VIRTIO_MSG_TYPE_RESPONSE,
						   request->msg_id);
	}

	ret = tx_msg(vmamp, request);

	if (!response)
		return ret;

	if (!ret) {
		ret = wait_for_completion_interruptible_timeout(&rdata->completion,
								TIMEOUT_JIFFIES);
		if (ret > 0) {
			ret = 0;
		} else {
			if (!ret)
				ret = -ETIMEDOUT;

			dev_err(vmamp->dev,
				"Failed waiting for a response: %d\n", ret);
		}
	}

	rdata->response = NULL;
	rdata->expected_response = 0;
	return ret;
}

static int virtio_msg_amp_transfer(struct virtio_msg_device *vmdev,
				   struct virtio_msg *request,
				   struct virtio_msg *response)
{
	struct virtio_msg_amp_device *vmadev = to_vmadev(vmdev);
	struct virtio_msg_amp *vmamp = to_vmamp(vmdev);

	return xfer_msg(vmamp, &vmadev->rdata, request, response);
}

static const char *virtio_msg_amp_bus_info(struct virtio_msg_device *vmdev,
					   u16 *msg_size, u32 *rev)
{
	struct virtio_msg_amp *vmamp = to_vmamp(vmdev);

	*msg_size = VIRTIO_MSG_AMP_SIZE;
	*rev = VIRTIO_MSG_REVISION_1;

	return dev_name(vmamp->dev);
}

static struct virtio_msg_ops virtio_msg_amp_ops = {
	.transfer = virtio_msg_amp_transfer,
	.bus_info = virtio_msg_amp_bus_info,
};

static void remove_vmdevs(struct virtio_msg_amp *vmamp)
{
	struct virtio_msg_device *vmdev;
	int i = ARRAY_SIZE(vmamp->devs);

	while (i--) {
		vmdev = &vmamp->devs[i].vmdev;

		if (vmdev->bus_data) {
			virtio_msg_unregister(vmdev);
			vmdev->bus_data = NULL;
		}
	}
}

static int virtio_msg_amp_add_devices(struct virtio_msg_amp *vmamp)
{
	u8 req_buf[VIRTIO_MSG_AMP_SIZE];
	u8 res_buf[VIRTIO_MSG_AMP_SIZE];
	struct virtio_msg *request = (struct virtio_msg *)&req_buf;
	struct virtio_msg *response = (struct virtio_msg *)&res_buf;
	struct bus_get_devices *req_payload = virtio_msg_payload(request);
	struct bus_get_devices_resp *res_payload = virtio_msg_payload(response);
	struct virtio_msg_device *vmdev;
	int ret, i;

	virtio_msg_prepare(request, VIRTIO_MSG_BUS_GET_DEVICES, TOKEN_FIXED,
			   sizeof(*req_payload));
	req_payload->offset = 0;
	req_payload->num = cpu_to_le16(VMA_MAX_DEVS);

	ret = xfer_msg(vmamp, &vmamp->rdata, request, response);
	if (ret < 0)
		return ret;

	if (res_payload->offset ||
	    le16_to_cpu(res_payload->num) != VMA_MAX_DEVS)
		return -EINVAL;

	for (i = 0; i < VMA_MAX_DEVS; i++) {
		/* Check if the device's bit is set in devices[] array */
		if (res_payload->devices[i / 8] & (1 << (i & 7))) {
			vmdev = &vmamp->devs[i].vmdev;

			vmdev->dev_id = i;
			vmdev->bus_data = vmamp;
			vmdev->ops = &virtio_msg_amp_ops;
			vmdev->vdev.dev.parent = vmamp->dev;
			init_completion(&vmamp->devs[i].rdata.completion);

			ret = virtio_msg_register(vmdev);
			if (ret) {
				dev_err(vmamp->dev,
					"Failed to register virtio-msg device :%d (%d)\n", i, ret);
				vmdev->bus_data = NULL;
				remove_vmdevs(vmamp);
				return ret;
			}
		}
	}

	return 0;
}

static int virtio_msg_amp_bus_ping(struct virtio_msg_amp *vmamp,
				   bool is_response, u16 token, u32 data)
{
	u8 req_buf[VIRTIO_MSG_AMP_SIZE];
	u8 res_buf[VIRTIO_MSG_AMP_SIZE];
	struct virtio_msg *request = (struct virtio_msg *)&req_buf;
	struct virtio_msg *response = (struct virtio_msg *)&res_buf;
	struct bus_ping *req_payload = virtio_msg_payload(request);
	struct bus_ping *res_payload = virtio_msg_payload(response);
	int ret;

	virtio_msg_prepare(request, VIRTIO_MSG_BUS_PING, token,
			   sizeof(*req_payload));
	req_payload->data = cpu_to_le32(data);

	if (is_response) {
		request->type |= VIRTIO_MSG_TYPE_RESPONSE;
		response = NULL;
	}

	ret = xfer_msg(vmamp, &vmamp->rdata, request, response);
	if (ret < 0)
		return ret;

	if (!is_response && res_payload->data != req_payload->data)
		return -EIO;

	return 0;
}

static void ping_work(struct work_struct *work)
{
	struct virtio_msg_amp *vmamp =
		container_of(work, struct virtio_msg_amp, ping.work);
	int ret;

	ret = virtio_msg_amp_bus_ping(vmamp, false, TOKEN_FIXED, 1);
	if (ret) {
		vmamp->broken = true;
		dev_err(vmamp->dev, "Failed to PING, mark broken: %d\n", ret);
	} else {
		schedule_delayed_work(&vmamp->ping, msecs_to_jiffies(ping_ms));
	}
}

static void virtio_msg_amp_bus_event(struct work_struct *work)
{
	struct virtio_msg_amp *vmamp =
		container_of(work, struct virtio_msg_amp, work);
	struct virtio_msg *vmsg = (void *)vmamp->bus_buf;
	int ret;

	switch (vmsg->msg_id) {
	case VIRTIO_MSG_BUS_EVENT_DEVICE:
		{
			struct bus_event_device *payload = virtio_msg_payload(vmsg);
			u16 dev_num = le16_to_cpu(payload->dev_num);
			u16 state = le16_to_cpu(payload->dev_state);
			struct virtio_msg_amp_device *vmadev =
				find_vmadev(vmamp, dev_num);

			if (vmadev && state & VIRTIO_MSG_BUS_EVENT_DEV_STATE_REMOVED) {
				virtio_msg_unregister(&vmadev->vmdev);
			} else {
				dev_err(vmamp->dev,
					"Failed to parse EVENT_DEVICE for dev_id: %d: state:%d\n",
					dev_num, state);
			}

			break;
		}
	case VIRTIO_MSG_BUS_PING:
		{
			struct bus_ping *payload = virtio_msg_payload(vmsg);

			ret = virtio_msg_amp_bus_ping(vmamp, true,
						      le16_to_cpu(vmsg->token),
						      le32_to_cpu(payload->data));
			if (ret)
				dev_err(vmamp->dev, "PING response failed: %d\n", ret);
			break;
		}

	default:
		break;
	}
}

void virtio_msg_amp_event(struct virtio_msg_amp *vmamp)
{
	struct virtio_msg_amp_device *vmadev;
	struct virtio_msg *vmsg;
	int ret = 0;
	u16 dev_id;

	if (unlikely(vmamp->broken))
		return;

	while (spsc_recv(&vmamp->dev2drv, vmamp->irq_buf, sizeof(vmamp->irq_buf))) {
		vmsg = (struct virtio_msg *)vmamp->irq_buf;
		dev_id = le16_to_cpu(vmsg->dev_id);
		dev_dbg(vmamp->dev, "RX MSG: %40ph\n", vmsg);

		if (vmsg->type & VIRTIO_MSG_TYPE_RESPONSE) {
			if (vmsg->type & VIRTIO_MSG_TYPE_BUS) {
				if (rx_msg(&vmamp->rdata, vmsg))
					continue;
			} else {
				vmadev = find_vmadev(vmamp, dev_id);
				if (vmadev && rx_msg(&vmadev->rdata, vmsg))
					continue;
			}
		} else if (vmsg->type & VIRTIO_MSG_TYPE_BUS) {
			memcpy(vmamp->bus_buf, vmsg, sizeof(vmamp->bus_buf));
			schedule_work(&vmamp->work);
			continue;
		} else {
			vmadev = find_vmadev(vmamp, dev_id);
			if (vmadev) {
				ret = virtio_msg_event(&vmadev->vmdev, vmsg);
				if (!ret || ret == -EIO)
					continue;
			}
		}

		dev_warn_ratelimited(vmamp->dev,
				     "Unexpected msg dev_id=%d, type/id=%02x/%02x, ret=%d\n",
				     vmsg->dev_id, vmsg->type, vmsg->msg_id, ret);
	}
}
EXPORT_SYMBOL_GPL(virtio_msg_amp_event);

int virtio_msg_amp_register(struct virtio_msg_amp *vmamp)
{
	int ret;

	vmamp->broken = false;
	spin_lock_init(&vmamp->tx_lock);
	init_completion(&vmamp->rdata.completion);
	INIT_WORK(&vmamp->work, virtio_msg_amp_bus_event);
	INIT_DELAYED_WORK(&vmamp->ping, ping_work);

	/* create the structures that point to the message FIFOs in memory */
	spsc_init(&vmamp->drv2dev, "drv2dev", spsc_capacity(VMSG_PAGE_SIZE),
		  vmamp->shmem);
	spsc_init(&vmamp->dev2drv, "dev2drv", spsc_capacity(VMSG_PAGE_SIZE),
		  (void __iomem *)((u8 *)vmamp->shmem + VMSG_PAGE_SIZE));

	ret = virtio_msg_amp_add_devices(vmamp);
	if (ret)
		return ret;

	if (ping_ms)
		schedule_delayed_work(&vmamp->ping, msecs_to_jiffies(ping_ms));
	return 0;
}
EXPORT_SYMBOL_GPL(virtio_msg_amp_register);

void virtio_msg_amp_unregister(struct virtio_msg_amp *vmamp)
{
	/*
	 * Caller must ensure that no calls to virtio_msg_amp_event() are in
	 * progress at this point.
	 */
	cancel_delayed_work_sync(&vmamp->ping);
	cancel_work_sync(&vmamp->work);

	remove_vmdevs(vmamp);
}
EXPORT_SYMBOL_GPL(virtio_msg_amp_unregister);

MODULE_AUTHOR("Bill Mills <bill.mills@linaro.org>");
MODULE_DESCRIPTION("Virtio-message for AMP systems");
MODULE_LICENSE("GPL");
