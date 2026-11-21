#define _GNU_SOURCE
#define CAMERA_CAPTURE_NO_MAIN
#include "../tools/camera_capture_probe.c"

#include <math.h>
#include <pthread.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <stddef.h>
#include <sys/socket.h>
#include <sys/un.h>

#include "ipu6-channel.h"

/*
 * Output 1280x960 YUYV (4:3, the sensors' aspect). Colour is computed on a COL_WIDTH x
 * COL_HEIGHT grid (one 4x4 mosaic window per point, as the 640x480 output was); the output
 * takes its brightness detail from the green sites (half of all sites, a checkerboard on both
 * sensors) at full output resolution. See convert_bayer_yuyv.
 */
/*
 * 1920x1080 since 2026-11-20 (boot-18). Google's camera app creates a CameraX video use case, and
 * with no encoder profiles for these cameras CameraX only accepts its own quality sizes: SD
 * 720x480/640x480, HD 1280x720, FHD 1920x1080, QHD 2560x1440, UHD 3840x2160. With 1280x960 as the
 * only size no quality matched: "Required value was null" in the app's MainViewModel (boot-18),
 * the silent stall of boot-17. 640x480 (SD) worked on boot-15. Every quality of 1280 or wider is
 * 16:9, so the 4:3 sensors are cropped (sensor_crop). -DOUT_WIDTH/-DOUT_HEIGHT: the CAMERA_SIZE
 * builds (1280x720, 640x480).
 */
#ifndef OUT_WIDTH
#define OUT_WIDTH 1920
#define OUT_HEIGHT 1080
#endif
#define COL_WIDTH (OUT_WIDTH / 2)
#define COL_HEIGHT (OUT_HEIGHT / 2)
#define OUT_FOURCC V4L2_PIX_FMT_YUYV
#define OUT_FRAME_BYTES ((size_t)OUT_WIDTH * OUT_HEIGHT * 2)
#define BLACK_LEVEL 64
#define RAW_RANGE (1023U - BLACK_LEVEL)
/* Demosaic and white balance work on 12-bit linear values; gamma maps them to 8-bit sRGB. */
#define LINEAR_MAX 4095U
#define MAX_FRAME_BYTES (64U * 1024U * 1024U)

/* Auto-exposure: hold the mean green (raw units above black) near 18% grey. */
#define AE_TARGET (0.18 * RAW_RANGE)
#define AE_DEADBAND 0.15
#define AE_MAX_STEP 1.6
#define AE_INTERVAL 3	/* frames between updates; sensor settings lag about two frames */
/*
 * Fast phase when an app starts streaming (ae_wake): the idle state's exposure is often far off.
 * Boot-20: on every rear session the first frames were ~2x over the target (green mean 345 vs
 * 174 just before, at 11.6x gain), and with 1.6x steps every 3 frames the picture stayed blown out
 * for over a second (the raw dump 1 s in). For AE_FAST_FRAMES frames AE updates every
 * AE_FAST_INTERVAL frames (the settings' lag) with steps of up to AE_FAST_MAX_STEP.
 */
#define AE_FAST_FRAMES 12
#define AE_FAST_INTERVAL 2
#define AE_FAST_MAX_STEP 4.0
/*
 * A blown-out picture hides how far over it is (clipped sites stop at the sensor's maximum, so the
 * mean under-reports). In the fast phase, when more than AE_FAST_CLIP_FRACTION of the green samples
 * are clipped, exposure at least halves. Normal operation is unchanged (a lamp in the picture must
 * not darken the rest).
 */
#define AE_FAST_CLIP_FRACTION 0.05
static double ae_clip_fraction;	/* green samples at >= 95% of the clip level (last statistics) */
/*
 * Exposure program limits from the module's Windows tuning (OV5678_0BF501T3_TGL.aiqb, AE
 * exposure-program record 261): analogue gain at most 15.5x (also the top of its black-level
 * calibration; the driver allows 2047/128 = 16x), and in automatic mode exposure up to
 * 66.7 ms, i.e. the frame is stretched from 30 to 15 fps in low light. The 4x analogue gain
 * reached before stretching the frame is our choice (keeps 30 fps in ordinary indoor light).
 */
#define AE_AGAIN_MAX_X 15.5
#define AE_LONG_FRAME_X 2.0
#define AE_LONG_KNEE_X 4.0
/*
 * While no consumer reads a bridge, until its idle stop (IDLE_STOP_S_DEFAULT), its sensor
 * streams with a frame 8x the default (about 3.75 fps): two full-rate streams at once
 * overflowed the ISYS transfer FIFOs on both CSI ports (2026-11-18 boot-9). AE statistics
 * continue at the low rate; the planned frame comes back as soon as a consumer streams.
 */
#define AE_IDLE_FRAME_X 8.0

/*
 * Error-marked or short IPU6 frames (CSI FIFO overflow, sync errors) are requeued and skipped;
 * the last converted picture is repeated meanwhile. The bridge gives up (for the launcher to
 * restart it) only when no good frame arrived for BAD_FRAME_GIVE_UP_S while frames or
 * timeouts kept coming.
 */
#define BAD_FRAME_GIVE_UP_S 10.0

static volatile sig_atomic_t running = 1;

struct bridge_buffer {
	void *data;
	size_t length;
};

static void stop_bridge(int sig)
{
	(void)sig;
	running = 0;
}

/* Colour-filter sites in a 4x4 tile: 0 = R, 1 = G, 2 = B, 3 = IR. */
#define CFA_IR 3
static unsigned char cfa[4][4];

/*
 * Latitude 7320 front OV5675 (ACPI OVTI5678): an RGB-IR 4x4 mosaic, although the
 * driver reports MEDIA_BUS_FMT_SGRBG10_1X10. Measured on hardware 2026-11-17 from
 * per-phase means: IR sites ~21 vs G ~172; R 123 at (0,1),(2,3); B 100 at (2,1),(0,3).
 * R and B occupy 2 of 16 sites each, so demosaicing needs a 4x4 window (rgb_at).
 */
static const unsigned char ov5675_rgbir[4][4] = {
	{ 1, 3, 1, 3 },
	{ 0, 1, 2, 1 },
	{ 1, 3, 1, 3 },
	{ 2, 1, 0, 1 },
};

/* Per-site channel masks and the per-channel site count of any 4x4 window (rgb_linear_at). */
static uint32_t cfa_mask[4][4][4];
static unsigned int cfa_count[4];

static void cfa_changed(void)
{
	memset(cfa_count, 0, sizeof(cfa_count));
	for (int y = 0; y < 4; y++)
		for (int x = 0; x < 4; x++) {
			for (int c = 0; c < 4; c++)
				cfa_mask[y][x][c] = cfa[y][x] == c ? ~0u : 0;
			cfa_count[cfa[y][x]]++;
		}
}

static void set_rgbir_pattern(void)
{
	memcpy(cfa, ov5675_rgbir, sizeof(cfa));
	cfa_changed();
}

/* Plain 2x2 Bayer from the media bus code, expanded to the 4x4 table. */
static int set_bayer_pattern(__u32 code)
{
	static const struct { __u32 code; unsigned char cfa[2][2]; } patterns[] = {
		{ MEDIA_BUS_FMT_SGRBG10_1X10, { { 1, 0 }, { 2, 1 } } },
		{ MEDIA_BUS_FMT_SRGGB10_1X10, { { 0, 1 }, { 1, 2 } } },
		{ MEDIA_BUS_FMT_SBGGR10_1X10, { { 2, 1 }, { 1, 0 } } },
		{ MEDIA_BUS_FMT_SGBRG10_1X10, { { 1, 2 }, { 0, 1 } } },
	};

	for (size_t i = 0; i < sizeof(patterns) / sizeof(patterns[0]); i++)
		if (patterns[i].code == code) {
			for (int y = 0; y < 4; y++)
				for (int x = 0; x < 4; x++)
					cfa[y][x] = patterns[i].cfa[y & 1][x & 1];
			cfa_changed();
			return 0;
		}
	return -1;
}

/*
 * The Latitude 7320's two IPU6 sensors. Each runs its own bridge process (front on
 * /dev/video64, rear on /dev/video65). entity is the media-graph name prefix the kernel
 * driver registers; cal_prefix names the stick's calibration files (<prefix>_ccm.txt,
 * <prefix>_lsc.txt), extracted from that module's Windows tuning file.
 */
struct sensor_profile {
	const char *entity, *label, *cal_prefix;
	int rgbir;	/* 1: the measured RGB-IR 4x4 map; 0: plain Bayer from the bus code */
};

static const struct sensor_profile sensor_profiles[] = {
	/* Front: an RGB-IR OV5678 (module 0BF501T3) bound by the ov5675 driver. */
	{ "ov5675", "front OV5678 (ov5675 driver)", "ov5678", 1 },
	/* Rear: OV8856 (module 0BA801T3), GRBG10 Bayer at 3280x2464 (HANDOFF 2026-11-17). */
	{ "ov8856", "rear OV8856", "ov8856", 0 },
};

static const struct sensor_profile *find_sensor_profile(const char *entity)
{
	for (size_t i = 0; i < sizeof(sensor_profiles) / sizeof(sensor_profiles[0]); i++)
		if (!strcmp(sensor_profiles[i].entity, entity))
			return &sensor_profiles[i];
	return NULL;
}

/*
 * Pick the colour-filter layout for this sensor and its reported bus code. The RGB-IR
 * layout was measured relative to the orientation the driver reports as GRBG10; a flip
 * would move the sites, so any other code is refused there. Bayer sensors take the 2x2
 * order from the bus code (it follows the sensor's flip settings).
 */
static int select_cfa(const struct sensor_profile *p, __u32 code)
{
	if (p->rgbir) {
		if (code != MEDIA_BUS_FMT_SGRBG10_1X10) {
			fprintf(stderr, "unexpected %s bus code 0x%x (RGB-IR layout measured for GRBG10)\n",
				p->label, code);
			return -1;
		}
		set_rgbir_pattern();
		printf("cfa: %s RGB-IR 4x4 (bus code 0x%x): %s\n", p->label, code, "GIGI/RGBG/GIGI/BGRG");
		return 0;
	}
	if (set_bayer_pattern(code)) {
		fprintf(stderr, "unsupported %s bus code 0x%x (10-bit Bayer expected)\n", p->label, code);
		return -1;
	}
	printf("cfa: %s Bayer 2x2 from bus code 0x%x\n", p->label, code);
	return 0;
}

static int channel_at(unsigned int x, unsigned int y)
{
	return cfa[y & 3][x & 3];
}

static int cfa_has_ir_sites(void)
{
	for (int y = 0; y < 4; y++)
		for (int x = 0; x < 4; x++)
			if (cfa[y][x] == CFA_IR)
				return 1;
	return 0;
}

/* Raw code (10 bits, with black level) to linear 0..LINEAR_MAX. */
static uint16_t raw_lin_lut[1024];

static void init_raw_lut(void)
{
	static int ready;

	if (ready)
		return;
	for (unsigned int code = 0; code < 1024; code++) {
		unsigned int value = code > BLACK_LEVEL ? code - BLACK_LEVEL : 0;
		raw_lin_lut[code] = (uint16_t)((value * LINEAR_MAX + RAW_RANGE / 2) / RAW_RANGE);
	}
	ready = 1;
}

/*
 * The sensor area the picture shows: centred, with the output's aspect ratio (all of a 4:3
 * sensor for 4:3 output; a 16:9 band for 16:9 output). Set for each frame by sensor_crop().
 */
static unsigned int crop_x, crop_y, crop_w = 1, crop_h = 1;

static void sensor_crop(unsigned int width, unsigned int height)
{
	crop_x = crop_y = 0;
	crop_w = width;
	crop_h = height;
	if ((uint64_t)width * OUT_HEIGHT > (uint64_t)height * OUT_WIDTH) {
		crop_w = (unsigned int)((uint64_t)height * OUT_WIDTH / OUT_HEIGHT) & ~1u;
		crop_x = ((width - crop_w) / 2) & ~1u;
	} else if ((uint64_t)width * OUT_HEIGHT < (uint64_t)height * OUT_WIDTH) {
		crop_h = (unsigned int)((uint64_t)width * OUT_HEIGHT / OUT_WIDTH) & ~1u;
		crop_y = ((height - crop_h) / 2) & ~1u;
	}
}

static unsigned int raw_at(const uint16_t *raw, unsigned int width,
			   unsigned int height, unsigned int stride,
			   int x, int y)
{
	if (x < 0) x = 0;
	if (y < 0) y = 0;
	if ((unsigned int)x >= width) x = (int)width - 1;
	if ((unsigned int)y >= height) y = (int)height - 1;
	init_raw_lut();
	return raw_lin_lut[raw[(size_t)y * (stride / 2) + (unsigned int)x] & 1023];
}

/*
 * Infrared on the RGB-IR sensor. An IR site reads near-infrared light (NIR) plus a little
 * visible light: under the module tuning file's calibration lights, which carry almost no NIR,
 * it reads 0.096-0.105 of green (the per-light ratios stored with the tuning file's RGB-IR
 * record 27); boot-11's LED room gave 0.103-0.106. Subtracting the IR site from every colour
 * site (what the bridge did) is right for that visible share, and it is what the calibrated
 * white points assume: boot-5/11 under LED light sit on the locus.
 *
 * Daylight adds real NIR (IR site 0.25-0.35 of green on boot-8 and boot-12), and the blue sites
 * respond to it less than the IR site does: after a full subtraction every daylight frame of
 * boot-8 and boot-12 (25 samples) sat 0.07-0.15 off the locus on the low-blue side, always
 * with raw R = raw B, while the rear camera in the same room sat on its own locus (0.02-0.04)
 * at about 6000 K. Fitting a blue NIR response k_B (red and green at 1) to those samples, with
 * lens-shading-corrected statistics as below, gives a median locus distance of 0.033-0.042 for
 * k_B = 0.5-0.6 (0.135 with 1.0); at 0.60 boot-12's daylight frames sit 0.001-0.022 from the
 * locus, boot-8's 0.03-0.08 and boot-11's LED frames stay at 0.005-0.017. This is our fit under
 * the grey-world assumption, not a value from the tuning file (its RGB-IR record 27 holds
 * per-light G/R/B grids whose meaning and scale are not confirmed; see HANDOFF 2026-11-19).
 *
 *   NIR n = max(0, IR x (1 + v) - v x G),  v = ir_visible_q12 / 4096   (window or frame means)
 *   colour = C - IR + (1 - k_c) x n,        nir_keep_q12[c] = (1 - k_c) x 4096
 *
 * With no NIR (n = 0) this is the old full subtraction, so LED and lamp light are unchanged.
 * Plain Bayer has no IR sites: IR = 0, n = 0.
 */
static int ir_visible_q12 = 410;		/* 0.10 */
/*
 * 2026-11-20 (boot-15): k_B = 0.75, not 0.60. The 0.60 fit assumed daylight sits on the locus.
 * Boot-14 showed real lights 0.10-0.14 off it on the green side, and at 0.60 the objects that
 * reflect much NIR came out too blue in boot-15's window light: a dark green T-shirt teal, a
 * dark teal wall navy (host simulation: a true k_B of 0.75 under the 0.60 model gives exactly
 * that teal). At 0.75 boot-12's daylight frames sit on the locus (0.00-0.03; at 0.60 they were
 * 0.04 on the magenta side), boot-8's 0.07 on the green side, within the green-side zone.
 */
static int nir_keep_q12[3] = { 0, 0, 1024 };	/* R, G: k = 1; B: k = 0.75 */


static int nir_estimate(int ir, int green)
{
	int n = (int)(((int64_t)ir * (4096 + ir_visible_q12) - (int64_t)green * ir_visible_q12 + 2048) >> 12);

	return n > 0 ? n : 0;
}

static double nir_estimate_f(double ir, double green)
{
	double v = ir_visible_q12 / 4096.0, n = ir * (1 + v) - v * green;

	return n > 0 ? n : 0;
}

/*
 * Sensor saturation. The highlight fade (rgb_linear_at) must reach white where a site clips;
 * if the sensor saturates below code 1023, a fade keyed to full scale stops short and the
 * clipped colour (green clips first: WB then lifts R and B above it and the matrix turns that
 * magenta) shows: the pink sky of boot-12. Each frame's white-balance sampling looks for a
 * flat top among the green sites in the top quarter of the range: at least 0.1% of them (and
 * 8) within 2 codes of the maximum mean the sensor clipped there. Otherwise full scale.
 */
#define CLIP_MIN_RAW 768
static int clip_detect = 1;			/* 0: assume full scale (host-test negative control) */
static unsigned int raw_clip = 1023;		/* raw code (with black level) where sites clip */
static unsigned int highlight_start_lin, highlight_full_lin;

static unsigned int raw_to_linear(unsigned int raw)
{
	unsigned int value = raw > BLACK_LEVEL ? raw - BLACK_LEVEL : 0;
	return (value * LINEAR_MAX + RAW_RANGE / 2) / RAW_RANGE;
}

/* Fade from 80% to 97% of the clip level (in linear units). */
static void set_highlight_range(unsigned int clip)
{
	unsigned int lin = raw_to_linear(clip);

	raw_clip = clip;
	highlight_start_lin = lin * 80 / 100;
	highlight_full_lin = lin * 97 / 100;
}

static unsigned char gamma_lut[LINEAR_MAX + 1];

/* sRGB transfer curve: linear 0..LINEAR_MAX to display-encoded 0..255. */
static void init_gamma(void)
{
	static int ready;

	if (ready)
		return;
	for (unsigned int i = 0; i <= LINEAR_MAX; i++) {
		double x = (double)i / LINEAR_MAX;
		double v = x <= 0.0031308 ? 12.92 * x : 1.055 * pow(x, 1.0 / 2.4) - 0.055;
		gamma_lut[i] = (unsigned char)(v * 255.0 + 0.5);
	}
	ready = 1;
}

/* Generic colour boost when no calibrated matrix is loaded: 1.6x in linear light. */
static int saturation_q12 = 6554;
static double wb_last_mean[3];
static unsigned int wb_last_gain_q12[3];

/*
 * Calibrated colour matrices, one per illuminant, from the camera module's Windows
 * tuning file (tools/extract_aiqb_ccm.py; proprietary, so loaded from the stick at run
 * time and never part of this repository). Each entry has the sensor's raw white point
 * (R/G, B/G after IR removal) under that illuminant.
 */
#define CCM_MAX 16
struct ccm_entry {
	int id;
	double rg, bg, m[9];
};
static struct ccm_entry ccm_table[CCM_MAX];
static int ccm_count, ccm_active, ccm_q12[9], ccm_pick[2];
static double ccm_weight;

/* Returns the number of illuminants loaded (0 if the file is missing or malformed). */
static int load_ccm_table(const char *path)
{
	FILE *f = fopen(path, "r");
	char line[512];

	ccm_count = 0;
	if (!f)
		return 0;
	while (ccm_count < CCM_MAX && fgets(line, sizeof(line), f)) {
		struct ccm_entry e;
		double x, y;
		if (line[0] == '#')
			continue;
		if (sscanf(line, "%d %lf %lf %lf %lf %lf %lf %lf %lf %lf %lf %lf %lf %lf", &e.id, &e.rg,
			   &e.bg, &x, &y, &e.m[0], &e.m[1], &e.m[2], &e.m[3], &e.m[4], &e.m[5], &e.m[6],
			   &e.m[7], &e.m[8]) != 14 || e.rg <= 0 || e.bg <= 0)
			continue;
		int ok = 1;
		for (int r = 0; r < 3; r++)
			if (fabs(e.m[r * 3] + e.m[r * 3 + 1] + e.m[r * 3 + 2] - 1.0) > 0.1)
				ok = 0;
		if (ok)
			ccm_table[ccm_count++] = e;
	}
	fclose(f);
	return ccm_count;
}

/*
 * Blend the matrices of the two calibrated illuminants whose white points lie nearest
 * the measured one (inverse-distance weights), as an ISP interpolates between them.
 */
static void select_ccm(double rg, double bg)
{
	int a = -1, b = -1;
	double da = 1e9, db = 1e9;

	ccm_active = 0;
	if (ccm_count == 0 || rg <= 0 || bg <= 0)
		return;
	for (int i = 0; i < ccm_count; i++) {
		double d = hypot(ccm_table[i].rg - rg, ccm_table[i].bg - bg);
		if (d < da) { b = a; db = da; a = i; da = d; }
		else if (d < db) { b = i; db = d; }
	}
	double wa = (b < 0 || da < 1e-9) ? 1.0 : db / (da + db);
	for (int k = 0; k < 9; k++) {
		double v = wa * ccm_table[a].m[k] + (b < 0 ? 0 : (1 - wa) * ccm_table[b].m[k]);
		ccm_q12[k] = (int)lround(v * 4096.0);
	}
	ccm_pick[0] = ccm_table[a].id;
	ccm_pick[1] = b < 0 ? ccm_table[a].id : ccm_table[b].id;
	ccm_weight = wa;
	ccm_active = 1;
}

/*
 * White-balance locus: the calibrated illuminants' white points, ordered warm to cool,
 * form a polyline in (R/G, B/G). A measured grey-world point is projected onto it and
 * may stray at most WB_LOCUS_BAND from it (real lights sit near the locus; a scene
 * dominated by one colour pulls grey-world far off it). Without a table: unchanged.
 */
#define WB_LOCUS_BAND 0.03
#define WB_MIN_SIGNAL 4.0	/* mean green (raw units above black) below which WB keeps its last state */
/*
 * The calibration lights carry almost no near infrared, so the locus is less certain in light
 * that does (daylight): the band widens by WB_LOCUS_NIR_BAND per unit of NIR relative to green
 * (wb_nir_ratio, see the infrared model below), up to WB_LOCUS_BAND_MAX. Boot-12 daylight has
 * a ratio of about 0.3-0.45 (band about 0.06-0.075); boot-11's LED room about 0.02 (0.032).
 */
static double wb_locus_nir_band = 0.10;
#define WB_LOCUS_BAND_MAX 0.08
static double wb_white_rg = 1.0, wb_white_bg = 1.0;
static double wb_locus_distance, wb_locus_band = WB_LOCUS_BAND, wb_nir_ratio;

/*
 * Green side. Many real lights sit off the locus on its green side (both R/G and B/G lower than
 * the calibrated light of the same colour temperature): fluorescent tubes, CFLs and many LEDs.
 * Boot-14 (2026-11-19, evening room light, white tiled wall filling the frame, NIR 0.00-0.02):
 * the front's grey-world sat 0.10-0.14 off the locus on that side (the rear, in the same room,
 * 0.04-0.05 on the same side), and the 0.03 band pulled it back: the wall came out green/cyan
 * (R gain about 8%, B gain about 15-20% short). So on the front the white point is measured
 * only on regions whose colour a light could have (within WB_LOCUS_ACCEPT_MAGENTA x band on
 * the magenta side, wb_locus_green_band on the green side), and when enough of the frame
 * qualifies that measurement is trusted up to wb_locus_green_band. A saturated green scene
 * (foliage) lies outside the zone; if too little of the frame qualifies, grey-world with the
 * plain band is used as before. The rear is unchanged (its colour is reported good).
 */
static double wb_locus_green_band = 0.18;	/* 0: the plain band (host-test negative control) */
#define WB_LOCUS_ACCEPT_MAGENTA 2.0
#define WB_REGIONS_X 16
#define WB_REGIONS_Y 12
#define WB_REGIONS_MIN_FRACTION 0.25	/* of the regions with signal */
/*
 * 2026-11-21 (calibration shot, boot-23 vs Windows): the white point is taken from the brightest
 * of the regions a light could have lit, as white surfaces (paper, walls, a flag's white) are the
 * brightest things in most rooms: each accepted region weighs (its green / the brightest
 * accepted region's green) ^ WB_BRIGHT_POWER. With every region weighing its brightness once
 * (the old rule) a large saturated object nearby dragged the white point: a red flag in the sun
 * made the white shield light blue (white point R/G 0.63 for a white at 0.46). Regions are 16x12
 * (were 8x6) so a white patch is less often averaged together with its surroundings.
 */
static double wb_bright_power = 3.0;	/* 0: plain average (host-test negative control) */
static double wb_locus_side;		/* > 0 green side, < 0 magenta side (last measurement) */
static int wb_regions_used, wb_regions_valid;

/*
 * Nearest point of the locus polyline (calibrated white points, warm to cool). Returns the
 * distance, signed: > 0 on the green side (towards lower R/G and B/G), < 0 on the magenta side.
 * Needs ccm_count >= 2.
 */
static double locus_nearest(double rg, double bg, double *px_out, double *py_out)
{
	int order[CCM_MAX];
	double best_d = 1e9, best_x = rg, best_y = bg, best_side = 1;

	for (int i = 0; i < ccm_count; i++)
		order[i] = i;
	for (int i = 1; i < ccm_count; i++)	/* insertion sort by R/G, warm (high) first */
		for (int k = i; k > 0 && ccm_table[order[k]].rg > ccm_table[order[k - 1]].rg; k--) {
			int t = order[k]; order[k] = order[k - 1]; order[k - 1] = t;
		}
	for (int i = 0; i + 1 < ccm_count; i++) {
		double ax = ccm_table[order[i]].rg, ay = ccm_table[order[i]].bg;
		double bx = ccm_table[order[i + 1]].rg, by = ccm_table[order[i + 1]].bg;
		double dx = bx - ax, dy = by - ay, len2 = dx * dx + dy * dy;
		double t = len2 > 0 ? ((rg - ax) * dx + (bg - ay) * dy) / len2 : 0;
		if (t < 0) t = 0;
		if (t > 1) t = 1;
		double px = ax + t * dx, py = ay + t * dy, d = hypot(rg - px, bg - py);
		if (d < best_d) {
			/* Warm-to-cool runs towards lower R/G, higher B/G: the origin side is green. */
			double cross = dx * (bg - ay) - dy * (rg - ax);
			best_d = d; best_x = px; best_y = py;
			best_side = cross >= 0 ? 1 : -1;
		}
	}
	*px_out = best_x;
	*py_out = best_y;
	return best_side * best_d;
}

/* The band for this frame: WB_LOCUS_BAND, widened with NIR (see above). */
static void update_locus_band(void)
{
	wb_locus_band = WB_LOCUS_BAND + wb_locus_nir_band * wb_nir_ratio;
	if (wb_locus_band > WB_LOCUS_BAND_MAX)
		wb_locus_band = WB_LOCUS_BAND_MAX;
}

/* Pull (rg, bg) to within the band of the locus; green_band (if larger) on the green side. */
static void constrain_white_point_sided(double *rg, double *bg, double green_band)
{
	double px, py;

	wb_locus_distance = 0;
	wb_locus_side = 0;
	update_locus_band();
	if (ccm_count < 2)
		return;
	double side = locus_nearest(*rg, *bg, &px, &py), d = fabs(side);
	double band = side > 0 && green_band > wb_locus_band ? green_band : wb_locus_band;
	wb_locus_distance = d;
	wb_locus_side = side;
	if (d > band) {
		double k = band / d;
		*rg = px + (*rg - px) * k;
		*bg = py + (*bg - py) * k;
	}
}

#ifdef CAMERA_BRIDGE_TEST
/* The plain band on both sides (the rear, and the front's grey-world fallback). */
static void constrain_white_point(double *rg, double *bg)
{
	constrain_white_point_sided(rg, bg, 0);
}
#endif

/* Could a light have this colour? (Inside the zone around the locus described above.) */
static int locus_accepts(double rg, double bg)
{
	double px, py, side = locus_nearest(rg, bg, &px, &py);

	return side >= 0 ? side <= fmax(wb_locus_green_band, wb_locus_band)
			 : -side <= WB_LOCUS_ACCEPT_MAGENTA * wb_locus_band;
}

/*
 * Lens-shading gain grids, one per illuminant (tools/extract_aiqb_ccm.py --lsc; stick only,
 * like the matrices). Each grid spans the full sensor and is normalised to 1.0 at the centre.
 * The Latitude's OV5678 needs about 3.5-4.3x at the corners. A full correction would also
 * amplify the corners' noise that much, so as camera ISPs do, the strength eases off at high
 * sensor gain: full up to 8x total gain, falling to 60% at 64x.
 */
#define LSC_MAX_ILL 8
#define LSC_MAX_GRID (128 * 128)
struct lsc_entry {
	int id, gw, gh;
	double rg, bg;
	float g[3][LSC_MAX_GRID];
};
static struct lsc_entry lsc_table[LSC_MAX_ILL];
static int lsc_count, lsc_active, lsc_pick[2] = { -1, -1 };
static double lsc_weight, lsc_strength = 1.0, sensor_total_gain = 1.0;
static uint16_t lsc_map[3][COL_HEIGHT][COL_WIDTH];	/* per colour-grid point */
static struct { int pick[2]; double weight, strength; unsigned int w, h, crop_x, crop_y; } lsc_built = { { -2, -2 }, 0, 0, 0, 0, 0, 0 };
static unsigned int lsc_generation;	/* counts lsc_map rebuilds (the GPU copy follows it) */

static int load_lsc_table(const char *path)
{
	FILE *f = fopen(path, "r");
	char word[16];

	lsc_count = 0;
	if (!f)
		return 0;
	while (lsc_count < LSC_MAX_ILL && fscanf(f, "%15s", word) == 1) {
		if (word[0] == '#') {
			int c;
			while ((c = fgetc(f)) != EOF && c != '\n')
				;
			continue;
		}
		struct lsc_entry *e = &lsc_table[lsc_count];
		if (strcmp(word, "lsc") || fscanf(f, "%d %lf %lf %d %d", &e->id, &e->rg, &e->bg, &e->gw, &e->gh) != 5 ||
		    e->gw < 2 || e->gh < 2 || e->gw * e->gh > LSC_MAX_GRID || e->rg <= 0 || e->bg <= 0)
			break;
		int ok = 1;
		for (int c = 0; c < 3 && ok; c++)
			for (int k = 0; k < e->gw * e->gh; k++)
				if (fscanf(f, "%f", &e->g[c][k]) != 1 || !(e->g[c][k] >= 0.25f && e->g[c][k] <= 16.0f)) {
					ok = 0;
					break;
				}
		if (!ok)
			break;
		lsc_count++;
	}
	fclose(f);
	return lsc_count;
}

static double lsc_strength_for_gain(double total_gain)
{
	if (total_gain <= 8.0)
		return 1.0;
	double s = 1.0 - 0.4 * log2(total_gain / 8.0) / 3.0;
	return s < 0.6 ? 0.6 : s;
}

static double lsc_grid_at(const struct lsc_entry *e, int c, double gx, double gy)
{
	int x0 = (int)gx, y0 = (int)gy;
	if (x0 > e->gw - 2) x0 = e->gw - 2;
	if (y0 > e->gh - 2) y0 = e->gh - 2;
	double fx = gx - x0, fy = gy - y0;
	const float *g = e->g[c];
	double top = g[y0 * e->gw + x0] * (1 - fx) + g[y0 * e->gw + x0 + 1] * fx;
	double bot = g[(y0 + 1) * e->gw + x0] * (1 - fx) + g[(y0 + 1) * e->gw + x0 + 1] * fx;
	return top * (1 - fy) + bot * fy;
}

/* Pick the grids for the measured white point (as for the matrices); rebuild the map on change. */
static void update_lsc(double rg, double bg, unsigned int width, unsigned int height)
{
	int a = -1, b = -1;
	double da = 1e9, db = 1e9;

	lsc_active = 0;
	if (lsc_count == 0 || rg <= 0 || bg <= 0)
		return;
	for (int i = 0; i < lsc_count; i++) {
		double d = hypot(lsc_table[i].rg - rg, lsc_table[i].bg - bg);
		if (d < da) { b = a; db = da; a = i; da = d; }
		else if (d < db) { b = i; db = d; }
	}
	if (b < 0 || lsc_table[b].gw != lsc_table[a].gw || lsc_table[b].gh != lsc_table[a].gh)
		b = a;
	double wa = (b == a || da < 1e-9) ? 1.0 : db / (da + db);
	lsc_pick[0] = lsc_table[a].id; lsc_pick[1] = lsc_table[b].id;
	lsc_weight = wa;
	lsc_strength = lsc_strength_for_gain(sensor_total_gain);
	lsc_active = 1;
	if (lsc_built.pick[0] == a && lsc_built.pick[1] == b && fabs(lsc_built.weight - wa) < 0.05 &&
	    lsc_built.crop_y == crop_y && lsc_built.crop_x == crop_x &&
	    fabs(lsc_built.strength - lsc_strength) < 0.05 && lsc_built.w == width && lsc_built.h == height)
		return;
	const struct lsc_entry *ea = &lsc_table[a], *eb = &lsc_table[b];
	for (int oy = 0; oy < COL_HEIGHT; oy++) {
		double sy = (crop_y + (oy * (double)crop_h) / COL_HEIGHT + 1.0) / height;	/* window centre, 0..1 */
		double gy = sy * (ea->gh - 1);
		for (int ox = 0; ox < COL_WIDTH; ox++) {
			double sx = (crop_x + (ox * (double)crop_w) / COL_WIDTH + 1.0) / width;
			double gx = sx * (ea->gw - 1);
			for (int c = 0; c < 3; c++) {
				double g = wa * lsc_grid_at(ea, c, gx, gy) + (1 - wa) * lsc_grid_at(eb, c, gx, gy);
				g = pow(g, lsc_strength) * 4096.0 + 0.5;
				lsc_map[c][oy][ox] = (uint16_t)(g > 65535 ? 65535 : g);
			}
		}
	}
	lsc_built.pick[0] = a; lsc_built.pick[1] = b;
	lsc_built.weight = wa; lsc_built.strength = lsc_strength;
	lsc_built.w = width; lsc_built.h = height;
	lsc_built.crop_x = crop_x; lsc_built.crop_y = crop_y;
	lsc_generation++;
}

static void calculate_white_balance(const uint16_t *raw, unsigned int width,
				    unsigned int height, unsigned int stride,
				    unsigned int gain_q12[3], double *mean_green)
{
	uint64_t count[4] = { 0 }, all_sum = 0, all_count = 0;
	double sum[4] = { 0 };
	static double rsum[WB_REGIONS_Y][WB_REGIONS_X][4];
	static uint64_t rcount[WB_REGIONS_Y][WB_REGIONS_X][4];

	int shaded = lsc_active && lsc_built.w == width && lsc_built.h == height && cfa_has_ir_sites();
	static unsigned int ghist[1024];
	unsigned int gmax = 0, gnear = 0, gcount = 0;
	double mean[4];

	sensor_crop(width, height);

	/* Sensor clip level: a flat top among this frame's sampled green sites (see raw_clip). */
	memset(ghist, 0, sizeof(ghist));
	for (unsigned int y = 0; y < height; y += 16)
		for (unsigned int x = 0; x < width; x += 16)
			for (unsigned int dy = 0; dy < 4 && y + dy < height; dy++)
				for (unsigned int dx = 0; dx < 4 && x + dx < width; dx++)
					if (channel_at(x + dx, y + dy) == 1) {
						ghist[raw[(size_t)(y + dy) * (stride / 2) + x + dx] & 1023]++;
						gcount++;
					}
	for (gmax = 1023; gmax > 0 && !ghist[gmax]; gmax--)
		;
	for (unsigned int v = gmax >= 2 ? gmax - 2 : 0; v <= gmax; v++)
		gnear += ghist[v];
	set_highlight_range(clip_detect && gmax >= CLIP_MIN_RAW && gnear >= 8 && gnear * 1000 >= gcount ?
			    gmax : 1023);

	/*
	 * Grey-world over 4x4 tiles every 16 pixels. Tiles with a site near the clip level carry
	 * no colour (one clipped channel) and are left out, as ISPs leave saturated patches out
	 * of their white-balance statistics; the AE mean still counts them. With lens shading
	 * active each tile takes the shading gains of its position (IR sites the green gain), so
	 * the white point is measured in the same units as the picture: on the raw frame the
	 * corners, where blue falls off less than green, raised B/G by ~5%, which left the centre
	 * of a grey wall slightly yellow-green (host simulation, boot-11 and boot-12 levels: U -4).
	 * Front (RGB-IR) only: the rear's grey-world sat on its locus without it (boot-12: 0.02-0.04)
	 * and its colour is reported good, so it is left as it was (see HANDOFF 2026-11-19).
	 */
	unsigned int tile_limit = raw_clip - (raw_clip - BLACK_LEVEL) / 20;	/* 95% of the range */
	unsigned int clipped = 0;
	for (unsigned int v = tile_limit; v < 1024; v++)
		clipped += ghist[v];
	ae_clip_fraction = gcount ? (double)clipped / gcount : 0;
	memset(rsum, 0, sizeof(rsum));
	memset(rcount, 0, sizeof(rcount));
	/* Statistics (white balance, AE) over what the picture shows: the crop. */
	for (unsigned int y = crop_y; y < crop_y + crop_h; y += 16) {
		for (unsigned int x = crop_x; x < crop_x + crop_w; x += 16) {
			uint64_t tsum[4] = { 0 }, tcount[4] = { 0 };
			unsigned int tpeak = 0;
			for (unsigned int dy = 0; dy < 4 && y + dy < height; dy++) {
				for (unsigned int dx = 0; dx < 4 && x + dx < width; dx++) {
					int channel = channel_at(x + dx, y + dy);
					unsigned int value =
						raw[(size_t)(y + dy) * (stride / 2) + x + dx] & 1023;
					if (value > tpeak)
						tpeak = value;
					if (value > BLACK_LEVEL)
						tsum[channel] += value - BLACK_LEVEL;
					tcount[channel]++;
				}
			}
			all_sum += tsum[1];
			all_count += tcount[1];
			if (tpeak >= tile_limit)
				continue;
			double shade[4] = { 1, 1, 1, 1 };
			if (shaded) {
				unsigned int ox = (unsigned int)(((uint64_t)(x - crop_x) * COL_WIDTH) / crop_w);
				unsigned int oy = (unsigned int)(((uint64_t)(y - crop_y) * COL_HEIGHT) / crop_h);
				shade[0] = lsc_map[0][oy][ox] / 4096.0;
				shade[1] = shade[CFA_IR] = lsc_map[1][oy][ox] / 4096.0;
				shade[2] = lsc_map[2][oy][ox] / 4096.0;
			}
			unsigned int rx = (unsigned int)((uint64_t)(x - crop_x) * WB_REGIONS_X / crop_w);
			unsigned int ry = (unsigned int)((uint64_t)(y - crop_y) * WB_REGIONS_Y / crop_h);
			for (int channel = 0; channel < 4; channel++) {
				sum[channel] += tsum[channel] * shade[channel];
				count[channel] += tcount[channel];
				rsum[ry][rx][channel] += tsum[channel] * shade[channel];
				rcount[ry][rx][channel] += tcount[channel];
			}
		}
	}

	for (int channel = 0; channel < 4; channel++)
		mean[channel] = count[channel] ? sum[channel] / count[channel] : 0;
	/* AE meters raw green over every tile (IR included: light the sensor really collects). */
	if (mean_green)
		*mean_green = all_count ? (double)all_sum / all_count : 0;
	/* Balance the colour after removing the IR that leaks into R, G and B (model above). */
	double nir = nir_estimate_f(mean[CFA_IR], mean[1]);
	for (int channel = 0; channel < 3; channel++) {
		mean[channel] += -mean[CFA_IR] + nir * nir_keep_q12[channel] / 4096.0;
		if (mean[channel] < 0)
			mean[channel] = 0;
	}
	wb_nir_ratio = mean[1] > WB_MIN_SIGNAL ? nir / mean[1] : 0;
	gain_q12[0] = gain_q12[2] = 4096;
	memcpy(wb_last_mean, mean, sizeof(wb_last_mean));
	/*
	 * A near-black frame (stream start, lens covered) carries no colour information:
	 * one with green at 0 once snapped the white point to the warmest end of the locus
	 * (distance 18 on boot-8). Keep the previous white point and gains for such frames.
	 */
	if (mean[1] < WB_MIN_SIGNAL || mean[0] <= 0 || mean[2] <= 0) {
		gain_q12[0] = (unsigned int)(fmin(4.0, fmax(0.25, 1.0 / wb_white_rg)) * 4096.0 + 0.5);
		gain_q12[2] = (unsigned int)(fmin(4.0, fmax(0.25, 1.0 / wb_white_bg)) * 4096.0 + 0.5);
	} else {
		/*
		 * Grey-world white point (IR-free raw R/G, B/G, the units of the calibrated
		 * white points), constrained to the module's illuminant locus when the
		 * calibration table is loaded: a yellow wall or green room no longer
		 * becomes a colour cast.
		 */
		double rg = mean[0] / mean[1], bg = mean[2] / mean[1];
		double green_band = 0;

		/* Front: measure on the regions a light could have lit (see wb_locus_green_band). */
		wb_regions_used = wb_regions_valid = 0;
		if (cfa_has_ir_sites() && ccm_count >= 2 && wb_locus_green_band > 0) {
			double acc[3] = { 0 }, gmax = 0;
			static double accepted[WB_REGIONS_Y * WB_REGIONS_X][3];
			int na = 0;
			update_locus_band();
			for (int ry = 0; ry < WB_REGIONS_Y; ry++)
				for (int rx = 0; rx < WB_REGIONS_X; rx++) {
					double m[4];
					for (int c = 0; c < 4; c++)
						m[c] = rcount[ry][rx][c] ? rsum[ry][rx][c] / rcount[ry][rx][c] : 0;
					double n = nir_estimate_f(m[CFA_IR], m[1]);
					for (int c = 0; c < 3; c++)
						m[c] = fmax(0, m[c] - m[CFA_IR] + n * nir_keep_q12[c] / 4096.0);
					if (m[1] < 4 * WB_MIN_SIGNAL || m[0] <= 0 || m[2] <= 0)
						continue;
					wb_regions_valid++;
					if (!locus_accepts(m[0] / m[1], m[2] / m[1]))
						continue;
					wb_regions_used++;
					memcpy(accepted[na++], m, sizeof(accepted[0]));
					if (m[1] > gmax)
						gmax = m[1];
				}
			for (int i = 0; i < na; i++) {	/* the brightest regions weigh most */
				double w = wb_bright_power > 0 ? pow(accepted[i][1] / gmax, wb_bright_power) : 1.0;
				for (int c = 0; c < 3; c++)
					acc[c] += w * accepted[i][c];
			}
			if (wb_regions_used > 0 &&
			    wb_regions_used >= WB_REGIONS_MIN_FRACTION * wb_regions_valid) {
				rg = acc[0] / acc[1];
				bg = acc[2] / acc[1];
				green_band = wb_locus_green_band;
			}
		}
		constrain_white_point_sided(&rg, &bg, green_band);
		wb_white_rg = rg;
		wb_white_bg = bg;
		double gain = 1.0 / rg;
		if (gain < 0.25) gain = 0.25;
		if (gain > 4.0) gain = 4.0;
		gain_q12[0] = (unsigned int)(gain * 4096.0 + 0.5);
		gain = 1.0 / bg;
		if (gain < 0.25) gain = 0.25;
		if (gain > 4.0) gain = 4.0;
		gain_q12[2] = (unsigned int)(gain * 4096.0 + 0.5);
		select_ccm(rg, bg);
	}
	memcpy(wb_last_gain_q12, gain_q12, sizeof(wb_last_gain_q12));
}

/*
 * Diagnostic: mean level above black at each of the 16 positions of a 4x4 tile.
 * Plain Bayer repeats every 2 pixels; an RGB-IR mosaic repeats every 4 and has
 * dim IR sites. Logged to the runtime file so the real layout can be read off.
 */
static void cfa_phase_means(const uint16_t *raw, unsigned int width,
			    unsigned int height, unsigned int stride, double means[4][4])
{
	uint64_t sum[4][4] = { { 0 } }, count[4][4] = { { 0 } };

	for (unsigned int y = 0; y + 4 <= height; y += 16)
		for (unsigned int x = 0; x + 4 <= width; x += 16)
			for (unsigned int dy = 0; dy < 4; dy++)
				for (unsigned int dx = 0; dx < 4; dx++) {
					unsigned int value =
						raw[(size_t)(y + dy) * (stride / 2) + x + dx] & 1023;
					sum[dy][dx] += value > BLACK_LEVEL ? value - BLACK_LEVEL : 0;
					count[dy][dx]++;
				}
	for (int dy = 0; dy < 4; dy++)
		for (int dx = 0; dx < 4; dx++)
			means[dy][dx] = count[dy][dx] ? (double)sum[dy][dx] / count[dy][dx] : 0;
}

static void log_cfa_phases(const uint16_t *raw, unsigned int width,
			   unsigned int height, unsigned int stride, unsigned int frame)
{
	double m[4][4];

	cfa_phase_means(raw, width, height, stride, m);
	printf("cfa4x4 frame=%u (mean above black; rows y%%4, cols x%%4):\n", frame);
	for (int dy = 0; dy < 4; dy++)
		printf("cfa4x4 y%d: %6.1f %6.1f %6.1f %6.1f\n", dy, m[dy][0], m[dy][1], m[dy][2], m[dy][3]);
	printf("wb: mean R=%.1f G=%.1f B=%.1f gain R=%.2f B=%.2f%s\n",
	       wb_last_mean[0], wb_last_mean[1], wb_last_mean[2],
	       wb_last_gain_q12[0] / 4096.0, wb_last_gain_q12[2] / 4096.0,
	       wb_last_gain_q12[0] >= 16384 || wb_last_gain_q12[2] >= 16384 ||
	       wb_last_gain_q12[0] <= 1024 || wb_last_gain_q12[2] <= 1024 ? " (clamped)" : "");
	if (ccm_active)
		printf("ccm: illuminant %d (%.0f%%) + %d, grey-world R/G=%.3f B/G=%.3f, used %.3f/%.3f (locus distance %.3f %s, band %.3f",
		       ccm_pick[0], ccm_weight * 100.0, ccm_pick[1],
		       wb_last_mean[1] > 0 ? wb_last_mean[0] / wb_last_mean[1] : 0.0,
		       wb_last_mean[1] > 0 ? wb_last_mean[2] / wb_last_mean[1] : 0.0,
		       wb_white_rg, wb_white_bg, wb_locus_distance,
		       wb_locus_side > 0 ? "green side" : wb_locus_side < 0 ? "magenta side" : "on it",
		       wb_locus_band);
	if (ccm_active && wb_regions_valid)
		printf(", %d of %d regions lit like a light%s)\n", wb_regions_used, wb_regions_valid,
		       wb_regions_used >= WB_REGIONS_MIN_FRACTION * wb_regions_valid && wb_regions_used ?
		       ", green side allowed" : "; grey-world");
	else if (ccm_active)
		printf(")\n");
	else
		printf("ccm: no calibration table; generic saturation %.2f\n", saturation_q12 / 4096.0);
	if (cfa_has_ir_sites())
		printf("ir: NIR %.2f of green (IR site minus %.2f x green), blue NIR response %.2f; clip at raw %u\n",
		       wb_nir_ratio, ir_visible_q12 / 4096.0, 1.0 - nir_keep_q12[2] / 4096.0, raw_clip);
	else
		printf("clip: raw %u\n", raw_clip);
	if (lsc_active)
		printf("lsc: illuminant %d (%.0f%%) + %d, strength %.2f at sensor gain %.1fx\n", lsc_pick[0],
		       lsc_weight * 100.0, lsc_pick[1], lsc_strength, sensor_total_gain);
	else
		printf("lsc: %s\n", lsc_count ? "loaded, not applied yet (no frame converted since start)"
					      : "no lens-shading table");
}

static unsigned char clamp8(int value)
{
	if (value < 0) return 0;
	if (value > 255) return 255;
	return (unsigned char)value;
}

/* Image tuning (static so the host tests can vary them). */
static int chroma_radius = 2;		/* colour averaged over (2r+1)^2 output pixels; 0 = per pixel */
#define TNR_THRESHOLD 14		/* luma steps (0..255) still treated as noise */
/*
 * Above HIGH_GAIN_X total sensor gain the colour neighbourhood grows by one step at
 * HIGH_GAIN_X and another at 2 x HIGH_GAIN_X (5x5 -> 7x7 -> 9x9), the temporal luma
 * threshold grows with the square root of the gain (noise does), up to 2x, and colour is
 * faded out across strong luma edges, where the 4x4 window straddles the edge and its
 * R, B and G sites see different sides (purple/green speckle on 2026-11-18 boot-11).
 */
#define HIGH_GAIN_X 8.0
#define EDGE_DESATURATION 0.7	/* most colour removed at the strongest edges */

/*
 * chroma_radius is in cells of a 640-wide colour grid (about 4 sensor pixels each), where it was
 * tuned (boot-11..15). The grid is COL_WIDTH wide, so scale it to cover the same sensor area:
 * 3 cells at 960 (1920x1080 output), 1 at 320 (640x480). Boot-19: unscaled at 960, the green and
 * magenta speckle came back.
 */
static int chroma_radius_scaled(void)
{
	if (chroma_radius <= 0)
		return 0;
	int r = (chroma_radius * COL_WIDTH + 320) / 640;
	return r < 1 ? 1 : r;
}

static int chroma_radius_for_gain(double gain)
{
	if (chroma_radius <= 0)
		return 0;
	return chroma_radius_scaled() + (gain > HIGH_GAIN_X) + (gain > 2 * HIGH_GAIN_X);
}

static int tnr_threshold_for_gain(double gain)
{
	double k = gain > HIGH_GAIN_X ? sqrt(gain / HIGH_GAIN_X) : 1.0;
	return (int)(TNR_THRESHOLD * (k > 2.0 ? 2.0 : k) + 0.5);
}

/*
 * Fit a linear colour into 0..LINEAR_MAX: keep its luminance (Rec. 709) and hue, reduce its
 * saturation just enough; white when the luminance itself is past the top.
 */
static void soft_clip_rgb(int c[3])
{
	int mx = c[0] > c[1] ? c[0] : c[1];

	if (c[2] > mx)
		mx = c[2];
	if (mx <= (int)LINEAR_MAX)
		return;
	int64_t y = (2126LL * c[0] + 7152LL * c[1] + 722LL * c[2] + 5000) / 10000;
	for (int k = 0; k < 3; k++) {
		if (y >= (int64_t)LINEAR_MAX)
			c[k] = (int)LINEAR_MAX;
		else
			c[k] = (int)(y + ((int64_t)c[k] - y) * ((int64_t)LINEAR_MAX - y) / ((int64_t)mx - y));
		if (c[k] < 0)
			c[k] = 0;
		if (c[k] > (int)LINEAR_MAX)
			c[k] = (int)LINEAR_MAX;
	}
}

/*
 * Turn the output by 180 degrees (--rotate 180). The ov8856 driver exposes no mounting
 * rotation control, so this is a launcher setting (CAMERA_REAR_ROTATION in gbos.cfg).
 */
static int output_rotate180;

/* Output luma (full resolution, unrotated). */
static int16_t plane_y[OUT_HEIGHT][OUT_WIDTH];
static unsigned char tnr_previous[OUT_HEIGHT][OUT_WIDTH];
/*
 * Colour grid: linear (pre-gamma) R, G, B per point, their neighbourhood averages (then the
 * final colour), and the window's raw green (linear, no lens shading) for the detail pass.
 */
static int16_t plane_tmp[COL_HEIGHT][COL_WIDTH];
static int16_t lin_plane[3][COL_HEIGHT][COL_WIDTH], lin_avg[3][COL_HEIGHT][COL_WIDTH];
static uint16_t col_green[COL_HEIGHT][COL_WIDTH];
/* Colour grid in Y', U, V (gamma-encoded, BT.601 full range): the 640x480 picture as before. */
static int16_t col_y[COL_HEIGHT][COL_WIDTH], col_u[COL_HEIGHT][COL_WIDTH], col_v[COL_HEIGHT][COL_WIDTH];
static int tnr_have_previous;

/* Forget the previous frame (start, or resuming after idle): no blending with stale content. */
static void tnr_reset(void)
{
	tnr_have_previous = 0;
}

static void box_filter_plane(int16_t plane[COL_HEIGHT][COL_WIDTH], int r)
{
	/* Separable running-sum box filter, edges clamped. */
	for (int y = 0; y < COL_HEIGHT; y++) {
		int sum = 0;
		for (int k = -r; k <= r; k++)
			sum += plane[y][k < 0 ? 0 : (k >= COL_WIDTH ? COL_WIDTH - 1 : k)];
		for (int x = 0; x < COL_WIDTH; x++) {
			plane_tmp[y][x] = (int16_t)(sum / (2 * r + 1));
			int out_x = x - r, in_x = x + r + 1;
			sum -= plane[y][out_x < 0 ? 0 : out_x];
			sum += plane[y][in_x >= COL_WIDTH ? COL_WIDTH - 1 : in_x];
		}
	}
	for (int x = 0; x < COL_WIDTH; x++) {
		int sum = 0;
		for (int k = -r; k <= r; k++)
			sum += plane_tmp[k < 0 ? 0 : (k >= COL_HEIGHT ? COL_HEIGHT - 1 : k)][x];
		for (int y = 0; y < COL_HEIGHT; y++) {
			plane[y][x] = (int16_t)(sum / (2 * r + 1));
			int out_y = y - r, in_y = y + r + 1;
			sum -= plane_tmp[out_y < 0 ? 0 : out_y][x];
			sum += plane_tmp[in_y >= COL_HEIGHT ? COL_HEIGHT - 1 : in_y][x];
		}
	}
}

/*
 * Front (RGB-IR) colour reliability. Boot-15 (2026-11-20, selfie by a sunny window, NIR 0.7-0.9
 * of green over the frame): a dark green T-shirt came out teal, a dark teal wall navy, skin
 * pinkish, while the white ceiling was nearly neutral. Dyed fabric and paint reflect little
 * visible light but much NIR, so in their windows NIR is several times the visible green, and
 * any error in a channel's NIR response (the blue term is our fit, +/-0.1) is multiplied by
 * that ratio. White balance absorbs the error at the frame's own NIR level, so what is left
 * grows with the window's NIR ratio minus the frame's (wb_nir_ratio). The blue term is now refit
 * (k_B above); as a safety net, past NIR_CHROMA_FREE above the frame's ratio the window's colour
 * is faded towards its luminance: half kept at NIR_CHROMA_HALF more.
 *
 * And where the window's 8 green sites disagree strongly (stripes, sharp edges: the window
 * blinds), its 2 R and 2 B sites see other parts of the pattern than the greens: green and
 * magenta fringes at any gain. Past HF_CHROMA_START (green contrast (max-min)/(max+min)), colour
 * fades, to HF_CHROMA_MIN at HF_CHROMA_FULL.
 */
static int chroma_guard_strongest = 1;	/* 0: NIR relative to green, as before (host-test control) */
#define NIR_CHROMA_FREE 1.5
#define NIR_CHROMA_HALF 3.0
#define HF_CHROMA_START 0.25
#define HF_CHROMA_FULL 0.75
#define HF_CHROMA_MIN 0.15
static int chroma_guard = 1;	/* 0: off (host-test negative control) */

/* Colour kept (0..1) for a window: NIR ratio rho (NIR / visible green), green contrast hf. */
static double chroma_keep_for(double rho, double hf)
{
	double keep = 1.0, d = rho - wb_nir_ratio - NIR_CHROMA_FREE;

	if (d > 0)
		keep = 1.0 / (1.0 + d / NIR_CHROMA_HALF);
	if (hf > HF_CHROMA_START) {
		double t = (hf - HF_CHROMA_START) / (HF_CHROMA_FULL - HF_CHROMA_START);
		double k = 1.0 - (1.0 - HF_CHROMA_MIN) * (t > 1 ? 1 : t);
		if (k < keep)
			keep = k;
	}
	return keep;
}

/* Linear-light R, G, B (0..LINEAR_MAX) of the 4x4 window at x, y. */
static void rgb_linear_at(const uint16_t *raw, unsigned int width, unsigned int height,
			  unsigned int stride, const unsigned int gain_q12[3],
			  unsigned int x, unsigned int y, const unsigned int *shading_q12,
			  int rgb[3], unsigned int *green_out)
{
	/*
	 * A 4x4 window holds every site of the 4x4 mosaic exactly once (2 R, 2 B, 8 G,
	 * 4 IR; for Bayer 4/8/4). The old 3x3 window often held no R or no B site, so
	 * red or blue read as zero in drifting patches: the cyan/yellow grid.
	 * The window is shifted inward at the frame edges so it is always complete.
	 */
	unsigned int sum[4], n[4];
	unsigned int x0 = x > 0 ? x - 1 : 0, y0 = y > 0 ? y - 1 : 0;

	if (x0 > width - 4) x0 = width - 4;
	if (y0 > height - 4) y0 = height - 4;
	unsigned int peak = 0, gmin = ~0u, gmax = 0, s0 = 0, s1 = 0, s2 = 0, s3 = 0;
	init_raw_lut();
	/*
	 * Masked sums in registers: any 4x4 window holds each site of the mosaic once, so the
	 * per-channel counts are fixed (cfa_count); the channel of each site comes from its phase.
	 */
	for (unsigned int sy = y0; sy < y0 + 4; sy++) {
		const uint16_t *row = raw + (size_t)sy * (stride / 2);	/* the window is inside the frame */
		for (unsigned int sx = x0; sx < x0 + 4; sx++) {
			const uint32_t *m = cfa_mask[sy & 3][sx & 3];
			unsigned int v = raw_lin_lut[row[sx] & 1023];
			s0 += v & m[0]; s1 += v & m[1]; s2 += v & m[2]; s3 += v & m[3];
			peak = v > peak ? v : peak;
			unsigned int lo = v | ~m[1], hi = v & m[1];
			gmin = lo < gmin ? lo : gmin;
			gmax = hi > gmax ? hi : gmax;
		}
	}
	sum[0] = s0; sum[1] = s1; sum[2] = s2; sum[3] = s3;
	memcpy(n, cfa_count, sizeof(n));
	if (green_out)
		*green_out = n[1] ? sum[1] / n[1] : 0;
	/*
	 * Lens shading (shading_q12: R, G, B gains for this position; NULL = none). The IR
	 * sites see the same optics, so they take the green gain. Then remove the infrared
	 * that leaks into the colour sites (zero for plain Bayer).
	 */
	unsigned int shade[4] = { 4096, 4096, 4096, 4096 };
	if (shading_q12) {
		shade[0] = shading_q12[0]; shade[1] = shading_q12[1];
		shade[2] = shading_q12[2]; shade[CFA_IR] = shading_q12[1];
	}
	int mean[4];
	for (int channel = 0; channel < 4; channel++) {
		/* Site counts are 2, 4 or 8 on both mosaics: a shift instead of a division. */
		unsigned int c = n[channel];
		uint64_t v = (uint64_t)sum[channel] * shade[channel];
		mean[channel] = !c ? 0 : (c & (c - 1)) ? (int)((v / c + 2048) >> 12) :
			(int)((v + ((uint64_t)c << 11)) >> (12 + __builtin_ctz(c)));
	}
	int ir = mean[CFA_IR];
	int nir = n[CFA_IR] ? nir_estimate(ir, mean[1]) : 0;
	int lin[3];
	for (int channel = 0; channel < 3; channel++) {
		lin[channel] = n[channel] ?
			mean[channel] - ir + ((nir * nir_keep_q12[channel] + 2048) >> 12) : 0;
		if (lin[channel] < 0)
			lin[channel] = 0;
	}
	/* Front only (see chroma_keep_for); the rear's colour is reported good as it is. */
	double keep = 1.0;
	if (chroma_guard && n[CFA_IR]) {
		/*
		 * NIR relative to the window's strongest colour (2026-11-21, calibration shot): a red
		 * flag in window light reflects 2-6x more NIR than its green; measured against green,
		 * that genuine red was faded to brown (Windows renders it red). Measured against red,
		 * it keeps its colour, while a window where NIR swamps every colour still fades.
		 */
		int top = lin[1];
		if (chroma_guard_strongest) {
			if (lin[0] > top) top = lin[0];
			if (lin[2] > top) top = lin[2];
		}
		int g16 = top > 16 ? top : 16;
		/* Cheap integer pre-checks; the exact fade only where one of them can apply. */
		if ((gmax - gmin) * 4 > gmax + gmin + 64 ||
		    nir > (wb_nir_ratio + NIR_CHROMA_FREE) * g16) {
			double rho = (double)nir / g16;
			double hf = (double)(gmax - gmin) / (gmax + gmin + 64.0);
			keep = chroma_keep_for(rho, hf);
		}
	}
	rgb[0] = (int)(((uint64_t)lin[0] * gain_q12[0] + 2048) / 4096);
	rgb[1] = lin[1];
	rgb[2] = (int)(((uint64_t)lin[2] * gain_q12[2] + 2048) / 4096);
	/*
	 * Colour correction: the raw R/G/B responses overlap (and carry residual IR), so
	 * colours come out muted. With the module's calibration table (ccm_active) use the
	 * matrix blended for the measured illuminant; otherwise scale each channel's
	 * distance from linear luminance (Rec. 709 weights) by saturation_q12/4096.
	 * Rows sum to one either way, so greys stay grey.
	 */
	if (ccm_active) {
		int in[3] = { rgb[0], rgb[1], rgb[2] };
		for (int row = 0; row < 3; row++) {
			int64_t v = (int64_t)ccm_q12[row * 3] * in[0] + (int64_t)ccm_q12[row * 3 + 1] * in[1] +
				    (int64_t)ccm_q12[row * 3 + 2] * in[2];
			v = (v + 2048) >> 12;
			rgb[row] = v < 0 ? 0 : (int)v;
		}
	} else {
		int luma = (871 * rgb[0] + 2929 * rgb[1] + 296 * rgb[2] + 2048) >> 12;
		for (int channel = 0; channel < 3; channel++) {
			int v = luma + (int)(((int64_t)(rgb[channel] - luma) * saturation_q12 + 2048) >> 12);
			rgb[channel] = v < 0 ? 0 : v;
		}
	}
	/*
	 * Bright colours past the top of the range (lens shading lifts the edges up to ~4x,
	 * white balance R and B ~1.85x, then the matrix): clipping each channel on its own turns
	 * them pink or magenta. Desaturate towards their luminance instead, as ISPs do.
	 */
	if (keep < 1.0) {
		double y = 0.2126 * rgb[0] + 0.7152 * rgb[1] + 0.0722 * rgb[2];
		for (int channel = 0; channel < 3; channel++) {
			double v = y + keep * (rgb[channel] - y);
			rgb[channel] = v < 0 ? 0 : (int)(v + 0.5);
		}
	}
	soft_clip_rgb(rgb);
	/*
	 * Highlights: where any site in the window is near the sensor's clip level, its
	 * colour is no longer measured. On the RGB-IR sensor the IR sites clip too, and
	 * "clipped colour minus clipped IR" is zero: lamps turned black with a magenta rim
	 * (green clips first; 2026-11-17 photo). Fade such windows towards white, from no
	 * effect at 80% of the clip level to pure white at 97% (set_highlight_range).
	 */
	if (peak > highlight_start_lin) {
		int t = highlight_full_lin > highlight_start_lin ?
			(int)(((peak - highlight_start_lin) * 4096u) / (highlight_full_lin - highlight_start_lin)) : 4096;
		if (t > 4096)
			t = 4096;
		for (int channel = 0; channel < 3; channel++)
			rgb[channel] = (rgb[channel] * (4096 - t) + (int)LINEAR_MAX * t + 2048) >> 12;
	}
}

/*
 * Colour from neighbourhood averages in linear light, luma per pixel. Averaging after the
 * gamma curve (the earlier U/V box filter) is biased: gamma is concave, so the noisier
 * channels (R and B: 2 of 16 sites each, IR subtracted, amplified by the colour matrix)
 * lose more than G, and noisy greys turned green at high gain (2026-11-18 boot-11). Here
 * each colour-grid point keeps its own linear luminance but takes the R:G:B ratios of the
 * (2r+1)^2 average; near strong luma edges those ratios are pulled towards grey. The result
 * goes to col_y/col_u/col_v.
 */
struct mix_job { int r, edges; };

static void linear_colour_mix_rows(void *ctx, unsigned int row0, unsigned int row1)
{
	const struct mix_job *job = ctx;
	int r = job->r, edges = job->edges;

	for (int oy = (int)row0; oy < (int)row1; oy++)
		for (int ox = 0; ox < COL_WIDTH; ox++) {
			int lr = lin_plane[0][oy][ox], lg = lin_plane[1][oy][ox], lb = lin_plane[2][oy][ox];
			int ar = lin_avg[0][oy][ox], ag = lin_avg[1][oy][ox], ab = lin_avg[2][oy][ox];
			int c[3];
			int64_t ylin = 2126LL * lr + 7152LL * lg + 722LL * lb;	/* x 10000 */
			int64_t yavg = 2126LL * ar + 7152LL * ag + 722LL * ab;
			if (r == 0 || yavg <= 0) {
				c[0] = lr; c[1] = lg; c[2] = lb;
			} else {
				/* Strength of the local luma edge, relative to the local level. */
				double keep = 1.0;
				if (edges) {
					int xl = ox > 0 ? ox - 1 : ox, xr = ox + 1 < COL_WIDTH ? ox + 1 : ox;
					int yu = oy > 0 ? oy - 1 : oy, yd = oy + 1 < COL_HEIGHT ? oy + 1 : oy;
					double gx = fabs((double)lin_plane[1][oy][xr] - lin_plane[1][oy][xl]);
					double gy = fabs((double)lin_plane[1][yd][ox] - lin_plane[1][yu][ox]);
					double rel = (gx + gy) / (ag + 32.0);
					double t = (rel - 0.5) / 1.0;
					t = t < 0 ? 0 : t > 1 ? 1 : t;
					keep = 1.0 - EDGE_DESATURATION * t;
				}
				int avg[3] = { ar, ag, ab };
				for (int k = 0; k < 3; k++) {
					double ratio = (double)avg[k] * 10000.0 / (double)yavg;
					ratio = 1.0 + keep * (ratio - 1.0);
					double v = (double)ylin / 10000.0 * ratio;
					c[k] = v < 0 ? 0 : v > 4.0 * LINEAR_MAX ? 4 * (int)LINEAR_MAX : (int)(v + 0.5);
				}
				soft_clip_rgb(c);	/* a bright pixel taking a saturated average */
			}
			for (int k = 0; k < 3; k++)
				c[k] = gamma_lut[c[k]];
			col_y[oy][ox] = (int16_t)((77 * c[0] + 150 * c[1] + 29 * c[2] + 128) >> 8);
			col_u[oy][ox] = (int16_t)((-43 * c[0] - 85 * c[1] + 128 * c[2]) >> 8);
			col_v[oy][ox] = (int16_t)((128 * c[0] - 107 * c[1] - 21 * c[2]) >> 8);
		}
}

/*
 * Rows are independent in the colour grid, the mix and the output pass, so each runs in
 * BRIDGE_THREADS horizontal bands (1280x960 at 30 fps needs about 45 ms of one core per frame
 * on the i7-1180G7; two bands bring each frame under 33 ms). Threads are started per pass;
 * if one cannot be started its band runs here.
 */
#define BRIDGE_THREADS 2
struct band {
	void (*fn)(void *ctx, unsigned int row0, unsigned int row1);
	void *ctx;
	unsigned int row0, row1;
};

static void *band_run(void *arg)
{
	struct band *b = arg;

	b->fn(b->ctx, b->row0, b->row1);
	return NULL;
}

static void run_bands(void (*fn)(void *, unsigned int, unsigned int), void *ctx, unsigned int rows)
{
	pthread_t thread[BRIDGE_THREADS];
	struct band band[BRIDGE_THREADS];
	int started[BRIDGE_THREADS] = { 0 };

	for (int i = 0; i < BRIDGE_THREADS; i++) {
		band[i].fn = fn;
		band[i].ctx = ctx;
		band[i].row0 = rows * (unsigned int)i / BRIDGE_THREADS;
		band[i].row1 = rows * (unsigned int)(i + 1) / BRIDGE_THREADS;
	}
	for (int i = 1; i < BRIDGE_THREADS; i++)
		started[i] = pthread_create(&thread[i], NULL, band_run, &band[i]) == 0;
	band_run(&band[0]);
	for (int i = 1; i < BRIDGE_THREADS; i++) {
		if (started[i])
			pthread_join(thread[i], NULL);
		else
			band_run(&band[i]);
	}
}

static void linear_colour_mix(int r, double gain)
{
	struct mix_job job = { r, r > 0 && gain > HIGH_GAIN_X };

	for (int c = 0; c < 3; c++) {
		memcpy(lin_avg[c], lin_plane[c], sizeof(lin_avg[c]));
		if (r > 0)
			box_filter_plane(lin_avg[c], r);
	}
	run_bands(linear_colour_mix_rows, &job, COL_HEIGHT);
}

/*
 * Detail. Output pixels fall on colour-grid points or halfway between them; each takes the
 * grid's Y', U, V (bilinear), so colour is exactly the 640x480 picture's. Its brightness is
 * then scaled by its own green relative to the window's: the 2 green sites of the 2x2 block at
 * its sensor position (half of all sites are green, in a checkerboard, on the RGB-IR and on
 * the Bayer sensor) over the window's 8. That ratio is linear light, so Y' scales by its
 * 1/2.4 power (detail_lut). Green from 2 sites is noisier than from 8, so at high gain the
 * detail is reduced: full up to DETAIL_FULL_GAIN, DETAIL_MIN at DETAIL_MIN_GAIN and above.
 */
#define DETAIL_FULL_GAIN 8.0
#define DETAIL_MIN_GAIN 32.0
#define DETAIL_MIN 0.4
#define DETAIL_LUT_MAX 1024	/* ratio 0..4 in steps of 1/256 */
static int detail_enabled = 1;	/* 0: colour-grid brightness only (host-test negative control) */
static uint16_t detail_lut[DETAIL_LUT_MAX + 1];	/* Q12 factor for Y' */
static int detail_lut_q12 = -1;

static int detail_q12_for_gain(double gain)
{
	if (!detail_enabled)
		return 0;
	double t = (gain - DETAIL_FULL_GAIN) / (DETAIL_MIN_GAIN - DETAIL_FULL_GAIN);
	t = t < 0 ? 0 : t > 1 ? 1 : t;
	return (int)((1.0 - (1.0 - DETAIL_MIN) * t) * 4096.0 + 0.5);
}

static void detail_lut_build(int detail_q12)
{
	if (detail_lut_q12 == detail_q12)
		return;
	for (int i = 0; i <= DETAIL_LUT_MAX; i++) {
		double f = 1.0 + detail_q12 / 4096.0 * (i / 256.0 - 1.0);
		double y = pow(f > 0 ? f : 0, 1.0 / 2.4) * 4096.0 + 0.5;
		detail_lut[i] = (uint16_t)(y > 65535 ? 65535 : y);
	}
	detail_lut_q12 = detail_q12;
}

struct frame_job {
	const uint16_t *raw;
	unsigned int width, height, stride;
	const unsigned int *gain_q12;
	int detail_q12, tnr_threshold;
	unsigned char *out;
};

/* Colour grid rows: one 4x4 window per point to linear R, G, B, and the window's green. */
static void colour_grid_rows(void *ctx, unsigned int row0, unsigned int row1)
{
	const struct frame_job *job = ctx;

	for (unsigned int oy = row0; oy < row1; oy++) {
		unsigned int sy = crop_y + (unsigned int)(((uint64_t)oy * crop_h) / COL_HEIGHT);
		for (unsigned int ox = 0; ox < COL_WIDTH; ox++) {
			unsigned int sx = crop_x + (unsigned int)(((uint64_t)ox * crop_w) / COL_WIDTH);
			int c[3];
			unsigned int shading[3], green;
			if (lsc_active) {
				shading[0] = lsc_map[0][oy][ox];
				shading[1] = lsc_map[1][oy][ox];
				shading[2] = lsc_map[2][oy][ox];
			}
			rgb_linear_at(job->raw, job->width, job->height, job->stride, job->gain_q12, sx, sy,
				      lsc_active ? shading : NULL, c, &green);
			for (int k = 0; k < 3; k++)
				lin_plane[k][oy][ox] = (int16_t)c[k];
			col_green[oy][ox] = (uint16_t)green;
		}
	}
}

/*
 * Output rows (unrotated row sy): brightness with detail, temporal luma filter, then YUYV into
 * output row sy, or H - 1 - sy backwards with --rotate 180 (lens shading and the statistics
 * stay in sensor positions). U/V of a pixel pair: the mean of the pair's bilinear colour-grid
 * values (the pair covers grid point cx and halfway to cx + 1: weights 3/4, 1/4; rows: on a
 * grid row or halfway).
 *
 * Temporal filter: where a pixel changed by no more than the threshold since the last output
 * it moves only a third of the way, averaging noise in still areas; larger changes (motion)
 * pass through at once, so no ghosts.
 */
static void output_rows(void *ctx, unsigned int row0, unsigned int row1)
{
	const struct frame_job *job = ctx;
	const uint16_t *raw = job->raw;
	unsigned int width = job->width, height = job->height, stride = job->stride;
	int detail_q12 = job->detail_q12, tnr_threshold = job->tnr_threshold;

	for (unsigned int oy = row0; oy < row1; oy++) {
		unsigned int sy = crop_y + (unsigned int)(((uint64_t)oy * crop_h) / OUT_HEIGHT);
		if (sy > height - 2)
			sy = height - 2;
		const uint16_t *r0 = raw + (size_t)sy * (stride / 2), *r1 = r0 + stride / 2;
		const uint32_t (*m0)[4] = cfa_mask[sy & 3], (*m1)[4] = cfa_mask[(sy + 1) & 3];
		int cy0 = (int)(oy >> 1), cy1 = (oy & 1) && cy0 + 1 < COL_HEIGHT ? cy0 + 1 : cy0;
		int16_t *yrow = plane_y[oy];
		unsigned char *prev = tnr_previous[oy];
		for (unsigned int ox = 0; ox < OUT_WIDTH; ox++) {
			int cx0 = (int)(ox >> 1), cx1 = (ox & 1) && cx0 + 1 < COL_WIDTH ? cx0 + 1 : cx0;
			int y = (col_y[cy0][cx0] + col_y[cy0][cx1] + col_y[cy1][cx0] + col_y[cy1][cx1] + 2) >> 2;
			if (detail_q12 > 0) {
				unsigned int sx = crop_x + (unsigned int)(((uint64_t)ox * crop_w) / OUT_WIDTH);
				if (sx > width - 2)
					sx = width - 2;
				unsigned int gw = (col_green[cy0][cx0] + col_green[cy0][cx1] +
						   col_green[cy1][cx0] + col_green[cy1][cx1] + 2) >> 2;
				/* The 2 green sites of the 2x2 block (masks: the phase varies). */
				unsigned int g = (raw_lin_lut[r0[sx] & 1023] & m0[sx & 3][1]) +
					(raw_lin_lut[r0[sx + 1] & 1023] & m0[(sx + 1) & 3][1]) +
					(raw_lin_lut[r1[sx] & 1023] & m1[sx & 3][1]) +
					(raw_lin_lut[r1[sx + 1] & 1023] & m1[(sx + 1) & 3][1]);
				/* index = g / 2 / gw in steps of 1/256 */
				unsigned int i = gw ? (g * 128u) / gw : 256u;
				y = (y * detail_lut[i > DETAIL_LUT_MAX ? DETAIL_LUT_MAX : i] + 2048) >> 12;
			}
			if (tnr_have_previous) {
				int d = y - prev[ox];
				if (d >= -tnr_threshold && d <= tnr_threshold)
					y = prev[ox] + (d >= 0 ? (d + 1) / 3 : -((-d + 1) / 3));
			}
			y = clamp8(y);
			prev[ox] = (unsigned char)y;
			yrow[ox] = (int16_t)y;
		}
		unsigned char *dst = job->out + (size_t)(output_rotate180 ? OUT_HEIGHT - 1 - oy : oy) *
			OUT_WIDTH * 2;
		for (unsigned int ox = 0; ox < OUT_WIDTH; ox += 2) {
			int cx = (int)(ox >> 1), cxn = cx + 1 < COL_WIDTH ? cx + 1 : cx;
			int u = 3 * (col_u[cy0][cx] + col_u[cy1][cx]) + col_u[cy0][cxn] + col_u[cy1][cxn];
			int v = 3 * (col_v[cy0][cx] + col_v[cy1][cx]) + col_v[cy0][cxn] + col_v[cy1][cxn];
			unsigned char U = clamp8(128 + (u >= 0 ? (u + 4) >> 3 : -((-u + 4) >> 3)));
			unsigned char V = clamp8(128 + (v >= 0 ? (v + 4) >> 3 : -((-v + 4) >> 3)));
			unsigned int at = output_rotate180 ? OUT_WIDTH - 2 - ox : ox;
			dst[at * 2] = (unsigned char)yrow[output_rotate180 ? ox + 1 : ox];
			dst[at * 2 + 1] = U;
			dst[at * 2 + 2] = (unsigned char)yrow[output_rotate180 ? ox : ox + 1];
			dst[at * 2 + 3] = V;
		}
	}
}

#ifdef IPU6_GPU
static double monotonic_s(void);
#include "ipu6-gpu.c"
#endif

static int convert_bayer_yuyv(const uint16_t *raw, unsigned int width,
			      unsigned int height, unsigned int stride,
			      unsigned char *out, size_t out_size, double *mean_green)
{
	if (!raw || !out || width < 8 || height < 8 ||
	    (width & 1) || (height & 1) ||
	    (uint64_t)stride < (uint64_t)width * 2 ||
	    (uint64_t)stride * height > MAX_FRAME_BYTES ||
	    out_size < OUT_FRAME_BYTES)
		return -1;

	unsigned int gain_q12[3];
	init_gamma();
	init_raw_lut();
	calculate_white_balance(raw, width, height, stride, gain_q12, mean_green);
	if (wb_last_mean[1] > 0)
		update_lsc(wb_white_rg, wb_white_bg, width, height);
#ifdef IPU6_GPU
	if (gpu.ok) {
		if (gpu_convert(raw, width, height, stride, gain_q12, chroma_radius_for_gain(sensor_total_gain),
				sensor_total_gain, out) == 0) {
			tnr_have_previous = 1;
			return 0;
		}
		gpu.ok = 0;
		tnr_have_previous = 0;	/* the CPU's previous-frame plane is stale */
		fprintf(stderr, "gpu: disabled for the rest of this run; converting on the CPU\n");
	}
#endif

	struct frame_job job = {
		raw, width, height, stride, gain_q12,
		detail_q12_for_gain(sensor_total_gain), tnr_threshold_for_gain(sensor_total_gain), out
	};
	/* 1. Colour grid: demosaic one 4x4 window per point to linear R, G, B. */
	run_bands(colour_grid_rows, &job, COL_HEIGHT);
	/*
	 * 2. Chroma noise: R and B are only 2 of 16 sites, so at high gain colour speckle
	 * dominates. Colour comes from a neighbourhood average in linear light (growing with
	 * gain); luma keeps full detail.
	 */
	linear_colour_mix(chroma_radius_for_gain(sensor_total_gain), sensor_total_gain);
	/* 3. Full-resolution brightness from the green sites, temporal luma filter, YUYV. */
	detail_lut_build(job.detail_q12);
	run_bands(output_rows, &job, OUT_HEIGHT);
	tnr_have_previous = 1;
	return 0;
}

struct ae_control {
	__u32 id;
	int min, max, saved, value, present, def;
};

struct ae_state {
	int fd, enabled;
	unsigned int frames;
	struct ae_control exposure, again, dgain, vblank;
	/*
	 * Exposure lines possible at the default frame length (base_max) and with the frame
	 * stretched by VBLANK (long_max; equal to base_max without a VBLANK control), and the
	 * lines the driver keeps between exposure and frame length (frame = height + vblank).
	 */
	int height, base_max, long_max, margin;
	/* Idle: frame stretched to idle_vblank; applied_vblank is what the sensor has now. */
	int idle, idle_vblank, applied_vblank;
	int fast;		/* frames left in the fast phase (ae_wake) */
};

/* VBLANK the sensor should have: the plan's, or the idle frame while nobody reads. */
static int ae_effective_vblank(const struct ae_state *ae)
{
	if (ae->idle && ae->idle_vblank > ae->vblank.value)
		return ae->idle_vblank;
	return ae->vblank.value;
}

static int ae_again_max(const struct ae_state *ae)
{
	double cap = AE_AGAIN_MAX_X * ae->again.min;

	return ae->again.max < cap ? ae->again.max : (int)(cap + 0.5);
}

/* VBLANK for an exposure: the default unless the exposure needs a longer frame. */
static int ae_vblank_for(const struct ae_state *ae, int exposure)
{
	int vblank = exposure + ae->margin - ae->height;

	if (exposure <= ae->base_max || vblank < ae->vblank.def)
		return ae->vblank.def;
	return vblank > ae->vblank.max ? ae->vblank.max : vblank;
}

/* Frame time in ms, assuming the default frame is 1/30 s (what the driver's mode sets). */
static double ae_frame_ms(const struct ae_state *ae)
{
	if (!ae->vblank.present)
		return 1000.0 / 30;
	return 1000.0 / 30 * (ae->height + ae_effective_vblank(ae)) / (ae->height + ae->vblank.def);
}

/* Brightness relative to minimum exposure and gains: lines x analogue x digital multiplier. */
static double ae_total(const struct ae_state *ae)
{
	double total = ae->exposure.value;

	total *= (double)ae->again.value / ae->again.min;
	if (ae->dgain.present)
		total *= (double)ae->dgain.value / ae->dgain.min;
	return total;
}

static int ae_clamp(double value, int min, int max)
{
	if (value < min) return min;
	if (value > max) return max;
	return (int)(value + 0.5);
}

/*
 * Split a total into exposure time first (no added noise) up to the default frame, then
 * analogue gain up to AE_LONG_KNEE_X, then a longer frame (VBLANK) up to long_max, then
 * analogue gain up to its cap, then digital gain.
 */
static void ae_plan(struct ae_state *ae, double total)
{
	int again_max = ae_again_max(ae);
	double knee = AE_LONG_KNEE_X, exposure, rest;

	if (knee > (double)again_max / ae->again.min)
		knee = (double)again_max / ae->again.min;
	if (total <= ae->base_max)
		exposure = total;
	else if (total <= ae->base_max * knee)
		exposure = ae->base_max;
	else
		exposure = total / knee;
	ae->exposure.value = ae_clamp(exposure, ae->exposure.min, ae->long_max);
	rest = total / ae->exposure.value;
	ae->again.value = ae_clamp(rest * ae->again.min, ae->again.min, again_max);
	rest = rest * ae->again.min / ae->again.value;
	if (ae->dgain.present)
		ae->dgain.value = ae_clamp(rest * ae->dgain.min, ae->dgain.min, ae->dgain.max);
	if (ae->vblank.present)
		ae->vblank.value = ae_vblank_for(ae, ae->exposure.value);
}

/* Returns 1 when the plan changed, 0 when the scene is within the deadband or at a limit. */
static int ae_update(struct ae_state *ae, double mean_green)
{
	double ratio, before = ae_total(ae);
	int exposure = ae->exposure.value, again = ae->again.value, dgain = ae->dgain.value;
	int vblank = ae->vblank.value;

	double max_step = ae->fast > 0 ? AE_FAST_MAX_STEP : AE_MAX_STEP;
	ratio = mean_green > 1.0 ? AE_TARGET / mean_green : max_step;
	if (ae->fast > 0 && ae_clip_fraction > AE_FAST_CLIP_FRACTION && ratio > 0.5)
		ratio = 0.5;
	if (ratio > 1.0 - AE_DEADBAND && ratio < 1.0 + AE_DEADBAND)
		return 0;
	if (ratio > max_step) ratio = max_step;
	if (ratio < 1.0 / max_step) ratio = 1.0 / max_step;
	ae_plan(ae, before * ratio);
	return exposure != ae->exposure.value || again != ae->again.value ||
	       dgain != ae->dgain.value || vblank != ae->vblank.value;
}

/*
 * A control's AE range. The minimum is the unity step (ae_total divides by it), so it must
 * be positive. Exception: a digital gain whose range starts at 0 (OV8856: 0..4095, default
 * 1024 = 1x) uses its default as unity, so AE only raises it, never darkens with it.
 */
static int ae_control_from_query(struct ae_control *c, const struct v4l2_queryctrl *q, int value)
{
	int min = q->minimum;

	c->id = q->id;
	if (q->flags & (V4L2_CTRL_FLAG_DISABLED | V4L2_CTRL_FLAG_READ_ONLY))
		return -1;
	if (min <= 0 && q->id == V4L2_CID_DIGITAL_GAIN && q->default_value > 0)
		min = q->default_value;
	if (min <= 0 || q->maximum < min)
		return -1;
	c->min = min;
	c->max = q->maximum;
	c->def = q->default_value;
	c->saved = value;
	c->value = value < min ? min : value > c->max ? c->max : value;
	c->present = 1;
	return 0;
}

static int ae_query(int fd, struct ae_control *c, __u32 id)
{
	struct v4l2_queryctrl q = { .id = id };
	struct v4l2_control v = { .id = id };

	c->id = id;
	if (ioctl(fd, VIDIOC_QUERYCTRL, &q) || ioctl(fd, VIDIOC_G_CTRL, &v))
		return -1;
	return ae_control_from_query(c, &q, v.value);
}

#ifdef CAMERA_BRIDGE_TEST
/* Host tests (fd -1) record the order of control writes: E V A D. */
static char ae_set_log[16];
static void ae_set_log_reset(void) { ae_set_log[0] = 0; }
#endif

static int ae_set(int fd, const struct ae_control *c, int value)
{
	struct v4l2_control v = { .id = c->id, .value = value };

	if (!c->present)
		return 0;
#ifdef CAMERA_BRIDGE_TEST
	if (fd < 0) {
		size_t n = strlen(ae_set_log);
		if (n + 1 < sizeof(ae_set_log)) {
			ae_set_log[n] = c->id == V4L2_CID_EXPOSURE ? 'E' : c->id == V4L2_CID_VBLANK ? 'V' :
					c->id == V4L2_CID_ANALOGUE_GAIN ? 'A' : 'D';
			ae_set_log[n + 1] = 0;
		}
		return 0;
	}
#endif
	return ioctl(fd, VIDIOC_S_CTRL, &v);
}

/*
 * Exposure limits for the default and the stretched frame. The driver bounds exposure by
 * the current frame (height + vblank - margin), so the margin is measured from the
 * current VBLANK and exposure maximum. Without a usable VBLANK the frame stays fixed.
 */
static void ae_frame_limits(struct ae_state *ae, int height)
{
	ae->height = height;
	ae->base_max = ae->long_max = ae->exposure.max;
	ae->margin = 0;
	if (!ae->vblank.present || height <= 0)
		goto fixed;
	if (ae->vblank.def < ae->vblank.min || ae->vblank.def > ae->vblank.max)
		goto fixed;
	ae->margin = height + ae->vblank.value - ae->exposure.max;
	ae->base_max = height + ae->vblank.def - ae->margin;
	if (ae->margin < 0 || ae->base_max < ae->exposure.min)
		goto fixed;
	double frame = AE_LONG_FRAME_X * (height + ae->vblank.def);
	ae->long_max = (int)frame - ae->margin;
	if (ae->long_max > height + ae->vblank.max - ae->margin)
		ae->long_max = height + ae->vblank.max - ae->margin;
	if (ae->long_max < ae->base_max)
		ae->long_max = ae->base_max;
	ae->idle_vblank = (int)(AE_IDLE_FRAME_X * (height + ae->vblank.def)) - height;
	if (ae->idle_vblank > ae->vblank.max)
		ae->idle_vblank = ae->vblank.max;
	ae->applied_vblank = ae->vblank.value;
	return;
fixed:
	ae->vblank.present = 0;
	ae->idle_vblank = ae->applied_vblank = 0;
	ae->margin = 0;
	ae->base_max = ae->long_max = ae->exposure.max;
}

static void ae_init(struct ae_state *ae, int fd, int height)
{
	memset(ae, 0, sizeof(*ae));
	ae->fd = fd;
	if (ae_query(fd, &ae->exposure, V4L2_CID_EXPOSURE) ||
	    ae_query(fd, &ae->again, V4L2_CID_ANALOGUE_GAIN)) {
		printf("ae: sensor exposure/analogue gain controls unavailable; auto-exposure off\n");
		return;
	}
	(void)ae_query(fd, &ae->dgain, V4L2_CID_DIGITAL_GAIN);
	(void)ae_query(fd, &ae->vblank, V4L2_CID_VBLANK);
	ae_frame_limits(ae, height);
	ae->enabled = 1;
	printf("ae: exposure %d..%d (now %d) again %d..%d (now %d, cap %d) dgain %s %d..%d (now %d)\n",
	       ae->exposure.min, ae->exposure.max, ae->exposure.value,
	       ae->again.min, ae->again.max, ae->again.value, ae_again_max(ae),
	       ae->dgain.present ? "on" : "absent", ae->dgain.min, ae->dgain.max, ae->dgain.value);
	if (ae->vblank.present)
		printf("ae: vblank %d..%d (default %d, now %d): exposure up to %d lines at 30 fps, "
		       "%d lines at %.1f fps above %.1fx analogue gain\n",
		       ae->vblank.min, ae->vblank.max, ae->vblank.def, ae->vblank.value,
		       ae->base_max, ae->long_max, 30.0 / AE_LONG_FRAME_X, AE_LONG_KNEE_X);
	else
		printf("ae: no usable VBLANK control; frame length fixed\n");
}

/*
 * Apply settings in an order the driver accepts: it bounds exposure by the current frame,
 * so a longer frame goes in before a longer exposure and a shorter one after.
 */
static int ae_apply(const struct ae_state *ae, int exposure, int vblank, int again, int dgain,
		    int current_vblank)
{
	int stretch = ae->vblank.present && vblank > current_vblank;

	if (stretch && ae_set(ae->fd, &ae->vblank, vblank))
		return -1;
	if (ae_set(ae->fd, &ae->exposure, exposure))
		return -1;
	if (!stretch && ae->vblank.present && vblank != current_vblank &&
	    ae_set(ae->fd, &ae->vblank, vblank))
		return -1;
	if (ae_set(ae->fd, &ae->again, again) || ae_set(ae->fd, &ae->dgain, dgain))
		return -1;
	return 0;
}

/* Gain multiple applied on the sensor (analogue x digital); drives lens-shading strength. */
static double ae_sensor_gain(const struct ae_state *ae)
{
	double gain = (double)ae->again.value / ae->again.min;

	if (ae->dgain.present)
		gain *= (double)ae->dgain.value / ae->dgain.min;
	return gain < 1.0 ? 1.0 : gain;
}

static void ae_frame(struct ae_state *ae, double mean_green)
{
	if (!ae->enabled)
		return;
	ae->frames++;
	sensor_total_gain = ae_sensor_gain(ae);
	if (ae->frames % 150 == 0)
		printf("ae: mean=%.0f target=%.0f exposure=%d again=%d dgain=%d vblank=%d "
		       "(%.1f ms frame%s, %.1fx gain)\n", mean_green, AE_TARGET, ae->exposure.value,
		       ae->again.value, ae->dgain.value, ae_effective_vblank(ae), ae_frame_ms(ae),
		       ae->idle ? ", idle" : "", sensor_total_gain);
	int interval = ae->fast > 0 ? AE_FAST_INTERVAL : AE_INTERVAL;
	if (ae->fast > 0)
		ae->fast--;
	if (ae->frames % interval || !ae_update(ae, mean_green))
		return;
	int vblank = ae_effective_vblank(ae);
	if (ae_apply(ae, ae->exposure.value, vblank, ae->again.value, ae->dgain.value,
		     ae->applied_vblank)) {
		perror("ae: set sensor control; auto-exposure off");
		ae->enabled = 0;
		return;
	}
	ae->applied_vblank = vblank;
}

/* An app started streaming (or the stream restarted): converge fast for the next frames. */
static void ae_wake(struct ae_state *ae)
{
	ae->fast = AE_FAST_FRAMES;
	ae->frames = 0;
}

/* A consumer started or stopped: switch between the planned and the idle frame at once. */
static void ae_set_idle(struct ae_state *ae, int idle)
{
	if (!ae->enabled || !ae->vblank.present || ae->idle == idle)
		return;
	ae->idle = idle;
	int vblank = ae_effective_vblank(ae);
	if (vblank == ae->applied_vblank)
		return;
	if (ae_apply(ae, ae->exposure.value, vblank, ae->again.value, ae->dgain.value,
		     ae->applied_vblank)) {
		perror("ae: set idle frame; auto-exposure off");
		ae->enabled = 0;
		return;
	}
	ae->applied_vblank = vblank;
	printf("ae: %s: vblank %d (%.1f ms frame)\n", idle ? "idle frame" : "active frame",
	       vblank, ae_frame_ms(ae));
}

static void ae_restore(struct ae_state *ae)
{
	if (!ae->exposure.present || !ae->again.present)
		return;
	if (ae_apply(ae, ae->exposure.saved, ae->vblank.saved, ae->again.saved, ae->dgain.saved,
		     ae->vblank.present ? ae->applied_vblank : ae->vblank.saved))
		perror("ae: restore sensor controls");
}

/*
 * v4l2loopback's read-only "readers_active" control (V4L2LOOPBACK_CID_BASE + 4):
 * 1 while a capture stream holds the device, 0 when nothing is streaming from it.
 * Returns -1 when the control is missing (an older module): then always convert.
 */
#define LOOPBACK_CID_READERS ((V4L2_CID_USER_BASE | 0xf000) + 4)

static int loopback_readers(int fd)
{
	struct v4l2_control c = { .id = LOOPBACK_CID_READERS };

	return ioctl(fd, VIDIOC_G_CTRL, &c) ? -1 : c.value;
}

/*
 * With no consumer the conversion (~25 ms/frame) is skipped; the sensor keeps streaming
 * until the idle stop (below). The next frame after a consumer starts is converted.
 */
static int bridge_should_convert(int readers)
{
	return readers != 0;
}

/*
 * Idle stop. A streaming capture holds the ISYS stream open, and isys_suspend refuses system
 * sleep while any stream is open (-EBUSY): on boot-12, 24 of 25 s2idle attempts failed with
 * "isys_suspend ... returns -16" while both bridges streamed with no consumer, and both
 * sensors stayed powered. After IDLE_STOP_S without a consumer the bridge stops its stream
 * (STREAMOFF with the buffers kept, lens subdev closed: the sensor and VCM power down) but keeps
 * the loopback output and the sensor's controls, so Android still sees the camera and the
 * exposure is kept. A consumer (readers_active, or v4l2loopback's client-usage event) starts
 * the stream again.
 *
 * 15 s: longer than a camera switch or a photo/video mode change in an app (the stream is
 * closed for well under 2 s), so those do not cycle the sensor; shorter than Android's
 * shortest screen timeout (15 s) plus the time before it tries to suspend, so by the time the
 * screen is off the sensors are. A stop can take up to ~4 s (IPU6 firmware stop/close
 * timeouts of 2 s each); restarts take the sensor's power-up and first frames.
 * --idle-stop 0 keeps streaming as before. An older v4l2loopback without readers_active
 * (readers -1) never stops: the bridge could not tell when to start again.
 */
#define IDLE_STOP_S_DEFAULT 15.0
static double idle_stop_s = IDLE_STOP_S_DEFAULT;
enum { IDLE_KEEP, IDLE_STOP, IDLE_START };

struct idle_state {
	double since;	/* when the consumer went away; < 0 while one streams */
	int stopped;	/* sensor stream stopped */
	unsigned int stops, starts;
};

static void idle_reset(struct idle_state *s)
{
	memset(s, 0, sizeof(*s));
	s->since = -1;
}

static int idle_decide(struct idle_state *s, int readers, double now, double timeout)
{
	if (readers != 0) {
		s->since = -1;
		return s->stopped ? IDLE_START : IDLE_KEEP;
	}
	if (s->since < 0)
		s->since = now;
	if (!s->stopped && timeout > 0 && now - s->since >= timeout)
		return IDLE_STOP;
	return IDLE_KEEP;
}

/*
 * Idle stop and restart steps go through these, so the host tests can make a step fail or
 * block. Outside the tests they are plain ioctl() and poll().
 */
#ifdef CAMERA_BRIDGE_TEST
static int (*bridge_ioctl_hook)(int fd, unsigned long request, void *arg);
static int (*bridge_poll_hook)(struct pollfd *fds, nfds_t n, int timeout);
static int bridge_ioctl(int fd, unsigned long request, void *arg)
{
	return bridge_ioctl_hook ? bridge_ioctl_hook(fd, request, arg) : ioctl(fd, request, arg);
}
static int bridge_poll(struct pollfd *fds, nfds_t n, int timeout)
{
	return bridge_poll_hook ? bridge_poll_hook(fds, n, timeout) : poll(fds, n, timeout);
}
#else
#define bridge_ioctl ioctl
#define bridge_poll poll
#endif

/*
 * Idle stop/restart watchdog. Boot-13 (2026-11-19): after the first idle stop both bridges went
 * silent and never streamed again, so no app could open a camera. Each step is now logged, and
 * a step that does not return within IDLE_STEP_TIMEOUT_S ends the process with
 * EXIT_IDLE_FAILED: the launcher then starts a fresh bridge with idle stops off for the rest of
 * the boot. (The IPU6 firmware's own stop and close timeouts are 2 s each.) A wait the kernel
 * cannot interrupt still delays the exit until it returns.
 */
#define IDLE_STEP_TIMEOUT_S 12
static unsigned int idle_step_timeout_s = IDLE_STEP_TIMEOUT_S;	/* the tests shorten it */
#define EXIT_IDLE_FAILED 4
/* After a restart the first good frame must come within this, or the restart has failed. */
#define RESTART_FIRST_FRAME_S 5.0
static const char *volatile idle_step = "";

/* woke_at: when the consumer appeared (< 0 once a good frame came after the restart). */
static int idle_restart_overdue(double woke_at, double now)
{
	return woke_at >= 0 && now - woke_at > RESTART_FIRST_FRAME_S;
}

static void idle_watchdog_fired(int sig)
{
	static const char a[] = "ipu6: idle step '", b[] =
		"' blocked; exiting so the launcher restarts the bridge without idle stops\n";
	const char *step = idle_step;

	(void)sig;
	(void)!write(STDERR_FILENO, a, sizeof(a) - 1);
	(void)!write(STDERR_FILENO, step, strlen(step));
	(void)!write(STDERR_FILENO, b, sizeof(b) - 1);
	_exit(EXIT_IDLE_FAILED);
}

/* Name the step about to run and arm the watchdog for it; seconds 0 disarms. */
static void idle_watchdog(const char *step, unsigned int seconds)
{
	struct sigaction sa = { .sa_handler = idle_watchdog_fired };

	idle_step = step;
	if (seconds) {
		sigemptyset(&sa.sa_mask);
		sigaction(SIGALRM, &sa, NULL);	/* no SA_RESTART: interrupt the blocked call */
		printf("bridge: idle step: %s\n", step);
		fflush(stdout);
	}
	alarm(seconds);
}

/* The IPU6 capture node's buffers; started and stopped as a unit. */
struct capture {
	int fd;
	unsigned int count;
	size_t min_size;
	int streaming;
	struct bridge_buffer buffers[3];
};

static void capture_release(struct capture *c)
{
	struct v4l2_requestbuffers request = {
		.count = 0, .type = V4L2_BUF_TYPE_VIDEO_CAPTURE, .memory = V4L2_MEMORY_MMAP
	};

	for (unsigned int i = 0; i < c->count; i++)
		if (c->buffers[i].data) {
			munmap(c->buffers[i].data, c->buffers[i].length);
			c->buffers[i].data = NULL;
		}
	if (c->fd >= 0 && c->count && bridge_ioctl(c->fd, VIDIOC_REQBUFS, &request))
		perror("release IPU6 buffers");
	c->count = 0;
}

static void capture_stop(struct capture *c)
{
	enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

	if (c->streaming && bridge_ioctl(c->fd, VIDIOC_STREAMOFF, &type))
		perror("VIDIOC_STREAMOFF");
	c->streaming = 0;
	capture_release(c);
}

/*
 * Idle stop: STREAMOFF only. That ends the ISYS stream (stream_opened, which blocks system
 * suspend, drops at stream stop) and the sensor's s_stream, while the buffers stay requested and
 * mapped, so the restart is QBUF + STREAMON and avoids REQBUFS on a firmware that has just timed
 * out. 0 on success; on failure the buffers are released too and capture_resume() starts over.
 */
static int capture_pause(struct capture *c)
{
	enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

	if (!c->streaming)
		return 0;
	c->streaming = 0;
	if (bridge_ioctl(c->fd, VIDIOC_STREAMOFF, &type) == 0)
		return 0;
	perror("idle stop: VIDIOC_STREAMOFF");
	capture_release(c);
	return -1;
}

/* Request, map and queue the buffers, then stream on. 0 on success; on failure all released. */
static int capture_start(struct capture *c)
{
	struct v4l2_requestbuffers request = {
		.count = 3, .type = V4L2_BUF_TYPE_VIDEO_CAPTURE, .memory = V4L2_MEMORY_MMAP
	};
	enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

	if (bridge_ioctl(c->fd, VIDIOC_REQBUFS, &request)) {
		perror("VIDIOC_REQBUFS");
		return -1;
	}
	c->count = request.count > 3 ? 3 : request.count;
	if (request.count < 2 || request.count > 3) {
		fprintf(stderr, "unexpected IPU6 buffer count %u\n", request.count);
		goto fail;
	}
	for (unsigned int i = 0; i < c->count; i++) {
		struct v4l2_buffer b = { .type = type, .memory = V4L2_MEMORY_MMAP, .index = i };
		if (bridge_ioctl(c->fd, VIDIOC_QUERYBUF, &b) || b.length < c->min_size ||
		    b.length > MAX_FRAME_BYTES)
			goto fail;
		c->buffers[i].length = b.length;
		c->buffers[i].data = mmap(NULL, b.length, PROT_READ | PROT_WRITE,
					  MAP_SHARED, c->fd, b.m.offset);
		if (c->buffers[i].data == MAP_FAILED) {
			c->buffers[i].data = NULL;
			goto fail;
		}
		if (bridge_ioctl(c->fd, VIDIOC_QBUF, &b))
			goto fail;
	}
	if (bridge_ioctl(c->fd, VIDIOC_STREAMON, &type)) {
		perror("start IPU6 capture");
		goto fail;
	}
	c->streaming = 1;
	return 0;
fail:
	capture_release(c);
	return -1;
}

/*
 * Restart after an idle stop: queue the kept buffers and stream on. If that fails (or the pause
 * already released them), release everything and start from REQBUFS. 0 on success.
 */
static int capture_resume(struct capture *c)
{
	enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	unsigned int i;

	if (c->streaming)
		return 0;
	if (c->count) {
		for (i = 0; i < c->count; i++) {
			struct v4l2_buffer b = { .type = type, .memory = V4L2_MEMORY_MMAP, .index = i };
			if (bridge_ioctl(c->fd, VIDIOC_QBUF, &b)) {
				perror("idle restart: VIDIOC_QBUF");
				break;
			}
		}
		if (i == c->count && bridge_ioctl(c->fd, VIDIOC_STREAMON, &type) == 0) {
			c->streaming = 1;
			return 0;
		}
		if (i == c->count)
			perror("idle restart: VIDIOC_STREAMON");
		fprintf(stderr, "idle restart: kept buffers failed; requesting new ones\n");
		capture_release(c);
	}
	return capture_start(c);
}

/* v4l2loopback's private event: a capture client started or stopped streaming. */
#define LOOPBACK_EVENT_CLIENT_USAGE (V4L2_EVENT_PRIVATE_START + 0x08E00000 + 1)

static int loopback_subscribe_usage(int fd)
{
	struct v4l2_event_subscription sub = { .type = LOOPBACK_EVENT_CLIENT_USAGE };

	return ioctl(fd, VIDIOC_SUBSCRIBE_EVENT, &sub) == 0;
}

/*
 * Dequeue pending client-usage events without ever waiting. The loopback node is opened
 * without O_NONBLOCK, and there VIDIOC_DQEVENT sleeps until the next event: on boot-13 that is
 * where both bridges hung right after the idle stop (no "stream stopped" line; each app open
 * woke the call for one event, and it went back to sleep). So only dequeue while poll() says an
 * event is pending. Returns the number dequeued.
 */
static int loopback_drain_events(int fd)
{
	struct v4l2_event ev;
	int n = 0;

	while (n < 16) {
		struct pollfd p = { .fd = fd, .events = POLLPRI };
		if (bridge_poll(&p, 1, 0) <= 0 || !(p.revents & POLLPRI) ||
		    bridge_ioctl(fd, VIDIOC_DQEVENT, &ev))
			break;
		n++;
	}
	return n;
}

static void write_black(int fd, unsigned char *frame)
{
	for (size_t i = 0; i < OUT_FRAME_BYTES; i += 4) {
		frame[i] = 16;
		frame[i + 1] = 128;
		frame[i + 2] = 16;
		frame[i + 3] = 128;
	}
	if (fd >= 0 && write(fd, frame, OUT_FRAME_BYTES) != (ssize_t)OUT_FRAME_BYTES)
		perror("write black loopback frame");
}

/*
 * Where converted frames go: a v4l2loopback node (the USB camera HAL reads it), or, with the
 * output "channel:<camera id>", the frame channel to the gbOS libcamera plugin
 * (kernel/ipu6-channel.h). The channel is single-consumer like the loopback node, so readers is
 * 0 or 1 and the capture loop (idle, conversion, idle stop) works the same for both.
 */
struct out_sink {
	int channel;
	int fd;				/* loopback node, or the channel's listening socket */
	int client;			/* channel: connected plugin, or -1 */
	int memfd;
	unsigned char *map;
	size_t map_bytes;
	uint64_t seq;
	unsigned int next_slot;
	char name[64];
};

#define CHANNEL_SLOT_BYTES (((OUT_FRAME_BYTES) + 4095) & ~(size_t)4095)

static socklen_t channel_address(const char *name, struct sockaddr_un *a)
{
	memset(a, 0, sizeof(*a));
	a->sun_family = AF_UNIX;
	/* Abstract: sun_path[0] = 0, then the name (no terminator; the length says where it ends). */
	int n = snprintf(a->sun_path + 1, sizeof(a->sun_path) - 1, "%s%s", IPU6_CHANNEL_PREFIX, name);
	return (socklen_t)(offsetof(struct sockaddr_un, sun_path) + 1 + n);
}

static int channel_open(struct out_sink *s, const char *name)
{
	struct sockaddr_un a;
	socklen_t len;

	if (!*name || strlen(name) >= sizeof(s->name) || strchr(name, '/'))
		return -1;
	snprintf(s->name, sizeof(s->name), "%s", name);
	s->channel = 1;
	s->client = -1;
	s->map_bytes = IPU6_CHANNEL_HEADER_BYTES + IPU6_CHANNEL_SLOTS * CHANNEL_SLOT_BYTES;
	s->memfd = memfd_create("gbos-camera-frames", MFD_CLOEXEC);
	if (s->memfd < 0 || ftruncate(s->memfd, (off_t)s->map_bytes)) {
		perror("frame channel: memfd");
		return -1;
	}
	s->map = mmap(NULL, s->map_bytes, PROT_READ | PROT_WRITE, MAP_SHARED, s->memfd, 0);
	if (s->map == MAP_FAILED) {
		s->map = NULL;
		perror("frame channel: mmap");
		return -1;
	}
	s->fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
	len = channel_address(name, &a);
	if (s->fd < 0 || bind(s->fd, (struct sockaddr *)&a, len) || listen(s->fd, 2)) {
		fprintf(stderr, "frame channel: cannot listen on @%s%s: %s\n", IPU6_CHANNEL_PREFIX, name,
			strerror(errno));
		return -1;
	}
	return 0;
}

static void channel_drop_client(struct out_sink *s, const char *why)
{
	if (s->client < 0)
		return;
	close(s->client);
	s->client = -1;
	printf("frame channel: client %s\n", why);
	fflush(stdout);
}

/* A new client: the newest connection wins (a restarted camera service reconnects). */
static void channel_accept(struct out_sink *s)
{
	for (;;) {
		int c = accept4(s->fd, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC);
		if (c < 0)
			return;
		struct ipu6_channel_hello h = {
			.magic = IPU6_CHANNEL_MAGIC, .version = IPU6_CHANNEL_VERSION,
			.width = OUT_WIDTH, .height = OUT_HEIGHT, .slots = IPU6_CHANNEL_SLOTS,
			.slot_bytes = (uint32_t)CHANNEL_SLOT_BYTES, .header_bytes = IPU6_CHANNEL_HEADER_BYTES,
			.map_bytes = s->map_bytes,
		};
		char control[CMSG_SPACE(sizeof(int))];
		struct iovec iov = { .iov_base = &h, .iov_len = sizeof(h) };
		struct msghdr m = { .msg_iov = &iov, .msg_iovlen = 1, .msg_control = control,
				    .msg_controllen = sizeof(control) };
		memset(control, 0, sizeof(control));
		struct cmsghdr *cm = CMSG_FIRSTHDR(&m);
		cm->cmsg_level = SOL_SOCKET;
		cm->cmsg_type = SCM_RIGHTS;
		cm->cmsg_len = CMSG_LEN(sizeof(int));
		memcpy(CMSG_DATA(cm), &s->memfd, sizeof(int));
		if (sendmsg(c, &m, MSG_NOSIGNAL) != (ssize_t)sizeof(h)) {
			fprintf(stderr, "frame channel: hello failed: %s\n", strerror(errno));
			close(c);
			continue;
		}
		channel_drop_client(s, "replaced by a new connection");
		s->client = c;
		s->seq = 0;
		struct ipu6_channel_shared *sh = (struct ipu6_channel_shared *)s->map;
		for (int k = 0; k < IPU6_CHANNEL_SLOTS; k++)
			__atomic_store_n(&sh->seq[k], 0, __ATOMIC_RELEASE);
		printf("frame channel: client connected (%ux%u YUYV, %d slots)\n", OUT_WIDTH, OUT_HEIGHT,
		       IPU6_CHANNEL_SLOTS);
		fflush(stdout);
	}
}

/* 1 while a client is connected. Never blocks. */
static int channel_readers(struct out_sink *s)
{
	channel_accept(s);
	if (s->client >= 0) {
		struct pollfd p = { .fd = s->client, .events = POLLIN };
		if (poll(&p, 1, 0) > 0) {
			char buf[64];
			ssize_t n = recv(s->client, buf, sizeof(buf), MSG_DONTWAIT);
			if ((p.revents & (POLLHUP | POLLERR)) || n == 0 ||
			    (n < 0 && errno != EAGAIN && errno != EINTR))
				channel_drop_client(s, "disconnected");
		}
	}
	return s->client >= 0;
}

static void channel_send(struct out_sink *s, const unsigned char *frame, const struct ipu6_channel_frame *meta)
{
	struct ipu6_channel_shared *sh = (struct ipu6_channel_shared *)s->map;
	unsigned int k = s->next_slot;
	struct ipu6_channel_frame f = *meta;

	if (s->client < 0)
		return;
	s->next_slot = (k + 1) % IPU6_CHANNEL_SLOTS;
	__atomic_store_n(&sh->seq[k], 0, __ATOMIC_RELEASE);
	memcpy(s->map + IPU6_CHANNEL_HEADER_BYTES + k * CHANNEL_SLOT_BYTES, frame, OUT_FRAME_BYTES);
	f.magic = IPU6_CHANNEL_MAGIC;
	f.slot = k;
	f.seq = ++s->seq;
	__atomic_store_n(&sh->seq[k], f.seq, __ATOMIC_RELEASE);
	if (send(s->client, &f, sizeof(f), MSG_DONTWAIT | MSG_NOSIGNAL) < 0 &&
	    errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)
		channel_drop_client(s, "gone (send failed)");
}

/* Opens the output; for a loopback node also sets its format and primes it with black. */
static int sink_open(struct out_sink *s, const char *out_dev)
{
	memset(s, 0, sizeof(*s));
	s->fd = s->client = s->memfd = -1;
	if (!strncmp(out_dev, "channel:", 8))
		return channel_open(s, out_dev + 8);
	s->fd = open(out_dev, O_RDWR | O_CLOEXEC);
	if (s->fd < 0) {
		perror("open loopback node");
		return -1;
	}
	struct v4l2_format outfmt = { .type = V4L2_BUF_TYPE_VIDEO_OUTPUT };
	outfmt.fmt.pix.width = OUT_WIDTH;
	outfmt.fmt.pix.height = OUT_HEIGHT;
	outfmt.fmt.pix.pixelformat = OUT_FOURCC;
	outfmt.fmt.pix.field = V4L2_FIELD_NONE;
	outfmt.fmt.pix.bytesperline = OUT_WIDTH * 2;
	outfmt.fmt.pix.sizeimage = OUT_FRAME_BYTES;
	outfmt.fmt.pix.colorspace = V4L2_COLORSPACE_SRGB;
	if (ioctl(s->fd, VIDIOC_S_FMT, &outfmt) ||
	    outfmt.fmt.pix.pixelformat != OUT_FOURCC ||
	    outfmt.fmt.pix.width != OUT_WIDTH || outfmt.fmt.pix.height != OUT_HEIGHT ||
	    outfmt.fmt.pix.bytesperline != OUT_WIDTH * 2 ||
	    outfmt.fmt.pix.sizeimage < OUT_FRAME_BYTES) {
		perror("configure V4L2 loopback output");
		return -1;
	}
	return 0;
}

static void sink_close(struct out_sink *s)
{
	if (s->client >= 0) close(s->client);
	if (s->fd >= 0) close(s->fd);
	if (s->map) munmap(s->map, s->map_bytes);
	if (s->memfd >= 0) close(s->memfd);
	s->fd = s->client = s->memfd = -1;
	s->map = NULL;
}

static int sink_readers(struct out_sink *s)
{
	return s->channel ? channel_readers(s) : loopback_readers(s->fd);
}

/* 0 or -1 (loopback write failed); a channel frame is dropped rather than fail. */
static int sink_write(struct out_sink *s, const unsigned char *frame, const struct ipu6_channel_frame *meta)
{
	if (s->channel) {
		channel_send(s, frame, meta);
		return 0;
	}
	return write(s->fd, frame, OUT_FRAME_BYTES) == (ssize_t)OUT_FRAME_BYTES ? 0 : -1;
}

/* The loopback node for its client-usage events; -1 for a channel (nothing to drain). */
static int sink_event_fd(const struct out_sink *s)
{
	return s->channel ? -1 : s->fd;
}

/* Stopped: wait up to timeout_ms for a consumer (an event or a connection). <0 on a poll error. */
static int sink_wait(struct out_sink *s, int timeout_ms)
{
	struct pollfd p = { .fd = s->fd, .events = s->channel ? POLLIN : POLLPRI };
	int r = poll(&p, 1, timeout_ms);

	if (r < 0)
		return errno == EINTR ? 0 : -1;
	if (!s->channel && r > 0 && (p.revents & POLLPRI))
		loopback_drain_events(s->fd);
	return 0;
}

/*
 * Frame health: counts a run of bad frames (or poll timeouts) and says when to give up.
 * Returns 1 to give up: nothing good for BAD_FRAME_GIVE_UP_S seconds.
 */
struct frame_health {
	unsigned long bad_run, bad_total, recoveries;
	double last_good;
};

static double monotonic_s(void)
{
	struct timespec t;

	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec + t.tv_nsec / 1e9;
}

static void frame_health_start(struct frame_health *h, double now)
{
	memset(h, 0, sizeof(*h));
	h->last_good = now;
}

static void frame_health_good(struct frame_health *h, double now)
{
	if (h->bad_run) {
		h->recoveries++;
		printf("ipu6: recovered after %lu bad frame(s) in %.1f s (%lu bad in total)\n",
		       h->bad_run, now - h->last_good, h->bad_total);
		fflush(stdout);
	}
	h->bad_run = 0;
	h->last_good = now;
}

/* what: "error-marked", "short" or "timeout" (timeouts are not frames; they only age the run). */
static int frame_health_bad(struct frame_health *h, double now, const char *what,
			    unsigned int flags, unsigned int bytes)
{
	if (strcmp(what, "timeout")) {
		h->bad_run++;
		h->bad_total++;
		if (h->bad_run <= 3 || h->bad_run % 100 == 0)
			fprintf(stderr, "ipu6: %s frame (flags 0x%x, %u bytes), requeued; %lu in this run\n",
				what, flags, bytes, h->bad_run);
	}
	if (now - h->last_good < BAD_FRAME_GIVE_UP_S)
		return 0;
	fprintf(stderr, "ipu6: no good frame for %.1f s (%lu bad frames in this run); giving up\n",
		now - h->last_good, h->bad_run);
	return 1;
}

/*
 * Rear lens focus (OV8856 module 0BA801T3 with a DW9714 voice-coil motor). The VCM driver
 * binds, but nothing opened its subdev, so it stayed runtime-suspended and the lens sat at
 * its rest stop: the whole rear picture was blurred (2026-11-18 boot-11). The bridge keeps
 * the lens subdev open (powered) and runs a contrast-detect scan whenever a consumer starts:
 * coarse steps over the DAC range where the module focuses, then fine steps around the
 * sharpest, holding auto-exposure meanwhile. Sharpness: squared differences between
 * same-colour sites two pixels apart in the central ninth, relative to its brightness.
 * The module's Windows tuning (optomechanics record 13) lists 278, 400 and 1023, which we
 * read as the infinity and macro DAC codes and the maximum (not confirmed); AF_START is a
 * thin-lens estimate for about 1.5 m (f = 2.83 mm, same record) used until a scan ends.
 */
#define AF_START 286
#define AF_COARSE_MIN 230
#define AF_COARSE_MAX 470
#define AF_COARSE_STEP 20
#define AF_FINE_STEP 5
#define AF_SETTLE 2	/* frames after a move before measuring (lens and exposure lag) */

struct af_state {
	int fd, present, min, max, value;
	int phase;	/* 0 idle, 1 coarse scan, 2 fine scan */
	int pos, hi, step, settle, best_pos, coarse_best;
	double best_score;
};

static int af_move(struct af_state *af, int pos)
{
	struct v4l2_control c = { .id = V4L2_CID_FOCUS_ABSOLUTE };

	if (pos < af->min) pos = af->min;
	if (pos > af->max) pos = af->max;
	c.value = pos;
#ifdef CAMERA_BRIDGE_TEST
	if (af->fd < 0) {
		af->value = pos;
		return 0;
	}
#endif
	if (ioctl(af->fd, VIDIOC_S_CTRL, &c)) {
		perror("af: set lens position; focus control off");
		af->present = 0;
		af->phase = 0;
		return -1;
	}
	af->value = pos;
	return 0;
}

/* Open the lens subdev and take its FOCUS_ABSOLUTE range; absent lens = no AF (front). */
static void af_init(struct af_state *af, const char *lens_path)
{
	struct v4l2_queryctrl q = { .id = V4L2_CID_FOCUS_ABSOLUTE };

	memset(af, 0, sizeof(*af));
	af->fd = -1;
	if (!lens_path)
		return;
	af->fd = open(lens_path, O_RDWR | O_CLOEXEC);
	if (af->fd < 0 || ioctl(af->fd, VIDIOC_QUERYCTRL, &q) || q.maximum <= q.minimum) {
		perror("af: lens subdev unusable; fixed focus");
		if (af->fd >= 0)
			close(af->fd);
		af->fd = -1;
		return;
	}
	af->present = 1;
	af->min = q.minimum;
	af->max = q.maximum;
	if (af_move(af, AF_START) == 0)
		printf("af: lens %s range %d..%d, start %d\n", lens_path, af->min, af->max, af->value);
}

static void af_start(struct af_state *af)
{
	if (!af->present)
		return;
	af->phase = 1;
	af->step = AF_COARSE_STEP;
	af->hi = AF_COARSE_MAX;
	af->best_score = -1;
	af->best_pos = af->value;
	af->settle = AF_SETTLE;
	af_move(af, AF_COARSE_MIN);
	af->pos = af->value;
}

static double af_sharpness(const uint16_t *raw, unsigned int width, unsigned int height,
			   unsigned int stride)
{
	unsigned int x0 = width / 3, x1 = 2 * width / 3, y0 = height / 3, y1 = 2 * height / 3;
	double sum = 0, level = 0;
	unsigned long n = 0;

	for (unsigned int y = y0; y + 2 < y1; y += 2)
		for (unsigned int x = x0; x + 2 < x1; x++) {
			double v = raw_at(raw, width, height, stride, (int)x, (int)y);
			double dx = v - raw_at(raw, width, height, stride, (int)x + 2, (int)y);
			double dy = v - raw_at(raw, width, height, stride, (int)x, (int)y + 2);
			sum += dx * dx + dy * dy;
			level += v;
			n++;
		}
	if (!n)
		return 0;
	level /= n;
	return sum / n / (level * level + 1.0);
}

/* One converted frame. Returns 1 while a scan runs (hold AE). */
static int af_frame(struct af_state *af, const uint16_t *raw, unsigned int width,
		    unsigned int height, unsigned int stride)
{
	if (!af->present || !af->phase)
		return 0;
	if (af->settle > 0) {
		af->settle--;
		return 1;
	}
	double score = af_sharpness(raw, width, height, stride);
	if (score > af->best_score) {
		af->best_score = score;
		af->best_pos = af->pos;
	}
	if (af->pos + af->step <= af->hi) {
		af_move(af, af->pos + af->step);
		af->pos = af->value;
		af->settle = AF_SETTLE;
		return af->present;
	}
	if (af->phase == 1) {
		af->coarse_best = af->best_pos;
		af->phase = 2;
		af->step = AF_FINE_STEP;
		af->hi = af->coarse_best + AF_COARSE_STEP - AF_FINE_STEP;
		af->best_score = -1;
		af_move(af, af->coarse_best - AF_COARSE_STEP + AF_FINE_STEP);
		af->pos = af->value;
		af->settle = AF_SETTLE;
		return af->present;
	}
	af->phase = 0;
	af_move(af, af->best_pos);
	printf("af: focus at %d (coarse %d, sharpness %.4g)\n", af->value, af->coarse_best,
	       af->best_score);
	fflush(stdout);
	return 0;
}

/* The lens entity a sensor's ancillary link points to, as a /dev path; 0 if found. */
static int find_lens(const struct graph *g, const struct media_v2_topology *t, __u32 sensor_id,
		     char *path, size_t size)
{
	for (__u32 i = 0; i < t->num_links; i++) {
		const struct media_v2_link *l = &g->links[i];
		if ((l->flags & MEDIA_LNK_FL_LINK_TYPE) == MEDIA_LNK_FL_ANCILLARY_LINK &&
		    l->source_id == sensor_id)
			return device_path(g, t, l->sink_id, path, size);
	}
	return -1;
}

/*
 * The idle stop, one watched and logged step at a time: STREAMOFF (buffers kept), close the lens
 * (the VCM powers down), drain the loopback's usage events without waiting. Returns 0 when
 * STREAMOFF worked; on -1 the buffers were released and the restart requests new ones.
 */
static int idle_pause_stream(struct capture *cap, struct af_state *af, int outfd)
{
	double t0 = monotonic_s();

	idle_watchdog("stop: STREAMOFF", idle_step_timeout_s);
	int r = capture_pause(cap);
	printf("bridge: idle stop: STREAMOFF %s in %.2f s\n",
	       r ? "failed, buffers released" : "done, buffers kept", monotonic_s() - t0);
	if (monotonic_s() - t0 > 3.0)
		printf("bridge: idle stop: slow STREAMOFF (IPU6 firmware stop/close timeouts?)\n");
	if (af->fd >= 0) {
		idle_watchdog("stop: close lens", idle_step_timeout_s);
		close(af->fd);
	}
	af->fd = -1;
	af->present = 0;
	af->phase = 0;
	idle_watchdog("stop: drain loopback events", idle_step_timeout_s);
	int events = loopback_drain_events(outfd);
	idle_watchdog("", 0);
	printf("bridge: idle stop: %d usage event%s drained; total %.2f s\n", events,
	       events == 1 ? "" : "s", monotonic_s() - t0);
	fflush(stdout);
	return r;
}

/* The restart, watched and logged: QBUF the kept buffers and STREAMON (or start over). */
static int idle_resume_stream(struct capture *cap)
{
	double t0 = monotonic_s();

	idle_watchdog("restart: QBUF and STREAMON", idle_step_timeout_s);
	int r = capture_resume(cap);
	idle_watchdog("", 0);
	printf("bridge: idle restart: stream %s in %.2f s\n", r ? "failed" : "on", monotonic_s() - t0);
	fflush(stdout);
	return r;
}

/*
 * Raw dumps for tuning on the build machine (CAMERA_RAW_DUMP=N in gbos.cfg; the launcher passes
 * IPU6_RAW_DUMP_DIR and IPU6_RAW_DUMP_MAX). About one second into an app's session the bridge
 * writes the raw frame, the YUYV picture it made from it, and its state, as
 * <dir>/raw-<sensor>-<n>.{raw,yuyv,txt}; tools/ipu6_raw_render.c renders a dump with the CPU
 * (and GPU) path and compares it with the saved picture. At most IPU6_RAW_DUMP_MAX per run.
 */
static const char *raw_dump_dir;
static int raw_dump_left;

/*
 * The files are written by a detached thread from copies, never from the frame loop: boot-20's
 * first rear dump (16 MB raw + 4 MB YUYV onto the FAT stick) stalled the loop long enough for
 * the USB HAL to give up ("v4l2 dequeue timeout", "serious device error"). One dump at a time;
 * one requested while another is still being written is skipped.
 */
struct raw_dump_job {
	char base[480];
	void *raw, *yuyv;
	size_t raw_size;
	char info[1024];
};
static volatile int raw_dump_busy;

static void raw_dump_write(const char *path, const void *data, size_t size)
{
	FILE *f = fopen(path, "wb");

	if (f) {
		fwrite(data, 1, size, f);
		fclose(f);
	}
}

static void *raw_dump_thread(void *arg)
{
	struct raw_dump_job *job = arg;
	char path[512];

	snprintf(path, sizeof(path), "%s.raw", job->base);
	raw_dump_write(path, job->raw, job->raw_size);
	snprintf(path, sizeof(path), "%s.yuyv", job->base);
	raw_dump_write(path, job->yuyv, OUT_FRAME_BYTES);
	snprintf(path, sizeof(path), "%s.txt", job->base);	/* last: the dump is complete */
	raw_dump_write(path, job->info, strlen(job->info));
	printf("raw dump: %s.{raw,yuyv,txt} written\n", job->base);
	fflush(stdout);
	free(job->raw);
	free(job->yuyv);
	free(job);
	__atomic_store_n(&raw_dump_busy, 0, __ATOMIC_RELEASE);
	return NULL;
}

/* Returns 0 when a dump was started (or written, with wait set: the host tests). */
static int raw_dump_start(const char *sensor, const uint16_t *raw, unsigned int width,
			  unsigned int height, unsigned int stride, const unsigned char *yuyv,
			  const struct ae_state *ae, int wait)
{
	static int n;
	struct raw_dump_job *job;
	pthread_t thread;

	if (!raw_dump_dir || raw_dump_left <= 0 || __atomic_load_n(&raw_dump_busy, __ATOMIC_ACQUIRE))
		return -1;
	job = calloc(1, sizeof(*job));
	if (!job)
		return -1;
	job->raw_size = (size_t)stride * height;
	job->raw = malloc(job->raw_size);
	job->yuyv = malloc(OUT_FRAME_BYTES);
	if (!job->raw || !job->yuyv) {
		free(job->raw);
		free(job->yuyv);
		free(job);
		return -1;
	}
	memcpy(job->raw, raw, job->raw_size);
	memcpy(job->yuyv, yuyv, OUT_FRAME_BYTES);
	raw_dump_left--;
	n++;
	snprintf(job->base, sizeof(job->base), "%s/raw-%s-%d", raw_dump_dir, sensor, n);
	snprintf(job->info, sizeof(job->info),
		 "sensor %s\nwidth %u\nheight %u\nstride %u\nout %dx%d\nrotate %d\n"
		 "gain %.4f\nexposure %d\nagain %d\ndgain %d\nvblank %d\n"
		 "wb_white %.5f %.5f\nwb_mean %.2f %.2f %.2f\nwb_gain %u %u\nnir_ratio %.4f\n"
		 "ccm_active %d\nccm_pick %d %d %.3f\nlsc_active %d\nlsc_pick %d %d %.3f %.3f\n"
		 "converter %s\n",
		 sensor, width, height, stride, OUT_WIDTH, OUT_HEIGHT, output_rotate180,
		 sensor_total_gain, ae->exposure.value, ae->again.value, ae->dgain.value, ae->vblank.value,
		 wb_white_rg, wb_white_bg, wb_last_mean[0], wb_last_mean[1], wb_last_mean[2],
		 wb_last_gain_q12[0], wb_last_gain_q12[2], wb_nir_ratio,
		 ccm_active, ccm_pick[0], ccm_pick[1], ccm_weight, lsc_active, lsc_pick[0], lsc_pick[1],
		 lsc_weight, lsc_strength,
#ifdef IPU6_GPU
		 gpu.ok ? "gpu" : "cpu"
#else
		 "cpu"
#endif
		);
	__atomic_store_n(&raw_dump_busy, 1, __ATOMIC_RELEASE);
	printf("raw dump: %s (writing in the background)\n", job->base);
	if (wait) {
		raw_dump_thread(job);
		return 0;
	}
	if (pthread_create(&thread, NULL, raw_dump_thread, job)) {
		raw_dump_thread(job);	/* no thread: write now rather than lose it */
		return 0;
	}
	pthread_detach(thread);
	return 0;
}

static void raw_dump(const char *sensor, const uint16_t *raw, unsigned int width, unsigned int height,
		     unsigned int stride, const unsigned char *yuyv, const struct ae_state *ae)
{
	raw_dump_start(sensor, raw, width, height, stride, yuyv, ae, 0);
}

/*
 * What a frame channel client reports to Android for this frame: the state AE has set (the
 * sensor applies a change a frame or two later, so this lags a step while AE moves).
 */
static void frame_meta(struct ipu6_channel_frame *m, const struct ae_state *ae, const struct v4l2_buffer *b)
{
	struct timespec now;
	double line_us;

	memset(m, 0, sizeof(*m));
	if (b->timestamp.tv_sec || b->timestamp.tv_usec) {
		m->timestamp_ns = (int64_t)b->timestamp.tv_sec * 1000000000 + (int64_t)b->timestamp.tv_usec * 1000;
	} else {
		clock_gettime(CLOCK_MONOTONIC, &now);
		m->timestamp_ns = (int64_t)now.tv_sec * 1000000000 + now.tv_nsec;
	}
	/* The default frame (height + default VBLANK lines) is 1/30 s; ae_frame_ms assumes the same. */
	line_us = ae->height > 0 ? 1e6 / 30 / (ae->height + (ae->vblank.present ? ae->vblank.def : 0)) : 0;
	m->exposure_us = (int32_t)(ae->exposure.value * line_us + 0.5);
	m->frame_duration_us = (int32_t)(ae_frame_ms(ae) * 1000 + 0.5);
	m->analogue_gain = ae->again.min > 0 ? (float)ae->again.value / ae->again.min : 1.0f;
	m->digital_gain = ae->dgain.present && ae->dgain.min > 0 ? (float)ae->dgain.value / ae->dgain.min : 1.0f;
	m->wb_gain_r = wb_last_gain_q12[0] ? wb_last_gain_q12[0] / 4096.0f : 1.0f;
	m->wb_gain_b = wb_last_gain_q12[2] ? wb_last_gain_q12[2] / 4096.0f : 1.0f;
}

static int capture_to_loopback(const char *media_dev, const char *out_dev,
			       const struct sensor_profile *profile)
{
	struct ae_state ae = { .fd = -1 };
	struct af_state af = { .fd = -1 };
	char lens_path[128];
	struct graph graph = { 0 };
	struct media_v2_topology topology;
	struct media_device_info info = { 0 };
	struct path path;
	char sensor_path[128], csi_path[128], capture_path[128];
	int mfd = -1, sfd = -1, cfd = -1, vfd = -1;
	struct out_sink sink = { .fd = -1, .client = -1, .memfd = -1 };
	int linked = 0, result = 1, have_lens = 0;
	struct capture cap = { .fd = -1 };
	struct bridge_buffer *buffers = cap.buffers;
	struct v4l2_subdev_format sensor = { 0 }, saved_sink = { 0 }, set;
	struct v4l2_format video = { .type = V4L2_BUF_TYPE_VIDEO_CAPTURE };
	struct v4l2_format saved_video = { .type = V4L2_BUF_TYPE_VIDEO_CAPTURE };
	unsigned int frame_count = 0, session_frames = 0;
	int last_readers = -2;
	unsigned char *output = NULL;
	enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	struct idle_state idle;
	double woke_at = -1;	/* consumer appeared while stopped; cleared at the first good frame */

	idle_reset(&idle);

	mfd = open(media_dev, O_RDWR | O_CLOEXEC);
	if (mfd < 0 || ioctl(mfd, MEDIA_IOC_DEVICE_INFO, &info) ||
	    strncmp(info.driver, "intel-ipu6", sizeof(info.driver)) ||
	    read_graph(mfd, &graph, &topology) ||
	    find_path(&graph, &topology, profile->entity, &path) ||
	    device_path(&graph, &topology, path.sensor->id, sensor_path, sizeof(sensor_path)) ||
	    device_path(&graph, &topology, path.csi->id, csi_path, sizeof(csi_path)) ||
	    device_path(&graph, &topology, path.video->id, capture_path, sizeof(capture_path))) {
		fprintf(stderr, "cannot discover a unique %s IPU6 capture path\n", profile->label);
		goto done;
	}
	printf("IPU6 path (%s): %s -> %s -> %s; output=%s\n", profile->label,
	       sensor_path, csi_path, capture_path, out_dev);
	sfd = open(sensor_path, O_RDWR | O_CLOEXEC);
	cfd = open(csi_path, O_RDWR | O_CLOEXEC);
	vfd = open(capture_path, O_RDWR | O_NONBLOCK | O_CLOEXEC);
	if (sfd < 0 || cfd < 0 || vfd < 0) {
		perror("open camera nodes");
		goto done;
	}
	if (sink_open(&sink, out_dev))
		goto done;
	if (subformat(sfd, path.sensor_pad->index, &sensor, VIDIOC_SUBDEV_G_FMT) ||
	    subformat(cfd, path.csi_sink->index, &saved_sink, VIDIOC_SUBDEV_G_FMT) ||
	    ioctl(vfd, VIDIOC_G_FMT, &saved_video)) {
		perror("read existing camera formats");
		goto done;
	}
	__u32 fourcc = pixel_format(sensor.format.code);
	if (select_cfa(profile, sensor.format.code))
		goto done;
	if (!fourcc || setup_link(mfd, &path, path.output->flags | MEDIA_LNK_FL_ENABLED))
		goto done;
	linked = 1;
	set = sensor;
	if (subformat(cfd, path.csi_sink->index, &set, VIDIOC_SUBDEV_S_FMT) ||
	    !same_subformat(&set, &sensor))
		goto done;
	memset(&set, 0, sizeof(set));
	if (subformat(cfd, path.csi_source->index, &set, VIDIOC_SUBDEV_G_FMT) ||
	    !same_subformat(&set, &sensor))
		goto done;
	video.fmt.pix.width = sensor.format.width;
	video.fmt.pix.height = sensor.format.height;
	video.fmt.pix.pixelformat = fourcc;
	video.fmt.pix.field = V4L2_FIELD_NONE;
	if (ioctl(vfd, VIDIOC_S_FMT, &video) ||
	    video.fmt.pix.pixelformat != fourcc ||
	    video.fmt.pix.width != sensor.format.width ||
	    video.fmt.pix.height != sensor.format.height ||
	    (uint64_t)video.fmt.pix.bytesperline < (uint64_t)video.fmt.pix.width * 2 ||
	    (uint64_t)video.fmt.pix.sizeimage <
		(uint64_t)video.fmt.pix.bytesperline * video.fmt.pix.height) {
		fprintf(stderr, "IPU6 rejected or adjusted raw capture format\n");
		goto done;
	}
	printf("raw format=0x%x %ux%u stride=%u image=%u\n", fourcc,
	       video.fmt.pix.width, video.fmt.pix.height,
	       video.fmt.pix.bytesperline, video.fmt.pix.sizeimage);
	if (video.fmt.pix.sizeimage > MAX_FRAME_BYTES)
		goto done;
	cap.fd = vfd;
	cap.min_size = video.fmt.pix.sizeimage;
	if (capture_start(&cap))
		goto done;
	output = malloc(OUT_FRAME_BYTES);
	if (!output)
		goto done;
	for (size_t i = 0; i < OUT_FRAME_BYTES; i += 4) {
		output[i] = 16;
		output[i + 1] = 128;
		output[i + 2] = 16;
		output[i + 3] = 128;
	}
	if (!sink.channel && sink_write(&sink, output, NULL)) {
		perror("prime V4L2 loopback");
		goto done;
	}
	ae_init(&ae, sfd, (int)sensor.format.height);
	have_lens = !find_lens(&graph, &topology, path.sensor->id, lens_path, sizeof(lens_path));
	af_init(&af, have_lens ? lens_path : NULL);
	signal(SIGINT, stop_bridge);
	signal(SIGTERM, stop_bridge);
	int usage_events = sink.channel || loopback_subscribe_usage(sink.fd);
	printf("bridge active: %ux%u %s to %ux%u YUYV%s%s%s%s; ", video.fmt.pix.width, video.fmt.pix.height,
	       profile->rgbir ? "RGB-IR" : "Bayer", OUT_WIDTH, OUT_HEIGHT,
	       output_rotate180 ? ", rotated 180" : "",
	       sink.channel ? " via the frame channel @" IPU6_CHANNEL_PREFIX : "",
	       sink.channel ? sink.name : "", sink.channel ? " (libcamera)" : "");
	if (idle_stop_s > 0)
		printf("sensor stops after %.0f s without a consumer (wake-up: %s)\n", idle_stop_s,
		       sink.channel ? "channel connection" :
		       usage_events ? "client-usage event" : "polling readers_active");
	else
		printf("sensor remains streaming until stop\n");
	result = 0;
	struct frame_health health;
	int have_output = 0;
	frame_health_start(&health, monotonic_s());
	while (running) {
		if (idle.stopped) {
			/*
			 * Stream stopped: wait for a consumer on the loopback side. The client-usage
			 * event wakes us at once; readers_active is checked on every wake-up and
			 * at least every second (every 250 ms without the event).
			 */
			if (sink_wait(&sink, usage_events ? 1000 : 250)) {
				perror("poll loopback or frame channel");
				result = 1;
				break;
			}
			int readers = sink_readers(&sink);
			double now = monotonic_s();
			if (idle_decide(&idle, readers, now, idle_stop_s) != IDLE_START)
				continue;
			woke_at = now;
			printf("bridge: consumer streaming; restarting the sensor stream (start %u)\n",
			       idle.starts + 1);
			fflush(stdout);
			/* A frame at once, so the consumer is not left waiting; real ones follow. */
			have_output = 0;
			write_black(sink.channel ? -1 : sink.fd, output);
			idle_watchdog("restart: active frame controls", idle_step_timeout_s);
			ae_set_idle(&ae, 0);	/* the planned frame, before the sensor powers up */
			if (idle_resume_stream(&cap)) {
				fprintf(stderr, "ipu6: sensor stream restart failed; exiting so the launcher "
					"restarts the bridge without idle stops\n");
				result = EXIT_IDLE_FAILED;
				break;
			}
			idle.stopped = 0;
			idle.starts++;
			if (have_lens)
				af_init(&af, lens_path);
			last_readers = -2;	/* redo the consumer transition: TNR reset, AF scan */
			frame_health_start(&health, monotonic_s());
			continue;
		}
		struct pollfd pfd = { .fd = vfd, .events = POLLIN | POLLPRI };
		int ready = poll(&pfd, 1, 1000);
		if (ready < 0) {
			if (errno == EINTR) continue;
			perror("poll IPU6");
			result = 1;
			break;
		}
		if (idle_restart_overdue(woke_at, monotonic_s())) {
			fprintf(stderr, "ipu6: no good sensor frame %.1f s after the idle restart; exiting so "
				"the launcher restarts the bridge without idle stops\n",
				monotonic_s() - woke_at);
			result = EXIT_IDLE_FAILED;
			break;
		}
		if (!ready) {
			if (frame_health_bad(&health, monotonic_s(), "timeout", 0, 0)) {
				result = 3;
				break;
			}
			continue;
		}
		struct v4l2_buffer b = { .type = type, .memory = V4L2_MEMORY_MMAP };
		if (ioctl(vfd, VIDIOC_DQBUF, &b)) {
			if (errno == EAGAIN || errno == EINTR) continue;
			perror("dequeue IPU6 frame");
			result = 1;
			break;
		}
		if (b.index >= cap.count) {
			fprintf(stderr, "invalid IPU6 buffer index %u\n", b.index);
			result = 1;
			break;
		}
		int readers = sink_readers(&sink);
		if (readers != last_readers) {
			printf("bridge: %s\n", readers < 0 ? "readers_active unavailable; converting every frame" :
			       readers ? "consumer streaming; converting frames" :
			       idle_stop_s > 0 ? "no consumer; idle (sensor and auto-exposure keep running until the idle stop)" :
			       "no consumer; idle (sensor and auto-exposure keep running)");
			last_readers = readers;
			session_frames = 0;
			tnr_reset();	/* the scene may have changed while idle */
			ae_set_idle(&ae, !bridge_should_convert(readers));
			if (bridge_should_convert(readers)) {
				ae_wake(&ae);
				af_start(&af);
			}
		}
		if (idle_decide(&idle, readers, monotonic_s(), idle_stop_s) == IDLE_STOP) {
			double t0 = monotonic_s();
			printf("bridge: no consumer for %.0f s; stopping the sensor stream (stop %u)\n",
			       idle_stop_s, idle.stops + 1);
			fflush(stdout);
			idle_pause_stream(&cap, &af, sink_event_fd(&sink));	/* the dequeued buffer is requeued on restart */
			idle.stopped = 1;
			idle.stops++;
			printf("bridge: sensor stream stopped in %.1f s; sensor%s powered down, waiting for a consumer\n",
			       monotonic_s() - t0, have_lens ? " and lens" : "");
			fflush(stdout);
			continue;
		}
		const char *bad = (b.flags & V4L2_BUF_FLAG_ERROR) ? "error-marked" :
			b.bytesused < (uint64_t)video.fmt.pix.bytesperline * video.fmt.pix.height ||
			b.bytesused > buffers[b.index].length ? "short" : NULL;
		if (bad) {
			int give_up = frame_health_bad(&health, monotonic_s(), bad, b.flags, b.bytesused);
			/* Keep the consumer fed with the last picture rather than nothing. */
			if (!give_up && have_output && bridge_should_convert(readers) && !sink.channel &&
			    sink_write(&sink, output, NULL)) {
				fprintf(stderr, "write loopback frame failed: %s\n", strerror(errno));
				give_up = 1;
			}
			if (give_up || ioctl(vfd, VIDIOC_QBUF, &b)) {
				if (!give_up)
					perror("requeue IPU6 frame");
				result = give_up ? 3 : 1;
				break;
			}
			continue;
		}
		frame_health_good(&health, monotonic_s());
		if (woke_at >= 0) {
			printf("bridge: first sensor frame %.2f s after the consumer appeared\n",
			       monotonic_s() - woke_at);
			fflush(stdout);
			woke_at = -1;
		}
		double mean_green = 0;
		struct ipu6_channel_frame meta;
		frame_meta(&meta, &ae, &b);
		if (!bridge_should_convert(readers)) {
			/* Idle: only the cheap sampled statistics, so exposure is right on resume. */
			unsigned int unused_gain[3];
			calculate_white_balance(buffers[b.index].data, video.fmt.pix.width,
						video.fmt.pix.height, video.fmt.pix.bytesperline,
						unused_gain, &mean_green);
		} else if (convert_bayer_yuyv(buffers[b.index].data, video.fmt.pix.width,
					      video.fmt.pix.height, video.fmt.pix.bytesperline,
					      output, OUT_FRAME_BYTES, &mean_green) ||
			   sink_write(&sink, output, &meta)) {
			fprintf(stderr, "convert or write loopback frame failed: %s\n", strerror(errno));
			result = 1;
			break;
		} else {
			have_output = 1;
		}
		int af_busy = bridge_should_convert(readers) &&
			af_frame(&af, buffers[b.index].data, video.fmt.pix.width,
				 video.fmt.pix.height, video.fmt.pix.bytesperline);
		if (!af_busy)
			ae_frame(&ae, mean_green);
		frame_count++;
		/* Colour state while an app uses the camera: ~1 s in, then every 300 frames. */
		int converting = bridge_should_convert(readers);
		if (converting)
			session_frames++;
		if (frame_count == 30 || frame_count % 900 == 0 ||
		    (converting && (session_frames == 30 || session_frames % 300 == 0)))
			log_cfa_phases(buffers[b.index].data, video.fmt.pix.width,
				       video.fmt.pix.height, video.fmt.pix.bytesperline, frame_count);
		if (converting && session_frames == 30)
			raw_dump(profile->cal_prefix, buffers[b.index].data, video.fmt.pix.width,
				 video.fmt.pix.height, video.fmt.pix.bytesperline, output, &ae);
		if (ioctl(vfd, VIDIOC_QBUF, &b)) {
			perror("requeue IPU6 frame");
			result = 1;
			break;
		}
	}
done:
	if (cap.fd >= 0)
		capture_stop(&cap);
	if (af.fd >= 0)
		close(af.fd);
	if (ae.fd >= 0)
		ae_restore(&ae);
	if (vfd >= 0) {
		if (saved_video.fmt.pix.width) {
			if (ioctl(vfd, VIDIOC_S_FMT, &saved_video))
				perror("restore IPU6 video format");
		}
	}
	if (cfd >= 0 && saved_sink.format.width) {
		set = saved_sink;
		if (ioctl(cfd, VIDIOC_SUBDEV_S_FMT, &set))
			perror("restore IPU6 CSI sink format");
	}
	if (linked && setup_link(mfd, &path, path.output->flags))
		fprintf(stderr, "could not restore IPU6 media link; reboot required\n");
	free(output);
	sink_close(&sink);
	if (vfd >= 0) close(vfd);
	if (cfd >= 0) close(cfd);
	if (sfd >= 0) close(sfd);
	if (mfd >= 0) close(mfd);
	free_graph(&graph);
	return result;
}

/*
 * Optional calibration: a directory holding <prefix>_ccm.txt and <prefix>_lsc.txt (the
 * stick's gbos/camera/; prefix from the sensor profile), or, as before, a single
 * colour-matrix file.
 */
static void load_calibration(const char *arg, const char *prefix)
{
	struct stat st;
	char path[512];
	const char *ccm = arg;

	if (stat(arg, &st) == 0 && S_ISDIR(st.st_mode)) {
		snprintf(path, sizeof(path), "%s/%s_lsc.txt", arg, prefix);
		if (load_lsc_table(path) > 0)
			printf("lsc: loaded %d illuminants (%dx%d grids) from %s\n", lsc_count,
			       lsc_table[0].gw, lsc_table[0].gh, path);
		else
			printf("lsc: no usable table at %s; no lens-shading correction\n", path);
		snprintf(path, sizeof(path), "%s/%s_ccm.txt", arg, prefix);
		ccm = path;
	}
	if (load_ccm_table(ccm) > 0)
		printf("ccm: loaded %d calibrated illuminants from %s\n", ccm_count, ccm);
	else
		printf("ccm: no usable table at %s; generic saturation\n", ccm);
}

struct bridge_options {
	const struct sensor_profile *profile;
	int rotate180;
	double idle_stop;	/* seconds without a consumer before the stream stops; 0 = never */
	int idle_stop_given;
	const char *media, *output, *calibration;
};

/*
 * [--sensor ov5675|ov8856] [--rotate 0|180] [--idle-stop SECONDS] /dev/mediaN /dev/videoN
 * [calibration]. Without options: the front sensor, unrotated, idle stop after 15 s.
 */
static int parse_bridge_args(int argc, char **argv, struct bridge_options *o)
{
	int i = 1;

	memset(o, 0, sizeof(*o));
	o->profile = &sensor_profiles[0];
	o->idle_stop = IDLE_STOP_S_DEFAULT;
	for (; i + 1 < argc && !strncmp(argv[i], "--", 2); i += 2) {
		if (!strcmp(argv[i], "--sensor")) {
			o->profile = find_sensor_profile(argv[i + 1]);
			if (!o->profile)
				return -1;
		} else if (!strcmp(argv[i], "--rotate")) {
			if (!strcmp(argv[i + 1], "180"))
				o->rotate180 = 1;
			else if (strcmp(argv[i + 1], "0"))
				return -1;
		} else if (!strcmp(argv[i], "--idle-stop")) {
			char *end;
			long v = strtol(argv[i + 1], &end, 10);
			if (end == argv[i + 1] || *end || v < 0 || v > 86400)
				return -1;
			o->idle_stop = (double)v;
			o->idle_stop_given = 1;
		} else {
			return -1;
		}
	}
	if (argc - i != 2 && argc - i != 3)
		return -1;
	o->media = argv[i];
	o->output = argv[i + 1];
	o->calibration = argc - i == 3 ? argv[i + 2] : NULL;
	return 0;
}

/*
 * The launcher passes gbos.cfg's CAMERA_IDLE_STOP as IPU6_IDLE_STOP (0 = stream forever, as
 * before); --idle-stop on the command line wins. Returns 0 when the value was taken.
 */
static int idle_stop_from_env(const char *env, double *seconds)
{
	char *end;
	long v;

	if (!env || !*env)
		return -1;
	v = strtol(env, &end, 10);
	if (*end || v < 0 || v > 86400) {
		fprintf(stderr, "ignoring IPU6_IDLE_STOP=%s (seconds expected)\n", env);
		return -1;
	}
	*seconds = (double)v;
	return 0;
}

#ifndef CAMERA_BRIDGE_TEST
int main(int argc, char **argv)
{
	struct bridge_options o;

	(void)probe;
#ifdef IPU6_GPU
	/* --probe-gpu: can this build convert on the GPU here? (the launcher asks before using it) */
	if (argc == 2 && !strcmp(argv[1], "--probe-gpu")) {
		if (gpu_init())
			return 1;
		printf("gpu: Vulkan compute on %s\n", gpu.name);
		return 0;
	}
#endif
	if (parse_bridge_args(argc, argv, &o)) {
		fprintf(stderr, "usage: %s [--sensor ov5675|ov8856] [--rotate 0|180] [--idle-stop SECONDS] "
			"/dev/mediaN /dev/videoN|channel:CAMERA_ID [calibration dir or colour-matrix table]\n", argv[0]);
		return 2;
	}
	setvbuf(stdout, NULL, _IOLBF, 0);
	output_rotate180 = o.rotate180;
	idle_stop_s = o.idle_stop;
	if (!o.idle_stop_given)
		idle_stop_from_env(getenv("IPU6_IDLE_STOP"), &idle_stop_s);
	printf("sensor: %s (calibration prefix %s)%s\n", o.profile->label, o.profile->cal_prefix,
	       o.rotate180 ? "; output rotated 180" : "");
	if (o.calibration)
		load_calibration(o.calibration, o.profile->cal_prefix);
	raw_dump_dir = getenv("IPU6_RAW_DUMP_DIR");
	if (raw_dump_dir && getenv("IPU6_RAW_DUMP_MAX"))
		raw_dump_left = atoi(getenv("IPU6_RAW_DUMP_MAX"));
	if (raw_dump_dir && raw_dump_left > 0)
		printf("raw dumps: up to %d into %s\n", raw_dump_left, raw_dump_dir);
#ifdef IPU6_GPU
	/* IPU6_GPU=0 (gbos.cfg CAMERA_GPU=0, via the launcher) keeps this build on the CPU. */
	const char *use_gpu = getenv("IPU6_GPU");
	if (use_gpu && !strcmp(use_gpu, "0"))
		printf("gpu: off (IPU6_GPU=0); converting on the CPU\n");
	else if (gpu_init() == 0)
		printf("gpu: converting on %s (Vulkan compute)\n", gpu.name);
#else
	printf("gpu: not in this build; converting on the CPU\n");
#endif
	return capture_to_loopback(o.media, o.output, o.profile);
}
#endif
