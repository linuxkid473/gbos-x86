#define CAMERA_BRIDGE_TEST
#include "../kernel/ipu6-loopback.c"

static int test_gamma(void)
{
	init_gamma();
	if (gamma_lut[0] != 0 || gamma_lut[LINEAR_MAX] != 255)
		return 1;
	for (unsigned int i = 1; i <= LINEAR_MAX; i++)
		if (gamma_lut[i] < gamma_lut[i - 1])
			return 2;
	/* 18% linear grey is about 118 in sRGB, not 46 as a linear mapping gives. */
	unsigned char grey = gamma_lut[(unsigned int)(0.18 * LINEAR_MAX + 0.5)];
	return grey >= 114 && grey <= 122 ? 0 : 3;
}

/* OV5675 at 2592x1944@30: exposure 4..2012 lines, analogue 128..2047, digital 1024..4095. */
static void ov5675_ae(struct ae_state *ae)
{
	memset(ae, 0, sizeof(*ae));
	ae->fd = -1;
	ae->enabled = 1;
	ae->exposure = (struct ae_control){ V4L2_CID_EXPOSURE, 4, 2012, 2012, 2012, 1 };
	ae->again = (struct ae_control){ V4L2_CID_ANALOGUE_GAIN, 128, 2047, 128, 128, 1 };
	ae->dgain = (struct ae_control){ V4L2_CID_DIGITAL_GAIN, 1024, 4095, 1024, 1024, 1 };
}

static int test_auto_exposure(void)
{
	struct ae_state ae;

	ov5675_ae(&ae);
	/* Dark scene at driver defaults (max exposure, 1x gain): raise analogue gain, bounded step. */
	if (!ae_update(&ae, AE_TARGET / 4) || ae.exposure.value != 2012 ||
	    ae.again.value != 205 || ae.dgain.value != 1024)
		return 1;
	/* Within the deadband nothing changes. */
	if (ae_update(&ae, AE_TARGET * 1.05))
		return 2;
	/* Bright scene: gain comes down to 1x before exposure time is shortened. */
	for (int i = 0; i < 10; i++)
		ae_update(&ae, AE_TARGET * 4);
	if (ae.again.value != 128 || ae.dgain.value != 1024 || ae.exposure.value >= 2012)
		return 3;
	/* Very dark scene saturates at maximum exposure, analogue and then digital gain. */
	for (int i = 0; i < 40; i++)
		ae_update(&ae, 0);
	if (ae.exposure.value != 2012 || ae.again.value != 2047 || ae.dgain.value != 4095 ||
	    ae_update(&ae, 0))
		return 4;
	/* Missing digital gain control: analogue gain still clamps at its maximum. */
	ov5675_ae(&ae);
	ae.dgain.present = 0;
	for (int i = 0; i < 40; i++)
		ae_update(&ae, 0);
	return ae.again.value == 2047 && ae.exposure.value == 2012 ? 0 : 5;
}

static int test_bayer_patterns(void)
{
	if (set_bayer_pattern(MEDIA_BUS_FMT_SRGGB10_1X10) || channel_at(0, 0) != 0 ||
	    channel_at(1, 1) != 2)
		return 1;
	if (set_bayer_pattern(MEDIA_BUS_FMT_SBGGR10_1X10) || channel_at(0, 0) != 2 ||
	    channel_at(1, 1) != 0)
		return 2;
	if (set_bayer_pattern(MEDIA_BUS_FMT_SGBRG10_1X10) || channel_at(1, 0) != 2 ||
	    channel_at(0, 1) != 0)
		return 3;
	if (set_bayer_pattern(0x3011) == 0)
		return 4;
	return 0;
}

/*
 * A uniform-colour frame must convert to the same neutral chroma at every output
 * pixel, for the measured RGB-IR mosaic and for plain Bayer. The old 3x3 window
 * missed the sparse R/B sites of the RGB-IR mosaic in drifting patches (the
 * cyan/yellow grid); reading the IR sites as red gave a cyan cast.
 */
/* Raw R, G, B levels of the uniform test frame. */
static uint16_t uniform_levels[3] = { 600, 400, 200 };

static int uniform_frame_worst_chroma(int rgbir, unsigned int ir, int *luma)
{
	const unsigned int width = 2592, height = 1944;
	static unsigned char out[OUT_FRAME_BYTES];
	uint16_t *frame = malloc((size_t)width * height * sizeof(*frame));
	int worst = 0, lo = 255, hi = 0;

	if (!frame)
		return 999;
	if (rgbir)
		set_rgbir_pattern();
	else if (set_bayer_pattern(MEDIA_BUS_FMT_SGRBG10_1X10))
		return 999;
	for (unsigned int y = 0; y < height; y++)
		for (unsigned int x = 0; x < width; x++)
			frame[(size_t)y * width + x] =
				(uint16_t[]){ uniform_levels[0], uniform_levels[1], uniform_levels[2], (uint16_t)ir }[channel_at(x, y)];
	if (convert_bayer_yuyv(frame, width, height, width * 2, out, sizeof(out), NULL)) {
		free(frame);
		return 999;
	}
	free(frame);
	for (size_t i = 0; i < OUT_FRAME_BYTES; i += 2) {
		int d = abs((int)out[i + 1] - 128);
		if (d > worst)
			worst = d;
		if (out[i] < lo) lo = out[i];
		if (out[i] > hi) hi = out[i];
	}
	if (luma)
		*luma = hi - lo > 1 ? -1 : lo;
	return worst;
}

static int test_no_cfa_grid(void)
{
	int luma;

	if (uniform_frame_worst_chroma(1, 120, &luma) > 2 || luma < 0)
		return 1;
	if (uniform_frame_worst_chroma(0, 120, &luma) > 2 || luma < 0)
		return 2;
	return 0;
}

/* IR leaking equally into R, G and B is removed: a grey patch stays grey and darker. */
static int test_ir_subtraction(void)
{
	const unsigned int width = 64, height = 64;
	static uint16_t frame[64 * 64];
	static unsigned char out[OUT_FRAME_BYTES];
	unsigned char luma_with_ir, luma_without_ir;

	set_rgbir_pattern();
	if (channel_at(1, 0) != CFA_IR || channel_at(0, 1) != 0 || channel_at(2, 1) != 2 ||
	    channel_at(0, 3) != 2 || channel_at(2, 3) != 0 || channel_at(1, 1) != 1)
		return 1;
	for (unsigned int ir = 0; ir < 2; ir++) {
		for (unsigned int y = 0; y < height; y++)
			for (unsigned int x = 0; x < width; x++)
				frame[y * width + x] = channel_at(x, y) == CFA_IR ?
					(uint16_t)(BLACK_LEVEL + (ir ? 300 : 0)) :
					(uint16_t)(BLACK_LEVEL + 400 + (ir ? 300 : 0));
		if (convert_bayer_yuyv(frame, width, height, width * 2, out, sizeof(out), NULL))
			return 2;
		for (size_t i = 0; i < OUT_FRAME_BYTES; i += 2)
			if (abs((int)out[i + 1] - 128) > 2)
				return 3;
		if (ir)
			luma_with_ir = out[OUT_FRAME_BYTES / 2];
		else
			luma_without_ir = out[OUT_FRAME_BYTES / 2];
	}
	return abs((int)luma_with_ir - (int)luma_without_ir) <= 2 ? 0 : 4;
}

/* The phase statistics must separate a 2x2 Bayer tile and a 4x4 RGB-IR tile. */
static int test_cfa_phase_means(void)
{
	const unsigned int width = 64, height = 64;
	static uint16_t frame[64 * 64];
	double m[4][4];

	for (unsigned int y = 0; y < height; y++)
		for (unsigned int x = 0; x < width; x++)
			frame[y * width + x] = (uint16_t)(BLACK_LEVEL + 50 * (1 + (y & 3) * 4 + (x & 3)));
	cfa_phase_means(frame, width, height, width * 2, m);
	for (int dy = 0; dy < 4; dy++)
		for (int dx = 0; dx < 4; dx++)
			if (m[dy][dx] < 50.0 * (1 + dy * 4 + dx) - 0.5 ||
			    m[dy][dx] > 50.0 * (1 + dy * 4 + dx) + 0.5)
				return 1;
	return 0;
}


/* Fill a GRBG Bayer frame from per-channel raw levels chosen by a callback. */
static void fill_bayer(uint16_t *frame, unsigned int w, unsigned int h,
		       void (*levels)(unsigned int x, unsigned int y, unsigned int rgb[3]))
{
	for (unsigned int y = 0; y < h; y++)
		for (unsigned int x = 0; x < w; x++) {
			unsigned int rgb[3];
			levels(x, y, rgb);
			frame[y * w + x] = (uint16_t)(BLACK_LEVEL + rgb[channel_at(x, y)]);
		}
}

static void warm_cool_halves(unsigned int x, unsigned int y, unsigned int rgb[3])
{
	(void)y;
	rgb[0] = x < 32 ? 500 : 300; rgb[1] = 300; rgb[2] = x < 32 ? 300 : 500;
}

static unsigned int noise_state = 12345;
static void noisy_grey(unsigned int x, unsigned int y, unsigned int rgb[3])
{
	(void)x; (void)y;
	for (int c = 0; c < 3; c++) {
		noise_state = noise_state * 1103515245u + 12345u;
		rgb[c] = 300 + (noise_state >> 16) % 120;
	}
}

static unsigned int grey_level;
static void flat_grey(unsigned int x, unsigned int y, unsigned int rgb[3])
{
	(void)x; (void)y;
	rgb[0] = rgb[1] = rgb[2] = grey_level;
}

/* Saturation widens chroma for coloured areas (greys stay grey: test_no_cfa_grid). */
static int test_saturation(void)
{
	static uint16_t frame[64 * 64];
	static unsigned char out[OUT_FRAME_BYTES];
	int saved_s = saturation_q12, saved_r = chroma_radius, v[2];

	set_bayer_pattern(MEDIA_BUS_FMT_SGRBG10_1X10);
	fill_bayer(frame, 64, 64, warm_cool_halves);
	chroma_radius = 0;
	for (int i = 0; i < 2; i++) {
		saturation_q12 = i ? 6554 : 4096;
		tnr_reset();
		if (convert_bayer_yuyv(frame, 64, 64, 128, out, sizeof(out), NULL))
			return 1;
		v[i] = out[(240 * OUT_WIDTH + 100) * 2 + 3] - 128;	/* warm half: V > 0 */
	}
	saturation_q12 = saved_s; chroma_radius = saved_r;
	return v[0] > 0 && v[1] > v[0] + 2 ? 0 : 2;
}

static double chroma_variance(const unsigned char *out)
{
	double sum = 0, sq = 0, n = 0;
	for (size_t i = 0; i < OUT_FRAME_BYTES; i += 4) {
		double u = out[i + 1];
		sum += u; sq += u * u; n++;
	}
	return sq / n - (sum / n) * (sum / n);
}

/* The chroma box filter lowers colour noise. */
static int test_chroma_smoothing(void)
{
	const unsigned int w = 2592, h = 1944;
	static unsigned char out[OUT_FRAME_BYTES];
	uint16_t *frame = malloc((size_t)w * h * sizeof(*frame));
	int saved_r = chroma_radius;
	double var[2];

	if (!frame)
		return 1;
	set_bayer_pattern(MEDIA_BUS_FMT_SGRBG10_1X10);
	noise_state = 12345;
	fill_bayer(frame, w, h, noisy_grey);
	for (int i = 0; i < 2; i++) {
		chroma_radius = i ? 2 : 0;
		tnr_reset();
		convert_bayer_yuyv(frame, w, h, w * 2, out, sizeof(out), NULL);
		var[i] = chroma_variance(out);
	}
	chroma_radius = saved_r;
	free(frame);
	return var[1] < var[0] / 4 ? 0 : 2;
}

/* Temporal filter: a small change moves a third of the way, a large one passes at once. */
static int luma_after(unsigned int first, unsigned int second, int filtered)
{
	static uint16_t frame[64 * 64];
	static unsigned char out[OUT_FRAME_BYTES];

	set_bayer_pattern(MEDIA_BUS_FMT_SGRBG10_1X10);
	tnr_reset();
	if (filtered) {
		grey_level = first; fill_bayer(frame, 64, 64, flat_grey);
		convert_bayer_yuyv(frame, 64, 64, 128, out, sizeof(out), NULL);
	}
	grey_level = second; fill_bayer(frame, 64, 64, flat_grey);
	convert_bayer_yuyv(frame, 64, 64, 128, out, sizeof(out), NULL);
	return out[(240 * OUT_WIDTH + 320) * 2];
}

static int test_temporal_filter(void)
{
	int a = luma_after(0, 300, 0), b_small = luma_after(0, 320, 0), b_big = luma_after(0, 600, 0);
	int f_small = luma_after(300, 320, 1), f_big = luma_after(300, 600, 1);
	int expect = a + (b_small - a + 1) / 3;

	if (b_small - a < 3 || b_small - a > TNR_THRESHOLD || b_big - a <= TNR_THRESHOLD)
		return 1;	/* fixture levels must straddle the threshold */
	if (f_small < expect - 1 || f_small > expect + 1)
		return 2;
	return f_big == b_big ? 0 : 3;
}


/* Calibrated matrices: load, pick the nearest illuminants, keep greys grey. Synthetic data only. */
static int test_ccm_table(void)
{
	char path[] = "/tmp/ccm.XXXXXX";
	int fd = mkstemp(path);
	FILE *f;

	if (fd < 0 || !(f = fdopen(fd, "w")))
		return 1;
	fputs("# id r/g b/g x y ccm\n"
	      "1 0.90 0.45 0.44 0.41 1.8 -0.4 -0.4 -0.3 2.0 -0.7 -0.5 -0.8 2.3\n"
	      "2 0.50 0.65 0.31 0.33 1.5 -0.5 0.0 -0.1 1.4 -0.3 0.0 -0.6 1.6\n"
	      "3 0.70 0.50 0.40 0.40 2.0 -1.0 0.0 -0.2 1.5 -0.3 -0.2 -1.0 2.2\n"
	      "4 0.70 0.50 0.40 0.40 9.0 0.0 0.0 0.0 1.0 0.0 0.0 0.0 1.0\n"   /* rows must sum to 1 */
	      "garbage line\n", f);
	fclose(f);
	int n = load_ccm_table(path);
	unlink(path);
	if (n != 3)
		return 2;
	select_ccm(0.70, 0.50);			/* exactly illuminant 3 */
	if (!ccm_active || ccm_pick[0] != 3 || ccm_weight < 0.999 || ccm_q12[0] != 8192)
		return 3;
	select_ccm(0.80, 0.475);		/* halfway between 1 and 3 */
	if (!((ccm_pick[0] == 1 && ccm_pick[1] == 3) || (ccm_pick[0] == 3 && ccm_pick[1] == 1)) ||
	    ccm_weight < 0.45 || ccm_weight > 0.55)
		return 4;
	for (int r = 0; r < 3; r++)
		if (abs(ccm_q12[r * 3] + ccm_q12[r * 3 + 1] + ccm_q12[r * 3 + 2] - 4096) > 3)
			return 5;
	/*
	 * With a table loaded, a grey lit by a calibrated illuminant still converts to
	 * neutral grey: raw levels whose IR-free white point is illuminant 3 (R/G 0.70,
	 * B/G 0.50): net R 280, G 400, B 200, IR 56, plus the black level 64.
	 */
	int luma;
	uniform_levels[0] = 400; uniform_levels[1] = 520; uniform_levels[2] = 320;
	int worst = uniform_frame_worst_chroma(1, 120, &luma);
	uniform_levels[0] = 600; uniform_levels[1] = 400; uniform_levels[2] = 200;
	if (worst > 2 || luma < 0)
		return 6;
	ccm_count = 0; ccm_active = 0;		/* back to generic saturation for later tests */
	return load_ccm_table("/nonexistent/ccm.txt") == 0 ? 0 : 7;
}


/* Synthetic lens-shading grid: 1.0 at the centre rising to `corner` at the edges (radial). */
static double synthetic_shading(double fx, double fy, double corner)
{
	double r2 = ((fx - 0.5) * (fx - 0.5) + (fy - 0.5) * (fy - 0.5)) / 0.5;
	return 1.0 + (corner - 1.0) * r2;
}

static int write_lsc_table(const char *path, double corner)
{
	FILE *f = fopen(path, "w");
	const int gw = 17, gh = 13;

	if (!f)
		return -1;
	fputs("# synthetic\n", f);
	for (int ill = 0; ill < 2; ill++) {
		fprintf(f, "lsc %d %.3f %.3f %d %d\n", ill + 1, ill ? 0.5 : 0.9, ill ? 0.6 : 0.45, gw, gh);
		for (int c = 0; c < 3; c++) {
			for (int y = 0; y < gh; y++)
				for (int x = 0; x < gw; x++)
					fprintf(f, "%.4f ", synthetic_shading((double)x / (gw - 1), (double)y / (gh - 1), corner));
			fputc('\n', f);
		}
	}
	fclose(f);
	return 0;
}

static int test_lens_shading(void)
{
	char path[] = "/tmp/lsc.XXXXXX";
	int fd = mkstemp(path);
	const unsigned int w = 2592, h = 1944;

	if (fd < 0)
		return 1;
	close(fd);
	if (write_lsc_table(path, 4.0) || load_lsc_table(path) != 2 || lsc_table[0].gw != 17)
		return 2;
	unlink(path);

	/* Gain map: ~1.0 at the centre, ~4.0 near a corner at low sensor gain. */
	sensor_total_gain = 1.0;
	update_lsc(0.9, 0.45, w, h);
	if (!lsc_active || lsc_pick[0] != 1 || lsc_strength != 1.0)
		return 3;
	double centre = lsc_map[1][OUT_HEIGHT / 2][OUT_WIDTH / 2] / 4096.0;
	double corner = lsc_map[0][0][0] / 4096.0;
	if (centre < 0.98 || centre > 1.03 || corner < 3.7 || corner > 4.05)
		return 4;
	/* At 64x sensor gain the correction eases to 60%: 4^0.6 ~ 2.3 at the corner. */
	sensor_total_gain = 64.0;
	update_lsc(0.9, 0.45, w, h);
	corner = lsc_map[0][0][0] / 4096.0;
	if (lsc_strength > 0.61 || corner < 2.1 || corner > 2.35)
		return 5;

	/* A vignetted flat grey field (raw = flat / shading) comes out flat after correction. */
	sensor_total_gain = 1.0;
	set_bayer_pattern(MEDIA_BUS_FMT_SGRBG10_1X10);
	uint16_t *frame = malloc((size_t)w * h * sizeof(*frame));
	static unsigned char out[OUT_FRAME_BYTES];
	if (!frame)
		return 6;
	for (unsigned int y = 0; y < h; y++)
		for (unsigned int x = 0; x < w; x++)
			frame[(size_t)y * w + x] = (uint16_t)(BLACK_LEVEL + 600.0 /
				synthetic_shading((x + 0.5) / w, (y + 0.5) / h, 4.0) + 0.5);
	tnr_reset();
	int saved_r = chroma_radius;
	chroma_radius = 0;
	if (convert_bayer_yuyv(frame, w, h, w * 2, out, sizeof(out), NULL)) {
		free(frame);
		return 7;
	}
	chroma_radius = saved_r;
	free(frame);
	int lo = 255, hi = 0;
	for (unsigned int y = 8; y < OUT_HEIGHT - 8; y += 4)
		for (unsigned int x = 8; x < OUT_WIDTH - 8; x += 4) {
			int v = out[(y * OUT_WIDTH + x) * 2];
			if (v < lo) lo = v;
			if (v > hi) hi = v;
		}
	lsc_count = 0; lsc_active = 0;		/* later tests run without shading */
	return hi - lo <= 6 ? 0 : 8;
}


/* Highlights: a fully clipped RGB-IR area (lamp) is white, not black; green-only clipping is not magenta. */
static int clipped_frame(unsigned int r, unsigned int g, unsigned int b, unsigned int ir,
			 int *luma, int *u, int *v)
{
	static uint16_t frame[64 * 64];
	static unsigned char out[OUT_FRAME_BYTES];

	set_rgbir_pattern();
	for (unsigned int y = 0; y < 64; y++)
		for (unsigned int x = 0; x < 64; x++)
			frame[y * 64 + x] = (uint16_t)(unsigned int[]){ r, g, b, ir }[channel_at(x, y)];
	tnr_reset();
	if (convert_bayer_yuyv(frame, 64, 64, 128, out, sizeof(out), NULL))
		return -1;
	size_t at = (240 * OUT_WIDTH + 320) * 2;
	*luma = out[at]; *u = out[at + 1] - 128; *v = out[at + 3] - 128;
	return 0;
}

static int test_highlights(void)
{
	int y, u, v;

	if (clipped_frame(1023, 1023, 1023, 1023, &y, &u, &v) || y < 250 || abs(u) > 2 || abs(v) > 2)
		return 1;
	/* Green at full scale, R/B/IR well below: the window is mostly faded to white. */
	if (clipped_frame(700, 1023, 650, 300, &y, &u, &v) || y < 235 || abs(u) > 12 || abs(v) > 12)
		return 2;
	/* Below the threshold nothing changes: a mid grey stays mid grey. */
	if (clipped_frame(500, 500, 500, 100, &y, &u, &v) || y > 235 || abs(u) > 2 || abs(v) > 2)
		return 3;
	return 0;
}


/* White balance stays near the calibrated illuminant locus (synthetic three-point locus). */
static int test_wb_locus(void)
{
	char path[] = "/tmp/locus.XXXXXX";
	int fd = mkstemp(path);
	FILE *f;
	double rg, bg;

	if (fd < 0 || !(f = fdopen(fd, "w")))
		return 1;
	fputs("1 0.90 0.45 0.44 0.41 1 0 0 0 1 0 0 0 1\n"
	      "2 0.70 0.50 0.40 0.40 1 0 0 0 1 0 0 0 1\n"
	      "3 0.50 0.65 0.31 0.33 1 0 0 0 1 0 0 0 1\n", f);
	fclose(f);
	if (load_ccm_table(path) != 3) {
		unlink(path);
		return 2;
	}
	unlink(path);
	rg = 0.80; bg = 0.475;			/* on the locus, between 1 and 2 */
	constrain_white_point(&rg, &bg);
	if (fabs(rg - 0.80) > 1e-6 || fabs(bg - 0.475) > 1e-6)
		return 3;
	rg = 0.60; bg = 0.40;			/* pulled off by a coloured scene: 0.12 away */
	constrain_white_point(&rg, &bg);
	if (wb_locus_distance < 0.1 || fabs(hypot(rg - 0.60, bg - 0.40)) < 0.05)
		return 4;
	double d_after = 1e9;			/* now within the band of the locus */
	for (double t = 0; t <= 1.0; t += 0.001) {
		double px = 0.70 - 0.20 * t, py = 0.50 + 0.15 * t;
		double d = hypot(rg - px, bg - py);
		if (d < d_after) d_after = d;
	}
	if (d_after > WB_LOCUS_BAND + 1e-3)
		return 5;
	rg = 1.30; bg = 0.30;			/* beyond the warmest calibration */
	constrain_white_point(&rg, &bg);
	if (hypot(rg - 0.90, bg - 0.45) > WB_LOCUS_BAND + 1e-6)
		return 6;
	ccm_count = 0; ccm_active = 0;
	rg = 0.60; bg = 0.40;			/* no table: grey-world unchanged */
	constrain_white_point(&rg, &bg);
	return rg == 0.60 && bg == 0.40 ? 0 : 7;
}

int main(void)
{
	(void)capture_to_loopback;
	(void)probe;
	uint16_t raw[8 * 8];
	unsigned char out[OUT_FRAME_BYTES];
	double mean_green = 0;

	if (test_gamma())
		return 10 + test_gamma();
	if (test_auto_exposure())
		return 20 + test_auto_exposure();
	if (test_no_cfa_grid())
		return 40 + test_no_cfa_grid();
	if (test_cfa_phase_means())
		return 50;
	if (test_ir_subtraction())
		return 60 + test_ir_subtraction();
	/* Idle only when the module positively reports no streaming consumer. */
	if (bridge_should_convert(0) || !bridge_should_convert(1) || !bridge_should_convert(-1))
		return 70;
	if (test_saturation())
		return 80 + test_saturation();
	if (test_chroma_smoothing())
		return 85 + test_chroma_smoothing();
	if (test_temporal_filter())
		return 90 + test_temporal_filter();
	if (test_ccm_table())
		return 100 + test_ccm_table();
	if (test_lens_shading())
		return 110 + test_lens_shading();
	if (test_highlights())
		return 120 + test_highlights();
	if (test_wb_locus())
		return 130 + test_wb_locus();
	for (unsigned int y = 0; y < 8; y++)
		for (unsigned int x = 0; x < 8; x++)
			raw[y * 8 + x] = (uint16_t)(x + y * 8);

	if (test_bayer_patterns())
		return 30 + test_bayer_patterns();
	if (set_bayer_pattern(MEDIA_BUS_FMT_SGRBG10_1X10))
		return 1;
	if (channel_at(0, 0) != 1 || channel_at(1, 0) != 0 ||
	    channel_at(0, 1) != 2 || channel_at(1, 1) != 1 ||
	    channel_at(2, 2) != 1 || channel_at(3, 2) != 0)
		return 1;
	if (convert_bayer_yuyv(raw, 8, 8, 16, out, sizeof(out), NULL))
		return 2;
	if (convert_bayer_yuyv(raw, 8, 8, 15, out, sizeof(out), NULL) == 0)
		return 3;
	if (convert_bayer_yuyv(raw, 8, 8, 16, out, OUT_FRAME_BYTES - 1, NULL) == 0)
		return 4;
	set_rgbir_pattern();
	const unsigned int width = 2592, height = 1944, stride = width * 2;
	uint16_t *full = calloc((size_t)width * height, sizeof(*full));
	if (!full)
		return 5;
	for (unsigned int y = 0; y < height; y++)
		for (unsigned int x = 0; x < width; x++) {
			switch (channel_at(x, y)) {
			case 0: full[(size_t)y * width + x] = 512; break;
			case 1: full[(size_t)y * width + x] = 300; break;
			case 2: full[(size_t)y * width + x] = 200; break;
			default: full[(size_t)y * width + x] = 100; break;
			}
		}
	if (convert_bayer_yuyv(full, width, height, stride, out, sizeof(out), &mean_green) ||
	    out[0] < 40 || out[0] > 220 || mean_green < 230 || mean_green > 242) {
		free(full);
		return 6;
	}
	uint64_t u_sum = 0, v_sum = 0;
	for (unsigned int y = 0; y < OUT_HEIGHT; y++) {
		for (unsigned int x = 0; x < OUT_WIDTH; x += 2) {
			size_t offset = ((size_t)y * OUT_WIDTH + x) * 2;
			u_sum += out[offset + 1];
			v_sum += out[offset + 3];
		}
	}
	uint64_t chroma_samples = (uint64_t)OUT_WIDTH * OUT_HEIGHT / 2;
	if (u_sum / chroma_samples < 120 || u_sum / chroma_samples > 136 ||
	    v_sum / chroma_samples < 120 || v_sum / chroma_samples > 136) {
		free(full);
		return 7;
	}
	free(full);
	puts("IPU6_LOOPBACK_TEST_PASS");
	return 0;
}
