/*
 * Read-only media-controller topology diagnostic.
 * Build: cc -std=gnu11 -O2 -Wall -Wextra -Werror -static tools/camera_media_graph.c -o usb/installer/bin/camera-media-graph
 * Run: usb/installer/bin/camera-media-graph /dev/media0
 */
#include <errno.h>
#include <fcntl.h>
#include <linux/media.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#define MAX_OBJECTS 16384U
#define MAX_ATTEMPTS 3

struct graph {
	struct media_v2_entity *entities;
	struct media_v2_interface *interfaces;
	struct media_v2_pad *pads;
	struct media_v2_link *links;
};

static void free_graph(struct graph *g)
{
	free(g->entities);
	free(g->interfaces);
	free(g->pads);
	free(g->links);
	memset(g, 0, sizeof(*g));
}

static int allocate_graph(struct graph *g, const struct media_v2_topology *t)
{
	if (t->num_entities > MAX_OBJECTS || t->num_interfaces > MAX_OBJECTS ||
	    t->num_pads > MAX_OBJECTS || t->num_links > MAX_OBJECTS) {
		fprintf(stderr, "media graph: topology exceeds object limit\n");
		return -1;
	}
	g->entities = calloc(t->num_entities ? t->num_entities : 1, sizeof(*g->entities));
	g->interfaces = calloc(t->num_interfaces ? t->num_interfaces : 1, sizeof(*g->interfaces));
	g->pads = calloc(t->num_pads ? t->num_pads : 1, sizeof(*g->pads));
	g->links = calloc(t->num_links ? t->num_links : 1, sizeof(*g->links));
	if (!g->entities || !g->interfaces || !g->pads || !g->links) {
		fprintf(stderr, "media graph: allocation failed\n");
		free_graph(g);
		return -1;
	}
	return 0;
}

static int read_graph(int fd, struct graph *g, struct media_v2_topology *result)
{
	for (int attempt = 0; attempt < MAX_ATTEMPTS; attempt++) {
		struct media_v2_topology t = { 0 };
		struct media_v2_topology capacities;

		if (ioctl(fd, MEDIA_IOC_G_TOPOLOGY, &t) < 0) {
			perror("media graph: topology size query");
			return -1;
		}
		if (allocate_graph(g, &t) < 0)
			return -1;
		capacities = t;
		t.ptr_entities = (uintptr_t)g->entities;
		t.ptr_interfaces = (uintptr_t)g->interfaces;
		t.ptr_pads = (uintptr_t)g->pads;
		t.ptr_links = (uintptr_t)g->links;
		if (ioctl(fd, MEDIA_IOC_G_TOPOLOGY, &t) < 0) {
			int error = errno;
			free_graph(g);
			if (error == ENOSPC)
				continue;
			errno = error;
			perror("media graph: topology read");
			return -1;
		}
		if (t.topology_version != capacities.topology_version ||
		    t.num_entities > capacities.num_entities ||
		    t.num_interfaces > capacities.num_interfaces ||
		    t.num_pads > capacities.num_pads ||
		    t.num_links > capacities.num_links) {
			free_graph(g);
			continue;
		}
		*result = t;
		return 0;
	}
	fprintf(stderr, "media graph: topology changed during all attempts\n");
	return -1;
}

int main(int argc, char **argv)
{
	struct media_device_info info = { 0 };
	struct media_v2_topology t;
	struct graph g = { 0 };
	int fd;

	if (argc != 2) {
		fprintf(stderr, "usage: %s MEDIA_DEVICE\n", argv[0]);
		return 2;
	}
	fd = open(argv[1], O_RDONLY | O_CLOEXEC);
	if (fd < 0) {
		perror("media graph: open");
		return 1;
	}
	if (ioctl(fd, MEDIA_IOC_DEVICE_INFO, &info) < 0) {
		perror("media graph: device info");
		close(fd);
		return 1;
	}
	if (read_graph(fd, &g, &t) < 0) {
		close(fd);
		return 1;
	}
	printf("media model=%.*s driver=%.*s version=%llu\n",
	       (int)sizeof(info.model), info.model, (int)sizeof(info.driver),
	       info.driver, (unsigned long long)t.topology_version);
	printf("topology entities=%u interfaces=%u pads=%u links=%u\n",
	       t.num_entities, t.num_interfaces, t.num_pads, t.num_links);
	for (__u32 i = 0; i < t.num_entities; i++)
		printf("entity id=%u function=0x%x name=%.*s\n",
		       g.entities[i].id, g.entities[i].function,
		       (int)sizeof(g.entities[i].name), g.entities[i].name);
	for (__u32 i = 0; i < t.num_interfaces; i++)
		printf("interface id=%u type=0x%x dev=%u:%u\n",
		       g.interfaces[i].id, g.interfaces[i].intf_type,
		       g.interfaces[i].devnode.major, g.interfaces[i].devnode.minor);
	for (__u32 i = 0; i < t.num_pads; i++)
		printf("pad id=%u entity=%u index=%u flags=0x%x\n",
		       g.pads[i].id, g.pads[i].entity_id,
		       g.pads[i].index, g.pads[i].flags);
	for (__u32 i = 0; i < t.num_links; i++)
		printf("link id=%u source=%u sink=%u flags=0x%x\n",
		       g.links[i].id, g.links[i].source_id,
		       g.links[i].sink_id, g.links[i].flags);
	free_graph(&g);
	if (close(fd) < 0) {
		perror("media graph: close");
		return 1;
	}
	return 0;
}
