/*
 * The frame channel between an ipu6-loopback bridge and the gbOS libcamera plugin
 * (kernel/libcamera-gbos/gbos_ipu6.cpp), used instead of v4l2loopback when the bridge's output
 * argument is "channel:<camera id>".
 *
 * The bridge listens on the abstract unix socket "\0gbos-camera:<id>" (SOCK_SEQPACKET; abstract
 * names have no file, so no file permissions or labels are involved). One client at a time: a
 * new connection replaces the old one. While a client is connected the bridge converts frames,
 * exactly as while v4l2loopback has a reader; when it disconnects the bridge goes idle.
 *
 * On connect the bridge sends one ipu6_channel_hello with a memfd (SCM_RIGHTS) holding a header
 * (ipu6_channel_shared) and IPU6_CHANNEL_SLOTS YUYV pictures. For each converted frame it fills
 * the next slot and sends an ipu6_channel_frame. A slot's seq is 0 while it is written and the
 * frame's seq afterwards: a reader copies the slot and checks seq before and after.
 * A frame message the client cannot take at once is dropped (the bridge never waits).
 */
#ifndef IPU6_CHANNEL_H
#define IPU6_CHANNEL_H

#include <stdint.h>

#define IPU6_CHANNEL_MAGIC 0x31434247u		/* "GBC1" */
#define IPU6_CHANNEL_VERSION 1
#define IPU6_CHANNEL_PREFIX "gbos-camera:"
#define IPU6_CHANNEL_SLOTS 4
#define IPU6_CHANNEL_HEADER_BYTES 4096u

struct ipu6_channel_hello {
	uint32_t magic, version;
	uint32_t width, height;			/* YUYV, stride width * 2 */
	uint32_t slots, slot_bytes, header_bytes;	/* slot k at header_bytes + k * slot_bytes */
	uint32_t reserved;
	uint64_t map_bytes;			/* memfd size */
};

struct ipu6_channel_shared {
	uint64_t seq[IPU6_CHANNEL_SLOTS];	/* __atomic access; 0 while the slot is written */
};

struct ipu6_channel_frame {
	uint32_t magic, slot;
	uint64_t seq;				/* 1, 2, ... per connection */
	int64_t timestamp_ns;			/* CLOCK_MONOTONIC, from the capture buffer */
	int32_t exposure_us, frame_duration_us;
	float analogue_gain, digital_gain;
	float wb_gain_r, wb_gain_b;		/* white balance (relative to green) */
	float lux_level;			/* reserved (0) */
	uint32_t reserved[3];
};

#endif
