// SPDX-License-Identifier: GPL-2.0+
/*
 * Virtio message transport DMA OPS.
 *
 * Copyright (C) 2024 Google LLC and Linaro.
 * Viresh Kumar <viresh.kumar@linaro.org>
 *
 * The Virtio message transport allows virtio devices to be used over a virtual
 * virtio-msg channel. The channel interface is meant to be implemented using
 * the architecture specific hardware-assisted fast path, like ARM Firmware
 * Framework (FFA).
 */

#define pr_fmt(fmt) "virtio-msg-dma-ops: " fmt

#include <linux/dma-map-ops.h>
#include <linux/virtio.h>
#include <linux/virtio_anchor.h>
#include <xen/xen-ops.h>
#include <uapi/linux/virtio_msg.h>

#include "virtio_msg.h"

static void *virtio_msg_dma_alloc(struct device *dev, size_t size,
				  dma_addr_t *dma_handle, gfp_t gfp,
				  unsigned long attrs)
{
	size_t n_pages = PFN_UP(size);
	void *vaddr;
	int ret;

	vaddr = alloc_pages_exact(n_pages * PAGE_SIZE, gfp);
	if (!vaddr)
		return NULL;

	ret = vmsg_ffa_bus_area_share(dev, vaddr, n_pages, dma_handle);
	if (ret) {
		free_pages_exact(vaddr, n_pages * PAGE_SIZE);
		return NULL;
	}

	return vaddr;
}

static void virtio_msg_dma_free(struct device *dev, size_t size, void *vaddr,
				dma_addr_t dma_handle, unsigned long attrs)
{
	size_t n_pages = PFN_UP(size);
	int ret;

	ret = vmsg_ffa_bus_area_unshare(dev, &dma_handle, n_pages);
	if (ret)
		dev_err(dev, "%s: Failed to unshare area: %d", __func__, ret);

	free_pages_exact(vaddr, n_pages * PAGE_SIZE);
}

static struct page *virtio_msg_dma_alloc_pages(struct device *dev, size_t size,
					      dma_addr_t *dma_handle,
					      enum dma_data_direction dir,
					      gfp_t gfp)
{
	void *vaddr;

	vaddr = virtio_msg_dma_alloc(dev, size, dma_handle, gfp, 0);
	if (!vaddr)
		return NULL;

	return virt_to_page(vaddr);
}

static void virtio_msg_dma_free_pages(struct device *dev, size_t size,
				     struct page *page, dma_addr_t dma_handle,
				     enum dma_data_direction dir)
{
	virtio_msg_dma_free(dev, size, page_to_virt(page), dma_handle, 0);
}

static dma_addr_t virtio_msg_dma_map_page(struct device *dev, struct page *page,
					 unsigned long offset, size_t size,
					 enum dma_data_direction dir,
					 unsigned long attrs)
{
	size_t n_pages = PFN_UP(offset + size);
	dma_addr_t dma_handle;

	if (WARN_ON(dir == DMA_NONE))
		return DMA_MAPPING_ERROR;

	if (vmsg_ffa_bus_area_share(dev, page_to_virt(page), n_pages,
				    &dma_handle))
		return DMA_MAPPING_ERROR;

	return dma_handle + offset;
}

static void virtio_msg_dma_unmap_page(struct device *dev, dma_addr_t dma_handle,
				     size_t size, enum dma_data_direction dir,
				     unsigned long attrs)
{
	unsigned long dma_offset = offset_in_page(dma_handle);
	unsigned int n_pages = PFN_UP(size);
	int ret;

	if (WARN_ON(dir == DMA_NONE))
		return;

	dma_handle -= dma_offset;

	ret = vmsg_ffa_bus_area_unshare(dev, &dma_handle, n_pages);
	if (ret)
		dev_err(dev, "%s: Failed to unshare area: %d", __func__, ret);
}

static void virtio_msg_dma_unmap_sg(struct device *dev, struct scatterlist *sg,
				   int nents, enum dma_data_direction dir,
				   unsigned long attrs)
{
	struct scatterlist *s;
	unsigned int i;

	if (WARN_ON(dir == DMA_NONE))
		return;

	for_each_sg(sg, s, nents, i)
		virtio_msg_dma_unmap_page(dev, s->dma_address, sg_dma_len(s), dir,
				attrs);
}

static int virtio_msg_dma_map_sg(struct device *dev, struct scatterlist *sg,
				int nents, enum dma_data_direction dir,
				unsigned long attrs)
{
	struct scatterlist *s;
	unsigned int i;

	if (WARN_ON(dir == DMA_NONE))
		return -EINVAL;

	for_each_sg(sg, s, nents, i) {
		s->dma_address = virtio_msg_dma_map_page(dev, sg_page(s), s->offset,
				s->length, dir, attrs);
		if (s->dma_address == DMA_MAPPING_ERROR)
			goto out;

		sg_dma_len(s) = s->length;
	}

	return nents;

out:
	virtio_msg_dma_unmap_sg(dev, sg, i, dir, attrs | DMA_ATTR_SKIP_CPU_SYNC);
	sg_dma_len(sg) = 0;

	return -EIO;
}

static int virtio_msg_dma_supported(struct device *dev, u64 mask)
{
	return mask == DMA_BIT_MASK(64);
}

const struct dma_map_ops virtio_msg_dma_ops = {
	.alloc = virtio_msg_dma_alloc,
	.free = virtio_msg_dma_free,
	.alloc_pages_op = virtio_msg_dma_alloc_pages,
	.free_pages = virtio_msg_dma_free_pages,
	.mmap = dma_common_mmap,
	.get_sgtable = dma_common_get_sgtable,
	.map_page = virtio_msg_dma_map_page,
	.unmap_page = virtio_msg_dma_unmap_page,
	.map_sg = virtio_msg_dma_map_sg,
	.unmap_sg = virtio_msg_dma_unmap_sg,
	.dma_supported = virtio_msg_dma_supported,
};
EXPORT_SYMBOL_GPL(virtio_msg_ffa_dma_ops);

static bool virtio_msg_dma_ops_init(struct virtio_device *dev)
{
	/* DMA OPS should already be set by the underlying channel driver */
	if (dev->dev.parent->dma_ops)
		return true;

	/* Fallback to Xen DMA OPS if enabled */
	if (IS_ENABLED(CONFIG_XEN_VIRTIO))
		return xen_virtio_restricted_mem_acc(dev);

	return false;
}

static int __init virtio_msg_init(void)
{
	/* Register callback to setup DMA ops */
	virtio_set_mem_acc_cb_type(virtio_msg_dma_ops_init, CB_TYPE_VIRTIO_MSG);
	return 0;
}
early_initcall(virtio_msg_init);
