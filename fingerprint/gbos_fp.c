// SPDX-License-Identifier: Apache-2.0
/*
 * gbos_fp: android.hardware.biometrics.fingerprint.IFingerprint/default (AIDL V4) for the
 * Latitude 7320 Detachable's Goodix 27c6:6384 match-on-chip USB sensor.
 * Experimental research source only; neither its ABI nor authentication behavior has been validated.
 *
 * Google's crosfp HAL only drives Google's own fingerprint MCU, so a laptop with a Goodix sensor has
 * no fingerprint at all. The sensor stores and matches templates itself (goodix.h, a port of libfprint's
 * driver); this HAL names them, keeps each user's list in /data/vendor/crosfp/gbos_fp/ and speaks AIDL.
 *
 * Security model:
 *  - Declared SensorStrength::WEAK (Class 2): it unlocks the lock screen and answers BIOMETRIC_WEAK
 *    prompts, but never releases auth-bound Keystore keys. The match is made on a USB device the host
 *    cannot authenticate, and the only HMAC key here is soft_gk's software one, so STRONG would be a lie.
 *    As the AIDL contract requires for WEAK, authenticate returns an empty HardwareAuthToken and
 *    getAuthenticatorId returns 0.
 *  - Enrolling needs a fresh password HardwareAuthToken from soft_gk carrying a challenge this HAL handed
 *    out (HMAC with soft_gk's key, PASSWORD type, timestamp within 10 minutes); clearing a lockout needs a
 *    fresh valid one. Only system_server (uid 1000) may call in, whatever SELinux mode the system runs in.
 *  - Lockout: 30 s after every 5 consecutive rejects, permanent (password needed) after 20, persisted.
 *  - Templates are named "GBOS-<install>-u<user>-<id>" ("GBOS-L-..." when /data is tmpfs). Goodix
 *    identifies from its full device database, so this HAL accepts only a returned name belonging to the
 *    current Android user. When a session opens it deletes the sensor templates no Android can reach
 *    any more (gbos_fp_policy.h): the sensor's check-duplicate otherwise rejects that finger forever.
 *    It never touches templates of other OSes, other users, or of a gbOS86 install that may still use
 *    them.
 *
 * Built like soft_gk: no NDK, the bionic / libbinder_ndk / BoringSSL / liblog symbols it uses are declared
 * below and it links against the image's own libraries (build.sh). Wire format: AOSP's frozen
 * android.hardware.biometrics.fingerprint V4 (IFingerprint codes 1-2, ISession 1-22, ISessionCallback
 * 1-16), android.hardware.biometrics.common V4 (ICancellationSignal: 1 cancel) and keymaster V4
 * (HardwareAuthToken), plus 0xffffff getInterfaceVersion and 0xfffffe getInterfaceHash.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdarg.h>

/* ---- bionic ---- */
typedef struct { void (**preinit_array)(void); void (**init_array)(void); void (**fini_array)(void);
		 size_t preinit_array_count, init_array_count, fini_array_count; } structors_array_t;
void __libc_init(void *raw_args, void (*onexit)(void), int (*main)(int, char **, char **),
		 const structors_array_t *structors) __attribute__((noreturn));
void *malloc(size_t); void *calloc(size_t, size_t); void free(void *);
void *memcpy(void *, const void *, size_t); void *memset(void *, int, size_t);
int memcmp(const void *, const void *, size_t); size_t strlen(const char *);
int strcmp(const char *, const char *); int strncmp(const char *, const char *, size_t);
int snprintf(char *, size_t, const char *, ...) __attribute__((format(printf, 3, 4)));
int vsnprintf(char *, size_t, const char *, va_list) __attribute__((format(printf, 3, 0)));
struct tm { int tm_sec, tm_min, tm_hour, tm_mday, tm_mon, tm_year, tm_wday, tm_yday, tm_isdst;
	    long tm_gmtoff; const char *tm_zone; };	/* bionic LP64 */
struct tm *localtime_r(const long *, struct tm *);
int getpid(void); int gettid(void);
void arc4random_buf(void *, size_t);
struct timespec { long tv_sec, tv_nsec; };
int clock_gettime(int, struct timespec *);
#define CLOCK_REALTIME 0
#define CLOCK_MONOTONIC 1
#define CLOCK_BOOTTIME 7
int open(const char *, int, ...); int close(int);
long read(int, void *, size_t); long write(int, const void *, size_t);
int ioctl(int, int, ...); int fsync(int); int rename(const char *, const char *);
int mkdir(const char *, unsigned int); int usleep(unsigned int);
struct statfs_buf { uint64_t f_type, rest[31]; };	/* bionic LP64 struct statfs starts with f_type; 120 bytes */
int statfs(const char *, struct statfs_buf *);
#define TMPFS_MAGIC 0x01021994
#define RAMFS_MAGIC 0x858458f6
struct pollfd { int fd; short events, revents; };
int poll(struct pollfd *, unsigned int, int);
#define POLLOUT 4
int *__errno(void);
#define errno (*__errno())
#define O_RDONLY 0
#define O_WRONLY 1
#define O_RDWR 2
#define O_CREAT 0100
#define O_TRUNC 01000
#define O_APPEND 02000
#define O_NOFOLLOW 0400000
#define O_CLOEXEC 02000000
#define ENOENT 2
#define EINTR 4
#define EAGAIN 11
#define EINVAL 22
#define ENODEV 19
#define EIO 5
#define EEXIST 17
#define EPROTO 71
#define ETIMEDOUT 110
#define ECANCELED 125
typedef long pthread_t;
typedef struct { int32_t __private[10]; } pthread_mutex_t;	/* bionic LP64 layouts; all-zero = default init */
typedef struct { int32_t __private[12]; } pthread_cond_t;
int pthread_create(pthread_t *, const void *attr, void *(*)(void *), void *);
int pthread_mutex_lock(pthread_mutex_t *); int pthread_mutex_unlock(pthread_mutex_t *);
int pthread_cond_signal(pthread_cond_t *);
int pthread_cond_wait(pthread_cond_t *, pthread_mutex_t *);
int pthread_cond_clockwait(pthread_cond_t *, pthread_mutex_t *, int clock, const struct timespec *);

/* ---- liblog, and our own copy of each line ---- */
int __android_log_write(int prio, const char *tag, const char *text);
int __android_log_print(int prio, const char *tag, const char *fmt, ...) __attribute__((format(printf, 3, 4)));

#include "fp_log.h"
#define LOGI(...) fp_log(4, __VA_ARGS__)
#define LOGW(...) fp_log(5, __VA_ARGS__)
#define LOGE(...) fp_log(6, __VA_ARGS__)
#define LOGD(...) fp_log(3, __VA_ARGS__)

/* ---- BoringSSL ---- */
typedef struct env_md_st EVP_MD;
const EVP_MD *EVP_sha256(void);
uint8_t *HMAC(const EVP_MD *, const void *key, size_t key_len, const uint8_t *data, size_t data_len,
	      uint8_t *out, unsigned int *out_len);
int CRYPTO_memcmp(const void *, const void *, size_t);

/* ---- libbinder_ndk ---- */
typedef struct AIBinder AIBinder; typedef struct AIBinder_Class AIBinder_Class;
typedef struct AParcel AParcel; typedef struct AStatus AStatus;
typedef int32_t binder_status_t; typedef uint32_t transaction_code_t; typedef uint32_t binder_flags_t;
AIBinder_Class *AIBinder_Class_define(const char *descriptor, void *(*on_create)(void *),
	void (*on_destroy)(void *),
	binder_status_t (*on_transact)(AIBinder *, transaction_code_t, const AParcel *, AParcel *));
AIBinder *AIBinder_new(const AIBinder_Class *, void *args);
void *AIBinder_getUserData(AIBinder *);
bool AIBinder_associateClass(AIBinder *, const AIBinder_Class *);
void AIBinder_incStrong(AIBinder *); void AIBinder_decStrong(AIBinder *);
void AIBinder_markVintfStability(AIBinder *);
uint32_t AIBinder_getCallingUid(void);
binder_status_t AIBinder_prepareTransaction(AIBinder *, AParcel **in);
binder_status_t AIBinder_transact(AIBinder *, transaction_code_t, AParcel **in, AParcel **out, binder_flags_t);
void AParcel_delete(AParcel *);
int32_t AServiceManager_addService(AIBinder *, const char *instance);
bool ABinderProcess_setThreadPoolMaxThreadCount(uint32_t);
void ABinderProcess_joinThreadPool(void);
binder_status_t AParcel_readInt32(const AParcel *, int32_t *);
binder_status_t AParcel_readInt64(const AParcel *, int64_t *);
binder_status_t AParcel_readStrongBinder(const AParcel *, AIBinder **);
binder_status_t AParcel_readByteArray(const AParcel *, void *data,
	bool (*alloc)(void *data, int32_t len, int8_t **out));
binder_status_t AParcel_readInt32Array(const AParcel *, void *data,
	bool (*alloc)(void *data, int32_t len, int32_t **out));
binder_status_t AParcel_writeInt32(AParcel *, int32_t);
binder_status_t AParcel_writeInt64(AParcel *, int64_t);
binder_status_t AParcel_writeByte(AParcel *, int8_t);
binder_status_t AParcel_writeBool(AParcel *, bool);
binder_status_t AParcel_writeByteArray(AParcel *, const int8_t *, int32_t len);
binder_status_t AParcel_writeInt32Array(AParcel *, const int32_t *, int32_t len);
binder_status_t AParcel_writeString(AParcel *, const char *, int32_t len);
binder_status_t AParcel_writeStrongBinder(AParcel *, AIBinder *);
int32_t AParcel_getDataPosition(const AParcel *);
binder_status_t AParcel_setDataPosition(const AParcel *, int32_t);
binder_status_t AParcel_writeStatusHeader(AParcel *, const AStatus *);
AStatus *AStatus_newOk(void); void AStatus_delete(AStatus *);
#define STATUS_OK 0
#define STATUS_BAD_VALUE (-22)
#define STATUS_PERMISSION_DENIED (-1)
#define STATUS_UNKNOWN_TRANSACTION (-74)

/* ---- Goodix sensor ---- */
#define GOODIX_ERRNO errno
#define GOODIX_LOGI LOGI
#define GOODIX_LOGW LOGW
#define GOODIX_LOGE LOGE
#include "goodix.h"
#define FP_LOGI LOGI
#define FP_LOGW LOGW
#include "gbos_fp_policy.h"

/* ---- AIDL ---- */
#define FP_DESCRIPTOR "android.hardware.biometrics.fingerprint.IFingerprint"
#define FP_INSTANCE FP_DESCRIPTOR "/default"
#define SESSION_DESCRIPTOR "android.hardware.biometrics.fingerprint.ISession"
#define CALLBACK_DESCRIPTOR "android.hardware.biometrics.fingerprint.ISessionCallback"
#define CANCEL_DESCRIPTOR "android.hardware.biometrics.common.ICancellationSignal"
#define FP_VERSION 4
#define FP_HASH "41a730a7a6b5aa9cebebce70ee5b5e509b0af6fb"	/* aidl_api/.../fingerprint/4/.hash */
#define COMMON_VERSION 4
#define COMMON_HASH "8a6cd86630181a4df6f20056259ec200ffe39209"	/* aidl_api/.../common/4/.hash */

enum {	/* ISessionCallback */
	CB_CHALLENGE_GENERATED = 1, CB_CHALLENGE_REVOKED, CB_ACQUIRED, CB_ERROR, CB_ENROLLMENT_PROGRESS,
	CB_AUTH_SUCCEEDED, CB_AUTH_FAILED, CB_LOCKOUT_TIMED, CB_LOCKOUT_PERMANENT, CB_LOCKOUT_CLEARED,
	CB_INTERACTION_DETECTED, CB_ENROLLMENTS_ENUMERATED, CB_ENROLLMENTS_REMOVED, CB_AUTHID_RETRIEVED,
	CB_AUTHID_INVALIDATED, CB_SESSION_CLOSED,
};
enum { ACQ_GOOD = 1, ACQ_INSUFFICIENT = 3 };	/* AcquiredInfo */
enum { ERR_HW_UNAVAILABLE = 1, ERR_UNABLE_TO_PROCESS = 2, ERR_NO_SPACE = 4, ERR_CANCELED = 5,
       ERR_UNABLE_TO_REMOVE = 6 };	/* Error */
#define STRENGTH_WEAK 1		/* SensorStrength */
#define TYPE_UNKNOWN 0		/* The exact sensor's physical location has not been verified. */
#define HAT_PASSWORD 1		/* HardwareAuthenticatorType */
#define AID_SYSTEM 1000

/* ---- policy ---- */
#define SENSOR_ID 0
#define MAX_ENROLL 5
#define CHALLENGE_TTL_MS (10 * 60 * 1000)
#define HAT_MAX_AGE_MS (10 * 60 * 1000)
#define LOCKOUT_TIMED_FAILS 5
#define LOCKOUT_TIMED_MS 30000
#define LOCKOUT_PERMANENT_FAILS 20
#define DATA_DIR "/data/vendor/crosfp/gbos_fp"	/* parent made by gbos.rc (and crosfp's rc) as system:system */

/* soft_gk's HardwareAuthToken key (gatekeeper/soft_gk.c, auth_key): its password tokens are what
 * system_server hands to enroll and resetLockout. tools/test_gbos_fp.sh checks the two stay equal. */
static const uint8_t auth_key[32] = "gbos soft gatekeeper authtoken ";

struct hat {
	int64_t challenge, user_id, authenticator_id, timestamp_ms;
	int32_t type, mac_len;
	uint8_t mac[32];
};

struct user_state {	/* DATA_DIR/u<user> */
	int32_t user;
	int32_t ids[MAX_ENROLL];
	int n;
	int32_t failed;
	bool permanent;
	bool trusted;	/* the file was read, or does not exist: `ids` is the whole truth */
};

struct session {
	int refs;		/* binder object, `active`, queued ops; under mu */
	AIBinder *cb;		/* ISessionCallback */
	int32_t user;
	bool closed;
	bool clear_pending;	/* a timed lockout is running: send onLockoutCleared when it ends */
	bool reconciled;	/* worker only: the sensor's stale templates were cleaned up for this session */
	struct user_state st;	/* only the worker touches it after createSession */
};

enum { OP_CHALLENGE, OP_REVOKE, OP_ENROLL, OP_AUTH, OP_DETECT, OP_ENUM, OP_REMOVE, OP_AUTHID,
       OP_INVALIDATE, OP_RESET_LOCKOUT, OP_CLOSE, OP_RECONCILE };

struct op {
	struct op *next;
	struct session *ss;
	int kind;
	int64_t id;		/* the ICancellationSignal handed out for it carries the same id */
	bool cancelled;
	int64_t arg;
	struct hat hat;
	int32_t *ids, n;
};

static pthread_mutex_t mu;
static pthread_cond_t cv;
static struct op *q_head, *q_tail, *cur;
static int64_t next_op_id = 1;
static struct session *active;
static struct goodix dev;		/* the worker's; other threads only call goodix_request_cancel */
static char instance[9];
static bool data_live;			/* DATA_DIR is on tmpfs: forgotten at reboot */
static bool no_install;			/* --no-gbos-install: hook.sh found no LABEL=GBOSSYS */
static bool installed;			/* --installed (booted from the NVMe install), /data persistent per
					 * statfs and its instance id stable: gbos_fp_policy.h FP_DELETE_REPLACED */
static struct { int32_t user; int64_t value, expires; } challenges[16];	/* under mu */
static struct { int32_t user; int64_t until; } timed[8];		/* under mu, monotonic ms */
static AIBinder_Class *fp_cls, *session_cls, *cb_cls, *cancel_cls;
static char hw_version[16], fw_version[40], serial[16];

static int64_t now_ms(int clock)
{
	struct timespec ts;

	clock_gettime(clock, &ts);
	return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* ---- storage ---- */

static int read_file(const char *path, char *buf, int size)
{
	int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW), n = -1;

	if (fd >= 0) {
		n = (int)read(fd, buf, (size_t)size - 1);
		close(fd);
	}
	buf[n > 0 ? n : 0] = 0;
	return n;
}

/* write + fsync + rename, so a crash leaves either the old or the new file */
static bool write_file(const char *path, const char *data, int len)
{
	char tmp[96];
	int fd, ok;

	snprintf(tmp, sizeof(tmp), "%s.tmp", path);
	fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW, 0600);
	if (fd < 0) {
		LOGE("open %s: %d", tmp, errno);
		return false;
	}
	ok = write(fd, data, (size_t)len) == len && fsync(fd) == 0;
	close(fd);
	if (!ok || rename(tmp, path) != 0) {
		LOGE("write %s failed: %d", path, errno);
		return false;
	}
	return true;
}

static bool parse_int(const char **p, int64_t *out)
{
	const char *s = *p;
	bool neg = *s == '-';
	int64_t v = 0;

	if (neg)
		s++;
	if (*s < '0' || *s > '9')
		return false;
	while (*s >= '0' && *s <= '9' && v < (1LL << 40))
		v = v * 10 + (*s++ - '0');
	*out = neg ? -v : v;
	*p = s;
	return true;
}

static bool starts(const char *s, const char *prefix)
{
	while (*prefix && *s == *prefix)
		s++, prefix++;
	return !*prefix;
}

static bool is_hex8(const char *s)
{
	for (int i = 0; i < 8; i++)
		if (!((s[i] >= '0' && s[i] <= '9') || (s[i] >= 'a' && s[i] <= 'f')))
			return false;
	return true;
}

/*
 * A random id per /data, so two gbos installs on one laptop (stick and SSD) never share template names.
 * Returns true when the id is this /data's for good: read from its file, or new and saved because there
 * was none. An unreadable or damaged file is left alone (this run uses a temporary id and the cleanup
 * treats other instances as it would on a live boot with an install).
 */
static bool load_instance(void)
{
	char buf[16];
	uint32_t r;
	int n = read_file(DATA_DIR "/instance", buf, sizeof(buf));
	bool missing = n < 0 && errno == ENOENT;

	if (n >= 8 && is_hex8(buf)) {
		memcpy(instance, buf, 8);
		instance[8] = 0;
		return true;
	}
	arc4random_buf(&r, sizeof(r));
	snprintf(instance, sizeof(instance), "%08x", r);
	if (!missing) {
		LOGE(DATA_DIR "/instance unreadable or damaged (%d, errno %d): using %s for this boot only",
		     n, errno, instance);
		return false;
	}
	return write_file(DATA_DIR "/instance", instance, 8);
}

static void st_load(struct user_state *st, int32_t user)
{
	char path[64], buf[1024];
	const char *p;
	int64_t v;
	int n;

	memset(st, 0, sizeof(*st));
	st->user = user;
	snprintf(path, sizeof(path), DATA_DIR "/u%d", user);
	n = read_file(path, buf, sizeof(buf));
	if (n <= 0) {
		/* no file: this user never enrolled here. A read error or an empty file (st_save never
		 * writes one): unknown, so nothing is deleted on its strength. */
		st->trusted = n < 0 && errno == ENOENT;
		if (!st->trusted)
			LOGE("user %d: state %s unreadable (%d, errno %d)", user, path, n, errno);
		return;
	}
	for (p = buf; *p; ) {
		if (starts(p, "end\n"))
			st->trusted = true;	/* st_save's last line: the file is complete */
		if (starts(p, "enrollment ")) {
			p += 11;
			if (parse_int(&p, &v) && v > 0 && v <= INT32_MAX && st->n < MAX_ENROLL)
				st->ids[st->n++] = (int32_t)v;
		} else if (starts(p, "failed ")) {
			p += 7;
			if (parse_int(&p, &v) && v >= 0 && v <= LOCKOUT_PERMANENT_FAILS)
				st->failed = (int32_t)v;
		} else if (starts(p, "permanent ")) {
			p += 10;
			if (parse_int(&p, &v))
				st->permanent = v != 0;
		}
		while (*p && *p != '\n')
			p++;
		if (*p)
			p++;
	}
}

static bool st_save(const struct user_state *st)
{
	char path[64], buf[512];
	int len;

	snprintf(path, sizeof(path), DATA_DIR "/u%d", st->user);
	len = snprintf(buf, sizeof(buf), "failed %d\npermanent %d\n", st->failed, st->permanent);
	for (int i = 0; i < st->n; i++)
		len += snprintf(buf + len, sizeof(buf) - (size_t)len, "enrollment %d\n", st->ids[i]);
	len += snprintf(buf + len, sizeof(buf) - (size_t)len, "end\n");	/* files before 2026-11-18 lack it */
	return write_file(path, buf, len);
}

static bool template_name(char out[FP_NAME_MAX + 1], int32_t user, int32_t id)
{
	return fp_name_format(out, data_live, instance, user, (uint32_t)id) > 0;
}

static struct fp_policy session_policy(const struct session *ss)
{
	struct fp_policy p = {
		.instance = instance, .live = data_live, .no_install = no_install, .user = ss->user,
		.ids = ss->st.ids, .n = ss->st.n, .ids_trusted = ss->st.trusted, .installed = installed,
	};

	return p;
}

/*
 * The sensor is open. Delete its templates that no Android can reach any more (gbos_fp_policy.h). Returns
 * fp_reconcile's result: negative means the sensor stopped answering and must be reopened.
 */
static int reconcile(struct session *ss)
{
	struct fp_policy p = session_policy(ss);
	int r, deleted, failed, fail = 0;

	r = fp_reconcile(&dev, &p, &deleted, &failed, &fail);
	if (r != GOODIX_OK) {
		LOGW("user %d: sensor template cleanup failed: %d (sensor code %d); retried at the next enroll",
		     ss->user, r, fail);
		return r;
	}
	ss->reconciled = failed == 0;
	return r;
}

static void op_reconcile(struct session *ss)
{
	int r;

	if (ss->reconciled)
		return;
	if ((r = goodix_open(&dev)) < 0) {
		LOGW("user %d: sensor unavailable for template cleanup: %d; retried at the next enroll",
		     ss->user, r);
		return;
	}
	reconcile(ss);
	goodix_close(&dev);
}

/* After a cancelled operation, before the sensor is closed: leave it idle for the next one. */
static void settle_after_cancel(void)
{
	int r = goodix_settle(&dev);

	if (r < 0)
		LOGW("sensor did not confirm idle after the cancel: %d (the next open reads past stale packets)", r);
}

/* ---- HardwareAuthToken ---- */

static uint64_t bswap64(uint64_t v) { return __builtin_bswap64(v); }

/* KeyMint layout, as soft_gk signs it: 0 || challenge || user_id || authenticator_id || htonl(type) || htonq(ts) */
static bool hat_signed(const struct hat *h)
{
	uint8_t buf[1 + 8 + 8 + 8 + 4 + 8], mac[32];
	uint32_t type_be = __builtin_bswap32((uint32_t)h->type);
	uint64_t ts_be = bswap64((uint64_t)h->timestamp_ms);
	unsigned int len;

	if (h->mac_len != 32)
		return false;
	buf[0] = 0;
	memcpy(buf + 1, &h->challenge, 8);
	memcpy(buf + 9, &h->user_id, 8);
	memcpy(buf + 17, &h->authenticator_id, 8);
	memcpy(buf + 25, &type_be, 4);
	memcpy(buf + 29, &ts_be, 8);
	HMAC(EVP_sha256(), auth_key, sizeof(auth_key), buf, sizeof(buf), mac, &len);
	return CRYPTO_memcmp(mac, h->mac, 32) == 0;
}

/* A password token from soft_gk, signed, and recent (soft_gk stamps CLOCK_BOOTTIME milliseconds). */
static bool hat_password_ok(const struct hat *h, const char *what)
{
	int64_t now = now_ms(CLOCK_BOOTTIME);

	if (!(h->type & HAT_PASSWORD) || !hat_signed(h)) {
		LOGE("%s: token not a signed password token (type %d)", what, h->type);
		return false;
	}
	if (h->timestamp_ms > now + 60000 || now - h->timestamp_ms > HAT_MAX_AGE_MS) {
		LOGE("%s: token too old or from the future", what);
		return false;
	}
	return true;
}

/* ---- parcels ---- */

struct bytes { int8_t *p; int32_t len; };
struct ints { int32_t *p; int32_t len; };

static bool byte_alloc(void *data, int32_t len, int8_t **out)
{
	struct bytes *b = data;

	b->len = len;
	b->p = len > 0 && len <= 4096 ? malloc((size_t)len) : NULL;
	*out = b->p;
	return len <= 0 || b->p;
}

static bool int_alloc(void *data, int32_t len, int32_t **out)
{
	struct ints *a = data;

	a->len = len;
	a->p = len > 0 && len <= 64 ? malloc((size_t)len * 4) : NULL;
	*out = a->p;
	return len <= 0 || a->p;
}

/* Parcelable framing: a non-null marker, then a size-prefixed body. */
static int32_t begin_parcelable(AParcel *out)
{
	int32_t start;

	AParcel_writeInt32(out, 1);
	start = AParcel_getDataPosition(out);
	AParcel_writeInt32(out, 0);
	return start;
}

static void end_parcelable(AParcel *out, int32_t start)
{
	int32_t end = AParcel_getDataPosition(out);

	AParcel_setDataPosition(out, start);
	AParcel_writeInt32(out, end - start);
	AParcel_setDataPosition(out, end);
}

static void write_ok_header(AParcel *out)
{
	AStatus *st = AStatus_newOk();

	AParcel_writeStatusHeader(out, st);
	AStatus_delete(st);
}

static void write_str(AParcel *out, const char *s) { AParcel_writeString(out, s, (int32_t)strlen(s)); }

/* Reading a parcelable: the marker, then fields only while they lie inside the declared size, then skip to
 * its end (a newer writer may have appended fields). */
struct pin { const AParcel *in; int32_t start, size; };

static binder_status_t pin_begin(struct pin *p, const AParcel *in, bool *present)
{
	int32_t marker;
	binder_status_t r;

	p->in = in;
	if ((r = AParcel_readInt32(in, &marker)))
		return r;
	*present = marker != 0;
	if (!*present)
		return STATUS_OK;
	p->start = AParcel_getDataPosition(in);
	if ((r = AParcel_readInt32(in, &p->size)))
		return r;
	return p->size < 4 ? STATUS_BAD_VALUE : STATUS_OK;
}

static bool pin_more(const struct pin *p) { return AParcel_getDataPosition(p->in) - p->start < p->size; }
static void pin_end(const struct pin *p) { AParcel_setDataPosition(p->in, p->start + p->size); }

static binder_status_t read_hat(const AParcel *in, struct hat *h)
{
	struct pin p, t;
	struct bytes mac = {0};
	bool present;
	binder_status_t r;

	memset(h, 0, sizeof(*h));
	if ((r = pin_begin(&p, in, &present)))
		return r;
	if (!present)
		return STATUS_BAD_VALUE;
	if (pin_more(&p) && (r = AParcel_readInt64(in, &h->challenge)))
		return r;
	if (pin_more(&p) && (r = AParcel_readInt64(in, &h->user_id)))
		return r;
	if (pin_more(&p) && (r = AParcel_readInt64(in, &h->authenticator_id)))
		return r;
	if (pin_more(&p) && (r = AParcel_readInt32(in, &h->type)))
		return r;
	if (pin_more(&p)) {
		if ((r = pin_begin(&t, in, &present)))
			return r;
		if (present) {
			if (pin_more(&t) && (r = AParcel_readInt64(in, &h->timestamp_ms)))
				return r;
			pin_end(&t);
		}
	}
	if (pin_more(&p)) {
		if ((r = AParcel_readByteArray(in, &mac, byte_alloc)))
			return r;
		h->mac_len = mac.len;
		if (mac.len == 32)
			memcpy(h->mac, mac.p, 32);
		free(mac.p);
	}
	pin_end(&p);
	return STATUS_OK;
}

/* ---- ISessionCallback (called from the worker, never with mu held) ---- */

struct cbargs { int32_t a, b; int64_t l; const int32_t *ids; int32_t n; };

static void cb_send(struct session *ss, transaction_code_t code, void (*fill)(AParcel *, const struct cbargs *),
		    const struct cbargs *a)
{
	AParcel *in = NULL, *out = NULL;
	binder_status_t r = AIBinder_prepareTransaction(ss->cb, &in);

	if (r == STATUS_OK) {
		if (fill)
			fill(in, a);
		r = AIBinder_transact(ss->cb, code, &in, &out, 0);
	}
	if (out)
		AParcel_delete(out);
	if (r != STATUS_OK)
		LOGE("callback %u for user %d failed: %d", code, ss->user, r);
}

static void fill_i64(AParcel *in, const struct cbargs *a) { AParcel_writeInt64(in, a->l); }
static void fill_byte_int(AParcel *in, const struct cbargs *a) { AParcel_writeByte(in, (int8_t)a->a); AParcel_writeInt32(in, a->b); }
static void fill_int_int(AParcel *in, const struct cbargs *a) { AParcel_writeInt32(in, a->a); AParcel_writeInt32(in, a->b); }
static void fill_ids(AParcel *in, const struct cbargs *a) { AParcel_writeInt32Array(in, a->ids, a->n); }

/* onAuthenticationSucceeded(id, hat): a WEAK sensor must send an empty token. */
static void fill_auth_ok(AParcel *in, const struct cbargs *a)
{
	int32_t start, ts;

	AParcel_writeInt32(in, a->a);
	start = begin_parcelable(in);
	AParcel_writeInt64(in, 0);	/* challenge */
	AParcel_writeInt64(in, 0);	/* userId */
	AParcel_writeInt64(in, 0);	/* authenticatorId */
	AParcel_writeInt32(in, 0);	/* authenticatorType NONE */
	ts = begin_parcelable(in);
	AParcel_writeInt64(in, 0);
	end_parcelable(in, ts);
	AParcel_writeByteArray(in, NULL, 0);
	end_parcelable(in, start);
}

static void cb_void(struct session *ss, transaction_code_t code) { cb_send(ss, code, NULL, NULL); }
static void cb_long(struct session *ss, transaction_code_t code, int64_t v)
{
	struct cbargs a = { .l = v };

	cb_send(ss, code, fill_i64, &a);
}
static void cb_error(struct session *ss, int err, int vendor)
{
	struct cbargs a = { .a = err, .b = vendor };

	cb_send(ss, CB_ERROR, fill_byte_int, &a);
}
static void cb_acquired(struct session *ss, int info)
{
	struct cbargs a = { .a = info };

	cb_send(ss, CB_ACQUIRED, fill_byte_int, &a);
}
static void cb_ids(struct session *ss, transaction_code_t code, const int32_t *ids, int32_t n)
{
	struct cbargs a = { .ids = ids, .n = n };

	cb_send(ss, code, fill_ids, &a);
}

/* ---- sessions and the operation queue ---- */

static void session_unref_locked(struct session *ss)
{
	if (--ss->refs == 0) {
		AIBinder_decStrong(ss->cb);
		free(ss);
	}
}

static int64_t timed_left_locked(int32_t user)
{
	int64_t now = now_ms(CLOCK_MONOTONIC);

	for (size_t i = 0; i < sizeof(timed) / sizeof(timed[0]); i++)
		if (timed[i].until && timed[i].user == user)
			return timed[i].until > now ? timed[i].until - now : 0;
	return 0;
}

static void timed_set_locked(int32_t user, int64_t until)
{
	size_t i, slot = 0;

	for (i = 0; i < sizeof(timed) / sizeof(timed[0]); i++) {
		if (timed[i].until && timed[i].user == user)
			break;
		if (!timed[i].until || timed[i].until < timed[slot].until)
			slot = i;
	}
	if (i == sizeof(timed) / sizeof(timed[0]))
		i = slot;
	timed[i].user = user;
	timed[i].until = until;
}

static int64_t enqueue(struct session *ss, struct op *op)
{
	int64_t id;

	pthread_mutex_lock(&mu);
	op->ss = ss;
	op->id = id = next_op_id++;
	ss->refs++;
	if (q_tail)
		q_tail->next = op;
	else
		q_head = op;
	q_tail = op;
	pthread_cond_signal(&cv);
	pthread_mutex_unlock(&mu);
	return id;
}

/* ---- ICancellationSignal ---- */

static void *on_create(void *args) { return args; }
static void on_destroy_free(void *data) { free(data); }

static binder_status_t cancel_transact(AIBinder *b, transaction_code_t code, const AParcel *in, AParcel *out)
{
	int64_t id = *(int64_t *)AIBinder_getUserData(b);

	(void)in;
	if (AIBinder_getCallingUid() != AID_SYSTEM)
		return STATUS_PERMISSION_DENIED;
	switch (code) {
	case 1:		/* oneway cancel() */
		pthread_mutex_lock(&mu);
		if (cur && cur->id == id) {
			goodix_request_cancel(&dev);
		} else {
			for (struct op *o = q_head; o; o = o->next)
				if (o->id == id)
					o->cancelled = true;
		}
		pthread_mutex_unlock(&mu);
		return STATUS_OK;
	case 0xffffff:
		write_ok_header(out);
		return AParcel_writeInt32(out, COMMON_VERSION);
	case 0xfffffe:
		write_ok_header(out);
		return AParcel_writeString(out, COMMON_HASH, sizeof(COMMON_HASH) - 1);
	default:
		return STATUS_UNKNOWN_TRANSACTION;
	}
}

static binder_status_t reply_cancel_signal(AParcel *out, int64_t id)
{
	int64_t *tok = malloc(sizeof(*tok));
	AIBinder *b;
	binder_status_t r;

	if (!tok)
		return STATUS_BAD_VALUE;
	*tok = id;
	if (!(b = AIBinder_new(cancel_cls, tok))) {
		free(tok);
		return STATUS_BAD_VALUE;
	}
	AIBinder_markVintfStability(b);
	write_ok_header(out);
	r = AParcel_writeStrongBinder(out, b);
	AIBinder_decStrong(b);
	return r;
}

/* ---- the worker: every operation runs here, in order, one at a time ---- */

struct op_ctx { struct session *ss; int32_t enroll_id; };

static void on_sensor_event(void *ctx, int kind, int value)
{
	struct op_ctx *c = ctx;

	if (kind == GOODIX_EV_CAPTURE) {
		cb_acquired(c->ss, ACQ_GOOD);
	} else if (kind == GOODIX_EV_RETRY) {
		cb_acquired(c->ss, ACQ_INSUFFICIENT);
	} else if (kind == GOODIX_EV_PROGRESS && c->enroll_id) {
		/* remaining = 0 is sent by op_enroll, only once the enrollment is saved */
		struct cbargs a = { .a = c->enroll_id, .b = value < GOODIX_ENROLL_STAGES ? GOODIX_ENROLL_STAGES - value : 1 };

		cb_send(c->ss, CB_ENROLLMENT_PROGRESS, fill_int_int, &a);
	}
}

/*
 * goodix_open() failed. -ECANCELED is the framework cancelling the operation while the sensor was being opened
 * (boot-13: keyguard restarts authentication right after a lockout is cleared): report a cancel, as
 * BiometricScheduler expects, not ERR_HW_UNAVAILABLE, which made it recreate the session every time.
 */
static void open_failed(struct session *ss, int r, const char *when)
{
	if (r == -ECANCELED) {
		LOGI("user %d: cancelled while opening the sensor%s", ss->user, when);
		cb_error(ss, ERR_CANCELED, 0);
		return;
	}
	LOGE("user %d: sensor open failed%s: %d", ss->user, when, r);
	cb_error(ss, ERR_HW_UNAVAILABLE, 0);
}

/* A rejected finger: count it and lock out at the thresholds. True when the operation must end. */
static bool count_failure(struct session *ss)
{
	struct user_state *st = &ss->st;

	st->failed++;
	if (st->failed >= LOCKOUT_PERMANENT_FAILS) {
		st->permanent = true;
		st_save(st);
		LOGW("user %d: permanent lockout after %d failures", ss->user, st->failed);
		cb_void(ss, CB_LOCKOUT_PERMANENT);
		return true;
	}
	st_save(st);
	if (st->failed % LOCKOUT_TIMED_FAILS == 0) {
		pthread_mutex_lock(&mu);
		timed_set_locked(ss->user, now_ms(CLOCK_MONOTONIC) + LOCKOUT_TIMED_MS);
		ss->clear_pending = true;
		pthread_mutex_unlock(&mu);
		LOGW("user %d: %d s lockout after %d failures", ss->user, LOCKOUT_TIMED_MS / 1000, st->failed);
		cb_long(ss, CB_LOCKOUT_TIMED, LOCKOUT_TIMED_MS);
		return true;
	}
	return false;
}

static void op_authenticate(struct session *ss)
{
	struct user_state *st = &ss->st;
	struct op_ctx ctx = { ss, 0 };
	struct goodix_ev ev = { on_sensor_event, &ctx };
	char names[MAX_ENROLL][FP_NAME_MAX + 1], matched[GOODIX_MAX_UID + 1];
	const char *list[MAX_ENROLL];
	int64_t left;
	int r, fail = 0, i;

	pthread_mutex_lock(&mu);
	left = timed_left_locked(ss->user);
	pthread_mutex_unlock(&mu);
	if (st->permanent) {
		cb_void(ss, CB_LOCKOUT_PERMANENT);
		return;
	}
	if (left) {
		cb_long(ss, CB_LOCKOUT_TIMED, left);
		return;
	}
	if (!st->n) {
		cb_error(ss, ERR_UNABLE_TO_PROCESS, 0);
		return;
	}
	for (i = 0; i < st->n; i++) {
		if (!template_name(names[i], ss->user, st->ids[i])) {
			cb_error(ss, ERR_UNABLE_TO_PROCESS, 0);
			return;
		}
		list[i] = names[i];
	}
	LOGI("user %d: authenticate starting (%d enrollments)", ss->user, st->n);
	if ((r = goodix_open(&dev)) < 0) {
		open_failed(ss, r, "");
		return;
	}
	for (;;) {
		r = goodix_identify(&dev, list, st->n, &ev, matched, &fail);
		if (r < 0 && r != -ECANCELED && goodix_cancel_requested(&dev)) {
			LOGW("identify ended with %d while being cancelled: reported as cancelled", r);
			r = -ECANCELED;
		}
		if (r == GOODIX_OK) {
			for (i = 0; i < st->n && strcmp(matched, names[i]); i++)
				;
			if (i == st->n) {	/* not one of the names offered: never accept it */
				LOGE("sensor matched a template that was not offered");
				cb_void(ss, CB_AUTH_FAILED);
				if (count_failure(ss))
					break;
				continue;
			}
			if (st->failed) {
				st->failed = 0;
				st_save(st);
			}
			LOGI("user %d: accepted enrollment %d", ss->user, st->ids[i]);
			{
				struct cbargs a = { .a = st->ids[i] };

				cb_send(ss, CB_AUTH_SUCCEEDED, fill_auth_ok, &a);
			}
			break;
		}
		if (r == GOODIX_NOMATCH) {
			cb_void(ss, CB_AUTH_FAILED);
			if (count_failure(ss))
				break;
			continue;
		}
		if (r == GOODIX_RETRY)
			continue;
		if (r == -ECANCELED) {
			cb_error(ss, ERR_CANCELED, 0);
			settle_after_cancel();
		} else if (r == GOODIX_NORECORD) {
			LOGE("user %d: none of the %d templates is on the sensor any more (deleted outside "
			     "Android?): remove and enroll the fingerprints again", ss->user, st->n);
			cb_error(ss, ERR_UNABLE_TO_PROCESS, GOODIX_NO_RECORD);
		} else {
			LOGE("identify failed: %d (sensor code %d)", r, fail);
			cb_error(ss, r == GOODIX_FAILED ? ERR_UNABLE_TO_PROCESS : ERR_HW_UNAVAILABLE, fail);
		}
		break;
	}
	goodix_close(&dev);
}

static bool challenge_valid(int32_t user, int64_t value)
{
	int64_t now = now_ms(CLOCK_MONOTONIC);
	bool ok = false;

	pthread_mutex_lock(&mu);
	for (size_t i = 0; i < sizeof(challenges) / sizeof(challenges[0]); i++)
		if (challenges[i].value == value && challenges[i].user == user && challenges[i].expires > now)
			ok = true;
	pthread_mutex_unlock(&mu);
	return ok && value != 0;
}

static void op_enroll(struct session *ss, const struct hat *hat)
{
	struct user_state *st = &ss->st;
	struct op_ctx ctx = { ss, 0 };
	struct goodix_ev ev = { on_sensor_event, &ctx };
	struct goodix_template dup;
	char name[FP_NAME_MAX + 1];
	int32_t id;
	int r, fail = 0, i;

	if (!hat_password_ok(hat, "enroll") || !challenge_valid(ss->user, hat->challenge)) {
		LOGE("user %d: enroll refused (token or challenge invalid)", ss->user);
		cb_error(ss, ERR_UNABLE_TO_PROCESS, 0);
		return;
	}
	if (st->n >= MAX_ENROLL) {
		cb_error(ss, ERR_NO_SPACE, 0);
		return;
	}
	do {	/* positive, and unique for this user */
		arc4random_buf(&id, sizeof(id));
		id &= INT32_MAX;
		for (i = 0; i < st->n && st->ids[i] != id; i++)
			;
	} while (id == 0 || i < st->n);
	ctx.enroll_id = id;
	if (!template_name(name, ss->user, id)) {
		cb_error(ss, ERR_UNABLE_TO_PROCESS, 0);
		return;
	}
	LOGI("user %d: enroll %d starting as %s", ss->user, id, name);
	if ((r = goodix_open(&dev)) < 0) {
		open_failed(ss, r, "");
		return;
	}
	/* createSession queued a cleanup; if it could not run, do it now, before the sensor's duplicate check */
	if (!ss->reconciled && reconcile(ss) < 0) {
		goodix_close(&dev);
		if ((r = goodix_open(&dev)) < 0) {
			open_failed(ss, r, " after the template cleanup");
			return;
		}
	}
	{
		struct fp_policy p = session_policy(ss);
		int restarted;

		r = fp_enroll(&dev, &p, name, &ev, &fail, &dup, &restarted);
		if (restarted)
			LOGI("user %d: enrollment restarted %d time(s) after deleting a stale template", ss->user,
			     restarted);
	}
	/* Settings cancels its "find the sensor" enroll at the first touch: an I/O error on the way out is still a
	 * cancel to the framework */
	if (r < 0 && r != -ECANCELED && goodix_cancel_requested(&dev)) {
		LOGW("user %d: enroll ended with %d while being cancelled: reported as cancelled", ss->user, r);
		r = -ECANCELED;
	}
	if (r == GOODIX_OK) {
		st->ids[st->n++] = id;
		if (st_save(st)) {
			struct cbargs a = { .a = id, .b = 0 };

			LOGI("user %d: enrolled %d", ss->user, id);
			cb_send(ss, CB_ENROLLMENT_PROGRESS, fill_int_int, &a);
		} else {	/* not recorded: don't leave an unreachable template on the sensor */
			st->n--;
			goodix_delete(&dev, name, &fail);
			cb_error(ss, ERR_UNABLE_TO_PROCESS, 0);
		}
	} else if (r == -ECANCELED) {
		cb_error(ss, ERR_CANCELED, 0);
		settle_after_cancel();
	} else if (r == GOODIX_DUPLICATE) {	/* fp_enroll logged who owns it and why it stays */
		LOGE("user %d: enroll failed: finger already stored on the sensor (sensor code %d)", ss->user, fail);
		cb_error(ss, ERR_UNABLE_TO_PROCESS, fail);
	} else if (r == GOODIX_FAILED) {
		int gone = 0;

		LOGE("user %d: enroll failed, sensor code %d", ss->user, fail);
		/* a refused commit may still have stored it: don't leave a template Android cannot reach */
		if (goodix_delete(&dev, name, &gone) == GOODIX_OK)
			LOGW("user %d: removed the partly stored template %s", ss->user, name);
		cb_error(ss, fail == GOODIX_NO_SPACE ? ERR_NO_SPACE : ERR_UNABLE_TO_PROCESS, fail);
	} else {
		LOGE("user %d: enroll failed: %d", ss->user, r);
		cb_error(ss, ERR_HW_UNAVAILABLE, 0);
	}
	goodix_close(&dev);
}

static void op_remove(struct session *ss, const int32_t *ids, int32_t n)
{
	struct user_state *st = &ss->st;
	char name[48];
	bool failed = false, opened = false;
	int fail = 0;

	for (int32_t k = 0; k < n; k++) {
		int i, r;

		for (i = 0; i < st->n && st->ids[i] != ids[k]; i++)
			;
		if (i == st->n)
			continue;	/* not ours (or already gone): nothing to delete */
		if (!opened && goodix_open(&dev) < 0) {
			failed = true;
			break;
		}
		opened = true;
		template_name(name, ss->user, ids[k]);
		r = goodix_delete(&dev, name, &fail);
		if (r != GOODIX_OK && r != GOODIX_NORECORD) {
			LOGE("user %d: deleting %d failed: %d (sensor code %d)", ss->user, ids[k], r, fail);
			failed = true;
			continue;
		}
		st->ids[i] = st->ids[--st->n];
	}
	if (opened)
		goodix_close(&dev);
	st_save(st);
	if (failed)
		cb_error(ss, ERR_UNABLE_TO_REMOVE, 0);
	else
		cb_ids(ss, CB_ENROLLMENTS_REMOVED, ids, n);
}

static void op_challenge(struct session *ss)
{
	int64_t value, now = now_ms(CLOCK_MONOTONIC);
	size_t slot = 0;

	do
		arc4random_buf(&value, sizeof(value));
	while (value == 0);
	pthread_mutex_lock(&mu);
	for (size_t i = 0; i < sizeof(challenges) / sizeof(challenges[0]); i++)
		if (challenges[i].expires <= now || challenges[i].expires < challenges[slot].expires)
			slot = i;
	challenges[slot].user = ss->user;
	challenges[slot].value = value;
	challenges[slot].expires = now + CHALLENGE_TTL_MS;
	pthread_mutex_unlock(&mu);
	cb_long(ss, CB_CHALLENGE_GENERATED, value);
}

static void op_revoke(struct session *ss, int64_t value)
{
	pthread_mutex_lock(&mu);
	for (size_t i = 0; i < sizeof(challenges) / sizeof(challenges[0]); i++)
		if (challenges[i].value == value && challenges[i].user == ss->user)
			challenges[i].expires = 0;
	pthread_mutex_unlock(&mu);
	cb_long(ss, CB_CHALLENGE_REVOKED, value);
}

static void op_reset_lockout(struct session *ss, const struct hat *hat)
{
	if (!hat_password_ok(hat, "resetLockout")) {
		cb_error(ss, ERR_UNABLE_TO_PROCESS, 0);
		return;
	}
	ss->st.failed = 0;
	ss->st.permanent = false;
	st_save(&ss->st);
	pthread_mutex_lock(&mu);
	timed_set_locked(ss->user, 0);
	ss->clear_pending = false;
	pthread_mutex_unlock(&mu);
	LOGI("user %d: lockout cleared with the password", ss->user);
	cb_void(ss, CB_LOCKOUT_CLEARED);
}

static void run(struct op *op)
{
	struct session *ss = op->ss;

	if (op->cancelled) {	/* cancelled before it started */
		cb_error(ss, ERR_CANCELED, 0);
		return;
	}
	switch (op->kind) {
	case OP_CHALLENGE: op_challenge(ss); break;
	case OP_REVOKE: op_revoke(ss, op->arg); break;
	case OP_ENROLL: op_enroll(ss, &op->hat); break;
	case OP_AUTH: op_authenticate(ss); break;
	case OP_DETECT: cb_error(ss, ERR_UNABLE_TO_PROCESS, 0); break;	/* supportsDetectInteraction = false */
	case OP_ENUM: cb_ids(ss, CB_ENROLLMENTS_ENUMERATED, ss->st.ids, ss->st.n); break;
	case OP_REMOVE: op_remove(ss, op->ids, op->n); break;
	case OP_AUTHID: cb_long(ss, CB_AUTHID_RETRIEVED, 0); break;		/* WEAK: always 0 */
	case OP_INVALIDATE: cb_long(ss, CB_AUTHID_INVALIDATED, 0); break;
	case OP_RESET_LOCKOUT: op_reset_lockout(ss, &op->hat); break;
	case OP_RECONCILE: op_reconcile(ss); break;	/* internal: no callback */
	case OP_CLOSE:
		pthread_mutex_lock(&mu);
		ss->closed = true;
		pthread_mutex_unlock(&mu);
		cb_void(ss, CB_SESSION_CLOSED);
		break;
	}
}

static void *worker(void *arg)
{
	(void)arg;
	pthread_mutex_lock(&mu);
	for (;;) {
		struct op *op;

		if (!q_head) {
			struct session *ss = active;
			int64_t left;

			if (ss && ss->clear_pending && !ss->closed) {	/* timed lockout: report its end */
				if ((left = timed_left_locked(ss->user)) == 0) {
					ss->clear_pending = false;
					ss->refs++;
					pthread_mutex_unlock(&mu);
					cb_void(ss, CB_LOCKOUT_CLEARED);
					pthread_mutex_lock(&mu);
					session_unref_locked(ss);
				} else {
					int64_t until = now_ms(CLOCK_MONOTONIC) + left;
					struct timespec ts = { (long)(until / 1000), (long)(until % 1000) * 1000000 };

					pthread_cond_clockwait(&cv, &mu, CLOCK_MONOTONIC, &ts);
				}
				continue;
			}
			pthread_cond_wait(&cv, &mu);
			continue;
		}
		op = q_head;
		q_head = op->next;
		if (!q_head)
			q_tail = NULL;
		if (op->ss->closed && op->kind != OP_CLOSE) {	/* session gone: drop silently */
			session_unref_locked(op->ss);
			free(op->ids);
			free(op);
			continue;
		}
		cur = op;
		goodix_clear_cancel(&dev);
		pthread_mutex_unlock(&mu);
		run(op);
		pthread_mutex_lock(&mu);
		cur = NULL;
		session_unref_locked(op->ss);
		free(op->ids);
		free(op);
	}
	return NULL;
}

/* ---- ISession ---- */

static void session_destroy(void *data)
{
	pthread_mutex_lock(&mu);
	session_unref_locked(data);
	pthread_mutex_unlock(&mu);
}

static binder_status_t queue_simple(struct session *ss, int kind, int64_t arg, AParcel *out)
{
	struct op *op = calloc(1, sizeof(*op));

	if (!op)
		return STATUS_BAD_VALUE;
	op->kind = kind;
	op->arg = arg;
	enqueue(ss, op);
	write_ok_header(out);
	return STATUS_OK;
}

static binder_status_t queue_cancellable(struct session *ss, int kind, const struct hat *hat, AParcel *out)
{
	struct op *op = calloc(1, sizeof(*op));

	if (!op)
		return STATUS_BAD_VALUE;
	op->kind = kind;
	if (hat)
		op->hat = *hat;
	return reply_cancel_signal(out, enqueue(ss, op));
}

static binder_status_t session_transact(AIBinder *b, transaction_code_t code, const AParcel *in, AParcel *out)
{
	struct session *ss = AIBinder_getUserData(b);
	struct hat hat;
	struct ints ids = {0};
	struct op *op;
	int64_t v;
	binder_status_t r;

	if (AIBinder_getCallingUid() != AID_SYSTEM)
		return STATUS_PERMISSION_DENIED;
	switch (code) {
	case 1:		/* generateChallenge */
		return queue_simple(ss, OP_CHALLENGE, 0, out);
	case 2:		/* revokeChallenge(long) */
		if ((r = AParcel_readInt64(in, &v)))
			return r;
		return queue_simple(ss, OP_REVOKE, v, out);
	case 3:		/* enroll(hat) */
	case 16:	/* enrollWithContext(hat, context): the context only describes the UI */
		if ((r = read_hat(in, &hat)))
			return r;
		return queue_cancellable(ss, OP_ENROLL, &hat, out);
	case 4:		/* authenticate(operationId): only a STRONG sensor would put it in a token */
	case 15:	/* authenticateWithContext(operationId, context) */
		if ((r = AParcel_readInt64(in, &v)))
			return r;
		return queue_cancellable(ss, OP_AUTH, NULL, out);
	case 5:		/* detectInteraction */
	case 17:	/* detectInteractionWithContext */
		return queue_cancellable(ss, OP_DETECT, NULL, out);
	case 6:		/* enumerateEnrollments */
		return queue_simple(ss, OP_ENUM, 0, out);
	case 7:		/* removeEnrollments(int[]) */
		if ((r = AParcel_readInt32Array(in, &ids, int_alloc)))
			return r;
		if (!(op = calloc(1, sizeof(*op)))) {
			free(ids.p);
			return STATUS_BAD_VALUE;
		}
		op->kind = OP_REMOVE;
		op->ids = ids.p;
		op->n = ids.len > 0 ? ids.len : 0;
		enqueue(ss, op);
		write_ok_header(out);
		return STATUS_OK;
	case 8:		/* getAuthenticatorId */
		return queue_simple(ss, OP_AUTHID, 0, out);
	case 9:		/* invalidateAuthenticatorId */
		return queue_simple(ss, OP_INVALIDATE, 0, out);
	case 10:	/* resetLockout(hat) */
		if ((r = read_hat(in, &hat)))
			return r;
		if (!(op = calloc(1, sizeof(*op))))
			return STATUS_BAD_VALUE;
		op->kind = OP_RESET_LOCKOUT;
		op->hat = hat;
		enqueue(ss, op);
		write_ok_header(out);
		return STATUS_OK;
	case 11:	/* close */
		return queue_simple(ss, OP_CLOSE, 0, out);
	case 12: case 13: case 14: case 18: case 19: case 20: case 21: case 22:
		/* pointer / UI / context notifications: only for under-display sensors */
		write_ok_header(out);
		return STATUS_OK;
	case 0xffffff:
		write_ok_header(out);
		return AParcel_writeInt32(out, FP_VERSION);
	case 0xfffffe:
		write_ok_header(out);
		return AParcel_writeString(out, FP_HASH, sizeof(FP_HASH) - 1);
	default:
		return STATUS_UNKNOWN_TRANSACTION;
	}
}

/* ---- IFingerprint ---- */

static binder_status_t write_sensor_props(AParcel *out)
{
	int32_t props, common, comp;

	write_ok_header(out);
	AParcel_writeInt32(out, 1);		/* SensorProps[1] */
	props = begin_parcelable(out);
	common = begin_parcelable(out);		/* CommonProps */
	AParcel_writeInt32(out, SENSOR_ID);
	AParcel_writeByte(out, STRENGTH_WEAK);
	AParcel_writeInt32(out, MAX_ENROLL);
	AParcel_writeInt32(out, 1);		/* ComponentInfo[1] */
	comp = begin_parcelable(out);
	write_str(out, "Goodix fingerprint sensor (27c6:6384)");
	write_str(out, hw_version);
	write_str(out, fw_version);
	write_str(out, serial);
	write_str(out, "gbos_fp 1");
	end_parcelable(out, comp);
	end_parcelable(out, common);
	AParcel_writeByte(out, TYPE_UNKNOWN);
	AParcel_writeInt32(out, 0);		/* SensorLocation[0]: no unverified position */
	AParcel_writeBool(out, false);		/* supportsNavigationGestures */
	AParcel_writeBool(out, false);		/* supportsDetectInteraction */
	AParcel_writeBool(out, false);		/* halHandlesDisplayTouches */
	AParcel_writeBool(out, false);		/* halControlsIllumination */
	AParcel_writeInt32(out, 0);		/* touchDetectionParameters: null */
	end_parcelable(out, props);
	return STATUS_OK;
}

static binder_status_t create_session(const AParcel *in, AParcel *out)
{
	int32_t sensor_id, user;
	AIBinder *cb = NULL, *b;
	struct session *ss, *old;
	binder_status_t r;

	if ((r = AParcel_readInt32(in, &sensor_id)) || (r = AParcel_readInt32(in, &user)) ||
	    (r = AParcel_readStrongBinder(in, &cb)))
		return r;
	if (sensor_id != SENSOR_ID || user < 0 || !cb || !AIBinder_associateClass(cb, cb_cls)) {
		LOGE("createSession(%d, %d) refused", sensor_id, user);
		if (cb)
			AIBinder_decStrong(cb);
		return STATUS_BAD_VALUE;
	}
	if (!(ss = calloc(1, sizeof(*ss)))) {
		AIBinder_decStrong(cb);
		return STATUS_BAD_VALUE;
	}
	ss->cb = cb;
	ss->user = user;
	ss->refs = 2;		/* the binder object and `active` */
	st_load(&ss->st, user);
	if (!(b = AIBinder_new(session_cls, ss))) {
		AIBinder_decStrong(cb);
		free(ss);
		return STATUS_BAD_VALUE;
	}
	AIBinder_markVintfStability(b);

	pthread_mutex_lock(&mu);
	old = active;
	active = ss;
	if (timed_left_locked(user))
		ss->clear_pending = true;
	if (old) {	/* a new session without closing the old one (system_server restarted): retire it */
		LOGW("createSession while user %d's session is open: closing it", old->user);
		old->closed = true;
		if (cur && cur->ss == old)
			goodix_request_cancel(&dev);
		session_unref_locked(old);
	}
	pthread_mutex_unlock(&mu);
	LOGI("session for user %d (%d enrollments%s)", user, ss->st.n,
	     ss->st.trusted ? "" : ", state not trusted: no template cleanup for this user");
	{	/* delete the sensor's unreachable templates before anything else runs for this session */
		struct op *op = calloc(1, sizeof(*op));

		if (op) {
			op->kind = OP_RECONCILE;
			enqueue(ss, op);
		}
	}

	write_ok_header(out);
	r = AParcel_writeStrongBinder(out, b);
	AIBinder_decStrong(b);
	return r;
}

static binder_status_t fp_transact(AIBinder *b, transaction_code_t code, const AParcel *in, AParcel *out)
{
	(void)b;
	if (AIBinder_getCallingUid() != AID_SYSTEM)
		return STATUS_PERMISSION_DENIED;
	switch (code) {
	case 1:		/* getSensorProps */
		return write_sensor_props(out);
	case 2:		/* createSession(sensorId, userId, cb) */
		return create_session(in, out);
	case 0xffffff:
		write_ok_header(out);
		return AParcel_writeInt32(out, FP_VERSION);
	case 0xfffffe:
		write_ok_header(out);
		return AParcel_writeString(out, FP_HASH, sizeof(FP_HASH) - 1);
	default:
		return STATUS_UNKNOWN_TRANSACTION;
	}
}

static void on_destroy_none(void *data) { (void)data; }
static binder_status_t no_transact(AIBinder *b, transaction_code_t code, const AParcel *in, AParcel *out)
{
	(void)b; (void)code; (void)in; (void)out;
	return STATUS_UNKNOWN_TRANSACTION;
}

int main(int argc, char **argv, char **envp)
{
	AIBinder *binder;
	pthread_t t;
	int32_t ex;
	struct statfs_buf fs;
	bool boot_installed = false, persistent = false;
	int i, probe_result = 0;

	(void)envp;
	for (i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--no-gbos-install") == 0)
			no_install = true;
		else if (strcmp(argv[i], "--installed") == 0)
			boot_installed = true;
		else if (strcmp(argv[i], "--log") == 0 && i + 1 < argc) {
			log_fd = open(argv[++i], O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC | O_NOFOLLOW, 0644);
			if (log_fd < 0)
				__android_log_print(6, "gbos_fp", "cannot open log %s: errno %d", argv[i], errno);
		}
		else
			LOGW("ignoring unknown argument %s", argv[i]);
	}
	goodix_init(&dev);
	if (mkdir(DATA_DIR, 0700) != 0 && errno != EEXIST)
		LOGE("mkdir " DATA_DIR ": %d", errno);
	/* A statfs failure counts as persistent: it only ever makes the cleanup keep more templates. */
	if (statfs(DATA_DIR, &fs) == 0) {
		data_live = fs.f_type == TMPFS_MAGIC || fs.f_type == RAMFS_MAGIC;
		persistent = !data_live;
	} else {
		LOGW("statfs " DATA_DIR ": %d; treating /data as persistent", errno);
	}
	/* all three, or the install keeps other instances' templates as before */
	installed = load_instance() && boot_installed && persistent && !no_install;
	LOGI("data %s, gbOS86 install on this machine: %s%s", data_live ? "tmpfs (live boot)" : "persistent",
	     no_install ? "none" : "present or unknown",
	     installed ? "; this is the install's /data" :
	     boot_installed ? "; booted from the install but its /data is not confirmed" : "");

	/* hook.sh starts this before system_server; retries cover USB nodes still settling. */
	for (i = 0; i < 12; i++) {
		LOGI("sensor initialization attempt %d/12", i + 1);
		probe_result = goodix_open(&dev);
		if (probe_result == 0)
			break;
		LOGW("sensor initialization attempt %d/12 failed: %d", i + 1, probe_result);
		if (i < 11)
			usleep(5 * 1000 * 1000);
	}
	if (i == 12) {
		LOGE("no usable Goodix sensor after 12 attempts (last error %d), not registering " FP_INSTANCE,
		     probe_result);
		return 1;
	}
	snprintf(hw_version, sizeof(hw_version), "27c6:%04x", dev.pid);
	snprintf(fw_version, sizeof(fw_version), "%.8s", dev.fw_version);
	serial[0] = 0;
	goodix_close(&dev);
	LOGI("sensor %s firmware %s, install %s", hw_version, fw_version, instance);

	ABinderProcess_setThreadPoolMaxThreadCount(0);
	fp_cls = AIBinder_Class_define(FP_DESCRIPTOR, on_create, on_destroy_none, fp_transact);
	session_cls = AIBinder_Class_define(SESSION_DESCRIPTOR, on_create, session_destroy, session_transact);
	cb_cls = AIBinder_Class_define(CALLBACK_DESCRIPTOR, on_create, on_destroy_none, no_transact);
	cancel_cls = AIBinder_Class_define(CANCEL_DESCRIPTOR, on_create, on_destroy_free, cancel_transact);
	if (pthread_create(&t, NULL, worker, NULL) != 0) {
		LOGE("worker thread: %d", errno);
		return 1;
	}
	binder = AIBinder_new(fp_cls, NULL);
	AIBinder_markVintfStability(binder);
	ex = AServiceManager_addService(binder, FP_INSTANCE);
	if (ex != 0) {
		LOGE("addService(%s) failed: %d", FP_INSTANCE, ex);
		return 1;
	}
	LOGI("registered %s", FP_INSTANCE);
	ABinderProcess_joinThreadPool();
	return 0;
}

static const structors_array_t structors;

int start_main(void *raw_args) __asm__("fp_start_main");
__attribute__((used)) int start_main(void *raw_args)
{
	__libc_init(raw_args, NULL, main, &structors);
}

__asm__(".text\n.globl _start\n.type _start,@function\n_start:\n"
	"  xorl %ebp, %ebp\n  movq %rsp, %rdi\n  andq $-16, %rsp\n  call fp_start_main\n  hlt\n");
