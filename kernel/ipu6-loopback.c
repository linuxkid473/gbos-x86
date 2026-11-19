#define _GNU_SOURCE
#define CAMERA_CAPTURE_NO_MAIN
#include "../tools/camera_capture_probe.c"

#include <math.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define OUT_WIDTH 640
#define OUT_HEIGHT 480
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
 * While no consumer reads a bridge, its sensor keeps streaming (the IPU6 firmware cannot stop
 * cleanly) but with a frame 8x the default (about 3.75 fps): two full-rate streams at once
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

static void set_rgbir_pattern(void)
{
	memcpy(cfa, ov5675_rgbir, sizeof(cfa));
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

static unsigned int raw_at(const uint16_t *raw, unsigned int width,
			   unsigned int height, unsigned int stride,
			   int x, int y)
{
	if (x < 0) x = 0;
	if (y < 0) y = 0;
	if ((unsigned int)x >= width) x = (int)width - 1;
	if ((unsigned int)y >= height) y = (int)height - 1;
	unsigned int value = raw[(size_t)y * (stride / 2) + (unsigned int)x] & 1023;
	value = value > BLACK_LEVEL ? value - BLACK_LEVEL : 0;
	return (value * LINEAR_MAX + RAW_RANGE / 2) / RAW_RANGE;
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
static double wb_white_rg = 1.0, wb_white_bg = 1.0;
static double wb_locus_distance;

static void constrain_white_point(double *rg, double *bg)
{
	int order[CCM_MAX];
	double best_d = 1e9, best_x = *rg, best_y = *bg;

	wb_locus_distance = 0;
	if (ccm_count < 2)
		return;
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
		double t = len2 > 0 ? ((*rg - ax) * dx + (*bg - ay) * dy) / len2 : 0;
		if (t < 0) t = 0;
		if (t > 1) t = 1;
		double px = ax + t * dx, py = ay + t * dy, d = hypot(*rg - px, *bg - py);
		if (d < best_d) { best_d = d; best_x = px; best_y = py; }
	}
	wb_locus_distance = best_d;
	if (best_d > WB_LOCUS_BAND) {
		double k = WB_LOCUS_BAND / best_d;
		*rg = best_x + (*rg - best_x) * k;
		*bg = best_y + (*bg - best_y) * k;
	}
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
static uint16_t lsc_map[3][OUT_HEIGHT][OUT_WIDTH];
static struct { int pick[2]; double weight, strength; unsigned int w, h; } lsc_built = { { -2, -2 }, 0, 0, 0, 0 };

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
	    fabs(lsc_built.strength - lsc_strength) < 0.05 && lsc_built.w == width && lsc_built.h == height)
		return;
	const struct lsc_entry *ea = &lsc_table[a], *eb = &lsc_table[b];
	for (int oy = 0; oy < OUT_HEIGHT; oy++) {
		double sy = ((oy * (double)height) / OUT_HEIGHT + 1.0) / height;	/* window centre, 0..1 */
		double gy = sy * (ea->gh - 1);
		for (int ox = 0; ox < OUT_WIDTH; ox++) {
			double sx = ((ox * (double)width) / OUT_WIDTH + 1.0) / width;
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
}

static void calculate_white_balance(const uint16_t *raw, unsigned int width,
				    unsigned int height, unsigned int stride,
				    unsigned int gain_q12[3], double *mean_green)
{
	uint64_t sum[4] = { 0 }, count[4] = { 0 };
	double mean[4];

	for (unsigned int y = 0; y < height; y += 16) {
		for (unsigned int x = 0; x < width; x += 16) {
			for (unsigned int dy = 0; dy < 4 && y + dy < height; dy++) {
				for (unsigned int dx = 0; dx < 4 && x + dx < width; dx++) {
					int channel = channel_at(x + dx, y + dy);
					unsigned int value =
						raw[(size_t)(y + dy) * (stride / 2) + x + dx] & 1023;
					if (value > BLACK_LEVEL)
						sum[channel] += value - BLACK_LEVEL;
					count[channel]++;
				}
			}
		}
	}

	for (int channel = 0; channel < 4; channel++)
		mean[channel] = count[channel] ? (double)sum[channel] / count[channel] : 0;
	/* AE meters raw green (IR included: it is light the sensor really collects). */
	if (mean_green)
		*mean_green = mean[1];
	/* Balance the colour after removing the IR that leaks into R, G and B. */
	for (int channel = 0; channel < 3; channel++) {
		mean[channel] -= mean[CFA_IR];
		if (mean[channel] < 0)
			mean[channel] = 0;
	}
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
		constrain_white_point(&rg, &bg);
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
		printf("ccm: illuminant %d (%.0f%%) + %d, grey-world R/G=%.3f B/G=%.3f, used %.3f/%.3f (locus distance %.3f)\n",
		       ccm_pick[0], ccm_weight * 100.0, ccm_pick[1],
		       wb_last_mean[1] > 0 ? wb_last_mean[0] / wb_last_mean[1] : 0.0,
		       wb_last_mean[1] > 0 ? wb_last_mean[2] / wb_last_mean[1] : 0.0,
		       wb_white_rg, wb_white_bg, wb_locus_distance);
	else
		printf("ccm: no calibration table; generic saturation %.2f\n", saturation_q12 / 4096.0);
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

static int chroma_radius_for_gain(double gain)
{
	if (chroma_radius <= 0)
		return 0;
	return chroma_radius + (gain > HIGH_GAIN_X) + (gain > 2 * HIGH_GAIN_X);
}

static int tnr_threshold_for_gain(double gain)
{
	double k = gain > HIGH_GAIN_X ? sqrt(gain / HIGH_GAIN_X) : 1.0;
	return (int)(TNR_THRESHOLD * (k > 2.0 ? 2.0 : k) + 0.5);
}
#define HIGHLIGHT_START (LINEAR_MAX * 85 / 100)	/* raw level where clipping handling starts */

/*
 * Turn the output by 180 degrees (--rotate 180). The ov8856 driver exposes no mounting
 * rotation control, so this is a launcher setting (CAMERA_REAR_ROTATION in gbos.cfg).
 */
static int output_rotate180;

static int16_t plane_y[OUT_HEIGHT][OUT_WIDTH], plane_u[OUT_HEIGHT][OUT_WIDTH],
	plane_v[OUT_HEIGHT][OUT_WIDTH], plane_tmp[OUT_HEIGHT][OUT_WIDTH];
/* Linear (pre-gamma) R, G, B per output pixel, and their neighbourhood averages. */
static int16_t lin_plane[3][OUT_HEIGHT][OUT_WIDTH], lin_avg[3][OUT_HEIGHT][OUT_WIDTH];
static unsigned char tnr_previous[OUT_HEIGHT][OUT_WIDTH];
static int tnr_have_previous;

/* Forget the previous frame (start, or resuming after idle): no blending with stale content. */
static void tnr_reset(void)
{
	tnr_have_previous = 0;
}

static void box_filter_plane(int16_t plane[OUT_HEIGHT][OUT_WIDTH], int r)
{
	/* Separable running-sum box filter, edges clamped. */
	for (int y = 0; y < OUT_HEIGHT; y++) {
		int sum = 0;
		for (int k = -r; k <= r; k++)
			sum += plane[y][k < 0 ? 0 : (k >= OUT_WIDTH ? OUT_WIDTH - 1 : k)];
		for (int x = 0; x < OUT_WIDTH; x++) {
			plane_tmp[y][x] = (int16_t)(sum / (2 * r + 1));
			int out_x = x - r, in_x = x + r + 1;
			sum -= plane[y][out_x < 0 ? 0 : out_x];
			sum += plane[y][in_x >= OUT_WIDTH ? OUT_WIDTH - 1 : in_x];
		}
	}
	for (int x = 0; x < OUT_WIDTH; x++) {
		int sum = 0;
		for (int k = -r; k <= r; k++)
			sum += plane_tmp[k < 0 ? 0 : (k >= OUT_HEIGHT ? OUT_HEIGHT - 1 : k)][x];
		for (int y = 0; y < OUT_HEIGHT; y++) {
			plane[y][x] = (int16_t)(sum / (2 * r + 1));
			int out_y = y - r, in_y = y + r + 1;
			sum -= plane_tmp[out_y < 0 ? 0 : out_y][x];
			sum += plane_tmp[in_y >= OUT_HEIGHT ? OUT_HEIGHT - 1 : in_y][x];
		}
	}
}

/* Linear-light R, G, B (0..LINEAR_MAX) of the 4x4 window at x, y. */
static void rgb_linear_at(const uint16_t *raw, unsigned int width, unsigned int height,
			  unsigned int stride, const unsigned int gain_q12[3],
			  unsigned int x, unsigned int y, const unsigned int *shading_q12,
			  int rgb[3])
{
	/*
	 * A 4x4 window holds every site of the 4x4 mosaic exactly once (2 R, 2 B, 8 G,
	 * 4 IR; for Bayer 4/8/4). The old 3x3 window often held no R or no B site, so
	 * red or blue read as zero in drifting patches: the cyan/yellow grid.
	 * The window is shifted inward at the frame edges so it is always complete.
	 */
	unsigned int sum[4] = { 0 }, n[4] = { 0 };
	unsigned int x0 = x > 0 ? x - 1 : 0, y0 = y > 0 ? y - 1 : 0;

	if (x0 > width - 4) x0 = width - 4;
	if (y0 > height - 4) y0 = height - 4;
	unsigned int peak = 0;
	for (unsigned int sy = y0; sy < y0 + 4; sy++)
		for (unsigned int sx = x0; sx < x0 + 4; sx++) {
			int channel = channel_at(sx, sy);
			unsigned int v = raw_at(raw, width, height, stride, (int)sx, (int)sy);
			sum[channel] += v;
			n[channel]++;
			if (v > peak)
				peak = v;
		}
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
	for (int channel = 0; channel < 4; channel++)
		mean[channel] = n[channel] ?
			(int)(((uint64_t)sum[channel] * shade[channel] / n[channel] + 2048) / 4096) : 0;
	int ir = mean[CFA_IR];
	int lin[3];
	for (int channel = 0; channel < 3; channel++) {
		lin[channel] = n[channel] ? mean[channel] - ir : 0;
		if (lin[channel] < 0)
			lin[channel] = 0;
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
	 * Highlights: where any site in the window is near the sensor's full scale, its
	 * colour is no longer measured. On the RGB-IR sensor the IR sites clip too, and
	 * "clipped colour minus clipped IR" is zero: lamps turned black with a magenta rim
	 * (green clips first; 2026-11-17 photo). Fade such windows towards white, from no
	 * effect at HIGHLIGHT_START of full scale to pure white when a site is at full scale.
	 */
	if (peak > HIGHLIGHT_START) {
		int t = (int)(((peak - HIGHLIGHT_START) * 4096u) / (LINEAR_MAX - HIGHLIGHT_START));
		if (t > 4096)
			t = 4096;
		for (int channel = 0; channel < 3; channel++) {
			int v = rgb[channel] > (int)LINEAR_MAX ? (int)LINEAR_MAX : rgb[channel];
			rgb[channel] = (v * (4096 - t) + (int)LINEAR_MAX * t + 2048) >> 12;
		}
	}
	for (int channel = 0; channel < 3; channel++)
		if (rgb[channel] > (int)LINEAR_MAX)
			rgb[channel] = (int)LINEAR_MAX;
}

/*
 * Colour from neighbourhood averages in linear light, luma per pixel. Averaging after the
 * gamma curve (the earlier U/V box filter) is biased: gamma is concave, so the noisier
 * channels (R and B: 2 of 16 sites each, IR subtracted, amplified by the colour matrix)
 * lose more than G, and noisy greys turned green at high gain (2026-11-18 boot-11). Here
 * each pixel keeps its own linear luminance but takes the R:G:B ratios of the (2r+1)^2
 * average; near strong luma edges those ratios are pulled towards grey.
 */
static void linear_colour_to_yuv(int r, double gain)
{
	for (int c = 0; c < 3; c++) {
		memcpy(lin_avg[c], lin_plane[c], sizeof(lin_avg[c]));
		if (r > 0)
			box_filter_plane(lin_avg[c], r);
	}
	int edges = r > 0 && gain > HIGH_GAIN_X;
	for (int oy = 0; oy < OUT_HEIGHT; oy++)
		for (int ox = 0; ox < OUT_WIDTH; ox++) {
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
					int xl = ox > 0 ? ox - 1 : ox, xr = ox + 1 < OUT_WIDTH ? ox + 1 : ox;
					int yu = oy > 0 ? oy - 1 : oy, yd = oy + 1 < OUT_HEIGHT ? oy + 1 : oy;
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
					c[k] = v < 0 ? 0 : v > LINEAR_MAX ? (int)LINEAR_MAX : (int)(v + 0.5);
				}
			}
			for (int k = 0; k < 3; k++)
				c[k] = gamma_lut[c[k]];
			plane_y[oy][ox] = (int16_t)((77 * c[0] + 150 * c[1] + 29 * c[2] + 128) >> 8);
			plane_u[oy][ox] = (int16_t)((-43 * c[0] - 85 * c[1] + 128 * c[2]) >> 8);
			plane_v[oy][ox] = (int16_t)((128 * c[0] - 107 * c[1] - 21 * c[2]) >> 8);
		}
}

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
	calculate_white_balance(raw, width, height, stride, gain_q12, mean_green);
	if (wb_last_mean[1] > 0)
		update_lsc(wb_white_rg, wb_white_bg, width, height);

	/* 1. Demosaic every output pixel to full-range BT.601 Y/U/V. */
	for (unsigned int oy = 0; oy < OUT_HEIGHT; oy++) {
		unsigned int sy = (unsigned int)(((uint64_t)oy * height) / OUT_HEIGHT);
		for (unsigned int ox = 0; ox < OUT_WIDTH; ox++) {
			unsigned int sx = (unsigned int)(((uint64_t)ox * width) / OUT_WIDTH);
			int c[3];
			unsigned int shading[3];
			if (lsc_active) {
				shading[0] = lsc_map[0][oy][ox];
				shading[1] = lsc_map[1][oy][ox];
				shading[2] = lsc_map[2][oy][ox];
			}
			rgb_linear_at(raw, width, height, stride, gain_q12, sx, sy,
				      lsc_active ? shading : NULL, c);
			/* Lens shading stays indexed by sensor position; only the output is turned. */
			unsigned int ty = output_rotate180 ? OUT_HEIGHT - 1 - oy : oy;
			unsigned int tx = output_rotate180 ? OUT_WIDTH - 1 - ox : ox;
			for (int k = 0; k < 3; k++)
				lin_plane[k][ty][tx] = (int16_t)c[k];
		}
	}
	/*
	 * 2. Chroma noise: R and B are only 2 of 16 sites, so at high gain colour speckle
	 * dominates. Colour comes from a neighbourhood average in linear light (growing with
	 * gain); luma keeps full detail.
	 */
	linear_colour_to_yuv(chroma_radius_for_gain(sensor_total_gain), sensor_total_gain);
	/*
	 * 3. Luma noise: recursive temporal filter. Where a pixel changed by no more than
	 * TNR_THRESHOLD since the last output it moves only a third of the way, averaging
	 * noise in still areas; larger changes (motion) pass through at once, so no ghosts.
	 */
	int tnr_threshold = tnr_threshold_for_gain(sensor_total_gain);
	for (unsigned int oy = 0; oy < OUT_HEIGHT; oy++)
		for (unsigned int ox = 0; ox < OUT_WIDTH; ox++) {
			int y = plane_y[oy][ox];
			if (tnr_have_previous) {
				int d = y - tnr_previous[oy][ox];
				if (d >= -tnr_threshold && d <= tnr_threshold)
					y = tnr_previous[oy][ox] + (d >= 0 ? (d + 1) / 3 : -((-d + 1) / 3));
			}
			y = clamp8(y);
			tnr_previous[oy][ox] = (unsigned char)y;
			plane_y[oy][ox] = (int16_t)y;
		}
	tnr_have_previous = 1;

	/* 4. Pack YUYV 4:2:2: U/V of each pixel pair averaged. */
	for (unsigned int oy = 0; oy < OUT_HEIGHT; oy++) {
		unsigned char *dst = out + (size_t)oy * OUT_WIDTH * 2;
		for (unsigned int ox = 0; ox < OUT_WIDTH; ox += 2) {
			dst[ox * 2] = (unsigned char)plane_y[oy][ox];
			dst[ox * 2 + 1] = clamp8(128 + (plane_u[oy][ox] + plane_u[oy][ox + 1]) / 2);
			dst[ox * 2 + 2] = (unsigned char)plane_y[oy][ox + 1];
			dst[ox * 2 + 3] = clamp8(128 + (plane_v[oy][ox] + plane_v[oy][ox + 1]) / 2);
		}
	}
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

	ratio = mean_green > 1.0 ? AE_TARGET / mean_green : AE_MAX_STEP;
	if (ratio > 1.0 - AE_DEADBAND && ratio < 1.0 + AE_DEADBAND)
		return 0;
	if (ratio > AE_MAX_STEP) ratio = AE_MAX_STEP;
	if (ratio < 1.0 / AE_MAX_STEP) ratio = 1.0 / AE_MAX_STEP;
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
	if (ae->frames % AE_INTERVAL || !ae_update(ae, mean_green))
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
 * The IPU6 firmware cannot stop a stream cleanly, so the sensor keeps streaming;
 * but with no consumer the ~12 ms/frame conversion (about a third of a core at
 * 30 fps) is skipped. The next frame after a consumer starts is converted.
 */
static int bridge_should_convert(int readers)
{
	return readers != 0;
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
	int mfd = -1, sfd = -1, cfd = -1, vfd = -1, outfd = -1;
	int linked = 0, streaming = 0, result = 1;
	struct bridge_buffer buffers[3] = { 0 };
	struct v4l2_subdev_format sensor = { 0 }, saved_sink = { 0 }, set;
	struct v4l2_format video = { .type = V4L2_BUF_TYPE_VIDEO_CAPTURE };
	struct v4l2_format saved_video = { .type = V4L2_BUF_TYPE_VIDEO_CAPTURE };
	struct v4l2_requestbuffers request = {
		.count = 3, .type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
		.memory = V4L2_MEMORY_MMAP
	};
	unsigned int buffer_count = 0, frame_count = 0;
	int last_readers = -2;
	unsigned char *output = NULL;
	enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

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
	outfd = open(out_dev, O_RDWR | O_CLOEXEC);
	if (sfd < 0 || cfd < 0 || vfd < 0 || outfd < 0) {
		perror("open camera or loopback node");
		goto done;
	}
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
	if (ioctl(vfd, VIDIOC_REQBUFS, &request)) {
		perror("VIDIOC_REQBUFS");
		goto done;
	}
	if (request.count < 2 || request.count > 3) {
		fprintf(stderr, "unexpected IPU6 buffer count %u\n", request.count);
		goto done;
	}
	buffer_count = request.count;
	for (unsigned int i = 0; i < buffer_count; i++) {
		struct v4l2_buffer b = { .type = type, .memory = V4L2_MEMORY_MMAP, .index = i };
		if (ioctl(vfd, VIDIOC_QUERYBUF, &b) || b.length < video.fmt.pix.sizeimage ||
		    b.length > MAX_FRAME_BYTES)
			goto done;
		buffers[i].length = b.length;
		buffers[i].data = mmap(NULL, b.length, PROT_READ | PROT_WRITE,
				       MAP_SHARED, vfd, b.m.offset);
		if (buffers[i].data == MAP_FAILED) {
			buffers[i].data = NULL;
			goto done;
		}
		if (ioctl(vfd, VIDIOC_QBUF, &b))
			goto done;
	}
	if (ioctl(vfd, VIDIOC_STREAMON, &type)) {
		perror("start IPU6 capture");
		goto done;
	}
	streaming = 1;
	struct v4l2_format outfmt = { .type = V4L2_BUF_TYPE_VIDEO_OUTPUT };
	outfmt.fmt.pix.width = OUT_WIDTH;
	outfmt.fmt.pix.height = OUT_HEIGHT;
	outfmt.fmt.pix.pixelformat = OUT_FOURCC;
	outfmt.fmt.pix.field = V4L2_FIELD_NONE;
	outfmt.fmt.pix.bytesperline = OUT_WIDTH * 2;
	outfmt.fmt.pix.sizeimage = OUT_FRAME_BYTES;
	outfmt.fmt.pix.colorspace = V4L2_COLORSPACE_SRGB;
	if (ioctl(outfd, VIDIOC_S_FMT, &outfmt) ||
	    outfmt.fmt.pix.pixelformat != OUT_FOURCC ||
	    outfmt.fmt.pix.width != OUT_WIDTH || outfmt.fmt.pix.height != OUT_HEIGHT ||
	    outfmt.fmt.pix.bytesperline != OUT_WIDTH * 2 ||
	    outfmt.fmt.pix.sizeimage < OUT_FRAME_BYTES) {
		perror("configure V4L2 loopback output");
		goto done;
	}
	output = malloc(OUT_FRAME_BYTES);
	if (!output)
		goto done;
	for (size_t i = 0; i < OUT_FRAME_BYTES; i += 4) {
		output[i] = 16;
		output[i + 1] = 128;
		output[i + 2] = 16;
		output[i + 3] = 128;
	}
	if (write(outfd, output, OUT_FRAME_BYTES) != (ssize_t)OUT_FRAME_BYTES) {
		perror("prime V4L2 loopback");
		goto done;
	}
	ae_init(&ae, sfd, (int)sensor.format.height);
	af_init(&af, find_lens(&graph, &topology, path.sensor->id, lens_path, sizeof(lens_path)) ?
		NULL : lens_path);
	signal(SIGINT, stop_bridge);
	signal(SIGTERM, stop_bridge);
	printf("bridge active: %ux%u %s to %ux%u YUYV%s; sensor remains streaming until stop\n",
	       video.fmt.pix.width, video.fmt.pix.height, profile->rgbir ? "RGB-IR" : "Bayer",
	       OUT_WIDTH, OUT_HEIGHT, output_rotate180 ? ", rotated 180" : "");
	result = 0;
	struct frame_health health;
	int have_output = 0;
	frame_health_start(&health, monotonic_s());
	while (running) {
		struct pollfd pfd = { .fd = vfd, .events = POLLIN | POLLPRI };
		int ready = poll(&pfd, 1, 1000);
		if (ready < 0) {
			if (errno == EINTR) continue;
			perror("poll IPU6");
			result = 1;
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
		if (b.index >= buffer_count) {
			fprintf(stderr, "invalid IPU6 buffer index %u\n", b.index);
			result = 1;
			break;
		}
		int readers = loopback_readers(outfd);
		if (readers != last_readers) {
			printf("bridge: %s\n", readers < 0 ? "readers_active unavailable; converting every frame" :
			       readers ? "consumer streaming; converting frames" :
			       "no consumer; idle (sensor and auto-exposure keep running)");
			last_readers = readers;
			tnr_reset();	/* the scene may have changed while idle */
			ae_set_idle(&ae, !bridge_should_convert(readers));
			if (bridge_should_convert(readers))
				af_start(&af);
		}
		const char *bad = (b.flags & V4L2_BUF_FLAG_ERROR) ? "error-marked" :
			b.bytesused < (uint64_t)video.fmt.pix.bytesperline * video.fmt.pix.height ||
			b.bytesused > buffers[b.index].length ? "short" : NULL;
		if (bad) {
			int give_up = frame_health_bad(&health, monotonic_s(), bad, b.flags, b.bytesused);
			/* Keep the consumer fed with the last picture rather than nothing. */
			if (!give_up && have_output && bridge_should_convert(readers) &&
			    write(outfd, output, OUT_FRAME_BYTES) != (ssize_t)OUT_FRAME_BYTES) {
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
		double mean_green = 0;
		if (!bridge_should_convert(readers)) {
			/* Idle: only the cheap sampled statistics, so exposure is right on resume. */
			unsigned int unused_gain[3];
			calculate_white_balance(buffers[b.index].data, video.fmt.pix.width,
						video.fmt.pix.height, video.fmt.pix.bytesperline,
						unused_gain, &mean_green);
		} else if (convert_bayer_yuyv(buffers[b.index].data, video.fmt.pix.width,
					      video.fmt.pix.height, video.fmt.pix.bytesperline,
					      output, OUT_FRAME_BYTES, &mean_green) ||
			   write(outfd, output, OUT_FRAME_BYTES) != (ssize_t)OUT_FRAME_BYTES) {
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
		if (frame_count == 30 || frame_count % 900 == 0)
			log_cfa_phases(buffers[b.index].data, video.fmt.pix.width,
				       video.fmt.pix.height, video.fmt.pix.bytesperline, frame_count);
		if (ioctl(vfd, VIDIOC_QBUF, &b)) {
			perror("requeue IPU6 frame");
			result = 1;
			break;
		}
	}
done:
	if (streaming && ioctl(vfd, VIDIOC_STREAMOFF, &type))
		perror("VIDIOC_STREAMOFF");
	if (af.fd >= 0)
		close(af.fd);
	if (ae.fd >= 0)
		ae_restore(&ae);
	for (unsigned int i = 0; i < buffer_count; i++)
		if (buffers[i].data) munmap(buffers[i].data, buffers[i].length);
	if (vfd >= 0) {
		request.count = 0;
		if (ioctl(vfd, VIDIOC_REQBUFS, &request))
			perror("release IPU6 buffers");
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
	if (outfd >= 0) close(outfd);
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
	const char *media, *output, *calibration;
};

/*
 * [--sensor ov5675|ov8856] [--rotate 0|180] /dev/mediaN /dev/videoN [calibration].
 * Without options: the front sensor, unrotated, as before the rear camera existed.
 */
static int parse_bridge_args(int argc, char **argv, struct bridge_options *o)
{
	int i = 1;

	memset(o, 0, sizeof(*o));
	o->profile = &sensor_profiles[0];
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

#ifndef CAMERA_BRIDGE_TEST
int main(int argc, char **argv)
{
	struct bridge_options o;

	(void)probe;
	if (parse_bridge_args(argc, argv, &o)) {
		fprintf(stderr, "usage: %s [--sensor ov5675|ov8856] [--rotate 0|180] "
			"/dev/mediaN /dev/videoN [calibration dir or colour-matrix table]\n", argv[0]);
		return 2;
	}
	setvbuf(stdout, NULL, _IOLBF, 0);
	output_rotate180 = o.rotate180;
	printf("sensor: %s (calibration prefix %s)%s\n", o.profile->label, o.profile->cal_prefix,
	       o.rotate180 ? "; output rotated 180" : "");
	if (o.calibration)
		load_calibration(o.calibration, o.profile->cal_prefix);
	return capture_to_loopback(o.media, o.output, o.profile);
}
#endif
