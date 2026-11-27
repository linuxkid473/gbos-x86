/* milan_matcher_stub.c - matcher stub for builds without Milan.
 *
 * Links instead of milan_matcher.c when the Milan sources are not
 * wanted (or not available). The HAL still probes, registers and
 * reports the sensor; enroll/identify fail with
 * GM168SEC_ERR_NO_MATCHER. build.sh links exactly one of the two.
 */
#include "gm168sec_hal.h"

int gm_matcher_check_setup(struct goodix *s, const uint16_t *setup)
{
	(void)s; (void)setup;
	return 0;
}
int gm_matcher_enroll_begin(struct goodix *s, const char *const *uids,
			    const uint8_t *const *tpls, const size_t *tpl_lens, int n)
{
	(void)s; (void)uids; (void)tpls; (void)tpl_lens; (void)n;
	return GM168SEC_ERR_NO_MATCHER;
}
int gm_matcher_enroll_sample(struct goodix *s, const uint16_t *setup,
			     const uint16_t *live, int *quality, int *coverage,
			     char *dup_uid)
{
	(void)s; (void)setup; (void)live; (void)quality; (void)coverage; (void)dup_uid;
	return GM168SEC_ERR_NO_MATCHER;
}
int gm_matcher_enroll_finish(struct goodix *s, uint8_t **tpl, size_t *tpl_len)
{
	(void)s; (void)tpl; (void)tpl_len;
	return GM168SEC_ERR_NO_MATCHER;
}
int gm_matcher_match(struct goodix *s, const uint16_t *setup,
		     const uint16_t *live, const char *const *uids,
		     const uint8_t *const *tpls, const size_t *tpl_lens, int n,
		     int *winner)
{
	(void)s; (void)setup; (void)live; (void)uids; (void)tpls; (void)tpl_lens;
	(void)n; (void)winner;
	return GM168SEC_ERR_NO_MATCHER;
}
