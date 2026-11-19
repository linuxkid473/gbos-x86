// SPDX-License-Identifier: Apache-2.0
/*
 * gbos_fp's log lines: liblog, and with --log a copy in a file. Needs, declared before inclusion:
 * __android_log_write, vsnprintf, snprintf, clock_gettime and CLOCK_REALTIME, localtime_r and struct tm,
 * getpid, gettid, write, memset. Included by gbos_fp.c (bionic) and fp_log_test.c (host).
 */
#ifndef GBOS_FP_LOG_H
#define GBOS_FP_LOG_H

/*
 * --log FILE: every line also goes to FILE (logs/boot-N/fingerprint.txt on the gbos volume), one O_APPEND
 * write(2) per line, in logcat's threadtime layout. Boot-14: `logcat -f` keeps its last partial 4 KB block in
 * memory, and init's SIGTERM at shutdown dropped it: fingerprint.txt was 4096 bytes, cut mid-line, and every
 * identify line was missing. A line written here is in the kernel's page cache at once.
 */
static int log_fd = -1;

static void fp_log(int prio, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void fp_log(int prio, const char *fmt, ...)
{
	static const char letters[] = "??VDIWEF";
	char msg[768], line[900];
	struct timespec ts;
	struct tm tm;
	va_list ap;
	int n;

	va_start(ap, fmt);
	vsnprintf(msg, sizeof(msg), fmt, ap);
	va_end(ap);
	__android_log_write(prio, "gbos_fp", msg);
	if (log_fd < 0)
		return;
	clock_gettime(CLOCK_REALTIME, &ts);
	if (!localtime_r(&ts.tv_sec, &tm))
		memset(&tm, 0, sizeof(tm));
	n = snprintf(line, sizeof(line), "%02d-%02d %02d:%02d:%02d.%03ld %5d %5d %c gbos_fp : %s\n",
		     tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec, ts.tv_nsec / 1000000,
		     getpid(), gettid(), prio >= 0 && prio < 8 ? letters[prio] : '?', msg);
	if (n > (int)sizeof(line) - 1) {
		n = (int)sizeof(line) - 1;
		line[n - 1] = '\n';
	}
	if (n > 0 && write(log_fd, line, (size_t)n) != n)
		return;	/* nothing better to do with a failed log write */
}
#endif
