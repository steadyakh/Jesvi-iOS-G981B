/*
 * Settings' tweak panes on the G981B. PreferenceLoader adds every tweak's pane to Settings by hooking
 * -[PSUIPrefsListController specifiers] when it is loaded -- but libredwood-tweaks opens the tweaks before Settings has
 * loaded PreferencesUI, where that class lives, so the hook found no class and no pane ever showed. This library is
 * opened first (its name sorts before the others in TweakInject) and loads PreferencesUI, so the class is there when
 * PreferenceLoader looks. Settings would load the framework a moment later anyway; nothing else is changed.
 */
extern void *dlopen(const char *, int);
extern int open(const char *, int, ...);
extern long write(int, const void *, unsigned long);
extern int close(int);
extern void *objc_getClass(const char *);

__attribute__((constructor)) static void
start(void)
{
	static const char yes[] = "x1s-early: PreferencesUI loaded before the tweaks\n";
	static const char no[] = "x1s-early: PreferencesUI could not be loaded\n";
	int fd;

	(void)dlopen("/System/Library/PrivateFrameworks/PreferencesUI.framework/PreferencesUI", 1 /* RTLD_LAZY */);
	fd = open("/dev/console", 0x20001);
	if (fd >= 0) {
		if (objc_getClass("PSUIPrefsListController")) write(fd, yes, sizeof(yes) - 1);
		else write(fd, no, sizeof(no) - 1);
		close(fd);
	}
}
