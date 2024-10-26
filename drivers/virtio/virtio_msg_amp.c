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

static void __exit virtio_msg_mmio_exit(void) {}
module_exit(virtio_msg_mmio_exit);

EXPORT_SYMBOL_GPL(virtio_msg_amp_register_v0);
EXPORT_SYMBOL_GPL(virtio_msg_amp_register);
EXPORT_SYMBOL_GPL(virtio_msg_amp_unregister);
EXPORT_SYMBOL_GPL(virtio_msg_amp_notify_rx);

MODULE_DESCRIPTION("Virtio-msg for AMP systems");
MODULE_AUTHOR("Bill Mills <bill.mills@linaro.org>");
MODULE_LICENSE("GPL v2");
