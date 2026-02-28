/*
 * SPDX-License-Identifier: GPL-2.0
 *
 * Copyright (C) 2026 Philippe Gerum  <rpm@xenomai.org>
 */

#include <uapi/evl/types-abi.h>
#include <evl/assert.h>
#include <evl/map.h>

/*
 * The largest value a fundle may have. This limit was picked to cover
 * the largest possible element count among threads, monitors and
 * observables while increasing the odds to generate a recently
 * released fundle (under normal circumstances, that is). This value
 * must fit in __FUNDLE_KEY_MASK.
 */
#define EVL_FUNDLE_LIMIT  (1U << 16)

void evl_init_map(struct evl_map *map)
{
	BUILD_BUG_ON(EVL_FUNDLE_LIMIT > __FUNDLE_KEY_MASK);
	raw_spin_lock_init(&map->lock);
	map->root = RB_ROOT;
	map->generator = EVL_NO_HANDLE;
}

static int map_node_at(struct evl_map *map,
		struct evl_map_node *n, fundle_t fundle)
{
	struct rb_node **rbp, *parent;
	struct evl_map_node *tmp;

	parent = NULL;
	rbp = &map->root.rb_node;
	while (*rbp) {
		tmp = rb_entry(*rbp, struct evl_map_node, rb_node);
		parent = *rbp;
		if (fundle == tmp->fundle)
			return -EEXIST;
		if (fundle < tmp->fundle)
			rbp = &(*rbp)->rb_left;
		else
			rbp = &(*rbp)->rb_right;
	}

	n->fundle = fundle;
	rb_link_node(&n->rb_node, parent, rbp);
	rb_insert_color(&n->rb_node, &map->root);

	return 0;
}

fundle_t evl_map_node(struct evl_map *map, struct evl_map_node *n)
{
	fundle_t fundle, guard = 0;
	unsigned long flags;
	int ret;

	/*
	 * We enforce inband-only context because dealing with
	 * conflicts after the generator wrapped around might cause
	 * the code to behave like a CPU hog, spinning until a free
	 * fundle or the limit is reached, whichever comes first. This
	 * assertion clearly states that node mapping should never
	 * happen from a time-critical context, in case the generating
	 * counter wraps around. Lifing such restriction would require
	 * a significantly more complex fundle generation logic.
	 */
	inband_context_only();

	do {
		/*
		 * Make sure not to enter an infinite loop scanning a
		 * fully packed map.
		 */
		if (++guard > EVL_FUNDLE_LIMIT) {
			n->fundle = EVL_NO_HANDLE;
			WARN_ON_ONCE("out of fundle index space");
			return EVL_NO_HANDLE;
		}

		raw_spin_lock_irqsave(&map->lock, flags);

		fundle = ++map->generator;
		if (fundle == EVL_FUNDLE_LIMIT)	/* Wrap around */
			fundle = map->generator = 1;

		ret = map_node_at(map, n, fundle);

		raw_spin_unlock_irqrestore(&map->lock, flags);
	} while (ret);

	return fundle;
}

void evl_unmap_node(struct evl_map *map, struct evl_map_node *n)
{
	unsigned long flags;

	if (n->fundle != EVL_NO_HANDLE) {
		raw_spin_lock_irqsave(&map->lock, flags);
		rb_erase(&n->rb_node, &map->root);
		raw_spin_unlock_irqrestore(&map->lock, flags);
	}
}

static struct evl_map_node *
search_map(struct evl_map *map, fundle_t fundle) /* map->lock held, irqs off */
{
	struct evl_map_node *n;
	struct rb_node *rb;

	rb = map->root.rb_node;
	while (rb) {
		n = rb_entry(rb, struct evl_map_node, rb_node);
		if (fundle == n->fundle)
			break;
		if (fundle < n->fundle)
			rb = rb->rb_left;
		else
			rb = rb->rb_right;
	}

	return rb ? n : NULL;
}

struct evl_map_node *
evl_get_map_node(struct evl_map *map, fundle_t fundle)
{
	struct evl_map_node *n;
	unsigned long flags;

	raw_spin_lock_irqsave(&map->lock, flags);

	n = search_map(map, fundle);
	if (n && !evl_ref_map_node(n))
		n = NULL;

	raw_spin_unlock_irqrestore(&map->lock, flags);

	return n;
}
