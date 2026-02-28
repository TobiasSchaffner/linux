/*
 * SPDX-License-Identifier: GPL-2.0
 *
 * Copyright (C) 2026 Philippe Gerum  <rpm@xenomai.org>
 */

#include <linux/slab.h>
#include <evl/namespace.h>

struct evl_core_namespace evl_core_ns;

void evl_init_core_ns(struct evl_core_namespace *cns)
{
	evl_init_map(&cns->m_monitor);
	evl_init_map(&cns->m_thread);
	evl_init_map(&cns->m_observable);
}
