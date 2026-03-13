/*
 * SPDX-License-Identifier: GPL-2.0
 *
 * Derived from Xenomai Cobalt, https://xenomai.org/
 * Copyright (C) 2005-2020 Philippe Gerum  <rpm@xenomai.org>
 * Copyright (C) 2005 Gilles Chanteperdrix  <gilles.chanteperdrix@xenomai.org>
 */

#include <linux/types.h>
#include <linux/err.h>
#include <linux/kernel.h>
#include <linux/unistd.h>
#include <linux/sched.h>
#include <linux/dovetail.h>
#include <linux/kconfig.h>
#include <linux/nospec.h>
#include <linux/atomic.h>
#include <linux/prctl.h>
#include <linux/sched/task_stack.h>
#include <linux/sched/signal.h>
#include <evl/thread.h>
#include <evl/monitor.h>
#include <evl/sched.h>
#include <evl/factory.h>
#include <asm/syscall.h>
#include <uapi/evl/syscall-abi.h>
#include <asm/evl/syscall.h>
#include <trace/events/evl.h>

#define __EVL_SYSCALL(__name, __args)	long EVL_ ## __name __args
#define __EVL_SYSCALL_NAME(__name)	[sys_evl_ ## __name] = #__name

#define __EVL_SYSCALL_PROPAGATE   0
#define __EVL_SYSCALL_STOP        1

static const char *evl_sysnames[] = {
	   __EVL_SYSCALL_NAME(read),
	   __EVL_SYSCALL_NAME(write),
	   __EVL_SYSCALL_NAME(ioctl),
	   __EVL_SYSCALL_NAME(functl),
};

static __EVL_SYSCALL(read, (int fd, char __user *u_buf, size_t size))
{
	struct evl_file *efilp = evl_get_file(fd);
	struct file *filp;
	ssize_t ret;

	if (efilp == NULL)
		return -EBADF;

	filp = efilp->filp;
	if (!(filp->f_mode & FMODE_READ)) {
		ret = -EBADF;
		goto out;
	}

	if (filp->f_op->oob_read == NULL) {
		ret = -EINVAL;
		goto out;
	}

	ret = filp->f_op->oob_read(filp, u_buf, size);
out:
	evl_put_file(efilp);

	return ret;
}

static __EVL_SYSCALL(write, (int fd, const char __user *u_buf, size_t size))
{
	struct evl_file *efilp = evl_get_file(fd);
	struct file *filp;
	ssize_t ret;

	if (efilp == NULL)
		return -EBADF;

	filp = efilp->filp;
	if (!(filp->f_mode & FMODE_WRITE)) {
		ret = -EBADF;
		goto out;
	}

	if (filp->f_op->oob_write == NULL) {
		ret = -EINVAL;
		goto out;
	}

	ret = filp->f_op->oob_write(filp, u_buf, size);
out:
	evl_put_file(efilp);

	return ret;
}

static __EVL_SYSCALL(ioctl, (int fd, unsigned int request, unsigned long arg))
{
	struct evl_file *efilp = evl_get_file(fd);
	long ret = -ENOTTY;
	struct file *filp;

	if (efilp == NULL)
		return -EBADF;

	filp = efilp->filp;

	if (unlikely(is_compat_oob_call())) {
		if (filp->f_op->compat_oob_ioctl)
			ret = filp->f_op->compat_oob_ioctl(filp, request, arg);
	} else  if (filp->f_op->oob_ioctl) {
		ret = filp->f_op->oob_ioctl(filp, request, arg);
	}

	if (ret == -ENOIOCTLCMD)
		ret = -ENOTTY;

	evl_put_file(efilp);

	return ret;
}

static __EVL_SYSCALL(functl, (fundle_t fundle, unsigned int request, unsigned long arg))
{
	struct evl_thread *thread;
	struct evl_monitor *mon;
	long ret = -EINVAL;

	switch (__evl_fundle_type(fundle)) {
	case __evl_type_thread:
		thread = evl_lookup_ns(&evl_core_ns, fundle, thread);
		if (likely(thread)) {
			ret = evl_functl_thread(thread, request, arg);
			evl_put_element(&thread->element);
		}
		break;
	case __evl_type_monitor:
		mon = evl_lookup_ns(&evl_core_ns, fundle, monitor);
		if (likely(mon)) {
			ret = evl_functl_monitor(mon, request, arg);
			evl_put_element(&mon->element);
		}
		break;
	}

	return ret;
}

static __always_inline
void invoke_syscall(unsigned int nr, struct pt_regs *regs,
		unsigned long *args)
{
	int error;
	long ret;

	/*
	 * We have only very few syscalls, prefer a plain switch to a
	 * pointer indirection which ends up being fairly costly due
	 * to exploit mitigations.
	 */
	switch (nr) {
	case sys_evl_read:
		ret = EVL_read((int)args[0],
			(char __user *)args[1],
			(size_t)args[2]);
		break;
	case sys_evl_write:
		ret = EVL_write((int)args[0],
				(const char __user *)args[1],
				(size_t)args[2]);
		break;
	case sys_evl_ioctl:
		ret = EVL_ioctl((int)args[0],
				(unsigned int)args[1],
				args[2]);
		break;
	case sys_evl_functl:
	case sys_evl_ifunctl:
		ret = EVL_functl((int)args[0],
				(unsigned int)args[1],
				args[2]);
		break;
	default:
		BUG();
	}

	error = IS_ERR_VALUE(ret) ? ret : 0;
	syscall_set_return_value(current, regs, error, ret);
}

/*
 * Intercepting __NR_clock_gettime (or __NR_clock_gettime64 on 32bit
 * archs) here means that we are handling a fallback syscall for
 * clock_gettime*() from the vDSO, which failed performing a direct
 * access to the clocksource.  Such fallback would involve a switch to
 * in-band mode unless we provide the service directly from here,
 * which is not optimal but still correct.
 */
static bool handle_vdso_fallback(struct pt_regs *regs, unsigned int nr,
				unsigned long *args)
{
	struct __kernel_old_timespec __user *u_old_ts;
	struct __kernel_timespec uts, __user *u_uts;
	struct __kernel_old_timespec old_ts;
	int clock_id, ret = 0, error;
	struct evl_clock *clock;
	struct timespec64 ts64;

#define is_clock_gettime(__nr) ((__nr) == __NR_clock_gettime)
#ifndef __NR_clock_gettime64
#define is_clock_gettime64(__nr)  0
#else
#define is_clock_gettime64(__nr) ((__nr) == __NR_clock_gettime64)
#endif

	if (!is_clock_gettime(nr) && !is_clock_gettime64(nr))
		return false;

	clock_id = (int)args[0];
	switch (clock_id) {
	case CLOCK_MONOTONIC:
		clock = &evl_mono_clock;
		break;
	case CLOCK_REALTIME:
		clock = &evl_realtime_clock;
		break;
	default:
		return false;
	}

	ts64 = ktime_to_timespec64(evl_read_clock(clock));

	if (is_clock_gettime(nr)) {
		old_ts.tv_sec = (__kernel_old_time_t)ts64.tv_sec;
		old_ts.tv_nsec = ts64.tv_nsec;
		u_old_ts = (struct __kernel_old_timespec __user *)args[1];
		if (raw_copy_to_user(u_old_ts, &old_ts, sizeof(old_ts)))
			ret = -EFAULT;
	} else if (is_clock_gettime64(nr)) {
		uts.tv_sec = ts64.tv_sec;
		uts.tv_nsec = ts64.tv_nsec;
		u_uts = (struct __kernel_timespec __user *)args[1];
		if (raw_copy_to_user(u_uts, &uts, sizeof(uts)))
			ret = -EFAULT;
	}

	error = IS_ERR_VALUE((long)ret) ? ret : 0;
	syscall_set_return_value(current, regs, error, ret);

#undef is_clock_gettime
#undef is_clock_gettime64

	return true;
}

static int do_oob_syscall(struct irq_stage *stage, struct pt_regs *regs,
			unsigned int scno, unsigned long *args, bool is_evlsc)
{
	struct task_struct *tsk = current;
	struct evl_thread *curr;

	if (!is_evlsc)
		goto do_inband;

	if (scno >= NR_EVL_SYSCALLS) {
		printk(EVL_WARNING "invalid out-of-band syscall <%#x>\n", scno);
		goto bad_syscall;
	}

	curr = evl_current();
	if (curr == NULL || !cap_raised(current_cap(), CAP_SYS_NICE)) {
		if (EVL_DEBUG(CORE))
			printk(EVL_WARNING
				"syscall <oob_%s> denied to %s[%d]\n",
				evl_sysnames[scno], tsk->comm, task_pid_nr(tsk));
		syscall_set_return_value(tsk, regs, -EPERM, 0);
		return __EVL_SYSCALL_STOP;
	}

	/*
	 * If the syscall originates from the in-band stage, we need
	 * to take the long route. Tell Dovetail to hand it over to
	 * the next stage down the pipeline (i.e. in-band).
	 */
	if (stage != &oob_stage)
		return __EVL_SYSCALL_PROPAGATE;

	trace_evl_oob_sysentry(scno);

	invoke_syscall(scno, regs, args);

	/*
	 * The syscall might have (already) switched in-band, recheck
	 * before determining if we need to demote.
	 */
	if (unlikely(evl_is_inband()))
		goto do_stop;

	/*
	 * Epilogue: we might have to demote the caller to the in-band
	 * stage, if any of the following conditions is true:
	 *
	 * - __evl_wait_schedule() woke up on a (in-band) signal
	 *   receipt while the syscall was waiting out-of-band for
	 *   some event to happen. In such a case, the syscall handler
	 *   should have returned -ERESTARTSYS, as received from
	 *   evl_wait_schedule().
	 *
	 * - evl_kick_thread() was called for current in order to
	 *   forcibly demote it (e.g. mayday trap). This also covers a
	 *   signal receipt.
	 *
	 * - the caller is undergoing the SCHED_WEAK policy, which
	 *   means that we have to switch it back to the in-band stage
	 *   on the syscall return path.
	 */
	evl_exit_to_user();
do_stop:
	/* Update the stats and user visible info. */
	evl_opt_counter_inc(&curr->stat.sc);
	evl_sync_sstate(curr);

	trace_evl_oob_sysexit(syscall_get_return_value(tsk, regs));

	return __EVL_SYSCALL_STOP;

do_inband:
	if (evl_is_inband())
		return __EVL_SYSCALL_PROPAGATE;

	/*
	 * We don't want to trigger a stage switch whenever the
	 * current request issued from the out-of-band stage is not a
	 * valid in-band syscall, but rather deliver -ENOSYS directly
	 * instead.  Otherwise, switch to in-band mode before
	 * propagating the syscall down the pipeline.
	 */
	if (is_valid_inband_syscall(scno)) {
		if (handle_vdso_fallback(regs, scno, args))
			return __EVL_SYSCALL_STOP;
		evl_switch_inband(EVL_HMDIAG_SYSDEMOTE);
		return __EVL_SYSCALL_PROPAGATE;
	}

	printk(EVL_WARNING "invalid in-band syscall <%u>\n", scno);

bad_syscall:
	syscall_set_return_value(tsk, regs, -ENOSYS, 0);

	return __EVL_SYSCALL_STOP;
}

static inline bool force_inband_call(bool is_evlsc, unsigned int scno)
{
	return is_evlsc && scno == sys_evl_ifunctl;
}

static int do_inband_syscall(struct pt_regs *regs, unsigned int scno,
			unsigned long *args,
			bool is_evlsc)
{
	struct evl_thread *curr = evl_current();
	struct task_struct *tsk = current;
	int ret;

	if (likely(curr)) {
		/*
		 * Catch cancellation requests pending for EVL threads
		 * undergoing the weak scheduling policy which issue
		 * in-band syscalls. Those are less likely to cross
		 * evl_exit_to_user() as they should run in-band most
		 * of the time.
		 */
		evl_test_cancel();

		/* Handle pending lazy schedparam updates. */
		evl_propagate_schedparam_change(curr);
	}

	/* Propagate in-band syscalls. */
	if (!is_evlsc)
		return __EVL_SYSCALL_PROPAGATE;

	trace_evl_inband_sysentry(scno);

	/*
	 * At this point, we need to switch EVL threads to the
	 * out-of-band stage for handling out-of-band EVL syscalls.
	 *
	 * NOTE: -ERESTARTSYS might be received if switching oob was
	 * blocked by a pending signal, otherwise -EINTR might be
	 * received upon signal detection after the transition to oob
	 * context, in which case the common logic applies (i.e. based
	 * on EVL_T_KICKED and/or signal_pending()).
	 */
	if (likely(!force_inband_call(is_evlsc, scno))) {
		ret = evl_switch_oob();
		if (ret == -ERESTARTSYS) {
			syscall_set_return_value(tsk, regs, ret, 0);
			goto done;
		}
	}

	invoke_syscall(scno, regs, args);

	if (unlikely(!curr))
		goto out;

	if (!evl_is_inband())
		evl_exit_to_user();
done:
	if (curr->local_info & EVL_T_IGNOVR)
		curr->local_info &= ~EVL_T_IGNOVR;

	evl_opt_counter_inc(&curr->stat.sc);
	evl_sync_sstate(curr);
out:
	trace_evl_inband_sysexit(syscall_get_return_value(tsk, regs));

	return __EVL_SYSCALL_STOP;
}

static bool collect_syscall_args(struct pt_regs *regs,
				unsigned long *args,
				unsigned int *scno)
{
	struct task_struct *tsk = current;

	/*
	 * We'll need the arguments later on for handling either of
	 * inband or evl syscalls.
	 */
	syscall_get_arguments(tsk, regs, args);

	if (!in_oob_syscall(regs)) {
		*scno = syscall_get_nr(tsk, regs);
		return false;
	}

	/*
	 * Since ABI 36, we recognize EVL requests only when folded
	 * into a prctl() call, such as prctl(PR_OOB_SYSCALL, @nr,
	 * args...). If so, fetch the EVL syscall number then shift
	 * the arguments left to skip it (3 arguments max).
	 * Otherwise, assume this is an in-band syscall, so leave the
	 * argument vector unchanged.
	 */
	*scno = args[1];
	args[0] = args[2];
	args[1] = args[3];
	args[2] = args[4];

	return true;
}

/*
 * When legacy syscall support is enabled, Dovetail might be confused
 * by architectures using special in-band syscall numbers which have
 * the __OOB_SYSCALL_BIT set, e.g. some may be issued in aarch32 over
 * aarch64 compat mode. Ideally, CONFIG_DOVETAIL_LEGACY_SYSCALL_RANGE
 * should be off when EVL is enabled since we don't support the legacy
 * call form. Unfortunately, Dovetail still enables this compat
 * feature by default, which might cause our in-band handler to
 * receive non-EVL syscalls advertised as EVL ones. The following
 * routine detects such situation and propagates the misrouted syscall
 * downstream to the common in-band handler.
 */
static inline
bool detect_misrouted_syscall(struct pt_regs *regs)
{
	unsigned int nr = syscall_get_nr(current, regs);

	if (IS_ENABLED(CONFIG_DOVETAIL_LEGACY_SYSCALL_RANGE) &&
		nr & __OOB_SYSCALL_BIT)
		return true;

	return false;
}

int handle_pipelined_syscall(struct irq_stage *stage, struct pt_regs *regs)
{
	unsigned long args[6] = { 0 };
	unsigned int scno;
	bool is_evlsc;

	is_evlsc = collect_syscall_args(regs, args, &scno);

	if (unlikely(running_inband())) {
		if (detect_misrouted_syscall(regs))
			return __EVL_SYSCALL_PROPAGATE;
		return do_inband_syscall(regs, scno, args, is_evlsc);
	}

	return do_oob_syscall(stage, regs, scno, args, is_evlsc);
}

int handle_oob_syscall(struct pt_regs *regs)
{
	unsigned long args[6] = { 0 };
	unsigned int scno;
	bool is_evlsc;
	int ret;

	is_evlsc = collect_syscall_args(regs, args, &scno);
	if (unlikely(force_inband_call(is_evlsc, scno))) {
		evl_switch_inband(EVL_HMDIAG_SYSDEMOTE);
		ret = do_inband_syscall(regs, scno, args, is_evlsc);
	} else {
		ret = do_oob_syscall(&oob_stage, regs, scno, args, is_evlsc);
	}

	EVL_WARN_ON(CORE, ret == __EVL_SYSCALL_PROPAGATE); /* Keep me there! */

	return ret;
}
