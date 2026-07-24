/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Per-edge dwell-time statistics for RV monitors.
 *
 * Copyright (C) 2026 Siemens AG
 * Author:       Tobias Schaffner <tobias.schaffner@siemens.com>.
 */
#ifndef _LINUX_RV_EDGE_STAT_H
#define _LINUX_RV_EDGE_STAT_H

#include <linux/bitops.h>
#include <linux/cache.h>
#include <linux/compiler.h>
#include <linux/log2.h>
#include <linux/types.h>

#define RV_EDGE_HIST_BINS	24

/* Coverage bitmap  allocate per-CPU bitmap */
#define RV_EDGE_COVERAGE_LONGS(n_edges)		BITS_TO_LONGS(n_edges)

static __always_inline __attribute__((no_instrument_function))
void rv_edge_coverage_mark(unsigned long *map, unsigned int edge)
{
	map[edge / BITS_PER_LONG] |= 1UL << (edge % BITS_PER_LONG);
}

struct rv_edge_stat {
	u64	count;
	u64	sum_ns;
	u64	max_ns;
	u32	hist[RV_EDGE_HIST_BINS];
};

static __always_inline __attribute__((no_instrument_function))
void rv_edge_stat_account(struct rv_edge_stat *s, u64 dwell_ns)
{
	unsigned int b;

	s->count  += 1;
	s->sum_ns += dwell_ns;
	if (dwell_ns > s->max_ns)
		s->max_ns = dwell_ns;

	/* log2 bucket: fls64(x|1)-1 keeps zero in bucket 0 */
	b = fls64(dwell_ns | 1) - 1;
	if (b >= RV_EDGE_HIST_BINS)
		b = RV_EDGE_HIST_BINS - 1;
	s->hist[b] += 1;
}

#endif /* _LINUX_RV_EDGE_STAT_H */
