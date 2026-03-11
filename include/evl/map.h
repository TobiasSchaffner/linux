/*
 * SPDX-License-Identifier: GPL-2.0
 *
 * Copyright (C) 2026 Philippe Gerum  <rpm@xenomai.org>
 */

#ifndef _EVL_MAP_H
#define _EVL_MAP_H

#include <linux/spinlock.h>
#include <linux/refcount.h>
#include <linux/rbtree.h>
#include <uapi/evl/types-abi.h>

struct evl_map {
	/* rb tree root. */
	struct rb_root root;
	/* Lock guarding the map for access. */
	hard_spinlock_t lock;
	/* Latest allocated key in map (aka 'fundle'). */
	fundle_t generator;
};

struct evl_map_node {
	/* Index key (all reserved bits are cleared). */
	fundle_t fundle;
	/* Number of active users. */
	refcount_t refs;
	/* Anchor node in rb tree. */
	struct rb_node rb_node;
};

void evl_init_map(struct evl_map *map);

fundle_t evl_map_node(struct evl_map *map, struct evl_map_node *n,
	unsigned int type);

void evl_unmap_node(struct evl_map *map, struct evl_map_node *n);

static inline int evl_ref_map_node(struct evl_map_node *n)
{
	return refcount_inc_not_zero(&n->refs);
}

struct evl_map_node *
evl_get_map_node(struct evl_map *map, fundle_t fundle);

static inline int evl_put_map_node(struct evl_map_node *n)
{
	return refcount_dec_and_test(&n->refs);
}

static inline void evl_init_map_node(struct evl_map_node *n)
{
	n->fundle = EVL_NO_HANDLE;
	refcount_set(&n->refs, 1);
}

#endif /* !_EVL_MAP_H */
