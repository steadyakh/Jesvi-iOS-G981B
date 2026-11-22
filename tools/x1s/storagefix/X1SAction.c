/*
 * The Action button on the G981B: holding the side (power) button runs the action chosen in Settings > Action Button
 * (X1SAction.plist, PreferenceLoader), as an iPhone 15 Pro's Action button does. "No Action" (or nothing chosen)
 * leaves the long press to iOS (Siri). SpringBoard only.
 *
 * Settings write com.x1s.actionbutton: action = silent | flashlight | camera | voicememos | magnifier | shortcut |
 * screenshot | translate | none; shortcut = the name of the shortcut to run.
 */
typedef unsigned long size_t;
typedef void *id, *SEL, *Class, *Method, *IMP;
typedef signed char BOOL;

extern id objc_msgSend(id, SEL, ...);
extern Class objc_getClass(const char *);
extern SEL sel_registerName(const char *);
extern Method class_getInstanceMethod(Class, SEL);
extern IMP method_setImplementation(Method, IMP);
extern void _dyld_register_func_for_add_image(void (*)(const void *, long));
extern int open(const char *, int, ...);
extern long write(int, const void *, size_t);
extern int close(int);
extern int snprintf(char *, size_t, const char *, ...);
extern int strcmp(const char *, const char *);

#define S(n) sel_registerName(n)
#define M0(o, s) (((id (*)(id, SEL))objc_msgSend)((id)(o), S(s)))
#define M1(o, s, a) (((id (*)(id, SEL, id))objc_msgSend)((id)(o), S(s), (id)(a)))
#define M2(o, s, a, b) (((id (*)(id, SEL, id, id))objc_msgSend)((id)(o), S(s), (id)(a), (id)(b)))
#define STR(c) (((id (*)(id, SEL, const char *))objc_msgSend)((id)objc_getClass("NSString"), S("stringWithUTF8String:"), (c)))
#define CSTR(o) (((const char *(*)(id, SEL))objc_msgSend)((id)(o), S("UTF8String")))
#define RESPONDS(o, s) ((((BOOL (*)(id, SEL, SEL))objc_msgSend)((id)(o), S("respondsToSelector:"), S(s))) != 0)

static void (*old_long_press)(id, SEL);
static int done;

static void
say(const char *what)
{
	char line[300];
	int fd = open("/dev/console", 0x20001), n = snprintf(line, sizeof(line), "x1s-action: %s%c", what, 10);
	if (fd >= 0) { write(fd, line, (size_t)n); close(fd); }
}

static id
pref(const char *key)
{
	id d = M1(M0(objc_getClass("NSUserDefaults"), "alloc"), "initWithSuiteName:", STR("com.x1s.actionbutton"));
	id v = d ? M1(d, "objectForKey:", STR(key)) : 0;
	return v;
}

static void
tap(void)
{
	/* a firm tap, as the Action button gives */
	id g = ((id (*)(id, SEL, long))objc_msgSend)(M0(objc_getClass("UIImpactFeedbackGenerator"), "alloc"), S("initWithStyle:"), 1);
	if (g) M0(g, "impactOccurred");
}

static void
open_app(const char *bundle)
{
	id ws = M0(objc_getClass("LSApplicationWorkspace"), "defaultWorkspace");
	if (ws) M1(ws, "openApplicationWithBundleID:", STR(bundle));
}

static void
open_url(id url_string)
{
	id ws = M0(objc_getClass("LSApplicationWorkspace"), "defaultWorkspace");
	id url = M1(objc_getClass("NSURL"), "URLWithString:", url_string);
	if (ws && url) M2(ws, "openSensitiveURL:withOptions:", url, 0);
}

static void
long_press(id self, SEL cmd)
{
	id a = pref("action");
	const char *action = a ? CSTR(a) : 0;

	if (!action)
		action = "silent";            /* the default, as an iPhone ships it */
	if (!strcmp(action, "none")) {
		old_long_press(self, cmd);
		return;
	}
	say(action);
	tap();
	if (!strcmp(action, "silent")) {
		id sb = M0(objc_getClass("UIApplication"), "sharedApplication");
		id ringer = sb && RESPONDS(sb, "ringerControl") ? M0(sb, "ringerControl") : 0;
		if (ringer) M0(ringer, "toggleRingerMute");
	} else if (!strcmp(action, "flashlight")) {
		id fl = M0(objc_getClass("SBUIFlashlightController"), "sharedInstance");
		if (fl) {
			float level = RESPONDS(fl, "level") ? ((float (*)(id, SEL))objc_msgSend)(fl, S("level")) :
			    RESPONDS(fl, "flashlightLevel") ? ((float (*)(id, SEL))objc_msgSend)(fl, S("flashlightLevel")) : 0;
			if (RESPONDS(fl, "turnFlashlightOnForReason:")) {
				if (level > 0) M1(fl, "turnFlashlightOffForReason:", STR("x1s-action"));
				else M1(fl, "turnFlashlightOnForReason:", STR("x1s-action"));
			}
		}
	} else if (!strcmp(action, "camera")) {
		open_app("com.apple.camera");
	} else if (!strcmp(action, "voicememos")) {
		open_app("com.apple.VoiceMemos");
	} else if (!strcmp(action, "magnifier")) {
		open_app("com.apple.Magnifier");
	} else if (!strcmp(action, "translate")) {
		open_app("com.apple.Translate");
	} else if (!strcmp(action, "screenshot")) {
		id sb = M0(objc_getClass("UIApplication"), "sharedApplication");
		if (sb && RESPONDS(sb, "takeScreenshot")) M0(sb, "takeScreenshot");
	} else if (!strcmp(action, "shortcut")) {
		id name = pref("shortcut");
		if (name && ((size_t (*)(id, SEL))objc_msgSend)(name, S("length"))) {
			id allowed = M0(objc_getClass("NSCharacterSet"), "URLQueryAllowedCharacterSet");
			id escaped = M1(name, "stringByAddingPercentEncodingWithAllowedCharacters:", allowed);
			open_url(M1(STR("shortcuts://run-shortcut?name="), "stringByAppendingString:", escaped));
		} else {
			open_url(STR("shortcuts://run-shortcut?name=Action%20Button"));
		}
	}
}

static void
added(const void *mh, long slide)
{
	Class c;
	Method m;

	(void)mh; (void)slide;
	if (done || !(c = objc_getClass("SBLockHardwareButtonActions")))
		return;
	if (!(m = class_getInstanceMethod(c, S("performLongPressActions"))))
		return;
	done = 1;
	old_long_press = (void (*)(id, SEL))method_setImplementation(m, (IMP)long_press);
	say("side button long press: Action button");
}

__attribute__((constructor)) static void
start(void)
{
	_dyld_register_func_for_add_image(added);
}
