/*
 * GPU (Vulkan compute) converter against the CPU converter: the same frames through both, the
 * YUYV outputs compared byte by byte. Host build with Mesa's lavapipe (a CPU Vulkan driver):
 * tools/test_ipu6_gpu.sh. The shaders use the CPU path's integer arithmetic, so outputs match
 * except where the CPU computes in double (colour guard, ratio mix): there one step at most.
 */
#define IPU6_LOOPBACK_TEST_LIB
#define IPU6_GPU
#include "test_ipu6_loopback.c"

static unsigned int scene_state;

static double scene_noise(double v, double sigma)
{
	double g = 0;
	for (int i = 0; i < 4; i++) {
		scene_state = scene_state * 1103515245u + 12345u;
		g += ((scene_state >> 8) & 0xffff) / 65536.0 - 0.5;
	}
	return v + sigma * g * 1.732 * sqrt(v / 178.0 + 0.05);
}

/* Scenes: 0 grey wall (boot-14 levels), 1 colour patches, 2 stripes (blinds), 3 NIR dark green over daylight, 4 highlights. */
static void gpu_scene(int id, uint16_t *f, unsigned int w, unsigned int h, double sigma)
{
	static const double patch[12][3] = {
		{ 160, 60, 40 }, { 60, 160, 50 }, { 50, 70, 160 }, { 150, 150, 40 },
		{ 40, 150, 150 }, { 150, 50, 150 }, { 120, 120, 120 }, { 60, 60, 60 },
		{ 200, 130, 90 }, { 90, 130, 60 }, { 70, 90, 130 }, { 180, 180, 180 },
	};
	static const double grey[4] = { 97.2, 159.3, 78.4, 14.3 };
	scene_state = 11u + (unsigned int)id;
	for (unsigned int y = 0; y < h; y++)
		for (unsigned int x = 0; x < w; x++) {
			int ch = channel_at(x, y);
			double v;
			if (id == 0) {
				v = grey[ch];
			} else if (id == 1) {
				const double *p = patch[(y * 3 / h) * 4 + x * 4 / w];
				v = ch == CFA_IR ? 0.1 * p[1] : p[ch] * 0.9 + 0.1 * p[1];
			} else if (id == 2) {
				v = grey[ch] * (((x / 3) & 1) ? 1.7 : 0.3);
			} else if (id == 3) {
				double l[4], top = y < h / 3;
				nir_sites(l, top ? 12 : 75, top ? 40 : 150, top ? 14 : 97.5, top ? 120 : 60, 0.75);
				v = l[ch];
			} else {
				v = grey[ch] * (x > w / 2 && y < h / 2 ? 8.0 : 1.0);	/* clipped quarter */
			}
			v = scene_noise(v, sigma) + BLACK_LEVEL;
			f[(size_t)y * w + x] = (uint16_t)(v < 0 ? 0 : v > 1023 ? 1023 : v + 0.5);
		}
}

/*
 * Three frames through one path (TNR runs on frames 2 and 3), from the same state each time:
 * lens shading starts inactive (the first frame's statistics are unshaded) and the white point
 * neutral, as at stream start.
 */
static int run_path(int use_gpu, const uint16_t *f, unsigned int w, unsigned int h, unsigned char *out)
{
	gpu.ok = use_gpu;
	lsc_active = 0;
	lsc_built.pick[0] = lsc_built.pick[1] = -2;
	wb_white_rg = wb_white_bg = 1.0;
	tnr_reset();
	for (int k = 0; k < 3; k++)
		if (convert_bayer_yuyv(f, w, h, w * 2, out, OUT_FRAME_BYTES, NULL))
			return -1;
	return use_gpu && !gpu.ok ? -1 : 0;	/* the GPU must not have fallen back */
}

static int compare(const char *name, const unsigned char *a, const unsigned char *b)
{
	size_t differ = 0, big = 0;
	int worst = 0;
	double sum = 0;

	for (size_t i = 0; i < OUT_FRAME_BYTES; i++) {
		int d = abs((int)a[i] - (int)b[i]);
		sum += d;
		if (d) differ++;
		if (d > 1) big++;
		if (d > worst) worst = d;
	}
	printf("%-34s differing bytes %zu (%.4f%%), >1: %zu, worst %d, mean %.5f\n", name, differ,
	       100.0 * differ / OUT_FRAME_BYTES, big, worst, sum / OUT_FRAME_BYTES);
	/*
	 * Integer stages identical; the float spots (colour guard, ratio mix) one step in a few pixels,
	 * rarely two after gamma (lavapipe: <= 33 bytes differ, 1 by 2).
	 */
	return worst <= 2 && big * 100000 < OUT_FRAME_BYTES && differ * 10000 < OUT_FRAME_BYTES ? 0 : 1;
}

int main(void)
{
	static unsigned char cpu_out[OUT_FRAME_BYTES], gpu_out[OUT_FRAME_BYTES];
	char lsc_path[] = "/tmp/gpu-lsc.XXXXXX";
	int fd, failed = 0;

	setvbuf(stdout, NULL, _IOLBF, 0);
	gpu.allow_cpu_device = 1;
	if (gpu_init()) {
		printf("IPU6_GPU_TEST_SKIP (no Vulkan device)\n");
		return 77;
	}
	printf("gpu: %s\n", gpu.name);
	if ((fd = mkstemp(lsc_path)) < 0)
		return 2;
	close(fd);

	const struct { const char *name; int rgbir, scene, lsc, rotate; double gain, sigma; } cases[] = {
		{ "front grey wall, 4x", 1, 0, 0, 0, 4.0, 10 },
		{ "front patches + lens shading", 1, 1, 1, 0, 2.0, 6 },
		{ "front stripes (fringe guard)", 1, 2, 0, 0, 2.0, 4 },
		{ "front NIR dark green", 1, 3, 1, 0, 2.0, 8 },
		{ "front highlights, 20x (edges)", 1, 4, 1, 0, 20.0, 30 },
		{ "front 40x, rotated", 1, 0, 0, 1, 40.0, 40 },
		{ "rear patches (Bayer)", 0, 1, 0, 1, 2.0, 6 },
		{ "rear stripes + lens shading", 0, 2, 1, 0, 9.0, 12 },
	};
	const unsigned int w = 1296, h = 972;
	uint16_t *f = malloc((size_t)w * h * sizeof(*f));
	if (!f || load_test_locus())
		return 2;
	for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
		if (cases[i].rgbir)
			set_rgbir_pattern();
		else
			set_bayer_pattern(MEDIA_BUS_FMT_SGRBG10_1X10);
		if (cases[i].lsc) {
			if (write_lsc_table(lsc_path, 3.0) || load_lsc_table(lsc_path) != 2)
				return 2;
		} else {
			lsc_count = 0;
		}
		output_rotate180 = cases[i].rotate;
		sensor_total_gain = cases[i].gain;
		gpu_scene(cases[i].scene, f, w, h, cases[i].sigma);
		if (run_path(0, f, w, h, cpu_out) || run_path(1, f, w, h, gpu_out)) {
			printf("%s: conversion failed\n", cases[i].name);
			failed = 1;
			continue;
		}
		failed |= compare(cases[i].name, cpu_out, gpu_out);
	}
	/* Negative control: the comparison catches a real difference (GPU without the fringe guard). */
	set_rgbir_pattern();
	lsc_count = 0;
	output_rotate180 = 0;
	sensor_total_gain = 2.0;
	gpu_scene(2, f, w, h, 4);
	run_path(0, f, w, h, cpu_out);
	chroma_guard = 0;
	run_path(1, f, w, h, gpu_out);
	chroma_guard = 1;
	if (!compare("control: GPU without fringe guard", cpu_out, gpu_out)) {
		printf("negative control was not detected\n");
		failed = 1;
	}
	unlink(lsc_path);
	free(f);
	output_rotate180 = 0;
	puts(failed ? "IPU6_GPU_TEST_FAIL" : "IPU6_GPU_TEST_PASS");
	return failed;
}
