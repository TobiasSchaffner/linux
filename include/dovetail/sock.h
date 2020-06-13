/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _DOVETAIL_SOCK_H
#define _DOVETAIL_SOCK_H

#ifdef CONFIG_NET_OOB

struct oob_sock_state {
	/* a pointer to some extended context. */
	void *data;
};

#else  /* !CONFIG_NET_OOB */

struct oob_sock_state {
};

#endif	/* !CONFIG_NET_OOB */

#endif /* !_DOVETAIL_SOCK_H */
