/*
 * Settings > General > iPhone Storage on the G981B. The page (StorageSettings.framework, STStorageDiskMonitor) sizes the
 * phone by the root volume, which here is only the 2.5 GB system image read into memory, while the user's data is an
 * HFS+ volume of its own at /private/var (about 105 GB); on a real iPhone both are one APFS container. A tweak for
 * Settings alone: STStorageDiskMonitor's totalBytes, availableBytes and usedBytes answer for the two together --
 * total = system + data, available = the data volume's free space. Nothing is written anywhere.
 *
 * Only Objective-C methods are replaced (method_setImplementation), when StorageSettings is loaded: the first try
 * hooked statfs itself through ElleKit and Settings died at once (SIGSYS: the system call's own stub was patched).
 * Built on Windows with LLVM (build.sh), linked against stubs of libSystem and libobjc (*.tbd).
 */
typedef unsigned int uint32_t;
typedef int int32_t;
typedef unsigned long long uint64_t;
typedef long long int64_t;

struct statfs {                 /* <sys/mount.h>, 64-bit inode layout (arm64) */
	uint32_t f_bsize;
	int32_t f_iosize;
	uint64_t f_blocks, f_bfree, f_bavail, f_files, f_ffree;
	int32_t f_fsid[2];
	uint32_t f_owner, f_type, f_flags, f_fssubtype;
	char f_fstypename[16];
	char f_mntonname[1024];
	char f_mntfromname[1024];
	uint32_t f_flags_ext;
	uint32_t f_reserved[7];
};

typedef void *id;
typedef void *SEL;
typedef void *Class;
typedef void *Method;
typedef void *IMP;
struct mach_header;

extern int statfs(const char *, struct statfs *);
extern int strcmp(const char *, const char *);
extern void _dyld_register_func_for_add_image(void (*)(const struct mach_header *, long));
extern Class objc_getClass(const char *);
extern SEL sel_registerName(const char *);
extern Method class_getInstanceMethod(Class, SEL);
extern IMP method_setImplementation(Method, IMP);
extern int open(const char *, int, ...);
extern Method *class_copyMethodList(Class, unsigned *);
extern SEL method_getName(Method);
extern const char *sel_getName(SEL);
extern Class object_getClass(void *);
extern long write(int, const void *, unsigned long);
extern int close(int);
extern int snprintf(char *, unsigned long, const char *, ...);

/* a line to the console (what the phone's log keeps), as libredwood-tweaks does */
static void
note(const char *what, long long value)
{
	char line[200];
	int fd = open("/dev/console", 0x20001), n = snprintf(line, sizeof(line), "x1s-storage: %s %lld%c", what, value, 10);

	if (fd >= 0) {
		write(fd, line, (unsigned long)n);
		close(fd);
	}
}

static int done;

/* system + data, and the data volume's free space; 0 when either cannot be read (then Apple's answer stands) */
static int
sizes(int64_t *total, int64_t *avail)
{
	struct statfs root, data;

	if (statfs("/", &root) != 0 || statfs("/private/var", &data) != 0 || strcmp(data.f_mntonname, "/private/var") != 0 ||
	    strcmp(root.f_mntonname, "/") != 0) {
		return 0;
	}
	*total = (int64_t)(root.f_blocks * root.f_bsize + data.f_blocks * data.f_bsize);
	*avail = (int64_t)(data.f_bavail * data.f_bsize);
	return 1;
}

/* STStorageSpace is made with the sizes the page shows (initWithTotal:free:purgeable:); STStorageDiskMonitor's
 * deviceSize is the capacity line. Both get system + data and the data volume's free space. */
static id (*old_init)(id, SEL, int64_t, int64_t, int64_t);
static int64_t (*old_device)(id, SEL);

static id
my_init(id self, SEL cmd, int64_t total, int64_t free_bytes, int64_t purgeable)
{
	int64_t t, a;

	if (sizes(&t, &a)) {
		note("space made, total was", total);
		total = t;
		free_bytes = a;
	}
	return old_init(self, cmd, total, free_bytes, purgeable);
}

static int64_t
my_device(id self, SEL cmd)
{
	int64_t t, a;
	return sizes(&t, &a) ? t : old_device(self, cmd);
}

static void
replace(Class c, const char *name, IMP mine, void *old)
{
	Method m = c ? class_getInstanceMethod(c, sel_registerName(name)) : 0;

	if (m != 0) {
		*(IMP *)old = method_setImplementation(m, mine);
	}
	note(name, m != 0);
}

static void
added(const struct mach_header *mh, long slide)
{
	(void)mh;
	(void)slide;
	if (done || objc_getClass("STStorageSpace") == 0 || objc_getClass("STStorageDiskMonitor") == 0) {
		return;
	}
	done = 1;
	replace(objc_getClass("STStorageSpace"), "initWithTotal:free:purgeable:", (IMP)my_init, &old_init);
	replace(objc_getClass("STStorageDiskMonitor"), "deviceSize", (IMP)my_device, &old_device);
}

__attribute__((constructor)) static void
start(void)
{
	/* called for every image already loaded, and for each one loaded later (StorageSettings, when the page opens) */
	_dyld_register_func_for_add_image(added);
}
