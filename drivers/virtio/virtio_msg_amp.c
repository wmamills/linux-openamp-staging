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

/* normal API */
int  virtio_msg_amp_register(struct virtio_msg_amp *amp_dev) {
	return 0;
}

void virtio_msg_amp_unregister(struct virtio_msg_amp *amp_dev) {
}

int  virtio_msg_amp_notify_rx(struct virtio_msg_amp *amp_dev, u32 notify_idx) {
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
