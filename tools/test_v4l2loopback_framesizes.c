#include <assert.h>
#include <stdio.h>

#include "../kernel/v4l2loopback/frame_sizes.h"

int main(void)
{
	struct v4l2l_frame_size size;
	const struct v4l2l_frame_size expected[] = {
		{1920, 1080}, {1280, 720}, {960, 540},
		{640, 480}, {640, 360}, {320, 240}, {1280, 960},
	};
	unsigned int i;

	for (i = 0; i < sizeof(expected) / sizeof(expected[0]); i++) {
		assert(v4l2l_frame_size_at(i, 2, 8192, 1, 8192, &size) == 0);
		assert(size.width == expected[i].width && size.height == expected[i].height);
	}
	assert(v4l2l_frame_size_at(7, 2, 8192, 1, 8192, &size) != 0);
	/* The Latitude bounds list exactly 1280x960. */
	assert(v4l2l_frame_size_at(0, 1280, 1280, 960, 960, &size) == 0);
	assert(size.width == 1280 && size.height == 960);
	assert(v4l2l_frame_size_at(1, 1280, 1280, 960, 960, &size) != 0);
	assert(v4l2l_frame_size_at(0, 640, 640, 480, 480, &size) == 0);
	assert(size.width == 640 && size.height == 480);
	assert(v4l2l_frame_size_at(1, 640, 640, 480, 480, &size) != 0);
	assert(v4l2l_frame_size_at(0, 640, 640, 360, 480, &size) == 0);
	assert(size.width == 640 && size.height == 480);
	assert(v4l2l_frame_size_at(1, 640, 640, 360, 480, &size) == 0);
	assert(size.width == 640 && size.height == 360);
	assert(v4l2l_frame_size_at(2, 640, 640, 360, 480, &size) != 0);
	assert(v4l2l_frame_size_at(0, 650, 700, 400, 500, &size) != 0);
	puts("V4L2LOOPBACK_FRAME_SIZES_TEST_PASS");
	return 0;
}
