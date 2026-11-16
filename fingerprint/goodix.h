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
#define GOODIX_USB_RESET 0x5514
#define GOODIX_INTERFACE 0
#define GOODIX_CFG_SIZE 128
#define GOODIX_CFG_CRC_OFFSET 124

enum {
	GOODIX_OK = 0,
	GOODIX_NOMATCH,
	GOODIX_RETRY,
	GOODIX_NORECORD,
	GOODIX_FAILED,
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
	bool was_reset;
	bool need_reset;
	bool cancel;
	uint8_t sequence;
	uint8_t config[26];
	char fw_version[9];
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
	int n;

	for (;;) {
		unsigned int timeout = cancellable ? 250 : timeout_ms;

		if (cancellable && goodix_cancel_requested(s))
			return -ECANCELED;
		n = goodix_bulk(s, GOODIX_EP_IN, buffer, (unsigned int)buffer_size, timeout);
		if (n == -ETIMEDOUT && cancellable)
			continue;
		if (n < 0) {
			if (n == -ETIMEDOUT)
				s->need_reset = true;
			return n;
		}
		if (n == 0 && cancellable)
			continue;
		if (goodix_parse_packet(buffer, (size_t)n, packet) < 0) {
			s->need_reset = true;
			GOODIX_LOGE("invalid Goodix packet (%d bytes)", n);
			return -EPROTO;
		}
		return 0;
	}
}

static inline int goodix_command(struct goodix *s, uint8_t cmd0, uint8_t cmd1,
				 const uint8_t *payload, size_t payload_len,
				 bool cancellable, struct goodix_response *response)
{
	uint8_t tx[GOODIX_HEADER_SIZE + GOODIX_MAX_PAYLOAD + GOODIX_CRC_SIZE];
	uint8_t rx[GOODIX_HEADER_SIZE + GOODIX_MAX_PAYLOAD + GOODIX_CRC_SIZE];
	struct goodix_packet packet;
	size_t tx_len;
	int n, r;

	if (goodix_cancel_requested(s))
		return -ECANCELED;
	if (goodix_build_packet(tx, sizeof(tx), &tx_len, cmd0, cmd1,
				++s->sequence, payload, payload_len) < 0)
		return -EINVAL;
	n = goodix_bulk(s, GOODIX_EP_OUT, tx, (unsigned int)tx_len, 1000);
	if (n < 0) {
		s->need_reset = true;
		return n;
	}
	if ((size_t)n != tx_len) {
		s->need_reset = true;
		return -EIO;
	}
	r = goodix_receive(s, &packet, rx, sizeof(rx), cancellable, 2000);
	if (r < 0) {
		if (r == -ECANCELED)
			s->need_reset = true;
		return r;
	}
	if (packet.cmd0 != GOODIX_CMD_RESPONSE || packet.payload_len < 2 ||
	    packet.payload[0] != GOODIX_SUCCESS || packet.payload[1] != cmd0) {
		s->need_reset = true;
		return -EPROTO;
	}
	if (goodix_cancel_requested(s)) {
		s->need_reset = true;
		return -ECANCELED;
	}
	r = goodix_receive(s, &packet, rx, sizeof(rx), cancellable, 5000);
	if (r < 0) {
		if (r == -ECANCELED)
			s->need_reset = true;
		return r;
	}
	if (packet.cmd0 != cmd0 || packet.payload_len < 1) {
		s->need_reset = true;
		return -EPROTO;
	}
	response->result = packet.payload[0];
	response->len = packet.payload_len - 1;
	if (response->len)
		memcpy(response->data, packet.payload + 1, response->len);
	return 0;
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
	out[6] = GOODIX_ENROLL_STAGES;
	out[26] = 1;
	crc = goodix_crc32(out, GOODIX_CFG_CRC_OFFSET);
	out[124] = (uint8_t)crc;
	out[125] = (uint8_t)(crc >> 8);
	out[126] = (uint8_t)(crc >> 16);
	out[127] = (uint8_t)(crc >> 24);
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
	if (!s->was_reset || s->need_reset) {
		if (ioctl(s->fd, (int)GOODIX_USB_RESET, 0) < 0) {
			r = -GOODIX_ERRNO;
			goto fail;
		}
		s->was_reset = true;
		s->need_reset = false;
	}
	if (ioctl(s->fd, (int)GOODIX_USB_CLAIMINTERFACE, &interface) < 0) {
		r = -GOODIX_ERRNO;
		goto fail;
	}
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

	/* Configure runtime thresholds only; do not write configuration to flash. */
	goodix_make_config(s, config);
	if ((r = goodix_command(s, GOODIX_CMD_CONFIG, 0, config, sizeof(config),
			       false, &response)) < 0)
		goto fail;
	if (response.result != GOODIX_SUCCESS) {
		GOODIX_LOGE("Goodix runtime configuration failed: 0x%02x", response.result);
		r = -EIO;
		goto fail;
	}
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

static inline int goodix_capture(struct goodix *s, const struct goodix_ev *ev,
				 int stage, int *fail)
{
	uint8_t parameters[3] = { 0, s->config[4], s->config[5] };
	struct goodix_response response;
	int r;

	goodix_emit(ev, GOODIX_EV_CAPTURE, 0);
	r = goodix_command(s, GOODIX_CMD_CAPTURE, 0, parameters, sizeof(parameters),
			   true, &response);
	if (r < 0)
		return r;
	if (response.result >= GOODIX_ERROR_THRESHOLD) {
		*fail = response.result;
		goodix_emit(ev, GOODIX_EV_RETRY, stage);
		return GOODIX_RETRY;
	}
	if (response.len < 2) {
		*fail = GOODIX_FAILED;
		return GOODIX_FAILED;
	}
	if (response.data[0] < s->config[4] || response.data[1] < s->config[5]) {
		goodix_emit(ev, GOODIX_EV_RETRY, stage);
		return GOODIX_RETRY;
	}
	return GOODIX_OK;
}

static inline int goodix_enroll(struct goodix *s, const char *uid,
				const struct goodix_ev *ev, int *fail)
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
		goto done;
	}
	memcpy(tid, response.data, sizeof(tid));
	for (stage = 0; stage < GOODIX_ENROLL_STAGES; ) {
		r = goodix_capture(s, ev, stage, fail);
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
		*fail = response.result;
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

static inline int goodix_identify(struct goodix *s, const char *const *uids, int count,
				  const struct goodix_ev *ev, char *matched, int *fail)
{
	uint8_t nonce[GOODIX_TEMPLATE_ID_SIZE] = { 0 };
	struct goodix_response response;
	struct goodix_template found;
	int r, result = GOODIX_FAILED;
	bool shield = false, ours = false;

	if (count <= 0 || count > 255)
		return -EINVAL;
	if ((r = goodix_power_shield(s, GOODIX_CMD_SHIELD_ON)) < 0)
		return r;
	shield = true;
	r = goodix_capture(s, ev, 0, fail);
	if (r != GOODIX_OK) {
		result = r;
		goto done;
	}
	if ((r = goodix_command(s, GOODIX_CMD_IDENTIFY, 0, nonce, sizeof(nonce),
				false, &response)) < 0)
		goto done;
	if (response.result != GOODIX_SUCCESS) {
		result = GOODIX_NOMATCH;
		goto done;
	}
	if (response.len < 9) {
		*fail = GOODIX_FAILED;
		goto done;
	}
	{
		size_t record_len = (size_t)response.data[7] |
				    (size_t)response.data[8] << 8;

		if (record_len > response.len - 9 ||
		    goodix_parse_finger_id(response.data + 9, record_len, &found) < 0) {
			*fail = GOODIX_FAILED;
			goto done;
		}
	}
	for (int i = 0; i < count; i++) {
		if (found.uid_len == strlen(uids[i]) &&
		    memcmp(found.uid, uids[i], found.uid_len) == 0) {
			ours = true;
			break;
		}
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
	if (shield && r != -ECANCELED && !s->need_reset) {
		int off = goodix_power_shield(s, GOODIX_CMD_SHIELD_OFF);

		if (off < 0 && result == GOODIX_OK)
			result = off;
	}
	return result;
}

static inline int goodix_delete(struct goodix *s, const char *uid, int *fail)
{
	uint8_t fid[GOODIX_MAX_UID + 72];
	struct goodix_template templates[GOODIX_MAX_TEMPLATES];
	struct goodix_response response;
	size_t count = 0, fid_len;
	int r;

	if ((r = goodix_list_templates(s, templates, &count, fail)) != GOODIX_OK)
		return r;
	for (size_t i = 0; i < count; i++) {
		if (templates[i].uid_len != strlen(uid) ||
		    memcmp(uid, templates[i].uid, templates[i].uid_len) != 0)
			continue;
		if (goodix_encode_finger_id(fid, sizeof(fid), &fid_len,
					    templates[i].tid, uid) < 0)
			return -EINVAL;
		if ((r = goodix_command(s, GOODIX_CMD_DELETE, GOODIX_CMD_DELETE_ONE,
					fid, fid_len, false, &response)) < 0)
			return r;
		if (response.result == GOODIX_SUCCESS || response.result == GOODIX_NO_RECORD)
			return GOODIX_OK;
		*fail = response.result;
		return GOODIX_FAILED;
	}
	return GOODIX_NORECORD;
}

#endif
