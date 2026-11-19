// SPDX-License-Identifier: Apache-2.0
/*
 * Which templates on the Goodix sensor gbos_fp may delete.
 *
 * The sensor keeps one template database for every OS on the laptop and its enroll step rejects a finger
 * that matches any stored template (the check-duplicate command). gbos_fp names its templates after the
 * /data it was created for, so it can tell which ones no Android can reach any more:
 *
 *   GBOS-<instance>-u<user>-<id>     persistent /data (an install, or a live boot before 2026-11-18)
 *   GBOS-L-<instance>-u<user>-<id>   tmpfs /data (live boot): unreachable once that boot has ended
 *
 * <instance> is 8 hex digits, random per /data (DATA_DIR/instance), <id> the AIDL enrollment id in hex.
 * A name stays at most 32 bytes, the uid length libfprint's encode_finger_id() stores.
 *
 * Deleted: this /data's templates of the session's user that the HAL does not report to Android (the
 * framework only knows the ids enumerateEnrollments returned, so nothing it holds can point at them);
 * GBOS-L templates of another instance; old-style templates of another instance in a live boot on a
 * machine with no gbOS86 install (nothing left that could reference them); and the same in the installed
 * system itself: its /data is the only persistent one on the machine (stage0 mounts ext4 /data only when
 * booting the NVMe install), so another persistent instance's /data was replaced (reinstall, wipe) or was
 * a live boot before 2026-11-18. Without that rule a stale live-boot template blocks its finger on the
 * install for good, since live boots keep old-style names once an install exists.
 * Kept: everything not named by gbos_fp (Windows Hello, fprintd, ...), other users' templates, any
 * old-style template of another instance in a live boot while an install exists (the install may use
 * it), and in the install any template whose user and id Android still lists, whatever its instance.
 *
 * Include goodix.h first; define FP_LOGI/FP_LOGW for log output.
 */
#ifndef GBOS_FP_POLICY_H
#define GBOS_FP_POLICY_H

#ifndef FP_LOGI
#define FP_LOGI(...) ((void)0)
#define FP_LOGW(...) ((void)0)
#endif

#define FP_NAME_MAX 32		/* libfprint stores at most TEMPLATE_ID_SIZE bytes of the uid */

struct fp_name {
	bool live;		/* GBOS-L- */
	char instance[9];
	int32_t user;
	uint32_t id;
};

struct fp_policy {
	const char *instance;	/* this /data's instance */
	bool live;		/* this /data is tmpfs */
	bool no_install;	/* hook.sh found no gbOS86 install (LABEL=GBOSSYS) on this machine */
	int32_t user;		/* the session's Android user */
	const int32_t *ids;	/* that user's enrollments, as reported to Android */
	int n;
	bool ids_trusted;	/* the user's state file was read, or does not exist (false on a read error) */
	bool installed;		/* booted from the NVMe install (hook.sh --installed), /data confirmed persistent
				 * by statfs, and this /data's instance file read or newly created */
};

enum fp_verdict {
	FP_KEEP_NOT_GBOS,	/* not a gbos_fp name: another OS's template */
	FP_KEEP_ENROLLED,	/* this /data, this user, an enrollment Android knows */
	FP_KEEP_OTHER_USER,	/* this /data, another Android user */
	FP_KEEP_UNTRUSTED,	/* this user's state could not be read: never guess */
	FP_KEEP_OTHER_SYSTEM,	/* another persistent /data: an installed gbOS86 may use it */
	FP_DELETE_UNKNOWN,	/* this /data, this user, not an enrollment Android knows */
	FP_DELETE_OLD_LIVE,	/* a previous live boot's (GBOS-L) */
	FP_DELETE_ORPHAN,	/* old-style, another /data, live boot and no install on this machine */
	FP_DELETE_REPLACED,	/* old-style, another /data, seen from the install's own persistent /data */
};

static inline bool fp_verdict_deletes(enum fp_verdict v) { return v >= FP_DELETE_UNKNOWN; }

static inline const char *fp_verdict_text(enum fp_verdict v)
{
	switch (v) {
	case FP_KEEP_NOT_GBOS: return "not created by gbOS86 (another OS, e.g. Windows Hello)";
	case FP_KEEP_ENROLLED: return "enrolled for this user";
	case FP_KEEP_OTHER_USER: return "enrolled for another Android user";
	case FP_KEEP_UNTRUSTED: return "this user's fingerprint state could not be read";
	case FP_KEEP_OTHER_SYSTEM: return "from another gbOS86 system (an install may still use it)";
	case FP_DELETE_UNKNOWN: return "this system's, but not an enrollment Android knows";
	case FP_DELETE_OLD_LIVE: return "from an earlier live boot";
	case FP_DELETE_ORPHAN: return "from an earlier live boot (old name; no gbOS86 install on this machine)";
	case FP_DELETE_REPLACED: return "from an earlier /data (a live boot or a replaced install), not this install's";
	}
	return "?";
}

static inline bool fp_hex8(const char *s, uint32_t *out)
{
	uint32_t v = 0;

	for (int i = 0; i < 8; i++) {
		char c = s[i];

		if (c >= '0' && c <= '9')
			v = v << 4 | (uint32_t)(c - '0');
		else if (c >= 'a' && c <= 'f')
			v = v << 4 | (uint32_t)(c - 'a' + 10);
		else
			return false;
	}
	if (out)
		*out = v;
	return true;
}

/* Length of the name, or -1 if it would not fit FP_NAME_MAX. */
static inline int fp_name_format(char out[FP_NAME_MAX + 1], bool live, const char *instance,
				 int32_t user, uint32_t id)
{
	int n = snprintf(out, FP_NAME_MAX + 1, "GBOS-%s%.8s-u%d-%08x", live ? "L-" : "", instance,
			 (int)user, (unsigned int)id);

	return n > 0 && n <= FP_NAME_MAX && user >= 0 ? n : -1;
}

/* Exactly the names fp_name_format makes; anything else is not ours. */
static inline bool fp_name_parse(const char *s, struct fp_name *out)
{
	int64_t user = 0;
	int digits = 0;

	if (strncmp(s, "GBOS-", 5) != 0)	/* stops at a short name's end */
		return false;
	s += 5;
	out->live = s[0] == 'L' && s[1] == '-';
	if (out->live)
		s += 2;
	if (!fp_hex8(s, NULL) || s[8] != '-' || s[9] != 'u')
		return false;
	memcpy(out->instance, s, 8);
	out->instance[8] = 0;
	s += 10;
	if (s[0] == '0' && s[1] != '-')
		return false;	/* no leading zeros: one name per user */
	while (*s >= '0' && *s <= '9' && digits < 9) {
		user = user * 10 + (*s++ - '0');
		digits++;
	}
	if (!digits || *s != '-' || !fp_hex8(s + 1, &out->id) || s[9] != 0)
		return false;
	out->user = (int32_t)user;
	return true;
}

static inline enum fp_verdict fp_classify(const struct fp_policy *p, const char *uid)
{
	struct fp_name name;

	if (!uid[0] || !fp_name_parse(uid, &name))
		return FP_KEEP_NOT_GBOS;
	if (name.live == p->live && strcmp(name.instance, p->instance) == 0) {
		if (name.user != p->user)
			return FP_KEEP_OTHER_USER;
		for (int i = 0; i < p->n; i++)
			if ((uint32_t)p->ids[i] == name.id)
				return FP_KEEP_ENROLLED;
		return p->ids_trusted ? FP_DELETE_UNKNOWN : FP_KEEP_UNTRUSTED;
	}
	if (name.live)
		return FP_DELETE_OLD_LIVE;
	if (p->live && p->no_install)
		return FP_DELETE_ORPHAN;
	if (p->installed && !p->live) {
		/* an instance file lost and recreated must not cost the user a finger Android still shows */
		if (name.user == p->user) {
			if (!p->ids_trusted)
				return FP_KEEP_UNTRUSTED;
			for (int i = 0; i < p->n; i++)
				if ((uint32_t)p->ids[i] == name.id)
					return FP_KEEP_ENROLLED;
		}
		return FP_DELETE_REPLACED;
	}
	return FP_KEEP_OTHER_SYSTEM;
}

/*
 * List the sensor's templates and delete the ones the policy allows. The sensor must be open. Returns
 * GOODIX_OK, GOODIX_FAILED (with *fail) or a negative errno when the list cannot be read or the sensor
 * stops answering. A template the sensor refuses to delete is logged and counted in *failed; *deleted
 * counts the ones removed.
 */
static inline int fp_reconcile(struct goodix *s, const struct fp_policy *p, int *deleted, int *failed,
			       int *fail)
{
	struct goodix_template templates[GOODIX_MAX_TEMPLATES];
	size_t count = 0;
	int r, kept = 0;

	*deleted = *failed = 0;
	if ((r = goodix_list_templates(s, templates, &count, fail)) != GOODIX_OK)
		return r;
	for (size_t i = 0; i < count; i++) {
		enum fp_verdict v = fp_classify(p, templates[i].uid);

		FP_LOGI("sensor template %s: %s", templates[i].uid[0] ? templates[i].uid : "(unnamed)",
			fp_verdict_text(v));
		if (!fp_verdict_deletes(v)) {
			kept++;
			continue;
		}
		r = goodix_delete_template(s, &templates[i], fail);
		if (r == GOODIX_OK) {
			FP_LOGI("deleted sensor template %s (no longer listed)", templates[i].uid);
			(*deleted)++;
		} else {
			FP_LOGW("deleting sensor template %s (%s) failed: %d (sensor code 0x%02x)",
				templates[i].uid, fp_verdict_text(v), r, (unsigned int)*fail);
			(*failed)++;
			if (r < 0)
				return r;	/* transport error: the sensor needs reopening */
		}
	}
	FP_LOGI("sensor templates for user %d: %u stored, %d deleted, %d kept, %d enrollments known",
		(int)p->user, (unsigned int)count, *deleted, kept, p->n);
	return GOODIX_OK;
}

/*
 * goodix_enroll returned GOODIX_DUPLICATE: the finger matches stored template *dup (empty uid when the
 * sensor did not name it). Delete it when the policy allows, so the next attempt succeeds; otherwise log
 * who owns it. Android's AIDL v4 Error has no "already enrolled", so the caller reports UNABLE_TO_PROCESS
 * and this log line is the only place the reason appears. Returns true when the template was deleted.
 */
static inline bool fp_on_duplicate(struct goodix *s, const struct fp_policy *p,
				   const struct goodix_template *dup, int code)
{
	enum fp_verdict v;
	int r, fail = 0;

	if (!dup->uid_len) {
		FP_LOGW("user %d: this finger is already stored on the sensor (sensor code %d), which did not "
			"say under which name: not enrolled; use another finger", (int)p->user, code);
		return false;
	}
	v = fp_classify(p, dup->uid);
	if (fp_verdict_deletes(v)) {
		r = goodix_delete(s, dup->uid, &fail);
		if (r == GOODIX_OK) {
			FP_LOGW("user %d: this finger was already stored on the sensor as %s (%s): deleted it "
				"(no longer listed)", (int)p->user, dup->uid, fp_verdict_text(v));
			return true;
		}
		if (r == GOODIX_NORECORD)
			FP_LOGW("user %d: check-duplicate matched %s (%s), but the sensor's template list does not "
				"contain it: cannot delete it", (int)p->user, dup->uid, fp_verdict_text(v));
		else
			FP_LOGW("user %d: this finger is already stored on the sensor as %s (%s); deleting it "
				"failed: %d (sensor code 0x%02x)", (int)p->user, dup->uid, fp_verdict_text(v), r,
				(unsigned int)fail);
		return false;
	}
	if (v == FP_KEEP_ENROLLED)
		FP_LOGW("user %d: this finger is already enrolled for this user (%s): not enrolled twice",
			(int)p->user, dup->uid);
	else
		FP_LOGW("user %d: this finger is already enrolled on the sensor as %s (%s): kept, so not "
			"enrolled here; remove it there or use another finger", (int)p->user, dup->uid,
			fp_verdict_text(v));
	return false;
}

#define FP_ENROLL_RESTARTS 2

/*
 * goodix_enroll, and when the finger matches a stored template the policy lets this HAL delete: delete it
 * (verified) and start the enrollment over, at most FP_ENROLL_RESTARTS times, so the user is not sent back
 * to Settings. Returns goodix_enroll's result; a GOODIX_DUPLICATE that comes back is one that stays.
 * *restarted counts the restarts (progress starts again from the first stage each time).
 */
static inline int fp_enroll(struct goodix *s, const struct fp_policy *p, const char *uid,
			    const struct goodix_ev *ev, int *fail, struct goodix_template *dup, int *restarted)
{
	int r;

	for (*restarted = 0;; (*restarted)++) {
		*fail = 0;
		r = goodix_enroll(s, uid, ev, fail, dup);
		if (r != GOODIX_DUPLICATE)
			return r;
		if (!fp_on_duplicate(s, p, dup, *fail))
			return r;
		if (*restarted == FP_ENROLL_RESTARTS) {
			FP_LOGW("user %d: still matching a stored template after %d restarts: giving up",
				(int)p->user, *restarted);
			return r;
		}
		FP_LOGI("user %d: enrollment restarts: touch the sensor again", (int)p->user);
	}
}

#endif
