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
#include <linux/delay.h>
#include <linux/err.h>
#include <linux/idr.h>
#include <linux/kthread.h>
#include <linux/list.h>
#include <linux/module.h>
#include <linux/of_reserved_mem.h>
#include <linux/scatterlist.h>
#include <linux/slab.h>
#include <linux/types.h>
#include <linux/virtio.h>
#include <uapi/linux/virtio_msg_ffa.h>

#include "virtio_msg.h"

/* Represents area shared with a partition */
struct shared_area {
	u64 handle;
	u32 id;
	void *vaddr;
	dma_addr_t dma_handle;
	size_t n_pages;
	u32 count;
	struct list_head list;
};

/* Represents channel bus corresponding to a partition */
struct virtio_msg_ffa_device {
	struct ffa_device *ffa_dev;
	struct virtio_msg_device *vmdevs;
	struct virtio_msg_user_device vmudev;
	int vmdev_count;
	u16 msg_size;
	bool indirect;
	bool reserved_mem;
	bool passive;
	struct virtio_msg_async async;
	struct task_struct *used_event_task;
	void *response;

	struct ida area_id_map;
	struct list_head area_list;
	struct mutex lock; /* protects area_list */
};

#define to_vmfdev(_vmdev) ((struct virtio_msg_ffa_device *) vmdev->priv)
#define vmudev_to_vmfdev(_vmudev)	container_of(_vmudev, struct virtio_msg_ffa_device, vmudev)

static inline dma_addr_t ffa_to_dma(u32 area_id, dma_addr_t offset)
{
	return ((u64) area_id << VIRTIO_MSG_FFA_AREA_ID_OFFSET) |
		(offset & VIRTIO_MSG_FFA_OFFSET_MASK);
}

static inline u32 dma_to_ffa(dma_addr_t dma_handle, dma_addr_t *offset)
{
	*offset = dma_handle & VIRTIO_MSG_FFA_OFFSET_MASK;

	return dma_handle >> VIRTIO_MSG_FFA_AREA_ID_OFFSET;
}

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
	if (async)
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

static int used_event_task(void *data)
{
	struct virtio_msg_ffa_device *vmfdev = data;
	struct virtio_msg_device *vmdev;
	struct event_used *payload;
	struct virtio_msg *vmsg __free(kfree);
	struct virtqueue *vq;
	u32 index;
	int i;

	vmsg = ffa_msg_alloc(vmfdev);
	if (!vmsg)
		return -ENOMEM;
	payload = virtio_msg_payload(vmsg);

	virtio_msg_prepare(vmsg, VIRTIO_MSG_EVENT_USED, sizeof(*payload));

	while (!kthread_should_stop()) {
		for (i = 0; i < vmfdev->vmdev_count; i++) {
			vmdev = &vmfdev->vmdevs[i];
			index = 0;

			virtio_device_for_each_vq(&vmdev->vdev, vq) {
				payload->index = cpu_to_le32(index++);
				virtio_msg_event(vmdev, vmsg);
			}
		}

		/* sleep for 1ms */
		fsleep(1000);
	}

	return 0;
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

	if (vmfdev->passive) {
		/*
		 * Set `msg` to `buf` and finish the completion to wake up the
		 * `read()` thread.
		 */
		vmfdev->vmudev.msg = buf;
		virtio_msg_async_complete(&vmfdev->vmudev.r_async);

		/*
		 * Wait here for the `write()` thread to finish and not return
		 * before the operation is finished to avoid any potential
		 * races.
		 *
		 * Can't make a sleep-able call here.
		 */
		virtio_msg_async_wait_nosleep(&vmfdev->vmudev.w_async);
	} else {
		handle_async_event(vmfdev, buf);
	}
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

static int vmsg_ffa_bus_area_share_single(struct ffa_device *ffa_dev, void *vaddr,
		size_t n_pages, dma_addr_t *dma_handle)
{
	struct virtio_msg_ffa_device *vmfdev = ffa_dev->dev.driver_data;
	struct ffa_mem_region_attributes mem_attr = {
		.receiver = ffa_dev->vm_id,
		.attrs = FFA_MEM_RW,
	};
	struct ffa_mem_ops_args args = {
		.use_txbuf = true,
		.attrs = &mem_attr,
		.nattrs = 1,
	};
	struct page **pages __free(kfree) = NULL;
	struct virtio_msg *vmsg __free(kfree);
	struct bus_area_share *payload;
	struct shared_area *area;
	struct sg_table sgt;
	int ret, i;

	vmsg = ffa_msg_alloc(vmfdev);
	if (!vmsg)
		return -ENOMEM;
	payload = virtio_msg_payload(vmsg);

	pages = kmalloc(sizeof(*pages) * n_pages, GFP_KERNEL);
	if (!pages)
		return -ENOMEM;

	for (i = 0; i < n_pages; i++)
		pages[i] = virt_to_page((void *)((u64)vaddr + PAGE_SIZE * i));

	area = kzalloc(sizeof(*area), GFP_KERNEL);
	if (!area)
		return -ENOMEM;

	ret = ida_alloc_range(&vmfdev->area_id_map, 1, U32_MAX - 1, GFP_KERNEL);
	if (ret < 0)
		goto free_area;
	area->id = ret;

	/* Share the pages */
	ret = sg_alloc_table_from_pages(&sgt, pages, n_pages, 0,
					n_pages * PAGE_SIZE, GFP_KERNEL);
	if (ret)
		goto free_ida;

	args.sg = sgt.sgl;
	ret = ffa_dev->ops->mem_ops->memory_share(&args);
	sg_free_table(&sgt);

	if (ret)
		goto free_ida;

	area->handle = args.g_handle;
	area->vaddr = vaddr;
	area->dma_handle = virt_to_phys(vaddr);
	area->n_pages = n_pages;
	area->count = 1;

	ffa_msg_prepare(vmfdev, vmsg, VIRTIO_MSG_FFA_BUS_AREA_SHARE, sizeof(*payload));
	payload->area_id = cpu_to_le16(area->id);
	payload->mem_handle = cpu_to_le64(area->handle);
	payload->count = cpu_to_le32(n_pages);

	ret = vmsg_ffa_send(vmfdev, &vmfdev->async, vmsg, NULL);
	if (ret < 0)
		goto mem_reclaim;

	if (dma_handle)
		*dma_handle = ffa_to_dma(area->id, 0);

	mutex_lock(&vmfdev->lock);
	list_add(&area->list, &vmfdev->area_list);
	mutex_unlock(&vmfdev->lock);

	return 0;

mem_reclaim:
	ffa_dev->ops->mem_ops->memory_reclaim(area->handle, 0);
free_ida:
	ida_free(&vmfdev->area_id_map, area->id);
free_area:
	kfree(area);

	return ret;
}

/* vaddr is always page aligned */
int vmsg_ffa_bus_area_share(struct device *dev, void *vaddr, size_t n_pages,
			    dma_addr_t *dma_handle)
{
	struct ffa_device *ffa_dev = to_ffa_dev(dev);
	struct virtio_msg_ffa_device *vmfdev = ffa_dev->dev.driver_data;
	struct shared_area *area;
	int ret = 0;

	mutex_lock(&vmfdev->lock);
	/*
	 * If "restricted-dma-pool" is supported, we should have already mapped
	 * a big enough area at initialization time. Make sure that "dma_handle"
	 * lies within that and update dma_handle properly.
	 */
	if (vmfdev->reserved_mem) {
		BUG_ON(!list_is_singular(&vmfdev->area_list));

		area = list_first_entry(&vmfdev->area_list, struct shared_area,
					list);

		if ((*dma_handle < area->dma_handle) ||
		    ((*dma_handle + n_pages * PAGE_SIZE) >
		     (area->dma_handle + area->n_pages * PAGE_SIZE))) {
			dev_err(dev, "Vaddr out of range\n");
			ret = -EINVAL;
		} else {
			*dma_handle = ffa_to_dma(area->id, *dma_handle - area->dma_handle);
			area->count++;
		}

		mutex_unlock(&vmfdev->lock);
		return ret;
	}

	/* Check if area is already mapped */
	list_for_each_entry(area, &vmfdev->area_list, list) {
		/* TODO: Only support exact page match for now */
		if (area->vaddr == vaddr && area->n_pages == n_pages) {
			*dma_handle = ffa_to_dma(area->id, 0);
			area->count++;
			mutex_unlock(&vmfdev->lock);
			return 0;
		}
	}
	mutex_unlock(&vmfdev->lock);

	return vmsg_ffa_bus_area_share_single(ffa_dev, vaddr, n_pages, dma_handle);
}
EXPORT_SYMBOL_GPL(vmsg_ffa_bus_area_share);

static int vmsg_ffa_bus_area_unshare_single(struct ffa_device *ffa_dev,
		struct shared_area *area)
{
	struct virtio_msg_ffa_device *vmfdev = ffa_dev->dev.driver_data;
	struct virtio_msg *vmsg __free(kfree);
	struct bus_area_unshare *payload;
	int ret;

	vmsg = ffa_msg_alloc(vmfdev);
	if (!vmsg)
		return -ENOMEM;
	payload = virtio_msg_payload(vmsg);

	ffa_msg_prepare(vmfdev, vmsg, VIRTIO_MSG_FFA_BUS_AREA_UNSHARE, sizeof(*payload));
	payload->area_id = cpu_to_le16(area->id);

	ret = vmsg_ffa_send(vmfdev, &vmfdev->async, vmsg, NULL);
	if (!ret) {
		/* reclaim shared memory */
		ret = ffa_dev->ops->mem_ops->memory_reclaim(area->handle, 0);
	}

	ida_free(&vmfdev->area_id_map, area->id);
	kfree(area);

	return ret;
}

int vmsg_ffa_bus_area_unshare(struct device *dev, dma_addr_t *dma_handle,
			      size_t n_pages)
{
	struct ffa_device *ffa_dev = to_ffa_dev(dev);
	struct virtio_msg_ffa_device *vmfdev = ffa_dev->dev.driver_data;
	struct shared_area *area;
	dma_addr_t offset;
	u32 area_id;

	area_id = dma_to_ffa(*dma_handle, &offset);

	mutex_lock(&vmfdev->lock);
	list_for_each_entry(area, &vmfdev->area_list, list) {
		if (area->id == area_id) {
			*dma_handle = area->dma_handle + offset;

			if (--area->count) {
				mutex_unlock(&vmfdev->lock);
				return 0;
			}

			WARN_ON(area->n_pages != n_pages);
			list_del(&area->list);
			mutex_unlock(&vmfdev->lock);

			return vmsg_ffa_bus_area_unshare_single(ffa_dev, area);
		}
	}
	mutex_unlock(&vmfdev->lock);

	return -EINVAL;
}
EXPORT_SYMBOL_GPL(vmsg_ffa_bus_area_unshare);

static struct virtio_msg_ops vmf_ops = {
	.transfer = virtio_msg_ffa_transfer,
	.bus_info = virtio_msg_ffa_bus_info,
};

static int virtio_msg_ffa_user_send(struct virtio_msg_user_device *vmudev,
				    struct virtio_msg *msg)
{
	struct virtio_msg_ffa_device *vmfdev = vmudev_to_vmfdev(vmudev);

	vmsg_ffa_send_indirect(vmfdev, NULL, msg, NULL);
	return 0;
}

static struct virtio_msg_user_ops vmf_user_ops = {
	.send = virtio_msg_ffa_user_send,
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
#ifdef CONFIG_VIRTIO_MSG_FFA_DMA_OPS
	struct reserved_mem *rmem;
	struct device_node *np;
#endif
	int ret, i;

	vmfdev = devm_kzalloc(dev, sizeof(*vmfdev), GFP_KERNEL);
	if (!vmfdev)
		return -ENOMEM;

	ida_init(&vmfdev->area_id_map);

	/* Activate passive mode on host domain */
	vmfdev->passive = ffa_dev->vm_id != 1;

	/* Try direct message first */
	vmfdev->indirect = false;

	vmfdev->ffa_dev = ffa_dev;
	vmfdev->msg_size = VIRTIO_MSG_FFA_BUS_MSG_SIZE;
	ffa_dev_set_drvdata(ffa_dev, vmfdev);
	INIT_LIST_HEAD(&vmfdev->area_list);
	virtio_msg_async_init(&vmfdev->async);
	mutex_init(&vmfdev->lock);

	ret = dma_set_mask_and_coherent(dev, DMA_BIT_MASK(64));
	if (ret)
		ret = dma_set_mask_and_coherent(dev, DMA_BIT_MASK(32));
	if (ret)
		dev_warn(dev, "Failed to enable 64-bit or 32-bit DMA\n");

	ret = vmsg_ffa_indirect_notify_setup(vmfdev);
	if (ret)
		goto ida_destroy;

	if (vmfdev->passive) {
		vmfdev->vmudev.ops = &vmf_user_ops;
		vmfdev->vmudev.parent = &ffa_dev->dev;

		ret = virtio_msg_user_register(&vmfdev->vmudev);
		if (ret) {
			dev_err(&ffa_dev->dev, "Could not register virtio-msg user device\n");
			goto notify_cleanup;
		}

		return 0;
	}

	/* Set DMA OPs for the channel bus device */
#ifdef CONFIG_VIRTIO_MSG_FFA_DMA_OPS
	np = of_parse_phandle(dev->of_node, "memory-region", 0);
	if (!np)
		goto notify_cleanup;

	rmem = of_reserved_mem_lookup(np);
	if (rmem) {
		ret = vmsg_ffa_bus_area_share(dev, phys_to_virt(rmem->base),
					      PFN_UP(rmem->size), NULL);
		if (ret)
			goto notify_cleanup;
		vmfdev->reserved_mem = true;
	}

	dev->dma_ops = &virtio_msg_ffa_dma_ops;
#endif

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

	/* Run the kthread if indirect messages aren't supported */
	if (!(features & VIRTIO_MSG_FFA_FEATURE_INDIRECT_MSG_SUPP)) {
		vmfdev->used_event_task = kthread_run(used_event_task, vmfdev, "vmsg-ffa-ue");
		if (IS_ERR(vmfdev->used_event_task)) {
			ret = PTR_ERR(vmfdev->used_event_task);
			goto unregister;
		}
	}

	return 0;

unregister:
	remove_vmdevs(vmfdev, i);
notify_cleanup:
	vmsg_ffa_indirect_notify_cleanup(vmfdev);
ida_destroy:
	ida_destroy(&vmfdev->area_id_map);
	return ret;
}

static void virtio_msg_ffa_remove(struct ffa_device *ffa_dev)
{
	struct virtio_msg_ffa_device *vmfdev = ffa_dev->dev.driver_data;

	if (vmfdev->passive) {
		virtio_msg_user_unregister(&vmfdev->vmudev);
	} else {
		kthread_stop(vmfdev->used_event_task);
		remove_vmdevs(vmfdev, vmfdev->vmdev_count);
	}

	vmsg_ffa_indirect_notify_cleanup(vmfdev);
	ida_destroy(&vmfdev->area_id_map);
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
	virtio_msg_ffa_dma_init();

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
