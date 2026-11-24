/* SPDX-License-Identifier: GPL-2.0+ */
#ifndef V4L2LOOPBACK_FRAME_SIZES_H
#define V4L2LOOPBACK_FRAME_SIZES_H

struct v4l2l_frame_size {
	unsigned int width;
	unsigned int height;
};

static const struct v4l2l_frame_size v4l2l_frame_sizes[] = {
	{ 1920, 1080 }, { 1280, 720 }, { 960, 540 },
	{ 640, 480 }, { 640, 360 }, { 320, 240 },
	/* Latitude 7320 cameras (4:3), last so the generic list keeps its order. */
	{ 1280, 960 },
};

static int v4l2l_frame_size_at(unsigned int index, unsigned int min_width,
			       unsigned int max_width, unsigned int min_height,
			       unsigned int max_height, struct v4l2l_frame_size *result)
{
	unsigned int i;

	for (i = 0; i < sizeof(v4l2l_frame_sizes) / sizeof(v4l2l_frame_sizes[0]); i++) {
		const struct v4l2l_frame_size *size = &v4l2l_frame_sizes[i];

		if (size->width < min_width || size->width > max_width ||
		    size->height < min_height || size->height > max_height)
			continue;
		if (index-- == 0) {
			*result = *size;
			return 0;
		}
	}
	return -1;
}

#endif
