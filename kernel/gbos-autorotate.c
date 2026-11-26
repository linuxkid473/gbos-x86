#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <errno.h>
#include <signal.h>
#include <time.h>
#include <math.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <linux/input.h>

static volatile sig_atomic_t run_ = 1;
static void on_sig(int s) { (void)s; run_ = 0; }

static void logf_(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
#include <stdarg.h>
static void logf_(const char *fmt, ...) {
	time_t t = time(NULL);
	char ts[32];
	strftime(ts, sizeof(ts), "%H:%M:%S", localtime(&t));
	printf("[%s] ", ts);
	va_list ap; va_start(ap, fmt); vprintf(fmt, ap); va_end(ap);
	putchar('\n');
}

static int read_int(const char *path, long *v) {
	char b[64];
	int fd = open(path, O_RDONLY);
	if (fd < 0) return -1;
	ssize_t n = read(fd, b, sizeof(b) - 1);
	close(fd);
	if (n <= 0) return -1;
	b[n] = 0;
	char *e;
	*v = strtol(b, &e, 10);
	return e == b ? -1 : 0;
}

static char accel_dir[300];

static int find_accel(void) {
	DIR *d = opendir("/sys/bus/iio/devices");
	if (!d) return -1;
	struct dirent *e;
	int found = -1;
	while ((e = readdir(d))) {
		if (strncmp(e->d_name, "iio:device", 10)) continue;
		char p[320], name[64] = "";
		snprintf(p, sizeof(p), "/sys/bus/iio/devices/%s/name", e->d_name);
		FILE *f = fopen(p, "r");
		if (!f) continue;
		if (fgets(name, sizeof(name), f)) name[strcspn(name, "\n")] = 0;
		fclose(f);
		snprintf(p, sizeof(p), "/sys/bus/iio/devices/%s/in_accel_x_raw", e->d_name);
		if (access(p, R_OK)) continue;

		if (found < 0 || !strcmp(name, "accel_3d")) {
			snprintf(accel_dir, sizeof(accel_dir), "/sys/bus/iio/devices/%s", e->d_name);
			found = 0;
		}
	}
	closedir(d);
	return found;
}

static int read_accel(double g[3]) {
	static const char ax[3] = { 'x', 'y', 'z' };
	for (int i = 0; i < 3; i++) {
		char p[340];
		long v;
		snprintf(p, sizeof(p), "%s/in_accel_%c_raw", accel_dir, ax[i]);
		if (read_int(p, &v)) return -1;
		g[i] = v;
	}
	double m = sqrt(g[0] * g[0] + g[1] * g[1] + g[2] * g[2]);
	if (m <= 0) return -1;
	for (int i = 0; i < 3; i++) g[i] /= m;
	return 0;
}

#define NBITS(x) ((((x) - 1) / (8 * sizeof(long))) + 1)
static int tablet_fd = -1;

static void find_tablet_switch(void) {
	for (int i = 0; i < 64; i++) {
		char p[32];
		snprintf(p, sizeof(p), "/dev/input/event%d", i);
		int fd = open(p, O_RDONLY | O_NONBLOCK);
		if (fd < 0) continue;
		unsigned long sw[NBITS(SW_MAX + 1)] = { 0 };
		if (ioctl(fd, EVIOCGBIT(EV_SW, sizeof(sw)), sw) >= 0 &&
		    (sw[SW_TABLET_MODE / (8 * sizeof(long))] >> (SW_TABLET_MODE % (8 * sizeof(long)))) & 1) {
			char name[128] = "";
			ioctl(fd, EVIOCGNAME(sizeof(name)), name);
			logf_("tablet-mode switch: %s (%s)", p, name);
			tablet_fd = fd;
			return;
		}
		close(fd);
	}
	logf_("no input device reports SW_TABLET_MODE");
}

static int tablet_mode(void) {
	if (tablet_fd < 0) return -1;
	unsigned long sw[NBITS(SW_MAX + 1)] = { 0 };
	if (ioctl(tablet_fd, EVIOCGSW(sizeof(sw)), sw) < 0) return -1;
	return (sw[SW_TABLET_MODE / (8 * sizeof(long))] >> (SW_TABLET_MODE % (8 * sizeof(long)))) & 1;
}

static int map_axis[4], map_sign[4];

static int parse_map(const char *s) {
	char buf[64];
	snprintf(buf, sizeof(buf), "%s", s);
	char *tok = strtok(buf, ",");
	for (int r = 0; r < 4; r++) {
		if (!tok || strlen(tok) != 2 || (tok[0] != '+' && tok[0] != '-') || tok[1] < 'x' || tok[1] > 'z') return -1;
		map_sign[r] = tok[0] == '+' ? 1 : -1;
		map_axis[r] = tok[1] - 'x';
		tok = strtok(NULL, ",");
	}
	return 0;
}

static int orientation(const double g[3]) {
	int best = -1;
	double bv = 0;
	for (int r = 0; r < 4; r++) {
		double v = map_sign[r] * g[map_axis[r]];
		if (v > bv) { bv = v; best = r; }
	}
	if (bv < 0.65) return -1;
	return best;
}

static int run_cmd(const char *const argv[]) {
	pid_t p = fork();
	if (p == 0) {
		int fd = open("/dev/null", O_RDWR);
		if (fd >= 0) { dup2(fd, 0); dup2(fd, 1); }
		execv(argv[0], (char *const *)argv);
		_exit(127);
	}
	if (p < 0) return -1;
	int st;
	if (waitpid(p, &st, 0) < 0) return -1;
	return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}

static int put_setting(const char *ns, const char *key, const char *val, int dry) {
	if (dry) { logf_("(dry run) %s %s = %s", ns, key, val); return 0; }
	const char *fast[] = { "/system/bin/cmd", "settings", "put", ns, key, val, NULL };
	if (access(fast[0], X_OK) == 0 && run_cmd(fast) == 0) return 0;
	const char *slow[] = { "/system/bin/settings", "put", ns, key, val, NULL };
	int r = run_cmd(slow);
	if (r) logf_("settings put %s %s %s failed (%d)", ns, key, val, r);
	return r;
}

static char kbd_serio[32];

static int write_str(const char *path, const char *v) {
	int fd = open(path, O_WRONLY);
	if (fd < 0) return -1;
	ssize_t n = write(fd, v, strlen(v));
	close(fd);
	return n == (ssize_t)strlen(v) ? 0 : -1;
}

static void internal_keyboard(int enable, int dry) {
	if (!kbd_serio[0]) {
		DIR *d = opendir("/sys/bus/serio/drivers/atkbd");
		if (!d) return;
		struct dirent *e;
		while ((e = readdir(d)))
			if (!strncmp(e->d_name, "serio", 5)) { snprintf(kbd_serio, sizeof(kbd_serio), "%.24s", e->d_name); break; }
		closedir(d);
		if (!kbd_serio[0]) return;
	}
	if (dry) { logf_("(dry run) internal keyboard %s %s", kbd_serio, enable ? "on" : "off"); return; }
	char link[96];
	snprintf(link, sizeof(link), "/sys/bus/serio/drivers/atkbd/%s", kbd_serio);
	int bound = access(link, F_OK) == 0;
	if (enable == bound) return;
	int r = write_str(enable ? "/sys/bus/serio/drivers/atkbd/bind" : "/sys/bus/serio/drivers/atkbd/unbind", kbd_serio);
	logf_("internal keyboard %s %s%s", kbd_serio, enable ? "on" : "off", r ? " FAILED" : "");
}

static int set_rotation(int r, int dry) {
	char v[8];
	snprintf(v, sizeof(v), "%d", r);
	if (put_setting("system", "user_rotation", v, dry)) return -1;
	logf_("rotation -> %d", r);
	return 0;
}

int main(int argc, char **argv) {
	const char *mode = "tablet", *map = "-y,-x,+y,+x";
	int hold_ms = 500, dry = 0, read_once = 0, ime = 1;
	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--dry-run")) dry = 1;
		else if (!strcmp(argv[i], "--read")) read_once = 1;
		else if (!strcmp(argv[i], "--no-ime")) ime = 0;
		else if (i + 1 < argc && !strcmp(argv[i], "--iio")) snprintf(accel_dir, sizeof(accel_dir), "%s", argv[++i]);
		else if (i + 1 < argc && !strcmp(argv[i], "--mode")) mode = argv[++i];
		else if (i + 1 < argc && !strcmp(argv[i], "--map")) map = argv[++i];
		else if (i + 1 < argc && !strcmp(argv[i], "--hold")) hold_ms = atoi(argv[++i]);
		else { fprintf(stderr, "usage: gbos-autorotate [--mode tablet|always|off] [--map -y,-x,+y,+x] [--hold ms] [--no-ime] [--dry-run] [--read]\n"); return 64; }
	}
	setvbuf(stdout, NULL, _IOLBF, 0);
	if (parse_map(map)) { fprintf(stderr, "bad --map '%s' (four of +x -x +y -y +z -z, comma separated)\n", map); return 64; }
	if (!accel_dir[0] && find_accel()) { logf_("no IIO accelerometer (in_accel_x_raw): nothing to do"); return 0; }
	find_tablet_switch();
	if (read_once) {
		double g[3];
		if (read_accel(g)) { printf("accel read failed\n"); return 1; }
		printf("accel %s: x=%+.2f y=%+.2f z=%+.2f -> rotation %d (map %s), tablet_mode=%d\n", accel_dir, g[0], g[1], g[2],
		       orientation(g), map, tablet_mode());
		return 0;
	}
	signal(SIGTERM, on_sig); signal(SIGINT, on_sig);
	signal(SIGHUP, SIG_IGN);
	int always = !strcmp(mode, "always"), off = !strcmp(mode, "off");
	logf_("accelerometer %s, mode %s, map %s, hold %d ms", accel_dir, mode, map, hold_ms);

	int applied = -2, cand = -1, cand_ms = 0, last_tab = -2, fails = 0, logged = 0;
	const int step_ms = 100;
	if (!off) put_setting("system", "accelerometer_rotation", "0", dry);
	while (run_) {
		int tab = tablet_mode();
		if (tab != last_tab) {
			logf_("tablet mode %d", tab);
			if (ime && tab >= 0) {
				internal_keyboard(!tab, dry);
				put_setting("secure", "show_ime_with_hard_keyboard", tab ? "1" : "0", dry);
			}
			last_tab = tab;
		}
		int want, measured = 0;
		double g[3] = { 0, 0, 0 };
		if (off) want = -1;
		else if (!always && tab == 0) want = 0;
		else if (read_accel(g) == 0) { want = orientation(g); fails = 0; measured = 1; }
		else { if (++fails == 20) logf_("accelerometer reads failing"); want = -1; }
		if (measured && logged < 8 && want >= 0 && want != applied) {
			logf_("gravity x=%+.2f y=%+.2f z=%+.2f -> %d", g[0], g[1], g[2], want);
			logged++;
		}
		if (want >= 0 && want != applied) {
			if (want != cand) { cand = want; cand_ms = 0; }
			else if ((cand_ms += step_ms) >= hold_ms && set_rotation(want, dry) == 0) applied = want;
		} else cand = -1;
		usleep(step_ms * 1000);
	}
	if (ime) internal_keyboard(1, dry);
	return 0;
}
