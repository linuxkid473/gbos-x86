#define CAMERA_BRIDGE_TEST
#include "../kernel/ipu6-loopback.c"

int main(void)
{
	(void)capture_to_loopback;
	(void)probe;
	uint16_t raw[8 * 8];
	unsigned char out[OUT_FRAME_BYTES];
	for (unsigned int y = 0; y < 8; y++)
		for (unsigned int x = 0; x < 8; x++)
			raw[y * 8 + x] = (uint16_t)(x + y * 8);

	if (channel_at(0, 0) != 1 || channel_at(1, 1) != 1 ||
	    channel_at(2, 1) != 2 || channel_at(0, 1) != 0 ||
	    channel_at(1, 0) != 3)
		return 1;
	if (convert_rgbir_yuyv(raw, 8, 8, 16, out, sizeof(out)))
		return 2;
	if (convert_rgbir_yuyv(raw, 8, 8, 15, out, sizeof(out)) == 0)
		return 3;
	if (convert_rgbir_yuyv(raw, 8, 8, 16, out, OUT_FRAME_BYTES - 1) == 0)
		return 4;
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
			default: full[(size_t)y * width + x] = 900; break;
			}
		}
	if (convert_rgbir_yuyv(full, width, height, stride, out, sizeof(out)) ||
	    out[0] < 40 || out[0] > 220) {
		free(full);
		return 6;
	}
	free(full);
	puts("IPU6_LOOPBACK_TEST_PASS");
	return 0;
}
