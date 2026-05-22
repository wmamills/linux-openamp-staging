/* SPDX-License-Identifier: ((GPL-2.0 WITH Linux-syscall-note) OR BSD-3-Clause) */
/*
 * Virtio-msg AMP shared memory header.
 *
 * Copyright (c) 2026 Advanced Micro Devices, Inc and Linaro.
 * Viresh Kumar <viresh.kumar@linaro.org>
 *
 * This implements the helpers to parse layout of the shared memory and send
 * receive messages over it.
 */

#ifndef _DRIVERS_VIRTIO_MSG_AMP_SHMEM_H
#define _DRIVERS_VIRTIO_MSG_AMP_SHMEM_H

#include <linux/align.h>
#include <linux/delay.h>
#include <linux/overflow.h>
#include <linux/types.h>

#include <uapi/linux/virtio_msg_amp_shmem.h>

/* Delay of 1 second over a period of 5 minutes */
#define VMAMP_SHMEM_RETRIES	300
#define VMAMP_SHMEM_DELAY_MIN	1000000
#define VMAMP_SHMEM_DELAY_MAX	2000000

struct vmamp_shmem_notif {
	__le64 *tx;
	__le64 *rx;
	__le64 *mask;
};

struct vmamp_shmem {
	struct vmamp_shmem_layout *layout;
	u64 layout_size;
	struct vmamp_shmem_notif my_notif;
	struct vmamp_shmem_notif peer_notif;
	struct vmamp_shmem_queue_head *my_qh;
	struct vmamp_shmem_queue_head *peer_qh;
	u8 *my_queue;
	u8 *peer_queue;
	bool masked_notif;
	u16 size_queue_elements;
	u16 num_queue_elements;
	u8 num_notif;
	u8 num_vq_notif_per_device;
};

struct vmamp_shmem_region {
	u64 offset;
	size_t size;
};

#define VMAMP_SHMEM_SUPPORTED_FEATURES	VMAMP_SHMEM_FEAT_MASKED_NOTIF
#define VMAMP_SHMEM_QH_ALIGNMENT	__alignof__(__le16)
#define VMAMP_SHMEM_NOTIF_ALIGNMENT	__alignof__(__le64)
#define VMAMP_SHMEM_QUEUE_ALIGNMENT	1

#define get_val(_ptr, _name)		\
	le16_to_cpu(smp_load_acquire(&(_ptr)->_name))
#define set_val(_ptr, _name, _val)	\
	smp_store_release(&(_ptr)->_name, cpu_to_le16(_val))

static inline u16 next_entry(u16 idx, u16 size)
{
	return (idx + 1 == size) ? 0 : idx + 1;
}

static inline u16 get_queue_magic(struct vmamp_shmem_queue_head *qh)
{
	return get_val(qh, magic);
}

static inline void set_queue_magic(struct vmamp_shmem_queue_head *qh, u16 val)
{
	set_val(qh, magic, val);
}

static inline u16 get_queue_status(struct vmamp_shmem_queue_head *qh)
{
	return get_val(qh, status) & VMAMP_SHMEM_QUEUE_STATE_MASK;
}

static inline void set_queue_status(struct vmamp_shmem_queue_head *qh, u16 val)
{
	set_val(qh, status, val & VMAMP_SHMEM_QUEUE_STATE_MASK);
}

static inline u16 get_queue_tail(struct vmamp_shmem_queue_head *qh)
{
	return get_val(qh, tail);
}

static inline void set_queue_tail(struct vmamp_shmem_queue_head *qh, u16 val)
{
	set_val(qh, tail, val);
}

static inline u16 get_queue_head(struct vmamp_shmem_queue_head *qh)
{
	return get_val(qh, head);
}

static inline void set_queue_head(struct vmamp_shmem_queue_head *qh, u16 val)
{
	set_val(qh, head, val);
}

static inline bool vmamp_shmem_valid_vq_notif_count(u8 count)
{
	return count == 1 || count == 2 || count == 4 || count == 8 ||
	       count == 16;
}

static inline int vmamp_shmem_validate_region(struct vmamp_shmem *shmem,
					      u64 offset, size_t size,
					      size_t alignment)
{
	u64 end;

	if (!offset || !size || !IS_ALIGNED(offset, alignment))
		return -EINVAL;

	if (check_add_overflow(offset, size, &end))
		return -EINVAL;

	if (end > shmem->layout_size)
		return -EINVAL;

	return 0;
}

static inline bool
vmamp_shmem_regions_overlap(const struct vmamp_shmem_region *a,
			    const struct vmamp_shmem_region *b)
{
	return a->offset < b->offset + b->size &&
	       b->offset < a->offset + a->size;
}

static inline int
vmamp_shmem_validate_regions(const struct vmamp_shmem_region *regions, int nr)
{
	int i, j;

	for (i = 0; i < nr; i++) {
		for (j = i + 1; j < nr; j++) {
			if (vmamp_shmem_regions_overlap(&regions[i],
							&regions[j]))
				return -EINVAL;
		}
	}

	return 0;
}

static inline int vmamp_shmem_send(struct vmamp_shmem *shmem, void *buf, size_t size)
{
	struct vmamp_shmem_queue_head *myqh = shmem->my_qh;
	struct vmamp_shmem_queue_head *peerqh = shmem->peer_qh;
	u16 tail, next_tail;

	if (unlikely(!size || size > shmem->size_queue_elements))
		return -EINVAL;

	tail = get_queue_tail(myqh);
	next_tail = next_entry(tail, shmem->num_queue_elements);

	/* Queue full ? */
	if (unlikely(next_tail == get_queue_head(peerqh)))
		return -EAGAIN;

	memcpy(shmem->my_queue + shmem->size_queue_elements * tail, buf, size);
	set_queue_tail(myqh, next_tail);
	return 0;
}

static inline int vmamp_shmem_recv(struct vmamp_shmem *shmem, void *buf, size_t size)
{
	struct vmamp_shmem_queue_head *myqh = shmem->my_qh;
	struct vmamp_shmem_queue_head *peerqh = shmem->peer_qh;
	u16 head;

	if (unlikely(!size || size > shmem->size_queue_elements))
		return -EINVAL;

	head = get_queue_head(myqh);

	/* Queue empty ? */
	if (unlikely(head == get_queue_tail(peerqh)))
		return -EAGAIN;

	memcpy(buf, shmem->peer_queue + shmem->size_queue_elements * head, size);
	set_queue_head(myqh, next_entry(head, shmem->num_queue_elements));
	return 0;
}

static inline int vmamp_shmem_queue_init(struct vmamp_shmem *shmem)
{
	struct vmamp_shmem_queue_head *myqh = shmem->my_qh;
	struct vmamp_shmem_queue_head *peerqh = shmem->peer_qh;
	u32 retries = 0;
	u16 val;

	set_queue_status(myqh, VMAMP_SHMEM_QUEUE_STATE_INIT);
	set_queue_magic(myqh, VMAMP_SHMEM_QUEUE_MAGIC_READY);

	/* Wait for the peer to initialize the queue */
	while (get_queue_magic(peerqh) != VMAMP_SHMEM_QUEUE_MAGIC_READY) {
		usleep_range(VMAMP_SHMEM_DELAY_MIN, VMAMP_SHMEM_DELAY_MAX);

		if (++retries == VMAMP_SHMEM_RETRIES)
			goto shutdown;
	}

	if (get_queue_status(peerqh) > VMAMP_SHMEM_QUEUE_STATE_RUN)
		goto shutdown;

	/* Publish initialized head/tail before marking queue READY */
	set_queue_head(myqh, 0);
	set_queue_tail(myqh, 0);
	set_queue_status(myqh, VMAMP_SHMEM_QUEUE_STATE_READY);

	retries = 0;

	/* Wait for the peer to be in READY or RUN state */
	while (1) {
		val = get_queue_status(peerqh);

		if (val == VMAMP_SHMEM_QUEUE_STATE_INIT) {
			usleep_range(VMAMP_SHMEM_DELAY_MIN, VMAMP_SHMEM_DELAY_MAX);

			if (++retries == VMAMP_SHMEM_RETRIES)
				goto shutdown;
		} else if (unlikely(val > VMAMP_SHMEM_QUEUE_STATE_RUN)) {
			goto shutdown;
		} else {
			break;
		}
	}

	set_queue_status(myqh, VMAMP_SHMEM_QUEUE_STATE_RUN);
	return 0;

shutdown:
	set_queue_status(myqh, VMAMP_SHMEM_QUEUE_STATE_SHUTDOWN);
	return -EIO;
}

static inline int vmamp_shmem_queue_dead(struct vmamp_shmem *shmem)
{
	struct vmamp_shmem_queue_head *myqh = shmem->my_qh;
	struct vmamp_shmem_queue_head *peerqh = shmem->peer_qh;
	u32 retries = 0;

	set_queue_status(myqh, VMAMP_SHMEM_QUEUE_STATE_PEER_DEAD);

	while (get_queue_status(peerqh) > VMAMP_SHMEM_QUEUE_STATE_READY) {
		usleep_range(VMAMP_SHMEM_DELAY_MIN, VMAMP_SHMEM_DELAY_MAX);

		if (++retries == VMAMP_SHMEM_RETRIES)
			return -ETIMEDOUT;
	}

	return 0;
}

static inline void vmamp_shmem_queue_shutdown(struct vmamp_shmem *shmem)
{
	if (shmem->my_qh)
		set_queue_status(shmem->my_qh, VMAMP_SHMEM_QUEUE_STATE_SHUTDOWN);
}

static inline int vmamp_shmem_init(struct vmamp_shmem *shmem)
{
	struct vmamp_shmem_layout *layout = shmem->layout;
	u64 dev_notif_offset, drv_notif_offset;
	u64 dev_qh_offset, drv_qh_offset;
	u64 dev_queue_offset, drv_queue_offset;
	struct vmamp_shmem_region regions[7];
	size_t notif_size = 0, queue_size;
	u16 features;
	int nr = 0, ret;

	if (!layout)
		return -ENOMEM;

	if (shmem->layout_size < sizeof(*layout))
		return -EINVAL;

	if (get_val(layout, magic) != VMAMP_SHMEM_MAGIC_READY)
		return -EAGAIN;

	/*
	 * Once the magic number is set, the peer will not update rest of the
	 * fields in layout. Don't need READ_ONCE() for rest of the reads.
	 */
	if (le16_to_cpu(layout->length) != sizeof(*layout))
		return -EINVAL;

	if (le16_to_cpu(layout->version) != VMAMP_SHMEM_VERSION_1)
		return -EINVAL;

	features = le16_to_cpu(layout->features);
	if (features & ~VMAMP_SHMEM_SUPPORTED_FEATURES)
		return -EINVAL;

	if (le16_to_cpu(layout->reserved))
		return -EINVAL;

	if (layout->num_notif > VMAMP_SHMEM_NUM_NOTIF_MAX)
		return -EINVAL;

	if (layout->num_notif &&
	    !vmamp_shmem_valid_vq_notif_count(layout->num_vq_notif_per_device))
		return -EINVAL;

	shmem->size_queue_elements = le16_to_cpu(layout->size_queue_elements);
	shmem->num_queue_elements = le16_to_cpu(layout->num_queue_elements);

	if (!shmem->size_queue_elements || shmem->num_queue_elements < 2)
		return -EINVAL;

	if (check_mul_overflow((size_t)shmem->num_queue_elements,
			       (size_t)shmem->size_queue_elements,
			       &queue_size))
		return -EINVAL;

	if (layout->num_notif) {
		shmem->num_notif = layout->num_notif;
		shmem->masked_notif = features & VMAMP_SHMEM_FEAT_MASKED_NOTIF;
		shmem->num_vq_notif_per_device = layout->num_vq_notif_per_device;
		if (check_mul_overflow((size_t)shmem->num_notif,
				       sizeof(*shmem->my_notif.tx) *
				       (shmem->masked_notif ? 3 : 2),
				       &notif_size))
			return -EINVAL;
	} else {
		shmem->num_notif = 0;
		shmem->masked_notif = false;
		shmem->num_vq_notif_per_device = 0;
	}

	dev_qh_offset = le64_to_cpu(layout->dev_queue_head_offset);
	drv_qh_offset = le64_to_cpu(layout->drv_queue_head_offset);
	dev_queue_offset = le64_to_cpu(layout->dev_queue_elements_offset);
	drv_queue_offset = le64_to_cpu(layout->drv_queue_elements_offset);
	dev_notif_offset = le64_to_cpu(layout->dev_notif_offset);
	drv_notif_offset = le64_to_cpu(layout->drv_notif_offset);

	ret = vmamp_shmem_validate_region(shmem, dev_qh_offset,
					  sizeof(*shmem->peer_qh),
					  VMAMP_SHMEM_QH_ALIGNMENT);
	if (ret)
		return ret;

	ret = vmamp_shmem_validate_region(shmem, drv_qh_offset,
					  sizeof(*shmem->my_qh),
					  VMAMP_SHMEM_QH_ALIGNMENT);
	if (ret)
		return ret;

	ret = vmamp_shmem_validate_region(shmem, dev_queue_offset, queue_size,
					  VMAMP_SHMEM_QUEUE_ALIGNMENT);
	if (ret)
		return ret;

	ret = vmamp_shmem_validate_region(shmem, drv_queue_offset, queue_size,
					  VMAMP_SHMEM_QUEUE_ALIGNMENT);
	if (ret)
		return ret;

	if (shmem->num_notif) {
		ret = vmamp_shmem_validate_region(shmem, dev_notif_offset,
						  notif_size,
						  VMAMP_SHMEM_NOTIF_ALIGNMENT);
		if (ret)
			return ret;

		ret = vmamp_shmem_validate_region(shmem, drv_notif_offset,
						  notif_size,
						  VMAMP_SHMEM_NOTIF_ALIGNMENT);
		if (ret)
			return ret;
	}

	regions[nr++] = (struct vmamp_shmem_region){ 0, sizeof(*layout) };
	regions[nr++] = (struct vmamp_shmem_region){ dev_qh_offset,
						     sizeof(*shmem->peer_qh) };
	regions[nr++] = (struct vmamp_shmem_region){ drv_qh_offset,
						     sizeof(*shmem->my_qh) };
	regions[nr++] = (struct vmamp_shmem_region){ dev_queue_offset,
						     queue_size };
	regions[nr++] = (struct vmamp_shmem_region){ drv_queue_offset,
						     queue_size };
	if (shmem->num_notif) {
		regions[nr++] = (struct vmamp_shmem_region){ dev_notif_offset,
							     notif_size };
		regions[nr++] = (struct vmamp_shmem_region){ drv_notif_offset,
							     notif_size };
	}

	if (vmamp_shmem_validate_regions(regions, nr))
		return -EINVAL;

	shmem->peer_qh = (void *)layout + dev_qh_offset;
	shmem->peer_queue = (void *)layout + dev_queue_offset;

	if (shmem->num_notif) {
		shmem->peer_notif.tx = (void *)layout + dev_notif_offset;

		shmem->peer_notif.rx = shmem->peer_notif.tx + shmem->num_notif;
		if (shmem->masked_notif)
			shmem->peer_notif.mask =
				shmem->peer_notif.rx + shmem->num_notif;
		else
			shmem->peer_notif.mask = NULL;
	}

	shmem->my_qh = (void *)layout + drv_qh_offset;
	shmem->my_queue = (void *)layout + drv_queue_offset;

	if (shmem->num_notif) {
		shmem->my_notif.tx = (void *)layout + drv_notif_offset;
		shmem->my_notif.rx =
			shmem->my_notif.tx + shmem->num_notif;
		if (shmem->masked_notif)
			shmem->my_notif.mask =
				shmem->my_notif.rx + shmem->num_notif;
		else
			shmem->my_notif.mask = NULL;
	}

	/* Initialize the driver area */
	memset(shmem->my_queue, 0,
	       shmem->num_queue_elements * shmem->size_queue_elements);
	if (shmem->num_notif) {
		memset(shmem->my_notif.tx, 0,
		       shmem->num_notif * sizeof(*shmem->my_notif.tx) *
		       (shmem->masked_notif ? 3 : 2));
	}

	return 0;
}
#endif /* _DRIVERS_VIRTIO_MSG_AMP_SHMEM_H */
