/* SPDX-License-Identifier: LGPL-2.1-or-later */
/*
 * The bridge's YUYV picture to an NV12 stream of any size (gbos_ipu6.cpp). Plain C, so the
 * host tests (tools/test_libcamera_gbos_convert.c) build it with gcc.
 *
 * The stream is a centred crop of the picture with the stream's aspect (a 16:9 stream from the
 * 4:3 picture loses the top and bottom), scaled bilinearly. A crop that already has the
 * stream's size (1920x1080 from 1920x1440) is copied without scaling. Chroma is sampled on the
 * picture's own chroma grid (one U, V per pixel pair), so colours stay where the luma is.
 */
#ifndef GBOS_CONVERT_H
#define GBOS_CONVERT_H

#include <stdint.h>
#include <stdlib.h>

struct gbos_crop {
	unsigned int x, y, w, h;
};

/* Centred, even-aligned crop of sw x sh with the aspect of dw x dh. */
static inline struct gbos_crop gbos_crop_for(unsigned int sw, unsigned int sh, unsigned int dw,
					     unsigned int dh)
{
	struct gbos_crop c = { 0, 0, sw, sh };

	if ((uint64_t)dw * sh > (uint64_t)dh * sw)
		c.h = (unsigned int)((uint64_t)sw * dh / dw) & ~1u;
	else if ((uint64_t)dw * sh < (uint64_t)dh * sw)
		c.w = (unsigned int)((uint64_t)sh * dw / dh) & ~1u;
	if (c.w < 2) c.w = 2;
	if (c.h < 2) c.h = 2;
	c.x = ((sw - c.w) / 2) & ~1u;
	c.y = ((sh - c.h) / 2) & ~1u;
	return c;
}

/*
 * Source positions for n outputs over [start, start + len) in 16.8 fixed point: the integer part
 * and the fraction of the next sample, centres aligned.
 */
static inline void gbos_taps(unsigned int n, unsigned int start, unsigned int len, unsigned int *i0,
			     unsigned int *i1, unsigned int *frac)
{
	for (unsigned int k = 0; k < n; k++) {
		int64_t pos = ((int64_t)(2 * k + 1) * len * 256) / (2 * (int64_t)n) - 128;
		if (pos < 0)
			pos = 0;
		unsigned int a = (unsigned int)(pos >> 8);
		if (a > len - 1)
			a = len - 1;
		i0[k] = start + a;
		i1[k] = start + (a + 1 < len ? a + 1 : a);
		frac[k] = a + 1 < len ? (unsigned int)(pos & 255) : 0;
	}
}

static inline uint8_t gbos_lerp2(unsigned int a, unsigned int b, unsigned int c, unsigned int d,
				 unsigned int fx, unsigned int fy)
{
	unsigned int top = a * (256 - fx) + b * fx, bottom = c * (256 - fx) + d * fx;

	return (uint8_t)((top * (256 - fy) + bottom * fy + 32768) >> 16);
}

/*
 * src: sw x sh YUYV (stride sw * 2). y: dh rows of dw bytes at stride; uv: dh / 2 rows of dw
 * interleaved U, V at stride. dw, dh even. Returns 0, or -1 without memory for the tables.
 */
static inline int gbos_yuyv_to_nv12(const uint8_t *src, unsigned int sw, unsigned int sh, uint8_t *y,
				    uint8_t *uv, unsigned int stride, unsigned int dw, unsigned int dh)
{
	struct gbos_crop c = gbos_crop_for(sw, sh, dw, dh);
	const size_t row = (size_t)sw * 2;

	if (c.w == dw && c.h == dh) {
		for (unsigned int r = 0; r < dh; r++) {
			const uint8_t *s = src + (c.y + r) * row + (size_t)c.x * 2;
			uint8_t *d = y + (size_t)r * stride;
			for (unsigned int x = 0; x < dw; x++)
				d[x] = s[2 * x];
		}
		for (unsigned int r = 0; r < dh / 2; r++) {
			const uint8_t *s0 = src + (c.y + 2 * r) * row + (size_t)c.x * 2, *s1 = s0 + row;
			uint8_t *d = uv + (size_t)r * stride;
			for (unsigned int p = 0; p < dw / 2; p++) {
				d[2 * p] = (uint8_t)((s0[4 * p + 1] + s1[4 * p + 1] + 1) >> 1);
				d[2 * p + 1] = (uint8_t)((s0[4 * p + 3] + s1[4 * p + 3] + 1) >> 1);
			}
		}
		return 0;
	}

	unsigned int n = dw > dh ? dw : dh;
	unsigned int *t = (unsigned int *)malloc(sizeof(unsigned int) * 6 * n);
	if (!t)
		return -1;
	unsigned int *x0 = t, *x1 = t + n, *fx = t + 2 * n, *y0 = t + 3 * n, *y1 = t + 4 * n, *fy = t + 5 * n;

	gbos_taps(dw, c.x, c.w, x0, x1, fx);
	gbos_taps(dh, c.y, c.h, y0, y1, fy);
	for (unsigned int r = 0; r < dh; r++) {
		const uint8_t *a = src + y0[r] * row, *b = src + y1[r] * row;
		uint8_t *d = y + (size_t)r * stride;
		for (unsigned int x = 0; x < dw; x++)
			d[x] = gbos_lerp2(a[2 * x0[x]], a[2 * x1[x]], b[2 * x0[x]], b[2 * x1[x]], fx[x], fy[r]);
	}
	/* Chroma grid: sw / 2 pairs per row, every row. */
	gbos_taps(dw / 2, c.x / 2, c.w / 2, x0, x1, fx);
	gbos_taps(dh / 2, c.y, c.h, y0, y1, fy);
	for (unsigned int r = 0; r < dh / 2; r++) {
		const uint8_t *a = src + y0[r] * row, *b = src + y1[r] * row;
		uint8_t *d = uv + (size_t)r * stride;
		for (unsigned int p = 0; p < dw / 2; p++) {
			unsigned int i = 4 * x0[p], j = 4 * x1[p];
			d[2 * p] = gbos_lerp2(a[i + 1], a[j + 1], b[i + 1], b[j + 1], fx[p], fy[r]);
			d[2 * p + 1] = gbos_lerp2(a[i + 3], a[j + 3], b[i + 3], b[j + 3], fx[p], fy[r]);
		}
	}
	free(t);
	return 0;
}

#endif
