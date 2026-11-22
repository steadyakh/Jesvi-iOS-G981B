/* Crash reporter for apps on the G981B (inserted into TrollStore apps; not part of iOS): every Objective-C exception
 * as it is thrown (name, reason), and on SIGSEGV/SIGBUS/SIGTRAP/SIGILL/SIGABRT the faulting address, pc and lr with
 * the library and nearest symbol of each (dladdr), then the signal goes on as before. Lines "x1s-exc: ..." on
 * /dev/console. (iOS's own crash reports never arrive on this port: EXC_CORPSE_NOTIFY fails.) */
typedef unsigned long size_t;
typedef unsigned long uintptr_t;
typedef unsigned long long uint64_t;
typedef void *id, *SEL;
typedef struct { const char *dli_fname; void *dli_fbase; const char *dli_sname; void *dli_saddr; } Dl_info;
extern int open(const char *, int, ...);
extern long write(int, const void *, size_t);
extern int close(int);
extern int snprintf(char *, size_t, const char *, ...);
extern int dladdr(const void *, Dl_info *);
extern id objc_msgSend(id, SEL, ...);
extern SEL sel_registerName(const char *);
typedef id (*preprocessor)(id);
extern preprocessor objc_setExceptionPreprocessor(preprocessor);

/* <sys/signal.h>, arm64 Darwin */
struct x1s_sigaction { void (*handler)(int, void *, void *); unsigned mask; int flags; };
extern int sigaction(int, const struct x1s_sigaction *, struct x1s_sigaction *);
#define SA_SIGINFO 0x40
#define SA_RESETHAND 0x4
struct siginfo_min { int signo, errno_, code, pid; unsigned uid; int status; void *addr; };

static preprocessor previous;

static void
line(const char *s)
{
	int fd = open("/dev/console", 0x20001);
	size_t n = 0;
	while (s[n]) n++;
	if (fd >= 0) { write(fd, s, n); close(fd); }
}

static const char *
text(id o, const char *sel)
{
	id s = o ? ((id (*)(id, SEL))objc_msgSend)(o, sel_registerName(sel)) : 0;
	const char *c = s ? ((const char *(*)(id, SEL))objc_msgSend)(s, sel_registerName("UTF8String")) : 0;
	return c ? c : "?";
}

static id
seen(id exception)
{
	char l[1200];
	snprintf(l, sizeof(l), "x1s-exc: %s: %s%c", text(exception, "name"), text(exception, "reason"), 10);
	line(l);
	return previous ? previous(exception) : exception;
}

static void
where(const char *what, uintptr_t a)
{
	char l[600];
	Dl_info i = { 0 };
	const char *lib = "?", *sym = "?", *slash;

	if (dladdr((void *)a, &i)) {
		lib = i.dli_fname ? i.dli_fname : "?";
		for (slash = lib; *slash; slash++) if (*slash == '/') lib = slash + 1;
		sym = i.dli_sname ? i.dli_sname : "?";
	}
	snprintf(l, sizeof(l), "x1s-exc:   %s %#lx = %s + %#lx (%s + %#lx)%c", what, a, lib, a - (uintptr_t)i.dli_fbase, sym,
	    i.dli_saddr ? a - (uintptr_t)i.dli_saddr : 0, 10);
	line(l);
}

static void
fault(int sig, void *info, void *context)
{
	char l[200];
	struct siginfo_min *si = info;
	/* ucontext_t: uc_mcontext (pointer) at offset 48; __ss after __es (16 bytes): x0..x28, fp, lr, sp, pc */
	uint64_t *ss = context ? (uint64_t *)(*(char **)((char *)context + 48) + 16) : 0;

	static int reported;
	if (reported++ >= 3) return;            /* an app that recovers from its own faults: the first three only */
	snprintf(l, sizeof(l), "x1s-exc: signal %d at address %p%c", sig, si ? si->addr : 0, 10);
	line(l);
	if (ss) {
		uint64_t *fp = (uint64_t *)ss[29];
		where("pc", (uintptr_t)ss[32]);
		where("lr", (uintptr_t)ss[30]);
		/* the frame chain: [fp] the caller's fp, [fp + 8] its return address */
		for (int k = 0; k < 12 && fp && ((uintptr_t)fp & 7) == 0; k++) {
			where("caller", (uintptr_t)fp[1]);
			fp = (uint64_t *)fp[0];
		}
	}
	/* SA_RESETHAND: returning re-runs the instruction with the default action, which ends the process as before */
}

__attribute__((constructor)) static void
start(void)
{
	static const int sigs[] = { 11, 10, 5, 4, 6 };   /* SEGV BUS TRAP ILL ABRT */
	struct x1s_sigaction sa = { fault, 0, SA_SIGINFO | SA_RESETHAND };

	previous = objc_setExceptionPreprocessor(seen);
	for (unsigned i = 0; i < 5; i++) sigaction(sigs[i], &sa, 0);
}
