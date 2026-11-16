/* Bounded, opt-in IPU6 raw capture diagnostic. No frame data is saved. */
#define main camera_media_graph_unused_main
#include "camera_media_graph.c"
#undef main

#include <linux/media-bus-format.h>
#include <linux/v4l2-subdev.h>
#include <linux/videodev2.h>
#include <poll.h>
#include <stdint.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>

static const struct media_v2_pad *pad(const struct graph *g,
				      const struct media_v2_topology *t, __u32 id)
{
	for (__u32 i = 0; i < t->num_pads; i++)
		if (g->pads[i].id == id)
			return &g->pads[i];
	return NULL;
}

static const struct media_v2_entity *entity(const struct graph *g,
					   const struct media_v2_topology *t, __u32 id)
{
	for (__u32 i = 0; i < t->num_entities; i++)
		if (g->entities[i].id == id)
			return &g->entities[i];
	return NULL;
}

static int device_path(const struct graph *g, const struct media_v2_topology *t,
		       __u32 id, char *path, size_t size)
{
	for (__u32 i = 0; i < t->num_links; i++) {
		const struct media_v2_link *link = &g->links[i];
		if ((link->flags & MEDIA_LNK_FL_LINK_TYPE) != MEDIA_LNK_FL_INTERFACE_LINK ||
		    link->sink_id != id)
			continue;
		for (__u32 j = 0; j < t->num_interfaces; j++) {
			const struct media_v2_interface *in = &g->interfaces[j];
			if (in->id != link->source_id)
				continue;
			char sys[80], line[256];
			snprintf(sys, sizeof(sys), "/sys/dev/char/%u:%u/uevent",
				 in->devnode.major, in->devnode.minor);
			FILE *f = fopen(sys, "r");
			if (!f) {
				perror(sys);
				return -1;
			}
			while (fgets(line, sizeof(line), f)) {
				if (strncmp(line, "DEVNAME=", 8))
					continue;
				char *name = line + 8;
				name[strcspn(name, "\r\n")] = 0;
				if (strchr(name, '/') || !*name ||
				    snprintf(path, size, "/dev/%s", name) >= (int)size) {
					fclose(f);
					fprintf(stderr, "invalid device name\n");
					return -1;
				}
				fclose(f);
				struct stat st;
				if (stat(path, &st) || !S_ISCHR(st.st_mode) ||
				    major(st.st_rdev) != in->devnode.major ||
				    minor(st.st_rdev) != in->devnode.minor) {
					fprintf(stderr, "device node mismatch: %s\n", path);
					return -1;
				}
				return 0;
			}
			fclose(f);
			fprintf(stderr, "no DEVNAME in %s\n", sys);
			return -1;
		}
	}
	fprintf(stderr, "missing interface for entity %u\n", id);
	return -1;
}

struct path {
	const struct media_v2_entity *sensor, *csi, *video;
	const struct media_v2_pad *sensor_pad, *csi_sink, *csi_source, *video_pad;
	const struct media_v2_link *output;
};

static int find_path(const struct graph *g, const struct media_v2_topology *t,
		     const char *sensor_name, struct path *p)
{
	unsigned int count = 0;
	memset(p, 0, sizeof(*p));
	for (__u32 i = 0; i < t->num_entities; i++) {
		const struct media_v2_entity *e = &g->entities[i];
		size_t n = strlen(sensor_name);
		if (!strncmp(e->name, sensor_name, n) && e->name[n] == ' ') {
			p->sensor = e;
			count++;
		}
	}
	if (count != 1) {
		fprintf(stderr, "%s: expected one sensor, found %u\n", sensor_name, count);
		return -1;
	}
	count = 0;
	for (__u32 i = 0; i < t->num_links; i++) {
		const struct media_v2_link *l = &g->links[i];
		const struct media_v2_pad *src = pad(g, t, l->source_id);
		const struct media_v2_pad *dst = pad(g, t, l->sink_id);
		if ((l->flags & MEDIA_LNK_FL_LINK_TYPE) || !src || !dst ||
		    src->entity_id != p->sensor->id || !(l->flags & MEDIA_LNK_FL_ENABLED))
			continue;
		const struct media_v2_entity *csi = entity(g, t, dst->entity_id);
		if (!csi || strncmp(csi->name, "Intel IPU6 CSI2 ", 16))
			continue;
		p->sensor_pad = src;
		p->csi_sink = dst;
		p->csi = csi;
		count++;
	}
	if (count != 1) {
		fprintf(stderr, "%s: expected one enabled CSI link, found %u\n", sensor_name, count);
		return -1;
	}
	for (__u32 i = 0; i < t->num_links; i++) {
		const struct media_v2_link *l = &g->links[i];
		const struct media_v2_pad *src = pad(g, t, l->source_id);
		const struct media_v2_pad *dst = pad(g, t, l->sink_id);
		if ((l->flags & MEDIA_LNK_FL_LINK_TYPE) || !src || !dst ||
		    src->entity_id != p->csi->id)
			continue;
		const struct media_v2_entity *v = entity(g, t, dst->entity_id);
		if (!v || strncmp(v->name, "Intel IPU6 ISYS Capture ", 24))
			continue;
		if (l->flags & MEDIA_LNK_FL_ENABLED) {
			fprintf(stderr, "%s: CSI output already in use; refusing changes\n", sensor_name);
			return -1;
		}
		if (!p->output && !(l->flags & MEDIA_LNK_FL_IMMUTABLE)) {
			p->output = l;
			p->csi_source = src;
			p->video_pad = dst;
			p->video = v;
		}
	}
	if (!p->output) {
		fprintf(stderr, "%s: no configurable capture link\n", sensor_name);
		return -1;
	}
	for (__u32 i = 0; i < t->num_links; i++) {
		const struct media_v2_link *l = &g->links[i];
		if (!(l->flags & MEDIA_LNK_FL_LINK_TYPE) &&
		    l->sink_id == p->video_pad->id && (l->flags & MEDIA_LNK_FL_ENABLED)) {
			fprintf(stderr, "%s: capture node already linked; refusing changes\n", sensor_name);
			return -1;
		}
	}
	return 0;
}

static int setup_link(int fd, const struct path *p, __u32 flags)
{
	struct media_link_desc d = { 0 };
	d.source.entity = p->csi->id;
	d.source.index = p->csi_source->index;
	d.source.flags = p->csi_source->flags;
	d.sink.entity = p->video->id;
	d.sink.index = p->video_pad->index;
	d.sink.flags = p->video_pad->flags;
	d.flags = flags;
	if (ioctl(fd, MEDIA_IOC_SETUP_LINK, &d)) {
		perror("MEDIA_IOC_SETUP_LINK");
		return -1;
	}
	return 0;
}

static int subformat(int fd, unsigned int pad_index, struct v4l2_subdev_format *fmt,
		     unsigned long request)
{
	fmt->which = V4L2_SUBDEV_FORMAT_ACTIVE;
	fmt->pad = pad_index;
	if (ioctl(fd, request, fmt)) {
		perror(request == VIDIOC_SUBDEV_G_FMT ? "VIDIOC_SUBDEV_G_FMT" : "VIDIOC_SUBDEV_S_FMT");
		return -1;
	}
	printf("subdev pad=%u code=0x%x size=%ux%u\n", pad_index,
	       fmt->format.code, fmt->format.width, fmt->format.height);
	return 0;
}

static __u32 pixel_format(__u32 code)
{
	switch (code) {
	case MEDIA_BUS_FMT_SGRBG10_1X10: return V4L2_PIX_FMT_SGRBG10;
	case MEDIA_BUS_FMT_SBGGR10_1X10: return V4L2_PIX_FMT_SBGGR10;
	default: return 0;
	}
}

static int same_subformat(const struct v4l2_subdev_format *a,
			  const struct v4l2_subdev_format *b)
{
	return a->format.code == b->format.code &&
	       a->format.width == b->format.width &&
	       a->format.height == b->format.height &&
	       a->format.field == b->format.field;
}

static int restore_csi(int fd, unsigned int sink_pad, unsigned int source_pad,
		       const struct v4l2_subdev_format *sink,
		       const struct v4l2_subdev_format *source,
		       const struct v4l2_subdev_selection *saved_crop)
{
	struct v4l2_subdev_format set = *sink;
	struct v4l2_subdev_selection crop = *saved_crop;
	int result = 0;
	if (subformat(fd, sink_pad, &set, VIDIOC_SUBDEV_S_FMT) || !same_subformat(&set, sink)) {
		fprintf(stderr, "CSI sink restore failed or was adjusted\n"); result = -2;
	}
	if (!crop.r.width || !crop.r.height) {
		fprintf(stderr, "original CSI crop was empty and cannot be restored; current state may differ; reboot required\n");
		result = -2;
	} else if (ioctl(fd, VIDIOC_SUBDEV_S_SELECTION, &crop)) {
		perror("restore CSI crop"); result = -2;
	} else if (memcmp(&crop.r, &saved_crop->r, sizeof(crop.r))) {
		fprintf(stderr, "CSI crop restore was adjusted\n"); result = -2;
	}
	memset(&set, 0, sizeof(set));
	if (subformat(fd, source_pad, &set, VIDIOC_SUBDEV_G_FMT) || !same_subformat(&set, source)) {
		fprintf(stderr, "CSI source restore differs from original\n"); result = -2;
	}
	return result;
}

static int capture(int fd, const struct v4l2_format *f)
{
	struct v4l2_requestbuffers req = { .count = 3, .type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
					   .memory = V4L2_MEMORY_MMAP };
	struct { void *p; size_t len; } buffers[3] = { 0 };
	int result = -1, streaming = 0;
	uint64_t payload = (uint64_t)f->fmt.pix.bytesperline * f->fmt.pix.height;
	enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	if (!f->fmt.pix.height || !f->fmt.pix.width ||
	    (uint64_t)f->fmt.pix.width * 2 > f->fmt.pix.bytesperline ||
	    payload > f->fmt.pix.sizeimage) {
		fprintf(stderr, "invalid 10-bit unpacked format stride or size\n");
		return -1;
	}
	if (ioctl(fd, VIDIOC_REQBUFS, &req)) {
		perror("VIDIOC_REQBUFS");
		return -1;
	}
	if (req.count < 2 || req.count > 3) {
		fprintf(stderr, "unexpected buffer count %u\n", req.count);
		goto out;
	}
	for (unsigned int i = 0; i < req.count; i++) {
		struct v4l2_buffer b = { .type = type, .memory = V4L2_MEMORY_MMAP, .index = i };
		if (ioctl(fd, VIDIOC_QUERYBUF, &b)) { perror("VIDIOC_QUERYBUF"); goto out; }
		if (b.length < f->fmt.pix.sizeimage || b.length > 64U * 1024U * 1024U) {
			fprintf(stderr, "unsafe buffer length %u\n", b.length); goto out;
		}
		buffers[i].len = b.length;
		buffers[i].p = mmap(NULL, b.length, PROT_READ | PROT_WRITE, MAP_SHARED, fd, b.m.offset);
		if (buffers[i].p == MAP_FAILED) { buffers[i].p = NULL; perror("mmap"); goto out; }
		if (ioctl(fd, VIDIOC_QBUF, &b)) { perror("VIDIOC_QBUF"); goto out; }
	}
	printf("VIDIOC_STREAMON\n");
	fflush(stdout);
	if (ioctl(fd, VIDIOC_STREAMON, &type)) { perror("VIDIOC_STREAMON"); goto out; }
	streaming = 1;
	for (unsigned int i = 0; i < 3; i++) {
		struct pollfd pollfd = { .fd = fd, .events = POLLIN | POLLPRI };
		int r = poll(&pollfd, 1, 5000);
		if (r <= 0) {
			if (!r) fprintf(stderr, "capture timed out after 5 seconds\n");
			else perror("poll");
			goto out;
		}
		struct v4l2_buffer b = { .type = type, .memory = V4L2_MEMORY_MMAP };
		if (ioctl(fd, VIDIOC_DQBUF, &b)) { perror("VIDIOC_DQBUF"); goto out; }
		printf("frame index=%u bytes=%u flags=0x%x sequence=%u payload=%llu allocation=%u\n",
		       b.index, b.bytesused, b.flags, b.sequence,
		       (unsigned long long)payload, f->fmt.pix.sizeimage);
		if (b.index >= req.count || b.bytesused < payload || b.bytesused > buffers[b.index].len ||
		    (b.flags & V4L2_BUF_FLAG_ERROR)) {
			fprintf(stderr, "short, invalid or error-marked frame\n");
			goto out;
		}
		puts("FRAME_CAPTURED (raw bytes; image content not verified or saved)");
		if (ioctl(fd, VIDIOC_QBUF, &b)) { perror("VIDIOC_QBUF requeue"); goto out; }
	}
	result = 0;
out:
	if (streaming) {
		puts("VIDIOC_STREAMOFF begin");
		if (ioctl(fd, VIDIOC_STREAMOFF, &type)) { perror("VIDIOC_STREAMOFF"); result = -2; }
		else puts("VIDIOC_STREAMOFF returned; check kernel log for firmware stop/close errors");
	}
	for (unsigned int i = 0; i < 3; i++)
		if (buffers[i].p && munmap(buffers[i].p, buffers[i].len)) { perror("munmap"); result = -2; }
	req.count = 0;
	if (ioctl(fd, VIDIOC_REQBUFS, &req)) { perror("VIDIOC_REQBUFS release"); result = -2; }
	return result;
}

static int probe(int mfd, const struct graph *g, const struct media_v2_topology *t,
		 const char *name)
{
	struct path p;
	char sname[128], cname[128], vname[128];
	int sfd = -1, cfd = -1, vfd = -1, linked = 0, changed = 0, video_changed = 0, result = -1;
	struct v4l2_subdev_format sensor = { 0 }, sink = { 0 }, source = { 0 }, set;
	struct v4l2_format original = { .type = V4L2_BUF_TYPE_VIDEO_CAPTURE }, format;
	struct v4l2_subdev_selection crop = {
		.which = V4L2_SUBDEV_FORMAT_ACTIVE, .target = V4L2_SEL_TGT_CROP
	}, saved_crop;
	if (find_path(g, t, name, &p) ||
	    device_path(g, t, p.sensor->id, sname, sizeof(sname)) ||
	    device_path(g, t, p.csi->id, cname, sizeof(cname)) ||
	    device_path(g, t, p.video->id, vname, sizeof(vname)))
		return -1;
	printf("path %s (%s) -> %s (%s) -> %s (%s) link=%u flags=0x%x\n",
	       p.sensor->name, sname, p.csi->name, cname, p.video->name, vname,
	       p.output->id, p.output->flags);
	sfd = open(sname, O_RDWR | O_CLOEXEC);
	cfd = open(cname, O_RDWR | O_CLOEXEC);
	vfd = open(vname, O_RDWR | O_NONBLOCK | O_CLOEXEC);
	if (sfd < 0 || cfd < 0 || vfd < 0) { perror("open camera path"); goto out; }
	if (subformat(sfd, p.sensor_pad->index, &sensor, VIDIOC_SUBDEV_G_FMT) ||
	    subformat(cfd, p.csi_sink->index, &sink, VIDIOC_SUBDEV_G_FMT) ||
	    subformat(cfd, p.csi_source->index, &source, VIDIOC_SUBDEV_G_FMT))
		goto out;
	crop.pad = p.csi_source->index;
	if (ioctl(cfd, VIDIOC_SUBDEV_G_SELECTION, &crop)) {
		perror("VIDIOC_SUBDEV_G_SELECTION crop");
		goto out;
	}
	saved_crop = crop;
	printf("original CSI crop left=%d top=%d size=%ux%u\n",
	       crop.r.left, crop.r.top, crop.r.width, crop.r.height);
	__u32 fourcc = pixel_format(sensor.format.code);
	if (!fourcc) { fprintf(stderr, "unsupported sensor bus code 0x%x\n", sensor.format.code); goto out; }
	if (ioctl(vfd, VIDIOC_G_FMT, &original)) { perror("VIDIOC_G_FMT"); goto out; }
	if (setup_link(mfd, &p, p.output->flags | MEDIA_LNK_FL_ENABLED)) goto out;
	linked = 1;
	set = sensor;
	changed = 1;
	if (subformat(cfd, p.csi_sink->index, &set, VIDIOC_SUBDEV_S_FMT)) goto out;
	if (!same_subformat(&set, &sensor)) {
		fprintf(stderr, "CSI sink format differs from sensor; refusing stream\n");
		goto out;
	}
	memset(&set, 0, sizeof(set));
	if (subformat(cfd, p.csi_source->index, &set, VIDIOC_SUBDEV_G_FMT)) goto out;
	if (!same_subformat(&set, &sensor)) {
		fprintf(stderr, "CSI source format differs from sensor; refusing stream\n");
		goto out;
	}
	memset(&format, 0, sizeof(format));
	format.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	format.fmt.pix.width = sensor.format.width;
	format.fmt.pix.height = sensor.format.height;
	format.fmt.pix.pixelformat = fourcc;
	format.fmt.pix.field = V4L2_FIELD_NONE;
	video_changed = 1;
	if (ioctl(vfd, VIDIOC_S_FMT, &format)) { perror("VIDIOC_S_FMT"); goto out; }
	printf("capture format fourcc=0x%x size=%ux%u stride=%u image=%u\n",
	       format.fmt.pix.pixelformat, format.fmt.pix.width, format.fmt.pix.height,
	       format.fmt.pix.bytesperline, format.fmt.pix.sizeimage);
	if (format.fmt.pix.pixelformat != fourcc || format.fmt.pix.width != sensor.format.width ||
	    format.fmt.pix.height != sensor.format.height) {
		fprintf(stderr, "capture format differs from sensor; refusing stream\n");
		goto out;
	}
	result = capture(vfd, &format);
out:
	if (video_changed) {
		format = original;
		if (ioctl(vfd, VIDIOC_S_FMT, &format)) { perror("restore VIDIOC_S_FMT"); result = -2; }
		else if (format.fmt.pix.pixelformat != original.fmt.pix.pixelformat ||
			 format.fmt.pix.width != original.fmt.pix.width ||
			 format.fmt.pix.height != original.fmt.pix.height) {
			fprintf(stderr, "video format restore was adjusted\n"); result = -2;
		}
	}
	if (changed) {
		if (restore_csi(cfd, p.csi_sink->index, p.csi_source->index,
				&sink, &source, &saved_crop)) result = -2;
	}
	if (linked && setup_link(mfd, &p, p.output->flags)) { fprintf(stderr, "link restore failed\n"); result = -2; }
	if (sfd >= 0) close(sfd);
	if (cfd >= 0) close(cfd);
	if (vfd >= 0) close(vfd);
	printf("RESULT %s %s\n", name, result ? "FAIL" : "FRAME_CAPTURED");
	return result;
}

#ifndef CAMERA_CAPTURE_NO_MAIN
int main(int argc, char **argv)
{
	struct graph g = { 0 };
	struct media_v2_topology t;
	struct media_device_info info = { 0 };
	if (argc != 3 || (strcmp(argv[2], "ov5675") && strcmp(argv[2], "ov8856"))) {
		fprintf(stderr, "usage: %s /dev/mediaN ov5675|ov8856\n", argv[0]);
		return 2;
	}
	setvbuf(stdout, NULL, _IOLBF, 0);
	int fd = open(argv[1], O_RDWR | O_CLOEXEC);
	if (fd < 0) { perror("open media"); return 1; }
	if (ioctl(fd, MEDIA_IOC_DEVICE_INFO, &info) ||
	    strncmp(info.driver, "intel-ipu6", sizeof(info.driver)) ||
	    read_graph(fd, &g, &t)) {
		fprintf(stderr, "not a readable Intel IPU6 media graph\n");
		close(fd);
		return 1;
	}
	int result = probe(fd, &g, &t, argv[2]);
	free_graph(&g);
	close(fd);
	return result == -2 ? 3 : result != 0;
}
#endif
