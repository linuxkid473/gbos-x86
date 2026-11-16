#define _GNU_SOURCE
#define CAMERA_CAPTURE_NO_MAIN
#include "../tools/camera_capture_probe.c"

#include <signal.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#define OUT_WIDTH 640
#define OUT_HEIGHT 480
#define OUT_FOURCC V4L2_PIX_FMT_YUYV
#define OUT_FRAME_BYTES ((size_t)OUT_WIDTH * OUT_HEIGHT * 2)
#define BLACK_LEVEL 64
#define MAX_FRAME_BYTES (64U * 1024U * 1024U)

static volatile sig_atomic_t running = 1;

struct bridge_buffer {
	void *data;
	size_t length;
};

static void stop_bridge(int sig)
{
	(void)sig;
	running = 0;
}

static int channel_at(unsigned int x, unsigned int y)
{
	static const unsigned char mosaic[4][4] = {
		{ 1, 3, 1, 3 },
		{ 0, 1, 2, 1 },
		{ 1, 3, 1, 3 },
		{ 2, 1, 0, 1 }
	};
	return mosaic[y & 3][x & 3];
}

static unsigned int raw_at(const uint16_t *raw, unsigned int width,
			   unsigned int height, unsigned int stride,
			   int x, int y)
{
	if (x < 0) x = 0;
	if (y < 0) y = 0;
	if ((unsigned int)x >= width) x = (int)width - 1;
	if ((unsigned int)y >= height) y = (int)height - 1;
	unsigned int value = raw[(size_t)y * (stride / 2) + (unsigned int)x] & 1023;
	value = value > BLACK_LEVEL ? value - BLACK_LEVEL : 0;
	return (value * 255U + (1023U - BLACK_LEVEL) / 2) /
		(1023U - BLACK_LEVEL);
}

static unsigned char clamp8(int value)
{
	if (value < 0) return 0;
	if (value > 255) return 255;
	return (unsigned char)value;
}

static void rgb_at(const uint16_t *raw, unsigned int width, unsigned int height,
		   unsigned int stride, unsigned int x, unsigned int y,
		   int rgb[3])
{
	unsigned int rsum = 0, gsum = 0, bsum = 0;
	unsigned int rn = 0, gn = 0, bn = 0;
	for (int dy = -1; dy <= 1; dy++) {
		for (int dx = -1; dx <= 1; dx++) {
			int sx = (int)x + dx, sy = (int)y + dy;
			if (sx < 0 || sy < 0 || (unsigned int)sx >= width ||
			    (unsigned int)sy >= height)
				continue;
			unsigned int value = raw_at(raw, width, height, stride, sx, sy);
			switch (channel_at((unsigned int)sx, (unsigned int)sy)) {
			case 0: rsum += value; rn++; break;
			case 1: gsum += value; gn++; break;
			case 2: bsum += value; bn++; break;
			default: break;
			}
		}
	}
	rgb[0] = rn ? (int)(rsum / rn) : 0;
	rgb[1] = gn ? (int)(gsum / gn) : 0;
	rgb[2] = bn ? (int)(bsum / bn) : 0;
}

static int convert_rgbir_yuyv(const uint16_t *raw, unsigned int width,
			      unsigned int height, unsigned int stride,
			      unsigned char *out, size_t out_size)
{
	if (!raw || !out || width < 8 || height < 8 ||
	    (width & 1) || (height & 1) ||
	    (uint64_t)stride < (uint64_t)width * 2 ||
	    (uint64_t)stride * height > MAX_FRAME_BYTES ||
	    out_size < OUT_FRAME_BYTES)
		return -1;

	for (unsigned int oy = 0; oy < OUT_HEIGHT; oy++) {
		unsigned int sy = (unsigned int)(((uint64_t)oy * height) / OUT_HEIGHT);
		unsigned char *dst = out + (size_t)oy * OUT_WIDTH * 2;
		for (unsigned int ox = 0; ox < OUT_WIDTH; ox += 2) {
			unsigned int sx0 = (unsigned int)(((uint64_t)ox * width) / OUT_WIDTH);
			unsigned int sx1 = (unsigned int)(((uint64_t)(ox + 1) * width) / OUT_WIDTH);
			int a[3], b[3];
			rgb_at(raw, width, height, stride, sx0, sy, a);
			rgb_at(raw, width, height, stride, sx1, sy, b);
			int y0 = (77 * a[0] + 150 * a[1] + 29 * a[2] + 128) >> 8;
			int y1 = (77 * b[0] + 150 * b[1] + 29 * b[2] + 128) >> 8;
			int r = (a[0] + b[0]) / 2;
			int g = (a[1] + b[1]) / 2;
			int bl = (a[2] + b[2]) / 2;
			dst[ox * 2] = clamp8(y0);
			dst[ox * 2 + 1] = clamp8(128 + ((-43 * r - 85 * g + 128 * bl) >> 8));
			dst[ox * 2 + 2] = clamp8(y1);
			dst[ox * 2 + 3] = clamp8(128 + ((128 * r - 107 * g - 21 * bl) >> 8));
		}
	}
	return 0;
}

static int capture_to_loopback(const char *media_dev, const char *out_dev)
{
	struct graph graph = { 0 };
	struct media_v2_topology topology;
	struct media_device_info info = { 0 };
	struct path path;
	char sensor_path[128], csi_path[128], capture_path[128];
	int mfd = -1, sfd = -1, cfd = -1, vfd = -1, outfd = -1;
	int linked = 0, streaming = 0, result = 1;
	struct bridge_buffer buffers[3] = { 0 };
	struct v4l2_subdev_format sensor = { 0 }, saved_sink = { 0 }, set;
	struct v4l2_format video = { .type = V4L2_BUF_TYPE_VIDEO_CAPTURE };
	struct v4l2_format saved_video = { .type = V4L2_BUF_TYPE_VIDEO_CAPTURE };
	struct v4l2_requestbuffers request = {
		.count = 3, .type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
		.memory = V4L2_MEMORY_MMAP
	};
	unsigned int buffer_count = 0;
	unsigned char *output = NULL;
	enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

	mfd = open(media_dev, O_RDWR | O_CLOEXEC);
	if (mfd < 0 || ioctl(mfd, MEDIA_IOC_DEVICE_INFO, &info) ||
	    strncmp(info.driver, "intel-ipu6", sizeof(info.driver)) ||
	    read_graph(mfd, &graph, &topology) ||
	    find_path(&graph, &topology, "ov5675", &path) ||
	    device_path(&graph, &topology, path.sensor->id, sensor_path, sizeof(sensor_path)) ||
	    device_path(&graph, &topology, path.csi->id, csi_path, sizeof(csi_path)) ||
	    device_path(&graph, &topology, path.video->id, capture_path, sizeof(capture_path))) {
		fprintf(stderr, "cannot discover a unique OV5675 IPU6 capture path\n");
		goto done;
	}
	printf("IPU6 path: %s -> %s -> %s; output=%s\n",
	       sensor_path, csi_path, capture_path, out_dev);
	sfd = open(sensor_path, O_RDWR | O_CLOEXEC);
	cfd = open(csi_path, O_RDWR | O_CLOEXEC);
	vfd = open(capture_path, O_RDWR | O_NONBLOCK | O_CLOEXEC);
	outfd = open(out_dev, O_RDWR | O_CLOEXEC);
	if (sfd < 0 || cfd < 0 || vfd < 0 || outfd < 0) {
		perror("open camera or loopback node");
		goto done;
	}
	if (subformat(sfd, path.sensor_pad->index, &sensor, VIDIOC_SUBDEV_G_FMT) ||
	    subformat(cfd, path.csi_sink->index, &saved_sink, VIDIOC_SUBDEV_G_FMT) ||
	    ioctl(vfd, VIDIOC_G_FMT, &saved_video)) {
		perror("read existing camera formats");
		goto done;
	}
	__u32 fourcc = pixel_format(sensor.format.code);
	if (!fourcc || setup_link(mfd, &path, path.output->flags | MEDIA_LNK_FL_ENABLED))
		goto done;
	linked = 1;
	set = sensor;
	if (subformat(cfd, path.csi_sink->index, &set, VIDIOC_SUBDEV_S_FMT) ||
	    !same_subformat(&set, &sensor))
		goto done;
	memset(&set, 0, sizeof(set));
	if (subformat(cfd, path.csi_source->index, &set, VIDIOC_SUBDEV_G_FMT) ||
	    !same_subformat(&set, &sensor))
		goto done;
	video.fmt.pix.width = sensor.format.width;
	video.fmt.pix.height = sensor.format.height;
	video.fmt.pix.pixelformat = fourcc;
	video.fmt.pix.field = V4L2_FIELD_NONE;
	if (ioctl(vfd, VIDIOC_S_FMT, &video) ||
	    video.fmt.pix.pixelformat != fourcc ||
	    video.fmt.pix.width != sensor.format.width ||
	    video.fmt.pix.height != sensor.format.height ||
	    (uint64_t)video.fmt.pix.bytesperline < (uint64_t)video.fmt.pix.width * 2 ||
	    (uint64_t)video.fmt.pix.sizeimage <
		(uint64_t)video.fmt.pix.bytesperline * video.fmt.pix.height) {
		fprintf(stderr, "IPU6 rejected or adjusted raw capture format\n");
		goto done;
	}
	printf("raw format=0x%x %ux%u stride=%u image=%u\n", fourcc,
	       video.fmt.pix.width, video.fmt.pix.height,
	       video.fmt.pix.bytesperline, video.fmt.pix.sizeimage);
	if (video.fmt.pix.sizeimage > MAX_FRAME_BYTES)
		goto done;
	if (ioctl(vfd, VIDIOC_REQBUFS, &request)) {
		perror("VIDIOC_REQBUFS");
		goto done;
	}
	if (request.count < 2 || request.count > 3) {
		fprintf(stderr, "unexpected IPU6 buffer count %u\n", request.count);
		goto done;
	}
	buffer_count = request.count;
	for (unsigned int i = 0; i < buffer_count; i++) {
		struct v4l2_buffer b = { .type = type, .memory = V4L2_MEMORY_MMAP, .index = i };
		if (ioctl(vfd, VIDIOC_QUERYBUF, &b) || b.length < video.fmt.pix.sizeimage ||
		    b.length > MAX_FRAME_BYTES)
			goto done;
		buffers[i].length = b.length;
		buffers[i].data = mmap(NULL, b.length, PROT_READ | PROT_WRITE,
				       MAP_SHARED, vfd, b.m.offset);
		if (buffers[i].data == MAP_FAILED) {
			buffers[i].data = NULL;
			goto done;
		}
		if (ioctl(vfd, VIDIOC_QBUF, &b))
			goto done;
	}
	if (ioctl(vfd, VIDIOC_STREAMON, &type)) {
		perror("start IPU6 capture");
		goto done;
	}
	streaming = 1;
	struct v4l2_format outfmt = { .type = V4L2_BUF_TYPE_VIDEO_OUTPUT };
	outfmt.fmt.pix.width = OUT_WIDTH;
	outfmt.fmt.pix.height = OUT_HEIGHT;
	outfmt.fmt.pix.pixelformat = OUT_FOURCC;
	outfmt.fmt.pix.field = V4L2_FIELD_NONE;
	outfmt.fmt.pix.bytesperline = OUT_WIDTH * 2;
	outfmt.fmt.pix.sizeimage = OUT_FRAME_BYTES;
	outfmt.fmt.pix.colorspace = V4L2_COLORSPACE_SRGB;
	if (ioctl(outfd, VIDIOC_S_FMT, &outfmt) ||
	    outfmt.fmt.pix.pixelformat != OUT_FOURCC ||
	    outfmt.fmt.pix.width != OUT_WIDTH || outfmt.fmt.pix.height != OUT_HEIGHT ||
	    outfmt.fmt.pix.bytesperline != OUT_WIDTH * 2 ||
	    outfmt.fmt.pix.sizeimage < OUT_FRAME_BYTES) {
		perror("configure V4L2 loopback output");
		goto done;
	}
	output = malloc(OUT_FRAME_BYTES);
	if (!output)
		goto done;
	for (size_t i = 0; i < OUT_FRAME_BYTES; i += 4) {
		output[i] = 16;
		output[i + 1] = 128;
		output[i + 2] = 16;
		output[i + 3] = 128;
	}
	if (write(outfd, output, OUT_FRAME_BYTES) != (ssize_t)OUT_FRAME_BYTES) {
		perror("prime V4L2 loopback");
		goto done;
	}
	signal(SIGINT, stop_bridge);
	signal(SIGTERM, stop_bridge);
	printf("bridge active: %ux%u RGB-IR to %ux%u YUYV; sensor remains streaming until stop\n",
	       video.fmt.pix.width, video.fmt.pix.height, OUT_WIDTH, OUT_HEIGHT);
	result = 0;
	while (running) {
		struct pollfd pfd = { .fd = vfd, .events = POLLIN | POLLPRI };
		int ready = poll(&pfd, 1, 1000);
		if (ready < 0) {
			if (errno == EINTR) continue;
			perror("poll IPU6");
			result = 1;
			break;
		}
		if (!ready)
			continue;
		struct v4l2_buffer b = { .type = type, .memory = V4L2_MEMORY_MMAP };
		if (ioctl(vfd, VIDIOC_DQBUF, &b)) {
			if (errno == EAGAIN || errno == EINTR) continue;
			perror("dequeue IPU6 frame");
			result = 1;
			break;
		}
		if (b.index >= buffer_count ||
		    b.bytesused < (uint64_t)video.fmt.pix.bytesperline * video.fmt.pix.height ||
		    b.bytesused > buffers[b.index].length ||
		    (b.flags & V4L2_BUF_FLAG_ERROR)) {
			fprintf(stderr, "invalid or error-marked IPU6 frame\n");
			result = 1;
			break;
		}
		if (convert_rgbir_yuyv(buffers[b.index].data, video.fmt.pix.width,
				       video.fmt.pix.height, video.fmt.pix.bytesperline,
				       output, OUT_FRAME_BYTES) ||
		    write(outfd, output, OUT_FRAME_BYTES) != (ssize_t)OUT_FRAME_BYTES) {
			fprintf(stderr, "convert or write loopback frame failed: %s\n", strerror(errno));
			result = 1;
			break;
		}
		if (ioctl(vfd, VIDIOC_QBUF, &b)) {
			perror("requeue IPU6 frame");
			result = 1;
			break;
		}
	}
done:
	if (streaming && ioctl(vfd, VIDIOC_STREAMOFF, &type))
		perror("VIDIOC_STREAMOFF");
	for (unsigned int i = 0; i < buffer_count; i++)
		if (buffers[i].data) munmap(buffers[i].data, buffers[i].length);
	if (vfd >= 0) {
		request.count = 0;
		if (ioctl(vfd, VIDIOC_REQBUFS, &request))
			perror("release IPU6 buffers");
		if (saved_video.fmt.pix.width) {
			if (ioctl(vfd, VIDIOC_S_FMT, &saved_video))
				perror("restore IPU6 video format");
		}
	}
	if (cfd >= 0 && saved_sink.format.width) {
		set = saved_sink;
		if (ioctl(cfd, VIDIOC_SUBDEV_S_FMT, &set))
			perror("restore IPU6 CSI sink format");
	}
	if (linked && setup_link(mfd, &path, path.output->flags))
		fprintf(stderr, "could not restore IPU6 media link; reboot required\n");
	free(output);
	if (outfd >= 0) close(outfd);
	if (vfd >= 0) close(vfd);
	if (cfd >= 0) close(cfd);
	if (sfd >= 0) close(sfd);
	if (mfd >= 0) close(mfd);
	free_graph(&graph);
	return result;
}

#ifndef CAMERA_BRIDGE_TEST
int main(int argc, char **argv)
{
	(void)probe;
	if (argc != 3) {
		fprintf(stderr, "usage: %s /dev/mediaN /dev/videoN\n", argv[0]);
		return 2;
	}
	setvbuf(stdout, NULL, _IOLBF, 0);
	return capture_to_loopback(argv[1], argv[2]);
}
#endif
