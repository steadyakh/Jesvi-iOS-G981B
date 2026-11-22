/* Diagnostic for Settings (not shipped): does PreferenceLoader ever turn a tweak's entry into rows?
 * Wraps libprefs' -[PSListController specifiersFromEntry:sourcePreferenceLoaderBundlePath:title:] and
 * +[PSSpecifier environmentPassesPreferenceLoaderFilter:] and logs each call. Lines to /dev/console as "x1s-diag: ...". */
typedef unsigned long size_t;
typedef unsigned long long uint64_t;
typedef void *id, *SEL, *Class, *Method, *IMP;
extern int open(const char *, int, ...);
extern long write(int, const void *, size_t);
extern int close(int);
extern int snprintf(char *, size_t, const char *, ...);
extern Class objc_getClass(const char *);
extern Class object_getClass(void *);
extern Method class_getInstanceMethod(Class, SEL);
extern IMP method_setImplementation(Method, IMP);
extern SEL sel_registerName(const char *);
extern void _dyld_register_func_for_add_image(void (*)(const void *, long));
extern id objc_msgSend(id, SEL, ...);
#define AT(arr, i) (((id (*)(id, SEL, size_t))objc_msgSend)((arr), sel_registerName("objectAtIndex:"), (size_t)(i)))
#define MSG0(o, s) (((id (*)(id, SEL))objc_msgSend)((o), sel_registerName(s)))

static void
say(const char *s, long long v)
{
	char l[300];
	int fd = open("/dev/console", 0x20001), n = snprintf(l, sizeof(l), "x1s-diag: %s %lld%c", s, v, 10);
	if (fd >= 0) { write(fd, l, (size_t)n); close(fd); }
}

static id (*old_entry)(id, SEL, id, id, id);
static char (*old_filter)(id, SEL, id);
static int done, outer;
static id (*old_root)(id, SEL);
extern uint64_t dispatch_time(uint64_t, long long);
extern void dispatch_after_f(uint64_t, void *, void *, void (*)(void *));
extern char _dispatch_main_q;
extern const char *object_getClassName(id);

static id
my_root(id self, SEL cmd)
{
	id r = old_root(self, cmd);
	long long n = r ? (long long)(size_t)MSG0(r, "count") : -1;
	say("root list rows", n);
	for (long long i = n > 6 ? n - 6 : 0; i < n; i++) {
		id sp = AT(r, i);
		id name = MSG0(sp, "name");
		const char *c = name ? (const char *)MSG0(name, "UTF8String") : "(no name)";
		say(c ? c : "?", i);
	}
	return r;
}

static void
wrap_root(void *unused)
{
	Method m = class_getInstanceMethod(objc_getClass("PSUIPrefsListController"), sel_registerName("specifiers"));
	(void)unused;
	if (m && !outer) { outer = 1; old_root = (id (*)(id, SEL))method_setImplementation(m, (IMP)my_root); say("root wrapped", 1); }
}

static id
my_entry(id self, SEL cmd, id entry, id path, id title)
{
	id r = old_entry(self, cmd, entry, path, title);
	long long n = r ? (long long)(size_t)MSG0(r, "count") : -1;
	if (n > 0) { id sp = AT(r, 0); id nm = MSG0(sp, "name"); say(nm ? (const char *)MSG0(nm, "UTF8String") : "(no name)", n); }
	return r;
}

static char
my_filter(id self, SEL cmd, id filter)
{
	char r = old_filter(self, cmd, filter);
	say("filter passes", r);
	return r;
}

static void
added(const void *mh, long slide)
{
	Class ps = objc_getClass("PSListController"), spec = objc_getClass("PSSpecifier");
	Method a, b;

	(void)mh; (void)slide;
	if (done || !ps || !spec) return;
	a = class_getInstanceMethod(ps, sel_registerName("specifiersFromEntry:sourcePreferenceLoaderBundlePath:title:"));
	b = class_getInstanceMethod(object_getClass(spec), sel_registerName("environmentPassesPreferenceLoaderFilter:"));
	if (!a) return;         /* libprefs not loaded yet */
	done = 1;
	old_entry = (id (*)(id, SEL, id, id, id))method_setImplementation(a, (IMP)my_entry);
	if (b) old_filter = (char (*)(id, SEL, id))method_setImplementation(b, (IMP)my_filter);
	say("wrapped (filter too)", b != 0);
}

__attribute__((constructor)) static void
start(void)
{
	say("loaded", 0);
	dispatch_after_f(dispatch_time(0, 300000000LL), &_dispatch_main_q, 0, wrap_root);
	_dyld_register_func_for_add_image(added);
}
