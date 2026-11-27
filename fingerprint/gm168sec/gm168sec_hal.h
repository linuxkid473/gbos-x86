/* SPDX-License-Identifier: LGPL-2.1-or-later */
/*
 * Blocking usbfs backend for the Goodix 27c6:533c (GF5288/GM168SEC) sensor
 * in the Dell XPS 13 9310, for the gbOS86 fingerprint HAL (gbos_fp.c).
 *
 * Ported from seaweeduk/goodix53x5-libfprint, branch pr334 (PR #334, head
 * 52584a5 "Count only empty waits against the 533c handshake limit").
 * That driver is a libfprint driver: its device layer runs on libfprint's
 * FpiSsm async state machine, GUsb bulk transfers, and glib. None of that
 * exists on Bionic, so this file is a blocking rewrite of the same protocol,
 * in the shape of upstream gbos-x86's fingerprint/goodix.h (MOC backend):
 * plain C, static inline, usbfs ioctls, no glib, no libfprint.
 *
 * What is ported faithfully (same bytes on the wire):
 *   - wrapped pack framing: 4-byte header [flags][len LE][checksum=sum of
 *     first 3 bytes], payload, zero-padded to 64-byte USB writes
 *     (family/gm168sec/gm168sec.c:40-100, goodix_frame_pack).
 *   - protocol messages: [cmd=(category<<4)|(command<<1)][size LE =
 *     payload+1][payload][checksum=(0xAA-sum)&0xFF, or 0x88 for handshake]
 *     (device/proto.c:40-108, goodix_proto_build_message /
 *     goodix_proto_validate_checksum).
 *   - raw12 image decode: 6 bytes -> 4 pixels, the driver's byte order
 *     (device/image.c:35-62, goodix_device_decode_image).
 *   - CRC32-MPEG2 image check (device/crypto.c:102-112, init 0xFFFFFFFF,
 *     poly 0x04C11DB7, no final xor) and the custom u32 decode
 *     (crypto.c:120: data[0]*0x100 + data[1] + data[2]*0x1000000 +
 *     data[3]*0x10000).
 *   - TLS 1.2-PSK channel: sensor is the TLS client, host is the server,
 *     cipher PSK-AES128-CBC-SHA256, identity "Client_identity"
 *     (family/gm168sec/tls.c:19-20, goodix_tls_new). OpenSSL is reached
 *     through dlopen() on Android (libssl/libcrypto are in the lapis
 *     system_a and vendor_a scans) and linked directly on the host.
 *   - bring-up order (family/gm168sec/open.c: PROBE -> FIRMWARE ->
 *     PSK_READ -> PSK_CHECK -> RESET -> CHIP -> READ_OTP -> PARSE_OTP ->
 *     CHANNEL, then session.c: restore config, arm FDT).
 *   - command bytes (device/commands.c): probe/fw cat 0x0a cmd 4;
 *     wake NOP cat 0x00 cmd 0x00 (4 zero bytes for gm168sec);
 *     reset cat 0x0a cmd 0x01 payload {0x01|0x05, 0x14};
 *     read chip ID cat 0x08 cmd 0x01 payload {0,0,0,0x04,0};
 *     read OTP cat 0x0a cmd 0x03 payload {0,0};
 *     MCU send cat 0x0d cmd 0x01; TLS request cat 0x0d cmd 0x00 {0,0};
 *     config download cat 0x09 cmd 0x00; FDT down/up cat 0x03 cmd 0x01/0x02;
 *     request image cat 0x02 cmd 0x00 (commands.c:31) with the 4-byte
 *     request built by goodix_build_image_request (commands.c:177-193):
 *     [op_code][hv][dac LE], op_code = tx?0x01:0x81, |=0x40 if finger.
 *   - config blob + patch + checksum (device/calibration.c:238
 *     default_config[256], goodix_device_patch_config, OTP parse at
 *     calibration.c goodix_device_parse_otp; vendored as gm168sec_config.h).
 *   - PSK rule: the sensor ships with the all-zero PSK; the driver holds
 *     it unless a PSK file was imported (device/session.c:298
 *     goodix_load_psk reads GOODIX_PSK_STATE_FILE, missing file means
 *     all-zero). The sensor's stored hash (preset slot 0xbb020001) must
 *     equal SHA-256 of the held PSK (goodix_require_current_psk);
 *     a mismatch is terminal here - the driver never writes a PSK.
 *     (family/gm168sec/open.c:136-138).
 *
 * What is deliberately different from the libfprint driver:
 *   - No FpiSsm: every operation is a blocking call with short bulk
 *     timeouts, so goodix_request_cancel() is honoured between transfers.
 *   - No GUsb: bulk transfers go through USBDEVFS_BULK ioctls, as in
 *     upstream goodix.h (GOODIX_USB_BULK 0xc0185502).
 *   - No on-sensor template database: 533c is host-side matching. The
 *     MOC backend's list/delete/check-duplicate commands (a3/a4/a7) do
 *     not exist here. Templates are Milan blobs stored as files in
 *     DATA_DIR (see gbos_fp_533c_policy.h); goodix_list_templates /
 *     goodix_delete / goodix_delete_template operate on those files, so
 *     the policy header works unchanged. Windows Hello / fprintd
 *     templates live elsewhere and are never touched.
 *   - Matching is the Milan runtime (milan/), behind the gm_matcher
 *     interface below. Without GM168SEC_WITH_MILAN the matcher stubs
 *     return GM168SEC_ERR_NO_MATCHER: the HAL still probes, registers
 *     and reports sensor properties (Milestone 0), but enroll and
 *     identify refuse cleanly instead of pretending to work.
 *
 * Transport differences from the 53x5 parts, verified in
 * seaweeduk/goodix53x5-libfprint#33: endpoints 0x01 OUT / 0x83 IN
 * (not 0x03/0x81), wrapped framing (not wrapless). The 0x01/0x83 pair
 * is the same pair the 6384 MOC HAL uses - same pair, different
 * protocol; do not conflate them.
 */
#ifndef GBOS_GM168SEC_HAL_H
#define GBOS_GM168SEC_HAL_H

/* readlink() and friends under -std=c11. */
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE 1
#endif
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <fcntl.h>
#include <time.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/ioctl.h>

#ifndef GOODIX_ERRNO
#define GOODIX_ERRNO errno
#endif
#ifndef GOODIX_LOGI
#define GOODIX_LOGI(...) ((void)0)
#define GOODIX_LOGW(...) ((void)0)
#define GOODIX_LOGE(...) ((void)0)
#endif

#define GM168SEC_VID 0x27c6
#define GM168SEC_PID 0x533c
#define GM168SEC_EP_OUT 0x01
#define GM168SEC_EP_IN 0x83
#define GM168SEC_INTERFACE 0
#define GM168SEC_USB_BULK 0xc0185502
#define GM168SEC_USB_CLAIMINTERFACE 0x8004550f
#define GM168SEC_USB_RELEASEINTERFACE 0x80045510
#define GM168SEC_USB_CHUNK_SIZE 64

#define GM168SEC_SENSOR_WIDTH 108
#define GM168SEC_SENSOR_HEIGHT 88
#define GM168SEC_SENSOR_PIXELS (108 * 88)		/* 9504 */
#define GM168SEC_RAW12_BYTES (((9504 + 3) / 4) * 6)	/* 14256 */
#define GM168SEC_WRAPPED_IMAGE_LEN (14256 + 4)		/* raw12 + CRC32 */
#define GM168SEC_ENROLL_SAMPLES 12
#define GM168SEC_MAX_TEMPLATES 32
#define GM168SEC_PSK_LEN 32
#define GM168SEC_PSK_HASH_TYPE 0xbb020001
#define GM168SEC_FW_EXPECTED "GF5288_GM168SEC_APP_13016"

/* Template files live in the HAL's DATA_DIR (gbos_fp.c), one per GBOS name:
 *   <DATA_DIR>/tpl-GBOS-<instance>-u<user>-<id>.bin
 * Milan preprocess state (persistence.c format, version 2):
 *   <DATA_DIR>/milan-state.bin
 * Optional imported PSK (hex, as session.c goodix_load_psk reads
 * /var/lib/fprint/goodix53x5.psk on Linux):
 *   <DATA_DIR>/goodix53x5.psk
 */
/* Compatibility names used by the adapted gbos_fp.c / policy header,
 * which were defined by upstream goodix.h (MOC backend). */
#define GOODIX_MAX_TEMPLATES GM168SEC_MAX_TEMPLATES	/* 32 */
#define GOODIX_ENROLL_STAGES GM168SEC_ENROLL_SAMPLES	/* 12 */
#define GOODIX_MAX_UID 32
#define GOODIX_NO_RECORD GOODIX_NORECORD

#ifndef GM168SEC_DATA_DIR
#define GM168SEC_DATA_DIR "/data/vendor/crosfp/gbos_fp"
#endif
#define GM168SEC_TPL_PREFIX "tpl-"
#define GM168SEC_TPL_SUFFIX ".bin"

enum {
	GOODIX_OK = 0,
	GOODIX_NOMATCH,
	GOODIX_RETRY,
	GOODIX_NORECORD,
	GOODIX_FAILED,
	GOODIX_DUPLICATE,	/* enroll: the finger matches a stored template */
	GOODIX_NO_SPACE,	/* enroll: template store is full (32) */
	GOODIX_REOPEN,		/* sensor needs close+reopen (e.g. after system suspend) */
	GM168SEC_ERR_NO_MATCHER = 100,	/* Milan matcher not compiled in */
	GM168SEC_ERR_PSK,		/* sensor PSK is not the PSK we hold */
	GM168SEC_ERR_FIRMWARE,		/* firmware is not GM168/GM168SEC */
};

enum {
	GOODIX_EV_CAPTURE,
	GOODIX_EV_RETRY,
	GOODIX_EV_PROGRESS,
};

struct goodix_ev {
	void (*fn)(void *ctx, int kind, int value);
	void *ctx;
};

struct goodix_template {
	char uid[33];	/* GBOS-<instance>-u<user>-<id>, at most 32 bytes */
	size_t uid_len;
};

/* OTP-derived calibration (device/calibration.c goodix_device_parse_otp). */
struct gm168sec_calib {
	uint16_t tcode;
	uint16_t delta_fdt, delta_down, delta_up, delta_img, delta_nav;
	uint16_t dac_h, dac_l;
	bool dac_from_otp;
	uint16_t dac_delta;
};

struct goodix {
	int fd;
	char path[32];
	uint16_t pid;
	bool need_reset;
	volatile int cancel;
	uint8_t sequence;
	char fw_version[32];
	char sysfs_dev[32];	/* e.g. "1-10": /sys/bus/usb/devices/<dev> */
	uint8_t psk[GM168SEC_PSK_LEN];
	size_t psk_len;
	bool psk_from_file;
	void *tls;		/* struct gm_tls *, or NULL */
	struct gm168sec_calib calib;
	uint8_t otp[64];
	size_t otp_len;
	uint16_t default_dac;
	/* Receive stream: partial pack bytes kept between bulk reads.
	 * Must hold a full encrypted 0xb2 image pack (~14.3 KiB + framing). */
	uint8_t rx[20480];
	size_t rx_len;
	/* TLS records that arrived while a command was outstanding, kept
	 * for the TLS layer to drain (it never runs while a command is
	 * outstanding, so this only ever holds handshake traffic). */
	uint8_t tls_pending[4096];
	size_t tls_pending_len;
	/* FDT bases (calibration.c): 24 bytes each, derived from FDT_MANUAL / events. */
	uint8_t fdt_base_down[24];
	uint8_t fdt_base_up[24];
	bool fdt_base_valid;
	/* Milan matcher state, owned by milan_matcher.c (opaque here). */
	void *milan_state;
	bool delete_blocked;	/* kept for policy-header compatibility; unused */
};

struct goodix_usb_bulk {
	unsigned int ep, len, timeout;
	void *data;
};

/* ---- CRC32-MPEG2 (device/crypto.c:102-112) ----
 * poly 0x04C11DB7, init 0xFFFFFFFF, no final xor. The table is generated
 * from the polynomial at first use, so it cannot drift from the static
 * table in crypto.c (first entries 0x00000000, 0x04C11DB7, ...). */
static uint32_t gm_crc32_mpeg2_table[256];
static bool gm_crc32_mpeg2_ready = false;
static inline void gm_crc32_mpeg2_init(void)
{
	if (gm_crc32_mpeg2_ready)
		return;
	for (int i = 0; i < 256; i++) {
		uint32_t c = (uint32_t)i << 24;
		for (int b = 0; b < 8; b++)
			c = (c & 0x80000000u) ? (c << 1) ^ 0x04C11DB7u : (c << 1);
		gm_crc32_mpeg2_table[i] = c;
	}
	gm_crc32_mpeg2_ready = true;
}
static inline uint32_t gm_crc32_mpeg2(const uint8_t *data, size_t len)
{
	uint32_t crc = 0xFFFFFFFFu;
	gm_crc32_mpeg2_init();
	for (size_t i = 0; i < len; i++)
		crc = (crc << 8) ^ gm_crc32_mpeg2_table[((crc >> 24) ^ data[i]) & 0xFF];
	return crc;
}
/* crypto.c:120: data[0]*0x100 + data[1] + data[2]*0x1000000 + data[3]*0x10000 */
static inline uint32_t gm_decode_u32(const uint8_t *d)
{
	return (uint32_t)d[0] * 0x100u + d[1] + (uint32_t)d[2] * 0x1000000u +
	       (uint32_t)d[3] * 0x10000u;
}

/* ---- wrapped pack framing (family/gm168sec/gm168sec.c:40-100) ----
 * Header: [flags][len LE][checksum = header[0]+header[1]+header[2]].
 * flags 0xa0 = protocol message, 0xb0 = TLS record. Payload is
 * zero-padded to whole 64-byte USB writes. */
#define GM_PACK_HEADER 4
#define GM_PACK_MESSAGE 0xa0
#define GM_PACK_TLS 0xb0
#define GM_PACK_FAMILY_MESSAGE 0xa
#define GM_PACK_FAMILY_TLS 0xb
#define GM_PACK_TLS_DATA_HDR 9	/* GOODIX_PACK_TLS_DATA_HDR (tls.c): 9-byte prefix on image packs */
#define GM_PACK_MAX_PAYLOAD (9 + 5 + 16384 + 2048)

static inline uint8_t gm_pack_checksum(const uint8_t *h)
{
	return (uint8_t)(h[0] + h[1] + h[2]);
}
static inline void gm_pack_write_header(uint8_t *h, uint8_t flags, size_t len)
{
	h[0] = flags;
	h[1] = (uint8_t)(len & 0xff);
	h[2] = (uint8_t)((len >> 8) & 0xff);
	h[3] = gm_pack_checksum(h);
}
/* Returns false when the 4 bytes are not a pack header. */
static inline bool gm_pack_read_header(const uint8_t *h, size_t *len)
{
	uint8_t family = (uint8_t)(h[0] >> 4);
	if ((family != GM_PACK_FAMILY_MESSAGE && family != GM_PACK_FAMILY_TLS) ||
	    gm_pack_checksum(h) != h[3])
		return false;
	*len = (size_t)h[1] | ((size_t)h[2] << 8);
	return *len <= GM_PACK_MAX_PAYLOAD;
}
/* Frame @data as one pack of kind @flags. Returns malloc'd bytes (caller
 * frees) or NULL when the payload does not fit the 16-bit length. */
static inline uint8_t *gm_frame_pack(uint8_t flags, const uint8_t *data,
				     size_t data_len, size_t *out_len)
{
	size_t len = (GM_PACK_HEADER + data_len + GM168SEC_USB_CHUNK_SIZE - 1) /
		     GM168SEC_USB_CHUNK_SIZE * GM168SEC_USB_CHUNK_SIZE;
	uint8_t *framed;
	if (data_len > 0xFFFF)
		return NULL;
	framed = (uint8_t *)calloc(1, len);
	if (!framed)
		return NULL;
	gm_pack_write_header(framed, flags, data_len);
	memcpy(framed + GM_PACK_HEADER, data, data_len);
	*out_len = len;
	return framed;
}
static inline uint8_t *gm_frame_message(const uint8_t *msg, size_t msg_len,
					size_t *out_len)
{
	return gm_frame_pack(GM_PACK_MESSAGE, msg, msg_len, out_len);
}

/* ---- protocol messages (device/proto.c:40-108) ----
 * [cmd=(category<<4)|(command<<1)][size LE = payload+1][payload][checksum],
 * checksum = (0xAA - sum(all prior bytes)) & 0xFF, or 0x88 for handshake
 * (the no-checksum marker, always accepted). */
#define GM_PROTO_CAT_NOP 0x00
#define GM_PROTO_CAT_IMAGE 0x02
#define GM_PROTO_CMD_IMAGE 0x00	/* commands.c:31 */
#define GM_PROTO_CAT_FDT 0x03
#define GM_PROTO_CMD_FDT_DOWN 0x01
#define GM_PROTO_CMD_FDT_UP 0x02
#define GM_PROTO_CMD_FDT_MANUAL 0x03
#define GM_PROTO_CAT_REGISTER 0x08
#define GM_PROTO_CAT_CONFIG 0x09
#define GM_PROTO_CMD_CONFIG_DOWNLOAD 0x00
#define GM_PROTO_CAT_SYSTEM 0x0a
#define GM_PROTO_CAT_ACK 0x0b
#define GM_PROTO_CMD_ACK 0x00
#define GM_PROTO_CAT_NOTICE 0x0c
#define GM_PROTO_CAT_MCU 0x0d
#define GM_PROTO_CAT_PRODUCTION 0x0e
#define GM_PROTO_CMD_PRODUCTION_WRITE 0x01
#define GM_PROTO_CAT_FIRMWARE 0x0f
#define GM_PROTO_RX_BUF_SIZE (16 * 1024)

static inline uint8_t *gm_proto_build_message(uint8_t category, uint8_t command,
					      const uint8_t *payload,
					      size_t payload_len, bool use_checksum,
					      size_t *out_len)
{
	uint8_t cmd_byte = (uint8_t)((category << 4) | (command << 1));
	uint16_t size_field = (uint16_t)(payload_len + 1);
	size_t total = 1 + 2 + payload_len + 1;
	uint8_t *msg = (uint8_t *)malloc(total);
	if (!msg)
		return NULL;
	msg[0] = cmd_byte;
	msg[1] = (uint8_t)(size_field & 0xFF);
	msg[2] = (uint8_t)((size_field >> 8) & 0xFF);
	if (payload_len)
		memcpy(msg + 3, payload, payload_len);
	if (use_checksum) {
		unsigned sum = 0;
		for (size_t i = 0; i < total - 1; i++)
			sum += msg[i];
		msg[total - 1] = (uint8_t)((0xAA - sum) & 0xFF);
	} else {
		msg[total - 1] = 0x88;
	}
	*out_len = total;
	return msg;
}
static inline bool gm_proto_validate_checksum(const uint8_t *data, size_t len)
{
	uint8_t msg_checksum, computed;
	unsigned sum = 0;
	if (len < 4)
		return false;
	msg_checksum = data[len - 1];
	if (msg_checksum == 0x88)
		return true;
	for (size_t i = 0; i < len - 1; i++)
		sum += data[i];
	computed = (uint8_t)((0xAA - sum) & 0xFF);
	return computed == msg_checksum;
}

/* Reassembly buffer for one protocol message, fed 64-byte cells.
 * (device/proto.h GoodixReassembly; the cell split is gm168sec.c
 * goodix_rx_message: the first cell carries the message start, each
 * continuation cell is prefixed with msg[0]|1 and carries 63 bytes.) */
struct gm_rx {
	uint8_t *buf;
	size_t len;
	size_t expected;
	uint8_t cmd_byte;
};
static inline void gm_rx_reset(struct gm_rx *rx)
{
	rx->len = 0;
	rx->expected = 0;
	rx->cmd_byte = 0;
}
static inline bool gm_rx_feed_cell(struct gm_rx *rx, const uint8_t *cell,
				   size_t cell_len)
{
	const uint8_t *data = cell;
	size_t n = cell_len;
	if (!rx->buf) {
		rx->buf = (uint8_t *)malloc(GM_PROTO_RX_BUF_SIZE);
		if (!rx->buf)
			return false;
	}
	if (rx->len == 0) {
		if (cell_len < 4)
			return false;
		rx->cmd_byte = cell[0];
		/* size field counts payload+checksum, after the 3-byte header */
		rx->expected = 3 + (size_t)(cell[1] | (cell[2] << 8));
	} else {
		/* Continuation cell: first byte is cmd|1, skip it. */
		if (cell_len < 1 || (cell[0] & 0xFE) != (rx->cmd_byte & 0xFE))
			return false;
		data = cell + 1;
		n = cell_len - 1;
	}
	if (rx->len + n > GM_PROTO_RX_BUF_SIZE)
		return false;
	memcpy(rx->buf + rx->len, data, n);
	rx->len += n;
	return true;
}
static inline bool gm_rx_complete(const struct gm_rx *rx)
{
	return rx->expected != 0 && rx->len >= rx->expected;
}
static inline bool gm_rx_parse(const struct gm_rx *rx, uint8_t *out_category,
			       uint8_t *out_command, const uint8_t **out_payload,
			       size_t *out_payload_len)
{
	size_t total;
	if (!gm_rx_complete(rx) || !gm_proto_validate_checksum(rx->buf, rx->expected))
		return false;
	total = rx->expected;
	*out_category = (uint8_t)(rx->buf[0] >> 4);
	*out_command = (uint8_t)((rx->buf[0] >> 1) & 0x07);
	*out_payload = rx->buf + 3;
	*out_payload_len = total - 4;	/* minus cmd+size+checksum */
	return true;
}

/* ---- raw12 image decode (device/image.c:35-62) ----
 * 6 bytes -> 4 pixels of 12 bits. Byte order is the driver's, NOT
 * standard LE packed. */
static inline void gm_decode_raw12(const uint8_t *data, uint16_t *image)
{
	for (size_t i = 0, p = 0; i + 5 < GM168SEC_RAW12_BYTES && p + 3 < GM168SEC_SENSOR_PIXELS; i += 6) {
		image[p++] = (uint16_t)(((data[i + 0] & 0x0F) << 8) | data[i + 1]);
		image[p++] = (uint16_t)(((uint16_t)data[i + 3] << 4) | (data[i + 0] >> 4));
		image[p++] = (uint16_t)(((data[i + 5] & 0x0F) << 8) | data[i + 2]);
		image[p++] = (uint16_t)(((uint16_t)data[i + 4] << 4) | (data[i + 5] >> 4));
	}
}
/* A decrypted image pack is valid when it is exactly 14260 bytes and the
 * trailing CRC32-MPEG2 of the first 14256 bytes matches, in the sensor's
 * byte order (channel.c goodix_wrapped_image_valid). */
static inline bool gm_wrapped_image_valid(const uint8_t *image, size_t len)
{
	return len == GM168SEC_WRAPPED_IMAGE_LEN &&
	       gm_crc32_mpeg2(image, GM168SEC_RAW12_BYTES) ==
		       gm_decode_u32(image + GM168SEC_RAW12_BYTES);
}

/* ---- config blob: patch + checksum (device/calibration.c) ---- */
#include "gm168sec_config.h"

#define GM_TCODE_TAG 0x5C		/* calibration.h:274 */
#define GM_DAC_L_TAG 0x220		/* calibration.h:275 */
#define GM_DELTA_DOWN_TAG 0x82		/* calibration.h:276 */

static inline void gm_replace_value_in_section(uint8_t *config, size_t config_len,
					       int section_num, uint16_t tag,
					       uint16_t value)
{
	const uint8_t *section_table = config + 1;
	uint8_t section_base, section_size;
	if (config_len < 3)
		return;
	section_base = section_table[section_num * 2];
	section_size = section_table[section_num * 2 + 1];
	for (int entry_base = section_base;
	     entry_base + 3 < (int)config_len && entry_base < section_base + section_size;
	     entry_base += 4) {
		uint16_t entry_tag = (uint16_t)(config[entry_base] |
					((uint16_t)config[entry_base + 1] << 8));
		if (entry_tag == tag) {
			config[entry_base + 2] = (uint8_t)(value & 0xFF);
			config[entry_base + 3] = (uint8_t)((value >> 8) & 0xFF);
		}
	}
}
static inline void gm_fix_config_checksum(uint8_t *config, size_t config_len)
{
	uint32_t checksum = 0xA5A5;
	for (size_t i = 0; i + 1 < config_len - 1; i += 2) {
		uint16_t w = (uint16_t)(config[i] | ((uint16_t)config[i + 1] << 8));
		checksum += w;
		checksum &= 0xFFFF;
	}
	checksum = 0x10000 - checksum;
	config[config_len - 2] = (uint8_t)(checksum & 0xFF);
	config[config_len - 1] = (uint8_t)((checksum >> 8) & 0xFF);
}
/* calibration.c goodix_device_patch_config: tcode into sections 2,3,4;
 * dac_l into sections 2,3 when the OTP supplied one; delta_down into
 * section 2; then the checksum. */
static inline void gm_patch_config(uint8_t *config, size_t config_len,
				   const struct gm168sec_calib *params)
{
	if (params->tcode != 0) {
		gm_replace_value_in_section(config, config_len, 2, GM_TCODE_TAG, params->tcode);
		gm_replace_value_in_section(config, config_len, 3, GM_TCODE_TAG, params->tcode);
		gm_replace_value_in_section(config, config_len, 4, GM_TCODE_TAG, params->tcode);
	}
	if (params->dac_from_otp) {
		gm_replace_value_in_section(config, config_len, 2, GM_DAC_L_TAG,
					    (uint16_t)((params->dac_l << 4) | 8));
		gm_replace_value_in_section(config, config_len, 3, GM_DAC_L_TAG,
					    (uint16_t)((params->dac_l << 4) | 8));
	}
	gm_replace_value_in_section(config, config_len, 2, GM_DELTA_DOWN_TAG,
				    (uint16_t)((params->delta_down << 8) | 0x80));
	gm_fix_config_checksum(config, config_len);
}
/* calibration.c goodix_device_parse_otp. otp[] indexes are the driver's. */
static inline void gm_parse_otp(const uint8_t *otp, size_t otp_len,
				struct gm168sec_calib *params)
{
	uint8_t diff;
	uint16_t rounded, scaled;
	memset(params, 0, sizeof(*params));
	params->tcode = (otp_len > 23 && otp[23] != 0) ? (uint16_t)(otp[23] + 1) : 0;
	diff = (uint8_t)((otp_len > 17 ? (otp[17] >> 1) : 0) & 0x1F);
	if (diff == 0) {
		params->delta_fdt = 0;
		params->delta_down = 0xD;
		params->delta_up = 0xB;
		params->delta_img = 0xC8;
		params->delta_nav = 0x28;
	} else {
		rounded = (uint16_t)(diff + 5);
		scaled = (uint16_t)((rounded * 0x32) >> 4);
		params->delta_fdt = (uint16_t)(scaled / 5);
		params->delta_down = (uint16_t)(scaled / 3);
		params->delta_up = (uint16_t)(params->delta_down - 2);
		params->delta_img = 0xC8;
		params->delta_nav = (uint16_t)(rounded * 4);
	}
	if (otp_len <= 17 || otp[17] == 0 || otp_len <= 22 || otp[22] == 0 ||
	    (otp_len > 31 ? otp[31] == 0 : true)) {
		params->dac_h = 0x97;
		params->dac_l = 0xD0;
	} else {
		params->dac_h = (uint16_t)(((uint16_t)otp[17] << 8 ^ otp[22]) & 0x1FF);
		params->dac_l = (uint16_t)(((otp[17] & 0x40) << 2) ^ otp[31]);
		params->dac_from_otp = true;
	}
	params->dac_delta = params->tcode != 0 ? (uint16_t)(0xC83 / params->tcode) : 0;
}

/* ---- usbfs transport (same ioctls as upstream goodix.h) ---- */
static inline void goodix_init(struct goodix *s)
{
	memset(s, 0, sizeof(*s));
	s->fd = -1;
	s->pid = GM168SEC_PID;
}
static inline void goodix_request_cancel(struct goodix *s)
{
	__atomic_store_n(&s->cancel, 1, __ATOMIC_RELEASE);
}
static inline void goodix_clear_cancel(struct goodix *s)
{
	__atomic_store_n(&s->cancel, 0, __ATOMIC_RELEASE);
}
static inline bool goodix_cancel_requested(struct goodix *s)
{
	return __atomic_load_n(&s->cancel, __ATOMIC_ACQUIRE) != 0;
}
static inline int gm_bulk(struct goodix *s, unsigned int ep, void *buf,
			  unsigned int len, unsigned int timeout_ms)
{
	struct goodix_usb_bulk b = { ep, len, timeout_ms, buf };
	int r;
	if (s->fd < 0)
		return -ENODEV;
	r = ioctl(s->fd, GM168SEC_USB_BULK, &b);
	if (r < 0)
		return -GOODIX_ERRNO;
	return r;
}
/* Scan /sys/bus/usb/devices for 27c6:533c and open its /dev/bus/usb node.
 * (Upstream goodix.h goodix_probe_node / goodix_find.) */
static inline bool gm_probe_node(const char *path, uint16_t *pid)
{
	char buf[64];
	int fd, n;
	if (snprintf(buf, sizeof(buf), "%s/idVendor", path) < 0)
		return false;
	fd = open(buf, O_RDONLY);
	if (fd < 0)
		return false;
	n = (int)read(fd, buf, sizeof(buf) - 1);
	close(fd);
	if (n < 4 || memcmp(buf, "27c6", 4) != 0)
		return false;
	if (snprintf(buf, sizeof(buf), "%s/idProduct", path) < 0)
		return false;
	fd = open(buf, O_RDONLY);
	if (fd < 0)
		return false;
	n = (int)read(fd, buf, sizeof(buf) - 1);
	close(fd);
	if (n < 4 || memcmp(buf, "533c", 4) != 0)
		return false;
	*pid = GM168SEC_PID;
	return true;
}
static inline int gm_find(struct goodix *s)
{
	DIR *d = opendir("/sys/bus/usb/devices");
	struct dirent *e;
	if (!d)
		return -GOODIX_ERRNO;
	while ((e = readdir(d)) != NULL) {
		char path[128], busnum[16], devnum[16];
		int fd, n, bus, dev;
		uint16_t pid;
		if (e->d_name[0] == '.')
			continue;
		snprintf(path, sizeof(path), "/sys/bus/usb/devices/%s", e->d_name);
		if (!gm_probe_node(path, &pid))
			continue;
		snprintf(path, sizeof(path), "/sys/bus/usb/devices/%s/busnum", e->d_name);
		fd = open(path, O_RDONLY);
		if (fd < 0)
			continue;
		n = (int)read(fd, busnum, sizeof(busnum) - 1);
		close(fd);
		if (n <= 0)
			continue;
		busnum[n] = 0;
		snprintf(path, sizeof(path), "/sys/bus/usb/devices/%s/devnum", e->d_name);
		fd = open(path, O_RDONLY);
		if (fd < 0)
			continue;
		n = (int)read(fd, devnum, sizeof(devnum) - 1);
		close(fd);
		if (n <= 0)
			continue;
		devnum[n] = 0;
		bus = atoi(busnum);
		dev = atoi(devnum);
		snprintf(s->path, sizeof(s->path), "/dev/bus/usb/%03d/%03d", bus, dev);
		snprintf(s->sysfs_dev, sizeof(s->sysfs_dev), "%s", e->d_name);
		s->pid = pid;
		closedir(d);
		return 0;
	}
	closedir(d);
	return -ENODEV;
}
/* The sensor advertises a USB CDC descriptor, so cdc_acm claims it as a
 * serial modem. Unbind it from this device's interface 0 before claiming
 * (the AUR package ships 91-goodix-fingerprint.rules for the same reason).
 * Best-effort only: the HAL runs as user system and usually cannot write
 * the unbind sysfs node. hook.sh (hook-533c.patch) does the real unbind
 * as root, before the service starts. */
static inline void gm_unbind_cdc_acm(struct goodix *s)
{
	char driver_path[192], id[64];
	ssize_t n;
	int fd;
	if (!s->sysfs_dev[0])
		return;
	snprintf(driver_path, sizeof(driver_path),
		 "/sys/bus/usb/devices/%s/%s:1.0/driver", s->sysfs_dev, s->sysfs_dev);
	n = readlink(driver_path, id, sizeof(id) - 1);
	if (n < 0)
		return;
	id[n] = 0;
	if (strstr(id, "cdc_acm") == NULL)
		return;
	snprintf(id, sizeof(id), "%s:1.0", s->sysfs_dev);
	fd = open("/sys/bus/usb/drivers/cdc_acm/unbind", O_WRONLY);
	if (fd < 0)
		return;
	if (write(fd, id, strlen(id)) < 0)
		GOODIX_LOGW("533c: cdc_acm unbind of %s failed: %d", id, GOODIX_ERRNO);
	else
		GOODIX_LOGI("533c: unbound cdc_acm from %s", id);
	close(fd);
}

/* ---- command layer ----
 * One command, blocking: frame the message, bulk OUT once (Dell's driver
 * sends each 533c command once, PR #334 58c1927 - no IoHub retry), wait
 * for the ACK (category 0x0b, command 0x00), then the data reply when
 * expect_data. Budgets from gm168sec.c:220-223: the MCU waits 1500 ms
 * for every ACK and reply; EC control alone gives its ACK 500 ms.
 * Returns 0 with *resp filled, or a negative errno. *fail gets the
 * sensor status byte when the reply carries one. */
struct gm_response {
	uint8_t result;		/* ACK status byte, or first payload byte */
	uint8_t data[GM_PROTO_RX_BUF_SIZE];
	size_t len;
	uint8_t category, command;
};
static inline int gm_read_pack(struct goodix *s, uint8_t *flags_out,
			       uint8_t *payload, size_t *payload_len,
			       unsigned int timeout_ms)
{
	/* Persistent stream buffer (goodix_receive_wrapped): bulk chunks
	 * are appended, zero padding is skipped (a pack never starts with
	 * zero), one complete pack is extracted, the rest stays buffered
	 * for the next call. Short slices keep cancel responsive. */
	uint8_t chunk[GM168SEC_USB_CHUNK_SIZE];
	int waited = 0;
	size_t cap = *payload_len;
	while (waited < (int)timeout_ms) {
		/* Try to extract a pack from what is already buffered. */
		size_t pad = 0;
		while (pad < s->rx_len && s->rx[pad] == 0)
			pad++;
		if (pad) {
			memmove(s->rx, s->rx + pad, s->rx_len - pad);
			s->rx_len -= pad;
		}
		if (s->rx_len >= GM_PACK_HEADER) {
			size_t plen;
			if (!gm_pack_read_header(s->rx, &plen)) {
				GOODIX_LOGW("533c: dropping %zu wrapped bytes with a bad pack header",
					    s->rx_len);
				s->rx_len = 0;
				continue;
			}
			if (s->rx_len >= GM_PACK_HEADER + plen) {
				if (plen > cap)
					return -EMSGSIZE;
				*flags_out = s->rx[0];
				memcpy(payload, s->rx + GM_PACK_HEADER, plen);
				*payload_len = plen;
				memmove(s->rx, s->rx + GM_PACK_HEADER + plen,
					s->rx_len - (GM_PACK_HEADER + plen));
				s->rx_len -= GM_PACK_HEADER + plen;
				return 0;
			}
		}
		int slice = timeout_ms - waited < 100 ? (int)(timeout_ms - waited) : 100;
		int r = gm_bulk(s, GM168SEC_EP_IN, chunk, sizeof(chunk), (unsigned)slice);
		if (r < 0) {
			if (GOODIX_ERRNO == ETIMEDOUT || GOODIX_ERRNO == EAGAIN) {
				waited += slice;
				if (goodix_cancel_requested(s))
					return -ECANCELED;
				continue;
			}
			return -GOODIX_ERRNO;
		}
		if (s->rx_len + (size_t)r > sizeof(s->rx))
			return -EMSGSIZE;
		memcpy(s->rx + s->rx_len, chunk, (size_t)r);
		s->rx_len += (size_t)r;
	}
	return -ETIMEDOUT;
}
static inline int gm_command(struct goodix *s, uint8_t category, uint8_t command,
			     const uint8_t *payload, size_t payload_len,
			     bool expect_data, struct gm_response *resp,
			     unsigned int ack_ms, unsigned int data_ms)
{
	uint8_t *msg, *framed;
	size_t msg_len, framed_len;
	int r;
	struct gm_rx rx;
	const uint8_t *pl;
	size_t pl_len;
	uint8_t cat, cmd;
	memset(resp, 0, sizeof(*resp));
	msg = gm_proto_build_message(category, command, payload, payload_len, true, &msg_len);
	if (!msg)
		return -ENOMEM;
	framed = gm_frame_message(msg, msg_len, &framed_len);
	free(msg);
	if (!framed)
		return -ENOMEM;
	r = gm_bulk(s, GM168SEC_EP_OUT, framed, (unsigned)framed_len, ack_ms);
	free(framed);
	if (r < 0)
		return -GOODIX_ERRNO;
	/* ACK, then the data reply. ACK-only commands return after the ACK. */
	gm_rx_reset(&rx);
	rx.buf = NULL;
	for (;;) {
		uint8_t flags, pack[GM_PACK_MAX_PAYLOAD + GM_PACK_HEADER];
		size_t plen = sizeof(pack) - GM_PACK_HEADER;
		r = gm_read_pack(s, &flags, pack, &plen, expect_data ? data_ms : ack_ms);
		if (r < 0) {
			free(rx.buf);
			return r;
		}
		if ((flags >> 4) == GM_PACK_FAMILY_TLS) {
			/* A TLS record arriving mid-command: stash it for the
			 * TLS layer to drain, and keep waiting for the reply.
			 * Image packs carry the 0x02 prefixed flag: strip the
			 * 9-byte prefix (GOODIX_PACK_TLS_DATA_HDR) first.
			 * (Handshake traffic only ever arrives while no command
			 * is outstanding, so this is a safety net, not the
			 * normal path.) */
			const uint8_t *rec = pack;
			size_t reclen = plen;
			if (flags & 0x02) {
				if (reclen < 9)
					continue;
				rec += 9;
				reclen -= 9;
			}
			if (s->tls_pending_len + reclen <= sizeof(s->tls_pending)) {
				memcpy(s->tls_pending + s->tls_pending_len, rec, reclen);
				s->tls_pending_len += reclen;
			} else {
				GOODIX_LOGW("533c: dropping %zu-byte TLS record: pending buffer full",
					    reclen);
			}
			continue;
		}
		/* Split the message back into 64-byte cells for the
		 * reassembly parser, exactly as gm168sec.c goodix_rx_message
		 * does: the first cell is the message start, each
		 * continuation cell is [msg[0]|1] + the next 63 bytes. */
		{
			size_t off = plen < GM168SEC_USB_CHUNK_SIZE ?
				     plen : GM168SEC_USB_CHUNK_SIZE;
			uint8_t cell[GM168SEC_USB_CHUNK_SIZE];
			if (!gm_rx_feed_cell(&rx, pack, off)) {
				free(rx.buf);
				return -EBADMSG;
			}
			while (off < plen) {
				size_t n = plen - off < GM168SEC_USB_CHUNK_SIZE - 1 ?
					   plen - off : GM168SEC_USB_CHUNK_SIZE - 1;
				cell[0] = (uint8_t)(pack[0] | 1);
				memcpy(cell + 1, pack + off, n);
				if (!gm_rx_feed_cell(&rx, cell, n + 1)) {
					free(rx.buf);
					return -EBADMSG;
				}
				off += n;
			}
		}
		if (!gm_rx_complete(&rx))
			continue;
		if (!gm_rx_parse(&rx, &cat, &cmd, &pl, &pl_len)) {
			free(rx.buf);
			return -EBADMSG;
		}
		resp->category = cat;
		resp->command = cmd;
		if (cat == GM_PROTO_CAT_ACK && cmd == GM_PROTO_CMD_ACK) {
			resp->result = pl_len ? pl[0] : 0;
			if (!expect_data) {
				free(rx.buf);
				return 0;
			}
			gm_rx_reset(&rx);
			continue;
		}
		if (pl_len > sizeof(resp->data)) {
			free(rx.buf);
			return -EMSGSIZE;
		}
		memcpy(resp->data, pl, pl_len);
		resp->len = pl_len;
		resp->result = pl_len ? pl[0] : 0;
		free(rx.buf);
		return 0;
	}
}

/* ---- SHA-256 (FIPS 180-4), for the PSK hash check and TLS transcript ----
 * Self-contained: no external digest state needed. */
static inline void gm_sha256(const uint8_t *data, size_t len, uint8_t out[32])
{
	static const uint32_t K[64] = {
		0x428a2f98u,0x71374491u,0xb5c0fbcfu,0xe9b5dba5u,0x3956c25bu,0x59f111f1u,0x923f82a4u,0xab1c5ed5u,
		0xd807aa98u,0x12835b01u,0x243185beu,0x550c7dc3u,0x72be5d74u,0x80deb1feu,0x9bdc06a7u,0xc19bf174u,
		0xe49b69c1u,0xefbe4786u,0x0fc19dc6u,0x240ca1ccu,0x2de92c6fu,0x4a7484aau,0x5cb0a9dcu,0x76f988dau,
		0x983e5152u,0xa831c66du,0xb00327c8u,0xbf597fc7u,0xc6e00bf3u,0xd5a79147u,0x06ca6351u,0x14292967u,
		0x27b70a85u,0x2e1b2138u,0x4d2c6dfcu,0x53380d13u,0x650a7354u,0x766a0abbu,0x81c2c92eu,0x92722c85u,
		0xa2bfe8a1u,0xa81a664bu,0xc24b8b70u,0xc76c51a3u,0xd192e819u,0xd6990624u,0xf40e3585u,0x106aa070u,
		0x19a4c116u,0x1e376c08u,0x2748774cu,0x34b0bcb5u,0x391c0cb3u,0x4ed8aa4au,0x5b9cca4fu,0x682e6ff3u,
		0x748f82eeu,0x78a5636fu,0x84c87814u,0x8cc70208u,0x90befffau,0xa4506cebu,0xbef9a3f7u,0xc67178f2u };
	uint32_t h[8] = { 0x6a09e667u,0xbb67ae85u,0x3c6ef372u,0xa54ff53au,
			  0x510e527fu,0x9b05688cu,0x1f83d9abu,0x5be0cd19u };
	uint64_t bits = (uint64_t)len * 8;
	size_t padded = ((len + 9 + 63) / 64) * 64;
	uint8_t *msg = (uint8_t *)calloc(1, padded);
	if (!msg)
		return;
	memcpy(msg, data, len);
	msg[len] = 0x80;
	for (int i = 0; i < 8; i++)
		msg[padded - 1 - i] = (uint8_t)(bits >> (8 * i));
	for (size_t off = 0; off < padded; off += 64) {
		uint32_t w[64];
		for (int i = 0; i < 16; i++)
			w[i] = ((uint32_t)msg[off + 4*i] << 24) | ((uint32_t)msg[off + 4*i + 1] << 16) |
			       ((uint32_t)msg[off + 4*i + 2] << 8) | msg[off + 4*i + 3];
		for (int i = 16; i < 64; i++) {
			uint32_t s0 = ((w[i-15] >> 7) | (w[i-15] << 25)) ^ ((w[i-15] >> 18) | (w[i-15] << 14)) ^ (w[i-15] >> 3);
			uint32_t s1 = ((w[i-2] >> 17) | (w[i-2] << 15)) ^ ((w[i-2] >> 19) | (w[i-2] << 13)) ^ (w[i-2] >> 10);
			w[i] = w[i-16] + s0 + w[i-7] + s1;
		}
		uint32_t a=h[0],b=h[1],c=h[2],d=h[3],e=h[4],f=h[5],g=h[6],hh=h[7];
		for (int i = 0; i < 64; i++) {
			uint32_t S1 = ((e >> 6) | (e << 26)) ^ ((e >> 11) | (e << 21)) ^ ((e >> 25) | (e << 7));
			uint32_t ch = (e & f) ^ ((~e) & g);
			uint32_t t1 = hh + S1 + ch + K[i] + w[i];
			uint32_t S0 = ((a >> 2) | (a << 30)) ^ ((a >> 13) | (a << 19)) ^ ((a >> 22) | (a << 10));
			uint32_t mj = (a & b) ^ (a & c) ^ (b & c);
			uint32_t t2 = S0 + mj;
			hh=g; g=f; f=e; e=d+t1; d=c; c=b; b=a; a=t1+t2;
		}
		h[0]+=a; h[1]+=b; h[2]+=c; h[3]+=d; h[4]+=e; h[5]+=f; h[6]+=g; h[7]+=hh;
	}
	free(msg);
	for (int i = 0; i < 8; i++) {
		out[4*i] = (uint8_t)(h[i] >> 24);
		out[4*i+1] = (uint8_t)(h[i] >> 16);
		out[4*i+2] = (uint8_t)(h[i] >> 8);
		out[4*i+3] = (uint8_t)h[i];
	}
}

/* ---- Self-contained TLS 1.2 PSK server (TLS_PSK_WITH_AES_128_CBC_SHA256, 0x00AE) ----
 * The GM168SEC sensor ONLY offers cipher suite 0x00AE (and 0x00FF SCSV).
 * Android's BoringSSL libssl.so removed 0x00AE (keeping only 0x008C
 * PSK-AES128-CBC-SHA) and does not export SSL_CTX_ctrl. Implementing the
 * minimal RFC 5246 / RFC 4279 / RFC 5487 server state machine directly on
 * libcrypto.so (EVP_aes_128_cbc, HMAC-SHA256, RAND_bytes, CRYPTO_memcmp)
 * removes all dependency on libssl.so and works identically on both
 * Android BoringSSL and host OpenSSL 3. */
#ifdef GM168SEC_HOST_TEST
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/rand.h>
#include <openssl/crypto.h>
#else
struct env_md_st;
struct evp_cipher_st;
struct evp_cipher_ctx_st;
struct engine_st;
const struct env_md_st *EVP_sha256(void);
uint8_t *HMAC(const struct env_md_st *, const void *key, size_t key_len,
	      const uint8_t *data, size_t data_len, uint8_t *out, unsigned int *out_len);
int CRYPTO_memcmp(const void *, const void *, size_t);
const struct evp_cipher_st *EVP_aes_128_cbc(void);
struct evp_cipher_ctx_st *EVP_CIPHER_CTX_new(void);
void EVP_CIPHER_CTX_free(struct evp_cipher_ctx_st *);
int EVP_CIPHER_CTX_set_padding(struct evp_cipher_ctx_st *, int pad);
int EVP_EncryptInit_ex(struct evp_cipher_ctx_st *, const struct evp_cipher_st *,
		       struct engine_st *, const uint8_t *key, const uint8_t *iv);
int EVP_EncryptUpdate(struct evp_cipher_ctx_st *, uint8_t *out, int *outl,
		      const uint8_t *in, int inl);
int EVP_DecryptInit_ex(struct evp_cipher_ctx_st *, const struct evp_cipher_st *,
		       struct engine_st *, const uint8_t *key, const uint8_t *iv);
int EVP_DecryptUpdate(struct evp_cipher_ctx_st *, uint8_t *out, int *outl,
		      const uint8_t *in, int inl);
int RAND_bytes(uint8_t *buf, size_t len);
#endif

#define GM_TLS_CIPHER "PSK-AES128-CBC-SHA256"
#define GM_TLS_IDENTITY "Client_identity"
#define GM_TLS_MAX_HANDSHAKE_CALLS 10

enum {
	GM_TLS_WAIT_CH = 0,
	GM_TLS_WAIT_CKE = 1,
	GM_TLS_WAIT_CCS = 2,
	GM_TLS_WAIT_FIN = 3,
	GM_TLS_ESTABLISHED = 4,
	GM_TLS_FAILED = -1,
};

struct gm_tls {
	int state;
	uint8_t psk[GM168SEC_PSK_LEN];
	size_t psk_len;
	int handshake_calls;	/* only empty calls count (tls.c:141) */
	uint8_t client_random[32];
	uint8_t server_random[32];
	uint8_t master_secret[48];
	uint8_t client_mac_key[32];
	uint8_t server_mac_key[32];
	uint8_t client_key[16];
	uint8_t server_key[16];
	uint64_t client_seq;
	uint64_t server_seq;
	uint8_t hs_buf[4096];
	size_t hs_len;
	uint8_t rx_buf[4096];
	size_t rx_len;
};

static inline void gm_tls_put_be64(uint8_t out[8], uint64_t v)
{
	for (int i = 0; i < 8; i++)
		out[i] = (uint8_t)(v >> (56 - 8 * i));
}

static inline bool gm_tls_hs_append(struct gm_tls *tls, const uint8_t *data, size_t len)
{
	if (tls->hs_len + len > sizeof(tls->hs_buf))
		return false;
	memcpy(tls->hs_buf + tls->hs_len, data, len);
	tls->hs_len += len;
	return true;
}

static inline bool gm_tls_hmac_sha256(const uint8_t *key, size_t key_len,
				      const uint8_t *data, size_t data_len,
				      uint8_t out[32])
{
	unsigned int olen = 0;
#ifdef GM168SEC_HOST_TEST
	return HMAC(EVP_sha256(), key, (int)key_len, data, data_len, out, &olen) != NULL && olen == 32;
#else
	return HMAC(EVP_sha256(), key, key_len, data, data_len, out, &olen) != NULL && olen == 32;
#endif
}

/* TLS 1.2 PRF-SHA256 (RFC 5246 section 5). */
static inline bool gm_tls_prf(const uint8_t *secret, size_t secret_len,
			      const char *label, const uint8_t *seed, size_t seed_len,
			      uint8_t *out, size_t out_len)
{
	size_t l_len = strlen(label);
	size_t ls_len = l_len + seed_len;
	uint8_t *buf = (uint8_t *)malloc(32 + ls_len);
	uint8_t a[32], block[32];
	size_t done = 0;
	if (!buf)
		return false;
	memcpy(buf + 32, label, l_len);
	memcpy(buf + 32 + l_len, seed, seed_len);
	/* A(1) = HMAC(secret, label || seed) */
	if (!gm_tls_hmac_sha256(secret, secret_len, buf + 32, ls_len, a)) {
		memset(buf, 0, 32 + ls_len);
		free(buf);
		return false;
	}
	while (done < out_len) {
		size_t take = out_len - done < 32 ? out_len - done : 32;
		memcpy(buf, a, 32);
		if (!gm_tls_hmac_sha256(secret, secret_len, buf, 32 + ls_len, block)) {
			memset(buf, 0, 32 + ls_len);
			free(buf);
			return false;
		}
		memcpy(out + done, block, take);
		done += take;
		if (done < out_len) {
			if (!gm_tls_hmac_sha256(secret, secret_len, a, 32, a)) {
				memset(buf, 0, 32 + ls_len);
				free(buf);
				return false;
			}
		}
	}
	memset(a, 0, sizeof(a));
	memset(block, 0, sizeof(block));
	memset(buf, 0, 32 + ls_len);
	free(buf);
	return true;
}

static inline bool gm_tls_record_encrypt(struct gm_tls *tls, uint8_t rec_type,
					 const uint8_t *plain, size_t plain_len,
					 uint8_t *out_rec, size_t *out_rec_len)
{
	uint8_t iv[16], mac[32];
	uint8_t *mac_in = NULL, *block_in = NULL;
	struct evp_cipher_ctx_st *ctx = NULL;
	size_t pad_len = 16 - ((plain_len + 32) % 16);	/* 1..16 bytes; value = pad_len - 1 */
	size_t ct_len = plain_len + 32 + pad_len;
	int outl = 0;
	bool ok = false;

	if (RAND_bytes(iv, sizeof(iv)) != 1)
		return false;
	mac_in = (uint8_t *)malloc(13 + plain_len);
	block_in = (uint8_t *)malloc(ct_len);
	ctx = EVP_CIPHER_CTX_new();
	if (!mac_in || !block_in || !ctx)
		goto out;
	gm_tls_put_be64(mac_in, tls->server_seq);
	mac_in[8] = rec_type;
	mac_in[9] = 0x03;
	mac_in[10] = 0x03;
	mac_in[11] = (uint8_t)(plain_len >> 8);
	mac_in[12] = (uint8_t)(plain_len & 0xFF);
	memcpy(mac_in + 13, plain, plain_len);
	if (!gm_tls_hmac_sha256(tls->server_mac_key, 32, mac_in, 13 + plain_len, mac))
		goto out;
	memcpy(block_in, plain, plain_len);
	memcpy(block_in + plain_len, mac, 32);
	memset(block_in + plain_len + 32, (int)(pad_len - 1), pad_len);
	out_rec[0] = rec_type;
	out_rec[1] = 0x03;
	out_rec[2] = 0x03;
	out_rec[3] = (uint8_t)((16 + ct_len) >> 8);
	out_rec[4] = (uint8_t)((16 + ct_len) & 0xFF);
	memcpy(out_rec + 5, iv, 16);
	if (EVP_EncryptInit_ex(ctx, EVP_aes_128_cbc(), NULL, tls->server_key, iv) != 1 ||
	    EVP_CIPHER_CTX_set_padding(ctx, 0) != 1 ||
	    EVP_EncryptUpdate(ctx, out_rec + 5 + 16, &outl, block_in, (int)ct_len) != 1 ||
	    outl != (int)ct_len)
		goto out;
	tls->server_seq++;
	*out_rec_len = 5 + 16 + ct_len;
	ok = true;
out:
	if (ctx)
		EVP_CIPHER_CTX_free(ctx);
	if (mac_in) {
		memset(mac_in, 0, 13 + plain_len);
		free(mac_in);
	}
	if (block_in) {
		memset(block_in, 0, ct_len);
		free(block_in);
	}
	memset(mac, 0, sizeof(mac));
	return ok;
}

static inline int gm_tls_record_decrypt(struct gm_tls *tls, uint8_t rec_type,
					uint8_t ver_hi, uint8_t ver_lo,
					const uint8_t *payload, size_t payload_len,
					uint8_t **plain_out, size_t *plain_len_out)
{
	struct evp_cipher_ctx_st *ctx = NULL;
	uint8_t *dec = NULL, *mac_in = NULL;
	uint8_t mac[32];
	size_t ct_len, pad_total, plain_len, i;
	uint8_t pad_val;
	int outl = 0, r = -1;

	*plain_out = NULL;
	*plain_len_out = 0;
	if (payload_len < 16 + 48 || payload_len > 16 + 18432 || ((payload_len - 16) % 16) != 0)
		return -1;
	ct_len = payload_len - 16;
	dec = (uint8_t *)malloc(ct_len);
	ctx = EVP_CIPHER_CTX_new();
	if (!dec || !ctx)
		goto out;
	if (EVP_DecryptInit_ex(ctx, EVP_aes_128_cbc(), NULL, tls->client_key, payload) != 1 ||
	    EVP_CIPHER_CTX_set_padding(ctx, 0) != 1 ||
	    EVP_DecryptUpdate(ctx, dec, &outl, payload + 16, (int)ct_len) != 1 ||
	    outl != (int)ct_len)
		goto out;
	pad_val = dec[ct_len - 1];
	pad_total = (size_t)pad_val + 1;
	if (pad_total + 32 > ct_len)
		goto out;
	for (i = 0; i < pad_total; i++) {
		if (dec[ct_len - 1 - i] != pad_val)
			goto out;
	}
	plain_len = ct_len - 32 - pad_total;
	mac_in = (uint8_t *)malloc(13 + plain_len);
	if (!mac_in)
		goto out;
	gm_tls_put_be64(mac_in, tls->client_seq);
	mac_in[8] = rec_type;
	mac_in[9] = ver_hi;
	mac_in[10] = ver_lo;
	mac_in[11] = (uint8_t)(plain_len >> 8);
	mac_in[12] = (uint8_t)(plain_len & 0xFF);
	if (plain_len)
		memcpy(mac_in + 13, dec, plain_len);
	if (!gm_tls_hmac_sha256(tls->client_mac_key, 32, mac_in, 13 + plain_len, mac))
		goto out;
	if (CRYPTO_memcmp(mac, dec + plain_len, 32) != 0)
		goto out;
	tls->client_seq++;
	*plain_out = dec;
	*plain_len_out = plain_len;
	dec = NULL;
	r = 0;
out:
	if (ctx)
		EVP_CIPHER_CTX_free(ctx);
	if (mac_in) {
		memset(mac_in, 0, 13 + plain_len);
		free(mac_in);
	}
	if (dec) {
		memset(dec, 0, ct_len);
		free(dec);
	}
	memset(mac, 0, sizeof(mac));
	return r;
}

static inline struct gm_tls *gm_tls_new(const uint8_t *psk, size_t psk_len)
{
	struct gm_tls *tls;
	if (psk_len > GM168SEC_PSK_LEN)
		return NULL;
	tls = (struct gm_tls *)calloc(1, sizeof(*tls));
	if (!tls)
		return NULL;
	memcpy(tls->psk, psk, psk_len);
	tls->psk_len = psk_len;
	tls->state = GM_TLS_WAIT_CH;
	return tls;
}

static inline void gm_tls_free(struct gm_tls *tls)
{
	if (!tls)
		return;
	memset(tls, 0, sizeof(*tls));
	free(tls);
}

static inline bool gm_tls_established(struct gm_tls *tls)
{
	return tls && tls->state == GM_TLS_ESTABLISHED;
}

/* One handshake step: feed @in (may be empty or contain 1+ records),
 * advance the TLS 1.2 PSK server state machine, and emit output records.
 * Returns 1 when handshake finished, 0 when it wants more input, -1 on error. */
static inline int gm_tls_handshake_step(struct gm_tls *tls, const uint8_t *in,
					size_t in_len, uint8_t **out, size_t *out_len)
{
	uint8_t tx[256];
	size_t tx_len = 0;
	*out = NULL;
	*out_len = 0;
	if (!tls || tls->state == GM_TLS_FAILED)
		return -1;
	if (tls->state == GM_TLS_ESTABLISHED)
		return 1;
	if (!in_len && ++tls->handshake_calls > GM_TLS_MAX_HANDSHAKE_CALLS)
		return -1;
	if (in_len) {
		if (tls->rx_len + in_len > sizeof(tls->rx_buf)) {
			tls->state = GM_TLS_FAILED;
			return -1;
		}
		memcpy(tls->rx_buf + tls->rx_len, in, in_len);
		tls->rx_len += in_len;
	}
	while (tls->rx_len >= 5) {
		uint8_t rtype = tls->rx_buf[0];
		uint8_t ver_hi = tls->rx_buf[1];
		uint8_t ver_lo = tls->rx_buf[2];
		size_t rlen = ((size_t)tls->rx_buf[3] << 8) | tls->rx_buf[4];
		size_t total = 5 + rlen;
		const uint8_t *body = tls->rx_buf + 5;
		if (total > sizeof(tls->rx_buf)) {
			tls->state = GM_TLS_FAILED;
			return -1;
		}
		if (tls->rx_len < total)
			break;

		if (tls->state == GM_TLS_WAIT_CH) {
			if (rtype != 0x16 || rlen < 4 || body[0] != 0x01) {
				tls->state = GM_TLS_FAILED;
				return -1;
			}
			size_t hslen = ((size_t)body[1] << 16) | ((size_t)body[2] << 8) | body[3];
			if (4 + hslen != rlen || hslen < 38) {
				tls->state = GM_TLS_FAILED;
				return -1;
			}
			const uint8_t *p = body + 4, *end = body + rlen;
			if (p[0] < 3 || (p[0] == 3 && p[1] < 3)) {
				tls->state = GM_TLS_FAILED;
				return -1;
			}
			memcpy(tls->client_random, p + 2, 32);
			p += 34;
			uint8_t sid_len = *p++;
			if (p + sid_len + 2 > end) {
				tls->state = GM_TLS_FAILED;
				return -1;
			}
			p += sid_len;
			uint16_t cs_len = (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
			p += 2;
			if (cs_len < 2 || (cs_len & 1) || p + cs_len + 1 > end) {
				tls->state = GM_TLS_FAILED;
				return -1;
			}
			bool has_00ae = false, want_reneg = false;
			for (uint16_t j = 0; j < cs_len; j += 2) {
				uint16_t cs = (uint16_t)(((uint16_t)p[j] << 8) | p[j + 1]);
				if (cs == 0x00AE)
					has_00ae = true;
				else if (cs == 0x00FF)
					want_reneg = true;
			}
			p += cs_len;
			uint8_t comp_len = *p++;
			if (comp_len == 0 || p + comp_len > end) {
				tls->state = GM_TLS_FAILED;
				return -1;
			}
			bool has_null_comp = false;
			for (uint8_t j = 0; j < comp_len; j++)
				if (p[j] == 0)
					has_null_comp = true;
			p += comp_len;
			if (!has_00ae || !has_null_comp) {
				tls->state = GM_TLS_FAILED;
				return -1;
			}
			if (p + 2 <= end) {
				uint16_t ext_total = (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
				p += 2;
				if (p + ext_total <= end) {
					const uint8_t *eend = p + ext_total;
					while (p + 4 <= eend) {
						uint16_t etype = (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
						uint16_t elen = (uint16_t)(((uint16_t)p[2] << 8) | p[3]);
						p += 4;
						if (p + elen > eend)
							break;
						if (etype == 0xFF01 && elen == 1 && p[0] == 0)
							want_reneg = true;
						p += elen;
					}
				}
			}
			if (!gm_tls_hs_append(tls, body, rlen) ||
			    RAND_bytes(tls->server_random, 32) != 1) {
				tls->state = GM_TLS_FAILED;
				return -1;
			}
			/* Build ServerHello + ServerHelloDone in one 0x16 record. */
			uint8_t sh[64];
			size_t sh_body = 38 + (want_reneg ? 7 : 0);
			sh[0] = 0x02;
			sh[1] = 0x00;
			sh[2] = (uint8_t)(sh_body >> 8);
			sh[3] = (uint8_t)(sh_body & 0xFF);
			sh[4] = 0x03;
			sh[5] = 0x03;
			memcpy(sh + 6, tls->server_random, 32);
			sh[38] = 0x00;	/* session_id_len */
			sh[39] = 0x00;
			sh[40] = 0xAE;	/* TLS_PSK_WITH_AES_128_CBC_SHA256 */
			sh[41] = 0x00;	/* null compression */
			if (want_reneg) {
				sh[42] = 0x00; sh[43] = 0x05;
				sh[44] = 0xFF; sh[45] = 0x01;
				sh[46] = 0x00; sh[47] = 0x01;
				sh[48] = 0x00;
			}
			size_t sh_len = 4 + sh_body;
			static const uint8_t shd[4] = { 0x0E, 0x00, 0x00, 0x00 };
			if (!gm_tls_hs_append(tls, sh, sh_len) ||
			    !gm_tls_hs_append(tls, shd, sizeof(shd))) {
				tls->state = GM_TLS_FAILED;
				return -1;
			}
			size_t rec_body_len = sh_len + sizeof(shd);
			tx[tx_len++] = 0x16;
			tx[tx_len++] = 0x03;
			tx[tx_len++] = 0x03;
			tx[tx_len++] = (uint8_t)(rec_body_len >> 8);
			tx[tx_len++] = (uint8_t)(rec_body_len & 0xFF);
			memcpy(tx + tx_len, sh, sh_len);
			tx_len += sh_len;
			memcpy(tx + tx_len, shd, sizeof(shd));
			tx_len += sizeof(shd);
			tls->state = GM_TLS_WAIT_CKE;
		} else if (tls->state == GM_TLS_WAIT_CKE) {
			if (rtype != 0x16 || rlen < 6 || body[0] != 0x10) {
				tls->state = GM_TLS_FAILED;
				return -1;
			}
			size_t hslen = ((size_t)body[1] << 16) | ((size_t)body[2] << 8) | body[3];
			if (4 + hslen != rlen || hslen < 2) {
				tls->state = GM_TLS_FAILED;
				return -1;
			}
			uint16_t id_len = (uint16_t)(((uint16_t)body[4] << 8) | body[5]);
			size_t want_id_len = strlen(GM_TLS_IDENTITY);
			if (2 + (size_t)id_len != hslen || id_len != want_id_len ||
			    memcmp(body + 6, GM_TLS_IDENTITY, want_id_len) != 0) {
				tls->state = GM_TLS_FAILED;
				return -1;
			}
			if (!gm_tls_hs_append(tls, body, rlen)) {
				tls->state = GM_TLS_FAILED;
				return -1;
			}
			/* Derive master_secret and key_block (RFC 4279 section 2). */
			uint8_t pms[4 + 2 * GM168SEC_PSK_LEN];
			size_t pms_len = 4 + 2 * tls->psk_len;
			pms[0] = (uint8_t)(tls->psk_len >> 8);
			pms[1] = (uint8_t)(tls->psk_len & 0xFF);
			memset(pms + 2, 0, tls->psk_len);
			pms[2 + tls->psk_len] = (uint8_t)(tls->psk_len >> 8);
			pms[3 + tls->psk_len] = (uint8_t)(tls->psk_len & 0xFF);
			memcpy(pms + 4 + tls->psk_len, tls->psk, tls->psk_len);
			uint8_t seed_ms[64], seed_kb[64], key_block[96];
			memcpy(seed_ms, tls->client_random, 32);
			memcpy(seed_ms + 32, tls->server_random, 32);
			memcpy(seed_kb, tls->server_random, 32);
			memcpy(seed_kb + 32, tls->client_random, 32);
			bool ok = gm_tls_prf(pms, pms_len, "master secret", seed_ms, 64,
					     tls->master_secret, 48) &&
				  gm_tls_prf(tls->master_secret, 48, "key expansion", seed_kb, 64,
					     key_block, 96);
			memset(pms, 0, sizeof(pms));
			if (!ok) {
				memset(key_block, 0, sizeof(key_block));
				tls->state = GM_TLS_FAILED;
				return -1;
			}
			memcpy(tls->client_mac_key, key_block, 32);
			memcpy(tls->server_mac_key, key_block + 32, 32);
			memcpy(tls->client_key, key_block + 64, 16);
			memcpy(tls->server_key, key_block + 80, 16);
			memset(key_block, 0, sizeof(key_block));
			tls->state = GM_TLS_WAIT_CCS;
		} else if (tls->state == GM_TLS_WAIT_CCS) {
			if (rtype != 0x14 || rlen != 1 || body[0] != 0x01) {
				tls->state = GM_TLS_FAILED;
				return -1;
			}
			tls->client_seq = 0;
			tls->state = GM_TLS_WAIT_FIN;
		} else if (tls->state == GM_TLS_WAIT_FIN) {
			uint8_t *plain = NULL;
			size_t plain_len = 0;
			uint8_t hash[32], expect_fin[12], srv_fin[16];
			size_t fin_rec_len = 0;
			if (rtype != 0x16 ||
			    gm_tls_record_decrypt(tls, rtype, ver_hi, ver_lo, body, rlen,
						  &plain, &plain_len) != 0) {
				tls->state = GM_TLS_FAILED;
				return -1;
			}
			gm_sha256(tls->hs_buf, tls->hs_len, hash);
			bool ok = plain_len == 16 &&
				  plain[0] == 0x14 && plain[1] == 0x00 &&
				  plain[2] == 0x00 && plain[3] == 0x0C &&
				  gm_tls_prf(tls->master_secret, 48, "client finished",
					     hash, 32, expect_fin, 12) &&
				  CRYPTO_memcmp(plain + 4, expect_fin, 12) == 0 &&
				  gm_tls_hs_append(tls, plain, 16);
			memset(plain, 0, plain_len);
			free(plain);
			if (!ok) {
				tls->state = GM_TLS_FAILED;
				return -1;
			}
			/* Emit ChangeCipherSpec + encrypted Server Finished. */
			gm_sha256(tls->hs_buf, tls->hs_len, hash);
			srv_fin[0] = 0x14;
			srv_fin[1] = 0x00;
			srv_fin[2] = 0x00;
			srv_fin[3] = 0x0C;
			if (!gm_tls_prf(tls->master_secret, 48, "server finished",
					hash, 32, srv_fin + 4, 12)) {
				tls->state = GM_TLS_FAILED;
				return -1;
			}
			tls->server_seq = 0;
			tx[tx_len++] = 0x14;
			tx[tx_len++] = 0x03;
			tx[tx_len++] = 0x03;
			tx[tx_len++] = 0x00;
			tx[tx_len++] = 0x01;
			tx[tx_len++] = 0x01;
			if (!gm_tls_record_encrypt(tls, 0x16, srv_fin, sizeof(srv_fin),
						   tx + tx_len, &fin_rec_len)) {
				tls->state = GM_TLS_FAILED;
				return -1;
			}
			tx_len += fin_rec_len;
			tls->state = GM_TLS_ESTABLISHED;
		}
		memmove(tls->rx_buf, tls->rx_buf + total, tls->rx_len - total);
		tls->rx_len -= total;
		if (tls->state == GM_TLS_ESTABLISHED)
			break;
	}
	if (tx_len) {
		*out = (uint8_t *)malloc(tx_len);
		if (!*out) {
			tls->state = GM_TLS_FAILED;
			return -1;
		}
		memcpy(*out, tx, tx_len);
		*out_len = tx_len;
	}
	return tls->state == GM_TLS_ESTABLISHED ? 1 : 0;
}

/* tls.c:181-192 goodix_tls_records_whole: walk 5-byte record headers. */
static inline bool gm_tls_records_whole(const uint8_t *records, size_t len)
{
	size_t offset = 0;
	while (offset + 5 <= len)
		offset += 5 + (size_t)((records[offset + 3] << 8) | records[offset + 4]);
	return offset == len;
}

/* Decrypt application_data TLS records from the sensor. Returns malloc'd
 * plaintext or NULL when incomplete or authentication fails. */
static inline uint8_t *gm_tls_decrypt(struct gm_tls *tls, const uint8_t *records,
				      size_t records_len, size_t *out_len)
{
	uint8_t *plain = NULL;
	size_t plain_len = 0, plain_cap = 0, off = 0;
	*out_len = 0;
	if (!gm_tls_established(tls) || !records_len || !gm_tls_records_whole(records, records_len))
		return NULL;
	while (off + 5 <= records_len) {
		uint8_t rtype = records[off];
		uint8_t ver_hi = records[off + 1];
		uint8_t ver_lo = records[off + 2];
		size_t rlen = ((size_t)records[off + 3] << 8) | records[off + 4];
		uint8_t *chunk = NULL;
		size_t chunk_len = 0;
		if (rtype != 0x17 || ver_hi != 0x03 || ver_lo != 0x03)
			goto fail;
		if (gm_tls_record_decrypt(tls, rtype, ver_hi, ver_lo,
					  records + off + 5, rlen, &chunk, &chunk_len) != 0)
			goto fail;
		if (chunk_len) {
			if (plain_len + chunk_len > plain_cap) {
				size_t ncap = plain_cap ? plain_cap * 2 : 16384;
				while (ncap < plain_len + chunk_len)
					ncap *= 2;
				uint8_t *n = (uint8_t *)realloc(plain, ncap);
				if (!n) {
					memset(chunk, 0, chunk_len);
					free(chunk);
					goto fail;
				}
				plain = n;
				plain_cap = ncap;
			}
			memcpy(plain + plain_len, chunk, chunk_len);
			plain_len += chunk_len;
		}
		if (chunk) {
			memset(chunk, 0, chunk_len);
			free(chunk);
		}
		off += 5 + rlen;
	}
	if (plain_len == 0)
		goto fail;
	*out_len = plain_len;
	return plain;
fail:
	if (plain) {
		memset(plain, 0, plain_len);
		free(plain);
	}
	return NULL;
}

/* ---- PSK ----
 * Default is the all-zero PSK (the sensor ships with it). If
 * DATA_DIR/goodix53x5.psk exists it holds the imported PSK as 64 hex
 * chars (session.c goodix_load_psk; missing file means all-zero).
 * The sensor's stored hash is read from preset slot 0xbb020001 and must
 * equal SHA-256 of the held PSK (goodix_require_current_psk). A mismatch
 * is terminal: this driver never writes a PSK. */
static inline int gm_load_psk(struct goodix *s)
{
	char path[128], hex[80];
	int fd, n, i;
	memset(s->psk, 0, sizeof(s->psk));
	s->psk_len = GM168SEC_PSK_LEN;
	s->psk_from_file = false;
	snprintf(path, sizeof(path), GM168SEC_DATA_DIR "/goodix53x5.psk");
	fd = open(path, O_RDONLY);
	if (fd < 0)
		return 0;	/* missing file: all-zero PSK */
	n = (int)read(fd, hex, sizeof(hex) - 1);
	close(fd);
	if (n < 64)
		return -EINVAL;
	for (i = 0; i < 32; i++) {
		unsigned v;
		char b[3] = { hex[2*i], hex[2*i+1], 0 };
		if (sscanf(b, "%02x", &v) != 1)
			return -EINVAL;
		s->psk[i] = (uint8_t)v;
	}
	s->psk_from_file = true;
	return 0;
}
/* Preset read (gm168sec.c:287): category 0x0e, command byte 0xe4, payload
 * 16 bytes: [length LE u32][offset 0][slot LE u32][zero u32]. The reply
 * carries the stored hash in production_read layout. */
static inline int gm_preset_read_hash(struct goodix *s, uint8_t hash_out[32])
{
	uint8_t payload[16] = { 32, 0, 0, 0, 0, 0, 0, 0,
				0x01, 0x00, 0x02, 0xbb, 0, 0, 0, 0 };
	struct gm_response resp;
	int r = gm_command(s, GM_PROTO_CAT_PRODUCTION, 0x02, payload, sizeof(payload),
			   true, &resp, 1500, 1500);
	if (r < 0)
		return r;
	/* production_read reply layout (proto.c): [status(1)][slot(4 LE)][len(4 LE)][data(len)] */
	if (resp.len < 9 + 32 || resp.data[0] != 0)
		return -EBADMSG;
	uint32_t slot = (uint32_t)resp.data[1] | ((uint32_t)resp.data[2] << 8) |
			((uint32_t)resp.data[3] << 16) | ((uint32_t)resp.data[4] << 24);
	uint32_t dlen = (uint32_t)resp.data[5] | ((uint32_t)resp.data[6] << 8) |
			((uint32_t)resp.data[7] << 16) | ((uint32_t)resp.data[8] << 24);
	if (slot != GM168SEC_PSK_HASH_TYPE || dlen < 32 || resp.len < 9 + dlen)
		return -EBADMSG;
	memcpy(hash_out, resp.data + 9, 32);
	return 0;
}
static inline int gm_require_current_psk(struct goodix *s)
{
	uint8_t stored[32], mine[32];
	int r = gm_preset_read_hash(s, stored);
	if (r < 0)
		return r;
	gm_sha256(s->psk, s->psk_len, mine);
	if (memcmp(stored, mine, 32) != 0) {
		GOODIX_LOGE("533c: sensor PSK hash is not the held PSK's hash "
			    "(sensor was paired by another OS - see PORT_PLAN.md)");
		return -GM168SEC_ERR_PSK;
	}
	return 0;
}
static inline bool gm_firmware_ok(const char *fw)
{
	/* gm168sec.c goodix_wrapped_self_check: chip_platform_mode_version,
	 * platform GM168 or GM168SEC. */
	char tmp[32];
	const char *plat;
	size_t i, n = 0;
	if (!fw)
		return false;
	snprintf(tmp, sizeof(tmp), "%s", fw);
	for (i = 0; tmp[i]; i++)
		if (tmp[i] == '_')
			n++;
	if (n != 3)
		return false;
	plat = tmp;
	while (*plat && *plat != '_')
		plat++;
	plat++;
	return strncasecmp(plat, "GM168", 5) == 0 &&
	       (plat[5] == '_' || strncasecmp(plat, "GM168SEC", 8) == 0);
}

/* ---- FDT (family.h + gm168sec.c goodix_gm168sec_fdt_event) ----
 * An FDT notification is a category 0x03 message; payload[0:2] is the IRQ
 * LE u16: 0x0002 down, 0x0200 up, 0x0080/0x0082 reverse, 0x0100 manual
 * (payload[2:4] is the manual touch flag), anything else is a config
 * event. A full event is 4 + 24 (GOODIX_FDT_BASE_LEN) bytes. */
#define GM_FDT_BASE_LEN 24
#define GM_FDT_IRQ_DOWN 0x0002
#define GM_FDT_IRQ_UP 0x0200
#define GM_FDT_IRQ_REVERSE 0x0080
#define GM_FDT_IRQ_REVERSE_ALT 0x0082
static inline void gm_generate_fdt_base(const uint8_t *fdt_data, size_t len, uint8_t *fdt_base)
{
	for (size_t i = 0; i + 1 < len; i += 2) {
		uint16_t sample = (uint16_t)(fdt_data[i] | ((uint16_t)fdt_data[i + 1] << 8));
		uint16_t word = (uint16_t)(((sample & 0xFFFEu) * 0x80u) + (sample >> 1));
		fdt_base[i] = (uint8_t)(word & 0xFFu);
		fdt_base[i + 1] = (uint8_t)((word >> 8) & 0xFFu);
	}
}
static inline void gm_generate_fdt_up_base(const uint8_t *fdt_data, uint16_t touch_flag,
					   const struct gm168sec_calib *params,
					   uint8_t *fdt_base_up)
{
	for (int i = 0; i < 12; i++) {
		uint16_t sample = (uint16_t)(fdt_data[i * 2] | ((uint16_t)fdt_data[i * 2 + 1] << 8));
		uint16_t word;
		if (((touch_flag >> i) & 1u) == 0) {
			word = (uint16_t)((params->delta_up << 8) | params->delta_up);
		} else {
			uint16_t adj = (uint16_t)((sample >> 1) + params->delta_down);
			word = (uint16_t)((adj << 8) | (adj & 0xFFu));
		}
		fdt_base_up[i * 2] = (uint8_t)(word & 0xFFu);
		fdt_base_up[i * 2 + 1] = (uint8_t)((word >> 8) & 0xFFu);
	}
}
static inline int gm_refresh_fdt_base(struct goodix *s)
{
	uint8_t payload[2 + GM_FDT_BASE_LEN];
	struct gm_response resp;
	int r;
	payload[0] = 0x0D; /* FDT_MANUAL with TX enabled (commands.c:633) */
	payload[1] = 1;
	memcpy(payload + 2, s->fdt_base_down, GM_FDT_BASE_LEN);
	r = gm_command(s, GM_PROTO_CAT_FDT, GM_PROTO_CMD_FDT_MANUAL,
		       payload, sizeof(payload), true, &resp, 1500, 1500);
	if (r < 0)
		return r;
	if (resp.len < 4 + GM_FDT_BASE_LEN)
		return -EBADMSG;
	gm_generate_fdt_base(resp.data + 4, GM_FDT_BASE_LEN, s->fdt_base_down);
	s->fdt_base_valid = true;
	return 0;
}
static inline int gm_arm_fdt(struct goodix *s, bool want_down)
{
	uint8_t payload[2 + GM_FDT_BASE_LEN];
	struct gm_response resp;
	if (want_down && !s->fdt_base_valid)
		gm_refresh_fdt_base(s);
	payload[0] = want_down ? 0x0C : 0x0E; /* commands.c:445 */
	payload[1] = 1;
	memcpy(payload + 2, want_down ? s->fdt_base_down : s->fdt_base_up, GM_FDT_BASE_LEN);
	return gm_command(s, GM_PROTO_CAT_FDT,
			  want_down ? GM_PROTO_CMD_FDT_DOWN : GM_PROTO_CMD_FDT_UP,
			  payload, sizeof(payload), false, &resp, 1500, 1500);
}
/* Difference between CLOCK_BOOTTIME (ticks during suspend) and CLOCK_MONOTONIC
 * (pauses during suspend) in milliseconds. A jump >= 250 ms proves the host
 * went through kernel suspend (s2idle / S3), during which the 533c MCU drops
 * out of FDT_DOWN armed state even when USB stays connected. */
static inline int64_t gm_suspend_offset_ms(void)
{
	struct timespec tb, tm;
	if (clock_gettime(CLOCK_BOOTTIME, &tb) != 0 ||
	    clock_gettime(CLOCK_MONOTONIC, &tm) != 0)
		return 0;
	return ((int64_t)tb.tv_sec - (int64_t)tm.tv_sec) * 1000 +
	       ((int64_t)tb.tv_nsec - (int64_t)tm.tv_nsec) / 1000000;
}
static inline int gm_wait_fdt(struct goodix *s, bool want_down, int timeout_ms)
{
	uint8_t pack[GM_PACK_MAX_PAYLOAD + GM_PACK_HEADER];
	uint16_t want = want_down ? GM_FDT_IRQ_DOWN : GM_FDT_IRQ_UP;
	int64_t suspend_off0 = gm_suspend_offset_ms();
	int waited = 0;
	if (goodix_cancel_requested(s))
		return -ECANCELED;
	int arm_r = gm_arm_fdt(s, want_down);
	if (arm_r < 0)
		return arm_r;
	while (timeout_ms < 0 || waited < timeout_ms) {
		uint8_t flags;
		size_t plen = sizeof(pack) - GM_PACK_HEADER;
		int r = gm_read_pack(s, &flags, pack, &plen, 100);
		if (r == -ECANCELED)
			return -ECANCELED;
		if (r == -ETIMEDOUT) {
			if (want_down && timeout_ms < 0) {
				int64_t slept_ms = gm_suspend_offset_ms() - suspend_off0;
				if (slept_ms >= 250) {
					GOODIX_LOGI("533c: system resumed from suspend (+%lld ms); reinitializing sensor",
						    (long long)slept_ms);
					return GOODIX_REOPEN;
				}
			}
			waited += 100;
			continue;
		}
		if (r < 0)
			return r;
		if ((flags >> 4) == GM_PACK_FAMILY_TLS) {
			/* TLS traffic while waiting: stash for the TLS layer. */
			const uint8_t *rec = pack;
			size_t reclen = plen;
			if (flags & 0x02) {
				if (reclen < 9)
					continue;
				rec += 9;
				reclen -= 9;
			}
			if (s->tls_pending_len + reclen <= sizeof(s->tls_pending)) {
				memcpy(s->tls_pending + s->tls_pending_len, rec, reclen);
				s->tls_pending_len += reclen;
			}
			continue;
		}
		/* Unwrap one protocol message from the pack and check it. */
		struct gm_rx rx;
		const uint8_t *pl;
		size_t pl_len;
		uint8_t cat, cmd;
		size_t off = plen < GM168SEC_USB_CHUNK_SIZE ? plen : GM168SEC_USB_CHUNK_SIZE;
		uint8_t cell[GM168SEC_USB_CHUNK_SIZE];
		gm_rx_reset(&rx);
		rx.buf = NULL;
		if (!gm_rx_feed_cell(&rx, pack, off)) {
			free(rx.buf);
			continue;
		}
		while (off < plen) {
			size_t n = plen - off < GM168SEC_USB_CHUNK_SIZE - 1 ?
				   plen - off : GM168SEC_USB_CHUNK_SIZE - 1;
			cell[0] = (uint8_t)(pack[0] | 1);
			memcpy(cell + 1, pack + off, n);
			if (!gm_rx_feed_cell(&rx, cell, n + 1)) {
				free(rx.buf);
				rx.buf = NULL;
				break;
			}
			off += n;
		}
		if (!rx.buf || !gm_rx_parse(&rx, &cat, &cmd, &pl, &pl_len)) {
			free(rx.buf);
			continue;
		}
		if (cat == GM_PROTO_CAT_FDT && pl_len >= 2) {
			uint16_t irq = (uint16_t)(pl[0] | ((uint16_t)pl[1] << 8));
			if (irq == want) {
				if (pl_len >= 4 + GM_FDT_BASE_LEN) {
					if (want_down) {
						uint16_t tf = (uint16_t)(pl[2] | ((uint16_t)pl[3] << 8));
						gm_generate_fdt_up_base(pl + 4, tf, &s->calib, s->fdt_base_up);
					} else {
						gm_generate_fdt_base(pl + 4, GM_FDT_BASE_LEN, s->fdt_base_down);
						s->fdt_base_valid = true;
					}
				}
				free(rx.buf);
				return 0;
			}
			if ((irq == GM_FDT_IRQ_REVERSE || irq == GM_FDT_IRQ_REVERSE_ALT) && want_down) {
				GOODIX_LOGI("533c: FDT reverse event while waiting for down; re-arming");
				if (pl_len >= 4 + GM_FDT_BASE_LEN) {
					gm_generate_fdt_base(pl + 4, GM_FDT_BASE_LEN, s->fdt_base_down);
					s->fdt_base_valid = true;
				}
				free(rx.buf);
				gm_arm_fdt(s, true);
				continue;
			}
		}
		free(rx.buf);
	}
	return -ETIMEDOUT;
}

/* ---- TLS handshake driver ----
 * Send the TLS request (cat 0x0d cmd 0x00, {0,0}); the sensor ACKs and
 * sends its ClientHello as a TLS pack. Drive gm_tls_handshake_step until
 * established, sending each output flight framed as 0xb0 packs, one
 * record per pack (channel.c goodix_tls_send_handler). */
static inline int gm_tls_handshake(struct goodix *s)
{
	struct gm_tls *tls = (struct gm_tls *)s->tls;
	struct gm_response resp;
	uint8_t pack[GM_PACK_MAX_PAYLOAD + GM_PACK_HEADER];
	int r;
	if (!tls)
		return -EINVAL;
	r = gm_command(s, GM_PROTO_CAT_MCU, 0x00, (const uint8_t[]){ 0, 0 }, 2,
		       false, &resp, 1500, 1500);
	if (r < 0)
		return r;
	for (;;) {
		uint8_t *out = NULL;
		size_t out_len = 0;
		/* Drain anything stashed while commands were outstanding. */
		const uint8_t *in = NULL;
		size_t in_len = 0;
		if (s->tls_pending_len) {
			in = s->tls_pending;
			in_len = s->tls_pending_len;
			s->tls_pending_len = 0;
		} else {
			uint8_t flags;
			size_t plen = sizeof(pack) - GM_PACK_HEADER;
			r = gm_read_pack(s, &flags, pack, &plen, 5000);
			if (r < 0)
				return r;
			if ((flags >> 4) != GM_PACK_FAMILY_TLS)
				continue;	/* not handshake traffic; ignore */
			in = pack;
			in_len = plen;
			if (flags & 0x02) {
				if (in_len < 9)
					continue;
				in += 9;
				in_len -= 9;
			}
		}
		int hs = gm_tls_handshake_step(tls, in, in_len, &out, &out_len);
		if (hs < 0) {
			free(out);
			return -EIO;
		}
		if (out_len) {
			/* One pack per TLS record (channel.c:74-79). */
			size_t off = 0;
			while (off + 5 <= out_len) {
				size_t reclen = 5 + (size_t)((out[off + 3] << 8) | out[off + 4]);
				uint8_t *framed;
				size_t flen;
				if (off + reclen > out_len)
					break;
				framed = gm_frame_pack(GM_PACK_TLS, out + off, reclen, &flen);
				if (!framed) {
					free(out);
					return -ENOMEM;
				}
				int wr = gm_bulk(s, GM168SEC_EP_OUT, framed, (unsigned)flen, 1500);
				free(framed);
				if (wr < 0) {
					free(out);
					return -GOODIX_ERRNO;
				}
				/* TOD's write and send callback sleep 1 + 2 ms between
				 * TLS records (channel.c:70-71); without it the sensor
				 * drops consecutive records and times out. */
				usleep(3000);
				off += reclen;
			}
			free(out);
		}
		if (hs == 1)
			return 0;
		/* r == 0: the sensor's next flight is awaited; an empty
		 * 50 ms wait counts against the 10-call budget inside
		 * gm_tls_handshake_step (tls.c:141). */
	}
}

/* ---- bring-up (family/gm168sec/open.c + session.c open flow) ----
 * probe -> firmware check -> PSK read/check -> reset -> chip ID ->
 * OTP read/parse -> TLS channel -> restore config -> arm FDT down.
 * Returns 0 or a negative errno; GM168SEC_ERR_PSK and
 * GM168SEC_ERR_FIRMWARE are the two terminal, logged distinctly. */
static inline void goodix_close(struct goodix *s);
static inline int goodix_open(struct goodix *s)
{
	struct gm_response resp;
	uint8_t cfg[256];
	int r;
	if (s->fd >= 0)
		return 0;
	if ((r = gm_find(s)) < 0)
		return r;
	gm_unbind_cdc_acm(s);
	s->fd = open(s->path, O_RDWR);
	if (s->fd < 0)
		return -GOODIX_ERRNO;
	if (ioctl(s->fd, GM168SEC_USB_CLAIMINTERFACE, &(int){ GM168SEC_INTERFACE }) < 0) {
		r = -GOODIX_ERRNO;
		GOODIX_LOGW("533c: claim interface 0 failed: %d (cdc_acm may still own it)", r);
		close(s->fd);
		s->fd = -1;
		return r;
	}
	/* Drain any stale IN packets left over from an aborted transfer. */
	for (int d = 0; d < 4; d++) {
		uint8_t junk[GM168SEC_USB_CHUNK_SIZE];
		if (gm_bulk(s, GM168SEC_EP_IN, junk, sizeof(junk), 20) <= 0)
			break;
	}
	/* PROBE + FIRMWARE: category 0x0a command 4, 2 zero bytes. */
	r = gm_command(s, GM_PROTO_CAT_SYSTEM, 0x04, (const uint8_t[]){ 0, 0 }, 2,
		       true, &resp, 1500, 1500);
	if (r < 0)
		goto fail;
	snprintf(s->fw_version, sizeof(s->fw_version), "%.*s",
		 (int)(resp.len < sizeof(s->fw_version) - 1 ? resp.len : sizeof(s->fw_version) - 1),
		 resp.data);
	if (!gm_firmware_ok(s->fw_version)) {
		GOODIX_LOGE("533c: firmware '%s' is not GM168/GM168SEC", s->fw_version);
		r = -GM168SEC_ERR_FIRMWARE;
		goto fail;
	}
	GOODIX_LOGI("533c: firmware %s", s->fw_version);
	/* PSK_READ + PSK_CHECK. */
	if ((r = gm_load_psk(s)) < 0)
		goto fail;
	if ((r = gm_require_current_psk(s)) < 0)
		goto fail;
	/* RESET (request IRQ) + 10 ms settle (open.c:113) + CHIP ID. */
	r = gm_command(s, GM_PROTO_CAT_SYSTEM, 0x01, (const uint8_t[]){ 0x05, 0x14 }, 2,
		       true, &resp, 1500, 1500);
	if (r < 0)
		goto fail;
	usleep(10000);
	r = gm_command(s, GM_PROTO_CAT_REGISTER, 0x01,
		       (const uint8_t[]){ 0, 0, 0, 0x04, 0 }, 5,
		       true, &resp, 1500, 1500);
	if (r < 0)
		goto fail;
	if (resp.len >= 4) {
		uint32_t chip = gm_decode_u32(resp.data);
		GOODIX_LOGI("533c: chip id 0x%08x", chip);
		if ((chip & 0xFFFFFF00u) != 0x00220c00u)
			GOODIX_LOGW("533c: chip id 0x%08x is outside the 0x00220cxx family", chip);
	}
	/* OTP READ + PARSE. (OTP hash verification, goodix_device_verify_otp,
	 * is not ported yet: the OTP bytes are used as read. See PORT_PLAN.) */
	r = gm_command(s, GM_PROTO_CAT_SYSTEM, 0x03, (const uint8_t[]){ 0, 0 }, 2,
		       true, &resp, 1500, 1500);
	if (r < 0)
		goto fail;
	s->otp_len = resp.len < sizeof(s->otp) ? resp.len : sizeof(s->otp);
	memcpy(s->otp, resp.data, s->otp_len);
	gm_parse_otp(s->otp, s->otp_len, &s->calib);
	s->default_dac = s->calib.dac_h;
	GOODIX_LOGI("533c: tcode 0x%x dac_h 0x%x dac_l 0x%x (otp: %s)",
		    s->calib.tcode, s->calib.dac_h, s->calib.dac_l,
		    s->calib.dac_from_otp ? "yes" : "defaults");
	/* TLS CHANNEL. */
	s->tls = gm_tls_new(s->psk, s->psk_len);
	if (!s->tls) {
		r = -ENOMEM;
		goto fail;
	}
	if ((r = gm_tls_handshake(s)) < 0) {
		GOODIX_LOGE("533c: TLS handshake failed: %d", r);
		goto fail;
	}
	GOODIX_LOGI("533c: TLS 1.2-PSK established");
	/* RESTORE CONFIG: default blob, patched with the OTP calibration.
	 * Config download returns ACK + 2-byte data reply (commands.c:347). */
	memcpy(cfg, gm168sec_default_config, sizeof(cfg));
	gm_patch_config(cfg, sizeof(cfg), &s->calib);
	r = gm_command(s, GM_PROTO_CAT_CONFIG, GM_PROTO_CMD_CONFIG_DOWNLOAD,
		       cfg, sizeof(cfg), true, &resp, 1500, 1500);
	if (r < 0)
		goto fail;
	/* Seed initial FDT baseline via FDT_MANUAL (TX on) without entering
	 * FDT wait mode yet, so setup-frame capture can run immediately. */
	memset(s->fdt_base_down, 0, sizeof(s->fdt_base_down));
	memset(s->fdt_base_up, 0, sizeof(s->fdt_base_up));
	s->fdt_base_valid = false;
	if ((r = gm_refresh_fdt_base(s)) < 0)
		GOODIX_LOGW("533c: initial FDT_MANUAL failed (%d), will retry on arm", r);
	return 0;
fail:
	goodix_close(s);
	return r;
}
static inline void goodix_close(struct goodix *s)
{
	if (s->fd >= 0) {
		struct gm_response resp;
		/* Sleep mode (commands.c:674): cat 0x06 cmd 0, {0x01,0x00}. */
		gm_command(s, 0x06, 0x00, (const uint8_t[]){ 0x01, 0x00 }, 2,
			   false, &resp, 500, 500);
		ioctl(s->fd, GM168SEC_USB_RELEASEINTERFACE, &(int){ GM168SEC_INTERFACE });
		close(s->fd);
		s->fd = -1;
	}
	if (s->tls) {
		gm_tls_free((struct gm_tls *)s->tls);
		s->tls = NULL;
	}
	s->rx_len = 0;
	s->tls_pending_len = 0;
}
/* After a cancel: drain stale packs and switch the power-button shield
 * off (upstream goodix_settle; the 533c equivalent is McuSetEcCtrl off -
 * three zero bytes, family.h - sent by the capture loop's teardown;
 * the exact command is listed as TO-VERIFY in PORT_PLAN.md). */
static inline int goodix_settle(struct goodix *s)
{
	uint8_t pack[GM_PACK_MAX_PAYLOAD + GM_PACK_HEADER];
	size_t plen;
	uint8_t flags;
	int r;
	if (s->fd < 0)
		return 0;
	for (int i = 0; i < 4; i++) {
		plen = sizeof(pack) - GM_PACK_HEADER;
		r = gm_read_pack(s, &flags, pack, &plen, 50);
		if (r != 0)
			break;
		GOODIX_LOGI("533c: settle discarded a stale pack (flags 0x%02x)", flags);
	}
	return 0;
}

/* ---- capture ----
 * One frame: wait for finger-down, request the image
 * (cat 0x02 cmd 0x00, [op][hv][dac LE] per goodix_build_image_request),
 * read the 0xb2 image pack, strip the 9-byte prefix, TLS-decrypt,
 * validate the CRC and unpack to u16[9504]. The setup (background)
 * frame uses is_finger=0; live frames use is_finger=1. */
static inline int gm_capture_frame(struct goodix *s, bool is_finger,
				   uint16_t dac, uint16_t *out_u16)
{
	struct gm_response resp;
	uint8_t img_req[4];
	uint8_t op_code = 0x01;	/* tx on */
	uint8_t hv_value = 0x06;	/* GOODIX_HV_VALUE (commands.c:34): hv_enable=TRUE */
	int r;
	if (is_finger)
		op_code |= 0x40;
	img_req[0] = op_code;
	img_req[1] = hv_value;
	img_req[2] = (uint8_t)(dac & 0xFF);
	img_req[3] = (uint8_t)((dac >> 8) & 0xFF);
	r = gm_command(s, GM_PROTO_CAT_IMAGE, GM_PROTO_CMD_IMAGE,
		       img_req, sizeof(img_req), false, &resp, 1500, 5000);
	if (r < 0)
		return r;
	/* The image arrives as a TLS pack, not as the command reply: the
	 * reply above is the ACK; the frame follows on the IN stream. */
	for (;;) {
		uint8_t pack[GM_PACK_MAX_PAYLOAD + GM_PACK_HEADER];
		uint8_t flags;
		size_t plen = sizeof(pack) - GM_PACK_HEADER;
		r = gm_read_pack(s, &flags, pack, &plen, 5000);
		if (r < 0)
			return r;
		if ((flags >> 4) != GM_PACK_FAMILY_TLS)
			continue;
		const uint8_t *rec = pack;
		size_t reclen = plen;
		if (flags & 0x02) {
			if (reclen < GM_PACK_TLS_DATA_HDR)
				continue;
			rec += GM_PACK_TLS_DATA_HDR;
			reclen -= GM_PACK_TLS_DATA_HDR;
		}
		size_t plain_len = 0;
		uint8_t *plain = gm_tls_decrypt((struct gm_tls *)s->tls, rec, reclen, &plain_len);
		if (!plain)
			return -EIO;
		if (!gm_wrapped_image_valid(plain, plain_len)) {
			free(plain);
			return GOODIX_RETRY;	/* bad frame, not a bad finger */
		}
		gm_decode_raw12(plain, out_u16);
		free(plain);
		return 0;
	}
}
static inline void goodix_emit(const struct goodix_ev *ev, int kind, int value)
{
	if (ev && ev->fn)
		ev->fn(ev->ctx, kind, value);
}

/* ---- host template files ----
 * Templates are Milan blobs, one file per GBOS name. The policy header
 * classifies by name, so Windows/fprintd templates (which live in
 * /var/lib/fprint, not here) can never be deleted by reconcile. */
static inline int gm_tpl_path(char *out, size_t out_len, const char *uid)
{
	return snprintf(out, out_len, GM168SEC_DATA_DIR "/" GM168SEC_TPL_PREFIX "%s" GM168SEC_TPL_SUFFIX, uid);
}
static inline int gm_tpl_save(const char *uid, const uint8_t *tpl, size_t tpl_len)
{
	char path[160];
	int fd;
	gm_tpl_path(path, sizeof(path), uid);
	fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
	if (fd < 0)
		return -GOODIX_ERRNO;
	if (write(fd, tpl, tpl_len) != (ssize_t)tpl_len) {
		close(fd);
		return -EIO;
	}
	if (fsync(fd) != 0) {
		close(fd);
		return -EIO;
	}
	close(fd);
	return 0;
}
static inline int gm_tpl_load(const char *uid, uint8_t **tpl, size_t *tpl_len)
{
	char path[160];
	struct stat st;
	int fd;
	gm_tpl_path(path, sizeof(path), uid);
	fd = open(path, O_RDONLY);
	if (fd < 0)
		return -GOODIX_ERRNO;
	if (fstat(fd, &st) != 0 || st.st_size <= 0 || st.st_size > 1024 * 1024) {
		close(fd);
		return -EBADMSG;
	}
	*tpl = (uint8_t *)malloc((size_t)st.st_size);
	if (!*tpl) {
		close(fd);
		return -ENOMEM;
	}
	if (read(fd, *tpl, (size_t)st.st_size) != st.st_size) {
		free(*tpl);
		close(fd);
		return -EIO;
	}
	close(fd);
	*tpl_len = (size_t)st.st_size;
	return 0;
}
static inline int goodix_list_templates(struct goodix *s, struct goodix_template *templates,
					size_t *count, int *fail)
{
	DIR *d = opendir(GM168SEC_DATA_DIR);
	struct dirent *e;
	size_t n = 0;
	(void)s;
	*fail = 0;
	if (!d)
		return -GOODIX_ERRNO;
	while ((e = readdir(d)) != NULL && n < GM168SEC_MAX_TEMPLATES) {
		const char *name = e->d_name;
		size_t len = strlen(name);
		size_t pre = strlen(GM168SEC_TPL_PREFIX), suf = strlen(GM168SEC_TPL_SUFFIX);
		if (len <= pre + suf || memcmp(name, GM168SEC_TPL_PREFIX, pre) != 0 ||
		    memcmp(name + len - suf, GM168SEC_TPL_SUFFIX, suf) != 0)
			continue;
		size_t uid_len = len - pre - suf;
		if (uid_len > 32)
			continue;
		memcpy(templates[n].uid, name + pre, uid_len);
		templates[n].uid[uid_len] = 0;
		templates[n].uid_len = uid_len;
		n++;
	}
	closedir(d);
	*count = n;
	return GOODIX_OK;
}
static inline int goodix_delete(struct goodix *s, const char *uid, int *fail)
{
	char path[160];
	(void)s;
	*fail = 0;
	gm_tpl_path(path, sizeof(path), uid);
	if (unlink(path) != 0)
		return GOODIX_ERRNO == ENOENT ? GOODIX_NORECORD : -GOODIX_ERRNO;
	return GOODIX_OK;
}
static inline int goodix_delete_template(struct goodix *s, const struct goodix_template *t, int *fail)
{
	return goodix_delete(s, t->uid, fail);
}

/* ---- matcher interface ----
 * The Milan runtime (milan/milan_matcher.c) implements these three
 * functions; milan_matcher_stub.c implements them as
 * GM168SEC_ERR_NO_MATCHER. build.sh links exactly one of the two.
 * The gallery is the caller's stored templates, in the same order as
 * @uids. enroll_sample returns 0 (sample accepted), GOODIX_RETRY
 * (bad image: lift and touch again), GOODIX_DUPLICATE (dup_uid set to
 * the stored template this finger already matches), or a negative
 * errno. The runtime's own verdict is authoritative: the HAL uses
 * output->status (MATCH/NO_MATCH/RETRY), not a hand-rolled score
 * threshold (milan/match/policy.c holds the threshold tables). */
int gm_matcher_check_setup(struct goodix *s, const uint16_t *setup);
int gm_matcher_enroll_begin(struct goodix *s, const char *const *uids,
			    const uint8_t *const *tpls, const size_t *tpl_lens, int n);
int gm_matcher_enroll_sample(struct goodix *s, const uint16_t *setup,
			     const uint16_t *live, int *quality, int *coverage,
			     char *dup_uid);
int gm_matcher_enroll_finish(struct goodix *s, uint8_t **tpl, size_t *tpl_len);
int gm_matcher_match(struct goodix *s, const uint16_t *setup,
		     const uint16_t *live, const char *const *uids,
		     const uint8_t *const *tpls, const size_t *tpl_lens, int n,
		     int *winner);

/* ---- enroll / identify ----
 * Enroll: one setup (background) frame, then 12 live samples
 * (GOODIX_ENROLL_SAMPLES, driver-private.h:49). Progress events carry
 * the stage (0..12); a bad image is GOODIX_RETRY (lift and touch again).
 * The template is written to disk only after enroll_finish succeeds;
 * the duplicate check runs inside the matcher against the gallery.
 * Identify: one setup frame + one live frame, matched against the
 * gallery. One touch is one attempt (the 6384 boot-13 lesson): after
 * the result, wait for finger-up before returning so a still-down
 * finger cannot produce five rapid failures. */
static inline int gm_load_gallery(const char *const *uids, int n,
				  uint8_t ***tpls_out, size_t **lens_out)
{
	uint8_t **tpls = (uint8_t **)calloc((size_t)(n > 0 ? n : 1), sizeof(uint8_t *));
	size_t *lens = (size_t *)calloc((size_t)(n > 0 ? n : 1), sizeof(size_t));
	if (!tpls || !lens) {
		free(tpls);
		free(lens);
		return -ENOMEM;
	}
	for (int i = 0; i < n; i++) {
		if (gm_tpl_load(uids[i], &tpls[i], &lens[i]) != 0) {
			tpls[i] = NULL;
			lens[i] = 0;
		}
	}
	*tpls_out = tpls;
	*lens_out = lens;
	return 0;
}
static inline void gm_free_gallery(uint8_t **tpls, size_t *lens, int n)
{
	if (!tpls)
		return;
	for (int i = 0; i < n; i++)
		free(tpls[i]);
	free(tpls);
	free(lens);
}
/* Capture an empty-sensor background setup frame and verify it passes Milan's
 * setup admission gate. If the user's finger is still resting on the sensor
 * (e.g. transitioning from Settings' "Find the sensor" screen or back-to-back
 * authentication), wait in 50 ms cancel-responsive slices and recapture. */
static inline int gm_capture_valid_setup(struct goodix *s, uint16_t *setup)
{
	for (int tries = 0; tries < 20; tries++) {
		int r;
		if (goodix_cancel_requested(s))
			return -ECANCELED;
		r = gm_capture_frame(s, false, s->calib.dac_l, setup);
		if (r == GOODIX_RETRY) {
			for (int w = 0; w < 2; w++) {
				if (goodix_cancel_requested(s))
					return -ECANCELED;
				usleep(50 * 1000);
			}
			continue;
		}
		if (r < 0)
			return r;
		if (gm_matcher_check_setup(s, setup) == 0) {
			if (tries > 0)
				gm_refresh_fdt_base(s);
			return 0;
		}
		if (tries == 0)
			GOODIX_LOGI("533c: finger still on sensor during setup capture; waiting for lift");
		for (int w = 0; w < 5; w++) {
			if (goodix_cancel_requested(s))
				return -ECANCELED;
			usleep(50 * 1000);
		}
	}
	return 0;
}
static inline int goodix_enroll(struct goodix *s, const char *uid,
				const struct goodix_ev *ev, int *fail,
				struct goodix_template *duplicate)
{
	uint16_t *setup = NULL, *live = NULL;
	struct goodix_template all[GM168SEC_MAX_TEMPLATES];
	size_t nall = 0;
	const char *uids[GM168SEC_MAX_TEMPLATES];
	uint8_t **tpls = NULL;
	size_t *lens = NULL;
	int r, stage = 0;
	*fail = 0;
	memset(duplicate, 0, sizeof(*duplicate));
	setup = (uint16_t *)calloc(GM168SEC_SENSOR_PIXELS, sizeof(uint16_t));
	live = (uint16_t *)calloc(GM168SEC_SENSOR_PIXELS, sizeof(uint16_t));
	if (!setup || !live) {
		free(setup);
		free(live);
		return -ENOMEM;
	}
	if (goodix_list_templates(s, all, &nall, fail) != GOODIX_OK)
		nall = 0;
	/* Template store full: the upstream MOC backend reports
	 * GOODIX_NO_SPACE here (goodix.c:609-610); the 533c store is
	 * files, so the count check lives here instead of in the sensor. */
	if (nall >= GM168SEC_MAX_TEMPLATES) {
		*fail = GOODIX_NO_SPACE;
		free(setup);
		free(live);
		return GOODIX_FAILED;
	}
	for (size_t i = 0; i < nall; i++)
		uids[i] = all[i].uid;
	if (gm_load_gallery(uids, (int)nall, &tpls, &lens) < 0) {
		free(setup);
		free(live);
		return -ENOMEM;
	}
	r = gm_matcher_enroll_begin(s, uids, (const uint8_t *const *)tpls,
				    (const size_t *)lens, (int)nall);
	if (r < 0)
		goto out;
	/* Setup (background) frame: no finger on the sensor, uses dac_l (base.c:738).
	 * Do not emit GOODIX_EV_CAPTURE here — the user has not touched yet. */
	if ((r = gm_capture_valid_setup(s, setup)) != 0)
		goto out;
	for (stage = 0; stage < GM168SEC_ENROLL_SAMPLES;) {
		int quality = 0, coverage = 0;
		char dup[33] = { 0 };
		if ((r = gm_wait_fdt(s, true, -1)) < 0)
			goto out;
		goodix_emit(ev, GOODIX_EV_CAPTURE, 0);
		r = gm_capture_frame(s, true, s->calib.dac_h, live);
		if (r == GOODIX_RETRY) {
			goodix_emit(ev, GOODIX_EV_RETRY, stage);
			if (gm_wait_fdt(s, false, 5000) == -ECANCELED) {
				r = -ECANCELED;
				goto out;
			}
			continue;
		}
		if (r < 0)
			goto out;
		r = gm_matcher_enroll_sample(s, setup, live, &quality, &coverage, dup);
		if (r == GOODIX_RETRY) {
			goodix_emit(ev, GOODIX_EV_RETRY, stage);
			if (gm_wait_fdt(s, false, 5000) == -ECANCELED) {
				r = -ECANCELED;
				goto out;
			}
			if (gm_matcher_check_setup(s, setup) != 0) {
				if ((r = gm_capture_valid_setup(s, setup)) != 0)
					goto out;
			}
			continue;
		}
		if (r == GOODIX_DUPLICATE) {
			size_t dl = strlen(dup);
			if (dl > 32)
				dl = 32;
			memcpy(duplicate->uid, dup, dl);
			duplicate->uid[dl] = 0;
			duplicate->uid_len = dl;
			*fail = 0;
			r = GOODIX_DUPLICATE;
			goto out;
		}
		if (r < 0)
			goto out;
		GOODIX_LOGI("533c: enroll stage %d: quality %d coverage %d",
			    stage, quality, coverage);
		stage++;
		goodix_emit(ev, GOODIX_EV_PROGRESS, stage);
		/* Lift the finger between samples. */
		if (stage < GM168SEC_ENROLL_SAMPLES && gm_wait_fdt(s, false, 5000) == -ECANCELED) {
			r = -ECANCELED;
			goto out;
		}
	}
	{
		uint8_t *tpl = NULL;
		size_t tpl_len = 0;
		r = gm_matcher_enroll_finish(s, &tpl, &tpl_len);
		if (r < 0)
			goto out;
		r = gm_tpl_save(uid, tpl, tpl_len);
		free(tpl);
		if (r < 0) {
			r = -GOODIX_ERRNO;
			goto out;
		}
	}
	r = GOODIX_OK;
out:
	gm_free_gallery(tpls, lens, (int)nall);
	free(setup);
	free(live);
	return r;
}
static inline int goodix_identify(struct goodix *s, const char *const *uids, int count,
				  const struct goodix_ev *ev, char *matched, int *fail)
{
	uint16_t *setup = NULL, *live = NULL;
	uint8_t **tpls = NULL;
	size_t *lens = NULL;
	int r, winner = -1;
	bool touched = false;
	*fail = 0;
	matched[0] = 0;
	if (count <= 0)
		return GOODIX_NOMATCH;
	setup = (uint16_t *)calloc(GM168SEC_SENSOR_PIXELS, sizeof(uint16_t));
	live = (uint16_t *)calloc(GM168SEC_SENSOR_PIXELS, sizeof(uint16_t));
	if (!setup || !live) {
		free(setup);
		free(live);
		return -ENOMEM;
	}
	if (gm_load_gallery(uids, count, &tpls, &lens) < 0) {
		free(setup);
		free(live);
		return -ENOMEM;
	}
	if ((r = gm_capture_valid_setup(s, setup)) != 0)
		goto out;
	if ((r = gm_wait_fdt(s, true, -1)) < 0)
		goto out;
	touched = true;
	goodix_emit(ev, GOODIX_EV_CAPTURE, 0);
	r = gm_capture_frame(s, true, s->calib.dac_h, live);
	if (r == GOODIX_RETRY) {
		goodix_emit(ev, GOODIX_EV_RETRY, 0);
		r = GOODIX_RETRY;
		goto out;
	}
	if (r < 0)
		goto out;
	r = gm_matcher_match(s, setup, live, uids, (const uint8_t *const *)tpls,
			     (const size_t *)lens, count, &winner);
	if (r < 0)
		goto out;
	if (r == 0 && winner >= 0 && winner < count) {
		snprintf(matched, 33, "%s", uids[winner]);
		r = GOODIX_OK;
	} else {
		r = GOODIX_NOMATCH;
	}
out:
	/* One touch, one attempt: on reject or retry wait for finger-up so
	 * a still-down finger cannot produce 5 rapid failures. Return
	 * immediately on match or cancel. */
	if (touched && (r == GOODIX_NOMATCH || r == GOODIX_RETRY) && s->fd >= 0) {
		if (gm_wait_fdt(s, false, 3000) == -ECANCELED)
			r = -ECANCELED;
	}
	gm_free_gallery(tpls, lens, count);
	free(setup);
	free(live);
	return r;
}

#endif /* GBOS_GM168SEC_HAL_H */
