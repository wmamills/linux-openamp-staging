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

#if 0
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
#endif

/* this one is temporary as the v0 layout is not self describing */
int virtio_msg_amp_register_v0(struct virtio_msg_amp *amp_dev) {
	return 0;
}

static u8 demo_msg[40] = {
	0x00, /* type */
	0x09, /* id: get status */
	0x00, 0x00, /* dev_id */
	0x00, 0x00, 0x00, 0x00  /* index */
};

static void rx_dump_all(struct virtio_msg_amp *amp_dev) {
	char buf[64];
	struct device *pdev = amp_dev->ops->get_device(amp_dev);

	while (spsc_recv(&amp_dev->dev2drv, buf, 64)) {
		dev_info(pdev, "RX MSG: %16ph \n", buf);
	}
}

/* normal API */
int  virtio_msg_amp_register(struct virtio_msg_amp *amp_dev) {
	size_t page_size = 4096;
	char* mem = amp_dev->shmem;
	void* page0 = &mem[0 * page_size];
	void* page1 = &mem[1 * page_size];

	/* init internal state */
	init_completion(&amp_dev->irq_done);

	spsc_open(&amp_dev->drv2dev, "drv2dev", page0, page_size);
	spsc_open(&amp_dev->dev2drv, "dev2drv", page1, page_size);

	/* empty the rx queue */
	rx_dump_all(amp_dev);

	/* queue a message */
	spsc_send(&amp_dev->drv2dev, demo_msg, sizeof demo_msg);

	/* Notify the peer */
	amp_dev->ops->tx_notify(amp_dev, 0);

	return 0;
}

void virtio_msg_amp_unregister(struct virtio_msg_amp *amp_dev) {
	/* unblock any straglers */
	complete_all(&amp_dev->irq_done);
}

int  virtio_msg_amp_notify_rx(struct virtio_msg_amp *amp_dev, u32 notify_idx) {
	rx_dump_all(amp_dev);
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
