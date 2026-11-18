/* SPDX-License-Identifier: LGPL-2.1-or-later */
#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

static void goodix_test_log(const char *format, ...);
#define GOODIX_LOGI(...) ((void)0)
#define GOODIX_LOGW(...) ((void)0)
#define GOODIX_LOGE(...) goodix_test_log(__VA_ARGS__)
static int goodix_test_ioctl(int fd, unsigned long request, ...);
#define ioctl goodix_test_ioctl
#include "goodix.h"
#undef ioctl
#include <stdarg.h>

static char mock_log[512];
static struct goodix *mock_sensor;
static int mock_reads;
static int mock_timeout_read = -1;
static unsigned int mock_empty_reads;
static unsigned int mock_empty_count;
static bool mock_empty_forever;
static bool mock_cancel_after_ack;
static bool mock_write_timeout;
static bool mock_open_mode;
static uint8_t mock_command;
static unsigned int mock_reset_count;
static int mock_config_cmd1 = -1;
static unsigned int mock_config_len;

static void goodix_test_log(const char *format, ...)
{
	va_list ap;

	va_start(ap, format);
	vsnprintf(mock_log, sizeof(mock_log), format, ap);
	va_end(ap);
}

static int goodix_test_ioctl(int fd, unsigned long request, ...)
{
	struct goodix_usb_bulk *bulk;
	va_list ap;
	uint8_t payload[3], version[20];
	size_t len;

	(void)fd;
	if (request == (unsigned long)(int)0x5514) {
		mock_reset_count++;
		return 0;
	}
	if (request == (unsigned long)(int)GOODIX_USB_CLAIMINTERFACE ||
	    request == (unsigned long)(int)GOODIX_USB_RELEASEINTERFACE)
		return 0;
	assert(request == (unsigned long)(int)GOODIX_USB_BULK);
	va_start(ap, request);
	bulk = va_arg(ap, struct goodix_usb_bulk *);
	va_end(ap);
	if (bulk->ep == GOODIX_EP_OUT) {
		if (mock_write_timeout) {
			errno = ETIMEDOUT;
			return -1;
		}
		mock_command = ((uint8_t *)bulk->data)[0];
		if (mock_command == GOODIX_CMD_CONFIG) {
			mock_config_cmd1 = ((uint8_t *)bulk->data)[1];
			mock_config_len = bulk->len;
		}
		mock_reads = 0;
		return (int)bulk->len;
	}
	assert(bulk->ep == GOODIX_EP_IN);
	if (mock_reads == mock_timeout_read) {
		mock_reads++;
		errno = ETIMEDOUT;
		return -1;
	}
	if (mock_empty_forever || mock_empty_reads) {
		if (mock_empty_reads)
			mock_empty_reads--;
		mock_empty_count++;
		return 0;
	}
	if (mock_reads++ == 0) {
		payload[0] = GOODIX_SUCCESS;
		payload[1] = mock_command;
		assert(goodix_build_packet(bulk->data, bulk->len, &len,
					   GOODIX_CMD_RESPONSE, 0, 1, payload, 2) == 0);
		if (mock_cancel_after_ack)
			goodix_request_cancel(mock_sensor);
	} else if (mock_open_mode && mock_command == GOODIX_CMD_VERSION) {
		memset(version, 0, sizeof(version));
		version[0] = GOODIX_SUCCESS;
		memcpy(version + 3, "APP", 3);
		memcpy(version + 11, "12345678", 8);
		assert(goodix_build_packet(bulk->data, bulk->len, &len,
					   mock_command, 0, 1, version, sizeof(version)) == 0);
	} else if (mock_open_mode) {
		payload[0] = GOODIX_SUCCESS;
		assert(goodix_build_packet(bulk->data, bulk->len, &len,
					   mock_command, 0, 1, payload, 1) == 0);
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
	mock_empty_reads = 2;
	mock_empty_count = 0;
	mock_empty_forever = false;
	mock_cancel_after_ack = false;
	mock_write_timeout = false;
	assert(goodix_command(&sensor, GOODIX_CMD_VERSION, 0, &payload, 1, false, &response) == 0);
	assert(mock_reads == 2 && mock_empty_reads == 0 && mock_empty_count == 2);
	assert(response.result == GOODIX_SUCCESS && response.len == 2);
	assert(response.data[0] == 0x5a && response.data[1] == 0xa5);

	mock_reads = 0;
	mock_empty_reads = 1;
	mock_empty_count = 0;
	mock_cancel_after_ack = true;
	assert(goodix_command(&sensor, GOODIX_CMD_VERSION, 0, &payload, 1, true, &response) == -ECANCELED);
	assert(mock_reads == 1 && mock_empty_reads == 0 && mock_empty_count == 1 && sensor.need_reset);

	goodix_clear_cancel(&sensor);
	sensor.need_reset = false;
	mock_empty_count = 0;
	mock_empty_forever = true;
	{
		uint8_t buffer[GOODIX_HEADER_SIZE + GOODIX_MAX_PAYLOAD + GOODIX_CRC_SIZE];
		struct goodix_packet packet;

		assert(goodix_receive(&sensor, &packet, buffer, sizeof(buffer), false, 1) == -ETIMEDOUT);
		assert(mock_empty_count > 0 && sensor.need_reset);
	}
	mock_empty_forever = false;
	sensor.need_reset = false;
	mock_write_timeout = true;
	assert(goodix_command(&sensor, GOODIX_CMD_VERSION, 0, &payload, 1, false, &response) == -ETIMEDOUT);
	assert(sensor.need_reset);
}

static void test_command_timeout_diagnostics(void)
{
	struct goodix sensor;
	struct goodix_response response;
	const uint8_t payload = 0;

	goodix_init(&sensor);
	sensor.fd = 1;
	mock_sensor = &sensor;
	mock_empty_forever = false;
	mock_empty_reads = 0;
	mock_cancel_after_ack = false;
	mock_timeout_read = -1;
	mock_write_timeout = true;
	mock_log[0] = 0;
	assert(goodix_command(&sensor, GOODIX_CMD_VERSION, 0, &payload, 1, false,
			      &response) == -ETIMEDOUT);
	assert(strstr(mock_log, "USB OUT command d0:00 failed: -110") != NULL);

	mock_write_timeout = false;
	mock_reads = 0;
	mock_timeout_read = 0;
	mock_log[0] = 0;
	assert(goodix_command(&sensor, GOODIX_CMD_VERSION, 0, &payload, 1, false,
			      &response) == -ETIMEDOUT);
	assert(strstr(mock_log, "command d0:00 acknowledgement receive failed: -110") != NULL);

	mock_reads = 0;
	mock_timeout_read = 1;
	mock_log[0] = 0;
	assert(goodix_command(&sensor, GOODIX_CMD_VERSION, 0, &payload, 1, false,
			      &response) == -ETIMEDOUT);
	assert(strstr(mock_log, "command d0:00 response receive failed: -110") != NULL);
	mock_timeout_read = -1;
}

static void test_open_without_usb_reset(void)
{
	struct goodix sensor;
	uint8_t descriptor[18] = { 18, 1 };
	char path[] = "/tmp/gbf.XXXXXX";
	int fd = mkstemp(path), result;

	assert(fd >= 0);
	descriptor[8] = 0xc6;
	descriptor[9] = 0x27;
	descriptor[10] = 0x84;
	descriptor[11] = 0x63;
	assert(write(fd, descriptor, sizeof(descriptor)) == (ssize_t)sizeof(descriptor));
	close(fd);

	goodix_init(&sensor);
	assert(snprintf(sensor.path, sizeof(sensor.path), "%s", path) < (int)sizeof(sensor.path));
	mock_open_mode = true;
	mock_reset_count = 0;
	mock_empty_forever = false;
	mock_empty_reads = 0;
	mock_cancel_after_ack = false;
	mock_write_timeout = false;
	mock_config_cmd1 = -1;
	mock_config_len = 0;
	result = goodix_open(&sensor);
	if (result < 0)
		fprintf(stderr, "mock Goodix open failed: %d (reads=%d command=%02x)\n",
			result, mock_reads, mock_command);
	assert(result == 0);
	assert(sensor.pid == GOODIX_PID && strcmp(sensor.fw_version, "12345678") == 0);
	assert(mock_reset_count == 0);
	/* libfprint FP_INIT_CONFIG: c0:01 with the 128-byte config block. */
	assert(mock_config_cmd1 == GOODIX_CMD_WRITE_CFG_TO_FLASH);
	assert(mock_config_len == GOODIX_HEADER_SIZE + GOODIX_CFG_SIZE + GOODIX_CRC_SIZE);
	goodix_close(&sensor);
	mock_open_mode = false;
	unlink(path);
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

	memset(packet_bytes + written, 0xa5, 64 - written);
	assert(goodix_parse_packet(packet_bytes, 64, &packet) == 0);
	assert(packet.cmd0 == GOODIX_CMD_VERSION && packet.payload_len == 1);
	assert(goodix_parse_packet(packet_bytes, written - 1, &packet) < 0);

	packet_bytes[12] ^= 1;
	assert(goodix_parse_packet(packet_bytes, written, &packet) < 0);
	packet_bytes[12] ^= 1;
	packet_bytes[6] ^= 1;
	{
		uint32_t crc = goodix_crc32(packet_bytes, written - GOODIX_CRC_SIZE);

		packet_bytes[written - 4] = (uint8_t)crc;
		packet_bytes[written - 3] = (uint8_t)(crc >> 8);
		packet_bytes[written - 2] = (uint8_t)(crc >> 16);
		packet_bytes[written - 1] = (uint8_t)(crc >> 24);
	}
	assert(goodix_parse_packet(packet_bytes, written, &packet) == 0);
	packet_bytes[written - 1] ^= 1;
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
	assert(goodix_parse_finger_id(encoded + 2, 126, &decoded) == 1);
	assert(decoded.uid_len == 0);
	assert(memcmp(decoded.tid, tid, sizeof(tid)) == 0);
}

static void test_runtime_config(void)
{
	struct goodix sensor = {0};
	uint8_t config[GOODIX_CFG_SIZE], defaults[GOODIX_CFG_SIZE];
	uint32_t expected_crc, actual_crc;

	goodix_make_config(&sensor, config);
	expected_crc = (uint32_t)config[124] |
		       (uint32_t)config[125] << 8 |
		       (uint32_t)config[126] << 16 |
		       (uint32_t)config[127] << 24;
	/* libfprint computes the CRC before setting byte 6 (8 -> 12) and never updates it. */
	memcpy(defaults, config, sizeof(defaults));
	defaults[6] = 0x08;
	actual_crc = goodix_crc32(defaults, GOODIX_CFG_CRC_OFFSET);
	assert(expected_crc == actual_crc);
	assert(expected_crc != goodix_crc32(config, GOODIX_CFG_CRC_OFFSET));
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
	test_command_timeout_diagnostics();
	test_open_without_usb_reset();
	puts("GOODIX_PROTOCOL_TEST_PASS");
	return 0;
}
