/*
 * SPDX-License-Identifier: GPL-2.0
 *
 * Copyright (C) 2026 Philippe Gerum  <rpm@xenomai.org>
 */

#ifndef _EVL_NAMESPACE_H
#define _EVL_NAMESPACE_H

#include <linux/err.h>
#include <dovetail/mm_info.h>
#include <evl/map.h>

struct evl_core_namespace {
	/* Monitor element map. */
	struct evl_map m_monitor;
	/* Thread element map. */
	struct evl_map m_thread;
	/* Observable element map. */
	struct evl_map m_observable;
	/* Scope of the core namespace. */
	struct evl_scope scope;
};

void evl_init_core_ns(struct evl_core_namespace *cns);

/*
 * evl_private_scope - Private scope of the current process.
 *
 * May return NULL if current is not attached to the core.
 */
#define evl_private_scope()						\
	({								\
		struct oob_mm_state *oob_mm = dovetail_mm_state();	\
		oob_mm ? &oob_mm->scope : NULL;				\
	})

/*
 * evl_access_scope - Access scope in a context-sensitive way.
 *
 * Returns the valid access scope of an element, which is influenced
 * by its shareability attribute.
 */
#define evl_access_scope(__ns, __e)					\
	({								\
		(__e)->clone_flags & EVL_CLONE_SHAREABLE ?		\
			&(__ns)->scope :				\
			evl_private_scope() ?: &(__ns)->scope;		\
	})

#define __evl_type_external	0
#define __evl_type_thread	1
#define __evl_type_monitor	2
#define __evl_type_observable	3

/*
 * Index an element into the core namespace, storing the namespace of
 * the calling context into the element. The fundle annotated with the
 * element type is returned.
 */
#define evl_add_ns(__ns, __e, __tag)					\
	({								\
		fundle_t __fundle;					\
		__fundle = evl_map_node(&(__ns)->m_ ## __tag,		\
					&(__e)->ns_node,		\
					__evl_type_ ## __tag);		\
		__fundle == EVL_NO_HANDLE ? EVL_NO_HANDLE :		\
			((__e)->scope = evl_access_scope(__ns, __e),	\
				__fundle);				\
	})

/*
 * Remove element @e from the namespace @ns.
 */
#define evl_remove_ns(__ns, __e, __tag)						\
	do {									\
		evl_unmap_node(&(__ns)->m_ ## __tag, &(__e)->ns_node);		\
	} while (0)

#define __evl_lookup_ns(__ns, __fundle, __tag)					\
	({									\
		struct evl_map_node *__n;					\
		__n = evl_get_map_node(&(__ns)->m_ ## __tag, __evl_fundle_key(__fundle)); \
		__n ? container_of(__n, struct evl_element, ns_node) : NULL;	\
	})

/*
 * evl_lookup_ns_any - Search for an element with no scope
 * restriction.
 *
 * Returns the element container indexed on @fundle in namespace @ns
 * on success, or NULL if not found. The reference count of the found
 * element is incremented, evl_put_element() should be called to drop
 * this reference.
 */
#define evl_lookup_ns_any(__ns, __fundle, __tag)					\
	({										\
		struct evl_element *__e = __evl_lookup_ns(__ns, __fundle, __tag);	\
		__e ? container_of(__e, struct evl_ ## __tag, element) : NULL;		\
	})

/*
 * evl_check_scope - Check whether an element defined in namespace @ns
 * is visible from the current scope.
 *
 * Returns true if @e is visible to the caller.
 */
#define evl_check_scope(__ns, __e)					\
	({								\
		__e && evl_access_scope(__ns, __e) == (__e)->scope;	\
	})

/*
 * evl_lookup_ns - Search for an element in a context-specific way.
 *
 * Returns the element container indexed on @fundle in namespace @ns
 * on success. Otherwise, NULL is returned if not found, or
 * ERR_PTR(-EPERM) if found by out of scope. The reference count of
 * the found element is incremented, evl_put_element() should be
 * called to drop this reference.
 *
 * CAUTION: we may _never_, _ever_ dereference e->scope unless it is
 * set to &ns->scope. Reason is that the anchor struct representing a
 * process-private scope lives in the oob_mm block of a thread. That
 * block might be stale memory at the moment of the lookup call
 * because the thread might be gone, although the element might still
 * be pending deletion until the delayed fput worker releases the file
 * that element is bound to. IOW, we match scopes only by comparing
 * scope pointers.
 */
#define evl_lookup_ns(__ns, __fundle, __tag)						\
	({										\
		struct evl_ ## __tag *__p = evl_lookup_ns_any(__ns, __fundle, __tag);	\
		struct evl_element *__e = __p ? &__p->element : NULL;			\
		evl_check_scope(__ns, __e) ? __p : __e ? ERR_PTR(-EPERM) : NULL;	\
	})

extern struct evl_core_namespace evl_core_ns;

#endif /* !_EVL_NAMESPACE_H */
