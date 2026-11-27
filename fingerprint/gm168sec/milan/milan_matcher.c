/* milan_matcher.c - Milan matcher wiring for the 533c HAL.
 *
 * Implements the gm_matcher_* interface declared in gm168sec_hal.h on
 * top of the vendored Milan runtime (milan/src/runtime.h). The runtime
 * is the matcher: one run takes a setup (no-finger) frame + a live
 * frame + a gallery of stored templates and returns a verdict.
 *
 * Entry points (runtime.h):
 *   goodix_milan_runtime_input_new (runtime.h:185) - takes the setup and
 *     live frames as guint16[9504] (12-bit values in 16-bit words, NOT
 *     packed raw12), plus tcode/dac from the OTP calibration and the
 *     sensor subtype (12, Milan profile 9).
 *   goodix_milan_runtime_run (runtime.h:245) - returns
 *     GoodixMilanRuntimeOutput with status MATCH/NO_MATCH/RETRY,
 *     winner_index, score, quality, coverage, probe_template (the
 *     enrolled sample's template) and gallery_results (per-template
 *     accepted flags, used for duplicate detection).
 *   goodix_milan_runtime_enrollment_admitted (runtime.h:736) - the
 *     admission gate: preprocess valid, probe present, quality >= 25,
 *     coverage >= 65 (GOODIX_MILAN_ENROLL_MIN_QUALITY/COVERAGE,
 *     runtime.h:173-174).
 *   goodix_milan_enrollment_transaction_* (match/match.h:41-56) -
 *     accumulates accepted samples; publish returns the finished
 *     template as GBytes.
 *
 * The HAL's verdict comes from output->status, never from a hand-rolled
 * score threshold (threshold tables live in milan/match/policy.c).
 * Templates are stored verbatim: the published GBytes are written to
 * disk as-is by goodix_enroll (gm168sec_hal.h gm_tpl_save); there is
 * no GVariant FpPrint envelope (see milan-audit.md).
 */
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE 1
#endif
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#ifdef __ANDROID__
extern void gbos_fp_log(int prio, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
#define GOODIX_LOGI(...) gbos_fp_log(4, __VA_ARGS__)
#define GOODIX_LOGW(...) gbos_fp_log(5, __VA_ARGS__)
#define GOODIX_LOGE(...) gbos_fp_log(6, __VA_ARGS__)
#endif

#include "gm168sec_hal.h"
#include "milan/runtime.h"
#include "milan/match/match.h"
#include "milan/preprocess/state.h"

#define GM_MILAN_SENSOR_SUBTYPE 12	/* Milan profile 9 sensor (audit) */

struct gm_milan_state {
	GoodixMilanProfileState profile;
	GoodixMilanPreprocessState preprocess;
	GoodixMilanEnrollmentTransaction *txn;
	uint64_t epoch;
	/* Gallery for the current operation (duplicate detection on
	 * enroll, matching on identify). Pointers only: the caller
	 * (goodix_enroll / goodix_identify) owns the buffers for the
	 * duration of the operation. */
	const char *const *uids;
	const uint8_t *const *tpls;
	const size_t *tpl_lens;
	int n;
};

static struct gm_milan_state *gm_milan_state_get(struct goodix *s)
{
	struct gm_milan_state *st = (struct gm_milan_state *)s->milan_state;
	if (!st) {
		st = (struct gm_milan_state *)calloc(1, sizeof(*st));
		if (!st)
			return NULL;
		s->milan_state = st;
	}
	return st;
}

/* One runtime run. Returns the output (caller frees with
 * goodix_milan_runtime_output_free) or NULL on setup-admission failure
 * (*setup_failed = 1) or allocation failure (*setup_failed = 0).
 * The setup frame is copied: goodix_milan_runtime_initialize_setup
 * clamps/admits it in place (runtime.h, static inline). */
static GoodixMilanRuntimeOutput *gm_milan_run(struct goodix *s,
					       const uint16_t *setup,
					       const uint16_t *live,
					       GoodixMilanPreprocessPurpose purpose,
					       int *setup_failed)
{
	struct gm_milan_state *st = gm_milan_state_get(s);
	uint16_t setup_copy[GOODIX_MILAN_SENSOR_PIXELS];
	GoodixMilanRuntimeGalleryInput **gallery = NULL;
	GoodixMilanRuntimeInput *input = NULL;
	GoodixMilanRuntimeOutput *out = NULL;
	gint32 setup_status;
	int i;
	if (setup_failed)
		*setup_failed = 0;
	if (!st)
		return NULL;
	memcpy(setup_copy, setup, sizeof(setup_copy));
	setup_status = goodix_milan_runtime_initialize_setup(&st->profile, setup_copy);
	if (setup_status != 0) {
		GOODIX_LOGW("milan: setup admission failed: 0x%x", (unsigned)setup_status);
		if (setup_failed)
			*setup_failed = 1;
		return NULL;
	}
	if (st->n > 0) {
		gallery = (GoodixMilanRuntimeGalleryInput **)calloc((size_t)st->n,
								    sizeof(*gallery));
		if (!gallery)
			return NULL;
		for (i = 0; i < st->n; i++) {
			GBytes *b = g_bytes_new(st->tpls[i], st->tpl_lens[i]);
			if (!b)
				goto out_free_gallery;
			/* input_new takes a ref; drop ours. */
			gallery[i] = goodix_milan_runtime_gallery_input_new((guint)i, b);
			g_bytes_unref(b);
			if (!gallery[i])
				goto out_free_gallery;
		}
	}
	input = goodix_milan_runtime_input_new(st->epoch++, 0, purpose,
					       &st->preprocess, &st->profile,
					       setup_copy, live,
					       s->calib.tcode, s->calib.dac_h, s->calib.dac_l,
					       GM_MILAN_SENSOR_SUBTYPE,
					       (GoodixMilanRuntimeGalleryInput *const *)gallery,
					       (gsize)st->n);
	/* input_new copies the gallery entries; free ours now. */
out_free_gallery:
	if (gallery) {
		for (i = 0; i < st->n; i++)
			if (gallery[i])
				goodix_milan_runtime_gallery_input_free(gallery[i]);
		free(gallery);
	}
	if (!input)
		return NULL;
	out = goodix_milan_runtime_run(input);
	goodix_milan_runtime_input_free(input);
	return out;
}

int gm_matcher_check_setup(struct goodix *s, const uint16_t *setup)
{
	GoodixMilanProfileState tmp;
	uint16_t setup_copy[GOODIX_MILAN_SENSOR_PIXELS];
	(void)s;
	memset(&tmp, 0, sizeof(tmp));
	tmp.setup_refresh_pending = 1;
	memcpy(setup_copy, setup, sizeof(setup_copy));
	return goodix_milan_runtime_initialize_setup(&tmp, setup_copy) == 0 ? 0 : GOODIX_RETRY;
}

int gm_matcher_enroll_begin(struct goodix *s, const char *const *uids,
			    const uint8_t *const *tpls, const size_t *tpl_lens, int n)
{
	struct gm_milan_state *st = gm_milan_state_get(s);
	if (!st)
		return -ENOMEM;
	if (st->txn) {
		goodix_milan_enrollment_transaction_free(st->txn);
		st->txn = NULL;
	}
	st->txn = goodix_milan_enrollment_transaction_new();
	if (!st->txn)
		return -ENOMEM;
	st->uids = uids;
	st->tpls = tpls;
	st->tpl_lens = tpl_lens;
	st->n = n;
	/* New finger: force the next run to re-admit the setup frame
	 * instead of reusing the previous finger's retained setup map. */
	st->profile.setup_refresh_pending = 1;
	return 0;
}

int gm_matcher_enroll_sample(struct goodix *s, const uint16_t *setup,
			     const uint16_t *live, int *quality, int *coverage,
			     char *dup_uid)
{
	struct gm_milan_state *st = gm_milan_state_get(s);
	GoodixMilanRuntimeOutput *out;
	GoodixMilanEnrollmentAttemptStatus attempt;
	GoodixMilanEnrollmentResult result;
	guint bad_records = 0, bad_continue = 0;
	int setup_failed = 0;
	size_t i;
	if (!st || !st->txn)
		return -EINVAL;
	out = gm_milan_run(s, setup, live, GOODIX_MILAN_PURPOSE_ENROLL, &setup_failed);
	if (!out)
		return setup_failed ? GOODIX_RETRY : -ENOMEM;
	if (quality)
		*quality = out->quality;
	if (coverage)
		*coverage = out->coverage;
	/* Duplicate: the enroll run already matched the gallery. Any
	 * accepted gallery result means this finger is already enrolled. */
	if (out->gallery_results) {
		for (i = 0; i < out->gallery_results->len; i++) {
			GoodixMilanRuntimeGalleryResult *r =
				(GoodixMilanRuntimeGalleryResult *)
				g_ptr_array_index(out->gallery_results, i);
			if (r->accepted && r->gallery_index < (guint)st->n) {
				if (dup_uid && st->uids)
					snprintf(dup_uid, 33, "%s", st->uids[r->gallery_index]);
				GOODIX_LOGI("milan: enroll duplicate of gallery %u",
					    r->gallery_index);
				goodix_milan_runtime_output_free(out);
				return GOODIX_DUPLICATE;
			}
		}
	}
	if (!goodix_milan_runtime_enrollment_admitted(out)) {
		GOODIX_LOGI("milan: enroll sample not admitted (quality %d coverage %d)",
			    out->quality, out->coverage);
		goodix_milan_runtime_output_free(out);
		return GOODIX_RETRY;
	}
	attempt = goodix_milan_enrollment_transaction_attempt(&st->txn,
							      out->probe_template,
							      &bad_records, &bad_continue,
							      &result);
	goodix_milan_runtime_output_free(out);
	switch (attempt) {
	case GOODIX_MILAN_ENROLLMENT_ACCEPTED:
		return 0;
	case GOODIX_MILAN_ENROLLMENT_RETRY_CENTER:
	case GOODIX_MILAN_ENROLLMENT_RETRY_REMOVE:
		return GOODIX_RETRY;
	default:
		return -EIO;
	}
}

int gm_matcher_enroll_finish(struct goodix *s, uint8_t **tpl, size_t *tpl_len)
{
	struct gm_milan_state *st = (struct gm_milan_state *)s->milan_state;
	GBytes *b;
	size_t n;
	const uint8_t *data;
	if (!st || !st->txn)
		return -EINVAL;
	b = goodix_milan_enrollment_transaction_publish(st->txn);
	goodix_milan_enrollment_transaction_free(st->txn);
	st->txn = NULL;
	if (!b)
		return -EIO;
	data = (const uint8_t *)g_bytes_get_data(b, &n);
	*tpl = (uint8_t *)malloc(n ? n : 1);
	if (!*tpl) {
		g_bytes_unref(b);
		return -ENOMEM;
	}
	if (n)
		memcpy(*tpl, data, n);
	*tpl_len = n;
	g_bytes_unref(b);
	GOODIX_LOGI("milan: enroll finished, template %zu bytes", n);
	return 0;
}

int gm_matcher_match(struct goodix *s, const uint16_t *setup,
		     const uint16_t *live, const char *const *uids,
		     const uint8_t *const *tpls, const size_t *tpl_lens, int n,
		     int *winner)
{
	struct gm_milan_state *st = gm_milan_state_get(s);
	GoodixMilanRuntimeOutput *out;
	int setup_failed = 0, r;
	if (!st)
		return -ENOMEM;
	st->uids = uids;
	st->tpls = tpls;
	st->tpl_lens = tpl_lens;
	st->n = n;
	/* One-shot identify: re-admit the fresh setup frame. */
	st->profile.setup_refresh_pending = 1;
	out = gm_milan_run(s, setup, live, GOODIX_MILAN_PURPOSE_IDENTIFY, &setup_failed);
	if (!out)
		return setup_failed ? GOODIX_RETRY : -ENOMEM;
	switch (out->status) {
	case GOODIX_MILAN_RUNTIME_MATCH:
		if (winner)
			*winner = (int)out->winner_index;
		GOODIX_LOGI("milan: match winner %u score %d", out->winner_index,
			    (int)out->score);
		r = 0;
		break;
	case GOODIX_MILAN_RUNTIME_NO_MATCH:
		r = GOODIX_NOMATCH;
		break;
	case GOODIX_MILAN_RUNTIME_RETRY:
		r = GOODIX_RETRY;
		break;
	default:
		r = -EIO;
		break;
	}
	goodix_milan_runtime_output_free(out);
	return r;
}
