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
				(uint16_t[]){ 600, 400, 200, (uint16_t)ir }[channel_at(x, y)];
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
