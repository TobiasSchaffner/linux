// SPDX-License-Identifier: GPL-2.0
/*
 * Generic userspace implementations of gettimeofday() and similar.
 */
#include <vdso/auxclock.h>
#include <vdso/datapage.h>
#include <vdso/helpers.h>

/* Bring in default accessors */
#include <vdso/vsyscall.h>

static bool do_hres_timens(const struct vdso_time_data *vdns,
			const struct vdso_clock *vcns,
			clockid_t clk, struct __kernel_timespec *ts);

#ifndef vdso_clocksource_ok
static inline bool vdso_clocksource_ok(const struct vdso_clock *vc)
{
	return vc->clock_mode != VDSO_CLOCKMODE_NONE;
}
#endif

#ifndef vdso_cycles_ok
static inline bool vdso_cycles_ok(u64 cycles)
{
	return true;
}
#endif

#ifndef vdso_calc_ns
static __always_inline u64 vdso_calc_ns(const struct vdso_clock *vc, u64 cycles,
					u64 base);
#endif

static __always_inline
bool vdso_get_timestamp(const struct vdso_time_data *vd, const struct vdso_clock *vc,
			unsigned int clkidx, u64 *sec, u64 *ns);

#if defined(CONFIG_GENERIC_VDSO_CLOCKSOURCE) && (!defined(CONFIG_COMPAT_VDSO) || !defined(BUILD_VDSO32))

#include <linux/fcntl.h>
#include <linux/io.h>
#include <linux/ioctl.h>
#include <uapi/linux/clocksource.h>

static __always_inline notrace u64 readl_mmio_up(const struct clksrc_info *vinfo)
{
	const struct clksrc_user_mmio_info *info = &vinfo->mmio;
	return readl_relaxed(info->reg_lower);
}

static __always_inline notrace u64 readl_mmio_down(const struct clksrc_info *vinfo)
{
	const struct clksrc_user_mmio_info *info = &vinfo->mmio;
	return ~(u64)readl_relaxed(info->reg_lower) & info->mask_lower;
}

static __always_inline notrace u64 readw_mmio_up(const struct clksrc_info *vinfo)
{
	const struct clksrc_user_mmio_info *info = &vinfo->mmio;
	return readw_relaxed(info->reg_lower);
}

static __always_inline notrace u64 readw_mmio_down(const struct clksrc_info *vinfo)
{
	const struct clksrc_user_mmio_info *info = &vinfo->mmio;
	return ~(u64)readl_relaxed(info->reg_lower) & info->mask_lower;
}

static __always_inline notrace u64 readl_dmmio_up(const struct clksrc_info *vinfo)
{
	const struct clksrc_user_mmio_info *info = &vinfo->mmio;
	void __iomem *reg_lower, *reg_upper;
	u32 upper, old_upper, lower;

	reg_lower = info->reg_lower;
	reg_upper = info->reg_upper;

	upper = readl_relaxed(reg_upper);
	do {
		old_upper = upper;
		lower = readl_relaxed(reg_lower);
		upper = readl_relaxed(reg_upper);
	} while (upper != old_upper);

	return (((u64)upper) << info->bits_lower) | lower;
}

static __always_inline notrace u64 readw_dmmio_up(const struct clksrc_info *vinfo)
{
	const struct clksrc_user_mmio_info *info = &vinfo->mmio;
	void __iomem *reg_lower, *reg_upper;
	u16 upper, old_upper, lower;

	reg_lower = info->reg_lower;
	reg_upper = info->reg_upper;

	upper = readw_relaxed(reg_upper);
	do {
		old_upper = upper;
		lower = readw_relaxed(reg_lower);
		upper = readw_relaxed(reg_upper);
	} while (upper != old_upper);

	return (((u64)upper) << info->bits_lower) | lower;
}

static __always_inline u16 to_cs_type(u32 cs_type_seq)
{
	return cs_type_seq >> 16;
}

static __always_inline u16 to_seq(u32 cs_type_seq)
{
	return cs_type_seq;
}

static __always_inline u32 to_cs_type_seq(u16 type, u16 seq)
{
	return (u32)type << 16U | seq;
}

static notrace noinline __cold
void map_clocksource(const struct vdso_clock *vc,
		const struct vdso_time_data *vd, struct vdso_priv_cs *vpcs,
		u32 seq, u32 new_cs_type_seq)
{
	vdso_read_cycles_t *read_cycles = NULL;
	u32 new_cs_seq, new_cs_type;
	struct clksrc_info *info;
	int fd, ret;

	new_cs_seq = to_seq(new_cs_type_seq);
	new_cs_type = to_cs_type(new_cs_type_seq);
	info = &vpcs->clksrc_info[new_cs_type];

	if (new_cs_type < CLOCKSOURCE_VDSO_MMIO)
		goto done;

	fd = clock_open_device(vc->cs_mmdev, O_RDONLY);
	if (fd < 0)
		goto fallback_to_syscall;

	ret = clock_ioctl_device(fd, CLKSRC_USER_MMIO_MAP, (long)&info->mmio);
	clock_close_device(fd);
	if (ret < 0)
		goto fallback_to_syscall;

	switch (info->mmio.type) {
	case CLKSRC_MMIO_L_UP:
		read_cycles = &readl_mmio_up;
		break;
	case CLKSRC_MMIO_L_DOWN:
		read_cycles = &readl_mmio_down;
		break;
	case CLKSRC_MMIO_W_UP:
		read_cycles = &readw_mmio_up;
		break;
	case CLKSRC_MMIO_W_DOWN:
		read_cycles = &readw_mmio_down;
		break;
	case CLKSRC_DMMIO_L_UP:
		read_cycles = &readl_dmmio_up;
		break;
	case CLKSRC_DMMIO_W_UP:
		read_cycles = &readw_dmmio_up;
		break;
	default:
		/* Mmhf, misconfigured. */
		goto fallback_to_syscall;
	}
done:
	info->read_cycles = read_cycles;
	smp_wmb();
	new_cs_type_seq = to_cs_type_seq(new_cs_type, new_cs_seq);
	WRITE_ONCE(vpcs->current_cs_type_seq, new_cs_type_seq);

	return;

fallback_to_syscall:
	new_cs_type = CLOCKSOURCE_VDSO_NONE;
	info = &vpcs->clksrc_info[new_cs_type];
	goto done;
}

static __always_inline bool get_hw_counter(const struct vdso_time_data *vd,
					   const struct vdso_clock *vc,
					   unsigned int clkidx, u64 *sec,
					   u64 *ns)
{
	const struct vdso_timestamp *vdso_ts = &vc->basetime[clkidx];
	const struct clksrc_info *info;
	struct vdso_priv_cs *vpcs;
	u32 seq, cs_type_seq;
	unsigned int cs;
	u64 cycles;

	vpcs = &__arch_get_vdso_u_priv_data()->cs_bases[
		clkidx == CLOCK_MONOTONIC_RAW ? CS_RAW : CS_HRES_COARSE];

	for (;;) {
		seq = vdso_read_begin(vc);
		cs_type_seq = READ_ONCE(vpcs->current_cs_type_seq);
		if (likely(to_seq(cs_type_seq) == to_seq(vc->cs_type_seq)))
			break;

		map_clocksource(vc, vd, vpcs, seq, vc->cs_type_seq);
	}

	switch (to_cs_type(cs_type_seq)) {
	case CLOCKSOURCE_VDSO_NONE:
		return false; /* Use fallback. */
	case CLOCKSOURCE_VDSO_ARCHITECTED:
		return vdso_get_timestamp(vd, vc, clkidx, sec, ns);
		break;
	default:
		cs = to_cs_type(READ_ONCE(cs_type_seq));
		info = &vpcs->clksrc_info[cs];
		cycles = info->read_cycles(info);
		*ns = vdso_calc_ns(vc, cycles, vdso_ts->nsec);
		*sec = vdso_ts->sec;
		break;
	}

	return true;
}

#else

static __always_inline bool get_hw_counter(const struct vdso_time_data *vd,
					   const struct vdso_clock *vc,
					   unsigned int clkidx, u64 *sec,
					   u64 *ns)
{
	return vdso_get_timestamp(vd, vc, clkidx, sec, ns);
}

#endif /* CONFIG_GENERIC_VDSO_CLOCKSOURCE */

#ifndef vdso_calc_ns

#ifdef VDSO_DELTA_NOMASK
# define VDSO_DELTA_MASK(vd)	ULLONG_MAX
#else
# define VDSO_DELTA_MASK(vd)	(vd->mask)
#endif

#ifdef CONFIG_GENERIC_VDSO_OVERFLOW_PROTECT
static __always_inline bool vdso_delta_ok(const struct vdso_clock *vc, u64 delta)
{
	return delta < vc->max_cycles;
}
#else
static __always_inline bool vdso_delta_ok(const struct vdso_clock *vc, u64 delta)
{
	return true;
}
#endif

#ifndef vdso_shift_ns
static __always_inline u64 vdso_shift_ns(u64 ns, u32 shift)
{
	return ns >> shift;
}
#endif

/*
 * Default implementation which works for all sane clocksources. That
 * obviously excludes x86/TSC.
 */
static __always_inline u64 vdso_calc_ns(const struct vdso_clock *vc, u64 cycles, u64 base)
{
	u64 delta = (cycles - vc->cycle_last) & VDSO_DELTA_MASK(vc);

	if (likely(vdso_delta_ok(vc, delta)))
		return vdso_shift_ns((delta * vc->mult) + base, vc->shift);

	return mul_u64_u32_add_u64_shr(delta, vc->mult, base, vc->shift);
}
#endif /* vdso_calc_ns */

#ifndef __arch_vdso_hres_capable
static inline bool __arch_vdso_hres_capable(void)
{
	return true;
}
#endif

static __always_inline bool vdso_clockid_valid(clockid_t clock)
{
	/* Check for negative values or invalid clocks */
	return likely((u32) clock <= CLOCK_AUX_LAST);
}

/*
 * Must not be invoked within the sequence read section as a race inside
 * that loop could result in __iter_div_u64_rem() being extremely slow.
 */
static __always_inline void vdso_set_timespec(struct __kernel_timespec *ts, u64 sec, u64 ns)
{
	ts->tv_sec = sec + __iter_div_u64_rem(ns, NSEC_PER_SEC, &ns);
	ts->tv_nsec = ns;
}

static __always_inline
bool vdso_get_timestamp(const struct vdso_time_data *vd, const struct vdso_clock *vc,
			unsigned int clkidx, u64 *sec, u64 *ns)
{
	const struct vdso_timestamp *vdso_ts = &vc->basetime[clkidx];
	u64 cycles;

	if (unlikely(!vdso_clocksource_ok(vc)))
		return false;

	cycles = __arch_get_hw_counter(vc->clock_mode, vd);
	if (unlikely(!vdso_cycles_ok(cycles)))
		return false;

	*ns = vdso_calc_ns(vc, cycles, vdso_ts->nsec);
	*sec = vdso_ts->sec;

	return true;
}

static __always_inline
const struct vdso_time_data *__arch_get_vdso_u_timens_data(const struct vdso_time_data *vd)
{
	return (void *)vd + PAGE_SIZE;
}

static __always_inline
bool do_hres_timens(const struct vdso_time_data *vdns, const struct vdso_clock *vcns,
		    clockid_t clk, struct __kernel_timespec *ts)
{
	const struct vdso_time_data *vd = __arch_get_vdso_u_timens_data(vdns);
	const struct timens_offset *offs = &vcns->offset[clk];
	const struct vdso_clock *vc = vd->clock_data;
	u32 seq;
	s64 sec;
	u64 ns;

	if (clk != CLOCK_MONOTONIC_RAW)
		vc = &vc[CS_HRES_COARSE];
	else
		vc = &vc[CS_RAW];

	do {
		seq = vdso_read_begin(vc);

		if (!get_hw_counter(vd, vc, clk, &sec, &ns))
			return false;
	} while (unlikely(vdso_read_retry(vc, seq)));

	/* Add the namespace offset */
	sec += offs->sec;
	ns += offs->nsec;

	vdso_set_timespec(ts, sec, ns);

	return true;
}

static __always_inline
bool do_hres(const struct vdso_time_data *vd, const struct vdso_clock *vc,
	     clockid_t clk, struct __kernel_timespec *ts)
{
	u64 sec, ns;
	u32 seq;

	/* Allows to compile the high resolution parts out */
	if (!__arch_vdso_hres_capable())
		return false;

	do {
		/*
		 * Open coded function vdso_read_begin() to handle
		 * VDSO_CLOCKMODE_TIMENS. Time namespace enabled tasks have a
		 * special VVAR page installed which has vc->seq set to 1 and
		 * vc->clock_mode set to VDSO_CLOCKMODE_TIMENS. For non time
		 * namespace affected tasks this does not affect performance
		 * because if vc->seq is odd, i.e. a concurrent update is in
		 * progress the extra check for vc->clock_mode is just a few
		 * extra instructions while spin waiting for vc->seq to become
		 * even again.
		 */
		while (unlikely((seq = READ_ONCE(vc->seq)) & 1)) {
			if (IS_ENABLED(CONFIG_TIME_NS) &&
			    vc->clock_mode == VDSO_CLOCKMODE_TIMENS)
				return do_hres_timens(vd, vc, clk, ts);
			cpu_relax();
		}
		smp_rmb();

		if (!get_hw_counter(vd, vc, clk, &sec, &ns))
			return false;
	} while (unlikely(vdso_read_retry(vc, seq)));

	vdso_set_timespec(ts, sec, ns);

	return true;
}

static __always_inline
bool do_coarse_timens(const struct vdso_time_data *vdns, const struct vdso_clock *vcns,
		      clockid_t clk, struct __kernel_timespec *ts)
{
	const struct vdso_time_data *vd = __arch_get_vdso_u_timens_data(vdns);
	const struct timens_offset *offs = &vcns->offset[clk];
	const struct vdso_clock *vc = vd->clock_data;
	const struct vdso_timestamp *vdso_ts;
	u64 nsec;
	s64 sec;
	s32 seq;

	vdso_ts = &vc->basetime[clk];

	do {
		seq = vdso_read_begin(vc);
		sec = vdso_ts->sec;
		nsec = vdso_ts->nsec;
	} while (unlikely(vdso_read_retry(vc, seq)));

	/* Add the namespace offset */
	sec += offs->sec;
	nsec += offs->nsec;

	vdso_set_timespec(ts, sec, nsec);

	return true;
}

static __always_inline
bool do_coarse(const struct vdso_time_data *vd, const struct vdso_clock *vc,
	       clockid_t clk, struct __kernel_timespec *ts)
{
	const struct vdso_timestamp *vdso_ts = &vc->basetime[clk];
	u32 seq;

	do {
		/*
		 * Open coded function vdso_read_begin() to handle
		 * VDSO_CLOCK_TIMENS. See comment in do_hres().
		 */
		while ((seq = READ_ONCE(vc->seq)) & 1) {
			if (IS_ENABLED(CONFIG_TIME_NS) &&
			    vc->clock_mode == VDSO_CLOCKMODE_TIMENS)
				return do_coarse_timens(vd, vc, clk, ts);
			cpu_relax();
		}
		smp_rmb();

		ts->tv_sec = vdso_ts->sec;
		ts->tv_nsec = vdso_ts->nsec;
	} while (unlikely(vdso_read_retry(vc, seq)));

	return true;
}

static __always_inline
bool do_aux(const struct vdso_time_data *vd, clockid_t clock, struct __kernel_timespec *ts)
{
	const struct vdso_clock *vc;
	u32 seq, idx;
	u64 sec, ns;

	if (!IS_ENABLED(CONFIG_POSIX_AUX_CLOCKS))
		return false;

	idx = clock - CLOCK_AUX;
	vc = &vd->aux_clock_data[idx];

	do {
		/*
		 * Open coded function vdso_read_begin() to handle
		 * VDSO_CLOCK_TIMENS. See comment in do_hres().
		 */
		while ((seq = READ_ONCE(vc->seq)) & 1) {
			if (IS_ENABLED(CONFIG_TIME_NS) && vc->clock_mode == VDSO_CLOCKMODE_TIMENS) {
				vd = __arch_get_vdso_u_timens_data(vd);
				vc = &vd->aux_clock_data[idx];
				/* Re-read from the real time data page */
				continue;
			}
			cpu_relax();
		}
		smp_rmb();

		/* Auxclock disabled? */
		if (vc->clock_mode == VDSO_CLOCKMODE_NONE)
			return false;

		if (!vdso_get_timestamp(vd, vc, VDSO_BASE_AUX, &sec, &ns))
			return false;
	} while (unlikely(vdso_read_retry(vc, seq)));

	vdso_set_timespec(ts, sec, ns);

	return true;
}

static __always_inline bool
__cvdso_clock_gettime_common(const struct vdso_time_data *vd, clockid_t clock,
			     struct __kernel_timespec *ts)
{
	const struct vdso_clock *vc = vd->clock_data;
	u32 msk;

	if (!vdso_clockid_valid(clock))
		return false;

	/*
	 * Convert the clockid to a bitmask and use it to check which
	 * clocks are handled in the VDSO directly.
	 */
	msk = 1U << clock;
	if (likely(msk & VDSO_HRES))
		vc = &vc[CS_HRES_COARSE];
	else if (msk & VDSO_COARSE)
		return do_coarse(vd, &vc[CS_HRES_COARSE], clock, ts);
	else if (msk & VDSO_RAW)
		vc = &vc[CS_RAW];
	else if (msk & VDSO_AUX)
		return do_aux(vd, clock, ts);
	else
		return false;

	return do_hres(vd, vc, clock, ts);
}

static __maybe_unused int
__cvdso_clock_gettime_data(const struct vdso_time_data *vd, clockid_t clock,
			   struct __kernel_timespec *ts)
{
	bool ok;

	ok = __cvdso_clock_gettime_common(vd, clock, ts);

	if (unlikely(!ok))
		return clock_gettime_fallback(clock, ts);
	return 0;
}

static __maybe_unused int
__cvdso_clock_gettime(clockid_t clock, struct __kernel_timespec *ts)
{
	return __cvdso_clock_gettime_data(__arch_get_vdso_u_time_data(), clock, ts);
}

#ifdef BUILD_VDSO32
static __maybe_unused int
__cvdso_clock_gettime32_data(const struct vdso_time_data *vd, clockid_t clock,
			     struct old_timespec32 *res)
{
	struct __kernel_timespec ts;
	bool ok;

	ok = __cvdso_clock_gettime_common(vd, clock, &ts);

	if (unlikely(!ok))
		return clock_gettime32_fallback(clock, res);

	/* For ok == true */
	res->tv_sec = ts.tv_sec;
	res->tv_nsec = ts.tv_nsec;

	return 0;
}

static __maybe_unused int
__cvdso_clock_gettime32(clockid_t clock, struct old_timespec32 *res)
{
	return __cvdso_clock_gettime32_data(__arch_get_vdso_u_time_data(), clock, res);
}
#endif /* BUILD_VDSO32 */

static __maybe_unused int
__cvdso_gettimeofday_data(const struct vdso_time_data *vd,
			  struct __kernel_old_timeval *tv, struct timezone *tz)
{
	const struct vdso_clock *vc = vd->clock_data;

	if (likely(tv != NULL)) {
		struct __kernel_timespec ts;

		if (!do_hres(vd, &vc[CS_HRES_COARSE], CLOCK_REALTIME, &ts))
			return gettimeofday_fallback(tv, tz);

		tv->tv_sec = ts.tv_sec;
		tv->tv_usec = (u32)ts.tv_nsec / NSEC_PER_USEC;
	}

	if (unlikely(tz != NULL)) {
		if (IS_ENABLED(CONFIG_TIME_NS) &&
		    vc->clock_mode == VDSO_CLOCKMODE_TIMENS)
			vd = __arch_get_vdso_u_timens_data(vd);

		tz->tz_minuteswest = vd[CS_HRES_COARSE].tz_minuteswest;
		tz->tz_dsttime = vd[CS_HRES_COARSE].tz_dsttime;
	}

	return 0;
}

static __maybe_unused int
__cvdso_gettimeofday(struct __kernel_old_timeval *tv, struct timezone *tz)
{
	return __cvdso_gettimeofday_data(__arch_get_vdso_u_time_data(), tv, tz);
}

#ifdef VDSO_HAS_TIME
static __maybe_unused __kernel_old_time_t
__cvdso_time_data(const struct vdso_time_data *vd, __kernel_old_time_t *time)
{
	const struct vdso_clock *vc = vd->clock_data;
	__kernel_old_time_t t;

	if (IS_ENABLED(CONFIG_TIME_NS) &&
	    vc->clock_mode == VDSO_CLOCKMODE_TIMENS) {
		vd = __arch_get_vdso_u_timens_data(vd);
		vc = vd->clock_data;
	}

	t = READ_ONCE(vc[CS_HRES_COARSE].basetime[CLOCK_REALTIME].sec);

	if (time)
		*time = t;

	return t;
}

static __maybe_unused __kernel_old_time_t __cvdso_time(__kernel_old_time_t *time)
{
	return __cvdso_time_data(__arch_get_vdso_u_time_data(), time);
}
#endif /* VDSO_HAS_TIME */

#ifdef VDSO_HAS_CLOCK_GETRES
static __always_inline
bool __cvdso_clock_getres_common(const struct vdso_time_data *vd, clockid_t clock,
				 struct __kernel_timespec *res)
{
	const struct vdso_clock *vc = vd->clock_data;
	u32 msk;
	u64 ns;

	if (!vdso_clockid_valid(clock))
		return false;

	if (IS_ENABLED(CONFIG_TIME_NS) &&
	    vc->clock_mode == VDSO_CLOCKMODE_TIMENS)
		vd = __arch_get_vdso_u_timens_data(vd);

	/*
	 * Convert the clockid to a bitmask and use it to check which
	 * clocks are handled in the VDSO directly.
	 */
	msk = 1U << clock;
	if (msk & (VDSO_HRES | VDSO_RAW)) {
		/*
		 * Preserves the behaviour of posix_get_hrtimer_res().
		 */
		ns = READ_ONCE(vd->hrtimer_res);
	} else if (msk & VDSO_COARSE) {
		/*
		 * Preserves the behaviour of posix_get_coarse_res().
		 */
		ns = LOW_RES_NSEC;
	} else if (msk & VDSO_AUX) {
		ns = aux_clock_resolution_ns();
	} else {
		return false;
	}

	if (likely(res)) {
		res->tv_sec = 0;
		res->tv_nsec = ns;
	}
	return true;
}

static __maybe_unused
int __cvdso_clock_getres_data(const struct vdso_time_data *vd, clockid_t clock,
			      struct __kernel_timespec *res)
{
	bool ok;

	ok =  __cvdso_clock_getres_common(vd, clock, res);

	if (unlikely(!ok))
		return clock_getres_fallback(clock, res);
	return 0;
}

static __maybe_unused
int __cvdso_clock_getres(clockid_t clock, struct __kernel_timespec *res)
{
	return __cvdso_clock_getres_data(__arch_get_vdso_u_time_data(), clock, res);
}

#ifdef BUILD_VDSO32
static __maybe_unused int
__cvdso_clock_getres_time32_data(const struct vdso_time_data *vd, clockid_t clock,
				 struct old_timespec32 *res)
{
	struct __kernel_timespec ts;
	bool ok;

	ok = __cvdso_clock_getres_common(vd, clock, &ts);

	if (unlikely(!ok))
		return clock_getres32_fallback(clock, res);

	if (likely(res)) {
		res->tv_sec = ts.tv_sec;
		res->tv_nsec = ts.tv_nsec;
	}
	return 0;
}

static __maybe_unused int
__cvdso_clock_getres_time32(clockid_t clock, struct old_timespec32 *res)
{
	return __cvdso_clock_getres_time32_data(__arch_get_vdso_u_time_data(),
						clock, res);
}
#endif /* BUILD_VDSO32 */
#endif /* VDSO_HAS_CLOCK_GETRES */
