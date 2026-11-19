#define CAMERA_BRIDGE_TEST
#include "../kernel/ipu6-loopback.c"
#include <stdarg.h>
#include <fcntl.h>
#include <sys/wait.h>

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

/*
 * OV5675 driver at 2592x1944@30: exposure 4..2016 lines (frame 1944 + vblank 76, margin 4),
 * analogue 128..2047, digital 1024..4095, VBLANK 76..30823.
 */
static void ov5675_ae(struct ae_state *ae, int with_vblank)
{
	memset(ae, 0, sizeof(*ae));
	ae->fd = -1;
	ae->enabled = 1;
	ae->exposure = (struct ae_control){ V4L2_CID_EXPOSURE, 4, 2016, 2016, 2016, 1, 2016 };
	ae->again = (struct ae_control){ V4L2_CID_ANALOGUE_GAIN, 128, 2047, 128, 128, 1, 128 };
	ae->dgain = (struct ae_control){ V4L2_CID_DIGITAL_GAIN, 1024, 4095, 1024, 1024, 1, 1024 };
	if (with_vblank)
		ae->vblank = (struct ae_control){ V4L2_CID_VBLANK, 76, 30823, 76, 76, 1, 76 };
	ae_frame_limits(ae, 1944);
}

static int test_auto_exposure(void)
{
	struct ae_state ae;

	ov5675_ae(&ae, 1);
	/* 2 x (1944 + 76) - 4: 15 fps at most. */
	if (ae.margin != 4 || ae.base_max != 2016 || ae.long_max != 4036)
		return 1;
	/* Dark scene at driver defaults (max exposure, 1x gain): raise analogue gain, bounded step. */
	if (!ae_update(&ae, AE_TARGET / 4) || ae.exposure.value != 2016 ||
	    ae.again.value != 205 || ae.dgain.value != 1024 || ae.vblank.value != 76)
		return 2;
	/* Within the deadband nothing changes. */
	if (ae_update(&ae, AE_TARGET * 1.05))
		return 3;
	/* Bright scene: gain comes down to 1x before exposure time is shortened. */
	for (int i = 0; i < 10; i++)
		ae_update(&ae, AE_TARGET * 4);
	if (ae.again.value != 128 || ae.dgain.value != 1024 || ae.exposure.value >= 2016 ||
	    ae.vblank.value != 76)
		return 4;
	/* Very dark scene: 15 fps frame, analogue gain capped at 15.5x, then digital gain. */
	for (int i = 0; i < 40; i++)
		ae_update(&ae, 0);
	if (ae.exposure.value != 4036 || ae.vblank.value != 4036 + 4 - 1944 ||
	    ae.again.value != 1984 || ae.dgain.value != 4095 || ae_update(&ae, 0))
		return 5;
	if (fabs(ae_frame_ms(&ae) - 2000.0 / 30) > 0.1)
		return 6;
	/* And back in bright light the frame returns to 30 fps. */
	for (int i = 0; i < 40; i++)
		ae_update(&ae, AE_TARGET * 4);
	if (ae.vblank.value != 76 || ae.exposure.value >= 2016 || ae.again.value != 128)
		return 7;
	/* Missing digital gain control: analogue gain still clamps at its cap. */
	ov5675_ae(&ae, 1);
	ae.dgain.present = 0;
	for (int i = 0; i < 40; i++)
		ae_update(&ae, 0);
	if (ae.again.value != 1984 || ae.exposure.value != 4036)
		return 8;
	/* No VBLANK control: the frame stays at 30 fps (the previous behaviour, with the cap). */
	ov5675_ae(&ae, 0);
	for (int i = 0; i < 40; i++)
		ae_update(&ae, 0);
	if (ae.vblank.present || ae.exposure.value != 2016 || ae.again.value != 1984 ||
	    ae.dgain.value != 4095)
		return 9;
	return 0;
}

/* The order of the exposure program: frame time, analogue gain to 4x, longer frame, gain. */
static int test_exposure_program(void)
{
	struct ae_state ae;

	ov5675_ae(&ae, 1);
	ae_plan(&ae, 2016 * 3.0);	/* 3x: gain at 30 fps */
	if (ae.exposure.value != 2016 || ae.again.value != 384 || ae.vblank.value != 76)
		return 1;
	ae_plan(&ae, 2016 * 6.0);	/* 6x: 4x gain, 1.5x frame */
	if (ae.exposure.value != 3024 || ae.again.value != 512 ||
	    ae.vblank.value != 3024 + 4 - 1944)
		return 2;
	ae_plan(&ae, 4036 * 10.0);	/* past the knee at 15 fps: analogue gain again */
	if (ae.exposure.value != 4036 || ae.again.value != 1280 || ae.dgain.value != 1024)
		return 3;
	ae_plan(&ae, 4036 * 15.5 * 2.0);	/* past the analogue cap: digital gain */
	if (ae.again.value != 1984 || ae.dgain.value != 2048)
		return 4;
	/* The default frame keeps the default VBLANK. */
	if (ae_vblank_for(&ae, 2016) != 76 || ae_vblank_for(&ae, 100) != 76)
		return 5;
	/* A sensor whose analogue gain tops out below the knee stretches the frame at its max. */
	ov5675_ae(&ae, 1);
	ae.again.max = 384;	/* 3x */
	ae_plan(&ae, 2016 * 4.5);
	if (ae.again.value != 384 || ae.exposure.value != 3024)
		return 6;
	/* A VBLANK limit shortens the longest exposure. */
	ov5675_ae(&ae, 1);
	ae.vblank.max = 576;
	ae_frame_limits(&ae, 1944);
	if (ae.long_max != 1944 + 576 - 4)
		return 7;
	/* Inconsistent ranges disable the frame stretch rather than guessing. */
	ov5675_ae(&ae, 1);
	ae.exposure.max = 3000;	/* more than the current frame allows */
	ae_frame_limits(&ae, 1944);
	if (ae.vblank.present || ae.long_max != 3000)
		return 8;
	return 0;
}

/* Settings go to the driver in an order its exposure bound accepts. */
static int test_apply_order(void)
{
	struct ae_state ae;

	ov5675_ae(&ae, 1);
	ae_set_log_reset();
	if (ae_apply(&ae, 4036, 2096, 1984, 1024, 76) || strcmp(ae_set_log, "VEAD") != 0)
		return 1;
	ae_set_log_reset();
	if (ae_apply(&ae, 2016, 76, 128, 1024, 2096) || strcmp(ae_set_log, "EVAD") != 0)
		return 2;
	ae_set_log_reset();
	if (ae_apply(&ae, 1000, 76, 128, 1024, 76) || strcmp(ae_set_log, "EAD") != 0)
		return 3;
	return 0;
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


/* A near-black frame keeps the previous white point and gains (no snap to the locus end). */
static int test_wb_dark_frame(void)
{
	static uint16_t frame[64 * 64];
	unsigned int gains_lit[3], gains_dark[3];

	set_bayer_pattern(MEDIA_BUS_FMT_SGRBG10_1X10);
	ccm_count = 0; ccm_active = 0;
	for (unsigned int y = 0; y < 64; y++)		/* lit: R/G 0.5, B/G 0.75 */
		for (unsigned int x = 0; x < 64; x++)
			frame[y * 64 + x] = (uint16_t)(BLACK_LEVEL + (unsigned int[]){ 200, 400, 300 }[channel_at(x, y)]);
	calculate_white_balance(frame, 64, 64, 128, gains_lit, NULL);
	double rg = wb_white_rg, bg = wb_white_bg;
	if (fabs(rg - 0.5) > 0.01 || fabs(bg - 0.75) > 0.01)
		return 1;
	for (unsigned int i = 0; i < 64 * 64; i++)	/* black: green 0, a little red/blue noise */
		frame[i] = (uint16_t)(BLACK_LEVEL + (channel_at(i % 64, i / 64) == 1 ? 0 : 3));
	calculate_white_balance(frame, 64, 64, 128, gains_dark, NULL);
	if (wb_white_rg != rg || wb_white_bg != bg)
		return 2;
	return gains_dark[0] == gains_lit[0] && gains_dark[2] == gains_lit[2] ? 0 : 3;
}

/* Front and rear profiles: entity names, CFA kind and calibration file prefixes. */
static int test_sensor_profiles(void)
{
	const struct sensor_profile *front = find_sensor_profile("ov5675");
	const struct sensor_profile *rear = find_sensor_profile("ov8856");

	if (!front || !front->rgbir || strcmp(front->cal_prefix, "ov5678"))
		return 1;
	if (!rear || rear->rgbir || strcmp(rear->cal_prefix, "ov8856"))
		return 2;
	if (find_sensor_profile("ov13b10") || find_sensor_profile("ov56") || find_sensor_profile(""))
		return 3;
	/* Without --sensor the bridge must still be the front camera. */
	return &sensor_profiles[0] == front ? 0 : 4;
}

static int cfa_has_ir(void)
{
	for (unsigned int y = 0; y < 4; y++)
		for (unsigned int x = 0; x < 4; x++)
			if (channel_at(x, y) == CFA_IR)
				return 1;
	return 0;
}

static int test_select_cfa(void)
{
	const struct sensor_profile *front = find_sensor_profile("ov5675");
	const struct sensor_profile *rear = find_sensor_profile("ov8856");

	/* Front: the measured RGB-IR map, only for the GRBG10 code it was measured with. */
	if (select_cfa(front, MEDIA_BUS_FMT_SGRBG10_1X10) || channel_at(1, 0) != CFA_IR ||
	    channel_at(0, 1) != 0 || channel_at(2, 1) != 2)
		return 1;
	if (select_cfa(front, MEDIA_BUS_FMT_SRGGB10_1X10) == 0)
		return 2;
	/* Rear: GRBG Bayer (Gr R / B Gb) and never an IR site. */
	if (select_cfa(rear, MEDIA_BUS_FMT_SGRBG10_1X10) || cfa_has_ir() ||
	    channel_at(0, 0) != 1 || channel_at(1, 0) != 0 || channel_at(0, 1) != 2 ||
	    channel_at(1, 1) != 1 || channel_at(3, 2) != 0)
		return 3;
	/* A flipped OV8856 reports BGGR; the Bayer order follows the code. */
	if (select_cfa(rear, MEDIA_BUS_FMT_SBGGR10_1X10) || cfa_has_ir() || channel_at(0, 0) != 2)
		return 4;
	if (select_cfa(rear, 0x3011) == 0)	/* not a 10-bit Bayer code */
		return 5;
	return 0;
}

static int parse(struct bridge_options *o, int argc, ...)
{
	char *argv[12] = { "ipu6-loopback" };
	va_list ap;

	va_start(ap, argc);
	for (int i = 1; i < argc && i < 12; i++)
		argv[i] = va_arg(ap, char *);
	va_end(ap);
	return parse_bridge_args(argc, argv, o);
}

static int test_parse_args(void)
{
	struct bridge_options o;

	/* The front launcher's existing command lines keep their meaning. */
	if (parse(&o, 3, "/dev/media0", "/dev/video64") || strcmp(o.profile->entity, "ov5675") ||
	    o.rotate180 || o.calibration || strcmp(o.output, "/dev/video64"))
		return 1;
	if (parse(&o, 4, "/dev/media0", "/dev/video64", "/cal") || strcmp(o.calibration, "/cal") ||
	    strcmp(o.profile->entity, "ov5675"))
		return 2;
	if (parse(&o, 8, "--sensor", "ov8856", "--rotate", "180", "/dev/media0", "/dev/video65", "/cal") ||
	    strcmp(o.profile->entity, "ov8856") || !o.rotate180 || strcmp(o.media, "/dev/media0") ||
	    strcmp(o.output, "/dev/video65") || strcmp(o.calibration, "/cal"))
		return 3;
	if (parse(&o, 5, "--rotate", "0", "/dev/media0", "/dev/video65") || o.rotate180)
		return 4;
	if (!parse(&o, 5, "--sensor", "imx258", "/dev/media0", "/dev/video65") ||
	    !parse(&o, 5, "--rotate", "90", "/dev/media0", "/dev/video65") ||
	    !parse(&o, 5, "--mirror", "1", "/dev/media0", "/dev/video65") ||
	    !parse(&o, 4, "--sensor", "ov8856", "/dev/media0") ||
	    !parse(&o, 2, "/dev/media0") ||
	    !parse(&o, 6, "/dev/media0", "/dev/video65", "/cal", "extra", "more"))
		return 5;
	return 0;
}

/* OV8856 at 3280x2464@30 (ov8856.c): exposure 6..2482, analogue 128..2047, digital 0..4095 (1024 = 1x). */
static int test_ov8856_ae(void)
{
	struct ae_state ae;
	struct v4l2_queryctrl q;

	memset(&ae, 0, sizeof(ae));
	ae.fd = -1;
	ae.enabled = 1;
	q = (struct v4l2_queryctrl){ .id = V4L2_CID_EXPOSURE, .minimum = 6, .maximum = 2482, .default_value = 2482 };
	if (ae_control_from_query(&ae.exposure, &q, 2482))
		return 1;
	q = (struct v4l2_queryctrl){ .id = V4L2_CID_ANALOGUE_GAIN, .minimum = 128, .maximum = 2047, .default_value = 128 };
	if (ae_control_from_query(&ae.again, &q, 128))
		return 2;
	q = (struct v4l2_queryctrl){ .id = V4L2_CID_DIGITAL_GAIN, .minimum = 0, .maximum = 4095, .default_value = 1024 };
	if (ae_control_from_query(&ae.dgain, &q, 1024) || ae.dgain.min != 1024 || ae.dgain.saved != 1024)
		return 3;
	/* Only a digital gain may borrow its default as unity: a zero exposure minimum is refused. */
	struct ae_control c;
	q = (struct v4l2_queryctrl){ .id = V4L2_CID_EXPOSURE, .minimum = 0, .maximum = 2482, .default_value = 100 };
	if (ae_control_from_query(&c, &q, 100) == 0)
		return 4;
	q = (struct v4l2_queryctrl){ .id = V4L2_CID_DIGITAL_GAIN, .minimum = 0, .maximum = 4095, .default_value = 0 };
	if (ae_control_from_query(&c, &q, 0) == 0)
		return 5;
	q = (struct v4l2_queryctrl){ .id = V4L2_CID_ANALOGUE_GAIN, .minimum = 128, .maximum = 2047,
				     .flags = V4L2_CTRL_FLAG_READ_ONLY };
	if (ae_control_from_query(&c, &q, 128) == 0)
		return 6;
	/* The front's ranges (digital 1024..4095) are taken as they are. */
	q = (struct v4l2_queryctrl){ .id = V4L2_CID_DIGITAL_GAIN, .minimum = 1024, .maximum = 4095, .default_value = 1024 };
	if (ae_control_from_query(&c, &q, 1024) || c.min != 1024)
		return 7;
	/* Without a VBLANK control the frame stays fixed (3280x2464 mode). */
	ae_frame_limits(&ae, 2464);
	if (ae.base_max != 2482 || ae.long_max != 2482)
		return 10;
	/* Dark: exposure, then analogue gain (capped at 15.5x, the OV8856 Windows tuning's automatic
	 * maximum too), then digital gain up to its maximum; never below 1x. */
	for (int i = 0; i < 40; i++)
		ae_update(&ae, 0);
	if (ae.exposure.value != 2482 || ae.again.value != 1984 || ae.dgain.value != 4095)
		return 8;
	/* Bright: digital and analogue gain back to 1x before exposure time shortens. */
	for (int i = 0; i < 40; i++)
		ae_update(&ae, AE_TARGET * 4);
	if (ae.dgain.value != 1024 || ae.again.value != 128 || ae.exposure.value >= 2482 ||
	    ae.exposure.value < 6)
		return 9;
	return 0;
}

/* --rotate 180 turns the output: a bright top-left corner ends up bottom-right. */
static void bright_top_left(unsigned int x, unsigned int y, unsigned int rgb[3])
{
	rgb[0] = rgb[1] = rgb[2] = x < 32 && y < 32 ? 700 : 60;
}

static int test_rotation(void)
{
	static uint16_t frame[64 * 64];
	static unsigned char out[OUT_FRAME_BYTES];
	int saved_r = chroma_radius, tl[2], br[2];

	set_bayer_pattern(MEDIA_BUS_FMT_SGRBG10_1X10);
	fill_bayer(frame, 64, 64, bright_top_left);
	chroma_radius = 0;
	for (int i = 0; i < 2; i++) {
		output_rotate180 = i;
		tnr_reset();
		if (convert_bayer_yuyv(frame, 64, 64, 128, out, sizeof(out), NULL))
			return 1;
		tl[i] = out[(40 * OUT_WIDTH + 40) * 2];
		br[i] = out[((OUT_HEIGHT - 40) * OUT_WIDTH + OUT_WIDTH - 40) * 2];
	}
	output_rotate180 = 0;
	chroma_radius = saved_r;
	if (tl[0] < br[0] + 60)
		return 2;
	/* Rotated: the same two levels, swapped. */
	return abs(tl[1] - br[0]) <= 2 && abs(br[1] - tl[0]) <= 2 ? 0 : 3;
}

/* Each bridge loads its own sensor's calibration files from the shared directory. */
static int test_calibration_prefix(void)
{
	char dir[] = "/tmp/cal.XXXXXX", path[256];
	FILE *f;

	if (!mkdtemp(dir))
		return 1;
	snprintf(path, sizeof(path), "%s/ov5678_ccm.txt", dir);
	if (!(f = fopen(path, "w")))
		return 2;
	fputs("1 0.90 0.45 0.44 0.41 1.8 -0.4 -0.4 -0.3 2.0 -0.7 -0.5 -0.8 2.3\n"
	      "2 0.50 0.65 0.31 0.33 1.5 -0.5 0.0 -0.1 1.4 -0.3 0.0 -0.6 1.6\n"
	      "3 0.70 0.50 0.40 0.40 2.0 -1.0 0.0 -0.2 1.5 -0.3 -0.2 -1.0 2.2\n", f);
	fclose(f);
	snprintf(path, sizeof(path), "%s/ov8856_ccm.txt", dir);
	if (!(f = fopen(path, "w")))
		return 3;
	fputs("19 0.55 0.70 0.31 0.33 1.6 -0.5 -0.1 -0.2 1.5 -0.3 0.0 -0.5 1.5\n"
	      "0 1.10 0.35 0.46 0.41 1.7 -0.5 -0.2 -0.3 1.8 -0.5 -0.1 -0.9 2.0\n", f);
	fclose(f);
	snprintf(path, sizeof(path), "%s/ov8856_lsc.txt", dir);
	if (write_lsc_table(path, 6.0))
		return 4;

	int result = 0;
	load_calibration(dir, "ov8856");
	if (ccm_count != 2 || ccm_table[0].id != 19 || ccm_table[1].id != 0 || lsc_count != 2)
		result = 5;
	load_calibration(dir, "ov5678");	/* front: its matrices, and no shading file here */
	if (!result && (ccm_count != 3 || lsc_count != 0))
		result = 6;
	snprintf(path, sizeof(path), "%s/ov5678_ccm.txt", dir); unlink(path);
	snprintf(path, sizeof(path), "%s/ov8856_ccm.txt", dir); unlink(path);
	snprintf(path, sizeof(path), "%s/ov8856_lsc.txt", dir); unlink(path);
	rmdir(dir);
	ccm_count = 0; ccm_active = 0; lsc_count = 0; lsc_active = 0;
	return result;
}

/* No consumer: the sensor runs at an 8x frame (about 3.75 fps) and comes straight back. */
static int test_idle_frame(void)
{
	struct ae_state ae;

	ov5675_ae(&ae, 1);
	if (ae.idle_vblank != 8 * (1944 + 76) - 1944 || ae.applied_vblank != 76)
		return 1;
	ae_set_log_reset();
	ae_set_idle(&ae, 1);	/* longer frame first, exposure unchanged */
	if (ae.applied_vblank != ae.idle_vblank || strcmp(ae_set_log, "VEAD") != 0 ||
	    fabs(ae_frame_ms(&ae) - 8000.0 / 30) > 0.1)
		return 2;
	/* AE keeps planning while idle; the planned long frame stays below the idle frame. */
	ae_plan(&ae, 4036 * 10.0);
	if (ae.vblank.value != 2096 || ae_effective_vblank(&ae) != ae.idle_vblank)
		return 3;
	ae_set_log_reset();
	ae_set_idle(&ae, 0);	/* back to the plan: exposure is already within it */
	if (ae.applied_vblank != 2096 || strcmp(ae_set_log, "EVAD") != 0)
		return 4;
	ae_set_log_reset();
	ae_set_idle(&ae, 0);	/* no change, no writes */
	if (ae_set_log[0])
		return 5;
	/* Without VBLANK nothing is written. */
	ov5675_ae(&ae, 0);
	ae_set_log_reset();
	ae_set_idle(&ae, 1);
	if (ae_set_log[0] || ae_effective_vblank(&ae) != 0)
		return 6;
	/* A small VBLANK range caps the idle frame. */
	ov5675_ae(&ae, 1);
	ae.vblank.max = 1000;
	ae_frame_limits(&ae, 1944);
	return ae.idle_vblank == 1000 ? 0 : 7;
}

/* Bad frames are skipped; only 10 s without a good one gives up (boot-9/10 exited on one). */
static int test_frame_health(void)
{
	struct frame_health h;

	frame_health_start(&h, 100.0);
	if (frame_health_bad(&h, 101.0, "error-marked", V4L2_BUF_FLAG_ERROR, 0) ||
	    frame_health_bad(&h, 109.9, "short", 0, 100) || h.bad_run != 2)
		return 1;
	frame_health_good(&h, 110.0);
	if (h.bad_run || h.recoveries != 1 || h.bad_total != 2)
		return 2;
	for (double t = 110.5; t < 119.9; t += 0.5)
		if (frame_health_bad(&h, t, "error-marked", V4L2_BUF_FLAG_ERROR, 0))
			return 3;
	if (!frame_health_bad(&h, 120.1, "error-marked", V4L2_BUF_FLAG_ERROR, 0))
		return 4;
	/* Poll timeouts age the run without counting as frames. */
	frame_health_start(&h, 0.0);
	if (frame_health_bad(&h, 5.0, "timeout", 0, 0) || h.bad_run ||
	    !frame_health_bad(&h, 10.5, "timeout", 0, 0))
		return 5;
	return 0;
}

/* RGB-IR frames for the colour-noise tests: noisy grey (mode 0) or a neutral dark/bright edge. */
static unsigned int rgbir_noise = 7;
static void fill_rgbir_test(uint16_t *f, unsigned int w, unsigned int h, int mode)
{
	static const double level[4] = { 100, 160, 85, 19 };	/* R G B IR above black, boot-11-like */
	for (unsigned int y = 0; y < h; y++)
		for (unsigned int x = 0; x < w; x++) {
			int ch = channel_at(x, y);
			double v;
			if (mode == 0) {
				rgbir_noise = rgbir_noise * 1103515245u + 12345u;
				v = level[ch] + (double)((rgbir_noise >> 16) % 121) - 60;
			} else {
				double base = x < w / 2 ? 60 : 600;
				v = ch == CFA_IR ? base * 0.1 : base;
			}
			f[(size_t)y * w + x] = (uint16_t)(BLACK_LEVEL + (v < 0 ? 0 : v));
		}
}

/* Mean U, V over the frame (mode 0) or the largest |U|, |V| next to the edge (mode 1). */
static void rgbir_test_chroma(int mode, int radius, double gain, double *a, double *b)
{
	const unsigned int w = 2592, h = 1944;
	static unsigned char out[OUT_FRAME_BYTES];
	uint16_t *f = malloc((size_t)w * h * sizeof(*f));
	int saved_r = chroma_radius;
	double saved_g = sensor_total_gain, su = 0, sv = 0, n = 0, mx = 0;

	*a = *b = 1e9;
	if (!f)
		return;
	set_rgbir_pattern();
	chroma_radius = radius;
	sensor_total_gain = gain;
	rgbir_noise = 7;
	fill_rgbir_test(f, w, h, mode);
	for (int k = 0; k < 3; k++) {	/* white balance settles */
		tnr_reset();
		convert_bayer_yuyv(f, w, h, w * 2, out, sizeof(out), NULL);
	}
	for (int y = 20; y < OUT_HEIGHT - 20; y++)
		for (int x = 20; x < OUT_WIDTH - 20; x += 2) {
			int u = out[((size_t)y * OUT_WIDTH + x) * 2 + 1] - 128;
			int v = out[((size_t)y * OUT_WIDTH + x) * 2 + 3] - 128;
			if (mode == 0) {
				su += u; sv += v; n++;
			} else if (x >= 310 && x <= 330) {
				if (abs(u) > mx) mx = abs(u);
				if (abs(v) > mx) mx = abs(v);
			}
		}
	*a = mode == 0 ? su / n : mx;
	*b = mode == 0 ? sv / n : mx;
	chroma_radius = saved_r;
	sensor_total_gain = saved_g;
	free(f);
}

/*
 * Noisy greys stay grey: colour from linear-light averages. The per-pixel path (radius 0,
 * the same gamma-domain averaging the earlier U/V box filter did) shows the green bias.
 */
static int test_noise_colour_bias(void)
{
	double u, v;

	rgbir_test_chroma(0, 2, 17.5, &u, &v);
	if (fabs(u) > 1.0 || fabs(v) > 1.0)
		return 1;
	rgbir_test_chroma(0, 0, 17.5, &u, &v);	/* negative control */
	if (u > -2.0 || v > -1.0)
		return 2;
	return 0;
}

/* A neutral edge keeps (almost) no colour; per pixel it shows a fringe. */
static int test_edge_colour(void)
{
	double hi, lo, unused;

	rgbir_test_chroma(1, 2, 17.5, &lo, &unused);
	rgbir_test_chroma(1, 0, 17.5, &hi, &unused);
	return lo <= 2 && hi >= lo + 3 ? 0 : 1;
}

/* Gain-scaled denoise steps. */
static int test_gain_denoise_steps(void)
{
	if (chroma_radius_for_gain(4) != 2 || chroma_radius_for_gain(10) != 3 ||
	    chroma_radius_for_gain(40) != 4)
		return 1;
	if (tnr_threshold_for_gain(4) != TNR_THRESHOLD || tnr_threshold_for_gain(32) != 2 * TNR_THRESHOLD ||
	    tnr_threshold_for_gain(100) != 2 * TNR_THRESHOLD)
		return 2;
	int saved = chroma_radius;
	chroma_radius = 0;
	int off = chroma_radius_for_gain(40);
	chroma_radius = saved;
	return off == 0 ? 0 : 3;
}

/* Synthetic lens: stripes whose contrast peaks when the lens is at `target`. */
static void af_test_frame(uint16_t *f, unsigned int w, unsigned int h, int pos, int target)
{
	double d = (pos - target) / 15.0, contrast = 1.0 / (1.0 + d * d);
	for (unsigned int y = 0; y < h; y++)
		for (unsigned int x = 0; x < w; x++)
			f[y * w + x] = (uint16_t)(BLACK_LEVEL + 300 + (((x / 3) & 1) ? 1 : -1) * 200 * contrast);
}

/* Contrast AF: coarse then fine scan ends at the sharpest position and holds AE meanwhile. */
static int test_autofocus(void)
{
	static uint16_t f[192 * 144];
	struct af_state af;
	int targets[] = { 337, 252, 461 };

	for (int t = 0; t < 3; t++) {
		memset(&af, 0, sizeof(af));
		af.fd = -1;
		af.present = 1;
		af.min = 0;
		af.max = 1023;
		af_move(&af, AF_START);
		af_start(&af);
		if (af.value != AF_COARSE_MIN || af.phase != 1)
			return 1;
		int frames = 0, busy = 1;
		while (busy && frames < 400) {
			af_test_frame(f, 192, 144, af.value, targets[t]);
			busy = af_frame(&af, f, 192, 144, 192 * 2);
			frames++;
		}
		if (busy || abs(af.value - targets[t]) > AF_FINE_STEP)
			return 2 + t;
		/* coarse 13 positions + fine 7, each settling AF_SETTLE frames then measured */
		if (frames > 21 * (AF_SETTLE + 1))
			return 5;
	}
	/* No lens (front): nothing to do, AE never held. */
	memset(&af, 0, sizeof(af));
	af.fd = -1;
	af_start(&af);
	return af_frame(&af, f, 192, 144, 192 * 2) == 0 && af.phase == 0 ? 0 : 6;
}

/*
 * 2026-11-19 (boot-12): boot12_daylight infrared, clipped highlights, idle stop.
 *
 * Synthetic three-illuminant locus and matrices (test data only, not the stick's tables).
 */
static int load_test_locus(void)
{
	char path[] = "/tmp/daylocus.XXXXXX";
	int fd = mkstemp(path);
	FILE *f;

	if (fd < 0 || !(f = fdopen(fd, "w")))
		return -1;
	fputs("1 0.90 0.45 0.44 0.41 1.8 -0.4 -0.4 -0.3 2.0 -0.7 -0.5 -0.8 2.3\n"
	      "3 0.70 0.50 0.40 0.40 2.0 -1.0 0.0 -0.2 1.5 -0.3 -0.2 -1.0 2.2\n"
	      "2 0.50 0.65 0.31 0.33 1.5 -0.5 0.0 -0.1 1.4 -0.3 0.0 -0.6 1.6\n", f);
	fclose(f);
	int n = load_ccm_table(path);
	unlink(path);
	return n == 3 ? 0 : -1;
}

/*
 * A uniform RGB-IR frame from per-site levels above black (R, G, B, IR), optionally with a
 * top band of other levels and Gaussian-ish noise; converted a few times so white balance
 * settles. Reports mean U, V of the lower centre and of the top band, and their luma.
 */
struct rgbir_scene {
	double level[4], top[4];
	unsigned int top_rows, clip;	/* clip: raw code where the sensor saturates (0 = 1023) */
	double sigma, gain;
};
struct scene_result { double y, u, v, top_y, top_u, top_v; };

static int run_rgbir_scene(const struct rgbir_scene *s, struct scene_result *r)
{
	const unsigned int w = 1296, h = 972;
	static unsigned char out[OUT_FRAME_BYTES];
	uint16_t *f = malloc((size_t)w * h * sizeof(*f));
	double saved_g = sensor_total_gain;
	unsigned int clip = s->clip ? s->clip : 1023, state = 99;

	if (!f)
		return -1;
	set_rgbir_pattern();
	sensor_total_gain = s->gain > 0 ? s->gain : 1.0;
	for (int k = 0; k < 4; k++) {
		for (unsigned int y = 0; y < h; y++)
			for (unsigned int x = 0; x < w; x++) {
				int ch = channel_at(x, y);
				double v = y < s->top_rows ? s->top[ch] : s->level[ch];
				if (s->sigma > 0) {
					double g = 0;
					for (int i = 0; i < 4; i++) {
						state = state * 1103515245u + 12345u;
						g += ((state >> 8) & 0xffff) / 65536.0 - 0.5;
					}
					v += s->sigma * g * 1.732 * sqrt(v / 178.0 + 0.05);
				}
				v += BLACK_LEVEL;
				if (v > clip) v = clip;
				f[(size_t)y * w + x] = (uint16_t)(v < 0 ? 0 : v + 0.5);
			}
		tnr_reset();
		if (convert_bayer_yuyv(f, w, h, w * 2, out, sizeof(out), NULL)) {
			free(f);
			return -1;
		}
	}
	free(f);
	sensor_total_gain = saved_g;
	memset(r, 0, sizeof(*r));
	double n = 0, tn = 0;
	unsigned int top_out = (unsigned int)((uint64_t)s->top_rows * OUT_HEIGHT / h);
	for (unsigned int oy = 0; oy < OUT_HEIGHT; oy++)
		for (unsigned int ox = 0; ox < OUT_WIDTH; ox += 2) {
			size_t at = ((size_t)oy * OUT_WIDTH + ox) * 2;
			double Y = out[at], U = out[at + 1] - 128, V = out[at + 3] - 128;
			if (oy > top_out + 16 && oy < OUT_HEIGHT - 16 && ox > OUT_WIDTH / 4 && ox < OUT_WIDTH * 3 / 4) {
				r->y += Y; r->u += U; r->v += V; n++;
			} else if (top_out > 32 && oy > 8 && oy + 16 < top_out && ox > 8 && ox + 8 < OUT_WIDTH) {
				r->top_y += Y; r->top_u += U; r->top_v += V; tn++;
			}
		}
	if (n) { r->y /= n; r->u /= n; r->v /= n; }
	if (tn) { r->top_y /= tn; r->top_u /= tn; r->top_v /= tn; }
	return 0;
}

/*
 * Boot-12 frame 900 (cfa4x4 log), by a window at 11:0x, 1x gain: raw R 135.6, G 198.6,
 * B 133.6, IR 68.9 above black. IR is 35% of green and raw R = raw B.
 */
static const struct rgbir_scene boot12_daylight = { { 135.6, 198.6, 133.6, 68.9 }, { 0 }, 0, 0, 0, 1.0 };

static void restore_colour_defaults(int keep_b, double nir_band)
{
	nir_keep_q12[2] = keep_b;
	wb_locus_nir_band = nir_band;
	clip_detect = 1;
	ccm_count = 0; ccm_active = 0;
}

/*
 * A boot12_daylight grey with strong infrared is neutral. Negative control: the old full IR
 * subtraction with the fixed 0.03 band (boot-12's bridge) pulls it to the locus: green.
 */
static int test_daylight_ir_grey(void)
{
	struct rgbir_scene sc = boot12_daylight;
	struct scene_result r;
	int keep = nir_keep_q12[2];
	double band = wb_locus_nir_band;
	int result = 0;

	if (load_test_locus())
		return 1;
	if (run_rgbir_scene(&sc, &r) || fabs(r.u) > 2 || fabs(r.v) > 2)
		result = 2;
	else if (wb_nir_ratio < 0.35 || wb_nir_ratio > 0.5 || wb_locus_distance > wb_locus_band)
		result = 3;	/* NIR ~0.43 of green; within the (widened) band */
	/* Negative control: no blue NIR term, fixed band. */
	nir_keep_q12[2] = 0;
	wb_locus_nir_band = 0;
	if (!result && (run_rgbir_scene(&sc, &r) || (fabs(r.u) <= 2 && fabs(r.v) <= 2)))
		result = 4;
	/* The NIR term alone already makes it neutral; the band alone does not. */
	nir_keep_q12[2] = keep;
	if (!result && (run_rgbir_scene(&sc, &r) || fabs(r.u) > 2 || fabs(r.v) > 2))
		result = 5;
	nir_keep_q12[2] = 0;
	wb_locus_nir_band = band;
	if (!result && (run_rgbir_scene(&sc, &r) || (fabs(r.u) <= 2 && fabs(r.v) <= 2)))
		result = 6;
	restore_colour_defaults(keep, band);
	return result;
}

/*
 * Without NIR (LED room, boot-11: IR 18.9, G 178.5 at 17.5x, noisy) nothing changes: neutral,
 * and within 0.5 of the old full subtraction.
 */
static int test_lowlight_led_grey(void)
{
	struct rgbir_scene sc = { { 121.8, 178.5, 104.5, 18.9 }, { 0 }, 0, 0, 40.0, 17.5 };
	struct scene_result r, old;
	int keep = nir_keep_q12[2];
	double band = wb_locus_nir_band;
	int result = 0;

	if (load_test_locus())
		return 1;
	if (run_rgbir_scene(&sc, &r) || fabs(r.u) > 1.5 || fabs(r.v) > 1.5)
		result = 2;
	else if (wb_nir_ratio > 0.05)
		result = 3;
	nir_keep_q12[2] = 0;
	wb_locus_nir_band = 0;
	if (!result && (run_rgbir_scene(&sc, &old) || fabs(old.u - r.u) > 0.5 || fabs(old.v - r.v) > 0.5))
		result = 4;
	restore_colour_defaults(keep, band);
	return result;
}

/*
 * Clipped highlights come out white, not pink. The daylit sky: green saturates (here at raw
 * 920, below full scale, as a sensor can) while R and B, under white-balance gains of ~1.85,
 * do not. Negative control: the fade keyed to full scale (clip detection off) leaves pink
 * (U +5, V +4 here).
 */
static int test_clipped_sky(void)
{
	struct rgbir_scene sc = boot12_daylight;
	struct scene_result r;
	int result = 0;

	/* Sky 2.5x brighter than green can hold: G clips, R/B/IR do not (little NIR in the sky). */
	sc.top[1] = 2000; sc.top[3] = 120; sc.top[0] = 0.48 * 1100 + 120; sc.top[2] = 0.70 * 1100 + 120;
	sc.top_rows = 972 / 3;
	sc.clip = 920;
	if (load_test_locus())
		return 1;
	if (run_rgbir_scene(&sc, &r) || r.top_y < 245 || fabs(r.top_u) > 2 || fabs(r.top_v) > 2)
		result = 2;
	else if (raw_clip != 920)
		result = 3;
	/* The clipped sky is left out of the white balance: the grey below stays neutral. */
	else if (fabs(r.u) > 2 || fabs(r.v) > 2)
		result = 4;
	clip_detect = 0;
	if (!result && (run_rgbir_scene(&sc, &r) || !(r.top_u > 2 && r.top_v > 2)))
		result = 5;	/* negative control: magenta (U > 0 and V > 0) */
	restore_colour_defaults(nir_keep_q12[2], wb_locus_nir_band);
	return result;
}

/* Colours past the top keep luminance and hue order instead of clipping per channel. */
static int test_soft_clip(void)
{
	int c[3] = { 6140, 2304, 8850 }, w[3] = { 3000, 2000, 1000 }, hot[3] = { 9000, 8000, 7000 };
	int64_t y0 = (2126LL * 6140 + 7152LL * 2304 + 722LL * 8850) / 10000;

	soft_clip_rgb(c);
	if (c[2] != (int)LINEAR_MAX || !(c[0] < c[2] && c[1] < c[0]))
		return 1;
	int64_t y1 = (2126LL * c[0] + 7152LL * c[1] + 722LL * c[2]) / 10000;
	if (llabs(y1 - y0) > y0 / 100)
		return 2;
	/* Per-channel clipping would give R = B (magenta): the soft clip keeps R well below B. */
	if (c[2] - c[0] < 100)
		return 3;
	soft_clip_rgb(w);	/* in range: untouched */
	if (w[0] != 3000 || w[1] != 2000 || w[2] != 1000)
		return 4;
	soft_clip_rgb(hot);	/* luminance past the top: white */
	return hot[0] == (int)LINEAR_MAX && hot[1] == (int)LINEAR_MAX && hot[2] == (int)LINEAR_MAX ? 0 : 5;
}

/*
 * Idle stop: no consumer for longer than the timeout stops the stream once; a consumer starts
 * it again; short gaps (camera switch) and an unknown reader count (old module) never stop it.
 * Negative control: timeout 0 (boot-12's behaviour) never stops, so suspend stays blocked.
 */
static int test_idle_stop(void)
{
	struct idle_state s;
	int stops = 0, starts = 0;

	idle_reset(&s);
	/* Boot: no consumer from t = 0; frames every 0.27 s (idle frame). */
	for (double t = 0; t < 14.9; t += 0.27)
		if (idle_decide(&s, 0, t, 15.0) != IDLE_KEEP)
			return 1;
	if (idle_decide(&s, 0, 15.0, 15.0) != IDLE_STOP)
		return 2;
	s.stopped = 1;
	/* Stopped: polls keep it stopped, no repeated stop. */
	for (double t = 15.0; t < 300; t += 1.0)
		if (idle_decide(&s, 0, t, 15.0) != IDLE_KEEP)
			return 3;
	/* A consumer streams: start once, then keep while it streams. */
	if (idle_decide(&s, 1, 300.2, 15.0) != IDLE_START)
		return 4;
	s.stopped = 0;
	for (double t = 300.3; t < 400; t += 0.033)
		if (idle_decide(&s, 1, t, 15.0) != IDLE_KEEP)
			return 5;
	/* Camera switch or mode change: a 3 s gap does not stop the sensor. */
	for (double t = 400; t < 403; t += 0.033)
		if (idle_decide(&s, 0, t, 15.0) != IDLE_KEEP)
			return 6;
	if (idle_decide(&s, 1, 403.1, 15.0) != IDLE_KEEP)
		return 7;
	/* The app closes: stop 15 s later, not before. */
	for (double t = 500; t < 514.9; t += 0.27)
		if (idle_decide(&s, 0, t, 15.0) != IDLE_KEEP)
			return 8;
	if (idle_decide(&s, 0, 515.0, 15.0) != IDLE_STOP)
		return 9;
	/* No readers_active control (readers -1): never stops. */
	idle_reset(&s);
	for (double t = 0; t < 1000; t += 1.0)
		if (idle_decide(&s, -1, t, 15.0) != IDLE_KEEP)
			return 10;
	/* Negative control: timeout 0 = stream forever (the old bridge). */
	idle_reset(&s);
	for (double t = 0; t < 1000; t += 1.0) {
		int a = idle_decide(&s, 0, t, 0);
		stops += a == IDLE_STOP;
		starts += a == IDLE_START;
	}
	return stops == 0 && starts == 0 ? 0 : 11;
}

/* Restart: the idle (8x) frame is dropped before the stream starts, exposure is kept. */
static int test_restart_exposure(void)
{
	struct ae_state ae;

	ov5675_ae(&ae, 1);
	ae_plan(&ae, 2016 * 3.0);	/* 3x gain at 30 fps, as the last scene needed */
	int exposure = ae.exposure.value, again = ae.again.value;
	ae_set_idle(&ae, 1);		/* consumer gone: idle frame, then the stop */
	if (ae.applied_vblank != ae.idle_vblank)
		return 1;
	ae_set_log_reset();
	ae_set_idle(&ae, 0);		/* consumer back: before STREAMON */
	if (ae.applied_vblank != 76 || strcmp(ae_set_log, "EVAD") != 0)
		return 2;
	return ae.exposure.value == exposure && ae.again.value == again ? 0 : 3;
}

static int test_idle_args(void)
{
	struct bridge_options o;

	if (parse(&o, 3, "/dev/media0", "/dev/video64") || o.idle_stop != IDLE_STOP_S_DEFAULT)
		return 1;
	if (parse(&o, 5, "--idle-stop", "0", "/dev/media0", "/dev/video64") || o.idle_stop != 0)
		return 2;
	if (parse(&o, 7, "--sensor", "ov8856", "--idle-stop", "30", "/dev/media0", "/dev/video65") ||
	    o.idle_stop != 30 || strcmp(o.profile->entity, "ov8856"))
		return 3;
	if (!parse(&o, 5, "--idle-stop", "-1", "/dev/media0", "/dev/video64") ||
	    !parse(&o, 5, "--idle-stop", "x", "/dev/media0", "/dev/video64") ||
	    !parse(&o, 5, "--idle-stop", "5s", "/dev/media0", "/dev/video64") ||
	    !parse(&o, 4, "--idle-stop", "/dev/media0", "/dev/video64"))
		return 4;
	/* gbos.cfg CAMERA_IDLE_STOP via the environment. */
	double v = 15;
	if (idle_stop_from_env("0", &v) || v != 0 || idle_stop_from_env("45", &v) || v != 45)
		return 5;
	if (!idle_stop_from_env(NULL, &v) || !idle_stop_from_env("", &v) ||
	    !idle_stop_from_env("ten", &v) || !idle_stop_from_env("-3", &v) || v != 45)
		return 6;
	return 0;
}

/*
 * Fake IPU6 capture and loopback nodes for the idle stop/restart (boot-13: the bridges hung
 * after the first idle stop). The log records each call: S=STREAMOFF, s=STREAMON, R<n>=REQBUFS,
 * Q<i>=QBUF, U=QUERYBUF, E=DQEVENT.
 */
static char idle_log[128];
static int fake_events;		/* usage events pending on the loopback */
static int fake_dqevent_blocked;	/* a DQEVENT was issued with nothing pending */
static unsigned long fake_fail;	/* request that fails */
static int fake_fail_reqbufs3;	/* REQBUFS with count 3 fails */

static void idle_log_add(const char *fmt, ...)
{
	va_list ap;
	size_t n = strlen(idle_log);

	va_start(ap, fmt);
	vsnprintf(idle_log + n, sizeof(idle_log) - n, fmt, ap);
	va_end(ap);
}

static int fake_ioctl(int fd, unsigned long request, void *arg)
{
	(void)fd;
	if (request == VIDIOC_DQEVENT) {
		idle_log_add("E");
		if (!fake_events) {
			fake_dqevent_blocked = 1;	/* the real call would sleep here */
			errno = EAGAIN;
			return -1;
		}
		fake_events--;
		return 0;
	}
	if (request == VIDIOC_STREAMOFF)
		idle_log_add("S");
	else if (request == VIDIOC_STREAMON)
		idle_log_add("s");
	else if (request == VIDIOC_QBUF)
		idle_log_add("Q%u", ((struct v4l2_buffer *)arg)->index);
	else if (request == VIDIOC_QUERYBUF)
		idle_log_add("U");
	else if (request == VIDIOC_REQBUFS) {
		struct v4l2_requestbuffers *r = arg;
		idle_log_add("R%u", r->count);
		if (r->count == 3 && fake_fail_reqbufs3) {
			errno = EIO;
			return -1;
		}
	}
	if (request == fake_fail) {
		errno = EIO;
		return -1;
	}
	return 0;
}

static int fake_poll(struct pollfd *fds, nfds_t n, int timeout)
{
	(void)n;
	(void)timeout;
	fds[0].revents = fake_events ? POLLPRI : 0;
	return fake_events ? 1 : 0;
}

static void fake_reset(struct capture *c, int streaming)
{
	memset(c, 0, sizeof(*c));
	c->fd = 99;
	c->count = 3;
	c->streaming = streaming;
	idle_log[0] = 0;
	fake_events = 0;
	fake_dqevent_blocked = 0;
	fake_fail = 0;
	fake_fail_reqbufs3 = 0;
	bridge_ioctl_hook = fake_ioctl;
	bridge_poll_hook = fake_poll;
}

static int idle_quiet(int (*fn)(void))
{
	/* The steps log to stdout; keep the test output readable. */
	fflush(stdout);
	int saved = dup(STDOUT_FILENO), null = open("/dev/null", O_WRONLY);
	dup2(null, STDOUT_FILENO);
	int r = fn();
	fflush(stdout);
	dup2(saved, STDOUT_FILENO);
	close(saved);
	close(null);
	bridge_ioctl_hook = NULL;
	bridge_poll_hook = NULL;
	return r;
}

/* The drain must never issue a DQEVENT with nothing pending: on a blocking fd it sleeps. */
static int idle_drain_body(void)
{
	struct capture c;

	fake_reset(&c, 0);
	if (loopback_drain_events(7) != 0 || fake_dqevent_blocked || strcmp(idle_log, ""))
		return 1;
	fake_events = 3;
	if (loopback_drain_events(7) != 3 || fake_dqevent_blocked || strcmp(idle_log, "EEE"))
		return 2;
	fake_events = 40;	/* bounded per call */
	if (loopback_drain_events(7) != 16 || fake_events != 24)
		return 3;
	return 0;
}

static int test_idle_drain(void)
{
	return idle_quiet(idle_drain_body);
}

static int idle_pause_resume_body(void)
{
	struct capture c;
	struct af_state af = { .fd = -1 };

	/* Stop: STREAMOFF only, buffers kept; events drained without waiting. */
	fake_reset(&c, 1);
	fake_events = 1;
	if (idle_pause_stream(&c, &af, 7) || c.streaming || c.count != 3 ||
	    strcmp(idle_log, "SE") || fake_dqevent_blocked)
		return 1;
	/* Restart: requeue the kept buffers, stream on. No REQBUFS. */
	idle_log[0] = 0;
	if (idle_resume_stream(&c) || !c.streaming || strcmp(idle_log, "Q0Q1Q2s"))
		return 2;
	/* A second stop/restart cycle works the same. */
	idle_log[0] = 0;
	if (idle_pause_stream(&c, &af, 7) || idle_resume_stream(&c) || strcmp(idle_log, "SQ0Q1Q2s"))
		return 3;

	/* STREAMOFF fails: the buffers are released, the restart requests new ones. */
	fake_reset(&c, 1);
	fake_fail = VIDIOC_STREAMOFF;
	if (!idle_pause_stream(&c, &af, 7) || c.streaming || c.count || strcmp(idle_log, "SR0"))
		return 4;
	fake_fail = 0;
	fake_fail_reqbufs3 = 1;
	idle_log[0] = 0;
	if (!idle_resume_stream(&c) || c.streaming || strcmp(idle_log, "R3"))
		return 5;

	/* STREAMON fails with the kept buffers: release, start over; that fails too -> error. */
	fake_reset(&c, 0);
	fake_fail = VIDIOC_STREAMON;
	fake_fail_reqbufs3 = 1;
	if (!idle_resume_stream(&c) || c.streaming || c.count || strcmp(idle_log, "Q0Q1Q2sR0R3"))
		return 6;
	/* A QBUF that fails stops queueing at once. */
	fake_reset(&c, 0);
	fake_fail = VIDIOC_QBUF;
	fake_fail_reqbufs3 = 1;
	if (!idle_resume_stream(&c) || strcmp(idle_log, "Q0R0R3"))
		return 7;
	/* The lens is closed at the stop. */
	fake_reset(&c, 1);
	af.fd = open("/dev/null", O_RDONLY);
	af.present = 1;
	if (af.fd < 0 || idle_pause_stream(&c, &af, 7) || af.fd != -1 || af.present)
		return 8;
	return 0;
}

static int test_idle_pause_resume(void)
{
	return idle_quiet(idle_pause_resume_body);
}

/* A STREAMOFF that never returns: the watchdog ends the process with EXIT_IDLE_FAILED. */
static int blocking_ioctl(int fd, unsigned long request, void *arg)
{
	(void)fd;
	(void)arg;
	if (request == VIDIOC_STREAMOFF || request == VIDIOC_STREAMON)
		for (;;)
			pause();
	return 0;
}

static int test_idle_watchdog(void)
{
	for (int which = 0; which < 2; which++) {
		double t0 = monotonic_s();
		fflush(stdout);
		pid_t pid = fork();
		if (pid < 0)
			return 1;
		if (pid == 0) {
			int null = open("/dev/null", O_WRONLY);
			dup2(null, STDOUT_FILENO);
			dup2(null, STDERR_FILENO);
			struct capture c = { .fd = 99, .count = 3, .streaming = !which };
			struct af_state af = { .fd = -1 };
			idle_step_timeout_s = 1;
			bridge_ioctl_hook = blocking_ioctl;
			if (which == 0)
				idle_pause_stream(&c, &af, 7);
			else
				idle_resume_stream(&c);
			_exit(0);	/* the step returned: the watchdog did not fire */
		}
		int status, w;
		/* Our own deadline: a watchdog that never fires must fail the test, not hang it. */
		while ((w = waitpid(pid, &status, WNOHANG)) == 0 && monotonic_s() - t0 < 6.0)
			usleep(20000);
		if (w != pid) {
			kill(pid, SIGKILL);
			waitpid(pid, &status, 0);
			return 2;
		}
		double took = monotonic_s() - t0;
		if (!WIFEXITED(status) || WEXITSTATUS(status) != EXIT_IDLE_FAILED)
			return 3 + which;
		if (took < 0.9 || took > 4.0)
			return 5 + which;
	}
	/* Disarmed after a step that returned: no stray alarm later. */
	struct capture c;
	fake_reset(&c, 1);
	idle_step_timeout_s = 1;
	int r = idle_quiet(idle_drain_body);	/* any short step sequence */
	fake_reset(&c, 1);
	struct af_state af = { .fd = -1 };
	fflush(stdout);
	int saved = dup(STDOUT_FILENO), null = open("/dev/null", O_WRONLY);
	dup2(null, STDOUT_FILENO);
	idle_pause_stream(&c, &af, 7);
	fflush(stdout);
	dup2(saved, STDOUT_FILENO);
	close(saved);
	close(null);
	bridge_ioctl_hook = NULL;
	bridge_poll_hook = NULL;
	idle_step_timeout_s = IDLE_STEP_TIMEOUT_S;
	if (r || alarm(0) != 0)
		return 7;
	return 0;
}

/* A restart without a good frame within RESTART_FIRST_FRAME_S has failed. */
static int test_idle_restart_deadline(void)
{
	if (idle_restart_overdue(-1, 1000))
		return 1;	/* not restarting */
	if (idle_restart_overdue(100, 100 + RESTART_FIRST_FRAME_S - 0.1))
		return 2;
	if (!idle_restart_overdue(100, 100 + RESTART_FIRST_FRAME_S + 0.1))
		return 3;
	return EXIT_IDLE_FAILED == 4 && EXIT_IDLE_FAILED != 3 ? 0 : 4;
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
	if (test_exposure_program())
		return 150 + test_exposure_program();
	if (test_apply_order())
		return 160 + test_apply_order();
	if (test_idle_frame())
		return 200 + test_idle_frame();
	if (test_frame_health())
		return 210 + test_frame_health();
	if (test_noise_colour_bias())
		return 220 + test_noise_colour_bias();
	if (test_edge_colour())
		return 230 + test_edge_colour();
	if (test_gain_denoise_steps())
		return 240 + test_gain_denoise_steps();
	if (test_autofocus())
		return 250 + test_autofocus();
	{
		int r;
		if ((r = test_daylight_ir_grey()))
			return printf("test_daylight_ir_grey %d\n", r), 260 + r;
		if ((r = test_lowlight_led_grey()))
			return printf("test_lowlight_led_grey %d\n", r), 270 + r;
		if ((r = test_clipped_sky()))
			return printf("test_clipped_sky %d\n", r), 280 + r;
		if ((r = test_soft_clip()))
			return printf("test_soft_clip %d\n", r), 290 + r;
		if ((r = test_idle_stop()))
			return printf("test_idle_stop %d\n", r), 300 + r;
		if ((r = test_restart_exposure()))
			return printf("test_restart_exposure %d\n", r), 320 + r;
		if ((r = test_idle_args()))
			return printf("test_idle_args %d\n", r), 330 + r;
		if ((r = test_idle_drain()))
			return printf("test_idle_drain %d\n", r), 1;
		if ((r = test_idle_pause_resume()))
			return printf("test_idle_pause_resume %d\n", r), 1;
		if ((r = test_idle_watchdog()))
			return printf("test_idle_watchdog %d\n", r), 1;
		if ((r = test_idle_restart_deadline()))
			return printf("test_idle_restart_deadline %d\n", r), 1;
	}
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
	if (test_wb_dark_frame())
		return 140 + test_wb_dark_frame();
	if (test_sensor_profiles())
		return 150 + test_sensor_profiles();
	if (test_select_cfa())
		return 160 + test_select_cfa();
	if (test_parse_args())
		return 170 + test_parse_args();
	if (test_ov8856_ae())
		return 180 + test_ov8856_ae();
	if (test_rotation())
		return 190 + test_rotation();
	if (test_calibration_prefix())
		return 200 + test_calibration_prefix();
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
