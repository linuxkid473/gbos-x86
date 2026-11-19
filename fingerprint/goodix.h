/* SPDX-License-Identifier: LGPL-2.1-or-later */
/*
 * Blocking usbfs backend for Goodix MOC sensors. Protocol and command
 * sequences are adapted from libfprint's goodixmoc driver (v1.94.9).
 * No storage-reset or firmware-update command is implemented.
 */
#ifndef GBOS_GOODIX_H
#define GBOS_GOODIX_H

#include "goodix_proto.h"

#ifndef GOODIX_ERRNO
#define GOODIX_ERRNO errno
#endif
#ifndef GOODIX_LOGI
#define GOODIX_LOGI(...) ((void)0)
#define GOODIX_LOGW(...) ((void)0)
#define GOODIX_LOGE(...) ((void)0)
#endif

#define GOODIX_VID 0x27c6
#define GOODIX_PID 0x6384
#define GOODIX_EP_OUT 0x01
#define GOODIX_EP_IN 0x83
#define GOODIX_USB_BULK 0xc0185502
#define GOODIX_USB_CLAIMINTERFACE 0x8004550f
#define GOODIX_USB_RELEASEINTERFACE 0x80045510
#define GOODIX_INTERFACE 0
#define GOODIX_CFG_SIZE 128
#define GOODIX_CFG_CRC_OFFSET 124

enum {
	GOODIX_OK = 0,
	GOODIX_NOMATCH,
	GOODIX_RETRY,
	GOODIX_NORECORD,
	GOODIX_FAILED,
	GOODIX_DUPLICATE,	/* enroll: the finger matches a stored template (check-duplicate) */
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

struct goodix {
	int fd;
	char path[32];
	uint16_t pid;
	bool need_reset;
	bool cancel;
	uint8_t sequence;
	uint8_t config[26];
	char fw_version[9];
	bool delete_blocked;	/* a delete removed some other template too: no more deletes while this process runs */
};

struct goodix_usb_bulk {
	unsigned int ep, len, timeout;
	void *data;
};

struct goodix_response {
	uint8_t result;
	uint8_t data[GOODIX_MAX_PAYLOAD];
	size_t len;
};

static inline void goodix_init(struct goodix *s)
{
	memset(s, 0, sizeof(*s));
	s->fd = -1;
}

static inline void goodix_request_cancel(struct goodix *s)
{
	__atomic_store_n(&s->cancel, true, __ATOMIC_RELEASE);
}

static inline void goodix_clear_cancel(struct goodix *s)
{
	__atomic_store_n(&s->cancel, false, __ATOMIC_RELEASE);
}

static inline bool goodix_cancel_requested(struct goodix *s)
{
	return __atomic_load_n(&s->cancel, __ATOMIC_ACQUIRE);
}

static inline int goodix_bulk(struct goodix *s, unsigned int ep, void *data,
			      unsigned int len, unsigned int timeout_ms)
{
	struct goodix_usb_bulk bulk = { ep, len, timeout_ms, data };
	int n = ioctl(s->fd, (int)GOODIX_USB_BULK, &bulk);

	if (n >= 0)
		return n;
	if (GOODIX_ERRNO != ETIMEDOUT)
		s->need_reset = true;
	return -GOODIX_ERRNO;
}

static inline bool goodix_probe_node(const char *path, uint16_t *pid)
{
	uint8_t descriptor[18];
	int fd, n;

	fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return false;
	n = (int)read(fd, descriptor, sizeof(descriptor));
	close(fd);
	if (n < (int)sizeof(descriptor) || descriptor[1] != 1 ||
	    (descriptor[8] | (uint16_t)descriptor[9] << 8) != GOODIX_VID ||
	    (descriptor[10] | (uint16_t)descriptor[11] << 8) != GOODIX_PID)
		return false;
	*pid = (uint16_t)(descriptor[10] | (uint16_t)descriptor[11] << 8);
	return true;
}

static inline int goodix_find(struct goodix *s)
{
	if (s->path[0] && goodix_probe_node(s->path, &s->pid))
		return 0;
	for (int bus = 1; bus <= 32; bus++)
		for (int dev = 1; dev <= 127; dev++) {
			snprintf(s->path, sizeof(s->path), "/dev/bus/usb/%03d/%03d", bus, dev);
			if (goodix_probe_node(s->path, &s->pid))
				return 0;
		}
	s->path[0] = 0;
	return -ENODEV;
}

static inline int goodix_receive(struct goodix *s, struct goodix_packet *packet,
				 uint8_t *buffer, size_t buffer_size,
				 bool cancellable, unsigned int timeout_ms)
{
	struct timespec deadline = {0}, now;
	int n;

	if (!cancellable) {
		if (clock_gettime(CLOCK_MONOTONIC, &deadline) < 0)
			return -GOODIX_ERRNO;
		deadline.tv_sec += timeout_ms / 1000;
		deadline.tv_nsec += (long)(timeout_ms % 1000) * 1000000L;
		if (deadline.tv_nsec >= 1000000000L) {
			deadline.tv_sec++;
			deadline.tv_nsec -= 1000000000L;
		}
	}

	for (;;) {
		unsigned int timeout;

		if (cancellable && goodix_cancel_requested(s))
			return -ECANCELED;
		if (cancellable) {
			timeout = 250;
		} else {
			long seconds, nanoseconds;

			if (clock_gettime(CLOCK_MONOTONIC, &now) < 0)
				return -GOODIX_ERRNO;
			seconds = deadline.tv_sec - now.tv_sec;
			nanoseconds = deadline.tv_nsec - now.tv_nsec;
			if (nanoseconds < 0) {
				seconds--;
				nanoseconds += 1000000000L;
			}
			if (seconds < 0 || (seconds == 0 && nanoseconds == 0)) {
				s->need_reset = true;
				return -ETIMEDOUT;
			}
			timeout = (unsigned int)(seconds * 1000 +
						 (nanoseconds + 999999) / 1000000);
			if (timeout == 0)
				timeout = 1;
		}
		n = goodix_bulk(s, GOODIX_EP_IN, buffer, (unsigned int)buffer_size, timeout);
		if (n == -ETIMEDOUT && cancellable)
			continue;
		if (n < 0) {
			if (n == -ETIMEDOUT)
				s->need_reset = true;
			return n;
		}
		/* Goodix may complete a bulk read with no data; upstream resubmits the read. */
		if (n == 0)
			continue;
		if (goodix_parse_packet(buffer, (size_t)n, packet) < 0) {
			s->need_reset = true;
			if (n >= GOODIX_HEADER_SIZE)
				GOODIX_LOGE("invalid Goodix packet (%d bytes, cmd=%02x%02x len=%u hdr=%02x/%02x)",
					    n, buffer[0], buffer[1],
					    (unsigned int)buffer[4] | (unsigned int)buffer[5] << 8,
					    buffer[6], buffer[7]);
			else
				GOODIX_LOGE("invalid short Goodix packet (%d bytes)", n);
			return -EPROTO;
		}
		return 0;
	}
}

/*
 * Packets for another command: what the sensor still sends for a command that was abandoned when an
 * operation was cancelled. libfprint never sees them because it resets the USB device on every open; this
 * backend does not reset (it disrupts this sensor), so it reads past a few. 2026-11-18: after a cancel
 * during the b0:02 acknowledgement wait, the next VERSION read b0's late acknowledgement, failed with
 * -EPROTO and the framework got HW_UNAVAILABLE ("Fingerprint sensor hardware unavailable").
 */
#define GOODIX_MAX_STALE 4

static inline int goodix_command_timeout(struct goodix *s, uint8_t cmd0, uint8_t cmd1,
					 const uint8_t *payload, size_t payload_len, bool cancellable,
					 unsigned int ack_ms, unsigned int response_ms,
					 struct goodix_response *response)
{
	uint8_t tx[GOODIX_HEADER_SIZE + GOODIX_MAX_PAYLOAD + GOODIX_CRC_SIZE];
	uint8_t rx[GOODIX_HEADER_SIZE + GOODIX_MAX_PAYLOAD + GOODIX_CRC_SIZE];
	struct goodix_packet packet;
	size_t tx_len;
	int n, r, stale;

	if (goodix_cancel_requested(s))
		return -ECANCELED;
	if (goodix_build_packet(tx, sizeof(tx), &tx_len, cmd0, cmd1,
				++s->sequence, payload, payload_len) < 0)
		return -EINVAL;
	n = goodix_bulk(s, GOODIX_EP_OUT, tx, (unsigned int)tx_len, 1000);
	if (n < 0) {
		s->need_reset = true;
		GOODIX_LOGE("USB OUT command %02x:%02x failed: %d", cmd0, cmd1, n);
		return n;
	}
	if ((size_t)n != tx_len) {
		s->need_reset = true;
		GOODIX_LOGE("USB OUT command %02x:%02x short write: %d of %u bytes",
			    cmd0, cmd1, n, (unsigned int)tx_len);
		return -EIO;
	}
	for (stale = 0;; stale++) {
		r = goodix_receive(s, &packet, rx, sizeof(rx), cancellable, ack_ms);
		if (r < 0) {
			if (r == -ECANCELED) {	/* an expected end, not a sensor fault */
				s->need_reset = true;
				GOODIX_LOGW("command %02x:%02x cancelled while awaiting acknowledgement",
					    cmd0, cmd1);
			} else {
				GOODIX_LOGE("command %02x:%02x acknowledgement receive failed: %d",
					    cmd0, cmd1, r);
			}
			return r;
		}
		if (packet.cmd0 == GOODIX_CMD_RESPONSE && packet.payload_len >= 2 &&
		    packet.payload[1] == cmd0)
			break;
		if (stale == GOODIX_MAX_STALE) {
			s->need_reset = true;
			GOODIX_LOGE("command %02x:%02x: no acknowledgement among %d packets",
				    cmd0, cmd1, stale + 1);
			return -EPROTO;
		}
		GOODIX_LOGW("command %02x:%02x: discarding stale packet %02x:%02x (%s %02x)",
			    cmd0, cmd1, packet.cmd0, packet.cmd1,
			    packet.cmd0 == GOODIX_CMD_RESPONSE ? "acknowledgement of" : "response",
			    packet.cmd0 == GOODIX_CMD_RESPONSE && packet.payload_len >= 2 ?
			    packet.payload[1] : packet.cmd0);
	}
	if (packet.payload[0] != GOODIX_SUCCESS) {
		s->need_reset = true;
		GOODIX_LOGE("command %02x:%02x refused: acknowledgement status 0x%02x",
			    cmd0, cmd1, packet.payload[0]);
		return -EPROTO;
	}
	/* a command that cannot be cancelled is answered at once: read its response, or it is left in the pipe as a
	 * stale packet for the next command (boot-13: a cancel during the open's d0:00) */
	if (cancellable && goodix_cancel_requested(s)) {
		s->need_reset = true;
		return -ECANCELED;
	}
	for (stale = 0;; stale++) {
		r = goodix_receive(s, &packet, rx, sizeof(rx), cancellable, response_ms);
		if (r < 0) {
			if (r == -ECANCELED) {
				s->need_reset = true;
				GOODIX_LOGW("command %02x:%02x cancelled while awaiting its response",
					    cmd0, cmd1);
			} else {
				GOODIX_LOGE("command %02x:%02x response receive failed: %d",
					    cmd0, cmd1, r);
			}
			return r;
		}
		if (packet.cmd0 == cmd0 && packet.payload_len >= 1)
			break;
		if (stale == GOODIX_MAX_STALE) {
			s->need_reset = true;
			GOODIX_LOGE("command %02x:%02x: no response among %d packets",
				    cmd0, cmd1, stale + 1);
			return -EPROTO;
		}
		GOODIX_LOGW("command %02x:%02x: discarding stale packet %02x:%02x while awaiting its response",
			    cmd0, cmd1, packet.cmd0, packet.cmd1);
	}
	response->result = packet.payload[0];
	response->len = packet.payload_len - 1;
	if (response->len)
		memcpy(response->data, packet.payload + 1, response->len);
	return 0;
}

static inline int goodix_command(struct goodix *s, uint8_t cmd0, uint8_t cmd1,
				 const uint8_t *payload, size_t payload_len,
				 bool cancellable, struct goodix_response *response)
{
	return goodix_command_timeout(s, cmd0, cmd1, payload, payload_len, cancellable,
				      2000, 5000, response);
}

static inline void goodix_make_config(struct goodix *s, uint8_t out[GOODIX_CFG_SIZE])
{
	static const uint8_t defaults[26] = {
		0x00, 0x00, 0x64, 0x50, 0x0f, 0x41, 0x08, 0x0a, 0x18, 0x00, 0x00, 0x23, 0x00,
		0x00, 0x01, 0x01, 0x00, 0x01, 0x01, 0x01, 0x01, 0x00, 0x01, 0x01, 0x05, 0x05
	};
	uint32_t crc;

	memset(out, 0, GOODIX_CFG_SIZE);
	memcpy(out, defaults, sizeof(defaults));
	out[26] = 1;
	/*
	 * Byte-identical to libfprint v1.94.9: gx_proto_init_sensor_config() stores the
	 * CRC over the defaults (byte 6 = 8), then gx_fp_init() sets byte 6 to the
	 * enroll stage count without recomputing it. The firmware accepts that block.
	 */
	crc = goodix_crc32(out, GOODIX_CFG_CRC_OFFSET);
	out[124] = (uint8_t)crc;
	out[125] = (uint8_t)(crc >> 8);
	out[126] = (uint8_t)(crc >> 16);
	out[127] = (uint8_t)(crc >> 24);
	out[6] = GOODIX_ENROLL_STAGES;
	memcpy(s->config, defaults, sizeof(defaults));
	s->config[6] = GOODIX_ENROLL_STAGES;
}

static inline void goodix_close(struct goodix *s)
{
	unsigned int interface = GOODIX_INTERFACE;

	if (s->fd < 0)
		return;
	(void)ioctl(s->fd, (int)GOODIX_USB_RELEASEINTERFACE, &interface);
	close(s->fd);
	s->fd = -1;
	goodix_clear_cancel(s);
}

static inline int goodix_open(struct goodix *s)
{
	static const uint8_t zero = 0;
	uint8_t config[GOODIX_CFG_SIZE];
	unsigned int interface = GOODIX_INTERFACE;
	struct goodix_response response;
	int r;

	if (s->fd >= 0)
		return 0;
	if ((r = goodix_find(s)) < 0)
		return r;
	s->fd = open(s->path, O_RDWR | O_CLOEXEC);
	if (s->fd < 0)
		return -GOODIX_ERRNO;
	/* Match libfprint's interface-claim flow; a USB reset disrupts this sensor. */
	if (ioctl(s->fd, (int)GOODIX_USB_CLAIMINTERFACE, &interface) < 0) {
		r = -GOODIX_ERRNO;
		goto fail;
	}
	s->need_reset = false;
	if ((r = goodix_command(s, GOODIX_CMD_VERSION, 0, &zero, 1, false, &response)) < 0)
		goto fail;
	if (response.result != GOODIX_SUCCESS || response.len < 18 ||
	    memcmp(response.data + 2, "APP", 3) != 0) {
		GOODIX_LOGE("unsupported Goodix firmware type or version response");
		r = -EPROTO;
		goto fail;
	}
	memcpy(s->fw_version, response.data + 10, 8);
	s->fw_version[8] = 0;

	/*
	 * Same CONFIG as libfprint's FP_INIT_CONFIG (cmd1 WRITE_CFG_TO_FLASH). The
	 * 0x00 "don't write" form is acknowledged by 27c6:6384 firmware but never
	 * answered (2026-11-17 boot: every attempt timed out on the c0:00 response).
	 * This stores the matching config only; templates are untouched.
	 */
	goodix_make_config(s, config);
	if ((r = goodix_command(s, GOODIX_CMD_CONFIG, GOODIX_CMD_WRITE_CFG_TO_FLASH,
			       config, sizeof(config), false, &response)) < 0)
		goto fail;
	/* libfprint's fp_init_config_cb() does not reject a nonzero status either. */
	if (response.result != GOODIX_SUCCESS)
		GOODIX_LOGW("Goodix configuration status 0x%02x (continuing, as libfprint does)",
			    response.result);
	if (response.len >= 2)
		GOODIX_LOGI("Goodix 27c6:6384 firmware %.8s ready, max stored prints %u",
			    s->fw_version, (unsigned int)response.data[1]);
	else
		GOODIX_LOGI("Goodix 27c6:6384 firmware %.8s ready", s->fw_version);
	return 0;
fail:
	goodix_close(s);
	return r;
}

static inline void goodix_emit(const struct goodix_ev *ev, int kind, int value)
{
	if (ev && ev->fn)
		ev->fn(ev->ctx, kind, value);
}

static inline int goodix_power_shield(struct goodix *s, uint8_t enabled)
{
	struct goodix_response response;
	int r = goodix_command(s, GOODIX_CMD_POWER_BUTTON_SHIELD, enabled,
			       NULL, 0, false, &response);

	if (r < 0)
		return r;
	return response.result == GOODIX_SUCCESS ? 0 : -EIO;
}

/*
 * After a cancelled operation: drop the cancel request, read past what the sensor still sends for the
 * abandoned command, and switch the power-button shield off again (libfprint ends every enroll and
 * identify with that), so the next operation starts from an idle sensor. Short timeouts: the framework
 * usually starts the next operation at once.
 */
static inline int goodix_settle(struct goodix *s)
{
	struct goodix_response response;
	int r;

	if (s->fd < 0)
		return -ENODEV;
	goodix_clear_cancel(s);
	r = goodix_command_timeout(s, GOODIX_CMD_POWER_BUTTON_SHIELD, GOODIX_CMD_SHIELD_OFF,
				   NULL, 0, false, 500, 1000, &response);
	if (r < 0)
		return r;
	s->need_reset = false;
	return response.result == GOODIX_SUCCESS ? 0 : -EIO;
}

static inline int goodix_list_templates(struct goodix *s, struct goodix_template *templates,
					size_t *count, int *fail)
{
	const uint8_t zero = 0;
	struct goodix_response response;
	size_t offset = 1;
	int r;

	if ((r = goodix_command(s, GOODIX_CMD_LIST, 0, &zero, 1, false, &response)) < 0)
		return r;
	if (response.result != GOODIX_SUCCESS) {
		*fail = response.result;
		return GOODIX_FAILED;
	}
	if (response.len < 1 || response.data[0] > GOODIX_MAX_TEMPLATES) {
		*fail = GOODIX_FAILED;
		return GOODIX_FAILED;
	}
	*count = response.data[0];
	for (size_t i = 0; i < *count; i++) {
		size_t record_len;

		if (offset + 2 > response.len) {
			*fail = GOODIX_FAILED;
			return GOODIX_FAILED;
		}
		record_len = (size_t)response.data[offset] |
			     (size_t)response.data[offset + 1] << 8;
		offset += 2;
		if (record_len > response.len - offset ||
		    goodix_parse_finger_id(response.data + offset, record_len, &templates[i]) < 0) {
			*fail = GOODIX_FAILED;
			return GOODIX_FAILED;
		}
		offset += record_len;
	}
	if (offset != response.len) {
		*fail = GOODIX_FAILED;
		return GOODIX_FAILED;
	}
	return GOODIX_OK;
}

static inline int goodix_wait_finger_up(struct goodix *s)
{
	const uint8_t zero = 0;

	for (;;) {
		struct goodix_response response;
		int r = goodix_command(s, GOODIX_CMD_FINGER_MODE, GOODIX_CMD_FINGER_UP,
				       &zero, 1, true, &response);

		if (r < 0)
			return r;
		if (response.result == GOODIX_SUCCESS)
			return 0;
		if (response.result != GOODIX_WAIT_FINGER_UP_TIMEOUT)
			return -EIO;
	}
}

#define GOODIX_VERIFY_MIN_COVERAGE 35	/* libfprint fp_verify_capture_cb() */

/*
 * One a2 capture; waits for a finger. libfprint captures differently for the two uses:
 *   enroll  {0, config[4], config[5]}, then quality >= config[4] and coverage >= config[5] (fp_enroll_capture_cb)
 *   verify  {1, config[10], config[11]}, then quality > 0 and coverage >= 35 (fp_verify_sm_run_state and
 *           fp_verify_capture_cb). Boot-13 identified after an enroll-mode capture: every touch was a no-match.
 * Returns GOODIX_OK, GOODIX_RETRY (a finger was there, but the image is unusable), GOODIX_FAILED or a
 * negative errno.
 */
static inline int goodix_capture(struct goodix *s, const struct goodix_ev *ev,
				 int stage, bool verify, int *fail)
{
	uint8_t parameters[3] = { 0, s->config[4], s->config[5] };
	struct goodix_response response;
	int r;

	if (verify) {
		parameters[0] = 1;
		parameters[1] = s->config[10];
		parameters[2] = s->config[11];
	}
	goodix_emit(ev, GOODIX_EV_CAPTURE, 0);
	r = goodix_command(s, GOODIX_CMD_CAPTURE, 0, parameters, sizeof(parameters),
			   true, &response);
	if (r < 0)
		return r;
	if (response.result >= GOODIX_ERROR_THRESHOLD) {
		*fail = response.result;
		if (verify)
			GOODIX_LOGI("identify: capture status 0x%02x", response.result);
		goodix_emit(ev, GOODIX_EV_RETRY, stage);
		return GOODIX_RETRY;
	}
	if (response.len < 2) {
		*fail = GOODIX_FAILED;
		return GOODIX_FAILED;
	}
	if (verify)
		GOODIX_LOGI("identify: capture quality %u, coverage %u", response.data[0], response.data[1]);
	if (verify ? response.data[0] == 0 || response.data[1] < GOODIX_VERIFY_MIN_COVERAGE :
		     response.data[0] < s->config[4] || response.data[1] < s->config[5]) {
		goodix_emit(ev, GOODIX_EV_RETRY, stage);
		return GOODIX_RETRY;
	}
	return GOODIX_OK;
}

/*
 * libfprint gx_proto_parse_body(MOC_CMD0_CHECK4DUPLICATE): any nonzero status means "duplicate", and the
 * body after the status is a little-endian record length and the matching template's finger id record.
 * Returns false when that record is missing or unreadable (*t then has an empty uid).
 */
static inline bool goodix_parse_duplicate(const struct goodix_response *response,
					  struct goodix_template *t)
{
	size_t record_len;

	memset(t, 0, sizeof(*t));
	if (response->len < 2)
		return false;
	record_len = (size_t)response->data[0] | (size_t)response->data[1] << 8;
	if (record_len > response->len - 2)
		return false;
	return goodix_parse_finger_id(response->data + 2, record_len, t) >= 0 && t->uid_len;
}

/*
 * Returns GOODIX_OK, GOODIX_DUPLICATE (*duplicate names the stored template the finger matched, or has an
 * empty uid), GOODIX_FAILED (*fail is the sensor status), GOODIX_RETRY or a negative errno.
 */
static inline int goodix_enroll(struct goodix *s, const char *uid,
				const struct goodix_ev *ev, int *fail,
				struct goodix_template *duplicate)
{
	uint8_t zero = 0, parameters[3], tid[GOODIX_TEMPLATE_ID_SIZE], fid[GOODIX_MAX_UID + 72];
	struct goodix_response response;
	struct goodix_template templates[GOODIX_MAX_TEMPLATES];
	size_t count = 0, fid_len, uid_len = strlen(uid);
	int stage, r, result = GOODIX_FAILED;
	bool shield = false;

	if (uid_len == 0 || uid_len > GOODIX_MAX_UID || uid[0] == ' ')
		return -EINVAL;
	if ((r = goodix_list_templates(s, templates, &count, fail)) != GOODIX_OK)
		return r;
	if (count >= GOODIX_MAX_TEMPLATES) {
		*fail = GOODIX_NO_SPACE;
		return GOODIX_FAILED;
	}
	if ((r = goodix_power_shield(s, GOODIX_CMD_SHIELD_ON)) < 0)
		return r;
	shield = true;
	if ((r = goodix_command(s, GOODIX_CMD_ENROLL_INIT, 0, &zero, 1,
				false, &response)) < 0)
		goto done;
	if (response.result != GOODIX_SUCCESS || response.len < sizeof(tid)) {
		*fail = response.result ? response.result : GOODIX_FAILED;
		GOODIX_LOGW("enroll init: status 0x%02x, %u bytes", response.result,
			    (unsigned int)response.len);
		goto done;
	}
	memcpy(tid, response.data, sizeof(tid));
	for (stage = 0; stage < GOODIX_ENROLL_STAGES; ) {
		r = goodix_capture(s, ev, stage, false, fail);
		if (r == GOODIX_RETRY)
			continue;
		if (r != GOODIX_OK) {
			result = r;
			goto done;
		}
		parameters[0] = 1;
		parameters[1] = s->config[2];
		parameters[2] = s->config[3];
		if ((r = goodix_command(s, GOODIX_CMD_ENROLL, 0, parameters,
					sizeof(parameters), false, &response)) < 0)
			goto done;
		if (response.len < 2) {
			*fail = response.result ? response.result : GOODIX_FAILED;
			GOODIX_LOGW("enroll stage %d: status 0x%02x, %u bytes", stage + 1,
				    response.result, (unsigned int)response.len);
			goto done;
		}
		if (response.data[1] > s->config[3] || response.result >= GOODIX_ERROR_THRESHOLD) {
			goodix_emit(ev, GOODIX_EV_RETRY, stage);
		} else {
			stage++;
			goodix_emit(ev, GOODIX_EV_PROGRESS, stage);
		}
		if (stage < GOODIX_ENROLL_STAGES &&
		    (r = goodix_wait_finger_up(s)) < 0)
			goto done;
	}
	parameters[0] = 0;
	parameters[1] = s->config[4];
	parameters[2] = s->config[5];
	if ((r = goodix_command(s, GOODIX_CMD_DUPLICATE, 0, parameters,
				sizeof(parameters), false, &response)) < 0)
		goto done;
	if (response.result != GOODIX_SUCCESS) {
		struct goodix_template found;

		*fail = response.result;
		if (goodix_parse_duplicate(&response, &found))
			GOODIX_LOGW("check-duplicate: status 0x%02x, finger matches stored template %s",
				    response.result, found.uid);
		else
			GOODIX_LOGW("check-duplicate: status 0x%02x, no readable template record (%u bytes)",
				    response.result, (unsigned int)response.len);
		if (duplicate)
			*duplicate = found;
		result = GOODIX_DUPLICATE;
		goto done;
	}
	if (goodix_encode_finger_id(fid, sizeof(fid), &fid_len, tid, uid) < 0) {
		r = -EINVAL;
		goto done;
	}
	if ((r = goodix_command(s, GOODIX_CMD_COMMIT, 0, fid, fid_len,
				false, &response)) < 0)
		goto done;
	if (response.result != GOODIX_SUCCESS) {
		*fail = response.result;
		GOODIX_LOGW("commit: status 0x%02x", response.result);
		goto done;
	}
	result = GOODIX_OK;
done:
	if (r < 0 && result == GOODIX_FAILED)
		result = r;
	if (shield && r != -ECANCELED && !s->need_reset) {
		int off = goodix_power_shield(s, GOODIX_CMD_SHIELD_OFF);

		if (off < 0 && result == GOODIX_OK)
			result = off;
	}
	return result;
}

/*
 * One touch: capture (verify mode), a5 identify, and, when the finger was on the sensor but did not match or
 * gave an unusable image, wait until it is lifted, so the caller's next attempt is a new touch (boot-13: five
 * no-matches 290 ms apart from one touch, then a lockout). libfprint's a5 reply: status (0 = match), then
 * reject detail (16 bits), score (32), study (8), a 16-bit record length and the matched template's record.
 * Returns GOODIX_OK (*matched: one of uids), GOODIX_NOMATCH, GOODIX_RETRY, GOODIX_FAILED or a negative errno.
 */
static inline int goodix_identify(struct goodix *s, const char *const *uids, int count,
				  const struct goodix_ev *ev, char *matched, int *fail)
{
	uint8_t nonce[GOODIX_TEMPLATE_ID_SIZE] = { 0 };
	struct goodix_response response;
	struct goodix_template found;
	int r, result = GOODIX_FAILED;
	bool shield = false, ours = false, touched = false;

	if (count <= 0 || count > 255)
		return -EINVAL;
	if ((r = goodix_power_shield(s, GOODIX_CMD_SHIELD_ON)) < 0)
		return r;
	shield = true;
	r = goodix_capture(s, ev, 0, true, fail);
	if (r != GOODIX_OK) {
		touched = r == GOODIX_RETRY;
		result = r;
		goto done;
	}
	touched = true;
	if ((r = goodix_command(s, GOODIX_CMD_IDENTIFY, 0, nonce, sizeof(nonce),
				false, &response)) < 0)
		goto done;
	if (response.result != GOODIX_SUCCESS) {
		GOODIX_LOGI("identify: status 0x%02x: no match", response.result);
		result = GOODIX_NOMATCH;
		goto done;
	}
	if (response.len < 9) {
		GOODIX_LOGW("identify: status 0x00 but a %u-byte reply", (unsigned int)response.len);
		*fail = GOODIX_FAILED;
		goto done;
	}
	{
		size_t record_len = (size_t)response.data[7] |
				    (size_t)response.data[8] << 8;
		uint32_t score = (uint32_t)response.data[2] | (uint32_t)response.data[3] << 8 |
				 (uint32_t)response.data[4] << 16 | (uint32_t)response.data[5] << 24;

		(void)score;	/* logged only */

		if (record_len > response.len - 9 ||
		    goodix_parse_finger_id(response.data + 9, record_len, &found) < 0) {
			GOODIX_LOGW("identify: status 0x00, score %u, but no readable template record", score);
			*fail = GOODIX_FAILED;
			goto done;
		}
		for (int i = 0; i < count; i++) {
			if (found.uid_len == strlen(uids[i]) &&
			    memcmp(found.uid, uids[i], found.uid_len) == 0) {
				ours = true;
				break;
			}
		}
		GOODIX_LOGI("identify: status 0x00, score %u, study %u, template %s%s", score, response.data[6],
			    found.uid_len ? found.uid : "(unnamed)", ours ? "" : " (not one of this user's)");
	}
	if (!ours) {
		result = GOODIX_NOMATCH;
		goto done;
	}
	memcpy(matched, found.uid, found.uid_len + 1);
	result = GOODIX_OK;
done:
	if (r < 0 && result == GOODIX_FAILED)
		result = r;
	if (touched && (result == GOODIX_NOMATCH || result == GOODIX_RETRY)) {
		int up = goodix_wait_finger_up(s);

		if (up < 0) {
			result = up;
			r = up;
		}
	}
	if (shield && r != -ECANCELED && !s->need_reset) {
		int off = goodix_power_shield(s, GOODIX_CMD_SHIELD_OFF);

		if (off < 0 && result == GOODIX_OK)
			result = off;
	}
	return result;
}

/* 1 when a template named uid is in the sensor's list, 0 when not, or goodix_list_templates' error. */
static inline int goodix_template_listed(struct goodix *s, const char *uid, int *fail)
{
	struct goodix_template templates[GOODIX_MAX_TEMPLATES];
	size_t count = 0;
	int r;

	if ((r = goodix_list_templates(s, templates, &count, fail)) != GOODIX_OK)
		return r < 0 ? r : -EPROTO;
	for (size_t i = 0; i < count; i++)
		if (templates[i].uid_len == strlen(uid) && memcmp(uid, templates[i].uid, templates[i].uid_len) == 0)
			return 1;
	return 0;
}

/*
 * One a7:00 with this payload: a little-endian record length, then the record (or, as libfprint sends it, all
 * but its last two bytes). Returns the sensor's status or a negative errno.
 */
static inline int goodix_delete_payload(struct goodix *s, const uint8_t *payload, size_t len)
{
	struct goodix_response response;
	int r;

	_Static_assert(GOODIX_CMD_DELETE_ONE == 0x00, "a7:01 would delete every template on the sensor");
	if (len < 2 || len > GOODIX_MAX_RECORD + 2)
		return -EINVAL;
	if ((r = goodix_command(s, GOODIX_CMD_DELETE, GOODIX_CMD_DELETE_ONE, payload, len, false,
				&response)) < 0)
		return r;
	return response.result;
}

static inline bool goodix_same_template(const struct goodix_template *a, const struct goodix_template *b)
{
	return a->uid_len == b->uid_len && memcmp(a->uid, b->uid, a->uid_len) == 0 &&
	       memcmp(a->tid, b->tid, GOODIX_TEMPLATE_ID_SIZE) == 0;
}

/*
 * After one delete attempt: list again and compare with the list from before the first attempt. Returns a
 * negative errno, else sets *gone (the target is no longer listed) and returns how many of the other
 * templates have disappeared (0 unless the firmware deleted more than it was asked to).
 */
static inline int goodix_delete_check(struct goodix *s, const struct goodix_template *before, size_t nbefore,
				      const struct goodix_template *t, bool *gone, int *fail)
{
	struct goodix_template after[GOODIX_MAX_TEMPLATES];
	size_t nafter = 0;
	int r, lost = 0;

	if ((r = goodix_list_templates(s, after, &nafter, fail)) != GOODIX_OK)
		return r < 0 ? r : -EPROTO;
	*gone = true;
	for (size_t j = 0; j < nafter; j++)
		if (goodix_same_template(&after[j], t))
			*gone = false;
	for (size_t i = 0; i < nbefore; i++) {
		bool kept = false;

		if (goodix_same_template(&before[i], t))
			continue;
		for (size_t j = 0; j < nafter && !kept; j++)
			kept = goodix_same_template(&after[j], &before[i]);
		if (!kept) {
			GOODIX_LOGE("delete %s also removed %s from the sensor", t->uid,
				    before[i].uid_len ? before[i].uid : "an unnamed template");
			lost++;
		}
	}
	return lost;
}

/*
 * Delete one listed template and check that the sensor's list no longer has it. Boot-11 and boot-12 sent
 * a7:04, which firmware 01010264 acknowledges with 0x00 while keeping the template; libfprint deletes with
 * a7:00. Each form below is one a7:00, and only a template gone from the list counts:
 *   libfprint   encode_finger_id()'s buffer sent with its fid_len, as gx_fp_template_delete() does (the
 *               length prefix and the record without its last two bytes)
 *   listed      the length prefix and the record exactly as the sensor listed it
 *   re-encoded  the length prefix and the whole record rebuilt from tid and uid, if it differs from the listed one
 * libfprint never sends the last two, so they go out only while every stored template is gbos_fp's (named
 * GBOS-): a firmware that took an odd payload as a wildcard must not cost Windows Hello its prints. After every
 * attempt the whole list is compared with the one from before; if any other template went missing, no delete
 * is sent again until gbos_fp restarts (s->delete_blocked).
 * Returns GOODIX_OK (gone, nothing else lost), GOODIX_FAILED (*fail: the last delete status, or GOODIX_FAILED
 * if the sensor reported success but still lists it, another template was lost, or deletes are blocked) or a
 * negative errno.
 */
static inline int goodix_delete_template(struct goodix *s, const struct goodix_template *t, int *fail)
{
	uint8_t fid[GOODIX_MAX_UID + 72], listed[GOODIX_MAX_RECORD + 2];
	struct goodix_delete_form { const uint8_t *payload; size_t len; const char *form; } forms[3];
	struct goodix_template before[GOODIX_MAX_TEMPLATES];
	size_t fid_len = 0, nbefore = 0;
	int nforms = 0, status = GOODIX_FAILED, r;
	bool encoded, others_gbos = true, gone, listed_now = false;

	if (!t->uid_len)
		return -EINVAL;
	if (s->delete_blocked) {
		GOODIX_LOGW("delete %s not sent: an earlier delete removed other templates", t->uid);
		*fail = GOODIX_FAILED;
		return GOODIX_FAILED;
	}
	if ((r = goodix_list_templates(s, before, &nbefore, fail)) != GOODIX_OK)
		return r < 0 ? r : -EPROTO;
	for (size_t i = 0; i < nbefore; i++) {
		if (goodix_same_template(&before[i], t))
			listed_now = true;
		else if (before[i].uid_len < 5 || memcmp(before[i].uid, "GBOS-", 5) != 0)
			others_gbos = false;
	}
	if (!listed_now)
		return GOODIX_OK;
	encoded = goodix_encode_finger_id(fid, sizeof(fid), &fid_len, t->tid, t->uid) == 0;
	if (encoded)
		forms[nforms++] = (struct goodix_delete_form){ fid, fid_len - 2, "libfprint" };
	if (others_gbos && t->raw_len) {
		listed[0] = (uint8_t)t->raw_len;
		listed[1] = (uint8_t)(t->raw_len >> 8);
		memcpy(listed + 2, t->raw, t->raw_len);
		forms[nforms++] = (struct goodix_delete_form){ listed, t->raw_len + 2, "listed" };
	}
	if (others_gbos && encoded && !(t->raw_len == fid_len - 2 && memcmp(t->raw, fid + 2, t->raw_len) == 0))
		forms[nforms++] = (struct goodix_delete_form){ fid, fid_len, "re-encoded" };
	if (!others_gbos)
		GOODIX_LOGI("delete %s: the sensor holds templates not named GBOS-, so only libfprint's payload is sent",
			    t->uid);
	for (int i = 0; i < nforms; i++) {
		r = goodix_delete_payload(s, forms[i].payload, forms[i].len);
		if (r < 0)
			return r;
		status = r;
		GOODIX_LOGI("delete %s (a7:00, %s record, %u bytes, type %02x, finger %02x): status 0x%02x", t->uid,
			    forms[i].form, (unsigned int)forms[i].len, forms[i].payload[3], forms[i].payload[4],
			    (unsigned int)status);
		for (int check = 0; check < 2; check++) {
			if ((r = goodix_delete_check(s, before, nbefore, t, &gone, fail)) < 0)
				return r;
			if (r > 0) {
				s->delete_blocked = true;
				GOODIX_LOGE("delete %s (%s record) removed %d other template(s): no more deletes until "
					    "gbos_fp restarts", t->uid, forms[i].form, r);
				*fail = GOODIX_FAILED;
				return GOODIX_FAILED;
			}
			if (gone)
				return GOODIX_OK;
			if (status != GOODIX_SUCCESS)
				break;
			usleep(100 * 1000);	/* acknowledged: give a flash write a moment before checking again */
		}
		GOODIX_LOGW("delete %s: still listed after status 0x%02x", t->uid, (unsigned int)status);
	}
	*fail = status == GOODIX_SUCCESS ? GOODIX_FAILED : status;
	return GOODIX_FAILED;
}

/* GOODIX_OK (deleted, verified), GOODIX_NORECORD (not in the sensor's list), GOODIX_FAILED or a negative errno. */
static inline int goodix_delete(struct goodix *s, const char *uid, int *fail)
{
	struct goodix_template templates[GOODIX_MAX_TEMPLATES];
	size_t count = 0;
	int r;

	if ((r = goodix_list_templates(s, templates, &count, fail)) != GOODIX_OK)
		return r;
	for (size_t i = 0; i < count; i++) {
		if (templates[i].uid_len != strlen(uid) ||
		    memcmp(uid, templates[i].uid, templates[i].uid_len) != 0)
			continue;
		return goodix_delete_template(s, &templates[i], fail);
	}
	return GOODIX_NORECORD;
}

#endif
