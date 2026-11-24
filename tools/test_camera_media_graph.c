#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <unistd.h>

static int mock_ioctl(int fd, unsigned long request, ...);
static int mock_open(const char *path, int flags, ...);
static int mock_close(int fd);
#define ioctl mock_ioctl
#define open mock_open
#define close mock_close
#define main camera_media_graph_main
#include "camera_media_graph.c"
#undef main
#undef ioctl
#undef open
#undef close

static int calls;
static int mode;

static int mock_open(const char *path, int flags, ...)
{
	if (strcmp(path, "test-media") || (flags & O_ACCMODE) != O_RDONLY)
		abort();
	return 10;
}

static int mock_close(int fd)
{
	if (fd != 10)
		abort();
	return 0;
}

static int mock_ioctl(int fd, unsigned long request, ...)
{
	va_list ap;
	struct media_v2_topology *t;
	(void)fd;
	if (request == MEDIA_IOC_DEVICE_INFO) {
		struct media_device_info *info;
		va_start(ap, request);
		info = va_arg(ap, struct media_device_info *);
		va_end(ap);
		strcpy(info->driver, "intel-ipu6");
		strcpy(info->model, "ipu6");
		return 0;
	}
	if (request != MEDIA_IOC_G_TOPOLOGY)
		abort();
	va_start(ap, request);
	t = va_arg(ap, struct media_v2_topology *);
	va_end(ap);
	calls++;
	if (!t->ptr_entities) {
		t->topology_version = 1;
		t->num_entities = mode == 2 ? MAX_OBJECTS + 1 : 1;
		t->num_interfaces = 1;
		t->num_pads = 1;
		t->num_links = 1;
		return 0;
	}
	if (mode == 1 && calls == 2) {
		errno = ENOSPC;
		return -1;
	}
	if (mode == 3) {
		t->topology_version++;
		return 0;
	}
	if (mode == 4) {
		errno = EIO;
		return -1;
	}
	struct media_v2_entity *entity = (void *)(uintptr_t)t->ptr_entities;
	struct media_v2_pad *pad = (void *)(uintptr_t)t->ptr_pads;
	struct media_v2_link *link = (void *)(uintptr_t)t->ptr_links;
	entity[0].id = 42;
	strcpy(entity[0].name, "ov5675 1-0036");
	pad[0].id = 43;
	pad[0].entity_id = 42;
	link[0].source_id = 43;
	link[0].sink_id = 44;
	link[0].flags = MEDIA_LNK_FL_ENABLED;
	return 0;
}

int main(void)
{
	struct graph g = { 0 };
	struct media_v2_topology t = { 0 };

	if (read_graph(0, &g, &t) || calls != 2 || t.num_entities != 1 ||
	    g.entities[0].id != 42 || g.pads[0].entity_id != 42)
		return 1;
	free_graph(&g);
	calls = 0;
	mode = 1;
	if (read_graph(0, &g, &t) || calls != 4)
		return 2;
	free_graph(&g);
	calls = 0;
	mode = 2;
	if (!read_graph(0, &g, &t) || calls != 1 || g.entities)
		return 3;
	calls = 0;
	mode = 3;
	if (!read_graph(0, &g, &t) || calls != MAX_ATTEMPTS * 2 || g.entities)
		return 4;
	calls = 0;
	mode = 4;
	if (!read_graph(0, &g, &t) || calls != 2 || g.entities)
		return 5;
	calls = 0;
	mode = 0;
	char *argv[] = { "camera-media-graph", "test-media", NULL };
	if (camera_media_graph_main(2, argv) || calls != 2)
		return 6;
	puts("CAMERA_MEDIA_GRAPH_TEST_PASS");
	return 0;
}
