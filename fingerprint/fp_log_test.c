// SPDX-License-Identifier: Apache-2.0
/*
 * Host test for fp_log.h: every line must be complete in the file the moment fp_log returns, so init's SIGTERM
 * at shutdown cannot drop it (boot-14: `logcat -f` lost its last 4 KB block). A buffered stdio writer, killed
 * the same way, is the negative control.
 */
#define _GNU_SOURCE
#include <assert.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static int android_lines;
int __android_log_write(int prio, const char *tag, const char *text)
{
	(void)prio; (void)tag; (void)text;
	return ++android_lines;
}

#include "fp_log.h"

static char path[] = "/tmp/fplog.XXXXXX";

static size_t slurp(char *buf, size_t size)
{
	int fd = open(path, O_RDONLY);
	ssize_t n;

	assert(fd >= 0);
	n = read(fd, buf, size - 1);
	close(fd);
	assert(n >= 0);
	buf[n] = 0;
	return (size_t)n;
}

static void killed_child(int stdio)
{
	pid_t pid = fork();
	int status;

	assert(pid >= 0);
	if (pid == 0) {
		if (stdio) {
			FILE *f = fopen(path, "a");

			for (int i = 1; i <= 3; i++)
				fprintf(f, "stdio line %d\n", i);
		} else {
			log_fd = open(path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
			for (int i = 1; i <= 3; i++)
				fp_log(4, "identify: status 0x00, score %d", 27 * i);
			fp_log(5, "%s", "a warning");
		}
		signal(SIGTERM, SIG_DFL);
		raise(SIGTERM);		/* what init sends at shutdown: no exit handlers, no flush */
		_exit(1);
	}
	assert(waitpid(pid, &status, 0) == pid && WIFSIGNALED(status) && WTERMSIG(status) == SIGTERM);
}

int main(void)
{
	char buf[8192], big[2000];
	size_t n;
	int fd = mkstemp(path);

	assert(fd >= 0);
	close(fd);

	/* fp_log: all four lines, complete, in logcat's threadtime layout */
	killed_child(0);
	n = slurp(buf, sizeof(buf));
	assert(n > 0 && buf[n - 1] == '\n');
	assert(strstr(buf, " I gbos_fp : identify: status 0x00, score 27\n"));
	assert(strstr(buf, " I gbos_fp : identify: status 0x00, score 81\n"));
	assert(strstr(buf, " W gbos_fp : a warning\n"));
	assert(buf[2] == '-' && buf[5] == ' ' && buf[8] == ':' && buf[14] == '.');	/* MM-DD HH:MM:SS.mmm */

	/* negative control: the same kill loses a buffered writer's lines */
	assert(truncate(path, 0) == 0);
	killed_child(1);
	assert(slurp(buf, sizeof(buf)) == 0);

	/* an over-long message is cut but still ends its line; without --log only liblog gets it */
	assert(truncate(path, 0) == 0);
	log_fd = open(path, O_WRONLY | O_APPEND);
	memset(big, 'x', sizeof(big) - 1);
	big[sizeof(big) - 1] = 0;
	fp_log(6, "%s", big);
	close(log_fd);
	log_fd = -1;
	android_lines = 0;
	fp_log(4, "not in the file");
	n = slurp(buf, sizeof(buf));
	assert(n < 900 && buf[n - 1] == '\n' && strstr(buf, " E gbos_fp : xxx") && android_lines == 1);
	unlink(path);
	puts("FP_LOG_TEST_PASS");
	return 0;
}
