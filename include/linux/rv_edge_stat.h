/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Per-edge dwell-time statistics for RV-style monitors.
 *
 * Lock-free per-CPU storage: count, sum, max and a fixed log2
 * histogram (bucket b covers [2^b, 2^(b+1)) ns). The caller is
 * expected to keep hard IRQs off (true on every EVL OOB path) or
 * otherwise guarantee exclusion on @s.
 */
#ifndef _LINUX_RV_EDGE_STAT_H
#define _LINUX_RV_EDGE_STAT_H

#include <linux/bitops.h>
#include <linux/cache.h>
#include <linux/compiler.h>
#include <linux/log2.h>
#include <linux/types.h>

#define RV_EDGE_HIST_BINS	24

/*
 * Coverage bitmap helpers. A monitor that wants edge-coverage
 * reporting allocates a per-CPU bitmap of @n_edges bits and calls
 * rv_edge_coverage_mark() from the same site as
 * rv_edge_stat_account(); the framework folds the per-CPU bitmaps via
 * the monitor's coverage_snapshot() callback. Open-coded to stay
 * trivially inlinable on the OOB hot path.
 */
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

	/*
	 * Branchless log2 bucket: fls64() returns the 1-based position
	 * of the highest set bit; OR'ing 1 keeps dwell_ns == 0 in
	 * bucket 0 without an explicit zero test.
	 */
	b = fls64(dwell_ns | 1) - 1;
	if (b >= RV_EDGE_HIST_BINS)
		b = RV_EDGE_HIST_BINS - 1;
	s->hist[b] += 1;
}

#endif /* _LINUX_RV_EDGE_STAT_H */
