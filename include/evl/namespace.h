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
	struct evl_map m_monitor;
	struct evl_map m_thread;
	struct evl_map m_observable;
	struct evl_namespace ns;
};

void evl_init_core_ns(struct evl_core_namespace *cns);

/*
 * evl_private_ns - Private namespace of the current process.
 *
 * May return NULL if current is not attached to the core.
 */
#define evl_private_ns()						\
	({								\
		struct oob_mm_state *oob_mm = dovetail_mm_state();	\
		oob_mm ? &oob_mm->ns : NULL;				\
	})

/*
 * evl_access_ns - Access namespace in a context-sensitive way.
 *
 * Returns the access namespace based on the access scope defined for
 * the element and the current thread context.
 */
#define evl_access_ns(__e)						\
	({								\
		(__e)->clone_flags & EVL_CLONE_SHAREABLE ?		\
			&evl_core_ns.ns :				\
			evl_private_ns() ?: &evl_core_ns.ns;		\
	})

#define __evl_type_external	(0 << __FUNDLE_TYPE_SHIFT)
#define __evl_type_thread	(1 << __FUNDLE_TYPE_SHIFT)
#define __evl_type_monitor	(2 << __FUNDLE_TYPE_SHIFT)
#define __evl_type_observable	(3 << __FUNDLE_TYPE_SHIFT)

/*
 * Index an element into the core namespace, storing the namespace of
 * the calling context into the element. The fundle annotated with the
 * element type is returned.
 *
 * CAUTION: the element type is an external information which is NOT
 * encoded into ns_node.fundle. The latter is a mere index key in the
 * map with all reserved bits cleared.
 */
#define evl_add_ns(__e, __tag)						\
	({								\
		fundle_t __fundle;					\
		__fundle = evl_map_node(&evl_core_ns.m_ ## __tag,	\
					&(__e)->ns_node);		\
		__fundle == EVL_NO_HANDLE ? EVL_NO_HANDLE :		\
			((__e)->ns = evl_access_ns(__e),		\
				__fundle | __evl_type_ ## __tag);	\
	})

#define evl_remove_ns(__e, __tag)						\
	do {									\
		evl_unmap_node(&evl_core_ns.m_ ## __tag, &(__e)->ns_node);	\
	} while (0)

#define __evl_lookup_ns(__fundle, __tag)					\
	({									\
		struct evl_map_node *__n;					\
		__n = evl_get_map_node(&evl_core_ns.m_ ## __tag, __fundle);	\
		__n ? container_of(__n, struct evl_element, ns_node) : NULL;	\
	})

/*
 * evl_lookup_ns_any - Search for an element with no namespace
 * restriction.
 *
 * Returns the element container indexed on @__fundle on success, or
 * NULL if not found.
 */
#define evl_lookup_ns_any(__fundle, __tag)					\
	({									\
		struct evl_element *__e = __evl_lookup_ns(__fundle, __tag); 	\
		__e ? container_of(__e, struct evl_ ## __tag, element) : NULL;	\
	})

/*
 * evl_lookup_ns - Search for an element in a context-specific way.
 *
 * Returns the element container indexed on @__fundle on
 * success. Otherwise, NULL is returned if not found, or
 * ERR_PTR(-EPERM) if found by out of scope.
 *
 * CAUTION: we may _never_, _ever_ dereference e->ns unless it is set
 * to &evl_core_ns. Reason is that the anchor struct representing a
 * private namespace lives in the oob_mm block of a thread. That block
 * might be stale memory at the moment of the lookup call because the
 * thread has exited, although the element might still be pending
 * deletion until the delayed fput worker releases the file that
 * element is bound to. IOW, we match namespaces only by comparing
 * namespace pointers.
 */
#define evl_lookup_ns(__fundle, __tag)						\
	({									\
		struct evl_ ## __tag *__p = evl_lookup_ns_any(__fundle, __tag); \
		struct evl_element *__e = __p ? &__p->element : NULL;		\
		__e && evl_access_ns(__e) == __e->ns ? __p : __e ? ERR_PTR(-EPERM) : NULL; \
	})

extern struct evl_core_namespace evl_core_ns;

#endif /* !_EVL_NAMESPACE_H */
