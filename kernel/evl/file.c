/*
 * SPDX-License-Identifier: GPL-2.0
 *
 * Copyright (C) 2018 Philippe Gerum  <rpm@xenomai.org>
 */

#include <linux/stdarg.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/file.h>
#include <linux/slab.h>
#include <linux/err.h>
#include <linux/completion.h>
#include <linux/irq_work.h>
#include <linux/spinlock.h>
#include <evl/file.h>
#include <evl/memory.h>
#include <evl/assert.h>
#include <evl/sched.h>
#include <evl/poll.h>

/*
 * evl_get_file - Get an oob-enabled file by descriptor.
 * @fd	the file descriptor
 *
 * Performs a lookup by file descriptor on the current file table. If
 * an oob-enabled file is found, a reference is taken on it before
 * returning the EVL file pointer.
 *
 * Dovetail enables fget() for oob callers, so this routine may be
 * called from any stage.
 */
struct evl_file *evl_get_file(unsigned int fd)
{
	struct evl_file *efilp;
	CLASS(fd, f)(fd);

	if (unlikely(fd_empty(f)))
		return NULL;

	efilp = fd_file(f)->f_oob_state.data;
	if (unlikely(!efilp))
		return NULL;

	evl_get_fileref(efilp);

	return efilp;
}
EXPORT_SYMBOL_GPL(evl_get_file);

/**
 * evl_open_file - Open new file with oob capabilities
 *
 * Called by chrdev with oob capabilities when a new @efilp is
 * opened. @efilp is paired with the in-band file struct at @filp.
 */
int evl_open_file(struct evl_file *efilp, struct file *filp)
{
	efilp->filp = filp;
	filp->f_oob_state.data = efilp; /* mark filp as oob-capable. */
	evl_init_crossing(&efilp->crossing);
	INIT_LIST_HEAD(&efilp->watchpoints);
	raw_spin_lock_init(&efilp->lock);

	return 0;
}
EXPORT_SYMBOL_GPL(evl_open_file);

/**
 * evl_release_file - Drop an oob-capable file
 *
 * Called by chrdev with oob capabilities when @efilp is about to be
 * released. Must be called from a fops->release() handler, and paired
 * with a previous call to evl_open_file() from the fops->open()
 * handler.
 */
void evl_release_file(struct evl_file *efilp)
{
	evl_release_watchers(efilp);

	/*
	 * Release the original reference on @efilp. If oob references
	 * are still pending (e.g. some thread is still blocked in
	 * fops->oob_read()), we must wait for them to be dropped
	 * before allowing the in-band code to dismantle @efilp->filp.
	 */
	evl_pass_crossing(&efilp->crossing);
}
EXPORT_SYMBOL_GPL(evl_release_file);
