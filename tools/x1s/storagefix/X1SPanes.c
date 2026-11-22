/*
 * Settings' tweak panes on the G981B. PreferenceLoader makes a row for every tweak with a settings pane (measured:
 * CC26, FloatingDockXVI), but Jesvi's two root-list add-ons (libredwood-alwayson, cellular_settings) each rebuild the
 * list from their own copy and store it back, and PreferenceLoader's rows are lost on the way (the final list had 62
 * rows, none of them a tweak's). This wraps -[PSUIPrefsListController specifiers] last, outside both, and adds a
 * "Tweaks" group with the rows PreferenceLoader would have made, through libprefs' own
 * -specifiersFromEntry:sourcePreferenceLoaderBundlePath:title:, when they are not in the list already.
 */
typedef unsigned long size_t;
typedef unsigned long long uint64_t;
typedef void *id, *SEL, *Class, *Method, *IMP, *Ivar;

extern int open(const char *, int, ...);
extern long write(int, const void *, size_t);
extern int close(int);
extern int snprintf(char *, size_t, const char *, ...);
extern uint64_t dispatch_time(uint64_t, long long);
extern void dispatch_after_f(uint64_t, void *, void *, void (*)(void *));
extern char _dispatch_main_q;
extern Class objc_getClass(const char *);
extern Method class_getInstanceMethod(Class, SEL);
extern IMP method_setImplementation(Method, IMP);
extern SEL sel_registerName(const char *);
extern Ivar class_getInstanceVariable(Class, const char *);
extern void object_setIvarWithStrongDefault(id, Ivar, id);
extern id objc_msgSend(id, SEL, ...);

#define S(n) sel_registerName(n)
#define M0(o, s) (((id (*)(id, SEL))objc_msgSend)((o), S(s)))
#define M1(o, s, a) (((id (*)(id, SEL, id))objc_msgSend)((o), S(s), (a)))
#define M2(o, s, a, b) (((id (*)(id, SEL, id, id))objc_msgSend)((o), S(s), (a), (b)))
#define M3(o, s, a, b, c) (((id (*)(id, SEL, id, id, id))objc_msgSend)((o), S(s), (a), (b), (c)))
#define AT(o, i) (((id (*)(id, SEL, size_t))objc_msgSend)((o), S("objectAtIndex:"), (size_t)(i)))
#define COUNT(o) ((size_t)(((size_t (*)(id, SEL))objc_msgSend)((o), S("count"))))
#define STR(c) (((id (*)(id, SEL, const char *))objc_msgSend)((id)objc_getClass("NSString"), S("stringWithUTF8String:"), (c)))
#define EQ(a, b) ((((char (*)(id, SEL, id))objc_msgSend)((a), S("isEqualToString:"), (b))) != 0)

#define DIR "/var/jb/Library/PreferenceLoader/Preferences"

static id (*old_root)(id, SEL);
static int wrapped;

static void
say(const char *s, long long v)
{
	char l[300];
	int fd = open("/dev/console", 0x20001), n = snprintf(l, sizeof(l), "x1s-panes: %s %lld%c", s, v, 10);
	if (fd >= 0) { write(fd, l, (size_t)n); close(fd); }
}

/* is there a row with this name in the list */
static int
has(id list, id name)
{
	size_t n = COUNT(list);
	for (size_t i = 0; i < n; i++) {
		id nm = M0(AT(list, i), "name");
		if (nm && EQ(nm, name)) return 1;
	}
	return 0;
}

static id
my_root(id self, SEL cmd)
{
	id list = old_root(self, cmd), items = 0, files, fm, dir;
	size_t n, added = 0;

	if (list == 0) return list;
	fm = M0((id)objc_getClass("NSFileManager"), "defaultManager");
	dir = STR(DIR);
	files = M2(fm, "contentsOfDirectoryAtPath:error:", dir, 0);
	n = files ? COUNT(files) : 0;
	for (size_t i = 0; i < n; i++) {
		id file = AT(files, i), plist, entry, label, made;
		if (!EQ(M0(file, "pathExtension"), STR("plist"))) continue;
		plist = M1((id)objc_getClass("NSDictionary"), "dictionaryWithContentsOfFile:", M1(dir, "stringByAppendingPathComponent:", file));
		entry = plist ? M1(plist, "objectForKey:", STR("entry")) : 0;
		label = entry ? M1(entry, "objectForKey:", STR("label")) : 0;
		if (entry == 0 || label == 0 || has(list, label)) continue;
		made = M3(self, "specifiersFromEntry:sourcePreferenceLoaderBundlePath:title:", entry,
		    M1(dir, "stringByAppendingPathComponent:", file), label);
		if (made == 0 || COUNT(made) == 0) continue;
		if (items == 0) {
			items = M0(list, "mutableCopy");
			M1(items, "addObject:", M1((id)objc_getClass("PSSpecifier"), "groupSpecifierWithName:", STR("Tweaks")));
		}
		M1(items, "addObjectsFromArray:", made);
		added += COUNT(made);
	}
	if (items == 0) return list;
	/* stored as the others store it, with the reference counting done by KVC (the ivar directly: no setter named so) */
	/* not stored here: Jesvi's two add-ons, wrapped around this one, copy what it returns and store that */
	M0(items, "autorelease");
	say("tweak rows put back", (long long)added);
	return items;
}

static void
wrap(void *unused)
{
	Method m;

	(void)unused;
	if (wrapped) return;
	m = class_getInstanceMethod(objc_getClass("PSUIPrefsListController"), S("specifiers"));
	if (m == 0) {
		dispatch_after_f(dispatch_time(0, 300000000LL), &_dispatch_main_q, 0, wrap);
		return;
	}
	wrapped = 1;
	old_root = (id (*)(id, SEL))method_setImplementation(m, (IMP)my_root);
	say("Settings root list wrapped", 1);
}

extern void *dlopen(const char *, int);

__attribute__((constructor)) static void
start(void)
{
	/* inside Jesvi's two add-ons, which wrap the method only once PreferencesUI is loaded (asynchronously): it is
	 * loaded here and wrapped at once, so theirs go around this one and keep its rows */
	(void)dlopen("/System/Library/PrivateFrameworks/PreferencesUI.framework/PreferencesUI", 1);
	wrap(0);
}
