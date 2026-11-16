/* SPDX-License-Identifier: LGPL-2.1-or-later */
#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

static int goodix_test_ioctl(int fd, unsigned long request, ...);
#define ioctl goodix_test_ioctl
#include "goodix.h"
#undef ioctl
#include <stdarg.h>

static struct goodix *mock_sensor;
static int mock_reads;
static bool mock_cancel_after_ack;
static bool mock_write_timeout;

static int goodix_test_ioctl(int fd, unsigned long request, ...)
{
	struct goodix_usb_bulk *bulk;
	va_list ap;
	uint8_t payload[3];
	size_t len;

	(void)fd;
	assert(request == (unsigned long)(int)GOODIX_USB_BULK);
	va_start(ap, request);
	bulk = va_arg(ap, struct goodix_usb_bulk *);
	va_end(ap);
	if (bulk->ep == GOODIX_EP_OUT) {
		if (mock_write_timeout) {
			errno = ETIMEDOUT;
			return -1;
		}
		return (int)bulk->len;
	}
	assert(bulk->ep == GOODIX_EP_IN);
	if (mock_reads++ == 0) {
		payload[0] = GOODIX_SUCCESS;
		payload[1] = GOODIX_CMD_VERSION;
		assert(goodix_build_packet(bulk->data, bulk->len, &len,
					   GOODIX_CMD_RESPONSE, 0, 1, payload, 2) == 0);
		if (mock_cancel_after_ack)
			goodix_request_cancel(mock_sensor);
	} else {
		payload[0] = GOODIX_SUCCESS;
		payload[1] = 0x5a;
		payload[2] = 0xa5;
		assert(goodix_build_packet(bulk->data, bulk->len, &len,
					   GOODIX_CMD_VERSION, 0, 1, payload, 3) == 0);
	}
	return (int)len;
}

static void test_command_transport(void)
{
	struct goodix sensor;
	struct goodix_response response;
	const uint8_t payload = 0;

	goodix_init(&sensor);
	sensor.fd = 1;
	mock_sensor = &sensor;
	mock_reads = 0;
	mock_cancel_after_ack = false;
	mock_write_timeout = false;
	assert(goodix_command(&sensor, GOODIX_CMD_VERSION, 0, &payload, 1, false, &response) == 0);
	assert(mock_reads == 2 && response.result == GOODIX_SUCCESS && response.len == 2);
	assert(response.data[0] == 0x5a && response.data[1] == 0xa5);

	mock_reads = 0;
	mock_cancel_after_ack = true;
	assert(goodix_command(&sensor, GOODIX_CMD_VERSION, 0, &payload, 1, true, &response) == -ECANCELED);
	assert(mock_reads == 1 && sensor.need_reset);

	goodix_clear_cancel(&sensor);
	sensor.need_reset = false;
	mock_write_timeout = true;
	assert(goodix_command(&sensor, GOODIX_CMD_VERSION, 0, &payload, 1, false, &response) == -ETIMEDOUT);
	assert(sensor.need_reset);
}

static void test_checksums_and_packet(void)
{
	static const uint8_t expected[] = {
		0xd0, 0x00, 0x00, 0x01, 0x05, 0x00, 0xd1, 0x2e,
		0x00, 0x69, 0x5f, 0xfe, 0x76,
	};
	static const uint8_t payload[] = { 0 };
	static const uint8_t check[] = "123456789";
	uint8_t packet_bytes[64];
	struct goodix_packet packet;
	size_t written;

	assert(goodix_crc32(check, sizeof(check) - 1) == 0xcbf43926U);
	assert(goodix_build_packet(packet_bytes, sizeof(packet_bytes), &written,
				   GOODIX_CMD_VERSION, 0, 1, payload, sizeof(payload)) == 0);
	assert(written == sizeof(expected));
	assert(memcmp(packet_bytes, expected, sizeof(expected)) == 0);
	assert(goodix_parse_packet(packet_bytes, written, &packet) == 0);
	assert(packet.cmd0 == GOODIX_CMD_VERSION && packet.cmd1 == 0);
	assert(packet.payload_len == 1 && packet.payload[0] == 0);

	packet_bytes[12] ^= 1;
	assert(goodix_parse_packet(packet_bytes, written, &packet) < 0);
	packet_bytes[12] ^= 1;
	packet_bytes[6] ^= 1;
	assert(goodix_parse_packet(packet_bytes, written, &packet) < 0);
}

static void test_template_encoding(void)
{
	static const char uid[] = "GBOS-1234abcd-u0-a1b2c3d4";
	uint8_t tid[GOODIX_TEMPLATE_ID_SIZE], encoded[GOODIX_MAX_UID + 72];
	struct goodix_template decoded;
	size_t encoded_len;

	for (size_t i = 0; i < sizeof(tid); i++)
		tid[i] = (uint8_t)i;
	assert(goodix_encode_finger_id(encoded, sizeof(encoded), &encoded_len, tid, uid) == 0);
	assert(encoded_len == 2 + 70 + strlen(uid));
	assert(goodix_parse_finger_id(encoded + 2, encoded_len - 2, &decoded) == 0);
	assert(decoded.uid_len == strlen(uid));
	assert(strcmp(decoded.uid, uid) == 0);
	assert(memcmp(decoded.tid, tid, sizeof(tid)) == 0);
	assert(goodix_encode_finger_id(encoded, sizeof(encoded), &encoded_len, tid, " ") < 0);
	assert(goodix_parse_finger_id(encoded + 2, 68, &decoded) < 0);

	memset(encoded, 0, sizeof(encoded));
	encoded[0] = 127;
	encoded[1] = 0;
	encoded[2] = 67;
	encoded[70] = 57;
	memcpy(encoded + 38, tid, sizeof(tid));
	assert(goodix_parse_finger_id(encoded + 2, 127, &decoded) == 1);
	assert(decoded.uid_len == 0);
	assert(memcmp(decoded.tid, tid, sizeof(tid)) == 0);
}

static void test_runtime_config(void)
{
	struct goodix sensor = {0};
	uint8_t config[GOODIX_CFG_SIZE];
	uint32_t expected_crc, actual_crc;

	goodix_make_config(&sensor, config);
	expected_crc = (uint32_t)config[124] |
		       (uint32_t)config[125] << 8 |
		       (uint32_t)config[126] << 16 |
		       (uint32_t)config[127] << 24;
	actual_crc = goodix_crc32(config, GOODIX_CFG_CRC_OFFSET);
	assert(expected_crc == actual_crc);
	assert(config[6] == GOODIX_ENROLL_STAGES);
	assert(config[26] == 1);
	assert(sensor.config[4] == 0x0f && sensor.config[5] == 0x41);
}

int main(void)
{
	test_checksums_and_packet();
	test_template_encoding();
	test_runtime_config();
	test_command_transport();
	puts("GOODIX_PROTOCOL_TEST_PASS");
	return 0;
}
