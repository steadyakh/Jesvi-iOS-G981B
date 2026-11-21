/*
 * XNU loader: a UEFI application that plays the role of iBoot for an
 * open-source arm64 XNU kernel on non-Apple hardware.
 *
 *   - finds the "XNUP" payload in RAM (placed there by the boot shim on the
 *     phone, by `-device loader` under QEMU) and the Mach-O kernel inside it
 *   - picks a block of free RAM to be XNU's [physBase, physBase + memSize)
 *   - copies the Mach-O segments there, builds an Apple-format device tree
 *     and a boot_args structure behind the kernel
 *   - exits boot services, turns the MMU off and jumps to the kernel entry
 */
#include "efi.h"
#include "s20_memory.h"
#include "s20_efi_memory.h"
#ifdef CELLULAR_SPI_LOOPBACK
#include "../tools/cellular/s5123-spi.h"
#endif
#ifdef CELLULAR_CP_GPIO_PREFLIGHT
#include "../tools/cellular/s5123-gpio-fields.h"
#endif
#include "identity.h"         /* who iOS shall think we are: from the iPhone's own device tree (mkidentity.py) */

/* ------------------------------------------------------------------------ */
/* Fixed conventions shared with the boot shim, the packer and the kernel   */

/*
 * The XNUP payload is embedded in the unused tail of the UEFI FD image itself,
 * so the stock boot shim delivers it together with the FD (no custom shim, and
 * no image_size mismatch for the bootloader).
 */
#define PAYLOAD_BASE        0xE8100000ULL         /* FD base 0xE8000000 + 0x100000 */
#define PAYLOAD_MAX_SIZE    0x00080000ULL         /* 512 KB, inside the 2 MB FD region */
#define PAYLOAD_MAGIC       0x50554E58u /* "XNUP" */

struct payload_header { uint32_t magic; uint32_t size; };
struct payload_entry { char name[16]; uint32_t size; uint32_t reserved; };

#define KERNEL_VIRT_BASE    0xFFFFFFF004000000ULL /* boot_args.virtBase */
#define KERNEL_LINK_OFFSET  0x03000000ULL         /* kernel text sits 48 MB above the base */
#define BASE_ALIGN          0x02000000ULL         /* 32 MB: L2 block size with 16K pages */
#define MAX_MEM_SIZE        0x180000000ULL        /* 6 GB; the kernel's bootstrap page tables cover 8 */
/*
 * A root image too big for the payload area is placed here, at a fixed
 * offset inside the kernel's memory window, once; it then survives any
 * number of kernel runs (nothing writes to it: the root is read-only).
 */
#define RESIDENT_RD_OFFSET  0x08000000ULL
#define RESIDENT_RD_RESERVE 0x30000000ULL         /* RAM that must remain for the kernel itself */
#define PAGE_SIZE_MAX       0x4000ULL

/*
 * x1slte (SM-G980F): the log goes into super's free tail, not into recovery. sboot hung at the Samsung logo on cold
 * starts after runs had written their log into recovery (twice, 2026-11-05; a fresh flash of recovery cured it), so
 * nothing at runtime writes recovery any more. -DLOG_IN_RECOVERY=1 brings back y2s's place.
 */
#ifndef SUPER_TAIL_START
#define SUPER_TAIL_START 0x1E8B04000ULL
#endif
#if LOG_IN_BOOT
/*
 * x1s (SM-G981B): the owner's rule is that only the boot partition is ever written. The image there is laid out like
 * x1slte's recovery image (UEFI at 0, kernel at 32 MB, root at 52 MB), so the slot at 16 MB is free and outside the
 * AVB-hashed head. Recovery (TWRP) and super (the earlier port's system volume) are never written.
 */
#define LOG_PARTITION       u"boot"
#define LOG_PARTITION_ASCII "boot"
#define LOG_OFFSET          0x01000000ULL /* 16 MB into boot: clear of the UEFI image (0) and the kernel (32 MB) */
#elif LOG_IN_RECOVERY
#define LOG_PARTITION       u"recovery"   /* y2s has no logdump: park the log in the recovery image */
#define LOG_PARTITION_ASCII "recovery"
#define LOG_OFFSET          0x01000000ULL /* 16 MB into recovery: clear of the UEFI image (0) and the kernel (32 MB) */
#else
#define LOG_PARTITION       u"super"
#define LOG_PARTITION_ASCII "super"
#define LOG_OFFSET          SUPER_TAIL_START /* super's free tail (1.2 GB past its last logical partition): 8 MB */
#endif
#define CHECKPOINT_SELFTEST 999
#define PANIC_LOG_SIZE      0x80000u
#define NVRAM_BANK_SIZE     0x2000u
#define PANIC_LOG_BASE      (PAYLOAD_BASE + PAYLOAD_MAX_SIZE)
/* Allocated staging; 0xf0000000 belongs to the S20 modem IPC, not downloads. */
static uint64_t download_base;
#define DOWNLOAD_MAX_SIZE   0x01000000ULL

/* ------------------------------------------------------------------------ */
/* XNU structures (pexpert/pexpert/arm64/boot.h, mach-o/loader.h)           */

struct boot_video {
	uint64_t v_baseAddr, v_display, v_rowBytes, v_width, v_height, v_depth;
};

#define BOOT_LINE_LENGTH 608

struct boot_args {
	uint16_t revision;
	uint16_t version;
	uint64_t virtBase;
	uint64_t physBase;
	uint64_t memSize;
	uint64_t topOfKernelData;
	struct boot_video video;
	uint32_t machineType;
	uint64_t deviceTreeP;
	uint32_t deviceTreeLength;
	char commandLine[BOOT_LINE_LENGTH];
	uint64_t bootFlags;
	uint64_t memSizeActual;
};

#define MH_MAGIC_64      0xFEEDFACFu
#define LC_SEGMENT_64    0x19
#define LC_UNIXTHREAD    0x5

struct mach_header_64 { uint32_t magic, cputype, cpusubtype, filetype, ncmds, sizeofcmds, flags, reserved; };
struct load_command { uint32_t cmd, cmdsize; };
struct segment_command_64 {
	uint32_t cmd, cmdsize; char segname[16];
	uint64_t vmaddr, vmsize, fileoff, filesize;
	uint32_t maxprot, initprot, nsects, flags;
};

/* ------------------------------------------------------------------------ */
/* Globals and small helpers                                                */

static efi_system_table *ST;
static efi_boot_services *BS;
static efi_handle IMAGE;

static char log_buf[32768];
static uint32_t log_len;

void *
memset(void *dst, int c, size_t n)
{
	uint8_t *d = dst;
	while (n--) {
		*d++ = (uint8_t)c;
	}
	return dst;
}

int
memcmp(const void *a, const void *b, size_t n)
{
	const uint8_t *x = a, *y = b;

	for (; n > 0; n--, x++, y++) {
		if (*x != *y) {
			return *x < *y ? -1 : 1;
		}
	}
	return 0;
}

void *
memcpy(void *dst, const void *src, size_t n)
{
	uint8_t *d = dst;
	const uint8_t *s = src;

	/*
	 * Word at a time when possible: USB transfer buffers are uncached, where
	 * a byte loop costs one bus access per byte (6 MB/s instead of 30+).
	 */
	if ((((uintptr_t)d | (uintptr_t)s) & 7) == 0) {
		uint64_t *dw = (uint64_t *)d;
		const uint64_t *sw = (const uint64_t *)s;

		for (; n >= 32; n -= 32, dw += 4, sw += 4) {
			uint64_t a = sw[0], b = sw[1], c = sw[2], e = sw[3];
			dw[0] = a;
			dw[1] = b;
			dw[2] = c;
			dw[3] = e;
		}
		for (; n >= 8; n -= 8) {
			*dw++ = *sw++;
		}
		d = (uint8_t *)dw;
		s = (const uint8_t *)sw;
	}
	while (n--) {
		*d++ = *s++;
	}
	return dst;
}

static size_t
str_len(const char *s)
{
	size_t n = 0;
	while (s[n]) {
		n++;
	}
	return n;
}

static int
str_eq(const char *a, const char *b)
{
	while (*a && *a == *b) {
		a++, b++;
	}
	return *a == *b;
}

/*
 * Set once the loader takes commands over USB: its log travels that way, and
 * the screen is left to the kernel (and to "oem screenshot").
 */
static int screen_quiet;
/*
 * y2s: once the kernel has run, the firmware's console is not to be trusted until the log is in the
 * partition -- its state has been found corrupted after a run, and the first character printed then
 * took the loader down with the log unsaved (24 Oct, twice). Lines go to the log buffer only.
 */
static int console_suspect;

static void
put_char(char c)
{
	char16 w[3] = { 0, 0, 0 };

	if (log_len < sizeof(log_buf) - 1) {
		log_buf[log_len++] = c;
	}
	if (ST->con_out != NULL && !screen_quiet && !console_suspect) {
		if (c == '\n') {
			w[0] = '\r', w[1] = '\n';
		} else {
			w[0] = (char16)c;
		}
		ST->con_out->output_string(ST->con_out, w);
	}
}

/* printf subset: %s %x %d (64-bit values), %% */
static void
print(const char *fmt, ...)
{
	__builtin_va_list ap;
	__builtin_va_start(ap, fmt);
	for (; *fmt; fmt++) {
		if (*fmt != '%') {
			put_char(*fmt);
			continue;
		}
		fmt++;
		if (*fmt == 's') {
			const char *s = __builtin_va_arg(ap, const char *);
			while (*s) {
				put_char(*s++);
			}
		} else if (*fmt == 'x' || *fmt == 'd') {
			uint64_t v = __builtin_va_arg(ap, uint64_t);
			unsigned base = (*fmt == 'x') ? 16 : 10;
			char tmp[24];
			int i = 0;
			do {
				tmp[i++] = "0123456789abcdef"[v % base];
				v /= base;
			} while (v);
			while (i) {
				put_char(tmp[--i]);
			}
		} else {
			put_char(*fmt);
		}
	}
	__builtin_va_end(ap);
}

#define READ_SYSREG(name) ({ uint64_t _v; __asm__ volatile ("mrs %0, " #name : "=r" (_v)); _v; })

/*
 * CNTFRQ_EL0 is read-only from EL1 (an msr traps), and some Samsung firmware
 * leaves it at 0. The Exynos 990 arch timer runs at 26 MHz, so fall back to
 * that: otherwise every timeout below is a division by zero and the device
 * tree would advertise timebase-frequency = 0 to XNU.
 */
static uint64_t
timer_freq(void)
{
	uint64_t f;
	__asm__ volatile ("mrs %0, CNTFRQ_EL0" : "=r" (f));
	return f != 0 ? f : 26000000ULL;
}

/*
 * Write back and drop cached copies of a range. Required for memory the
 * kernel will touch with the MMU (and so the data cache) off: its early code
 * builds page tables that way, and a stale cached copy of those pages would
 * be what the table walker sees once caches are turned on again.
 */
static void
flush_dcache(uint64_t start, uint64_t end)
{
	uint64_t line = 4ULL << ((READ_SYSREG(CTR_EL0) >> 16) & 0xF);
	for (uint64_t p = start & ~(line - 1); p < end; p += line) {
		__asm__ volatile ("dc civac, %0" : : "r" (p));
	}
	__asm__ volatile ("dsb sy");
}

static void
clean_dcache(uint64_t start, uint64_t end)
{
	uint64_t line = 4ULL << ((READ_SYSREG(CTR_EL0) >> 16) & 0xF);
	for (uint64_t p = start & ~(line - 1); p < end; p += line) {
		__asm__ volatile ("dc cvac, %0" : : "r" (p));
	}
	__asm__ volatile ("dsb sy");
}

/* ------------------------------------------------------------------------ */
/* Log persistence: raw write to a GPT partition (phone only)               */

static efi_block_io *
find_partition(const char16 *want)
{
	static const efi_guid part_guid = EFI_PARTITION_INFO_GUID;
	static const efi_guid blk_guid = EFI_BLOCK_IO_GUID;
	efi_handle *handles;
	uint64_t count;

	if (EFI_ERROR(BS->locate_handle_buffer(ByProtocol, &part_guid, NULL, &count, &handles))) {
		return NULL;
	}
	for (uint64_t i = 0; i < count; i++) {
		efi_partition_info *info;
		efi_block_io *bio;
		int match = 1;

		if (EFI_ERROR(BS->handle_protocol(handles[i], &part_guid, (void **)&info)) || info->type != 2) {
			continue;
		}
		for (unsigned k = 0; k < 36; k++) {
			if (info->name[k] != want[k]) {
				match = 0;
				break;
			}
			if (want[k] == 0) {
				break;
			}
		}
		if (match && !EFI_ERROR(BS->handle_protocol(handles[i], &blk_guid, (void **)&bio))) {
			return bio;
		}
	}
	return NULL;
}

/* ASCII (payload entry) to the UTF-16 name a GPT partition carries. */
static efi_block_io *
find_partition_ascii(const char *name)
{
	char16 buf[37];
	unsigned i;

	for (i = 0; i < 36 && name[i] != 0; i++) {
		buf[i] = (char16)(uint8_t)name[i];
	}
	buf[i] = 0;
	return find_partition(buf);
}

static efi_block_io *
find_log_partition(void)
{
	return find_partition(LOG_PARTITION);
}

/*
 * The "staged" buffer holds the loader log followed by the kernel log of the
 * last run. It is what gets written to the log partition and what the host
 * fetches over USB (fastboot get_staged).
 */
static char out_buf[8 * 1024 * 1024];      /* recovery+16 MB up to the kernel at +32 MB has room for it */
/* the storage's reads for the kernel land here (block_poll); also where the log is read back */
#define BLOCK_BUFFER_SIZE  (4u << 20)
static uint8_t block_buffer[BLOCK_BUFFER_SIZE] __attribute__((aligned(0x2000)));
static const char *staged_data = out_buf;   /* what "upload" sends: normally the log in out_buf */
static uint64_t staged_len;

static void
compose_logs(const char *klog, uint64_t klog_len)
{
	static const char sep[] = "\n== KERNEL LOG ==\n", end[] = "\n== END OF LOG ==\n";
	uint64_t n = log_len;

	staged_data = out_buf;
	memset(out_buf, 0, sizeof(out_buf));
	memcpy(out_buf, log_buf, log_len);
	if (klog != NULL) {
		if (klog_len > sizeof(out_buf) - n - 8192) {
			klog_len = sizeof(out_buf) - n - 8192;
		}
		memcpy(out_buf + n, sep, sizeof(sep) - 1);
		n += sizeof(sep) - 1;
		memcpy(out_buf + n, klog, klog_len);
		n += klog_len;
		memcpy(out_buf + n, end, sizeof(end) - 1);
		n += sizeof(end) - 1;
	}
	staged_len = n;
}

static int hc_list_is_uefis(void);
static int hc_log_skips;                /* logs not written this run (said twice at most) */
static void hc_print_state(const char *when);
static void hc_take_back(const char *when);

#define LOG_SCREEN_TAIL     (16 * 1024)   /* bytes of the kernel log shown on the screen after a run */

static int store_logs(efi_block_io *bio, const char *klog, uint64_t klog_len);

static void
write_logs(const char *klog, uint64_t klog_len)
{
	efi_block_io *bio = find_log_partition();

	compose_logs(klog, klog_len);
	/*
	 * Into the partition first, on the screen after: printing a whole iOS boot log through the
	 * firmware's console takes minutes on the phone's 1440 x 3200 panel, and a reset during that
	 * used to lose the log.
	 */
	int stored = store_logs(bio, klog, klog_len);

	if (klog != NULL) {
		console_suspect = 0;    /* the kernel's log is safe (or cannot be made so): the screen may have it now */
	}

	if (klog != NULL) {
		/*
		 * Show the log on a clean screen: the kernel draws its own console straight into the
		 * framebuffer (with no font of ours), so the firmware's console would otherwise print
		 * over the mess it leaves behind. Only its end: the whole of it is in the partition.
		 */
		uint64_t from = klog_len > LOG_SCREEN_TAIL ? klog_len - LOG_SCREEN_TAIL : 0;

		if (ST->con_out != NULL && ST->con_out->clear_screen != NULL) {
			ST->con_out->clear_screen(ST->con_out);
		}
		print("== KERNEL LOG (last %d of %d bytes) ==\n", klog_len - from, klog_len);
		for (uint64_t i = from; i < klog_len; i++) {
			put_char(klog[i]);
		}
		print("\n== END OF LOG ==\n");
		print(stored ? "log: saved to " LOG_PARTITION_ASCII "+%x before it was shown: the phone may be reset now\n" :
		    "log: NOT SAVED to " LOG_PARTITION_ASCII "+%x (see above): only this screen has it\n", LOG_OFFSET);
	}
}

static int
store_logs(efi_block_io *bio, const char *klog, uint64_t klog_len)
{
	if (bio == NULL) {
		return 0;
	}
	uint64_t bs = bio->media->block_size;
	uint64_t len = (staged_len + 1 + bs - 1) / bs * bs;
	uint64_t lba = bs != 0 ? LOG_OFFSET / bs : 0;
	efi_status ws, rs = 0;
	int same = 0;

	/* only through UEFI's own transfer list (see hc_take_back) */
	if (!hc_list_is_uefis()) {
		if (hc_log_skips++ < 2) {
			hc_print_state("log not written");
			print("log: NOT WRITTEN to the log partition: UEFI's transfer list is not the one in the controller\n");
		}
		compose_logs(klog, klog_len);
		return 0;
	}
	ws = bio->write_blocks(bio, bio->media->media_id, lba, len, out_buf);

	if (!EFI_ERROR(ws)) {
		bio->flush_blocks(bio);
		/*
		 * And read back: that the log is in the partition is what a run of the kernel's own UFS
		 * driver must leave possible (the controller is shared with this one). The line goes into
		 * the copy the host fetches over USB, not into the partition, which already has the rest.
		 */
		if (len <= sizeof(block_buffer)) {
			rs = bio->read_blocks(bio, bio->media->media_id, lba, len, block_buffer);
			same = !EFI_ERROR(rs) && memcmp(block_buffer, out_buf, len) == 0;
		}
	}
	print("log: %d bytes written to " LOG_PARTITION_ASCII "+%x (%x), read back %s (%x)\n", len, LOG_OFFSET, ws,
	    EFI_ERROR(ws) ? "not tried" : same ? "the same" : "DIFFERENT", rs);
	compose_logs(klog, klog_len);
	return !EFI_ERROR(ws);
}

static void
flush_log(void)
{
	write_logs(NULL, 0);
}

/* ------------------------------------------------------------------------ */
/* Apple device tree writer                                                 */
/*                                                                          */
/* node := u32 nProperties, u32 nChildren, props[], children[]              */
/* prop := char name[32], u32 length, value padded to 4 bytes               */

struct dt_writer {
	uint8_t *base, *cur;
	uint32_t *counts[16]; /* [depth] -> {nProperties, nChildren} of the open node */
	int depth;
};

static void
dt_prop(struct dt_writer *w, const char *name, const void *data, uint32_t len)
{
	memset(w->cur, 0, 32);
	memcpy(w->cur, name, str_len(name));
	*(uint32_t *)(w->cur + 32) = len;
	memset(w->cur + 36, 0, (len + 3) & ~3u);
	memcpy(w->cur + 36, data, len);
	w->cur += 36 + ((len + 3) & ~3u);
	w->counts[w->depth - 1][0]++;
}

static void
dt_prop_str(struct dt_writer *w, const char *name, const char *value)
{
	dt_prop(w, name, value, (uint32_t)str_len(value) + 1);
}

static void
dt_prop_u32(struct dt_writer *w, const char *name, uint32_t value)
{
	dt_prop(w, name, &value, sizeof(value));
}

static void
dt_prop_u64(struct dt_writer *w, const char *name, uint64_t value)
{
	dt_prop(w, name, &value, sizeof(value));
}

/* Seconds since 1970 by the firmware's real-time clock, or 0 when it has none or it is not set. */
static uint64_t
firmware_epoch(void)
{
	static const uint16_t before_month[12] = { 0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334 };
	struct efi_time t;
	uint64_t days;

	if (ST->rt == NULL || ST->rt->get_time == NULL || EFI_ERROR(ST->rt->get_time(&t, NULL)) ||
	    t.year < 2024 || t.year > 2099 || t.month < 1 || t.month > 12) {
		return 0;
	}
	days = (uint64_t)(t.year - 1970) * 365 + (t.year - 1969) / 4 + before_month[t.month - 1] + (t.day - 1);
	if (t.month > 2 && t.year % 4 == 0) {
		days++;
	}
	uint64_t secs = days * 86400 + (uint64_t)t.hour * 3600 + (uint64_t)t.minute * 60 + t.second;
	if (t.timezone != 0x7FF) {
		secs += (int64_t)t.timezone * 60;       /* local time = UTC - timezone */
	}
	return secs;
}

/* A string in a field of fixed size, the rest zeros. */
static void
dt_prop_padded(struct dt_writer *w, const char *name, const char *value, uint32_t size)
{
	uint8_t field[64];

	memset(field, 0, sizeof(field));
	memcpy(field, value, str_len(value));
	dt_prop(w, name, field, size);
}

/*
 * Properties copied from the iPhone's device tree (identity.h), but one: the iPhone X's display is a
 * wide-gamut one ("artwork-display-gamut" P3), and with it iOS takes the screen for one that shows
 * extended colour (MobileGestalt's HasExtendedColorDisplay): WebKit then draws a page's tiles into
 * surfaces of ten bits a colour ('w30r', 'b3a8'), which neither the kernel's surfaces nor the window
 * server's renderer here do (eight bits, BGRA), and every page comes out black. Here the screen is
 * what the framebuffer is: sRGB.
 */
static int gamut_p3;    /* boot-arg gamut=p3 (a test): the iPhone's own "P3" left in place */
/*
 * boot-arg island=1: the iPhone 14 Pro's "artwork-device-subtype" (2556) in place of the iPhone X's
 * (2436). MobileGestalt's ArtworkDeviceSubType comes from it, and by it SpringBoard switches on its own
 * Dynamic Island (SystemAperture, the "Pill" status bar, the sensor-region layout), all of which is in
 * the iPhone X's iOS 16.5 already; the S20's camera hole sits top centre, under the pill.
 */
static int island;

static void
dt_identity(struct dt_writer *w, const struct identity_prop *props, uint32_t count, const unsigned char *data)
{
	for (uint32_t i = 0; i < count; i++) {
		if (str_eq(props[i].name, "artwork-display-gamut") && !gamut_p3) {
			dt_prop(w, props[i].name, "sRGB", 5);
			continue;
		}
		if (str_eq(props[i].name, "artwork-device-subtype") && island) {
			static const uint32_t island_subtype = 2556;
			dt_prop(w, props[i].name, &island_subtype, sizeof(island_subtype));
			continue;
		}
		dt_prop(w, props[i].name, data + props[i].offset, props[i].length);
	}
}

/* One property of those copied from the iPhone's device tree, by name. */
static void
dt_identity_one(struct dt_writer *w, const struct identity_prop *props, uint32_t count, const unsigned char *data, const char *name)
{
	for (uint32_t i = 0; i < count; i++) {
		if (str_eq(props[i].name, name)) {
			dt_prop(w, props[i].name, data + props[i].offset, props[i].length);
		}
	}
}

static void
dt_begin(struct dt_writer *w, const char *name)
{
	if (w->depth > 0) {
		w->counts[w->depth - 1][1]++;
	}
	w->counts[w->depth++] = (uint32_t *)w->cur;
	memset(w->cur, 0, 8);
	w->cur += 8;
	dt_prop_str(w, "name", name);
}

static void
dt_end(struct dt_writer *w)
{
	w->depth--;
}

/* ------------------------------------------------------------------------ */
/* Platform description                                                     */

struct platform {
	const char *name;
	uint64_t io_base, io_size;       /* one window covering the SoC peripherals */
	uint64_t gic_dist, gic_redist;   /* GICv3: distributor/redistributor; GICv2: distributor/CPU interface */
	uint64_t uart;                   /* PL011, 0 if none */
	int psci_hvc;                    /* the firmware's power interface is called with HVC, not SMC */
	int log_other_cpu;               /* keep notes in the log partition while a kernel starts on another core */
	int cores;                       /* at most this many processor cores, numbered from 0 ... */
	int core_shift;                  /* ... in this bit of MPIDR, when mpidrs is not given (Qualcomm) */
	const uint64_t *mpidrs;          /* or the MPIDR of each core, by index (Exynos: A55 0-3, A76 4-5, M5 0x100/0x101) */
	int gic_v2;                      /* the interrupt controller is a GICv2 (GIC-400), not a GICv3 */
	uint32_t timeout_scale;          /* a kernel on several cores gets this much longer to wait for a lock, 0: as it is */
	uint32_t small_cores;            /* a bit for each of the slower cores, by core number: "small-core" in the device tree */
	uint64_t fb_base;                /* the framebuffer the firmware reports (GOP): it tells this device apart */
	int watch_quietly;               /* bootcpu=N: no UEFI code on this core while the kernel runs (see run_on_other_cpu) */
};

/*
 * The MPIDR of a core index, and the core index of an MPIDR. Qualcomm numbers its cores in the
 * second affinity field (core << 8); some SoCs (Exynos 990) put several cores in one cluster and
 * one in another, which no single shift expresses -- those give an explicit table (mpidrs).
 */
static uint64_t
plat_mpidr_of(const struct platform *p, int core)
{
	return p->mpidrs ? p->mpidrs[core] : (uint64_t)core << p->core_shift;
}

static int
plat_core_of(const struct platform *p, uint64_t mpidr)
{
	if (p->mpidrs) {
		for (int i = 0; i < p->cores; i++) {
			if (p->mpidrs[i] == mpidr) {
				return i;
			}
		}
		return -1;
	}
	return (int)(mpidr >> p->core_shift);
}

static const struct platform platform_qemu_virt = {
	.name = "qemu-virt", .io_base = 0x08000000, .io_size = 0x08000000,
#ifdef GIC_V2
	/* -M virt,gic-version=2: a GIC-400 (GICv2), distributor at 0x08000000, CPU interface at 0x08010000 */
	.gic_dist = 0x08000000, .gic_redist = 0x08010000, .gic_v2 = 1, .uart = 0x09000000, .psci_hvc = 1,
#else
	.gic_dist = 0x08000000, .gic_redist = 0x080A0000, .uart = 0x09000000, .psci_hvc = 1,
#endif
	.cores = 8, .core_shift = 0,
	/*
	 * QEMU's emulated cores stop each other all the time (every flush of a page from all the
	 * TLBs is a rendezvous of all its CPU threads), and the kernel's patience with a spin lock
	 * is that of real hardware: an eighth of a second by a 24 MHz clock, a twentieth here.
	 */
	.timeout_scale = 20,
};

static const struct platform platform_redwood = {
	/* io_base must be non-zero: XNU treats a zero SoC base as "no SoC" and skips clock setup. */
	.name = "redwood", .io_base = 0x00400000, .io_size = 0x3FC00000,
	.gic_dist = 0x17A00000, .gic_redist = 0x17A60000, .uart = 0, .log_other_cpu = 1,
	.cores = 8, .core_shift = 8,
	/*
	 * Cores 0-3 are Cortex-A55s (1.8 GHz at most), 4-7 Cortex-A78s (2.4 GHz): a big one is about
	 * 2.5 times as fast. The kernel uses the small ones only when the big ones are all busy
	 * (XNU's machine_routines.c, ml_choose_slow_core_partners()).
	 */
	.small_cores = 0x0f,
	.fb_base = 0xE1000000,
};

/*
 * Exynos 990 (Galaxy S20+ 5G): 4 Cortex-A55 in one cluster, 2 Cortex-A76 in the next, and 2
 * Mongoose M5 in the third -- the MPIDRs are not core << 8, so they are given outright.
 */
static const uint64_t platform_y2s_mpidrs[8] = { 0x0, 0x1, 0x2, 0x3, 0x4, 0x5, 0x100, 0x101 };

static const struct platform platform_y2s = {
	/* peripherals span 0x02000000 (IRAM) .. 0x1B000000 (DECON/clock); io_base must be non-zero. */
	.name = "y2s", .io_base = 0x02000000, .io_size = 0x19000000,
	/* GICv2 (GIC-400): distributor and CPU interface (see osfmk/arm64/gic_v2_irq.c). */
	.gic_dist = 0x10101000, .gic_redist = 0x10102000, .gic_v2 = 1, .uart = 0, .log_other_cpu = 1,
	.cores = 8, .mpidrs = platform_y2s_mpidrs,
	/* cores 0-3 are the Cortex-A55s (the little cluster). */
	.small_cores = 0x0f,
	/* the UEFI's Display Reserved window (see the x1s MemoryMapLib). VERIFY: the runtime "display ... at" line. */
	.fb_base = 0xF1000000,
	/*
	 * Mu-Silicium's DXE core, left to take its timer interrupts on core 0 while the kernel started
	 * on core 0x100, jumped through a corrupted event ("evnt" as the PC, 24 Oct). Nothing of it is
	 * needed meanwhile on this phone (no watchdog, charger or USB here), so it is not run at all.
	 */
	.watch_quietly = 1,
};

/*
 * The devices the loader knows, told apart by the framebuffer base their UEFI reports. A port to another
 * device adds its description here (common/new-device.sh writes a starting point, docs/PORTING.md says
 * where each value comes from); a display that matches none is QEMU's ramfb.
 */
static const struct platform *const platforms[] = {
	&platform_redwood,
	&platform_y2s,
};

/* ------------------------------------------------------------------------ */

static void
reboot(void)
{
	hc_take_back("before the reset");
	flush_log();
	ST->rt->reset_system(0 /* EfiResetCold */, 0, 0, NULL);
	for (;;) {
	}
}

/*
 * Return-from-kernel support.
 *
 * Boot services are never exited: UEFI stays resident outside XNU's memory
 * window. loader_save_ctx() records the UEFI execution environment and
 * returns 0. When the kernel is done it jumps to loader_return_stub (mapped
 * 1:1 or with the MMU off) with x0 = PA of its log, x1 = length, x2 = reason
 * (0 = orderly, 1 = early exception, then x3..x7 = ESR, ELR, FAR, LR, SP).
 * The stub switches the MMU off, reinstates the saved environment and makes
 * loader_save_ctx() return a second time, with 1.
 *
 * The EL1 physical timer (CNTP_*) is deliberately left alone: the Qualcomm
 * hypervisor traps it. The CTX_CNTP_* slots are unused.
 */
enum {
	CTX_TTBR0, CTX_TCR, CTX_MAIR, CTX_SCTLR, CTX_VBAR, CTX_CPACR, CTX_SP, CTX_SPSEL, CTX_DAIF,
	CTX_CNTV_CTL, CTX_CNTV_CVAL, CTX_CNTP_CTL, CTX_CNTP_CVAL, CTX_CNTKCTL,
	CTX_X18, CTX_X19, /* ... x30 */
	CTX_LOG_PA = CTX_X19 + 12, CTX_LOG_LEN, CTX_REASON, CTX_ESR, CTX_ELR, CTX_FAR, CTX_LR, CTX_KSP,
	CTX_TPIDR_EL1, CTX_TPIDR_EL0, CTX_TPIDRRO_EL0, CTX_SP_EL0, CTX_CONTEXTIDR, CTX_MDSCR, CTX_FPCR, CTX_FPSR,
	CTX_ICC_PMR, CTX_ICC_IGRPEN1, CTX_ICC_BPR1, CTX_COUNT
};
uint64_t loader_ctx[CTX_COUNT];

/* set when the kernel hands back asking for Log access (CTX_REASON 3, kern.redwood_menu) */
static int y2s_want_log_access;
static int y2s_want_download;
int loader_save_ctx(void);
void loader_return_stub(void);

__asm__ (
        ".text\n"
        ".balign 64\n"
        ".globl loader_save_ctx\n"
        "loader_save_ctx:\n"
        "  adrp x10, loader_ctx\n"
        "  add  x10, x10, :lo12:loader_ctx\n"
        "  mrs x9, ttbr0_el1\n   str x9, [x10, #0]\n"
        "  mrs x9, tcr_el1\n     str x9, [x10, #8]\n"
        "  mrs x9, mair_el1\n    str x9, [x10, #16]\n"
        "  mrs x9, sctlr_el1\n   str x9, [x10, #24]\n"
        "  mrs x9, vbar_el1\n    str x9, [x10, #32]\n"
        "  mrs x9, cpacr_el1\n   str x9, [x10, #40]\n"
        "  mov x9, sp\n          str x9, [x10, #48]\n"
        "  mrs x9, spsel\n       str x9, [x10, #56]\n"
        "  mrs x9, daif\n        str x9, [x10, #64]\n"
        "  mrs x9, cntv_ctl_el0\n  str x9, [x10, #72]\n"
        "  mrs x9, cntv_cval_el0\n str x9, [x10, #80]\n"
        "  mrs x9, cntkctl_el1\n   str x9, [x10, #104]\n"
        /* Qualcomm's UEFI has its own thread scheduler and keeps its current-thread
         * pointer in TPIDR_EL1; the kernel overwrites it, so it must come back intact. */
        "  mrs x9, tpidr_el1\n     str x9, [x10, #280]\n"
        "  mrs x9, tpidr_el0\n     str x9, [x10, #288]\n"
        "  mrs x9, tpidrro_el0\n   str x9, [x10, #296]\n"
        "  mrs x9, contextidr_el1\n str x9, [x10, #312]\n"
        "  mrs x9, mdscr_el1\n     str x9, [x10, #320]\n"
        "  mrs x9, s3_3_c4_c4_0\n          str x9, [x10, #328]\n"
        "  mrs x9, s3_3_c4_c4_1\n          str x9, [x10, #336]\n"
        /* GICv3 CPU interface: the kernel reprograms it for its own interrupts.
         * A GICv2 (GIC-400) has no ICC_* system registers: reading them traps. */
#ifndef GIC_V2
        "  mrs x9, icc_pmr_el1\n     str x9, [x10, #344]\n"
        "  mrs x9, icc_igrpen1_el1\n str x9, [x10, #352]\n"
        "  mrs x9, icc_bpr1_el1\n    str x9, [x10, #360]\n"
#endif
        "  mrs x9, spsel\n"
        "  cbz x9, 1f\n"                  /* SP_EL0 is only accessible while running on SP_EL1 */
        "  mrs x9, sp_el0\n        str x9, [x10, #304]\n"
        "1:\n"
        "  str x18, [x10, #112]\n"
        "  stp x19, x20, [x10, #120]\n"
        "  stp x21, x22, [x10, #136]\n"
        "  stp x23, x24, [x10, #152]\n"
        "  stp x25, x26, [x10, #168]\n"
        "  stp x27, x28, [x10, #184]\n"
        "  stp x29, x30, [x10, #200]\n"
        "  mov x0, #0\n"
        "  ret\n"
        ".balign 64\n"
        ".globl loader_return_stub\n"
        "loader_return_stub:\n"
        "  msr daifset, #0xf\n"
        /* PSTATE bits the kernel may have left set. PAN in particular is fatal:
         * UEFI maps its memory as EL0-accessible, so with PAN every data access
         * (including the exception handler's own stack push) faults. */
        "  .inst 0xd500409f\n"          /* msr PAN, #0 */
        "  .inst 0xd500407f\n"          /* msr UAO, #0 */
        "  mrs x9, sctlr_el1\n"
        "  bic x9, x9, #0x1\n"
        "  bic x9, x9, #0x4\n"
        "  bic x9, x9, #0x1000\n"
        "  msr sctlr_el1, x9\n"
        "  isb\n"
        "  adrp x10, loader_ctx\n"
        "  add  x10, x10, :lo12:loader_ctx\n"
        "  ldr x9, [x10, #0]\n   msr ttbr0_el1, x9\n"
        "  ldr x9, [x10, #8]\n   msr tcr_el1, x9\n"
        "  ldr x9, [x10, #16]\n  msr mair_el1, x9\n"
        "  ldr x9, [x10, #32]\n  msr vbar_el1, x9\n"
        "  ldr x9, [x10, #40]\n  msr cpacr_el1, x9\n"
        "  isb\n"
        "  tlbi vmalle1\n"
        "  dsb sy\n"
        "  ic iallu\n"
        "  isb\n"
        "  ldr x9, [x10, #24]\n  msr sctlr_el1, x9\n"
        "  isb\n"
        "  ldr x9, [x10, #56]\n"
        "  cbz x9, 1f\n"
        "  msr spsel, #1\n"
        "  ldr x9, [x10, #304]\n msr sp_el0, x9\n"
        "  b 2f\n"
        "1:\n"
        "  msr spsel, #0\n"
        "2:\n"
        "  ldr x9, [x10, #48]\n  mov sp, x9\n"
        "  ldr x9, [x10, #280]\n msr tpidr_el1, x9\n"
        "  ldr x9, [x10, #288]\n msr tpidr_el0, x9\n"
        "  ldr x9, [x10, #296]\n msr tpidrro_el0, x9\n"
        "  ldr x9, [x10, #312]\n msr contextidr_el1, x9\n"
        "  ldr x9, [x10, #320]\n msr mdscr_el1, x9\n"
#ifndef GIC_V2
        "  ldr x9, [x10, #344]\n msr icc_pmr_el1, x9\n"
        "  ldr x9, [x10, #360]\n msr icc_bpr1_el1, x9\n"
        "  ldr x9, [x10, #352]\n msr icc_igrpen1_el1, x9\n"
#endif
        "  ldr x9, [x10, #328]\n msr s3_3_c4_c4_0, x9\n"
        "  ldr x9, [x10, #336]\n msr s3_3_c4_c4_1, x9\n"
        "  ldr x9, [x10, #104]\n msr cntkctl_el1, x9\n"
        "  ldr x9, [x10, #80]\n  msr cntv_cval_el0, x9\n"
        "  ldr x9, [x10, #72]\n  msr cntv_ctl_el0, x9\n"
        "  ldr x18, [x10, #112]\n"
        "  ldp x19, x20, [x10, #120]\n"
        "  ldp x21, x22, [x10, #136]\n"
        "  ldp x23, x24, [x10, #152]\n"
        "  ldp x25, x26, [x10, #168]\n"
        "  ldp x27, x28, [x10, #184]\n"
        "  ldp x29, x30, [x10, #200]\n"
        "  stp x0, x1, [x10, #216]\n"   /* kernel log PA and length, stored with caches on */
        "  stp x2, x3, [x10, #232]\n"   /* reason, ESR */
        "  stp x4, x5, [x10, #248]\n"   /* ELR, FAR */
        "  stp x6, x7, [x10, #264]\n"   /* LR, SP */
        "  ldr x9, [x10, #64]\n  msr daif, x9\n"
        "  mov x0, #1\n"
        "  ret\n"
        );

__attribute__((noreturn)) static void
jump_to_kernel(uint64_t entry, uint64_t boot_args_pa)
{
	__asm__ volatile (
                "msr daifset, #0xf\n"
                /* Last thing before the MMU goes off: write this stack back to RAM
                 * (dirty cache lines do not survive, and we come back to this frame). */
                "mov x2, sp\n"
                "sub x2, x2, #0x400\n"
                "bic x2, x2, #63\n"
                "add x3, x2, #0x4000\n"
                "1: dc cvac, x2\n"
                "add x2, x2, #64\n"
                "cmp x2, x3\n"
                "b.lo 1b\n"
                "dsb sy\n"
                "mrs x2, sctlr_el1\n"
                "bic x2, x2, #0x1\n"    /* M: MMU off */
                "bic x2, x2, #0x4\n"    /* C: data cache off */
                "bic x2, x2, #0x1000\n" /* I: instruction cache off */
                "msr sctlr_el1, x2\n"
                "isb\n"
                "ic iallu\n"
                "dsb sy\n"
                "isb\n"
                "mov x0, %1\n"
                "br %0\n"
                : : "r" (entry), "r" (boot_args_pa) : "x0", "x2", "x3");
	__builtin_unreachable();
}

/* ------------------------------------------------------------------------ */
/* The kernel on another core.                                               */
/*                                                                          */
/* The firmware runs on core 0, one of the four small ones (Cortex-A55); the */
/* kernel uses a single core, and it had better be a big one (the A78s are   */
/* about 2.5 times as fast). With "bootcpu=N" among the kernel's arguments   */
/* the loader does not jump into the kernel itself: it has the firmware      */
/* (PSCI) start core N at other_cpu_entry, which enters the kernel exactly   */
/* as core 0 would have, and waits. UEFI stays resident and lives on core 0, */
/* so the kernel's way back is different too: other_cpu_return_stub, on core */
/* N, leaves what the kernel returned where core 0 looks for it, wakes core  */
/* 0 and switches its own core off.                                         */

#define PSCI_CPU_OFF       0x84000002u
#define PSCI_CPU_ON        0xC4000003u
#define PSCI_AFFINITY_INFO 0xC4000004u      /* (MPIDR, 0): 0 = that core is on, 1 = off, 2 = being started */
#define PSCI_NOT_SUPPORTED (-1)

/* each alone in a cache line: one core reads them with its caches off */
struct {
	uint64_t entry, boot_args;
} other_cpu_start __attribute__((aligned(64)));
/*
 * The kernel's own CPU_ON (XNU psci_smp.c) goes through kernel_cpu_trampoline, here below 4 GB:
 * Samsung's firmware (Exynos 990) starts a core at the address it is given only if that address
 * fits in 32 bits, and it does not hand the core its context_id in x0. The kernel, which is far
 * above 4 GB, writes its reset vector and the core's id into kernel_cpu_slot before each CPU_ON
 * (it starts its cores one at a time); the device tree's /chosen says where both are.
 */
void kernel_cpu_trampoline(void);
extern uint64_t kernel_cpu_slot[2];
/*
 * ... and kernel_cpu_resume for a core coming back from PSCI CPU_SUSPEND (a power-down idle: XNU's
 * redwood_cpuidle.c). Any core may wake at any time, so one shared slot cannot say which it is; and its
 * MPIDR alone is ambiguous on this SoC (A55 #1 masks to the id M5 #0 is registered under). The kernel fills
 * kernel_cpu_resume_data: [0] its reset vector, then up to eight pairs { MPIDR & 0xffffff, phys id }; the
 * trampoline finds its own MPIDR there and enters the kernel with that phys id in x0, as kernel_cpu_trampoline.
 */
void kernel_cpu_resume(void);
extern uint64_t kernel_cpu_resume_data[17];
volatile uint64_t other_cpu_stage __attribute__((aligned(64)));  /* 1: that core has reached other_cpu_entry */
volatile uint64_t other_cpu_done __attribute__((aligned(64)));   /* 1: the kernel is back (other_cpu_return_stub) */
uint64_t psci_by_hvc __attribute__((aligned(64)));
void other_cpu_entry(void);
void other_cpu_return_stub(void);

__asm__ (
        ".text\n"
        ".balign 64\n"
        ".globl other_cpu_entry\n"
        "other_cpu_entry:\n"                    /* MMU and caches are off; x0 is NOT reliable (Samsung) */
        "  msr daifset, #0xf\n"
        /* until the kernel has vectors of its own, an exception must not go nowhere */
        "  adrp x9, other_cpu_vectors\n"
        "  add  x9, x9, :lo12:other_cpu_vectors\n"
        "  msr vbar_el1, x9\n"
        /* what the firmware had set up on the core the kernel used to start on, the kernel may take
         * for granted: floating point allowed (CPACR), the fixed bits of SCTLR */
        "  adrp x10, loader_ctx\n"
        "  add  x10, x10, :lo12:loader_ctx\n"
        "  ldr x9, [x10, #40]\n"
        "  msr cpacr_el1, x9\n"
        "  ldr x9, [x10, #24]\n"
        "  bic x9, x9, #0x1\n"
        "  bic x9, x9, #0x4\n"
        "  bic x9, x9, #0x1000\n"
        "  msr sctlr_el1, x9\n"
        "  isb\n"
        "  adrp x9, other_cpu_stage\n"
        "  add  x9, x9, :lo12:other_cpu_stage\n"
        "  mov x10, #1\n"
        "  str x10, [x9]\n"
        "  adrp x0, other_cpu_start\n"
        "  add  x0, x0, :lo12:other_cpu_start\n"
        "  ldp x9, x10, [x0]\n"
        "  mov x0, x10\n"
        "  br x9\n"
        ".balign 64\n"
        ".globl kernel_cpu_trampoline\n"
        "kernel_cpu_trampoline:\n"             /* MMU off: x0 <- the core's id, then its 64-bit entry */
        "  adr x9, kernel_cpu_slot\n"
        "  ldp x10, x0, [x9]\n"
        "  br x10\n"
        ".balign 64\n"
        ".globl kernel_cpu_slot\n"
        "kernel_cpu_slot:\n"
        "  .quad 0, 0\n"
        ".balign 64\n"
        ".globl kernel_cpu_resume\n"
        "kernel_cpu_resume:\n"                /* MMU off: find this core in the table by its MPIDR */
        "  mrs x11, mpidr_el1\n"
        "  and x11, x11, #0xffffff\n"
        "  adr x9, kernel_cpu_resume_data\n"
        "  ldr x12, [x9], #8\n"               /* the kernel's reset vector */
        "  mov x13, #8\n"
        "1:\n"
        "  ldp x10, x0, [x9], #16\n"
        "  cmp x10, x11\n"
        "  b.eq 2f\n"
        "  subs x13, x13, #1\n"
        "  b.ne 1b\n"
        "3:\n"                                 /* not in the table: nothing to enter; stay out of the way */
        "  wfi\n"
        "  b 3b\n"
        "2:\n"
        "  cbz x12, 3b\n"
        "  br x12\n"
        ".balign 64\n"
        ".globl kernel_cpu_resume_data\n"
        "kernel_cpu_resume_data:\n"
        "  .fill 17, 8, 0\n"
        ".balign 64\n"
        "other_cpu_fault:\n"                    /* as an early exception in the kernel: reason 1 */
        "  mrs x3, esr_el1\n"
        "  mrs x4, elr_el1\n"
        "  mrs x5, far_el1\n"
        "  mov x6, x30\n"
        "  mov x7, sp\n"
        "  mov x0, #0\n"
        "  mov x1, #0\n"
        "  mov x2, #1\n"
        "  b other_cpu_return_stub\n"
        ".balign 0x800\n"
        "other_cpu_vectors:\n"
        "  b other_cpu_fault\n  .balign 0x80\n  b other_cpu_fault\n  .balign 0x80\n"
        "  b other_cpu_fault\n  .balign 0x80\n  b other_cpu_fault\n  .balign 0x80\n"
        "  b other_cpu_fault\n  .balign 0x80\n  b other_cpu_fault\n  .balign 0x80\n"
        "  b other_cpu_fault\n  .balign 0x80\n  b other_cpu_fault\n  .balign 0x80\n"
        "  b other_cpu_fault\n  .balign 0x80\n  b other_cpu_fault\n  .balign 0x80\n"
        "  b other_cpu_fault\n  .balign 0x80\n  b other_cpu_fault\n  .balign 0x80\n"
        "  b other_cpu_fault\n  .balign 0x80\n  b other_cpu_fault\n  .balign 0x80\n"
        "  b other_cpu_fault\n  .balign 0x80\n  b other_cpu_fault\n  .balign 0x80\n"
        ".balign 64\n"
        ".globl other_cpu_return_stub\n"
        "other_cpu_return_stub:\n"              /* x0..x7 as for loader_return_stub */
        "  msr daifset, #0xf\n"
        "  .inst 0xd500409f\n"                  /* msr PAN, #0 */
        "  .inst 0xd500407f\n"                  /* msr UAO, #0 */
        "  mrs x9, sctlr_el1\n"
        "  bic x9, x9, #0x1\n"
        "  bic x9, x9, #0x4\n"
        "  bic x9, x9, #0x1000\n"
        "  msr sctlr_el1, x9\n"
        "  isb\n"
        "  adrp x10, loader_ctx\n"
        "  add  x10, x10, :lo12:loader_ctx\n"
        "  stp x0, x1, [x10, #216]\n"           /* kernel log PA and length */
        "  stp x2, x3, [x10, #232]\n"           /* reason, ESR */
        "  stp x4, x5, [x10, #248]\n"           /* ELR, FAR */
        "  stp x6, x7, [x10, #264]\n"           /* LR, SP */
        "  adrp x10, other_cpu_done\n"
        "  add  x10, x10, :lo12:other_cpu_done\n"
        "  mov x9, #1\n"
        "  str x9, [x10]\n"
        "  dsb sy\n"
        "  sev\n"
        "  adrp x10, psci_by_hvc\n"
        "  add  x10, x10, :lo12:psci_by_hvc\n"
        "  ldr x9, [x10]\n"
        "  mov x0, #0x0002\n"                   /* PSCI CPU_OFF: does not return */
        "  movk x0, #0x8400, lsl #16\n"
        "  cbnz x9, 2f\n"
        "  smc #0\n"
        "1: wfi\n"
        "  b 1b\n"
        "2: hvc #0\n"
        "  b 1b\n"
        );

static uint64_t smc_call(uint64_t id, uint64_t a1, uint64_t a2, uint64_t *r1, uint64_t *r2);
static uint64_t smc_x3;
static const struct platform *PLAT;
static void clean_dcache(uint64_t start, uint64_t end);
static void flush_dcache(uint64_t start, uint64_t end);

/* What the kernel shows a watching loader (struct loader_live in XNU's loader_return.c). */
struct kernel_live {
	uint64_t magic;                 /* "XNULIVE!" */
	uint64_t heartbeat;             /* goes up every 10 seconds while the kernel is alive */
	uint64_t log, log_len, log_ring_at, log_wrapped;        /* the kernel's virtual addresses: add the slide */
	uint64_t log_head, log_ring;    /* the log: this much from the start of the boot, then a ring this big */
	uint64_t recording;             /* (a virtual address too) { physical address, bytes } of a screen recording */
	uint64_t stop;                  /* ours to write: the host wants the run to end ("oem stop") */
	uint64_t magic2;                /* "XNULIVE2": the kernel is new enough to have what follows */
	uint64_t sound;                 /* (a virtual address too) the same for a recording of the sound */
	uint64_t magic3, menu_width, menu_height; /* XNULIVE3: cropped firmware channel on return */
};

/*
 * Battery state shared with XNU.  Qualcomm's charger firmware remains a UEFI
 * boot-service client on core 0 while XNU runs on another core.  The loader is
 * therefore the one safe place to call it; XNU only reads this cache.
 */
#define LOADER_BATTERY_MAGIC            0x2154544142574452ULL /* "RDWBATT!" */
#define LOADER_BATTERY_VERSION          1
#define LOADER_BATTERY_VALID            (1u << 0)
#define LOADER_BATTERY_PRESENT          (1u << 1)
#define LOADER_BATTERY_CHARGER_PRESENT  (1u << 2)
#define LOADER_BATTERY_VOLTAGE_VALID    (1u << 3)
#define LOADER_BATTERY_CHARGING         (1u << 4)
#define LOADER_BATTERY_SOC_ESTIMATED    (1u << 5)

struct loader_battery {
	uint64_t magic;
	uint32_t version, size;
	volatile uint32_t sequence;
	uint32_t flags;
	int32_t soc_percent;
	int32_t voltage_mv;
	int32_t current_ma;
	int32_t temperature_c;
	uint32_t charger_action;
	uint32_t polls;
	uint32_t failures;
	uint32_t reserved;
};

/*
 * Layout returned by QcomChargerDxeLA's GetChargingAction, word offsets confirmed by dumping the
 * raw buffer on this phone: [2] state of charge, [3] charge current (signed mA), [4] battery
 * voltage (mV), [5] temperature (C). `rest` is oversized on purpose so a firmware that writes
 * more than the fields we name cannot smash the stack.
 */
struct qcom_battery_status {
	uint32_t status0, status1;
	int32_t state_of_charge;
	int32_t charge_current_ma;
	int32_t battery_voltage_mv;
	int32_t battery_temperature_c;
	uint32_t rest[26];
};

static struct loader_battery charger_shared __attribute__((aligned(0x2000))) = {
	.magic = LOADER_BATTERY_MAGIC,
	.version = LOADER_BATTERY_VERSION,
	.size = sizeof(struct loader_battery),
	.soc_percent = -1,
	.voltage_mv = -1,
	.current_ma = 0,
	.temperature_c = -273,
	.charger_action = 0xffffffffu,
};
static efi_charger_ex *CHARGER_EX;
static efi_qcom_charger *QCOM_CHARGER;
static int charger_enabled;

/* The screen recording the last kernel left in memory, if it made one ("oem recording" stages it). */
static const char *recording_data;
static uint64_t recording_len;
/* ... and the recording of the sound ("oem sound"). */
static const char *sound_data;
static uint64_t sound_len;
#define KERNEL_LIVE_MAGIC2      0x324556494c554e58ULL
#define KERNEL_LIVE_MAGIC       0x214556494c554e58ULL
#define OTHER_CPU_NOT_STARTED   1000    /* run_on_other_cpu(): the firmware said yes but the core never arrived */
#define OTHER_CPU_HUNG          1001    /* ... the kernel stopped giving signs of life */
#define START_SECONDS           5
#define SILENCE_SECONDS         50
#define NOTE_SECONDS            24      /* how long the kernel's progress is written down as it happens */

static void wdt_pet(void);
static int wdt_armed;

/*
 * While a kernel runs on another core and this one watches it: where it shows its signs of life,
 * for the commands that the host sends meanwhile (usb_poll_command).
 */
static uint32_t menu_output_width, menu_output_height;
static const struct kernel_live *running_live;
static uint64_t running_slide;
static void usb_poll_command(void);

static uint64_t
seconds_since(uint64_t then)
{
	return (READ_SYSREG(CNTVCT_EL0) - then) / timer_freq();
}

/* Reverse a stretch of memory in place. */
static void
reverse(char *p, uint64_t n)
{
	for (uint64_t i = 0; i < n / 2; i++) {
		char c = p[i];

		p[i] = p[n - 1 - i];
		p[n - 1 - i] = c;
	}
}

/*
 * The log of a kernel that did not come back to hand it over: put its two parts in order
 * (the ring's oldest character first) and say where it is and how long.
 */
static void
log_of_hung_kernel(const struct kernel_live *live, uint64_t slide)
{
	char *log = (char *)(live->log + slide);
	uint32_t len, ring_at;

	loader_ctx[CTX_LOG_PA] = loader_ctx[CTX_LOG_LEN] = 0;
	len = *(volatile uint32_t *)(live->log_len + slide);
	ring_at = *(volatile uint32_t *)(live->log_ring_at + slide);
	if (*(volatile uint8_t *)(live->log_wrapped + slide) && ring_at < live->log_ring) {
		/* rotate the ring left by ring_at: three reversals */
		reverse(log + live->log_head, ring_at);
		reverse(log + live->log_head + ring_at, live->log_ring - ring_at);
		reverse(log + live->log_head, live->log_ring);
		len = (uint32_t)(live->log_head + live->log_ring);
	}
	if (len <= live->log_head + live->log_ring) {
		loader_ctx[CTX_LOG_PA] = (uint64_t)(uintptr_t)log;
		loader_ctx[CTX_LOG_LEN] = len;
	}
}

/*
 * The log of a kernel that is alive and is merely being asked how it is doing ("oem log" during
 * a run): a copy in the right order after the loader's own log, with nothing moved under the
 * kernel's feet.
 */
static void
compose_live_log(const struct kernel_live *live, uint64_t slide)
{
	static const char sep[] = "\n== KERNEL LOG (the kernel is still running) ==\n";
	const char *log = (const char *)(live->log + slide);
	uint32_t len = *(volatile uint32_t *)(live->log_len + slide);
	uint32_t ring_at = *(volatile uint32_t *)(live->log_ring_at + slide);
	int wrapped = *(volatile uint8_t *)(live->log_wrapped + slide);
	uint64_t n, room = sizeof(out_buf) - 8192;

	compose_logs(NULL, 0);
	n = staged_len;
#define PUT(from, size) do { \
	uint64_t take = (size); \
	if (n + take > room) { \
	        take = room - n; \
	} \
	memcpy(out_buf + n, (from), take); \
	n += take; \
} while (0)
	PUT(sep, sizeof(sep) - 1);
	if (!wrapped || ring_at >= live->log_ring) {
		PUT(log, len <= live->log_head + live->log_ring ? len : live->log_head + live->log_ring);
	} else {
		PUT(log, live->log_head);
		PUT(log + live->log_head + ring_at, live->log_ring - ring_at);
		PUT(log + live->log_head, ring_at);
	}
#undef PUT
	staged_len = n;
}

static void print(const char *fmt, ...);
static void flush_log(void);
static void put_char(char c);

/* A voltage-only fallback is deliberately conservative; the real QG SOC wins whenever available. */
static int
soc_from_voltage(uint32_t mv)
{
	if (mv <= 3300) return 0;
	if (mv < 3700) return (int)((mv - 3300) * 10 / 400);
	if (mv < 3900) return 10 + (int)((mv - 3700) * 40 / 200);
	if (mv < 4200) return 50 + (int)((mv - 3900) * 40 / 300);
	if (mv < 4400) return 90 + (int)((mv - 4200) * 10 / 200);
	return 100;
}

/* Read-only status query.  No TakeAction call and no charger-limit writes are made here. */
static void
charger_poll(void)
{
	struct qcom_battery_status status;
	uint32_t action = 0xffffffffu, flags = 0, voltage = 0;
	uint8_t present = 0, plugged = 0;
	efi_status s;
	int have_soc = 0, have_voltage = 0;

	if (!charger_enabled) {
		return;
	}
	memset(&status, 0, sizeof(status));

	charger_shared.sequence++;
	__asm__ volatile ("dmb ish" ::: "memory");
	if (CHARGER_EX != NULL) {
		s = CHARGER_EX->get_battery_presence(&present);
		if (!EFI_ERROR(s) && present) flags |= LOADER_BATTERY_PRESENT;
		s = CHARGER_EX->get_charger_presence(&plugged);
		if (!EFI_ERROR(s) && plugged) flags |= LOADER_BATTERY_CHARGER_PRESENT;
		s = CHARGER_EX->get_battery_voltage(&voltage);
		if (!EFI_ERROR(s) && voltage >= 2500 && voltage <= 5000) {
			have_voltage = 1;
			flags |= LOADER_BATTERY_VOLTAGE_VALID;
			charger_shared.voltage_mv = (int32_t)voltage;
		}
	}
	if (QCOM_CHARGER != NULL) {
		s = QCOM_CHARGER->get_charging_action(&action, &status, 0);
		if (!EFI_ERROR(s)) {
			charger_shared.charger_action = action;
			if (status.battery_voltage_mv >= 2500 && status.battery_voltage_mv <= 5000) {
				charger_shared.voltage_mv = status.battery_voltage_mv;
				have_voltage = 1;
				flags |= LOADER_BATTERY_VOLTAGE_VALID;
			}
			/*
			 * GetChargingAction returns the firmware's state of charge in word [2] of its
			 * output (confirmed by dumping the raw buffer on this device). We use it as-is
			 * EXCEPT when the fuel gauge is clearly not ready yet: right after initialize()
			 * it can report a near-empty SOC while the cell voltage says otherwise, which is
			 * where the old "shows 1% on a half-full battery" came from. A Li-ion cell above
			 * ~3.6 V is never under 5%, so in that case we distrust the gauge and fall back to
			 * the voltage estimate below until it settles.
			 */
			if (status.state_of_charge >= 0 && status.state_of_charge <= 100) {
				int vmv = have_voltage ? charger_shared.voltage_mv : 0;
				if (status.state_of_charge < 5 && vmv > 3600) {
					/* fuel gauge not settled; leave it to the voltage fallback */
				} else {
					charger_shared.soc_percent = status.state_of_charge;
					have_soc = 1;
					flags |= LOADER_BATTERY_VALID;
				}
			}
			charger_shared.current_ma = status.charge_current_ma;
			charger_shared.temperature_c = status.battery_temperature_c;
		}
	}
	if (!have_soc && have_voltage) {
		charger_shared.soc_percent = soc_from_voltage((uint32_t)charger_shared.voltage_mv);
		flags |= LOADER_BATTERY_VALID | LOADER_BATTERY_SOC_ESTIMATED;
	}
	if (plugged && charger_shared.soc_percent < 100) {
		flags |= LOADER_BATTERY_CHARGING;
	}
	charger_shared.flags = flags;
	charger_shared.polls++;
	if (!(flags & LOADER_BATTERY_VALID)) charger_shared.failures++;
	__asm__ volatile ("dmb ish" ::: "memory");
	charger_shared.sequence++;
	clean_dcache((uint64_t)(uintptr_t)&charger_shared,
	    (uint64_t)(uintptr_t)&charger_shared + sizeof(charger_shared));
}

/* ------------------------------------------------------------------------ */
/* UFS survey (ufs=1): what the phone's storage holds, read only            */

/*
 * The phone's storage as Qualcomm's own UFS driver in this UEFI sees it (UFSDxe): every raw
 * logical unit, every GPT partition with its name, place and size, and for the partitions there
 * are reference images of -- firmware-update/ in the ROM's fw.zip -- an FNV-1a hash of exactly
 * as many bytes as the image has, which tools/ufs-survey.py computes from the images on the host.
 * Where the two agree, reading through this driver gives the bytes that are on the flash, and that
 * is the ground a block device for the kernel is to stand on.
 *
 * Only read_blocks is ever called here: nothing is written, flushed or reset. It runs before the
 * kernel is started, with UEFI's boot services whole and interrupts on.
 */
static const struct {
	const char *name;
	uint64_t bytes;
} ufs_references[] = {
	{ "bluetooth", 503808 }, { "abl", 208896 }, { "aop", 208896 }, { "xbl", 3706880 },
	{ "tz", 3710976 }, { "hyp", 3653632 }, { "dsp", 67108864 }, { "modem", 214265856 },
};

static uint8_t ufs_buffer[1024 * 1024] __attribute__((aligned(4096)));

static uint64_t
ufs_fnv(uint64_t h, const uint8_t *p, uint64_t n)
{
	for (uint64_t i = 0; i < n; i++) {
		h = (h ^ p[i]) * 0x100000001b3ULL;
	}
	return h;
}

/* the partition's name in ASCII (GPT names are UTF-16; these are all plain letters) */
static void
ufs_name(const efi_partition_info *info, char *out, unsigned size)
{
	unsigned i = 0;

	for (; i + 1 < size && i < 36 && info->name[i] != 0; i++) {
		out[i] = info->name[i] < 0x80 ? (char)info->name[i] : '?';
	}
	out[i] = 0;
}

/* "bluetooth_a" and "bluetooth_b" are both "bluetooth": the image is the same for either slot */
static int
ufs_reference_of(const char *name)
{
	for (unsigned r = 0; r < sizeof(ufs_references) / sizeof(ufs_references[0]); r++) {
		const char *a = ufs_references[r].name, *b = name;

		while (*a && *a == *b) {
			a++;
			b++;
		}
		if (*a == 0 && (*b == 0 || ((b[0] == '_') && (b[1] == 'a' || b[1] == 'b') && b[2] == 0))) {
			return (int)r;
		}
	}
	return -1;
}

static void
ufs_hash_partition(efi_block_io *bio, const char *name, uint64_t bytes)
{
	uint64_t bs = bio->media->block_size, h = 0xcbf29ce484222325ULL, done = 0, started = READ_SYSREG(CNTVCT_EL0);
	uint64_t chunk = sizeof(ufs_buffer) / bs * bs, lba = 0;

	if (bs == 0 || bs > sizeof(ufs_buffer) || (bio->media->last_block + 1) * bs < bytes) {
		print("ufs: %s: %d bytes asked for, the partition is smaller\n", name, bytes);
		return;
	}
	while (done < bytes) {
		uint64_t want = bytes - done < chunk ? (bytes - done + bs - 1) / bs * bs : chunk;
		efi_status s = bio->read_blocks(bio, bio->media->media_id, lba, want, ufs_buffer);

		if (EFI_ERROR(s)) {
			print("ufs: %s: reading at block %d: error %x\n", name, lba, (uint64_t)s);
			return;
		}
		if (done == 0) {
			print("ufs: %s: begins", name);
			for (int k = 0; k < 16; k++) {
				print(" %x", (uint64_t)ufs_buffer[k]);
			}
			print("\n");
		}
		h = ufs_fnv(h, ufs_buffer, bytes - done < want ? bytes - done : want);
		done += bytes - done < want ? bytes - done : want;
		lba += want / bs;
		if (wdt_armed) {
			wdt_pet();
		}
	}
	print("ufs: %s: fnv %x over %d bytes, %d ms\n", name, h, bytes,
	    (READ_SYSREG(CNTVCT_EL0) - started) * 1000 / timer_freq());
}

/* CRC-32 as GPT uses it (IEEE 802.3, reflected, 0xedb88320) */
static uint32_t
ufs_crc32(const uint8_t *p, uint64_t n)
{
	uint32_t c = 0xffffffffu;

	for (uint64_t i = 0; i < n; i++) {
		c ^= p[i];
		for (int k = 0; k < 8; k++) {
			c = (c >> 1) ^ (0xedb88320u & (0u - (c & 1)));
		}
	}
	return ~c;
}

/*
 * A unit's GPT checks itself: its header carries a CRC-32 of the header and one of the table of
 * partitions. Whatever firmware is on the phone, the bytes read are the bytes on the flash only if
 * both come out right -- the one check here that does not depend on having the same images.
 */
static void
ufs_check_gpt(efi_block_io *bio, uint64_t unit)
{
	uint64_t bs = bio->media->block_size, entries_lba, count, size, bytes, read;
	uint32_t header_size, header_crc, entries_crc, got_header, got_entries;
	uint8_t header[512];

	if (bs < 512 || bs > sizeof(ufs_buffer) ||
	    EFI_ERROR(bio->read_blocks(bio, bio->media->media_id, 1, bs, ufs_buffer))) {
		print("ufs: unit %d: the GPT header could not be read\n", unit);
		return;
	}
	if (ufs_buffer[0] != 'E' || ufs_buffer[1] != 'F' || ufs_buffer[2] != 'I' || ufs_buffer[3] != ' ' ||
	    ufs_buffer[4] != 'P' || ufs_buffer[5] != 'A' || ufs_buffer[6] != 'R' || ufs_buffer[7] != 'T') {
		print("ufs: unit %d: no GPT header in block 1\n", unit);
		return;
	}
	memcpy(header, ufs_buffer, sizeof(header));
	header_size = *(uint32_t *)(header + 12);
	header_crc = *(uint32_t *)(header + 16);
	entries_lba = *(uint64_t *)(header + 72);
	count = *(uint32_t *)(header + 80);
	size = *(uint32_t *)(header + 84);
	entries_crc = *(uint32_t *)(header + 88);
	if (header_size < 92 || header_size > sizeof(header)) {
		print("ufs: unit %d: GPT header size %d\n", unit, (uint64_t)header_size);
		return;
	}
	*(uint32_t *)(header + 16) = 0;
	got_header = ufs_crc32(header, header_size);
	bytes = count * size;
	if (bytes == 0 || bytes > sizeof(ufs_buffer)) {
		print("ufs: unit %d: GPT header crc %s, a table of %d bytes not checked\n", unit,
		    got_header == header_crc ? "right" : "WRONG", bytes);
		return;
	}
	read = (bytes + bs - 1) / bs * bs;
	if (EFI_ERROR(bio->read_blocks(bio, bio->media->media_id, entries_lba, read, ufs_buffer))) {
		print("ufs: unit %d: the GPT table could not be read\n", unit);
		return;
	}
	got_entries = ufs_crc32(ufs_buffer, bytes);
	print("ufs: unit %d: GPT header crc %s (%x), table of %d entries crc %s (%x)\n", unit,
	    got_header == header_crc ? "right" : "WRONG", (uint64_t)got_header, count,
	    got_entries == entries_crc ? "right" : "WRONG", (uint64_t)got_entries);
}

/* one line per 32 bytes: "ufs: PART WHAT +OFFSET: hex" */
static void
ufs_dump_of(const char *part, const char *what, const uint8_t *p, uint64_t n)
{
	for (uint64_t at = 0; at < n; at += 32) {
		char line[72];
		unsigned k = 0;

		for (uint64_t i = at; i < n && i < at + 32; i++) {
			line[k++] = "0123456789abcdef"[p[i] >> 4];
			line[k++] = "0123456789abcdef"[p[i] & 15];
		}
		line[k] = 0;
		print("ufs: %s %s +%x: %s\n", part, what, at, line);
	}
}

static void
ufs_dump(const char *what, const uint8_t *p, uint64_t n)
{
	ufs_dump_of("super", what, p, n);
}

/*
 * The start of userdata: where an f2fs (or ext4) of Android's would have its superblock, at 1024
 * and, for f2fs, its copy at 4096 + 1024. Plain text there says the filesystem itself lies on the
 * flash as it is; anything else says it lies under an encryption of the whole partition
 * (Android's metadata encryption, dm-default-key), and only Android can see into it.
 */
static void
ufs_dump_userdata(efi_block_io *bio)
{
	uint64_t bs = bio->media->block_size, want = 8192;

	if (bs == 0 || want % bs || EFI_ERROR(bio->read_blocks(bio, bio->media->media_id, 0, want, ufs_buffer))) {
		print("ufs: userdata: its start could not be read\n");
		return;
	}
	ufs_dump_of("userdata", "sb", ufs_buffer + 1024, 1024);
	ufs_dump_of("userdata", "sb2", ufs_buffer + 4096 + 1024, 64);
}

/*
 * Android's own system (system, vendor, product, ...) is not in GPT partitions but in logical ones
 * inside "super", which liblp's metadata at its start describes: a geometry at 4096 (and its copy
 * at 8192), then its metadata slots from 12288 on -- each a header and its tables of partitions,
 * extents, groups and block devices. Printed raw for tools/ufs-super.py, which checks their SHA-256 sums and
 * says where in super nothing lies: a place a volume of this port's own could have without
 * anything of Android's being moved.
 */
static void
ufs_dump_super(efi_block_io *bio)
{
	uint64_t bs = bio->media->block_size, want = 256 * 1024;
	const uint8_t *geometry = ufs_buffer + 4096;
	uint32_t max_size, slots;

	if (bs == 0 || want % bs || (bio->media->last_block + 1) * bs < want ||
	    EFI_ERROR(bio->read_blocks(bio, bio->media->media_id, 0, want, ufs_buffer))) {
		print("ufs: super: its start could not be read\n");
		return;
	}
	if (*(const uint32_t *)geometry != 0x616c4467) {
		print("ufs: super: no liblp geometry (magic %x)\n", (uint64_t)*(const uint32_t *)geometry);
		return;
	}
	ufs_dump("geometry", geometry, 52);
	max_size = *(const uint32_t *)(geometry + 40);
	slots = *(const uint32_t *)(geometry + 44);
	/* a slot per A/B slot: the first describes super as slot _a sees it, the second as _b does */
	for (uint32_t slot = 0; slot < slots && slot < 2; slot++) {
		const uint8_t *header = ufs_buffer + 3 * 4096 + (uint64_t)slot * max_size;
		uint32_t header_size, tables_size;
		char what[8] = "header0", tables[8] = "tables0";

		if (3 * 4096 + (uint64_t)(slot + 1) * max_size > want || *(const uint32_t *)header != 0x414c5030) {
			print("ufs: super: slot %d: no metadata header within the first %d KB\n", (uint64_t)slot, want >> 10);
			continue;
		}
		header_size = *(const uint32_t *)(header + 8);
		tables_size = *(const uint32_t *)(header + 44);
		if (header_size < 128 || header_size > 256 || tables_size > max_size - header_size || tables_size > 16384) {
			print("ufs: super: slot %d: header of %d bytes, tables of %d: not dumped\n", (uint64_t)slot,
			    (uint64_t)header_size, (uint64_t)tables_size);
			continue;
		}
		what[6] = tables[6] = (char)('0' + slot);
		ufs_dump(what, header, header_size);
		ufs_dump(tables, header + header_size, tables_size);
	}
}

static void
ufs_survey(void)
{
	static const efi_guid part_guid = EFI_PARTITION_INFO_GUID;
	static const efi_guid blk_guid = EFI_BLOCK_IO_GUID;
	efi_handle *handles;
	uint64_t count, units = 0, partitions = 0;

	/* the raw logical units: the block devices that are not partitions of another */
	if (!EFI_ERROR(BS->locate_handle_buffer(ByProtocol, &blk_guid, NULL, &count, &handles))) {
		for (uint64_t i = 0; i < count; i++) {
			efi_block_io *bio;

			if (EFI_ERROR(BS->handle_protocol(handles[i], &blk_guid, (void **)&bio)) || bio->media->logical) {
				continue;
			}
			print("ufs: unit %d: media %d, %d blocks of %d (%d MB), %s, align %d\n", units++,
			    (uint64_t)bio->media->media_id, bio->media->last_block + 1, (uint64_t)bio->media->block_size,
			    (bio->media->last_block + 1) * bio->media->block_size >> 20,
			    bio->media->read_only ? "read only" : "writable", (uint64_t)bio->media->io_align);
			ufs_check_gpt(bio, units - 1);
		}
	}
	/* the GPT partitions, with the hash of those there are images of */
	if (EFI_ERROR(BS->locate_handle_buffer(ByProtocol, &part_guid, NULL, &count, &handles))) {
		print("ufs: no partition information in this UEFI\n");
		return;
	}
	for (uint64_t i = 0; i < count; i++) {
		efi_partition_info *info;
		efi_block_io *bio;
		char name[40];
		int r;

		if (EFI_ERROR(BS->handle_protocol(handles[i], &part_guid, (void **)&info)) || info->type != 2 ||
		    EFI_ERROR(BS->handle_protocol(handles[i], &blk_guid, (void **)&bio))) {
			continue;
		}
		ufs_name(info, name, sizeof(name));
		partitions++;
		print("ufs: part %s: lba %d..%d, %d KB\n", name, info->start_lba, info->end_lba,
		    (bio->media->last_block + 1) * bio->media->block_size >> 10);
		if ((r = ufs_reference_of(name)) >= 0) {
			ufs_hash_partition(bio, name, ufs_references[r].bytes);
		}
		if (name[0] == 's' && name[1] == 'u' && name[2] == 'p' && name[3] == 'e' && name[4] == 'r' && name[5] == 0) {
			ufs_dump_super(bio);
		}
		if (name[0] == 'u' && name[1] == 's' && name[2] == 'e' && name[3] == 'r' && name[4] == 'd' &&
		    name[5] == 'a' && name[6] == 't' && name[7] == 'a' && name[8] == 0) {
			ufs_dump_userdata(bio);
		}
	}
	print("ufs: %d raw units, %d partitions\n", units, partitions);
}

/* ------------------------------------------------------------------------ */
/* UFS block service (ufs=2): the kernel reads the phone's storage through  */
/* this loader, and this loader through Qualcomm's own UFS driver           */

/*
 * The kernel has no driver for the phone's storage, and does not need one: UEFI never exits here,
 * Qualcomm's UFS driver (UFSDxe) has the controller, its PHY, clocks and power up and stays
 * resident, and this loader already serves the kernel from core 0 while the kernel runs on another
 * (the battery). So the kernel asks for blocks through a page both can see, and core 0 reads them
 * with that driver's ReadBlocks into a buffer both can see. The kernel's side is
 * bsd/dev/redwood_ufs.c; it gets the page's address as ufs_shared_pa= on its command line.
 *
 * Reads, and -- with ufs_write=1 on the command line -- writes into one partition and nowhere
 * else: the port's own darwin, proved by its GPT entry (name, both GUIDs, first and last LBA, its
 * logical unit), and in it only past everything the build writes from the computer, so that no
 * answer to the kernel can reach Android's partitions, the iOS volume or the cryptexes. The
 * refusals are here, in the kernel (bsd/dev/redwood_ufs.c) and in UEFI's own driver, which gives
 * out a partition's BlockIo and bounds it: a request has to pass all three.
 *
 * Each side writes its own cache lines only: the kernel the request, core 0 the answer (and the
 * table, once, before the kernel starts). A line one side cleans out of its cache therefore never
 * carries a stale copy of what the other side wrote into it. Into the data buffer core 0 writes
 * only what it has read back after a write of its own, so after the controller's DMA there is
 * nothing of its own in its cache to write back over the data; the kernel, which fills the buffer
 * for a write, cleans its own lines of it before it asks.
 */
#define BLOCK_MAGIC        0x4b434f4c42574452ULL       /* "RDWBLOCK" */
#define BLOCK_VERSION      3
#define BLOCK_MAX_PARTS    160
#define BLOCK_MAX_UNITS    16
#define BLOCK_NATIVE_ONLY  1u      /* native.flags: the loader serves no request while the kernel runs */
#define BLOCK_PART_UNIT    1u                          /* the entry is a whole logical unit, not a partition */
#define BLOCK_OP_READ      0u
#define BLOCK_OP_WRITE     1u

/*
 * The one partition a write may reach, and from which of its blocks. darwin is the port's own
 * 20 GiB at the end of logical unit 0 (README, "The port's partition"); what the build puts there
 * from the computer (tools/darwin-write.py) is the iOS volume at its start and the two cryptexes
 * behind it, and nothing of this port reads or writes the rest -- so the data of iOS lives there
 * and a write is refused before that block. The identity is the whole of darwin-write.py's: the
 * same partition, named the same, of the same size, in the same place, on logical unit 0.
 */
#define BLOCK_WRITE_FIRST  2042368ULL                  /* darwin's blocks of 4096: past both cryptexes */
#define BLOCK_DARWIN_FIRST 25374720ULL                 /* its first and last LBA of unit 0, as the GPT has them */
#define BLOCK_DARWIN_LAST  30617594ULL
#define BLOCK_DARWIN_TYPE  { 0x7c3457ef, 0x0000, 0x11aa, { 0xaa, 0x11, 0x00, 0x30, 0x65, 0x43, 0xec, 0xac } }
#define BLOCK_DARWIN_UNIQ  { 0xfd6854ad, 0x24aa, 0x4fc3, { 0x94, 0xfe, 0x29, 0x01, 0x16, 0x85, 0x8d, 0x68 } }

/*
 * The partitions first, then the logical units they are on (BLOCK_PART_UNIT, named unit0, unit1,
 * ... in the order UEFI lists them). Each partition says which of those it is on and where it
 * begins there, so that a reader of the units themselves -- the kernel's own UFS driver -- finds
 * it; a unit says its logical unit number if its device path does.
 */
struct loader_block_part {
	char name[36];
	uint32_t block_size;
	uint64_t blocks;
	uint64_t start_lba;            /* where it begins on its unit, in the unit's blocks (0 for a unit) */
	uint16_t unit;                 /* the unit it is on, its number among the units; 0xffff: not known */
	uint8_t lun;                   /* a unit's logical unit number as its device path says; 0xff: it does not */
	uint8_t flags;                 /* BLOCK_PART_UNIT */
	uint32_t reserved;
};

struct loader_block {
	/* written by the loader, once, before the kernel starts */
	uint64_t magic;
	uint32_t version, size;
	uint32_t parts, buffer_size;
	uint64_t buffer_pa;
	/*
	 * Which partition takes writes this run, as its index among part[] plus one (0: none), and
	 * from which of its blocks. They are here, in what was padding before ask, so that the page
	 * stays the size and the version it was: one phone is shared between working copies, and a
	 * kernel built before writes existed must go on reading through a loader built after them,
	 * and the other way about -- a kernel that does not know these fields leaves ask.op zero,
	 * which is a read, and a loader that does not know them leaves these zero, which is no write.
	 */
	uint32_t write_part_plus_one;
	uint32_t write_reserved;
	uint64_t write_first;
	/* written by the kernel: what it wants, then request is advanced */
	struct {
		volatile uint32_t request;
		uint32_t part;
		uint64_t lba, count;           /* in the partition's own blocks */
		uint32_t op;                   /* BLOCK_OP_READ, BLOCK_OP_WRITE */
		uint32_t reserved;
	} ask __attribute__((aligned(128)));
	/* written by the loader: answered becomes request when the data is in the buffer */
	struct {
		volatile uint32_t answered;
		uint32_t reserved;
		int64_t status;                /* the EFI status of the read, 0 when it went well */
		uint64_t served, failed, bytes, ticks;
	} answer __attribute__((aligned(128)));
	struct loader_block_part part[BLOCK_MAX_PARTS] __attribute__((aligned(128)));
	/*
	 * Written by the loader before the kernel starts, with ufs_native= on the command line: where
	 * the kernel may put a transfer request list of its own into the UFS host controller for the
	 * run (hc_area, below), and UEFI's, which the loader puts back. hc_pa 0: it may not.
	 */
	struct {
		uint64_t hc_pa;                /* the controller's registers */
		uint64_t area_pa;              /* the list (1 KB, at the start) and command descriptors */
		uint32_t area_size, ucd_offset, ucd_size, slots;
		uint32_t uefi_utrlba, uefi_utrlbau;
		uint32_t flags;                /* BLOCK_NATIVE_ONLY */
		uint32_t gpt_units;            /* with BLOCK_NATIVE_ONLY: unit k's block 1 at gpt_pa + k * 4096 */
		uint64_t gpt_pa;
	} native __attribute__((aligned(128)));
	/* written by the loader during a run: it takes the controller back (a hung kernel); send nothing more */
	struct {
		volatile uint32_t take_back;
	} loader_native __attribute__((aligned(128)));
	/* written by the kernel: its list is in the controller (for the loader's log) */
	struct {
		volatile uint32_t list_in;
		uint32_t switched_in, switched_out;
		volatile uint32_t cnr_at_switch;       /* UTRLCNR when the kernel's list went in */
	} kernel_native __attribute__((aligned(128)));
};

_Static_assert(__builtin_offsetof(struct loader_block, ask) == 128,
    "the page must stay the size it was: the fields before ask have to fit in what was padding");

/* (8 KB is as far as this target aligns; the kernel maps whole 16 KB pages around both) */
static struct loader_block block_shared __attribute__((aligned(0x2000)));
static void block_io2_probe(efi_handle h, efi_block_io *bio);
static int block_probe_requested;       /* ufs_probe=1: block_io2_probe() at block_start() */
/* the units' GPT headers for a kernel on this core (not block_buffer: the log's read-back reuses that) */
static uint8_t block_gpt[BLOCK_MAX_UNITS][4096] __attribute__((aligned(0x2000)));
static efi_block_io *block_bios[BLOCK_MAX_PARTS];
static int block_enabled;
static int block_write_requested;       /* ufs_write=1 on the command line */
static int block_write_part = -1;       /* the only partition a write may reach; -1: none, writes refused */
static uint64_t block_writes, block_writes_refused, block_write_bytes;

/* the bytes of a device path before its end node */
static uint64_t
device_path_bytes(const efi_device_path *dp)
{
	const uint8_t *p = (const uint8_t *)dp;
	uint64_t n = 0;

	for (int nodes = 0; nodes < 64; nodes++) {
		uint64_t len = p[n + 2] | (uint64_t)p[n + 3] << 8;

		if (p[n] == 0x7f || len < 4) {
			break;
		}
		n += len;
	}
	return n;
}

/*
 * A unit's device path, a node at a time ("ufs: unit N: path TYPE.SUBTYPE[data] ..."), and its
 * logical unit number if a UFS node (messaging, subtype 0x19: target, then LUN) says it.
 */
static uint8_t
device_path_print(uint64_t unit, const efi_device_path *dp)
{
	const uint8_t *p = (const uint8_t *)dp;
	uint64_t n = 0, total = device_path_bytes(dp);
	uint8_t lun = 0xff;

	print("ufs: unit %d: path", unit);
	while (n < total) {
		uint64_t len = p[n + 2] | (uint64_t)p[n + 3] << 8;

		print(" %x.%x[", (uint64_t)p[n], (uint64_t)p[n + 1]);
		for (uint64_t i = 4; i < len && i < 4 + 24; i++) {
			print("%s%x", (uint64_t)p[n + i] < 16 ? "0" : "", (uint64_t)p[n + i]);
		}
		print("%s]", len > 4 + 24 ? "..." : "");
		if (p[n] == 3 && p[n + 1] == 0x19 && len >= 6) {
			lun = p[n + 5];
		}
		n += len;
	}
	print(lun == 0xff ? "\n" : " (LUN %d)\n", (uint64_t)lun);
	return lun;
}

/*
 * The UFS host controller, for the kernel's own driver (ufs_native= on the command line; without it
 * this loader never touches the controller's registers). The kernel may, for the length of a run,
 * stop the controller's transfer request list, point it at a list of its own in hc_area (memory of
 * this loader, so that putting UEFI's back never depends on the kernel's memory), and start it
 * again; UEFI's list is one slot and its command descriptor, with no room for another request.
 * This loader keeps UEFI's list address (UTRLBA/UTRLBAU) from before the first run and checks it,
 * and puts it back if need be, on every way back from a run (hc_take_back) before anything here
 * touches the storage; it reads for the kernel (block_poll) and writes the log (write_logs) only
 * while UEFI's list is the one in the controller.
 */
/*
 * ufshc@1d84000 ("ufs_mem") in the Xiaomi's Android device tree; on the S20+ (Exynos 990) the
 * controller is at 0x13100000 (Samsung's ufs-exynos.c, Silicium's PlatformUfsLib), where offsets
 * and lengths in a request descriptor are in bytes and a slot is cleared by writing a one.
 */
#define HC_PA              (PLAT == &platform_y2s ? 0x13100000ULL : 0x1d84000ULL)
#define HC_EXYNOS          (PLAT == &platform_y2s)
#define HC_CAP             0x00
#define HC_VER             0x08
#define HC_IS              0x20
#define HC_IE              0x24
#define HC_HCS             0x30
#define HC_HCE             0x34
#define HC_UTRLBA          0x50
#define HC_UTRLBAU         0x54
#define HC_UTRLDBR         0x58
#define HC_UTRLCLR         0x5c
#define HC_UTRLRSR         0x60
#define HC_UTRLCNR         0x64
#define HC_UTMRLDBR        0x78
#define HC_SLOTS           32
#define HC_UCD_OFFSET      0x1000
#define HC_UCD_SIZE        0x1000              /* (Exynos: a PRD entry per 4 KB, 65 of them for 256 KB) */
#define HC_AREA_SIZE       (HC_UCD_OFFSET + HC_SLOTS * HC_UCD_SIZE)

static uint8_t hc_area[HC_AREA_SIZE] __attribute__((aligned(0x2000)));
static int hc_requested;                /* ufs_native= on the command line */
static int hc_known;                    /* UEFI's list address is kept here: check it, put it back */
static uint32_t hc_uefi_lo, hc_uefi_hi;
static int hc_refused_reads;

static inline uint32_t
hc_rd(uint32_t off)
{
	return *(volatile uint32_t *)(uintptr_t)(HC_PA + off);
}

static inline void
hc_wr(uint32_t off, uint32_t v)
{
	__asm__ volatile ("dsb sy" ::: "memory");
	*(volatile uint32_t *)(uintptr_t)(HC_PA + off) = v;
	__asm__ volatile ("dsb sy" ::: "memory");
}

static uint64_t
hc_ms_since(uint64_t then)
{
	return (READ_SYSREG(CNTVCT_EL0) - then) * 1000 / timer_freq();
}

/* UEFI's list in, and running: what UEFI's driver needs to read or write */
static int
hc_list_is_uefis(void)
{
	return !hc_known || (hc_rd(HC_UTRLBA) == hc_uefi_lo && hc_rd(HC_UTRLBAU) == hc_uefi_hi &&
	       (hc_rd(HC_UTRLRSR) & 1) != 0);
}

static void
hc_print_state(const char *when)
{
	print("ufs: %s: list %x:%x (UEFI's %x:%x), run %x, doorbell %x, HCS %x, IS %x, IE %x, completion %x, "
	    "task doorbell %x\n", when, (uint64_t)hc_rd(HC_UTRLBAU), (uint64_t)hc_rd(HC_UTRLBA), (uint64_t)hc_uefi_hi,
	    (uint64_t)hc_uefi_lo, (uint64_t)hc_rd(HC_UTRLRSR), (uint64_t)hc_rd(HC_UTRLDBR), (uint64_t)hc_rd(HC_HCS),
	    (uint64_t)hc_rd(HC_IS), (uint64_t)hc_rd(HC_IE), (uint64_t)hc_rd(HC_UTRLCNR), (uint64_t)hc_rd(HC_UTMRLDBR));
}

/*
 * Whatever the kernel left: UEFI's list back in the controller. The kernel is told to send nothing
 * more (it looks before every doorbell, with interrupts off), given 50 ms, then what is in flight
 * is waited for (1 s), cleared if it does not finish (UTRLCLR: every slot of that list is the
 * kernel's), the list stopped, UEFI's address written back, the list started, all read back.
 */
/*
 * The S20+: IS's UTRCS left set by the kernel's commands makes UEFI's next command keep its doorbell
 * bit for ever (its driver clears IS only after the bit clears): every read and write times out.
 * Acknowledged before UEFI's driver is used again.
 */
static void
hc_clear_utrcs(const char *when)
{
	uint32_t is;

	if (!HC_EXYNOS) {
		return;
	}
	is = hc_rd(HC_IS);
	if (is & 1) {
		hc_wr(HC_IS, 1);
		print("ufs: %s: IS was %x: the transfer-complete status acknowledged (now %x), for UEFI's driver\n", when,
		    (uint64_t)is, (uint64_t)hc_rd(HC_IS));
	}
}

static void
hc_take_back(const char *when)
{
	uint32_t lo, hi, rsr, db;
	uint64_t t;

	if (!hc_known) {
		return;
	}
	lo = hc_rd(HC_UTRLBA);
	hi = hc_rd(HC_UTRLBAU);
	rsr = hc_rd(HC_UTRLRSR);
	db = hc_rd(HC_UTRLDBR);
	if (lo == hc_uefi_lo && hi == hc_uefi_hi && (rsr & 1)) {
		print("ufs: %s: UEFI's transfer list is in the controller (%x), running, doorbell %x\n", when, (uint64_t)lo,
		    (uint64_t)db);
		hc_clear_utrcs(when);
		return;
	}
	print("ufs: %s: the controller has list %x:%x (run %x, doorbell %x), not UEFI's %x:%x: taking it back\n", when,
	    (uint64_t)hi, (uint64_t)lo, (uint64_t)rsr, (uint64_t)db, (uint64_t)hc_uefi_hi, (uint64_t)hc_uefi_lo);
	block_shared.loader_native.take_back = 1;
	clean_dcache((uint64_t)(uintptr_t)&block_shared.loader_native,
	    (uint64_t)(uintptr_t)&block_shared.loader_native + sizeof(block_shared.loader_native));
	for (t = READ_SYSREG(CNTVCT_EL0); hc_ms_since(t) < 50;) {
	}
	for (t = READ_SYSREG(CNTVCT_EL0); (db = hc_rd(HC_UTRLDBR)) != 0 && hc_ms_since(t) < 1000;) {
	}
	if (db != 0) {
		print("ufs: %s: doorbell %x still set after 1 s\n", when, (uint64_t)db);
		if (hc_rd(HC_UTRLBA) != hc_uefi_lo || hc_rd(HC_UTRLBAU) != hc_uefi_hi) {
			print("ufs: %s: clearing the kernel's slots (UTRLCLR)\n", when);
			hc_wr(HC_UTRLCLR, HC_EXYNOS ? 0xffffffffu : 0);
			for (t = READ_SYSREG(CNTVCT_EL0); (db = hc_rd(HC_UTRLDBR)) != 0 && hc_ms_since(t) < 1000;) {
			}
			print("ufs: %s: doorbell %x after the clear\n", when, (uint64_t)db);
		}
	}
	/* the completion notifications the kernel's requests left (it says what there was when its list went in) */
	flush_dcache((uint64_t)(uintptr_t)&block_shared.kernel_native,
	    (uint64_t)(uintptr_t)&block_shared.kernel_native + sizeof(block_shared.kernel_native));
	if (block_shared.kernel_native.switched_in != 0) {
		uint32_t cnr = hc_rd(HC_UTRLCNR), ours = cnr & ~block_shared.kernel_native.cnr_at_switch;

		if (ours != 0) {
			hc_wr(HC_UTRLCNR, ours);
		}
		print("ufs: %s: completion %x, at the kernel's switch %x: cleared %x, now %x\n", when, (uint64_t)cnr,
		    (uint64_t)block_shared.kernel_native.cnr_at_switch, (uint64_t)ours, (uint64_t)hc_rd(HC_UTRLCNR));
	}
	hc_wr(HC_UTRLRSR, 0);
	for (t = READ_SYSREG(CNTVCT_EL0); (hc_rd(HC_UTRLRSR) & 1) && hc_ms_since(t) < 100;) {
	}
	hc_wr(HC_UTRLBA, hc_uefi_lo);
	hc_wr(HC_UTRLBAU, hc_uefi_hi);
	hc_wr(HC_UTRLRSR, 1);
	for (t = READ_SYSREG(CNTVCT_EL0); hc_ms_since(t) < 50;) {
	}
	hc_clear_utrcs(when);
	hc_print_state(when);
	print("ufs: %s: UEFI's list %s\n", when, hc_list_is_uefis() ? "put back" : "NOT PUT BACK: the storage is not used");
}

/*
 * Before a run (ufs_native=): what the controller looks like, and UEFI's list address kept -- the
 * first time; later it must be the same (or this loader's own area, which is then taken back).
 */
static int
hc_init(void)
{
	uint32_t lo, hi, area = (uint32_t)(uintptr_t)hc_area;

	if (!hc_requested) {
		return 0;
	}
	lo = hc_rd(HC_UTRLBA);
	hi = hc_rd(HC_UTRLBAU);
	print("ufs: controller at %x: CAP %x, VER %x, HCE %x, HCS %x, IS %x, IE %x, list %x:%x, run %x, doorbell %x\n",
	    HC_PA, (uint64_t)hc_rd(HC_CAP), (uint64_t)hc_rd(HC_VER), (uint64_t)hc_rd(HC_HCE), (uint64_t)hc_rd(HC_HCS),
	    (uint64_t)hc_rd(HC_IS), (uint64_t)hc_rd(HC_IE), (uint64_t)hi, (uint64_t)lo, (uint64_t)hc_rd(HC_UTRLRSR),
	    (uint64_t)hc_rd(HC_UTRLDBR));
	if (!hc_known) {
		if (hi == 0 && lo == area) {
			print("ufs: the controller has this loader's own area for its list before any run: left alone\n");
			return 0;
		}
		hc_uefi_lo = lo;
		hc_uefi_hi = hi;
		hc_known = 1;
		print("ufs: UEFI's transfer list is at %x:%x; it is put back after every run\n", (uint64_t)hi, (uint64_t)lo);
	} else if (lo != hc_uefi_lo || hi != hc_uefi_hi) {
		if (hi == 0 && lo == area) {
			hc_take_back("before the run");
		} else {
			print("ufs: UEFI's list moved from %x:%x to %x:%x: that is UEFI's now\n", (uint64_t)hc_uefi_hi,
			    (uint64_t)hc_uefi_lo, (uint64_t)hi, (uint64_t)lo);
			hc_uefi_lo = lo;
			hc_uefi_hi = hi;
		}
	}
	return hc_list_is_uefis();
}

/*
 * The firmware's configuration tables, by GUID (an ACPI or device tree table among them would say
 * where the storage controller is).
 */
static void
block_tables(void)
{
	for (uint64_t i = 0; ST->tables != NULL && i < ST->nr_tables && i < 32; i++) {
		const efi_guid *g = &ST->tables[i].guid;

		print("ufs: firmware table %d: %x-%x-%x-%x%x-%x%x%x%x%x%x at %x\n", i, (uint64_t)g->a, (uint64_t)g->b,
		    (uint64_t)g->c, (uint64_t)g->d[0], (uint64_t)g->d[1], (uint64_t)g->d[2], (uint64_t)g->d[3],
		    (uint64_t)g->d[4], (uint64_t)g->d[5], (uint64_t)g->d[6], (uint64_t)g->d[7],
		    (uint64_t)(uintptr_t)ST->tables[i].table);
	}
}

/*
 * Is this partition the one a write may reach? Everything about it has to be what it was when the
 * port was set up; one field out of place and nothing is written this run. The name UEFI reports
 * is already in bp->name (ufs_name), in ASCII.
 */
static int
block_is_darwin(const efi_partition_info *info, const efi_block_io *bio, const struct loader_block_part *bp)
{
	static const efi_guid type = BLOCK_DARWIN_TYPE, uniq = BLOCK_DARWIN_UNIQ;
	static const char name[] = "darwin";
	const char *why = NULL;

	for (unsigned i = 0; i < sizeof(name); i++) {
		if (bp->name[i] != name[i]) {
			return 0;               /* another partition: nothing to say about it */
		}
	}
	if (memcmp(&info->type_guid, &type, sizeof(type)) != 0) {
		why = "its type GUID is not the one it had";
	} else if (memcmp(&info->unique_guid, &uniq, sizeof(uniq)) != 0) {
		why = "its unique GUID is not the one it had";
	} else if (info->start_lba != BLOCK_DARWIN_FIRST || info->end_lba != BLOCK_DARWIN_LAST) {
		why = "it does not begin and end where it did";
	} else if (bp->unit != 0) {
		/* the first unit UEFI lists: this firmware's device paths say no logical unit number
		 * (device_path_print finds no 3.19 node), so bp->lun is 0xff for every unit */
		why = "it is not on the first logical unit";
	} else if (bp->block_size != 4096 || bp->blocks != BLOCK_DARWIN_LAST - BLOCK_DARWIN_FIRST + 1) {
		why = "its blocks are not what they were";
	} else if (bio->media->read_only) {
		why = "UEFI gives it out read only";
	}
	if (why != NULL) {
		print("ufs: darwin: no write will be served: %s\n", why);
		return 0;
	}
	print("ufs: darwin (LBA %d..%d of unit 0): its blocks from %d on may be written this run\n",
	    info->start_lba, info->end_lba, BLOCK_WRITE_FIRST);
	return 1;
}

static void
block_start(int kernel_on_this_core)
{
	static const efi_guid part_guid = EFI_PARTITION_INFO_GUID;
	static const efi_guid blk_guid = EFI_BLOCK_IO_GUID;
	efi_handle *handles;
	uint64_t count;

	static const efi_guid dp_guid = EFI_DEVICE_PATH_GUID;
	static struct {
		efi_block_io *bio;
		const efi_device_path *path;
		uint64_t path_bytes;
		uint8_t lun;
	} units[BLOCK_MAX_UNITS];
	uint64_t nunits = 0;

	memset(&block_shared, 0, sizeof(block_shared));
	block_tables();
	/* the logical units: the block devices that are not partitions of another */
	if (!EFI_ERROR(BS->locate_handle_buffer(ByProtocol, &blk_guid, NULL, &count, &handles))) {
		for (uint64_t i = 0; i < count && nunits < BLOCK_MAX_UNITS; i++) {
			efi_block_io *bio;
			efi_device_path *dp;

			if (EFI_ERROR(BS->handle_protocol(handles[i], &blk_guid, (void **)&bio)) || bio->media->logical ||
			    bio->media->block_size == 0 || bio->media->block_size > BLOCK_BUFFER_SIZE ||
			    EFI_ERROR(BS->handle_protocol(handles[i], &dp_guid, (void **)&dp))) {
				continue;
			}
			units[nunits].bio = bio;
			units[nunits].path = dp;
			units[nunits].path_bytes = device_path_bytes(dp);
			units[nunits].lun = device_path_print(nunits, dp);
			nunits++;
		}
	}
	if (EFI_ERROR(BS->locate_handle_buffer(ByProtocol, &part_guid, NULL, &count, &handles))) {
		print("ufs: no partition information: the block service is off\n");
		return;
	}
	for (uint64_t i = 0; i < count && block_shared.parts + nunits < BLOCK_MAX_PARTS; i++) {
		struct loader_block_part *bp = &block_shared.part[block_shared.parts];
		efi_partition_info *info;
		efi_block_io *bio;
		efi_device_path *dp;

		if (EFI_ERROR(BS->handle_protocol(handles[i], &part_guid, (void **)&info)) || info->type != 2 ||
		    EFI_ERROR(BS->handle_protocol(handles[i], &blk_guid, (void **)&bio)) ||
		    bio->media->block_size == 0 || bio->media->block_size > BLOCK_BUFFER_SIZE) {
			continue;
		}
		ufs_name(info, bp->name, sizeof(bp->name));
		bp->block_size = bio->media->block_size;
		bp->blocks = bio->media->last_block + 1;
		bp->start_lba = info->start_lba;
		bp->unit = 0xffff;
		bp->lun = 0xff;
		/* its unit: the one whose device path its own begins with */
		if (!EFI_ERROR(BS->handle_protocol(handles[i], &dp_guid, (void **)&dp))) {
			uint64_t bytes = device_path_bytes(dp);

			for (uint64_t u = 0; u < nunits; u++) {
				if (units[u].path_bytes < bytes &&
				    memcmp(units[u].path, dp, units[u].path_bytes) == 0) {
					bp->unit = (uint16_t)u;
					bp->lun = units[u].lun;
				}
			}
		}
#if !FLASH_BOOT_ONLY      /* x1s (SM-G981B): the kernel gets no write path at all (owner's rule: boot only) */
		if (block_write_requested && block_is_darwin(info, bio, bp)) {
			block_write_part = (int)block_shared.parts;
		}
#endif
		block_bios[block_shared.parts++] = bio;
		if (block_probe_requested && bp->name[0] == 'd' && bp->name[1] == 'a' && bp->name[2] == 'r' &&
		    bp->name[3] == 'w' && bp->name[4] == 'i' && bp->name[5] == 'n' && bp->name[6] == 0) {
			block_io2_probe(handles[i], bio);
		}
	}
	for (uint64_t u = 0; u < nunits; u++) {
		struct loader_block_part *bp = &block_shared.part[block_shared.parts];
		char *n = bp->name;

		n[0] = 'u', n[1] = 'n', n[2] = 'i', n[3] = 't';
		n[4] = (char)('0' + u / 10 % 10), n[5] = (char)('0' + u % 10), n[6] = 0;
		if (u < 10) {
			n[4] = n[5], n[5] = 0;
		}
		bp->block_size = units[u].bio->media->block_size;
		bp->blocks = units[u].bio->media->last_block + 1;
		bp->unit = (uint16_t)u;
		bp->lun = units[u].lun;
		bp->flags = BLOCK_PART_UNIT;
		block_bios[block_shared.parts++] = units[u].bio;
	}
	/* the kernel's own transfer list: this loader's area, if the controller is as UEFI keeps it */
	if (hc_known && hc_list_is_uefis() && hc_rd(HC_UTRLDBR) == 0 && (hc_rd(HC_HCE) & 1)) {
		memset(hc_area, 0, sizeof(hc_area));
		flush_dcache((uint64_t)(uintptr_t)hc_area, (uint64_t)(uintptr_t)hc_area + sizeof(hc_area));
		block_shared.native.hc_pa = HC_PA;
		block_shared.native.area_pa = (uint64_t)(uintptr_t)hc_area;
		block_shared.native.area_size = sizeof(hc_area);
		block_shared.native.ucd_offset = HC_UCD_OFFSET;
		block_shared.native.ucd_size = HC_UCD_SIZE;
		block_shared.native.slots = HC_SLOTS;
		block_shared.native.uefi_utrlba = hc_uefi_lo;
		block_shared.native.uefi_utrlbau = hc_uefi_hi;
		print("ufs: the kernel may put its own transfer list in, at %x (this loader's), for the run\n",
		    (uint64_t)(uintptr_t)hc_area);
	} else if (hc_requested) {
		print("ufs: the kernel's own transfer list is not offered: the controller is not as UEFI keeps it\n");
	}
	/*
	 * The kernel runs on this core (the S20+): nobody serves the page while it runs, so its own
	 * driver is the only way to the storage. What it would ask the loader for -- each unit's GPT
	 * header, to know which logical unit is which -- is read here now, unit k's into the buffer
	 * at k * 4096 (bsd/dev/redwood_ufs.c, ufs_read_mailbox).
	 */
	if (kernel_on_this_core) {
		block_shared.native.flags |= BLOCK_NATIVE_ONLY;
		for (uint64_t u = 0; u < nunits; u++) {
			efi_block_io *bio = units[u].bio;

			memset(block_gpt[u], 0, 4096);
			if (bio->media->block_size != 4096 ||
			    EFI_ERROR(bio->read_blocks(bio, bio->media->media_id, 1, 4096, block_gpt[u]))) {
				print("ufs: unit %d: its GPT header not read\n", u);
			}
		}
		block_shared.native.gpt_units = (uint32_t)nunits;
		block_shared.native.gpt_pa = (uint64_t)(uintptr_t)block_gpt;
		clean_dcache((uint64_t)(uintptr_t)block_gpt, (uint64_t)(uintptr_t)block_gpt + sizeof(block_gpt));
		print("ufs: the kernel runs on this core: nothing is served while it runs; the units' GPT headers are kept "
		    "for its own driver\n");
	}
	hc_refused_reads = 0;
	hc_log_skips = 0;
	/* per run, like the two above: this loader stays resident across runs, and a tally carried
	 * over from the run before is read as this one's */
	block_writes = block_writes_refused = block_write_bytes = 0;
	if (block_write_part >= 0) {
		block_shared.write_part_plus_one = (uint32_t)block_write_part + 1;
		block_shared.write_first = BLOCK_WRITE_FIRST;
	}
	block_shared.magic = BLOCK_MAGIC;
	block_shared.version = BLOCK_VERSION;
	block_shared.size = sizeof(block_shared);
	block_shared.buffer_size = BLOCK_BUFFER_SIZE;
	block_shared.buffer_pa = (uint64_t)(uintptr_t)block_buffer;
	clean_dcache((uint64_t)(uintptr_t)&block_shared, (uint64_t)(uintptr_t)&block_shared + sizeof(block_shared));
	block_enabled = 1;
	print("ufs: block service: %d partitions and %d units, page %x, buffer %x of %d KB\n",
	    (uint64_t)block_shared.parts - nunits, nunits, (uint64_t)(uintptr_t)&block_shared, block_shared.buffer_pa,
	    (uint64_t)(BLOCK_BUFFER_SIZE >> 10));
}

/*
 * Whether the storage could have more than one request in flight (ufs_probe=1): Block I/O 2
 * on the partitions, and whether its reads come back before they are done.  Eight 4 KB
 * reads at places 64 MB apart in "darwin", one after another through Block I/O and then all
 * eight at once through Block I/O 2, each into its own part of the buffer.  Before the kernel
 * runs (the UEFI timer still ticks), and only to say what there is.
 *
 * What it says on this phone: Block I/O 2 is there and the calls come back at once (eight in
 * 160 us, none done), but the eight are done only after 61.5 ms, against 1.0 ms one at a time
 * through Block I/O -- Qualcomm's driver finishes them from the UEFI timer, which besides does
 * not tick on core 0 while the kernel runs.  So one request at a time, synchronously.
 */
static void
block_io2_probe(efi_handle h, efi_block_io *bio)
{
	static const efi_guid blk2_guid = EFI_BLOCK_IO2_GUID;
	efi_status (EFIAPI *create_event)(uint32_t, uint64_t, void *, void *, void **) =
	    (efi_status (EFIAPI *)(uint32_t, uint64_t, void *, void *, void **))BS->create_event;
	efi_status (EFIAPI *check_event)(void *) = (efi_status (EFIAPI *)(void *))BS->check_event;
	static efi_block_io2_token tokens[8];
	efi_block_io2 *bio2 = NULL;
	uint64_t bs = bio->media->block_size, t0, t_sync, t_issue = 0, t_all, freq = timer_freq();
	uint64_t returned_done = 0, pending, polls = 0;
	efi_status s;

	if (EFI_ERROR(BS->handle_protocol(h, &blk2_guid, (void **)&bio2)) || bio2 == NULL) {
		print("ufs: darwin: no Block I/O 2: one request at a time it is\n");
		return;
	}
	t0 = READ_SYSREG(CNTVCT_EL0);
	for (int i = 0; i < 8; i++) {
		s = bio->read_blocks(bio, bio->media->media_id, (uint64_t)i * (64ULL << 20) / bs + 7, bs,
		    block_buffer + i * 65536);
		if (EFI_ERROR(s)) {
			print("ufs: probe: Block I/O read failed %x\n", s);
			return;
		}
	}
	t_sync = READ_SYSREG(CNTVCT_EL0) - t0;
	for (int i = 0; i < 8; i++) {
		tokens[i].transaction_status = 0;
		if (EFI_ERROR(create_event(0, 8 /* TPL_CALLBACK */, NULL, NULL, &tokens[i].event))) {
			print("ufs: probe: no event\n");
			return;
		}
	}
	t0 = READ_SYSREG(CNTVCT_EL0);
	for (int i = 0; i < 8; i++) {
		/* other places than the synchronous reads, so that nothing the storage kept is read again */
		s = bio2->read_blocks_ex(bio2, bio2->media->media_id, (uint64_t)i * (64ULL << 20) / bs + 1031,
		    &tokens[i], bs, block_buffer + 1048576 + i * 65536);
		if (EFI_ERROR(s)) {
			print("ufs: probe: ReadBlocksEx %d failed %x\n", (uint64_t)i, s);
			return;
		}
		if (check_event(tokens[i].event) == 0) {
			returned_done++;        /* already signalled when the call came back */
		}
	}
	t_issue = READ_SYSREG(CNTVCT_EL0) - t0;
	pending = 8 - returned_done;
	while (pending > 0 && polls < 50000000) {
		pending = 0;
		for (int i = 0; i < 8; i++) {
			if (tokens[i].event != NULL && check_event(tokens[i].event) != 0) {
				pending++;
			} else if (tokens[i].event != NULL) {
				BS->close_event(tokens[i].event);
				tokens[i].event = NULL;
			}
		}
		polls++;
	}
	t_all = READ_SYSREG(CNTVCT_EL0) - t0;
	print("ufs: darwin: Block I/O 2 there. 8 random 4 KB reads: one at a time %d us; at once: the calls came back "
	    "after %d us (%d of them done by then), all done after %d us, %d still pending\n",
	    t_sync * 1000000 / freq, t_issue * 1000000 / freq, returned_done, t_all * 1000000 / freq, pending);
}

/* one request, if there is one: called from core 0's watch loop while the kernel runs */
static void
block_poll(void)
{
	uint64_t ask = (uint64_t)(uintptr_t)&block_shared.ask;
	uint32_t request, part, op;
	uint64_t lba, count, bytes, started;
	efi_status s = 0x8000000000000002ULL;     /* EFI_INVALID_PARAMETER: a request out of bounds */

	/* the kernel writes the request into memory past this core's cache: look at memory */
	flush_dcache(ask, ask + sizeof(block_shared.ask));
	request = block_shared.ask.request;
	if (request == block_shared.answer.answered) {
		return;
	}
	part = block_shared.ask.part;
	lba = block_shared.ask.lba;
	count = block_shared.ask.count;
	op = block_shared.ask.op;
	started = READ_SYSREG(CNTVCT_EL0);
	bytes = 0;
	if (part < block_shared.parts && count != 0 && count <= BLOCK_BUFFER_SIZE / block_shared.part[part].block_size &&
	    lba < block_shared.part[part].blocks && count <= block_shared.part[part].blocks - lba) {
		efi_block_io *bio = block_bios[part];

		bytes = count * block_shared.part[part].block_size;
		if (!hc_list_is_uefis()) {
			/* the kernel's own list is in: UEFI's driver must not ring now (the kernel should not ask) */
			if (hc_refused_reads++ < 4) {
				print("ufs: a read asked for while the kernel's own transfer list is in the controller: refused\n");
			}
			s = 0x800000000000000FULL;      /* EFI_ACCESS_DENIED */
			bytes = 0;
		} else if (op == BLOCK_OP_WRITE) {
			if (block_write_part < 0 || part != (uint32_t)block_write_part || lba < BLOCK_WRITE_FIRST) {
				/* not darwin, or not past what the build writes from the computer: never */
				if (block_writes_refused++ < 8) {
					print("ufs: a write of %d blocks from %d of %s: REFUSED\n", count, lba,
					    block_shared.part[part].name);
				}
				s = 0x800000000000000FULL;      /* EFI_ACCESS_DENIED */
				bytes = 0;
			} else {
				/*
				 * The kernel filled the buffer and cleaned its lines of it; drop whatever this
				 * core holds, write, and read back -- the answer says the blocks hold what the
				 * kernel gave, or the write is no good. The read back overwrites the buffer,
				 * which the kernel does not need: it has the data.
				 */
				uint32_t want;

				flush_dcache((uint64_t)(uintptr_t)block_buffer,
				    (uint64_t)(uintptr_t)block_buffer + bytes);
				want = ufs_crc32(block_buffer, bytes);
				s = bio->write_blocks(bio, bio->media->media_id, lba, bytes, block_buffer);
				if (!EFI_ERROR(s)) {
					bio->flush_blocks(bio);
					s = bio->read_blocks(bio, bio->media->media_id, lba, bytes, block_buffer);
					flush_dcache((uint64_t)(uintptr_t)block_buffer,
					    (uint64_t)(uintptr_t)block_buffer + bytes);
					if (!EFI_ERROR(s) && ufs_crc32(block_buffer, bytes) != want) {
						print("ufs: %d blocks from %d of darwin read back DIFFERENT\n",
						    count, lba);
						s = 0x800000000000000AULL;      /* EFI_VOLUME_CORRUPTED */
					}
				}
				if (!EFI_ERROR(s)) {
					block_writes++;
					block_write_bytes += bytes;
				}
			}
		} else if (op == BLOCK_OP_READ) {
			s = bio->read_blocks(bio, bio->media->media_id, lba, bytes, block_buffer);
			/* what the controller wrote is in memory; drop whatever this core held of the buffer */
			flush_dcache((uint64_t)(uintptr_t)block_buffer, (uint64_t)(uintptr_t)block_buffer + bytes);
		} else {
			bytes = 0;              /* an operation this loader does not know: nothing is touched */
		}
	}
	block_shared.answer.status = (int64_t)s;
	block_shared.answer.ticks += READ_SYSREG(CNTVCT_EL0) - started;
	if (EFI_ERROR(s)) {
		block_shared.answer.failed++;
	} else {
		block_shared.answer.served++;
		block_shared.answer.bytes += bytes;
	}
	__asm__ volatile ("dmb sy");
	block_shared.answer.answered = request;
	clean_dcache((uint64_t)(uintptr_t)&block_shared.answer,
	    (uint64_t)(uintptr_t)&block_shared.answer + sizeof(block_shared.answer));
}

static void
charger_start(void)
{
	static const efi_guid charger_ex_guid = QCOM_CHARGER_EX_GUID;
	static const efi_guid qcom_charger_guid = QCOM_CHARGER_GUID;
	efi_status ex_status, qcom_status, init_status = 0;

	if (charger_enabled || PLAT != &platform_redwood) {
		return;
	}
	ex_status = BS->locate_protocol(&charger_ex_guid, NULL, (void **)&CHARGER_EX);
	qcom_status = BS->locate_protocol(&qcom_charger_guid, NULL, (void **)&QCOM_CHARGER);
	if (!EFI_ERROR(qcom_status) && QCOM_CHARGER != NULL) {
		/* Re-open the driver's status path after QcomChargerApp handed off to the OS. */
		init_status = QCOM_CHARGER->initialize(1);
	}
	charger_enabled = (!EFI_ERROR(ex_status) && CHARGER_EX != NULL) ||
	    (!EFI_ERROR(qcom_status) && QCOM_CHARGER != NULL);
	print("battery: ChargerEx=%x QcomCharger=%x init=%x shared=%x\n", ex_status, qcom_status, init_status,
	    (uint64_t)(uintptr_t)&charger_shared);
	charger_poll();
	print("battery: flags=%x SOC=%d%% V=%d mV I=%d mA T=%d C action=%d\n", (uint64_t)charger_shared.flags,
	    (uint64_t)(int64_t)charger_shared.soc_percent, (uint64_t)(int64_t)charger_shared.voltage_mv,
	    (uint64_t)(int64_t)charger_shared.current_ma, (uint64_t)(int64_t)charger_shared.temperature_c,
	    (uint64_t)charger_shared.charger_action);
}

/* For the record: how much the kernel on the other core has logged so far, and the end of it. */
static void
note_kernel_progress(const struct kernel_live *live, uint64_t slide, uint64_t seconds)
{
	const char *log;
	uint32_t len, from;

	if (live == NULL) {
		print("%d s\n", seconds);
		return;
	}
	log = (const char *)(live->log + slide);
	/* its first words are written with the caches off: look at memory */
	flush_dcache(live->log_len + slide, live->log_len + slide + 4);
	len = *(volatile uint32_t *)(live->log_len + slide);
	if (len > live->log_head + live->log_ring) {
		len = 0;
	}
	from = len > 240 ? len - 240 : 0;
	flush_dcache((uint64_t)(uintptr_t)log + from, (uint64_t)(uintptr_t)log + len);
	print("%d s: the kernel has logged %d bytes, heartbeat %d; the last of it:\n", seconds, (uint64_t)len, live->heartbeat);
	for (uint32_t i = from; i < len; i++) {
		put_char(log[i] >= 0x20 && log[i] < 0x7f ? log[i] : log[i] == '\n' ? '\n' : '.');
	}
	print("\n");
}

/* A call of the firmware's power interface, the way this platform makes them. */
static int64_t
psci_call(uint64_t function, uint64_t a1, uint64_t a2, uint64_t a3)
{
	int64_t status;

	if (PLAT->psci_hvc) {
		register uint64_t x0 __asm__("x0") = function;
		register uint64_t x1 __asm__("x1") = a1;
		register uint64_t x2 __asm__("x2") = a2;
		register uint64_t x3 __asm__("x3") = a3;

		__asm__ volatile ("hvc #0" : "+r" (x0), "+r" (x1), "+r" (x2), "+r" (x3)
                        : : "x4", "x5", "x6", "x7", "x8", "x9", "x10", "x11", "x12", "x13", "x14", "x15", "x16", "x17");
		status = (int64_t)x0;
	} else {
		smc_x3 = a3;
		status = (int64_t)smc_call(function, a1, a2, NULL, NULL);
		smc_x3 = 0;
	}
	return status;
}

/*
 * Run the kernel on the core with that MPIDR and wait until it is back. Returns 0 when it has
 * been and gone (the caller goes on as after loader_return_stub), a negative number if the
 * firmware refused to start the core, OTHER_CPU_NOT_STARTED, or OTHER_CPU_HUNG: then the kernel
 * is still sitting on that core, but what it logged is there to be read (loader_ctx, as after a
 * return). `live` is where the kernel shows its signs of life, if it has such a place;
 * `budget` is how many seconds it has said it will run.
 */
static int64_t
run_on_other_cpu(uint64_t mpidr, uint64_t entry, uint64_t boot_args_pa, const struct kernel_live *live, uint64_t slide,
    uint64_t budget)
{
	uint64_t daif = 0, group1 = 0, started, last_pet, last_battery, last_beat_at, last_beat = 0, noted = 0;
	int64_t status;
	int arrived = 0, watching_only = 0;

	running_live = live;
	running_slide = slide;
	other_cpu_start.entry = entry;
	other_cpu_start.boot_args = boot_args_pa;
	other_cpu_stage = 0;
	other_cpu_done = 0;
	psci_by_hvc = PLAT->psci_hvc;
	/* that core starts with its caches off: all it reads must be in memory, not in this core's cache */
	clean_dcache((uint64_t)(uintptr_t)&other_cpu_start, (uint64_t)(uintptr_t)&other_cpu_start + sizeof(other_cpu_start));
	clean_dcache((uint64_t)(uintptr_t)&other_cpu_stage, (uint64_t)(uintptr_t)&other_cpu_stage + 8);
	clean_dcache((uint64_t)(uintptr_t)&other_cpu_done, (uint64_t)(uintptr_t)&other_cpu_done + 8);
	clean_dcache((uint64_t)(uintptr_t)&psci_by_hvc, (uint64_t)(uintptr_t)&psci_by_hvc + 8);
	clean_dcache((uint64_t)(uintptr_t)&other_cpu_entry, (uint64_t)(uintptr_t)&other_cpu_entry + 0x1000);
	clean_dcache((uint64_t)(uintptr_t)loader_ctx, (uint64_t)(uintptr_t)loader_ctx + sizeof(loader_ctx));

	if (PLAT->watch_quietly) {
		/* what is known so far into the partition now: after CPU_ON this core calls nothing of UEFI's */
		print("core %x: starting the kernel there; this core watches with its interrupts off\n", mpidr);
		flush_log();
		watching_only = 1;
		daif = READ_SYSREG(DAIF);
		__asm__ volatile ("msr daifset, #0xf");
	}
	/* the last argument arrives in that core's x0 (not on Samsung's firmware: other_cpu_entry finds it itself) */
	status = psci_call(PSCI_CPU_ON, mpidr, (uint64_t)(uintptr_t)&other_cpu_entry, (uint64_t)(uintptr_t)&other_cpu_start);

	started = last_pet = last_battery = last_beat_at = READ_SYSREG(CNTVCT_EL0);
	if (!PLAT->watch_quietly) {
		print("core %x: the firmware answers %d to the request to start it\n", mpidr, (uint64_t)status);
		if (PLAT->log_other_cpu) {
			flush_log();
		}
	}
	while (status == 0) {
		/*
		 * The first seconds are the ones in which a new arrangement goes wrong, and a machine
		 * that resets takes this loader's memory with it: where there is storage for the log,
		 * what the kernel is seen to do goes there as it happens. That needs UEFI's services,
		 * so until NOTE_SECONDS are over this core keeps its interrupts.
		 */
		if (!watching_only && seconds_since(started) >= noted + 2) {
			noted = seconds_since(started);
			if (PLAT->log_other_cpu) {
				if (!arrived) {
					print("%d s: the core has not arrived\n", noted);
				} else {
					note_kernel_progress(live, slide, noted);
				}
				flush_log();
			}
		}
		/*
		 * After that this core only watches: no interrupts (the kernel reprograms the interrupt
		 * controller's shared half; UEFI's handlers had better not run on top of that for long).
		 */
		if (!watching_only && seconds_since(started) >= NOTE_SECONDS) {
			watching_only = 1;
			daif = READ_SYSREG(DAIF);
			__asm__ volatile ("msr daifset, #0xf");
#ifndef GIC_V2
			group1 = READ_SYSREG(S3_0_C12_C12_7);          /* ICC_IGRPEN1_EL1 */
			__asm__ volatile ("msr S3_0_C12_C12_7, %0\nisb" : : "r" ((uint64_t)0));
#endif
		}
		/* both are written by a core with its caches off: look at memory, not at our copy */
		flush_dcache((uint64_t)(uintptr_t)&other_cpu_done, (uint64_t)(uintptr_t)&other_cpu_done + 8);
		if (other_cpu_done) {
			/* what the kernel returned, written the same way */
			flush_dcache((uint64_t)(uintptr_t)&loader_ctx[CTX_LOG_PA], (uint64_t)(uintptr_t)&loader_ctx[CTX_KSP + 1]);
			break;
		}
		if (!arrived) {
			flush_dcache((uint64_t)(uintptr_t)&other_cpu_stage, (uint64_t)(uintptr_t)&other_cpu_stage + 8);
			arrived = other_cpu_stage != 0;
			if (!arrived && seconds_since(started) >= START_SECONDS) {
				status = OTHER_CPU_NOT_STARTED;
			}
		}
		/*
		 * The watchdog is this core's to feed for as long as the kernel is expected to run:
		 * the kernel does it too, but whether that counts from another core is not known.
		 * After that a kernel that neither returns nor hangs visibly meets the watchdog.
		 */
		if (wdt_armed && seconds_since(last_pet) >= 5 && seconds_since(started) < budget + 60) {
			wdt_pet();
			last_pet = READ_SYSREG(CNTVCT_EL0);
		}
		/* Qualcomm's UEFI charger stack owns the PMIC/GLINK side; keep XNU's cache live. */
		if (charger_enabled && seconds_since(last_battery) >= 5 && hc_list_is_uefis()) {
			/* (not while the kernel's own transfer list is in: Qualcomm's DXEs are not to touch the storage) */
			charger_poll();
			last_battery = READ_SYSREG(CNTVCT_EL0);
		}
		if (arrived && live != NULL) {
			if (live->heartbeat != last_beat) {
				last_beat = live->heartbeat;
				last_beat_at = READ_SYSREG(CNTVCT_EL0);
			} else if (seconds_since(last_beat_at) >= SILENCE_SECONDS) {
				log_of_hung_kernel(live, slide);
				status = OTHER_CPU_HUNG;
			}
		}
		if (block_enabled) {
			block_poll();
		}
		if (!PLAT->watch_quietly) {
			usb_poll_command();
		}
		if (block_enabled) {
			/*
			 * The pause is spent looking at the kernel's mailbox: with one look a pass, a
			 * request waited 60-75 us on average before this core noticed it, more than a
			 * 4 KB ReadBlocks takes (27 us). Same length of pause as below.
			 */
			for (int k = 0; k < 64; k++) {
				block_poll();
				for (volatile int i = 0; i < 300; i++) {
				}
			}
		} else {
			for (volatile int i = 0; i < 20000; i++) {
			}
		}
	}
	running_live = NULL;
	if (PLAT->watch_quietly) {
		print("core %x: the firmware answered %d to the request to start it\n", mpidr, (uint64_t)status);
	}

	if (watching_only) {
#ifndef GIC_V2
		__asm__ volatile ("msr S3_0_C12_C12_7, %0\nisb" : : "r" (group1));
#endif
		__asm__ volatile ("msr daif, %0" : : "r" (daif));
	}
	return status;
}

/* ------------------------------------------------------------------------ */
/* Hypervisor watchdog (Qualcomm "Haven" virtual watchdog, SMC interface as  */
/* used by Linux drivers/virt/haven/hh_virt_wdt.c). Armed for the whole stay */
/* in the loader and the kernel: any hang ends in a reset without a human.   */

#define VIRT_WDT_CONTROL   0x86000005u
#define VIRT_WDT_STATUS    0x86000006u
#define VIRT_WDT_PET       0x86000007u
#define VIRT_WDT_SET_TIME  0x86000008u
#define WDT_BITE_MS        30000u

static const struct platform *PLAT;
static int wdt_armed;

static uint64_t smc_x3;             /* the fourth argument register, for the one call that has one */

static uint64_t
smc_call(uint64_t id, uint64_t a1, uint64_t a2, uint64_t *r1, uint64_t *r2)
{
	register uint64_t x0 __asm__("x0") = id;
	register uint64_t x1 __asm__("x1") = a1;
	register uint64_t x2 __asm__("x2") = a2;
	register uint64_t x3 __asm__("x3") = smc_x3;

	__asm__ volatile ("smc #0" : "+r" (x0), "+r" (x1), "+r" (x2), "+r" (x3)
                : : "x4", "x5", "x6", "x7", "x8", "x9", "x10", "x11", "x12", "x13", "x14", "x15", "x16", "x17");
	if (r1 != NULL) {
		*r1 = x1;
	}
	if (r2 != NULL) {
		*r2 = x2;
	}
	return x0;
}

static void
wdt_pet(void)
{
	if (wdt_armed) {
		smc_call(VIRT_WDT_PET, 0, 0, NULL, NULL);
	}
}

/* Change the time to the bite, and start counting again. The hypervisor takes 16 bits of milliseconds. */
static void
wdt_set_ms(uint64_t ms)
{
	if (ms > 65000) {
		ms = 65000;
	}
	if (wdt_armed) {
		smc_call(VIRT_WDT_SET_TIME, ms, ms, NULL, NULL);
		smc_call(VIRT_WDT_PET, 0, 0, NULL, NULL);
	}
}

static void
wdt_arm(void)
{
	uint64_t ctl = 0, since = 0;

	if (PLAT != &platform_redwood) {
		return; /* SMC is not routed anywhere useful on QEMU virt */
	}
	uint64_t st = smc_call(VIRT_WDT_STATUS, 0, 0, &ctl, &since);
	uint64_t r_time = smc_call(VIRT_WDT_SET_TIME, WDT_BITE_MS, WDT_BITE_MS, NULL, NULL);
	uint64_t r_ctl = smc_call(VIRT_WDT_CONTROL, 3, 0, NULL, NULL);
	uint64_t r_pet = smc_call(VIRT_WDT_PET, 0, 0, NULL, NULL);
	print("watchdog: status ret=%x ctl=%x since_pet=%x; set_time=%x enable=%x pet=%x\n", st, ctl, since, r_time,
	    r_ctl, r_pet);
	wdt_armed = (r_pet == 0);
}

/* ------------------------------------------------------------------------ */
/* UEFI exception hook: put the fault into the log instead of on the screen  */

extern char __ImageBase;

static void EFIAPI
exception_handler(int64_t type, uint64_t *ctx)
{
	print("\nUEFI EXCEPTION type=%d elr=%x esr=%x far=%x lr=%x sp=%x spsr=%x\n", (uint64_t)type, ctx[EXC_CTX_ELR],
	    ctx[EXC_CTX_ESR], ctx[EXC_CTX_FAR], ctx[EXC_CTX_LR], ctx[EXC_CTX_SP], ctx[EXC_CTX_SPSR]);
	print("loader image base %x (elr offset %x)\n", (uint64_t)(uintptr_t)&__ImageBase,
	    ctx[EXC_CTX_ELR] - (uint64_t)(uintptr_t)&__ImageBase);
	flush_log();
	if (PLAT == &platform_y2s) {
		/*
		 * Nobody resets this phone unattended, and a reset wipes the message before anyone can
		 * read it (the log write may fail in this state too). Keep it on the screen: Vol-Down +
		 * Power for 10-15 s resets.
		 */
		print("held here: photograph this screen, then hold Vol-Down + Power to reset\n");
		for (;;) {
		}
	}
	ST->rt->reset_system(0 /* EfiResetCold */, 0, 0, NULL);
	for (;;) {
	}
}

static void
install_exception_hook(void)
{
	static const efi_guid cpu_guid = EFI_CPU_ARCH_GUID;
	efi_cpu_arch *cpu;

	if (EFI_ERROR(BS->locate_protocol(&cpu_guid, NULL, (void **)&cpu))) {
		return;
	}
	for (int64_t type = 0; type <= 3; type += 3) { /* synchronous, SError */
		cpu->register_interrupt_handler(cpu, type, NULL);
		efi_status s = cpu->register_interrupt_handler(cpu, type, exception_handler);
		if (EFI_ERROR(s)) {
			print("exception hook %d: status %x\n", (uint64_t)type, s);
		}
	}
}

/* ------------------------------------------------------------------------ */
/* Payload                                                                  */

struct payload_info {
	const uint8_t *kernel;
	const uint8_t *ramdisk;
	uint32_t ramdisk_size;
	const char *cmdline;
	uint64_t checkpoint;
	uint64_t forced_base;
	uint64_t resident_rd;           /* size of a root image the host put at the resident address */
	int usb_mode;
	int devmode;                    /* payload entry "devmode": the HackiOS menu, countdown and text (development) */
	char kpart[32];                 /* where the kernel lives on storage, if not in the payload */
	uint64_t koff;                  /* byte offset of the kernel inside that partition */
	char rpart[32];                 /* same for the root ramdisk (md1) */
	uint64_t roff, rsize;
	char rdpart[32];                /* same for the resident root image (md0) */
	uint64_t rdoff, rdsize;
};

static uint64_t
parse_number(const char *c, unsigned base)
{
	uint64_t v = 0;
	for (; *c; c++) {
		uint64_t d = (*c >= '0' && *c <= '9') ? (uint64_t)(*c - '0') : (*c >= 'a' && *c <= 'f') ? (uint64_t)(*c - 'a' + 10) : 16;
		if (d >= base) {
			break;
		}
		v = v * base + d;
	}
	return v;
}

static int
parse_payload(struct payload_info *pi, int verbose)
{
	struct payload_header *ph = (struct payload_header *)PAYLOAD_BASE;

	memset(pi, 0, sizeof(*pi));
	pi->cmdline = "";
	if (ph->magic != PAYLOAD_MAGIC) {
		print("no payload at %x (magic %x)\n", PAYLOAD_BASE, (uint64_t)ph->magic);
		return 0;
	}
	const uint8_t *p = (const uint8_t *)(ph + 1), *pend = p + ph->size;
	while (p + sizeof(struct payload_entry) <= pend) {
		const struct payload_entry *e = (const struct payload_entry *)p;
		const char *text = (const char *)(p + sizeof(*e));
		if (e->name[0] == 0) {
			break;
		}
		if (verbose) {
			print("payload entry '%s' size %x\n", e->name, (uint64_t)e->size);
		}
		if (str_eq(e->name, "ramdisk")) {
			pi->ramdisk = p + sizeof(*e);             /* becomes /dev/md0, the root device */
			pi->ramdisk_size = e->size;
		} else if (str_eq(e->name, "kernel")) {
			pi->kernel = p + sizeof(*e);
		} else if (str_eq(e->name, "cmdline")) {
			pi->cmdline = text;
		} else if (str_eq(e->name, "physbase")) {
			pi->forced_base = parse_number(text, 16); /* testing aid; must be free RAM */
		} else if (str_eq(e->name, "rdres")) {
			pi->resident_rd = parse_number(text, 16); /* e.g. loaded there by QEMU; see RESIDENT_RD_OFFSET */
		} else if (str_eq(e->name, "checkpoint")) {
			pi->checkpoint = parse_number(text, 10);  /* where the kernel should stop and come back */
		} else if (str_eq(e->name, "usb")) {
			pi->usb_mode = 1;                         /* stay in the loader and take commands over USB */
		} else if (str_eq(e->name, "devmode")) {
			pi->devmode = 1;                          /* the developer's boot screen (menu, countdown, text) */
		} else if (str_eq(e->name, "kpart")) {
			/* the kernel is not in the payload: it sits on a GPT partition */
			unsigned k = 0;
			while (k + 1 < sizeof(pi->kpart) && text[k] != 0) {
				pi->kpart[k] = text[k];
				k++;
			}
			pi->kpart[k] = 0;
		} else if (str_eq(e->name, "koff")) {
			pi->koff = parse_number(text, 16);
		} else if (str_eq(e->name, "rpart")) {
			unsigned k = 0;
			while (k + 1 < sizeof(pi->rpart) && text[k] != 0) {
				pi->rpart[k] = text[k];
				k++;
			}
			pi->rpart[k] = 0;
		} else if (str_eq(e->name, "roff")) {
			pi->roff = parse_number(text, 16);
		} else if (str_eq(e->name, "rsize")) {
			pi->rsize = parse_number(text, 16);
		} else if (str_eq(e->name, "rdpart")) {
			unsigned k = 0;
			while (k + 1 < sizeof(pi->rdpart) && text[k] != 0) {
				pi->rdpart[k] = text[k];
				k++;
			}
			pi->rdpart[k] = 0;
		} else if (str_eq(e->name, "rdoff")) {
			pi->rdoff = parse_number(text, 16);
		} else if (str_eq(e->name, "rdsize")) {
			pi->rdsize = parse_number(text, 16);
		}
		p += sizeof(*e) + ((e->size + 15) & ~15u);
	}
	return 1;
}

/* ------------------------------------------------------------------------ */
/* NVRAM image                                                              */
/*                                                                          */
/* iBoot hands the kernel a copy of the NVRAM contents in CHRP format       */
/* (iokit/Kernel/IONVRAMCHRPHandler.cpp). There is no NVRAM here, so build  */
/* an image with a valid header and one empty "common" partition.           */

static void
nvram_chrp_header(uint8_t *h, uint8_t sig, uint32_t bytes, const char *name)
{
	uint16_t blocks = (uint16_t)(bytes / 16), sum;

	memset(h, 0, 16);
	h[0] = sig;
	h[2] = (uint8_t)blocks;
	h[3] = (uint8_t)(blocks >> 8);
	memcpy(h + 4, name, str_len(name));
	/* checksum over signature, length and name, folded to 8 bits */
	sum = h[0];
	for (int i = 2; i < 16; i++) {
		sum += h[i];
	}
	while (sum > 0xff) {
		sum = (sum & 0xff) + (sum >> 8);
	}
	h[1] = (uint8_t)sum;
}

static void
build_nvram_image(uint8_t *image)
{
	uint32_t lo = 1, hi = 0, generation = 1;

	memset(image, 0, NVRAM_BANK_SIZE);
	nvram_chrp_header(image, 0x5a, 0x20, "nvram");                           /* Apple header: 32 bytes */
	nvram_chrp_header(image + 0x20, 0x70, NVRAM_BANK_SIZE - 0x20, "common"); /* the rest: common partition */
	memcpy(image + 20, &generation, 4);
	/* Adler-32 over everything after the checksum field itself */
	for (uint32_t i = 20; i < NVRAM_BANK_SIZE; i++) {
		lo = (lo + image[i]) % 65521;
		hi = (hi + lo) % 65521;
	}
	uint32_t adler = (hi << 16) | lo;
	memcpy(image + 16, &adler, 4);
}

/* ------------------------------------------------------------------------ */
/* Running the kernel                                                       */

static efi_gop *GOP;
/* The display handed to the kernel: the firmware's, or one we set up ourselves (QEMU). base 0 = none. */
static struct {
	uint64_t base;
	uint32_t width, height, stride;
} SCREEN;
static uint64_t win_base, win_size; /* XNU's physical memory window, claimed once */
static uint64_t resident_rd_size;   /* root image kept at win_base + RESIDENT_RD_OFFSET, 0 if none */
static char last_result[64] = "idle";

static void
set_result(const char *what, uint64_t a, uint64_t b)
{
	uint32_t saved = log_len;
	uint32_t start = log_len;
	print("%s %d %x", what, a, b);
	uint32_t n = log_len - start;
	if (n > sizeof(last_result) - 1) {
		n = sizeof(last_result) - 1;
	}
	memcpy(last_result, log_buf + start, n);
	last_result[n] = 0;
	log_len = saved;
}

/* Every range of the firmware's memory map of 1 MB or more, and the total of each type. */
static void
print_memory_map(void)
{
	static uint8_t map_buf[65536];
	uint64_t map_size = sizeof(map_buf), map_key, desc_size, totals[16];
	uint32_t desc_ver;

	if (EFI_ERROR(BS->get_memory_map(&map_size, map_buf, &map_key, &desc_size, &desc_ver))) {
		print("GetMemoryMap failed\n");
		return;
	}
	memset(totals, 0, sizeof(totals));
	print("memory map (type 7 = free, 1/2 = loader, 3/4 = boot services, 5/6 = runtime, 0 = reserved):\n");
	for (uint64_t off = 0; off < map_size; off += desc_size) {
		efi_memory_descriptor *d = (efi_memory_descriptor *)(map_buf + off);

		if (d->type < 16) {
			totals[d->type] += d->pages << 12;
		}
		if (d->pages >= 256) {
			print("  %x - %x  type %d  %d MB\n", d->phys_start, d->phys_start + (d->pages << 12), (uint64_t)d->type,
			    d->pages >> 8);
		}
	}
	for (int t = 0; t < 16; t++) {
		if (totals[t] != 0) {
			print("  total of type %d: %d MB\n", (uint64_t)t, totals[t] >> 20);
		}
	}
}

/*
 * y2s: memory for the Wi-Fi chip's DMA. Samsung's hypervisor lets the PCIe block (HSI1) reach memory
 * only where the kernel grants it, 64 KB at a time (HVC 0xc6000101), and z3s, which has this chip
 * working, grants only between 2 and 4 GB; the kernel's own window is above 4 GB. So a piece of it
 * there, never UEFI's again, said in /chosen as wifi-dma-base and wifi-dma-size.
 */
#define WIFI_DMA_SIZE   (32ULL << 20)
static uint64_t wifi_dma_base, wifi_dma_size;
static int cellular_memory_ready;
#ifdef CELLULAR_CP_BOOT_PREFLIGHT
static int cellular_cp_boot_can_reset(void);
static int cellular_cp_retry_stop(void);
#endif

/* Allocate exactly the usable span, aligned and excluding the S20 CP map.
 * Works with old UEFI too, but only a rebuilt UEFI reserves CP memory from ALL
 * firmware allocations. This alone is not permission to start modem DMA. */
static int
allocate_loader_memory(uint64_t size, uint64_t alignment, uint64_t low, uint64_t high, uint64_t *address)
{
	static uint8_t map_buf[65536];
	uint64_t map_size = sizeof(map_buf), key, stride, best = 0;
	uint32_t version;
	if (EFI_ERROR(BS->get_memory_map(&map_size, map_buf, &key, &stride, &version)) ||
	    stride < sizeof(efi_memory_descriptor) || map_size > sizeof(map_buf)) return 0;
	for (uint64_t off = 0; off + stride <= map_size; off += stride) {
		efi_memory_descriptor *d = (efi_memory_descriptor *)(map_buf + off);
		if (d->type != EfiConventionalMemory || d->pages > (~0ULL - d->phys_start) / 4096) continue;
		uint64_t lo = d->phys_start > low ? d->phys_start : low;
		uint64_t hi = d->phys_start + (d->pages << 12);
		if (hi > high) hi = high;
		uint64_t at = s20_memory_candidate(lo, hi, size, alignment);
		if (at > best) best = at;
	}
	if (!best) {
		print("cellular allocation: no candidate size %x align %x low %x high %x\n", size, alignment, low, high);
		return 0;
	}
	efi_status status = BS->allocate_pages(AllocateAddress, EfiLoaderData, size >> 12, &best);
	if (EFI_ERROR(status)) {
		print("cellular allocation: address %x size %x status %x\n", best, size, status);
		return 0;
	}
	*address = best;
	return 1;
}

static int
allocate_low_dma(uint64_t size, uint64_t alignment, uint64_t *address)
{
	return allocate_loader_memory(size, alignment, 0x80000000ULL, 0x100000000ULL, address);
}

static int
reserve_download_buffer(void)
{
	if (download_base) return 1;
	if (PLAT == &platform_y2s) {
		if (!allocate_low_dma(DOWNLOAD_MAX_SIZE, 0x10000, &download_base)) return 0;
	} else if (!allocate_loader_memory(DOWNLOAD_MAX_SIZE, 0x1000, 0x1000, ~0ULL, &download_base)) return 0;
	print("download buffer: %x size %x\n", download_base, (uint64_t)DOWNLOAD_MAX_SIZE);
	return 1;
}

/* Confirm firmware ownership separately from our allocator exclusions. A new
 * loader on an old UEFI must not claim that the CP is fully reserved. */
static efi_dxe_services *
cellular_dxe_services(void)
{
	static const efi_guid guid = {0x05ad34ba, 0x6f02, 0x4214, {0x95,0x2e,0x4d,0xa0,0x39,0x8e,0x2b,0xb9}};
	if (ST->nr_tables > 256) return NULL;
	for (uint64_t i = 0; i < ST->nr_tables; i++) {
		if (memcmp(&ST->tables[i].guid, &guid, sizeof(guid))) continue;
		efi_dxe_services *dxe = ST->tables[i].table;
		if (dxe && dxe->hdr.signature == 0x565245535f455844ULL && dxe->hdr.header_size >= sizeof(*dxe)) return dxe;
	}
	return NULL;
}

static int
cellular_normal_nc(uint64_t base, uint64_t size)
{
	efi_dxe_services *dxe = cellular_dxe_services();
	efi_gcd_memory_descriptor d;
	if (!dxe || (READ_SYSREG(CurrentEL) >> 2) != 1 ||
	    EFI_ERROR(dxe->get_memory_space_descriptor(base, &d)) ||
	    d.base > base || d.length > ~0ULL - d.base || d.base + d.length < base + size ||
	    (d.attributes & 0x1f) != 2 /* EFI_MEMORY_WC = Normal NC in ArmMmuLib */) return 0;
	uint64_t saved = READ_SYSREG(PAR_EL1);
	int okay = 1;
	for (uint64_t address = base; address < base + size; address += 4096) {
		__asm__ volatile ("at s1e1r, %0; isb" :: "r"(address) : "memory");
		if (!s20_par_normal_nc(READ_SYSREG(PAR_EL1), address)) { okay = 0; break; }
	}
	__asm__ volatile ("msr par_el1, %0; isb" :: "r"(saved) : "memory");
	return okay;
}

static void
report_cellular_memory(void)
{
	static uint8_t map_buf[65536];
	uint64_t map_size = sizeof(map_buf), key, stride;
	uint32_t version, reserved = 0, normal_nc = 0;
	cellular_memory_ready = 0;
	if (PLAT != &platform_y2s) return;
	if (EFI_ERROR(BS->get_memory_map(&map_size, map_buf, &key, &stride, &version)) ||
	    stride < sizeof(efi_memory_descriptor) || map_size > sizeof(map_buf)) {
		print("cellular memory: reserved 0/3 (memory map unavailable)\n");
		return;
	}
	for (unsigned i = 0; i < sizeof(s20_cp_regions) / sizeof(s20_cp_regions[0]); i++) {
		int owned = s20_efi_map_covers(map_buf, map_size, stride,
		    s20_cp_regions[i].base, s20_cp_regions[i].size, EfiReservedMemoryType, 0);
		reserved += owned;
		if (owned) normal_nc += cellular_normal_nc(s20_cp_regions[i].base, s20_cp_regions[i].size);
	}
	print("cellular memory: reserved %d/3 (UEFI ownership only)\n", (uint64_t)reserved);
	print("cellular memory: normal-nc %d/3 (DXE and live EL1 translation; every CP page)\n", (uint64_t)normal_nc);
	cellular_memory_ready = reserved == 3 && normal_nc == 3;
}

static void
reserve_wifi_dma(void)
{
	uint64_t at;

	if (wifi_dma_size != 0 || PLAT != &platform_y2s) {
		if (wifi_dma_size) print("wifi dma: %x size %x (reused)\n", wifi_dma_base, wifi_dma_size);
		return;
	}
	if (!allocate_low_dma(WIFI_DMA_SIZE, 0x10000, &at)) {
		print("wifi dma: no %d MB below 4 GB\n", (uint64_t)(WIFI_DMA_SIZE >> 20));
		return;
	}
	if (at < 0x80000000ULL) {
		print("wifi dma: %x is not where the PCIe block may be granted: not used\n", at);
		BS->free_pages(at, WIFI_DMA_SIZE >> 12);
		return;
	}
	wifi_dma_base = at;     /* aligned to the 64-KiB S2MPU grant granule */
	wifi_dma_size = WIFI_DMA_SIZE;
	print("wifi dma: %x size %x\n", wifi_dma_base, wifi_dma_size);
}

/*
 * y2s: memory for the display controller's DMA to read CoreAnimation's frames from (the kernel's
 * fb_scaler, MiniIOSurface.cpp). Its base registers hold 32 bits and nothing translates its
 * addresses, and XNU's own memory is all above 4 GB (its VM found no page below: "scanned 1
 * pages", 25 October). So a piece below 4 GB, never UEFI's again, said in /chosen as
 * display-pool-base and display-pool-size: room for four full-screen frames.
 */
static uint64_t display_pool_base, display_pool_size;

/*
 * As much as is free of 192, 128 or 64 MB (13, 9 or 4 slots of 14 MB): the window server's frames
 * and an application's full-screen layers all want a slot, and memory below 4 GB is nobody
 * else's (XNU has none of it).
 */
static void
reserve_display_pool(void)
{
	static const uint64_t sizes[] = { 192ULL << 20, 128ULL << 20, 64ULL << 20 };

	if (display_pool_size != 0 || PLAT != &platform_y2s) {
		if (display_pool_size) print("display pool: %x size %x (reused)\n", display_pool_base, display_pool_size);
		return;
	}
	for (unsigned i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
		uint64_t at;

		if (allocate_low_dma(sizes[i], 0x10000, &at)) {
			display_pool_base = at;
			display_pool_size = sizes[i];
			print("display pool: %x size %x\n", display_pool_base, display_pool_size);
			return;
		}
	}
	print("display pool: not even 64 MB below 4 GB\n");
}

static int
choose_window(const struct payload_info *pi)
{
	static uint8_t map_buf[65536];
	uint64_t map_size = sizeof(map_buf), map_key, desc_size;
	uint32_t desc_ver;
	uint64_t best_base = 0, best_size = 0;
	uint64_t pay_lo = PAYLOAD_BASE, pay_hi = PAYLOAD_BASE + PAYLOAD_MAX_SIZE;

	if (win_size != 0) {
		return 1;
	}
	if (EFI_ERROR(BS->get_memory_map(&map_size, map_buf, &map_key, &desc_size, &desc_ver))) {
		print("GetMemoryMap failed\n");
		return 0;
	}
	for (uint64_t off = 0; off < map_size; off += desc_size) {
		efi_memory_descriptor *d = (efi_memory_descriptor *)(map_buf + off);
		if (d->type != EfiConventionalMemory) {
			continue;
		}
		uint64_t lo = d->phys_start, hi = d->phys_start + (d->pages << 12);
		/* the payload sits in RAM that UEFI may consider free: stay clear of it */
		if (lo < pay_hi && hi > pay_lo) {
			if (pay_lo - lo > hi - pay_hi && pay_lo > lo) {
				hi = pay_lo;
			} else {
				lo = pay_hi;
			}
		}
		lo = (lo + BASE_ALIGN - 1) & ~(BASE_ALIGN - 1);
		hi &= ~(BASE_ALIGN - 1);
		if (hi > lo && hi - lo > best_size) {
			best_base = lo;
			best_size = hi - lo;
		}
	}
	print("largest free block %x size %x\n", best_base, best_size);
	if (pi->forced_base != 0) {
		best_base = pi->forced_base;
		best_size = MAX_MEM_SIZE;
	}
	if (best_size > MAX_MEM_SIZE) {
		best_size = MAX_MEM_SIZE;
	}
	/* UEFI stays alive while the kernel runs, so the kernel must own all of its window. */
	uint64_t claim = best_base;
	efi_status s = BS->allocate_pages(AllocateAddress, EfiLoaderData, best_size >> 12, &claim);
	print("phys window %x size %x, claim status %x\n", best_base, best_size, s);
	if (EFI_ERROR(s)) {
		return 0;
	}
	win_base = best_base;
	win_size = best_size;
	return 1;
}

/*
 * The kernel does not have to live in the payload: a "kpart"/"koff" pair in the
 * payload points at a GPT partition and a byte offset, and the image is read
 * into the download staging area. This keeps the UEFI image small and lets the
 * kernel be replaced from Android with dd.
 */
static int
read_partition_into(const char *part, uint64_t off, void *dest, uint64_t len)
{
	efi_block_io *bio = find_partition_ascii(part);
	uint8_t *d = dest;
	uint64_t bs, lba, left = len, done = 0;

	if (bio == NULL || bio->media == NULL) {
		print("storage: no partition '%s'\n", part);
		return 0;
	}
	bs = bio->media->block_size;
	if (bs == 0 || (off % bs) != 0) {
		print("storage: bad offset %x\n", off);
		return 0;
	}
	lba = off / bs;
	while (left != 0) {
		uint64_t chunk = left > (4ULL << 20) ? (4ULL << 20) : left;
		if (EFI_ERROR(bio->read_blocks(bio, bio->media->media_id, lba, chunk, d + done))) {
			print("storage: read failed at %x\n", off + done);
			return 0;
		}
		done += chunk;
		lba += chunk / bs;
		left -= chunk;
	}
	return 1;
}

static int
load_kernel_from_partition(const char *part, uint64_t off)
{
	uint8_t *dest = (uint8_t *)(uintptr_t)download_base;

	if (!read_partition_into(part, off, dest, DOWNLOAD_MAX_SIZE)) {
		return 0;
	}
	if (*(const uint32_t *)dest != 0xfeedfacf) {
		print("kernel: bad Mach-O magic %x at '%s'+%x\n", (uint64_t)*(const uint32_t *)dest, part, off);
		return 0;
	}
	print("kernel: read %x bytes from '%s' at %x\n", (uint64_t)DOWNLOAD_MAX_SIZE, part, off);
	return 1;
}

/*
 * Load and run the kernel from the payload. Returns when the kernel comes
 * back (panic, checkpoint, early exception); the combined log is staged and
 * written to the log partition. Returns 0 if the kernel could not be started.
 */
static int starts_with(const char *s, const char *prefix);

static int
run_kernel(int have_checkpoint, uint64_t checkpoint)
{
	static uint8_t map_buf[65536];
	uint64_t map_size, map_key, desc_size;
	uint32_t desc_ver;
	struct payload_info pi;

	if (!parse_payload(&pi, 1)) {
		print("no payload\n");
		return 0;
	}
	/* The menu clears its log before each run; repeat boot evidence here. */
	print("download buffer: %x size %x\n", download_base, (uint64_t)DOWNLOAD_MAX_SIZE);
	report_cellular_memory();
	if (pi.kernel == NULL && pi.kpart[0] != 0) {
		if (!load_kernel_from_partition(pi.kpart, pi.koff)) {
			return 0;
		}
		pi.kernel = (const uint8_t *)(uintptr_t)download_base;
	}
	if (pi.kernel == NULL) {
		print("no kernel in payload\n");
		return 0;
	}
	if (pi.ramdisk == NULL && pi.rpart[0] != 0) {
		pi.ramdisk_size = pi.rsize;      /* read straight into rd_pa below */
	}
	if (!have_checkpoint) {
		checkpoint = pi.checkpoint;
	}
	const uint8_t *kernel = pi.kernel;
	const struct mach_header_64 *mh = (const struct mach_header_64 *)kernel;
	if (mh->magic != MH_MAGIC_64) {
		print("kernel is not a 64-bit Mach-O\n");
		return 0;
	}

	/* --- kernel extent -------------------------------------------------- */
	uint64_t vm_lo = ~0ULL, vm_hi = 0, entry_va = 0;
	const struct load_command *lc = (const struct load_command *)(mh + 1);
	for (uint32_t i = 0; i < mh->ncmds; i++, lc = (const void *)((const uint8_t *)lc + lc->cmdsize)) {
		if (lc->cmd == LC_SEGMENT_64) {
			const struct segment_command_64 *sg = (const void *)lc;
			if (sg->vmsize == 0) {
				continue;
			}
			if (sg->vmaddr < vm_lo) {
				vm_lo = sg->vmaddr;
			}
			if (sg->vmaddr + sg->vmsize > vm_hi) {
				vm_hi = sg->vmaddr + sg->vmsize;
			}
		} else if (lc->cmd == LC_UNIXTHREAD) {
			/* cmd, cmdsize, flavor, count, x0..x28, fp, lr, sp, pc */
			entry_va = ((const uint64_t *)((const uint8_t *)lc + 16))[32];
		}
	}
	print("kernel va %x..%x entry %x\n", vm_lo, vm_hi, entry_va);
	if (vm_lo < KERNEL_VIRT_BASE + KERNEL_LINK_OFFSET || entry_va == 0) {
		print("unexpected kernel layout\n");
		return 0;
	}
	if (!choose_window(&pi)) {
		return 0;
	}

	uint64_t phys_base = win_base;
	uint64_t slide = phys_base - KERNEL_VIRT_BASE; /* pa = va + slide */
	uint64_t kern_lo = vm_lo + slide, kern_hi = (vm_hi + slide + PAGE_SIZE_MAX - 1) & ~(PAGE_SIZE_MAX - 1);
	/* The ramdisk must sit inside the kernel's static mapping, i.e. below topOfKernelData. */
	uint64_t rd_pa = kern_hi;
	uint64_t rd_size = (pi.ramdisk_size + PAGE_SIZE_MAX - 1) & ~(PAGE_SIZE_MAX - 1);
	int rd_resident = 0;
	/* A resident image next to a payload ramdisk: two disks (which is which is decided where the device tree is written). */
	uint64_t rd1_pa = 0, rd1_size = 0;

	/*
	 * ufs_root=: the kernel mounts its root from the flash itself (bsd/dev/redwood_ufs.c): the
	 * system image is not copied into memory, and the payload's small image is the only RAM disk,
	 * md1 as always ("RAMDisk1" alone below).
	 */
	int root_on_flash = 0;

	for (const char *c = pi.cmdline; *c; c++) {
		root_on_flash |= starts_with(c, "ufs_root=") && (c == pi.cmdline || c[-1] == ' ');
	}
	if (pi.resident_rd != 0) {
		resident_rd_size = pi.resident_rd;
	}
	if (pi.rdpart[0] != 0) {
		resident_rd_size = pi.rdsize;    /* read straight to res_pa below */
	}
	if (root_on_flash) {
		resident_rd_size = 0;
		print("the root is on the flash (ufs_root=): the system image is not read into memory\n");
	}
	if (resident_rd_size != 0) {
		uint64_t res_pa = phys_base + RESIDENT_RD_OFFSET;
		uint64_t res_size = (resident_rd_size + PAGE_SIZE_MAX - 1) & ~(PAGE_SIZE_MAX - 1);

		if (pi.rdpart[0] != 0) {
			print("resident image: reading %x bytes from '%s'+%x\n", resident_rd_size, pi.rdpart, pi.rdoff);
			if (!read_partition_into(pi.rdpart, pi.rdoff, (void *)res_pa, resident_rd_size)) {
				return 0;
			}
			print("resident image: read\n");
		}
		if (kern_hi + rd_size > res_pa) {
			print("kernel and ramdisk overlap the resident image\n");
			return 0;
		}
		if (rd_size == 0) {
			rd_pa = res_pa;
			rd_size = res_size;
			rd_resident = 1;
		} else {
			rd1_pa = res_pa;
			rd1_size = res_size;
		}
	}
	/* everything up to topOfKernelData is the kernel's static region: it must cover both images */
	uint64_t dt_pa = rd1_size != 0 ? rd1_pa + rd1_size : rd_pa + rd_size;
	uint64_t dt_max = 0x10000;
	uint64_t ba_pa = dt_pa + dt_max;
	uint64_t top = ba_pa + PAGE_SIZE_MAX;

	if (win_size < top - phys_base + 0x4000000ULL) {
		print("not enough contiguous RAM\n");
		return 0;
	}

	/* --- load segments -------------------------------------------------- */
	lc = (const struct load_command *)(mh + 1);
	for (uint32_t i = 0; i < mh->ncmds; i++, lc = (const void *)((const uint8_t *)lc + lc->cmdsize)) {
		const struct segment_command_64 *sg = (const void *)lc;
		if (lc->cmd != LC_SEGMENT_64 || sg->vmsize == 0) {
			continue;
		}
		memset((void *)(sg->vmaddr + slide), 0, sg->vmsize);
		memcpy((void *)(sg->vmaddr + slide), kernel + sg->fileoff, sg->filesize);
	}
	print("kernel loaded at pa %x..%x\n", kern_lo, kern_hi);
	if (rd_resident) {
		print("resident ramdisk at pa %x size %x\n", rd_pa, rd_size);
	} else if (rd_size != 0) {
		memset((void *)rd_pa, 0, rd_size);
		if (pi.ramdisk != NULL) {
			memcpy((void *)rd_pa, pi.ramdisk, pi.ramdisk_size);
		} else if (!read_partition_into(pi.rpart, pi.roff, (void *)rd_pa, pi.ramdisk_size)) {
			return 0;
		}
		print("ramdisk at pa %x size %x\n", rd_pa, rd_size);
	}
	if (rd1_size != 0) {
		print("resident image at pa %x size %x: it is the root (md0), the payload ramdisk is md1\n", rd1_pa, rd1_size);
	}

	/* "bootcpu=N" among the kernel's arguments: run it on core N (see run_on_other_cpu) */
	uint64_t this_mpidr = READ_SYSREG(MPIDR_EL1) & 0xFFFFFF, kernel_mpidr = this_mpidr;
	/* "cpus=N" or "cpumask=M" there too: the kernel may have more cores than the one it starts on */
	int more_cores = 0, battery_requested = 0, ufs_requested = 0;
	/* "torch=N" (not 0) there too: the phone's flashlight is the kernel's (bsd/dev/redwood_torch.c) */
	int torch_requested = 0;
	/* "cam=5" there too: the phone's wide camera is iOS's (bsd/dev/redwood_camera.c) */
	int camera_requested = 0;
	int touchid_requested = 0;
	/* (the loader stays for run after run: what the last run's arguments said must not hold for this one) */
	gamut_p3 = 0;
	island = 0;
	/* a run that does not ask for writes does not get them, whatever an earlier run of this
	 * loader asked for (the loader stays resident across runs) */
	block_write_requested = 0;
	block_write_part = -1;
	for (const char *c = pi.cmdline; *c; c++) {
		if (starts_with(c, "bootcpu=") && (c == pi.cmdline || c[-1] == ' ') && c[8] >= '0' && c[8] <= '7') {
			kernel_mpidr = plat_mpidr_of(PLAT, c[8] - '0');
		}
		more_cores |= (starts_with(c, "cpus=") || starts_with(c, "cpumask=")) && (c == pi.cmdline || c[-1] == ' ');
		battery_requested |= starts_with(c, "battery=1") && (c == pi.cmdline || c[-1] == ' ') &&
		    (c[9] == 0 || c[9] == ' ');
		torch_requested |= starts_with(c, "torch=") && (c == pi.cmdline || c[-1] == ' ') && c[6] >= '1' && c[6] <= '9';
		camera_requested |= starts_with(c, "cam=5") && (c == pi.cmdline || c[-1] == ' ') && (c[5] == 0 || c[5] == ' ');
		touchid_requested |= starts_with(c, "touchid=1") && (c == pi.cmdline || c[-1] == ' ') && (c[9] == 0 || c[9] == ' ');
		gamut_p3 |= starts_with(c, "gamut=p3") && (c == pi.cmdline || c[-1] == ' ');
		island |= starts_with(c, "island=1") && (c == pi.cmdline || c[-1] == ' ') && (c[8] == 0 || c[8] == ' ');
		if (starts_with(c, "ufs=") && (c == pi.cmdline || c[-1] == ' ') && c[4] >= '1' && c[4] <= '2' &&
		    (c[5] == 0 || c[5] == ' ')) {
			ufs_requested = c[4] - '0';
		}
		block_probe_requested |= starts_with(c, "ufs_probe=1") && (c == pi.cmdline || c[-1] == ' ');
		block_write_requested |= starts_with(c, "ufs_write=1") && (c == pi.cmdline || c[-1] == ' ') &&
		    (c[11] == 0 || c[11] == ' ');
		hc_requested |= starts_with(c, "ufs_native=") && (c == pi.cmdline || c[-1] == ' ') && c[11] >= '1' &&
		    c[11] <= '9';
	}
	if (battery_requested) {
		charger_start();
	}
	if (ufs_requested >= 2) {
		hc_init();              /* (ufs_native=: UEFI's transfer list kept, before anything reads) */
	}
	if (ufs_requested >= 1) {
		ufs_survey();
	}
	if (ufs_requested >= 2) {
		block_start(kernel_mpidr == this_mpidr);
	}
	if (checkpoint == CHECKPOINT_SELFTEST) {
		kernel_mpidr = this_mpidr;
	}

	/* where the kernel will show its signs of life, and how long it has said it will run */
	const struct kernel_live *kernel_live = NULL;
	uint64_t kernel_budget = 0;
	for (uint64_t *q = (uint64_t *)kern_lo; q < (uint64_t *)kern_hi; q++) {
		if (*q == KERNEL_LIVE_MAGIC) {
			kernel_live = (const struct kernel_live *)q;
			break;
		}
	}
	for (const char *c = pi.cmdline; *c; c++) {
		if (starts_with(c, "loader_return_after=")) {
			for (c += 20; *c >= '0' && *c <= '9'; c++) {
				kernel_budget = kernel_budget * 10 + (uint64_t)(*c - '0');
			}
			break;
		}
	}

	/* Give the kernel the address of our return stub and the checkpoint to stop at. */
	for (uint64_t *q = (uint64_t *)kern_lo; q < (uint64_t *)kern_hi; q++) {
		if (*q == 0x545352444C554E58ULL /* "XNULDRST" */) {
			q[1] = kernel_mpidr != this_mpidr ? (uint64_t)(uintptr_t)&other_cpu_return_stub :
			    (uint64_t)(uintptr_t)&loader_return_stub;
			q[2] = checkpoint;
			q[3] = slide;
			print("return slot patched at %x, checkpoint %d\n", (uint64_t)(uintptr_t)q, checkpoint);
			break;
		}
	}

	/* --- boot_args ------------------------------------------------------ */
	struct boot_args *ba = (struct boot_args *)ba_pa;
	memset(ba, 0, sizeof(*ba));
	ba->revision = 2;
	ba->version = 2;
	ba->virtBase = KERNEL_VIRT_BASE;
	ba->physBase = phys_base;
	ba->memSize = win_size;
	ba->memSizeActual = win_size;
	ba->topOfKernelData = top;
	/* any display the firmware has set up: the phone's panel, or QEMU's ramfb */
	if (SCREEN.base != 0) {
		/*
		 * v_display is how iBoot says "graphics mode": the kernel then keeps
		 * its text off the screen. A verbose boot (-v) wants the text.
		 */
		int verbose = 0;
		for (const char *c = pi.cmdline; *c; c++) {
			verbose |= starts_with(c, "-v") && (c[2] == ' ' || c[2] == 0) && (c == pi.cmdline || c[-1] == ' ');
		}
		ba->video.v_baseAddr = SCREEN.base;
		ba->video.v_display = verbose ? 0 : 1;
		ba->video.v_rowBytes = SCREEN.stride;
		ba->video.v_width = SCREEN.width;
		ba->video.v_height = SCREEN.height;
		ba->video.v_depth = 32;
	}
	/*
	 * No "-v" by default: on the phone the verbose console is a text
	 * framebuffer that scrolls 10 MB per line and slows boot to a crawl.
	 * The log comes back through the loader instead. Add -v via the
	 * payload's cmdline entry to watch the boot on screen.
	 */
	/*
	 * serial=3 sends the console to the UART. Where there is none it would
	 * only silence the screen; and a "serial=" in the payload's own
	 * arguments must win, which means ours must not come first.
	 */
	int own_serial = 0;
	for (const char *c = pi.cmdline; *c; c++) {
		own_serial |= starts_with(c, "serial=");
	}
	const char *base_cmdline = (PLAT->uart != 0 && !own_serial) ? "debug=0x8 serial=3 " : "debug=0x8 ";
	uint32_t n = 0;
	for (const char *c = base_cmdline; *c && n < BOOT_LINE_LENGTH - 1; c++) {
		ba->commandLine[n++] = *c;
	}
	/* one processor unless the payload asks for more (of two "cpus=" the kernel takes the first) */
	if (!more_cores) {
		for (const char *c = "cpus=1 "; *c && n < BOOT_LINE_LENGTH - 1; c++) {
			ba->commandLine[n++] = *c;
		}
	}
	/* the root is the first RAM disk unless the payload says otherwise */
	int own_root = 0;
	for (const char *c = pi.cmdline; *c; c++) {
		own_root |= starts_with(c, "rd=") && (c == pi.cmdline || c[-1] == ' ');
	}
	if (rd_size != 0 && !own_root) {
		for (const char *c = "rd=md0 "; *c && n < BOOT_LINE_LENGTH - 1; c++) {
			ba->commandLine[n++] = *c;
		}
	}
	if (wdt_armed) {
		for (const char *c = "hv_wdt_pet=1 "; *c && n < BOOT_LINE_LENGTH - 1; c++) {
			ba->commandLine[n++] = *c;
		}
	}
	if (block_enabled) {
		static const char prefix[] = "ufs_shared_pa=0x";
		static const char hex[] = "0123456789abcdef";
		uint64_t address = (uint64_t)(uintptr_t)&block_shared;

		for (const char *c = prefix; *c && n < BOOT_LINE_LENGTH - 1; c++) {
			ba->commandLine[n++] = *c;
		}
		for (int shift = 60; shift >= 0 && n < BOOT_LINE_LENGTH - 1; shift -= 4) {
			ba->commandLine[n++] = hex[(address >> shift) & 0xf];
		}
		if (n < BOOT_LINE_LENGTH - 1) ba->commandLine[n++] = ' ';
	}
	if (battery_requested) {
		static const char prefix[] = "battery_shared_pa=0x";
		static const char hex[] = "0123456789abcdef";
		uint64_t address = (uint64_t)(uintptr_t)&charger_shared;

		for (const char *c = prefix; *c && n < BOOT_LINE_LENGTH - 1; c++) {
			ba->commandLine[n++] = *c;
		}
		for (int shift = 60; shift >= 0 && n < BOOT_LINE_LENGTH - 1; shift -= 4) {
			ba->commandLine[n++] = hex[(address >> shift) & 0xf];
		}
		if (n < BOOT_LINE_LENGTH - 1) ba->commandLine[n++] = ' ';
	}
	for (const char *c = pi.cmdline; *c && n < BOOT_LINE_LENGTH - 1; c++) {
		ba->commandLine[n++] = *c;
	}

	/* --- device tree ---------------------------------------------------- */
	const struct platform *plat = PLAT;
	struct dt_writer w = { .base = (uint8_t *)dt_pa, .cur = (uint8_t *)dt_pa, .depth = 0 };
	uint64_t cntfrq = timer_freq();
	uint64_t ranges[3] = { 0, plat->io_base, plat->io_size };
	uint64_t reg2[2];
	uint8_t seed[64];

	for (unsigned i = 0; i < sizeof(seed); i++) {
		seed[i] = (uint8_t)(READ_SYSREG(CNTVCT_EL0) >> (i & 7)) ^ (uint8_t)(i * 37);
	}

	dt_begin(&w, "device-tree");
	/*
	 * The identity is the iPhone's (model, compatible, target-type ...): iOS user space looks
	 * its device up by these, and its files -- the dyld shared cache to begin with -- are
	 * those of that phone. XNU itself does not care; the hardware is described further down.
	 */
	dt_identity(&w, identity_root, sizeof(identity_root) / sizeof(identity_root[0]), identity_root_data);
	dt_prop_str(&w, "platform-name", "t8015");
	/* what iBoot takes from the factory configuration of a real phone; fixed size fields */
	dt_prop_padded(&w, "serial-number", "RDW00POCOX5P", 32);
	dt_prop_padded(&w, "mlb-serial-number", "RDW00POCOX5PMLB00", 32);
	dt_prop_padded(&w, "model-number", "MQAC2", 32);
	dt_prop_padded(&w, "region-info", "LL/A", 32);
	dt_prop_padded(&w, "regulatory-model-number", "A1865", 32);
	dt_prop_u32(&w, "#address-cells", 2);
	dt_prop_u32(&w, "#size-cells", 2);

	dt_begin(&w, "chosen");
	dt_prop(&w, "random-seed", seed, sizeof(seed));
	dt_prop_u64(&w, "dram-base", phys_base);
	dt_prop_u64(&w, "dram-size", win_size);
	/* A machine-readable handoff for the opt-in IPC backend. Recheck actual
	 * firmware ownership/cache state after all preceding allocations. */
	report_cellular_memory();
	if (cellular_memory_ready) {
		dt_prop_u64(&w, "cellular-memory-version", 1);
		dt_prop_u64(&w, "cellular-ipc-base", 0xf0000000ULL);
		dt_prop_u64(&w, "cellular-ipc-size", 0x800000ULL);
	}

	reserve_wifi_dma();
	if (wifi_dma_size != 0) {
		dt_prop_u64(&w, "wifi-dma-base", wifi_dma_base);
		dt_prop_u64(&w, "wifi-dma-size", wifi_dma_size);
	}
	reserve_display_pool();
	if (display_pool_size != 0) {
		dt_prop_u64(&w, "display-pool-base", display_pool_base);
		dt_prop_u64(&w, "display-pool-size", display_pool_size);
	}
	dt_prop_u32(&w, "debug-enabled", 1);
	/* what MobileGestalt answers "main screen scale" from: 3 device pixels per point */
	dt_prop_u32(&w, "display-scale", 3);
	dt_prop_u32(&w, "display-rotation", 0);
	/* how the kernel is to ask the firmware for more cores (PSCI) */
	dt_prop_str(&w, "psci-method", plat->psci_hvc ? "hvc" : "smc");
	/* ... and where it may start them from: below 4 GB, with a slot for the real entry (see kernel_cpu_trampoline) */
	dt_prop_u64(&w, "psci-cpu-on-trampoline", (uint64_t)(uintptr_t)&kernel_cpu_trampoline);
	dt_prop_u64(&w, "psci-cpu-on-slot", (uint64_t)(uintptr_t)kernel_cpu_slot);
	/* ... and where a core coming back from a power-down idle enters (see kernel_cpu_resume) */
	dt_prop_u64(&w, "psci-cpu-resume-trampoline", (uint64_t)(uintptr_t)&kernel_cpu_resume);
	dt_prop_u64(&w, "psci-cpu-resume-data", (uint64_t)(uintptr_t)kernel_cpu_resume_data);
	/* the time now, by the firmware's clock (the kernel has no real-time clock driver) */
	uint64_t epoch = firmware_epoch();
	if (epoch != 0) {
		dt_prop_u64(&w, "boot-epoch", epoch);
	}
	/*
	 * More of what iBoot reports about the phone. MobileGestalt builds the device identifier
	 * (UDID) from the serial number, the chip ID and the two addresses; when one is missing it
	 * asks its helper for the factory data, is refused and waits forever (and lsd with it).
	 */
	static const uint8_t wifi_address[6] = { 0x02, 0x52, 0x44, 0x57, 0x00, 0x01 };
	static const uint8_t bluetooth_address[6] = { 0x02, 0x52, 0x44, 0x57, 0x00, 0x02 };
	static const uint8_t manifest_hash[48] = { 0x72, 0x65, 0x64, 0x77, 0x6f, 0x6f, 0x64 };
	static const uint8_t no_behavior[16];
	dt_prop_u64(&w, "unique-chip-id", 0x000052445730001ULL);
	dt_prop_u32(&w, "chip-id", 0x8015);
	dt_prop_u32(&w, "board-id", 0x06);
	dt_prop(&w, "mac-address-wifi0", wifi_address, sizeof(wifi_address));
	dt_prop(&w, "mac-address-bluetooth0", bluetooth_address, sizeof(bluetooth_address));
	dt_prop(&w, "boot-manifest-hash", manifest_hash, sizeof(manifest_hash));
	dt_prop(&w, "software-behavior", no_behavior, sizeof(no_behavior));
	dt_prop_str(&w, "firmware-version", "iBoot-8422.100.650");
	dt_prop_u32(&w, "certificate-production-status", 1);
	dt_prop_u32(&w, "certificate-security-mode", 1);
	dt_prop_u32(&w, "effective-production-status-ap", 1);
	dt_prop_u32(&w, "effective-security-mode-ap", 1);
	dt_prop_u32(&w, "secure-boot", 1);
	dt_prop_u32(&w, "production-cert", 1);
	dt_prop_u32(&w, "development-cert", 0);
	dt_prop_u32(&w, "embedded-panic-log-size", PANIC_LOG_SIZE);
	/* An empty but well-formed NVRAM image: IOKit refuses to start without it. */
	static uint8_t nvram_image[NVRAM_BANK_SIZE];
	build_nvram_image(nvram_image);
	dt_prop_u32(&w, "nvram-bank-size", NVRAM_BANK_SIZE);
	dt_prop_u32(&w, "nvram-bank-count", 1);
	dt_prop_u32(&w, "nvram-current-bank", 0);
	dt_prop(&w, "nvram-proxy-data", nvram_image, sizeof(nvram_image));
	dt_begin(&w, "memory-map");
	/*
	 * "RAMDisk" becomes /dev/md0, the root (the kernel and launchd treat
	 * only md0 as a RAM disk boot); "RAMDisk1" becomes /dev/md1. With both
	 * images present the big resident one is the root and the payload's
	 * small one is the second disk.
	 */
	if (root_on_flash && rd_size != 0 && rd1_size == 0) {
		uint64_t only[2] = { rd_pa, rd_size };

		dt_prop(&w, "RAMDisk1", only, sizeof(only));
	} else if (rd_size != 0 || rd1_size != 0) {
		uint64_t first[2] = { rd1_size != 0 ? rd1_pa : rd_pa, rd1_size != 0 ? rd1_size : rd_size };
		dt_prop(&w, "RAMDisk", first, sizeof(first));
	}
	if (rd_size != 0 && rd1_size != 0) {
		uint64_t second[2] = { rd_pa, rd_size };
		dt_prop(&w, "RAMDisk1", second, sizeof(second));
	}
	dt_end(&w);
	if (more_cores && plat->timeout_scale != 0) {
		/* multiplies every "machine timeout" of the kernel (machine_timeout_init_with_suffix()) */
		dt_begin(&w, "machine-timeouts");
		dt_prop_u32(&w, "global-scale", plat->timeout_scale);
		dt_end(&w);
	}
	dt_end(&w);

	/*
	 * What iBoot's device tree tells the kernel to do unless told otherwise. The compressor: an
	 * embedded kernel left to itself keeps it for the freezer alone, and with nothing being
	 * compressed the memory is gone twice as fast (2 = VM_PAGER_COMPRESSOR_NO_SWAP, as on a phone).
	 */
	dt_begin(&w, "defaults");
	dt_prop_u32(&w, "kern.vm_compressor", 2);
	dt_end(&w);

	/* the product: screen class, artwork scale, name ... as on the real phone */
	dt_begin(&w, "product");
	dt_identity(&w, identity_product, sizeof(identity_product) / sizeof(identity_product[0]), identity_product_data);
	/*
	 * and its haptics: "closed-loop" (MobileGestalt's DeviceSupportsClosedLoopHaptics) makes iOS play
	 * its vibrations and feedbacks as waveforms on the "Actuator" audio device (the audio plug-in
	 * hands them to the kernel's motor driver); without it iOS looks for an IOKit vibration motor
	 * that the iPhone X does not have, and nothing vibrates.
	 */
	dt_begin(&w, "haptics");
	dt_identity(&w, identity_haptics, sizeof(identity_haptics) / sizeof(identity_haptics[0]), identity_haptics_data);
	dt_end(&w);
	/*
	 * and of its camera what is here: with torch=N the flash (MobileGestalt's camera-flash; without
	 * it Control Center has no flashlight); with cam=5 the node itself, for a camera there is (with it
	 * MobileGestalt's video-camera). The rest of the iPhone's camera is not here, nor said to be.
	 */
	if (torch_requested || camera_requested) {
		dt_begin(&w, "camera");
		if (torch_requested) {
			dt_identity_one(&w, identity_camera, sizeof(identity_camera) / sizeof(identity_camera[0]), identity_camera_data, "flash");
		}
		dt_end(&w);
	}
	dt_end(&w);

	/*
	 * With island=1 (the 14 Pro's subtype, see dt_identity), what a 14 Pro's tree says and the iPhone X's does
	 * not: no home button. MobileGestalt's HomeButtonType is /buttons "home-button-type" (2 = none), and
	 * BaseBoard's -[BSPlatform homeButtonType] calls a phone with no answer buttonless only when its subtype
	 * is 2436; at 2556 iOS would take the S20 for a home-button iPhone (no home-bar swipe, "press home").
	 */
	if (island) {
		dt_begin(&w, "buttons");
		dt_prop_u32(&w, "home-button-type", 2);
		dt_end(&w);
	}

	/* IOKit publishes its NVRAM object on this node; without it IOKit asserts. */
	dt_begin(&w, "options");
	dt_end(&w);

	/* Panic log region: RAM outside XNU's window (tail of the payload reservation). */
	uint64_t pram_reg[2] = { PANIC_LOG_BASE, PANIC_LOG_SIZE };
	memset((void *)PANIC_LOG_BASE, 0, PANIC_LOG_SIZE);
	dt_begin(&w, "pram");
	dt_prop(&w, "reg", pram_reg, sizeof(pram_reg));
	dt_end(&w);

	/*
	 * The processors. The first is the one the kernel starts on. With "cpus=N" in its
	 * arguments the kernel takes that one and the next N-1 of this list and starts them
	 * itself (PSCI CPU_ON); "cpumask=M" picks by position, bit 0 being the first. So the
	 * order matters: after the boot core come those whose numbers are nearest to its own
	 * (on the phone, where the kernel boots on the biggest core, 7: the other big ones
	 * 6, 5, 4, then the small ones 3, 2, 1). Only cores that are off are listed: the one
	 * this loader stays on while it watches is not the kernel's, nor any the firmware runs.
	 */
	uint64_t cpu_mpidr[8] = { kernel_mpidr };
	int cpu_count = 1, boot_core = plat_core_of(plat, kernel_mpidr);

	if (more_cores) {
		for (int distance = 1; distance < plat->cores; distance++) {
			for (int side = 0; side < 2; side++) {
				int core = side == 0 ? boot_core + distance : boot_core - distance;
				uint64_t mpidr;
				int64_t state;

				if (core < 0 || core >= plat->cores) {
					continue;
				}
				mpidr = plat_mpidr_of(plat, core);
				if (mpidr == this_mpidr) {
					continue;
				}
				state = psci_call(PSCI_AFFINITY_INFO, mpidr, 0, 0);
				if (state == 1 || state == PSCI_NOT_SUPPORTED) {
					cpu_mpidr[cpu_count++] = mpidr;
				} else if (state >= 0) {
					print("core %x is not off (the firmware says %d): not the kernel's to take\n", mpidr, (uint64_t)state);
				}
			}
		}
		print("cores for the kernel, in the order it takes them:");
		for (int i = 0; i < cpu_count; i++) {
			print(" %x", cpu_mpidr[i]);
		}
		print("\n");
	}

	dt_begin(&w, "cpus");
	for (int i = 0; i < cpu_count; i++) {
		char name[5] = { 'c', 'p', 'u', (char)('0' + i), 0 };

		dt_begin(&w, name);
		dt_prop_str(&w, "device_type", "cpu");
		dt_prop_str(&w, "state", i == 0 ? "running" : "waiting");
		dt_prop_u32(&w, "reg", (uint32_t)cpu_mpidr[i]);
		dt_prop_u32(&w, "cpu-id", (uint32_t)i);
		dt_prop_u64(&w, "timebase-frequency", cntfrq);
		dt_prop_u64(&w, "fixed-frequency", cntfrq);
		dt_prop_u64(&w, "clock-frequency", 1800000000ULL);
		dt_prop_u64(&w, "bus-frequency", 100000000ULL);
		dt_prop_u64(&w, "memory-frequency", 100000000ULL);
		dt_prop_u64(&w, "peripheral-frequency", 24000000ULL);
		if (plat->small_cores & (1u << (plat_core_of(plat, cpu_mpidr[i]) & 31))) {
			dt_prop_u32(&w, "small-core", 1);
		}
		dt_end(&w);
	}
	dt_end(&w);

	dt_begin(&w, "arm-io");
	dt_prop_str(&w, "device_type", plat->name);
	dt_prop(&w, "ranges", ranges, sizeof(ranges));
	dt_prop_u32(&w, "#address-cells", 2);
	dt_prop_u32(&w, "#size-cells", 2);
	/*
	 * With cam=5, where MobileGestalt looks for the cameras (IODeviceTree:/arm-io/isp, its still-camera and
	 * rear-facing-camera, by which SpringBoard shows Camera.app): the iPhone's ISP node, of its
	 * properties only the camera at the back. Neither a compatible nor a device_type: no driver matches it.
	 */
	if (camera_requested) {
		dt_begin(&w, "isp");
		dt_prop_u32(&w, "camera-rear", 1);
		dt_end(&w);
	}
	/*
	 * With touchid=1, Touch ID where an iPhone 8 has it (its tree: /arm-io/spi3/mesa, "biosensor,mesa", mesaType 1),
	 * so MobileGestalt and Settings take the S20 for a Touch ID iPhone. Only the identity: no reg or interrupts, as
	 * no Apple driver runs here; the kernel's RedwoodMesa plays the Secure Enclave's Touch ID part.
	 */
	if (touchid_requested) {
		dt_begin(&w, "mesa");
		dt_prop_str(&w, "device_type", "mesa");
		dt_prop_str(&w, "compatible", "biosensor,mesa");
		dt_prop_u32(&w, "mesaType", 1);
		dt_end(&w);
	}
	dt_begin(&w, "gic");
	dt_prop_str(&w, "device_type", "interrupt-controller");
	if (plat->gic_v2) {
		/* GICv2: distributor + CPU interface; osfmk/arm64/gic_v2_irq.c reads this. */
		dt_prop_str(&w, "compatible", "arm,cortex-a15-gic");
		uint64_t gic_reg[4] = { plat->gic_dist - plat->io_base, 0x1000, plat->gic_redist - plat->io_base, 0x1000 };
		dt_prop(&w, "reg", gic_reg, sizeof(gic_reg));
	} else {
		/* GICv3: distributor + redistributor; osfmk/arm64/gic_v3_irq.c reads this. */
		dt_prop_str(&w, "compatible", "arm,gic-v3");
		uint64_t gic_reg[4] = { plat->gic_dist - plat->io_base, 0x10000, plat->gic_redist - plat->io_base, 0x100000 };
		dt_prop(&w, "reg", gic_reg, sizeof(gic_reg));
	}
	dt_end(&w);
	if (plat->uart != 0) {
		dt_begin(&w, "uart0");
		dt_prop_str(&w, "device_type", "uart");
		dt_prop_str(&w, "compatible", "arm,pl011");
		reg2[0] = plat->uart - plat->io_base;
		reg2[1] = 0x1000;
		dt_prop(&w, "reg", reg2, sizeof(reg2));
		dt_prop_u32(&w, "boot-console", 1);
		dt_end(&w);
	}
	dt_end(&w);

	dt_end(&w);

	uint32_t dt_len = (uint32_t)(w.cur - w.base);
	ba->deviceTreeP = dt_pa - slide;
	ba->deviceTreeLength = dt_len;
	print("device tree %x len %x, boot_args %x, top %x\n", dt_pa, (uint64_t)dt_len, ba_pa, top);
	print("cmdline: %s\n", ba->commandLine);
	if (kernel_mpidr != this_mpidr) {
		print("the kernel will run on core %x (this is core %x)\n", kernel_mpidr, this_mpidr);
	}
	print("jumping to %x\n", entry_va + slide);
	flush_log();
	/* From here on the kernel feeds the watchdog (boot-arg hv_wdt_pet, see loader_return.c in XNU). */
	wdt_set_ms(60000);

	/* --- hand off (UEFI is left resident, see loader_save_ctx) ------------ */
	if (loader_save_ctx() == 0) {
		/*
		 * On real hardware (under the Qualcomm hypervisor) dirty data-cache
		 * lines do not survive the MMU-off round trip. UEFI stays resident,
		 * so everything it owns must be written back to RAM first: clean
		 * every allocated region in the memory map, then this stack again.
		 */
		map_size = sizeof(map_buf);
		if (!EFI_ERROR(BS->get_memory_map(&map_size, map_buf, &map_key, &desc_size, &desc_ver))) {
			for (uint64_t off = 0; off < map_size; off += desc_size) {
				efi_memory_descriptor *d = (efi_memory_descriptor *)(map_buf + off);
				/* loader/boot-services/runtime code and data, ACPI tables; XNU's own window is handled below */
				if (d->phys_start >= win_base && d->phys_start < win_base + win_size) {
					continue;
				}
				if ((d->type >= 1 && d->type <= 6) || d->type == 9 || d->type == 10) {
					clean_dcache(d->phys_start, d->phys_start + (d->pages << 12));
				}
			}
		}
		uint64_t sp_now = (uint64_t)(uintptr_t)__builtin_frame_address(0);
		clean_dcache(sp_now - 0x2000, sp_now + 0x2000);
		flush_dcache(phys_base, top);
		/* the kernel writes the slot uncached and new cores read it with their caches off (the slot
		 * is in .text, which the firmware maps read-only: not written from here, it is 0 in the image) */
		flush_dcache((uint64_t)(uintptr_t)&kernel_cpu_trampoline,
		    (uint64_t)(uintptr_t)kernel_cpu_resume_data + sizeof(uint64_t) * 17);
		console_suspect = PLAT == &platform_y2s;
		if (checkpoint == CHECKPOINT_SELFTEST) {
			/* Do not enter the kernel at all: exercise only the return path. */
			jump_to_kernel((uint64_t)(uintptr_t)&loader_return_stub, 0);
		}
		if (kernel_mpidr != this_mpidr) {
			int64_t status = run_on_other_cpu(kernel_mpidr, entry_va + slide, ba_pa, kernel_live, slide, kernel_budget);

			wdt_set_ms(WDT_BITE_MS);
			if (status != 0) {
				/* a kernel that hung may have left its own transfer list in the controller */
				hc_take_back(status == OTHER_CPU_HUNG ? "the kernel hung" : "the kernel did not start");
			}
			if (status == OTHER_CPU_HUNG) {
				/* the kernel still has that core; what it logged can be had all the same */
				print("THE KERNEL HUNG on core %x: no sign of life for %d seconds; its log follows\n", kernel_mpidr,
				    (uint64_t)SILENCE_SECONDS);
				set_result("hung", loader_ctx[CTX_LOG_LEN], 0);
				write_logs((const char *)loader_ctx[CTX_LOG_PA], loader_ctx[CTX_LOG_LEN]);
				return 1;
			} else if (status != 0) {
				/* the kernel was told it would be on that core (return path, device tree): no second try */
				if (status == OTHER_CPU_NOT_STARTED) {
					print("core %x was started but never arrived\n", kernel_mpidr);
				} else {
					print("core %x did not start: PSCI error %d\n", kernel_mpidr, (uint64_t)-status);
				}
				set_result("no-such-core", (uint64_t)(status < 0 ? -status : status), 0);
				write_logs(NULL, 0);
				return 1;
			}
		} else {
			jump_to_kernel(entry_va + slide, ba_pa);
		}
	}

	/* --- the kernel came back ------------------------------------------- */
	wdt_set_ms(WDT_BITE_MS);
	hc_take_back("the kernel came back");   /* before anything here touches the storage */
	if (hc_known) {
		flush_dcache((uint64_t)(uintptr_t)&block_shared.kernel_native,
		    (uint64_t)(uintptr_t)&block_shared.kernel_native + sizeof(block_shared.kernel_native));
		print("ufs: the kernel put its own list in %d times and took it out %d times\n",
		    (uint64_t)block_shared.kernel_native.switched_in, (uint64_t)block_shared.kernel_native.switched_out);
	}
	if (block_write_requested) {
		print("ufs: %d writes into darwin, %d bytes, read back the same; %d refused\n", block_writes,
		    block_write_bytes, block_writes_refused);
	}
	/* ... and its other cores should all be off again: the next kernel cannot start one that is not */
	for (int i = 1; i < cpu_count; i++) {
		int64_t state = psci_call(PSCI_AFFINITY_INFO, cpu_mpidr[i], 0, 0);

		if (state != 1 && state != PSCI_NOT_SUPPORTED) {
			print("THE KERNEL LEFT CORE %x RUNNING (the firmware says %d): restart before running another\n", cpu_mpidr[i],
			    (uint64_t)state);
		}
	}
	recording_data = NULL;
	recording_len = 0;
	if (kernel_live != NULL && kernel_live->recording != 0 && checkpoint != CHECKPOINT_SELFTEST) {
		const uint64_t *recording = (const uint64_t *)(kernel_live->recording + slide);

		if (recording[0] != 0 && recording[1] > 24 && recording[1] <= (1ULL << 30)) {
			recording_data = (const char *)recording[0];
			recording_len = recording[1];
			print("the kernel left a screen recording: %d bytes at %x\n", recording_len, recording[0]);
		}
	}
	sound_data = NULL;
	sound_len = 0;
	if (kernel_live != NULL && kernel_live->magic2 == KERNEL_LIVE_MAGIC2 && kernel_live->sound != 0 &&
	    checkpoint != CHECKPOINT_SELFTEST) {
		const uint64_t *sound = (const uint64_t *)(kernel_live->sound + slide);

		if (sound[0] != 0 && sound[1] > 24 && sound[1] <= (1ULL << 30)) {
			sound_data = (const char *)sound[0];
			sound_len = sound[1];
			print("the kernel left a sound recording: %d bytes at %x\n", sound_len, sound[0]);
		}
	}
    if (kernel_live != NULL && kernel_live->magic3 == 0x334556494c554e58ULL &&
        SCREEN.width == 1440 && SCREEN.height == 3200 &&
        ((kernel_live->menu_width == 1080 && kernel_live->menu_height == 2400) ||
         (kernel_live->menu_width == 720 && kernel_live->menu_height == 1600))) {
        menu_output_width = (uint32_t)kernel_live->menu_width;
        menu_output_height = (uint32_t)kernel_live->menu_height;
    } else {
        menu_output_width = menu_output_height = 0;
    }
	const char *klog = (const char *)loader_ctx[CTX_LOG_PA];
	uint64_t klog_len = loader_ctx[CTX_LOG_LEN];
	uint64_t reason = loader_ctx[CTX_REASON];

	if (reason == 3) {
		y2s_want_log_access = 1;
	} else if (reason == 4) {
		y2s_want_download = 1;      /* the kernel asked for Download Mode */
	}
	if (checkpoint == CHECKPOINT_SELFTEST) {
		print("return-path self test passed\n");
		set_result("selftest-ok", 0, 0);
		klog = NULL;
		klog_len = 0;
	} else {
		print("kernel returned: reason %d, log at %x, %d bytes\n", reason,
		    (uint64_t)(uintptr_t)klog, klog_len);
		if (reason == 2) {
			print("STOPPED AT CHECKPOINT %d\n", loader_ctx[CTX_ESR]);
			set_result("checkpoint", loader_ctx[CTX_ESR], 0);
		} else if (reason == 1) {
			print("EARLY KERNEL EXCEPTION: esr=%x elr=%x far=%x lr=%x sp=%x\n", loader_ctx[CTX_ESR], loader_ctx[CTX_ELR],
			    loader_ctx[CTX_FAR], loader_ctx[CTX_LR], loader_ctx[CTX_KSP]);
			print("  (pa-va slide %x: subtract it from physical addresses to symbolicate)\n", slide);
			set_result("early-exception", 0, loader_ctx[CTX_ELR]);
			klog_len = klog_len > (1u << 20) ? 0 : klog_len;
		} else {
			set_result("returned", klog_len, 0);
		}
	}
	write_logs(klog, klog_len);
	return 1;
}

/* ------------------------------------------------------------------------ */
/* USB command channel: a small fastboot-protocol server.                    */
/*                                                                          */
/* Host side (stock `fastboot` tool):                                        */
/*   fastboot stage payload.bin    download a new "XNUP" payload            */
/*   fastboot oem run [N]          run its kernel (optionally stop at N)    */
/*   fastboot getvar result        blocks until the kernel is back          */
/*   fastboot get_staged log.txt   fetch loader + kernel log                */
/*   fastboot oem hang             stop petting the watchdog (test)         */
/*   fastboot reboot               cold reset                               */

#define USB_EP_OUT   0x01
#define USB_EP_IN    0x81
#define USB_CHUNK    (1024 * 1024)

static usb_device_protocol *USB;
static int download_to_ramdisk;     /* set by "oem ramdisk" for the one download that follows */
static uint8_t *usb_rx_buf, *usb_tx_buf;
static int usb_connected;

static const uint8_t usb_dev_desc[18] = { 18, 1, 0x10, 0x02, 0, 0, 0, 64, 0xd1, 0x18, 0x0d, 0xd0, 0x00, 0x01, 1, 2, 3, 1 };
static const uint8_t usb_ss_dev_desc[18] = { 18, 1, 0x00, 0x03, 0, 0, 0, 9, 0xd1, 0x18, 0x0d, 0xd0, 0x00, 0x01, 1, 2, 3, 1 };
static const uint8_t usb_cfg_desc[32] = {
	9, 2, 32, 0, 1, 1, 0, 0x80, 0x50,
	9, 4, 0, 0, 2, 0xff, 0x42, 0x03, 4,   /* fastboot interface */
	7, 5, USB_EP_IN, 2, 0x00, 0x02, 0,
	7, 5, USB_EP_OUT, 2, 0x00, 0x02, 0,
};
static const uint8_t usb_ss_cfg_desc[44] = {
	9, 2, 44, 0, 1, 1, 0, 0x80, 0x10,
	9, 4, 0, 0, 2, 0xff, 0x42, 0x03, 4,
	7, 5, USB_EP_IN, 2, 0x00, 0x04, 0,   6, 0x30, 4, 0, 0, 0,
	7, 5, USB_EP_OUT, 2, 0x00, 0x04, 0,  6, 0x30, 4, 0, 0, 0,
};
static const uint8_t usb_qualifier[10] = { 10, 6, 0x00, 0x02, 0xff, 0xff, 0xff, 64, 1, 0 };
static const uint8_t usb_bos[42] = {
	5, 0x0f, 42, 0, 3,
	7, 0x10, 0x02, 0x06, 0, 0, 0,
	10, 0x10, 0x03, 0x00, 0x0e, 0x00, 0x01, 0x07, 0x65, 0x00,
	20, 0x10, 0x0a, 0x00, 0x01, 0, 0, 0, 0x00, 0x11, 0, 0, 0x30, 0x40, 0x0a, 0x00, 0xb0, 0x40, 0x0a, 0x00,
};
static const uint8_t usb_str0[4] = { 4, 3, 0x09, 0x04 };
static const uint8_t usb_str_mfr[8] = { 8, 3, 'X', 0, 'N', 0, 'U', 0 };
static const uint8_t usb_str_prod[20] = { 20, 3, 'x', 0, 'n', 0, 'u', 0, 'l', 0, 'o', 0, 'a', 0, 'd', 0, 'e', 0, 'r', 0 };
static const uint8_t usb_str_serial[20] = { 20, 3, 'x', 0, 'n', 0, 'u', 0, 'l', 0, 'o', 0, 'a', 0, 'd', 0, 'e', 0, 'r', 0 };
static const uint8_t usb_str_iface[18] = { 18, 3, 'f', 0, 'a', 0, 's', 0, 't', 0, 'b', 0, 'o', 0, 'o', 0, 't', 0 };

static void EFIAPI
usb_dummy_notify(void *event, void *ctx)
{
}

static int
usb_start(void)
{
	static const efi_guid usb_guid = QCOM_USB_DEVICE_GUID;
	static const efi_guid init_guid = QCOM_USB_INIT_EVENT_GUID;
	static const void *cfg = usb_cfg_desc, *ss_cfg = usb_ss_cfg_desc;
	static const void *strings[5] = { usb_str0, usb_str_mfr, usb_str_prod, usb_str_serial, usb_str_iface };
	static usb_device_descriptor_set set;
	void *event;

	/* Ask the platform to bring the controller up in peripheral mode (as the stock fastboot does). */
	if (!EFI_ERROR(BS->create_event_ex(0x200 /* EVT_NOTIFY_SIGNAL */, 8 /* TPL_CALLBACK */, usb_dummy_notify, NULL,
	    &init_guid, &event))) {
		BS->signal_event(event);
		BS->close_event(event);
	}
	if (EFI_ERROR(BS->locate_protocol(&usb_guid, NULL, (void **)&USB))) {
		print("usb: device protocol not found\n");
		return 0;
	}
	set.device_descriptor = usb_dev_desc;
	set.descriptors = &cfg;
	set.ss_device_descriptor = usb_ss_dev_desc;
	set.ss_descriptors = &ss_cfg;
	set.device_qualifier = usb_qualifier;
	set.bos = usb_bos;
	set.string_count = 5;
	set.strings = strings;
	efi_status s = USB->start_ex(&set);
	if (EFI_ERROR(s)) {
		print("usb: start failed %x\n", s);
		return 0;
	}
	if (EFI_ERROR(USB->allocate_transfer_buffer(USB_CHUNK, (void **)&usb_rx_buf)) ||
	    EFI_ERROR(USB->allocate_transfer_buffer(USB_CHUNK, (void **)&usb_tx_buf))) {
		print("usb: no transfer buffers\n");
		return 0;
	}
	print("usb: started\n");
	return 1;
}

/*
 * Pump controller events until a transfer on `endpoint` finishes.
 * Returns the byte count, or -1 if the transfer was cancelled or the cable
 * state changed. Keeps the watchdog fed while idle.
 */
static int64_t
usb_wait(uint8_t endpoint)
{
	/* static: a long transfer is many short waits, and the watchdog must be fed across them */
	static uint64_t last_pet;
	uint64_t freq = timer_freq(), began = READ_SYSREG(CNTVCT_EL0);

	for (;;) {
		uint32_t msg = UsbDeviceEventNoEvent;
		uint64_t size = 0;
		usb_device_event_data data;

		USB->handle_event(&msg, &size, &data);
		if (msg == UsbDeviceEventDeviceStateChange) {
			usb_connected = (data.device_state == UsbDeviceStateConnected);
			return -1;
		}
		/* with a kernel running, a host that went away must not keep this core from watching it */
		if (running_live != NULL && READ_SYSREG(CNTVCT_EL0) - began > 3 * freq) {
			return -1;
		}
		if (msg == UsbDeviceEventTransferNotification && data.transfer.endpoint_index == endpoint) {
			return data.transfer.status == UsbDeviceTransferStatusCompleteOK ? (int64_t)data.transfer.bytes_completed : -1;
		}
		if (READ_SYSREG(CNTVCT_EL0) - last_pet > freq) {
			last_pet = READ_SYSREG(CNTVCT_EL0);
			wdt_pet();
		}
	}
}

static int usb_rx_queued;       /* usb_poll_command() has left the receive for the next command with the controller */
static struct efi_usbfn_io *USBFN;      /* y2s: the transport (see the boot menu's section) */
static int64_t ufn_rx(uint8_t *buf, uint64_t len);
static int64_t ufn_tx(const uint8_t *data, uint64_t len);
static uint8_t *stage_buf;
static uint64_t stage_size, stage_len;
static int stage_last_log(void);
static const char *stage_write(const char *part, uint64_t off);
static int usb_boot_asked;
static void reboot_download(void);
static void reboot_recovery(void);

static int64_t
usb_rx(uint8_t *buf, uint64_t len)
{
	if (USBFN != NULL) {
		return ufn_rx(buf, len);
	}
	if (usb_rx_queued) {
		usb_rx_queued = 0;      /* (every command is received the same way, into the same buffer) */
	} else if (EFI_ERROR(USB->send(USB_EP_OUT, len, buf))) {
		return -1;
	}
	return usb_wait(USB_EP_OUT);
}

static int64_t
usb_tx(const uint8_t *data, uint64_t len)
{
	if (data != usb_tx_buf) {
		memcpy(usb_tx_buf, data, len);
	}
	if (USBFN != NULL) {
		return ufn_tx(usb_tx_buf, len);
	}
	if (EFI_ERROR(USB->send(USB_EP_IN, len, usb_tx_buf))) {
		return -1;
	}
	return usb_wait(USB_EP_IN);
}

static void
fb_reply(const char *code, const char *text)
{
	uint32_t n = 0;
	for (; *code; code++) {
		usb_tx_buf[n++] = (uint8_t)*code;
	}
	for (; *text && n < 63; text++) {
		usb_tx_buf[n++] = (uint8_t)*text;
	}
	usb_tx(usb_tx_buf, n);
}

static void
fb_reply_data(uint64_t len)
{
	char msg[13] = "DATA00000000";
	for (int i = 0; i < 8; i++) {
		msg[4 + i] = "0123456789abcdef"[(len >> (28 - 4 * i)) & 0xf];
	}
	usb_tx((const uint8_t *)msg, 12);
}

static int
starts_with(const char *s, const char *prefix)
{
	while (*prefix) {
		if (*s++ != *prefix++) {
			return 0;
		}
	}
	return 1;
}

/*
 * One command from the host. While a kernel runs on another core (running_live) only what does
 * not disturb it: questions, the screen, the log so far, and "oem stop".
 */
static void
fb_command(char *cmd)
{
	int busy = running_live != NULL;

	{
		if (busy && (starts_with(cmd, "oem ramdisk") || starts_with(cmd, "download:") || starts_with(cmd, "oem run") ||
		    starts_with(cmd, "oem selftest") || starts_with(cmd, "oem hang") || starts_with(cmd, "oem recording") ||
		    starts_with(cmd, "oem sound") || starts_with(cmd, "oem clearscreen") || starts_with(cmd, "oem memmap"))) {
			fb_reply("FAIL", "a kernel is running (oem stop ends it)");
		} else if (starts_with(cmd, "oem stop")) {
			/* the kernel looks at this once a second and comes back as it does when its time is up */
			if (busy) {
				((struct kernel_live *)(uintptr_t)running_live)->stop = 1;
			}
			fb_reply("OKAY", "");
		} else if (starts_with(cmd, "getvar:")) {
			const char *var = cmd + 7;
			if (str_eq(var, "product")) {
				fb_reply("OKAY", "xnuloader");
			} else if (str_eq(var, "version")) {
				fb_reply("OKAY", "0.5");
			} else if (str_eq(var, "max-download-size")) {
				fb_reply("OKAY", download_to_ramdisk ? "0xffffffff" : stage_buf != NULL ? "0x05f00000" : "0x03f00000");
			} else if (str_eq(var, "result")) {
				fb_reply("OKAY", last_result);
			} else if (str_eq(var, "screen") && SCREEN.base != 0) {
				/* width height bytes-per-row */
				uint32_t mark = log_len;
				print("%d %d %d", (uint64_t)SCREEN.width, (uint64_t)SCREEN.height, (uint64_t)SCREEN.stride);
				log_buf[log_len] = 0;
				fb_reply("OKAY", log_buf + mark);
				log_len = mark;
			} else {
				fb_reply("OKAY", "");
			}
		} else if (starts_with(cmd, "oem ramdisk")) {
			/* "oem ramdisk": the next download is a root image for the resident area; "oem ramdisk none" drops it */
			struct payload_info pi;

			if (starts_with(cmd, "oem ramdisk none")) {
				resident_rd_size = 0;
				fb_reply("OKAY", "");
				return;
			}
			parse_payload(&pi, 0);
			log_len = 0;
			if (!choose_window(&pi) || win_size <= RESIDENT_RD_OFFSET + RESIDENT_RD_RESERVE) {
				fb_reply("FAIL", "no memory window");
				return;
			}
			download_to_ramdisk = 1;
			fb_reply("OKAY", "");
		} else if (starts_with(cmd, "download:")) {
			uint64_t total = parse_number(cmd + 9, 16), got = 0;
			uint64_t limit = stage_buf != NULL ? stage_size : DOWNLOAD_MAX_SIZE;
			uint8_t *dest = stage_buf != NULL ? stage_buf : (uint8_t *)(uintptr_t)download_base;
			int to_ramdisk = download_to_ramdisk;

			download_to_ramdisk = 0;
			if (to_ramdisk) {
				limit = win_size - RESIDENT_RD_OFFSET - RESIDENT_RD_RESERVE;
				dest = (uint8_t *)(win_base + RESIDENT_RD_OFFSET);
				resident_rd_size = 0;
			}
			if (total == 0 || total > limit) {
				fb_reply("FAIL", "bad size");
				return;
			}
			fb_reply_data(total);
			while (got < total) {
				uint64_t want = total - got > USB_CHUNK ? USB_CHUNK : total - got;
				int64_t r = usb_rx(usb_rx_buf, want);
				if (r <= 0) {
					break;
				}
				memcpy(dest + got, usb_rx_buf, (size_t)r);
				got += (uint64_t)r;
			}
			if (got == total) {
				if (to_ramdisk) {
					resident_rd_size = total;
				}
				if (stage_buf != NULL && !to_ramdisk) {
					stage_len = total;
					/* (a write is rounded up to the block: what follows is zeroes, not the last download's) */
					memset(stage_buf + total, 0, (size_t)((total + 4095) / 4096 * 4096 - total));
				}
				fb_reply("OKAY", "");
			} else {
				fb_reply("FAIL", "short download");
			}
		} else if (starts_with(cmd, "upload")) {
			uint64_t sent = 0;
			fb_reply_data(staged_len);
			while (sent < staged_len) {
				uint64_t chunk = staged_len - sent > USB_CHUNK ? USB_CHUNK : staged_len - sent;
				if (usb_tx((const uint8_t *)staged_data + sent, chunk) < 0) {
					break;
				}
				sent += chunk;
			}
			fb_reply("OKAY", "");
		} else if (starts_with(cmd, "oem screenshot")) {
			/* stage the raw framebuffer; "getvar screen" gives its geometry */
			if (SCREEN.base == 0) {
				fb_reply("FAIL", "no display");
				return;
			}
			staged_data = (const char *)(uintptr_t)SCREEN.base;
			staged_len = (uint64_t)SCREEN.stride * SCREEN.height;
			fb_reply("OKAY", "");
		} else if (starts_with(cmd, "oem recording")) {
			/* stage what the kernel's display driver recorded (boot-arg rec_secs) */
			if (recording_data == NULL) {
				fb_reply("FAIL", "no recording");
				return;
			}
			staged_data = recording_data;
			staged_len = recording_len;
			fb_reply("OKAY", "");
		} else if (starts_with(cmd, "oem sound")) {
			/* stage what the kernel recorded of the sound (boot-arg rec_sound) */
			if (sound_data == NULL) {
				fb_reply("FAIL", "no sound recording");
				return;
			}
			staged_data = sound_data;
			staged_len = sound_len;
			fb_reply("OKAY", "");
		} else if (starts_with(cmd, "oem clearscreen")) {
			if (SCREEN.base != 0) {
				memset((void *)(uintptr_t)SCREEN.base, 0, (size_t)SCREEN.stride * SCREEN.height);
			}
			fb_reply("OKAY", "");
		} else if (starts_with(cmd, "oem run") || starts_with(cmd, "oem selftest")) {
			int selftest = starts_with(cmd, "oem selftest");
			const char *arg = cmd + 7;
			while (*arg == ' ') {
				arg++;
			}
			int have_cp = selftest || (*arg >= '0' && *arg <= '9');
			uint64_t cp = selftest ? CHECKPOINT_SELFTEST : parse_number(arg, 10);

			set_result("running", cp, 0);
			fb_reply("OKAY", "");
			log_len = 0;
			print("== XNULOADER run (usb) ==\n");
			if (!run_kernel(have_cp, cp)) {
				set_result("not-started", 0, 0);
				compose_logs(NULL, 0);
			}
		} else if (starts_with(cmd, "oem hang")) {
			fb_reply("OKAY", "watchdog test: not petting any more");
			for (;;) {
			}
		} else if (starts_with(cmd, "oem memmap")) {
			/* the firmware's memory map into the log (then "oem log" and fetch it) */
			print_memory_map();
			fb_reply("OKAY", "");
		} else if (starts_with(cmd, "oem read ")) {
			/* "oem read PART HEXOFFSET HEXLEN": a piece of a partition into the stage; then "upload" (diagnostics) */
			const char *a = cmd + 9;
			char part[40];
			int n = 0;
			uint64_t off, len;
			uint8_t *buf = stage_buf != NULL ? stage_buf : (uint8_t *)(uintptr_t)download_base;
			uint64_t limit = stage_buf != NULL ? stage_size : DOWNLOAD_MAX_SIZE;

			while (*a == ' ') {
				a++;
			}
			while (*a && *a != ' ' && n < (int)sizeof(part) - 1) {
				part[n++] = *a++;
			}
			part[n] = 0;
			while (*a == ' ') {
				a++;
			}
			off = parse_number(a, 16);      /* (lower-case hex) */
			while (*a && *a != ' ') {
				a++;
			}
			while (*a == ' ') {
				a++;
			}
			len = parse_number(a, 16);
			if (len == 0 || len > limit || !read_partition_into(part, off, buf, len)) {
				fb_reply("FAIL", "oem read PART HEXOFFSET HEXLEN: no such partition, a bad offset, or too long");
			} else {
				staged_data = (const char *)buf;
				staged_len = len;
				fb_reply("OKAY", "");
			}
		} else if (starts_with(cmd, "oem lastlog")) {
			/* the last run's log as it was saved in the partition; then "upload" (fastboot get_staged) */
			fb_reply(stage_last_log() ? "OKAY" : "FAIL", "");
		} else if (starts_with(cmd, "oem write ")) {
			/* "oem write PART HEXOFFSET": the last download into the partition, read back */
			char part[24];
			const char *a = cmd + 10, *why;
			unsigned n = 0;

			while (*a == ' ') {
				a++;
			}
			while (*a && *a != ' ' && n < sizeof(part) - 1) {
				part[n++] = *a++;
			}
			part[n] = 0;
			while (*a == ' ') {
				a++;
			}
			why = stage_write(part, parse_number(a, 16));
			if (why == NULL) {
				fb_reply("INFO", "written and read back");
				fb_reply("OKAY", "");
			} else {
				fb_reply("FAIL", why);
			}
#ifdef CELLULAR_CP_BOOT_PREFLIGHT
		} else if (starts_with(cmd, "oem cp-stop")) {
			int safe=cellular_cp_retry_stop();
			fb_reply(safe?"OKAY":"FAIL",safe?"CP reset/power held low":"CP cleanup not confirmed; memory remains reserved");
#endif
		} else if (starts_with(cmd, "oem boot")) {
#ifdef CELLULAR_CP_BOOT_PREFLIGHT
			if (!cellular_cp_boot_can_reset()) {fb_reply("FAIL","CP stop not confirmed; use oem cp-stop");return;}
#endif
			fb_reply("OKAY", "booting iOS");
			usb_boot_asked = 1;
		} else if (starts_with(cmd, "oem recovery-mode")) {
#ifdef CELLULAR_CP_BOOT_PREFLIGHT
			if (!cellular_cp_boot_can_reset()) {fb_reply("FAIL","CP stop not confirmed; use oem cp-stop");return;}
#endif
			fb_reply("OKAY", "");
			BS->stall(200000);
			reboot_recovery();
		} else if (starts_with(cmd, "oem download-mode")) {
#ifdef CELLULAR_CP_BOOT_PREFLIGHT
			if (!cellular_cp_boot_can_reset()) {fb_reply("FAIL","CP stop not confirmed; use oem cp-stop");return;}
#endif
			fb_reply("OKAY", "");
			BS->stall(200000);
			reboot_download();
		} else if (starts_with(cmd, "oem log")) {
			if (busy) {
				compose_live_log(running_live, running_slide);
			} else {
				compose_logs(NULL, 0);
			}
			fb_reply("OKAY", "");
		} else if (starts_with(cmd, "reboot") || starts_with(cmd, "oem reboot")) {
#ifdef CELLULAR_CP_BOOT_PREFLIGHT
			if (!cellular_cp_boot_can_reset()) {fb_reply("FAIL","CP stop not confirmed; use oem cp-stop");return;}
#endif
			fb_reply("OKAY", "");
			BS->stall(200000);
			ST->rt->reset_system(0 /* EfiResetCold */, 0, 0, NULL);
		} else {
			fb_reply("FAIL", "unknown command");
		}
	}
}

/*
 * A kernel is running on another core and this one watches it: see whether the host has said
 * something meanwhile, without waiting for it to. This is how a run can be asked to end, and
 * the screen and the log be had while it lasts.
 */
static void
usb_poll_command(void)
{
	uint32_t msg = UsbDeviceEventNoEvent;
	uint64_t size = 0, n;
	usb_device_event_data data;
	char cmd[128];

	if (USB == NULL) {
		return;         /* this run was not started over USB */
	}
	if (!usb_rx_queued && usb_connected && !EFI_ERROR(USB->send(USB_EP_OUT, 511, usb_rx_buf))) {
		usb_rx_queued = 1;
	}
	USB->handle_event(&msg, &size, &data);
	if (msg == UsbDeviceEventDeviceStateChange) {
		usb_connected = (data.device_state == UsbDeviceStateConnected);
		usb_rx_queued = 0;
		return;
	}
	if (msg != UsbDeviceEventTransferNotification || data.transfer.endpoint_index != USB_EP_OUT) {
		return;
	}
	usb_rx_queued = 0;
	if (data.transfer.status != UsbDeviceTransferStatusCompleteOK || data.transfer.bytes_completed == 0) {
		return;
	}
	n = data.transfer.bytes_completed < sizeof(cmd) - 1 ? data.transfer.bytes_completed : sizeof(cmd) - 1;
	memcpy(cmd, usb_rx_buf, (size_t)n);
	cmd[n] = 0;
	fb_command(cmd);
}

static void
usb_command_loop(void)
{
	screen_quiet = 1;
	for (;;) {
		char cmd[128];
		int64_t n;

		if (!usb_connected) {
			usb_wait(0xff); /* returns on the next cable state change */
			continue;
		}
		n = usb_rx(usb_rx_buf, 511);
		if (n <= 0) {
			continue;
		}
		if (n > (int64_t)sizeof(cmd) - 1) {
			n = sizeof(cmd) - 1;
		}
		memcpy(cmd, usb_rx_buf, (size_t)n);
		cmd[n] = 0;
		fb_command(cmd);
	}
}

/* ------------------------------------------------------------------------ */
/* y2s: the boot menu, USB through EFI_USBFN_IO_PROTOCOL, Download Mode       */
/* (PLANS/09-standalone.md)                                                  */

#include "menu_font.h"

#define STAGE_SIZE      (96ULL << 20)   /* stage_buf: where downloads land on y2s (a whole recovery image) */

/* the panel, through the firmware's framebuffer */
static uint32_t
menu_rgb(uint32_t rgb)
{
	/* GOP pixel format 0 is RGBX in memory (R first), 1 BGRX */
	if (GOP != NULL && GOP->mode->info->pixel_format == 0) {
		return ((rgb & 0xff) << 16) | (rgb & 0xff00) | ((rgb >> 16) & 0xff);
	}
	return rgb;
}

static void
menu_fill(int x, int y, int w, int h, uint32_t rgb, int radius)
{
	uint32_t c = menu_rgb(rgb);

	for (int j = 0; j < h; j++) {
		int py = y + j;

		if (py < 0 || py >= (int)SCREEN.height) {
			continue;
		}
		uint32_t *row = (uint32_t *)(uintptr_t)(SCREEN.base + (uint64_t)(menu_output_height ? (uint32_t)py * menu_output_height / SCREEN.height : (uint32_t)py) * SCREEN.stride);
		int inset = 0;

		if (radius > 0 && (j < radius || j >= h - radius)) {
			int dy = j < radius ? radius - 1 - j : j - (h - radius);

			/* the first x of the row inside a corner circle of this radius */
			inset = radius;
			while (inset > 0 && (radius - inset) * (radius - inset) + dy * dy < radius * radius) {
				inset--;
			}
		}
		for (int i = inset; i < w - inset; i++) {
			int px = x + i;

			if (px >= 0 && px < (int)SCREEN.width) {
				row[menu_output_width ? (uint32_t)px * menu_output_width / SCREEN.width : (uint32_t)px] = c;
			}
		}
	}
}

static int
menu_text_width(const char *s, int scale)
{
	return (int)str_len(s) * 8 * scale;
}

static void
menu_text(int x, int y, const char *s, int scale, uint32_t rgb)
{
	uint32_t c = menu_rgb(rgb);

	for (; *s; s++, x += 8 * scale) {
		const unsigned char *glyph = menu_font + (unsigned char)*s * 16;

		for (int gy = 0; gy < 16; gy++) {
			for (int gx = 0; gx < 8; gx++) {
				if (!(glyph[gy] & (1u << gx))) {
					continue;
				}
				for (int sy = 0; sy < scale; sy++) {
					int py = y + gy * scale + sy;

					if (py < 0 || py >= (int)SCREEN.height) {
						continue;
					}
					uint32_t *row = (uint32_t *)(uintptr_t)(SCREEN.base + (uint64_t)(menu_output_height ? (uint32_t)py * menu_output_height / SCREEN.height : (uint32_t)py) * SCREEN.stride);

					for (int sx = 0; sx < scale; sx++) {
						int px = x + gx * scale + sx;

						if (px >= 0 && px < (int)SCREEN.width) {
							row[menu_output_width ? (uint32_t)px * menu_output_width / SCREEN.width : (uint32_t)px] = c;
						}
					}
				}
			}
		}
	}
}

static void
menu_text_center(int y, const char *s, int scale, uint32_t rgb)
{
	menu_text(((int)SCREEN.width - menu_text_width(s, scale)) / 2, y, s, scale, rgb);
}

static void
menu_flush(void)
{
	clean_dcache(SCREEN.base, SCREEN.base + (uint64_t)SCREEN.stride * SCREEN.height);
}

/*
 * The boot screen for people who just use the phone (A, 2026-11-15): no text and no menu, only iOS's own boot logo
 * (ProgressUI apple-logo@3x, white on black, centered, the size it has once iOS runs) from the moment the loader
 * starts until iOS draws -- the same picture Samsung's bootloader shows from up_param (tools/bootlogo/mkbootlogo.py
 * makes both from YOUR iOS root; the artwork is never committed: without xnuloader/applelogo.h the screen is black).
 * A volume key pressed while the logo shows opens the menu. Dev mode (payload entry "devmode", x1slte.sh DEVMODE=1;
 * or bootui=0 in the cmdline): the loader's text and the HackiOS menu with its countdown, as before.
 */
#if __has_include("applelogo.h")
#include "applelogo.h"
#endif
static int bootui = 1;
#define BOOTUI_KEY_MS   700             /* how long a volume key press is looked for before iOS starts */

static void
boot_logo(void)
{
	menu_fill(0, 0, (int)SCREEN.width, (int)SCREEN.height, 0x000000, 0);
#ifdef APPLELOGO_W
	/* drawn for the 1440-wide native panel (iOS's 3x at 1080 is x 4/3); centered as iOS centers it */
	int ox = ((int)SCREEN.width - APPLELOGO_W) / 2, oy = ((int)SCREEN.height - APPLELOGO_H) / 2;
	uint32_t at = 0;

	for (uint32_t i = 0; i + 1 < sizeof(applelogo_rle); i += 2) {
		uint32_t v = applelogo_rle[i], n = applelogo_rle[i + 1];

		for (uint32_t k = 0; k < n; k++, at++) {
			int px = ox + (int)(at % APPLELOGO_W), py = oy + (int)(at / APPLELOGO_W);

			if (v == 0 || px < 0 || py < 0 || px >= (int)SCREEN.width || py >= (int)SCREEN.height) {
				continue;
			}
			uint32_t *row = (uint32_t *)(uintptr_t)(SCREEN.base + (uint64_t)(menu_output_height ?
			    (uint32_t)py * menu_output_height / SCREEN.height : (uint32_t)py) * SCREEN.stride);
			row[menu_output_width ? (uint32_t)px * menu_output_width / SCREEN.width : (uint32_t)px] =
			    menu_rgb(v * 0x010101u);
		}
	}
#endif
	menu_flush();
}

enum { MENU_BOOT, MENU_USB, MENU_DOWNLOAD, MENU_REBOOT, MENU_OFF, MENU_COUNT };
static const char *const menu_items[MENU_COUNT] = { "Boot iOS", "Log access (USB)", "Download Mode", "Reboot", "Power off" };

#define MENU_TOP        980
#define MENU_ROW        230
#define MENU_SCALE      5

static void
menu_draw(int selected, const char *banner, int countdown)
{
	int w = (int)SCREEN.width;
	char line[64];

	menu_fill(0, 0, w, (int)SCREEN.height, 0x000000, 0);
	menu_text_center(300, "JesviOS", 10, 0xffffff);
	menu_text_center(500, "Galaxy S20  -  iOS 16.5", 3, 0x8e8e93);
	if (banner != NULL && banner[0] != 0) {
		menu_text_center(640, banner, 3, 0xffd60a);
	}
	for (int i = 0; i < MENU_COUNT; i++) {
		int y = MENU_TOP + i * MENU_ROW, x = 90;

		menu_fill(x, y, w - 2 * x, MENU_ROW - 40, i == selected ? 0x0a84ff : 0x1c1c1e, 48);
		menu_text(x + 70, y + (MENU_ROW - 40 - 16 * MENU_SCALE) / 2, menu_items[i], MENU_SCALE,
		    i == selected ? 0xffffff : 0xebebf5);
	}
	if (countdown > 0) {
		static const char head[] = "Booting iOS in ", tail[] = " s - press a key to stay";
		unsigned n = 0;

		for (unsigned i = 0; head[i]; i++) {
			line[n++] = head[i];
		}
		line[n++] = (char)('0' + (countdown > 9 ? 9 : countdown));
		for (unsigned i = 0; tail[i]; i++) {
			line[n++] = tail[i];
		}
		line[n] = 0;
		menu_text_center(MENU_TOP + MENU_COUNT * MENU_ROW + 60, line, 3, 0x8e8e93);
	}
	menu_text_center((int)SCREEN.height - 360, "Volume Up / Down: move", 3, 0x8e8e93);
	menu_text_center((int)SCREEN.height - 290, "Power: select", 3, 0x8e8e93);
	menu_flush();
}

/* a key, or 0 for none: 'u' up, 'd' down, 's' select */
static int
menu_key(void)
{
	efi_simple_text_input *in = (efi_simple_text_input *)ST->con_in;
	efi_input_key key;

	if (in == NULL || EFI_ERROR(in->read_key_stroke(in, &key))) {
		return 0;
	}
	if (key.scan_code == EFI_SCAN_UP || key.scan_code == EFI_SCAN_LEFT) {
		return 'u';
	}
	if (key.scan_code == EFI_SCAN_DOWN || key.scan_code == EFI_SCAN_RIGHT) {
		return 'd';
	}
	if (key.unicode_char == '\r' || key.unicode_char == '\n' || key.scan_code == EFI_SCAN_ESC) {
		return 's';
	}
	return 'x';
}

/* the choice: `wait` seconds untouched boot iOS (0: wait for a key) */
static int
menu_choose(const char *banner, int wait)
{
	uint64_t freq = timer_freq(), began = READ_SYSREG(CNTVCT_EL0), last_pet = began;
	int selected = MENU_BOOT, left = wait, touched = wait == 0;

	while (menu_key() != 0) {               /* (keys still held from the bootloader's combination) */
	}
	menu_draw(selected, banner, touched ? 0 : left);
	for (;;) {
		int k = menu_key();
		uint64_t now = READ_SYSREG(CNTVCT_EL0);

		if (now - last_pet > freq) {
			last_pet = now;
			wdt_pet();
		}
		if (k == 0) {
			if (!touched) {
				int remaining = wait - (int)((now - began) / freq);

				if (remaining <= 0) {
					return MENU_BOOT;
				}
				if (remaining != left) {
					left = remaining;
					menu_draw(selected, banner, left);
				}
			}
			BS->stall(20000);
			continue;
		}
		touched = 1;
		if (k == 'u') {
			selected = (selected + MENU_COUNT - 1) % MENU_COUNT;
		} else if (k == 'd') {
			selected = (selected + 1) % MENU_COUNT;
		} else if (k == 's') {
			return selected;
		}
		menu_draw(selected, banner, 0);
	}
}

/* the logo is up: a volume key within BOOTUI_KEY_MS opens the menu, else iOS starts */
static int
bootui_choose(void)
{
	uint64_t freq = timer_freq(), began = READ_SYSREG(CNTVCT_EL0);

	while (menu_key() != 0) {               /* (keys still held from the bootloader's combination) */
	}
	while (READ_SYSREG(CNTVCT_EL0) - began < freq * BOOTUI_KEY_MS / 1000) {
		int k = menu_key();

		if (k == 'u' || k == 'd') {
			return menu_choose("", 0);
		}
		BS->stall(10000);
	}
	return MENU_BOOT;
}

/* a status screen: a title and up to four lines */
static void
menu_status(const char *title, const char *l1, const char *l2, const char *l3, const char *l4)
{
	const char *lines[4] = { l1, l2, l3, l4 };

	menu_fill(0, 0, (int)SCREEN.width, (int)SCREEN.height, 0x000000, 0);
	menu_text_center(300, "JesviOS", 10, 0xffffff);
	menu_text_center(700, title, 5, 0x0a84ff);
	for (int i = 0; i < 4; i++) {
		if (lines[i] != NULL) {
			menu_text_center(1000 + i * 120, lines[i], 3, 0xebebf5);
		}
	}
	menu_text_center((int)SCREEN.height - 290, "Power: back to the menu", 3, 0x8e8e93);
	menu_flush();
}

/*
 * Samsung's Download Mode, as `reboot download` asks for it (drivers/samsung/sec_reboot.c, exynos-reboot.c):
 * PMU INFORM2[23:16] = 0x4E (a normal reboot, not into the charging screen), INFORM3 = 0x12345601
 * (SEC_RESET_REASON_DOWNLOAD), RST_STAT cleared, the software reset.
 */
#define Y2S_PMU                 0x15860000ULL
#define SEC_RESET_REASON_DOWNLOAD 0x12345601u
#define SEC_RESET_REASON_RECOVERY 0x12345604u   /* sec_reboot.c "recovery": S-Boot starts the recovery partition */
static void
reboot_samsung(uint32_t reason)
{
	volatile uint32_t *pmu = (volatile uint32_t *)(uintptr_t)Y2S_PMU;
	uint32_t v = pmu[0x808 / 4];

	v &= ~(0xffu << 16);
	v |= 0x4eu << 16;
	pmu[0x808 / 4] = v;
	pmu[0x80c / 4] = reason;
	__asm__ volatile ("dsb sy" ::: "memory");
	pmu[0x404 / 4] = 0;
	pmu[0x3a00 / 4] = 1u << 1;
	__asm__ volatile ("dsb sy" ::: "memory");
	BS->stall(1000000);
	ST->rt->reset_system(0 /* EfiResetCold */, 0, 0, NULL);
}
static void reboot_download(void) { reboot_samsung(SEC_RESET_REASON_DOWNLOAD); }
/* Samsung's `reboot recovery`: TWRP after tools/install/flash-twrp.sh wrote it (Jesvi iOS's BOOT holds this loader too,
 * so a plain restart comes back here, not to recovery) */
static void reboot_recovery(void) { reboot_samsung(SEC_RESET_REASON_RECOVERY); }

static void
power_off(void)
{
	volatile uint32_t *pmu = (volatile uint32_t *)(uintptr_t)Y2S_PMU;

	ST->rt->reset_system(2 /* EfiResetShutdown */, 0, 0, NULL);
	/* (sec_power_off: PS_HOLD low; with a charger in, S-Boot comes back to the charging screen) */
	pmu[0x30c / 4] &= ~(1u << 8);
	__asm__ volatile ("dsb sy" ::: "memory");
	BS->stall(2000000);
}

/* ---- USB: the fastboot server over EFI_USBFN_IO_PROTOCOL (high speed: bulk 512, ep0 64) ---- */

static const uint8_t ufn_dev_desc[18] = { 18, 1, 0x00, 0x02, 0, 0, 0, 64, 0xd1, 0x18, 0x0d, 0xd0, 0x00, 0x01, 1, 2, 3, 1 };
static uint8_t ufn_ep0_buf[256] __attribute__((aligned(64)));
static int ufn_configured;
static int ufn_done[2];                 /* a transfer on endpoint 1 finished: [0] OUT (rx), [1] IN (tx) */
static uint64_t ufn_bytes[2];
static int ufn_status[2];

static void
ufn_setup(const efi_usb_device_request *r)
{
	static const uint8_t status0[2] = { 0, 0 };
	const uint8_t *data = NULL;
	uint64_t len = 0;
	uint8_t one = 1;

	if (r->request_type == 0x80 && r->request == 6) {       /* GET_DESCRIPTOR */
		uint8_t type = (uint8_t)(r->value >> 8), index = (uint8_t)r->value;
		static const uint8_t *const strings[5] = { usb_str0, usb_str_mfr, usb_str_prod, usb_str_serial, usb_str_iface };

		if (type == 1) {
			data = ufn_dev_desc, len = sizeof(ufn_dev_desc);
		} else if (type == 2) {
			data = usb_cfg_desc, len = sizeof(usb_cfg_desc);
		} else if (type == 3 && index < 5) {
			data = strings[index], len = strings[index][0];
		} else if (type == 6) {
			data = usb_qualifier, len = sizeof(usb_qualifier);
		}
	} else if (r->request_type == 0x80 && r->request == 0) {       /* GET_STATUS */
		data = status0, len = 2;
	} else if (r->request_type == 0x80 && r->request == 8) {       /* GET_CONFIGURATION */
		ufn_ep0_buf[0] = (uint8_t)ufn_configured;
		data = ufn_ep0_buf, len = 1;
	} else if (r->request_type == 0x00 && r->request == 9) {       /* SET_CONFIGURATION: the status stage is the driver's */
		ufn_configured = r->value != 0;
		usb_connected = ufn_configured;
		return;
	} else if ((r->request_type == 0x02) && (r->request == 1 || r->request == 3) && r->value == 0 && (r->index & 0x0f) != 0) {
		/*
		 * CLEAR_FEATURE / SET_FEATURE (ENDPOINT_HALT) of a bulk endpoint. macOS's fastboot clears both pipes every
		 * time it opens the device, and the host's data toggle goes back to DATA0 with it: the device's must too
		 * (DWC3's CLEARSTALL resets the sequence number), or every other packet after the first session is
		 * dropped and the next command never arrives (25 October: the first command answered, the rest hung).
		 */
		uint8_t stall = r->request == 3;

		USBFN->set_endpoint_stall_state(USBFN, (uint8_t)(r->index & 0x0f), (r->index & 0x80) ? UsbfnDirDeviceTx :
		    UsbfnDirDeviceRx, &stall);
		return;
	} else if (r->length == 0) {
		return;                 /* SET_INTERFACE, ...: nothing to say, the driver acknowledges */
	}
	if (data == NULL) {
		USBFN->set_endpoint_stall_state(USBFN, 0, (r->request_type & 0x80) ? UsbfnDirDeviceTx : UsbfnDirDeviceRx, &one);
		return;
	}
	if (len > r->length) {
		len = r->length;
	}
	if (data != ufn_ep0_buf) {
		memcpy(ufn_ep0_buf, data, (size_t)len);
	}
	USBFN->transfer(USBFN, 0, UsbfnDirDeviceTx, &len, ufn_ep0_buf);
}

/* one message from the driver: 0 nothing, 1 something */
static int
ufn_pump(void)
{
	uint32_t msg = EfiUsbMsgNone;
	uint64_t size = sizeof(efi_usbfn_message_payload);
	efi_usbfn_message_payload p;

	if (EFI_ERROR(USBFN->event_handler(USBFN, &msg, &size, &p)) || msg == EfiUsbMsgNone) {
		return 0;
	}
	switch (msg) {
	case EfiUsbMsgSetupPacket:
		ufn_setup(&p.udr);
		break;
	case EfiUsbMsgEndpointStatusChangedRx:
	case EfiUsbMsgEndpointStatusChangedTx:
		if (p.utr.endpoint == 1) {
			int tx = msg == EfiUsbMsgEndpointStatusChangedTx;

			ufn_done[tx] = 1;
			ufn_bytes[tx] = p.utr.bytes;
			ufn_status[tx] = (int)p.utr.status;
		}
		break;
	case EfiUsbMsgBusEventDetach:
	case EfiUsbMsgBusEventReset:
		ufn_configured = 0;
		usb_connected = 0;
		break;
	default:
		break;
	}
	return 1;
}

static int ufn_leave;                   /* Power pressed, or "oem boot": out of the command loop */

/* until endpoint 1's transfer `tx` finishes: the bytes, or -1 (aborted, unplugged, Power pressed) */
static int64_t
ufn_wait(int tx)
{
	uint64_t freq = timer_freq(), began = READ_SYSREG(CNTVCT_EL0), last_pet = began;

	for (;;) {
		/* a send the host does not take within 5 s: it went away (a fastboot that was stopped) */
		if (tx && READ_SYSREG(CNTVCT_EL0) - began > 5 * freq) {
			USBFN->abort_transfer(USBFN, 1, UsbfnDirDeviceTx);
			ufn_done[1] = 0;
			return -1;
		}
		if (!ufn_pump()) {
			if (menu_key() == 's') {
				ufn_leave = 1;
				USBFN->abort_transfer(USBFN, 1, tx ? UsbfnDirDeviceTx : UsbfnDirDeviceRx);
				return -1;
			}
		}
		if (ufn_done[tx]) {
			ufn_done[tx] = 0;
			return ufn_status[tx] == UsbfnTransferComplete ? (int64_t)ufn_bytes[tx] : -1;
		}
		if (!ufn_configured) {
			return -1;
		}
		if (READ_SYSREG(CNTVCT_EL0) - last_pet > freq) {
			last_pet = READ_SYSREG(CNTVCT_EL0);
			wdt_pet();
		}
	}
}

static int64_t
ufn_rx(uint8_t *buf, uint64_t len)
{
	uint64_t n = (len + 511) & ~511ULL;
	int64_t got;

	ufn_done[0] = 0;
	if (EFI_ERROR(USBFN->transfer(USBFN, 1, UsbfnDirDeviceRx, &n, buf))) {
		return -1;
	}
	got = ufn_wait(0);
	if (got > 0) {
		flush_dcache((uint64_t)(uintptr_t)buf, (uint64_t)(uintptr_t)buf + (uint64_t)got);
	}
	return got;
}

static int64_t
ufn_tx(const uint8_t *data, uint64_t len)
{
	uint64_t n = len;

	ufn_done[1] = 0;
	if (EFI_ERROR(USBFN->transfer(USBFN, 1, UsbfnDirDeviceTx, &n, (void *)(uintptr_t)data))) {
		return -1;
	}
	return ufn_wait(1);
}

static int
ufn_start(void)
{
	static const efi_guid guid = EFI_USBFN_IO_GUID;
	static const void *const endpoints[2] = { usb_cfg_desc + 18, usb_cfg_desc + 25 };
	static const efi_usb_interface_info iface = { usb_cfg_desc + 9, endpoints };
	static const efi_usb_interface_info *const ifaces[1] = { &iface };
	static const efi_usb_config_info config = { usb_cfg_desc, ifaces };
	static const efi_usb_config_info *const configs[1] = { &config };
	static efi_usb_device_info info = { ufn_dev_desc, configs };
	static int started;
	efi_status s;

	if (started) {
		return 1;
	}
	if (EFI_ERROR(BS->locate_protocol(&guid, NULL, (void **)&USBFN))) {
		print("usb: no EFI_USBFN_IO_PROTOCOL\n");
		USBFN = NULL;
		return 0;
	}
	s = USBFN->start_controller(USBFN);
	print("usb: UsbfnDwc3: start %x\n", s);
	s = USBFN->configure_enable_endpoints(USBFN, &info);
	print("usb: endpoints %x\n", s);
	if (EFI_ERROR(s)) {
		return 0;
	}
	if (usb_rx_buf == NULL) {
		uint64_t at = 0;

		if (!allocate_loader_memory(2 * USB_CHUNK, 0x1000, 0x1000, ~0ULL, &at)) {
			return 0;
		}
		usb_rx_buf = (uint8_t *)(uintptr_t)at;
		usb_tx_buf = (uint8_t *)(uintptr_t)(at + USB_CHUNK);
	}
	started = 1;
	return 1;
}

/* the last run's log, as it was saved: recovery + 16 MB, NULs and all (the host drops them) */
static int
stage_last_log(void)
{
	uint8_t *buf = stage_buf != NULL ? stage_buf : (uint8_t *)(uintptr_t)download_base;

	if (!read_partition_into(LOG_PARTITION_ASCII, LOG_OFFSET, buf, 8ULL << 20)) {
		return 0;
	}
	staged_data = (const char *)buf;
	staged_len = 8ULL << 20;
	return 1;
}

/*
 * What was downloaded, into a partition at an offset (bytes, a multiple of the block): only where this port
 * keeps its own things -- recovery, boot, super's tail (system.apfs at 0x184B00000) -- never the bootloaders,
 * EFS or Samsung's other partitions. Read back and compared.
 */
/*
 * Where super's free tail begins: past the last logical partition in super's LP metadata, which differs per phone
 * and firmware. y2s (SM-G986B): 0x184B00000. x1slte (SM-G980F, G980FXXSNHYB7): 0x1E8B04000 -- 0x184B00000 is
 * inside its vendor partition. Set with -DSUPER_TAIL_START=... (xnuloader/build.sh, from the device's .conf).
 */
#ifndef SUPER_TAIL_START
#define SUPER_TAIL_START 0x1E8B04000ULL
#endif
/*
 * A window of userdata the port may write (bytes): a pinned, preallocated f2fs file's single extent,
 * read with FIEMAP. 0 (the default): no write to userdata at all.
 */
#ifndef USERDATA_WIN_OFF
#define USERDATA_WIN_OFF 0ULL
#endif
#ifndef SUPER_ROOT_LEN
#define SUPER_ROOT_LEN 0ULL             /* -DSUPER_ROOT_LEN: super [0, LEN) is the iOS system volume's (x1slte, iOS only) */
#endif
#ifndef USERDATA_WIN_LEN
#define USERDATA_WIN_LEN 0ULL
#endif

static const char *
stage_write(const char *part, uint64_t off)
{
	efi_block_io *bio = find_partition_ascii(part);
	uint64_t bs, size, len = stage_len, done = 0;
	uint8_t *src = stage_buf;

#if FLASH_BOOT_ONLY
	/* x1s (SM-G981B): the owner's rule: only the boot partition is written, nothing else, whatever is asked */
	if (!str_eq(part, "boot")) {
		return "refused: this build (FLASH_BOOT_ONLY) writes only the boot partition";
	}
#endif
	if (str_eq(part, "userdata")) {
		if (USERDATA_WIN_LEN == 0 || off < USERDATA_WIN_OFF || off + len > USERDATA_WIN_OFF + USERDATA_WIN_LEN) {
			return "refused: userdata only inside the port's own window (USERDATA_WIN_OFF/LEN)";
		}
	} else if (str_eq(part, "super") && SUPER_ROOT_LEN != 0 && off + len <= SUPER_ROOT_LEN && SUPER_ROOT_LEN <= SUPER_TAIL_START) {
		/* x1slte, iOS only (2026-11-06): the system volume at the head of super (Android's system/vendor are gone) */
	} else if (str_eq(part, "up_param")) {
		/* (A, 2026-11-15) Samsung's splash pictures (logo.jpg...): a tar of JPEGs, nothing the phone runs. Only a
		 * whole tar from offset 0 (ustar magic in its first header), so a wrong download cannot land here. */
		if (off != 0 || stage_len < 512 || memcmp(stage_buf + 257, "ustar", 5) != 0) {
			return "refused: up_param takes only a tar of pictures, from offset 0";
		}
	} else if (!(str_eq(part, "recovery") || str_eq(part, "boot") || (str_eq(part, "super") && off >= SUPER_TAIL_START))) {
		return "refused: only recovery, boot, up_param, super's free tail (SUPER_TAIL_START) and the userdata window";
	}
	if (bio == NULL || bio->media == NULL || src == NULL || len == 0) {
		return "no such partition, or nothing downloaded";
	}
	bs = bio->media->block_size;
	size = (bio->media->last_block + 1) * bs;
	if (bs == 0 || off % bs != 0) {
		return "the offset is not a multiple of the block";
	}
	len = (len + bs - 1) / bs * bs;         /* (the stage's tail beyond the download is zeroed) */
	if (off + len > size || len > stage_size) {
		return "past the end of the partition";
	}
	print("write: %d bytes to %s+%x\n", len, part, off);
	while (done < len) {
		uint64_t chunk = len - done > (4ULL << 20) ? (4ULL << 20) : len - done;

		if (EFI_ERROR(bio->write_blocks(bio, bio->media->media_id, (off + done) / bs, chunk, src + done))) {
			return "the write failed";
		}
		done += chunk;
		wdt_pet();
	}
	bio->flush_blocks(bio);
	for (done = 0; done < len;) {
		uint64_t chunk = len - done > sizeof(ufs_buffer) ? sizeof(ufs_buffer) : len - done;

		if (EFI_ERROR(bio->read_blocks(bio, bio->media->media_id, (off + done) / bs, chunk, ufs_buffer)) ||
		    memcmp(ufs_buffer, src + done, (size_t)chunk) != 0) {
			return "READ BACK DIFFERENT";
		}
		done += chunk;
	}
	return NULL;
}

/*
 * "Log access": the fastboot server until Power is pressed or the host says "oem boot". Returns 1 to boot iOS.
 * Host: ./y2s.sh ulog (oem lastlog, get_staged), ./y2s.sh uflash (stage, oem write recovery 0), oem boot.
 */
static int
log_access(void)
{
	/* Recheck firmware ownership/cache policy after the kernel returned. */
	report_cellular_memory();
	if (stage_buf == NULL) {
		uint64_t at = 0;

		if (allocate_loader_memory(STAGE_SIZE, 0x1000, 0x1000, ~0ULL, &at)) {
			stage_buf = (uint8_t *)(uintptr_t)at;
			stage_size = STAGE_SIZE;
		}
	}
	if (!ufn_start()) {
		menu_status("Log access", "USB could not be started", "(no EFI_USBFN_IO_PROTOCOL)", NULL, NULL);
		while (menu_key() != 's') {
			BS->stall(20000);
			wdt_pet();
		}
		return 0;
	}
	/* (the last log is read from the flash when the host asks for it: oem lastlog) */
	menu_status("Log access (USB)", "Connect the phone to the Mac, then:", "./x1slte.sh ulog    the last log",
	    "./x1slte.sh uflash  a new build", "./x1slte.sh uboot   boot iOS");
	ufn_leave = 0;
	usb_boot_asked = 0;
	while (!ufn_leave && !usb_boot_asked) {
		char cmd[128];
		int64_t n;

		if (!ufn_configured) {
			if (!ufn_pump()) {
				if (menu_key() == 's') {
					break;
				}
				BS->stall(2000);
			}
			continue;
		}
		n = usb_rx(usb_rx_buf, 511);
		if (n <= 0) {
			continue;
		}
		if (n > (int64_t)sizeof(cmd) - 1) {
			n = sizeof(cmd) - 1;
		}
		memcpy(cmd, usb_rx_buf, (size_t)n);
		cmd[n] = 0;
		fb_command(cmd);
	}
	return usb_boot_asked;
}

/*
 * The y2s loader: the menu at power-on, and again whenever iOS comes back. "Boot iOS" by itself after
 * 3 s at power-on; after a run, the menu waits.
 */

/*
 * x1slte: sboot starts recovery (this loader) only while misc's bootloader message says "boot-recovery"; after a reset
 * it fell back to boot and hung at the Samsung logo (2026-11-05). The loader puts the command back at every start, so
 * any reset or crash comes back here. Only the message's command field (misc's first 32 bytes) is written, read back.
 * Android: the stock misc (ios-port-research/device/misc/misc-stock-android.img) through Download Mode.
 */
static void
misc_keep_recovery(void)
{
	static uint8_t blk[4096] __attribute__((aligned(4096)));
	static const char cmd[] = "boot-recovery";
	efi_block_io *bio = find_partition_ascii("misc");
	uint64_t bs;

#if FLASH_BOOT_ONLY
	/* x1s (SM-G981B): starts from boot, not recovery, and only boot is ever written: misc is left alone */
	(void)blk; (void)cmd; (void)bio; (void)bs;
	print("misc: not written (FLASH_BOOT_ONLY)\n");
	return;
#endif
	if (bio == NULL || bio->media == NULL || (bs = bio->media->block_size) == 0 || bs > sizeof(blk)) {
		print("misc: no partition (or an odd block size): the boot-recovery flag is not kept\n");
		return;
	}
	if (EFI_ERROR(bio->read_blocks(bio, bio->media->media_id, 0, bs, blk))) {
		print("misc: not read\n");
		return;
	}
	if (memcmp(blk, cmd, sizeof(cmd)) == 0) {
		print("misc: boot-recovery is set\n");
		return;
	}
	memset(blk, 0, 32);
	memcpy(blk, cmd, sizeof(cmd));
	if (EFI_ERROR(bio->write_blocks(bio, bio->media->media_id, 0, bs, blk))) {
		print("misc: boot-recovery could not be written\n");
		return;
	}
	if (bio->flush_blocks != NULL) {
		bio->flush_blocks(bio);
	}
	memset(blk, 0, 32);
	(void)bio->read_blocks(bio, bio->media->media_id, 0, bs, blk);
	print("misc: boot-recovery %s\n", memcmp(blk, cmd, sizeof(cmd)) == 0 ? "written and read back" : "written, but reads back otherwise");
}
static void
y2s_menu_loop(void)
{
	const char *banner = "";
	int wait = 3, first = 1;

	misc_keep_recovery();

	for (;;) {
		int choice;

		if (y2s_want_download) {
			y2s_want_download = 0;
			menu_status("Download Mode", "the kernel asked for it", NULL, NULL, NULL);
			reboot_download();
		}
		if (y2s_want_log_access) {
			y2s_want_log_access = 0;
			choice = MENU_USB;
			wait = 0;
			banner = "the kernel asked for Log access";
		} else if (bootui && first) {
			choice = bootui_choose();
		} else {
			choice = menu_choose(banner, wait);
		}
		first = 0;
		wait = 0;
		if (choice == MENU_USB) {
			if (!log_access()) {
				banner = "";
				continue;
			}
			choice = MENU_BOOT;
		}
		if (choice == MENU_BOOT) {
			/* The firmware must restore native DSIM/DSC geometry before another kernel. */
			if (menu_output_width != 0) {
				ST->rt->reset_system(0 /* EfiResetCold */, 0, 0, NULL);
				continue;
			}
			if (bootui) {
				boot_logo();
			} else {
				menu_fill(0, 0, (int)SCREEN.width, (int)SCREEN.height, 0x000000, 0);
				menu_text_center((int)SCREEN.height / 2 - 40, "Starting iOS", 5, 0xffffff);
				menu_flush();
			}
			log_len = 0;
			print("== XNULOADER run (menu) ==\n");
			if (!run_kernel(0, 0)) {
				flush_log();            /* (why, from this loader's own lines, for Log access) */
				banner = "iOS did not start: see Log access";
			} else {
				banner = "iOS returned: its log is saved (Log access)";
			}
			console_suspect = 1;            /* (the firmware's console after a run: see put_char) */
		} else if (choice == MENU_DOWNLOAD) {
			menu_status("Download Mode", "Rebooting into Samsung's Download Mode", NULL, NULL, NULL);
			reboot_download();
		} else if (choice == MENU_REBOOT) {
			ST->rt->reset_system(0 /* EfiResetCold */, 0, 0, NULL);
		} else if (choice == MENU_OFF) {
			menu_status("Power off", "Turning off", NULL, NULL, NULL);
			power_off();
		}
	}
}

/* ------------------------------------------------------------------------ */

/*
 * QEMU's "ramfb" display shows whatever guest memory it is told to, in any size; the firmware
 * only offers a few small landscape modes of it. So we point it at memory of our own with the
 * shape of the iPhone's screen, through the fw_cfg file "etc/ramfb" (the firmware's own mode
 * is simply replaced). In QEMU the display is then exactly what iOS expects; on the real phone
 * the kernel's display driver scales that picture to the panel.
 */
#define PHONE_WIDTH     1125            /* the screen of the iPhone X, whose identity we carry */
#define PHONE_HEIGHT    2436
/* HACKIOS_RAMFB_HEIGHT: QEMU's ramfb refuses a mode taller than 2048 (bochs-vbe's
 * VBE_DISPI_MAX_YRES), so the panel is asked for 2048 rows and the kernel scales the
 * 1125x2436 display into it -- the same thing it does on a real phone. */
#define RAMFB_HEIGHT    2048
#define FW_CFG_BASE     0x09020000ULL   /* QEMU virt: data at +0, selector at +8, DMA address at +16 */
#define FW_CFG_FILE_DIR 0x19
#define FW_CFG_DMA_SELECT 0x08
#define FW_CFG_DMA_WRITE  0x10

static uint32_t
be32(uint32_t v)
{
	return __builtin_bswap32(v);
}

static void
fw_cfg_read(void *buf, uint32_t len)
{
	for (uint32_t i = 0; i < len; i++) {
		((uint8_t *)buf)[i] = *(volatile uint8_t *)FW_CFG_BASE;
	}
}

static void
ramfb_phone_screen(void)
{
	static struct {
		uint64_t addr;
		uint32_t fourcc, flags, width, height, stride;
	} __attribute__((packed)) cfg;
	static struct {
		uint32_t control, length;
		uint64_t address;
	} dma;
	struct {
		uint32_t size;
		uint16_t select, reserved;
		char name[56];
	} file;
	uint32_t count, select = 0;
	uint64_t fb = 0, bytes = (uint64_t)PHONE_WIDTH * 4 * RAMFB_HEIGHT;

	*(volatile uint16_t *)(FW_CFG_BASE + 8) = __builtin_bswap16(FW_CFG_FILE_DIR);
	fw_cfg_read(&count, sizeof(count));
	count = be32(count);
	for (uint32_t i = 0; i < count && i < 256 && select == 0; i++) {
		fw_cfg_read(&file, sizeof(file));
		if (str_eq(file.name, "etc/ramfb")) {
			select = __builtin_bswap16(file.select);
		}
	}
	if (select == 0 || !allocate_loader_memory((bytes + 0xFFF) & ~0xFFFULL, 0x1000, 0x1000, ~0ULL, &fb)) {
		return;
	}
	memset((void *)(uintptr_t)fb, 0, (size_t)bytes);
	cfg.addr = __builtin_bswap64(fb);
	cfg.fourcc = be32(0x34325258);          /* 'XR24': blue, green, red, unused */
	cfg.flags = 0;
	cfg.width = be32(PHONE_WIDTH);
	cfg.height = be32(RAMFB_HEIGHT);
	cfg.stride = be32(PHONE_WIDTH * 4);
	dma.control = be32(select << 16 | FW_CFG_DMA_SELECT | FW_CFG_DMA_WRITE);
	dma.length = be32(sizeof(cfg));
	dma.address = __builtin_bswap64((uint64_t)(uintptr_t)&cfg);
	*(volatile uint64_t *)(FW_CFG_BASE + 16) = __builtin_bswap64((uint64_t)(uintptr_t)&dma);
	if (dma.control != 0) {
		/* HACKIOS: this is the value we wrote ourselves; nothing writes back into it, so it
		 * says nothing about whether the request worked. QEMU's ramfb refuses a mode taller
		 * than 2048 (VBE_DISPI_MAX_YRES), and the mode it takes is the one in the panel. */
		print("ramfb: control %x after the request (the mode is taken as it stands)\n",
		    (uint64_t)be32(dma.control));
	}
	SCREEN.base = fb;
	SCREEN.width = PHONE_WIDTH;
	SCREEN.height = RAMFB_HEIGHT;
	SCREEN.stride = PHONE_WIDTH * 4;
}

/*
 * y2s/x1s: ABOX's Calliope DSP (two Cortex-A32s) is not reset by a warm restart. One left running by the last run
 * (snd_calliope) keeps fetching through mappings the new kernel no longer owns and can write into its memory before
 * the kernel's own stop (bsd/dev/redwood_exynos_sound.c) runs: the boot loops of 2026-11-06. Stop both cores first
 * thing, every boot: PMU (0x15860000) core enable 0x31a0/0x3220 bit 0, then core power 0x3180/0x3200 bit 0
 * (exynos9830.dts abox-core@18c55000/@18c55080 samsung,pmu_enable / pmu_power).
 */
static void
y2s_calliope_stop(void)
{
	static const uint32_t regs[4] = { 0x31a0, 0x3220, 0x3180, 0x3200 };
	uint32_t before[4];

	for (unsigned i = 0; i < 4; i++) {
		volatile uint32_t *r = (volatile uint32_t *)(uintptr_t)(0x15860000ULL + regs[i]);

		before[i] = *r;
		if (before[i] & 1) {
			*r = before[i] & ~1U;
		}
	}
	__asm__ volatile ("dsb sy" ::: "memory");
	print("calliope: enable %x/%x power %x/%x%s\n", (uint64_t)before[0], (uint64_t)before[1], (uint64_t)before[2],
	    (uint64_t)before[3], ((before[0] | before[1] | before[2] | before[3]) & 1) ? ": was running, stopped" : "");
}

#ifdef CELLULAR_RESERVE_CP
/* Opt-in legacy-FD reservation: no modem action. Claim each CP range through
 * boot services, then set Normal NC through DXE (which calls CPU Arch).
 * Only the known fixed XNU Download descriptor may be relinquished. All
 * other ranges must already be free or reserved; partial ownership fails.
 * Preflight restores original attributes/ownership before fallback handoff;
 * normal iOS runs retain the reservations. A failed request is rolled back.
 */
static struct {
	uint64_t size, attributes;
	int claimed, cache_changed;
} cp_probe[3];
static int cp_old_download_freed;
static efi_dxe_services *cp_dxe;

static int
reserve_cellular_memory(void)
{
	static uint8_t map[65536];
	uint64_t bytes = sizeof(map), key, stride;
	uint32_t version;
	if (PLAT != &platform_y2s) return 0;
	cp_dxe = cellular_dxe_services();
	if (!cp_dxe) return 0;
	if (EFI_ERROR(BS->get_memory_map(&bytes, map, &key, &stride, &version)) ||
	    stride < sizeof(efi_memory_descriptor) || bytes > sizeof(map) || bytes % stride) return 0;
	int legacy = 0;
	for (uint64_t off = 0; off < bytes; off += stride) {
		efi_memory_descriptor *d = (efi_memory_descriptor *)(map + off);
		if (d->phys_start == 0xf0000000 && d->pages == 0x1000 && d->type == EfiBootServicesData) legacy++;
	}
	/* Validate all ownership and GCD ranges before changing any of them. */
	for (unsigned i = 0; i < 3; i++) {
		uint64_t base = s20_cp_regions[i].base;
		cp_probe[i].size = (s20_cp_regions[i].size + 0xffff) & ~0xffffULL;
		efi_gcd_memory_descriptor d;
		efi_status status = cp_dxe->get_memory_space_descriptor(base, &d);
		if (EFI_ERROR(status) || d.base > base || d.length > ~0ULL - d.base ||
		    d.base + d.length < base + cp_probe[i].size || !(d.capabilities & 2)) return 0;
		cp_probe[i].attributes = d.attributes;
		print("cellular reservation: GCD %x size %x attributes %x capabilities %x\n",
		    base, cp_probe[i].size, d.attributes, d.capabilities);
		if (s20_efi_map_covers(map, bytes, stride, base, cp_probe[i].size, EfiReservedMemoryType, 0)) continue;
		if (i == 0 && legacy == 1) continue;
		if (!s20_efi_map_covers(map, bytes, stride, base, cp_probe[i].size, EfiConventionalMemory, 0)) return 0;
	}
	if (legacy == 1) {
		efi_status status = BS->free_pages(0xf0000000, 0x1000);
		print("cellular reservation: release fixed download status %x\n", status);
		if (EFI_ERROR(status)) return 0;
		cp_old_download_freed = 1;
	}
	for (unsigned i = 0; i < 3; i++) {
		uint64_t base = s20_cp_regions[i].base;
		if (!s20_efi_map_covers(map, bytes, stride, base, cp_probe[i].size, EfiReservedMemoryType, 0)) {
			uint64_t at = base;
			efi_status status = BS->allocate_pages(AllocateAddress, EfiReservedMemoryType, cp_probe[i].size >> 12, &at);
			print("cellular reservation: claim %x size %x status %x\n", base, cp_probe[i].size, status);
			if (EFI_ERROR(status)) return 0;
			cp_probe[i].claimed = 1;
		}
		if ((cp_probe[i].attributes & 0x1f) != 2) {
			cp_probe[i].cache_changed = 1;
			efi_status status = cp_dxe->set_memory_space_attributes(base, cp_probe[i].size,
			    (cp_probe[i].attributes & ~0x1fULL) | 2);
			print("cellular reservation: set Normal NC %x size %x status %x\n", base, cp_probe[i].size, status);
			if (EFI_ERROR(status)) return 0;
		}
	}
	return 1;
}

static void
release_cellular_memory(void)
{
	for (unsigned i = 0; i < 3; i++) {
		uint64_t base = s20_cp_regions[i].base;
		if (cp_probe[i].cache_changed) {
			efi_status status = cp_dxe->set_memory_space_attributes(base, cp_probe[i].size, cp_probe[i].attributes);
			print("cellular reservation: restore attributes %x status %x\n", base, status);
		}
		if (cp_probe[i].claimed) {
			efi_status status = BS->free_pages(base, cp_probe[i].size >> 12);
			print("cellular reservation: release %x status %x\n", base, status);
		}
	}
	if (cp_old_download_freed) {
		uint64_t at = 0xf0000000;
		efi_status status = BS->allocate_pages(AllocateAddress, EfiBootServicesData, 0x1000, &at);
		print("cellular reservation: restore fixed download status %x\n", status);
	}
}
#endif
#ifdef CELLULAR_PREFLIGHT_ONLY
#ifdef CELLULAR_CP_GPIO_PREFLIGHT
/* Ownership/readback test only. All five AP-owned signals are held LOW;
 * POWER_ON is never asserted, no firmware is sent, CP2AP inputs are untouched.
 * Restore only each target's CON/DAT fields; protect RMW at TPL_HIGH_LEVEL,
 * preserving neighboring Bluetooth GPG1-1 and every unrelated GPIO field.
 */
static int
cellular_cp_gpio_preflight(void)
{
	static const struct {uint64_t base; unsigned pin;} pins[] = {
		{0x15c30120,0}, /* RESET_N */
		{0x15c30280,0}, /* POWER_ON */
		{0x15c30320,0}, /* AP_STATUS */
		{0x107300c0,3}, /* AP2CP_WAKE; NOT CP2AP_WAKE GPM30 */
		{0x10730060,6}, /* DUMP_NOTI, later shared with SPI */
	};
	uint32_t saved_con[5], saved_dat[5];
	int changed = 0, passed = 1;
	report_cellular_memory();
	uint64_t saved_par = READ_SYSREG(PAR_EL1), pmu = 0x15860720;
	__asm__ volatile ("at s1e1r, %0; isb" :: "r"(pmu) : "memory");
	int pmu_mapped = s20_par_device(READ_SYSREG(PAR_EL1), pmu);
	__asm__ volatile ("msr par_el1, %0; isb" :: "r"(saved_par) : "memory");
	if (!cellular_memory_ready || !pmu_mapped || (*(volatile uint32_t *)(uintptr_t)pmu & 1) ||
	    (*(volatile uint32_t *)(uintptr_t)0x15c30284 & 1) ||
	    (*(volatile uint32_t *)(uintptr_t)0x15c30384 & 1)) return 0;
	for (unsigned i=0;i<5;i++) {
		volatile uint32_t *bank = (volatile uint32_t *)(uintptr_t)pins[i].base;
		saved_con[i]=bank[0]; saved_dat[i]=bank[1];
		if ((saved_con[i]>>(4*pins[i].pin)) & 15) {
			print("cellular GPIO: refused existing owner at %x pin %d\n", pins[i].base, (uint64_t)pins[i].pin);
			return 0;
		}
	}
	for (unsigned i=0;i<5;i++) {
		volatile uint32_t *bank = (volatile uint32_t *)(uintptr_t)pins[i].base;
		uint32_t con,dat;
		uint64_t tpl=BS->raise_tpl(31 /* TPL_HIGH_LEVEL */);
		int valid=s5123_gpio_output_values(bank[0],bank[1],pins[i].pin,0,&con,&dat);
		if (valid) {bank[1]=dat; __asm__ volatile ("dsb sy" ::: "memory"); bank[0]=con; __asm__ volatile ("dsb sy" ::: "memory");}
		BS->restore_tpl(tpl);
		changed=(int)i+1;
		uint32_t got_con=bank[0],got_dat=bank[1];
		int okay=valid && ((got_con>>(4*pins[i].pin)) & 15)==1 && !((got_dat>>pins[i].pin)&1);
		print("cellular GPIO: output-low %x pin %d verified %d\n",pins[i].base,(uint64_t)pins[i].pin,(uint64_t)okay);
		if (!okay) {passed=0;break;}
	}
	if (passed) BS->stall(50000);
	for (int i=changed-1;i>=0;i--) {
		volatile uint32_t *bank = (volatile uint32_t *)(uintptr_t)pins[i].base;
		uint32_t con_mask=15u<<(4*pins[i].pin),dat_mask=1u<<pins[i].pin;
		uint64_t tpl=BS->raise_tpl(31);
		bank[0]=(bank[0]&~con_mask)|(saved_con[i]&con_mask);
		__asm__ volatile ("dsb sy" ::: "memory");
		bank[1]=(bank[1]&~dat_mask)|(saved_dat[i]&dat_mask);
		__asm__ volatile ("dsb sy" ::: "memory");
		BS->restore_tpl(tpl);
		int restored=(bank[0]&con_mask)==(saved_con[i]&con_mask);
		int neighbors=(bank[0]&~con_mask)==(saved_con[i]&~con_mask);
		print("cellular GPIO: restore %x pin %d verified %d neighbors preserved %d\n",
		    pins[i].base,(uint64_t)pins[i].pin,(uint64_t)restored,(uint64_t)neighbors);
		if (!restored || !neighbors) passed=0;
	}
	print("cellular GPIO: no power-on or firmware; result %d\n",(uint64_t)passed);
	return passed;
}
#endif
#ifdef CELLULAR_SPI_LOOPBACK
struct cellular_loopback_state {
	uint64_t base;
	const uint8_t *expected;
	uint32_t received, mismatches;
	int active;
};
static uint32_t
cellular_reg_read(uint64_t address)
{
	return *(volatile uint32_t *)(uintptr_t)address;
}
static void
cellular_reg_write(uint64_t address, uint32_t value)
{
	*(volatile uint32_t *)(uintptr_t)address = value;
	__asm__ volatile ("dsb sy" ::: "memory");
}
static int
cellular_clock_wait(uint64_t address, uint32_t mask, uint32_t value)
{
	for (unsigned i = 0; i < 1000; i++) {
		if ((cellular_reg_read(address) & mask) == value) return 1;
		BS->stall(10);
	}
	return 0;
}
static uint32_t
cellular_spi_read32(void *context, uint32_t offset)
{
	struct cellular_loopback_state *state = context;
	return cellular_reg_read(state->base + offset);
}
static uint8_t
cellular_spi_read8(void *context, uint32_t offset)
{
	struct cellular_loopback_state *state = context;
	uint8_t value = *(volatile uint8_t *)(uintptr_t)(state->base + offset);
	if (state->active) {
		if (state->received >= 16 || value != state->expected[state->received]) state->mismatches++;
		state->received++;
	}
	return value;
}
static void
cellular_spi_write32(void *context, uint32_t offset, uint32_t value)
{
	struct cellular_loopback_state *state = context;
	if (offset == S5123_MODE) value |= 1u << 3; /* INTERNAL loopback only. */
	if (offset == S5123_CH) state->active = value == 3;
	cellular_reg_write(state->base + offset, value);
}
static void
cellular_spi_write8(void *context, uint32_t offset, uint8_t value)
{
	struct cellular_loopback_state *state = context;
	*(volatile uint8_t *)(uintptr_t)(state->base + offset) = value;
}
static uint64_t
cellular_spi_now(void *context)
{
	(void)context;
	uint64_t ticks = READ_SYSREG(CNTVCT_EL0), frequency = timer_freq();
	return (ticks / frequency) * 1000000 + (ticks % frequency) * 1000000 / frequency;
}

/* One generated 16-byte internal-loopback packet, never firmware. External
 * SPI pins MUST stay GPIO inputs. No CP power/reset/pinmux write is allowed.
 * Only USI mode and SPI clock divider/mux are changed, then restored along
 * with the controller configuration. Refuse an already-configured owner.
 */
static int
cellular_spi_loopback(void)
{
	static const uint32_t spi_offsets[] = {S5123_CH,S5123_MODE,S5123_CS,S5123_IRQ,S5123_PACKET,S5123_SWAP,S5123_FEEDBACK,S5123_USI_CON};
	uint32_t saved_spi[sizeof(spi_offsets)/sizeof(spi_offsets[0])] = {0};
	uint32_t mux = cellular_reg_read(0x10700690), divider = cellular_reg_read(0x10701820);
	uint32_t mode = cellular_reg_read(0x10721048), pins = cellular_reg_read(0x10730060);
	uint32_t gate0 = cellular_reg_read(0x107020fc), gate1 = cellular_reg_read(0x1070208c);
	int changed_clock = 0, changed_mode = 0, saved_controller = 0, passed = 0;
	report_cellular_memory();
	if (!cellular_memory_ready || !timer_freq() || mode != 0 || (pins & 0xffff0000u) ||
	    (mux & (1u<<16)) || (divider & (1u<<16)) ||
	    !(gate0 & (1u<<21)) || !(gate1 & (1u<<21)) ||
	    (cellular_reg_read(0x15c30284) & 1) || (cellular_reg_read(0x15c30384) & 1)) {
		print("cellular SPI loopback: refused (memory, clock, existing owner, external pins or CP levels)\n");
		return 0;
	}
	print("cellular SPI loopback: generated 16-byte packet; external pins remain inputs; no CP writes\n");
	changed_clock = 1;
	cellular_reg_write(0x10701820, (divider & ~15u) | 3); /* 26MHz /4 IP /4 = 1.625MHz. */
	if (!cellular_clock_wait(0x10701820, (1u<<16)|15, 3)) goto restore;
	cellular_reg_write(0x10700690, mux & ~(1u<<4));
	if (!cellular_clock_wait(0x10700690, (1u<<16)|(1u<<4), 0)) goto restore;
	changed_mode = 1;
	cellular_reg_write(0x10721048, 2); /* SPI, while all four external pads remain inputs. */
	if (cellular_reg_read(0x10721048) != 2) goto restore;
	for (unsigned i = 0; i < sizeof(spi_offsets)/sizeof(spi_offsets[0]); i++)
		saved_spi[i] = cellular_reg_read(0x10960000 + spi_offsets[i]);
	saved_controller = 1;
	if ((saved_spi[0] & 3) || saved_spi[3] || saved_spi[4]) {
		print("cellular SPI loopback: controller unexpectedly active; no transfer\n");
		goto restore;
	}
	uint8_t pattern[16];
	for (unsigned i = 0; i < sizeof(pattern); i++) pattern[i] = (uint8_t)(0x35 + 17*i);
	struct cellular_loopback_state state = {.base=0x10960000, .expected=pattern};
	struct s5123_spi_ops ops = {&state,cellular_spi_read32,cellular_spi_read8,
	    cellular_spi_write32,cellular_spi_write8,cellular_spi_now};
	struct s5123_spi_result result = {0};
	int error = s5123_spi_send_boot(&ops, &result, pattern, sizeof(pattern), 1625000, 20000, 1);
	passed = !error && result.completed_bytes == sizeof(pattern) && state.received == sizeof(pattern) && !state.mismatches;
	print("cellular SPI loopback: error %s%d completed %d received %d mismatches %d status %x\n",
	    error < 0 ? "-" : "", (uint64_t)(error < 0 ? -error : error), (uint64_t)result.completed_bytes, (uint64_t)state.received,
	    (uint64_t)state.mismatches, (uint64_t)result.last_status);
restore:
	if (saved_controller) {
		cellular_reg_write(0x10960000 + S5123_CH, 0);
		/* Restore channel enable last, while pads still remain inputs. */
		for (unsigned i = 1; i < sizeof(spi_offsets)/sizeof(spi_offsets[0]); i++)
			cellular_reg_write(0x10960000 + spi_offsets[i], saved_spi[i]);
		cellular_reg_write(0x10960000 + S5123_CH, saved_spi[0]);
		int controller_restored = 1;
		for (unsigned i = 0; i < sizeof(spi_offsets)/sizeof(spi_offsets[0]); i++)
			if (cellular_reg_read(0x10960000 + spi_offsets[i]) != saved_spi[i]) controller_restored = 0;
		print("cellular SPI loopback: controller restored %d\n", (uint64_t)controller_restored);
		if (!controller_restored) passed = 0;
	}
	if (changed_mode) cellular_reg_write(0x10721048, mode);
	if (changed_clock) {
		cellular_reg_write(0x10701820, divider);
		if (!cellular_clock_wait(0x10701820, (1u<<16)|15, divider & 15)) passed = 0;
		cellular_reg_write(0x10700690, mux);
		if (!cellular_clock_wait(0x10700690, (1u<<16)|(1u<<4), mux & (1u<<4))) passed = 0;
	}
	int restored = cellular_reg_read(0x10721048) == mode && cellular_reg_read(0x10730060) == pins &&
	    (cellular_reg_read(0x10701820) & 15) == (divider & 15) &&
	    (cellular_reg_read(0x10700690) & (1u<<4)) == (mux & (1u<<4));
	print("cellular SPI loopback: controls restored %d; result %d\n", (uint64_t)restored, (uint64_t)(passed && restored));
	return passed && restored;
}
#endif
#ifdef CELLULAR_MMIO_PREFLIGHT
/* Add only known peripheral MMIO holes through DXE, then verify live Device
 * identity mappings before any register read. Added maps remain until reboot;
 * the host restores original BOOT/cold-resets after collecting the RAM report.
 * SPI is mapped/translated only: its controller is deliberately NOT read while
 * USI/clocks/pins remain unconfigured. CP power/reset are never written here.
 */
static int
cellular_mmio_preflight(void)
{
	static const struct { uint64_t base, size; } regions[] = {
		{0x10700000, 0x4000}, /* CMU PERIC1 */
		{0x10720000, 0x10000}, /* Existing SYSREG PERIC1 */
		{0x10730000, 0x1000}, /* GPIO PERIC1 */
		{0x15c30000, 0x1000}, /* GPIO CMGP */
		{0x10960000, 0x1000}, /* SPI16: mapping only, no controller access */
#ifdef CELLULAR_PCIE_MAP_PREFLIGHT
		{0x13c00000, 0x4000}, /* HSI2 clock controls; never write shared UFS clocks */
		{0x13c30000, 0x1000}, /* GPF3 CLKREQ/PERST inputs */
		{0x13c20000, 0x2000}, /* PCIe1 SYSREG: mapping only */
		{0x13d20000, 0x2000}, /* ELBI: mapping only; PHY remains isolated */
		{0x13d50000, 0x2000}, /* PHY: mapping only */
		{0x13800000, 0x301000}, /* DBI: mapping only */
		{0x13d40000, 0x1000}, /* PCS: mapping only */
		{0x14ffe000, 0x2000}, /* configuration window: mapping only */
		{0x13d00000, 0x1000}, /* IA: mapping only */
#endif
	};
	static const uint64_t clock_pin_regs[] = {
		0x10700690,0x10701820,0x107020fc,0x1070208c,0x10703070,
		0x10721048,0x10730060,0x10730064,0x10730068
	};
	static const uint64_t gpio_banks[] = {0x15c30120,0x15c30280,0x15c30320,0x15c30380,0x15c303c0,0x15c303e0};
	efi_dxe_services *dxe = cellular_dxe_services();
	if (!dxe || (READ_SYSREG(CurrentEL) >> 2) != 1) return 0;
	for (unsigned i = 0; i < sizeof(regions)/sizeof(regions[0]); i++) {
		uint64_t base = regions[i].base, size = regions[i].size;
		efi_gcd_memory_descriptor d;
		efi_status status = dxe->get_memory_space_descriptor(base, &d);
		if (EFI_ERROR(status) || d.base > base || d.length > ~0ULL-d.base || d.base+d.length < base+size) return 0;
		print("cellular MMIO: GCD %x size %x type %d attributes %x\n", base, size, (uint64_t)d.type, d.attributes);
		if (d.type == 0) {
			status = dxe->add_memory_space(3 /* EfiGcdMemoryTypeMemoryMappedIo */, base, size, 1 /* UC */);
			print("cellular MMIO: add %x status %x\n", base, status);
			if (EFI_ERROR(status)) return 0;
		} else if (d.type != 3 || !(d.capabilities & 1)) return 0;
		status = dxe->set_memory_space_attributes(base, size, (d.attributes & ~0x1fULL) | 1);
		print("cellular MMIO: Device %x status %x\n", base, status);
		if (EFI_ERROR(status)) return 0;
		uint64_t saved = READ_SYSREG(PAR_EL1);
		int okay = 1;
		for (uint64_t address = base; address < base+size; address += 4096) {
			__asm__ volatile ("at s1e1r, %0; isb" :: "r"(address) : "memory");
			if (!s20_par_device(READ_SYSREG(PAR_EL1), address)) { okay = 0; break; }
		}
		__asm__ volatile ("msr par_el1, %0; isb" :: "r"(saved) : "memory");
		print("cellular MMIO: live Device identity %x verified %d\n", base, (uint64_t)okay);
		if (!okay) return 0;
	}
	for (unsigned i = 0; i < sizeof(clock_pin_regs)/sizeof(clock_pin_regs[0]); i++) {
		uint64_t address = clock_pin_regs[i];
		print("cellular MMIO: read %x value %x\n", address, (uint64_t)*(volatile uint32_t *)(uintptr_t)address);
	}
	for (unsigned i = 0; i < sizeof(gpio_banks)/sizeof(gpio_banks[0]); i++) {
		volatile uint32_t *bank = (volatile uint32_t *)(uintptr_t)gpio_banks[i];
		print("cellular MMIO: GPIO %x CON %x DAT %x PUD %x\n", gpio_banks[i],
		    (uint64_t)bank[0], (uint64_t)bank[1], (uint64_t)bank[2]);
	}
	print("cellular MMIO: complete; SPI controller not read; CP unchanged\n");
#ifdef CELLULAR_PCIE_MAP_PREFLIGHT
	static const uint64_t pcie_controls[]={0x13c00600,0x13c00610,0x13c02018,0x13c0201c,
	    0x13c02020,0x13c02024,0x13c02028,0x13c0202c,0x13c02030,0x13c02034,
	    0x13c30000,0x13c30004,0x13c30008};
	for (unsigned i=0;i<sizeof(pcie_controls)/sizeof(pcie_controls[0]);i++)
		print("cellular PCIe map: control %x value %x\n",pcie_controls[i],
		    (uint64_t)*(volatile uint32_t *)(uintptr_t)pcie_controls[i]);
	print("cellular PCIe map: complete; translation only, no controller read or link/power request\n");
#ifdef CELLULAR_PCIE_ACCESS_PREFLIGHT
	/* Cold root-port access is not equivalent to Linux's already-initialized
	 * link-down path. Keep CP off and PERST asserted, temporarily bypass PHY
	 * isolation, then restore isolation/pin ownership before fallback. No
	 * shared HSI2/UFS clock, LTSSM, ATU, endpoint or DMA writes.
	 */
	volatile uint32_t *pmu=(volatile uint32_t *)0x15860720;
	volatile uint32_t *con=(volatile uint32_t *)0x13c30000;
	volatile uint32_t *dat=(volatile uint32_t *)0x13c30004;
	volatile uint32_t *input_clock=(volatile uint32_t *)0x13d500d8;
	uint32_t saved_pmu=*pmu,saved_con=*con,saved_dat=*dat,old_clock=0;
	uint32_t identity=0,command=0,link=0,ltssm=0;
	int clock_changed=0,clock_ok=0,restored=1;
	if ((saved_pmu&1) || (*(volatile uint32_t *)0x15c30284&1) ||
	    (*(volatile uint32_t *)0x15c30384&1) || (saved_con&0xff)) {
		print("cellular PCIe access: owner/power guard refused\n");return 0;
	}
	for (uint64_t a=0x13c02018;a<=0x13c02034;a+=4) {
		if (!(*(volatile uint32_t *)(uintptr_t)a & (1u<<21))) {
			print("cellular PCIe access: clock guard refused %x\n",a);return 0;
		}
	}
#define PCIE_CHECKPOINT(message) do { \
	print("cellular PCIe access: checkpoint " message "\n"); \
	compose_logs(NULL,0); \
	if (!store_logs(find_log_partition(),NULL,0)) goto pcie_access_restore; \
} while (0)
	PCIE_CHECKPOINT("before PERST ownership and isolation bypass");
	uint64_t tpl=BS->raise_tpl(31);
	*dat &= ~2u;__asm__ volatile ("dsb sy" ::: "memory");
	*con=(*con&~0xf0u)|0x10u;__asm__ volatile ("dsb sy" ::: "memory");
	BS->restore_tpl(tpl);
	if ((*con&0xf0u)!=0x10u || (*dat&2)) goto pcie_access_restore;
	*pmu |= 1;__asm__ volatile ("dsb sy" ::: "memory");
	if (!(*pmu&1) || EFI_ERROR(BS->stall(1000))) goto pcie_access_restore;
	PCIE_CHECKPOINT("isolation bypass verified; before ELBI LTSSM read 13d20054");
	ltssm=*(volatile uint32_t *)0x13d20054;
	print("cellular PCIe access: LTSSM enable %x\n",(uint64_t)ltssm);
	if (ltssm) goto pcie_access_restore;
	PCIE_CHECKPOINT("before PHY clock read 13d500d8");
	old_clock=*input_clock;
	print("cellular PCIe access: input clock before %x\n",(uint64_t)old_clock);
	PCIE_CHECKPOINT("before PHY clock write28");
	clock_changed=1;
	*input_clock=0x28;__asm__ volatile ("dsb sy" ::: "memory");
	clock_ok=*input_clock==0x28;
	if (clock_ok) {
		PCIE_CHECKPOINT("before ROOT identity read13800000");
		identity=*(volatile uint32_t *)0x13800000;
		print("cellular PCIe access: ROOT identity read %x\n",(uint64_t)identity);
		PCIE_CHECKPOINT("before ROOT command read13800004");
		command=*(volatile uint32_t *)0x13800004;
		PCIE_CHECKPOINT("before ELBI state read13d202c8");
		link=*(volatile uint32_t *)0x13d202c8;
		print("cellular PCIe access: ROOT id %x command/status %x link %x (not endpoint)\n",
		    (uint64_t)identity,(uint64_t)command,(uint64_t)link);
	}
pcie_access_restore:
	print("cellular PCIe access: restoring clock/isolation/PERST ownership\n");flush_log();
	if (clock_changed) {
		*input_clock=old_clock;__asm__ volatile ("dsb sy" ::: "memory");
		if (*input_clock!=old_clock) restored=0;
	}
	*pmu=(*pmu&~1u)|(saved_pmu&1);__asm__ volatile ("dsb sy" ::: "memory");
	if ((*pmu&1)!=(saved_pmu&1)) restored=0;
	/* CP stayed off throughout; restore only our PERST pin, no CLKREQ writes. */
	tpl=BS->raise_tpl(31);
	*con=(*con&~0xf0u)|(saved_con&0xf0u);__asm__ volatile ("dsb sy" ::: "memory");
	*dat=(*dat&~2u)|(saved_dat&2);__asm__ volatile ("dsb sy" ::: "memory");
	BS->restore_tpl(tpl);
	if (*con!=saved_con || (*dat&2)!=(saved_dat&2)) restored=0;
	int valid=clock_ok && identity && identity!=0xffffffffu && restored;
	print("cellular PCIe access: restored %d result %d; CP off, no link/DMA request\n",(uint64_t)restored,(uint64_t)valid);
	flush_log();
#undef PCIE_CHECKPOINT
	if (!valid) return 0;
#endif
#endif
	return 1;
}
#endif
#ifdef CELLULAR_CP_BOOT_PREFLIGHT
#include "../tools/cellular/s5123-uefi-boot.h"
#endif
/* Exercise the real allocation path without booting a kernel with new pools.
 * The original installed loader is loaded first and started after all probe
 * allocations have been released. Additional opt-in probes may exercise CP
 * controls/BOOT; those must confirm shutdown before releasing reservations.
 * A dedicated payload entry retains the report in FD RAM while the original
 * loader runs iOS; the host reads it through the existing read-only debug port.
 */
static efi_status
cellular_memory_preflight(void)
{
	struct payload_header *ph = (struct payload_header *)PAYLOAD_BASE;
	efi_handle fallback = NULL;
	uint8_t *report = NULL;
	uint64_t report_size = 0;
#ifdef CELLULAR_CP_BOOT_PREFLIGHT
	const uint8_t *cp_image=NULL,*cp_info=NULL;
	uint64_t cp_length=0,cp_info_length=0;
#endif
	efi_status status;
	if (ph->magic != PAYLOAD_MAGIC || ph->size > PAYLOAD_MAX_SIZE - sizeof(*ph)) return EFI_ERROR_BIT | 2;
	const uint8_t *p = (const uint8_t *)(ph + 1), *end = p + ph->size;
	while ((uint64_t)(end - p) >= sizeof(struct payload_entry)) {
		const struct payload_entry *entry = (const struct payload_entry *)p;
		if (!entry->name[0]) break;
		uint64_t padded = ((uint64_t)entry->size + 15) & ~15ULL;
		if (padded > (uint64_t)(end - p) - sizeof(*entry)) return EFI_ERROR_BIT | 2;
		if (memcmp(entry->name, "fallback.efi\0\0\0\0", 16) == 0) {
			status = BS->load_image(0, IMAGE, NULL, (void *)(p + sizeof(*entry)), entry->size, &fallback);
			print("cellular preflight: original loader LoadImage status %x\n", status);
			if (EFI_ERROR(status)) return status;
		} else if (memcmp(entry->name, "probe.report\0\0\0\0", 16) == 0) {
			report = (uint8_t *)(p + sizeof(*entry));
			report_size = entry->size;
#ifdef CELLULAR_CP_BOOT_PREFLIGHT
		} else if (memcmp(entry->name,"cp.boot\0\0\0\0\0\0\0\0\0",16)==0) {
			cp_image=p+sizeof(*entry);cp_length=entry->size;
		} else if (memcmp(entry->name,"cp.boot.info\0\0\0\0",16)==0) {
			cp_info=p+sizeof(*entry);cp_info_length=entry->size;
#endif
		}
		p += sizeof(*entry) + padded;
	}
	if (!fallback || !report || report_size < 8192) return EFI_ERROR_BIT | 14;
#ifdef CELLULAR_CP_BOOT_PREFLIGHT
	print("== CELLULAR CP BOOT PREFLIGHT (one guarded power/BOOT attempt) ==\n");
#else
	print("== CELLULAR MEMORY PREFLIGHT (no modem start) ==\n");
#endif
#ifdef CELLULAR_RESERVE_CP
	print("cellular reservation: probe result %d\n", (uint64_t)reserve_cellular_memory());
#endif
#ifdef CELLULAR_MMIO_PREFLIGHT
	int mmio_ready = cellular_mmio_preflight();
	print("cellular MMIO: preflight result %d\n", (uint64_t)mmio_ready);
#ifdef CELLULAR_SPI_LOOPBACK
	int spi_ready=mmio_ready && cellular_spi_loopback();
	print("cellular SPI loopback: preflight result %d\n", (uint64_t)spi_ready);
#endif
#ifdef CELLULAR_CP_GPIO_PREFLIGHT
	int gpio_ready=mmio_ready && cellular_cp_gpio_preflight();
	print("cellular GPIO: preflight result %d\n", (uint64_t)gpio_ready);
#endif
#ifdef CELLULAR_CP_BOOT_PREFLIGHT
	if (mmio_ready && spi_ready && gpio_ready) {
		int safe=cellular_cp_boot_preflight(cp_image,cp_length,cp_info,cp_info_length);
		while (!safe) {
			print("cellular CP BOOT: HOLD; cleanup unconfirmed; CP RAM retained. USB oem cp-stop then oem boot\n");
			flush_log();
			(void)log_access();
			safe=cellular_cp_retry_stop();
		}
	} else print("cellular CP BOOT: prerequisite failed; no power-on or BOOT attempt\n");
#endif
#endif
	print_memory_map();
	report_cellular_memory();
	int staging_ok = reserve_download_buffer();
	reserve_wifi_dma();
	reserve_display_pool();
	print("cellular preflight: staging %d wifi %x display %x\n", (uint64_t)staging_ok, wifi_dma_size, display_pool_size);
	if (display_pool_size) {
		status = BS->free_pages(display_pool_base, display_pool_size >> 12);
		print("cellular preflight: display free status %x\n", status);
	}
	if (wifi_dma_size) {
		status = BS->free_pages(wifi_dma_base, wifi_dma_size >> 12);
		print("cellular preflight: wifi free status %x\n", status);
	}
	if (download_base) {
		status = BS->free_pages(download_base, DOWNLOAD_MAX_SIZE >> 12);
		print("cellular preflight: staging free status %x\n", status);
	}
#ifdef CELLULAR_RESERVE_CP
	release_cellular_memory();
	print("cellular reservation: after undo\n");
	report_cellular_memory();
#endif
	print("cellular preflight: handing off to the original loader\n");
	uint32_t report_len = log_len < report_size - 8 ? log_len : (uint32_t)(report_size - 8);
	memset(report, 0, (size_t)report_size);
	memcpy(report + 8, log_buf, report_len);
	/* Refuse to present a silently truncated probe as complete evidence. */
	((uint32_t *)report)[1] = report_len | (log_len > report_size - 8 ? 0x80000000u : 0);
	((uint32_t *)report)[0] = 0x304d5043; /* CPM0 */
	__asm__ volatile ("dsb sy" ::: "memory");
	return BS->start_image(fallback, NULL, NULL);
}
#endif

efi_status EFIAPI
efi_main(efi_handle image, efi_system_table *st)
{
	static const efi_guid gop_guid = EFI_GOP_GUID;
	struct payload_info pi;

	ST = st;
	BS = st->bs;
	IMAGE = image;

	BS->set_watchdog_timer(0, 0, 0, NULL);
	screen_quiet = 1;               /* (the boot screen: no text; bootui=0 brings it back once the payload is read) */

	print("== XNULOADER v3 ==\n");
	print("CurrentEL=%x MIDR=%x MMFR0=%x CNTFRQ=%d image=%x\n", READ_SYSREG(CurrentEL) >> 2, READ_SYSREG(MIDR_EL1),
	    READ_SYSREG(ID_AA64MMFR0_EL1), timer_freq(), (uint64_t)(uintptr_t)&__ImageBase);

	PLAT = &platform_qemu_virt;
	if (EFI_ERROR(BS->locate_protocol(&gop_guid, NULL, (void **)&GOP)) || GOP->mode == NULL ||
	    GOP->mode->info == NULL || GOP->mode->fb_base == 0) {
		GOP = NULL;                     /* no display (QEMU without -device ramfb) */
	} else {
		SCREEN.base = GOP->mode->fb_base;
		SCREEN.width = GOP->mode->info->width;
		SCREEN.height = GOP->mode->info->height;
		SCREEN.stride = GOP->mode->info->pixels_per_scanline * 4;
		for (unsigned i = 0; i < sizeof(platforms) / sizeof(platforms[0]); i++)
			if (platforms[i]->fb_base == SCREEN.base)
				PLAT = platforms[i];
		if (PLAT == &platform_qemu_virt)
			ramfb_phone_screen();
		print("display %d x %d at %x\n", (uint64_t)SCREEN.width, (uint64_t)SCREEN.height, SCREEN.base);
	}
	print("platform %s\n", PLAT->name);
	if (PLAT == &platform_y2s && SCREEN.base != 0) {
		boot_logo();
	}
#ifdef CELLULAR_PREFLIGHT_ONLY
	return cellular_memory_preflight();
#endif
#ifdef CELLULAR_RESERVE_CP
	if (PLAT == &platform_y2s) {
		if (reserve_cellular_memory()) {
			print("cellular reservation: retained for iOS; modem stays off\n");
		} else {
			print("cellular reservation: failed; restoring original ownership; modem unavailable\n");
			release_cellular_memory();
		}
	}
#endif
	if (PLAT == &platform_y2s) {
		y2s_calliope_stop();
	}

	install_exception_hook();
	wdt_arm();
	if (!reserve_download_buffer()) {
		print("download buffer: no safe staging memory; loader holds without booting\n");
		for (;;) { BS->stall(1000000); wdt_pet(); }
	}
	report_cellular_memory();

	int have_payload = parse_payload(&pi, 0);
	for (const char *c = have_payload && pi.cmdline ? pi.cmdline : ""; *c; c++) {
		if (starts_with(c, "bootui=0") && (c == pi.cmdline || c[-1] == ' ')) {
			bootui = 0;
			screen_quiet = 0;
		}
	}
	if (have_payload && pi.devmode) {
		bootui = 0;             /* dev mode (x1slte.sh DEVMODE=1): the HackiOS menu with its countdown, and the text */
		screen_quiet = 0;
	}
	if (PLAT != &platform_y2s) {
		screen_quiet = 0;       /* (QEMU and the rest: as before) */
	}
	if (have_payload && pi.usb_mode && usb_start()) {
		flush_log();
		usb_command_loop();
	}

	/* y2s: the boot menu (Boot iOS by itself after 3 s, Log access over USB, Download Mode) */
	if (have_payload && PLAT == &platform_y2s && SCREEN.base != 0 && !pi.usb_mode) {
		y2s_menu_loop();
	}
	/* One-shot mode: run the kernel that came with the image, then reset. */
	if (have_payload) {
		run_kernel(0, 0);
	}
	/* Keep the result on screen: a reboot loop would hide the panic. */
	print("\nloader: done, holding. Hold Power+VolDown to reset.\n");
	for (;;) {
		BS->stall(1000000);
	}
}
