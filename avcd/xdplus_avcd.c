/*
 * xdplus_avcd — record SELinux denials where the GPD XD+ menu can read them.
 *
 * Reads the kernel ring buffer incrementally via /dev/kmsg, filters for
 * "avc: ... denied" records, and writes them (with a wall-clock stamp) to
 * /data/misc/xdplus/avc-denials.log. The log is world-readable because the
 * Settings page that displays it runs as system, not root.
 *
 * This is a single native process so no shell pipeline is needed: under
 * SELinux enforcement a pipe would need a fifo_file rule from toolbox to this
 * domain, which is the wrong shape for a denial recorder.
 */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <sys/stat.h>
#include <sys/types.h>

#define LOGDIR "/data/misc/xdplus"
#define LOGPATH LOGDIR "/avc-denials.log"
#define LOGPATH_1 LOGPATH ".1"
#define MAXBYTES (1 * 1024 * 1024)

static int open_log(void)
{
	int fd;

	if (mkdir(LOGDIR, 0755) == -1 && errno != EEXIST)
		return -1;
	if (chmod(LOGDIR, 0755) == -1)
		return -1;

	fd = open(LOGPATH, O_WRONLY | O_APPEND | O_CREAT, 0644);
	if (fd == -1)
		return -1;
	if (fchmod(fd, 0644) == -1) {
		close(fd);
		return -1;
	}
	return fd;
}

static int rotate_if_large(int fd)
{
	struct stat st;
	int newfd;

	if (fstat(fd, &st) == -1)
		return fd;
	if (st.st_size <= MAXBYTES)
		return fd;

	if (rename(LOGPATH, LOGPATH_1) == -1)
		return fd;

	newfd = open_log();
	if (newfd == -1)
		return fd;

	close(fd);
	return newfd;
}

static size_t format_stamp(const char *fmt, char *out, size_t out_size)
{
	time_t now;
	struct tm tm;

	time(&now);
	localtime_r(&now, &tm);
	return strftime(out, out_size, fmt, &tm);
}

static void write_line(int fd, const char *line, size_t len)
{
	char stamp[32];
	size_t stamp_len;

	stamp_len = format_stamp("%m-%d %H:%M:%S ", stamp, sizeof(stamp));
	if (stamp_len == 0)
		stamp_len = 0;

	write(fd, stamp, stamp_len);
	write(fd, line, len);
	if (!len || line[len - 1] != '\n')
		write(fd, "\n", 1);
}

static const char *strip_kmsg_prefix(const char *line)
{
	const char *p = strchr(line, ';');
	if (p) {
		p++;
		if (*p == ' ')
			p++;
		return p;
	}
	return line;
}

static int is_avc_denial(const char *line)
{
	const char *body = strip_kmsg_prefix(line);
	return strstr(body, "avc: ") && strstr(body, "denied");
}

static const char *current_mode(void)
{
	static char buf[16];
	ssize_t n;
	int fd;

	fd = open("/sys/fs/selinux/enforce", O_RDONLY);
	if (fd == -1)
		return "unknown";
	n = read(fd, buf, sizeof(buf) - 1);
	close(fd);
	if (n <= 0)
		return "unknown";
	buf[n] = '\0';
	if (buf[n - 1] == '\n')
		buf[n - 1] = '\0';
	if (strcmp(buf, "1") == 0)
		return "Enforcing";
	if (strcmp(buf, "0") == 0)
		return "Permissive";
	return buf;
}

int main(void)
{
	int logfd, kmsg;
	char buf[8192];
	size_t filled = 0;

	logfd = open_log();
	if (logfd == -1)
		return 1;

	{
		char stamp[32];
		format_stamp("%Y-%m-%d %H:%M:%S", stamp, sizeof(stamp));
		dprintf(logfd, "--- xdplus_avcd started %s enforce=%s\n",
			stamp, current_mode());
	}

	kmsg = open("/dev/kmsg", O_RDONLY | O_CLOEXEC);
	if (kmsg == -1)
		return 1;

	/*
	 * /dev/kmsg returns one record per read when no bytes are buffered,
	 * but records can be larger than a single read. Read into a buffer and
	 * split on newlines; incomplete trailing data is kept for the next read.
	 */
	for (;;) {
		ssize_t n;
		char *p, *end, *newline;

		n = read(kmsg, buf + filled, sizeof(buf) - 1 - filled);
		if (n == -1) {
			if (errno == EINTR)
				continue;
			sleep(1);
			continue;
		}
		if (n == 0) {
			sleep(1);
			continue;
		}
		filled += (size_t)n;
		buf[filled] = '\0';

		p = buf;
		end = buf + filled;
		while ((newline = memchr(p, '\n', end - p)) != NULL) {
			*newline = '\0';
			if (is_avc_denial(p)) {
				const char *body = strip_kmsg_prefix(p);
				logfd = rotate_if_large(logfd);
				if (logfd != -1)
					write_line(logfd, body, newline - body);
			}
			p = newline + 1;
		}

		if (p != buf) {
			filled = end - p;
			memmove(buf, p, filled);
		}
	}
}
