/*
 * Settings > General > iPhone Storage on the G981B: Settings sizes the phone by the root volume ("/"), which here is
 * only the 2.5 GB system image read into memory (statfs "/": 2520 MB, 0 MB free), while the user's data is an HFS+
 * volume of its own at /private/var (about 105 GB). On a real iPhone both are one APFS container. A tweak for
 * Settings alone: statfs of "/" answers with the two together -- capacity = system + data, free = the data
 * volume's free space. Nothing is written anywhere; every other path is answered as before.
 *
 * Built on Windows with LLVM (tools/x1s/storagefix/build.sh); hooks through ElleKit's MSHookFunction, found at run time.
 */
typedef unsigned int uint32_t;
typedef int int32_t;
typedef unsigned long long uint64_t;
typedef unsigned long size_t;

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

extern void *dlopen(const char *, int);
extern void *dlsym(void *, const char *);
extern int strcmp(const char *, const char *);

#define RTLD_DEFAULT ((void *)-2)

typedef int (*statfs_fn)(const char *, struct statfs *);
static statfs_fn real_statfs, real_statfs64;

static void
merge(const char *path, struct statfs *s, statfs_fn real)
{
	struct statfs data;

	if (path == 0 || s == 0 || strcmp(s->f_mntonname, "/") != 0 || real("/private/var", &data) != 0 ||
	    strcmp(data.f_mntonname, "/private/var") != 0 || s->f_bsize == 0) {
		return;
	}
	s->f_blocks = s->f_blocks + data.f_blocks * data.f_bsize / s->f_bsize;
	s->f_bfree = data.f_bfree * data.f_bsize / s->f_bsize;
	s->f_bavail = data.f_bavail * data.f_bsize / s->f_bsize;
}

static int
my_statfs(const char *path, struct statfs *s)
{
	int r = real_statfs(path, s);
	if (r == 0) merge(path, s, real_statfs);
	return r;
}

static int
my_statfs64(const char *path, struct statfs *s)
{
	int r = real_statfs64(path, s);
	if (r == 0) merge(path, s, real_statfs64);
	return r;
}

__attribute__((constructor)) static void
start(void)
{
	void (*hook)(void *, void *, void **);
	void *h = dlopen("/var/jb/usr/lib/libsubstrate.dylib", 1);
	void *a = dlsym(RTLD_DEFAULT, "statfs"), *b = dlsym(RTLD_DEFAULT, "statfs64");

	hook = h ? (void (*)(void *, void *, void **))dlsym(h, "MSHookFunction") : 0;
	if (hook == 0) {
		return;
	}
	if (a) hook(a, (void *)my_statfs, (void **)&real_statfs);
	if (b && b != a) hook(b, (void *)my_statfs64, (void **)&real_statfs64);
}
