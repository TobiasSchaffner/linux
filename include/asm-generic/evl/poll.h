/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_GENERIC_EVL_POLL_H
#define _ASM_GENERIC_EVL_POLL_H

#include <evl/poll_head.h>

/*
 * Poll operation descriptor for f_op->oob_poll.  Can be attached
 * concurrently to at most EVL_POLL_NR_CONNECTORS poll heads.
 */
#define EVL_POLL_NR_CONNECTORS  4

struct oob_poll_wait {
	struct evl_poll_connector {
		struct evl_poll_head *head;
		struct list_head next;
		void (*unwatch)(struct evl_poll_head *head);
		int events_received;
		int index;
	} connectors[EVL_POLL_NR_CONNECTORS];
};

#define poll_signal_oob(__pwq, __mask)	\
	evl_signal_poll_events(&(__pwq)->head, __mask)

#define poll_wait_oob(__pwq, __wait)	\
	evl_poll_watch(&(__pwq)->head, __wait, NULL)

#endif /* !_ASM_GENERIC_EVL_POLL_H */
