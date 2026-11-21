/*
 * redwood_exynos_acpm.c: the clocks of the Exynos 990's processors, GPU and memory, set through
 * Samsung's ACPM (the power-management core that owns the PLLs and the regulators), and the
 * temperatures it reads -- PLANS/01-performance.md P5, PLANS/04-gpu.md G8.
 *
 * Until this, every clock was what S-Boot left: the A55s at ~1.05 GHz, the A76s at ~1.07 GHz, the
 * Mongoose M5s at 266 MHz (so slow they are left out: cpumask=0x3f), the GPU at its boot clock.
 * No register of a PLL is touched here; ACPM does that, voltage first, on request.
 *
 * The protocol is Samsung's (android_kernel_samsung_exynos990: drivers/soc/samsung/acpm/acpm_ipc.c,
 * drivers/soc/samsung/cal-if/acpm_dvfs.c, drivers/thermal/samsung/exynos_acpm_tmu.c), and every
 * number in this file comes from the phone's own device tree (research/dts/dtb.img.0.dts):
 *   acpm_ipc   mailbox 0x15900000, SRAM 0x02039000 (0x38000), initdata-base 0x7000
 *   acpm_dvfs  channel 5: { domain, kHz, FREQ_REQ 0 | FREQ_GET 1, 0 }
 *   acpm_tmu   channel 9: READ_TEMP 0x02 for zone BIG 0, MID 1, LITTLE 2, G3D 3
 *   domains    MIF 0, INT 1, CL0 2 (cpus 0-3, A55), CL1 3 (4-5, A76), CL2 4 (6-7, M5), DISP 6, G3D 10
 * A channel's queues live in SRAM: the framework at initdata-base names an array of channel
 * descriptors; the AP writes a command into the descriptor's rx queue, bumps its front, rings
 * INTGR0 (bit 16 + id), and the answer appears in the tx queue with the same sequence number.
 *
 * Boot args:
 *   dvfs=1       read: every domain's clock and the temperatures, then every 10 s
 *   dvfs=2       ... and set the clocks below (kHz; 0 leaves a domain as it is):
 *                dvfs_cl0 (A55, default 2002000), dvfs_cl1 (A76, 2504000), dvfs_cl2 (M5, 2730000),
 *                dvfs_g3d (GPU, 800000), dvfs_mif (memory, 2028000)
 *   dvfs_efficiency=1  cap CPU/GPU/memory clocks; runtime: kern.redwood.efficiency=0|1
 *   dvfs_wait=S  seconds after boot before anything is asked (default 10)
 *   dvfs_hot=C   the first step of each domain's throttling, in degrees of its own zone (default:
 *                78 / 86 for M5, 82 / 90 for A55/A76/GPU, using each domain's own zone); a step is let go
 *                8 degrees below where it was taken. An override sets the second step to C + 8.
 *                Step 1 is the "mid" clock, step 2 the safe one. Only the domain that is hot slows.
 *   fb_hot=C     the panel held under 70 % of its brightness from C degrees in the hottest zone (default
 *                82; 0: never), under 50 % from C + 6; the 50 % let go below C + 2, the 70 % below C - 4.
 *                The panel's power is heat in the same body as the SoC (a phone's thermal policy does the
 *                same; Samsung's is in user space, SSRM, so these marks are this driver's own).
 * Every line goes to the loader's log with a `dvfs:` prefix.
 */

#include <os/atomic_private.h>
#include <sys/errno.h>
#include <kern/clock.h>
#include <kern/sched_prim.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <pexpert/pexpert.h>
#include <stdarg.h>
#include <sys/systm.h>
#include <sys/sysctl.h>
#include <sys/redwood_m2m.h>
#include <mach/processor_info.h>
#include <mach/machine.h>
#include <sys/redwood_dbgport.h>
#include <sys/redwood_power_policy.h>
#include <sys/redwood_gpu_policy.h>

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern void loader_log_line(const char *line);
extern unsigned int real_ncpus;
extern processor_t processor_array[];
extern kern_return_t processor_info(processor_t processor, processor_flavor_t flavor, host_t *host,
    processor_info_t info, mach_msg_type_number_t *count);
extern uint32_t processor_avail_count;

#define ACPM_INTR_PHYS          0x15900000UL
#define ACPM_SRAM_PHYS          0x02039000UL
#define ACPM_SRAM_SIZE          0x38000
#define ACPM_INITDATA           0x7000

#define INTGR0                  0x08            /* AP -> APM: bit 16 + channel id */
#define INTCR1                  0x20            /* APM -> AP: clear */
#define INTMR1                  0x24            /*            mask (1 = no interrupt: we poll) */
#define INTSR1                  0x28            /*            status */

#define CH_DVFS                 5
#define CH_TMU                  9

#define FREQ_REQ                0
#define FREQ_GET                1
#define TMU_IPC_INIT            0x01
#define TMU_IPC_READ_TEMP       0x02

#define TYPE_BUFFER             2               /* framework.h: one slot, no ring (TYPE_QUEUE 1) */

#define CMU_TOP_PHYS            0x1a330000UL
#define PLL_CON3_PLL_G3D        0x10c           /* ENABLE 31, STABLE 29, M 25:16, P 13:8, S 2:0; 26 MHz in */

#define SEQ_SHIFT               16
#define SEQ_MASK                (0x3fu << SEQ_SHIFT)

enum { D_MIF = 0, D_INT = 1, D_CL0 = 2, D_CL1 = 3, D_CL2 = 4, D_DISP = 6, D_G3D = 10 };

static const struct {
	unsigned domain;
	const char *name;
	const char *arg;
	uint32_t target;        /* kHz, dvfs=2's default */
	uint32_t safe;          /* kHz, when too hot */
	uint32_t mid;           /* kHz, the first throttling step */
	int zone;               /* the zone that throttles it (zones[]), -1: none */
	int step1, step2;       /* degrees */
} domains[] = {
	/* x1slte (2026-11-06): the tops are Samsung's (dtb cpufreq domains: A55 2002, A76 2504 as sold, M5 2730; gpu_max_clock
	 * 800 MHz); the thermal steps below cap them as Samsung's thermal zones do */
	/*
	 * (B, 2026-11-07: 86/96 read once a second let the big cluster reach 108-111 C during boot and the SoC's thermal
	 * trip reset the phone through the PMIC -- RST_STAT PINRESET, PWRONSRC WTSR -- 2-3 times after a cold start. Now
	 * 85/95 read every 250 ms and let go 10 degrees below the mark; 80/90 throttled plain UI use, 18:50.)
	 * Cluster-aware policy: M5 steps at 78/86 C, others at 82/90 C; 8 C release hysteresis.
	 * lpmd engages earlier at 76 C M5, 80 C A76/GPU, 82 C A55.
	 */
	{ D_CL0, "cl0 (A55)", "dvfs_cl0", 2002000, 1053000, 1456000, 2, 82, 90 },
	/* cl1's second step 1482, not 1066 (B, 2026-11-14): above 90 C the A76 fell from 1794 to 1066 at once (log: "cl1 zone
	 * mid 90 C: step 2, 1066 MHz") and frame preparation with it -- the same cliff as the GPU's 572 -> 377 */
	{ D_CL1, "cl1 (A76)", "dvfs_cl1", 2504000, 1482000, 1794000, 1, 82, 90 },
	{ D_CL2, "cl2 (M5)",  "dvfs_cl2", 2730000, 1196000, 1586000, 0, 78, 86 },  /* only with every core up (checked below); dvfs_cl2=0 leaves it */
	/* g3d's second step 455, not 377 (B, 2026-11-14): one step of the GPU's own table down from 572, not two -- above
	 * 90 C swipes went from 572 straight to 377 and the frames past 35 ms doubled (fb histograms); the trip is ~108 C */
	{ D_G3D, "g3d",       "dvfs_g3d", 800000,  455000,  572000,  3, 82, 90 },
	{ D_MIF, "mif",       "dvfs_mif", 2028000, 0,       0,       -1, 0, 0 },  /* (2730 = LPDDR5 top: heat for no UI gain, 18:50); dvfs_mif=0 leaves it */
	{ D_INT, "int",       NULL,       0,       0,       0,       -1, 0, 0 },
	{ D_DISP, "disp",     NULL,       0,       0,       0,       -1, 0, 0 },
};
#define NDOMAINS        (sizeof(domains) / sizeof(domains[0]))

static const char *const zones[] = { "big", "mid", "little", "g3d" };

/* one channel, as acpm_ipc.c's channel_init() reads it out of SRAM */
struct channel {
	uint32_t id, polling, elem, len, type;
	volatile uint8_t *tx_front, *tx_rear, *tx_base;        /* AP -> APM (the descriptor's "rx") */
	volatile uint8_t *rx_front, *rx_rear, *rx_base;        /* APM -> AP (the descriptor's "tx") */
	uint32_t seq;
};

static volatile uint8_t *intr, *sram, *cmu_top;
static struct channel ch_dvfs, ch_tmu;
static uint32_t targets[NDOMAINS];

uint32_t redwood_dvfs_log = 0;          /* kern.redwood.dvfs_log: every clock change in the log */
static void dlog(const char *format, ...) __printflike(1, 2);

static void
dlog(const char *format, ...)
{
	char line[320];
	va_list list;
	size_t len;

	strlcpy(line, "dvfs: ", sizeof(line));
	va_start(list, format);
	vsnprintf(line + 6, sizeof(line) - 7, format, list);
	va_end(list);
	/* into mali2metal's channel as well, but not each clock request (the governor's, several a second) */
	if (strncmp(line + 6, "domain ", 7) != 0) {
		m2m_klog('I', "PERF", "%s", line);
	}
	len = strlen(line);
	line[len] = '\n';
	line[len + 1] = '\0';
	loader_log_line(line);
}

static inline uint32_t
rd(volatile uint8_t *at)
{
	return *(volatile uint32_t *)at;
}

static inline void
wr(volatile uint8_t *at, uint32_t value)
{
	*(volatile uint32_t *)at = value;
	__asm__ volatile ("dsb sy" ::: "memory");
}

/*
 * The descriptor of the channel whose id is `want` (acpm_ipc_request_channel looks channels up by
 * id, not by place). struct ipc_channel { id, field, owner, type, channel_info ch, ap_poll } with
 * channel_info { rx_rear, rx_front, rx_base, rx_indr_buf, rx_indr_buf_base, rx_indr_buf_size,
 * tx_rear, tx_front, tx_base, q_len, q_elem_size, credit, mbox_addr }: 18 words.
 */
static int
find_channel(uint32_t want, struct channel *c)
{
	uint32_t channels = rd(sram + ACPM_INITDATA + 8);
	uint32_t count = rd(sram + ACPM_INITDATA + 24);         /* ipc_ap_max */

	if (channels == 0 || channels >= ACPM_SRAM_SIZE || count == 0 || count > 32) {
		dlog("the framework at SRAM+%#x names %u channels at %#x: not what was expected",
		    ACPM_INITDATA, count, channels);
		return -1;
	}
	/*
	 * Linux finds the channel whose id is `want` and then sends through acpm_ipc->channel[id], the
	 * descriptor at POSITION `want` (acpm_ipc_request_channel returns the id; __acpm_ipc_send_data
	 * indexes the array with it). Looking the descriptor up by its id instead found channel 5 with a
	 * queue of one entry, which no ring can use (25 October). So: the descriptor at that position.
	 */
	for (uint32_t i = 0; i < count; i++) {
		volatile uint8_t *d = sram + channels + i * 72;

		if (want == CH_DVFS) {
			dlog("descriptor %u: id %u field %u owner %d type %u, queue %u x %u, polled %u, to ACPM %#x/%#x/%#x, "
			    "from ACPM %#x/%#x/%#x", i, rd(d), rd(d + 4), (int)rd(d + 8), rd(d + 12), rd(d + 52), rd(d + 56),
			    rd(d + 68), rd(d + 16), rd(d + 20), rd(d + 24), rd(d + 40), rd(d + 44), rd(d + 48));
		}
	}
	for (uint32_t i = want; i < count; i = count) {
		volatile uint8_t *d = sram + channels + i * 72;
		uint32_t rx_rear = rd(d + 16), rx_front = rd(d + 20), rx_base = rd(d + 24);
		uint32_t tx_rear = rd(d + 40), tx_front = rd(d + 44), tx_base = rd(d + 48);

		c->id = rd(d);
		c->len = rd(d + 52);
		c->elem = rd(d + 56);
		c->polling = rd(d + 68);
		c->type = rd(d + 12);
		if (c->len == 0 || c->len > 64 || c->elem < 16 || c->elem > 64 ||
		    rx_base >= ACPM_SRAM_SIZE || tx_base >= ACPM_SRAM_SIZE ||
		    rx_front >= ACPM_SRAM_SIZE || tx_front >= ACPM_SRAM_SIZE) {
			dlog("channel %u: queue of %u x %u bytes, offsets rx %#x/%#x/%#x tx %#x/%#x/%#x: "
			    "out of range", want, c->len, c->elem, rx_rear, rx_front, rx_base, tx_rear,
			    tx_front, tx_base);
			return -1;
		}
		c->tx_rear = sram + rx_rear;
		c->tx_front = sram + rx_front;
		c->tx_base = sram + rx_base;
		c->rx_rear = sram + tx_rear;
		c->rx_front = sram + tx_front;
		c->rx_base = sram + tx_base;
		c->seq = 0;
		dlog("channel %u (descriptor %u, id %u): %s, %u x %u bytes, %s; to ACPM front %u rear %u, from ACPM front %u rear %u",
		    want, i, c->id, c->type == TYPE_BUFFER ? "buffer" : "queue", c->len, c->elem,
		    c->polling ? "polled" : "interrupt-driven", rd(c->tx_front),
		    rd(c->tx_rear), rd(c->rx_front), rd(c->rx_rear));
		return 0;
	}
	dlog("no descriptor %u among the %u the framework names", want, count);
	return -1;
}

/* One command and its answer (acpm_ipc.c __acpm_ipc_send_data + check_response). 0, or -1. */
static int
ipc(struct channel *c, uint32_t cmd[4])
{
	uint32_t front = rd(c->tx_front), next = front + 1 == c->len ? 0 : front + 1;
	uint32_t seq, i;
	uint64_t deadline;

	/*
	 * A buffer channel (DVFS, 5) has one slot and no ring: front and rear stay 0, so the ring's
	 * "full" test is always true for it. acpm_ipc.c applies that test anyway, but its waits time
	 * out on our side of the firmware's start-up. Linux does not wait for an answer on it either
	 * (not polled: the answer is an interrupt and a callback): the command is written to slot 0,
	 * the doorbell rung, and the answer taken if one comes in 20 ms (1: sent, no answer).
	 */
	if (c->type == TYPE_BUFFER) {
		front = 0;
		next = 0;
	} else if (next == rd(c->tx_rear)) {
		dlog("channel %u: the queue to ACPM is full (front %u rear %u)", c->id, front, rd(c->tx_rear));
		return -1;
	}
	seq = c->seq = c->seq >= 63 ? 1 : c->seq + 1;
	cmd[0] = (cmd[0] & ~SEQ_MASK) | (seq << SEQ_SHIFT);
	for (i = 0; i < c->elem / 4; i++) {
		wr(c->tx_base + c->elem * front + i * 4, i < 4 ? cmd[i] : 0);
	}
	wr(c->tx_front, next);
	wr(intr + INTGR0, (1u << c->id) << 16);

	clock_interval_to_deadline(20, NSEC_PER_MSEC, &deadline);
	while (mach_absolute_time() < deadline) {
		if ((rd(intr + INTSR1) & (1u << c->id)) && c->type == TYPE_BUFFER) {
			for (uint32_t w = 0; w < 4; w++) {
				cmd[w] = rd(c->rx_base + w * 4);
			}
			wr(intr + INTCR1, 1u << c->id);
			return 0;
		}
		if (rd(intr + INTSR1) & (1u << c->id)) {
			uint32_t rear = rd(c->rx_rear), last = rd(c->rx_front);

			for (i = rear; i != last; i = i + 1 == c->len ? 0 : i + 1) {
				volatile uint8_t *e = c->rx_base + c->elem * i;

				if (((rd(e) & SEQ_MASK) >> SEQ_SHIFT) != seq) {
					continue;
				}
				for (uint32_t w = 0; w < 4; w++) {
					cmd[w] = rd(e + w * 4);
				}
				if (i != rear) {        /* keep the queue dense: the rear's entry takes this place */
					for (uint32_t w = 0; w < c->elem / 4; w++) {
						wr(e + w * 4, rd(c->rx_base + c->elem * rear + w * 4));
					}
				}
				rear = rear + 1 == c->len ? 0 : rear + 1;
				wr(c->rx_rear, rear);
				if (rear == rd(c->rx_front)) {
					wr(intr + INTCR1, 1u << c->id);
				}
				return 0;
			}
		}
		delay(10);
	}
	if (c->type == TYPE_BUFFER) {
		return 1;
	}
	dlog("channel %u: no answer to %#x %#x %#x %#x in 20 ms (status %#x, from ACPM front %u rear %u)",
	    c->id, cmd[0], cmd[1], cmd[2], cmd[3], rd(intr + INTSR1), rd(c->rx_front), rd(c->rx_rear));
	return -1;
}

static int set_rate(unsigned domain, uint32_t khz);
/*
 * The debug port's `acpm.rate DOMAIN KHZ' (A, 2026-11-07): any ACPM DVFS domain (MIF 0, INT 1, CL0 2, CL1 3, CL2 4,
 * NPU 5, DISP 6, DSP 7, AUD 8, CP 9, G3D 10, ...; acpm_dvfs_exynos9830.h), carried out by the ACPM thread on its next
 * tick so it never races the governor's own requests.
 */
static volatile uint32_t dbg_domain = ~0u, dbg_khz, dbg_result = ~0u;
extern int redwood_gov_wake;

static void
dbg_acpm_rate(const uint64_t *v, unsigned n, char *out, size_t cap)
{
	if (n < 2) {
		snprintf(out, cap, "acpm.rate DOMAIN KHZ (AUD 8, INT 1, MIF 0); last result %d\n", (int)dbg_result);
		return;
	}
	dbg_result = ~0u;
	dbg_khz = (uint32_t)v[1];
	dbg_domain = (uint32_t)v[0];
	thread_wakeup((event_t)&redwood_gov_wake);
	for (int i = 0; i < 40 && dbg_result == ~0u; i++) {
		delay_for_interval(10, NSEC_PER_MSEC);
	}
	snprintf(out, cap, "domain %llu to %llu kHz: %s\n", v[0], v[1], dbg_result == 0 ? "sent and answered" :
	    dbg_result == ~0u ? "not done yet (the ACPM thread did not run)" : "FAILED");
}

static int
set_rate(unsigned domain, uint32_t khz)
{
	uint32_t cmd[4] = { domain, khz, FREQ_REQ, 0 };
	int ret = ipc(&ch_dvfs, cmd);

	/* every clock change was a log line (~200 in a few minutes of use, B 2026-11-14): only failures now, all of them
	 * with kern.redwood.dvfs_log=1 */
	if (ret != 0 || redwood_dvfs_log) {
		dlog("domain %u to %u kHz: %s (%#x %#x %#x %#x)", domain, khz, ret == 0 ? "answered" : ret == 1 ? "sent, no answer" :
		    "NOT sent", cmd[0], cmd[1], cmd[2], cmd[3]);
	}
	return ret < 0 ? -1 : 0;
}

/* what PLL_G3D really runs at: the proof that a request was taken */
static uint32_t
g3d_pll_mhz(void)
{
	uint32_t con3 = cmu_top ? rd(cmu_top + PLL_CON3_PLL_G3D) : 0, m = (con3 >> 16) & 0x3ff, p = (con3 >> 8) & 0x3f;

	return p ? 26 * m / (p << (con3 & 7)) : 0;
}

/* degrees C, or -1 (exynos_acpm_tmu.c: data[1] = type | tzid << 16, answer's temp in byte 3) */
static int
read_temp(unsigned zone)
{
	uint32_t cmd[4] = { 0, TMU_IPC_READ_TEMP | (zone << 16), 0, 0 };

	if (ipc(&ch_tmu, cmd) != 0 || (int8_t)(cmd[1] >> 8) != 0) {
		return -1;
	}
	return (int)((cmd[1] >> 24) & 0xff);
}

static void
report(const char *why)
{
	char line[300];
	size_t at = 0;

	/* (ACPM does not answer FREQ_GET as acpm_dvfs.c asks it: every domain read 0, run 20) */
	at += snprintf(line + at, sizeof(line) - at, "PLL_G3D %u MHz", g3d_pll_mhz());
	if (ch_tmu.len != 0) {
		at += snprintf(line + at, sizeof(line) - at, "; temperatures");
		for (unsigned z = 0; z < sizeof(zones) / sizeof(zones[0]) && at < sizeof(line) - 16; z++) {
			at += snprintf(line + at, sizeof(line) - at, " %s %d", zones[z], read_temp(z));
		}
	}
	dlog("%s: %s", why, line);
}

/* Cache only accepted requests. A failed cap must be retried, not remembered
 * as applied. Return 1 changed, 0 already requested, -1 failed. */
static int
governor_set_rate(unsigned domain, uint32_t khz, uint32_t *sent)
{
	if (*sent == khz) {
		return 0;
	}
	if (set_rate(domain, khz) != 0) {
		return -1;
	}
	*sent = khz;
	return 1;
}

static int power_profile(void);
static uint32_t profile_cap(unsigned d, uint32_t cap, int prof);

static void
apply(int hot, bool skip_g3d)
{
	for (unsigned i = 0; i < NDOMAINS; i++) {
		uint32_t khz = targets[i] ? profile_cap(i, hot ? domains[i].safe : targets[i], power_profile()) : 0;

		/* (cluster 2 is asked on its own, last: acpm_thread) */
		if (khz == 0 || (skip_g3d && domains[i].domain == D_G3D) || domains[i].domain == D_CL2) {
			continue;
		}
		if (set_rate(domains[i].domain, khz) != 0) {
			dlog("%s: %u kHz was not taken", domains[i].name, khz);
		}
		if (domains[i].domain == D_G3D && cmu_top != NULL) {
			delay(2000);
			dlog("g3d asked for %u MHz: PLL_G3D now %u MHz", khz / 1000, g3d_pll_mhz());
		}
	}
}

static int
hottest(void)
{
	int worst = -1;

	for (unsigned z = 0; ch_tmu.len != 0 && z < sizeof(zones) / sizeof(zones[0]); z++) {
		int t = read_temp(z);

		worst = t > worst ? t : worst;
	}
	return worst;
}

/*
 * dvfs_gov=1 (default with dvfs=2): the CPU clusters' clocks follow their load, as a phone's do. Pinned at
 * their tops they ran 66-75 C with the GPU idle and hit 105 C (the M5s) at boot. Every 200 ms: each
 * cluster's busiest core; over 70 % busy the top rung at once (a touch must not wait for the clock),
 * otherwise the rung that would make it ~60 % busy, coming down one rung at a time after 0.6 s. The
 * thermal steps cap it.
 *
 * The GPU (dvfs_gov_gpu=1, the default) the same way from its own busy time (redwood_mali_busy_pct), over
 * Samsung's table (dtb gpu_dvfs_table: 156 to 702 MHz here; 800 and 832 are not used), but aiming at ~40 %
 * busy and jumping to the top at 60 %: the window server waits for the GPU in every swap, so a GPU kept
 * busier is a longer frame. It ran at 702 MHz 90-96 % idle (runs 1-31), the SoC at 81 C idle.
 *
 * A touch: Samsung's input booster for this phone (dts input_booster, booster_key@3 "touch": head_time
 * 130 ms, tail_time 500 ms) holds each cluster at least at its head clock for 130 ms after a finger comes
 * down and at its tail clock until 500 ms after the last touch event; here each as the lowest rung at or
 * above it, applied the moment the finger lands (the governor is woken) rather than at the next tick.
 * The booster's MIF (memory, 1014 MHz) floor is not applied: the memory's clock is not driven here.
 * The booster sets no GPU floor; this one holds the GPU at 572 MHz for a touch: measured cold (27 October)
 * a heavy frame's fragment jobs ran 17-23 ms at 156-260 MHz, and 572 is the lowest of Samsung's rungs that
 * would bring them near 8 ms by the clock alone (an estimate: the memory's speed may cap it).
 *
 * A GPU job longer than half a refresh (redwood_mali.c: redwood_mali_slow_jobs, which wakes this thread)
 * takes the GPU to its top rung at once; it comes down as from any other jump.
 */
int redwood_gov_wake;                   /* the event a touch or a slow GPU job wakes the governor with */

/*
 * Power profiles (B, 2026-11-07): caps on top of the governor and the thermal steps.
 *   screen off (MiniIOMobileFramebuffer's display off): every cluster at its thermal "safe" clock, the GPU at its
 *     safe clock (377 MHz), memory at 421 MHz (measured live: 2.7 GB/s there vs 15.7 at 2730; ACPM takes every step);
 *   Low Power Mode (kern.redwood.lowpower=1, from iOS's switch): clusters at their first thermal step, memory 1352 MHz;
 *   normal: memory 2028 MHz.
 * dvfs_sleep=0 turns the screen-off profile off; dvfs_lpm=0 ignores Low Power Mode.
 */
static volatile int redwood_screen_on = 1;
volatile int redwood_display_is_on = 1;      /* the same, for other drivers (redwood_ssp.c: its polling) */
int redwood_lowpower;                   /* kern.redwood.lowpower (redwood_battery.c's node) */
static uint32_t prof_sleep = 1, prof_lpm = 1;
static int efficiency;                 /* manual whole-device profile */
static int efficiency_auto = 1;        /* independent automatic caps, on by default */
static unsigned efficiency_mask;       /* updated by the existing ACPM thermal loop */

/* Existing Samsung clock levels, kHz; a ceiling, never a request to raise a clock. */
static uint32_t
efficiency_cap(unsigned domain)
{
	switch (domain) {
	case D_CL0: return 1456000;
	case D_CL1: return 2106000;
	case D_CL2: return 1794000;
	case D_G3D: return 572000;
	case D_MIF: return 1352000;
	default: return 0;
	}
}
static int
efficiency_active(unsigned domain)
{
	unsigned bit;

	if (os_atomic_load(&efficiency, relaxed)) {
		return 1;
	}
	if (!os_atomic_load(&efficiency_auto, relaxed)) {
		return 0;
	}
	switch (domain) {
	case D_CL0: bit = 0; break;
	case D_CL1: bit = 1; break;
	case D_CL2: bit = 2; break;
	case D_G3D: bit = 3; break;
	case D_MIF: return 0; /* No memory-specific automatic telemetry. */
	default: return 0;
	}
	return (os_atomic_load(&efficiency_mask, relaxed) & (1u << bit)) != 0;
}
enum { PROF_NORMAL = 0, PROF_LPM = 1, PROF_OFF = 2 };

/*
 * Screen off: the small cores only (A, 2026-11-15). With small_cores=2 the A55s are used only when every big core is
 * busy -- right for the UI, backwards with the screen off: every daemon's and driver's wake-up landed on an M5/A76
 * (each woken ~250 times a second in AOD, measured with wake_stats) while the A55s slept. iPhones do the opposite
 * (background work on the E-cores, the P cluster off). So with the screen off the scheduler is given cores 0-3 only
 * (its recommended cores, as Apple's CLPC does); all 8 come back the moment the display does, before the wake boost.
 * Not while sound plays (music, a call's voice: redwood_audio.c) or for 10 s after. Boot-arg scroff_little=0: off;
 * live: kern.redwood.scroff_little.
 */
extern void sched_perfcontrol_update_recommended_cores(uint32_t recommended_cores);
extern uint64_t redwood_audio_active_at;
static uint32_t scroff_little = 1;
static int scroff_little_on;            /* the scheduler has cores 0-3 only now */
static uint32_t scroff_little_switches;
LCK_GRP_DECLARE(scroff_lck_grp, "redwood-scroff");
LCK_MTX_DECLARE(scroff_lock, &scroff_lck_grp);

/*
 * The big cores off when hot (A, 2026-11-20): Samsung hotplugs cpu6-7 out at 96 C (BIG zone) and back in at 91 C
 * (exynos9830 DTS, tmu BIG hotplug_out/in_threshold; docs/devices/x1slte/power-thermal-samsung.md). Here they are
 * taken from the scheduler's recommended cores, as the screen-off mask does; the phone used to climb to 105-110 C and
 * be reset by the PMIC. Boot-arg dvfs_bigoff=0: off; dvfs_bigoff_out=C / dvfs_bigoff_in=C move the marks.
 */
static uint32_t bigoff = 1, bigoff_out = 96, bigoff_in = 91;
static int bigoff_on;                   /* cpu6-7 out of the scheduler now */
static uint32_t bigoff_switches;
/*
 * Big cores parked while the A76s cope (A, 2026-11-20): Samsung's scheduler weights the M5s 20-25 against the A76s'
 * 200 and moves a task up only above ~57 % of a core (EMS ontime), so in UI use they mostly sleep; XNU here spreads
 * work over all eight. kern.redwood.bigpark=1: cpu6-7 out of the recommended cores until the A76s are at least
 * bigpark_up % busy for 3 ticks (150 ms); back out after 2 s with the A76s under bigpark_down % and the M5s idle.
 */
static uint32_t bigpark = 1, bigpark_up = 80, bigpark_down = 40;
static int bigpark_on;
static uint32_t bigpark_switches;

static void
cores_apply(void)                       /* scroff_lock held */
{
	uint32_t m = scroff_little_on ? 0x0fu : 0xffu;

	if (bigoff_on || bigpark_on) {
		m &= ~0xc0u;
	}
	sched_perfcontrol_update_recommended_cores(m == 0xffu ? 0xffffffffu : m);
}

static void
scroff_little_set(int on)               /* scroff_lock held */
{
	if (on == scroff_little_on) {
		return;
	}
	scroff_little_on = on;
	scroff_little_switches++;
	cores_apply();
}

static void
bigpark_set_default(uint32_t on)
{
	bigpark = on;
}

/* every tick (screen on), with the A76s' (gov[1]) and the M5s' (gov[2]) last busy % */
static void
bigpark_check(int a76, int m5)
{
	static unsigned up, down;
	int want = bigpark_on;

	if (!bigpark || !redwood_screen_on) {
		want = 0;
		up = down = 0;
	} else if (bigpark_on) {
		up = a76 >= (int)bigpark_up ? up + 1 : 0;
		if (up >= 3) {
			want = 0;
			up = 0;
		}
	} else {
		down = a76 >= 0 && a76 < (int)bigpark_down && m5 < 20 ? down + 1 : 0;
		if (down >= 40) {               /* 2 s */
			want = 1;
			down = 0;
		}
	}
	if (want == bigpark_on) {
		return;
	}
	lck_mtx_lock(&scroff_lock);
	bigpark_on = want;
	bigpark_switches++;
	cores_apply();
	lck_mtx_unlock(&scroff_lock);
}

/* Darwin thermal pressure values used by SpringBoard on iOS 16.5.
 * Critical at the default 96 C M5 hotplug threshold, kept down to 93 C. This does not change hardware protection.
 */
static int thermal_pressure = -1;

static int
sysctl_thermal_pressure SYSCTL_HANDLER_ARGS
{
	int value = os_atomic_load(&thermal_pressure, relaxed);
	return sysctl_handle_int(oidp, &value, 0, req);
}

SYSCTL_DECL(_kern_redwood);
SYSCTL_PROC(_kern_redwood, OID_AUTO, thermal_pressure, CTLTYPE_INT | CTLFLAG_RD | CTLFLAG_LOCKED,
    NULL, 0, sysctl_thermal_pressure, "I", "Darwin thermal pressure, or -1 before a valid Exynos TMU sample");

/*
 * Boot-arg thermal_stop=C (x1s, SM-G981B; 0 or absent: off, as before): power the phone off when any zone reads C
 * degrees or more twice in a row (two samples, 250 ms apart). The G981B's owner's limit is 70 C. The sequence is the
 * earlier HakiOS-S20 port's heat guard (drivers/power/hakios.exynos_thermal.c there), run on that phone on 11 Nov
 * 2026, which is Samsung's sec_power_off: the power chip's restart features off (WTSR, SMPL; RTC block 2, register
 * 0x01 bits 6-7, register 0x03 bit 3), the APM told to stop (channel 4, protocol stop), PS_HOLD low (PMU 0x30C bit 8).
 * A USB cable can keep the power chip on: then the phone halts here instead.
 */
#define HEAT_ACPM_CH_FRAMEWORK  4
#define HEAT_ACPM_STOP          (1u << 22)
#define HEAT_PMIC_RTC           2
#define HEAT_PMU_PHYS           0x15860000U
#define HEAT_PMU_PS_HOLD        0x030cU
#define HEAT_PS_HOLD_EN         (1u << 8)
int redwood_pmic_read(uint8_t type, uint8_t reg, uint8_t *value);
int redwood_pmic_write(uint8_t type, uint8_t reg, uint8_t value);
static uint32_t heat_stop_c;
static unsigned heat_stop_seen;

static int
heat_pmic_clear(uint8_t reg, uint8_t bits)
{
	uint8_t v;

	if (redwood_pmic_read(HEAT_PMIC_RTC, reg, &v) != 0) {
		return -1;
	}
	return (v & bits) == 0 ? 0 : redwood_pmic_write(HEAT_PMIC_RTC, reg, (uint8_t)(v & ~bits));
}

static void __dead2
heat_power_off(int celsius)
{
	uint32_t stop[4] = { HEAT_ACPM_STOP, 0, 0, 0 };
	struct channel fw;
	int wtsr, sub, apm = -1;
	vm_offset_t pmu = ml_io_map(HEAT_PMU_PHYS, 0x1000);
	boolean_t istate;

	/* everything said while interrupts are on (XNU panics on console output with them off) */
	dlog("thermal_stop: %d C, at or above thermal_stop=%u: powering the phone off", celsius, heat_stop_c);
	wtsr = heat_pmic_clear(0x01, 0xC0);
	sub = heat_pmic_clear(0x03, 0x08);
	if (find_channel(HEAT_ACPM_CH_FRAMEWORK, &fw) == 0) {
		apm = ipc(&fw, stop);
	}
	dlog("thermal_stop: WTSR/SMPL off %d, SUB_SMPL off %d, APM stop %d; PS_HOLD low", wtsr, sub, apm);
	istate = ml_set_interrupts_enabled(FALSE);
	if (pmu != 0) {
		__builtin_arm_dsb(0xf);
		wr((volatile uint8_t *)(pmu + HEAT_PMU_PS_HOLD), rd((volatile uint8_t *)(pmu + HEAT_PMU_PS_HOLD)) & ~HEAT_PS_HOLD_EN);
		__builtin_arm_dsb(0xf);
	}
	delay_for_interval(2000, NSEC_PER_MSEC);
	(void)ml_set_interrupts_enabled(istate);
	dlog("thermal_stop: still on: a USB cable keeps the power chip on. Unplug it, or hold Volume Down + Power.");
	(void)ml_set_interrupts_enabled(FALSE);
	for (;;) {
		__builtin_arm_wfi();
	}
}

static void
heat_stop_check(const int temp[4])
{
	int hottest = -1;

	if (heat_stop_c == 0) {
		return;
	}
	for (unsigned z = 0; z < 4; z++) {
		if (temp[z] > hottest) {
			hottest = temp[z];
		}
	}
	heat_stop_seen = hottest >= (int)heat_stop_c ? heat_stop_seen + 1 : 0;
	if (heat_stop_seen >= 2) {
		heat_power_off(hottest);
	}
}

static void
thermal_pressure_sample(const int temp[4])
{
	int hottest = -1;
	bool complete = true;

	for (unsigned z = 0; z < 4; z++) {
		if (temp[z] < 0) {
			complete = false;
		}
		if (temp[z] > hottest) {
			hottest = temp[z];
		}
	}
	if (hottest < 0) {
		return;
	}
	int previous = os_atomic_load(&thermal_pressure, relaxed);
	int pressure = 0;
	/* The Exynos 990 works at 80-90 C (A, 2026-11-21): heavy at 88 C greyed out iOS's flashlight on a phone that was
	 * not hot. Moderate 88, heavy 93, critical 96 (Samsung's M5 hotplug), each with a 3 C hysteresis. */
	if (hottest >= 96 || (previous >= 40 && hottest > 93)) {
		pressure = 40;
	} else if (hottest >= 93 || (previous >= 30 && hottest > 90)) {
		pressure = 30;
	} else if (hottest >= 88 || (previous >= 20 && hottest > 85)) {
		pressure = 20;
	}
	/* A missing reading can never clear a warning. */
	if (complete || pressure > previous) {
		os_atomic_store(&thermal_pressure, pressure, relaxed);
	}
}

/* from the thermal step, with the BIG zone's temperature */
static void
bigoff_check(int t)
{
	int want;

	if (!bigoff || t < 0) {
		return;
	}
	want = bigoff_on ? t > (int)bigoff_in : t >= (int)bigoff_out;
	if (want == bigoff_on) {
		return;
	}
	lck_mtx_lock(&scroff_lock);
	bigoff_on = want;
	bigoff_switches++;
	cores_apply();
	lck_mtx_unlock(&scroff_lock);
	dlog("thermal: big zone %d C: cores 6-7 %s", t, want ? "OFF (hotplug, as Samsung at 96 C)" : "back on");
}

void redwood_dvfs_display(int on);
void
redwood_dvfs_display(int on)
{
	redwood_screen_on = on != 0;
	redwood_display_is_on = on != 0;
	if (on && scroff_little_on) {
		lck_mtx_lock(&scroff_lock);
		scroff_little_set(0);
		lck_mtx_unlock(&scroff_lock);
	}
	thread_wakeup((event_t)&redwood_gov_wake);
	thread_wakeup((event_t)&redwood_display_is_on);  /* Wi-Fi power save follows the screen (redwood_bcm4375.c) */
}

/* from the governor's thread, every tick */
static void
scroff_little_check(void)
{
	int want = scroff_little && prof_sleep && !redwood_screen_on;

	if (want && redwood_audio_active_at != 0) {
		uint64_t hold;

		nanoseconds_to_absolutetime(10ull * NSEC_PER_SEC, &hold);
		want = mach_absolute_time() - redwood_audio_active_at >= hold;
	}
	if (want == scroff_little_on) {
		return;
	}
	lck_mtx_lock(&scroff_lock);
	if (redwood_screen_on) {
		want = 0;                       /* (the display came on meanwhile: it has already given all 8 back) */
	}
	scroff_little_set(want);
	lck_mtx_unlock(&scroff_lock);
	dlog("power: screen off: %s", scroff_little_on ? "small cores only (0-3)" : "all 8 cores");
}

static int
sysctl_scroff_little SYSCTL_HANDLER_ARGS
{
	int value = (int)scroff_little;
	int error = sysctl_handle_int(oidp, &value, 0, req);

	if (error || req->newptr == USER_ADDR_NULL) {
		return error;
	}
	if (value != 0 && value != 1) {
		return EINVAL;
	}
	scroff_little = (uint32_t)value;
	thread_wakeup((event_t)&redwood_gov_wake);
	return 0;
}

static int
power_profile(void)
{
	if (!redwood_screen_on && prof_sleep) {
		return PROF_OFF;
	}
	return redwood_lowpower && prof_lpm ? PROF_LPM : PROF_NORMAL;
}

SYSCTL_INT(_kern_redwood, OID_AUTO, lowpower, CTLFLAG_RW | CTLFLAG_LOCKED, &redwood_lowpower, 0,
    "Low Power Mode: 1 caps the clocks (redwood_exynos_acpm.c)");
static int
sysctl_efficiency SYSCTL_HANDLER_ARGS
{
	int *setting = (int *)arg1;
	int value = os_atomic_load(setting, relaxed);
	int error = sysctl_handle_int(oidp, &value, 0, req);

	if (error || req->newptr == USER_ADDR_NULL) {
		return error;
	}
	if (value != 0 && value != 1) {
		return EINVAL;
	}
	os_atomic_store(setting, value, relaxed);
	thread_wakeup((event_t)&redwood_gov_wake);
	return 0;
}
SYSCTL_PROC(_kern_redwood, OID_AUTO, efficiency, CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED,
    &efficiency, 0, sysctl_efficiency, "I", "Efficiency clock caps: 0 normal, 1 underclocked (requires dvfs=2)");
SYSCTL_PROC(_kern_redwood, OID_AUTO, efficiency_auto, CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED,
    &efficiency_auto, 0, sysctl_efficiency, "I", "Independent automatic cooling: on by default, uses existing thermal samples");
SYSCTL_UINT(_kern_redwood, OID_AUTO, efficiency_mask, CTLFLAG_RD | CTLFLAG_LOCKED,
    &efficiency_mask, 0, "Automatic cooling bits: A55=1 A76=2 M5=4 GPU=8; memory independent");
SYSCTL_PROC(_kern_redwood, OID_AUTO, scroff_little, CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED,
    NULL, 0, sysctl_scroff_little, "I", "Screen off: the small cores (0-3) only; 1 on (default), 0 off");
SYSCTL_UINT(_kern_redwood, OID_AUTO, bigpark, CTLFLAG_RW | CTLFLAG_LOCKED, &bigpark, 0,
    "1: the M5s (cpu6-7) parked until the A76s are bigpark_up % busy (Samsung's EMS: big cores last)");
SYSCTL_UINT(_kern_redwood, OID_AUTO, bigpark_up, CTLFLAG_RW | CTLFLAG_LOCKED, &bigpark_up, 0, "A76 busy % that unparks the M5s");
SYSCTL_UINT(_kern_redwood, OID_AUTO, bigpark_down, CTLFLAG_RW | CTLFLAG_LOCKED, &bigpark_down, 0, "A76 busy % under which they park");
SYSCTL_INT(_kern_redwood, OID_AUTO, bigpark_now, CTLFLAG_RD | CTLFLAG_LOCKED, &bigpark_on, 0, "1: the M5s are parked now");
SYSCTL_UINT(_kern_redwood, OID_AUTO, bigpark_switches, CTLFLAG_RD | CTLFLAG_LOCKED, &bigpark_switches, 0, "park/unpark count");
SYSCTL_INT(_kern_redwood, OID_AUTO, bigoff_now, CTLFLAG_RD | CTLFLAG_LOCKED,
    &bigoff_on, 0, "1: cores 6-7 are out of the scheduler (BIG zone hot, Samsung's 96/91 C hotplug)");
SYSCTL_UINT(_kern_redwood, OID_AUTO, bigoff_switches, CTLFLAG_RD | CTLFLAG_LOCKED,
    &bigoff_switches, 0, "times the hot-big-cores hotplug switched");
SYSCTL_INT(_kern_redwood, OID_AUTO, scroff_little_now, CTLFLAG_RD | CTLFLAG_LOCKED,
    &scroff_little_on, 0, "1: the scheduler has cores 0-3 only right now");
SYSCTL_UINT(_kern_redwood, OID_AUTO, scroff_little_switches, CTLFLAG_RD | CTLFLAG_LOCKED,
    &scroff_little_switches, 0, "times the screen-off small-cores mode was switched");
/*
 * x1slte (2026-11-06): a tick of 50 ms (was 200: a frame's worth of work had come and gone before the clock moved;
 * Samsung's schedutil re-decides on every scheduler update), one rung down after GOV_DOWN_TICKS quiet ticks (300 ms);
 * eight rungs of Samsung's own levels (ECT steps of the Exynos 990) up to the real tops.
 */
#define GOV_TICK_MS     50
/* (A, 2026-11-15) screen off: one tick every 500 ms (dvfs_scroff_tick=250|500|1000; 50 = as awake). The clocks sit at
 * the screen-off caps anyway; the thermal steps then run every tick (500 ms) and the 10 s line stays 10 s. */
static uint32_t gov_scroff_tick_ms = 500;
#define GOV_DOWN_TICKS  6
#define GOV_RUNGS       8
static const struct {
	unsigned domain;
	unsigned first_cpu, ncpus;      /* ncpus 0: the GPU */
	uint32_t rung[GOV_RUNGS];       /* kHz, low to high; 0: unused */
	unsigned aim, jump;             /* the busy % a rung is chosen for, and the % that takes the top at once */
	uint32_t touch_head, touch_tail;        /* kHz the input booster holds at least (0: none) */
} gov[] = {
	/*
	 * The A55s' touch floor is above Samsung's (806 MHz there): Samsung's scheduler keeps the UI off the little
	 * cores (EMS), XNU here has one performance level for all eight (hw.nperflevels 1), so a UI thread can be on one.
	 */
	{ D_CL0, 0, 4, { 806000, 1053000, 1248000, 1456000, 1690000, 2002000, 0, 0 }, 60, 70, 1456000, 1248000 },
	/* rung 0 of the A76 and M5: Samsung's own idle floors (DTS user-default-qos 845 / 962 MHz); used with
	 * kern.redwood.gov_lowfloor=1 (A, 2026-11-20), else the old lowest 1066 / 1196 */
	{ D_CL1, 4, 2, { 845000, 1066000, 1404000, 1794000, 2106000, 2304000, 2504000, 0 }, 60, 70, 1404000, 1222000 },
	{ D_CL2, 6, 2, { 962000, 1196000, 1274000, 1586000, 1898000, 2208000, 2504000, 2730000 }, 60, 70, 1066000, 858000 },
	{ D_G3D, 0, 0, { 156000, 260000, 377000, 455000, 572000, 702000, 800000, 0 }, 40, 60, 572000, 572000 },
};
#define TOUCH_HEAD_MS   130
#define TOUCH_TAIL_MS   500
extern uint64_t redwood_touch_down_at, redwood_touch_last_at;
extern uint32_t redwood_mali_slow_jobs;
/*
 * dvfs_frame=1 (x1slte, 2026-11-06, B): while frames are coming (a swap on the display within FRAME_HOLD_MS), each
 * cluster is held at least at its frame floor: the big cores at their tops. The window server's frame is CPU work in
 * the Metal library (12-34 ms a frame measured: PLANS/10-mali2metal.md), so its speed is the big cores' clock; the
 * touch floors (Samsung's input booster: A76 1404, M5 1066 MHz) assume Samsung's scheduler-driven clocks, which react
 * within a frame -- this governor ticks every 50 ms. A still screen sends no swaps: idle is unchanged. The thermal
 * steps still cap. MiniIOMobileFramebuffer::present() sets redwood_fb_swap_at and wakes this thread when frames
 * start after a pause.
 */
#define FRAME_HOLD_MS   300
uint64_t redwood_fb_swap_at;
/*
 * The wake boost (B, 2026-11-06): the frame floor held until redwood_boost_until -- set for 1 s by a power/volume key
 * press (redwood_exynos_keys.c) and by the display coming on (MiniIOMobileFramebuffer::displayPower), so the lock
 * screen's wake animation (wallpaper zoom and unblur) is drawn at full clocks from its first frame, not from the
 * first swap after the cores had dropped to idle.
 */
uint64_t redwood_boost_until;
void redwood_gov_boost(uint32_t ms);
void
redwood_gov_boost(uint32_t ms)
{
	uint64_t len;

	nanoseconds_to_absolutetime((uint64_t) ms * NSEC_PER_MSEC, &len);
	redwood_boost_until = mach_absolute_time() + len;
	thread_wakeup((event_t)&redwood_gov_wake);
}
static uint32_t gov_frame = 1;                  /* dvfs_frame= (on by default; 0 off) */
/* M5 at 2208, not its 2730 top: held at the top through the tour it reached 89 C and was throttled to 1794 (step 1) */
/* A55 at its 2002 top too: the lock screen's wallpaper (PosterBoard's extensions, lower priority) is drawn on the little
 * cores while SpringBoard and backboardd hold the big ones -- its screen-off frames came 33-50 ms apart at 1456 */
/*
 * (B, 2026-11-07 18:50: the floors were 2002/2504/2208 -- near the tops -- for every frame, even a clock tick: the SoC sat
 * at 80-88 C in plain UI use and the thermal steps throttled scrolling. Efficient points now; the governor still raises
 * on load and on touch. dvfs_ffl_cl0/cl1/cl2/g3d= override.)
 */
static uint32_t gov_frame_khz[] = { 1456000, 2106000, 1898000, 572000 };  /* cl0, cl1, cl2, g3d (gov[] order) */
static uint32_t gov_frame_ticks;                /* ticks with the frame floor on (for the 10 s line) */
SYSCTL_UINT(_kern_redwood, OID_AUTO, ffl_cl0, CTLFLAG_RW | CTLFLAG_LOCKED, &gov_frame_khz[0], 0, "frame floor, A55 kHz");
SYSCTL_UINT(_kern_redwood, OID_AUTO, ffl_cl1, CTLFLAG_RW | CTLFLAG_LOCKED, &gov_frame_khz[1], 0, "frame floor, A76 kHz");
SYSCTL_UINT(_kern_redwood, OID_AUTO, ffl_cl2, CTLFLAG_RW | CTLFLAG_LOCKED, &gov_frame_khz[2], 0, "frame floor, M5 kHz");
SYSCTL_UINT(_kern_redwood, OID_AUTO, ffl_g3d, CTLFLAG_RW | CTLFLAG_LOCKED, &gov_frame_khz[3], 0, "frame floor, GPU kHz");

/* the lowest rung the touch booster holds cluster `g` at now (0: no boost) */
static unsigned
gov_touch_floor(unsigned g)
{
	uint64_t now = mach_absolute_time(), head, tail;
	uint32_t khz;
	unsigned r = 0;

	nanoseconds_to_absolutetime(TOUCH_HEAD_MS * NSEC_PER_MSEC, &head);
	nanoseconds_to_absolutetime(TOUCH_TAIL_MS * NSEC_PER_MSEC, &tail);
	khz = redwood_touch_down_at != 0 && now - redwood_touch_down_at < head ? gov[g].touch_head :
	    redwood_touch_last_at != 0 && now - redwood_touch_last_at < tail ? gov[g].touch_tail : 0;
	if (gov_frame && redwood_fb_swap_at != 0 && g < sizeof(gov_frame_khz) / sizeof(gov_frame_khz[0])) {
		uint64_t hold;

		nanoseconds_to_absolutetime(FRAME_HOLD_MS * NSEC_PER_MSEC, &hold);
		if ((now - redwood_fb_swap_at < hold || now < redwood_boost_until) && gov_frame_khz[g] > khz) {
			khz = gov_frame_khz[g];
		}
	}
	while (khz != 0 && r + 1 < GOV_RUNGS && gov[g].rung[r + 1] != 0 && gov[g].rung[r] < khz) {
		r++;
	}
	return r;
}
extern int redwood_mali_busy_pct(void);
#define NGOV            (sizeof(gov) / sizeof(gov[0]))

/*
 * Live tuning (A, 2026-11-20; docs/devices/x1slte/power-thermal-samsung.md): every value below is a sysctl, so the
 * clocks can be tried on a running phone (temperatures, frame times) before one becomes a default.
 */
static uint32_t gov_lowfloor = 1;       /* kern.redwood.gov_lowfloor: 1 Samsung's idle floors (A76 845, M5 962) */
static unsigned
gov_min_rung(unsigned g)
{
	return !gov_lowfloor && (gov[g].domain == D_CL1 || gov[g].domain == D_CL2) ? 1 : 0;
}
uint32_t redwood_wake_boost_ms = 1000;  /* kern.redwood.wake_boost_ms: the frame floor after a key press / display on */
static int gov_last_busy[8];            /* each gov[] entry's last busy % (for the big-core parking) */
SYSCTL_UINT(_kern_redwood, OID_AUTO, gov_lowfloor, CTLFLAG_RW | CTLFLAG_LOCKED, &gov_lowfloor, 0,
    "1: Samsung's idle floors (A76 845, M5 962 MHz); 0: the old 1066 / 1196");
SYSCTL_UINT(_kern_redwood, OID_AUTO, wake_boost_ms, CTLFLAG_RW | CTLFLAG_LOCKED, &redwood_wake_boost_ms, 0,
    "frame floor held after a key press or the display coming on, ms");

/*
 * Performance profiles (A, 2026-11-20): one switch for the tunables above. kern.redwood.perf_profile / boot-arg
 * perf_profile=: 0 cool (Samsung-like), 1 balanced (default: the user's pick from the A/B test), 2 performance. Every
 * profile keeps the 96 C big-core cutoff and Samsung's clock tables; a tunable changed by hand afterwards stays changed.
 */
static int perf_therm_offset;           /* degrees added to every thermal step (cool -4, performance +4) */
static uint32_t perf_profile = 1;
static uint32_t gpu_max_khz = 572000; /* user: lower GPU bursts by default */
static uint32_t m5_max_khz = 1586000; /* user: prioritize lower heat over M5 bursts */
/*
 * Sliding ceilings (A, 2026-11-20): instead of a fixed M5 / GPU cap, a ceiling that follows the zone's temperature,
 * as Samsung's power allocator converges on its control temperature: the full burst while the zone is cool, sliding
 * down one of Samsung's clock steps at a time to the profile's floor at the hot mark. It reuses the thermal step's
 * readings (no extra work), moves up again only 2 C below where it moved down, and writes m5_max / gpu_max, so the
 * caps in profile_cap() stay the one place that limits. kern.redwood.dyncap=0: the fixed caps of before.
 */
static uint32_t dyncap = 1;
static struct dyn { uint32_t hi, lo; int t_lo, t_hi; } dyn_m5 = { 2208000, 1274000, 70, 86 },
    dyn_gpu = { 702000, 455000, 72, 88 };
/* the ceiling for zone temperature t, on one of gov[g]'s rungs (Samsung's steps), never under the range's floor */
static uint32_t
dyn_curve(const struct dyn *r, int t, unsigned g)
{
	uint64_t want;
	uint32_t best = r->lo;

	if (t <= r->t_lo || r->t_hi <= r->t_lo) {
		want = r->hi;
	} else if (t >= r->t_hi) {
		want = r->lo;
	} else {
		want = r->hi - (uint64_t)(r->hi - r->lo) * (uint64_t)(t - r->t_lo) / (uint64_t)(r->t_hi - r->t_lo);
	}
	for (unsigned i = 0; i < GOV_RUNGS; i++) {      /* the highest rung at or under the curve */
		uint32_t k = gov[g].rung[i];
		if (k != 0 && k <= want && k > best) {
			best = k;
		}
	}
	return best;
}

/* from the thermal step: down at once, up only once the curve 2 C hotter allows it (no flapping at a mark) */
static void
dyn_apply(const struct dyn *r, int t, unsigned g, uint32_t *cap)
{
	uint32_t down, up;

	if (t < 0 || r->hi == 0) {
		return;
	}
	down = dyn_curve(r, t, g);
	up = dyn_curve(r, t + 2, g);
	if (down < *cap || *cap == 0) {
		*cap = down;
	} else if (up > *cap) {
		*cap = up;
	}
}
SYSCTL_UINT(_kern_redwood, OID_AUTO, dyncap, CTLFLAG_RW | CTLFLAG_LOCKED, &dyncap, 0,
    "1: M5/GPU ceilings slide with temperature (profile ranges); 0: fixed m5_max/gpu_max");
SYSCTL_UINT(_kern_redwood, OID_AUTO, dyn_m5_hi, CTLFLAG_RW | CTLFLAG_LOCKED, &dyn_m5.hi, 0, "M5 ceiling when cool, kHz");
SYSCTL_UINT(_kern_redwood, OID_AUTO, dyn_m5_lo, CTLFLAG_RW | CTLFLAG_LOCKED, &dyn_m5.lo, 0, "M5 ceiling when hot, kHz");
SYSCTL_INT(_kern_redwood, OID_AUTO, dyn_m5_tlo, CTLFLAG_RW | CTLFLAG_LOCKED, &dyn_m5.t_lo, 0, "BIG zone C up to which the M5 may burst");
SYSCTL_INT(_kern_redwood, OID_AUTO, dyn_m5_thi, CTLFLAG_RW | CTLFLAG_LOCKED, &dyn_m5.t_hi, 0, "BIG zone C from which the M5 sits at its floor");
SYSCTL_UINT(_kern_redwood, OID_AUTO, dyn_gpu_hi, CTLFLAG_RW | CTLFLAG_LOCKED, &dyn_gpu.hi, 0, "GPU ceiling when cool, kHz");
SYSCTL_UINT(_kern_redwood, OID_AUTO, dyn_gpu_lo, CTLFLAG_RW | CTLFLAG_LOCKED, &dyn_gpu.lo, 0, "GPU ceiling when hot, kHz");
SYSCTL_INT(_kern_redwood, OID_AUTO, dyn_gpu_tlo, CTLFLAG_RW | CTLFLAG_LOCKED, &dyn_gpu.t_lo, 0, "G3D zone C up to which the GPU may burst");
SYSCTL_INT(_kern_redwood, OID_AUTO, dyn_gpu_thi, CTLFLAG_RW | CTLFLAG_LOCKED, &dyn_gpu.t_hi, 0, "G3D zone C from which the GPU sits at its floor");
SYSCTL_UINT(_kern_redwood, OID_AUTO, m5_max, CTLFLAG_RW | CTLFLAG_LOCKED,
    &m5_max_khz, 0, "Additional Mongoose M5 ceiling in kHz; 0 disables this ceiling");
SYSCTL_UINT(_kern_redwood, OID_AUTO, gpu_max, CTLFLAG_RW | CTLFLAG_LOCKED,
    &gpu_max_khz, 0, "Additional GPU ceiling in kHz; 0 disables this ceiling");
static void bigpark_set_default(uint32_t on);
static void
perf_profile_apply(uint32_t p)
{
	static const struct {
		uint32_t lowfloor, ffl[4], wake_ms, park, m5_max, gpu_max;
		int therm;
	} t[3] = {
		{ 1, { 1456000, 1794000, 1274000, 455000 }, 500, 1, 1274000, 455000, -4 },     /* cool */
		{ 1, { 1456000, 2106000, 1274000, 572000 }, 500, 1, 1586000, 572000, 0 },      /* balanced */
		{ 0, { 1456000, 2106000, 1898000, 572000 }, 1000, 0, 2730000, 800000, 4 },     /* performance */
	};
	if (p > 2) {
		return;
	}
	perf_profile = p;
	gov_lowfloor = t[p].lowfloor;
	for (unsigned i = 0; i < 4; i++) {
		gov_frame_khz[i] = t[p].ffl[i];
	}
	redwood_wake_boost_ms = t[p].wake_ms;
	perf_therm_offset = t[p].therm;
	bigpark_set_default(t[p].park);
    m5_max_khz = t[p].m5_max;
    gpu_max_khz = t[p].gpu_max;
    {
        static const struct dyn m5r[3] = { { 1586000, 1196000, 66, 82 }, { 2208000, 1274000, 70, 86 }, { 2730000, 1586000, 74, 90 } };
        static const struct dyn gr[3] = { { 572000, 377000, 68, 84 }, { 702000, 455000, 72, 88 }, { 800000, 572000, 76, 92 } };
        dyn_m5 = m5r[p];
        dyn_gpu = gr[p];
    }
    bigpark_up = p < 2 ? 90 : 80;
    bigpark_down = p < 2 ? 60 : 40;
}
static int
sysctl_perf_profile SYSCTL_HANDLER_ARGS
{
	int value = (int)perf_profile, error = sysctl_handle_int(oidp, &value, 0, req);
	if (error || req->newptr == USER_ADDR_NULL) {
		return error;
	}
	if (value < 0 || value > 2) {
		return EINVAL;
	}
	perf_profile_apply((uint32_t)value);
	thread_wakeup((event_t)&redwood_gov_wake);
	return 0;
}
SYSCTL_PROC(_kern_redwood, OID_AUTO, perf_profile, CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED, NULL, 0, sysctl_perf_profile,
    "I", "0 cool (Samsung-like), 1 balanced (default), 2 performance");
SYSCTL_INT(_kern_redwood, OID_AUTO, perf_therm_offset, CTLFLAG_RW | CTLFLAG_LOCKED, &perf_therm_offset, 0,
    "degrees added to the thermal steps (profile sets it)");

static uint32_t gov_ticks[8][2];        /* busy, all: the last reading of each core */

/* the busiest core of the cluster since the last call (or the GPU's load), in percent; -1: no reading */
static int
gov_busy(unsigned g)
{
	int worst = -1;

	if (gov[g].ncpus == 0) {
		return redwood_mali_busy_pct();
	}

	for (unsigned c = gov[g].first_cpu; c < gov[g].first_cpu + gov[g].ncpus && c < 8 && c < real_ncpus; c++) {
		processor_cpu_load_info_data_t load;
		mach_msg_type_number_t count = PROCESSOR_CPU_LOAD_INFO_COUNT;
		host_t host;
		uint32_t busy, all, db, da;

		if (processor_array[c] == PROCESSOR_NULL ||
		    processor_info(processor_array[c], PROCESSOR_CPU_LOAD_INFO, &host, (processor_info_t)&load, &count) != KERN_SUCCESS) {
			continue;
		}
		busy = load.cpu_ticks[CPU_STATE_USER] + load.cpu_ticks[CPU_STATE_SYSTEM] + load.cpu_ticks[CPU_STATE_NICE];
		all = busy + load.cpu_ticks[CPU_STATE_IDLE];
		db = busy - gov_ticks[c][0];
		da = all - gov_ticks[c][1];
		gov_ticks[c][0] = busy;
		gov_ticks[c][1] = all;
		if (da != 0) {
			int pct = (int)(db * 100 / da);

			worst = pct > worst ? pct : worst;
		}
	}
	return worst;
}

/* Opt-in until cold-start phone comparisons pass. Legacy thermal limits remain
 * active, so enabling the new policy never weakens existing cooling. */
static int gpu_ladder, gpu_sustain;
static uint32_t gpu_policy_cap;
SYSCTL_INT(_kern_redwood, OID_AUTO, gpu_ladder, CTLFLAG_RW | CTLFLAG_LOCKED,
    &gpu_ladder, 0, "Samsung-derived GPU thermal ladder (experimental)");
SYSCTL_INT(_kern_redwood, OID_AUTO, gpu_sustain, CTLFLAG_RW | CTLFLAG_LOCKED,
    &gpu_sustain, 0, "455 MHz cap after 10 s at >=85 percent GPU load and >=78 C");
SYSCTL_UINT(_kern_redwood, OID_AUTO, gpu_policy_cap, CTLFLAG_RD | CTLFLAG_LOCKED,
    &gpu_policy_cap, 0, "GPU policy ceiling in kHz; 0 means no additional cap");

/* the profile's cap for domain index d on top of `cap' (0: none) */
static uint32_t
profile_cap(unsigned d, uint32_t cap, int prof)
{
	uint32_t p = prof == PROF_OFF ? domains[d].safe : prof == PROF_LPM ? domains[d].mid : 0;

    if (domains[d].domain == D_CL2 && m5_max_khz && (!p || m5_max_khz < p)) {
        p = m5_max_khz;
    }
    if (domains[d].domain == D_G3D && gpu_max_khz && (!p || gpu_max_khz < p)) {
        p = gpu_max_khz;
    }
    if (domains[d].domain == D_G3D && gpu_policy_cap && (!p || gpu_policy_cap < p)) {
        p = gpu_policy_cap;
    }

	if (domains[d].domain == D_MIF) {
		p = prof == PROF_OFF ? 421000 : prof == PROF_LPM ? 1352000 : 0;
	}
	if (efficiency_active(domains[d].domain)) {
		uint32_t e = efficiency_cap(domains[d].domain);

		if (e && (!p || e < p)) {
			p = e;
		}
	}
	return p && (!cap || p < cap) ? p : cap;
}

static void
acpm_thread(void *arg, wait_result_t wr_unused)
{
	uint32_t mode = 0, wait = 10, hot_at = 0, fb_hot = 82;
	struct redwood_cooling_state cooling[4] = {0};
	struct redwood_gpu_policy gpu_policy = {0};
	int bl_step = 0;
	int hot = 0, level[NDOMAINS] = { 0 }, temp[4];
	uint32_t govern = 1, govern_gpu = 1, gov_rung[NGOV], gov_low[NGOV], gov_sent[NGOV], gov_changes = 0, tick = 0;
	uint32_t gov_boosts = 0, gov_touches = 0, gov_slow_seen = 0, gov_slow_raises = 0, gov_slow_last = 0;
	uint64_t gov_khz_sum[NGOV];

	(void)arg;
	(void)wr_unused;
	PE_parse_boot_argn("dvfs", &mode, sizeof(mode));
	PE_parse_boot_argn("dvfs_wait", &wait, sizeof(wait));
	PE_parse_boot_argn("dvfs_hot", &hot_at, sizeof(hot_at));
	if (PE_parse_boot_argn("thermal_stop", &heat_stop_c, sizeof(heat_stop_c)) && heat_stop_c != 0) {
		if (heat_stop_c < 20 || heat_stop_c > 90) {
			heat_stop_c = 70;
		}
		dlog("thermal_stop=%u: the phone powers off at %u C in any zone (two samples)", heat_stop_c, heat_stop_c);
	}
	PE_parse_boot_argn("dvfs_gov", &govern, sizeof(govern));
	PE_parse_boot_argn("fb_hot", &fb_hot, sizeof(fb_hot));
	PE_parse_boot_argn("dvfs_gov_gpu", &govern_gpu, sizeof(govern_gpu));
    PE_parse_boot_argn("gpu_ladder", &gpu_ladder, sizeof(gpu_ladder));
    PE_parse_boot_argn("gpu_sustain", &gpu_sustain, sizeof(gpu_sustain));
	PE_parse_boot_argn("dvfs_frame", &gov_frame, sizeof(gov_frame));
	PE_parse_boot_argn("dvfs_ffl_cl0", &gov_frame_khz[0], sizeof(gov_frame_khz[0]));
	PE_parse_boot_argn("dvfs_ffl_cl1", &gov_frame_khz[1], sizeof(gov_frame_khz[1]));
	PE_parse_boot_argn("dvfs_ffl_cl2", &gov_frame_khz[2], sizeof(gov_frame_khz[2]));
	PE_parse_boot_argn("dvfs_ffl_g3d", &gov_frame_khz[3], sizeof(gov_frame_khz[3]));
	PE_parse_boot_argn("dvfs_sleep", &prof_sleep, sizeof(prof_sleep));
	PE_parse_boot_argn("scroff_little", &scroff_little, sizeof(scroff_little));
	PE_parse_boot_argn("dvfs_bigoff", &bigoff, sizeof(bigoff));
	{
		uint32_t p = 1;
		PE_parse_boot_argn("perf_profile", &p, sizeof(p));
		perf_profile_apply(p);
	}
	PE_parse_boot_argn("dvfs_gpu_max", &gpu_max_khz, sizeof(gpu_max_khz));
	PE_parse_boot_argn("dvfs_m5_max", &m5_max_khz, sizeof(m5_max_khz));
	PE_parse_boot_argn("dvfs_bigoff_out", &bigoff_out, sizeof(bigoff_out));
	PE_parse_boot_argn("dvfs_bigoff_in", &bigoff_in, sizeof(bigoff_in));
	PE_parse_boot_argn("dvfs_scroff_tick", &gov_scroff_tick_ms, sizeof(gov_scroff_tick_ms));
	if (gov_scroff_tick_ms != 50 && gov_scroff_tick_ms != 250 && gov_scroff_tick_ms != 500 && gov_scroff_tick_ms != 1000) {
		gov_scroff_tick_ms = 500;
	}
	PE_parse_boot_argn("dvfs_lpm", &prof_lpm, sizeof(prof_lpm));
	PE_parse_boot_argn("dvfs_efficiency", &efficiency, sizeof(efficiency));
	efficiency = efficiency != 0;
	for (unsigned g = 0; g < NGOV; g++) {
		gov_rung[g] = GOV_RUNGS - 1;
		while (gov_rung[g] > 0 && gov[g].rung[gov_rung[g]] == 0) {
			gov_rung[g]--;
		}
		gov_low[g] = 0;
		gov_sent[g] = 0;
		gov_khz_sum[g] = 0;
	}
	for (unsigned i = 0; i < NDOMAINS; i++) {
		targets[i] = domains[i].target;
		if (domains[i].arg != NULL) {
			PE_parse_boot_argn(domains[i].arg, &targets[i], sizeof(targets[i]));
		}
	}
	delay_for_interval(wait, NSEC_PER_SEC);
	/*
	 * Every core up first: a core is powered on through the firmware, and a clock change in
	 * flight there held cpu1-3 off for good once (25 October, with cluster 2's request hung).
	 */
	for (unsigned waited = 0; processor_avail_count < real_ncpus && waited < 120; waited++) {
		delay_for_interval(1, NSEC_PER_SEC);
	}
	if (processor_avail_count < real_ncpus) {
		dlog("only %u of %u cores up after two minutes: clocks are left alone", processor_avail_count, real_ncpus);
		mode = mode > 1 ? 1 : mode;
	}

	intr = (volatile uint8_t *)ml_io_map(ACPM_INTR_PHYS, 0x1000);
	sram = (volatile uint8_t *)ml_io_map(ACPM_SRAM_PHYS, ACPM_SRAM_SIZE);
	if (intr == NULL || sram == NULL) {
		dlog("the ACPM mailbox or its SRAM could not be mapped%s",
		    heat_stop_c ? ": thermal_stop CANNOT WORK this run; stop the run" : "");
		return;
	}
	dlog("dvfs=%u: ACPM framework at SRAM+%#x: %u plugins, %u channels (%u for the AP)", mode,
	    ACPM_INITDATA, rd(sram + ACPM_INITDATA + 4), rd(sram + ACPM_INITDATA + 12),
	    rd(sram + ACPM_INITDATA + 24));
	if (find_channel(CH_DVFS, &ch_dvfs) != 0) {
		if (heat_stop_c != 0) {
			dlog("thermal_stop: no DVFS channel, so no temperature loop: the heat power-off CANNOT work; stop the run");
		}
		return;
	}
	if (find_channel(CH_TMU, &ch_tmu) != 0) {
		ch_tmu.len = 0;
		if (heat_stop_c != 0) {
			dlog("thermal_stop: NO TEMPERATURE CHANNEL: the heat power-off CANNOT work this run; stop the run");
		}
	}
	/* we poll: no interrupt to the AP for these two (INTMR1 bit set = masked) */
	wr(intr + INTMR1, rd(intr + INTMR1) | (1u << CH_DVFS) | (ch_tmu.len ? 1u << CH_TMU : 0));
	if (ch_tmu.len != 0) {
		uint32_t cmd[4] = { 0, TMU_IPC_INIT, 0, 0 };

		if (ipc(&ch_tmu, cmd) != 0) {
			ch_tmu.len = 0;
		}
	}
	cmu_top = (volatile uint8_t *)ml_io_map(CMU_TOP_PHYS, 0x1000);
	report("as S-Boot left them");
	if (mode >= 2) {
		/* the GPU first, on its own: the CPUs only if PLL_G3D shows ACPM took it */
		uint32_t before = g3d_pll_mhz(), g3d = 0;

		for (unsigned i = 0; i < NDOMAINS; i++) {
			if (domains[i].domain == D_G3D) {
				g3d = targets[i] ? profile_cap(i, targets[i], power_profile()) : 0;
			}
		}
		int rate_error = set_rate(D_G3D, g3d);
		delay(5000);
        uint32_t after = g3d_pll_mhz();
        /* A lower cap can equal the boot clock. Successful readback at the
         * requested clock is valid even when the PLL did not need to move. */
		if (!redwood_gpu_rate_verified(rate_error, g3d, after)) {
			dlog("PLL_G3D %u MHz after asking for %u: ACPM rate not verified, nothing else is set", g3d_pll_mhz(),
			    g3d / 1000);
			mode = 1;
		} else {
			dlog("PLL_G3D %u -> %u MHz: ACPM DVFS works", before, g3d_pll_mhz());
		}
	}
	if (mode >= 2) {
		/*
		 * Cluster 2 (cores 6-7, M5) only when dvfs_cl2= says so: the cores are kept off
		 * (cpumask), and a request for a powered-down cluster was never answered and left
		 * ACPM deaf to everything after it (temperatures included) -- 25 October.
		 */
		apply(0, true);
		report("after the request");
		if (ch_tmu.len != 0 && hottest() < 0) {
			dlog("ACPM stopped answering after the clock requests: nothing more is asked of it");
			mode = 1;
		}
	}
	/*
	 * Cluster 2 (the two Mongoose M5, S-Boot leaves them at 266 MHz) last and alone, and only with every
	 * core up -- the one request ever left unanswered was for this cluster while it was powered down.
	 */
	for (unsigned i = 0; mode >= 2 && i < NDOMAINS; i++) {
		if (domains[i].domain != D_CL2 || targets[i] == 0) {
			continue;
		}
		if (processor_avail_count < real_ncpus) {
			dlog("cl2 (M5): not every core is up (%u of %u): its clock is left as S-Boot set it", processor_avail_count,
			    real_ncpus);
			break;
		}
		uint32_t khz = profile_cap(i, targets[i], power_profile());
		if (set_rate(D_CL2, khz) != 0) {
			dlog("cl2 (M5): %u kHz was not taken", khz);
		} else {
			dlog("cl2 (M5): asked for %u MHz", khz / 1000);
		}
		delay(5000);
		if (ch_tmu.len != 0 && hottest() < 0) {
			dlog("ACPM STOPPED ANSWERING AFTER THE CLUSTER 2 REQUEST: no temperatures, no throttling from here on");
			mode = 1;
		}
		report("after cluster 2");
	}
	redwood_dbgport_add("acpm.rate", "DOMAIN KHZ: an ACPM DVFS request (AUD 8, INT 1, MIF 0, G3D 10)", dbg_acpm_rate);
	for (;; tick++) {
		uint64_t tick_end;
		/* screen off: a longer tick, `step` short ones long; tick kept a multiple of step so the 250 ms (5) and
		 * 10 s (200) marks still fall on it */
		bool long_tick = mode >= 2 && prof_sleep && !redwood_screen_on && gov_scroff_tick_ms > GOV_TICK_MS;
		unsigned step = long_tick ? gov_scroff_tick_ms / GOV_TICK_MS : 1;

		if (step > 1) {
			tick = (tick + step - 1) / step * step;
		}
		scroff_little_check();

		if (dbg_domain != ~0u) {
			uint32_t d = dbg_domain;

			dbg_domain = ~0u;
			dbg_result = (uint32_t)set_rate(d, dbg_khz);
		}

		/* a tick, or sooner a finger coming down or a slow GPU job: its boost at once */
		clock_interval_to_deadline(GOV_TICK_MS * step, NSEC_PER_MSEC, &tick_end);
		for (;;) {
			bool slow;

			assert_wait_deadline((event_t)&redwood_gov_wake, THREAD_UNINT, tick_end);
			if (thread_block(THREAD_CONTINUE_NULL) != THREAD_AWAKENED) {
				break;
			}
			if (long_tick && redwood_screen_on) {
				break;                  /* the display is back: a normal tick now, not in up to 500 ms */
			}
			if (!long_tick && !redwood_screen_on && step == 1 && gov_scroff_tick_ms > GOV_TICK_MS) {
				scroff_little_check();  /* the display went off: hand the work to the small cores now */
			}
			slow = redwood_mali_slow_jobs != gov_slow_seen;
			gov_slow_seen = redwood_mali_slow_jobs;
			gov_touches += !slow;
			for (unsigned g = 0; govern && mode >= 2 && g < NGOV; g++) {
				unsigned d, floor = gov_touch_floor(g);
				uint32_t cap, khz;

				if (slow && gov[g].ncpus == 0) {
					for (floor = GOV_RUNGS - 1; floor > 0 && gov[g].rung[floor] == 0; floor--) {
					}
					gov_slow_raises += floor > gov_rung[g];
				}

				for (d = 0; d < NDOMAINS && domains[d].domain != gov[g].domain; d++) {
				}
				if (d == NDOMAINS || targets[d] == 0 || floor <= gov_rung[g] || (gov[g].ncpus == 0 && !govern_gpu)) {
					continue;
				}
				gov_rung[g] = floor;
				gov_low[g] = 0;
				cap = level[d] == 2 ? domains[d].safe : level[d] == 1 ? domains[d].mid : targets[d];
				cap = profile_cap(d, cap, power_profile());
				khz = gov[g].rung[floor];
				khz = khz > targets[d] ? targets[d] : khz;
				khz = cap != 0 && khz > cap ? cap : khz;
				if (governor_set_rate(domains[d].domain, khz, &gov_sent[g]) > 0) {
					gov_changes++;
					gov_boosts++;
				}
			}
		}
		/* memory per power profile (on change only) */
		if (mode >= 2) {
			static int mif_prof = -1, mif_efficiency = -1;
			int prof = power_profile(), eff = efficiency_active(D_MIF);

			if (prof != mif_prof || eff != mif_efficiency) {
				unsigned m = 0;
				uint32_t khz;

				while (m < NDOMAINS && domains[m].domain != D_MIF) {
					m++;
				}
				khz = m < NDOMAINS && targets[m] ? profile_cap(m, targets[m], prof) : 0;
				if (khz != 0 && set_rate(D_MIF, khz) == 0) {
					dlog("power: %s: memory %u MHz%s", prof == PROF_OFF ? "screen off" : prof == PROF_LPM ?
					    "Low Power Mode" : "normal", khz / 1000, prof == PROF_NORMAL ? "" : ", clusters and GPU capped");
					mif_prof = prof;
					mif_efficiency = eff;
					dlog("efficiency: %s", eff ? "on" : "off");
				} else if (khz != 0) {
					static uint32_t told;

					if (told++ < 5) {
						dlog("power: memory %u MHz NOT taken (profile %d): tried again next tick", khz / 1000, prof);
					}
				}
			}
		}
		/* Fixed-clock boot overrides must honor the same runtime caps as the governor. */
		for (unsigned g = 0; mode >= 2 && g < NGOV; g++) {
			if (govern && (gov[g].ncpus || govern_gpu)) {
				continue;
			}
			for (unsigned d = 0; d < NDOMAINS; d++) {
				if (domains[d].domain != gov[g].domain || !targets[d]) {
					continue;
				}
				uint32_t cap = level[d] == 2 ? domains[d].safe : level[d] == 1 ? domains[d].mid : targets[d];
				uint32_t khz = profile_cap(d, cap < targets[d] ? cap : targets[d], power_profile());

				(void)governor_set_rate(domains[d].domain, khz, &gov_sent[g]);
			}
		}
		/* the governor: every tick */
		if (gov_frame && redwood_fb_swap_at != 0) {
			uint64_t hold;

			nanoseconds_to_absolutetime(FRAME_HOLD_MS * NSEC_PER_MSEC, &hold);
			gov_frame_ticks += (mach_absolute_time() - redwood_fb_swap_at < hold) ? step : 0;
		}
		for (unsigned g = 0; govern && mode >= 2 && g < NGOV; g++) {
			unsigned d = 0, top = GOV_RUNGS - 1, want;
			int busy;
			uint32_t cap, khz;

			for (d = 0; d < NDOMAINS && domains[d].domain != gov[g].domain; d++) {
			}
			if (d == NDOMAINS || targets[d] == 0) {
				continue;
			}
			while (top > 0 && gov[g].rung[top] == 0) {
				top--;
			}
			if (gov[g].ncpus == 0 && !govern_gpu) {
				continue;
			}
			busy = gov_busy(g);
			want = gov_rung[g];
			/* Missing utilization must not skip a thermal/profile cap. Preserve
			 * the requested rung and still apply ceilings below. */
			if (busy >= (int)gov[g].jump) {
				want = top;
				gov_low[g] = 0;
			} else if (busy >= 0) {
				/* the lowest rung that would leave it at most `aim` % busy */
				uint32_t current = efficiency_active(domains[d].domain) && gov_sent[g] ?
				    gov_sent[g] : gov[g].rung[gov_rung[g]];
				uint64_t need = (uint64_t)current * (uint64_t)busy / gov[g].aim;
				unsigned fit = 0;

				while (fit < top && gov[g].rung[fit] < need) {
					fit++;
				}
				if (fit > gov_rung[g]) {
					want = fit;
					gov_low[g] = 0;
				} else if (fit < gov_rung[g] && ++gov_low[g] >= GOV_DOWN_TICKS) {
					want = gov_rung[g] - 1;
					gov_low[g] = 0;
				}
			}
			if (want < gov_touch_floor(g)) {
				want = gov_touch_floor(g);
				gov_low[g] = 0;
			}
			if (want < gov_min_rung(g)) {
				want = gov_min_rung(g);
			}
			gov_last_busy[g] = busy;
			gov_rung[g] = want;
			cap = level[d] == 2 ? domains[d].safe : level[d] == 1 ? domains[d].mid : targets[d];
			cap = profile_cap(d, cap, power_profile());
			khz = gov[g].rung[want];
			khz = khz > targets[d] ? targets[d] : khz;
			khz = cap != 0 && khz > cap ? cap : khz;
			gov_khz_sum[g] += (uint64_t)khz * step;
			if (khz != gov_sent[g]) {
				if (governor_set_rate(domains[d].domain, khz, &gov_sent[g]) < 0) {
					dlog("%s: %u kHz was not taken", domains[d].name, khz);
				} else {
					gov_changes++;
				}
			}
		}
		if (mode >= 2) {
			bigpark_check(gov_last_busy[1], gov_last_busy[2]);
		}
		/* the temperatures and the thermal steps: every 250 ms (once a second was too slow for the M5 cores), and every
		 * tick (50 ms) once a zone is at 80 C or more -- Samsung's IPA polls every 50 ms while hot (A, 2026-11-20) */
		static int therm_fast;
		if (!therm_fast && tick % (250 / GOV_TICK_MS) != 0) {
			continue;
		}
		hot = 0;
		therm_fast = 0;
		for (unsigned z = 0; z < 4; z++) {
			temp[z] = ch_tmu.len != 0 ? read_temp(z) : -1;
			if (temp[z] >= 80) {
				therm_fast = 1;
			}
		}
		thermal_pressure_sample(temp);
		heat_stop_check(temp);
		if (mode >= 2) {
			bigoff_check(temp[0]);          /* zones[0] = "big" */
			if (dyncap) {
				uint32_t m5 = m5_max_khz, g3d = gpu_max_khz;

				dyn_apply(&dyn_m5, temp[0], 2, &m5_max_khz);     /* gov[2] = M5, zone big */
				dyn_apply(&dyn_gpu, temp[3], 3, &gpu_max_khz);   /* gov[3] = GPU, zone g3d */
				if (m5 != m5_max_khz || g3d != gpu_max_khz) {
					static uint32_t told;
					if (told++ < 200) {
						dlog("dyncap: big %d C g3d %d C: M5 <= %u, GPU <= %u MHz", temp[0], temp[3],
						    m5_max_khz / 1000, gpu_max_khz / 1000);
					}
				}
			}
            uint64_t gpu_ns;
            absolutetime_to_nanoseconds(mach_absolute_time(), &gpu_ns);
            uint32_t previous = gpu_policy_cap;
            gpu_policy_cap = redwood_gpu_policy_update(&gpu_policy, temp[3],
                govern && govern_gpu ? gov_last_busy[3] : -1,
                gpu_ns / NSEC_PER_MSEC, gpu_ladder, gpu_sustain);
            if (previous != gpu_policy_cap) {
                dlog("GPU policy: %d C, busy %d: ceiling %u MHz", temp[3],
                    gov_last_busy[3], gpu_policy_cap / 1000);
            }
		}
		/* Reuse the four samples above: no extra reads, thread, timer or log polling.
		 * Decisions affect the next governor tick; stricter thermal/profile caps still win. */
		if (mode >= 2) {
			uint64_t ns;
			unsigned previous = os_atomic_load(&efficiency_mask, relaxed), mask = 0;

			if (os_atomic_load(&efficiency_auto, relaxed)) {
				absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
				mask = redwood_cooling_mask(cooling, temp, ns / NSEC_PER_SEC);
			} else {
				memset(cooling, 0, sizeof(cooling));
			}
			os_atomic_store(&efficiency_mask, mask, relaxed);
			if (mask != previous) {
				dlog("automatic cooling: mask %#x -> %#x (A55=1 A76=2 M5=4 GPU=8)", previous, mask);
			}
		}
		if (fb_hot != 0) {
			extern void redwood_fb_thermal_cap(uint32_t nits);
			static const uint32_t cap_nits[3] = { 0, 294, 210 };        /* none, 70 %, 50 % of the panel's 420 */
			int t = -1, want = bl_step;

			for (unsigned z = 0; z < 4; z++) {
				t = temp[z] > t ? temp[z] : t;
			}
			/* as the clusters' steps: taken at the mark, let go 4 degrees below it */
			if (t < 0) {
				want = bl_step;
			} else if (t >= (int)fb_hot + 6) {
				want = 2;
			} else if (t >= (int)fb_hot && want < 1) {
				want = 1;
			} else if (want == 2 && t < (int)fb_hot + 2) {
				want = t >= (int)fb_hot - 4 ? 1 : 0;
			} else if (want == 1 && t < (int)fb_hot - 4) {
				want = 0;
			}
			if (want != bl_step) {
				bl_step = want;
				redwood_fb_thermal_cap(cap_nits[want]);
				dlog("display: hottest zone %d C: brightness %s", t, want == 0 ? "as asked again" : want == 1 ?
				    "held under 70 %" : "held under 50 %");
			}
		}
		for (unsigned i = 0; mode >= 2 && i < NDOMAINS; i++) {
			int z = domains[i].zone, t, s1, s2, want;
			uint32_t khz;

			if (z < 0 || targets[i] == 0 || (t = temp[z]) < 0) {
				continue;
			}
			s1 = (hot_at ? (int)hot_at : domains[i].step1) + perf_therm_offset;
			s2 = (hot_at ? (int)hot_at + 8 : domains[i].step2) + perf_therm_offset;
			want = level[i];
			if (t >= s2) {
				want = 2;
			} else if (t >= s1 && want < 1) {
				want = 1;
			} else if (want == 2 && t < s2 - 8) {
				want = t >= s1 - 8 ? 1 : 0;
			} else if (want == 1 && t < s1 - 8) {
				want = 0;
			}
			if (want != level[i]) {
				khz = want == 2 ? domains[i].safe : want == 1 ? (domains[i].mid < targets[i] ? domains[i].mid : targets[i]) : targets[i];
				level[i] = want;
				/* (a governed cluster gets its clock from the governor, capped by this step, next tick) */
				bool governed = govern && (domains[i].domain == D_CL0 || domains[i].domain == D_CL1 ||
				    domains[i].domain == D_CL2 || (domains[i].domain == D_G3D && govern_gpu));
				if (khz != 0 && !governed && set_rate(domains[i].domain,
				    profile_cap(i, khz < targets[i] ? khz : targets[i], power_profile())) != 0) {
					dlog("%s: %u kHz was not taken", domains[i].name, khz);
				}
				dlog("%s zone %s %d C: step %d, %u MHz", domains[i].name, zones[z], t, want, khz / 1000);
			}
			hot |= level[i] != 0;
		}
		if (tick % (10000 / GOV_TICK_MS) != 0) {
			continue;
		}
		if (govern && mode >= 2) {
			unsigned n = 10000 / GOV_TICK_MS;

			dlog("governor: 10 s: %u clock changes (%u raised the moment a finger came down, of %u touches; the GPU "
			    "raised to its top %u times by %u jobs over 8 ms); average cl0 %llu cl1 %llu cl2 %llu g3d %llu MHz; "
			    "frames held the floor %u %% of the time (dvfs_frame=%u)",
			    gov_changes, gov_boosts, gov_touches, gov_slow_raises, redwood_mali_slow_jobs - gov_slow_last,
			    gov_khz_sum[0] / n / 1000, gov_khz_sum[1] / n / 1000, gov_khz_sum[2] / n / 1000, gov_khz_sum[3] / n / 1000,
			    gov_frame_ticks * 100 / n, gov_frame);
			gov_boosts = gov_touches = gov_slow_raises = 0;
			gov_frame_ticks = 0;
			gov_slow_last = redwood_mali_slow_jobs;
			gov_changes = 0;
			for (unsigned g = 0; g < NGOV; g++) {
				gov_khz_sum[g] = 0;
			}
		}
		{
			extern uint64_t redwood_user_cache_ops, redwood_user_cache_flushes;
			static uint64_t last_ops, last_flushes;
			uint64_t ops = redwood_user_cache_ops, flushes = redwood_user_cache_flushes;

			extern uint64_t redwood_user_cache_small;
			static uint64_t last_small;
			uint64_t small = redwood_user_cache_small;

			dlog("user cache maintenance done for user code (trapped, osfmk/arm64/sleh.c): %llu in 10 s, of them "
			    "%llu whole sys_icache_invalidate ranges (every core's instruction cache; %llu line by line through "
			    "the kernel's alias, the rest IC IALLU)", ops - last_ops, flushes - last_flushes, small - last_small);
			last_small = small;
			last_ops = ops;
			last_flushes = flushes;
		}
		{
			struct cpuidle_count { uint64_t entered, resumed, returned, refused, skipped, slept_abs; };
			extern struct cpuidle_count redwood_cpuidle_counts[8];
			static struct cpuidle_count last[8];
			char line[512];
			size_t at = 0;
			uint64_t any = 0;

			at += snprintf(line + at, sizeof(line) - at, "cpuidle 10 s, per core powered off (asleep ms) / came "
			    "back at once / refused / too soon:");
			for (unsigned c = 0; c < 8; c++) {
				struct cpuidle_count *n = &redwood_cpuidle_counts[c], *l = &last[c];
				uint64_t ns;

				absolutetime_to_nanoseconds(n->slept_abs - l->slept_abs, &ns);
				any |= n->entered - l->entered;
				if (at >= sizeof(line)) {
					break;
				}
				at += snprintf(line + at, sizeof(line) - at, " %u:%llu(%llu)/%llu/%llu/%llu", c, n->resumed - l->resumed,
				    ns / NSEC_PER_MSEC, n->returned - l->returned, n->refused - l->refused, n->skipped - l->skipped);
				*l = *n;
			}
			if (any) {
				extern void redwood_wake_report(void);

				dlog("%s", line);
				redwood_wake_report();
			}
			{
				extern uint32_t gic_resume_seen[][2];
				static uint8_t told;

				for (unsigned c = 0; c < 8; c++) {
					if ((gic_resume_seen[c][0] & (1u << 31)) && !(told & (1u << c))) {
						told |= 1u << c;
						dlog("cpuidle: core %u's first wake: its GIC had GICC_CTLR %#x PMR %#x ISENABLER0 %#x "
						    "(before they were set again)", c, (gic_resume_seen[c][0] >> 16) & 0x7fff,
						    gic_resume_seen[c][0] & 0xffff, gic_resume_seen[c][1]);
					}
				}
			}
		}
		report(hot ? "every 10 s (throttled)" : "every 10 s");
	}
}

void redwood_exynos_acpm_init(void);

void
redwood_exynos_acpm_init(void)
{
	uint32_t mode = 0;
	thread_t thread;

	if (!PE_parse_boot_argn("dvfs", &mode, sizeof(mode)) || mode < 1 || mode > 2) {
		return;
	}
	if (kernel_thread_start(acpm_thread, NULL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
}
