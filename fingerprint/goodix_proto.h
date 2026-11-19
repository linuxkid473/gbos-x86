/* SPDX-License-Identifier: LGPL-2.1-or-later */
/*
 * Goodix MOC packet format, based on libfprint's goodixmoc driver:
 * https://gitlab.freedesktop.org/libfprint/libfprint/-/tree/v1.94.9/libfprint/drivers/goodixmoc
 * Copyright (C) 2019 Shenzhen Goodix Technology Co., Ltd.
 */
#ifndef GBOS_GOODIX_PROTO_H
#define GBOS_GOODIX_PROTO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#define GOODIX_HEADER_SIZE 8
#define GOODIX_CRC_SIZE 4
#define GOODIX_MAX_PAYLOAD 2048
#define GOODIX_TEMPLATE_ID_SIZE 32
#define GOODIX_MAX_UID 56
#define GOODIX_MAX_TEMPLATES 20
#define GOODIX_ENROLL_STAGES 12

#define GOODIX_SUCCESS 0x00
#define GOODIX_ERROR_THRESHOLD 0x80
#define GOODIX_NO_RECORD 0x9c
#define GOODIX_TEMPLATE_INCOMPLETE 0xb8
#define GOODIX_WAIT_FINGER_UP_TIMEOUT 0xc7
#define GOODIX_NO_SPACE 0x8f

#define GOODIX_CMD_RESPONSE 0xaa
#define GOODIX_CMD_ENROLL 0xa0
#define GOODIX_CMD_ENROLL_INIT 0xa1
#define GOODIX_CMD_CAPTURE 0xa2
#define GOODIX_CMD_DUPLICATE 0xa3
#define GOODIX_CMD_COMMIT 0xa4
#define GOODIX_CMD_IDENTIFY 0xa5
#define GOODIX_CMD_LIST 0xa6
#define GOODIX_CMD_DELETE 0xa7
#define GOODIX_CMD_FINGER_MODE 0xb0
#define GOODIX_CMD_CONFIG 0xc0
#define GOODIX_CMD_VERSION 0xd0
#define GOODIX_CMD_POWER_BUTTON_SHIELD 0xe0

#define GOODIX_CMD_WRITE_CFG_TO_FLASH 0x01
/* a7:00 deletes one template (libfprint gx_fp_template_delete: MOC_CMD1_DEFAULT). a7:04 (MOC_CMD1_DELETE_TEMPLATE,
 * defined but never sent by libfprint) is acknowledged with 0x00 and deletes nothing on firmware 01010264
 * (boot-12). a7:01 deletes every template on the sensor, Windows Hello's too: never sent, so not defined here. */
#define GOODIX_CMD_DELETE_ONE 0x00
#define GOODIX_CMD_FINGER_UP 0x02
#define GOODIX_CMD_SHIELD_OFF 0x00
#define GOODIX_CMD_SHIELD_ON 0x01

struct goodix_packet {
	uint8_t cmd0;
	uint8_t cmd1;
	const uint8_t *payload;
	size_t payload_len;
};

#define GOODIX_MAX_RECORD 160

struct goodix_template {
	uint8_t tid[GOODIX_TEMPLATE_ID_SIZE];
	char uid[GOODIX_MAX_UID + 1];
	size_t uid_len;
	/* the finger id record exactly as the sensor sent it (raw_len 0 if longer than GOODIX_MAX_RECORD):
	 * deleting with these bytes cannot differ from what the firmware stored in any field */
	uint8_t raw[GOODIX_MAX_RECORD];
	size_t raw_len;
};

static inline uint8_t goodix_crc8(const uint8_t *data, size_t len)
{
	uint16_t crc = 0;

	while (len--) {
		crc ^= (uint16_t)*data++ << 8;
		for (int i = 0; i < 8; i++) {
			if (crc & 0x8000)
				crc ^= 0x8380;
			crc <<= 1;
		}
	}
	return (uint8_t)~(crc >> 8);
}

static inline uint32_t goodix_crc32(const uint8_t *data, size_t len)
{
	uint32_t crc = UINT32_MAX;

	while (len--) {
		crc ^= *data++;
		for (int i = 0; i < 8; i++)
			crc = (crc >> 1) ^ ((crc & 1) ? 0xedb88320U : 0);
	}
	return ~crc;
}

static inline int goodix_build_packet(uint8_t *out, size_t out_size, size_t *written,
				      uint8_t cmd0, uint8_t cmd1, uint8_t sequence,
				      const uint8_t *payload, size_t payload_len)
{
	size_t total = GOODIX_HEADER_SIZE + payload_len + GOODIX_CRC_SIZE;
	uint32_t crc;

	if (!out || !written || (payload_len && !payload) ||
	    payload_len > GOODIX_MAX_PAYLOAD || total > out_size ||
	    payload_len + GOODIX_CRC_SIZE > UINT16_MAX)
		return -1;

	out[0] = cmd0;
	out[1] = cmd1;
	out[2] = 0;
	out[3] = sequence;
	out[4] = (uint8_t)(payload_len + GOODIX_CRC_SIZE);
	out[5] = (uint8_t)((payload_len + GOODIX_CRC_SIZE) >> 8);
	out[6] = goodix_crc8(out, 6);
	out[7] = (uint8_t)~out[6];
	if (payload_len)
		memcpy(out + GOODIX_HEADER_SIZE, payload, payload_len);
	crc = goodix_crc32(out, GOODIX_HEADER_SIZE + payload_len);
	out[GOODIX_HEADER_SIZE + payload_len] = (uint8_t)crc;
	out[GOODIX_HEADER_SIZE + payload_len + 1] = (uint8_t)(crc >> 8);
	out[GOODIX_HEADER_SIZE + payload_len + 2] = (uint8_t)(crc >> 16);
	out[GOODIX_HEADER_SIZE + payload_len + 3] = (uint8_t)(crc >> 24);
	*written = total;
	return 0;
}

static inline int goodix_parse_packet(const uint8_t *data, size_t len, struct goodix_packet *packet)
{
	size_t payload_len, packet_len, frame_len;
	uint32_t expected_crc, actual_crc;

	if (!data || !packet || len < GOODIX_HEADER_SIZE + GOODIX_CRC_SIZE)
		return -1;
	packet_len = (size_t)data[4] | (size_t)data[5] << 8;
	if (packet_len < GOODIX_CRC_SIZE)
		return -1;
	payload_len = packet_len - GOODIX_CRC_SIZE;
	frame_len = GOODIX_HEADER_SIZE + packet_len;
	if (payload_len > GOODIX_MAX_PAYLOAD ||
	    len < frame_len)
		return -1;
	/* libfprint validates CRC at the header-declared frame end; USB may pad to max packet size. */
	expected_crc = (uint32_t)data[frame_len - 4] |
		       (uint32_t)data[frame_len - 3] << 8 |
		       (uint32_t)data[frame_len - 2] << 16 |
		       (uint32_t)data[frame_len - 1] << 24;
	actual_crc = goodix_crc32(data, frame_len - GOODIX_CRC_SIZE);
	if (expected_crc != actual_crc)
		return -1;
	packet->cmd0 = data[0];
	packet->cmd1 = data[1];
	packet->payload = data + GOODIX_HEADER_SIZE;
	packet->payload_len = payload_len;
	return 0;
}

static inline int goodix_encode_finger_id(uint8_t *out, size_t out_size, size_t *written,
					  const uint8_t tid[GOODIX_TEMPLATE_ID_SIZE],
					  const char *uid)
{
	size_t uid_len, record_len, total;

	if (!out || !written || !tid || !uid)
		return -1;
	uid_len = strlen(uid);
	if (uid_len == 0 || uid_len > GOODIX_MAX_UID || uid[0] == ' ')
		return -1;
	record_len = 70 + uid_len;
	total = record_len + 2;
	if (total > out_size)
		return -1;
	out[0] = (uint8_t)record_len;
	out[1] = (uint8_t)(record_len >> 8);
	out[2] = 67;
	out[3] = 1;
	out[4] = 1;
	out[5] = 0;
	memset(out + 6, 0, 32);
	memcpy(out + 38, tid, GOODIX_TEMPLATE_ID_SIZE);
	out[70] = (uint8_t)uid_len;
	memcpy(out + 71, uid, uid_len);
	out[71 + uid_len] = 0;
	*written = total;
	return 0;
}

static inline int goodix_parse_finger_id(const uint8_t *data, size_t len,
					 struct goodix_template *template)
{
	size_t uid_len;

	if (!data || !template || len < 69 || data[0] != 67)
		return -1;
	uid_len = data[68];
	if (len < 69 + uid_len)
		return -1;
	memcpy(template->tid, data + 36, GOODIX_TEMPLATE_ID_SIZE);
	template->raw_len = len <= GOODIX_MAX_RECORD ? len : 0;
	if (template->raw_len)
		memcpy(template->raw, data, len);
	template->uid_len = 0;
	template->uid[0] = 0;
	if (uid_len == 0 || uid_len > GOODIX_MAX_UID)
		return 1;
	memcpy(template->uid, data + 69, uid_len);
	template->uid[uid_len] = 0;
	template->uid_len = uid_len;
	return 0;
}

#endif
