#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdarg.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <math.h>
#include <time.h>
#include <poll.h>
#include <dirent.h>
#include <pthread.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/file.h>
#include <sys/wait.h>
#include <sys/sysmacros.h>
#include <linux/videodev2.h>
#include <linux/media.h>
#include <linux/v4l2-subdev.h>

#ifndef V4L2_PIX_FMT_IPU3_SBGGR10
#define V4L2_PIX_FMT_IPU3_SBGGR10 v4l2_fourcc('i', 'p', '3', 'b')
#define V4L2_PIX_FMT_IPU3_SGBRG10 v4l2_fourcc('i', 'p', '3', 'g')
#define V4L2_PIX_FMT_IPU3_SGRBG10 v4l2_fourcc('i', 'p', '3', 'G')
#define V4L2_PIX_FMT_IPU3_SRGGB10 v4l2_fourcc('i', 'p', '3', 'r')
#endif
#ifndef MEDIA_ENT_F_CAM_SENSOR
#define MEDIA_ENT_F_CAM_SENSOR 0x00020001
#endif
#ifndef MEDIA_ENT_F_LENS
#define MEDIA_ENT_F_LENS 0x00020003
#endif
#ifndef V4L2_CID_CAMERA_ORIENTATION
#define V4L2_CID_CAMERA_ORIENTATION (V4L2_CID_CAMERA_CLASS_BASE + 34)
#endif
#ifndef V4L2_CID_CAMERA_SENSOR_ROTATION
#define V4L2_CID_CAMERA_SENSOR_ROTATION (V4L2_CID_CAMERA_CLASS_BASE + 35)
#endif
#ifndef V4L2_CID_ANALOGUE_GAIN
#define V4L2_CID_ANALOGUE_GAIN (V4L2_CID_IMAGE_SOURCE_CLASS_BASE + 3)
#endif
#ifndef V4L2_CID_DIGITAL_GAIN
#define V4L2_CID_DIGITAL_GAIN (V4L2_CID_IMAGE_PROC_CLASS_BASE + 5)
#endif

#define CID_LOOPBACK_READERS ((V4L2_CID_USER_BASE | 0xf000) + 4)

#define NBUF 8
#define MAX_THREADS 8
#define MAX_MODES 16
#define RAW_MAX 1023

enum { CH_R, CH_G, CH_B };

static volatile sig_atomic_t g_run = 1;
static void on_signal(int s) { (void)s; g_run = 0; }

static void logf_(const char *fmt, ...) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	printf("[%6ld.%03ld] ", (long)ts.tv_sec, ts.tv_nsec / 1000000);
	va_list ap;
	va_start(ap, fmt);
	vprintf(fmt, ap);
	va_end(ap);
	putchar('\n');
}
#define LOG(...) logf_(__VA_ARGS__)

static double now_s(void) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec / 1e9;
}

static int xioctl(int fd, unsigned long req, void *arg) {
	int r;
	do r = ioctl(fd, req, arg); while (r < 0 && errno == EINTR);
	return r;
}

static struct {
	const char *sensor;
	const char *out;
	const char *name;
	const char *debug_dir;
	const char *lock;
	int rotate;
	int mirror;
	int flicker_hz;
	double min_fps;
	double dark_fps;
	int black;
	double saturation;
	double sharpness;
	double denoise;
	int threads;
	int init_w, init_h;
} opt = {
	.sensor = NULL, .out = "/dev/video42", .name = "camera", .debug_dir = NULL, .lock = "/data/local/tmp/ipu3-webcam.lock",
	.rotate = -1, .mirror = 0, .flicker_hz = 50, .min_fps = 20.0, .dark_fps = 10.0, .black = 64, .saturation = 1.25, .sharpness = 1.0, .denoise = 1.0,
	.threads = 0, .init_w = 1280, .init_h = 720,
};

static void unpack_row(const uint8_t *src, uint16_t *dst, int width) {
	int x = 0;
	while (x + 25 <= width) {
		const uint8_t *b = src;
		for (int g = 0; g < 6; g++, b += 5) {
			dst[x++] = b[0] | (b[1] & 0x03) << 8;
			dst[x++] = b[1] >> 2 | (b[2] & 0x0f) << 6;
			dst[x++] = b[2] >> 4 | (b[3] & 0x3f) << 4;
			dst[x++] = b[3] >> 6 | b[4] << 2;
		}
		dst[x++] = b[0] | (b[1] & 0x03) << 8;
		src += 32;
	}
	for (int i = 0; x < width; i++, x++) {
		int bit = i * 10;
		dst[x] = ((src[bit >> 3] | src[(bit >> 3) + 1] << 8) >> (bit & 7)) & 0x3ff;
	}
}

static void pack_row(const uint16_t *src, uint8_t *dst, int width, int bpl) {
	if (bpl <= 0)
		return;
	memset(dst, 0, (size_t)bpl);
	for (int x = 0; x < width; x++) {
		int blk = x / 25, i = x % 25, bit = i * 10;
		uint8_t *b = dst + blk * 32;
		unsigned v = (src[x] & 0x3ff) << (bit & 7);
		b[bit >> 3] |= v & 0xff;
		b[(bit >> 3) + 1] |= (v >> 8) & 0xff;
	}
}

static int ipu3_bpl(int width) { return (width + 49) / 50 * 64; }

struct pipeline {

	int w, h;
	uint16_t *raw;
	int cfa[4];

	int ow, oh;
	int *map_x, *map_y;
	int crop_x, crop_y, crop_w, crop_h;

	uint8_t lut[3][RAW_MAX + 1];
	int sat_q8;
	int sharp_q8;
	uint8_t *yplane;
	uint8_t *prev;
	int prev_ok, tnr_q8, tnr_thr;
};

static void build_lut(struct pipeline *p, int black, const double wb[3], double gain) {
	double range = RAW_MAX - black;
	for (int c = 0; c < 3; c++) {
		for (int v = 0; v <= RAW_MAX; v++) {
			double k = gain * wb[c] * 1.18, lw = k < 1.0 ? 1.0 : k;
			double lin = (v > black ? v - black : 0) / range * k;
			lin = lin * (1.0 + lin / (lw * lw)) / (1.0 + lin);
			if (lin > 1.0) lin = 1.0;
			double s = lin <= 0.0031308 ? 12.92 * lin : 1.055 * pow(lin, 1 / 2.4) - 0.055;
			s = s + 0.24 * s * (1.0 - s) * (2.0 * s - 1.0);
			s = s < 0 ? 0 : s > 1 ? 1 : s;
			int o = (int)(s * 255.0 + 0.5);
			p->lut[c][v] = o < 0 ? 0 : o > 255 ? 255 : o;
		}
	}
}

static int setup_maps(struct pipeline *p, int ow, int oh, int rot, int mirror) {
	free(p->map_x); free(p->map_y); free(p->yplane); free(p->prev);
	p->yplane = malloc((size_t)ow * oh);
	p->prev = malloc((size_t)ow * oh * 2);
	p->prev_ok = 0;
	p->map_x = malloc(sizeof(int) * ow * ((rot == 90 || rot == 270) ? oh : 1));
	p->map_y = malloc(sizeof(int) * oh * ((rot == 90 || rot == 270) ? ow : 1));
	if (!p->map_x || !p->map_y) return -1;
	p->ow = ow; p->oh = oh;

	int tw = (rot == 90 || rot == 270) ? oh : ow;
	int th = (rot == 90 || rot == 270) ? ow : oh;
	double ta = (double)tw / th, sa = (double)p->w / p->h;
	int cw, ch;
	if (sa > ta) { ch = p->h; cw = (int)(p->h * ta + 0.5); }
	else { cw = p->w; ch = (int)(p->w / ta + 0.5); }
	if (cw > p->w) cw = p->w;
	if (ch > p->h) ch = p->h;
	p->crop_w = cw; p->crop_h = ch;
	p->crop_x = (p->w - cw) / 2; p->crop_y = (p->h - ch) / 2;
	if (rot == 0 || rot == 180) {
		for (int x = 0; x < ow; x++) {
			int ux = (rot == 180) != (mirror != 0) ? ow - 1 - x : x;
			int sx = p->crop_x + (int)(((2LL * ux + 1) * cw) / (2LL * ow));
			p->map_x[x] = sx < 1 ? 1 : sx > p->w - 2 ? p->w - 2 : sx;
		}
		for (int y = 0; y < oh; y++) {
			int uy = rot == 180 ? oh - 1 - y : y;
			int sy = p->crop_y + (int)(((2LL * uy + 1) * ch) / (2LL * oh));
			p->map_y[y] = sy < 1 ? 1 : sy > p->h - 2 ? p->h - 2 : sy;
		}
	} else {

		for (int x = 0; x < ow; x++) {
			int ux = mirror ? ow - 1 - x : x;
			int sy_i = rot == 90 ? ow - 1 - ux : ux;
			int sy = p->crop_y + (int)(((2LL * sy_i + 1) * ch) / (2LL * ow));
			p->map_x[x] = sy < 1 ? 1 : sy > p->h - 2 ? p->h - 2 : sy;
		}
		for (int y = 0; y < oh; y++) {
			int sx_i = rot == 90 ? y : oh - 1 - y;
			int sx = p->crop_x + (int)(((2LL * sx_i + 1) * cw) / (2LL * oh));
			p->map_y[y] = sx < 1 ? 1 : sx > p->w - 2 ? p->w - 2 : sx;
		}
	}
	return 0;
}

static void build_sel(const int cfa[4], int sel[4][3]) {
	for (int ph = 0; ph < 4; ph++) {
		int col = cfa[ph];
		if (col == CH_R) { sel[ph][0] = 0; sel[ph][1] = 1; sel[ph][2] = 2; }
		else if (col == CH_B) { sel[ph][0] = 2; sel[ph][1] = 1; sel[ph][2] = 0; }
		else if (cfa[(ph & 2) | ((ph & 1) ^ 1)] == CH_R) { sel[ph][0] = 3; sel[ph][1] = 0; sel[ph][2] = 4; }
		else { sel[ph][0] = 4; sel[ph][1] = 0; sel[ph][2] = 3; }
	}
}

static inline void demosaic_at(const struct pipeline *p, const int sel[4][3], int x, int y, int *r, int *g, int *b) {
	const int w = p->w;
	const uint16_t *c = p->raw + y * w + x;
	int l = c[-1], rr = c[1], u = c[-w], d = c[w];
	int v[5];
	v[0] = c[0];
	v[1] = (l + rr + u + d + 2) >> 2;
	v[2] = (c[-w - 1] + c[-w + 1] + c[w - 1] + c[w + 1] + 2) >> 2;
	v[3] = (l + rr + 1) >> 1;
	v[4] = (u + d + 1) >> 1;
	const int *s = sel[((y & 1) << 1) | (x & 1)];
	*r = v[s[0]];
	*g = v[s[1]];
	*b = v[s[2]];
}

struct rowjob { struct pipeline *p; uint8_t *out; int y0, y1, rot; };

static void *process_rows(void *arg) {
	struct rowjob *j = arg;
	struct pipeline *p = j->p;
	const int ow = p->ow, sat = p->sat_q8;
	const uint8_t *lr = p->lut[CH_R], *lg = p->lut[CH_G], *lb = p->lut[CH_B];
	const int swap = j->rot == 90 || j->rot == 270;
	int sel[4][3];
	build_sel(p->cfa, sel);
	for (int y = j->y0; y < j->y1; y++) {
		uint8_t *o = j->out + (size_t)y * ow * 2;
		uint8_t *yp = p->yplane ? p->yplane + (size_t)y * ow : NULL;
		for (int x = 0; x < ow; x += 2) {
			int R[2], G[2], B[2];
			for (int k = 0; k < 2; k++) {
				int sx, sy, r, g, b;
				if (!swap) { sx = p->map_x[x + k]; sy = p->map_y[y]; }
				else { sy = p->map_x[x + k]; sx = p->map_y[y]; }
				demosaic_at(p, sel, sx, sy, &r, &g, &b);
				R[k] = lr[r];
				G[k] = lg[g];
				B[k] = lb[b];
			}
			int y0 = (77 * R[0] + 150 * G[0] + 29 * B[0] + 128) >> 8;
			int y1 = (77 * R[1] + 150 * G[1] + 29 * B[1] + 128) >> 8;
			int rs = R[0] + R[1], gs = G[0] + G[1], bs = B[0] + B[1];
			int u = (-43 * rs - 85 * gs + 128 * bs) >> 9;
			int v = (128 * rs - 107 * gs - 21 * bs) >> 9;
			int ya = (y0 + y1) >> 1;
			int s = ya >= 48 ? sat : (sat * ya) / 48;
			u = 128 + ((u * s) >> 8);
			v = 128 + ((v * s) >> 8);
			y0 = y0 > 255 ? 255 : y0;
			y1 = y1 > 255 ? 255 : y1;
			o[0] = y0;
			o[1] = u < 0 ? 0 : u > 255 ? 255 : u;
			o[2] = y1;
			o[3] = v < 0 ? 0 : v > 255 ? 255 : v;
			if (yp) { yp[x] = y0; yp[x + 1] = y1; }
			o += 4;
		}
	}
	return NULL;
}

static void sharpen_rows(struct pipeline *p, uint8_t *out, int y0, int y1) {
	const int ow = p->ow, oh = p->oh, k = p->sharp_q8;
	if (y0 < 1) y0 = 1;
	if (y1 > oh - 1) y1 = oh - 1;
	for (int y = y0; y < y1; y++) {
		const uint8_t *c = p->yplane + (size_t)y * ow, *up = c - ow, *dn = c + ow;
		uint8_t *o = out + (size_t)y * ow * 2;
		for (int x = 1; x < ow - 1; x++) {
			int lap = 4 * c[x] - c[x - 1] - c[x + 1] - up[x] - dn[x];
			int v = c[x] + ((lap * k) >> 10);
			o[x * 2] = v < 0 ? 0 : v > 255 ? 255 : v;
		}
	}
}

struct unpackjob { struct pipeline *p; const uint8_t *src; int bpl, y0, y1; };

static void *unpack_rows(void *arg) {
	struct unpackjob *j = arg;
	for (int y = j->y0; y < j->y1; y++)
		unpack_row(j->src + (size_t)y * j->bpl, j->p->raw + (size_t)y * j->p->w, j->p->w);
	return NULL;
}

static int g_nthreads = 1;

static struct {
	pthread_t th[MAX_THREADS];
	int size, gen, pending, start_gen[MAX_THREADS];
	pthread_mutex_t m;
	pthread_cond_t go, done;
	void (*fn)(void *, int, int);
	void *arg;
} g_pool = { .m = PTHREAD_MUTEX_INITIALIZER, .go = PTHREAD_COND_INITIALIZER, .done = PTHREAD_COND_INITIALIZER };

static void *pool_worker(void *a) {
	int id = (int)(intptr_t)a, seen = g_pool.start_gen[id];
	for (;;) {
		pthread_mutex_lock(&g_pool.m);
		while (g_pool.gen == seen) pthread_cond_wait(&g_pool.go, &g_pool.m);
		seen = g_pool.gen;
		void (*fn)(void *, int, int) = g_pool.fn;
		void *arg = g_pool.arg;
		int n = g_nthreads;
		pthread_mutex_unlock(&g_pool.m);
		if (id < n) fn(arg, id, n);
		pthread_mutex_lock(&g_pool.m);
		if (--g_pool.pending == 0) pthread_cond_signal(&g_pool.done);
		pthread_mutex_unlock(&g_pool.m);
	}
	return NULL;
}

static void pool_grow(int n) {
	if (n > MAX_THREADS) n = MAX_THREADS;
	if (g_pool.size == 0) g_pool.size = 1;
	while (g_pool.size < n) {
		g_pool.start_gen[g_pool.size] = g_pool.gen;
		if (pthread_create(&g_pool.th[g_pool.size], NULL, pool_worker, (void *)(intptr_t)g_pool.size)) break;
		g_pool.size++;
	}
	if (g_nthreads > g_pool.size) g_nthreads = g_pool.size;
}

static void pool_run(void (*fn)(void *, int, int), void *arg) {
	if (g_pool.size <= 1 || g_nthreads <= 1) { fn(arg, 0, 1); return; }
	pthread_mutex_lock(&g_pool.m);
	g_pool.fn = fn; g_pool.arg = arg;
	g_pool.pending = g_pool.size - 1;
	g_pool.gen++;
	pthread_cond_broadcast(&g_pool.go);
	pthread_mutex_unlock(&g_pool.m);
	fn(arg, 0, g_nthreads);
	pthread_mutex_lock(&g_pool.m);
	while (g_pool.pending) pthread_cond_wait(&g_pool.done, &g_pool.m);
	pthread_mutex_unlock(&g_pool.m);
}

static void unpack_part(void *a, int id, int n) {
	struct unpackjob *j = a;
	struct unpackjob part = *j;
	int rows = j->y1 - j->y0;
	part.y0 = j->y0 + rows * id / n;
	part.y1 = j->y0 + rows * (id + 1) / n;
	unpack_rows(&part);
}

static void process_part(void *a, int id, int n) {
	struct rowjob *j = a;
	struct rowjob part = *j;
	part.y0 = j->p->oh * id / n;
	part.y1 = j->p->oh * (id + 1) / n;
	process_rows(&part);
}

static void run_parallel_unpack(struct pipeline *p, const uint8_t *src, int bpl, int y0, int y1) {
	struct unpackjob j = { p, src, bpl, y0, y1 };
	pool_grow(g_nthreads);
	pool_run(unpack_part, &j);
}

static void tnr_rows(struct pipeline *p, uint8_t *out, int y0, int y1) {
	const size_t row = (size_t)p->ow * 2;
	const int a = p->tnr_q8, thr = p->tnr_thr;
	for (int y = y0; y < y1; y++) {
		uint8_t *o = out + row * y, *pv = p->prev + row * y;
		if (p->prev_ok && a > 0) {
			for (size_t i = 0; i < row; i++) {
				int d = pv[i] - o[i];
				int ad = d < 0 ? -d : d;
				int w = ad >= thr ? 0 : a * (thr - ad) / thr;
				o[i] = (uint8_t)(o[i] + ((d * w) >> 8));
			}
		}
		memcpy(pv, o, row);
	}
}

static void tnr_part(void *a, int id, int n) {
	struct rowjob *j = a;
	tnr_rows(j->p, j->out, j->p->oh * id / n, j->p->oh * (id + 1) / n);
}

static void sharpen_part(void *a, int id, int n) {
	struct rowjob *j = a;
	sharpen_rows(j->p, j->out, j->p->oh * id / n, j->p->oh * (id + 1) / n);
}

static void run_parallel_process(struct pipeline *p, uint8_t *out, int rot) {
	struct rowjob j = { p, out, 0, p->oh, rot };
	pool_grow(g_nthreads);
	pool_run(process_part, &j);
	if (p->sharp_q8 > 0 && p->yplane) pool_run(sharpen_part, &j);
	if (p->prev) {
		pool_run(tnr_part, &j);
		p->prev_ok = 1;
	}
}

static void needed_rows(const struct pipeline *p, int rot, int *y0, int *y1) {
	(void)rot;
	*y0 = p->crop_y - 2 < 0 ? 0 : p->crop_y - 2;
	*y1 = p->crop_y + p->crop_h + 2 > p->h ? p->h : p->crop_y + p->crop_h + 2;
}

struct stats {
	double mean[3];
	double sat_frac;
	double sharp;
	double raw_sd;
};

static void compute_stats(const struct pipeline *p, int black, struct stats *st) {
	double rsum = 0, rsq = 0;
	long rn = 0;
	double sum[3] = { 0 }, wsum = 0, sharp = 0, gsum = 0;
	long satn = 0, n = 0;
	int x0 = p->crop_x + p->crop_w / 8, x1 = p->crop_x + p->crop_w * 7 / 8;
	int y0 = p->crop_y + p->crop_h / 8, y1 = p->crop_y + p->crop_h * 7 / 8;
	int step = p->crop_w / 96; if (step < 2) step = 2; step &= ~1;
	double cx = p->crop_x + p->crop_w / 2.0, cy = p->crop_y + p->crop_h / 2.0;
	double rx = p->crop_w / 2.0, ry = p->crop_h / 2.0;
	for (int y = y0 & ~1; y < y1 - 1; y += step) {
		for (int x = x0 & ~1; x < x1 - 3; x += step) {
			int v[3] = { 0 }, cnt[3] = { 0 }, hi = 0;
			for (int k = 0; k < 4; k++) {
				int xx = x + (k & 1), yy = y + (k >> 1);
				int raw = p->raw[yy * p->w + xx];
				int c = p->cfa[(yy & 1) * 2 + (xx & 1)];
				if (raw >= RAW_MAX - 24) hi = 1;
				rsum += raw; rsq += (double)raw * raw; rn++;
				v[c] += raw > black ? raw - black : 0;
				cnt[c]++;
			}
			double dx = (x - cx) / rx, dy = (y - cy) / ry;
			double wgt = 1.0 + 2.0 * fmax(0.0, 1.0 - (dx * dx + dy * dy));
			for (int c = 0; c < 3; c++) if (cnt[c]) sum[c] += wgt * v[c] / cnt[c];
			wsum += wgt;
			satn += hi; n++;

			if (fabs(dx) < 0.5 && fabs(dy) < 0.5) {
				int gx = p->cfa[(y & 1) * 2 + (x & 1)] == CH_G ? x : x + 1;
				int a = p->raw[y * p->w + gx], b = p->raw[y * p->w + gx + 2], d = p->raw[(y + 2) * p->w + gx];
				sharp += abs(a - b) + abs(a - d);
				gsum += a;
			}
		}
	}
	for (int c = 0; c < 3; c++) st->mean[c] = wsum > 0 ? sum[c] / wsum : 0;
	st->sat_frac = n ? (double)satn / n : 0;
	st->sharp = gsum > 0 ? sharp / gsum : 0;
	double rm = rn ? rsum / rn : 0;
	st->raw_sd = rn ? sqrt(fmax(0.0, rsq / rn - rm * rm)) : 0;
}

static void cfa_from_code(uint32_t code, int cfa[4]) {
	static const int rggb[4] = { CH_R, CH_G, CH_G, CH_B }, grbg[4] = { CH_G, CH_R, CH_B, CH_G };
	static const int gbrg[4] = { CH_G, CH_B, CH_R, CH_G }, bggr[4] = { CH_B, CH_G, CH_G, CH_R };
	const int *s = bggr;
	switch (code) {
	case MEDIA_BUS_FMT_SRGGB10_1X10: s = rggb; break;
	case MEDIA_BUS_FMT_SGRBG10_1X10: s = grbg; break;
	case MEDIA_BUS_FMT_SGBRG10_1X10: s = gbrg; break;
	case MEDIA_BUS_FMT_SBGGR10_1X10: s = bggr; break;
	}
	memcpy(cfa, s, sizeof(int) * 4);
}

static uint32_t pixfmt_from_code(uint32_t code) {
	switch (code) {
	case MEDIA_BUS_FMT_SRGGB10_1X10: return V4L2_PIX_FMT_IPU3_SRGGB10;
	case MEDIA_BUS_FMT_SGRBG10_1X10: return V4L2_PIX_FMT_IPU3_SGRBG10;
	case MEDIA_BUS_FMT_SGBRG10_1X10: return V4L2_PIX_FMT_IPU3_SGBRG10;
	case MEDIA_BUS_FMT_SBGGR10_1X10: return V4L2_PIX_FMT_IPU3_SBGGR10;
	}
	return 0;
}

struct mode { int w, h; };

struct camera {
	char media_path[32];
	int media_fd;
	uint32_t sensor_ent, csi2_ent, cio2_ent, lens_ent;
	uint16_t sensor_pad, csi2_sink_pad;
	char sensor_name[64], lens_name[64];
	char sensor_dev[64], csi2_dev[64], cio2_dev[64], lens_dev[64];
	int sensor_fd, csi2_fd, cap_fd, lens_fd;
	uint32_t code;
	struct mode modes[MAX_MODES];
	int nmodes;
	int orientation, rotation;

	int mode_w, mode_h, bpl;
	void *buf[NBUF];
	size_t buflen[NBUF];
	int nbuf, streaming;
};

static int devnode_for(uint32_t major, uint32_t minor, char *out, size_t outlen) {
	char path[300], line[128];
	snprintf(path, sizeof(path), "/sys/dev/char/%u:%u/uevent", major, minor);
	FILE *f = fopen(path, "r");
	if (f) {
		while (fgets(line, sizeof(line), f)) {
			if (!strncmp(line, "DEVNAME=", 8)) {
				line[strcspn(line, "\n")] = 0;
				snprintf(out, outlen, "/dev/%.40s", line + 8);
				fclose(f);
				return 0;
			}
		}
		fclose(f);
	}

	DIR *d = opendir("/dev");
	if (!d) return -1;
	struct dirent *e;
	while ((e = readdir(d))) {
		if (strncmp(e->d_name, "video", 5) && strncmp(e->d_name, "v4l-subdev", 10)) continue;
		struct stat st;
		snprintf(path, sizeof(path), "/dev/%.40s", e->d_name);
		if (!stat(path, &st) && S_ISCHR(st.st_mode) && major(st.st_rdev) == major && minor(st.st_rdev) == minor) {
			snprintf(out, outlen, "%.60s", path);
			closedir(d);
			return 0;
		}
	}
	closedir(d);
	return -1;
}

static int enum_links(int fd, uint32_t ent, int npads, int nlinks, struct media_link_desc **links) {
	struct media_links_enum le;
	memset(&le, 0, sizeof(le));
	le.entity = ent;
	le.pads = calloc(npads ? npads : 1, sizeof(struct media_pad_desc));
	*links = calloc(nlinks ? nlinks : 1, sizeof(struct media_link_desc));
	le.links = *links;
	int r = xioctl(fd, MEDIA_IOC_ENUM_LINKS, &le);
	free(le.pads);
	return r;
}

static int ctrl_get(int fd, uint32_t id, int *val) {
	struct v4l2_control c = { .id = id };
	if (xioctl(fd, VIDIOC_G_CTRL, &c) < 0) return -1;
	*val = c.value;
	return 0;
}

static int ctrl_set(int fd, uint32_t id, int val) {
	struct v4l2_control c = { .id = id, .value = val };
	return xioctl(fd, VIDIOC_S_CTRL, &c);
}

static void list_modes(struct camera *c) {
	c->nmodes = 0;
	for (int i = 0; c->nmodes < MAX_MODES; i++) {
		struct v4l2_subdev_frame_size_enum fse;
		memset(&fse, 0, sizeof(fse));
		fse.index = i; fse.pad = c->sensor_pad; fse.code = c->code; fse.which = V4L2_SUBDEV_FORMAT_ACTIVE;
		if (xioctl(c->sensor_fd, VIDIOC_SUBDEV_ENUM_FRAME_SIZE, &fse) < 0) break;
		c->modes[c->nmodes++] = (struct mode){ (int)fse.max_width, (int)fse.max_height };
	}
}

static int find_camera(struct camera *c, const char *want, int nth, int verbose) {
	int seen = 0;
	for (int m = 0; m < 16; m++) {
		char mp[32];
		snprintf(mp, sizeof(mp), "/dev/media%d", m);
		int fd = open(mp, O_RDWR);
		if (fd < 0) continue;
		struct media_device_info di;
		memset(&di, 0, sizeof(di));
		if (xioctl(fd, MEDIA_IOC_DEVICE_INFO, &di) < 0 || strcmp(di.driver, "ipu3-cio2")) { close(fd); continue; }
		struct media_entity_desc ents[64];
		int nent = 0;
		struct media_entity_desc e;
		memset(&e, 0, sizeof(e));
		e.id = MEDIA_ENT_ID_FLAG_NEXT;
		while (nent < 64 && xioctl(fd, MEDIA_IOC_ENUM_ENTITIES, &e) == 0) {
			ents[nent++] = e;
			e.id |= MEDIA_ENT_ID_FLAG_NEXT;
		}
		for (int i = 0; i < nent; i++) {
			if (ents[i].type != MEDIA_ENT_F_CAM_SENSOR) continue;
			if (want && !strcasestr(ents[i].name, want)) continue;
			if (!want && seen++ != nth) continue;
			memset(c, 0, sizeof(*c));
			c->sensor_fd = c->csi2_fd = c->cap_fd = c->lens_fd = -1;
			snprintf(c->media_path, sizeof(c->media_path), "%s", mp);
			c->media_fd = fd;
			c->sensor_ent = ents[i].id;
			snprintf(c->sensor_name, sizeof(c->sensor_name), "%s", ents[i].name);
			devnode_for(ents[i].dev.major, ents[i].dev.minor, c->sensor_dev, sizeof(c->sensor_dev));

			struct media_link_desc *lk;
			if (enum_links(fd, ents[i].id, ents[i].pads, ents[i].links, &lk) == 0) {
				for (int l = 0; l < ents[i].links; l++) {
					if (lk[l].source.entity != ents[i].id) continue;
					for (int k = 0; k < nent; k++) {
						if (ents[k].id == lk[l].sink.entity && strstr(ents[k].name, "ipu3-csi2")) {
							c->csi2_ent = ents[k].id;
							c->sensor_pad = lk[l].source.index;
							c->csi2_sink_pad = lk[l].sink.index;
							devnode_for(ents[k].dev.major, ents[k].dev.minor, c->csi2_dev, sizeof(c->csi2_dev));

							struct media_link_desc *lk2;
							if (enum_links(fd, ents[k].id, ents[k].pads, ents[k].links, &lk2) == 0) {
								for (int l2 = 0; l2 < ents[k].links; l2++) {
									if (lk2[l2].source.entity != ents[k].id) continue;
									for (int q = 0; q < nent; q++)
										if (ents[q].id == lk2[l2].sink.entity && strstr(ents[q].name, "ipu3-cio2")) {
											c->cio2_ent = ents[q].id;
											devnode_for(ents[q].dev.major, ents[q].dev.minor, c->cio2_dev, sizeof(c->cio2_dev));
										}
								}
							}
							free(lk2);
						}
					}
				}
			}
			free(lk);

			const char *sp = strchr(c->sensor_name, ' ');
			char bus[16] = "";
			if (sp) { snprintf(bus, sizeof(bus), "%s", sp + 1); char *dash = strchr(bus, '-'); if (dash) dash[1] = 0; }
			for (int k = 0; k < nent; k++) {
				if (ents[k].type != MEDIA_ENT_F_LENS) continue;
				const char *lp = strchr(ents[k].name, ' ');
				if (bus[0] && lp && !strncmp(lp + 1, bus, strlen(bus))) {
					c->lens_ent = ents[k].id;
					snprintf(c->lens_name, sizeof(c->lens_name), "%s", ents[k].name);
					devnode_for(ents[k].dev.major, ents[k].dev.minor, c->lens_dev, sizeof(c->lens_dev));
				}
			}
			if (!c->csi2_ent || !c->cio2_ent || !c->sensor_dev[0] || !c->csi2_dev[0] || !c->cio2_dev[0]) {
				if (verbose) LOG("%s: incomplete graph in %s (csi2=%u cio2=%u nodes '%s' '%s' '%s')", c->sensor_name, mp,
						 c->csi2_ent, c->cio2_ent, c->sensor_dev, c->csi2_dev, c->cio2_dev);
				continue;
			}
			c->sensor_fd = open(c->sensor_dev, O_RDWR);
			if (c->sensor_fd < 0) { if (verbose) LOG("open %s: %s", c->sensor_dev, strerror(errno)); continue; }

			struct v4l2_subdev_format sf;
			memset(&sf, 0, sizeof(sf));
			sf.which = V4L2_SUBDEV_FORMAT_ACTIVE; sf.pad = c->sensor_pad;
			if (xioctl(c->sensor_fd, VIDIOC_SUBDEV_G_FMT, &sf) == 0) c->code = sf.format.code;
			if (!pixfmt_from_code(c->code)) {
				struct v4l2_subdev_mbus_code_enum ce;
				for (int k = 0; ; k++) {
					memset(&ce, 0, sizeof(ce));
					ce.index = k; ce.pad = c->sensor_pad; ce.which = V4L2_SUBDEV_FORMAT_ACTIVE;
					if (xioctl(c->sensor_fd, VIDIOC_SUBDEV_ENUM_MBUS_CODE, &ce) < 0) break;
					if (pixfmt_from_code(ce.code)) { c->code = ce.code; break; }
				}
			}
			if (!pixfmt_from_code(c->code)) {
				if (verbose) LOG("%s: no 10-bit Bayer bus format (code 0x%x)", c->sensor_name, c->code);
				close(c->sensor_fd);
				continue;
			}
			list_modes(c);
			c->orientation = -1; c->rotation = 0;
			ctrl_get(c->sensor_fd, V4L2_CID_CAMERA_ORIENTATION, &c->orientation);
			ctrl_get(c->sensor_fd, V4L2_CID_CAMERA_SENSOR_ROTATION, &c->rotation);
			return 0;
		}
		close(fd);
	}
	return -1;
}

static void close_camera(struct camera *c) {
	if (c->lens_fd >= 0) close(c->lens_fd);
	if (c->cap_fd >= 0) close(c->cap_fd);
	if (c->csi2_fd >= 0) close(c->csi2_fd);
	if (c->sensor_fd >= 0) close(c->sensor_fd);
	if (c->media_fd >= 0) close(c->media_fd);
}

static int setup_link(struct camera *c) {
	struct media_link_desc l;
	memset(&l, 0, sizeof(l));
	l.source.entity = c->sensor_ent; l.source.index = c->sensor_pad; l.source.flags = MEDIA_PAD_FL_SOURCE;
	l.sink.entity = c->csi2_ent; l.sink.index = c->csi2_sink_pad; l.sink.flags = MEDIA_PAD_FL_SINK;
	l.flags = MEDIA_LNK_FL_ENABLED;
	if (xioctl(c->media_fd, MEDIA_IOC_SETUP_LINK, &l) < 0) {
		LOG("enable link %s -> csi2: %s", c->sensor_name, strerror(errno));
		return -1;
	}
	return 0;
}

static int mode_serves(struct mode m, int tw, int th, int rot) {
	if (rot == 90 || rot == 270) { int t = tw; tw = th; th = t; }
	double ta = (double)tw / th, sa = (double)m.w / m.h;
	int cw = sa > ta ? (int)(m.h * ta) : m.w;
	return m.w > 0 && cw * 10 >= tw * 8 && fabs(log(sa / ta)) < 0.05;
}

static struct mode pick_mode(const struct camera *c, int tw, int th, int rot) {
	if (rot == 90 || rot == 270) { int t = tw; tw = th; th = t; }
	if (tw < 1280) { th = (int)((long)th * 1280 / tw); tw = 1280; }
	double ta = (double)tw / th;
	int best = -1, largest = 0;
	long best_area = 0;
	double best_ad = 0;
	for (int i = 0; i < c->nmodes; i++) {
		const struct mode *m = &c->modes[i];
		if ((long)m->w * m->h > (long)c->modes[largest].w * c->modes[largest].h) largest = i;
		double sa = (double)m->w / m->h;
		int cw = sa > ta ? (int)(m->h * ta) : m->w;
		if (cw * 10 < tw * 8) continue;
		long area = (long)m->w * m->h;
		double ad = fabs(log(sa / ta));
		ad = ad < 0.05 ? 0 : ad;
		if (best < 0 || ad < best_ad - 1e-9 || (fabs(ad - best_ad) < 1e-9 && area < best_area)) { best = i; best_area = area; best_ad = ad; }
	}
	if (c->nmodes == 0) return (struct mode){ tw, th };
	return c->modes[best >= 0 ? best : largest];
}

static void free_buffers(struct camera *c) {
	for (int i = 0; i < c->nbuf; i++) if (c->buf[i] && c->buf[i] != MAP_FAILED) munmap(c->buf[i], c->buflen[i]);
	c->nbuf = 0;
	if (c->cap_fd >= 0) {
		struct v4l2_requestbuffers rb = { .count = 0, .type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE, .memory = V4L2_MEMORY_MMAP };
		xioctl(c->cap_fd, VIDIOC_REQBUFS, &rb);
	}
}

static int set_pad_fmt(int fd, int pad, int w, int h, uint32_t code, struct v4l2_mbus_framefmt *got) {
	struct v4l2_subdev_format sf;
	memset(&sf, 0, sizeof(sf));
	sf.which = V4L2_SUBDEV_FORMAT_ACTIVE; sf.pad = pad;
	sf.format.width = w; sf.format.height = h; sf.format.code = code; sf.format.field = V4L2_FIELD_NONE;
	if (xioctl(fd, VIDIOC_SUBDEV_S_FMT, &sf) < 0) return -1;
	if (got) *got = sf.format;
	return 0;
}

static int configure(struct camera *c, struct mode md) {
	free_buffers(c);
	if (c->cap_fd < 0 && (c->cap_fd = open(c->cio2_dev, O_RDWR | O_NONBLOCK)) < 0) { LOG("open %s: %s", c->cio2_dev, strerror(errno)); return -1; }
	if (c->csi2_fd < 0 && (c->csi2_fd = open(c->csi2_dev, O_RDWR)) < 0) { LOG("open %s: %s", c->csi2_dev, strerror(errno)); return -1; }
	if (setup_link(c) < 0) return -1;
	struct v4l2_mbus_framefmt f;
	if (set_pad_fmt(c->sensor_fd, c->sensor_pad, md.w, md.h, c->code, &f) < 0) { LOG("sensor S_FMT %dx%d: %s", md.w, md.h, strerror(errno)); return -1; }
	c->code = f.code;
	if (!pixfmt_from_code(c->code)) { LOG("sensor switched to bus code 0x%x, not 10-bit Bayer", c->code); return -1; }
	c->mode_w = f.width; c->mode_h = f.height;
	set_pad_fmt(c->csi2_fd, c->csi2_sink_pad, f.width, f.height, f.code, NULL);
	set_pad_fmt(c->csi2_fd, 1, f.width, f.height, f.code, NULL);
	struct v4l2_format vf;
	memset(&vf, 0, sizeof(vf));
	vf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	vf.fmt.pix_mp.width = f.width; vf.fmt.pix_mp.height = f.height;
	vf.fmt.pix_mp.pixelformat = pixfmt_from_code(f.code);
	vf.fmt.pix_mp.field = V4L2_FIELD_NONE;
	vf.fmt.pix_mp.num_planes = 1;
	if (xioctl(c->cap_fd, VIDIOC_S_FMT, &vf) < 0) { LOG("cio2 S_FMT: %s", strerror(errno)); return -1; }
	if ((int)vf.fmt.pix_mp.width != c->mode_w || (int)vf.fmt.pix_mp.height != c->mode_h) {
		LOG("cio2 gave %ux%u for sensor %dx%d", vf.fmt.pix_mp.width, vf.fmt.pix_mp.height, c->mode_w, c->mode_h);
		return -1;
	}
	c->bpl = vf.fmt.pix_mp.plane_fmt[0].bytesperline;
	if (c->bpl < ipu3_bpl(c->mode_w)) c->bpl = ipu3_bpl(c->mode_w);
	struct v4l2_requestbuffers rb = { .count = NBUF, .type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE, .memory = V4L2_MEMORY_MMAP };
	if (xioctl(c->cap_fd, VIDIOC_REQBUFS, &rb) < 0 || rb.count < 2) { LOG("REQBUFS: %s", strerror(errno)); return -1; }
	c->nbuf = rb.count > NBUF ? NBUF : rb.count;
	for (int i = 0; i < c->nbuf; i++) {
		struct v4l2_plane pl[VIDEO_MAX_PLANES];
		struct v4l2_buffer b;
		memset(&b, 0, sizeof(b)); memset(pl, 0, sizeof(pl));
		b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE; b.memory = V4L2_MEMORY_MMAP; b.index = i;
		b.m.planes = pl; b.length = VIDEO_MAX_PLANES;
		if (xioctl(c->cap_fd, VIDIOC_QUERYBUF, &b) < 0) { LOG("QUERYBUF: %s", strerror(errno)); return -1; }
		c->buflen[i] = pl[0].length;
		c->buf[i] = mmap(NULL, pl[0].length, PROT_READ | PROT_WRITE, MAP_SHARED, c->cap_fd, pl[0].m.mem_offset);
		if (c->buf[i] == MAP_FAILED) { LOG("mmap: %s", strerror(errno)); c->nbuf = i; return -1; }
	}
	LOG("%s: mode %dx%d, bus 0x%x, %d buffers, %d bytes/line", c->sensor_name, c->mode_w, c->mode_h, c->code, c->nbuf, c->bpl);
	return 0;
}

static int queue_buf(struct camera *c, int i) {
	struct v4l2_plane pl[VIDEO_MAX_PLANES];
	struct v4l2_buffer b;
	memset(&b, 0, sizeof(b)); memset(pl, 0, sizeof(pl));
	b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE; b.memory = V4L2_MEMORY_MMAP; b.index = i;
	b.m.planes = pl; b.length = 1;
	return xioctl(c->cap_fd, VIDIOC_QBUF, &b);
}

static int stream_on(struct camera *c) {
	for (int i = 0; i < c->nbuf; i++) if (queue_buf(c, i) < 0) { LOG("QBUF: %s", strerror(errno)); return -1; }
	enum v4l2_buf_type t = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	if (xioctl(c->cap_fd, VIDIOC_STREAMON, &t) < 0) { LOG("STREAMON: %s", strerror(errno)); return -1; }
	c->streaming = 1;
	return 0;
}

static void stream_off(struct camera *c) {
	if (!c->streaming) return;
	enum v4l2_buf_type t = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	xioctl(c->cap_fd, VIDIOC_STREAMOFF, &t);
	c->streaming = 0;
}

struct ctl { uint32_t id; int ok, min, max, def, cur; };

static void ctl_query(int fd, struct ctl *k) {
	struct v4l2_queryctrl q;
	memset(&q, 0, sizeof(q));
	q.id = k->id;
	k->ok = xioctl(fd, VIDIOC_QUERYCTRL, &q) == 0 && !(q.flags & V4L2_CTRL_FLAG_DISABLED);
	if (!k->ok) return;
	k->min = q.minimum; k->max = q.maximum; k->def = q.default_value;
	if (ctrl_get(fd, k->id, &k->cur) < 0) k->cur = k->def;
}

static int64_t ctrl_get64(int fd, uint32_t id) {
	struct v4l2_ext_control ec;
	struct v4l2_ext_controls ecs;
	memset(&ec, 0, sizeof(ec)); memset(&ecs, 0, sizeof(ecs));
	ec.id = id; ecs.count = 1; ecs.controls = &ec; ecs.which = V4L2_CTRL_WHICH_CUR_VAL;
	if (xioctl(fd, VIDIOC_G_EXT_CTRLS, &ecs) < 0) return 0;
	return ec.value64;
}

struct ae {
	struct ctl exp, again, dgain, vblank, hblank;
	double line_us;
	int exp_margin;
	int vblank_def;
	double sw_gain;
	double wb[3];
	int frames_since_set;
	int settled;
	int inited;
};

static void ae_init(struct camera *c, struct ae *a, int fresh) {
	int fd = c->sensor_fd;
	a->exp.id = V4L2_CID_EXPOSURE; a->again.id = V4L2_CID_ANALOGUE_GAIN; a->dgain.id = V4L2_CID_DIGITAL_GAIN;
	a->vblank.id = V4L2_CID_VBLANK; a->hblank.id = V4L2_CID_HBLANK;
	ctl_query(fd, &a->vblank); ctl_query(fd, &a->hblank);

	if (fresh && a->vblank.ok && a->vblank.cur != a->vblank.def) { ctrl_set(fd, V4L2_CID_VBLANK, a->vblank.def); a->vblank.cur = a->vblank.def; }
	a->vblank_def = a->vblank.ok ? a->vblank.def : 0;
	ctl_query(fd, &a->exp); ctl_query(fd, &a->again); ctl_query(fd, &a->dgain);
	int64_t pixrate = ctrl_get64(fd, V4L2_CID_PIXEL_RATE);
	a->line_us = (pixrate > 0 && a->hblank.ok) ? (c->mode_w + a->hblank.cur) * 1e6 / (double)pixrate : 0;
	a->exp_margin = (a->exp.ok && a->vblank.ok) ? c->mode_h + a->vblank.cur - a->exp.max : 8;
	if (a->exp_margin < 0) a->exp_margin = 8;
	if (a->sw_gain <= 0) a->sw_gain = 1.0;
	if (a->wb[0] <= 0) { a->wb[0] = 1.8; a->wb[1] = 1.0; a->wb[2] = 1.6; }

	if (!a->inited && a->exp.ok) {
		int e = a->line_us > 0 ? (int)(20000 / a->line_us) : a->exp.def;
		if (e > a->exp.max) e = a->exp.max;
		if (e < a->exp.min) e = a->exp.min;
		ctrl_set(fd, V4L2_CID_EXPOSURE, e); a->exp.cur = e;
	}
	if (!a->inited && a->again.ok) {
		int g = a->again.def > 0 ? a->again.def : a->again.cur;
		if (g > a->again.max) g = a->again.max;
		ctrl_set(fd, V4L2_CID_ANALOGUE_GAIN, g); a->again.cur = g;
	}
	if (!a->inited && a->dgain.ok && a->dgain.def > 0) { ctrl_set(fd, V4L2_CID_DIGITAL_GAIN, a->dgain.def); a->dgain.cur = a->dgain.def; }
	a->frames_since_set = 0; a->settled = 0; a->inited = 1;
	LOG("AE: exposure %s %d..%d (now %d), again %s %d..%d def %d, dgain %s, vblank %s def %d max %d, line %.2f us, pixel rate %lld",
	    a->exp.ok ? "yes" : "NO", a->exp.min, a->exp.max, a->exp.cur, a->again.ok ? "yes" : "NO", a->again.min, a->again.max,
	    a->again.def, a->dgain.ok ? "yes" : "no", a->vblank.ok ? "yes" : "no", a->vblank.def, a->vblank.max, a->line_us,
	    (long long)pixrate);
}

static double gain_factor(const struct ctl *k) { return (k->ok && k->def > 0) ? (double)k->cur / k->def : 1.0; }

static void ae_step(struct camera *c, struct ae *a, const struct stats *st, int black) {
	const double target = 0.17 * (RAW_MAX - black);

	double g = (st->mean[CH_G] < 1.0 ? 1.0 : st->mean[CH_G]) * a->sw_gain;
	double ratio = target / g;
	if (st->sat_frac > 0.04 && ratio > 0.85) ratio = 0.85;
	if (st->sat_frac > 0.15 && ratio > 0.5) ratio = 0.5;
	if (st->sat_frac > 0.40 && ratio > 0.3) ratio = 0.3;
	if (ratio > 0.9 && ratio < 1.1) { a->settled++; ratio = 1.0; }
	else a->settled = 0;
	ratio = pow(ratio, 0.75);
	if (ratio > 2.5) ratio = 2.5;
	if (ratio < 0.25) ratio = 0.25;

	if (ratio != 1.0) {
		double total = (a->exp.ok ? a->exp.cur : 1) * gain_factor(&a->again) * gain_factor(&a->dgain) * a->sw_gain;
		double want = total * ratio;
		int fd = c->sensor_fd;

		if (a->vblank.ok && a->exp.ok) {
			int frame_def = c->mode_h + a->vblank_def;
			int frame_min_fps = opt.min_fps > 0 && a->line_us > 0 ? (int)(1e6 / opt.min_fps / a->line_us) : frame_def;
			if (frame_min_fps < frame_def) frame_min_fps = frame_def;
			int frame_dark = opt.dark_fps > 0 && a->line_us > 0 ? (int)(1e6 / opt.dark_fps / a->line_us) : frame_min_fps;
			if (frame_dark < frame_min_fps) frame_dark = frame_min_fps;
			int need = (int)(want / 4.0) + a->exp_margin;
			if (need > frame_min_fps) {
				int n16 = (int)(want / 16.0) + a->exp_margin;
				need = n16 > frame_min_fps ? n16 : frame_min_fps;
			}
			int frame = need < frame_def ? frame_def : need > frame_dark ? frame_dark : need;
			int vb = frame - c->mode_h;
			if (vb > a->vblank.max) vb = a->vblank.max;
			if (vb < a->vblank.min) vb = a->vblank.min;
			if (abs(vb - a->vblank.cur) * 20 > frame_def || (vb == a->vblank_def && a->vblank.cur != vb)) {
				if (ctrl_set(fd, V4L2_CID_VBLANK, vb) == 0) {
					a->vblank.cur = vb;
					ctl_query(fd, &a->exp);
				}
			}
		}

		double rest = want;
		if (a->exp.ok) {
			double e = want;
			if (e > a->exp.max) e = a->exp.max;
			if (e < a->exp.min) e = a->exp.min;
			if (opt.flicker_hz > 0 && a->line_us > 0) {
				double period_lines = 1e6 / (2.0 * opt.flicker_hz) / a->line_us;
				if (e >= period_lines) e = floor(e / period_lines) * period_lines;
			}
			int ei = (int)e;
			if (ei != a->exp.cur && ctrl_set(fd, V4L2_CID_EXPOSURE, ei) == 0) a->exp.cur = ei;
			rest = want / a->exp.cur;
		}

		if (a->again.ok && a->again.def > 0) {
			int lim = a->again.def * 16 < a->again.max ? a->again.def * 16 : a->again.max;
			int gv = (int)(a->again.def * rest + 0.5);
			if (gv > lim) gv = lim;
			if (gv < a->again.def) gv = a->again.def;
			if (gv != a->again.cur && ctrl_set(fd, V4L2_CID_ANALOGUE_GAIN, gv) == 0) a->again.cur = gv;
			rest /= gain_factor(&a->again);
		}
		if (a->dgain.ok && a->dgain.def > 0 && rest > 1.01) {
			int lim = a->dgain.def * 4 < a->dgain.max ? a->dgain.def * 4 : a->dgain.max;
			int dv = (int)(a->dgain.def * rest);
			if (dv > lim) dv = lim;
			if (dv < a->dgain.def) dv = a->dgain.def;
			if (dv != a->dgain.cur && ctrl_set(fd, V4L2_CID_DIGITAL_GAIN, dv) == 0) a->dgain.cur = dv;
			rest /= gain_factor(&a->dgain);
		} else if (a->dgain.ok && a->dgain.def > 0 && a->dgain.cur != a->dgain.def && rest <= 1.0) {
			if (ctrl_set(fd, V4L2_CID_DIGITAL_GAIN, a->dgain.def) == 0) a->dgain.cur = a->dgain.def;
		}

		a->sw_gain = rest < 0.5 ? 0.5 : rest > 8.0 ? 8.0 : rest;
	}

	if (st->mean[CH_R] > 2 && st->mean[CH_B] > 2 && st->mean[CH_G] > 8) {
		double tr = st->mean[CH_G] / st->mean[CH_R], tb = st->mean[CH_G] / st->mean[CH_B];
		tr = tr < 1.0 ? 1.0 : tr > 3.5 ? 3.5 : tr;
		tb = tb < 1.0 ? 1.0 : tb > 3.5 ? 3.5 : tb;
		a->wb[CH_R] = 0.85 * a->wb[CH_R] + 0.15 * tr;
		a->wb[CH_B] = 0.85 * a->wb[CH_B] + 0.15 * tb;
	}
}

struct af {
	struct ctl focus;
	int state;
	int pos[24], npos, idx, wait;
	double score[24];
	double ref;
	int soft_frames;
	int lo, hi;
};

static void af_start(struct af *f, int lo, int hi, int steps, int state) {
	f->state = state; f->npos = steps; f->idx = 0; f->wait = 0; f->lo = lo; f->hi = hi;
	for (int i = 0; i < steps; i++) f->pos[i] = lo + (int)((long)(hi - lo) * i / (steps - 1));
}

static void af_set(struct camera *c, struct af *f, int v) {
	if (ctrl_set(c->lens_fd, V4L2_CID_FOCUS_ABSOLUTE, v) == 0) f->focus.cur = v;
}

static void af_frame(struct camera *c, struct af *f, const struct stats *st, int ae_settled) {
	if (c->lens_fd < 0 || !f->focus.ok) return;
	if (f->state == 0) {
		if (st->sharp < f->ref * 0.6 && ae_settled) f->soft_frames++;
		else f->soft_frames = 0;
		if (f->soft_frames > 45) {
			f->soft_frames = 0;
			af_start(f, f->focus.min, f->focus.max, 12, 1);
			af_set(c, f, f->pos[0]);
		}
		return;
	}
	if (++f->wait < 3) return;
	f->wait = 0;
	f->score[f->idx] = st->sharp;
	if (++f->idx < f->npos) { af_set(c, f, f->pos[f->idx]); return; }
	int best = 0;
	for (int i = 1; i < f->npos; i++) if (f->score[i] > f->score[best]) best = i;
	if (f->state == 1) {
		int span = (f->hi - f->lo) / (f->npos - 1);
		int lo = f->pos[best] - span, hi = f->pos[best] + span;
		if (lo < f->focus.min) lo = f->focus.min;
		if (hi > f->focus.max) hi = f->focus.max;
		af_start(f, lo, hi, 7, 2);
		af_set(c, f, f->pos[0]);
	} else {
		af_set(c, f, f->pos[best]);
		f->ref = f->score[best];
		f->state = 0;
		LOG("AF: focus %d (sharpness %.4f)", f->pos[best], f->ref);
	}
}

static int loopback_readers(int fd) {
	int v;
	if (ctrl_get(fd, CID_LOOPBACK_READERS, &v) < 0) return 1;
	return v;
}

static int loopback_fmt(int fd, int w, int h) {
	struct v4l2_format f;
	memset(&f, 0, sizeof(f));
	f.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
	f.fmt.pix.width = w; f.fmt.pix.height = h;
	f.fmt.pix.pixelformat = V4L2_PIX_FMT_YUYV;
	f.fmt.pix.field = V4L2_FIELD_NONE;
	f.fmt.pix.bytesperline = w * 2;
	f.fmt.pix.sizeimage = w * h * 2;
	f.fmt.pix.colorspace = V4L2_COLORSPACE_SRGB;
	return xioctl(fd, VIDIOC_S_FMT, &f);
}

static void loopback_size(int fd, int *w, int *h) {
	struct v4l2_format f;
	memset(&f, 0, sizeof(f));
	f.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
	if (xioctl(fd, VIDIOC_G_FMT, &f) == 0 && f.fmt.pix.width >= 16 && f.fmt.pix.height >= 16) {
		*w = f.fmt.pix.width & ~1;
		*h = f.fmt.pix.height;
	}
}

static void write_ppm(const char *path, const uint8_t *yuyv, int w, int h) {
	FILE *f = fopen(path, "wb");
	if (!f) return;
	fprintf(f, "P6\n%d %d\n255\n", w, h);
	for (int i = 0; i < w * h; i += 2) {
		const uint8_t *p = yuyv + i * 2;
		for (int k = 0; k < 2; k++) {
			int Y = p[k * 2], U = p[1] - 128, V = p[3] - 128;
			int r = Y + ((359 * V) >> 8), g = Y - ((88 * U + 183 * V) >> 8), b = Y + ((454 * U) >> 8);
			uint8_t px[3] = { r < 0 ? 0 : r > 255 ? 255 : r, g < 0 ? 0 : g > 255 ? 255 : g, b < 0 ? 0 : b > 255 ? 255 : b };
			fwrite(px, 1, 3, f);
		}
	}
	fclose(f);
}

static void denoise_for(const struct ae *a, struct pipeline *p) {
	double g = gain_factor(&a->again) * gain_factor(&a->dgain) * a->sw_gain;
	double t = g <= 4.0 ? 0.0 : g >= 64.0 ? 1.0 : log(g / 4.0) / log(16.0);
	p->tnr_q8 = (int)(215.0 * t * opt.denoise);
	p->tnr_thr = 6 + (int)(18.0 * t);
}

static int sharpness_for(const struct ae *a) {
	double g = gain_factor(&a->again) * gain_factor(&a->dgain) * a->sw_gain;
	double t = g <= 2.0 ? 0.0 : g >= 32.0 ? 1.0 : log(g / 2.0) / log(16.0);
	return (int)(256.0 * (0.9 - 0.6 * t) * opt.sharpness);
}

static int g_lock_fd = -1, g_lock_held = 0;

static void want_path(char *buf, size_t n) { snprintf(buf, n, "%s.want", opt.lock); }

static int want_is_mine(void) {
	char path[300], name[64] = "";
	want_path(path, sizeof(path));
	FILE *f = fopen(path, "r");
	if (!f) return 0;
	if (!fgets(name, sizeof(name), f)) name[0] = 0;
	fclose(f);
	name[strcspn(name, "\n")] = 0;
	return !strcmp(name, opt.name);
}

static int other_waiting(void) {
	char path[300];
	want_path(path, sizeof(path));
	return access(path, F_OK) == 0 && !want_is_mine();
}

static void want_clear_mine(void) {
	char path[300];
	want_path(path, sizeof(path));
	if (want_is_mine()) unlink(path);
}

static int lock_take(void) {
	if (g_lock_held) return 1;
	if (g_lock_fd < 0) g_lock_fd = open(opt.lock, O_RDWR | O_CREAT | O_CLOEXEC, 0666);
	if (g_lock_fd < 0) return 1;
	if (flock(g_lock_fd, LOCK_EX | LOCK_NB) == 0) {
		g_lock_held = 1;
		want_clear_mine();
		return 1;
	}
	char path[300];
	want_path(path, sizeof(path));
	if (!want_is_mine()) {
		FILE *f = fopen(path, "w");
		if (f) { fprintf(f, "%s\n", opt.name); fclose(f); }
	}
	return 0;
}

static void lock_drop(void) {
	if (g_lock_held && g_lock_fd >= 0) flock(g_lock_fd, LOCK_UN);
	g_lock_held = 0;
}

static int run(void) {
	struct camera cam;
	if (find_camera(&cam, opt.sensor, 0, 1) < 0) {
		LOG("no IPU3 sensor matching '%s' with a complete media graph", opt.sensor ? opt.sensor : "(any)");
		return 2;
	}
	int rot = opt.rotate >= 0 ? opt.rotate : cam.rotation;
	if (rot != 0 && rot != 90 && rot != 180 && rot != 270) rot = 0;
	LOG("%s (%s): %s -> %s -> %s, orientation %d, rotation %d (using %d)%s%s", opt.name, cam.sensor_name, cam.sensor_dev, cam.csi2_dev,
	    cam.cio2_dev, cam.orientation, cam.rotation, rot, cam.lens_ent ? ", lens " : "", cam.lens_ent ? cam.lens_name : "");
	for (int i = 0; i < cam.nmodes; i++) LOG("  mode %dx%d", cam.modes[i].w, cam.modes[i].h);

	int out = open(opt.out, O_RDWR);
	if (out < 0) { LOG("open %s: %s", opt.out, strerror(errno)); close_camera(&cam); return 1; }
	int ow = opt.init_w, oh = opt.init_h;
	if (loopback_fmt(out, ow, oh) < 0) LOG("loopback S_FMT %dx%d: %s", ow, oh, strerror(errno));
	loopback_size(out, &ow, &oh);
	uint8_t *frame = malloc((size_t)ow * oh * 2);
	if (!frame) return 1;

	for (size_t i = 0; i < (size_t)ow * oh * 2; i += 2) { frame[i] = 0; frame[i + 1] = 128; }
	if (write(out, frame, (size_t)ow * oh * 2) < 0) LOG("loopback prime write: %s", strerror(errno));

	struct pipeline p;
	memset(&p, 0, sizeof(p));
	p.sat_q8 = (int)(opt.saturation * 256);
	struct ae a;
	memset(&a, 0, sizeof(a));
	struct af f;
	memset(&f, 0, sizeof(f));
	struct stats st;
	int cur_mode_w = 0, cur_mode_h = 0, map_w = 0, map_h = 0;
	long frames = 0, session_frames = 0;
	int snap_done = 0, fails = 0;
	double t_last_log = 0, t_last_frame = 0, idle_since = 0, proc_sum = 0;
	long log_frames = 0;
	int pend_w = 0, pend_h = 0, waiting = 0, black_frames = 0;
	long dropped = 0;
	double last_black_reset = 0;
	double pend_since = 0;
	size_t frame_cap = (size_t)ow * oh * 2;

	while (g_run) {
		if (loopback_readers(out) <= 0) {
			if (cam.streaming && idle_since == 0) idle_since = now_s();
			if (!cam.streaming || now_s() - idle_since > 2.0 || other_waiting()) {
				if (cam.streaming) {
					stream_off(&cam);
					if (cam.lens_fd >= 0) { close(cam.lens_fd); cam.lens_fd = -1; }
					LOG("reader gone after %ld frames: sensor off%s", session_frames, other_waiting() ? " (other camera waiting)" : "");
				}
				lock_drop();
				want_clear_mine();
				waiting = 0;
				idle_since = 0;
				usleep(50000);
				continue;
			}
		} else idle_since = 0;
		if (!cam.streaming && !lock_take()) {
			if (!waiting++) LOG("waiting for the other camera to stop (they share the PMIC)");
			usleep(30000);
			continue;
		}
		waiting = 0;

		int tw = ow, th = oh;
		loopback_size(out, &tw, &th);
		if (!cam.streaming || tw != map_w || th != map_h || pend_w) {
			struct mode md = pick_mode(&cam, tw, th, rot);
			struct mode curm = { cur_mode_w, cur_mode_h };
			if (cam.streaming && (long)md.w * md.h < (long)cur_mode_w * cur_mode_h && mode_serves(curm, tw, th, rot)) md = curm;
			if (cam.streaming && (md.w != cur_mode_w || md.h != cur_mode_h)) {
				if (md.w != pend_w || md.h != pend_h) { pend_w = md.w; pend_h = md.h; pend_since = now_s(); }
				if (now_s() - pend_since < 0.3) md = curm;
				else stream_off(&cam);
			} else pend_w = pend_h = 0;
			if (!cam.streaming) {
				int fresh = 0;
				if (md.w != cur_mode_w || md.h != cur_mode_h || cam.nbuf == 0) {
					if (configure(&cam, md) < 0) {
						cur_mode_w = cur_mode_h = 0;
						if (++fails % 10 == 1) LOG("configure failed (%d), retrying", fails);
						usleep(500000);
						continue;
					}
					fresh = 1;
					cur_mode_w = cam.mode_w; cur_mode_h = cam.mode_h;
					free(p.raw);
					p.w = cam.mode_w; p.h = cam.mode_h;
					p.raw = calloc((size_t)p.w * p.h, sizeof(uint16_t));
					if (!p.raw) break;
					cfa_from_code(cam.code, p.cfa);
					map_w = map_h = 0;
				}
				ae_init(&cam, &a, fresh);
				build_lut(&p, opt.black, a.wb, a.sw_gain);
				p.sharp_q8 = sharpness_for(&a);
				denoise_for(&a, &p);
				if (cam.lens_ent && cam.lens_fd < 0 && (cam.lens_fd = open(cam.lens_dev, O_RDWR)) >= 0) {
					f.focus.id = V4L2_CID_FOCUS_ABSOLUTE;
					ctl_query(cam.lens_fd, &f.focus);
					if (f.focus.ok) {
						af_start(&f, f.focus.min, f.focus.max, 12, 1);
						af_set(&cam, &f, f.pos[0]);
					}
				}
				if (stream_on(&cam) < 0) {
					if (++fails % 10 == 1) LOG("stream start failed (%d), retrying", fails);
					usleep(300000);
					continue;
				}
				session_frames = 0;
				p.prev_ok = 0;
				black_frames = 0;
				t_last_frame = now_s();
				LOG("reader active: %s streaming %dx%d for %dx%d output", cam.sensor_name, cam.mode_w, cam.mode_h, tw, th);
			}
			if ((size_t)tw * th * 2 > frame_cap) {
				uint8_t *nf = realloc(frame, (size_t)tw * th * 2);
				if (!nf) break;
				frame = nf; frame_cap = (size_t)tw * th * 2;
			}
			if (tw != map_w || th != map_h) {
				setup_maps(&p, tw, th, rot, opt.mirror);
				map_w = tw; map_h = th;
				LOG("output %dx%d: crop %dx%d+%d+%d of %dx%d", tw, th, p.crop_w, p.crop_h, p.crop_x, p.crop_y, p.w, p.h);
			}
		}

		struct pollfd pfd = { .fd = cam.cap_fd, .events = POLLIN };
		int pr = poll(&pfd, 1, 500);
		if (pr <= 0) {
			if (now_s() - t_last_frame > 1.5) {
				LOG("no frame for 1.5 s: restarting the stream");
				stream_off(&cam);
				cur_mode_w = cur_mode_h = 0;
				t_last_frame = now_s();
			}
			continue;
		}
		struct v4l2_plane pl[VIDEO_MAX_PLANES];
		struct v4l2_buffer b;
		memset(&b, 0, sizeof(b)); memset(pl, 0, sizeof(pl));
		b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE; b.memory = V4L2_MEMORY_MMAP; b.m.planes = pl; b.length = VIDEO_MAX_PLANES;
		if (xioctl(cam.cap_fd, VIDIOC_DQBUF, &b) < 0) {
			if (errno == EAGAIN) continue;
			LOG("DQBUF: %s", strerror(errno));
			stream_off(&cam);
			cur_mode_w = cur_mode_h = 0;
			usleep(200000);
			continue;
		}
		for (;;) {
			struct v4l2_plane pl2[VIDEO_MAX_PLANES];
			struct v4l2_buffer nb;
			memset(&nb, 0, sizeof(nb)); memset(pl2, 0, sizeof(pl2));
			nb.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE; nb.memory = V4L2_MEMORY_MMAP; nb.m.planes = pl2; nb.length = VIDEO_MAX_PLANES;
			if (xioctl(cam.cap_fd, VIDIOC_DQBUF, &nb) < 0) break;
			queue_buf(&cam, b.index);
			b = nb;
			memcpy(pl, pl2, sizeof(pl));
			b.m.planes = pl;
			dropped++;
		}
		t_last_frame = now_s();
		double tp = t_last_frame;
		if (!(b.flags & V4L2_BUF_FLAG_ERROR)) {
			int y0, y1;
			needed_rows(&p, rot, &y0, &y1);
			run_parallel_unpack(&p, cam.buf[b.index], cam.bpl, y0, y1);
		}
		queue_buf(&cam, b.index);
		if (b.flags & V4L2_BUF_FLAG_ERROR) continue;

		compute_stats(&p, opt.black, &st);
		if (st.mean[CH_G] < 0.5 && st.mean[CH_R] < 0.5 && st.sat_frac == 0 && st.raw_sd < 0.75 && session_frames > 15) black_frames++;
		else black_frames = 0;
		if (black_frames > 45 && now_s() - last_black_reset > 10.0) {
			LOG("sensor delivers black frames: power-cycling it");
			last_black_reset = now_s();
			black_frames = 0;
			stream_off(&cam);
			cur_mode_w = cur_mode_h = 0;
			continue;
		}
		if (++a.frames_since_set >= 3) {
			ae_step(&cam, &a, &st, opt.black);
			a.frames_since_set = 0;
			build_lut(&p, opt.black, a.wb, a.sw_gain);
			p.sharp_q8 = sharpness_for(&a);
			denoise_for(&a, &p);
		}
		af_frame(&cam, &f, &st, a.settled > 2);
		run_parallel_process(&p, frame, rot);
		if (write(out, frame, (size_t)map_w * map_h * 2) < 0 && errno != EAGAIN) LOG("loopback write: %s", strerror(errno));
		frames++; session_frames++; log_frames++;
		proc_sum += now_s() - tp;

		if (opt.debug_dir && !snap_done && session_frames == 90) {
			char path[256];
			snprintf(path, sizeof(path), "%s/%s-%dx%d.ppm", opt.debug_dir, opt.name, map_w, map_h);
			write_ppm(path, frame, map_w, map_h);
			snap_done = 1;
			LOG("snapshot %s", path);
		}
		double t = now_s();
		if (t - t_last_log > 10.0) {
			double span = t_last_log > 0 ? t - t_last_log : 10.0;
			LOG("%.1f fps, %.1f ms/frame, %ld stale frames skipped", log_frames / span, log_frames ? proc_sum * 1000 / log_frames : 0, dropped);
			dropped = 0;
			log_frames = 0; proc_sum = 0;
			t_last_log = t;
			LOG("frames %ld: mean R/G/B %.0f/%.0f/%.0f clip %.1f%% | exp %d again %d dgain %d vblank %d sw %.2f | wb %.2f/%.2f%s",
			    frames, st.mean[0], st.mean[1], st.mean[2], st.sat_frac * 100, a.exp.cur, a.again.cur, a.dgain.cur, a.vblank.cur,
			    a.sw_gain, a.wb[0], a.wb[2], f.focus.ok ? "" : " (no lens)");
		}
	}
	stream_off(&cam);
	lock_drop();
	want_clear_mine();
	free_buffers(&cam);
	close_camera(&cam);
	close(out);
	free(frame); free(p.raw); free(p.map_x); free(p.map_y); free(p.yplane); free(p.prev);
	LOG("stopped after %ld frames", frames);
	return 0;
}

static int probe(void) {
	int n = 0;
	for (int i = 0; i < 8; i++) {
		struct camera c;
		if (find_camera(&c, NULL, i, 0) < 0) break;
		printf("%s orientation=%d rotation=%d lens=%s modes=", c.sensor_name, c.orientation, c.rotation, c.lens_ent ? c.lens_name : "-");
		for (int k = 0; k < c.nmodes; k++) printf("%s%dx%d", k ? "," : "", c.modes[k].w, c.modes[k].h);
		printf(" nodes=%s,%s,%s\n", c.sensor_dev, c.csi2_dev, c.cio2_dev);
		close_camera(&c);
		n++;
	}
	return n ? 0 : 1;
}

static int selftest(void) {
	int fails = 0;

	int widths[] = { 25, 50, 100, 1296, 1632, 2592, 3264, 37 };
	for (unsigned wi = 0; wi < sizeof(widths) / sizeof(widths[0]); wi++) {
		int w = widths[wi], bpl = ipu3_bpl(w);
		uint16_t *a = malloc(w * 2), *b = malloc(w * 2);
		uint8_t *pk = malloc(bpl + 8);
		for (int x = 0; x < w; x++) a[x] = (x * 37 + 11 * (x >> 3) + 5) & 0x3ff;
		pack_row(a, pk, w, bpl);
		unpack_row(pk, b, w);
		for (int x = 0; x < w; x++) if (a[x] != b[x]) { printf("FAIL unpack w=%d x=%d %u!=%u\n", w, x, a[x], b[x]); fails++; break; }
		free(a); free(b); free(pk);
	}

	uint32_t codes[4] = { MEDIA_BUS_FMT_SRGGB10_1X10, MEDIA_BUS_FMT_SGRBG10_1X10, MEDIA_BUS_FMT_SGBRG10_1X10, MEDIA_BUS_FMT_SBGGR10_1X10 };
	const char *names[4] = { "RGGB", "GRBG", "GBRG", "BGGR" };
	int ref[4] = { -1, -1, -1, -1 };
	for (int ci = 0; ci < 4; ci++) {
		struct pipeline p;
		memset(&p, 0, sizeof(p));
		p.w = 64; p.h = 48;
		p.raw = malloc(p.w * p.h * 2);
		cfa_from_code(codes[ci], p.cfa);
		int val[3] = { 700, 400, 200 };
		for (int y = 0; y < p.h; y++) for (int x = 0; x < p.w; x++) p.raw[y * p.w + x] = val[p.cfa[(y & 1) * 2 + (x & 1)]];
		double wb[3] = { 1, 1, 1 };
		build_lut(&p, 64, wb, 1.0);
		p.sat_q8 = 256;
		setup_maps(&p, 32, 24, 0, 0);
		uint8_t *o = malloc(32 * 24 * 2);
		g_nthreads = 2;
		run_parallel_process(&p, o, 0);
		int Y = o[(10 * 32 + 10) * 2], U = o[(10 * 32 + 10) * 2 + 1], V = o[(10 * 32 + 10) * 2 + 3];
		if (ref[0] < 0) { ref[0] = Y; ref[1] = U; ref[2] = V; }
		if (abs(Y - ref[0]) > 1 || abs(U - ref[1]) > 1 || abs(V - ref[2]) > 1) {
			printf("FAIL flat colour %s: YUV %d/%d/%d vs %d/%d/%d\n", names[ci], Y, U, V, ref[0], ref[1], ref[2]); fails++;
		}
		if (!(V > 128 && U < 128)) { printf("FAIL flat colour %s: expected red-orange, YUV %d/%d/%d\n", names[ci], Y, U, V); fails++; }
		free(o); free(p.raw); free(p.map_x); free(p.map_y); free(p.yplane); free(p.prev);
	}

	{
		struct pipeline p;
		memset(&p, 0, sizeof(p));
		p.w = 160; p.h = 120;
		p.raw = malloc(p.w * p.h * 2);
		cfa_from_code(MEDIA_BUS_FMT_SGRBG10_1X10, p.cfa);
		for (int y = 0; y < p.h; y++) for (int x = 0; x < p.w; x++) p.raw[y * p.w + x] = 64 + x * 5;
		double wb[3] = { 1, 1, 1 };
		build_lut(&p, 64, wb, 1.0);
		p.sat_q8 = 256;
		uint8_t *o = malloc(80 * 60 * 2);
		int rots[3] = { 0, 180, 90 };
		for (int r = 0; r < 3; r++) {
			setup_maps(&p, 80, 60, rots[r], 0);
			run_parallel_process(&p, o, rots[r]);
			int left = o[(30 * 80 + 4) * 2], right = o[(30 * 80 + 74) * 2];
			int top = o[(2 * 80 + 40) * 2], bottom = o[(57 * 80 + 40) * 2];
			int ok = rots[r] == 0 ? right > left + 50 : rots[r] == 180 ? left > right + 50 : bottom > top + 50;
			if (!ok) { printf("FAIL rotation %d: L %d R %d T %d B %d\n", rots[r], left, right, top, bottom); fails++; }
		}

		setup_maps(&p, 80, 60, 0, 1);
		run_parallel_process(&p, o, 0);
		if (!(o[(30 * 80 + 4) * 2] > o[(30 * 80 + 74) * 2] + 50)) { printf("FAIL mirror\n"); fails++; }
		free(o); free(p.raw); free(p.map_x); free(p.map_y); free(p.yplane); free(p.prev);
	}

	{
		struct camera c;
		memset(&c, 0, sizeof(c));
		struct mode ms[] = { { 2592, 1944 }, { 1296, 972 }, { 648, 486 }, { 2560, 1440 }, { 1280, 720 }, { 640, 360 } };
		memcpy(c.modes, ms, sizeof(ms)); c.nmodes = 6;
		struct { int tw, th, ew, eh; } t[] = { { 1280, 720, 1280, 720 }, { 1920, 1080, 2560, 1440 }, { 640, 480, 1296, 972 },
						       { 1280, 960, 1296, 972 }, { 320, 240, 1296, 972 }, { 3000, 2000, 2592, 1944 },
						       { 640, 360, 1280, 720 } };
		for (unsigned i = 0; i < sizeof(t) / sizeof(t[0]); i++) {
			struct mode m = pick_mode(&c, t[i].tw, t[i].th, 0);
			if (m.w != t[i].ew || m.h != t[i].eh) {
				printf("FAIL mode for %dx%d: %dx%d, expected %dx%d\n", t[i].tw, t[i].th, m.w, m.h, t[i].ew, t[i].eh); fails++;
			}
		}
		if (!mode_serves((struct mode){ 2560, 1440 }, 320, 180, 0) || mode_serves((struct mode){ 2560, 1440 }, 640, 480, 0)) {
			printf("FAIL mode_serves\n"); fails++;
		}
		struct mode m8[] = { { 3264, 2448 }, { 1632, 1224 } };
		memcpy(c.modes, m8, sizeof(m8)); c.nmodes = 2;
		struct mode m = pick_mode(&c, 1920, 1080, 0);
		if (m.w != 1632) { printf("FAIL OV8858 1080p mode: %dx%d\n", m.w, m.h); fails++; }
	}

	{
		struct pipeline p;
		memset(&p, 0, sizeof(p));
		p.w = 2560; p.h = 1440;
		int bpl = ipu3_bpl(p.w);
		uint8_t *packed = malloc((size_t)bpl * p.h);
		uint16_t *row = malloc(p.w * 2);
		for (int y = 0; y < p.h; y++) {
			for (int x = 0; x < p.w; x++) row[x] = (x ^ y) & 0x3ff;
			pack_row(row, packed + (size_t)y * bpl, p.w, bpl);
		}
		p.raw = malloc((size_t)p.w * p.h * 2);
		cfa_from_code(MEDIA_BUS_FMT_SGRBG10_1X10, p.cfa);
		double wb[3] = { 1.8, 1, 1.6 };
		build_lut(&p, 64, wb, 1.0);
		p.sat_q8 = 320;
		p.sharp_q8 = 230;
		setup_maps(&p, 1920, 1080, 180, 0);
		uint8_t *o = malloc(1920 * 1080 * 2);
		long cpus = sysconf(_SC_NPROCESSORS_ONLN);
		g_nthreads = cpus > 4 ? 4 : cpus < 1 ? 1 : (int)cpus;
		struct stats st;
		double t0 = now_s();
		for (int i = 0; i < 10; i++) {
			int y0, y1;
			needed_rows(&p, 180, &y0, &y1);
			run_parallel_unpack(&p, packed, bpl, y0, y1);
			compute_stats(&p, 64, &st);
			run_parallel_process(&p, o, 180);
		}
		double ms = (now_s() - t0) * 100;
		printf("1080p frame (from 2560x1440, %d threads): %.1f ms\n", g_nthreads, ms);
		free(packed); free(row); free(o); free(p.raw); free(p.map_x); free(p.map_y); free(p.yplane); free(p.prev);
	}
	{
		struct pipeline p;
		memset(&p, 0, sizeof(p));
		p.w = 64; p.h = 48;
		p.raw = malloc(p.w * p.h * 2);
		cfa_from_code(MEDIA_BUS_FMT_SGRBG10_1X10, p.cfa);
		for (int y = 0; y < p.h; y++) for (int x = 0; x < p.w; x++) p.raw[y * p.w + x] = x < 32 ? 200 : 600;
		double wb[3] = { 1, 1, 1 };
		build_lut(&p, 64, wb, 1.0);
		p.sat_q8 = 256;
		setup_maps(&p, 64, 48, 0, 0);
		uint8_t *o0 = malloc(64 * 48 * 2), *o1 = malloc(64 * 48 * 2);
		p.sharp_q8 = 0;
		run_parallel_process(&p, o0, 0);
		p.sharp_q8 = 256;
		run_parallel_process(&p, o1, 0);
		int flat0 = o0[(20 * 64 + 10) * 2], flat1 = o1[(20 * 64 + 10) * 2];
		int dark0 = o0[(20 * 64 + 30) * 2], dark1 = o1[(20 * 64 + 30) * 2];
		int lite0 = o0[(20 * 64 + 33) * 2], lite1 = o1[(20 * 64 + 33) * 2];
		if (flat0 != flat1 || !(dark1 < dark0) || !(lite1 > lite0)) {
			printf("FAIL sharpen: flat %d/%d edge dark %d/%d light %d/%d\n", flat0, flat1, dark0, dark1, lite0, lite1); fails++;
		}
		free(o0); free(o1); free(p.raw); free(p.map_x); free(p.map_y); free(p.yplane); free(p.prev);
	}
	{
		char lp[64];
		snprintf(lp, sizeof(lp), "/tmp/ipu3-webcam-test-%d.lock", (int)getpid());
		opt.lock = lp;
		opt.name = "front";
		int ok = lock_take() == 1;
		int pfd[2];
		if (pipe(pfd)) ok = 0;
		pid_t ch = fork();
		if (ch == 0) {
			g_lock_fd = -1; g_lock_held = 0; opt.name = "rear";
			int r1 = lock_take();
			if (write(pfd[1], "x", 1) < 0) _exit(3);
			for (int i = 0; i < 200 && !lock_take(); i++) usleep(10000);
			char wp[300];
			want_path(wp, sizeof(wp));
			_exit(r1 == 0 && g_lock_held && access(wp, F_OK) != 0 ? 0 : 2);
		}
		char c;
		if (read(pfd[0], &c, 1) != 1) ok = 0;
		for (int i = 0; i < 100 && !other_waiting(); i++) usleep(10000);
		ok = ok && other_waiting();
		lock_drop();
		int st = 0;
		waitpid(ch, &st, 0);
		ok = ok && WIFEXITED(st) && WEXITSTATUS(st) == 0 && !other_waiting();
		unlink(lp);
		if (!ok) { printf("FAIL camera lock handover (child status %d)\n", st); fails++; }
	}
	{
		struct pipeline p;
		memset(&p, 0, sizeof(p));
		p.w = 64; p.h = 48;
		p.raw = malloc(p.w * p.h * 2);
		cfa_from_code(MEDIA_BUS_FMT_SGRBG10_1X10, p.cfa);
		double wb[3] = { 1, 1, 1 };
		build_lut(&p, 64, wb, 1.0);
		p.sat_q8 = 256;
		setup_maps(&p, 64, 48, 0, 0);
		p.tnr_q8 = 200; p.tnr_thr = 24;
		uint8_t *o = malloc(64 * 48 * 2), *ref = malloc(64 * 48 * 2);
		for (int y = 0; y < p.h; y++) for (int x = 0; x < p.w; x++) p.raw[y * p.w + x] = 300;
		run_parallel_process(&p, o, 0);
		struct pipeline q = p;
		q.prev = NULL;
		for (int f = 0; f < 8; f++) {
			for (int y = 0; y < p.h; y++) for (int x = 0; x < p.w; x++) p.raw[y * p.w + x] = 300 + ((x * 7 + y * 13 + f * 3) % 5 - 2) * 6;
			run_parallel_process(&p, o, 0);
		}
		run_parallel_process(&q, ref, 0);
		long dn = 0, dr = 0;
		for (int i = 0; i < 64 * 48 * 2; i += 2) { dn += abs(o[i] - o[(20 * 64 + 20) * 2] ); dr += abs(ref[i] - ref[(20 * 64 + 20) * 2]); }
		for (int y = 0; y < p.h; y++) for (int x = 0; x < p.w; x++) p.raw[y * p.w + x] = 900;
		run_parallel_process(&q, ref, 0);
		run_parallel_process(&p, o, 0);
		int moved = abs(o[(20 * 64 + 20) * 2] - ref[(20 * 64 + 20) * 2]);
		if (!(dn * 2 < dr) || moved > 1) { printf("FAIL denoise: residual %ld vs %ld, motion error %d\n", dn, dr, moved); fails++; }
		free(o); free(ref); free(p.raw); free(p.map_x); free(p.map_y); free(p.yplane); free(p.prev);
	}
	printf(fails ? "SELFTEST FAILED (%d)\n" : "SELFTEST PASS\n", fails);
	return fails ? 1 : 0;
}

static void usage(void) {
	fprintf(stderr,
		"usage: ipu3-webcam --sensor NAME [--out /dev/video42] [--name front] [--rotate auto|0|90|180|270] [--mirror]\n"
		"                   [--flicker 50|60|0] [--min-fps 15] [--black 64] [--saturation 1.25] [--threads N]\n"
		"                   [--size 1280x720] [--debug-dir DIR]\n"
		"       ipu3-webcam --probe | --selftest\n");
}

int main(int argc, char **argv) {
	setvbuf(stdout, NULL, _IOLBF, 0);
	for (int i = 1; i < argc; i++) {
		const char *a = argv[i], *v = i + 1 < argc ? argv[i + 1] : NULL;
		if (!strcmp(a, "--probe")) return probe();
		if (!strcmp(a, "--selftest")) return selftest();
		if (!strcmp(a, "--mirror")) { opt.mirror = 1; continue; }
		if (!v) { usage(); return 64; }
		if (!strcmp(a, "--sensor")) opt.sensor = v;
		else if (!strcmp(a, "--out")) opt.out = v;
		else if (!strcmp(a, "--name")) opt.name = v;
		else if (!strcmp(a, "--debug-dir")) opt.debug_dir = v;
		else if (!strcmp(a, "--lock")) opt.lock = v;
		else if (!strcmp(a, "--rotate")) opt.rotate = strcmp(v, "auto") ? atoi(v) : -1;
		else if (!strcmp(a, "--flicker")) opt.flicker_hz = atoi(v);
		else if (!strcmp(a, "--min-fps")) opt.min_fps = atof(v);
		else if (!strcmp(a, "--dark-fps")) opt.dark_fps = atof(v);
		else if (!strcmp(a, "--black")) opt.black = atoi(v);
		else if (!strcmp(a, "--saturation")) opt.saturation = atof(v);
		else if (!strcmp(a, "--sharpness")) opt.sharpness = atof(v);
		else if (!strcmp(a, "--denoise")) opt.denoise = atof(v);
		else if (!strcmp(a, "--threads")) opt.threads = atoi(v);
		else if (!strcmp(a, "--size")) { if (sscanf(v, "%dx%d", &opt.init_w, &opt.init_h) != 2) { usage(); return 64; } }
		else { usage(); return 64; }
		i++;
	}
	if (!opt.sensor) { usage(); return 64; }
	if (opt.black < 0 || opt.black > 200) opt.black = 64;
	long cpus = sysconf(_SC_NPROCESSORS_ONLN);
	g_nthreads = opt.threads > 0 ? opt.threads : (cpus > 4 ? 4 : cpus < 1 ? 1 : (int)cpus);
	if (g_nthreads > MAX_THREADS) g_nthreads = MAX_THREADS;
	signal(SIGINT, on_signal);
	signal(SIGTERM, on_signal);
	signal(SIGPIPE, SIG_IGN);
	LOG("ipu3-webcam: sensor '%s' -> %s, %d threads, flicker %d Hz, min %.0f fps", opt.sensor, opt.out, g_nthreads, opt.flicker_hz,
	    opt.min_fps);
	return run();
}
