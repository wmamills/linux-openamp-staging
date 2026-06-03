// SPDX-License-Identifier: GPL-2.0+
/*
 * Virtio message transport.
 *
 * Copyright (C) 2026 Linaro.
 * Viresh Kumar <viresh.kumar@linaro.org>
 *
 * This implements unit tests for self-describing shared-memory layout used by
 * virtio message AMP.
 */

#define pr_fmt(fmt) "virtio-msg-amp-shmem-tests: " fmt

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/virtio.h>
#include <linux/sizes.h>
#include <linux/limits.h>
#include <asm/byteorder.h>

#include "virtio_msg_amp_shmem.h"

#define VMAMP_SHMEM_PKT_SIZE	64

#define TEST_PASS		0
#define TEST_FAIL		1
#define TEST_TOTAL		32

static int test_results[TEST_TOTAL];
static int test_count = 0;

/* Helper macros */
#define EXPECT_ERROR(name, ret, expected) \
	do { \
		if ((ret) == (expected)) { \
			pr_debug("PASS: %s (got %d as expected)\n", name, ret); \
			test_results[test_count] = TEST_PASS; \
		} else { \
			pr_err("FAIL: %s (expected %d, got %d)\n", name, expected, ret); \
			test_results[test_count] = TEST_FAIL; \
		} \
		test_count++; \
	} while(0)

#define EXPECT_SUCCESS(name, ret) EXPECT_ERROR(name, ret, 0)

/* Helper to initialize layout with defaults */
static struct vmamp_shmem_layout *init_layout_defaults(u64 layout_size)
{
	struct vmamp_shmem_layout *layout;
	u64 offset;

	layout = kzalloc(layout_size, GFP_KERNEL);
	if (!layout)
		return NULL;

	memset(layout, 0, layout_size);

	layout->length = cpu_to_le16(sizeof(*layout));
	layout->version = cpu_to_le16(VMAMP_SHMEM_VERSION_1);
	layout->features = 0;
	layout->num_notif = 0;
	layout->num_vq_notif_per_device = 0;
	layout->reserved = 0;
	layout->size_queue_elements = cpu_to_le16(VMAMP_SHMEM_PKT_SIZE);
	layout->num_queue_elements = cpu_to_le16(100);

	offset = sizeof(*layout);
	offset = ALIGN(offset, VMAMP_SHMEM_QH_ALIGNMENT);
	layout->dev_queue_head_offset = cpu_to_le64(offset);
	offset += sizeof(struct vmamp_shmem_queue_head);

	layout->drv_queue_head_offset = cpu_to_le64(offset);
	offset += sizeof(struct vmamp_shmem_queue_head);

	offset = ALIGN(offset, VMAMP_SHMEM_QUEUE_ALIGNMENT);
	layout->dev_queue_elements_offset = cpu_to_le64(offset);
	offset += 100 * VMAMP_SHMEM_PKT_SIZE;

	layout->drv_queue_elements_offset = cpu_to_le64(offset);

	layout->dev_notif_offset = 0;
	layout->drv_notif_offset = 0;

	layout->magic = cpu_to_le16(VMAMP_SHMEM_MAGIC_READY);
	return layout;
}

/* Test 1: Valid layout should pass */
static void test_valid_layout(void)
{
	struct vmamp_shmem_layout *layout;
	u64 layout_size = 16 * SZ_1K;
	struct vmamp_shmem shmem;
	int ret;

	layout = init_layout_defaults(layout_size);
	if (!layout)
		return;

	shmem.layout = layout;
	shmem.layout_size = layout_size;
	ret = vmamp_shmem_init(&shmem);
	EXPECT_SUCCESS("valid_layout", ret);

	kfree(layout);
}

/* Test 2: Layout size too small */
static void test_layout_size_too_small(void)
{
	struct vmamp_shmem shmem;
	struct vmamp_shmem_layout *layout;
	u64 layout_size = sizeof(*layout) - 1;
	int ret;

	layout = kzalloc(layout_size, GFP_KERNEL);
	if (!layout)
		return;

	shmem.layout = layout;
	shmem.layout_size = layout_size;
	ret = vmamp_shmem_init(&shmem);
	EXPECT_ERROR("layout_size_too_small", ret, -EINVAL);

	kfree(layout);
}

/* Test 3: Magic not set */
static void test_magic_not_set(void)
{
	struct vmamp_shmem shmem;
	struct vmamp_shmem_layout *layout;
	u64 layout_size = 16 * SZ_1K;
	int ret;

	layout = init_layout_defaults(layout_size);
	if (!layout)
		return;

	layout->magic = 0;

	shmem.layout = layout;
	shmem.layout_size = layout_size;
	ret = vmamp_shmem_init(&shmem);
	EXPECT_ERROR("magic_not_set", ret, -EAGAIN);

	kfree(layout);
}

/* Test 4: Invalid version */
static void test_invalid_version(void)
{
	struct vmamp_shmem shmem;
	struct vmamp_shmem_layout *layout;
	u64 layout_size = 16 * SZ_1K;
	int ret;

	layout = init_layout_defaults(layout_size);
	if (!layout)
		return;

	layout->version = cpu_to_le16(0x99);

	shmem.layout = layout;
	shmem.layout_size = layout_size;
	ret = vmamp_shmem_init(&shmem);
	EXPECT_ERROR("invalid_version", ret, -EINVAL);

	kfree(layout);
}

/* Test 5: Reserved field non-zero */
static void test_reserved_nonzero(void)
{
	struct vmamp_shmem shmem;
	struct vmamp_shmem_layout *layout;
	u64 layout_size = 16 * SZ_1K;
	int ret;

	layout = init_layout_defaults(layout_size);
	if (!layout)
		return;

	layout->reserved = cpu_to_le16(0x1234);

	shmem.layout = layout;
	shmem.layout_size = layout_size;
	ret = vmamp_shmem_init(&shmem);
	EXPECT_ERROR("reserved_nonzero", ret, -EINVAL);

	kfree(layout);
}

/* Test 6: Unsupported features */
static void test_unsupported_features(void)
{
	struct vmamp_shmem shmem;
	struct vmamp_shmem_layout *layout;
	u64 layout_size = 16 * SZ_1K;
	int ret;

	layout = init_layout_defaults(layout_size);
	if (!layout)
		return;

	layout->features = cpu_to_le16(0xFF);

	shmem.layout = layout;
	shmem.layout_size = layout_size;
	ret = vmamp_shmem_init(&shmem);
	EXPECT_ERROR("unsupported_features", ret, -EINVAL);

	kfree(layout);
}

/* Test 7: num_notif exceeds max */
static void test_num_notif_max(void)
{
	struct vmamp_shmem shmem;
	struct vmamp_shmem_layout *layout;
	u64 layout_size = 16 * SZ_1K;
	int ret;

	layout = init_layout_defaults(layout_size);
	if (!layout)
		return;

	layout->num_notif = VMAMP_SHMEM_NUM_NOTIF_MAX + 1;

	shmem.layout = layout;
	shmem.layout_size = layout_size;
	ret = vmamp_shmem_init(&shmem);
	EXPECT_ERROR("num_notif_max", ret, -EINVAL);

	kfree(layout);
}

/* Test 7b: Invalid vq_notif_per_device */
static void test_invalid_vq_notif(void)
{
	struct vmamp_shmem shmem;
	struct vmamp_shmem_layout *layout;
	u64 layout_size = 16 * SZ_1K;
	int ret;

	layout = init_layout_defaults(layout_size);
	if (!layout)
		return;

	layout->num_notif = 1;
	layout->num_vq_notif_per_device = 3; /* Invalid: not power of 2 */

	shmem.layout = layout;
	shmem.layout_size = layout_size;
	ret = vmamp_shmem_init(&shmem);
	EXPECT_ERROR("invalid_vq_notif", ret, -EINVAL);

	kfree(layout);
}

/* Test 8: num_queue_elements = 0 */
static void test_num_queue_elements_zero(void)
{
	struct vmamp_shmem shmem;
	struct vmamp_shmem_layout *layout;
	u64 layout_size = 16 * SZ_1K;
	int ret;

	layout = init_layout_defaults(layout_size);
	if (!layout)
		return;

	layout->num_queue_elements = cpu_to_le16(0);

	shmem.layout = layout;
	shmem.layout_size = layout_size;
	ret = vmamp_shmem_init(&shmem);
	EXPECT_ERROR("num_queue_elements_zero", ret, -EINVAL);

	kfree(layout);
}

/* Test 9: num_queue_elements = 1 (need >= 2) */
static void test_num_queue_elements_one(void)
{
	struct vmamp_shmem shmem;
	struct vmamp_shmem_layout *layout;
	u64 layout_size = 16 * SZ_1K;
	int ret;

	layout = init_layout_defaults(layout_size);
	if (!layout)
		return;

	layout->num_queue_elements = cpu_to_le16(1);

	shmem.layout = layout;
	shmem.layout_size = layout_size;
	ret = vmamp_shmem_init(&shmem);
	EXPECT_ERROR("num_queue_elements_one", ret, -EINVAL);

	kfree(layout);
}

/* Test 10: size_queue_elements = 0 */
static void test_size_queue_elements_zero(void)
{
	struct vmamp_shmem shmem;
	struct vmamp_shmem_layout *layout;
	u64 layout_size = 16 * SZ_1K;
	int ret;

	layout = init_layout_defaults(layout_size);
	if (!layout)
		return;

	layout->size_queue_elements = cpu_to_le16(0);

	shmem.layout = layout;
	shmem.layout_size = layout_size;
	ret = vmamp_shmem_init(&shmem);
	EXPECT_ERROR("size_queue_elements_zero", ret, -EINVAL);

	kfree(layout);
}

/* Test 11: Overlapping dev_qh and drv_qh */
static void test_overlapping_queue_heads(void)
{
	struct vmamp_shmem shmem;
	struct vmamp_shmem_layout *layout;
	uint64_t layout_size = 16 * SZ_1K;
	int ret;

	layout = init_layout_defaults(layout_size);
	if (!layout)
		return;

	/* Make drv_qh overlap with dev_qh */
	layout->drv_queue_head_offset = layout->dev_queue_head_offset;

	shmem.layout = layout;
	shmem.layout_size = layout_size;
	ret = vmamp_shmem_init(&shmem);
	EXPECT_ERROR("overlapping_queue_heads", ret, -EINVAL);

	kfree(layout);
}

/* Test 12: Overlapping queue element regions */
static void test_overlapping_queue_elements(void)
{
	struct vmamp_shmem shmem;
	struct vmamp_shmem_layout *layout;
	uint64_t layout_size = 16 * SZ_1K;
	uint64_t queue_size;
	int ret;

	layout = init_layout_defaults(layout_size);
	if (!layout)
		return;

	queue_size = (uint64_t)100 * VMAMP_SHMEM_PKT_SIZE;

	/* Make drv_queue_elements overlap with dev_queue_elements */
	layout->drv_queue_elements_offset =
		cpu_to_le64(le64_to_cpu(layout->dev_queue_elements_offset) +
		queue_size / 2);

	shmem.layout = layout;
	shmem.layout_size = layout_size;
	ret = vmamp_shmem_init(&shmem);
	EXPECT_ERROR("overlapping_queue_elements", ret, -EINVAL);

	kfree(layout);
}

/* Test 13: Queue region overlaps with layout header */
static void test_queue_overlaps_layout_header(void)
{
	struct vmamp_shmem shmem;
	struct vmamp_shmem_layout *layout;
	uint64_t layout_size = 16 * SZ_1K;
	int ret;

	layout = init_layout_defaults(layout_size);
	if (!layout)
		return;

	/* Move dev_queue_head to overlap with layout header */
	layout->dev_queue_head_offset = cpu_to_le64(4);

	shmem.layout = layout;
	shmem.layout_size = layout_size;
	ret = vmamp_shmem_init(&shmem);
	EXPECT_ERROR("queue_overlaps_layout_header", ret, -EINVAL);

	kfree(layout);
}

/* Test 14: Queue elements extend beyond shared memory */
static void test_queue_elements_beyond_shmem(void)
{
	struct vmamp_shmem shmem;
	struct vmamp_shmem_layout *layout;
	uint64_t layout_size = 512;  /* Very small */
	int ret;

	layout = init_layout_defaults(layout_size);
	if (!layout)
		return;

	/* Set a large number of elements that will exceed layout_size */
	layout->num_queue_elements = cpu_to_le16(1000);

	shmem.layout = layout;
	shmem.layout_size = layout_size;
	ret = vmamp_shmem_init(&shmem);
	EXPECT_ERROR("queue_elements_beyond_shmem", ret, -EINVAL);

	kfree(layout);
}

/* Test 15: Notification region overlaps with queue elements */
static void test_notif_overlaps_queue_elements(void)
{
	struct vmamp_shmem shmem;
	struct vmamp_shmem_layout *layout;
	uint64_t layout_size = 16 * SZ_1K;
	int ret;

	layout = init_layout_defaults(layout_size);
	if (!layout)
		return;

	layout->num_notif = 1;
	layout->num_vq_notif_per_device = 1;

	/* Place dev notification where queue elements are */
	layout->dev_notif_offset = layout->dev_queue_elements_offset;

	shmem.layout = layout;
	shmem.layout_size = layout_size;
	ret = vmamp_shmem_init(&shmem);
	EXPECT_ERROR("notif_overlaps_queue_elements", ret, -EINVAL);

	kfree(layout);
}

/* Test 16: dev_qh overlaps with dev_queue_elements */
static void test_dev_qh_overlaps_dev_queue(void)
{
	struct vmamp_shmem shmem;
	struct vmamp_shmem_layout *layout;
	uint64_t layout_size = 16 * SZ_1K;
	int ret;

	layout = init_layout_defaults(layout_size);
	if (!layout)
		return;

	/* Move dev_qh to overlap with dev_queue_elements start */
	layout->dev_queue_head_offset = layout->dev_queue_elements_offset;

	shmem.layout = layout;
	shmem.layout_size = layout_size;
	ret = vmamp_shmem_init(&shmem);
	EXPECT_ERROR("dev_qh_overlaps_dev_queue", ret, -EINVAL);

	kfree(layout);
}

/* Module init */
static int __init virtio_msg_test_init(void)
{
	int i, passed = 0;

	test_valid_layout();
	test_layout_size_too_small();
	test_magic_not_set();
	test_invalid_version();
	test_reserved_nonzero();
	test_unsupported_features();
	test_num_notif_max();
	test_invalid_vq_notif();
	test_num_queue_elements_zero();
	test_num_queue_elements_one();
	test_size_queue_elements_zero();
	test_overlapping_queue_heads();
	test_overlapping_queue_elements();
	test_queue_overlaps_layout_header();
	test_queue_elements_beyond_shmem();
	test_notif_overlaps_queue_elements();
	test_dev_qh_overlaps_dev_queue();

	for (i = 0; i < test_count; i++) {
		if (test_results[i] == TEST_PASS)
			passed++;
	}

	pr_info("%d/%d passed\n", passed, test_count);

	return (passed == test_count) ? 0 : -EINVAL;
}
core_initcall(virtio_msg_test_init);
