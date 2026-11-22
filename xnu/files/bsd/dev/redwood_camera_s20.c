/*
 * Galaxy S20 (x1slte, Exynos 990) camera bring-up.
 *
 * cam.s20 is a read-only pin inventory; cam.s20.id is a bounded rear-main
 * identity read with power, clock, pin and bus-state cleanup. This is separate
 * from the Qualcomm camera path.
 */
#include <kern/clock.h>
#include <kern/kalloc.h>
#include <kern/locks.h>
#include <sys/sysctl.h>
#include <sys/kauth.h>
#include <sys/errno.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <pexpert/pexpert.h>
#include <sys/redwood_bbi2c.h>
#include <sys/redwood_audio.h>
#include <sys/redwood_dbgport.h>
#include <sys/systm.h>
#include "redwood_camera_s20_controls.h"
#include "redwood_camera_s20_aux_power.h"
#include "redwood_camera_s20_gw2_power.h"
#include "redwood_camera_s20_gw2.h"
#include "redwood_camera_s20_gw2_table.h"
#include "redwood_camera_s20_main_voltage.h"
#include "redwood_camera_s20_aux_table.h"
#include "redwood_camera_s20_2la.h"
#include "redwood_camera_s20_2la_controls.h"
#include "redwood_camera_s20_3j1.h"
#include "redwood_camera_s20_2ld.h"
#include "redwood_camera_s20_2ld_ret.h"

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
#include "redwood_camera_s20_map_cache.h"
static LCK_GRP_DECLARE(s20_map_group,"s20_camera_register_maps");
static LCK_MTX_DECLARE(s20_map_lock,&s20_map_group);
static struct s20_map_cache s20_register_maps;
static uintptr_t s20_map_new(void *context,uint64_t physical,uint64_t bytes) {
    (void)context;return (uintptr_t)ml_io_map((vm_offset_t)physical,(vm_size_t)bytes);
}
static vm_offset_t cam_s20_io_map(vm_offset_t physical,vm_size_t bytes) {
    lck_mtx_lock(&s20_map_lock);
    vm_offset_t address=(vm_offset_t)s20_map_get(&s20_register_maps,physical,bytes,s20_map_new,NULL);
    lck_mtx_unlock(&s20_map_lock);return address;
}

extern bool redwood_s2mpb02_camera_read(uint8_t reg, uint8_t *value);
extern bool redwood_s2mpb02_camera_bits(uint8_t reg, uint8_t mask, uint8_t value);
extern bool redwood_s2mpb02_front_voltages(uint8_t saved[4], bool restore);
extern bool redwood_s2mpb02_main_voltages(struct cam_main_voltage *,bool,bool *,void *,
    bool(*)(void *,uint8_t,uint8_t *),bool(*)(void *,uint8_t,uint8_t));
extern int redwood_spmic_read(uint8_t type, uint8_t reg, uint8_t *value);

#define PINCTRL_PERIC1_GPC0       0x0a0U
#define PINCTRL_CMGP_GPM1         0x020U
#define PINCTRL_CMGP_GPM11        0x160U
#define PINCTRL_CMGP_GPM22        0x2c0U
#define PINCTRL_DRV               0x0cU
#define PINCTRL_PERIC0_PHYS       0x10430000U
#define PINCTRL_PERIC1_PHYS       0x10730000U
#define PINCTRL_CMGP_PHYS         0x15c30000U
#define PINCTRL_CMGP_GPM0         0x000U
#define CMU_TOP_PHYS              0x1a330000U
#define CMU_CSIS_PHYS             0x17000000U
#define PMU_PHYS                  0x15860000U
#define CSIS1_DPHY_PHYS           0x170d2b00U
#define CSIS1_DPHY_BIAS_PHYS      0x170d1000U
#define ISPPRE_SYSREG_PHYS        0x17020500U
#define SMC_PREPARE_PD_ONOFF      0x82000410ULL
#define CSIS_NEED_SMC             0x17010204ULL
#define CSIS1_PHYS                0x17040000U
#define CSIS1_DMA1_VC0_PHYS       0x170c2000U
#define CSIS1_DMA1_COMMON_PHYS    0x170c2400U
#define CMU_CIS_CLK0_MUX          0x1018U
#define CMU_CIS_CLK0_DIV          0x1814U
#define CMU_CIS_CLK0_GATE         0x201cU
#define CMU_CIS_CLK0_QCH          0x3004U
#define CMU_CIS_CLK1_MUX          0x101cU
#define CMU_CIS_CLK1_DIV          0x1818U
#define CMU_CIS_CLK1_GATE         0x2020U
#define CMU_CIS_CLK1_QCH          0x3008U
#define CAM_S2MPB03_ADDR          0x56U
#define CAM_S2MPB03_LDO2          0x04U /* VDDD_1.05V_CAM */
#define CAM_S2MPB03_LDO3          0x05U /* VDDIO_1.8V_CAM */
#define CAM_S2MPB03_LDO5          0x07U /* VDDA_2.8V_CAM */
#define CAM_S2MPB02_BUCK2         0x0dU
#define CAM_S2MPB02_LDO1          0x1eU /* VDDPHY_1.05V_CAM */
#define CAM_S2MPB02_LDO5          0x22U
#define CAM_S2MPB02_LDO6          0x23U
#define CAM_S2MPB02_LDO11         0x28U
#define CAM_S2MPB02_LDO12         0x29U /* VDDA_2.8V_WIDESUB, not a front-camera supply */
#define CAM_S2MPB02_LDO14         0x2bU /* VDDAF_COMMON_CAM */
#define CAM_S2MPB02_LDO16         0x2dU

static struct redwood_bbi2c cam_pmic03_bus = {
	.name = "S20 camera S2MPB03 gpm0-0/gpm1-0", .scl = 0, .sda = 0, .half_us = 4
};

static uint32_t
cam_s20_rd(volatile uint8_t *bank, uint32_t offset)
{
	uint32_t value = *(volatile uint32_t *)(bank + offset);
	__asm__ volatile ("dsb ld" ::: "memory");
	return value;
}

static void
cam_s20_wr(volatile uint8_t *bank, uint32_t offset, uint32_t value)
{
	/* Match Linux writel()/readl() ordering across reset, PHY and CSI blocks. */
	__asm__ volatile ("dsb st" ::: "memory");
	*(volatile uint32_t *)(bank + offset) = value;
}

static bool
cam_s20_pmic03_read(uint8_t reg, uint8_t *value)
{
	return redwood_bbi2c_read_reg(&cam_pmic03_bus, CAM_S2MPB03_ADDR, reg, value, 1);
}

static void
cam_s20_wait_ms(unsigned ms)
{
	delay_for_interval(ms, NSEC_PER_MSEC);
}

static bool main_voltage_read03(void *,uint8_t,uint8_t *);
static bool main_voltage_write03(void *,uint8_t,uint8_t);
static bool
cam_s20_pmic03_bits(uint8_t reg,uint8_t before,uint8_t value)
{
    (void)before; /* Snapshot only selects the requested enable bit, not live selectors. */
    return cam_main_enable_bits((struct cam_main_enable_io){NULL,main_voltage_read03,main_voltage_write03},reg,value&0x80);
}

static bool main_voltage_read03(void *ctx,uint8_t reg,uint8_t *value) {
    (void)ctx;return (reg==CAM_S2MPB03_LDO2||reg==CAM_S2MPB03_LDO3||reg==CAM_S2MPB03_LDO5)&&cam_s20_pmic03_read(reg,value);
}
static bool main_voltage_write03(void *ctx,uint8_t reg,uint8_t value) {
    (void)ctx;return (reg==CAM_S2MPB03_LDO2||reg==CAM_S2MPB03_LDO3||reg==CAM_S2MPB03_LDO5)&&
        redwood_bbi2c_write_reg(&cam_pmic03_bus,CAM_S2MPB03_ADDR,reg,value);
}
static bool main_voltages(struct cam_main_voltage *saved,bool restore) {
    bool clean=true,ok=redwood_s2mpb02_main_voltages(saved,restore,&clean,NULL,main_voltage_read03,main_voltage_write03);
    if(!clean)printf("camera: primary voltage rollback failed; PMIC access blocked\n");
    return ok&&clean;
}

static uint64_t
cam_s20_smc4(uint64_t function, uint64_t a1, uint64_t a2, uint64_t a3)
{
	register uint64_t x0 __asm__("x0") = function;
	register uint64_t x1 __asm__("x1") = a1;
	register uint64_t x2 __asm__("x2") = a2;
	register uint64_t x3 __asm__("x3") = a3;

	__asm__ volatile ("smc #0" : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3) : :
	    "x4", "x5", "x6", "x7", "x8", "x9", "x10", "x11", "x12", "x13", "x14", "x15", "x16", "x17", "memory");
	return x0;
}

/*
 * One bounded rear-main identification attempt, callable only through root-only kern.redwood.poke.
 * The x1slte overlay routes S5K2LD@0x10 through HSI2C3/gpp0-6/7. Its camera rails span S2MPB03
 * (LDO2/3/5) and S2MPB02 (BUCK2/LDO1/LDO14). Only enable bits are changed; all originals are
 * restored even after a NACK or failed readback. No sensor configuration register is written.
 */
static bool cam_3j1_write16(struct redwood_bbi2c *bus, uint16_t reg, uint16_t value);
static bool cam_3j1_read8(struct redwood_bbi2c *bus, uint16_t reg, uint8_t *value);
static bool cam_3j1_write8(struct redwood_bbi2c *bus, uint16_t reg, uint8_t value);
static bool cam_3j1_read16(struct redwood_bbi2c *bus, uint16_t reg, uint16_t *value);
static bool cam_3j1_apply(struct redwood_bbi2c *bus, const uint32_t *table, uint32_t count);
static uint64_t cam_s20_smc4(uint64_t fn, uint64_t a, uint64_t b, uint64_t c);

#define CSIS0_PHYS                0x17030000U
#define CSIS0_DCPHY_PHYS          0x170d1300U

/*
 * The rear main's receiver (2026-11-12): Samsung's __set_phy_cfg_0503_0000_dcphy() C-PHY branch for the csi0 combo
 * PHY, 3 trios, settle 7 (its 7nm C-PHY table at 2964 Msps; the same table's D-PHY 1807 Mbps value, 9, is what the
 * front runs on). No clock lane in C-PHY.
 */
static void
cam_s20_cphy_2ld_config(volatile uint8_t *phy, volatile uint8_t *bias)
{
	unsigned i;

	for (i = 0; i < 3; i++) cam_s20_wr(phy, 0x0100 + i * 0x100, 0);
	delay_for_interval(200, NSEC_PER_USEC);
	cam_s20_wr(bias, 0x0000, 0x10);
	cam_s20_wr(bias, 0x0004, 0x110);
	cam_s20_wr(bias, 0x0008, 0x3223);
	cam_s20_wr(bias, 0x0010, 0x40);
	for (i = 0; i < 3; i++) {
		uint32_t b = 0x0100 + i * 0x100;

		cam_s20_wr(phy, b + 0x04, 0x1450);
		cam_s20_wr(phy, b + 0x08, 0x9);
		cam_s20_wr(phy, b + 0x0c, 0x82c8);
		cam_s20_wr(phy, b + 0x10, 0x1);
		cam_s20_wr(phy, b + 0x14, 0x8680);
		cam_s20_wr(phy, b + 0x18, 0x4000);
		cam_s20_wr(phy, b + 0x1c, 0x200);
		cam_s20_wr(phy, b + 0x20, 0x638);
		cam_s20_wr(phy, b + 0x24, 0x40);
		cam_s20_wr(phy, b + 0x30, (cam_s20_rd(phy, b + 0x30) & ~0x1ffU) | 7U);  /* settle 7, settle_clk_sel 0 */
		cam_s20_wr(phy, b + 0x34, 0x32);
		cam_s20_wr(phy, b + 0x40, cam_s20_rd(phy, b + 0x40) & ~1U);
		cam_s20_wr(phy, b + 0x64, 0x1503);
		cam_s20_wr(phy, b + 0x68, 0x33);
		cam_s20_wr(phy, b, 1);
	}
	delay_for_interval(200, NSEC_PER_USEC);
}

/* the session streams the rear main (S5K2LD over C-PHY/CSIS0/WDMA2) instead of the front: REDWOOD_CAMERA_SENSOR mode 2 */
static bool cam_s20_rear;
static bool cam_s20_gw2_now;
void redwood_camera_s20_set_gw2(bool enabled){cam_s20_gw2_now=enabled;}
void redwood_camera_s20_set_rear(bool rear) { cam_s20_rear = rear; }
/*
 * The rear main in its full 4032x3024 mode (Samsung's setA A0: what the stock camera shows and shoots, 2964 Msps
 * like the binned mode, 12160 lines of 4240 clocks at 30 fps), not the 2x2-binned 2016x1512 (A10, a speed mode).
 * Binned, the picture was 1008x756 colour cells blown up to the 1504x1128 preview: soft, and every frame went
 * through libredwood-media's slow "magnified" path (11-21 fps, 2026-11-12). cam_s20_rear_full=0: the binned mode.
 */
/* Selected only by ios_select between sessions; never changed by a live sysctl.
 * -1 retains the boot profile; 0/1 explicitly select binned/full rear. */
static int cam_s20_rear_mode_override = -1;
void redwood_camera_s20_select_rear_mode(int full) { cam_s20_rear_mode_override = full; }
bool
redwood_camera_s20_rear_full(void)
{
	static int full = -1;
	if (cam_s20_rear_mode_override >= 0) return cam_s20_rear_mode_override != 0;

	if (full < 0) {
		/* OFF (2026-11-12): in this mode the sensor's RAW10 is no plain Bayer -- even rows carry light in 1 pixel of 4,
		 * odd rows in 3 of 4 (laid out for Samsung's ISP: dual-pixel/PDAF), a green-red-blue checkerboard here */
		uint32_t v = 0;   /* OFF (2026-11-12, user: binned was better): 12 MP has a sensor black band at row 1912 and a 32-row structure; 1 = 12 MP */

		(void)PE_parse_boot_argn("cam_s20_rear_full", &v, sizeof(v));
		full = v != 0;
	}
	return full != 0;
}


/*
 * The front camera's lens motor: an AK737x (AK7374/AK7377, product ID at 0xf3) at 0x0c on HSI2C24, which x1slte's
 * device tree (fragment@18) puts on gpp8-0 (SCL) and gpp8-1 (SDA); its supply is VDDAF_2.8V_VT (S2MPB02 LDO16), on
 * with the sensor's. Samsung's is-actuator-ak737x.c: CONT1 (0x02) 0x40 standby, 0x00 active, 0x20 sleep; the
 * position is 9 bits, 0x00 = bits 8..1 and 0x01 bit 7 = bit 0; first position 250 (vendor_first_pos), 2 ms to
 * settle; soft landing 100, 30 ms, 0, 30 ms, then sleep. Driven only from the stream thread.
 */
static struct redwood_bbi2c cam_s20_af_bus = { .name = "AK737x HSI2C24 gpp8-0/1", .scl = 0, .sda = 1, .half_us = 4 };
static bool cam_s20_af_ready;
static uint8_t cam_s20_af_address=0x0c;
static volatile uint32_t cam_s20_af_pos = 0xffffffffU;    /* 0..511 while the motor is up */
static volatile uint32_t cam_s20_af_want;
static volatile bool cam_s20_af_pending;

static bool
cam_s20_af_w(uint8_t reg, uint8_t value)
{
	return redwood_bbi2c_write_reg(&cam_s20_af_bus, cam_s20_af_address, reg, value);
}

static bool
cam_s20_af_set(uint32_t p)
{
	p = p > 511 ? 511 : p;
	return cam_s20_af_w(0x00, (uint8_t)(p >> 1)) && cam_s20_af_w(0x01, (uint8_t)((p & 1U) << 7));
}

static bool
cam_s20_af_up_for(bool rear,bool gw2)
{
    cam_s20_af_address=gw2?0x0f:0x0c;cam_s20_af_pending=false;
	char report[160];
	uint8_t id = 0;

	/*
	 * The rear main's lens motor is an AK737x too, at 0x0c on HSI2C0 (x1slte fragment@17 is-actuator@0C, id 0):
	 * gpp0-0 SCL, gpp0-1 SDA (pinctrl hsi2c0-bus) -- 2026-11-12.
	 */
	if (rear) {
		cam_s20_af_bus.name = "AK737x HSI2C0 gpp0-0/1 (rear)";
		cam_s20_af_bus.bank = redwood_gpio_bank(PINCTRL_PERIC0_PHYS, 0x00);  /* gpp0 */
	} else {
		cam_s20_af_bus.name = "AK737x HSI2C24 gpp8-0/1";
		cam_s20_af_bus.bank = redwood_gpio_bank(PINCTRL_PERIC1_PHYS, 0x60);  /* gpp8 */
	}
	if (cam_s20_af_bus.bank == NULL || !redwood_bbi2c_setup(&cam_s20_af_bus, true, report, sizeof(report))) {
		printf("camera: %s focus: its bus cannot be used (%s)\n", gw2?"GW2":rear?"rear":"front", report);
		return false;
	}
	if (!cam_s20_af_w(0x02, 0x40)) goto fail;
	cam_s20_wait_ms(1);
	if (!redwood_bbi2c_read_reg(&cam_s20_af_bus, cam_s20_af_address, 0xf3, &id, 1)) goto fail;
	if (!cam_s20_af_set(250)) goto fail;
	cam_s20_wait_ms(2);
	if (!cam_s20_af_w(0x02, 0x00)) goto fail;
	cam_s20_af_ready = true;
	cam_s20_af_pos = 250;
	printf("camera: %s focus: AK737x (id %#x) up, the lens at 250 of 511\n", gw2?"GW2":rear?"rear":"front", id);
	return true;
fail:
	printf("camera: %s focus: the AK737x does not answer at 0x0c; no focus in this session\n", gw2?"GW2":rear?"rear":"front");
	redwood_bbi2c_restore(&cam_s20_af_bus);
	return false;
}

static bool cam_s20_af_up(void){return cam_s20_af_up_for(cam_s20_rear,cam_s20_gw2_now);}

static void
cam_s20_af_down(void)
{
	if (!cam_s20_af_ready) return;
	(void)cam_s20_af_set(100);
	cam_s20_wait_ms(30);
	(void)cam_s20_af_set(0);
	cam_s20_wait_ms(30);
	(void)cam_s20_af_w(0x02, 0x20);
	redwood_bbi2c_restore(&cam_s20_af_bus);
	cam_s20_af_ready = false;
	cam_s20_af_pos = 0xffffffffU;
}

/* the bounded rear test's exposure (cam.s20.dma.test 2 [lines]): 16384 lines at 4x; under 16 lines it is 1x -- a black frame,
 * the sensor's black level per site (2026-11-12) */
static uint32_t cam_s20_rear_test_lines = 16384;
static int cam_s20_rear_i2c_half_us=1;
static bool cam_s20_rear_slow;   /* the fast rear bus failed once this boot: half_us 2 from then on */

/*
 * The rear main kept warm between sessions (2026-11-12): its rails, clock and reset stay as they are and the sensor
 * waits in software standby, so the next start sends only the mode table -- the 7354-record global table over
 * bit-banged I2C was most of the ~3.1 s every start and flip took. redwood_camera_s20_rear_cool() (the camera
 * thread, 20 s after the last session) takes it down as a session's end used to. cam_s20_rear_warm=0: never warm.
 */
/* Rear diagnostic options: bit0 stock-style no hold, bit1 force cold sessions.
 * Root-only writes; each session takes one snapshot. Default zero preserves behavior. */
static int cam_s20_rear_diag;
void redwood_camera_s20_rear_cool(void);
static bool cam_s20_rear_warm;
static struct {
	uint8_t p02[3], p03[3];
    struct cam_main_voltage voltage;
	uint32_t reset_con, reset_dat, reset_pud, mclk_con, mclk_dat, mclk_pud, clock[4];
} cam_s20_rear_cold;
bool redwood_camera_s20_rear_is_warm(void) { return cam_s20_rear_warm; }

/* (the stream's state, here so the rear's loop in cam_s20_identify sees it; see redwood_camera_s20_stream) */
typedef bool (*redwood_s20_frame_fn)(const volatile uint8_t *data, uint32_t width, uint32_t height, uint32_t stride);
static redwood_s20_frame_fn cam_s20_stream_fn;
static const volatile uint8_t *cam_s20_stream_buf[2];
static volatile uint32_t cam_s20_exp_lines, cam_s20_exp_gain, cam_s20_exp_frame;
static volatile bool cam_s20_exp_pending;
static uint32_t cam_s20_stream_frames, cam_s20_stream_errors, cam_s20_stream_bytes;
static bool cam_s20_stream_cached;
static bool cam_s20_stream_exposure_failed;
static volatile uint8_t *cam_s20_optical_dma;

static bool
cam_s20_exposure_write16(void *context, uint16_t reg, uint16_t value)
{
    return cam_3j1_write16(context, reg, value);
}

static bool
cam_s20_exposure_write8(void *context, uint16_t reg, uint8_t value)
{
    return cam_3j1_write8(context, reg, value);
}

static void
cam_s20_identify(const uint64_t *v, unsigned n, char *out, size_t cap)
{
	/* v[0] 1: after the ID, Samsung's global A3 table, the 2016x1512 30 fps mode, stream on for ~0.5 s with the
	 * sensor's own frame counter read (no receiver involved), stream off -- does the rear sensor stream at all */
	bool stream_test = n > 0 && (v[0] == 1 || v[0] == 2);
	/* v[0] 2: also the receiver -- csi0 C-PHY and CSIS0 set up (Samsung's sequence), CSIS0's frame counter and
	 * status read while the sensor streams; every PHY/CSIS/CMU/PMU register written is put back */
	bool rx_test = n > 0 && v[0] == 2, rx_saved = false;
	/* the iOS stream (cam_s20_stream_fn) in the full mode; the bounded tests keep the binned one they were measured in */
	const bool rear_full_now = cam_s20_stream_fn != NULL && redwood_camera_s20_rear_full();
	const uint32_t cam_s20_rear_w = rear_full_now ? 4032U : 2016U, cam_s20_rear_h = rear_full_now ? 3024U : 1512U;
	char rx_report[240] = "";
	volatile uint8_t *rx_csi = NULL, *rx_phy = NULL, *rx_bias = NULL, *rx_cmu = NULL, *rx_pmu = NULL, *rx_sys = NULL;
	uint32_t rx_csi_saved[12] = { 0 }, rx_phy_saved[3][16] = { { 0 } }, rx_bias_saved[4] = { 0 }, rx_qch = 0, rx_iso = 0, rx_rst = 0;
	uint32_t rx_bus_mux = 0, rx_bus_div = 0;
	bool rx_bus_changed = false;
	uint32_t rx_fc_before = 0, rx_fc_after = 0, rx_irq = 0, rx_irq_or = 0, rx_phy_or = 0, rx_phy_and = ~0U, rx_cmn = 0;
	static const uint32_t rx_csi_offs[12] = { 0x0004, 0x0008, 0x000c, 0x0010, 0x0018, 0x0040, 0x0044, 0x0050, 0x0054, 0x0600, 0x0690, 0x0704 };
	static const uint32_t rx_phy_offs[16] = { 0x00, 0x04, 0x08, 0x0c, 0x10, 0x14, 0x18, 0x1c, 0x20, 0x24, 0x30, 0x34, 0x40, 0x64, 0x68, 0x00 };
	uint8_t fc[4] = { 0 };
	char stream_phase[48] = "";
	/* half_us 1 (2026-11-12; was 2, and 4 before): the 7354-record global table is ~180 000 bits, and the rear
	 * still took ~3 s from a flip to its first frame at 2. The S5K2LD takes fast-mode I2C (400 kHz); the pin
	 * writes themselves keep a half period well above the 0.6 us fast mode wants. cam_s20_rear_i2c=N sets it. */
	struct redwood_bbi2c sensor_bus = { .name = "S20 rear main HSI2C3 gpp0-6/7", .scl = 6, .sda = 7, .half_us = (uint32_t)__atomic_load_n(&cam_s20_rear_i2c_half_us,__ATOMIC_RELAXED) };
	volatile uint8_t *reset = redwood_gpio_bank(PINCTRL_CMGP_PHYS, PINCTRL_CMGP_GPM22);
	volatile uint8_t *mclk = redwood_gpio_bank(PINCTRL_PERIC1_PHYS, PINCTRL_PERIC1_GPC0);
	volatile uint8_t *cmu = (volatile uint8_t *)cam_s20_io_map(CMU_TOP_PHYS, 0x4000);
	uint8_t p02[3] = { 0 }, p03[3] = { 0 }, id[2] = { 0 }, rev[2] = { 0 };
	uint32_t reset_con = cam_s20_rd(reset, REDWOOD_GPIO_CON), reset_dat = cam_s20_rd(reset, REDWOOD_GPIO_DAT);
	uint32_t reset_pud = cam_s20_rd(reset, REDWOOD_GPIO_PUD), mclk_con = cam_s20_rd(mclk, REDWOOD_GPIO_CON);
	uint32_t mclk_dat = cam_s20_rd(mclk, REDWOOD_GPIO_DAT), mclk_pud = cam_s20_rd(mclk, REDWOOD_GPIO_PUD);
	uint32_t clock[4] = { 0 }, mask_reset = 0xfU, mask_mclk = 0xfU;
	char phase[40] = "S2MPB03 bus";
	char acked[96] = "";
	bool pmic03_ready = false, sensor_ready = false, clock_ready = false, rails02 = false, rails03 = false;
	bool ok = false, sensor_ack = false, warm_start = false, cold_again = false;
    struct cam_main_voltage voltage={0};
	unsigned diag = (unsigned)__atomic_load_n(&cam_s20_rear_diag, __ATOMIC_RELAXED);
	uint32_t clock_offsets[4] = { CMU_CIS_CLK0_MUX, CMU_CIS_CLK0_DIV, CMU_CIS_CLK0_GATE, CMU_CIS_CLK0_QCH };
	unsigned i;

	if (cap == 0) return;
	{
		uint32_t half = 0;

		if (PE_parse_boot_argn("cam_s20_rear_i2c", &half, sizeof(half)) && half > 0 && half <= 8) sensor_bus.half_us = half;
		if (cam_s20_rear_slow && sensor_bus.half_us < 2) sensor_bus.half_us = 2;
	}
	if (cmu == NULL) {
		(void)snprintf(out, cap, "{\"camera\":\"S5K2LD\",\"result\":\"error\",\"phase\":\"CMU map\"}\n");
		return;
	}
	if (diag & 2U) redwood_camera_s20_rear_cool();
	cam_pmic03_bus.bank = redwood_gpio_bank(PINCTRL_CMGP_PHYS, PINCTRL_CMGP_GPM0);
	cam_pmic03_bus.sda_bank = redwood_gpio_bank(PINCTRL_CMGP_PHYS, PINCTRL_CMGP_GPM1);
	pmic03_ready = true;
	if (!redwood_bbi2c_setup(&cam_pmic03_bus, false, out, cap)) goto cleanup;
	if (!cam_s20_pmic03_read(CAM_S2MPB03_LDO2, &p03[0]) || !cam_s20_pmic03_read(CAM_S2MPB03_LDO3, &p03[1]) ||
	    !cam_s20_pmic03_read(CAM_S2MPB03_LDO5, &p03[2])) goto cleanup;
	rails03 = true;
	if (!redwood_s2mpb02_camera_read(CAM_S2MPB02_BUCK2, &p02[0]) ||
	    !redwood_s2mpb02_camera_read(CAM_S2MPB02_LDO1, &p02[1]) ||
	    !redwood_s2mpb02_camera_read(CAM_S2MPB02_LDO14, &p02[2])) {
		(void)snprintf(phase, sizeof(phase), "S2MPB02 snapshot");
		goto cleanup;
	}
	rails02 = true;
	if (cam_s20_rear_warm) {
		/* what was there before the rear was first powered: what the real power-down goes back to */
		memcpy(p02, cam_s20_rear_cold.p02, sizeof(p02));
        voltage=cam_s20_rear_cold.voltage;
		memcpy(p03, cam_s20_rear_cold.p03, sizeof(p03));
		reset_con = cam_s20_rear_cold.reset_con; reset_dat = cam_s20_rear_cold.reset_dat; reset_pud = cam_s20_rear_cold.reset_pud;
		mclk_con = cam_s20_rear_cold.mclk_con; mclk_dat = cam_s20_rear_cold.mclk_dat; mclk_pud = cam_s20_rear_cold.mclk_pud;
		for (i = 0; i < 4; i++) clock[i] = cam_s20_rear_cold.clock[i];
		warm_start = true;
		goto warm_bus;
	}
cold_start:
    (void)snprintf(phase,sizeof(phase),"primary board voltages");
    if(!main_voltages(&voltage,false))goto cleanup;
	/* x1slte's per-module power sequence: shared 1.3 V first, reset low, then VDDIO/analog/PHY/core. */
	(void)redwood_gpio_set(reset, 0, 0);
	redwood_gpio_func(reset, 0, REDWOOD_GPIO_OUTPUT);
	redwood_gpio_pull(reset, 0, REDWOOD_GPIO_PULL_NONE);
	(void)snprintf(phase, sizeof(phase), "BUCK2 enable");
	if (!redwood_s2mpb02_camera_bits(CAM_S2MPB02_BUCK2, 0xc0, 0xc0)) goto cleanup;
	(void)snprintf(phase, sizeof(phase), "camera rails");
	if (!cam_s20_pmic03_bits(CAM_S2MPB03_LDO3, p03[1], 0x80)) goto cleanup;
	cam_s20_wait_ms(1);
	if (!cam_s20_pmic03_bits(CAM_S2MPB03_LDO5, p03[2], 0x80) ||
	    !redwood_s2mpb02_camera_bits(CAM_S2MPB02_LDO1, 0x80, 0x80) ||
	    !cam_s20_pmic03_bits(CAM_S2MPB03_LDO2, p03[0], 0x80) ||
	    !redwood_s2mpb02_camera_bits(CAM_S2MPB02_LDO14, 0x80, 0x80)) goto cleanup;
	cam_s20_wait_ms(2);
warm_bus:
	(void)snprintf(phase, sizeof(phase), "sensor I2C pins");
	sensor_ready = true;
	sensor_bus.bank = redwood_gpio_bank(PINCTRL_PERIC0_PHYS, 0);
	/* This bus's saved pin state carries a pull-down, unlike the DT's I2C pin state (no internal pull). */
	if (!redwood_bbi2c_setup(&sensor_bus, true, out, cap)) goto cleanup;
	if (warm_start) {
		/* (clock running, reset high, the sensor in standby: straight to its ID) */
		clock_ready = true;
		sensor_ack = true;
		goto read_id;
	}
	/* CIS_CLK0: OSCCLK (26 MHz), divider 1; save and restore the mux, divider, QCH and gate. */
	if (!cold_again) for (i = 0; i < 4; i++) clock[i] = cam_s20_rd(cmu, clock_offsets[i]);
	cam_s20_wr(cmu, CMU_CIS_CLK0_MUX, clock[0] & ~1U);
	cam_s20_wr(cmu, CMU_CIS_CLK0_DIV, clock[1] & ~0x1fU);
	cam_s20_wr(cmu, CMU_CIS_CLK0_QCH, clock[3] | 3U);
	cam_s20_wr(cmu, CMU_CIS_CLK0_GATE, clock[2] | (1U << 20) | (1U << 21));
	clock_ready = true;
	(void)redwood_gpio_set(mclk, 0, 0);
	redwood_gpio_func(mclk, 0, 2); /* DT sensor_mclk0_fn: gpc0-0 function 2 */
	redwood_gpio_pull(mclk, 0, REDWOOD_GPIO_PULL_NONE);
	cam_s20_wait_ms(2);
	(void)redwood_gpio_set(reset, 0, 1);
	cam_s20_wait_ms(10); /* Samsung's 3 ms minimum plus startup margin for a cold module */
	/* Bounded read-only address scan on the dedicated sensor bus. This also catches a shifted-address mistake. */
	(void)snprintf(phase, sizeof(phase), "S5K2LD address scan");
	for (i = 0x08; i < 0x78; i++) {
		if (redwood_bbi2c_xfer(&sensor_bus, (uint8_t)i, NULL, 0, NULL, 0)) {
			if (acked[0] != '\0') strlcat(acked, ",", sizeof(acked));
			{
				char address[8];
				(void)snprintf(address, sizeof(address), "%s0x%02x", acked[0] == '\0' ? "" : "", i);
				strlcat(acked, address, sizeof(acked));
			}
			if (i == 0x10) sensor_ack = true;
		}
	}
	if (!sensor_ack) goto cleanup;
read_id:
	(void)snprintf(phase, sizeof(phase), "S5K2LD model ID read");
	{
		uint8_t a[2] = { 0x00, 0x00 }, b[2] = { 0x00, 0x02 };
		ok = redwood_bbi2c_xfer(&sensor_bus, 0x10, a, sizeof(a), id, sizeof(id));
		if (ok) {
			(void)snprintf(phase, sizeof(phase), "S5K2LD revision read");
			ok = redwood_bbi2c_xfer(&sensor_bus, 0x10, b, sizeof(b), rev, sizeof(rev));
		}
	}
	if (warm_start && !(ok && id[0] == 0x20 && id[1] == 0xcd)) {
		/* the warm sensor does not answer as itself: a real power cycle, from the top */
		printf("camera: rear: the warm sensor did not answer (id %02x%02x): cold start\n", id[0], id[1]);
        /* Keep the original snapshot/voltage journal until fully powered down. */
        redwood_camera_s20_rear_cool();
        voltage=cam_s20_rear_cold.voltage;
        if(voltage.changed||!redwood_bbi2c_setup(&cam_pmic03_bus,false,out,cap))goto cleanup;
		warm_start = false;
		cold_again = true;
		ok = false;
		redwood_bbi2c_restore(&sensor_bus);
		sensor_ready = false;
		goto cold_start;
	}
	if (ok && stream_test) {
		/* (Samsung picks Global_A3 for revision 0xA301: is-cis-2ld.c) */
		uint64_t t0 = mach_absolute_time(), ns;

		(void)snprintf(stream_phase, sizeof(stream_phase), "global A3");
		if (warm_start) {
			/* (the global table is in the sensor since its cold start) */
		} else if (!cam_3j1_apply(&sensor_bus, redwood_2ld_global_a3, REDWOOD_2LD_GLOBAL_A3_WORDS)) {
			/* the faster bus did not take it: the old speed, again from the top, and for the rest of this boot */
			if (sensor_bus.half_us >= 2) goto stream_done;
			cam_s20_rear_slow = true;
			sensor_bus.half_us = 2;
			printf("camera: rear: the global table failed at the fast bus speed; again at half_us 2 (kept)\n");
			if (!cam_3j1_apply(&sensor_bus, redwood_2ld_global_a3, REDWOOD_2LD_GLOBAL_A3_WORDS)) goto stream_done;
		}
		(void)snprintf(stream_phase, sizeof(stream_phase), "mode %ux%u", cam_s20_rear_w, cam_s20_rear_h);
		if (rear_full_now) {
			/*
			 * The full 4032x3024 (setA A0). Its table turns the sensor's own HDR on (0x021E = 0x0100; the binned table
			 * leaves 0): long and short exposures interleaved pixel by pixel for Samsung's ISP to merge -- through
			 * ours one green was black, the other doubled, red and blue in alternating blocks (2026-11-12). Off
			 * after the table: one exposure, a plain Bayer picture. (Samsung's retention sequence, tried the same
			 * day with the same result, is kept: cam_s20_rear_ret=1.)
			 */
			uint32_t ret = 0;

			(void)PE_parse_boot_argn("cam_s20_rear_ret", &ret, sizeof(ret));
			if (ret) {
				if (!cam_3j1_apply(&sensor_bus, redwood_2ld_full_retention, REDWOOD_2LD_FULL_RETENTION_WORDS) ||
				    !cam_3j1_write16(&sensor_bus, 0x6028, 0x3000) || !cam_3j1_write16(&sensor_bus, 0x602A, 0x0484) ||
				    !cam_3j1_write16(&sensor_bus, 0x6F12, 0x0100) || !cam_3j1_write16(&sensor_bus, 0xFCFC, 0x4000) ||
				    !cam_3j1_write16(&sensor_bus, 0x010E, 0x0100) || !cam_3j1_write16(&sensor_bus, 0x0BCC, 0x0000) ||
				    !cam_3j1_write16(&sensor_bus, 0x0100, 0x0100) || !cam_3j1_write8(&sensor_bus, 0x0100, 0x00)) goto stream_done;
				cam_s20_wait_ms(10);
				if (!cam_3j1_apply(&sensor_bus, redwood_2ld_4032x3024_30fps_load_sram,
				    REDWOOD_2LD_4032X3024_30FPS_LOAD_SRAM_WORDS)) goto stream_done;
			} else if (!cam_3j1_apply(&sensor_bus, redwood_2ld_4032x3024_30fps, REDWOOD_2LD_4032X3024_30FPS_WORDS)) {
				goto stream_done;
			}
			if (!cam_3j1_write16(&sensor_bus, 0xFCFC, 0x4000) || !cam_3j1_write16(&sensor_bus, 0x021E, 0x0000)) goto stream_done;
		} else if (!cam_3j1_apply(&sensor_bus, redwood_2ld_2016x1512_30fps, REDWOOD_2LD_2016X1512_30FPS_WORDS)) goto stream_done;
		/*
		 * What Samsung's sensor_2ld_cis_mode_change() writes after every mode table and we did not (2026-11-12): auto
		 * exposure bracketing off (0x0E0B = 0: left on, the sensor may alternate long and short exposures) and the low-
		 * noise mode explicitly off (0x0B30 = 0x0100, "LN OFF"; the mode table leaves 0x0000). The rear's black level
		 * wandered with gain AND exposure and its dark colours shifted as the exposure changed; these are the sensor's
		 * own settings for a plain single-exposure stream. cam_s20_rear_plain=0 leaves them out.
		 */
		{
			uint32_t plain = 0;   /* OFF: on 2026-11-12 it gave a frame of noise (with half_us 1) */

			(void)PE_parse_boot_argn("cam_s20_rear_plain", &plain, sizeof(plain));
			if (plain && (!cam_3j1_write16(&sensor_bus, 0xFCFC, 0x4000) || !cam_3j1_write8(&sensor_bus, 0x0E0B, 0x00) ||
			    !cam_3j1_write16(&sensor_bus, 0x0B30, 0x0100))) goto stream_done;
		}
		{
			/*
			 * The sensor's own black-level correction ON (SMIA++ 0x0B04). Samsung's tables write 0x0B04 = 0x0001 as one
			 * 16-bit word, so 0x0B04 (black level correction) gets 0 and 0x0B05 (mapped defects) 1: their ISP clamps
			 * the black from the optical-black lines itself. Ours does not, and the black wandered from 22 to 65 with
			 * gain and exposure -- the green/purple shadows (2026-11-12). cam_s20_rear_blc=0 leaves it as the table has it.
			 */
			uint32_t blc = 0;   /* OFF (2026-11-12): on, it clamps at 0 and the 12 MP mode's lower band (rows >= ~1914, a lower
			                     * black) is cut off below zero -- shadow detail lost; the band is corrected in libredwood-media */

			(void)PE_parse_boot_argn("cam_s20_rear_blc", &blc, sizeof(blc));
			if (blc && (!cam_3j1_write16(&sensor_bus, 0xFCFC, 0x4000) || !cam_3j1_write8(&sensor_bus, 0x0B04, 0x01))) goto stream_done;
		}
		{
			/* what the sensor holds now: its mode's key registers read back (2026-11-12: "everything at register level") */
			static const uint16_t regs[] = { 0x0340, 0x0342, 0x034C, 0x034E, 0x0900, 0x021E, 0x0B30, 0x0B80, 0x0E0B, 0x0310, 0x0B04 };
			char line[200] = "";
			size_t at = 0;

			(void)cam_3j1_write16(&sensor_bus, 0xFCFC, 0x4000);
			for (unsigned r = 0; r < sizeof(regs) / sizeof(regs[0]); r++) {
				uint8_t a[2] = { (uint8_t)(regs[r] >> 8), (uint8_t)regs[r] }, v[2] = { 0, 0 };
				bool okr = redwood_bbi2c_xfer(&sensor_bus, 0x10, a, 2, v, regs[r] == 0x0E0B || regs[r] == 0x0B04 ? 1 : 2);

				at += (size_t)snprintf(line + at, sizeof(line) - at, " %04X=%s%02X%02X", regs[r], okr ? "" : "?",
				    v[0], regs[r] == 0x0E0B || regs[r] == 0x0B04 ? 0 : v[1]);
			}
			printf("camera: rear %ux%u registers:%s\n", cam_s20_rear_w, cam_s20_rear_h, line);
		}
		if (rx_test) {
			unsigned k, t;

			(void)snprintf(stream_phase, sizeof(stream_phase), "receiver map");
			rx_pmu = (volatile uint8_t *)cam_s20_io_map(PMU_PHYS, 0x4000);
			rx_cmu = (volatile uint8_t *)cam_s20_io_map(CMU_CSIS_PHYS, 0x8000);
			rx_csi = (volatile uint8_t *)cam_s20_io_map(CSIS0_PHYS, 0x1000);
			rx_phy = (volatile uint8_t *)cam_s20_io_map(CSIS0_DCPHY_PHYS, 0x500);
			rx_bias = (volatile uint8_t *)cam_s20_io_map(CSIS1_DPHY_BIAS_PHYS, 0x100);
			rx_sys = (volatile uint8_t *)cam_s20_io_map(ISPPRE_SYSREG_PHYS, 0x4);
			if (rx_pmu == NULL || rx_cmu == NULL || rx_csi == NULL || rx_phy == NULL || rx_bias == NULL || rx_sys == NULL) goto stream_done;
			(void)snprintf(stream_phase, sizeof(stream_phase), "CSIS domain off");
			if (!(cam_s20_rd(rx_pmu, 0x1d84) & 1U)) goto stream_done;
			(void)snprintf(stream_phase, sizeof(stream_phase), "CSIS secure access");
			if (cam_s20_smc4(SMC_PREPARE_PD_ONOFF, 1, CSIS_NEED_SMC, 2) != 0) goto stream_done;
			/*
			 * The CSIS bus clock as Samsung's active capture has it (TOP 104c=1 PLL_SHARED4_DIV2, 1848=0); XNU's is about
			 * six times slower, enough for the front's 1807 Mbps x 4 but not the rear's ~2 Gpixel/s: CSIS0 ERR_OVER
			 * (2026-11-12). Only the dedicated CSIS bus branch; put back afterwards.
			 */
			{
				unsigned k;

				rx_bus_mux = cam_s20_rd(cmu, 0x104c);
				rx_bus_div = cam_s20_rd(cmu, 0x1848);
				rx_bus_changed = true;
				cam_s20_wr(cmu, 0x104c, (rx_bus_mux & ~3U) | 1U);
				for (k = 0; k < 1000 && (cam_s20_rd(cmu, 0x104c) & (1U << 16)); k++) delay_for_interval(1, NSEC_PER_USEC);
				cam_s20_wr(cmu, 0x1848, rx_bus_div & ~15U);
				for (k = 0; k < 1000 && (cam_s20_rd(cmu, 0x1848) & (1U << 16)); k++) delay_for_interval(1, NSEC_PER_USEC);
			}
			rx_qch = cam_s20_rd(rx_cmu, 0x3060);
			rx_iso = cam_s20_rd(rx_pmu, 0x0730);
			rx_rst = cam_s20_rd(rx_sys, 0);
			cam_s20_wr(rx_cmu, 0x3060, (rx_qch & ~7U) | 1U);         /* CSIS0's Q-channel: clock on */
			if (!(rx_iso & 1U)) cam_s20_wr(rx_pmu, 0x0730, rx_iso | 1U);
			for (k = 0; k < 12; k++) rx_csi_saved[k] = cam_s20_rd(rx_csi, rx_csi_offs[k]);
			for (k = 0; k < 3; k++) for (t = 0; t < 15; t++) rx_phy_saved[k][t] = cam_s20_rd(rx_phy, 0x100 + k * 0x100 + rx_phy_offs[t]);
			for (k = 0; k < 4; k++) rx_bias_saved[k] = cam_s20_rd(rx_bias, k == 3 ? 0x10 : k * 4);
			rx_saved = true;
			(void)snprintf(stream_phase, sizeof(stream_phase), "CSIS0 reset");
			cam_s20_wr(rx_csi, 0x0004, rx_csi_saved[0] | (1U << 1));
			for (k = 0; k < 10 && (cam_s20_rd(rx_csi, 0x0004) & (1U << 1)); k++) delay_for_interval(10, NSEC_PER_USEC);
			if (cam_s20_rd(rx_csi, 0x0004) & (1U << 1)) goto stream_done;
			/* csi0 PHY held in reset (ISPPRE sysreg bit 0: the DT's dcphy csi0 reset) while it is set up */
			cam_s20_wr(rx_sys, 0, rx_rst & ~1U);
			cam_s20_cphy_2ld_config(rx_phy, rx_bias);
			cam_s20_wr(rx_sys, 0, rx_rst | 1U);
			/* CMN_CTRL: PHY_SEL (bit 21) C-PHY, lane number 2 (3 trios), interleave 3 (DT mode4), no deskew */
			cam_s20_wr(rx_csi, 0x0004, (rx_csi_saved[0] & ~((7U << 13) | (3U << 10) | (3U << 8) | 3U)) | (1U << 21) |
			    (3U << 10) | (2U << 8));
			cam_s20_wr(rx_csi, 0x0008, rx_csi_saved[1] | (1U << 4));
			cam_s20_wr(rx_csi, 0x0704, (rx_csi_saved[11] & ~0x1fU) | (0x0fU << 1));    /* trios on */
			cam_s20_wr(rx_csi, 0x0040, (0x2bU << 2) | (2U << 12));                       /* VC0 RAW10 */
			cam_s20_wr(rx_csi, 0x0044, (cam_s20_rear_h << 16) | cam_s20_rear_w);
			/* LRTE_CONFIG EPD_EN: the DT's lrte 1 for every S5K2LD mode (Samsung: csi_hw_s_control(CSIS_CTRL_LRTE)) --
			 * off, CSIS0 passed frame starts and no lines (2026-11-12) */
			cam_s20_wr(rx_csi, 0x0600, rx_csi_saved[9] | (1U << 31));
			cam_s20_wr(rx_csi, 0x0690, rx_csi_saved[10] | (1U << 23));
			cam_s20_wr(rx_csi, 0x000c, 0xfU);
			cam_s20_wr(rx_csi, 0x0704, cam_s20_rd(rx_csi, 0x0704) | 1U);
			cam_s20_wr(rx_csi, 0x0010, 0x00fff07fU);
			cam_s20_wr(rx_csi, 0x0018, 0x0000007fU);
			cam_s20_wr(rx_csi, 0x0004, cam_s20_rd(rx_csi, 0x0004) | 1U);
			cam_s20_wait_ms(2);
			rx_fc_before = cam_s20_rd(rx_csi, 0x0500);
			rx_cmn = cam_s20_rd(rx_csi, 0x0004);
		}
		(void)snprintf(stream_phase, sizeof(stream_phase), "stream on");
		/* a fixed indoor exposure for the test: 16384 of the mode's 22564 lines (~24 ms), analog gain 4x (0x20 = 1x) */
		if (rx_test && (!cam_3j1_write16(&sensor_bus, 0xFCFC, 0x4000) ||
		    /* (lines + 100000 * g: gain code g -- 1 1x, 4 4x, 8 8x, 16 16x -- with that exposure, for dark frames) */
		    !cam_3j1_write16(&sensor_bus, 0x0340, (uint16_t)((cam_s20_rear_test_lines % 100000) + 64 > 22564 ?
		    (cam_s20_rear_test_lines % 100000) + 64 : 22564)) ||
		    !cam_3j1_write16(&sensor_bus, 0x0202, (uint16_t)(cam_s20_rear_test_lines % 100000)) ||
		    !cam_3j1_write16(&sensor_bus, 0x0204, cam_s20_rear_test_lines >= 100000 ? (uint16_t)(0x20 * (cam_s20_rear_test_lines / 100000)) :
		    cam_s20_rear_test_lines == 3 ? 0x0080 : cam_s20_rear_test_lines == 4 ? 0x0100 :
		    cam_s20_rear_test_lines == 5 ? 0x0200 : cam_s20_rear_test_lines < 16 ? 0x0020 : 0x0080))) goto stream_done;
		if (!cam_3j1_write16(&sensor_bus, 0xFCFC, 0x4000) || !cam_3j1_write16(&sensor_bus, 0x0BCC, 0x0100) ||
		    !cam_3j1_write16(&sensor_bus, 0x0100, 0x0100)) goto stream_done;
		if (rx_saved && cam_s20_stream_fn != NULL && cam_s20_optical_dma != NULL) {
			/*
			 * The rear main streaming to /dev/redwood-camera: the front's loop (cam_s20_front_stream_probe) on WDMA2.
			 * Frame end -> the finished slot (even buffer 0, odd buffer 1) -> cam_s20_stream_fn; exposure asked for
			 * is written in an SMIA group hold between frames.
			 */
			volatile uint8_t *s_cmn = cam_s20_optical_dma + 0x3400, *s_vc = cam_s20_optical_dma + 0x3000;
			uint32_t idle = 0;

			(void)cam_s20_af_up();

			for (;;) {
				uint32_t st = cam_s20_rd(s_cmn, 8);

				if ((st & (1U << 4)) && !(st & (1U << 8))) {
					cam_s20_wr(s_cmn, 8, 1U << 4);
					st &= ~(1U << 4);
				}
				if (st & (1U << 8)) {
					uint32_t p0 = (cam_s20_rd(s_vc, 0x90) >> 2) & 0x1fU, st2 = cam_s20_rd(s_cmn, 8), ptr;
					bool more;

					if (st & (1U << 4)) ptr = (p0 + 31U) & 0x1fU;
					else if (!(st2 & (1U << 4))) ptr = p0;
					else ptr = (((cam_s20_rd(s_vc, 0x90) >> 2) & 0x1fU) + 31U) & 0x1fU;
					cam_s20_wr(s_cmn, 8, (st | st2) & 0x01111110U);
					if (st & 0x01101000U) cam_s20_stream_errors++;
					if (cam_s20_stream_cached) {
						vm_offset_t line = (vm_offset_t)cam_s20_stream_buf[ptr & 1] & ~(vm_offset_t)63;
						vm_offset_t end = (vm_offset_t)cam_s20_stream_buf[ptr & 1] + cam_s20_stream_bytes;

						for (; line < end; line += 64) __asm__ volatile ("dc ivac, %0" : : "r" (line));
						__asm__ volatile ("dsb sy" ::: "memory");
					}
					more = cam_s20_stream_fn(cam_s20_stream_buf[ptr & 1], cam_s20_rear_w, cam_s20_rear_h,
					    ((cam_s20_rear_w * 10 / 8) + 15) & ~15U);
					cam_s20_stream_frames++;
					if (cam_s20_stream_frames % 30 == 1) {
						/* what the sensor really has: coarse integration, analog gain, frame length */
						uint16_t cit = 0, ag = 0, fll = 0;

						(void)cam_3j1_write16(&sensor_bus, 0xFCFC, 0x4000);
						(void)cam_3j1_read16(&sensor_bus, 0x0202, &cit);
						(void)cam_3j1_read16(&sensor_bus, 0x0204, &ag);
						(void)cam_3j1_read16(&sensor_bus, 0x0340, &fll);
						printf("camera: rear frame %u: sensor has exposure %u lines, gain %#x, frame %u lines (asked %u, %#x, %u)\n",
						    cam_s20_stream_frames, cit, ag, fll, cam_s20_exp_lines, cam_s20_exp_gain, cam_s20_exp_frame);
					}
					idle = 0;
					if (cam_s20_exp_pending) {
						uint32_t el = cam_s20_exp_lines, eg = cam_s20_exp_gain, ef = cam_s20_exp_frame;

						cam_s20_exp_pending = false;
						const struct redwood_2ld_control_io io = {
						    .context = &sensor_bus, .write16 = cam_s20_exposure_write16, .write8 = cam_s20_exposure_write8
						};
						uint16_t failed_reg;

						if (!redwood_2ld_write_exposure_mode(&io, !(diag & 1U), el, eg, ef, &failed_reg)) {
							/* Do not emit future frames tagged with an unacknowledged request.
							 * Break through the normal DMA abort/stream-off cleanup below. */
							cam_s20_stream_exposure_failed = true;
							cam_s20_stream_errors++;
							ok = false;
							(void)snprintf(phase, sizeof(phase), "rear exposure I2C reg %#x", failed_reg);
							break;
						}
					}
					if (cam_s20_af_pending && cam_s20_af_ready) {
						uint32_t want = cam_s20_af_want;

						cam_s20_af_pending = false;
						if (want != cam_s20_af_pos && cam_s20_af_set(want)) cam_s20_af_pos = want;
					}
					if (!more) break;
				} else {
					if (++idle > 2000) break;
					cam_s20_wait_ms(1);
				}
			}
			cam_s20_af_down();
			/*
			 * Memory output off while the sensor still sends: the frame being written ends (up to 80 ms), then the
			 * abort is asked and waited for (its acknowledgement, status bit 16). Asked with the sensor already going
			 * down, the write never ended: "previous DMA did not stop; reboot required", no camera until a reboot
			 * (2026-11-12, the user's failed switch).
			 */
			{
				unsigned k;

				cam_s20_wr(s_vc, 0, 0);
				for (k = 0; k < 80 && (cam_s20_rd(s_vc, 0x90) & 1U); k++) cam_s20_wait_ms(1);
				cam_s20_wr(s_cmn, 0x14, cam_s20_rd(s_cmn, 0x14) | 1U);
				for (k = 0; k < 100 && !(cam_s20_rd(s_cmn, 8) & (1U << 16)); k++) cam_s20_wait_ms(1);
			}
		} else
		for (i = 0; i < 4; i++) {
			unsigned w;

			for (w = 0; w < (i == 0 ? 50U : 150U); w++) {
				cam_s20_wait_ms(1);
				if (rx_saved) {
					uint32_t st = cam_s20_rd(rx_csi, 0x0700);

					rx_phy_or |= st;
					rx_phy_and &= st;
					rx_irq_or |= cam_s20_rd(rx_csi, 0x0014);
				}
			}
			(void)cam_3j1_read8(&sensor_bus, 0x0005, &fc[i]);
		}
		if (rx_saved) {
			rx_fc_after = cam_s20_rd(rx_csi, 0x0500);
			rx_irq = cam_s20_rd(rx_csi, 0x0014);
		}
		(void)cam_3j1_write16(&sensor_bus, 0x0100, 0x0000);
		cam_s20_wait_ms(40);
		absolutetime_to_nanoseconds(mach_absolute_time() - t0, &ns);
		if (ok) (void)snprintf(stream_phase, sizeof(stream_phase), "streamed (tables+run %llu ms)", ns / 1000000ULL);
	}
stream_done:
	if (rx_saved) {
		unsigned k, t;

		cam_s20_wr(rx_csi, 0x0004, cam_s20_rd(rx_csi, 0x0004) & ~1U);
		for (k = 12; k > 0; k--) cam_s20_wr(rx_csi, rx_csi_offs[k - 1], rx_csi_saved[k - 1]);
		for (k = 0; k < 3; k++) cam_s20_wr(rx_phy, 0x100 + k * 0x100, 0);
		for (k = 0; k < 3; k++) for (t = 14; t > 0; t--) cam_s20_wr(rx_phy, 0x100 + k * 0x100 + rx_phy_offs[t], rx_phy_saved[k][t]);
		for (k = 0; k < 3; k++) cam_s20_wr(rx_phy, 0x100 + k * 0x100, rx_phy_saved[k][0]);
		for (k = 0; k < 4; k++) cam_s20_wr(rx_bias, k == 3 ? 0x10 : k * 4, rx_bias_saved[k]);
		cam_s20_wr(rx_sys, 0, rx_rst);
		cam_s20_wr(rx_pmu, 0x0730, rx_iso);
		cam_s20_wr(rx_cmu, 0x3060, rx_qch);
	}
	if (rx_bus_changed) {
		unsigned k;

		cam_s20_wr(cmu, 0x1848, rx_bus_div);
		for (k = 0; k < 1000 && (cam_s20_rd(cmu, 0x1848) & (1U << 16)); k++) delay_for_interval(1, NSEC_PER_USEC);
		cam_s20_wr(cmu, 0x104c, rx_bus_mux);
		for (k = 0; k < 1000 && (cam_s20_rd(cmu, 0x104c) & (1U << 16)); k++) delay_for_interval(1, NSEC_PER_USEC);
	}
	if (rx_test) {
		(void)snprintf(rx_report, sizeof(rx_report), ",\"csis0_frames\":[%u,%u],\"csis0_irq\":\"%#x\",\"csis0_irq_seen\":\"%#x\",\"phy_status_or\":\"%#x\",\"phy_status_and\":\"%#x\",\"cmn\":\"%#x\"",
		    rx_fc_before, rx_fc_after, rx_irq, rx_irq_or, rx_phy_or, rx_phy_and, rx_cmn);
	}
cleanup:
	{
		uint32_t want = 1;

		(void)PE_parse_boot_argn("cam_s20_rear_warm", &want, sizeof(want));
		if (diag & 2U) want = 0;
		if (want && ok && cam_s20_stream_fn != NULL && cam_s20_stream_frames != 0 && clock_ready && rails02 && rails03) {
			/* kept warm: the sensor in standby (stream off above), everything else as it is */
			if (!cam_s20_rear_warm) {
				memcpy(cam_s20_rear_cold.p02, p02, sizeof(p02));
                cam_s20_rear_cold.voltage=voltage;
				memcpy(cam_s20_rear_cold.p03, p03, sizeof(p03));
				cam_s20_rear_cold.reset_con = reset_con; cam_s20_rear_cold.reset_dat = reset_dat; cam_s20_rear_cold.reset_pud = reset_pud;
				cam_s20_rear_cold.mclk_con = mclk_con; cam_s20_rear_cold.mclk_dat = mclk_dat; cam_s20_rear_cold.mclk_pud = mclk_pud;
				for (i = 0; i < 4; i++) cam_s20_rear_cold.clock[i] = clock[i];
				cam_s20_rear_warm = true;
			}
			if (sensor_ready) redwood_bbi2c_restore(&sensor_bus);
			if (pmic03_ready) redwood_bbi2c_restore(&cam_pmic03_bus);
			(void)snprintf(out, cap, "{\"camera\":\"S5K2LD\",\"result\":\"ok\",\"stream\":\"%s\",\"start\":\"%s\",\"kept_warm\":true}\n",
			    stream_phase, warm_start ? "warm" : "cold");
			return;
		}
		cam_s20_rear_warm = false;      /* (a session that failed, or a bounded test: all the way down) */
	}
	/* Stop clock and hold reset low before taking supplies away. */
	(void)redwood_gpio_set(reset, 0, 0);
	if (clock_ready) {
		(void)redwood_gpio_set(mclk, 0, 0);
		redwood_gpio_func(mclk, 0, REDWOOD_GPIO_OUTPUT);
		cam_s20_wr(cmu, CMU_CIS_CLK0_GATE, clock[2]);
		cam_s20_wr(cmu, CMU_CIS_CLK0_QCH, clock[3]);
		cam_s20_wr(cmu, CMU_CIS_CLK0_DIV, clock[1]);
		cam_s20_wr(cmu, CMU_CIS_CLK0_MUX, clock[0]);
	}
	if (sensor_ready) {
		redwood_bbi2c_restore(&sensor_bus);
	}
	if (rails02)
		(void)redwood_s2mpb02_camera_bits(CAM_S2MPB02_LDO14, 0x80, p02[2] & 0x80);
	if (rails03) (void)cam_s20_pmic03_bits(CAM_S2MPB03_LDO2, p03[0], p03[0] & 0x80);
	if (rails02)
		(void)redwood_s2mpb02_camera_bits(CAM_S2MPB02_LDO1, 0x80, p02[1] & 0x80);
	if (rails03) {
		(void)cam_s20_pmic03_bits(CAM_S2MPB03_LDO5, p03[2], p03[2] & 0x80);
		(void)cam_s20_pmic03_bits(CAM_S2MPB03_LDO3, p03[1], p03[1] & 0x80);
	}
	if (rails02)
		(void)redwood_s2mpb02_camera_bits(CAM_S2MPB02_BUCK2, 0xc0, p02[0] & 0xc0);
    if(voltage.changed&&!main_voltages(&voltage,true)){ok=false;cam_s20_stream_exposure_failed=true;}
	/* Use the GPIO helpers so register updates keep the shared pinctrl-bank lock. */
	redwood_gpio_restore(reset, 0, reset_con & mask_reset, reset_pud & mask_reset, reset_dat & 1U);
	redwood_gpio_restore(mclk, 0, mclk_con & mask_mclk, mclk_pud & mask_mclk, mclk_dat & 1U);
	if (pmic03_ready) redwood_bbi2c_restore(&cam_pmic03_bus);
	if (ok) {
		(void)snprintf(out, cap, "{\"camera\":\"S5K2LD\",\"address\":\"0x10\",\"result\":\"ok\",\"model_id\":\"0x%02x%02x\",\"revision\":\"0x%02x%02x\",\"stream\":\"%s\",\"frame_count\":[%u,%u,%u,%u]%s,\"state_restored\":true}\n",
		    id[0], id[1], rev[0], rev[1], stream_test ? stream_phase : "not asked", fc[0], fc[1], fc[2], fc[3], rx_report);
	} else {
		(void)snprintf(out, cap, "{\"camera\":\"S5K2LD\",\"address\":\"0x10\",\"result\":\"error\",\"phase\":\"%s\",\"acked\":\"%s\",\"state_restored\":true}\n", phase, acked);
	}
}

/* The warm rear main all the way down, as a session's end used to leave it (the camera thread, when nobody has used
 * a camera for a while). */
void
redwood_camera_s20_rear_cool(void)
{
	volatile uint8_t *reset = redwood_gpio_bank(PINCTRL_CMGP_PHYS, PINCTRL_CMGP_GPM22);
	volatile uint8_t *mclk = redwood_gpio_bank(PINCTRL_PERIC1_PHYS, PINCTRL_PERIC1_GPC0);
	volatile uint8_t *cmu = (volatile uint8_t *)cam_s20_io_map(CMU_TOP_PHYS, 0x4000);
	const uint8_t *p02 = cam_s20_rear_cold.p02, *p03 = cam_s20_rear_cold.p03;
	char report[160];

	if (!cam_s20_rear_warm || cmu == NULL) return;
	cam_s20_rear_warm = false;
	cam_pmic03_bus.bank = redwood_gpio_bank(PINCTRL_CMGP_PHYS, PINCTRL_CMGP_GPM0);
	cam_pmic03_bus.sda_bank = redwood_gpio_bank(PINCTRL_CMGP_PHYS, PINCTRL_CMGP_GPM1);
	if (!redwood_bbi2c_setup(&cam_pmic03_bus, false, report, sizeof(report))) {
		printf("camera: rear: cannot reach its supplies to power down (%s)\n", report);
		return;
	}
	(void)redwood_gpio_set(reset, 0, 0);
	(void)redwood_gpio_set(mclk, 0, 0);
	redwood_gpio_func(mclk, 0, REDWOOD_GPIO_OUTPUT);
	cam_s20_wr(cmu, CMU_CIS_CLK0_GATE, cam_s20_rear_cold.clock[2]);
	cam_s20_wr(cmu, CMU_CIS_CLK0_QCH, cam_s20_rear_cold.clock[3]);
	cam_s20_wr(cmu, CMU_CIS_CLK0_DIV, cam_s20_rear_cold.clock[1]);
	cam_s20_wr(cmu, CMU_CIS_CLK0_MUX, cam_s20_rear_cold.clock[0]);
	(void)redwood_s2mpb02_camera_bits(CAM_S2MPB02_LDO14, 0x80, p02[2] & 0x80);
	(void)cam_s20_pmic03_bits(CAM_S2MPB03_LDO2, p03[0], p03[0] & 0x80);
	(void)redwood_s2mpb02_camera_bits(CAM_S2MPB02_LDO1, 0x80, p02[1] & 0x80);
	(void)cam_s20_pmic03_bits(CAM_S2MPB03_LDO5, p03[2], p03[2] & 0x80);
	(void)cam_s20_pmic03_bits(CAM_S2MPB03_LDO3, p03[1], p03[1] & 0x80);
	(void)redwood_s2mpb02_camera_bits(CAM_S2MPB02_BUCK2, 0xc0, p02[0] & 0xc0);
    if(cam_s20_rear_cold.voltage.changed&&!main_voltages(&cam_s20_rear_cold.voltage,true))cam_s20_stream_exposure_failed=true;
	redwood_gpio_restore(reset, 0, cam_s20_rear_cold.reset_con & 0xfU, cam_s20_rear_cold.reset_pud & 0xfU, cam_s20_rear_cold.reset_dat & 1U);
	redwood_gpio_restore(mclk, 0, cam_s20_rear_cold.mclk_con & 0xfU, cam_s20_rear_cold.mclk_pud & 0xfU, cam_s20_rear_cold.mclk_dat & 1U);
	redwood_bbi2c_restore(&cam_pmic03_bus);
	printf("camera: rear: powered down after 20 s without a camera in use\n");
}

/* Front SAK3J1 candidate. Uses only the x1slte overlay's VT rails, HSI2C1, reset and MCLK1. */
static void
cam_s20_front_identify(__unused const uint64_t *v, __unused unsigned n, char *out, size_t cap)
{
	struct redwood_bbi2c sensor_bus = { .name = "S20 front SAK3J1 HSI2C1 gpp0-2/3", .scl = 2, .sda = 3, .half_us = 4 };
	volatile uint8_t *reset = redwood_gpio_bank(PINCTRL_CMGP_PHYS, PINCTRL_CMGP_GPM11);
	volatile uint8_t *mclk = redwood_gpio_bank(PINCTRL_PERIC1_PHYS, PINCTRL_PERIC1_GPC0);
	volatile uint8_t *cmu = (volatile uint8_t *)cam_s20_io_map(CMU_TOP_PHYS, 0x4000);
	uint8_t p[5] = { 0 }, id[2] = { 0 }, rev = 0;
	uint32_t rcon = cam_s20_rd(reset, REDWOOD_GPIO_CON), rdat = cam_s20_rd(reset, REDWOOD_GPIO_DAT);
	uint32_t rpud = cam_s20_rd(reset, REDWOOD_GPIO_PUD), mcon = cam_s20_rd(mclk, REDWOOD_GPIO_CON);
	uint32_t mdat = cam_s20_rd(mclk, REDWOOD_GPIO_DAT), mpud = cam_s20_rd(mclk, REDWOOD_GPIO_PUD);
	uint32_t clock[4] = { 0 }, offsets[4] = { CMU_CIS_CLK1_MUX, CMU_CIS_CLK1_DIV, CMU_CIS_CLK1_GATE, CMU_CIS_CLK1_QCH };
	char phase[40] = "S2MPB02 snapshot", acked[96] = "";
	bool sensor_ready = false, clock_ready = false, rails = false, ok = false;
	unsigned i;
	if (cap == 0) return;
	if (cmu == NULL) { (void)snprintf(out, cap, "{\"camera\":\"SAK3J1\",\"result\":\"error\",\"phase\":\"CMU map\"}\n"); return; }
	if (!redwood_s2mpb02_camera_read(CAM_S2MPB02_BUCK2, &p[0]) ||
	    !redwood_s2mpb02_camera_read(CAM_S2MPB02_LDO16, &p[1]) ||
	    !redwood_s2mpb02_camera_read(CAM_S2MPB02_LDO11, &p[2]) ||
	    !redwood_s2mpb02_camera_read(CAM_S2MPB02_LDO6, &p[3]) ||
	    !redwood_s2mpb02_camera_read(CAM_S2MPB02_LDO5, &p[4])) goto cleanup;
	rails = true;
	(void)redwood_gpio_set(reset, 0, 0);
	redwood_gpio_pull(reset, 0, REDWOOD_GPIO_PULL_NONE);
	redwood_gpio_set(reset, 0, 0);
	redwood_gpio_func(reset, 0, REDWOOD_GPIO_OUTPUT);
	(void)snprintf(phase, sizeof(phase), "BUCK2 enable");
	if (!redwood_s2mpb02_camera_bits(CAM_S2MPB02_BUCK2, 0xc0, 0xc0)) goto cleanup;
	(void)snprintf(phase, sizeof(phase), "VT camera rails");
	if (!redwood_s2mpb02_camera_bits(CAM_S2MPB02_LDO16, 0x80, 0x80) ||
	    !redwood_s2mpb02_camera_bits(CAM_S2MPB02_LDO11, 0x80, 0x80) ||
	    !redwood_s2mpb02_camera_bits(CAM_S2MPB02_LDO6, 0x80, 0x80) ||
	    !redwood_s2mpb02_camera_bits(CAM_S2MPB02_LDO5, 0x80, 0x80)) goto cleanup;
	cam_s20_wait_ms(1);
	(void)snprintf(phase, sizeof(phase), "sensor I2C pins");
	sensor_ready = true;
	sensor_bus.bank = redwood_gpio_bank(PINCTRL_PERIC0_PHYS, 0);
	if (!redwood_bbi2c_setup(&sensor_bus, true, out, cap)) goto cleanup;
	/* CIS_CLK1 is the front module's gpc0-1 output in the Samsung x1slte overlay. */
	for (i = 0; i < 4; i++) clock[i] = cam_s20_rd(cmu, offsets[i]);
	cam_s20_wr(cmu, CMU_CIS_CLK1_MUX, clock[0] & ~1U);
	cam_s20_wr(cmu, CMU_CIS_CLK1_DIV, clock[1] & ~0x1fU);
	cam_s20_wr(cmu, CMU_CIS_CLK1_QCH, clock[3] | 3U);
	cam_s20_wr(cmu, CMU_CIS_CLK1_GATE, clock[2] | (1U << 20) | (1U << 21));
	clock_ready = true;
	(void)redwood_gpio_set(mclk, 1, 0);
	redwood_gpio_func(mclk, 1, 2);
	redwood_gpio_pull(mclk, 1, REDWOOD_GPIO_PULL_NONE);
	cam_s20_wait_ms(10); /* on_i2c delay in the vendor power table */
	(void)redwood_gpio_set(reset, 0, 1);
	cam_s20_wait_ms(9);  /* MCLK settle from the SAK3J1 vendor power table */
	(void)snprintf(phase, sizeof(phase), "SAK3J1 address scan");
	for (i = 0x08; i < 0x78; i++) {
		if (redwood_bbi2c_xfer(&sensor_bus, (uint8_t)i, NULL, 0, NULL, 0)) {
			if (acked[0] != '\0') strlcat(acked, ",", sizeof(acked));
			{
				char address[8];
				(void)snprintf(address, sizeof(address), "0x%02x", i);
				strlcat(acked, address, sizeof(acked));
			}
			if (i == 0x10) ok = true;
		}
	}
	if (!ok) goto cleanup;
	(void)snprintf(phase, sizeof(phase), "SAK3J1 model/revision read");
	{
		uint8_t a[2] = { 0x00, 0x00 }, b[2] = { 0x00, 0x02 };
		ok = redwood_bbi2c_xfer(&sensor_bus, 0x10, a, sizeof(a), id, sizeof(id)) &&
		    redwood_bbi2c_xfer(&sensor_bus, 0x10, b, sizeof(b), &rev, sizeof(rev));
	}
cleanup:
	(void)redwood_gpio_set(reset, 0, 0);
	if (clock_ready) {
		(void)redwood_gpio_set(mclk, 1, 0);
		redwood_gpio_func(mclk, 1, REDWOOD_GPIO_OUTPUT);
		cam_s20_wr(cmu, CMU_CIS_CLK1_GATE, clock[2]);
		cam_s20_wr(cmu, CMU_CIS_CLK1_QCH, clock[3]);
		cam_s20_wr(cmu, CMU_CIS_CLK1_DIV, clock[1]);
		cam_s20_wr(cmu, CMU_CIS_CLK1_MUX, clock[0]);
	}
	if (sensor_ready) redwood_bbi2c_restore(&sensor_bus);
	if (rails) {
		(void)redwood_s2mpb02_camera_bits(CAM_S2MPB02_LDO5, 0x80, p[4] & 0x80);
		(void)redwood_s2mpb02_camera_bits(CAM_S2MPB02_LDO6, 0x80, p[3] & 0x80);
		(void)redwood_s2mpb02_camera_bits(CAM_S2MPB02_LDO11, 0x80, p[2] & 0x80);
		(void)redwood_s2mpb02_camera_bits(CAM_S2MPB02_LDO16, 0x80, p[1] & 0x80);
		(void)redwood_s2mpb02_camera_bits(CAM_S2MPB02_BUCK2, 0xc0, p[0] & 0xc0);
	}
	redwood_gpio_restore(reset, 0, rcon & 0xfU, rpud & 0xfU, rdat & 1U);
	redwood_gpio_restore(mclk, 1, mcon & (0xfU << 4), mpud & (0xfU << 4), mdat & 2U);
	if (ok) {
		(void)snprintf(out, cap, "{\"camera\":\"SAK3J1\",\"address\":\"0x10\",\"result\":\"ok\",\"model_id\":\"0x%02x%02x\",\"revision\":\"0x%02x\",\"state_restored\":true}\n", id[0], id[1], rev);
	} else {
		(void)snprintf(out, cap, "{\"camera\":\"SAK3J1\",\"address\":\"0x10\",\"result\":\"error\",\"phase\":\"%s\",\"acked\":\"%s\",\"state_restored\":true}\n", phase, acked);
	}
}

/* CSIS v5.3 DMA-only pattern test. No sensor, CSI link, PHY or clock-rate
 * writes. Sources: Exynos990 camera DTS and is-hw-csi-v5_3.c/h. All frame
 * addresses point inside an exclusively borrowed low-memory pool slot. */
extern bool redwood_camera_dma_get(uint64_t bytes, uint64_t *phys, void **virt);
extern void redwood_camera_dma_put(void);
extern void *redwood_camera_dma_cached(void);
extern int redwood_camera_cached_enable;
extern void ustacks_now(const char *name);
/* where every thread of mediaserverd is, into the kernel's log (osfmk/arm64/user_exception_trace.c) */
static void
cam_ustacks_media_cmd(__unused const uint64_t *v, __unused unsigned n, char *out, size_t cap)
{
	ustacks_now("mediaserverd");
	(void)snprintf(out, cap, "ustacks: mediaserverd's threads go to the kernel log\n");
}

extern int redwood_camera_s20_turn;
static void
cam_s20_turn_cmd(const uint64_t *v, unsigned n, char *out, size_t cap)
{
	if (n > 0) redwood_camera_s20_turn = (int)(v[0] & 7);
	(void)snprintf(out, cap, "cam.turn: %d (bit 2 on, bit 0 turn 180, bit 1 mirror, both cameras); switch or reopen the camera\n", redwood_camera_s20_turn);
}

static void
cam_s20_cached_cmd(const uint64_t *v, unsigned n, char *out, size_t cap)
{
	if (n > 0) redwood_camera_cached_enable = v[0] != 0;
	(void)snprintf(out, cap, "cam.s20.cached: the stream reads frames through the %s mapping (from the next camera open)\n",
	    redwood_camera_cached_enable ? "cached" : "write-combined");
}
static bool cam_s20_dma_quarantined;
static void cam_s20_front_stream_probe(const uint64_t *, unsigned, char *, size_t);
static LCK_GRP_DECLARE(cam_frame_group, "s20_camera_frame");
static LCK_MTX_DECLARE(cam_frame_lock, &cam_frame_group);
static uint8_t *cam_frame_copy;
static size_t cam_frame_size;

/* Export a CPU-owned copy, so DMA memory can be released before a host download.
 * RWCAM001 is the existing tools/camraw.py format: one packed MIPI RAW10 frame. */
static bool
cam_s20_save_frame_named(const volatile uint16_t *pixels, uint32_t width, uint32_t height, bool optical, const char *label)
{
	uint32_t stride = width * 5 / 4, bytes = stride * height;
	size_t size = 4096 + bytes;
	uint8_t *copy = kalloc_data(size, Z_WAITOK | Z_ZERO);
	if (!copy) return false;
	memcpy(copy, "RWCAM001", 8);
	uint32_t header[] = { width, height, stride, 1, 1, 1, bytes, optical ? 1U : 0U };
	memcpy(copy + 8, header, sizeof(header));
	strlcpy((char *)copy + 40, label, 16);
	for (uint32_t i = 0, o = 4096; i < width * height; i += 4, o += 5) {
		uint16_t a = pixels[i], b = pixels[i+1], c = pixels[i+2], d = pixels[i+3];
		copy[o] = a >> 2; copy[o+1] = b >> 2; copy[o+2] = c >> 2; copy[o+3] = d >> 2;
		copy[o+4] = (a & 3) | ((b & 3) << 2) | ((c & 3) << 4) | ((d & 3) << 6);
	}
	lck_mtx_lock(&cam_frame_lock);
	if (cam_frame_copy) kfree_data(cam_frame_copy, cam_frame_size);
	cam_frame_copy = copy; cam_frame_size = size;
	lck_mtx_unlock(&cam_frame_lock);
	return true;
}

static bool
cam_s20_save_frame(const volatile uint16_t *pixels,uint32_t width,uint32_t height,bool optical){
    return cam_s20_save_frame_named(pixels,width,height,optical,optical?"SAK3J1":"S20 DMA TPG");
}

static int
cam_s20_frame_sysctl(__unused struct sysctl_oid *oidp, __unused void *arg1,
    __unused int arg2, struct sysctl_req *req)
{
	if (!kauth_cred_issuser(kauth_cred_get())) return EPERM;
	lck_mtx_lock(&cam_frame_lock);
	int error = cam_frame_copy ? SYSCTL_OUT(req, cam_frame_copy, cam_frame_size) : ENOENT;
	lck_mtx_unlock(&cam_frame_lock);
	return error;
}
static int
cam_s20_rear_diag_sysctl(struct sysctl_oid *oidp, __unused void *arg1,
    __unused int arg2, struct sysctl_req *req)
{
    int value = __atomic_load_n(&cam_s20_rear_diag, __ATOMIC_RELAXED);
    int error = sysctl_handle_int(oidp, &value, 0, req);
    if (error || !req->newptr) return error;
    if (value < 0 || value > 3) return EINVAL;
    __atomic_store_n(&cam_s20_rear_diag, value, __ATOMIC_RELAXED);
    return 0;
}

static int
cam_s20_rear_i2c_sysctl(struct sysctl_oid *oidp,__unused void *arg1,__unused int arg2,struct sysctl_req *req) {
    int value=__atomic_load_n(&cam_s20_rear_i2c_half_us,__ATOMIC_RELAXED);
    int error=sysctl_handle_int(oidp,&value,0,req);
    if(error||!req->newptr)return error;
    if(value!=1&&value!=2)return EINVAL;
    __atomic_store_n(&cam_s20_rear_i2c_half_us,value,__ATOMIC_RELAXED);return 0;
}

SYSCTL_DECL(_kern_redwood);
SYSCTL_PROC(_kern_redwood, OID_AUTO, camera_rear_i2c, CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED,
    0,0,cam_s20_rear_i2c_sysctl,"I","Next primary setup I2C half-period:1 or2us; NACK fallback remains2");
SYSCTL_PROC(_kern_redwood, OID_AUTO, camera_rear_diag, CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED,
    0, 0, cam_s20_rear_diag_sysctl, "I", "Next rear session: bit0 no group hold; bit1 cold start");
int redwood_camera_s20_turn;    /* kern.redwood.camera_turn: libredwood-media turns the front camera's pictures (bit 0) */
SYSCTL_INT(_kern_redwood, OID_AUTO, camera_turn, CTLFLAG_RW | CTLFLAG_ANYBODY | CTLFLAG_LOCKED, &redwood_camera_s20_turn, 0, "");
SYSCTL_PROC(_kern_redwood, OID_AUTO, camera_frame, CTLTYPE_OPAQUE | CTLFLAG_RD | CTLFLAG_LOCKED,
    NULL, 0, cam_s20_frame_sysctl, "S", "last S20 diagnostic frame, RWCAM001, root only");


/*
 * Continuous front-camera stream for /dev/redwood-camera (B, 2026-11-11). The bounded optical DMA test below is the
 * proven path; with a frame callback set it does not stop at the first frame: WDMA1 alternates between two buffers
 * (its 32 frame-pointer slots, even/odd) and every finished frame is handed to the callback until it returns false.
 */
/* data: one frame of MIPI RAW10 (four pixels in five bytes), `stride' bytes from one line to the next */
/* the stream's sensor mode: full 3648x2736 (Samsung's 3648x2736_30fps, FULL MIPI PLL) or the 2x2-binned 1824x1368 */
static bool cam_s20_ultra;
void redwood_camera_s20_set_ultra(bool ultra){cam_s20_ultra=ultra;}
static bool cam_s20_2la_live(redwood_s20_frame_fn fn,char *out,size_t cap);
static bool cam_s20_gw2_live(redwood_s20_frame_fn fn,char *out,size_t cap);
static bool cam_s20_full;
#define CAM_S20_W (cam_s20_full ? 3648U : 1824U)
#define CAM_S20_H (cam_s20_full ? 2736U : 1368U)
void redwood_camera_s20_set_full(bool full) { cam_s20_full = full; }

/* the lens to `position', 0 (the furthest) to 1023 as /dev/redwood-camera has it */
void
redwood_camera_s20_focus(uint32_t position)
{
	cam_s20_af_want = (position > 1023 ? 1023 : position) >> 1;
	cam_s20_af_pending = true;
}

/* where the lens is, 0 to 1023, or 0xffffffff with no motor */
uint32_t
redwood_camera_s20_focus_now(void)
{
	uint32_t p = cam_s20_af_pos;
	return p == 0xffffffffU ? p : p << 1;
}

void
redwood_camera_s20_exposure(uint32_t lines, uint32_t gain, uint32_t frame_lines)
{
	cam_s20_exp_lines = lines; cam_s20_exp_gain = gain; cam_s20_exp_frame = frame_lines;
	cam_s20_exp_pending = true;
}

static void cam_s20_dma_pattern(const uint64_t *v, unsigned n, char *out, size_t cap);
bool
redwood_camera_s20_stream(redwood_s20_frame_fn fn, char *out, size_t cap)
{
	uint64_t args[2] = { cam_s20_rear ? 2 : 1, 14 };
	if (fn == NULL || cam_s20_stream_fn != NULL) return false;
	cam_s20_stream_frames = cam_s20_stream_errors = 0;
	cam_s20_stream_exposure_failed = false;
	cam_s20_stream_fn = fn;
	bool clean=true;
    if(cam_s20_gw2_now)clean=cam_s20_gw2_live(fn,out,cap);
    else if(cam_s20_ultra)clean=cam_s20_2la_live(fn,out,cap);
    else cam_s20_dma_pattern(args, 2, out, cap);
	cam_s20_stream_fn = NULL;
	return clean && cam_s20_stream_frames != 0 && !cam_s20_stream_exposure_failed;
}

static void
cam_s20_dma_pattern(const uint64_t *v, unsigned n, char *out, size_t cap)
{
	/* v[0] 2: the rear main (S5K2LD 2016x1512 -> C-PHY -> CSIS0 -> WDMA2), one bounded frame (2026-11-12) */
	const bool rear = n > 0 && v[0] == 2;
	const bool optical = n > 0 && (v[0] == 1 || rear);
	const bool full_frames = optical && !rear && cam_s20_stream_fn != NULL && cam_s20_full;
	const bool rear_full = rear && cam_s20_stream_fn != NULL && redwood_camera_s20_rear_full();
	const uint32_t width = rear ? (rear_full ? 4032 : 2016) : optical ? (full_frames ? 3648 : 1824) : 320;
	const uint32_t height = rear ? (rear_full ? 3024 : 1512) : optical ? (full_frames ? 2736 : 1368) : 240, guard = 16384;
	const bool stream = optical && cam_s20_stream_fn != NULL;
	/* The stream has the DMA write MIPI RAW10 as it comes (format 7, ANDROID10: four pixels in five bytes, lines padded
	 * to 16 bytes -- verified 2026-11-11, logs/camera/front-stage-b-20261111/fmt7-preview.png): the very layout
	 * /dev/redwood-camera hands out, so a frame is one copy. The bounded tests keep 16-bit pixels (format 6). */
	const uint32_t dma_fmt = stream ? 7U : (n > 2 && (v[2] == 4 || v[2] == 7)) ? (uint32_t)v[2] : 6U;
	const uint32_t raw10_stride = ((width * 10 / 8) + 15) & ~15U;
	const uint32_t bytes = stream ? raw10_stride * height : width * height * 2;
	const uint32_t total = ((stream ? 2 * bytes + 3 * guard : bytes + 2 * guard) + guard - 1) & ~(guard - 1);
	char sensor_report[4096] = "{}";
	uint64_t sensor_mode = n > 1 ? v[1] : 2;
	bool exported = false, sensor_started = false;
	volatile uint8_t *route = NULL;
	uint32_t route_saved = 0;
	static const uint32_t vc_offs[] = { 0, 4, 8, 12, 0xa4, 0xb0 };
	static const uint32_t cmn_offs[] = { 0, 4, 0x14, 0x1c };
	static const uint32_t top_offs[] = { 8, 0x20, 0x24, 0x28 };
	static const uint32_t gate_offs[] = { 0x20e4, 0x20e8, 0x20ec, 0x20f0 };
	static volatile uint8_t *pmu, *cmu, *dma, *mmu0, *mmu1;
	volatile uint8_t *vc, *cmn;
	uint32_t vc_save[6], cmn_save[4], top_save[4], addr_save[32];
	uint32_t mmu_ctrl[2] = { 0 }, mmu_ver[2] = { 0 }, active = 0, irq = 0, err = 0;
	uint32_t changed = 0, guard_bad = 0, lo = 0xffff, hi = 0, i, chan, virtchan;
	uint64_t phys = 0;
	void *buffer = NULL;
	const char *phase = "map";
	bool saved = false, allocated = false, stopped = true, ended = false;
	if (!cap) return;
	if (rear) {
		cam_s20_rear_test_lines = n > 1 && v[1] > 0 && v[1] < 2000000 ? (uint32_t)v[1] : 16384;
		sensor_mode = 2;
	}
	if ((n > 0 && v[0] > 2) || sensor_mode > 14) { phase = "invalid test mode"; goto report; }
	if (!pmu) pmu = (volatile uint8_t *)cam_s20_io_map(PMU_PHYS, 0x4000);
	if (!pmu) goto report;
	if (!(cam_s20_rd(pmu, 0x1d84) & 1U)) { phase = "CSIS domain off"; goto report; }
	if (!cmu) cmu = (volatile uint8_t *)cam_s20_io_map(CMU_CSIS_PHYS, 0x8000);
	if (!cmu) goto report;
	/* Read SysMMU only with all its S1/S2 clock gates observed on. */
	for (i = 0; i < 4; i++) {
		if (!(cam_s20_rd(cmu, gate_offs[i]) & (1U << 21))) {
			phase = "SysMMU clock off"; goto report;
		}
	}
	if (cam_s20_smc4(SMC_PREPARE_PD_ONOFF, 1, CSIS_NEED_SMC, 2)) {
		phase = "CSIS secure access"; goto report;
	}
	if (!mmu0) mmu0 = (volatile uint8_t *)cam_s20_io_map(0x17110000, 0x100);
	if (!mmu1) mmu1 = (volatile uint8_t *)cam_s20_io_map(0x17140000, 0x100);
	if (!dma) dma = (volatile uint8_t *)cam_s20_io_map(0x170c0000, 0x5000);
	if (!mmu0 || !mmu1 || !dma) goto report;
	if (cam_s20_dma_quarantined) {
		/* a stop that was not acknowledged in time may have ended since: looked at again instead of refusing until a reboot */
		bool busy = ((cam_s20_rd(dma, 0x2090) | cam_s20_rd(dma, 0x3090)) & 1U) != 0;

		if (busy) { phase = "previous DMA did not stop; reboot required"; goto report; }
		cam_s20_dma_quarantined = false;
		redwood_camera_dma_put();
		printf("camera: the DMA that had not stopped is idle now: used again\n");
	}
	mmu_ver[0] = cam_s20_rd(mmu0, 0x34); mmu_ver[1] = cam_s20_rd(mmu1, 0x34);
	mmu_ctrl[0] = cam_s20_rd(mmu0, 0); mmu_ctrl[1] = cam_s20_rd(mmu1, 0);
	/* This path understands v7.x pass-through only. Never replace live tables. */
	if ((mmu_ver[0] >> 28) != 7 || (mmu_ver[1] >> 28) != 7 ||
	    (mmu_ctrl[0] & 3U) || (mmu_ctrl[1] & 3U)) {
		phase = "SysMMU not v7 pass-through"; goto report;
	}
	if (cam_s20_rd(dma, 0x20) & (1U << 31)) { phase = "pattern generator busy"; goto report; }
	/* Pattern generator is shared by four WDMAs: refuse any active owner. */
	for (chan = 1; chan <= 4; chan++) for (virtchan = 0; virtchan < 4; virtchan++) {
		uint32_t base = chan * 0x1000 + virtchan * 0x100;
		if ((cam_s20_rd(dma, base) | cam_s20_rd(dma, base + 0x90)) & 1U) {
			phase = "DMA already active"; goto report;
		}
	}
	vc = dma + (rear ? 0x3000 : 0x2000); cmn = dma + (rear ? 0x3400 : 0x2400);
	/* x1s: a stop that was not acknowledged leaves its write-1-to-clear sources set (the clearing below is skipped on that
	 * path), and every later session refused here until a restart (zoom in Camera, 2026-11-22). Every WDMA is idle by
	 * the check above: the stale sources are cleared, and only sources that stay set refuse. */
	if (cam_s20_rd(cmn, 8)) {
		cam_s20_wr(cmn, 8, cam_s20_rd(cmn, 8));
		if (cam_s20_rd(cmn, 8)) { phase = "DMA has pending interrupts"; goto report; }
		printf("camera: stale DMA interrupt sources cleared (idle)\n");
	}
	if (!redwood_camera_dma_get(total, &phys, &buffer)) { phase = "no free low-memory slot"; goto report; }
	allocated = true;
	if (optical) {
		route = (volatile uint8_t *)cam_s20_io_map(rear ? 0x1702041c : 0x17020418, 4);
		if (!route) { phase = "DMA input mux map"; goto release; }
		route_saved = cam_s20_rd(route, 0);
	}
	lck_mtx_lock(&cam_frame_lock);
	if (cam_frame_copy) kfree_data(cam_frame_copy, cam_frame_size);
	cam_frame_copy = NULL; cam_frame_size = 0;
	lck_mtx_unlock(&cam_frame_lock);
	if (!phys || phys + total > (1ULL << 32)) { phase = "invalid DMA address"; goto release; }
	memset(buffer, 0xa5, total);
	__asm__ volatile("dsb sy" ::: "memory");
	for (i = 0; i < 6; i++) vc_save[i] = cam_s20_rd(vc, vc_offs[i]);
	for (i = 0; i < 4; i++) {
		cmn_save[i] = cam_s20_rd(cmn, cmn_offs[i]);
		top_save[i] = cam_s20_rd(dma, top_offs[i]);
	}
	for (i = 0; i < 32; i++) addr_save[i] = cam_s20_rd(vc, 0x10 + i * 4);
	saved = true;
	if (route) cam_s20_wr(route, 0, rear ? 0U : 1U); /* x1slte CSIS1 -> WDMA1; the rear: CSIS0 -> WDMA2 (DT mux_reg) */
	cam_s20_wr(dma, 8, top_save[0] | 2U); /* IP_PROCESSING */
	cam_s20_wr(cmn, 0x14, cmn_save[2] & ~1U); /* no stale abort request */
	cam_s20_wr(cmn, 0, cmn_save[0] & ~1U); /* OTF input */
	cam_s20_wr(cmn, 0x1c, cmn_save[3] & ~3U); /* RAW10 */
	cam_s20_wr(cmn, 4, 0x01111110U); /* VC0 frame/error/abort sources; GIC remains masked */
	/* DMA data format (is-hw-csi-v5_3.h): 6 = RAW10 in 16 bits (default), 4 = Samsung's 10-bit pack,
	 * 7 = ANDROID10 (MIPI RAW10 bytes). Third argument of the bounded test picks another for a raw dump. */
	cam_s20_wr(vc, 4, (vc_save[1] & ~0x7fU) | dma_fmt);
	cam_s20_wr(vc, 8, 0); cam_s20_wr(vc, 12, 0);
	cam_s20_wr(vc, 0xa4, width);
	for (i = 0; i < 32; i++) cam_s20_wr(vc, 0x10 + i * 4, (uint32_t)(phys + guard + ((stream && (i & 1)) ? bytes + guard : 0)));
	{
		/* frames are read through a cached mapping when there is one (several times faster than the
		 * write-combined one); its lines are forgotten before every frame is read */
		void *cached = stream ? redwood_camera_dma_cached() : NULL;
		uint8_t *rd_base = cached ? (uint8_t *)cached : (uint8_t *)buffer;
		cam_s20_stream_cached = cached != NULL;
		cam_s20_stream_bytes = bytes;
		cam_s20_stream_buf[0] = (const volatile uint8_t *)(rd_base + guard);
		cam_s20_stream_buf[1] = (const volatile uint8_t *)(rd_base + guard + bytes + guard);
	}
	cam_s20_wr(vc, 0xb0, ~0U); /* all 32 frame pointers valid */
	cam_s20_wr(dma, 0x24, 0x00804640U); /* Samsung's horizontal blanking */
	cam_s20_wr(dma, 0x28, (height << 16) | width);
	cam_s20_wr(dma, 0x20, 0x400fffffU); /* dual pixels; generous bounded vblank */
	stopped = false;
	cam_s20_wr(vc, 0, 3U); /* output on, update pointer to slot zero */
	__asm__ volatile("dsb sy" ::: "memory");
	if (optical) {
		sensor_started = true;
		if (rear) {
			uint64_t rx = 2;

			cam_s20_optical_dma = dma;
			cam_s20_identify(&rx, 1, sensor_report, sizeof(sensor_report));
            if(!cam_s20_stream_frames)printf("camera: primary setup failed: %s",sensor_report);
			cam_s20_optical_dma = NULL;
		} else {
			cam_s20_optical_dma = dma;
			cam_s20_front_stream_probe(&sensor_mode, 1, sensor_report, sizeof(sensor_report));
			cam_s20_optical_dma = NULL;
		}
		/* The standalone probe appends a human-readable register line. */
		char *end = strchr(sensor_report, '\n');
		if (end) *end = 0;
	} else {
		cam_s20_wr(dma, 0x20, 0xc00fffffU);
	}
	for (i = 0; i < (optical ? 1U : 500U); i++) {
		irq |= cam_s20_rd(cmn, 8);
		if (irq & ((1U << 8) | (1U << 12))) break;
		cam_s20_wait_ms(1);
	}
	ended = (irq & (1U << 8)) != 0;
	err = cam_s20_rd(cmn, 0x18);
	cam_s20_wr(dma, 0x20, cam_s20_rd(dma, 0x20) & ~(1U << 31));
	cam_s20_wr(vc, 0, 0);
	cam_s20_wr(cmn, 0x14, cmn_save[2] | 1U); /* drain any writes before returning memory */
	for (i = 0; i < 300; i++) {      /* x1s: up to 300 ms (100 was often too short on the G981B) */
		uint32_t status = cam_s20_rd(cmn, 8);
		active = cam_s20_rd(vc, 0x90);
		if ((status & (1U << 16)) && !(active & 1U)) { stopped = true; break; }
		cam_s20_wait_ms(1);
	}
	__asm__ volatile("dsb sy" ::: "memory");
	if (!stopped) {
		cam_s20_dma_quarantined = true;
		phase = "DMA stop not acknowledged; buffer retained";
		goto report;
	}
	if (stream) {
		/* three guards: before, between and after the two frames */
		for (i = 0; i < guard; i++) {
			if (((volatile uint8_t *)buffer)[i] != 0xa5) guard_bad++;
			if (((volatile uint8_t *)buffer)[guard + bytes + i] != 0xa5) guard_bad++;
			if (((volatile uint8_t *)buffer)[2 * guard + 2 * bytes + i] != 0xa5) guard_bad++;
		}
		changed = cam_s20_stream_frames;
		ended = cam_s20_stream_frames != 0;
		irq &= ~0x01101000U;
	} else {
	for (i = 0; i < bytes; i++) if (((volatile uint8_t *)buffer)[guard + i] != 0xa5) changed++;
	for (i = 0; i < total; i++) if ((i < guard || i >= guard + bytes) &&
	    ((volatile uint8_t *)buffer)[i] != 0xa5) guard_bad++;
	}
	for (i = 0; !stream && i < bytes / 2; i++) {
		uint16_t pixel = ((volatile uint16_t *)((uint8_t *)buffer + guard))[i];
		if (pixel < lo) lo = pixel;
		if (pixel > hi) hi = pixel;
	}
	phase = guard_bad ? "DMA guard overwritten" : (ended && changed && !err && !(irq & 0x01101000U) ? "frame captured" : "no clean DMA frame");
	if (!stream && dma_fmt != 6U && ended && changed && !guard_bad) {
		/* raw dump of the buffer as the DMA wrote it (RWRAWDMP + width, height, format, bytes), for the host */
		size_t size = 4096 + bytes;
		uint8_t *copy = kalloc_data(size, Z_WAITOK | Z_ZERO);
		if (copy) {
			uint32_t header[] = { width, height, dma_fmt, bytes };
			memcpy(copy, "RWRAWDMP", 8);
			memcpy(copy + 8, header, sizeof(header));
			memcpy(copy + 4096, (uint8_t *)buffer + guard, bytes);
			lck_mtx_lock(&cam_frame_lock);
			if (cam_frame_copy) kfree_data(cam_frame_copy, cam_frame_size);
			cam_frame_copy = copy; cam_frame_size = size;
			lck_mtx_unlock(&cam_frame_lock);
			exported = true;
		}
	} else
	if (!stream && ended && changed && !guard_bad && !err && !(irq & 0x01101000U) && hi <= 1023)
		exported = cam_s20_save_frame((volatile uint16_t *)((uint8_t *)buffer + guard), width, height, optical);
	/* No pending transfer remains, so old addresses and the pool slot are safe to restore. */
	for (i = 0; i < 32; i++) cam_s20_wr(vc, 0x10 + i * 4, addr_save[i]);
	for (i = 1; i < 6; i++) cam_s20_wr(vc, vc_offs[i], vc_save[i]);
	cam_s20_wr(vc, 0, vc_save[0] & ~3U);
	cam_s20_wr(cmn, 8, cam_s20_rd(cmn, 8)); /* test's W1C sources */
	for (i = 0; i < 4; i++) cam_s20_wr(cmn, cmn_offs[i], cmn_save[i]);
	for (i = 1; i < 4; i++) cam_s20_wr(dma, top_offs[i], top_save[i]);
	cam_s20_wr(dma, 8, top_save[0]);
	if (route) cam_s20_wr(route, 0, route_saved);
release:
	if (allocated) redwood_camera_dma_put();
report:
	(void)snprintf(out, cap, "{\"stage\":\"S20-DMA-pattern\",\"phase\":\"%s\",\"sensor_test_ran\":%s,\"exported\":%s,\"mmu_version\":[\"%#x\",\"%#x\"],\"mmu_ctrl\":[\"%#x\",\"%#x\"],\"buffer_pa\":\"%#llx\",\"width\":%u,\"height\":%u,\"frame_end\":%s,\"irq\":\"%#x\",\"dma_error\":\"%#x\",\"active\":\"%#x\",\"changed_bytes\":%u,\"guard_errors\":%u,\"pixel_min\":%u,\"pixel_max\":%u,\"state_restored\":%s,\"buffer_retained\":%s,\"sensor\":%s}\n",
	    phase, sensor_started ? "true" : "false", exported ? "true" : "false", mmu_ver[0], mmu_ver[1], mmu_ctrl[0], mmu_ctrl[1], phys, width, height, ended ? "true" : "false",
	    irq, err, active, changed, guard_bad, lo, hi, (!saved || stopped) ? "true" : "false",
	    cam_s20_dma_quarantined ? "true" : "false", sensor_report);
}

/* Read-only CSIS1/DMA1 baseline: whitelist-only registers from Samsung's v5.3 block map. */
static void
cam_s20_csis_inventory(__unused const uint64_t *v, __unused unsigned n, char *out, size_t cap)
{
	volatile uint8_t *top = (volatile uint8_t *)cam_s20_io_map(CMU_TOP_PHYS, 0x3000);
	volatile uint8_t *csi = (volatile uint8_t *)cam_s20_io_map(CSIS1_PHYS, 0x1000);
	volatile uint8_t *vc0 = (volatile uint8_t *)cam_s20_io_map(CSIS1_DMA1_VC0_PHYS, 0x100);
	volatile uint8_t *common = (volatile uint8_t *)cam_s20_io_map(CSIS1_DMA1_COMMON_PHYS, 0x100);
	if (cap == 0) return;
	if (top == NULL || csi == NULL || vc0 == NULL || common == NULL) {
		(void)snprintf(out, cap, "{\"camera\":\"SAK3J1\",\"stage\":\"csis1-inventory\",\"result\":\"error\",\"phase\":\"map\"}\n");
		return;
	}
	(void)snprintf(out, cap,
	    "{\"camera\":\"SAK3J1\",\"stage\":\"csis1-inventory\",\"result\":\"ok\",\"write\":false,"
	    "\"csis1_version\":\"%#x\",\"cmn_ctrl\":\"%#x\",\"clk_ctrl\":\"%#x\",\"phy_status\":\"%#x\","
	    "\"frame_count_vc0\":\"%#x\",\"frame_count_vc1\":\"%#x\",\"irq_src0\":\"%#x\","
	    "\"top_csis_bus_mux\":\"%#x\",\"top_csis_bus_div\":\"%#x\",\"top_csis_bus_gate\":\"%#x\","
	    "\"dma_vc0_ctrl\":\"%#x\",\"dma_vc0_fmt\":\"%#x\",\"dma_vc0_active\":\"%#x\","
	    "\"dma_vc0_resol\":\"%#x\",\"dma_vc0_sequence\":\"%#x\",\"dma_common_ctrl\":\"%#x\","
	    "\"dma_common_irq\":\"%#x\",\"dma_common_err\":\"%#x\"}\n",
	    cam_s20_rd(csi, 0x0000), cam_s20_rd(csi, 0x0004), cam_s20_rd(csi, 0x0008), cam_s20_rd(csi, 0x0700),
	    cam_s20_rd(csi, 0x0500), cam_s20_rd(csi, 0x0504), cam_s20_rd(csi, 0x0014),
	    /* Exynos990 cmucal-sfr.c: upstream clock source, divider and gate.
	     * CSIS v5.3 moved DMA ACT_CTRL/RESOL past its 32 frame pointers;
	     * v5.1's 0x30/0x40 are frame-address registers on this hardware. */
	    cam_s20_rd(top, 0x104c), cam_s20_rd(top, 0x1848), cam_s20_rd(top, 0x204c),
	    cam_s20_rd(vc0, 0x0000), cam_s20_rd(vc0, 0x0004), cam_s20_rd(vc0, 0x0090),
	    cam_s20_rd(vc0, 0x00a4), cam_s20_rd(vc0, 0x00b4), cam_s20_rd(common, 0x0014),
	    cam_s20_rd(common, 0x0008), cam_s20_rd(common, 0x0018));
}

/*
 * Read CSIS clock/power state only after the documented CSIS power domain is on.
 * An earlier experiment read the DPHY while its domain/clocks were off and wedged the phone;
 * this command deliberately does not map or touch any DPHY registers or start the sensor.
 */
static void
cam_s20_csis_power_inventory(__unused const uint64_t *v, __unused unsigned n, char *out, size_t cap)
{
	volatile uint8_t *pmu = (volatile uint8_t *)cam_s20_io_map(PMU_PHYS, 0x4000);
	volatile uint8_t *cmu = NULL;
	uint32_t conf = 0, status = 0, initial_status = 0, after = 0, regs[8] = { 0 }, i;
	uint64_t smc = 0;
	bool initially_on, enabled_here = false, map_ok = false;
	if (cap == 0) return;
	if (pmu == NULL) {
		(void)snprintf(out, cap, "{\"stage\":\"csis-power-inventory\",\"result\":\"error\",\"phase\":\"PMU map\"}\n");
		return;
	}
	conf = cam_s20_rd(pmu, 0x1d80);
	status = cam_s20_rd(pmu, 0x1d84);
	initial_status = status;
	initially_on = (status & 1U) != 0;
	if (!initially_on) {
		cam_s20_wr(pmu, 0x1d80, conf | 1U);
		for (i = 0; i < 100; i++) {
			status = cam_s20_rd(pmu, 0x1d84);
			if (status & 1U) break;
			cam_s20_wait_ms(1);
		}
		if (status & 1U) {
			enabled_here = true;
			smc = cam_s20_smc4(SMC_PREPARE_PD_ONOFF, 1, CSIS_NEED_SMC, 2);
		}
	}
	if (status & 1U) cmu = (volatile uint8_t *)cam_s20_io_map(CMU_CSIS_PHYS, 0x8000);
	map_ok = cmu != NULL;
	if (map_ok) {
		regs[0] = cam_s20_rd(cmu, 0x0600);
		regs[1] = cam_s20_rd(cmu, 0x0604);
		regs[2] = cam_s20_rd(cmu, 0x1800);
		regs[3] = cam_s20_rd(cmu, 0x3064);
		regs[4] = cam_s20_rd(cmu, 0x3078);
		regs[5] = cam_s20_rd(cmu, 0x3058);
		regs[6] = cam_s20_rd(cmu, 0x2018);
		regs[7] = cam_s20_rd(cmu, 0x2000);
	}
	if (enabled_here) {
		cam_s20_wr(pmu, 0x1d80, conf & ~1U);
		for (i = 0; i < 100; i++) {
			status = cam_s20_rd(pmu, 0x1d84);
			if (!(status & 1U)) break;
			cam_s20_wait_ms(1);
		}
	}
	after = cam_s20_rd(pmu, 0x1d84);
	if (map_ok) {
		(void)snprintf(out, cap,
		    "{\"stage\":\"csis-power-inventory\",\"result\":\"ok\",\"write_sensor\":false,\"phy_access\":false,"
		    "\"initial_domain_status\":\"%#x\",\"csis_bus_mux\":\"%#x\",\"csis_bus_mux_cfg\":\"%#x\","
		    "\"csis_bus_div\":\"%#x\",\"csis_qch\":\"%#x\",\"csis_dma_qch\":\"%#x\","
		    "\"csis_cmu_qch\":\"%#x\",\"csis_aclk1\":\"%#x\",\"csis_pclk\":\"%#x\","
		    "\"final_domain_status\":\"%#x\",\"powered_for_snapshot\":%s,\"smc_result\":\"%#llx\",\"state_restored\":%s}\n",
	    initial_status, regs[0], regs[1], regs[2], regs[3], regs[4], regs[5], regs[6], regs[7], after,
	    enabled_here ? "true" : "false", smc, (after & 1U) == (initial_status & 1U) ? "true" : "false");
	} else {
		(void)snprintf(out, cap,
		    "{\"stage\":\"csis-power-inventory\",\"result\":\"error\",\"phase\":\"CSIS domain or CMU map\","
		    "\"initial_conf\":\"%#x\",\"final_status\":\"%#x\",\"powered_for_snapshot\":%s,\"smc_result\":\"%#llx\"}\n",
		    conf, after, enabled_here ? "true" : "false", smc);
	}
}

/* Reversible CSIS1 QCH gate check. Requires the CSIS power domain to already be on. */
void
cam_s20_csis_gate_probe(__unused const uint64_t *v, __unused unsigned n, char *out, size_t cap)
{
	volatile uint8_t *pmu = (volatile uint8_t *)cam_s20_io_map(PMU_PHYS, 0x4000);
	volatile uint8_t *cmu = NULL;
	volatile uint8_t *csi = NULL;
	uint32_t pd_status, cmu_before = 0, cmu_during = 0, cmu_after = 0;
	uint32_t before = 0, during = 0, after = 0;
	bool cmu_changed = false, changed = false;
	if (cap == 0) return;
	if (pmu == NULL || !(cam_s20_rd(pmu, 0x1d84) & 1U)) {
		(void)snprintf(out, cap, "{\"stage\":\"csis1-gate\",\"result\":\"error\",\"phase\":\"CSIS power domain off\",\"write_sensor\":false}\n");
		return;
	}
	cmu = (volatile uint8_t *)cam_s20_io_map(CMU_CSIS_PHYS, 0x8000);
	csi = (volatile uint8_t *)cam_s20_io_map(CSIS1_PHYS, 0x1000);
	if (cmu == NULL || csi == NULL) {
		(void)snprintf(out, cap, "{\"stage\":\"csis1-gate\",\"result\":\"error\",\"phase\":\"map\",\"write_sensor\":false}\n");
		return;
	}
	cmu_before = cam_s20_rd(cmu, 0x3058);
	if (!(cmu_before & 1U)) {
		cam_s20_wr(cmu, 0x3058, cmu_before | 1U);
		cmu_changed = true;
	}
	cam_s20_wait_ms(2);
	cmu_during = cam_s20_rd(cmu, 0x3058);
	before = cam_s20_rd(cmu, 0x3064);
	if (!(before & 1U)) {
		cam_s20_wr(cmu, 0x3064, before | 1U);
		changed = true;
	}
	cam_s20_wait_ms(2);
	during = cam_s20_rd(cmu, 0x3064);
	(void)cam_s20_rd(csi, 0x0000); /* CSIS version: known safe read after its QCH is enabled. */
	if (changed) {
		cam_s20_wr(cmu, 0x3064, during & ~1U);
		cam_s20_wait_ms(2);
	}
	after = cam_s20_rd(cmu, 0x3064);
	if (cmu_changed) {
		cam_s20_wr(cmu, 0x3058, cmu_during & ~1U);
		cam_s20_wait_ms(2);
		cmu_after = cam_s20_rd(cmu, 0x3058);
	} else {
		cmu_after = cmu_before;
	}
	pd_status = cam_s20_rd(pmu, 0x1d84);
	(void)snprintf(out, cap,
	    "{\"stage\":\"csis1-gate\",\"result\":\"ok\",\"write_sensor\":false,\"phy_access\":false,"
	    "\"cmu_qch_before\":\"%#x\",\"cmu_qch_enabled\":\"%#x\",\"cmu_qch_after\":\"%#x\","
	    "\"qch_before\":\"%#x\",\"qch_enabled\":\"%#x\",\"qch_after\":\"%#x\","
	    "\"domain_status\":\"%#x\",\"state_restored\":%s}\n",
	    cmu_before, cmu_during, cmu_after, before, during, after, pd_status,
	    (((after ^ before) & 1U) == 0 && ((cmu_after ^ cmu_before) & 1U) == 0) ? "true" : "false");
}

/* Read-only idle DPHY snapshot. Parent CSIS QCHs and PHY isolation are restored exactly. */
static void
cam_s20_csis_dphy_snapshot(__unused const uint64_t *v, __unused unsigned n, char *out, size_t cap)
{
	volatile uint8_t *pmu = (volatile uint8_t *)cam_s20_io_map(PMU_PHYS, 0x4000);
	volatile uint8_t *cmu = NULL, *dphy = NULL, *isppre = NULL;
	uint32_t qcmu = 0, qcmu_on = 0, qcmu_after = 0, qcsis = 0, qcsis_on = 0, qcsis_after = 0;
	uint32_t iso = 0, iso_after = 0, reset_sysreg = 0, sc[8] = { 0 }, lane0[6] = { 0 };
	static const uint32_t sc_offs[] = { 0x0000, 0x0004, 0x0008, 0x000c, 0x0010, 0x0014, 0x0018, 0x0030 };
	static const uint32_t lane_offs[] = { 0x0100, 0x0104, 0x0108, 0x010c, 0x0110, 0x0114 };
	bool cmu_changed = false, csis_changed = false, iso_changed = false, gates_ready, restored;
	unsigned i;
	if (cap == 0) return;
	if (pmu == NULL || !(cam_s20_rd(pmu, 0x1d84) & 1U)) {
		(void)snprintf(out, cap, "{\"stage\":\"csis-dphy-snapshot\",\"result\":\"error\",\"phase\":\"CSIS domain off\"}\n");
		return;
	}
	cmu = (volatile uint8_t *)cam_s20_io_map(CMU_CSIS_PHYS, 0x8000);
	dphy = (volatile uint8_t *)cam_s20_io_map(CSIS1_DPHY_PHYS, 0x500);
	isppre = (volatile uint8_t *)cam_s20_io_map(ISPPRE_SYSREG_PHYS, 0x4);
	if (cmu == NULL || dphy == NULL || isppre == NULL) {
		(void)snprintf(out, cap, "{\"stage\":\"csis-dphy-snapshot\",\"result\":\"error\",\"phase\":\"map\"}\n");
		return;
	}
	qcmu = cam_s20_rd(cmu, 0x3058);
	qcsis = cam_s20_rd(cmu, 0x3064);
	iso = cam_s20_rd(pmu, 0x0730);
	if (!(qcmu & 1U)) { cam_s20_wr(cmu, 0x3058, qcmu | 1U); cmu_changed = true; }
	cam_s20_wait_ms(2);
	qcmu_on = cam_s20_rd(cmu, 0x3058);
	if (!(qcsis & 1U)) { cam_s20_wr(cmu, 0x3064, qcsis | 1U); csis_changed = true; }
	cam_s20_wait_ms(2);
	qcsis_on = cam_s20_rd(cmu, 0x3064);
	gates_ready = (qcmu_on & 1U) != 0 && (qcsis_on & 1U) != 0;
	if (gates_ready) {
		if (!(iso & 1U)) { cam_s20_wr(pmu, 0x0730, iso | 1U); iso_changed = true; }
		cam_s20_wait_ms(1);
		reset_sysreg = cam_s20_rd(isppre, 0);
		for (i = 0; i < sizeof(sc_offs) / sizeof(sc_offs[0]); i++) sc[i] = cam_s20_rd(dphy, sc_offs[i]);
		for (i = 0; i < sizeof(lane_offs) / sizeof(lane_offs[0]); i++) lane0[i] = cam_s20_rd(dphy, lane_offs[i]);
	}
	if (iso_changed) cam_s20_wr(pmu, 0x0730, iso);
	iso_after = cam_s20_rd(pmu, 0x0730);
	if (csis_changed) cam_s20_wr(cmu, 0x3064, qcsis);
	qcsis_after = cam_s20_rd(cmu, 0x3064);
	if (cmu_changed) cam_s20_wr(cmu, 0x3058, qcmu);
	qcmu_after = cam_s20_rd(cmu, 0x3058);
	restored = ((iso_after ^ iso) & 1U) == 0 && ((qcsis_after ^ qcsis) & 1U) == 0 &&
	    ((qcmu_after ^ qcmu) & 1U) == 0;
	(void)snprintf(out, cap,
	    "{\"stage\":\"csis-dphy-snapshot\",\"result\":\"%s\",\"phase\":\"%s\",\"write_sensor\":false,\"write_dphy\":false,"
	    "\"cmu_qch_before\":\"%#x\",\"cmu_qch_on\":\"%#x\",\"cmu_qch_after\":\"%#x\","
	    "\"csis_qch_before\":\"%#x\",\"csis_qch_on\":\"%#x\",\"csis_qch_after\":\"%#x\","
	    "\"isolation_before\":\"%#x\",\"isolation_after\":\"%#x\",\"reset_sysreg\":\"%#x\","
	    "\"sc_gnr_con0\":\"%#x\",\"sc_gnr_con1\":\"%#x\",\"sc_ana_con0\":\"%#x\",\"sc_ana_con1\":\"%#x\","
	    "\"sc_ana_con2\":\"%#x\",\"sc_ana_con3\":\"%#x\",\"sc_time_con0\":\"%#x\","
	    "\"lane0_gnr_con0\":\"%#x\",\"lane0_gnr_con1\":\"%#x\",\"lane0_ana_con0\":\"%#x\","
	    "\"lane0_ana_con1\":\"%#x\",\"lane0_ana_con2\":\"%#x\",\"lane0_ana_con3\":\"%#x\",\"state_restored\":%s}\n",
	    gates_ready ? "ok" : "error", gates_ready ? "readback" : "QCH enable",
	    qcmu, qcmu_on, qcmu_after, qcsis, qcsis_on, qcsis_after, iso, iso_after, reset_sysreg,
	    sc[0], sc[1], sc[2], sc[3], sc[4], sc[5], sc[6],
	    lane0[0], lane0[1], lane0[2], lane0[3], lane0[4], lane0[5], restored ? "true" : "false");
}

/* Restore the documented CSIS power-domain TZPC access through EL3; read reset sysreg only. */
static void
cam_s20_csis_secure_probe(__unused const uint64_t *v, __unused unsigned n, char *out, size_t cap)
{
	volatile uint8_t *pmu = (volatile uint8_t *)cam_s20_io_map(PMU_PHYS, 0x4000);
	volatile uint8_t *sysreg;
	uint32_t status;
	uint64_t smc;
	if (cap == 0) return;
	if (pmu == NULL || !(cam_s20_rd(pmu, 0x1d84) & 1U)) {
		(void)snprintf(out, cap, "{\"stage\":\"csis-secure\",\"result\":\"error\",\"phase\":\"CSIS domain off\"}\n");
		return;
	}
	/* exynos9830.dts: pd-csis need_smc=0x17010204; Linux calls this on domain-on. */
	smc = cam_s20_smc4(SMC_PREPARE_PD_ONOFF, 1, CSIS_NEED_SMC, 2);
	sysreg = (volatile uint8_t *)cam_s20_io_map(ISPPRE_SYSREG_PHYS, 0x4);
	if (sysreg == NULL) {
		(void)snprintf(out, cap, "{\"stage\":\"csis-secure\",\"result\":\"error\",\"phase\":\"sysreg map\",\"smc\":\"%#llx\"}\n", smc);
		return;
	}
	status = cam_s20_rd(sysreg, 0);
	(void)snprintf(out, cap,
	    "{\"stage\":\"csis-secure\",\"result\":\"ok\",\"smc\":\"%#llx\",\"domain_status\":\"%#x\","
	    "\"reset_sysreg_read\":\"%#x\",\"wrote_registers\":false}\n",
	    smc, cam_s20_rd(pmu, 0x1d84), status);
}

/* One-register reset-bit toggle and restore after the Samsung CSIS DTZPC restore; lanes stay disabled. */
static void
cam_s20_csis_reset_probe(__unused const uint64_t *v, __unused unsigned n, char *out, size_t cap)
{
	volatile uint8_t *pmu = (volatile uint8_t *)cam_s20_io_map(PMU_PHYS, 0x4000);
	volatile uint8_t *sysreg;
	uint32_t before, requested, during, after;
	uint64_t smc;
	if (cap == 0) return;
	if (pmu == NULL || !(cam_s20_rd(pmu, 0x1d84) & 1U)) {
		(void)snprintf(out, cap, "{\"stage\":\"csis-reset\",\"result\":\"error\",\"phase\":\"CSIS domain off\"}\n");
		return;
	}
	smc = cam_s20_smc4(SMC_PREPARE_PD_ONOFF, 1, CSIS_NEED_SMC, 2);
	sysreg = (volatile uint8_t *)cam_s20_io_map(ISPPRE_SYSREG_PHYS, 0x4);
	if (sysreg == NULL) {
		(void)snprintf(out, cap, "{\"stage\":\"csis-reset\",\"result\":\"error\",\"phase\":\"sysreg map\",\"smc\":\"%#llx\"}\n", smc);
		return;
	}
	before = cam_s20_rd(sysreg, 0);
	requested = before ^ (1U << 3); /* verify both reset values, then restore; lanes and clocks remain disabled */
	cam_s20_wr(sysreg, 0, requested);
	during = cam_s20_rd(sysreg, 0);
	cam_s20_wr(sysreg, 0, before);
	after = cam_s20_rd(sysreg, 0);
	(void)snprintf(out, cap,
	    "{\"stage\":\"csis-reset\",\"result\":\"%s\",\"smc\":\"%#llx\","
	    "\"before\":\"%#x\",\"requested\":\"%#x\",\"during\":\"%#x\",\"after\":\"%#x\","
	    "\"restored\":%s,\"dphy_written\":false,\"sensor_streamed\":false}\n",
	    during == requested && after == before ? "ok" : "error", smc, before, requested, during, after,
	    after == before ? "true" : "false");
}

static void
cam_s20_update_bits(volatile uint8_t *bank, uint32_t offset, uint32_t mask, uint32_t value)
{
	uint32_t old = cam_s20_rd(bank, offset);
	cam_s20_wr(bank, offset, (old & ~mask) | (value & mask));
}

#include "redwood_camera_regdump.h"

/* Read every address of the shared list (same list as the Samsung reference helper). Pages are mapped once
 * and kept: ml_io_map has no unmap here, and the list touches about a dozen pages. */
static uint32_t cam_regdump_vals[CAM_REGDUMP_COUNT];
static void
cam_s20_regdump(void)
{
	static vm_offset_t page_phys[24];
	static volatile uint8_t *page_virt[24];
	static unsigned pages;
	unsigned i, p;
	for (i = 0; i < CAM_REGDUMP_COUNT; i++) {
		vm_offset_t pa = cam_regdump_addr[i] & ~(vm_offset_t)0xfff;
		volatile uint8_t *va = NULL;
		for (p = 0; p < pages; p++) if (page_phys[p] == pa) { va = page_virt[p]; break; }
		if (va == NULL && pages < 24) {
			va = (volatile uint8_t *)cam_s20_io_map(pa, 0x1000);
			page_phys[pages] = pa; page_virt[pages] = va; pages++;
		}
		cam_regdump_vals[i] = va ? cam_s20_rd(va, cam_regdump_addr[i] & 0xfff) : 0xdeadbeefU;
	}
}

/* Samsung Exynos990 CONFIG_PABLO_V8_1_0 selects PHY 0x0503.0000, D-PHY.
 * Port of phy-exynos-mipi.c::__set_phy_cfg_0503_0000_dcphy for SAK3J1:
 * 4 lanes (zero-based 3), 1807 Mbps, settle 9, skew delay 3. */
/* Diagnostic pacing uses only the established PHY write addresses. */
static void
cam_s20_phy_wr(volatile uint8_t *bank, uint32_t offset, uint32_t value, unsigned pace_us)
{
    cam_s20_wr(bank, offset, value);
    if (pace_us) {
        (void)cam_s20_rd(bank, offset);
        delay_for_interval(pace_us, NSEC_PER_USEC);
    }
}

static void
cam_s20_phy_update(volatile uint8_t *bank, uint32_t offset, uint32_t mask, uint32_t value, unsigned pace_us)
{
    cam_s20_phy_wr(bank, offset, (cam_s20_rd(bank, offset) & ~mask) | (value & mask), pace_us);
}

static void
cam_s20_dphy_3j1_config(volatile uint8_t *dphy, volatile uint8_t *bias, unsigned pace_us)
{
    static const uint32_t lane_base[] = { 0x0100, 0x0200, 0x0300, 0x0400 };
    unsigned i;

    cam_s20_phy_wr(dphy, 0x0000, 0, pace_us);
    for (i = 0; i < 4; i++) cam_s20_phy_wr(dphy, lane_base[i], 0, pace_us);
    delay_for_interval(200, NSEC_PER_USEC);
    cam_s20_phy_wr(bias, 0x0000, 0x10, pace_us);
    cam_s20_phy_wr(bias, 0x0004, 0x110, pace_us);
    cam_s20_phy_wr(bias, 0x0008, 0x3223, pace_us);

    /* Exact __set_phy_cfg_0503_0000_dcphy() recipe, mode D/type D (CSI B/D/F). */
    cam_s20_phy_wr(dphy, 0x0004, 0x1450, pace_us);
    cam_s20_phy_wr(dphy, 0x0008, 0x8, pace_us);
    /* Keep the write order as well as the final values: skew is updated
     * by the data-lane loop after the clock lane has been enabled. */
    cam_s20_phy_wr(dphy, 0x0010, 0x2, pace_us);
    cam_s20_phy_wr(dphy, 0x0014, 0x8680, pace_us);
    cam_s20_phy_wr(dphy, 0x0018, 0x4000, pace_us);
    cam_s20_phy_wr(dphy, 0x0030, 0x301, pace_us);
    cam_s20_phy_wr(dphy, 0x0000, 1, pace_us); /* clock enable last */

    for (i = 0; i < 4; i++) {
        uint32_t b = lane_base[i];
        cam_s20_phy_wr(dphy, b + 0x0004, 0x1450, pace_us);
        cam_s20_phy_wr(dphy, b + 0x0010, 0x2, pace_us);
        /* Samsung updates the preceding lane (the clock on iteration zero). */
        cam_s20_phy_update(dphy, i * 0x100 + 0x0010, 3U << 8, 3U << 8, pace_us);
        cam_s20_phy_wr(dphy, b + 0x0014, 0x8680, pace_us);
        cam_s20_phy_wr(dphy, b + 0x0018, 0x4000, pace_us);
        cam_s20_phy_update(dphy, b + 0x0030, 0xffU, 9, pace_us);
		cam_s20_phy_update(dphy, b + 0x0030, 1U << 8, 0, pace_us); /* settle_clk_sel=0 at 1807 Mbps */
        cam_s20_phy_wr(dphy, b + 0x0034, 3, pace_us);
		cam_s20_phy_update(dphy, b + 0x0040, 1U, 1U, pace_us); /* skew_cal_en=1 at >=1500 Mbps */
        cam_s20_phy_wr(dphy, b + 0x0050, 0x81a, pace_us);
        cam_s20_phy_wr(dphy, b, 1, pace_us); /* data enable last */
    }
	/* __set_phy_cfg_0503_0000_dcphy() settles for 200 us after lane enable. */
	delay_for_interval(200, NSEC_PER_USEC);
}

/* Configure, inspect and exactly restore the idle DPHY; no sensor or CSIS core writes. */
void
cam_s20_csis_dphy_config_probe(__unused const uint64_t *v, __unused unsigned n, char *out, size_t cap)
{
	static const uint32_t sc_offs[] = { 0x0000, 0x0004, 0x0008, 0x000c, 0x0010, 0x0014, 0x0018, 0x0030 };
	static const uint32_t lane_base[] = { 0x0100, 0x0200, 0x0300, 0x0400 };
	static const uint32_t lane_regs[] = { 0x0000, 0x0004, 0x0008, 0x000c, 0x0010, 0x0014, 0x0018, 0x0030, 0x0034, 0x0040, 0x0050 };
	static const uint32_t bias_offs[] = { 0x0000, 0x0004, 0x0008, 0x0010 };
	volatile uint8_t *pmu = (volatile uint8_t *)cam_s20_io_map(PMU_PHYS, 0x4000);
	volatile uint8_t *cmu = NULL, *dphy = NULL, *bias = NULL, *isppre = NULL;
	uint32_t sc_saved[8] = { 0 }, lane_saved[4][11] = { { 0 } }, bias_saved[3] = { 0 };
	uint32_t qcmu = 0, qcsis = 0, iso = 0, reset = 0, qcmu_after = 0, qcsis_after = 0, iso_after = 0, reset_after = 0;
	uint32_t verify = 0, lane0_cfg = 0, lane3_cfg = 0, settle_cfg = 0, i, j;
	uint8_t ldo5 = 0, ldo6 = 0;
	int ldo5_status, ldo6_status;
	uint64_t smc;
	bool cmu_changed = false, csis_changed = false, iso_changed = false, saved = false;
	bool configured = false, state_ok = false;
	char phase[32] = "map";
	if (cap == 0) return;
	if (pmu == NULL || !(cam_s20_rd(pmu, 0x1d84) & 1U)) {
		(void)snprintf(out, cap, "{\"stage\":\"csis-dphy-config\",\"result\":\"error\",\"phase\":\"CSIS domain off\"}\n");
		return;
	}
	ldo5_status = redwood_spmic_read(1, 0x2e, &ldo5);
	ldo6_status = redwood_spmic_read(1, 0x2f, &ldo6);
	if (ldo5_status != 0 || ldo6_status != 0 || (ldo5 & 0xc0) != 0xc0 || (ldo6 & 0xc0) != 0xc0) {
		(void)snprintf(out, cap, "{\"stage\":\"csis-dphy-config\",\"result\":\"error\",\"phase\":\"PHY rails off\",\"ldo5s\":\"%#x\",\"ldo6s\":\"%#x\"}\n", ldo5, ldo6);
		return;
	}
	cmu = (volatile uint8_t *)cam_s20_io_map(CMU_CSIS_PHYS, 0x8000);
	dphy = (volatile uint8_t *)cam_s20_io_map(CSIS1_DPHY_PHYS, 0x500);
	bias = (volatile uint8_t *)cam_s20_io_map(CSIS1_DPHY_BIAS_PHYS, 0x100);
	isppre = (volatile uint8_t *)cam_s20_io_map(ISPPRE_SYSREG_PHYS, 0x4);
	if (cmu == NULL || dphy == NULL || bias == NULL || isppre == NULL) {
		(void)snprintf(out, cap, "{\"stage\":\"csis-dphy-config\",\"result\":\"error\",\"phase\":\"map\"}\n");
		return;
	}
	smc = cam_s20_smc4(SMC_PREPARE_PD_ONOFF, 1, CSIS_NEED_SMC, 2);
	if (smc != 0) {
		(void)snprintf(out, cap, "{\"stage\":\"csis-dphy-config\",\"result\":\"error\",\"phase\":\"CSIS DTZPC\",\"smc\":\"%#llx\"}\n", smc);
		return;
	}
	qcmu = cam_s20_rd(cmu, 0x3058); qcsis = cam_s20_rd(cmu, 0x3064); iso = cam_s20_rd(pmu, 0x0730);
	if (!(qcmu & 1U)) { cam_s20_update_bits(cmu, 0x3058, 1U, 1U); cmu_changed = true; }
	cam_s20_wait_ms(2);
	if (!(qcsis & 1U)) { cam_s20_update_bits(cmu, 0x3064, 1U, 1U); csis_changed = true; }
	cam_s20_wait_ms(2);
	if (!(cam_s20_rd(cmu, 0x3058) & 1U) || !(cam_s20_rd(cmu, 0x3064) & 1U)) {
		(void)snprintf(phase, sizeof(phase), "QCH enable"); goto cleanup;
	}
	if (!(iso & 1U)) { cam_s20_update_bits(pmu, 0x0730, 1U, 1U); iso_changed = true; }
	cam_s20_wait_ms(1);
	reset = cam_s20_rd(isppre, 0);
	for (i = 0; i < 8; i++) sc_saved[i] = cam_s20_rd(dphy, sc_offs[i]);
	for (i = 0; i < 4; i++) for (j = 0; j < 11; j++) lane_saved[i][j] = cam_s20_rd(dphy, lane_base[i] + lane_regs[j]);
	for (i = 0; i < 3; i++) bias_saved[i] = cam_s20_rd(bias, bias_offs[i]);
	saved = true;
	(void)snprintf(phase, sizeof(phase), "configure");
	cam_s20_wr(isppre, 0, reset & ~(1U << 3)); /* Samsung PHY driver asserts reset around PHY set */
	cam_s20_dphy_3j1_config(dphy, bias, 0);
	cam_s20_wr(isppre, 0, reset | (1U << 3));
	cam_s20_wait_ms(1);
	lane0_cfg = cam_s20_rd(dphy, 0x0100); lane3_cfg = cam_s20_rd(dphy, 0x0400);
	settle_cfg = cam_s20_rd(dphy, 0x0130) & 0xffU;
    /* Enable is bit 0; high status bits read back as 1 on this live PHY. */
    configured = (cam_s20_rd(dphy, 0x0000) & 1U) != 0 && (lane0_cfg & 1U) != 0 &&
        (lane3_cfg & 1U) != 0 && settle_cfg == 9U;
	if (configured) (void)snprintf(phase, sizeof(phase), "configured");
	else (void)snprintf(phase, sizeof(phase), "readback");
cleanup:
	if (saved) {
		cam_s20_wr(isppre, 0, reset & ~(1U << 3));
		for (i = 4; i > 0; i--) for (j = 11; j > 0; j--) cam_s20_wr(dphy, lane_base[i - 1] + lane_regs[j - 1], lane_saved[i - 1][j - 1]);
		for (i = 8; i > 0; i--) cam_s20_wr(dphy, sc_offs[i - 1], sc_saved[i - 1]);
		for (i = 3; i > 0; i--) cam_s20_wr(bias, bias_offs[i - 1], bias_saved[i - 1]);
		cam_s20_wr(isppre, 0, reset);
		for (i = 0; i < 8; i++) if (cam_s20_rd(dphy, sc_offs[i]) != sc_saved[i]) verify++;
		for (i = 0; i < 4; i++) for (j = 0; j < 11; j++) if (cam_s20_rd(dphy, lane_base[i] + lane_regs[j]) != lane_saved[i][j]) verify++;
		for (i = 0; i < 3; i++) if (cam_s20_rd(bias, bias_offs[i]) != bias_saved[i]) verify++;
		reset_after = cam_s20_rd(isppre, 0);
	}
	if (iso_changed) cam_s20_update_bits(pmu, 0x0730, 1U, iso & 1U);
	iso_after = cam_s20_rd(pmu, 0x0730);
	if (csis_changed) cam_s20_update_bits(cmu, 0x3064, 1U, qcsis & 1U);
	qcsis_after = cam_s20_rd(cmu, 0x3064);
	if (cmu_changed) cam_s20_update_bits(cmu, 0x3058, 1U, qcmu & 1U);
	qcmu_after = cam_s20_rd(cmu, 0x3058);
	state_ok = saved && verify == 0 && reset_after == reset && ((iso_after ^ iso) & 1U) == 0 &&
	    ((qcsis_after ^ qcsis) & 1U) == 0 && ((qcmu_after ^ qcmu) & 1U) == 0;
	(void)snprintf(out, cap,
	    "{\"stage\":\"csis-dphy-config\",\"result\":\"%s\",\"phase\":\"%s\",\"sensor_streamed\":false,"
	    "\"clock_enabled\":%s,\"lane0_enable\":\"%#x\",\"lane3_enable\":\"%#x\",\"settle\":\"%#x\","
	    "\"restore_mismatches\":%u,\"state_restored\":%s,\"smc\":\"%#llx\"}\n",
	    configured && state_ok ? "ok" : "error", phase, saved ? "true" : "false",
	    lane0_cfg, lane3_cfg, settle_cfg,
	    verify, state_ok ? "true" : "false", smc);
}

/* Read the two S2MPS22 rails required by the Exynos990 CSIS PHY. This is
 * intentionally read-only: no DPHY test is safe until the prior disconnect is
 * understood and these rails' baseline is known. */
static void
cam_s20_csis_rails(__unused const uint64_t *v, __unused unsigned n, char *out, size_t cap)
{
	uint8_t ldo5 = 0, ldo6 = 0;
	int r5, r6;
	if (cap == 0) return;
	r5 = redwood_spmic_read(1, 0x2e, &ldo5); /* S2MPS22 LDO5S, 0.85 V */
	r6 = redwood_spmic_read(1, 0x2f, &ldo6); /* S2MPS22 LDO6S, 1.8 V */
	(void)snprintf(out, cap,
	    "{\"stage\":\"csis-phy-rails\",\"result\":\"%s\",\"ldo5s\":\"%#x\",\"ldo5s_status\":%d,"
	    "\"ldo6s\":\"%#x\",\"ldo6s_status\":%d,\"writes\":false}\n",
	    r5 == 0 && r6 == 0 ? "ok" : "error", ldo5, r5, ldo6, r6);
}

#define CAM_3J1_BURST_ADDR 0xf0000001U
#define CAM_3J1_BURST_DATA 0xf0000002U
#define CAM_3J1_DELAY      0xf0000003U

static bool
cam_3j1_write16(struct redwood_bbi2c *bus, uint16_t reg, uint16_t value)
{
	uint8_t bytes[4] = { reg >> 8, reg, value >> 8, value };
	return redwood_bbi2c_xfer(bus, 0x10, bytes, sizeof(bytes), NULL, 0);
}

static bool
cam_3j1_write8(struct redwood_bbi2c *bus, uint16_t reg, uint8_t value)
{
	uint8_t bytes[3] = { reg >> 8, reg, value };
	return redwood_bbi2c_xfer(bus, 0x10, bytes, sizeof(bytes), NULL, 0);
}

static bool
cam_3j1_read16(struct redwood_bbi2c *bus, uint16_t reg, uint16_t *value)
{
	uint8_t address[2] = { reg >> 8, reg }, data[2] = { 0 };
	if (value == NULL || !redwood_bbi2c_xfer(bus, 0x10, address, sizeof(address), data, sizeof(data))) return false;
	*value = (uint16_t)((data[0] << 8) | data[1]);
	return true;
}

static bool
cam_3j1_read8(struct redwood_bbi2c *bus, uint16_t reg, uint8_t *value)
{
	uint8_t address[2] = { reg >> 8, reg }, data = 0;
	if (value == NULL || !redwood_bbi2c_xfer(bus, 0x10, address, sizeof(address), &data, sizeof(data))) return false;
	*value = data;
	return true;
}

static bool
cam_3j1_flush_burst(struct redwood_bbi2c *bus, uint8_t *bytes, uint32_t words)
{
	return words == 0 || redwood_bbi2c_xfer(bus, 0x10, bytes, 2 + 2 * words, NULL, 0);
}

/* Execute Samsung's three-word setfile records. Preserve its 200-word global burst. */
static bool
cam_3j1_apply(struct redwood_bbi2c *bus, const uint32_t *table, uint32_t count)
{
	uint8_t burst[402]; /* Samsung's global microcode is one 200-word I2C transfer. */
	uint32_t i, nburst = 0;
	bool in_burst = false;
	/* Samsung sensor_cis_set_registers() waits 3 ms before every setfile. */
	cam_s20_wait_ms(3);

	for (i = 0; i + 2 < count; i += 3) {
		uint32_t reg = table[i], value = table[i + 1], width = table[i + 2];
		if (reg == CAM_3J1_BURST_ADDR) {
			if (in_burst && !cam_3j1_flush_burst(bus, burst, nburst)) return false;
			burst[0] = value >> 8;
			burst[1] = value;
			nburst = 0;
			in_burst = true;
		} else if (reg == CAM_3J1_BURST_DATA && in_burst) {
			if (nburst == 200) {
				if (!cam_3j1_flush_burst(bus, burst, nburst)) return false;
				nburst = 0;
			}
			burst[2 + 2 * nburst] = value >> 8;
			burst[3 + 2 * nburst] = value;
			nburst++;
		} else {
			if (in_burst) {
				if (!cam_3j1_flush_burst(bus, burst, nburst)) return false;
				in_burst = false;
				nburst = 0;
			}
			if (reg == CAM_3J1_DELAY) {
				delay_for_interval(value, NSEC_PER_USEC);
			} else if (width == 2) {
				if (reg > 0xffff || value > 0xffff || !cam_3j1_write16(bus, (uint16_t)reg, (uint16_t)value)) return false;
			} else {
				return false;
			}
		}
	}
	return !in_burst || cam_3j1_flush_burst(bus, burst, nburst);
}

/* Read stable mode registers three times before stream-on. An ACK alone
 * does not establish that a software-I2C write or read was bit-exact. */
struct cam_3j1_verify {
    unsigned reads, mismatches, io_errors;
    uint16_t first_reg, expected, observed;
};

/* Verify every word of the global microcode, using the sensor's distinct
 * indirect READ page/address registers. Re-select each word explicitly. */
static bool
cam_3j1_verify_code(struct redwood_bbi2c *bus, struct cam_3j1_verify *check)
{
    uint16_t page = 0, address = 0;
    for (unsigned t = 0; t + 2 < REDWOOD_3J1_GLOBAL_WORDS; t += 3) {
        uint32_t reg = redwood_3j1_global[t];
        uint16_t expected = (uint16_t)redwood_3j1_global[t + 1], value = 0;
        if (reg == 0x6028) page = expected;
        else if (reg == 0x602a) address = expected;
        else if (reg == CAM_3J1_BURST_DATA) {
            bool ok = cam_3j1_write16(bus, 0x602c, page) &&
                cam_3j1_write16(bus, 0x602e, address) &&
                cam_3j1_read16(bus, 0x6f12, &value);
            check->reads++;
            if (!ok || value != expected) {
                if (!check->mismatches && !check->io_errors) {
                    check->first_reg = address; check->expected = expected;
                    check->observed = value;
                }
                if (!ok) check->io_errors++;
                else check->mismatches++;
            }
            address += 2;
        }
    }
    return check->reads == 200 && !check->mismatches && !check->io_errors;
}

/* Check final indirect configuration values, excluding the separately checked
 * microcode burst. Duplicate addresses take their last setfile value. Only the
 * independent read pointer is changed; the write page remains untouched. */
static bool
cam_3j1_verify_indirect(struct redwood_bbi2c *bus, const uint32_t *table,
    uint32_t count, struct cam_3j1_verify *check)
{
    struct { uint16_t page, address, value; } final[256];
    unsigned used = 0;
    uint16_t page = 0x2000, address = 0;
    for (unsigned t = 0; t + 2 < count; t += 3) {
        uint32_t reg = table[t];
        uint16_t value = (uint16_t)table[t + 1];
        if (reg == 0x6028) page = value;
        else if (reg == 0x602a) address = value;
        else if (reg == CAM_3J1_BURST_DATA) address += 2;
        else if (reg == 0x6f12) {
            unsigned r;
            for (r = 0; r < used; r++)
                if (final[r].page == page && final[r].address == address) break;
            if (r == used) {
                if (used == sizeof(final) / sizeof(final[0])) {
                    check->io_errors++;
                    return false;
                }
                used++;
            }
            final[r].page = page;
            final[r].address = address;
            final[r].value = value;
            address += 2;
        }
    }
    for (unsigned r = 0; r < used; r++) {
        uint16_t value = 0;
        bool ok = cam_3j1_write16(bus, 0x602c, final[r].page) &&
            cam_3j1_write16(bus, 0x602e, final[r].address) &&
            cam_3j1_read16(bus, 0x6f12, &value);
        check->reads++;
        if (!ok || value != final[r].value) {
            if (!check->mismatches && !check->io_errors) {
                check->first_reg = final[r].address;
                check->expected = final[r].value;
                check->observed = value;
            }
            if (!ok) check->io_errors++;
            else check->mismatches++;
        }
    }
    return used && !check->mismatches && !check->io_errors;
}

static bool
cam_3j1_verify_mode(struct redwood_bbi2c *bus, const uint32_t *table,
    uint32_t count, struct cam_3j1_verify *check)
{
    static const uint16_t regs[] = { 0x0112, 0x0114, 0x0116, 0x0136,
        0x0300, 0x0304, 0x0306, 0x030c, 0x030e, 0x0310, 0x0312,
        0x0340, 0x0342, 0x0344, 0x0346, 0x0348, 0x034a, 0x034c,
        0x034e, 0x0380, 0x0382, 0x0384, 0x0386, 0x0404, 0x0900 };
    for (unsigned r = 0; r < sizeof(regs) / sizeof(regs[0]); r++) {
        uint16_t expected = 0;
        bool found = false;
        for (unsigned t = 0; t + 2 < count; t += 3) {
            if (table[t] == regs[r] && table[t + 2] == 2) {
                expected = (uint16_t)table[t + 1];
                found = true;
            }
        }
        if (!found) continue;
        for (unsigned repeat = 0; repeat < 3; repeat++) {
            uint16_t value = 0;
            bool ok = cam_3j1_read16(bus, regs[r], &value);
            check->reads++;
            if (!ok || value != expected) {
                if (!check->mismatches && !check->io_errors) {
                    check->first_reg = regs[r]; check->expected = expected;
                    check->observed = value;
                }
                if (!ok) check->io_errors++;
                else check->mismatches++;
            }
        }
    }
    return check->reads && !check->mismatches && !check->io_errors;
}

/* Apply the vendor global table and a binned 30 fps mode, verify output dimensions, then power back down. */
static void
cam_s20_front_set_mode(__unused const uint64_t *v, __unused unsigned n, char *out, size_t cap)
{
	struct redwood_bbi2c bus = { .name = "SAK3J1 HSI2C1 gpp0-2/3", .scl = 2, .sda = 3, .half_us = 4 };
	volatile uint8_t *reset = redwood_gpio_bank(PINCTRL_CMGP_PHYS, PINCTRL_CMGP_GPM11);
	volatile uint8_t *mclk = redwood_gpio_bank(PINCTRL_PERIC1_PHYS, PINCTRL_PERIC1_GPC0);
	volatile uint8_t *cmu = (volatile uint8_t *)cam_s20_io_map(CMU_TOP_PHYS, 0x4000);
	uint8_t p[5] = { 0 };
	uint32_t rcon = cam_s20_rd(reset, REDWOOD_GPIO_CON), rdat = cam_s20_rd(reset, REDWOOD_GPIO_DAT);
	uint32_t rpud = cam_s20_rd(reset, REDWOOD_GPIO_PUD), mcon = cam_s20_rd(mclk, REDWOOD_GPIO_CON);
	uint32_t mdat = cam_s20_rd(mclk, REDWOOD_GPIO_DAT), mpud = cam_s20_rd(mclk, REDWOOD_GPIO_PUD);
	uint32_t clock[4] = { 0 }, offsets[4] = { CMU_CIS_CLK1_MUX, CMU_CIS_CLK1_DIV, CMU_CIS_CLK1_GATE, CMU_CIS_CLK1_QCH };
	uint8_t stream = 0;
	uint16_t id = 0, width = 0, height = 0, frame_length = 0;
	char phase[48] = "snapshot";
	bool rails = false, bus_ready = false, sensor_present = false, clock_ready = false, configured = false;
	unsigned i;
	if (cap == 0) return;
	if (cmu == NULL) { (void)snprintf(out, cap, "{\"camera\":\"SAK3J1\",\"result\":\"error\",\"phase\":\"CMU map\"}\n"); return; }
	if (!redwood_s2mpb02_camera_read(CAM_S2MPB02_BUCK2, &p[0]) ||
	    !redwood_s2mpb02_camera_read(CAM_S2MPB02_LDO16, &p[1]) ||
	    !redwood_s2mpb02_camera_read(CAM_S2MPB02_LDO11, &p[2]) ||
	    !redwood_s2mpb02_camera_read(CAM_S2MPB02_LDO6, &p[3]) ||
	    !redwood_s2mpb02_camera_read(CAM_S2MPB02_LDO5, &p[4])) goto cleanup;
	rails = true;
	(void)snprintf(phase, sizeof(phase), "BUCK2 and VT rails");
	(void)redwood_gpio_set(reset, 0, 0);
	redwood_gpio_pull(reset, 0, REDWOOD_GPIO_PULL_NONE);
	(void)redwood_gpio_set(reset, 0, 0);
	redwood_gpio_func(reset, 0, REDWOOD_GPIO_OUTPUT);
	if (!redwood_s2mpb02_camera_bits(CAM_S2MPB02_BUCK2, 0xc0, 0xc0) ||
	    !redwood_s2mpb02_camera_bits(CAM_S2MPB02_LDO16, 0x80, 0x80) ||
	    !redwood_s2mpb02_camera_bits(CAM_S2MPB02_LDO11, 0x80, 0x80) ||
	    !redwood_s2mpb02_camera_bits(CAM_S2MPB02_LDO6, 0x80, 0x80) ||
	    !redwood_s2mpb02_camera_bits(CAM_S2MPB02_LDO5, 0x80, 0x80)) goto cleanup;
	cam_s20_wait_ms(1);
	(void)snprintf(phase, sizeof(phase), "I2C bus setup");
	bus_ready = true;
	bus.bank = redwood_gpio_bank(PINCTRL_PERIC0_PHYS, 0);
	if (!redwood_bbi2c_setup(&bus, true, out, cap)) goto cleanup;
	for (i = 0; i < 4; i++) clock[i] = cam_s20_rd(cmu, offsets[i]);
	cam_s20_wr(cmu, CMU_CIS_CLK1_MUX, clock[0] & ~1U);
	cam_s20_wr(cmu, CMU_CIS_CLK1_DIV, clock[1] & ~0x1fU);
	cam_s20_wr(cmu, CMU_CIS_CLK1_QCH, clock[3] | 3U);
	cam_s20_wr(cmu, CMU_CIS_CLK1_GATE, clock[2] | (1U << 20) | (1U << 21));
	clock_ready = true;
	(void)redwood_gpio_set(mclk, 1, 0);
	redwood_gpio_func(mclk, 1, 2);
	redwood_gpio_pull(mclk, 1, REDWOOD_GPIO_PULL_NONE);
	cam_s20_wait_ms(10);
	(void)redwood_gpio_set(reset, 0, 1);
	cam_s20_wait_ms(9);
	(void)snprintf(phase, sizeof(phase), "SAK3J1 ID check");
	if (!cam_3j1_read16(&bus, 0x0000, &id) || id != 0x30a1) goto cleanup;
	sensor_present = true;
	(void)snprintf(phase, sizeof(phase), "Samsung global setfile");
	if (!cam_3j1_apply(&bus, redwood_3j1_global, REDWOOD_3J1_GLOBAL_WORDS)) goto cleanup;
	(void)snprintf(phase, sizeof(phase), "1824x1368 mode setfile");
	if (!cam_3j1_apply(&bus, redwood_3j1_1824x1368_30fps, REDWOOD_3J1_1824X1368_30FPS_WORDS)) goto cleanup;
	if (!cam_3j1_read16(&bus, 0x034c, &width) || !cam_3j1_read16(&bus, 0x034e, &height) ||
	    !cam_3j1_read16(&bus, 0x0340, &frame_length) || !cam_3j1_read8(&bus, 0x0100, &stream)) {
		(void)snprintf(phase, sizeof(phase), "mode readback");
		goto cleanup;
	}
	configured = width == 1824 && height == 1368 && stream == 0;
	if (!configured) (void)snprintf(phase, sizeof(phase), "mode verification");
cleanup:
	if (sensor_present) (void)cam_3j1_write16(&bus, 0x0100, 0);
	(void)redwood_gpio_set(reset, 0, 0);
	if (clock_ready) {
		(void)redwood_gpio_set(mclk, 1, 0);
		redwood_gpio_func(mclk, 1, REDWOOD_GPIO_OUTPUT);
		cam_s20_wr(cmu, CMU_CIS_CLK1_GATE, clock[2]);
		cam_s20_wr(cmu, CMU_CIS_CLK1_QCH, clock[3]);
		cam_s20_wr(cmu, CMU_CIS_CLK1_DIV, clock[1]);
		cam_s20_wr(cmu, CMU_CIS_CLK1_MUX, clock[0]);
	}
	if (bus_ready) redwood_bbi2c_restore(&bus);
	if (rails) {
		(void)redwood_s2mpb02_camera_bits(CAM_S2MPB02_LDO5, 0x80, p[4] & 0x80);
		(void)redwood_s2mpb02_camera_bits(CAM_S2MPB02_LDO6, 0x80, p[3] & 0x80);
		(void)redwood_s2mpb02_camera_bits(CAM_S2MPB02_LDO11, 0x80, p[2] & 0x80);
		(void)redwood_s2mpb02_camera_bits(CAM_S2MPB02_LDO16, 0x80, p[1] & 0x80);
		(void)redwood_s2mpb02_camera_bits(CAM_S2MPB02_BUCK2, 0xc0, p[0] & 0xc0);
	}
	redwood_gpio_restore(reset, 0, rcon & 0xfU, rpud & 0xfU, rdat & 1U);
	redwood_gpio_restore(mclk, 1, mcon & (0xfU << 4), mpud & (0xfU << 4), mdat & 2U);
	if (configured) {
		(void)snprintf(out, cap, "{\"camera\":\"SAK3J1\",\"stage\":\"mode-setup\",\"result\":\"ok\",\"width\":%u,\"height\":%u,\"frame_length\":%u,\"stream_on\":false,\"state_restored\":true}\n",
		    width, height, frame_length);
	} else {
		(void)snprintf(out, cap, "{\"camera\":\"SAK3J1\",\"stage\":\"mode-setup\",\"result\":\"error\",\"phase\":\"%s\",\"id\":\"0x%04x\",\"width\":%u,\"height\":%u,\"stream\":%u,\"state_restored\":true}\n",
		    phase, id, width, height, (unsigned)stream);
	}
}

/* One bounded optical-path proof: SAK3J1 -> Exynos990 DPHY -> CSIS1 frame counter.
 * No DMA or userspace buffers are enabled here. Every sensor/receiver setting is restored. */
static void
cam_s20_front_stream_probe(const uint64_t *v, unsigned n, char *out, size_t cap)
{
	static const uint32_t phy_sc_offs[] = { 0x0000, 0x0004, 0x0008, 0x0010, 0x0014, 0x0018, 0x0030 };
	static const uint32_t phy_lane_base[] = { 0x0100, 0x0200, 0x0300, 0x0400 };
	static const uint32_t phy_lane_offs[] = { 0x0000, 0x0004, 0x0010, 0x0014, 0x0018, 0x0030, 0x0034, 0x0040, 0x0050 };
	static const uint32_t phy_bias_offs[] = { 0x0000, 0x0004, 0x0008 };
	static const uint32_t csi_offs[] = { 0x0004, 0x0008, 0x000c, 0x0040, 0x0044, 0x0050, 0x0054, 0x0600, 0x0690, 0x0704 };
	struct redwood_bbi2c bus = { .name = "SAK3J1 HSI2C1 gpp0-2/3", .scl = 2, .sda = 3, .half_us = 4 };
	volatile uint8_t *pmu = (volatile uint8_t *)cam_s20_io_map(PMU_PHYS, 0x4000);
	volatile uint8_t *cmu_top = (volatile uint8_t *)cam_s20_io_map(CMU_TOP_PHYS, 0x4000);
	volatile uint8_t *cmu = (volatile uint8_t *)cam_s20_io_map(CMU_CSIS_PHYS, 0x8000);
	volatile uint8_t *csi = (volatile uint8_t *)cam_s20_io_map(CSIS1_PHYS, 0x1000);
	volatile uint8_t *dphy = (volatile uint8_t *)cam_s20_io_map(CSIS1_DPHY_PHYS, 0x500);
	volatile uint8_t *bias = (volatile uint8_t *)cam_s20_io_map(CSIS1_DPHY_BIAS_PHYS, 0x100);
	volatile uint8_t *isppre = (volatile uint8_t *)cam_s20_io_map(ISPPRE_SYSREG_PHYS, 0x4);
	volatile uint8_t *reset = redwood_gpio_bank(PINCTRL_CMGP_PHYS, PINCTRL_CMGP_GPM11);
	volatile uint8_t *mclk = redwood_gpio_bank(PINCTRL_PERIC1_PHYS, PINCTRL_PERIC1_GPC0);
	uint8_t voltage_saved[4] = { 0 };
	bool voltages_set = false, voltages_restored = true;
	uint8_t rails[5] = { 0 }, stream = 0, sensor_fc_before = 0, sensor_fc_after = 0;
	uint32_t phy_sc[7] = { 0 }, phy_lane[4][9] = { { 0 } }, phy_bias[3] = { 0 }, csi_saved[10] = { 0 };
	uint32_t clock[4] = { 0 }, clock_offs[4] = { CMU_CIS_CLK1_MUX, CMU_CIS_CLK1_DIV, CMU_CIS_CLK1_GATE, CMU_CIS_CLK1_QCH };
	uint32_t rcon, rdat, rpud, mcon, mdat, mpud;
	uint32_t reset_sys = 0;
	uint32_t mask0_saved = 0, mask1_saved = 0;
	uint32_t cmu_qch = 0, csi_qch = 0, dma_qch = 0, top_qch = 0, iso = 0;
	uint32_t before = 0, after = 0, irq = 0, fs_irq = 0, fe_irq = 0, phy_status = 0;
	uint32_t phy_status_or = 0, phy_status_and = ~0U, phy_active_samples = 0, irq_seen = 0;
	uint32_t cmn_live = 0, phy_live = 0, config_live = 0, config1_live = 0, resol_live = 0;
	uint32_t shadow1_live = 0, lrte_live = 0, dphy_clk = 0, lane0_live = 0, lane3_live = 0, i, j;
	uint32_t phy_sc_time1 = 0, phy_sd0_ana6 = 0, phy_sd0_ana7 = 0;
	uint32_t phy_sc_ana2 = 0, phy_sd0_ana2 = 0, phy_sd3_ana2 = 0;
	uint32_t phy_sd0_settle = 0, phy_sd0_deskew = 0, phy_bias0 = 0, phy_bias1 = 0, phy_bias2 = 0;
	uint32_t cmu_qch_live = 0, csi_qch_live = 0, dma_qch_live = 0, top_qch_live = 0;
	uint32_t mclk_mux_live = 0, mclk_div_live = 0, mclk_gate_live = 0, mclk_qch_live = 0;
	uint32_t csis_bus_mux_live = 0, pmu_iso_live = 0, phy_reset_live = 0, irq_mask_live = 0;
	uint8_t rails_live[5] = { 0 }, pattern_live = 0, frame_shift = 0, exposure_shift = 0;
	uint16_t sid = 0, sensor_fmt = 0, sensor_lanes = 0, sensor_mipi_pll = 0;
	uint16_t sensor_frame_length = 0, sensor_line_length = 0, sensor_crop_x = 0;
	uint16_t sensor_crop_end_x = 0, sensor_scaler = 0, sensor_binning = 0, sensor_pll2 = 0;
	struct cam_3j1_verify mode_check = { 0 }, code_check = { 0 }, indirect_check = { 0 };
	uint16_t sensor_code[5] = { 0 };
	static const uint16_t sensor_code_addrs[5] = { 0x918c, 0x918e, 0x9190, 0x9192, 0x9256 };
	static const uint16_t diag_regs[] = { 0x0850, 0x0b02, 0x0b04, 0x0b06, 0x0116, 0x0118,
	    0x0812, 0x0814, 0x080e, 0x0810, 0x0816, 0x6214, 0x6244, 0x6b5e, 0x6b60,
	    0x0a7a, 0x0a7c, 0x0a76, 0x0202, 0x0204, 0x020e, 0x0104 };
	uint16_t diag_vals[sizeof(diag_regs) / sizeof(diag_regs[0])] = { 0 };
	char phase[40] = "map";
	/* Second argument: 1 = 100 us, 2 = 1 ms per PHY write; zero preserves baseline. */
	unsigned phy_pace_us = n > 1 ? (v[1] == 1 ? 100U : v[1] == 2 ? 1000U : 0U) : 0U;
	/* Third argument bits: 1 defers sensor reads, 2 primes then restarts stream. */
	bool defer_stream_reads = n > 2 && (v[2] & 1U);
	bool warm_restart = n > 2 && (v[2] & 2U);
	/* Third argument bit 4: line probe. Before the receiver is restored, sample
	 * PHY_STATUS with the sensor in standby (LP-11 expected: stop states 0xf1),
	 * held in reset, and with its rails off (lines pulled low: a working D-PHY
	 * must leave 0xf1). Splits "sensor never transmits" from "PHY sees nothing". */
	bool line_probe = n > 2 && (v[2] & 4U);
	uint32_t line_or[3] = { 0 }, line_and[3] = { 0xffffffffU, 0xffffffffU, 0xffffffffU };
	bool line_done = false;
	/* Third argument bit 8: read the shared 273-register list while the sensor streams. */
	bool regdump = n > 2 && (v[2] & 8U);
	bool regdump_done = false;
	/* Third argument bit 16: set the last registers that differed from Samsung's active stream (2026-11-11 273-register
	 * compare): CSIS1 VC2/VC3 config, CSIS1->DMA mux 0x17020418=1, OIS MCU QCH manual request, unused MCLK pins
	 * GPC0-0/3 parked output-low, reset pin GPM11 pull-down. All saved and restored. Also adds a PHY-reset stage to
	 * the line probe (does CSIS PHY_STATUS react to the PHY being held in reset?). */
	bool match_rest = n > 2 && (v[2] & 16U), rest_changed = false;
	volatile uint8_t *isp_page = NULL;
	uint32_t mux_saved = 0, ois_saved[3] = { 0 }, gpc0_con_saved = 0, gpc0_dat_saved = 0, gpm11_pud_saved = 0;
	uint32_t vc23_saved[4] = { 0 };
	static const uint32_t ois_offs[3] = { 0x30c0, 0x30c4, 0x30cc };
	static const uint32_t vc23_offs[4] = { 0x0060, 0x0064, 0x0070, 0x0074 };
	uint32_t line_rst[2] = { 0, 0xffffffffU };
	/* Third argument bit 32: fresh CSIS power-domain cycle first, exactly Samsung's pmucal csis_off/csis_on
	 * (flexpmu_cal_local_exynos9830.h + pmucal_local.c): save CMU 1804/1800/610/600, DTZPC save SMC, OIS QCH=4,
	 * CONTROLLER_OPTION bit24=0, CONFIGURATION=0 / wait STATUS=0, then CONFIGURATION=1 / wait STATUS=1, DTZPC restore
	 * SMC (the existing call below), restore the four CMU values. Samsung powers this domain up fresh at camera
	 * open; XNU inherited it powered since boot. */
	bool pd_cycle = n > 2 && (v[2] & 32U);
	/* Third argument bit 64: the S2MPB02 supplies Samsung keeps always on (BUCK1 0x0b, buck-boost 0x0f, LDO18 0x2f),
	 * enabled before the sensor rails and restored afterwards. XNU idle: all three off (map 0b=18 0f=18 2f=6c). */
	bool pmic_base = n > 2 && (v[2] & 64U), pmic_base_on = false;
	uint8_t pmic_base_saved[3] = { 0 }, pmic_base_live[3] = { 0 };
	static const uint8_t pmic_base_regs[3] = { 0x0b, 0x0f, 0x2f };
	int pd_off_ok = -1, pd_on_ok = -1;
	uint64_t pd_save_smc = 0;
	uint32_t pd_cmu_saved[4] = { 0 };
	static const uint32_t pd_cmu_offs[4] = { 0x1804, 0x1800, 0x0610, 0x0600 };
	uint32_t prime_frames = 0;

	bool rails_saved = false, pins_saved = false, bus_ready = false, clock_ready = false, sensor_on = false;
	bool phy_saved = false, csi_saved_ok = false, irq_masks_saved = false, qch_changed = false, dma_qch_changed = false, top_qch_changed = false;
	bool iso_changed = false, result = false;
	bool pd_mode = n > 0 && (v[0] >= 1 && v[0] <= 14);
	bool stock_gain = n > 0 && (v[0] == 2 || v[0] == 4 || v[0] == 5 || (v[0] >= 6 && v[0] <= 14));
	bool test_pattern = n > 0 && v[0] == 3;
	bool receiver_first = n > 0 && (v[0] == 13 || v[0] == 14);
	bool prestream_verify = !(n > 0 && v[0] == 14);
	bool trace_timing = n > 0 && v[0] == 5;
	bool normal_timing = n > 0 && v[0] >= 6 && v[0] <= 14;
	bool reference_qch = n > 0 && ((v[0] >= 7 && v[0] <= 9) || v[0] == 11 || (v[0] >= 12 && v[0] <= 14));
	bool reference_mclk = n > 0 && (v[0] == 8 || v[0] == 9 || v[0] == 11 || (v[0] >= 12 && v[0] <= 14));
	bool reference_bus = n > 0 && (v[0] >= 9 && v[0] <= 14);
	bool bus_clock_changed = false, bus_clock_restored = true;
	uint32_t bus_mux_saved = 0, bus_div_saved = 0;
	uint32_t bus_mux_live = 0, bus_div_live = 0;
	uint32_t pll4_before = 0, pll4_active = 0, pll0_active = 0;
	uint32_t mclk_pin_drive = 0, reset_pin_drive = 0, mclk_drive_saved = 0;
	bool reference_drive = n > 0 && (v[0] >= 12 && v[0] <= 14), drive_changed = false, drive_restored = true;
	uint32_t phy_analog0[4] = { 0 }, phy_analog1[4] = { 0 };
	/* Documented CMU_CSIS oscillator/reset-sync/APB clocks and shared bias. */
	static const uint32_t platform_offs[] = { 0x2004, 0x20d4, 0x20d8, 0x20f4, 0x2054, 0x2070, 0x30a0, 0x310c };
	uint32_t platform_clocks[9] = { 0 };
	uint32_t qch_mask = reference_qch ? 7U : 1U, qch_value = reference_qch ? 2U : 1U;
	uint64_t smc = 0;
	if (cap == 0) return;
	if (n > 0 && v[0] == 4) bus.half_us = 2; /* 250 kHz software I2C, closer to Samsung HSI2C. */
	if (pmu == NULL || cmu_top == NULL || cmu == NULL || csi == NULL || dphy == NULL || bias == NULL || isppre == NULL) goto done;
	if (!(cam_s20_rd(pmu, 0x1d84) & 1U)) { strlcpy(phase, "CSIS domain off", sizeof(phase)); goto done; }
	if (pd_cycle) {
		for (i = 0; i < 4; i++) pd_cmu_saved[i] = cam_s20_rd(cmu, pd_cmu_offs[i]);
		pd_save_smc = cam_s20_smc4(SMC_PREPARE_PD_ONOFF, 0, CSIS_NEED_SMC, 2);
		if (pd_save_smc != 0) { strlcpy(phase, "CSIS DTZPC save", sizeof(phase)); goto done; }
		cam_s20_wr(cmu, 0x30c0, 4); cam_s20_wr(cmu, 0x30c4, 4); cam_s20_wr(cmu, 0x30cc, 4);
		cam_s20_update_bits(cmu, 0x0800, 1U << 24, 0);
		cam_s20_update_bits(pmu, 0x1d80, 1U, 0);
		for (i = 0; i < 1000 && (cam_s20_rd(pmu, 0x1d84) & 1U); i++) delay_for_interval(10, NSEC_PER_USEC);
		pd_off_ok = !(cam_s20_rd(pmu, 0x1d84) & 1U);
		delay_for_interval(1000, NSEC_PER_USEC);
		cam_s20_update_bits(pmu, 0x1d80, 1U, 1U);
		for (i = 0; i < 1000 && !(cam_s20_rd(pmu, 0x1d84) & 1U); i++) delay_for_interval(10, NSEC_PER_USEC);
		pd_on_ok = (cam_s20_rd(pmu, 0x1d84) & 1U) != 0;
		if (!pd_on_ok) { strlcpy(phase, "CSIS domain did not power on", sizeof(phase)); goto done; }
	}
	smc = cam_s20_smc4(SMC_PREPARE_PD_ONOFF, 1, CSIS_NEED_SMC, 2);
	if (smc != 0) { strlcpy(phase, "CSIS secure access", sizeof(phase)); goto done; }
	if (pd_cycle) for (i = 0; i < 4; i++) cam_s20_wr(cmu, pd_cmu_offs[i], pd_cmu_saved[i]);
	cmu_qch = cam_s20_rd(cmu, 0x3058); csi_qch = cam_s20_rd(cmu, 0x3064);
	dma_qch = cam_s20_rd(cmu, 0x3078); top_qch = cam_s20_rd(cmu, 0x307c); iso = cam_s20_rd(pmu, 0x0730);
	/* Samsung active SFR capture: all four QCH registers are 0x3f0002.
	 * Bit 0 enables automatic handshaking; it is not a clock-on bit.
	 * Modes 7/8/9/11/12 retain manual CLOCK_REQ=1, then restore all changed bits. */
	if ((cmu_qch & qch_mask) != qch_value) { cam_s20_update_bits(cmu, 0x3058, qch_mask, qch_value); qch_changed = true; }
	/* Samsung exynos9830_is_sensor_iclk_cfg enables the shared WDMA and PDP-top clocks too. */
	if ((dma_qch & qch_mask) != qch_value) { cam_s20_update_bits(cmu, 0x3078, qch_mask, qch_value); dma_qch_changed = true; }
	if ((top_qch & qch_mask) != qch_value) { cam_s20_update_bits(cmu, 0x307c, qch_mask, qch_value); top_qch_changed = true; }
	if ((csi_qch & qch_mask) != qch_value) { cam_s20_update_bits(cmu, 0x3064, qch_mask, qch_value); qch_changed = true; }
	if (!(iso & 1U)) { cam_s20_update_bits(pmu, 0x0730, 1U, 1U); iso_changed = true; }
	if (match_rest) {
		isp_page = (volatile uint8_t *)cam_s20_io_map(0x17020000, 0x1000);
		if (isp_page == NULL) { strlcpy(phase, "isppre page map", sizeof(phase)); goto cleanup; }
		mux_saved = cam_s20_rd(isp_page, 0x418);
		for (i = 0; i < 3; i++) ois_saved[i] = cam_s20_rd(cmu, ois_offs[i]);
		gpc0_con_saved = cam_s20_rd(mclk, REDWOOD_GPIO_CON); gpc0_dat_saved = cam_s20_rd(mclk, REDWOOD_GPIO_DAT);
		gpm11_pud_saved = cam_s20_rd(reset, REDWOOD_GPIO_PUD);
		for (i = 0; i < 4; i++) vc23_saved[i] = cam_s20_rd(csi, vc23_offs[i]);
		rest_changed = true;
		cam_s20_wr(isp_page, 0x418, 1);
		for (i = 0; i < 3; i++) cam_s20_wr(cmu, ois_offs[i], (ois_saved[i] & ~7U) | 6U);
		cam_s20_wr(mclk, REDWOOD_GPIO_DAT, gpc0_dat_saved & ~((1U << 0) | (1U << 3)));
		cam_s20_wr(mclk, REDWOOD_GPIO_CON, (gpc0_con_saved & ~(0xfU | (0xfU << 12))) | 1U | (1U << 12));
		cam_s20_wr(reset, REDWOOD_GPIO_PUD, (gpm11_pud_saved & ~0xfU) | 1U);
	}
	/* The active Samsung capture selects PLL_SHARED4_DIV2 without a further
	 * divider (TOP 104c=1, 1848=0); XNU inherited source 2 / divide-by-6.
	 * Change only the dedicated CSIS bus branch, never a shared PLL. Leave the
	 * slower divider in place while switching parent, and bound BUSY waits. */
	pll4_before = cam_s20_rd(cmu_top, 0x028c);
	if (reference_bus) {
		bus_mux_saved = cam_s20_rd(cmu_top, 0x104c);
		bus_div_saved = cam_s20_rd(cmu_top, 0x1848);
		bus_clock_changed = true;
		cam_s20_update_bits(cmu_top, 0x104c, 3U, v[0] == 11 ? 2U : 1U);
		for (i = 0; i < 1000 && (cam_s20_rd(cmu_top, 0x104c) & (1U << 16)); i++) delay_for_interval(1, NSEC_PER_USEC);
		if (cam_s20_rd(cmu_top, 0x104c) & (1U << 16)) { strlcpy(phase, "CSIS parent timeout", sizeof(phase)); goto cleanup; }
		cam_s20_update_bits(cmu_top, 0x1848, 15U, 0U);
		for (i = 0; i < 1000 && (cam_s20_rd(cmu_top, 0x1848) & (1U << 16)); i++) delay_for_interval(1, NSEC_PER_USEC);
		if (cam_s20_rd(cmu_top, 0x1848) & (1U << 16)) { strlcpy(phase, "CSIS divider timeout", sizeof(phase)); goto cleanup; }
	}
	bus_mux_live = cam_s20_rd(cmu_top, 0x104c);
	bus_div_live = cam_s20_rd(cmu_top, 0x1848);
	if (!redwood_s2mpb02_camera_read(CAM_S2MPB02_BUCK2, &rails[0]) ||
	    !redwood_s2mpb02_camera_read(CAM_S2MPB02_LDO16, &rails[1]) ||
	    !redwood_s2mpb02_camera_read(CAM_S2MPB02_LDO11, &rails[2]) ||
	    !redwood_s2mpb02_camera_read(CAM_S2MPB02_LDO6, &rails[3]) ||
	    !redwood_s2mpb02_camera_read(CAM_S2MPB02_LDO5, &rails[4])) { strlcpy(phase, "sensor rail snapshot", sizeof(phase)); goto cleanup; }
	rails_saved = true;
	rcon = cam_s20_rd(reset, REDWOOD_GPIO_CON); rdat = cam_s20_rd(reset, REDWOOD_GPIO_DAT); rpud = cam_s20_rd(reset, REDWOOD_GPIO_PUD);
	mcon = cam_s20_rd(mclk, REDWOOD_GPIO_CON); mdat = cam_s20_rd(mclk, REDWOOD_GPIO_DAT); mpud = cam_s20_rd(mclk, REDWOOD_GPIO_PUD);
	/* x1slte SAK3J1 DTS: BUCK2, LDO16/11/6/5; no GPC0-5 route-select rail/step. */
	pins_saved = true;
	(void)redwood_gpio_set(reset, 0, 0); redwood_gpio_pull(reset, 0, REDWOOD_GPIO_PULL_NONE); redwood_gpio_func(reset, 0, REDWOOD_GPIO_OUTPUT);
	if (pmic_base) {
		strlcpy(phase, "S2MPB02 base supplies", sizeof(phase));
		for (i = 0; i < 3; i++) if (!redwood_s2mpb02_camera_read(pmic_base_regs[i], &pmic_base_saved[i])) goto cleanup;
		pmic_base_on = true;
		if (!redwood_s2mpb02_camera_bits(0x0b, 0xc0, 0xc0)) goto cleanup;
		cam_s20_wait_ms(2);
		if (!redwood_s2mpb02_camera_bits(0x0f, 0xc0, 0xc0)) goto cleanup;
		cam_s20_wait_ms(2);
		if (!redwood_s2mpb02_camera_bits(0x2f, 0x80, 0x80)) goto cleanup;
		cam_s20_wait_ms(5);
	}
	strlcpy(phase, "sensor voltages", sizeof(phase));
	if (!redwood_s2mpb02_front_voltages(voltage_saved, false)) goto cleanup;
	voltages_set = true;
	strlcpy(phase, "sensor rails", sizeof(phase));
	if (!redwood_s2mpb02_camera_bits(CAM_S2MPB02_BUCK2, 0xc0, 0xc0) ||
	    !redwood_s2mpb02_camera_bits(CAM_S2MPB02_LDO16, 0x80, 0x80) ||
	    !redwood_s2mpb02_camera_bits(CAM_S2MPB02_LDO11, 0x80, 0x80) ||
	    !redwood_s2mpb02_camera_bits(CAM_S2MPB02_LDO6, 0x80, 0x80) ||
	    !redwood_s2mpb02_camera_bits(CAM_S2MPB02_LDO5, 0x80, 0x80)) goto cleanup;
	bus.bank = redwood_gpio_bank(PINCTRL_PERIC0_PHYS, 0);
	if (!redwood_bbi2c_setup(&bus, true, out, cap)) { strlcpy(phase, "sensor I2C setup", sizeof(phase)); goto cleanup; }
	bus_ready = true;
	/* Samsung order: on_i2c (10 ms), reset high, MCLK pin/clock on (9 ms). */
	cam_s20_wait_ms(10);
	(void)redwood_gpio_set(reset, 0, 1);
	for (i = 0; i < 4; i++) clock[i] = cam_s20_rd(cmu_top, clock_offs[i]);
	cam_s20_wr(cmu_top, CMU_CIS_CLK1_MUX, clock[0] & ~1U); cam_s20_wr(cmu_top, CMU_CIS_CLK1_DIV, clock[1] & ~0x1fU);
	cam_s20_wr(cmu_top, CMU_CIS_CLK1_QCH, reference_mclk ? ((clock[3] & ~7U) | 6U) : (clock[3] | 3U));
	cam_s20_wr(cmu_top, CMU_CIS_CLK1_GATE, reference_mclk ? ((clock[2] & ~(1U << 20)) | (1U << 21)) : (clock[2] | (1U << 20) | (1U << 21)));
	clock_ready = true;
	/* Samsung active GPC0 DRV=0x2202: only CIS_CLK1's nibble differs. */
	if (reference_drive) {
		mclk_drive_saved = cam_s20_rd(mclk, PINCTRL_DRV);
		cam_s20_update_bits(mclk, PINCTRL_DRV, 0xfU << 4, 0);
		drive_changed = true;
	}
	(void)redwood_gpio_set(mclk, 1, 0); redwood_gpio_func(mclk, 1, 2); redwood_gpio_pull(mclk, 1, REDWOOD_GPIO_PULL_NONE);
	cam_s20_wait_ms(9);
	/* Modes 13/14 isolate the ordering used by the successful Samsung replay:
	 * receiver configured before sensor setfiles. Mode 14 additionally omits
	 * pre-stream diagnostic reads; the report exposes that omission. */
	for (unsigned setup_stage = 0; setup_stage < 2; setup_stage++) {
		if ((setup_stage == 0) != receiver_first) {
			strlcpy(phase, "sensor ID", sizeof(phase));
			if (!cam_3j1_read16(&bus, 0x0000, &sid) || sid != 0x30a1) goto cleanup;
			strlcpy(phase, "sensor mode", sizeof(phase));
			if (!cam_3j1_apply(&bus, redwood_3j1_global, REDWOOD_3J1_GLOBAL_WORDS)) goto cleanup;
			if (prestream_verify) {
			strlcpy(phase, "sensor microcode readback", sizeof(phase));
			if (!cam_3j1_verify_code(&bus, &code_check)) goto cleanup;
			strlcpy(phase, "global indirect readback", sizeof(phase));
			if (!cam_3j1_verify_indirect(&bus, redwood_3j1_global, REDWOOD_3J1_GLOBAL_WORDS, &indirect_check)) goto cleanup;
			}
			strlcpy(phase, "sensor mode", sizeof(phase));
			if (!(cam_s20_full && cam_s20_stream_fn ? cam_3j1_apply(&bus, redwood_3j1_3648x2736_30fps_pd, REDWOOD_3J1_3648X2736_30FPS_PD_WORDS) :
			    pd_mode ? cam_3j1_apply(&bus, redwood_3j1_1824x1368_30fps_pd, REDWOOD_3J1_1824X1368_30FPS_PD_WORDS) :
			        cam_3j1_apply(&bus, redwood_3j1_1824x1368_30fps, REDWOOD_3J1_1824X1368_30FPS_WORDS))) goto cleanup;
			if (prestream_verify) {
			strlcpy(phase, "sensor mode readback", sizeof(phase));
			if (!cam_3j1_verify_mode(&bus,
			    pd_mode ? redwood_3j1_1824x1368_30fps_pd : redwood_3j1_1824x1368_30fps,
			    pd_mode ? REDWOOD_3J1_1824X1368_30FPS_PD_WORDS : REDWOOD_3J1_1824X1368_30FPS_WORDS,
			    &mode_check)) goto cleanup;
			strlcpy(phase, "mode indirect readback", sizeof(phase));
			if (!cam_3j1_verify_indirect(&bus,
			    pd_mode ? redwood_3j1_1824x1368_30fps_pd : redwood_3j1_1824x1368_30fps,
			    pd_mode ? REDWOOD_3J1_1824X1368_30FPS_PD_WORDS : REDWOOD_3J1_1824X1368_30FPS_WORDS,
			    &indirect_check)) goto cleanup;
			}
			/* Mirror sensor_3j1_cis_mode_change() and cis_set_size(): EMB off, 2x2 binning crop. */
			if (!cam_3j1_write8(&bus, 0x0118, 0)) goto cleanup;
			/* cis_set_size() selects the sensor's 0x2000 register page before crop writes. */
			if (!trace_timing && !normal_timing && !cam_3j1_write16(&bus, 0x6028, 0x2000)) goto cleanup;
			if (!pd_mode && (
			    !cam_3j1_write16(&bus, 0x0344, 164) || !cam_3j1_write16(&bus, 0x0346, 0) ||
			    !cam_3j1_write16(&bus, 0x0348, 3811) || !cam_3j1_write16(&bus, 0x034a, 2735) ||
			    !cam_3j1_write16(&bus, 0x034c, 1824) || !cam_3j1_write16(&bus, 0x034e, 1368) ||
			    !cam_3j1_write16(&bus, 0x0380, 1) || !cam_3j1_write16(&bus, 0x0382, 3) ||
			    !cam_3j1_write16(&bus, 0x0384, 1) || !cam_3j1_write16(&bus, 0x0386, 3) ||
			    !cam_3j1_write8(&bus, 0x0900, 1) || !cam_3j1_write8(&bus, 0x0901, 0x22) ||
			    !cam_3j1_write16(&bus, 0x0400, 0) || !cam_3j1_write16(&bus, 0x0404, 0x10))) goto cleanup;
			/* 2026-11-10 I2C trace: the minimal Vision boot writes 0340=b18d,
			 * 0702=0a before gains. Keep that diagnostic separate from normal 30 fps,
			 * whose long-exposure shifts are explicitly cleared by Samsung's LTE-off. */
			if (trace_timing && (!cam_3j1_write16(&bus, 0x0340, 0xb18d) ||
			    !cam_3j1_write8(&bus, 0x0702, 0x0a))) goto cleanup;
			if (!trace_timing && (!cam_3j1_write16(&bus, 0xfcfc, 0x4000) ||
			    !cam_3j1_write8(&bus, 0x0702, 0) || !cam_3j1_write8(&bus, 0x0704, 0))) goto cleanup;
			/* Samsung's mode-change path applies analog/digital gains before stream.
			 * Option 2 tests those exact clamp values while retaining mode 8. */
			if (stock_gain && (!cam_3j1_write16(&bus, 0x0204, 0x0200) ||
			    (!(trace_timing || normal_timing) && !cam_3j1_write16(&bus, 0x31e8, 0)) ||
			    !cam_3j1_write16(&bus, 0x020e, 0x1000))) goto cleanup;
			if (test_pattern && !cam_3j1_write8(&bus, 0x0601, 1)) goto cleanup;
		} else {
			for (i = 0; i < 7; i++) phy_sc[i] = cam_s20_rd(dphy, phy_sc_offs[i]);
			for (i = 0; i < 4; i++) for (j = 0; j < 9; j++) phy_lane[i][j] = cam_s20_rd(dphy, phy_lane_base[i] + phy_lane_offs[j]);
			for (i = 0; i < 3; i++) phy_bias[i] = cam_s20_rd(bias, phy_bias_offs[i]);
			phy_saved = true;
			for (i = 0; i < 10; i++) csi_saved[i] = cam_s20_rd(csi, csi_offs[i]);
			csi_saved_ok = true; reset_sys = cam_s20_rd(isppre, 0);
			mask0_saved = cam_s20_rd(csi, 0x0010); mask1_saved = cam_s20_rd(csi, 0x0018);
			irq_masks_saved = true;
			strlcpy(phase, "receiver setup", sizeof(phase));
			/* Samsung runs csi_hw_reset() during csi_init, before phy_set(). */
			cam_s20_wr(csi, 0x0004, csi_saved[0] | (1U << 1));
			for (i = 0; i < 10 && (cam_s20_rd(csi, 0x0004) & (1U << 1)); i++) delay_for_interval(10, NSEC_PER_USEC);
			if (cam_s20_rd(csi, 0x0004) & (1U << 1)) { strlcpy(phase, "CSIS reset timeout", sizeof(phase)); goto cleanup; }
			cam_s20_phy_wr(isppre, 0, reset_sys & ~(1U << 3), phy_pace_us);
            cam_s20_dphy_3j1_config(dphy, bias, phy_pace_us);
            cam_s20_phy_wr(isppre, 0, reset_sys | (1U << 3), phy_pace_us);
			/* Stock CSIS1 debug dump before sensor stream: CMN_CTRL=0x4b01.
			 * The 1807 Mbps SAK3J1 mode uses deskew level 2 and VC interleave mode 2;
			 * omitting either leaves the receiver in a different mode from Samsung's. */
			cam_s20_wr(csi, 0x0004,
			    (csi_saved[0] & ~((7U << 13) | (3U << 10) | (3U << 8) | (1U << 21) | 3U)) |
			    (2U << 13) | (2U << 10) | (3U << 8));
			cam_s20_wr(csi, 0x0008, csi_saved[1] | (1U << 4)); /* stock CLK_CTRL=0x10 */
			/* v5.3 ENABLE_DAT is bits 1..4 and ENABLE_CLK is bit 0.
			 * csi_hw_s_lane() enables data lanes before VC setup; csi_hw_enable()
			 * enables the clock lane only after all VC shadow registers are ready. */
			cam_s20_wr(csi, 0x0704, (csi_saved[9] & ~0x1fU) | (0x0fU << 1));
			cam_s20_wr(csi, 0x0040, (0x2bU << 2) | (2U << 12));
			cam_s20_wr(csi, 0x0044, (cam_s20_stream_fn ? (CAM_S20_H << 16) | CAM_S20_W : (1368U << 16) | 1824U));
			/* x1slte mode 8 sends VC1 RAW10 auxiliary rows, 1824x342. */
			cam_s20_wr(csi, 0x0050, (1U << 16) | (0x2bU << 2) | (2U << 12));
			/* VC1 PDAF rows: 1824x342 binned, 3648x684 full (x1slte DT mode0/mode8 vc1) */
			cam_s20_wr(csi, 0x0054, (cam_s20_full && cam_s20_stream_fn) ? ((684U << 16) | 3648U) : ((342U << 16) | 1824U));
			if (match_rest) { /* Samsung: unused VC2/VC3 = VC n, DT 0, mode 2, resolution 0 */
				cam_s20_wr(csi, 0x0060, 0x22000); cam_s20_wr(csi, 0x0064, 0);
				cam_s20_wr(csi, 0x0070, 0x32000); cam_s20_wr(csi, 0x0074, 0);
			}
			cam_s20_wr(csi, 0x0600, csi_saved[7] & ~(1U << 31)); /* LRTE_DISABLE */
			cam_s20_wr(csi, 0x0690, csi_saved[8] | (1U << 23)); /* CSIS_CTRL_PIXEL_ALIGN_MODE=1 */
			cam_s20_wr(csi, 0x000c, 0xfU); /* update all VC shadow registers */
			cam_s20_wr(csi, 0x0704, cam_s20_rd(csi, 0x0704) | 1U);
			/* Samsung request_irq() enables link frame/error sources before CSI_EN. The
			 * iOS GIC leaves SPI 215 masked; poll the raw source register here. */
			cam_s20_wr(csi, 0x0010, 0x00fff07fU);
			cam_s20_wr(csi, 0x0018, 0x0000007fU);
			cam_s20_wr(csi, 0x0004, cam_s20_rd(csi, 0x0004) | 1U); /* CSI_EN; DPHY is already configured */
			cam_s20_wait_ms(2);
		}
	}
	strlcpy(phase, "sensor stream", sizeof(phase));
	/* Samsung stream_on(): choose the BINNING default MIPI table before releasing hold, then wait 8 ms. */
	/* Samsung's sensor_3j1_setfile_A_mipi_{FULL,BINNING}_903_mhz: the same 1807 Mbps link, other PLL dividers */
	if (!cam_3j1_write16(&bus, 0x0136, 0x1a00) ||
	    !cam_3j1_write16(&bus, 0x0300, (cam_s20_full && cam_s20_stream_fn) ? 5 : 7) ||
	    !cam_3j1_write16(&bus, 0x0304, (cam_s20_full && cam_s20_stream_fn) ? 3 : 4) ||
	    !cam_3j1_write16(&bus, 0x0306, (cam_s20_full && cam_s20_stream_fn) ? 0xe9 : 0xd9) ||
	    !cam_3j1_write16(&bus, 0x030c, (cam_s20_full && cam_s20_stream_fn) ? 1 : 0) || !cam_3j1_write16(&bus, 0x030e, 4) ||
	    !cam_3j1_write16(&bus, 0x0310, 0x116) || !cam_3j1_write16(&bus, 0x0312, 1)) goto cleanup;
	/* SAK3J1's Samsung stream-on sequence configures dual-sync before 0x0100. */
	if (!trace_timing && !cam_3j1_write8(&bus, 0x0104, 0)) goto cleanup;
	cam_s20_wait_ms(8);
	if (!cam_3j1_write16(&bus, 0x0a70, 0x0001) || !cam_3j1_write16(&bus, 0x0a72, 0x0100) ||
	    !cam_3j1_write8(&bus, 0x0100, 1)) goto cleanup;
	sensor_on = true;
	if (!cam_3j1_write8(&bus, 0x021e, 0)) goto cleanup; /* non-WDR mode */
	if (warm_restart) {
		/* Keep the configured receiver, clocks and rails alive across a sensor
		 * stream cycle; previous probes powered everything down between calls. */
		cam_s20_wait_ms(500);
		prime_frames = cam_s20_rd(csi, 0x0500);
		strlcpy(phase, "warm sensor stop", sizeof(phase));
		if (!cam_3j1_write8(&bus, 0x0100, 0)) goto cleanup;
		for (i = 0; i < 100; i++) {
			uint8_t standby = 0;
			if (!cam_3j1_read8(&bus, 5, &standby)) goto cleanup;
			if (standby == 0xff) break;
			cam_s20_wait_ms(1);
		}
		if (i == 100) goto cleanup;
		cam_s20_wait_ms(8);
		strlcpy(phase, "warm sensor restart", sizeof(phase));
		if (!cam_3j1_write8(&bus, 0x0100, 1) || !cam_3j1_write8(&bus, 0x021e, 0)) goto cleanup;
	}
	if (!defer_stream_reads) {
		if (!cam_3j1_read8(&bus, 0x0100, &stream) || stream != 1) goto cleanup;
		(void)cam_3j1_read8(&bus, 0x0005, &sensor_fc_before);
	}
	/* CSIS v5.3 FRM_CNT_CH0 is 0x0500 (v5.2 used 0x0100). */
	before = cam_s20_rd(csi, 0x0500);
	if (cam_s20_stream_fn && cam_s20_optical_dma) {
		volatile uint8_t *s_cmn = cam_s20_optical_dma + 0x2400, *s_vc = cam_s20_optical_dma + 0x2000;
		uint32_t idle = 0;
		(void)cam_s20_af_up();
		for (;;) {
			uint32_t st = cam_s20_rd(s_cmn, 8);
			/* DMA_CMN status (is-hw-csi-v5_3.h): bit 4 VC0 frame start, bit 8 VC0 frame end (write 1 to clear).
			 * A frame's own start is cleared as soon as it is seen, so a start flagged together with an end is
			 * the NEXT frame's: then the active slot has moved on and the finished frame is the one before it.
			 * (Reading the wrong one copied a buffer the DMA was writing: tearing, 2026-11-11.) */
			if ((st & (1U << 4)) && !(st & (1U << 8))) {
				cam_s20_wr(s_cmn, 8, 1U << 4);
				st &= ~(1U << 4);
			}
			if (st & (1U << 8)) {
				uint32_t p0 = (cam_s20_rd(s_vc, 0x90) >> 2) & 0x1fU, st2 = cam_s20_rd(s_cmn, 8), ptr;
				bool more;
				if (st & (1U << 4)) ptr = (p0 + 31U) & 0x1fU;                   /* next frame began before p0 was read */
				else if (!(st2 & (1U << 4))) ptr = p0;                           /* still between frames */
				else ptr = (((cam_s20_rd(s_vc, 0x90) >> 2) & 0x1fU) + 31U) & 0x1fU;  /* it began just now */
				cam_s20_wr(s_cmn, 8, (st | st2) & 0x01111110U);
				if (st & 0x01101000U) cam_s20_stream_errors++;
				if (cam_s20_stream_frames < 6)
					printf("camera: S20 stream frame %u: dma status %#x, active slot %u\n", cam_s20_stream_frames, st, ptr);
				/* At frame end the active slot is the one just finished (it advances at the next frame start; a
				 * skipped frame moves it by two): even slots are buffer 0, odd ones buffer 1. */
				if (cam_s20_stream_cached) {
					vm_offset_t line = (vm_offset_t)cam_s20_stream_buf[ptr & 1] & ~(vm_offset_t)63;
					vm_offset_t end = (vm_offset_t)cam_s20_stream_buf[ptr & 1] + cam_s20_stream_bytes;
					for (; line < end; line += 64) __asm__ volatile ("dc ivac, %0" : : "r" (line));
					__asm__ volatile ("dsb sy" ::: "memory");
				}
				more = cam_s20_stream_fn(cam_s20_stream_buf[ptr & 1], CAM_S20_W, CAM_S20_H, ((CAM_S20_W * 10 / 8) + 15) & ~15U);
				cam_s20_stream_frames++;
				idle = 0;
				if (cam_s20_exp_pending) {
					uint32_t el = cam_s20_exp_lines, eg = cam_s20_exp_gain, ef = cam_s20_exp_frame;
					cam_s20_exp_pending = false;
					/* SMIA group hold: frame length, coarse integration, analog gain (0x20 = 1x) */
					(void)cam_3j1_write8(&bus, 0x0104, 1);
					(void)cam_3j1_write16(&bus, 0x0340, (uint16_t)ef);
					(void)cam_3j1_write16(&bus, 0x0202, (uint16_t)el);
					(void)cam_3j1_write16(&bus, 0x0204, (uint16_t)eg);
					(void)cam_3j1_write8(&bus, 0x0104, 0);
				}
				if (cam_s20_af_pending && cam_s20_af_ready) {
					uint32_t want = cam_s20_af_want;
					cam_s20_af_pending = false;
					if (want != cam_s20_af_pos && cam_s20_af_set(want)) cam_s20_af_pos = want;
				}
				if (!more) break;
			} else {
				if (++idle > 2000) { printf("camera: S20 stream: no frame for 2 s, stopping\n"); break; }
				cam_s20_wait_ms(1);
			}
		}
		cam_s20_af_down();
		/* stop memory output before the sensor goes down, as the bounded test does */
		cam_s20_wr(cam_s20_optical_dma, 0x2000, 0);
		cam_s20_wr(cam_s20_optical_dma, 0x2414, 1);
	} else
	for (i = 0; i < (cam_s20_optical_dma ? 5000U : 500U); i++) {
		uint32_t status = cam_s20_rd(csi, 0x0700);
		phy_status_or |= status;
		phy_status_and &= status;
		if ((status & 0xf1U) != 0xf1U) phy_active_samples++;
		irq_seen |= cam_s20_rd(csi, 0x0014);
		if (cam_s20_optical_dma) {
			/* Stop memory output on the first completed frame, before sensor
			 * shutdown; poll within the mode's vertical blanking interval. */
			if (cam_s20_rd(cam_s20_optical_dma, 0x2408) & (1U << 8)) {
				cam_s20_wr(cam_s20_optical_dma, 0x2000, 0);
				cam_s20_wr(cam_s20_optical_dma, 0x2414, 1);
				break;
			}
			delay_for_interval(100, NSEC_PER_USEC);
		} else cam_s20_wait_ms(1);
	}
	if (regdump) { cam_s20_regdump(); regdump_done = true; }
	after = cam_s20_rd(csi, 0x0500);
	if (defer_stream_reads && (!cam_3j1_read8(&bus, 0x0100, &stream) || stream != 1)) goto cleanup;
	(void)cam_3j1_read8(&bus, 0x0005, &sensor_fc_after);
	irq = cam_s20_rd(csi, 0x0014); fs_irq = cam_s20_rd(csi, 0x0024); fe_irq = cam_s20_rd(csi, 0x002c);
	cmn_live = cam_s20_rd(csi, 0x0004); phy_live = cam_s20_rd(csi, 0x0704);
	config_live = cam_s20_rd(csi, 0x0040); config1_live = cam_s20_rd(csi, 0x0050);
	shadow1_live = cam_s20_rd(csi, 0x0250); lrte_live = cam_s20_rd(csi, 0x0600);
	resol_live = cam_s20_rd(csi, 0x0044); phy_status = cam_s20_rd(csi, 0x0700);
	dphy_clk = cam_s20_rd(dphy, 0); lane0_live = cam_s20_rd(dphy, 0x100); lane3_live = cam_s20_rd(dphy, 0x400);
	phy_sc_time1 = cam_s20_rd(dphy, 0x34);
	phy_sd0_ana6 = cam_s20_rd(dphy, 0x120); phy_sd0_ana7 = cam_s20_rd(dphy, 0x124);
	phy_sc_ana2 = cam_s20_rd(dphy, 0x10); phy_sd0_ana2 = cam_s20_rd(dphy, 0x110);
	phy_sd3_ana2 = cam_s20_rd(dphy, 0x410); phy_sd0_settle = cam_s20_rd(dphy, 0x130);
	phy_sd0_deskew = cam_s20_rd(dphy, 0x140);
	phy_bias0 = cam_s20_rd(bias, 0); phy_bias1 = cam_s20_rd(bias, 4); phy_bias2 = cam_s20_rd(bias, 8);
	cmu_qch_live = cam_s20_rd(cmu, 0x3058); csi_qch_live = cam_s20_rd(cmu, 0x3064);
	dma_qch_live = cam_s20_rd(cmu, 0x3078); top_qch_live = cam_s20_rd(cmu, 0x307c);
	(void)cam_3j1_read16(&bus, 0x0112, &sensor_fmt); (void)cam_3j1_read16(&bus, 0x0114, &sensor_lanes);
	(void)cam_3j1_read16(&bus, 0x0306, &sensor_mipi_pll);
	(void)cam_3j1_read16(&bus, 0x0310, &sensor_pll2);
	(void)cam_3j1_read16(&bus, 0x0340, &sensor_frame_length);
	(void)cam_3j1_read16(&bus, 0x0342, &sensor_line_length);
	(void)cam_3j1_read16(&bus, 0x0344, &sensor_crop_x);
	(void)cam_3j1_read16(&bus, 0x0348, &sensor_crop_end_x);
	(void)cam_3j1_read16(&bus, 0x0404, &sensor_scaler);
	(void)cam_3j1_read16(&bus, 0x0900, &sensor_binning);
	(void)cam_3j1_read8(&bus, 0x0601, &pattern_live);
	(void)cam_3j1_read8(&bus, 0x0702, &frame_shift);
	(void)cam_3j1_read8(&bus, 0x0704, &exposure_shift);
	for (i = 0; i < sizeof(diag_regs) / sizeof(diag_regs[0]); i++)
		(void)cam_3j1_read16(&bus, diag_regs[i], &diag_vals[i]);
	/* Samsung sensors use 0x602C/0x602E for indirect reads; 0x6028/0x602A
	 * select the write address and yielded unrelated alternating values. */
	if (cam_3j1_write16(&bus, 0x602c, 0x2000)) {
		for (i = 0; i < 5; i++) {
			if (cam_3j1_write16(&bus, 0x602e, sensor_code_addrs[i]))
				(void)cam_3j1_read16(&bus, 0x6f12, &sensor_code[i]);
		}
	}
	(void)redwood_s2mpb02_camera_read(CAM_S2MPB02_BUCK2, &rails_live[0]);
	(void)redwood_s2mpb02_camera_read(CAM_S2MPB02_LDO16, &rails_live[1]);
	(void)redwood_s2mpb02_camera_read(CAM_S2MPB02_LDO11, &rails_live[2]);
	(void)redwood_s2mpb02_camera_read(CAM_S2MPB02_LDO6, &rails_live[3]);
	(void)redwood_s2mpb02_camera_read(CAM_S2MPB02_LDO5, &rails_live[4]);
	mclk_mux_live = cam_s20_rd(cmu_top, CMU_CIS_CLK1_MUX);
	mclk_div_live = cam_s20_rd(cmu_top, CMU_CIS_CLK1_DIV);
	mclk_gate_live = cam_s20_rd(cmu_top, CMU_CIS_CLK1_GATE);
	mclk_qch_live = cam_s20_rd(cmu_top, CMU_CIS_CLK1_QCH);
	pll4_active = cam_s20_rd(cmu_top, 0x028c);
	for (i = 0; i < 8; i++) platform_clocks[i] = cam_s20_rd(cmu, platform_offs[i]);
	platform_clocks[8] = cam_s20_rd(bias, 0x10);
	pll0_active = cam_s20_rd(cmu_top, 0x018c);
	mclk_pin_drive = cam_s20_rd(mclk, PINCTRL_DRV);
	reset_pin_drive = cam_s20_rd(reset, PINCTRL_DRV);
	for (i = 0; i < 4; i++) {
		phy_analog0[i] = cam_s20_rd(dphy, phy_lane_base[i] + 0x08);
		phy_analog1[i] = cam_s20_rd(dphy, phy_lane_base[i] + 0x0c);
	}
	csis_bus_mux_live = cam_s20_rd(cmu, 0x0600);
	pmu_iso_live = cam_s20_rd(pmu, 0x0730);
	phy_reset_live = cam_s20_rd(isppre, 0);
	irq_mask_live = cam_s20_rd(csi, 0x0010);
	if (pmic_base_on) for (i = 0; i < 3; i++) (void)redwood_s2mpb02_camera_read(pmic_base_regs[i], &pmic_base_live[i]);
	result = after != before;
	strlcpy(phase, result ? "frames observed" : "no CSIS frames", sizeof(phase));
cleanup:
	if (sensor_on || stream) (void)cam_3j1_write8(&bus, 0x0100, 0);
	if (line_probe && csi_saved_ok && phy_saved && clock_ready && rails_saved) {
		unsigned stage, k;
		for (stage = 0; stage < 3; stage++) {
			if (stage == 0) cam_s20_wait_ms(5);           /* standby after 0x0100=0 */
			if (stage == 1) { (void)redwood_gpio_set(reset, 0, 0); cam_s20_wait_ms(2); }
			if (stage == 2) {
				/* Samsung off order: MCLK off, reset low (done), then rails. */
				(void)redwood_gpio_set(mclk, 1, 0); redwood_gpio_func(mclk, 1, REDWOOD_GPIO_OUTPUT);
				if (bus_ready) { redwood_bbi2c_restore(&bus); bus_ready = false; }
				(void)redwood_s2mpb02_camera_bits(CAM_S2MPB02_LDO5, 0x80, rails[4] & 0x80);
				(void)redwood_s2mpb02_camera_bits(CAM_S2MPB02_LDO6, 0x80, rails[3] & 0x80);
				(void)redwood_s2mpb02_camera_bits(CAM_S2MPB02_LDO11, 0x80, rails[2] & 0x80);
				(void)redwood_s2mpb02_camera_bits(CAM_S2MPB02_LDO16, 0x80, rails[1] & 0x80);
				(void)redwood_s2mpb02_camera_bits(CAM_S2MPB02_BUCK2, 0xc0, rails[0] & 0xc0);
				cam_s20_wait_ms(20);
			}
			for (k = 0; k < 20; k++) {
				uint32_t st = cam_s20_rd(csi, 0x0700);
				line_or[stage] |= st; line_and[stage] &= st;
				delay_for_interval(100, NSEC_PER_USEC);
			}
		}
		if (match_rest) {
			unsigned k2;
			uint32_t rs = cam_s20_rd(isppre, 0);
			cam_s20_wr(isppre, 0, rs & ~(1U << 3)); /* PHY held in reset */
			delay_for_interval(500, NSEC_PER_USEC);
			for (k2 = 0; k2 < 20; k2++) {
				uint32_t st = cam_s20_rd(csi, 0x0700);
				line_rst[0] |= st; line_rst[1] &= st;
				delay_for_interval(100, NSEC_PER_USEC);
			}
			cam_s20_wr(isppre, 0, rs);
		}
		line_done = true;
	}
	if (irq_masks_saved) { cam_s20_wr(csi, 0x0010, mask0_saved); cam_s20_wr(csi, 0x0018, mask1_saved); }
	if (csi_saved_ok) cam_s20_wr(csi, 0x0004, cam_s20_rd(csi, 0x0004) & ~1U);
	if (csi_saved_ok) for (i = 10; i > 0; i--) cam_s20_wr(csi, csi_offs[i - 1], csi_saved[i - 1]);
	if (rest_changed) {
		for (i = 0; i < 4; i++) cam_s20_wr(csi, vc23_offs[i], vc23_saved[i]);
		cam_s20_wr(isp_page, 0x418, mux_saved);
		for (i = 0; i < 3; i++) cam_s20_wr(cmu, ois_offs[i], ois_saved[i]);
	}
	if (phy_saved) {
		cam_s20_wr(dphy, 0, 0);
		for (i = 4; i > 0; i--) for (j = 9; j > 0; j--) cam_s20_wr(dphy, phy_lane_base[i - 1] + phy_lane_offs[j - 1], phy_lane[i - 1][j - 1]);
		for (i = 7; i > 0; i--) cam_s20_wr(dphy, phy_sc_offs[i - 1], phy_sc[i - 1]);
		for (i = 3; i > 0; i--) cam_s20_wr(bias, phy_bias_offs[i - 1], phy_bias[i - 1]);
		cam_s20_wr(isppre, 0, reset_sys);
	}
	if (clock_ready) { (void)redwood_gpio_set(mclk, 1, 0); redwood_gpio_func(mclk, 1, REDWOOD_GPIO_OUTPUT);
		cam_s20_wr(cmu_top, CMU_CIS_CLK1_GATE, clock[2]); cam_s20_wr(cmu_top, CMU_CIS_CLK1_QCH, clock[3]);
		cam_s20_wr(cmu_top, CMU_CIS_CLK1_DIV, clock[1]); cam_s20_wr(cmu_top, CMU_CIS_CLK1_MUX, clock[0]); }
	if (pins_saved) (void)redwood_gpio_set(reset, 0, 0);
	if (bus_ready) redwood_bbi2c_restore(&bus);
	if (rails_saved) { (void)redwood_s2mpb02_camera_bits(CAM_S2MPB02_LDO5, 0x80, rails[4] & 0x80);
		(void)redwood_s2mpb02_camera_bits(CAM_S2MPB02_LDO6, 0x80, rails[3] & 0x80); (void)redwood_s2mpb02_camera_bits(CAM_S2MPB02_LDO11, 0x80, rails[2] & 0x80);
		(void)redwood_s2mpb02_camera_bits(CAM_S2MPB02_LDO16, 0x80, rails[1] & 0x80); (void)redwood_s2mpb02_camera_bits(CAM_S2MPB02_BUCK2, 0xc0, rails[0] & 0xc0); }
	if (voltages_set) voltages_restored = redwood_s2mpb02_front_voltages(voltage_saved, true);
	if (pmic_base_on) {
		(void)redwood_s2mpb02_camera_bits(0x2f, 0x80, pmic_base_saved[2] & 0x80);
		(void)redwood_s2mpb02_camera_bits(0x0f, 0xc0, pmic_base_saved[1] & 0xc0);
		(void)redwood_s2mpb02_camera_bits(0x0b, 0xc0, pmic_base_saved[0] & 0xc0);
	}
	if (!voltages_restored) { result = false; strlcpy(phase, "sensor voltage restore failed", sizeof(phase)); }
	if (pins_saved) {
		redwood_gpio_restore(reset, 0, rcon & 0xfU, rpud & 0xfU, rdat & 1U);
		redwood_gpio_restore(mclk, 1, mcon & (0xfU << 4), mpud & (0xfU << 4), mdat & 2U);
	}
	if (rest_changed) {
		/* only the fields changed here: GPC0-0/3 CON+DAT, GPM11-0 PUD */
		cam_s20_update_bits(mclk, REDWOOD_GPIO_CON, 0xfU | (0xfU << 12), gpc0_con_saved);
		cam_s20_update_bits(mclk, REDWOOD_GPIO_DAT, (1U << 0) | (1U << 3), gpc0_dat_saved);
		cam_s20_update_bits(reset, REDWOOD_GPIO_PUD, 0xfU, gpm11_pud_saved);
	}
	if (drive_changed) {
		cam_s20_update_bits(mclk, PINCTRL_DRV, 0xfU << 4, mclk_drive_saved);
		drive_restored = ((cam_s20_rd(mclk, PINCTRL_DRV) ^ mclk_drive_saved) & (0xfU << 4)) == 0;
		if (!drive_restored) { result = false; strlcpy(phase, "MCLK drive restore failed", sizeof(phase)); }
	}
	if (bus_clock_changed) {
		cam_s20_update_bits(cmu_top, 0x1848, 15U, bus_div_saved);
		for (i = 0; i < 1000 && (cam_s20_rd(cmu_top, 0x1848) & (1U << 16)); i++) delay_for_interval(1, NSEC_PER_USEC);
		cam_s20_update_bits(cmu_top, 0x104c, 3U, bus_mux_saved);
		for (i = 0; i < 1000 && (cam_s20_rd(cmu_top, 0x104c) & (1U << 16)); i++) delay_for_interval(1, NSEC_PER_USEC);
		bus_clock_restored = ((cam_s20_rd(cmu_top, 0x104c) ^ bus_mux_saved) & ((1U << 16) | 3U)) == 0 &&
		    ((cam_s20_rd(cmu_top, 0x1848) ^ bus_div_saved) & ((1U << 16) | 15U)) == 0;
		if (!bus_clock_restored) { result = false; strlcpy(phase, "CSIS clock restore failed", sizeof(phase)); }
	}
	if (iso_changed) cam_s20_update_bits(pmu, 0x0730, 1U, iso & 1U);
	if (qch_changed) { cam_s20_update_bits(cmu, 0x3064, qch_mask, csi_qch & qch_mask); cam_s20_update_bits(cmu, 0x3058, qch_mask, cmu_qch & qch_mask); }
	if (top_qch_changed) cam_s20_update_bits(cmu, 0x307c, qch_mask, top_qch & qch_mask);
	if (dma_qch_changed) cam_s20_update_bits(cmu, 0x3078, qch_mask, dma_qch & qch_mask);
done:
	(void)snprintf(out, cap, "{\"camera\":\"SAK3J1\",\"stage\":\"bounded-CSIS-stream\",\"result\":\"%s\",\"phase\":\"%s\",\"code_check\":{\"reads\":%u,\"mismatches\":%u,\"io_errors\":%u,\"first_reg\":\"%#x\",\"expected\":\"%#x\",\"observed\":\"%#x\"},\"mode_check\":{\"reads\":%u,\"mismatches\":%u,\"io_errors\":%u,\"first_reg\":\"%#x\",\"expected\":\"%#x\",\"observed\":\"%#x\"},\"indirect_check\":{\"reads\":%u,\"mismatches\":%u,\"io_errors\":%u,\"first_reg\":\"%#x\",\"expected\":\"%#x\",\"observed\":\"%#x\"},\"phy_pace_us\":%u,\"defer_stream_reads\":%s,\"warm_restart\":%s,\"prime_frames\":%u,\"platform_clocks\":[\"%#x\",\"%#x\",\"%#x\",\"%#x\",\"%#x\",\"%#x\",\"%#x\",\"%#x\",\"%#x\"],\"prestream_verify\":%s,\"drive_restored\":%s,\"sensor_stream\":%u,\"sensor_frame_counter_before\":%u,\"sensor_frame_counter_after\":%u,\"sensor_raw_format\":\"%#x\",\"sensor_lane_mode\":\"%#x\",\"sensor_mipi_pll\":\"%#x\",\"sensor_pll2\":\"%#x\",\"sensor_frame_length\":\"%#x\",\"frame_shift\":%u,\"exposure_shift\":%u,\"sensor_line_length\":\"%#x\",\"sensor_crop_x\":\"%#x\",\"sensor_crop_end_x\":\"%#x\",\"sensor_scaler\":\"%#x\",\"sensor_binning\":\"%#x\",\"sensor_code\":[\"%#x\",\"%#x\",\"%#x\",\"%#x\",\"%#x\"],\"pattern\":%u,\"rails\":[%u,%u,%u,%u,%u],\"mclk\":[\"%#x\",\"%#x\",\"%#x\",\"%#x\"],\"top_bus_mux\":\"%#x\",\"top_bus_div\":\"%#x\",\"pll4_before\":\"%#x\",\"pll4_active\":\"%#x\",\"pll0_active\":\"%#x\",\"pin_drive\":[\"%#x\",\"%#x\"],\"phy_analog0\":[\"%#x\",\"%#x\",\"%#x\",\"%#x\"],\"phy_analog1\":[\"%#x\",\"%#x\",\"%#x\",\"%#x\"],\"bus_clock_restored\":%s,\"bus_mux\":\"%#x\",\"pmu_iso\":\"%#x\",\"phy_reset\":\"%#x\",\"irq_mask\":\"%#x\",\"frames_before\":%u,\"frames_after\":%u,\"irq_src0\":\"%#x\",\"irq_seen\":\"%#x\",\"frame_start_irq\":\"%#x\",\"frame_end_irq\":\"%#x\",\"csis_cmn\":\"%#x\",\"csis_phy_ctrl\":\"%#x\",\"csis_config\":\"%#x\",\"csis_config1\":\"%#x\",\"csis_shadow1\":\"%#x\",\"csis_lrte\":\"%#x\",\"csis_resolution\":\"%#x\",\"phy_status\":\"%#x\",\"phy_status_or\":\"%#x\",\"phy_status_and\":\"%#x\",\"phy_active_samples\":%u,\"dphy_clock\":\"%#x\",\"dphy_lane0\":\"%#x\",\"dphy_lane3\":\"%#x\",\"phy_sc_time1\":\"%#x\",\"phy_sd0_ana6\":\"%#x\",\"phy_sd0_ana7\":\"%#x\",\"phy_sc_ana2\":\"%#x\",\"phy_sd0_ana2\":\"%#x\",\"phy_sd3_ana2\":\"%#x\",\"phy_sd0_settle\":\"%#x\",\"phy_sd0_deskew\":\"%#x\",\"phy_bias0\":\"%#x\",\"phy_bias1\":\"%#x\",\"phy_bias2\":\"%#x\",\"qch_csi_bus\":\"%#x\",\"qch_csis1\":\"%#x\",\"qch_wdma\":\"%#x\",\"qch_pdp_top\":\"%#x\",\"state_restored\":%s}\n",
	    result ? "ok" : "error", phase, code_check.reads, code_check.mismatches, code_check.io_errors, code_check.first_reg, code_check.expected, code_check.observed, mode_check.reads, mode_check.mismatches, mode_check.io_errors, mode_check.first_reg, mode_check.expected, mode_check.observed, indirect_check.reads, indirect_check.mismatches, indirect_check.io_errors, indirect_check.first_reg, indirect_check.expected, indirect_check.observed, phy_pace_us, defer_stream_reads ? "true" : "false", warm_restart ? "true" : "false", prime_frames, platform_clocks[0], platform_clocks[1], platform_clocks[2], platform_clocks[3], platform_clocks[4], platform_clocks[5], platform_clocks[6], platform_clocks[7], platform_clocks[8], prestream_verify ? "true" : "false", drive_restored ? "true" : "false", stream, sensor_fc_before, sensor_fc_after, sensor_fmt, sensor_lanes, sensor_mipi_pll, sensor_pll2, sensor_frame_length, frame_shift, exposure_shift, sensor_line_length, sensor_crop_x, sensor_crop_end_x, sensor_scaler, sensor_binning, sensor_code[0], sensor_code[1], sensor_code[2], sensor_code[3], sensor_code[4], pattern_live, rails_live[0], rails_live[1], rails_live[2], rails_live[3], rails_live[4], mclk_mux_live, mclk_div_live, mclk_gate_live, mclk_qch_live, bus_mux_live, bus_div_live, pll4_before, pll4_active, pll0_active, mclk_pin_drive, reset_pin_drive, phy_analog0[0], phy_analog0[1], phy_analog0[2], phy_analog0[3], phy_analog1[0], phy_analog1[1], phy_analog1[2], phy_analog1[3], bus_clock_restored ? "true" : "false", csis_bus_mux_live, pmu_iso_live, phy_reset_live, irq_mask_live, before, after, irq, irq_seen, fs_irq, fe_irq, cmn_live, phy_live, config_live, config1_live, shadow1_live, lrte_live, resol_live, phy_status, phy_status_or, phy_status_and, phy_active_samples, dphy_clk, lane0_live, lane3_live, phy_sc_time1, phy_sd0_ana6, phy_sd0_ana7, phy_sc_ana2, phy_sd0_ana2, phy_sd3_ana2, phy_sd0_settle, phy_sd0_deskew, phy_bias0, phy_bias1, phy_bias2, cmu_qch_live, csi_qch_live, dma_qch_live, top_qch_live, (voltages_restored && bus_clock_restored && drive_restored) ? "true" : "false");
	if (pmic_base) {
		size_t used = strlen(out);
		if (used < cap) (void)snprintf(out + used, cap - used, "{\"pmic_base\":{\"saved\":[\"%#x\",\"%#x\",\"%#x\"],\"live\":[\"%#x\",\"%#x\",\"%#x\"]}}\n",
		    pmic_base_saved[0], pmic_base_saved[1], pmic_base_saved[2], pmic_base_live[0], pmic_base_live[1], pmic_base_live[2]);
	}
	if (pd_cycle) {
		size_t used = strlen(out);
		if (used < cap) (void)snprintf(out + used, cap - used, "{\"pd_cycle\":{\"save_smc\":\"%#llx\",\"off\":%d,\"on\":%d}}\n",
		    pd_save_smc, pd_off_ok, pd_on_ok);
	}
	if (line_done) {
		size_t used = strlen(out);
		if (used < cap) (void)snprintf(out + used, cap - used,
		    "{\"line_probe\":{\"standby\":[\"%#x\",\"%#x\"],\"reset\":[\"%#x\",\"%#x\"],\"rails_off\":[\"%#x\",\"%#x\"],\"phy_in_reset\":[\"%#x\",\"%#x\"],\"matched_rest\":%s,\"note\":\"[or,and] of PHY_STATUS\"}}\n",
		    line_or[0], line_and[0], line_or[1], line_and[1], line_or[2], line_and[2], line_rst[0], line_rst[1], match_rest ? "true" : "false");
	}
	if (regdump_done) {
		size_t used = strlen(out);
		if (used < cap) { int w = snprintf(out + used, cap - used, "regdump %u:", CAM_REGDUMP_COUNT); if (w > 0) used += (size_t)w; }
		for (i = 0; i < CAM_REGDUMP_COUNT && used + 10 < cap; i++) {
			int w = snprintf(out + used, cap - used, " %08x", cam_regdump_vals[i]);
			if (w > 0) used += (size_t)w;
		}
		if (used + 1 < cap) { out[used++] = '\n'; out[used] = 0; }
	}
	if (sensor_on) {
		size_t used = strlen(out);
		for (i = 0; i < sizeof(diag_regs) / sizeof(diag_regs[0]) && used < cap; i++) {
			int written = snprintf(out + used, cap - used, "%04x=%04x%s", diag_regs[i], diag_vals[i],
			    i + 1 == sizeof(diag_regs) / sizeof(diag_regs[0]) ? "\n" : " ");
			if (written < 0) break;
			used += (size_t)written < cap - used ? (size_t)written : cap - used;
		}
	}
}

/* CSIS/DPHY streaming probe removed after 2026-11-08 receiver-path kernel panic. */

static size_t
cam_s20_pin_snapshot(char *out, size_t cap, size_t used, const char *resource,
	    uint32_t block, uint32_t bank_offset, uint32_t phys, const char *pin, unsigned index)
{
	volatile uint8_t *bank = redwood_gpio_bank(block, bank_offset);
	uint32_t con = cam_s20_rd(bank, REDWOOD_GPIO_CON);
	uint32_t dat = cam_s20_rd(bank, REDWOOD_GPIO_DAT);
	uint32_t pud = cam_s20_rd(bank, REDWOOD_GPIO_PUD);
	uint32_t drv = cam_s20_rd(bank, PINCTRL_DRV);
	uint32_t cfg = (con >> (index * 4)) & 0xf;
	uint32_t val = (dat >> index) & 1;
	uint32_t pull = (pud >> (index * 4)) & 0xf;
	uint32_t drive = (drv >> (index * 4)) & 0xf;

	if (used < cap) {
		used += (size_t)snprintf(out + used, cap - used,
		    "{\"resource\":\"%s\",\"pin\":\"%s\",\"phys\":\"%#x\",\"index\":%u,"
		    "\"function\":%u,\"value\":%u,\"pull\":%u,\"drive\":%u,"
		    "\"con_raw\":\"%#x\",\"dat_raw\":\"%#x\",\"pud_raw\":\"%#x\","
		    "\"drv_raw\":\"%#x\",\"write\":false}\n", resource, pin, phys, index,
		    cfg, val, pull, drive, con, dat, pud, drv);
	}
	return used;
}

static void
cam_s20_inventory(__unused const uint64_t *v, __unused unsigned n, char *out, size_t cap)
{
	size_t used;

	if (cap == 0) {
		return;
	}
	used = (size_t)snprintf(out, cap, "camera-s20: read-only pin inventory; no camera power or clock changes\n");
	/* x1slte fragment@16 targets hsi2c_3 and contains is-2ld@10:
	 * HSI2C3 at 0x10580000 uses gpp0-6/7. */
	used = cam_s20_pin_snapshot(out, cap, used, "main.sensor-i2c", PINCTRL_PERIC0_PHYS,
	    0x000U, 0x10430000U, "gpp0-6", 6);
	used = cam_s20_pin_snapshot(out, cap, used, "main.sensor-i2c", PINCTRL_PERIC0_PHYS,
	    0x000U, 0x10430000U, "gpp0-7", 7);
	/* x1slte fragment@17 targets hsi2c_1 and contains is-3j1@10:
	 * HSI2C1 at 0x10560000 uses gpp0-2/3. HSI2C4/gpp1-0/1 is the display-PMIC bus;
	 * HSI2C45/gpm14/15 is for rear tele 2LA. */
	used = cam_s20_pin_snapshot(out, cap, used, "front.sensor-i2c", PINCTRL_PERIC0_PHYS,
	    0x000U, 0x10430000U, "gpp0-2", 2);
	used = cam_s20_pin_snapshot(out, cap, used, "front.sensor-i2c", PINCTRL_PERIC0_PHYS,
	    0x000U, 0x10430000U, "gpp0-3", 3);
	used = cam_s20_pin_snapshot(out, cap, used, "main.mclk", PINCTRL_PERIC1_PHYS, PINCTRL_PERIC1_GPC0,
	    0x107300a0U, "gpc0-0", 0);
	used = cam_s20_pin_snapshot(out, cap, used, "front.mclk", PINCTRL_PERIC1_PHYS, PINCTRL_PERIC1_GPC0,
	    0x107300a0U, "gpc0-1", 1);
	used = cam_s20_pin_snapshot(out, cap, used, "front.reset", PINCTRL_CMGP_PHYS, PINCTRL_CMGP_GPM11,
	    0x15c30160U, "gpm11-0", 0);
	(void)cam_s20_pin_snapshot(out, cap, used, "main.reset", PINCTRL_CMGP_PHYS, PINCTRL_CMGP_GPM22,
	    0x15c302c0U, "gpm22-0", 0);
}

/* Explicit bounded auxiliary probes. Identity paths never write setfiles.
 * Only the separately requested A202 sensor-counter test writes ordinary2LA
 * configuration and starts/stops the sensor. No CSI/PHY/DMA access or bus scan. */
extern bool redwood_camera_aux_acquire(void);
extern void redwood_camera_aux_release(bool clean);
extern bool redwood_s2mpb02_2la_power(struct cam_aux_power *, bool, bool *);
extern bool redwood_s2mpb02_gw2_power(struct cam_gw2_power *,bool,bool *,void *,
    bool (*)(void *,uint8_t,uint8_t *),bool (*)(void *,uint8_t,uint8_t));
static bool aux_pmic03_read(void *ctx,uint8_t reg,uint8_t *v){(void)ctx;return cam_s20_pmic03_read(reg,v);}
static bool aux_pmic03_write(void *ctx,uint8_t reg,uint8_t v){
    (void)ctx;if(reg!=0x03&&reg!=0x05&&reg!=0x08&&reg!=0x09)return false;
    return redwood_bbi2c_write_reg(&cam_pmic03_bus,CAM_S2MPB03_ADDR,reg,v);
}
static bool aux_sensor_write16(void *p,uint16_t reg,uint16_t value){
    const uint8_t bytes[4]={(uint8_t)(reg>>8),(uint8_t)reg,(uint8_t)(value>>8),(uint8_t)value};
    return redwood_bbi2c_xfer(p,0x2d,bytes,4,NULL,0);
}
static bool aux_sensor_read(struct redwood_bbi2c *bus,uint16_t reg,uint8_t *v,unsigned bytes){
    const uint8_t address[2]={(uint8_t)(reg>>8),(uint8_t)reg};
    return redwood_bbi2c_xfer(bus,0x2d,address,2,v,bytes);
}
static bool aux_sensor_read16(void *p,uint16_t reg,uint16_t *value){
    uint8_t v[2]={0};if(!aux_sensor_read(p,reg,v,2))return false;*value=(uint16_t)((v[0]<<8)|v[1]);return true;
}
static bool aux_sensor_write8(void *p,uint16_t reg,uint8_t value){
    const uint8_t v[3]={(uint8_t)(reg>>8),(uint8_t)reg,value};return redwood_bbi2c_xfer(p,0x2d,v,3,NULL,0);
}
static void aux_sensor_pause(void *p,unsigned us){(void)p;delay_for_interval(us,NSEC_PER_USEC);}
static bool aux_bus_restore_checked(struct redwood_bbi2c *bus){
    volatile uint8_t *sb=bus->sda_bank?bus->sda_bank:bus->bank;
    redwood_bbi2c_restore(bus);
    uint32_t sc=0xfU<<(bus->scl*4),sd=0xfU<<(bus->sda*4);
    return !(((cam_s20_rd(bus->bank,REDWOOD_GPIO_CON)^bus->saved_con)&sc)||
        ((cam_s20_rd(bus->bank,REDWOOD_GPIO_PUD)^bus->saved_pud)&sc)||
        ((cam_s20_rd(sb,REDWOOD_GPIO_CON)^(sb==bus->bank?bus->saved_con:bus->saved_sda_con))&sd)||
        ((cam_s20_rd(sb,REDWOOD_GPIO_PUD)^(sb==bus->bank?bus->saved_pud:bus->saved_sda_pud))&sd));
}
#include "redwood_camera_s20_aux_rx_impl.h"

static bool aux_gw2_send(void *context,const uint8_t *bytes,unsigned count){
    return redwood_bbi2c_xfer(context,0x2d,bytes,count,NULL,0);
}
static bool aux_gw2_program(struct redwood_bbi2c *bus,bool full){
    struct cam_gw2_table_io io={bus,aux_gw2_send,aux_sensor_pause};
#define GW2_TABLE(t) cam_gw2_apply_table(io,t,(unsigned)(sizeof(t)/sizeof((t)[0])))
    bool ok=GW2_TABLE(redwood_GW2_setfile_A_Tnp)&&GW2_TABLE(redwood_GW2_setfile_A_Global)&&
        GW2_TABLE(redwood_gw2_dual_single_A_settings)&&(full?GW2_TABLE(redwood_gw2_setfile_A_4864x3648_30fps):GW2_TABLE(redwood_gw2_setfile_A_2432x1824_30fps));
#undef GW2_TABLE
    /* No unvalidated EEPROM data: stock driver's uncalibrated-module fallback.
     * PDXTC/PDAF calibration and image quality remain separate qualification. */
    return ok&&aux_sensor_write16(bus,0x6028,0x2000)&&aux_sensor_write16(bus,0x602a,0x55b0)&&
        aux_sensor_write16(bus,0x6f12,0)&&aux_sensor_write16(bus,0xfcfc,0x4000);
}
static bool cam_gw2_upload_slow;
static bool aux_gw2_tables(struct redwood_bbi2c *bus,bool full){
    /* Speed only the bounded upload; identity/control reads retain4us timing. */
    uint32_t saved=bus->half_us;
    bus->half_us=cam_gw2_upload_slow?4U:(uint32_t)__atomic_load_n(&cam_s20_rear_i2c_half_us,__ATOMIC_RELAXED);
    bool ok=aux_gw2_program(bus,full);
    if(!ok&&bus->half_us<4){
        cam_gw2_upload_slow=true;bus->half_us=4;
        printf("camera: GW2 fast upload failed; replaying sensor reset/TNP at4us\n");
        ok=aux_gw2_program(bus,full);
    }
    bus->half_us=saved;return ok;
}
static bool cam_s20_aux_run(bool gw2, bool stream, bool raw, const uint64_t *args, unsigned n, redwood_s20_frame_fn fn, bool session, char *out, size_t cap) {
    struct redwood_bbi2c bus={.name="S20 2LA HSI2C45 gpm14-0/gpm15-0",.scl=0,.sda=0,.half_us=4};
    struct cam_aux_power rails={0};
    struct cam_gw2_power gw2_rails={0};
    struct ax_receiver receiver={0};bool receiver_started=false;
    bool gw2_full=gw2&&raw&&n==3&&args[2]==1;
    struct ax_2la_control_report controls={0};
    uint64_t requested_lines=raw&&n>0?args[0]:3000,requested_gain=raw&&n>1?args[1]:0x20;
    if(session){requested_lines=cam_s20_exp_lines;requested_gain=cam_s20_exp_gain;}
    bool pmic03_saved=false,stream_sent=false,stream_stop=false,af_started=false;
    unsigned counter_changes=0;uint8_t last_counter=0xff;
    volatile uint8_t *reset=NULL,*mclk=NULL,*cmu=NULL;
    uint32_t offs[4]={0x1024,0x1820,0x2028,0x3010};
    unsigned mclk_pin=gw2?0:3;
    const char *sensor=gw2?"GW2":"2LA";
    uint32_t r[3]={0},m[3]={0},clock[4]={0};
    uint8_t model[2]={0},revision[2]={0};uint16_t mode_width=0,mode_height=0;
    bool pins=false,bus_saved=false,powered=false,clock_saved=false,clean=true,ok=false;
    const char *phase="busy";
    if(!cap)return false;
    if((!raw&&n)||(raw&&(n>(gw2?3U:2U)||(gw2&&n==3&&args[2]>1)||!ax_2la_controls_valid(requested_lines,requested_gain)))){snprintf(out,cap,"{\"camera\":\"%s\",\"result\":\"bad_arguments\"}\n",sensor);return false;}
    if(!session&&!redwood_camera_aux_acquire())goto report;
    if(raw&&!fn){
        lck_mtx_lock(&cam_frame_lock);
        if(cam_frame_copy)kfree_data(cam_frame_copy,cam_frame_size);
        cam_frame_copy=NULL;cam_frame_size=0;lck_mtx_unlock(&cam_frame_lock);
    }
    phase="map";
    reset=redwood_gpio_bank(PINCTRL_CMGP_PHYS,gw2?0x400:0x420);
    mclk=redwood_gpio_bank(PINCTRL_PERIC1_PHYS,PINCTRL_PERIC1_GPC0);
    bus.bank=redwood_gpio_bank(PINCTRL_CMGP_PHYS,0x1c0);
    bus.sda_bank=redwood_gpio_bank(PINCTRL_CMGP_PHYS,0x1e0);
    if(gw2){
        /* Active pinctrl hsi2c1-bus is gpp0-2/3. The recovery gpios property
         * lists gpc0 pins and MUST NOT be mistaken for native bus routing. */
        bus.name="S20 GW2 HSI2C1 gpp0-2/3";
        bus.bank=redwood_gpio_bank(PINCTRL_PERIC0_PHYS,0);bus.sda_bank=bus.bank;
        bus.scl=2;bus.sda=3;
        offs[0]=CMU_CIS_CLK0_MUX;offs[1]=CMU_CIS_CLK0_DIV;
        offs[2]=CMU_CIS_CLK0_GATE;offs[3]=CMU_CIS_CLK0_QCH;
    }
    cmu=(volatile uint8_t *)cam_s20_io_map(CMU_TOP_PHYS,0x4000);
    if(!reset||!mclk||!bus.bank||!bus.sda_bank||!cmu)goto cleanup;
    for(unsigned i=0;i<3;i++){r[i]=cam_s20_rd(reset,i*4);m[i]=cam_s20_rd(mclk,i*4);}
    pins=true;
    redwood_gpio_set(reset,0,0);redwood_gpio_func(reset,0,REDWOOD_GPIO_OUTPUT);
    redwood_gpio_pull(reset,0,REDWOOD_GPIO_PULL_NONE);
    phase="rails (cold dedicated supplies and BUCK2 1.3V required)";
    if(gw2){
        phase="S2MPB03 bus";
        cam_pmic03_bus.bank=redwood_gpio_bank(PINCTRL_CMGP_PHYS,PINCTRL_CMGP_GPM0);
        cam_pmic03_bus.sda_bank=redwood_gpio_bank(PINCTRL_CMGP_PHYS,PINCTRL_CMGP_GPM1);
        if(!cam_pmic03_bus.bank||!cam_pmic03_bus.sda_bank)goto cleanup;
        pmic03_saved=true;
        if(!redwood_bbi2c_setup(&cam_pmic03_bus,false,out,cap))goto cleanup;
        phase="GW2 fixed rail transaction";
        if(!redwood_s2mpb02_gw2_power(&gw2_rails,false,&clean,NULL,aux_pmic03_read,aux_pmic03_write))goto cleanup;
    }else if(!redwood_s2mpb02_2la_power(&rails,false,&clean))goto cleanup;
    powered=true;
    phase="I2C setup";bus_saved=true;
    if(!redwood_bbi2c_setup(&bus,true,out,cap))goto cleanup;
    delay_for_interval(10,NSEC_PER_USEC);
    if(!gw2){redwood_gpio_set(reset,0,1);delay_for_interval(10,NSEC_PER_USEC);}
    for(unsigned i=0;i<4;i++)clock[i]=cam_s20_rd(cmu,offs[i]);
    clock_saved=true;phase="MCLK";
    cam_s20_wr(cmu,offs[0],clock[0]&~1U);cam_s20_wr(cmu,offs[1],clock[1]&~0x1fU);
    cam_s20_wr(cmu,offs[3],clock[3]|3U);
    cam_s20_wr(cmu,offs[2],clock[2]|(1U<<20)|(1U<<21));
    redwood_gpio_set(mclk,mclk_pin,0);redwood_gpio_func(mclk,mclk_pin,2);redwood_gpio_pull(mclk,mclk_pin,REDWOOD_GPIO_PULL_NONE);
    if(gw2){redwood_gpio_set(reset,0,1);delay_for_interval(8,NSEC_PER_MSEC);}
    else delay_for_interval(2,NSEC_PER_MSEC);
    phase="model/revision";
    const uint8_t a[2]={0,0},b[2]={0,2};
    ok=redwood_bbi2c_xfer(&bus,0x2d,a,2,model,2)&&redwood_bbi2c_xfer(&bus,0x2d,b,2,revision,2);
    if(ok&&(model[0]==0&&model[1]==0))ok=false;
    if(ok&&(model[0]==0xff&&model[1]==0xff))ok=false;
    if(stream){
        if(!ok||(gw2?(model[0]!=0x09||model[1]!=0x72||revision[0]!=0xa3||revision[1]!=0):
            (model[0]!=0x20||model[1]!=0xca||revision[0]!=0xa2||revision[1]!=0x02))){ok=false;phase="A202 model/revision gate";goto cleanup;}
        phase=gw2?"A300 TNP/global/single-sync/2432x1824 mode":"A202 initial/global2/mode/single-sync";
        struct cam_aux_table_io io={&bus,aux_sensor_write16,aux_sensor_pause};
#define AUX_TABLE(t) cam_aux_apply_table(io,t,(unsigned)(sizeof(t)/sizeof((t)[0])))
        ok=gw2?aux_gw2_tables(&bus,gw2_full):AUX_TABLE(redwood_2la_setfile_A_initial_setting)&&AUX_TABLE(redwood_2la_setfile_A_Global_2)&&
           AUX_TABLE(redwood_2la_setfile_A_4032x3024_30fps)&&AUX_TABLE(redwood_2la_dual_single_A_settings);
#undef AUX_TABLE
        if(!ok)goto cleanup;
        uint8_t size[4]={0};phase="mode output readback";
        if(gw2){
            ok=aux_sensor_read16(&bus,0x034c,&mode_width)&&aux_sensor_read16(&bus,0x034e,&mode_height)&&mode_width==(gw2_full?4864:2432)&&mode_height==(gw2_full?3648:1824);
        }else{
            ok=aux_sensor_read(&bus,0x034c,size,4)&&size[0]==0x0f&&size[1]==0xc0&&size[2]==0x0b&&size[3]==0xd0;
            mode_width=(size[0]<<8)|size[1];mode_height=(size[2]<<8)|size[3];
        }
        if(!ok)goto cleanup;
        if(gw2&&raw){phase="GW2 AK737x motor0x0f";if(!cam_s20_af_up_for(true,true)){ok=false;goto cleanup;}af_started=true;}
        if(raw){
            phase="bounded exposure/gain readback";
            struct ax_2la_control_io control_io={&bus,aux_sensor_read16,aux_sensor_write16,aux_sensor_write8};
            if(!(gw2?(gw2_full?ax_gw2_full_controls_apply(control_io,(uint16_t)requested_lines,(uint16_t)requested_gain,&controls):ax_gw2_controls_apply(control_io,(uint16_t)requested_lines,(uint16_t)requested_gain,&controls)):ax_2la_controls_apply(control_io,(uint16_t)requested_lines,(uint16_t)requested_gain,&controls))){ok=false;goto cleanup;}
        }
        if(raw){receiver_started=true;if(!ax_receiver_begin_sensor(&receiver,fn!=NULL,gw2_full?2:gw2?1:0)){ok=false;phase=receiver.phase;goto cleanup;}}
        phase=raw?"sensor and RAW receiver":"sensor frame counter (no receiver)";stream_sent=true;
        ok=aux_sensor_write16(&bus,0x0100,0x0100);
        if(!ok)goto cleanup;
        if(fn){
            unsigned idle=0;bool more=true;
            cam_s20_exp_pending=false;
            while(more){
                bool failed=false;const volatile uint8_t *data=ax_receiver_next(&receiver,&failed);
                if(failed){ok=false;phase="continuous receiver error";break;}
                if(!data){if(++idle>1500){ok=false;phase="continuous frame timeout";break;}cam_s20_wait_ms(1);continue;}
                idle=0;cam_s20_stream_frames++;
                more=fn(data,mode_width,mode_height,((mode_width*10/8+15)&~15U));
                if(gw2&&cam_s20_af_pending&&cam_s20_af_ready){
                    uint32_t want=cam_s20_af_want;cam_s20_af_pending=false;
                    if(want!=cam_s20_af_pos){if(!cam_s20_af_set(want)){ok=false;phase="GW2 focus write failed";break;}cam_s20_af_pos=want;}
                }
                if(cam_s20_exp_pending&&more){
                    uint32_t lines=cam_s20_exp_lines,gain=cam_s20_exp_gain;
                    cam_s20_exp_pending=false;
                    struct ax_2la_control_io control_io={&bus,aux_sensor_read16,aux_sensor_write16,aux_sensor_write8};
                    if(!(gw2?(gw2_full?ax_gw2_full_controls_apply(control_io,(uint16_t)lines,(uint16_t)gain,&controls):ax_gw2_controls_apply(control_io,(uint16_t)lines,(uint16_t)gain,&controls)):ax_2la_controls_apply(control_io,(uint16_t)lines,(uint16_t)gain,&controls))){
                        ok=false;cam_s20_stream_exposure_failed=true;phase="continuous exposure readback failed";break;
                    }
                }
            }
        }else for(unsigned i=0;i<12;i++){
            uint8_t counter=0xff;delay_for_interval(50,NSEC_PER_MSEC);
            if(!aux_sensor_read(&bus,0x0005,&counter,1)){ok=false;break;}
            if(counter!=0xff){if(last_counter!=0xff&&counter!=last_counter)counter_changes++;last_counter=counter;}
        }
        if(!fn&&counter_changes<2)ok=false;
    }
cleanup:
    if(stream_sent){
        bool stopped=aux_sensor_write16(&bus,0xfcfc,0x4000)&&aux_sensor_write16(&bus,0x0100,0x0000);
        if(stopped)for(unsigned i=0;i<10;i++){
            uint8_t counter=0;delay_for_interval(10,NSEC_PER_MSEC);
            if(!aux_sensor_read(&bus,0x0005,&counter,1))break;
            if(counter==0xff){stream_stop=true;break;}
        }
        if(!stream_stop)ok=false;
    }
    if(receiver_started){
        if(!ax_receiver_end(&receiver))clean=false;
        if(!fn&&!receiver.exported)ok=false;
        if(!fn||!clean)phase=receiver.phase;
    }
    if(af_started)cam_s20_af_down();
    if(pins){redwood_gpio_set(reset,0,0);redwood_gpio_set(mclk,mclk_pin,0);redwood_gpio_func(mclk,mclk_pin,REDWOOD_GPIO_OUTPUT);}
    if(clock_saved){
        /* Gate first; then restore div/mux/QCH and the prior gate exactly. */
        cam_s20_wr(cmu,offs[2],clock[2]&~((1U<<20)|(1U<<21)));
        cam_s20_wr(cmu,offs[1],clock[1]);cam_s20_wr(cmu,offs[0],clock[0]);
        cam_s20_wr(cmu,offs[3],clock[3]);cam_s20_wr(cmu,offs[2],clock[2]);
        const uint32_t masks[4]={1U,0x1fU,1U<<20,3U};
        for(unsigned i=0;i<4;i++)if((cam_s20_rd(cmu,offs[i])^clock[i])&masks[i])clean=false;
    }
    if(bus_saved&&!aux_bus_restore_checked(&bus))clean=false;
    if(powered){
        bool restored=true;
        bool done=gw2?redwood_s2mpb02_gw2_power(&gw2_rails,true,&restored,NULL,aux_pmic03_read,aux_pmic03_write):
            redwood_s2mpb02_2la_power(&rails,true,&restored);
        if(!done||!restored)clean=false;
    }
    if(pmic03_saved&&!aux_bus_restore_checked(&cam_pmic03_bus))clean=false;
    if(pins){
        redwood_gpio_restore(reset,0,r[0],r[2],r[1]);
        redwood_gpio_restore(mclk,mclk_pin,m[0],m[2],m[1]);
        for(unsigned i=0;i<3;i++){
            uint32_t rm=i==1?((r[0]&0xfU)==REDWOOD_GPIO_OUTPUT?1U:0):0xfU;
            uint32_t mm=i==1?(((m[0]>>(mclk_pin*4))&0xfU)==REDWOOD_GPIO_OUTPUT?(1U<<mclk_pin):0):(0xfU<<(mclk_pin*4));
            if(((cam_s20_rd(reset,i*4)^r[i])&rm)||((cam_s20_rd(mclk,i*4)^m[i])&mm))clean=false;
        }
    }
    redwood_camera_aux_release(clean);
report:
    snprintf(out,cap,"{\"camera\":\"%s\",\"result\":\"%s\",\"phase\":\"%s\",\"model\":\"0x%02x%02x\",\"revision\":\"0x%02x%02x\",\"cleanup_ok\":%s,\"streamed\":%s,\"counter_changes\":%u,\"stream_stop_verified\":%s,\"raw_exported\":%s,\"receiver_irq\":%u,\"receiver_error\":%u,\"receiver_frames\":%u,\"changed_words\":%u,\"link_errors\":[%u,%u],\"link_irq_raw\":[%u,%u],\"controls_applied\":%s,\"controls_before\":[%u,%u,%u],\"controls_after\":[%u,%u,%u],\"frame_lines\":%u,\"line_pck\":%u,\"guard_errors\":%u,\"pixel_range\":[%u,%u],\"receiver_write\":[%u,%u,%u,%u,%u],\"mode_size\":[%u,%u],\"buffer_retained\":%s}\n",sensor,ok&&clean?(fn?"stream_complete":raw?"raw_exported":stream?"counter_advanced":"id_read"):"not_verified",phase,model[0],model[1],revision[0],revision[1],clean?"true":"false",stream_sent&&counter_changes>=2?"true":"false",counter_changes,stream_stop?"true":"false",
        receiver.exported?"true":"false",receiver.irq,receiver.error,receiver.frames,receiver.changed,receiver.link0&0x00fff07fU,receiver.link1&0x78U,
        receiver.link0,receiver.link1,controls.applied?"true":"false",controls.before[0],controls.before[1],controls.before[2],
        controls.after[0],controls.after[1],controls.after[2],controls.frame_lines,controls.line_pck,receiver.guard_bad,
        receiver.pixel_min,receiver.pixel_max,receiver.failed_region,receiver.failed_offset,receiver.failed_mask,receiver.failed_want,receiver.failed_actual,mode_width,mode_height,receiver.retained?"true":"false");
    return ok&&clean;
}

static bool cam_s20_gw2_live(redwood_s20_frame_fn fn,char *out,size_t cap){
    if(redwood_camera_s20_rear_is_warm())redwood_camera_s20_rear_cool();
    uint64_t args[3]={0,0,1};return cam_s20_aux_run(true,true,true,args,3,fn,true,out,cap);
}
static bool cam_s20_2la_live(redwood_s20_frame_fn fn,char *out,size_t cap){
    if(redwood_camera_s20_rear_is_warm())redwood_camera_s20_rear_cool();
    return cam_s20_aux_run(false,true,true,NULL,0,fn,true,out,cap);
}

static void cam_s20_2la_identify(__unused const uint64_t *v,unsigned n,char *out,size_t cap){cam_s20_aux_run(false,false,false,v,n,NULL,false,out,cap);}
static void cam_s20_gw2_identify(__unused const uint64_t *v,unsigned n,char *out,size_t cap){cam_s20_aux_run(true,false,false,v,n,NULL,false,out,cap);}

static void cam_s20_gw2_raw(__unused const uint64_t *v,unsigned n,char *out,size_t cap){cam_s20_aux_run(true,true,true,v,n,NULL,false,out,cap);}
static void cam_s20_gw2_stream(__unused const uint64_t *v,unsigned n,char *out,size_t cap){cam_s20_aux_run(true,true,false,v,n,NULL,false,out,cap);}

static void cam_s20_2la_stream(__unused const uint64_t *v,unsigned n,char *out,size_t cap){cam_s20_aux_run(false,true,false,v,n,NULL,false,out,cap);}

static void cam_s20_2la_raw(__unused const uint64_t *v,unsigned n,char *out,size_t cap){cam_s20_aux_run(false,true,true,v,n,NULL,false,out,cap);}

void
redwood_exynos_camera_s20_init(void)
{
	/* printf is not included in the phone's HTTP /log stream. Return snapshots
	 * over the existing root-only kern.redwood.poke interface instead. */
	redwood_dbgport_add("cam.s20.2la.raw", ": [lines [again-code]]: A202 native12MP RAW; default3000 lines/1x; stock bounds/readback, guarded DMA", cam_s20_2la_raw);
	redwood_dbgport_add("cam.s20.2la.stream", ": A202-only bounded native12MP sensor-counter test; no receiver/DMA; restores power and pins", cam_s20_2la_stream);
	redwood_dbgport_add("cam.s20.gw2.raw", ": [lines gain [full=1]] A300 guarded RAW10;2432x1824 or4864x3648 with mode/control readback", cam_s20_gw2_raw);
	redwood_dbgport_add("cam.s20.gw2.stream", ": cold/idle-only A300 TNP and bounded frame-counter test; no receiver or Camera exposure", cam_s20_gw2_stream);
	redwood_dbgport_add("cam.s20.gw2.id", ": cold/idle-only bounded GW2 identity read; both PMIC rail maps restored, no streaming", cam_s20_gw2_identify);
	redwood_dbgport_add("cam.s20.2la.id", ": cold/idle-only bounded 2LA identity read; rails/pins/clocks restored, no streaming", cam_s20_2la_identify);
	redwood_dbgport_add("cam.s20", ": read-only S20 camera pin inventory; no writes", cam_s20_inventory);
	redwood_dbgport_add("cam.s20.id", " [1]: one-shot rear S5K2LD model/revision read (1: then its tables, ~0.5 s of streaming read by its frame counter); rails and pins restored", cam_s20_identify);
	redwood_dbgport_add("cam.s20.front.id", ": one-shot front SAK3J1 model/revision read; camera rail enable bits and pins are restored", cam_s20_front_identify);
	redwood_dbgport_add("cam.s20.dma.test", ": [0|1|2 [sensor-mode]]: bounded DMA capture, 0 hardware pattern, 1 front sensor, 2 rear main (one 2016x1512 frame); refuses busy DMA", cam_s20_dma_pattern);
	redwood_dbgport_add("cam.s20.csis1", ": read-only CSIS1 and DMA1 version/status/counter snapshot", cam_s20_csis_inventory);
	redwood_dbgport_add("cam.s20.csis.power", ": bounded CSIS power-domain and clock inventory; no DPHY or sensor access", cam_s20_csis_power_inventory);
	redwood_dbgport_add("cam.s20.csis.dphy", ": idle read-only DPHY snapshot after enabling CSIS CMU/CSIS1 QCH; no sensor or DPHY writes", cam_s20_csis_dphy_snapshot);
	redwood_dbgport_add("cam.s20.csis.secure", ": EL3 CSIS DTZPC restore only, then read-only PHY reset sysreg check", cam_s20_csis_secure_probe);
	redwood_dbgport_add("cam.s20.csis.reset", ": EL3 restore then one reset-sysreg write/read/restore; no DPHY or sensor writes", cam_s20_csis_reset_probe);
	redwood_dbgport_add("cam.s20.csis.rails", ": read-only S2MPS22 LDO5S/LDO6S state required by Exynos990 CSIS PHY", cam_s20_csis_rails);
	redwood_dbgport_add("cam.s20.csis.dphy.config", ": bounded Exynos990 PHY 0x0503.0000 setup/readback/restore after rail check; no sensor streaming", cam_s20_csis_dphy_config_probe);
	redwood_dbgport_add("cam.s20.front.mode", ": apply Samsung SAK3J1 global and 1824x1368/30 non-PDAF setfiles, verify registers, power down", cam_s20_front_set_mode);
	redwood_dbgport_add("ustacks.media", ": every mediaserverd thread's stack into the kernel log", cam_ustacks_media_cmd);
	redwood_dbgport_add("cam.turn", " [N]: front camera orientation for libredwood-media (bit 0 turn 180, bit 1 mirror), from the next camera start", cam_s20_turn_cmd);
	redwood_dbgport_add("cam.s20.cached", " [0|1]: read stream frames through a cached mapping (faster); off at every boot", cam_s20_cached_cmd);
	redwood_dbgport_add("cam.s20.front.stream", ": bounded SAK3J1 to CSIS1 frame-counter test, 500 ms, no DMA; restores sensor/receiver state", cam_s20_front_stream_probe);
}
