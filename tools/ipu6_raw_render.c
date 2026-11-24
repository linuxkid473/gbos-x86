/*
 * Render a bridge raw dump (CAMERA_RAW_DUMP=N: raw-<sensor>-<n>.{raw,yuyv,txt} in a boot's log
 * directory) on the build machine, with the same converter, and compare it with the picture the
 * laptop made from that frame.
 *
 *   ipu6_raw_render [--gpu] DUMP_PREFIX CALIBRATION_DIR OUT.yuyv
 *       DUMP_PREFIX: e.g. build/boot-logs/boot-20/raw-ov5678-1 (without extension)
 *       CALIBRATION_DIR: the stick's gbos/camera (proprietary; local only)
 *   ipu6_raw_render --self-test
 *
 * Built at the dump's output size: gcc -DOUT_WIDTH=W -DOUT_HEIGHT=H (tools/ipu6_raw_render.sh
 * reads the size from the .txt). U and V are compared exactly; luma differs by the temporal
 * filter, which a single dumped frame cannot replay. tools/yuyv_to_png.py makes either picture a PNG.
 */
#define CAMERA_BRIDGE_TEST
#ifdef RAW_RENDER_GPU
#define IPU6_GPU
#endif
#include "../kernel/ipu6-loopback.c"

static int read_file(const char *path, void *buf, size_t size)
{
	FILE *f = fopen(path, "rb");
	size_t n = f ? fread(buf, 1, size, f) : 0;

	if (f)
		fclose(f);
	return n == size ? 0 : -1;
}

static int write_file(const char *path, const void *buf, size_t size)
{
	FILE *f = fopen(path, "wb");
	size_t n = f ? fwrite(buf, 1, size, f) : 0;

	if (f)
		fclose(f);
	return n == size ? 0 : -1;
}

struct dump_info {
	char sensor[32];
	unsigned int width, height, stride, out_w, out_h;
	int rotate;
	double gain;
};

static int read_info(const char *path, struct dump_info *d)
{
	FILE *f = fopen(path, "r");
	char line[256];

	if (!f)
		return -1;
	memset(d, 0, sizeof(*d));
	while (fgets(line, sizeof(line), f)) {
		sscanf(line, "sensor %31s", d->sensor);
		sscanf(line, "width %u", &d->width);
		sscanf(line, "height %u", &d->height);
		sscanf(line, "stride %u", &d->stride);
		sscanf(line, "out %ux%u", &d->out_w, &d->out_h);
		sscanf(line, "rotate %d", &d->rotate);
		sscanf(line, "gain %lf", &d->gain);
	}
	fclose(f);
	return d->width && d->height && d->stride ? 0 : -1;
}

/* Convert a frame as the bridge would after a few frames of the same scene (WB settled). */
static int render(const struct dump_info *d, const uint16_t *raw, unsigned char *out)
{
	if (!strcmp(d->sensor, "ov5678"))
		set_rgbir_pattern();
	else if (set_bayer_pattern(MEDIA_BUS_FMT_SGRBG10_1X10))
		return -1;
	sensor_total_gain = d->gain > 0 ? d->gain : 1.0;
	output_rotate180 = d->rotate;
	for (int k = 0; k < 4; k++) {
		tnr_reset();
		if (convert_bayer_yuyv(raw, d->width, d->height, d->stride, out, OUT_FRAME_BYTES, NULL))
			return -1;
	}
	return 0;
}

/* Mean |dU|, |dV| and |dY| between two YUYV pictures; the largest chroma difference. */
static void compare(const unsigned char *a, const unsigned char *b, double *du, double *dv, double *dy,
		    int *worst)
{
	double su = 0, sv = 0, sy = 0;
	size_t pairs = OUT_FRAME_BYTES / 4;

	*worst = 0;
	for (size_t i = 0; i < OUT_FRAME_BYTES; i += 4) {
		int u = abs(a[i + 1] - b[i + 1]), v = abs(a[i + 3] - b[i + 3]);
		su += u;
		sv += v;
		sy += abs(a[i] - b[i]) + abs(a[i + 2] - b[i + 2]);
		if (u > *worst) *worst = u;
		if (v > *worst) *worst = v;
	}
	*du = su / pairs;
	*dv = sv / pairs;
	*dy = sy / (2.0 * pairs);
}

/* Synthetic front frame through raw_dump() and back: the round trip must reproduce the chroma. */
static int self_test(void)
{
	char dir[] = "/tmp/rawdump.XXXXXX";
	const unsigned int w = 1296, h = 972;
	static unsigned char out[OUT_FRAME_BYTES], again[OUT_FRAME_BYTES];
	uint16_t *f = malloc((size_t)w * h * 2), *back = malloc((size_t)w * h * 2);
	struct ae_state ae;
	struct dump_info d;
	char path[600];
	unsigned int state = 5;

	if (!f || !back || !mkdtemp(dir))
		return 1;
	memset(&ae, 0, sizeof(ae));
	set_rgbir_pattern();
	for (unsigned int y = 0; y < h; y++)
		for (unsigned int x = 0; x < w; x++) {
			static const double lv[4] = { 110, 170, 85, 18 };
			state = state * 1103515245u + 12345u;
			double v = lv[channel_at(x, y)] * (x < w / 2 ? 1.0 : 0.6) + ((state >> 16) % 9);
			f[(size_t)y * w + x] = (uint16_t)(BLACK_LEVEL + v);
		}
	snprintf(d.sensor, sizeof(d.sensor), "ov5678");
	d.width = w; d.height = h; d.stride = w * 2; d.rotate = 0; d.gain = 2.0;
	if (render(&d, f, out))
		return 2;
	raw_dump_dir = dir;
	raw_dump_left = 1;
	raw_dump_start("ov5678", f, w, h, w * 2, out, &ae, 1);	/* synchronous for the test */
	snprintf(path, sizeof(path), "%s/raw-ov5678-1.txt", dir);
	if (read_info(path, &d) || d.out_w != OUT_WIDTH || d.out_h != OUT_HEIGHT)
		return 3;
	snprintf(path, sizeof(path), "%s/raw-ov5678-1.raw", dir);
	if (read_file(path, back, (size_t)w * h * 2) || memcmp(back, f, (size_t)w * h * 2))
		return 4;
	snprintf(path, sizeof(path), "%s/raw-ov5678-1.yuyv", dir);
	if (read_file(path, again, OUT_FRAME_BYTES) || memcmp(again, out, OUT_FRAME_BYTES))
		return 5;
	if (render(&d, back, again))
		return 6;
	double du, dv, dy;
	int worst;
	compare(out, again, &du, &dv, &dy, &worst);
	if (worst != 0)
		return 7;
	/* The bridge's path: written by a background thread; a dump asked for meanwhile is skipped. */
	raw_dump_left = 2;
	__atomic_store_n(&raw_dump_busy, 1, __ATOMIC_RELEASE);
	if (raw_dump_start("ov5678", f, w, h, w * 2, out, &ae, 0) != -1 || raw_dump_left != 2)
		return 8;
	__atomic_store_n(&raw_dump_busy, 0, __ATOMIC_RELEASE);
	if (raw_dump_start("ov5678", f, w, h, w * 2, out, &ae, 0))
		return 9;
	for (int ms = 0; __atomic_load_n(&raw_dump_busy, __ATOMIC_ACQUIRE); ms++) {
		if (ms > 10000)
			return 10;
		usleep(1000);
	}
	snprintf(path, sizeof(path), "%s/raw-ov5678-2.raw", dir);
	if (read_file(path, back, (size_t)w * h * 2) || memcmp(back, f, (size_t)w * h * 2))
		return 11;
	snprintf(path, sizeof(path), "%s/raw-ov5678-2.txt", dir);
	if (read_info(path, &d))
		return 12;
	for (int k = 1; k <= 2; k++)
		for (const char *ext = "raw"; ext; ext = !strcmp(ext, "raw") ? "yuyv" : !strcmp(ext, "yuyv") ? "txt" : NULL) {
			snprintf(path, sizeof(path), "%s/raw-ov5678-%d.%s", dir, k, ext);
			unlink(path);
		}
	rmdir(dir);
	free(f);
	free(back);
	puts("IPU6_RAW_RENDER_SELF_TEST_PASS");
	return 0;
}

int main(int argc, char **argv)
{
	static unsigned char out[OUT_FRAME_BYTES], hw[OUT_FRAME_BYTES];
	struct dump_info d;
	char path[600];
	int use_gpu = 0, i = 1;

	setvbuf(stdout, NULL, _IOLBF, 0);
	if (argc == 2 && !strcmp(argv[1], "--self-test"))
		return self_test();
	if (argc > 1 && !strcmp(argv[1], "--gpu")) {
		use_gpu = 1;
		i++;
	}
	if (argc - i != 3) {
		fprintf(stderr, "usage: %s [--gpu] DUMP_PREFIX CALIBRATION_DIR OUT.yuyv | --self-test\n", argv[0]);
		return 2;
	}
	snprintf(path, sizeof(path), "%s.txt", argv[i]);
	if (read_info(path, &d)) {
		fprintf(stderr, "cannot read %s\n", path);
		return 1;
	}
	if (d.out_w != OUT_WIDTH || d.out_h != OUT_HEIGHT) {
		fprintf(stderr, "dump is %ux%u, this build %dx%d: rebuild with -DOUT_WIDTH=%u -DOUT_HEIGHT=%u\n",
			d.out_w, d.out_h, OUT_WIDTH, OUT_HEIGHT, d.out_w, d.out_h);
		return 1;
	}
	uint16_t *raw = malloc((size_t)d.stride * d.height);
	snprintf(path, sizeof(path), "%s.raw", argv[i]);
	if (!raw || read_file(path, raw, (size_t)d.stride * d.height)) {
		fprintf(stderr, "cannot read %s\n", path);
		return 1;
	}
	load_calibration(argv[i + 1], d.sensor);
#ifdef RAW_RENDER_GPU
	if (use_gpu) {
		gpu.allow_cpu_device = 1;
		if (gpu_init())
			return 1;
	}
#else
	if (use_gpu) {
		fprintf(stderr, "--gpu needs a build with -DRAW_RENDER_GPU\n");
		return 2;
	}
#endif
	if (render(&d, raw, out) || write_file(argv[i + 2], out, OUT_FRAME_BYTES))
		return 1;
	printf("rendered %s (%s, gain %.1fx): wb %.3f/%.3f, ccm %d+%d, lsc %s\n", argv[i],
	       use_gpu ? "GPU" : "CPU", d.gain, wb_white_rg, wb_white_bg, ccm_pick[0], ccm_pick[1],
	       lsc_active ? "on" : "off");
	snprintf(path, sizeof(path), "%s.yuyv", argv[i]);
	if (read_file(path, hw, OUT_FRAME_BYTES) == 0) {
		double du, dv, dy;
		int worst;
		compare(out, hw, &du, &dv, &dy, &worst);
		printf("vs the laptop's picture: mean |dU| %.2f |dV| %.2f (worst %d), mean |dY| %.2f\n",
		       du, dv, worst, dy);
	}
	free(raw);
	return 0;
}
