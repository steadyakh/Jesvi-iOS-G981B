/* Diagnostic for Settings (not shipped): whose code runs the methods PreferenceLoader hooks to add tweak panes
 * (PreferenceLoader's or Apple's), eight seconds after start. Lines to /dev/console as "x1s-diag: ...". */
typedef unsigned long size_t;
typedef unsigned long long uint64_t;
typedef void *id, *SEL, *Class, *Method, *IMP;
typedef struct { const char *dli_fname; void *dli_fbase; const char *dli_sname; void *dli_saddr; } Dl_info;
extern int open(const char *, int, ...);
extern long write(int, const void *, size_t);
extern int close(int);
extern int snprintf(char *, size_t, const char *, ...);
extern int dladdr(const void *, Dl_info *);
extern uint64_t dispatch_time(uint64_t, long long);
extern void dispatch_after_f(uint64_t, void *, void *, void (*)(void *));
extern char _dispatch_main_q;
extern Class objc_getClass(const char *);
extern Method class_getInstanceMethod(Class, SEL);
extern IMP method_getImplementation(Method);
extern SEL sel_registerName(const char *);

static void
say(const char *s)
{
	char l[700];
	int fd = open("/dev/console", 0x20001), n = snprintf(l, sizeof(l), "x1s-diag: %s%c", s, 10);
	if (fd >= 0) { write(fd, l, (size_t)n); close(fd); }
}

static void
where(const char *cls, const char *sel)
{
	char line[600];
	Class c = objc_getClass(cls);
	Method m = c ? class_getInstanceMethod(c, sel_registerName(sel)) : 0;
	Dl_info i = { 0 };

	if (m && dladdr(method_getImplementation(m), &i)) {
		snprintf(line, sizeof(line), "-[%s %s] -> %s (%s)", cls, sel, i.dli_fname ? i.dli_fname : "?", i.dli_sname ? i.dli_sname : "?");
	} else {
		snprintf(line, sizeof(line), "-[%s %s]: %s", cls, sel, c ? "no method" : "no class");
	}
	say(line);
}

static void
later(void *unused)
{
	(void)unused;
	where("PSUIPrefsListController", "specifiers");
	where("PSListController", "specifiers");
	where("PSListController", "loadSpecifiersFromPlistName:target:");
	where("PSSpecifier", "setupIconImageWithBundle:");
	where("PSUIPrefsListController", "_specifiersForPreferenceLoader");
	say("end of report");
}

__attribute__((constructor)) static void
start(void)
{
	say("loaded");
	dispatch_after_f(dispatch_time(0, 8000000000LL), &_dispatch_main_q, 0, later);
}
