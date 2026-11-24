/*
 * kernel/libcamera-gbos/gbos_convert.h on the build machine: the bridge's YUYV picture to the NV12
 * streams the libcamera plugin delivers. Exact copy when no scaling is needed, uniform colours
 * stay exact at every size, the crop is centred with the stream's aspect, colours stay where
 * they are, and nothing is written past the stream's width in a padded buffer.
 */
#include <stdio.h>
#include <string.h>

#include "../kernel/libcamera-gbos/gbos_convert.h"

#define SENTINEL 0xa5

static unsigned char *picture(unsigned int w, unsigned int h)
{
	unsigned char *p = malloc((size_t)w * h * 2);

	for (unsigned int y = 0; y < h; y++)
		for (unsigned int x = 0; x < w; x += 2) {
			unsigned char *q = p + ((size_t)y * w + x) * 2;
			q[0] = (unsigned char)(16 + (x * 7 + y * 3) % 220);
			q[2] = (unsigned char)(16 + ((x + 1) * 7 + y * 3) % 220);
			q[1] = x < w / 2 ? 60 : 190;		/* U: left / right */
			q[3] = y < h / 2 ? 80 : 210;		/* V: top / bottom */
		}
	return p;
}

static int convert(const unsigned char *src, unsigned int sw, unsigned int sh, unsigned int dw,
		   unsigned int dh, unsigned int stride, unsigned char **y, unsigned char **uv)
{
	*y = malloc((size_t)stride * dh);
	*uv = malloc((size_t)stride * dh / 2);
	memset(*y, SENTINEL, (size_t)stride * dh);
	memset(*uv, SENTINEL, (size_t)stride * dh / 2);
	return gbos_yuyv_to_nv12(src, sw, sh, *y, *uv, stride, dw, dh);
}

static int padding_untouched(const unsigned char *y, const unsigned char *uv, unsigned int dw,
			     unsigned int dh, unsigned int stride)
{
	for (unsigned int r = 0; r < dh; r++)
		for (unsigned int x = dw; x < stride; x++)
			if (y[(size_t)r * stride + x] != SENTINEL ||
			    (r < dh / 2 && uv[(size_t)r * stride + x] != SENTINEL))
				return 0;
	return 1;
}

int main(void)
{
	const unsigned int sw = 1920, sh = 1440;
	unsigned char *src = picture(sw, sh), *y, *uv;

	/* Crops: 16:9 from 4:3 loses top and bottom; 4:3 keeps everything; 1:1 is centred. */
	struct gbos_crop c = gbos_crop_for(sw, sh, 1280, 720);
	if (c.x != 0 || c.y != 180 || c.w != 1920 || c.h != 1080)
		return 1;
	c = gbos_crop_for(sw, sh, 640, 480);
	if (c.x || c.y || c.w != sw || c.h != sh)
		return 2;
	c = gbos_crop_for(1920, 1080, 1440, 1080);
	if (c.x != 240 || c.y != 0 || c.w != 1440 || c.h != 1080)
		return 3;
	c = gbos_crop_for(sw, sh, 480, 480);
	if (c.w != 1440 || c.h != 1440 || c.x != 240)
		return 4;

	/* 1920x1080 from 1920x1440: an exact copy of rows 180..1259, chroma of each row pair. */
	if (convert(src, sw, sh, 1920, 1080, 1984, &y, &uv) || !padding_untouched(y, uv, 1920, 1080, 1984))
		return 5;
	for (unsigned int r = 0; r < 1080; r++)
		for (unsigned int x = 0; x < 1920; x++)
			if (y[(size_t)r * 1984 + x] != src[((size_t)(r + 180) * sw + x) * 2])
				return 6;
	for (unsigned int r = 0; r < 540; r++)
		for (unsigned int p = 0; p < 960; p++) {
			const unsigned char *a = src + ((size_t)(180 + 2 * r) * sw + 2 * p) * 2, *b = a + sw * 2;
			if (uv[(size_t)r * 1984 + 2 * p] != (a[1] + b[1] + 1) / 2 ||
			    uv[(size_t)r * 1984 + 2 * p + 1] != (a[3] + b[3] + 1) / 2)
				return 7;
		}
	free(y);
	free(uv);

	/* Uniform colour: exact at every size, down and up. */
	unsigned char *flat = malloc((size_t)sw * sh * 2);
	for (size_t i = 0; i < (size_t)sw * sh * 2; i += 4) {
		flat[i] = flat[i + 2] = 100;
		flat[i + 1] = 60;
		flat[i + 3] = 200;
	}
	static const unsigned int sizes[][2] = {
		{ 320, 240 }, { 640, 360 }, { 640, 480 }, { 1280, 720 }, { 1280, 960 }, { 1440, 1080 },
		{ 1920, 1440 }, { 2560, 1920 }, { 2, 2 },
	};
	for (unsigned int k = 0; k < sizeof(sizes) / sizeof(sizes[0]); k++) {
		unsigned int dw = sizes[k][0], dh = sizes[k][1], stride = dw + 64;
		if (convert(flat, sw, sh, dw, dh, stride, &y, &uv) || !padding_untouched(y, uv, dw, dh, stride))
			return 8;
		for (unsigned int r = 0; r < dh; r++)
			for (unsigned int x = 0; x < dw; x++)
				if (y[(size_t)r * stride + x] != 100 ||
				    (r < dh / 2 && uv[(size_t)r * stride + x] != ((x & 1) ? 200 : 60)))
					return 9;
		free(y);
		free(uv);
	}
	free(flat);

	/* Colours stay in place when scaled: U left/right, V top/bottom, away from the edges. */
	if (convert(src, sw, sh, 640, 480, 640, &y, &uv))
		return 10;
	for (unsigned int r = 0; r < 240; r++)
		for (unsigned int p = 0; p < 320; p++) {
			int u = uv[(size_t)r * 640 + 2 * p], v = uv[(size_t)r * 640 + 2 * p + 1];
			if ((p < 150 && u != 60) || (p > 170 && u != 190) ||
			    (r < 110 && v != 80) || (r > 130 && v != 210))
				return 11;
		}
	/* Luma: the mean of the scaled picture follows the source's. */
	double ms = 0, md = 0;
	for (size_t i = 0; i < (size_t)sw * sh * 2; i += 2)
		ms += src[i];
	for (size_t i = 0; i < (size_t)640 * 480; i++)
		md += y[i];
	ms /= (double)sw * sh;
	md /= 640.0 * 480;
	if (md < ms - 3 || md > ms + 3)
		return 12;
	free(y);
	free(uv);
	free(src);
	puts("LIBCAMERA_GBOS_CONVERT_TEST_PASS");
	return 0;
}
