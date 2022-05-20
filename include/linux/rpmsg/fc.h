/* SPDX-License-Identifier: GPL-2.0 */

#ifndef _LINUX_RPMSG_FC_H
#define _LINUX_RPMSG_FC_H

#include <linux/mod_devicetable.h>
#include <linux/rpmsg.h>
#include <linux/rpmsg/byteorder.h>
#include <linux/types.h>

/* The feature bitmap for the endpoint flow control flags */
#define	RPMSG_EPT_FC_ON	 BIT(0) /* Set when endpoint is ready to communicate */

/**
 * struct rpmsg_ept_msg - dynamic endpoint announcement message
 * @src: address of the endpoint that sends the message
 * @dest: address of the destination endpoint.
 * @flags: indicates the state of the endpoint based on @rpmsg_ept_flags enum.
 *
 * This message is sent across to inform the remote about the state of a local
 * endpoint associated with a remote endpoint:
 * - a RPMSG_EPT_OFF can be sent to inform that a local endpoint is suspended.
 * - a RPMSG_EPT_ON can be sent to inform that a local endpoint is ready to communicate.
 *
 * When we receive these messages, the appropriate endpoint is informed.
 */
struct rpmsg_ept_msg {
	__rpmsg32 src;
	__rpmsg32 dst;
	__rpmsg32 flags;
} __packed;

/* Address 54 is reserved for flow control advertising */
#define RPMSG_FC_ADDR                   (54)

#if IS_ENABLED(CONFIG_RPMSG_FC)

int rpmsg_fc_register_device(struct rpmsg_device *rpdev);

#else

static inline int rpmsg_fc_register_device(struct rpmsg_device *rpdev)
{
	/* This shouldn't be possible */
	WARN_ON(1);

	return -ENXIO;
}
#endif /* IS_ENABLED(CONFIG_RPMSG_FC)*/

#endif
