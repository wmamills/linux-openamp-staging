// SPDX-License-Identifier: GPL-2.0
/*
 * Virtio-msg-amp common code
 *
 * Copyright (c) Linaro Ltd, 2024
 *
 */

#include <linux/module.h>
#include <linux/init.h>

#include "virtio_msg_amp.h"

// do the "uio" test with Zephyr peer
static int uio_test(struct virtio_msg_amp *amp_dev)
{
	int i;
	const struct device *pdev = amp_dev->ops->get_device(amp_dev);
	volatile u32 *shm = (u32 *) amp_dev->shmem;
	u32 val;
	long remaining, consumed;
	u32 our_vmid = 1;

	/*
	* Set our peer ID, hard coded for now
	*/
	shm[0] = our_vmid;

	/* invalidate the memory pattern */
	for (i = 1 ; i < 32; i++) {
		shm[i] = 0;
	}

	dev_info(pdev, "SHMEM Before: %32ph \n", amp_dev->shmem);

	/* Notify peer */
	amp_dev->ops->tx_notify(amp_dev, 0);

	/*
	 * Wait notification. read() will block until Zephyr finishes writting
	 * the whole shmem region with value 0xb5b5b5b5.
	 */
	for ( remaining = msecs_to_jiffies(1000); remaining > 0;
		remaining -= consumed) {
		consumed = wait_for_completion_interruptible_timeout(
			&amp_dev->irq_done, remaining);
		if (consumed <= 0)
			break;
	}

	dev_info(pdev, "SHMEM After: %32ph \n", amp_dev->shmem);

	/* Check shmem region: 4 MiB */
	for (i = 1 /* skip peer id */; i < ((4 * 1024 * 1024) / 4) ; i++) {
		val = shm[i];
		if ( val != 0xb5b5b5b5 ) {
			dev_info(pdev, "Data mismatch at %d: %x\n",
				i, val);
			return 1;
		}
	}

	dev_info(pdev, "Data ok. %d byte(s) checked.\n", i * 4);
	return 0;
}

/* this one is temporary as the v0 layout is not self describing */
int virtio_msg_amp_register_v0(struct virtio_msg_amp *amp_dev) {
	return 0;
}

/* normal API */
int  virtio_msg_amp_register(struct virtio_msg_amp *amp_dev) {
	/* init internal state */
	init_completion(&amp_dev->irq_done);

	uio_test(amp_dev);
	return 0;
}

void virtio_msg_amp_unregister(struct virtio_msg_amp *amp_dev) {
	/* unblock any straglers */
	complete_all(&amp_dev->irq_done);
}

int  virtio_msg_amp_notify_rx(struct virtio_msg_amp *amp_dev, u32 notify_idx) {
	complete(&amp_dev->irq_done);
	return 0;
}

static int __init virtio_msg_amp_init(void)
{
	return 0;
}
module_init(virtio_msg_amp_init);

static void __exit virtio_msg_mmio_exit(void) {}
module_exit(virtio_msg_mmio_exit);

EXPORT_SYMBOL_GPL(virtio_msg_amp_register_v0);
EXPORT_SYMBOL_GPL(virtio_msg_amp_register);
EXPORT_SYMBOL_GPL(virtio_msg_amp_unregister);
EXPORT_SYMBOL_GPL(virtio_msg_amp_notify_rx);

MODULE_DESCRIPTION("Virtio-msg for AMP systems");
MODULE_AUTHOR("Bill Mills <bill.mills@linaro.org>");
MODULE_LICENSE("GPL v2");
