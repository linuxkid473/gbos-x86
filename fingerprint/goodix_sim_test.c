/* SPDX-License-Identifier: LGPL-2.1-or-later */
/*
 * Host tests for template cleanup, duplicate handling and cancel recovery, against a simulated Goodix MOC
 * sensor: it keeps a template database, answers each command as the 27c6:6384 firmware does (an
 * acknowledgement, then a response), can hold packets back so they arrive late (as after a cancel), and
 * models which finger is on the sensor so check-duplicate (a3) matches the way the firmware does: against
 * every stored template, whoever stored it.
 *
 * The 2026-11-18 boot-10 failure is test_boot10_stale_template: yesterday's live-boot template makes every
 * enrollment of that finger fail at check-duplicate with status 2.
 */
#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

static void sim_log(const char *format, ...);
#define GOODIX_LOGI(...) sim_log(__VA_ARGS__)
#define GOODIX_LOGW(...) sim_log(__VA_ARGS__)
#define GOODIX_LOGE(...) sim_log(__VA_ARGS__)
static int sim_ioctl(int fd, unsigned long request, ...);
#define ioctl sim_ioctl
#include "goodix.h"
#undef ioctl
#define FP_LOGI(...) sim_log(__VA_ARGS__)
#define FP_LOGW(...) sim_log(__VA_ARGS__)
#include "gbos_fp_policy.h"

#define PKT_MAX (GOODIX_HEADER_SIZE + GOODIX_MAX_PAYLOAD + GOODIX_CRC_SIZE)

/* rec: the record as the firmware keeps and lists it (little-endian length first) */
struct stored { struct goodix_template t; int finger; uint8_t rec[GOODIX_MAX_RECORD + 2]; size_t rec_len; };

/*
 * How a7:00 behaves. Every mode answers a7:04 with 0x00 and deletes nothing (firmware 01010264, boot-12), and
 * a7:01 (delete all) aborts the test.
 */
enum {
	DELETE_TID,		/* matches the tid (libfprint's short payload works, as fprintd's delete does) */
	DELETE_TID_UID,		/* deletes the template whose tid and uid match (libfprint's payload gets 0x9c) */
	DELETE_WILDCARD,	/* libfprint's payload: 0x00, nothing deleted; a whole record: every template goes */
	DELETE_EXACT,		/* only an exact copy of the stored record deletes it */
	DELETE_IGNORED,		/* answers success, keeps the template */
	DELETE_NO_RECORD,	/* answers "no such record" (0x9c), keeps the template */
};

static struct {
	struct stored db[GOODIX_MAX_TEMPLATES];
	int n;
	int finger;			/* the finger on the sensor during an enroll */
	uint8_t dup_status;		/* what a3 returns for a duplicate (boot-10: 2) */
	uint8_t commit_status;
	bool account_id;		/* the firmware writes its own account id into a committed record */
	int delete_mode;
	int deletes;			/* a7 commands received */
	int deletes_04;			/* of them a7:04, which deletes nothing */
	/* touches (used when ntouches > 0): each puts touches[i] on the sensor until b0:02 lifts it; a capture
	 * with no finger and no touch left never answers */
	int touches[8], ntouches, next_touch, finger_on;
	int bad_quality;		/* this many captures report quality 0 */
	int capture_mode;		/* the last a2's first parameter: 0 enroll, 1 verify */
	int captures, identifies;
	int cancel_after_ack;		/* the framework cancels as soon as this command's acknowledgement is read */
	/* packets to send, in order */
	uint8_t q[32][PKT_MAX];
	size_t qlen[32];
	int qn;
	/* held back: sent before the replies to the next command (a late packet after a cancel) */
	uint8_t late[8][PKT_MAX];
	size_t latelen[8];
	int laten;
	int cancel_cmd;			/* this command's ... */
	int cancel_at;			/* ... 0: acknowledgement, 1: response is held back and the operation cancelled */
	struct goodix *dev;
	bool shield;
	uint8_t cmds[512];
	int ncmds;
	int stale_sent;
	bool ghost_dup;			/* a3 names a template that LIST does not return */
	char log[64][256];
	int nlog;
} sim;

static void sim_log(const char *format, ...)
{
	va_list ap;

	va_start(ap, format);
	vsnprintf(sim.log[sim.nlog % 64], sizeof(sim.log[0]), format, ap);
	va_end(ap);
	sim.nlog++;
}

static bool logged(const char *needle)
{
	for (int i = 0; i < 64 && i < sim.nlog; i++)
		if (strstr(sim.log[i], needle))
			return true;
	return false;
}

static void sim_reset(void)
{
	memset(&sim, 0, sizeof(sim));
	sim.cancel_cmd = -1;
}

static void emit(uint8_t buf[][PKT_MAX], size_t *lens, int *n, uint8_t cmd0, uint8_t cmd1,
		 const uint8_t *payload, size_t len)
{
	assert(goodix_build_packet(buf[*n], PKT_MAX, &lens[*n], cmd0, cmd1, 1, payload, len) == 0);
	(*n)++;
}

static void store_tid(const char *uid, int finger, const uint8_t *tid)
{
	struct stored *s = &sim.db[sim.n++];

	memset(s, 0, sizeof(*s));
	for (int i = 0; i < GOODIX_TEMPLATE_ID_SIZE; i++)
		s->t.tid[i] = tid ? tid[i] : (uint8_t)(sim.n * 16 + i);
	snprintf(s->t.uid, sizeof(s->t.uid), "%s", uid);
	s->t.uid_len = strlen(uid);
	s->finger = finger;
	assert(goodix_encode_finger_id(s->rec, sizeof(s->rec), &s->rec_len, s->t.tid, s->t.uid) == 0);
	if (sim.account_id)
		memset(s->rec + 2 + 4, 0xa5, 32);	/* record bytes 4..35 */
}

static void store(const char *uid, int finger) { store_tid(uid, finger, NULL); }

static int find(const char *uid)
{
	for (int i = 0; i < sim.n; i++)
		if (strcmp(sim.db[i].t.uid, uid) == 0)
			return i;
	return -1;
}

static int find_tid(const uint8_t *tid)
{
	for (int i = 0; i < sim.n; i++)
		if (memcmp(sim.db[i].t.tid, tid, GOODIX_TEMPLATE_ID_SIZE) == 0)
			return i;
	return -1;
}

static size_t put_record(uint8_t *out, const struct stored *s)
{
	memcpy(out, s->rec, s->rec_len);
	return s->rec_len;	/* little-endian record length, then the record */
}

/* The firmware's answer to one command. */
static void respond(uint8_t cmd0, uint8_t cmd1, const uint8_t *in, size_t in_len)
{
	uint8_t r[GOODIX_MAX_PAYLOAD], ack[2] = { GOODIX_SUCCESS, cmd0 };
	size_t len = 1;
	struct goodix_template t;
	uint8_t rec[GOODIX_MAX_RECORD];
	size_t declared;
	int i, dup = -1;
	bool hold_ack = sim.cancel_cmd == cmd0 && sim.cancel_at == 0;
	bool hold_resp = sim.cancel_cmd == cmd0, no_resp = false;

	memset(r, 0, sizeof(r));
	r[0] = GOODIX_SUCCESS;
	switch (cmd0) {
	case GOODIX_CMD_VERSION:
		memcpy(r + 3, "APP", 3);
		memcpy(r + 11, "01010264", 8);
		len = 20;
		break;
	case GOODIX_CMD_CONFIG:
		r[2] = GOODIX_MAX_TEMPLATES;
		len = 3;
		break;
	case GOODIX_CMD_POWER_BUTTON_SHIELD:
		sim.shield = cmd1 == GOODIX_CMD_SHIELD_ON;
		break;
	case GOODIX_CMD_LIST:
		r[1] = (uint8_t)sim.n;
		len = 2;
		for (i = 0; i < sim.n; i++)
			len += put_record(r + len, &sim.db[i]);
		break;
	case GOODIX_CMD_DELETE:
		if (cmd1 == 0x01) {
			fprintf(stderr, "a7:01 (delete every template) sent\n");
			abort();
		}
		sim.deletes++;
		if (cmd1 == 0x04) {	/* boot-12: acknowledged, nothing deleted */
			sim.deletes_04++;
			break;
		}
		assert(cmd1 == GOODIX_CMD_DELETE_ONE && in_len > 2);
		/* the whole record, or libfprint's payload: its declared length, two bytes short of the record */
		declared = (size_t)(in[0] | in[1] << 8);
		assert(in_len == declared + 2 || in_len == declared);
		assert(declared <= sizeof(rec));
		memset(rec, 0, sizeof(rec));
		memcpy(rec, in + 2, in_len - 2);
		assert(goodix_parse_finger_id(rec, declared, &t) == 0);
		if (sim.delete_mode == DELETE_WILDCARD) {
			if (in_len == declared + 2)
				sim.n = 0;
			break;
		}
		i = sim.delete_mode == DELETE_TID ? find_tid(t.tid) : find(t.uid);
		if (i >= 0 && sim.delete_mode == DELETE_EXACT &&
		    !(sim.db[i].rec_len == in_len && memcmp(sim.db[i].rec, in, in_len) == 0))
			i = -1;
		if (i >= 0 && memcmp(sim.db[i].t.tid, t.tid, sizeof(t.tid)) != 0)
			i = -1;
		if (sim.delete_mode == DELETE_NO_RECORD || i < 0)
			r[0] = GOODIX_NO_RECORD;
		else if (sim.delete_mode != DELETE_IGNORED)
			sim.db[i] = sim.db[--sim.n];
		break;
	case GOODIX_CMD_ENROLL_INIT:
		memset(r + 1, 0x77, GOODIX_TEMPLATE_ID_SIZE);
		len = 1 + GOODIX_TEMPLATE_ID_SIZE;
		break;
	case GOODIX_CMD_CAPTURE:
		assert(in_len == 3);
		sim.capture_mode = in[0];
		if (sim.ntouches && !sim.finger_on) {
			if (sim.next_touch == sim.ntouches) {
				no_resp = true;	/* waits for a finger that never comes */
				break;
			}
			sim.finger_on = sim.touches[sim.next_touch++];
		}
		sim.captures++;
		r[1] = sim.bad_quality ? 0 : 0xff;
		r[2] = 0xff;
		if (sim.bad_quality)
			sim.bad_quality--;
		len = 3;
		break;
	case GOODIX_CMD_ENROLL:
		len = 3;	/* r[2] = 0: within config[3] */
		break;
	case GOODIX_CMD_FINGER_MODE:
		if (cmd1 == GOODIX_CMD_FINGER_UP)
			sim.finger_on = 0;
		break;
	case GOODIX_CMD_IDENTIFY:
		/* matches only an image captured for verification (libfprint's a2 {1, config[10], config[11]}) */
		sim.identifies++;
		for (i = 0; i < sim.n && sim.capture_mode == 1; i++)
			if (sim.db[i].finger == (sim.ntouches ? sim.finger_on : sim.finger))
				dup = i;
		if (dup < 0) {
			r[0] = 0x01;	/* any nonzero status is "no match" (libfprint) */
			break;
		}
		r[3] = 0xb0;	/* score 1200 */
		r[4] = 0x04;
		len = 8 + put_record(r + 8, &sim.db[dup]);
		break;
	case GOODIX_CMD_DUPLICATE:
		for (i = 0; i < sim.n; i++)
			if (sim.db[i].finger == sim.finger)
				dup = i;
		if (sim.ghost_dup) {
			struct stored ghost;

			memset(&ghost, 0, sizeof(ghost));
			snprintf(ghost.t.uid, sizeof(ghost.t.uid), "GBOS-cccccccc-u10-00000077");
			assert(goodix_encode_finger_id(ghost.rec, sizeof(ghost.rec), &ghost.rec_len, ghost.t.tid,
						       ghost.t.uid) == 0);
			r[0] = sim.dup_status;
			len = 1 + put_record(r + 1, &ghost);
		} else if (dup >= 0) {
			r[0] = sim.dup_status;
			len = 1 + put_record(r + 1, &sim.db[dup]);
		}
		break;
	case GOODIX_CMD_COMMIT:
		assert(goodix_parse_finger_id(in + 2, in_len - 2, &t) == 0);
		r[0] = sim.commit_status;
		if (sim.commit_status < GOODIX_ERROR_THRESHOLD)
			store_tid(t.uid, sim.finger, t.tid);
		break;
	default:
		assert(!"command the simulator does not know");
	}
	if (hold_ack)
		emit(sim.late, sim.latelen, &sim.laten, GOODIX_CMD_RESPONSE, 0, ack, 2);
	else
		emit(sim.q, sim.qlen, &sim.qn, GOODIX_CMD_RESPONSE, 0, ack, 2);
	if (no_resp)
		return;
	if (hold_resp)
		emit(sim.late, sim.latelen, &sim.laten, cmd0, cmd1, r, len);
	else
		emit(sim.q, sim.qlen, &sim.qn, cmd0, cmd1, r, len);
}

static int sim_ioctl(int fd, unsigned long request, ...)
{
	struct goodix_usb_bulk *bulk;
	struct goodix_packet p;
	va_list ap;

	(void)fd;
	if (request == (unsigned long)(int)GOODIX_USB_CLAIMINTERFACE ||
	    request == (unsigned long)(int)GOODIX_USB_RELEASEINTERFACE)
		return 0;
	assert(request == (unsigned long)(int)GOODIX_USB_BULK);
	va_start(ap, request);
	bulk = va_arg(ap, struct goodix_usb_bulk *);
	va_end(ap);
	if (bulk->ep == GOODIX_EP_OUT) {
		assert(goodix_parse_packet(bulk->data, bulk->len, &p) == 0);
		sim.cmds[sim.ncmds++ % sizeof(sim.cmds)] = p.cmd0;
		/* what the sensor still had to say about an abandoned command comes first */
		for (int i = 0; i < sim.laten; i++) {
			memcpy(sim.q[sim.qn], sim.late[i], sim.latelen[i]);
			sim.qlen[sim.qn++] = sim.latelen[i];
			sim.stale_sent++;
		}
		sim.laten = 0;
		respond(p.cmd0, p.cmd1, p.payload, p.payload_len);
		if (sim.cancel_cmd == p.cmd0)
			sim.cancel_cmd = -1;	/* once */
		return (int)bulk->len;
	}
	assert(bulk->ep == GOODIX_EP_IN);
	if (!sim.qn) {
		/* nothing to send: the user cancels (the framework's cancel arrives while the HAL waits) */
		goodix_request_cancel(sim.dev);
		errno = ETIMEDOUT;
		return -1;
	}
	assert(sim.qlen[0] <= bulk->len);
	memcpy(bulk->data, sim.q[0], sim.qlen[0]);
	if (sim.cancel_after_ack > 0 && sim.q[0][0] == GOODIX_CMD_RESPONSE &&
	    sim.q[0][GOODIX_HEADER_SIZE + 1] == sim.cancel_after_ack) {
		goodix_request_cancel(sim.dev);
		sim.cancel_after_ack = -1;
	}
	{
		int n = (int)sim.qlen[0];

		memmove(sim.q, sim.q[1], sizeof(sim.q[0]) * (size_t)(sim.qn - 1));
		memmove(sim.qlen, sim.qlen + 1, sizeof(sim.qlen[0]) * (size_t)(sim.qn - 1));
		sim.qn--;
		return n;
	}
}

static char node[] = "/tmp/gbsim.XXXXXX";

static void make_node(void)
{
	uint8_t d[18] = { 18, 1 };
	int fd = mkstemp(node);

	assert(fd >= 0);
	d[8] = 0xc6; d[9] = 0x27; d[10] = 0x84; d[11] = 0x63;
	assert(write(fd, d, sizeof(d)) == (ssize_t)sizeof(d));
	close(fd);
}

static void open_dev(struct goodix *s)
{
	goodix_init(s);
	snprintf(s->path, sizeof(s->path), "%s", node);
	sim.dev = s;
	assert(goodix_open(s) == 0);
	assert(strcmp(s->fw_version, "01010264") == 0);
}

/* ---- names and policy ---- */

static void test_names(void)
{
	char n[FP_NAME_MAX + 1];
	struct fp_name p;

	assert(fp_name_format(n, true, "0123abcd", 10, 0x7fffffff) > 0);
	assert(strcmp(n, "GBOS-L-0123abcd-u10-7fffffff") == 0);
	assert(fp_name_parse(n, &p) && p.live && p.user == 10 && p.id == 0x7fffffff &&
	       strcmp(p.instance, "0123abcd") == 0);
	/* names stay within 32 bytes: a live name has room for a 6-digit Android user id */
	assert(fp_name_format(n, true, "0123abcd", 999999, 0xffffffff) == 32);
	assert(fp_name_format(n, true, "0123abcd", 1000000, 0xffffffff) < 0);
	assert(fp_name_format(n, false, "0123abcd", 0, 1) > 0 && strcmp(n, "GBOS-0123abcd-u0-00000001") == 0);
	assert(fp_name_parse(n, &p) && !p.live && p.user == 0 && p.id == 1);
	assert(fp_name_format(n, false, "0123abcd", -1, 1) < 0);

	static const char *const not_ours[] = {
		"", "GBOS", "GBOS-", "GBOS-0123ABCD-u0-00000001", "GBOS-0123abcd-u00-00000001",
		"GBOS-0123abcd-u0-0000001", "GBOS-0123abcd-u0-000000011", "GBOS-0123abcd-0-00000001",
		"GBOS-L0123abcd-u0-00000001", "GBOS-X-0123abcd-u0-00000001", "gbos-0123abcd-u0-00000001",
		"FP1-20261117-7-3F2A9B11-user", "GBOS-0123abcd-u-00000001",
	};
	for (size_t i = 0; i < sizeof(not_ours) / sizeof(not_ours[0]); i++)
		assert(!fp_name_parse(not_ours[i], &p));
}

static void test_policy(void)
{
	static const int32_t ids[] = { 0x11 };
	struct fp_policy live = { "aaaaaaaa", true, true, 10, ids, 1, true, false };
	struct fp_policy install = { "bbbbbbbb", false, false, 10, ids, 1, true, false };
	struct fp_policy p;

	/* live boot, no install */
	assert(fp_classify(&live, "GBOS-L-aaaaaaaa-u10-00000011") == FP_KEEP_ENROLLED);
	assert(fp_classify(&live, "GBOS-L-aaaaaaaa-u10-00000012") == FP_DELETE_UNKNOWN);
	assert(fp_classify(&live, "GBOS-L-aaaaaaaa-u0-00000012") == FP_KEEP_OTHER_USER);
	assert(fp_classify(&live, "GBOS-L-cccccccc-u10-00000011") == FP_DELETE_OLD_LIVE);
	assert(fp_classify(&live, "GBOS-cccccccc-u10-2ab7f1c0") == FP_DELETE_ORPHAN);	/* boot-10 */
	assert(fp_classify(&live, "FP1-20261117-7-3F2A9B11-user") == FP_KEEP_NOT_GBOS);
	assert(fp_classify(&live, "") == FP_KEEP_NOT_GBOS);
	/* the same names on a live boot when an install exists: keep the old-style one */
	p = live;
	p.no_install = false;
	assert(fp_classify(&p, "GBOS-cccccccc-u10-2ab7f1c0") == FP_KEEP_OTHER_SYSTEM);
	assert(fp_classify(&p, "GBOS-L-cccccccc-u10-00000011") == FP_DELETE_OLD_LIVE);

	/* install (persistent /data): never touch what Android may still reference */
	assert(fp_classify(&install, "GBOS-bbbbbbbb-u10-00000011") == FP_KEEP_ENROLLED);
	assert(fp_classify(&install, "GBOS-bbbbbbbb-u0-00000011") == FP_KEEP_OTHER_USER);
	assert(fp_classify(&install, "GBOS-cccccccc-u10-00000011") == FP_KEEP_OTHER_SYSTEM);
	assert(fp_classify(&install, "GBOS-bbbbbbbb-u10-00000012") == FP_DELETE_UNKNOWN);
	p = install;
	p.no_install = true;	/* never set on a persistent /data, but even so: */
	assert(fp_classify(&p, "GBOS-cccccccc-u10-00000011") == FP_KEEP_OTHER_SYSTEM);
	/* a state file that could not be read completely: no guessing */
	p = install;
	p.ids_trusted = false;
	assert(fp_classify(&p, "GBOS-bbbbbbbb-u10-00000012") == FP_KEEP_UNTRUSTED);
	assert(fp_classify(&p, "GBOS-bbbbbbbb-u10-00000011") == FP_KEEP_ENROLLED);
	/* a live name with this instance but a persistent /data is not this /data's */
	assert(fp_classify(&install, "GBOS-L-bbbbbbbb-u10-00000011") == FP_DELETE_OLD_LIVE);

	/* the install's own /data (--installed, statfs persistent, instance stable): the only persistent
	 * /data on the machine, so another persistent instance's /data is gone */
	p = install;
	p.installed = true;
	assert(fp_classify(&p, "GBOS-1c2ac773-u10-5cfb789b") == FP_DELETE_REPLACED);	/* 2026-11-18 */
	assert(fp_classify(&p, "GBOS-cccccccc-u0-00000011") == FP_DELETE_REPLACED);
	assert(fp_classify(&p, "GBOS-bbbbbbbb-u10-00000011") == FP_KEEP_ENROLLED);
	assert(fp_classify(&p, "GBOS-bbbbbbbb-u0-00000011") == FP_KEEP_OTHER_USER);
	assert(fp_classify(&p, "GBOS-bbbbbbbb-u10-00000012") == FP_DELETE_UNKNOWN);
	assert(fp_classify(&p, "FP1-20261117-7-3F2A9B11-user") == FP_KEEP_NOT_GBOS);
	/* an id Android still lists keeps its template even under another instance (lost instance file) */
	assert(fp_classify(&p, "GBOS-cccccccc-u10-00000011") == FP_KEEP_ENROLLED);
	p.ids_trusted = false;
	assert(fp_classify(&p, "GBOS-cccccccc-u10-00000033") == FP_KEEP_UNTRUSTED);
	assert(fp_classify(&p, "GBOS-cccccccc-u0-00000033") == FP_DELETE_REPLACED);
	/* never on tmpfs, whatever the flag says */
	p = live;
	p.no_install = false;
	p.installed = true;
	assert(fp_classify(&p, "GBOS-cccccccc-u10-2ab7f1c0") == FP_KEEP_OTHER_SYSTEM);
}

/* ---- against the simulated sensor ---- */

static void test_reconcile_live(void)
{
	static const int32_t ids[] = { 0x11 };
	struct fp_policy p = { "aaaaaaaa", true, true, 10, ids, 1, true, false };
	struct goodix s;
	int deleted, failed, fail = 0;

	sim_reset();
	store("GBOS-cccccccc-u10-2ab7f1c0", 1);		/* 2026-11-17's live boot, old name */
	store("GBOS-L-dddddddd-u10-00000005", 2);	/* an earlier live boot */
	store("GBOS-L-aaaaaaaa-u10-00000011", 3);	/* enrolled now */
	store("GBOS-L-aaaaaaaa-u10-00000099", 4);	/* this boot's, Android does not know it */
	store("GBOS-L-aaaaaaaa-u0-00000099", 5);	/* another user */
	store("FP1-20261117-7-3F2A9B11-user", 6);	/* another OS */
	open_dev(&s);
	assert(fp_reconcile(&s, &p, &deleted, &failed, &fail) == GOODIX_OK);
	assert(deleted == 3 && failed == 0 && sim.n == 3);
	assert(find("GBOS-L-aaaaaaaa-u10-00000011") >= 0 && find("GBOS-L-aaaaaaaa-u0-00000099") >= 0 &&
	       find("FP1-20261117-7-3F2A9B11-user") >= 0);
	assert(fp_reconcile(&s, &p, &deleted, &failed, &fail) == GOODIX_OK && deleted == 0);
	goodix_close(&s);
}

static void test_reconcile_install(void)
{
	static const int32_t ids[] = { 0x11, 0x22 };
	struct fp_policy p = { "bbbbbbbb", false, false, 10, ids, 2, true, false };
	struct goodix s;
	int deleted, failed, fail = 0;

	sim_reset();
	store("GBOS-bbbbbbbb-u10-00000011", 1);
	store("GBOS-bbbbbbbb-u10-00000022", 2);
	store("GBOS-cccccccc-u10-00000033", 3);		/* another install, or a pre-GBOS-L live boot */
	store("GBOS-L-dddddddd-u10-00000005", 4);
	open_dev(&s);
	assert(fp_reconcile(&s, &p, &deleted, &failed, &fail) == GOODIX_OK);
	assert(deleted == 1 && sim.n == 3 && find("GBOS-L-dddddddd-u10-00000005") < 0);

	/* the same sensor seen as the install's own /data: the other instance's template goes */
	p.installed = true;
	assert(fp_reconcile(&s, &p, &deleted, &failed, &fail) == GOODIX_OK);
	assert(deleted == 1 && sim.n == 2 && find("GBOS-cccccccc-u10-00000033") < 0);
	assert(logged("not this install's"));
	p.installed = false;

	/* state unreadable: this system's templates all stay, even ones Android may not know */
	store("GBOS-bbbbbbbb-u10-00000044", 5);
	p.ids_trusted = false;
	assert(fp_reconcile(&s, &p, &deleted, &failed, &fail) == GOODIX_OK);
	assert(deleted == 0 && sim.n == 3);
	goodix_close(&s);
}

/* the first enrolment on the install meets the stale template of a pre-install live boot */
static void test_install_stale_template(void)
{
	struct fp_policy p = { "bbbbbbbb", false, false, 10, NULL, 0, true, true };
	struct goodix_template dup;
	struct goodix s;
	int fail, restarted = 0;

	sim_reset();
	sim.dup_status = 2;
	store("GBOS-1c2ac773-u10-5cfb789b", 1);
	sim.finger = 1;
	open_dev(&s);
	assert(fp_enroll(&s, &p, "GBOS-bbbbbbbb-u10-00000042", NULL, &fail, &dup, &restarted) == GOODIX_OK);
	assert(restarted == 1 && sim.n == 1 && find("GBOS-bbbbbbbb-u10-00000042") >= 0);

	goodix_close(&s);

	/* without --installed the same template blocks that finger */
	sim_reset();
	sim.dup_status = 2;
	store("GBOS-1c2ac773-u10-5cfb789b", 1);
	sim.finger = 1;
	p.installed = false;
	open_dev(&s);
	assert(fp_enroll(&s, &p, "GBOS-bbbbbbbb-u10-00000042", NULL, &fail, &dup, &restarted) == GOODIX_DUPLICATE);
	assert(sim.n == 1 && logged("kept, so not enrolled here"));
	goodix_close(&s);
}

static int enroll(struct goodix *s, const char *uid, int *fail, struct goodix_template *dup)
{
	*fail = 0;
	return goodix_enroll(s, uid, NULL, fail, dup);
}

/* boot-10: 12 stages complete, then check-duplicate says 2 because yesterday's template is still stored */
static void test_boot10_stale_template(void)
{
	struct fp_policy p = { "aaaaaaaa", true, true, 10, NULL, 0, true, false };
	struct goodix_template dup;
	struct goodix s;
	int fail, deleted, failed;

	sim_reset();
	sim.dup_status = 2;
	store("GBOS-cccccccc-u10-2ab7f1c0", 1);
	sim.finger = 1;
	open_dev(&s);

	/* the old HAL's behaviour: fails, every time */
	assert(enroll(&s, "GBOS-L-aaaaaaaa-u10-00000042", &fail, &dup) == GOODIX_DUPLICATE);
	assert(fail == 2 && strcmp(dup.uid, "GBOS-cccccccc-u10-2ab7f1c0") == 0);
	assert(!sim.shield && sim.n == 1);
	assert(logged("check-duplicate: status 0x02, finger matches stored template GBOS-cccccccc-u10-2ab7f1c0"));

	/* if the cleanup could not run before: the duplicate is deleted and the next attempt works */
	assert(fp_on_duplicate(&s, &p, &dup, fail));
	assert(sim.n == 0 && logged("deleted it (no longer listed)"));
	assert(enroll(&s, "GBOS-L-aaaaaaaa-u10-00000042", &fail, &dup) == GOODIX_OK);
	assert(find("GBOS-L-aaaaaaaa-u10-00000042") >= 0 && !sim.shield);

	/* the normal path: the session's cleanup removes it first, so the first attempt works */
	sim_reset();
	sim.dup_status = 2;
	store("GBOS-cccccccc-u10-2ab7f1c0", 1);
	sim.finger = 1;
	open_dev(&s);
	assert(fp_reconcile(&s, &p, &deleted, &failed, &fail) == GOODIX_OK && deleted == 1);
	assert(enroll(&s, "GBOS-L-aaaaaaaa-u10-00000042", &fail, &dup) == GOODIX_OK);
	assert(sim.n == 1);
	goodix_close(&s);
}

static void test_duplicate_kept(void)
{
	static const int32_t ids[] = { 0x11 };
	struct fp_policy p = { "aaaaaaaa", true, false, 10, ids, 1, true, false };
	struct goodix_template dup;
	struct goodix s;
	int fail;

	/* the finger is Windows Hello's: never deleted */
	sim_reset();
	sim.dup_status = 2;
	store("FP1-20261117-7-3F2A9B11-user", 1);
	sim.finger = 1;
	open_dev(&s);
	assert(enroll(&s, "GBOS-L-aaaaaaaa-u10-00000042", &fail, &dup) == GOODIX_DUPLICATE);
	assert(!fp_on_duplicate(&s, &p, &dup, fail) && sim.n == 1);
	assert(logged("not created by gbOS86"));

	/* the same finger enrolled twice for this user */
	sim.db[0].finger = 0;
	store("GBOS-L-aaaaaaaa-u10-00000011", 1);
	assert(enroll(&s, "GBOS-L-aaaaaaaa-u10-00000042", &fail, &dup) == GOODIX_DUPLICATE);
	assert(!fp_on_duplicate(&s, &p, &dup, fail) && sim.n == 2);
	assert(logged("already enrolled for this user"));

	/* an old-style template while an install exists */
	sim.db[1].finger = 0;
	store("GBOS-cccccccc-u10-00000033", 1);
	assert(enroll(&s, "GBOS-L-aaaaaaaa-u10-00000042", &fail, &dup) == GOODIX_DUPLICATE);
	assert(!fp_on_duplicate(&s, &p, &dup, fail) && sim.n == 3);

	/* a duplicate without a readable record */
	memset(&dup, 0, sizeof(dup));
	assert(!fp_on_duplicate(&s, &p, &dup, 2) && sim.n == 3);
	goodix_close(&s);
}

static void test_commit_refused(void)
{
	struct goodix_template dup;
	struct goodix s;
	int fail;

	sim_reset();
	sim.commit_status = 0x83;
	open_dev(&s);
	assert(enroll(&s, "GBOS-L-aaaaaaaa-u10-00000042", &fail, &dup) == GOODIX_FAILED);
	assert(fail == 0x83 && !sim.shield && logged("commit: status 0x83"));
	goodix_close(&s);
}

/*
 * boot-10 16:37:18 and 16:37:29: Settings cancels while the HAL waits for b0:02's acknowledgement. The
 * acknowledgement and response arrive later and, before, were read as the next open's VERSION reply
 * (HW_UNAVAILABLE). Now: the settle reads past them and switches the shield off, and the next open works.
 */
static void test_cancel_in_ack_wait(void)
{
	struct goodix_template dup;
	struct goodix s;
	int fail;

	sim_reset();
	sim.cancel_cmd = GOODIX_CMD_FINGER_MODE;
	sim.cancel_at = 0;
	open_dev(&s);
	assert(enroll(&s, "GBOS-L-aaaaaaaa-u10-00000042", &fail, &dup) == -ECANCELED);
	assert(sim.shield && sim.laten == 2);
	assert(goodix_settle(&s) == 0);
	assert(!sim.shield && sim.stale_sent == 2 && sim.qn == 0 && !goodix_cancel_requested(&s));
	assert(logged("discarding stale packet"));
	goodix_close(&s);
	open_dev(&s);
	assert(enroll(&s, "GBOS-L-aaaaaaaa-u10-00000042", &fail, &dup) == GOODIX_OK);
	goodix_close(&s);

	/* without the settle, the next open still reads past the late packets */
	sim_reset();
	sim.cancel_cmd = GOODIX_CMD_FINGER_MODE;
	open_dev(&s);
	assert(enroll(&s, "GBOS-L-aaaaaaaa-u10-00000042", &fail, &dup) == -ECANCELED);
	goodix_close(&s);
	open_dev(&s);
	assert(sim.stale_sent == 2 && sim.qn == 0);
	goodix_close(&s);
}

/* 16:37:42, 16:38:10: cancelled in the response wait */
static void test_cancel_in_response_wait(void)
{
	struct goodix_template dup;
	struct goodix s;
	int fail;

	sim_reset();
	sim.cancel_cmd = GOODIX_CMD_FINGER_MODE;
	sim.cancel_at = 1;
	open_dev(&s);
	assert(enroll(&s, "GBOS-L-aaaaaaaa-u10-00000042", &fail, &dup) == -ECANCELED);
	assert(sim.shield && sim.laten == 1);
	assert(goodix_settle(&s) == 0 && !sim.shield && sim.stale_sent == 1);
	goodix_close(&s);

	/* cancelled while waiting for a finger on the sensor (capture) */
	sim_reset();
	sim.cancel_cmd = GOODIX_CMD_CAPTURE;
	sim.cancel_at = 1;
	open_dev(&s);
	assert(enroll(&s, "GBOS-L-aaaaaaaa-u10-00000042", &fail, &dup) == -ECANCELED);
	assert(goodix_settle(&s) == 0 && !sim.shield);
	goodix_close(&s);
}

/*
 * boot-11: the delete was answered, the template stayed, and the HAL said "deleted it". A delete now counts
 * only once the template is gone from the list, and the record goes out exactly as the sensor listed it.
 */
static void test_delete_verified(void)
{
	static const int32_t none[1] = { 0 };
	struct fp_policy p = { "aaaaaaaa", true, true, 10, none, 0, true, false };
	struct goodix_template listed[GOODIX_MAX_TEMPLATES];
	struct goodix s;
	size_t count;
	uint8_t fid[GOODIX_MAX_UID + 72];
	size_t fid_len;
	int fail, deleted, failed;

	/* firmware that matches tid and uid */
	sim_reset();
	sim.delete_mode = DELETE_TID_UID;
	store("GBOS-cccccccc-u10-2ab7f1c0", 1);
	open_dev(&s);
	/* libfprint's short payload loses the uid's last byte, so this firmware answers 0x9c; the listed record works */
	assert(goodix_delete(&s, "GBOS-cccccccc-u10-2ab7f1c0", &fail) == GOODIX_OK && sim.n == 0 && sim.deletes == 2);
	assert(logged("(a7:00, libfprint record, 96 bytes, type 01, finger 01): status 0x9c"));
	assert(logged("(a7:00, listed record, 98 bytes, type 01, finger 01): status 0x00"));
	assert(sim.deletes_04 == 0);
	assert(goodix_delete(&s, "GBOS-cccccccc-u10-2ab7f1c0", &fail) == GOODIX_NORECORD);
	goodix_close(&s);

	/* firmware that matches the tid: libfprint's payload, the first one sent, deletes it */
	sim_reset();
	sim.delete_mode = DELETE_TID;
	store("GBOS-cccccccc-u10-2ab7f1c0", 1);
	store("GBOS-dddddddd-u10-00000001", 2);
	open_dev(&s);
	assert(goodix_delete(&s, "GBOS-cccccccc-u10-2ab7f1c0", &fail) == GOODIX_OK && sim.deletes == 1);
	assert(sim.n == 1 && find("GBOS-dddddddd-u10-00000001") == 0);
	goodix_close(&s);

	/* boot-12: a7:04 is acknowledged with 0x00 and the template stays, on every firmware model */
	sim_reset();
	store("GBOS-1c2ac773-u10-5cfb789b", 1);
	open_dev(&s);
	{
		uint8_t old[GOODIX_MAX_UID + 72];
		size_t old_len;
		struct goodix_response response;

		assert(goodix_list_templates(&s, listed, &count, &fail) == GOODIX_OK && count == 1);
		assert(goodix_encode_finger_id(old, sizeof(old), &old_len, listed[0].tid, listed[0].uid) == 0);
		assert(goodix_command(&s, GOODIX_CMD_DELETE, 0x04, old, old_len, false, &response) == 0);
		assert(response.result == GOODIX_SUCCESS && sim.n == 1 && sim.deletes_04 == 1);
	}
	assert(goodix_delete(&s, "GBOS-1c2ac773-u10-5cfb789b", &fail) == GOODIX_OK && sim.n == 0);
	assert(sim.deletes_04 == 1);
	goodix_close(&s);

	/* firmware that keeps its own bytes in the record and deletes only on an exact copy: the listed
	 * record works, the record re-encoded from tid and uid (what the boot-11 HAL sent) does not */
	sim_reset();
	sim.account_id = true;
	sim.delete_mode = DELETE_EXACT;
	store("GBOS-cccccccc-u10-2ab7f1c0", 1);
	open_dev(&s);
	assert(goodix_list_templates(&s, listed, &count, &fail) == GOODIX_OK && count == 1);
	assert(goodix_encode_finger_id(fid, sizeof(fid), &fid_len, listed[0].tid, listed[0].uid) == 0);
	assert(goodix_delete_payload(&s, fid, fid_len) == GOODIX_NO_RECORD && sim.n == 1);
	assert(goodix_delete(&s, "GBOS-cccccccc-u10-2ab7f1c0", &fail) == GOODIX_OK && sim.n == 0);
	goodix_close(&s);

	/* acknowledged but kept: reported as a failure, all three payloads tried */
	sim_reset();
	sim.account_id = true;
	sim.delete_mode = DELETE_IGNORED;
	store("GBOS-cccccccc-u10-2ab7f1c0", 1);
	open_dev(&s);
	assert(goodix_delete(&s, "GBOS-cccccccc-u10-2ab7f1c0", &fail) == GOODIX_FAILED);
	assert(fail == GOODIX_FAILED && sim.n == 1 && sim.deletes == 3 && sim.deletes_04 == 0);
	assert(logged("delete GBOS-cccccccc-u10-2ab7f1c0: still listed after status 0x00"));
	assert(logged("(a7:00, re-encoded record"));
	/* and the session cleanup counts it as failed, so op_enroll tries again */
	assert(fp_reconcile(&s, &p, &deleted, &failed, &fail) == GOODIX_OK && deleted == 0 && failed == 1);
	assert(logged("sensor template GBOS-cccccccc-u10-2ab7f1c0: from an earlier live boot"));
	goodix_close(&s);

	/* "no such record" for a listed template is not a deletion either */
	sim_reset();
	sim.delete_mode = DELETE_NO_RECORD;
	store("GBOS-cccccccc-u10-2ab7f1c0", 1);
	open_dev(&s);
	assert(goodix_delete(&s, "GBOS-cccccccc-u10-2ab7f1c0", &fail) == GOODIX_FAILED);
	assert(fail == GOODIX_NO_RECORD && sim.n == 1 && sim.deletes == 2);	/* libfprint, listed (= re-encoded) */
	goodix_close(&s);
}

/*
 * A firmware that took an odd a7:00 payload as "delete everything" must not get the chance to erase Windows
 * Hello's prints, and once it has erased anything else, no delete is sent again.
 */
static void test_delete_collateral(void)
{
	struct goodix s;
	int fail;

	/* only gbos_fp's templates: the listed record wipes both; detected, reported, deletes blocked */
	sim_reset();
	sim.delete_mode = DELETE_WILDCARD;
	store("GBOS-1c2ac773-u10-5cfb789b", 1);
	store("GBOS-L-956073cf-u10-13b9e5e2", 2);
	open_dev(&s);
	assert(goodix_delete(&s, "GBOS-1c2ac773-u10-5cfb789b", &fail) == GOODIX_FAILED && fail == GOODIX_FAILED);
	assert(sim.n == 0 && sim.deletes == 2 && s.delete_blocked);
	assert(logged("delete GBOS-1c2ac773-u10-5cfb789b also removed GBOS-L-956073cf-u10-13b9e5e2 from the sensor"));
	assert(logged("(listed record) removed 1 other template(s): no more deletes until gbos_fp restarts"));
	/* blocked: nothing is sent, even for a template the firmware still lists, and across close and open */
	store("GBOS-L-956073cf-u10-00000001", 3);
	goodix_close(&s);
	assert(goodix_open(&s) == 0);
	assert(goodix_delete(&s, "GBOS-L-956073cf-u10-00000001", &fail) == GOODIX_FAILED);
	assert(sim.deletes == 2 && sim.n == 1 && logged("not sent: an earlier delete removed other templates"));
	goodix_close(&s);

	/* a Windows Hello print on the sensor: libfprint's payload only, so the wildcard never fires */
	goodix_init(&s);
	sim_reset();
	sim.delete_mode = DELETE_WILDCARD;
	store("GBOS-1c2ac773-u10-5cfb789b", 1);
	store("FP1-20261117-7-3F2A9B11-user", 2);
	open_dev(&s);
	assert(goodix_delete(&s, "GBOS-1c2ac773-u10-5cfb789b", &fail) == GOODIX_FAILED);
	assert(sim.deletes == 1 && sim.n == 2 && find("FP1-20261117-7-3F2A9B11-user") >= 0 && !s.delete_blocked);
	assert(logged("holds templates not named GBOS-, so only libfprint's payload is sent"));
	goodix_close(&s);

	/* and with a working firmware, libfprint's payload removes the target and leaves Windows' print */
	sim_reset();
	store("GBOS-1c2ac773-u10-5cfb789b", 1);
	store("FP1-20261117-7-3F2A9B11-user", 2);
	open_dev(&s);
	assert(goodix_delete(&s, "GBOS-1c2ac773-u10-5cfb789b", &fail) == GOODIX_OK);
	assert(sim.deletes == 1 && sim.n == 1 && find("FP1-20261117-7-3F2A9B11-user") == 0);
	goodix_close(&s);
}

/*
 * Boot-13: the enrolled finger never matched (identify after an enroll-mode capture) and one touch was five
 * failures (no finger-up wait), so the first touch of every unlock locked the sensor out.
 */
static void test_identify_touches(void)
{
	static const char *const mine[] = { "GBOS-L-aaaaaaaa-u10-00000011", "GBOS-L-aaaaaaaa-u10-00000022" };
	char matched[GOODIX_MAX_UID + 1];
	struct goodix s;
	int fail;

	/* the enrolled finger, one touch: captured for verification, matched, logged with its score */
	sim_reset();
	store("GBOS-L-aaaaaaaa-u10-00000011", 1);
	store("GBOS-L-aaaaaaaa-u10-00000022", 3);
	sim.touches[0] = 3;
	sim.ntouches = 1;
	open_dev(&s);
	assert(goodix_identify(&s, mine, 2, NULL, matched, &fail) == GOODIX_OK);
	assert(strcmp(matched, "GBOS-L-aaaaaaaa-u10-00000022") == 0 && sim.capture_mode == 1);
	assert(logged("identify: capture quality 255, coverage 255"));
	assert(logged("identify: status 0x00, score 1200, study 0, template GBOS-L-aaaaaaaa-u10-00000022"));
	assert(!sim.shield);
	goodix_close(&s);

	/* a wrong finger, then the right one: the no-match waits for the lift, the next attempt is the new touch */
	sim_reset();
	store("GBOS-L-aaaaaaaa-u10-00000011", 1);
	sim.touches[0] = 2;
	sim.touches[1] = 1;
	sim.ntouches = 2;
	open_dev(&s);
	assert(goodix_identify(&s, mine, 2, NULL, matched, &fail) == GOODIX_NOMATCH);
	assert(sim.finger_on == 0 && logged("identify: status 0x01: no match"));
	assert(goodix_identify(&s, mine, 2, NULL, matched, &fail) == GOODIX_OK);
	assert(sim.captures == 2 && sim.identifies == 2);
	goodix_close(&s);

	/* one touch of a wrong finger is one failure: the next attempt waits for a new touch (here: cancelled) */
	sim_reset();
	store("GBOS-L-aaaaaaaa-u10-00000011", 1);
	sim.touches[0] = 2;
	sim.ntouches = 1;
	open_dev(&s);
	assert(goodix_identify(&s, mine, 2, NULL, matched, &fail) == GOODIX_NOMATCH);
	assert(goodix_identify(&s, mine, 2, NULL, matched, &fail) == -ECANCELED);
	assert(sim.captures == 1 && sim.identifies == 1);
	goodix_close(&s);

	/* an unusable image: retried after the lift, not identified, not a failure */
	sim_reset();
	store("GBOS-L-aaaaaaaa-u10-00000011", 1);
	sim.touches[0] = 1;
	sim.touches[1] = 1;
	sim.ntouches = 2;
	sim.bad_quality = 1;
	open_dev(&s);
	assert(goodix_identify(&s, mine, 2, NULL, matched, &fail) == GOODIX_RETRY && sim.identifies == 0);
	assert(sim.finger_on == 0 && logged("identify: capture quality 0, coverage 255"));
	assert(goodix_identify(&s, mine, 2, NULL, matched, &fail) == GOODIX_OK && sim.next_touch == 2);
	goodix_close(&s);

	/* another user's template matched: a no-match here, and said so */
	sim_reset();
	store("GBOS-L-aaaaaaaa-u0-00000033", 1);
	sim.touches[0] = 1;
	sim.ntouches = 1;
	open_dev(&s);
	assert(goodix_identify(&s, mine, 2, NULL, matched, &fail) == GOODIX_NOMATCH);
	assert(logged("template GBOS-L-aaaaaaaa-u0-00000033 (not one of this user's)") && sim.finger_on == 0);
	goodix_close(&s);
}

/*
 * Boot-13: keyguard cancelled an authentication while the sensor was being opened. The open's d0:00 response
 * was left unread ("sensor open failed: -125", then "discarding stale packet d0:00" at the next open). A
 * command that cannot be cancelled now finishes; the cancel ends the open at the next command.
 */
static void test_cancel_during_open(void)
{
	struct goodix s;

	sim_reset();
	goodix_init(&s);
	snprintf(s.path, sizeof(s.path), "%s", node);
	sim.dev = &s;
	sim.cancel_after_ack = GOODIX_CMD_VERSION;
	assert(goodix_open(&s) == -ECANCELED && s.fd < 0);
	assert(sim.qn == 0);	/* nothing left in the pipe */
	assert(goodix_open(&s) == 0 && !logged("discarding stale packet"));
	goodix_close(&s);
}

/* boot-11 again: after the verified delete, the enrollment starts over instead of failing */
static void test_boot11_restart(void)
{
	struct fp_policy p = { "5f0bf067", true, true, 10, NULL, 0, true, false };
	struct goodix_template dup;
	struct goodix s;
	int fail, restarted;

	sim_reset();
	sim.account_id = true;
	sim.delete_mode = DELETE_EXACT;
	sim.dup_status = 2;
	store("GBOS-1c2ac773-u10-5cfb789b", 1);
	sim.finger = 1;
	open_dev(&s);
	assert(fp_enroll(&s, &p, "GBOS-L-5f0bf067-u10-4c4a2bc2", NULL, &fail, &dup, &restarted) == GOODIX_OK);
	assert(restarted == 1 && sim.n == 1 && find("GBOS-L-5f0bf067-u10-4c4a2bc2") >= 0);
	assert(logged("enrollment restarts: touch the sensor again"));
	goodix_close(&s);

	/* the delete does not take: no restart, the duplicate is reported */
	sim_reset();
	sim.delete_mode = DELETE_IGNORED;
	sim.dup_status = 2;
	store("GBOS-1c2ac773-u10-5cfb789b", 1);
	sim.finger = 1;
	open_dev(&s);
	assert(fp_enroll(&s, &p, "GBOS-L-5f0bf067-u10-4c4a2bc2", NULL, &fail, &dup, &restarted) == GOODIX_DUPLICATE);
	assert(restarted == 0 && fail == 2 && sim.n == 1 && logged("deleting it failed"));
	goodix_close(&s);

	/* check-duplicate names a template the list does not have: nothing to delete, no restart */
	sim_reset();
	sim.ghost_dup = true;
	sim.dup_status = 2;
	open_dev(&s);
	assert(fp_enroll(&s, &p, "GBOS-L-5f0bf067-u10-4c4a2bc2", NULL, &fail, &dup, &restarted) == GOODIX_DUPLICATE);
	assert(restarted == 0 && sim.deletes == 0 && logged("template list does not contain it"));
	goodix_close(&s);

	/* two stale copies of the finger: two restarts; a third gives up */
	sim_reset();
	sim.dup_status = 2;
	store("GBOS-1c2ac773-u10-5cfb789b", 1);
	store("GBOS-L-0badcafe-u10-00000001", 1);
	sim.finger = 1;
	open_dev(&s);
	assert(fp_enroll(&s, &p, "GBOS-L-5f0bf067-u10-4c4a2bc2", NULL, &fail, &dup, &restarted) == GOODIX_OK);
	assert(restarted == 2 && sim.n == 1);
	goodix_close(&s);
	sim_reset();
	sim.dup_status = 2;
	store("GBOS-1c2ac773-u10-5cfb789b", 1);
	store("GBOS-L-0badcafe-u10-00000001", 1);
	store("GBOS-L-0badcafe-u10-00000002", 1);
	sim.finger = 1;
	open_dev(&s);
	assert(fp_enroll(&s, &p, "GBOS-L-5f0bf067-u10-4c4a2bc2", NULL, &fail, &dup, &restarted) == GOODIX_DUPLICATE);
	assert(restarted == 2 && sim.n == 0 && logged("giving up"));	/* the third is deleted too: the next attempt works */
	goodix_close(&s);
}

/* More stale packets than GOODIX_MAX_STALE is a protocol error, not an endless read. */
static void test_stale_limit(void)
{
	uint8_t junk[2] = { GOODIX_SUCCESS, GOODIX_CMD_CAPTURE };
	struct goodix_response response;
	struct goodix s;
	const uint8_t zero = 0;

	sim_reset();
	open_dev(&s);
	for (int i = 0; i <= GOODIX_MAX_STALE; i++)
		emit(sim.late, sim.latelen, &sim.laten, GOODIX_CMD_RESPONSE, 0, junk, 2);
	assert(goodix_command(&s, GOODIX_CMD_LIST, 0, &zero, 1, false, &response) == -EPROTO);
	assert(logged("no acknowledgement among 5 packets"));
	goodix_close(&s);
}

int main(void)
{
	make_node();
	test_names();
	test_policy();
	test_reconcile_live();
	test_reconcile_install();
	test_install_stale_template();
	test_boot10_stale_template();
	test_duplicate_kept();
	test_commit_refused();
	test_cancel_in_ack_wait();
	test_cancel_in_response_wait();
	test_stale_limit();
	test_delete_verified();
	test_boot11_restart();
	test_delete_collateral();
	test_identify_touches();
	test_cancel_during_open();
	unlink(node);
	puts("GOODIX_SIM_TEST_PASS");
	return 0;
}
