#include <stdarg.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <poll.h>

static int fake_ioctl(int fd, unsigned long request, ...);
static void *fake_mmap(void *p, size_t n, int prot, int flags, int fd, off_t off);
static int fake_munmap(void *p, size_t n);
static int fake_poll(struct pollfd *p, nfds_t n, int ms);
#define ioctl fake_ioctl
#define mmap fake_mmap
#define munmap fake_munmap
#define poll fake_poll
#define CAMERA_CAPTURE_NO_MAIN
#include "camera_capture_probe.c"
#undef CAMERA_CAPTURE_NO_MAIN
#undef ioctl
#undef mmap
#undef munmap
#undef poll

static int fail_stream, fail_release, poll_result = 1, released, stopped, unmapped, error_frame;
static int queued, dequeued, selection_sets, short_frame, restore_mode;
static int fail_requeue, fail_stop;
static struct v4l2_subdev_format restored;
static char storage[3][4096];

static int fake_ioctl(int fd, unsigned long request, ...)
{
	va_list ap;
	void *arg;
	(void)fd;
	va_start(ap, request);
	arg = va_arg(ap, void *);
	va_end(ap);
	if (request == VIDIOC_REQBUFS) {
		struct v4l2_requestbuffers *r = arg;
		if (!r->count) {
			released++;
			if (fail_release) { errno = EIO; return -1; }
		}
		else r->count = 3;
	} else if (request == VIDIOC_QUERYBUF) {
		struct v4l2_buffer *b = arg;
		b->length = 4096;
		b->m.offset = b->index;
	} else if (request == VIDIOC_QBUF) {
		queued++;
		if (fail_requeue && queued > 3) { errno = EIO; return -1; }
		return 0;
	} else if (request == VIDIOC_STREAMON) {
		if (fail_stream) { errno = EIO; return -1; }
	} else if (request == VIDIOC_STREAMOFF) {
		stopped++;
		if (fail_stop) { errno = EIO; return -1; }
	} else if (request == VIDIOC_DQBUF) {
		struct v4l2_buffer *b = arg;
		b->index = 1;
		b->bytesused = short_frame ? 2047 : 2048;
		b->flags = error_frame ? V4L2_BUF_FLAG_ERROR : 0;
		b->sequence = dequeued++;
	} else if (request == VIDIOC_SUBDEV_S_FMT && restore_mode) {
		restored = *(struct v4l2_subdev_format *)arg;
	} else if (request == VIDIOC_SUBDEV_G_FMT && restore_mode) {
		struct v4l2_subdev_format *f = arg;
		f->format = restored.format;
	} else if (request == VIDIOC_SUBDEV_S_SELECTION && restore_mode) {
		selection_sets++;
	} else {
		abort();
	}
	return 0;
}

static void *fake_mmap(void *p, size_t n, int prot, int flags, int fd, off_t off)
{
	(void)p; (void)n; (void)prot; (void)flags; (void)fd;
	return storage[off];
}

static int fake_munmap(void *p, size_t n)
{
	(void)p; (void)n;
	unmapped++;
	return 0;
}

static int fake_poll(struct pollfd *p, nfds_t n, int ms)
{
	(void)p;
	if (n != 1 || ms != 5000) abort();
	return poll_result;
}

int main(void)
{
	(void)probe;
	struct media_v2_entity entities[] = {
		{ .id = 1, .name = "ov5675 1-0036" },
		{ .id = 2, .name = "Intel IPU6 CSI2 6" },
		{ .id = 3, .name = "Intel IPU6 ISYS Capture 48" }
	};
	struct media_v2_pad pads[] = {
		{ .id = 11, .entity_id = 1, .flags = MEDIA_PAD_FL_SOURCE },
		{ .id = 12, .entity_id = 2, .flags = MEDIA_PAD_FL_SINK },
		{ .id = 13, .entity_id = 2, .index = 1, .flags = MEDIA_PAD_FL_SOURCE },
		{ .id = 14, .entity_id = 3, .flags = MEDIA_PAD_FL_SINK }
	};
	struct media_v2_link links[] = {
		{ .source_id = 11, .sink_id = 12, .flags = MEDIA_LNK_FL_ENABLED | MEDIA_LNK_FL_IMMUTABLE },
		{ .source_id = 13, .sink_id = 14 }
	};
	struct graph g = { .entities = entities, .pads = pads, .links = links };
	struct media_v2_topology t = { .num_entities = 3, .num_pads = 4, .num_links = 2 };
	struct path path;
	if (find_path(&g, &t, "ov5675", &path) || path.csi != &entities[1] ||
	    path.video != &entities[2] || path.csi_source != &pads[2]) return 1;
	links[1].flags = MEDIA_LNK_FL_ENABLED;
	if (!find_path(&g, &t, "ov5675", &path)) return 2;
	links[1].flags = 0;
	if (!find_path(&g, &t, "ov8856", &path)) return 3;
	if (pixel_format(MEDIA_BUS_FMT_SGRBG10_1X10) != V4L2_PIX_FMT_SGRBG10 ||
	    pixel_format(MEDIA_BUS_FMT_SBGGR10_1X10) != V4L2_PIX_FMT_SBGGR10 ||
	    pixel_format(0) != 0) return 4;
	struct v4l2_format fmt = { .type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
		.fmt.pix = { .width = 32, .height = 32, .bytesperline = 64, .sizeimage = 2112 } };
	if (capture(0, &fmt) || stopped != 1 || released != 1 || unmapped != 3 ||
	    queued != 6 || dequeued != 3) return 5;
	fail_stream = 1; stopped = released = unmapped = 0;
	if (!capture(0, &fmt) || stopped || released != 1 || unmapped != 3) return 6;
	fail_stream = 0; poll_result = 0; stopped = released = unmapped = 0;
	if (!capture(0, &fmt) || stopped != 1 || released != 1 || unmapped != 3) return 7;
	poll_result = 1; error_frame = 1; stopped = released = unmapped = 0;
	if (!capture(0, &fmt) || stopped != 1 || released != 1 || unmapped != 3) return 8;
	error_frame = 0; fail_release = 1;
	if (capture(0, &fmt) != -2) return 9;
	fail_release = 0; short_frame = 1;
	if (capture(0, &fmt) != -1) return 10;
	short_frame = 0; fmt.fmt.pix.bytesperline = 63;
	if (capture(0, &fmt) != -1) return 11;
	fmt.fmt.pix.bytesperline = 64; fmt.fmt.pix.sizeimage = 2047;
	if (capture(0, &fmt) != -1) return 12;
	struct v4l2_subdev_format sink = { .format = { .width = 4096, .height = 3072,
		.code = MEDIA_BUS_FMT_SGRBG10_1X10 } };
	struct v4l2_subdev_selection crop = { .r = { .width = 0, .height = 0 } };
	restore_mode = 1;
	if (restore_csi(0, 0, 1, &sink, &sink, &crop) != -2 || selection_sets) return 13;
	crop.r.width = 4096; crop.r.height = 3072;
	if (restore_csi(0, 0, 1, &sink, &sink, &crop) || selection_sets != 1) return 14;
	restore_mode = 0; fmt.fmt.pix.sizeimage = 2112;
	fail_requeue = 1; queued = stopped = released = unmapped = 0;
	if (capture(0, &fmt) != -1 || queued != 4 || stopped != 1 ||
	    released != 1 || unmapped != 3) return 15;
	fail_requeue = 0; fail_stop = 1; stopped = released = unmapped = 0;
	if (capture(0, &fmt) != -2 || stopped != 1 || released != 1 || unmapped != 3) return 16;
	puts("CAMERA_CAPTURE_PROBE_TEST_PASS");
	return 0;
}
