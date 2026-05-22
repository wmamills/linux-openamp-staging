/* SPDX-License-Identifier: ((GPL-2.0 WITH Linux-syscall-note) OR BSD-3-Clause) */
/*
 * Virtio-msg AMP shared memory header.
 *
 * Copyright (c) 2026 Advanced Micro Devices, Inc and Linaro.
 * Viresh Kumar <viresh.kumar@linaro.org>
 *
 * This file defines the self-describing layout placed at the beginning of the
 * shared memory region used for communication between peers.
 *
 * The layout allows both sides to discover:
 *  - notification areas
 *  - queue heads
 *  - message buffers
 *
 * All offsets are relative to the start of the shared memory region unless
 * specified otherwise.
 */

#ifndef _LINUX_VIRTIO_MSG_AMP_SHMEM_H
#define _LINUX_VIRTIO_MSG_AMP_SHMEM_H

#include <linux/types.h>

/**
 * struct vmamp_shmem_queue_head - per-endpoint queue head.
 * @magic: Validity indicator for the queue head. Must be
 *	   VMAMP_SHMEM_QUEUE_MAGIC_READY when valid.
 * @status: Current state of the queue.
 * @tail: Index of next element to be written (producer index).
 * @head: Index of next element to be read (consumer index).
 *
 * The queue is a circular buffer of @num_queue_elements entries, where one entry is
 * always kept unused.
 *
 * - head and tail are written only by this peer.
 * - head and tail are always < num_queue_elements
 * - empty: head == peer->tail
 * - full:  (tail + 1) % num_queue_elements == peer->head
 *
 * The separation of head and tail enables lock-free operation.
 */
struct vmamp_shmem_queue_head {
	#define VMAMP_SHMEM_QUEUE_MAGIC_READY			0x514F /* "QO - Queue OK" */
	__le16 magic;

	#define VMAMP_SHMEM_QUEUE_STATE_MASK			0x001F
		#define VMAMP_SHMEM_QUEUE_STATE_INIT		0x0
		#define VMAMP_SHMEM_QUEUE_STATE_READY		0x1
		#define VMAMP_SHMEM_QUEUE_STATE_RUN		0x2
		#define VMAMP_SHMEM_QUEUE_STATE_SHUTDOWN	0x3
		#define VMAMP_SHMEM_QUEUE_STATE_PEER_DEAD	0x4
	#define VMAMP_SHMEM_QUEUE_FLAG_MASK			0xF000
		#define VMAMP_SHMEM_QUEUE_FLAG_DEBUGGABLE	0x8000
	__le16 status;
	__le16 tail;
	__le16 head;
} __packed;

/*
 * struct vmamp_shmem_notif - Notification bitmap arrays
 * @tx: Bits set by this peer to notify the other side.
 * @rx: Bits set by this peer in response to other peer's tx bits.
 * @mask: Optional (if VMAMP_SHMEM_FEAT_MASKED_NOTIF is set), mask bits to
 *        suppress interrupt generation.
 *
 * Each array has `num_notif` elements.
 * Each element is a 64-bit word (64 notification bits).
 *
 * Pending events are computed as:
 *
 *   pending = rx[n] ^ peer->tx[n]
 *
 * Acknowledgment:
 *
 *   rx[n] ^= pending
 *
 * Hierarchical notification schemes may be implemented using multiple words.
 *
 * struct vmamp_shmem_notif {
 *	__le64 tx[num_notif];
 *	__le64 rx[num_notif];
 *	__le64 mask[num_notif];
 * };
 */

/**
 * struct vmamp_shmem_layout - Self-describing shared memory layout
 * @length: Size of this structure in bytes.
 * @magic: Validity indicator for the layout. Must be VMAMP_SHMEM_MAGIC_READY
 *         when valid.
 * @version: Layout version.
 * @features: Optional features supported by this layout (e.g. masked notifications).
 * @num_notif: Number of notification words (u64) per direction.
 *         Each word provides 64 notification bits. Supported values: 0 to 64.
 * @num_vq_notif_per_device: Number of per-device virtqueue notification bits.
 *         Supported values: 1, 2, 4, 8, 16.
 * @num_queue_elements: Number of elements in each message queue.
 * @size_queue_elements: Size (in bytes) of each queue element.
 * @reserved: Reserved for future use. Must be zero.
 *
 * @dev_notif_offset: Offset to device-owned notification area.
 *         Contains tx/rx (and optional mask) arrays for the device.
 * @dev_queue_head_offset: Offset to device queue head structure.
 * @dev_queue_elements_offset: Offset to device message buffer (queue elements).
 *
 * @drv_notif_offset: Offset to driver-owned notification area.
 * @drv_queue_head_offset: Offset to driver queue head structure.
 * @drv_queue_elements_offset: Offset to driver message buffer (queue elements).
 *
 * The shared memory is split logically into two halves:
 *  - Device-owned region (device writes, driver reads)
 *  - Driver-owned region (driver writes, device reads)
 *
 * Each side updates only its own structures to maintain lock-free operation.
 */
struct vmamp_shmem_layout {
	#define VMAMP_SHMEM_MAGIC_READY		0x4C4F /* "LO - Layout OK" */
	__le16 magic;
	__le16 length;

	#define VMAMP_SHMEM_VERSION_1		0x1
	__le16 version;

	#define VMAMP_SHMEM_FEAT_MASKED_NOTIF	0x1
	__le16 features;

	#define VMAMP_SHMEM_NUM_NOTIF_MAX	64
	__u8 num_notif;
	__u8 num_vq_notif_per_device;

	__le16 num_queue_elements;
	__le16 size_queue_elements;
	__le16 reserved;

	__le64 dev_notif_offset;
	__le64 dev_queue_head_offset;
	__le64 dev_queue_elements_offset;

	__le64 drv_notif_offset;
	__le64 drv_queue_head_offset;
	__le64 drv_queue_elements_offset;
} __attribute__((packed));

#endif /* _LINUX_VIRTIO_MSG_AMP_SHMEM_H */
