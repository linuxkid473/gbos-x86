#define _GNU_SOURCE
#define CAMERA_CAPTURE_NO_MAIN
#include "../tools/camera_capture_probe.c"

#include <math.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
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
	if (mean[0] > 0 && mean[1] > 0) {
		double gain = mean[1] / mean[0];
		if (gain < 0.25) gain = 0.25;
		if (gain > 4.0) gain = 4.0;
		gain_q12[0] = (unsigned int)(gain * 4096.0 + 0.5);
	}
	if (mean[2] > 0 && mean[1] > 0) {
		double gain = mean[1] / mean[2];
		if (gain < 0.25) gain = 0.25;
		if (gain > 4.0) gain = 4.0;
		gain_q12[2] = (unsigned int)(gain * 4096.0 + 0.5);
	}
	memcpy(wb_last_mean, mean, sizeof(wb_last_mean));
	/* The calibrated white points are IR-free raw ratios, the same as mean[] here. */
	if (mean[1] > 0)
		select_ccm(mean[0] / mean[1], mean[2] / mean[1]);
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
		printf("ccm: illuminant %d (%.0f%%) + %d, white R/G=%.3f B/G=%.3f\n", ccm_pick[0],
		       ccm_weight * 100.0, ccm_pick[1],
		       wb_last_mean[1] > 0 ? wb_last_mean[0] / wb_last_mean[1] : 0.0,
		       wb_last_mean[1] > 0 ? wb_last_mean[2] / wb_last_mean[1] : 0.0);
	else
		printf("ccm: no calibration table; generic saturation %.2f\n", saturation_q12 / 4096.0);
	if (lsc_active)
		printf("lsc: illuminant %d (%.0f%%) + %d, strength %.2f at sensor gain %.1fx\n", lsc_pick[0],
		       lsc_weight * 100.0, lsc_pick[1], lsc_strength, sensor_total_gain);
	else
		printf("lsc: no lens-shading table\n");
}

static unsigned char clamp8(int value)
{
	if (value < 0) return 0;
	if (value > 255) return 255;
	return (unsigned char)value;
}

/* Image tuning (static so the host tests can vary them). */
static int chroma_radius = 2;		/* 5x5 U/V box filter */
#define TNR_THRESHOLD 14		/* luma steps (0..255) still treated as noise */

static int16_t plane_y[OUT_HEIGHT][OUT_WIDTH], plane_u[OUT_HEIGHT][OUT_WIDTH],
	plane_v[OUT_HEIGHT][OUT_WIDTH], plane_tmp[OUT_HEIGHT][OUT_WIDTH];
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

static void smooth_chroma(int r)
{
	box_filter_plane(plane_u, r);
	box_filter_plane(plane_v, r);
}

static void rgb_at(const uint16_t *raw, unsigned int width, unsigned int height,
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
	for (unsigned int sy = y0; sy < y0 + 4; sy++)
		for (unsigned int sx = x0; sx < x0 + 4; sx++) {
			int channel = channel_at(sx, sy);
			sum[channel] += raw_at(raw, width, height, stride, (int)sx, (int)sy);
			n[channel]++;
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
	for (int channel = 0; channel < 3; channel++)
		rgb[channel] = gamma_lut[rgb[channel] > (int)LINEAR_MAX ? LINEAR_MAX
							      : (unsigned int)rgb[channel]];
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
		update_lsc(wb_last_mean[0] / wb_last_mean[1], wb_last_mean[2] / wb_last_mean[1], width, height);

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
			rgb_at(raw, width, height, stride, gain_q12, sx, sy, lsc_active ? shading : NULL, c);
			plane_y[oy][ox] = (int16_t)((77 * c[0] + 150 * c[1] + 29 * c[2] + 128) >> 8);
			plane_u[oy][ox] = (int16_t)((-43 * c[0] - 85 * c[1] + 128 * c[2]) >> 8);
			plane_v[oy][ox] = (int16_t)((128 * c[0] - 107 * c[1] - 21 * c[2]) >> 8);
		}
	}
	/*
	 * 2. Chroma noise: R and B are only 2 of 16 sites, so at high gain colour speckle
	 * dominates. Average U/V over a (2r+1)^2 neighbourhood; luma keeps full detail.
	 */
	if (chroma_radius > 0)
		smooth_chroma(chroma_radius);
	/*
	 * 3. Luma noise: recursive temporal filter. Where a pixel changed by no more than
	 * TNR_THRESHOLD since the last output it moves only a third of the way, averaging
	 * noise in still areas; larger changes (motion) pass through at once, so no ghosts.
	 */
	for (unsigned int oy = 0; oy < OUT_HEIGHT; oy++)
		for (unsigned int ox = 0; ox < OUT_WIDTH; ox++) {
			int y = plane_y[oy][ox];
			if (tnr_have_previous) {
				int d = y - tnr_previous[oy][ox];
				if (d >= -TNR_THRESHOLD && d <= TNR_THRESHOLD)
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
	int min, max, saved, value, present;
};

struct ae_state {
	int fd, enabled;
	unsigned int frames;
	struct ae_control exposure, again, dgain;
};

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

/* Split a total into exposure time first (no added noise), then analogue, then digital gain. */
static void ae_plan(struct ae_state *ae, double total)
{
	double rest;

	ae->exposure.value = ae_clamp(total, ae->exposure.min, ae->exposure.max);
	rest = total / ae->exposure.value;
	ae->again.value = ae_clamp(rest * ae->again.min, ae->again.min, ae->again.max);
	rest = rest * ae->again.min / ae->again.value;
	if (ae->dgain.present)
		ae->dgain.value = ae_clamp(rest * ae->dgain.min, ae->dgain.min, ae->dgain.max);
}

/* Returns 1 when the plan changed, 0 when the scene is within the deadband or at a limit. */
static int ae_update(struct ae_state *ae, double mean_green)
{
	double ratio, before = ae_total(ae);
	int exposure = ae->exposure.value, again = ae->again.value, dgain = ae->dgain.value;

	ratio = mean_green > 1.0 ? AE_TARGET / mean_green : AE_MAX_STEP;
	if (ratio > 1.0 - AE_DEADBAND && ratio < 1.0 + AE_DEADBAND)
		return 0;
	if (ratio > AE_MAX_STEP) ratio = AE_MAX_STEP;
	if (ratio < 1.0 / AE_MAX_STEP) ratio = 1.0 / AE_MAX_STEP;
	ae_plan(ae, before * ratio);
	return exposure != ae->exposure.value || again != ae->again.value ||
	       dgain != ae->dgain.value;
}

static int ae_query(int fd, struct ae_control *c, __u32 id)
{
	struct v4l2_queryctrl q = { .id = id };
	struct v4l2_control v = { .id = id };

	c->id = id;
	if (ioctl(fd, VIDIOC_QUERYCTRL, &q) || (q.flags & (V4L2_CTRL_FLAG_DISABLED |
						   V4L2_CTRL_FLAG_READ_ONLY)) ||
	    q.minimum <= 0 || q.maximum < q.minimum || ioctl(fd, VIDIOC_G_CTRL, &v))
		return -1;
	c->min = q.minimum;
	c->max = q.maximum;
	c->saved = c->value = v.value;
	c->present = 1;
	return 0;
}

static int ae_set(int fd, const struct ae_control *c, int value)
{
	struct v4l2_control v = { .id = c->id, .value = value };

	return c->present ? ioctl(fd, VIDIOC_S_CTRL, &v) : 0;
}

static void ae_init(struct ae_state *ae, int fd)
{
	memset(ae, 0, sizeof(*ae));
	ae->fd = fd;
	if (ae_query(fd, &ae->exposure, V4L2_CID_EXPOSURE) ||
	    ae_query(fd, &ae->again, V4L2_CID_ANALOGUE_GAIN)) {
		printf("ae: sensor exposure/analogue gain controls unavailable; auto-exposure off\n");
		return;
	}
	(void)ae_query(fd, &ae->dgain, V4L2_CID_DIGITAL_GAIN);
	ae->enabled = 1;
	printf("ae: exposure %d..%d (now %d) again %d..%d (now %d) dgain %s %d..%d (now %d)\n",
	       ae->exposure.min, ae->exposure.max, ae->exposure.value,
	       ae->again.min, ae->again.max, ae->again.value,
	       ae->dgain.present ? "on" : "absent", ae->dgain.min, ae->dgain.max, ae->dgain.value);
}

static void ae_frame(struct ae_state *ae, double mean_green)
{
	if (!ae->enabled)
		return;
	ae->frames++;
	sensor_total_gain = ae_total(ae) / ae->exposure.max;	/* gain multiple at full exposure */
	if (sensor_total_gain < 1.0)
		sensor_total_gain = 1.0;
	if (ae->frames % 150 == 0)
		printf("ae: mean=%.0f target=%.0f exposure=%d again=%d dgain=%d\n", mean_green,
		       AE_TARGET, ae->exposure.value, ae->again.value, ae->dgain.value);
	if (ae->frames % AE_INTERVAL || !ae_update(ae, mean_green))
		return;
	if (ae_set(ae->fd, &ae->exposure, ae->exposure.value) ||
	    ae_set(ae->fd, &ae->again, ae->again.value) ||
	    ae_set(ae->fd, &ae->dgain, ae->dgain.value)) {
		perror("ae: set sensor control; auto-exposure off");
		ae->enabled = 0;
	}
}

static void ae_restore(struct ae_state *ae)
{
	if (!ae->exposure.present || !ae->again.present)
		return;
	if (ae_set(ae->fd, &ae->exposure, ae->exposure.saved) ||
	    ae_set(ae->fd, &ae->again, ae->again.saved) ||
	    ae_set(ae->fd, &ae->dgain, ae->dgain.saved))
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

static int capture_to_loopback(const char *media_dev, const char *out_dev)
{
	struct ae_state ae = { .fd = -1 };
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
	    find_path(&graph, &topology, "ov5675", &path) ||
	    device_path(&graph, &topology, path.sensor->id, sensor_path, sizeof(sensor_path)) ||
	    device_path(&graph, &topology, path.csi->id, csi_path, sizeof(csi_path)) ||
	    device_path(&graph, &topology, path.video->id, capture_path, sizeof(capture_path))) {
		fprintf(stderr, "cannot discover a unique OV5675 IPU6 capture path\n");
		goto done;
	}
	printf("IPU6 path: %s -> %s -> %s; output=%s\n",
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
	/*
	 * The measured RGB-IR layout is relative to the orientation the driver reports
	 * as GRBG; a flip would move the sites, so refuse any other code.
	 */
	if (sensor.format.code != MEDIA_BUS_FMT_SGRBG10_1X10) {
		fprintf(stderr, "unexpected OV5675 bus code 0x%x (RGB-IR layout measured for GRBG10)\n",
			sensor.format.code);
		goto done;
	}
	set_rgbir_pattern();
	printf("cfa: OV5675 RGB-IR 4x4 (bus code 0x%x): %s\n", sensor.format.code,
	       "GIGI/RGBG/GIGI/BGRG");
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
	ae_init(&ae, sfd);
	signal(SIGINT, stop_bridge);
	signal(SIGTERM, stop_bridge);
	printf("bridge active: %ux%u Bayer to %ux%u YUYV; sensor remains streaming until stop\n",
	       video.fmt.pix.width, video.fmt.pix.height, OUT_WIDTH, OUT_HEIGHT);
	result = 0;
	while (running) {
		struct pollfd pfd = { .fd = vfd, .events = POLLIN | POLLPRI };
		int ready = poll(&pfd, 1, 1000);
		if (ready < 0) {
			if (errno == EINTR) continue;
			perror("poll IPU6");
			result = 1;
			break;
		}
		if (!ready)
			continue;
		struct v4l2_buffer b = { .type = type, .memory = V4L2_MEMORY_MMAP };
		if (ioctl(vfd, VIDIOC_DQBUF, &b)) {
			if (errno == EAGAIN || errno == EINTR) continue;
			perror("dequeue IPU6 frame");
			result = 1;
			break;
		}
		if (b.index >= buffer_count ||
		    b.bytesused < (uint64_t)video.fmt.pix.bytesperline * video.fmt.pix.height ||
		    b.bytesused > buffers[b.index].length ||
		    (b.flags & V4L2_BUF_FLAG_ERROR)) {
			fprintf(stderr, "invalid or error-marked IPU6 frame\n");
			result = 1;
			break;
		}
		double mean_green = 0;
		int readers = loopback_readers(outfd);
		if (readers != last_readers) {
			printf("bridge: %s\n", readers < 0 ? "readers_active unavailable; converting every frame" :
			       readers ? "consumer streaming; converting frames" :
			       "no consumer; idle (sensor and auto-exposure keep running)");
			last_readers = readers;
			tnr_reset();	/* the scene may have changed while idle */
		}
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
		}
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

#ifndef CAMERA_BRIDGE_TEST
/*
 * Optional calibration: a directory holding ov5678_ccm.txt and ov5678_lsc.txt (the stick's
 * gbos/camera/), or, as before, a single colour-matrix file.
 */
static void load_calibration(const char *arg)
{
	struct stat st;
	char path[512];
	const char *ccm = arg;

	if (stat(arg, &st) == 0 && S_ISDIR(st.st_mode)) {
		snprintf(path, sizeof(path), "%s/ov5678_lsc.txt", arg);
		if (load_lsc_table(path) > 0)
			printf("lsc: loaded %d illuminants (%dx%d grids) from %s\n", lsc_count,
			       lsc_table[0].gw, lsc_table[0].gh, path);
		else
			printf("lsc: no usable table at %s; no lens-shading correction\n", path);
		snprintf(path, sizeof(path), "%s/ov5678_ccm.txt", arg);
		ccm = path;
	}
	if (load_ccm_table(ccm) > 0)
		printf("ccm: loaded %d calibrated illuminants from %s\n", ccm_count, ccm);
	else
		printf("ccm: no usable table at %s; generic saturation\n", ccm);
}

int main(int argc, char **argv)
{
	(void)probe;
	if (argc != 3 && argc != 4) {
		fprintf(stderr, "usage: %s /dev/mediaN /dev/videoN [calibration dir or colour-matrix table]\n", argv[0]);
		return 2;
	}
	setvbuf(stdout, NULL, _IOLBF, 0);
	if (argc == 4)
		load_calibration(argv[3]);
	return capture_to_loopback(argv[1], argv[2]);
}
#endif
