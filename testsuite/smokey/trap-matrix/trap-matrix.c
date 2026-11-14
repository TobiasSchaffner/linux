// SPDX-License-Identifier: GPL-2.0
/*
 * Raise every kind of trap user space can cause, from a
 * thread running in primary mode, one after the other: the exceptions
 * which end in a signal, the faults and instructions the kernel fixes
 * up or emulates (some of them taking a page fault while doing so),
 * page faults which succeed, kernel faults on behalf of the thread and
 * a kprobe hit by a primary mode syscall. Each trap must deliver the
 * expected signal exactly once, with the expected si_code and si_addr,
 * or complete with the expected result and page fault count.
 *
 * This test only triggers the traps and checks what user space can
 * see. Whether the kernel notified each trap to Cobalt exactly once,
 * with the right trap number, Cobalt relaxed the thread for it, and
 * both handled it according to the Dovetail trap contract is checked
 * by the oob_trap RV monitor of the kernel (CONFIG_RV_MON_OOB_TRAP),
 * which should be enabled while this test runs.
 */

#include <sys/types.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <stdint.h>
#include <stdbool.h>
#include <signal.h>
#include <setjmp.h>
#include <error.h>
#include <fcntl.h>
#include <fenv.h>
#include <time.h>
#include <pthread.h>
#include <sys/cobalt.h>
#include <smokey/smokey.h>

smokey_test_plugin(trap_matrix,
		   SMOKEY_NOARGS,
		   "Raise every kind of trap from primary mode.\n"
		   "\tRun with the kernel's oob_trap RV monitor enabled."
);

#define ANY		-1	/* si_code, page fault count */
#define SKIP		1	/* from prepare() */

struct tcase {
	const char *name;
	int (*prepare)(void);	/* in-band, 0 or SKIP, optional */
	void (*trigger)(void);	/* primary mode */
	bool (*ok)(void);	/* result, optional */
	void (*cleanup)(void);	/* after prepare() returned 0, optional */
	int signo;		/* expected signal, 0 for none */
	int code;		/* expected si_code, or ANY */
	void **addr;		/* expected si_addr, NULL for any */
	int faults;		/* expected minor page faults, or ANY */
};

static sigjmp_buf recover;

static long page_size;

static int kret;

static volatile int nr_signals, last_signo, last_code;

static void *volatile last_addr;

/*
 * Mapped up front: mmap() is in-band and would relax us before the
 * trap. libcobalt locks and populates all memory, so the fresh page is
 * unlocked and dropped again right before use.
 */
static volatile char *ro_page, *retry_page, *bus_page, *cow_page;

static void *nx_page, *none_page, *fresh_page, *bus_addr, *null_addr;

static char buf[64] __attribute__((aligned(16)));

static uint64_t misval;

static bool misaligned_signals;

static void sanitize_fp(void)
{
	fedisableexcept(FE_ALL_EXCEPT);
	feclearexcept(FE_ALL_EXCEPT);
}

static void fault_handler(int sig, siginfo_t *si, void *uc)
{
	/* We may have entered with FP traps enabled. */
	sanitize_fp();

	nr_signals++;
	last_signo = sig;
	last_code = si->si_code;
	last_addr = si->si_addr;

	/* Fix the fault up and retry the access. */
	if (sig == SIGSEGV && si->si_addr == (void *)retry_page) {
		mprotect((void *)retry_page, page_size, PROT_READ | PROT_WRITE);
		return;
	}

	siglongjmp(recover, 1);
}

static void *map(size_t len, int prot, int flags, int fd)
{
	void *p = mmap(NULL, len, prot, flags, fd, 0);

	if (p == MAP_FAILED)
		error(1, errno, "mmap");

	return p;
}

static void *anon(int prot)
{
	return map(page_size, prot, MAP_PRIVATE | MAP_ANONYMOUS, -1);
}

/* Not resident: the next access faults. */
static int drop_fresh_page(void)
{
	if (munlock(fresh_page, page_size) ||
	    madvise(fresh_page, page_size, MADV_DONTNEED))
		error(1, errno, "munlock/madvise");

	return 0;
}

static long minor_faults(void)
{
	struct rusage ru;

	if (getrusage(RUSAGE_THREAD, &ru))
		error(1, errno, "getrusage");

	return ru.ru_minflt;
}

/*
 * Runs fn() in-band, to find out whether it traps at all on this CPU
 * or emulator. Returns the signal it raised, 0 if none.
 */
static int probe(void (*fn)(void))
{
	nr_signals = 0;
	last_signo = 0;
	if (!sigsetjmp(recover, 1))
		fn();

	return nr_signals ? last_signo : 0;
}

static int read_file(const char *path, char *s, size_t len)
{
	ssize_t n;
	int fd;

	fd = open(path, O_RDONLY);
	if (fd < 0)
		return -errno;

	n = read(fd, s, len - 1);
	close(fd);
	if (n < 0)
		return -errno;

	s[n] = '\0';

	return 0;
}

static int write_file(const char *path, const char *s)
{
	ssize_t n;
	int fd;

	fd = open(path, O_WRONLY | O_APPEND);
	if (fd < 0)
		return -errno;

	n = write(fd, s, strlen(s));
	close(fd);

	return n < 0 ? -errno : 0;
}

/* Common cases. */

static void read_null(void)
{
	(void)*(volatile int *)0;
}

static void write_ro(void)
{
	*ro_page = 1;
}

/* The handler makes the page writable, the store is retried. */
static int prepare_write_retry(void)
{
	if (mprotect((void *)retry_page, page_size, PROT_READ))
		error(1, errno, "mprotect");

	return 0;
}

static void write_retry(void)
{
	*retry_page = 0x5a;
}

static bool write_retry_ok(void)
{
	return *retry_page == 0x5a;
}

static void exec_nx(void)
{
	void (*f)(void);

	memcpy(&f, &nx_page, sizeof(f));
	f();
}

static void bus_eof(void)
{
	(void)bus_page[page_size];	/* Beyond EOF. */
}

/* Handled by the CPU, emulated by the kernel, or signaled. */
static void misaligned(void)
{
	misval = *(volatile uint64_t *)(buf + 1);
}

static int prepare_misaligned(void)
{
	misaligned_signals = probe(misaligned) == SIGBUS;
	misval = 0;

	return 0;
}

static bool misaligned_ok(void)
{
	uint64_t v;

	if (misaligned_signals)
		return nr_signals == 1 && last_signo == SIGBUS;

	memcpy(&v, buf + 1, sizeof(v));

	return nr_signals == 0 && misval == v;
}

static void fresh(void)
{
	*(volatile int *)fresh_page = 1;
}

static bool fresh_ok(void)
{
	return *(volatile int *)fresh_page == 1;
}

/*
 * Dovetail copies the pages of a dovetailed process at fork time instead
 * of write-protecting them: a write after fork() must not fault.
 */
static int prepare_fork_write(void)
{
	pid_t pid;

	if (munlock((void *)cow_page, page_size))
		error(1, errno, "munlock");

	*cow_page = 1;
	pid = fork();
	if (pid < 0)
		error(1, errno, "fork");
	if (pid == 0)
		_exit(0);
	waitpid(pid, NULL, 0);

	return 0;
}

static void fork_write(void)
{
	*cow_page = 2;
}

static bool fork_write_ok(void)
{
	return *cow_page == 2;
}

/*
 * The kernel faults on behalf of us and fixes up: thread_getstat is a
 * current mode syscall, which copies to our buffer directly.
 */
static void kernel_fault(void)
{
	kret = cobalt_thread_stat(0, none_page);
}

static bool kernel_fault_ok(void)
{
	return kret == -EFAULT;
}

/* The kernel faults on behalf of us and pages in. */
static void kernel_fresh(void)
{
	kret = cobalt_thread_stat(0, fresh_page);
}

static bool kernel_ok(void)
{
	return kret == 0;
}

/*
 * A kprobe on the thread_getstat syscall handler, called in primary
 * mode: the breakpoint traps from kernel mode over the oob stage. Needs
 * root and
 * kprobe events. With KPROBES_ON_FTRACE, a probe on the function entry
 * is an ftrace hook which does not trap, so x86 tries the first
 * instructions after the fentry call (and ENDBR64): the kernel rejects
 * offsets which are not on an instruction boundary.
 */
#define TRACEFS		"/sys/kernel/tracing"
#define KPROBES_OPT	"/proc/sys/debug/kprobes-optimization"
#define KPROBE_TARGET	"CoBaLt_thread_getstat"
#define KPROBE_EVENT	"trap_matrix"

#if defined(__x86_64__)
static const unsigned int kprobe_offsets[] = { 0, 5, 9 };
#else
static const unsigned int kprobe_offsets[] = { 0 };
#endif

static char kprobe_opt_saved[8];

/* 1 if our probe is a breakpoint, 0 if hooked otherwise, -1 if unknown. */
static int kprobe_traps(void)
{
	char list[8192], *line;

	if (read_file("/sys/kernel/debug/kprobes/list", list, sizeof(list)))
		return -1;

	for (line = strtok(list, "\n"); line; line = strtok(NULL, "\n"))
		if (strstr(line, KPROBE_TARGET))
			return !strstr(line, "[FTRACE]") &&
				!strstr(line, "[OPTIMIZED]");

	return -1;
}

static void cleanup_kprobe(void)
{
	write_file(TRACEFS "/events/kprobes/" KPROBE_EVENT "/enable", "0\n");
	write_file(TRACEFS "/kprobe_events", "-:" KPROBE_EVENT "\n");
	if (kprobe_opt_saved[0]) {
		write_file(KPROBES_OPT, kprobe_opt_saved);
		kprobe_opt_saved[0] = '\0';
	}
}

static int prepare_kprobe(void)
{
	char probe[64];
	unsigned int n;

	if (geteuid() || access(TRACEFS "/kprobe_events", W_OK))
		return SKIP;

	/* An optimized kprobe is a jump, not a trap. */
	if (!read_file(KPROBES_OPT, kprobe_opt_saved, sizeof(kprobe_opt_saved)) &&
	    write_file(KPROBES_OPT, "0\n"))
		kprobe_opt_saved[0] = '\0';

	for (n = 0; n < sizeof(kprobe_offsets) / sizeof(kprobe_offsets[0]); n++) {
		snprintf(probe, sizeof(probe), "p:" KPROBE_EVENT " "
			 KPROBE_TARGET "+%u\n", kprobe_offsets[n]);
		if (write_file(TRACEFS "/kprobe_events", probe))
			continue;
		if (!write_file(TRACEFS "/events/kprobes/" KPROBE_EVENT "/enable", "1\n") &&
		    kprobe_traps() != 0)
			return 0;
		write_file(TRACEFS "/kprobe_events", "-:" KPROBE_EVENT "\n");
	}

	cleanup_kprobe();

	return SKIP;
}

static void kprobe(void)
{
	struct cobalt_threadstat stat;

	kret = cobalt_thread_stat(0, &stat);
}

static volatile float fzero;

/* Enabling the FP trap is part of the trigger. */
static void fp_div0(void)
{
	feenableexcept(FE_DIVBYZERO);
	fzero = 1.0f / fzero;
}

/* Not all CPUs can trap on FP exceptions (riscv, most arm64). */
static int prepare_fp_div0(void)
{
	if (feenableexcept(FE_DIVBYZERO) < 0)
		return SKIP;

	sanitize_fp();

	return probe(fp_div0) == SIGFPE ? 0 : SKIP;
}

/* Architecture specific cases. */

#if defined(__x86_64__) || defined(__i386__)

#define SIGILL_CODE	ILL_ILLOPN
#define SIGTRAP_CODE	ANY		/* do_int3_user(): SI_KERNEL */

static void illegal(void)
{
	__asm__ volatile("ud2");
}

static void breakpoint(void)
{
	__asm__ volatile("int3");
}

/* #GP, signaled with force_sig(): SI_KERNEL. */
static void privileged(void)
{
	__asm__ volatile("hlt");
}

#define PRIVILEGED_SIGNO	SIGSEGV
#define PRIVILEGED_CODE		ANY

/* Other CPUs do not trap on integer division by zero. */
static void div0(void)
{
	volatile unsigned int zero = 0;

	__asm__ volatile("xor %%edx, %%edx\n"
			 "mov $1, %%eax\n"
			 "divl %0\n"
			 : : "m"(zero) : "eax", "edx", "cc", "memory");
}

#define DIV0_CASE \
	{ "div0", NULL, div0, NULL, NULL, SIGFPE, FPE_INTDIV, NULL, ANY },

#endif

#ifdef __x86_64__

/*
 * Legacy vsyscalls: calling into the vsyscall page traps, and the
 * kernel emulates the call unless booted with vsyscall=none. The
 * emulated gettimeofday() writes its result from the in-band fixup,
 * which faults on a fresh page, or fails on a bad address.
 */
#define VSYSCALL_GETTIMEOFDAY	0xffffffffff600000UL

typedef long (*vgettimeofday_t)(struct timeval *tv, struct timezone *tz);

static struct timeval vsyscall_tv;

static long vsyscall_ret;

static int prepare_vsyscall(void)
{
	char line[256];
	bool found = false;
	FILE *fp;

	fp = fopen("/proc/self/maps", "r");
	if (fp == NULL)
		error(1, errno, "/proc/self/maps");

	while (!found && fgets(line, sizeof(line), fp))
		found = strstr(line, "[vsyscall]") != NULL;

	fclose(fp);
	vsyscall_ret = -1;
	memset(&vsyscall_tv, 0, sizeof(vsyscall_tv));

	return found ? 0 : SKIP;
}

static void vsyscall(void)
{
	vsyscall_ret = ((vgettimeofday_t)VSYSCALL_GETTIMEOFDAY)(&vsyscall_tv, NULL);
}

static bool tv_ok(const struct timeval *tv)
{
	return vsyscall_ret == 0 && tv->tv_sec > 0 &&
		tv->tv_usec >= 0 && tv->tv_usec < 1000000;
}

static bool vsyscall_ok(void)
{
	return tv_ok(&vsyscall_tv);
}

static int prepare_vsyscall_fresh(void)
{
	return prepare_vsyscall() ?: drop_fresh_page();
}

static void vsyscall_fresh(void)
{
	vsyscall_ret = ((vgettimeofday_t)VSYSCALL_GETTIMEOFDAY)(fresh_page, NULL);
}

static bool vsyscall_fresh_ok(void)
{
	return tv_ok(fresh_page);
}

/* Lowest page is never mapped (vm.mmap_min_addr). */
static void vsyscall_badaddr(void)
{
	((vgettimeofday_t)VSYSCALL_GETTIMEOFDAY)((struct timeval *)0x10, NULL);
}

/*
 * With UMIP enabled (CR4.UMIP), SGDT and friends raise #GP from user
 * mode, the kernel emulates them with dummy results.
 */
#define UMIP_DUMMY_GDT_BASE	0xfffffffffffe0000ULL

struct dtr {
	uint16_t limit;
	uint64_t base;
} __attribute__((packed));

static void sgdt(void *dst)
{
	__asm__ volatile("sgdt (%0)" : : "r"(dst) : "memory");
}

static bool dtr_ok(const void *p)
{
	struct dtr dtr;

	memcpy(&dtr, p, sizeof(dtr));

	return dtr.base == UMIP_DUMMY_GDT_BASE;
}

static int prepare_umip(void)
{
	struct dtr dtr = { 0, 0 };

	sgdt(&dtr);
	memset(buf, 0, sizeof(buf));

	return dtr_ok(&dtr) ? 0 : SKIP;
}

static void umip(void)
{
	sgdt(buf);
}

static bool umip_ok(void)
{
	return dtr_ok(buf);
}

static int prepare_umip_fresh(void)
{
	return prepare_umip() ?: drop_fresh_page();
}

static void umip_fresh(void)
{
	sgdt(fresh_page);
}

static bool umip_fresh_ok(void)
{
	return dtr_ok(fresh_page);
}

/* A canonical kernel address: the emulation fails without a fault. */
static void umip_badaddr(void)
{
	sgdt((void *)0xffff800000000000UL);
}

#define ARCH_CASES							\
	DIV0_CASE							\
	{ "vsyscall", prepare_vsyscall, vsyscall, vsyscall_ok, NULL,	\
	  0, 0, NULL, ANY },						\
	{ "vsyscall-fresh", prepare_vsyscall_fresh, vsyscall_fresh,	\
	  vsyscall_fresh_ok, NULL, 0, 0, NULL, 1 },			\
	{ "vsyscall-badaddr", prepare_vsyscall, vsyscall_badaddr, NULL,	\
	  NULL, SIGSEGV, SI_KERNEL, NULL, ANY },			\
	{ "umip", prepare_umip, umip, umip_ok, NULL, 0, 0, NULL, ANY },	\
	{ "umip-fresh", prepare_umip_fresh, umip_fresh, umip_fresh_ok,	\
	  NULL, 0, 0, NULL, 1 },					\
	{ "umip-badaddr", prepare_umip, umip_badaddr, NULL, NULL,	\
	  SIGSEGV, SEGV_MAPERR, NULL, ANY },

#elif defined(__i386__)

#define ARCH_CASES	DIV0_CASE

#elif defined(__aarch64__)

#define SIGILL_CODE	ILL_ILLOPC
#define SIGTRAP_CODE	TRAP_BRKPT

static void illegal(void)
{
	__asm__ volatile(".inst 0x00000000");	/* udf #0 */
}

static void breakpoint(void)
{
	__asm__ volatile("brk #0");
}

/* EL1 system register: undefined at EL0. */
static void privileged(void)
{
	uint64_t v;

	__asm__ volatile("mrs %0, ttbr0_el1" : "=r"(v));
	(void)v;
}

#define PRIVILEGED_SIGNO	SIGILL
#define PRIVILEGED_CODE		ILL_ILLOPC

/*
 * SCTLR_EL1.SA0 is set by Linux: a data access through a misaligned
 * SP from EL0 raises an SP alignment fault. Emulators may not check
 * (QEMU/TCG does not). The handler runs on the alternate stack.
 */
static void sp_align(void)
{
	__asm__ volatile("mov x9, sp\n"
			 "add x10, %0, #1\n"
			 "mov sp, x10\n"
			 "ldr x11, [sp]\n"
			 "mov sp, x9\n"
			 : : "r"(buf) : "x9", "x10", "x11", "memory");
}

static int prepare_sp_align(void)
{
	return probe(sp_align) == SIGBUS ? 0 : SKIP;
}

#define ARCH_CASES							\
	{ "sp-align", prepare_sp_align, sp_align, NULL, NULL,		\
	  SIGBUS, BUS_ADRALN, NULL, ANY },

#elif defined(__arm__)

#define SIGILL_CODE	ILL_ILLOPC
#define SIGTRAP_CODE	TRAP_BRKPT

/*
 * ARM-state islands for the faulting instructions: the test might be
 * built as Thumb-2 code, where some encodings do not exist (SWP).
 */
__asm__(".text\n"
	".arm\n"
	".align 2\n"
	".type trap_arm_bkpt, %function\n"
	"trap_arm_bkpt:\n"
	"	.inst 0xe7f001f0\n"	/* gdb/ptrace ARM breakpoint */
	"	bx lr\n"
	".size trap_arm_bkpt, . - trap_arm_bkpt\n"
	".type trap_arm_undef, %function\n"
	"trap_arm_undef:\n"
	"	.inst 0xe7f000f0\n"	/* udf #0 */
	"	bx lr\n"
	".size trap_arm_undef, . - trap_arm_undef\n"
	/* unsigned int trap_arm_swp(unsigned int *addr, unsigned int val) */
	".type trap_arm_swp, %function\n"
	"trap_arm_swp:\n"
	"	mov r2, r0\n"
	"	.inst 0xe1020091\n"	/* swp r0, r1, [r2] */
	"	bx lr\n"
	".size trap_arm_swp, . - trap_arm_swp\n"
	/* void trap_arm_ldrd(void *addr, unsigned int out[2]) */
	".type trap_arm_ldrd, %function\n"
	"trap_arm_ldrd:\n"
	"	.inst 0xe1c020d0\n"	/* ldrd r2, r3, [r0] */
	"	stm r1, {r2, r3}\n"
	"	bx lr\n"
	".size trap_arm_ldrd, . - trap_arm_ldrd\n");

void trap_arm_bkpt(void);
void trap_arm_undef(void);
unsigned int trap_arm_swp(unsigned int *addr, unsigned int val);
void trap_arm_ldrd(void *addr, unsigned int out[2]);

static void illegal(void)
{
	trap_arm_undef();
}

static void breakpoint(void)
{
	trap_arm_bkpt();
}

/*
 * SWP is obsolete since ARMv8, an arm kernel with CONFIG_SWP_EMULATE
 * or an arm64 kernel with abi.swp=1 emulates it from the undefined
 * instruction trap. The emulation writes to user memory, which faults
 * on a fresh page, or fails on a bad address.
 */
static unsigned int swp_result;

static void swp_probe(void)
{
	unsigned int word = 0;

	swp_result = trap_arm_swp(&word, 1);
	if (word != 1)
		swp_result = ~0U;
}

static int prepare_swp(void)
{
	write_file("/proc/sys/abi/swp", "1\n");	/* arm64 kernel */

	return probe(swp_probe) || swp_result ? SKIP : 0;
}

static void swp(void)
{
	unsigned int *word = (unsigned int *)buf;

	*word = 0x11223344;
	swp_result = trap_arm_swp(word, 0x55667788);
}

static bool swp_ok(void)
{
	return swp_result == 0x11223344 && *(unsigned int *)buf == 0x55667788;
}

static int prepare_swp_fresh(void)
{
	return prepare_swp() ?: drop_fresh_page();
}

static void swp_fresh(void)
{
	swp_result = trap_arm_swp(fresh_page, 0x55667788);
}

static bool swp_fresh_ok(void)
{
	return swp_result == 0 && *(unsigned int *)fresh_page == 0x55667788;
}

static void swp_badaddr(void)
{
	trap_arm_swp((unsigned int *)0x10, 0);
}

/*
 * Misaligned LDRD: an arm kernel fixes it up or signals it as set in
 * /proc/cpu/alignment, an arm64 kernel as configured at build time
 * (COMPAT_ALIGNMENT_FIXUPS). Run the cases matching what we find.
 */
#define ALIGNMENT		"/proc/cpu/alignment"
#define ALIGN_FIXUP		"2\n"
#define ALIGN_SIGNAL		"4\n"

static unsigned int ldrd_result[2];

static char align_saved[64];

static void ldrd(void)
{
	trap_arm_ldrd(buf + 1, ldrd_result);
}

static int use_align_mode(const char *mode)
{
	char s[256], *p;

	if (!read_file(ALIGNMENT, s, sizeof(s)) &&
	    (p = strstr(s, "User faults:")) != NULL) {
		snprintf(align_saved, sizeof(align_saved), "%d\n",
			 atoi(p + strlen("User faults:")));
		if (!write_file(ALIGNMENT, mode))
			return 0;
		align_saved[0] = '\0';
	}

	memcpy(buf, "\x01\x02\x03\x04\x05\x06\x07\x08\x09\x0a", 10);
	if (probe(ldrd) == SIGBUS)
		return strcmp(mode, ALIGN_SIGNAL) ? SKIP : 0;

	return memcmp(ldrd_result, buf + 1, sizeof(ldrd_result)) ||
		strcmp(mode, ALIGN_FIXUP) ? SKIP : 0;
}

static void cleanup_align(void)
{
	if (align_saved[0]) {
		write_file(ALIGNMENT, align_saved);
		align_saved[0] = '\0';
	}
}

static int prepare_align_fixup(void)
{
	return use_align_mode(ALIGN_FIXUP);
}

static void align_fixup(void)
{
	memcpy(buf, "\x01\x02\x03\x04\x05\x06\x07\x08\x09\x0a", 10);
	memset(ldrd_result, 0, sizeof(ldrd_result));
	ldrd();
}

static bool align_fixup_ok(void)
{
	return !memcmp(ldrd_result, buf + 1, sizeof(ldrd_result));
}

static int prepare_align_fixup_fresh(void)
{
	return prepare_align_fixup() ?: drop_fresh_page();
}

static void align_fixup_fresh(void)
{
	ldrd_result[0] = ldrd_result[1] = ~0U;
	trap_arm_ldrd((char *)fresh_page + 1, ldrd_result);
}

static bool align_fixup_fresh_ok(void)
{
	return ldrd_result[0] == 0 && ldrd_result[1] == 0;
}

static int prepare_align_signal(void)
{
	return use_align_mode(ALIGN_SIGNAL);
}

#define ARCH_CASES							\
	{ "swp", prepare_swp, swp, swp_ok, NULL, 0, 0, NULL, ANY },	\
	{ "swp-fresh", prepare_swp_fresh, swp_fresh, swp_fresh_ok,	\
	  NULL, 0, 0, NULL, 1 },					\
	{ "swp-badaddr", prepare_swp, swp_badaddr, NULL, NULL,		\
	  SIGSEGV, ANY, NULL, ANY },					\
	{ "align-fixup", prepare_align_fixup, align_fixup,		\
	  align_fixup_ok, cleanup_align, 0, 0, NULL, ANY },		\
	{ "align-fixup-fresh", prepare_align_fixup_fresh,		\
	  align_fixup_fresh, align_fixup_fresh_ok, cleanup_align,	\
	  0, 0, NULL, 1 },						\
	{ "align-signal", prepare_align_signal, ldrd, NULL,		\
	  cleanup_align, SIGBUS, BUS_ADRALN, NULL, ANY },

#elif defined(__riscv)

#define SIGILL_CODE	ILL_ILLOPC
#define SIGTRAP_CODE	TRAP_BRKPT

static void illegal(void)
{
	__asm__ volatile(".word 0");
}

static void breakpoint(void)
{
	__asm__ volatile("ebreak");
}

static void privileged(void)
{
	unsigned long v;

	__asm__ volatile("csrr %0, sstatus" : "=r"(v));
	(void)v;
}

#define PRIVILEGED_SIGNO	SIGILL
#define PRIVILEGED_CODE		ILL_ILLOPC

#define ARCH_CASES

#else
#error "no trap cases for this architecture"
#endif

static const struct tcase cases[] = {
	{ "read-null", NULL, read_null, NULL, NULL,
	  SIGSEGV, SEGV_MAPERR, &null_addr, ANY },
	{ "write-ro", NULL, write_ro, NULL, NULL,
	  SIGSEGV, SEGV_ACCERR, (void **)&ro_page, ANY },
	{ "write-retry", prepare_write_retry, write_retry, write_retry_ok, NULL,
	  SIGSEGV, SEGV_ACCERR, (void **)&retry_page, ANY },
	{ "exec-nx", NULL, exec_nx, NULL, NULL,
	  SIGSEGV, SEGV_ACCERR, &nx_page, ANY },
	{ "bus-eof", NULL, bus_eof, NULL, NULL,
	  SIGBUS, BUS_ADRERR, &bus_addr, ANY },
	{ "illegal", NULL, illegal, NULL, NULL,
	  SIGILL, SIGILL_CODE, NULL, ANY },
	{ "breakpoint", NULL, breakpoint, NULL, NULL,
	  SIGTRAP, SIGTRAP_CODE, NULL, ANY },
#ifdef PRIVILEGED_SIGNO
	{ "privileged", NULL, privileged, NULL, NULL,
	  PRIVILEGED_SIGNO, PRIVILEGED_CODE, NULL, ANY },
#endif
	{ "misaligned", prepare_misaligned, misaligned, misaligned_ok, NULL,
	  ANY, 0, NULL, ANY },
	{ "fp-div0", prepare_fp_div0, fp_div0, NULL, NULL,
	  SIGFPE, FPE_FLTDIV, NULL, ANY },
	{ "fresh", drop_fresh_page, fresh, fresh_ok, NULL, 0, 0, NULL, 1 },
	{ "fork-write", prepare_fork_write, fork_write, fork_write_ok, NULL,
	  0, 0, NULL, 0 },
	{ "kernel-fault", NULL, kernel_fault, kernel_fault_ok, NULL,
	  0, 0, NULL, ANY },
	{ "kernel-fresh", drop_fresh_page, kernel_fresh, kernel_ok, NULL,
	  0, 0, NULL, 1 },
	{ "kprobe", prepare_kprobe, kprobe, kernel_ok, cleanup_kprobe,
	  0, 0, NULL, ANY },
	ARCH_CASES
	/* A notification an earlier case left pending shows here. */
	{ "read-null-again", NULL, read_null, NULL, NULL,
	  SIGSEGV, SEGV_MAPERR, &null_addr, ANY },
};

/*
 * Returns -1 if not in primary mode. No signal mask is saved:
 * rt_sigprocmask() would relax us, and SA_NODEFER keeps the signal unblocked after
 * siglongjmp().
 */
static int raise_trap(void (*fn)(void))
{
	nr_signals = 0;
	last_signo = 0;
	last_code = 0;
	last_addr = NULL;

	if (!sigsetjmp(recover, 0)) {
		if (cobalt_thread_mode() & XNRELAX)
			return -1;
		fn();
	}

	return 0;
}

/* signo ANY: the result check covers the signal too. */
static bool outcome_ok(const struct tcase *c, long faults)
{
	if (c->signo > 0 && (nr_signals != 1 || last_signo != c->signo))
		smokey_warning("%s: %d signal(s), last %s", c->name, nr_signals,
			    last_signo ? strsignal(last_signo) : "none");
	else if (!c->signo && nr_signals)
		smokey_warning("%s: %s", c->name, strsignal(last_signo));
	else if (c->signo > 0 && c->code != ANY && last_code != c->code)
		smokey_warning("%s: si_code %d", c->name, last_code);
	else if (c->signo > 0 && c->addr && last_addr != *c->addr)
		smokey_warning("%s: si_addr %p", c->name, last_addr);
	else if (c->ok && !c->ok())
		smokey_warning("%s: wrong result", c->name);
	else if (c->faults != ANY && faults != c->faults)
		smokey_warning("%s: %ld page faults, expected %d",
			    c->name, faults, c->faults);
	else
		return true;

	return false;
}

/* Back to primary mode. */
static int harden(void)
{
	struct timespec ts = { 0, 1000000 };

	return clock_nanosleep(CLOCK_MONOTONIC, 0, &ts, NULL);
}

static int run_trap_matrix(struct smokey_test *t, int argc, char *const argv[])
{
	static const int sigs[] = { SIGSEGV, SIGBUS, SIGILL, SIGTRAP, SIGFPE };
	static char altstack[64 * 1024] __attribute__((aligned(16)));
	stack_t ss = { .ss_sp = altstack, .ss_size = sizeof(altstack) };
	struct sched_param param = { .sched_priority = 1 };
	struct sigaction sa, old_sa[5];
	const struct tcase *c;
	int ret, fails = 0;
	unsigned int n;
	long faults;
	pid_t pid;
	FILE *f;

	page_size = sysconf(_SC_PAGESIZE);
	ro_page = anon(PROT_READ);
	retry_page = anon(PROT_READ | PROT_WRITE);
	nx_page = anon(PROT_READ | PROT_WRITE);
	none_page = anon(PROT_NONE);
	fresh_page = anon(PROT_READ | PROT_WRITE);
	cow_page = anon(PROT_READ | PROT_WRITE);
	f = tmpfile();
	if (!f || fputc(0, f) == EOF || fflush(f))
		error(1, errno, "tmpfile");
	bus_page = map(2 * page_size, PROT_READ, MAP_SHARED, fileno(f));
	bus_addr = (void *)(bus_page + page_size);
	for (n = 0; n < sizeof(buf); n++)
		buf[n] = n + 1;

	/* Some traps leave the stack unusable (arm64 sp-align). */
	if (sigaltstack(&ss, NULL))
		error(1, errno, "sigaltstack");

	memset(&sa, 0, sizeof(sa));
	sa.sa_sigaction = fault_handler;
	sa.sa_flags = SA_SIGINFO | SA_NODEFER | SA_ONSTACK;
	for (n = 0; n < sizeof(sigs) / sizeof(sigs[0]); n++)
		sigaction(sigs[n], &sa, &old_sa[n]);

	if (!__T(ret, pthread_setschedparam(pthread_self(), SCHED_FIFO, &param)))
		return ret;

	for (n = 0; n < sizeof(cases) / sizeof(cases[0]); n++) {
		c = &cases[n];
		if (c->prepare && c->prepare()) {
			smokey_note("%s: skipped", c->name);
			continue;
		}

		faults = minor_faults();
		if (!__T(ret, harden()))
			return ret;
		if (raise_trap(c->trigger)) {
			smokey_warning("%s: not in primary mode", c->name);
			fails++;
		} else if (!outcome_ok(c, minor_faults() - faults)) {
			fails++;
		} else {
			smokey_trace("%s: ok", c->name);
		}

		if (c->cleanup)
			c->cleanup();
	}

	/*
	 * Fork from the thread which took all the traps: a debug kernel
	 * catches an unbalanced trap entry here.
	 */
	pid = fork();
	if (pid < 0)
		error(1, errno, "fork");
	if (pid == 0)
		_exit(0);
	if (waitpid(pid, &ret, 0) != pid || !WIFEXITED(ret) || WEXITSTATUS(ret)) {
		smokey_warning("fork: child did not exit cleanly");
		fails++;
	}

	param.sched_priority = 0;
	pthread_setschedparam(pthread_self(), SCHED_OTHER, &param);
	for (n = 0; n < sizeof(sigs) / sizeof(sigs[0]); n++)
		sigaction(sigs[n], &old_sa[n], NULL);
	ss.ss_flags = SS_DISABLE;
	sigaltstack(&ss, NULL);
	fclose(f);

	return fails ? -EINVAL : 0;
}
