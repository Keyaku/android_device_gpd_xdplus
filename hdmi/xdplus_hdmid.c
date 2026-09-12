/* xdplus_hdmid — event-driven mini-HDMI plug/unplug watcher for GPD XD+.
 *
 * Runs the same bring-up/teardown protocol as the Settings buttons, but
 * automatically on cable plug and unplug.
 *
 * Why netlink and not a shell loop polling /sys/class/switch/hdmi/state:
 * a poll loop costs a wakeup every interval for the entire uptime, which is
 * not affordable against this device's 0.338 %/h standby floor. This daemon
 * blocks in recv() and costs nothing at all while idle.
 *
 * Why not poll() on the sysfs attribute instead of netlink: the android
 * switch class (drivers/switch/switch_class.c, switch_set_state) only calls
 * kobject_uevent_env(KOBJ_CHANGE, {SWITCH_NAME=..., SWITCH_STATE=...}) and
 * never sysfs_notify(), so the `state` attribute never wakes a poller. The
 * uevent is the only edge userspace can actually sleep on.
 *
 * The uevent carries SWITCH_STATE, but it is used purely as a wakeup: after
 * coalescing a burst the daemon re-reads the sysfs attribute and acts on that.
 * A cable can bounce several times in a second, and the last uevent in a burst
 * is not reliably the final state, whereas the attribute always is.
 */
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <linux/netlink.h>
#include <pthread.h>
#include <sys/system_properties.h>

#define LOG_TAG "xdplus_hdmid"
#include <log/log.h>

#define HDMI_STATE_PATH		"/sys/class/switch/hdmi/state"
#define TWEAKS_PATH		"/system/bin/xdplus_tweaks"

/* enum HDMI_STATE in the kernel's hdmi_drv.h. hdmi_switch_data only ever
 * carries these two; anything else means the driver changed under us.
 */
#define HDMI_NO_DEVICE		0
#define HDMI_ACTIVE		1

/* Coalesce a plug bounce into one action. */
#define DEBOUNCE_MS		1000

/* Sleep timeout: SurfaceFlinger publishes the built-in screen's power state
 * in POWER_PROP ("0" asleep, "1" awake); the external's own state is useless
 * here because a torn-down external has no display left to report the wake.
 * SLEEP_PROP holds the minutes asleep
 * after which the HDMI output is switched off; 0 leaves it on, showing black.
 * A torn-down output is rebuilt on wake through the same cable-event path as
 * a replug: the timer thread only pokes hdmictl, and the resulting switch
 * uevents drive the main loop, so hdmi_up/hdmi_down never run concurrently.
 */
#define POWER_PROP		"sys.xdplus.screen_power"
#define SLEEP_PROP		"persist.sys.xdplus.hdmi_sleep"
#define HDMICTL_PATH		"/system/bin/hdmictl"

/* Serialises the dispatcher runs of the main loop against the timer thread. */
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
/* Set while the timer, not the cable, took the output down. Under g_lock. */
static int g_timer_torn;

static int read_hdmi_state(void)
{
	char buf[16];
	int fd, n;

	fd = open(HDMI_STATE_PATH, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return -1;

	n = read(fd, buf, sizeof(buf) - 1);
	close(fd);
	if (n <= 0)
		return -1;

	buf[n] = '\0';
	return atoi(buf);
}

/* Run xdplus_tweaks synchronously. Bring-up takes seconds; further uevents
 * queue in the socket buffer meanwhile and are resolved by the sysfs re-read
 * on the next pass, so blocking here cannot lose a transition.
 */
static int run_child(const char *path, const char *arg0, const char *arg1)
{
	pid_t pid;
	int status = 0;

	pid = fork();
	if (pid < 0) {
		ALOGE("fork failed: %s", strerror(errno));
		return -1;
	}
	if (pid == 0) {
		execl(path, path, arg0, arg1, (char *)NULL);
		ALOGE("execl %s failed: %s", path, strerror(errno));
		_exit(127);
	}
	if (TEMP_FAILURE_RETRY(waitpid(pid, &status, 0)) < 0) {
		ALOGE("waitpid failed: %s", strerror(errno));
		return -1;
	}
	return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

static int prop_int(const char *name)
{
	char v[PROP_VALUE_MAX];

	if (__system_property_get(name, v) <= 0)
		return -1;
	return atoi(v);
}

/* Timer thread: sleeps on the power property, never polls. */
static void *sleep_timer(void *unused)
{
	const prop_info *pi = NULL;
	uint32_t serial = 0;

	(void)unused;
	for (;;) {
		int power, mins;

		if (pi == NULL) {
			/* The property does not exist until SurfaceFlinger first
			 * sets it; wait on the global serial until it does.
			 */
			pi = __system_property_find(POWER_PROP);
			if (pi == NULL) {
				__system_property_wait(NULL, serial, &serial, NULL);
				continue;
			}
			serial = 0;
		}

		power = prop_int(POWER_PROP);
		if (power == 0) {
			mins = prop_int(SLEEP_PROP);
			if (mins > 0) {
				struct timespec to = { .tv_sec = mins * 60, .tv_nsec = 0 };
				uint32_t next = serial;

				if (__system_property_wait(pi, serial, &next, &to)) {
					serial = next;	/* changed before the timeout */
					continue;
				}
				pthread_mutex_lock(&g_lock);
				if (prop_int(POWER_PROP) == 0 &&
				    read_hdmi_state() == HDMI_ACTIVE) {
					ALOGI("asleep %d min with HDMI up: switching the output off", mins);
					run_child(HDMICTL_PATH, "disable", NULL);
					g_timer_torn = 1;
				}
				pthread_mutex_unlock(&g_lock);
			}
		} else if (power == 1) {
			pthread_mutex_lock(&g_lock);
			if (g_timer_torn) {
				ALOGI("awake: switching the HDMI output back on");
				g_timer_torn = 0;
				run_child(HDMICTL_PATH, "power", "1");
				run_child(HDMICTL_PATH, "enable", NULL);
			}
			pthread_mutex_unlock(&g_lock);
		}

		__system_property_wait(pi, serial, &serial, NULL);
	}
	return NULL;
}

static void run_tweaks(const char *action)
{
	pid_t pid;
	int status = 0;

	ALOGI("HDMI %s: running %s %s", action, TWEAKS_PATH, action);

	pthread_mutex_lock(&g_lock);
	pid = fork();
	if (pid < 0) {
		ALOGE("fork failed: %s", strerror(errno));
		pthread_mutex_unlock(&g_lock);
		return;
	}

	if (pid == 0) {
		/* Exec the dispatcher itself rather than `sh <script>`: the
		 * exec target is what SELinux transitions on, and going
		 * through the shell would leave the bring-up running in this
		 * daemon's own domain.
		 */
		execl(TWEAKS_PATH, "xdplus_tweaks", action, (char *)NULL);
		ALOGE("execl failed: %s", strerror(errno));
		_exit(127);
	}

	if (TEMP_FAILURE_RETRY(waitpid(pid, &status, 0)) < 0)
		ALOGE("waitpid failed: %s", strerror(errno));
	else if (WIFEXITED(status) && WEXITSTATUS(status) != 0)
		ALOGW("%s exited %d", action, WEXITSTATUS(status));
	pthread_mutex_unlock(&g_lock);
}

/* True if this uevent belongs to the hdmi switch. The buffer is a run of
 * NUL-separated KEY=VALUE records; SWITCH_STATE is deliberately ignored.
 */
static int is_hdmi_switch_event(const char *buf, ssize_t len)
{
	ssize_t i = 0;

	while (i < len) {
		const char *rec = buf + i;
		size_t reclen = strnlen(rec, (size_t)(len - i));

		if (!strcmp(rec, "SWITCH_NAME=hdmi"))
			return 1;

		i += (ssize_t)reclen + 1;
	}

	return 0;
}

/* True if the record set carries SWITCH_STATE=0. The composer acts on every
 * uevent, so a plug-out inside a burst tears the external down even when
 * the burst ends plugged; the daemon has to notice it to bring it back.
 */
static int has_state_zero(const char *buf, ssize_t len)
{
	ssize_t i = 0;

	while (i < len) {
		const char *rec = buf + i;
		size_t reclen = strnlen(rec, (size_t)(len - i));

		if (!strcmp(rec, "SWITCH_STATE=0"))
			return 1;

		i += (ssize_t)reclen + 1;
	}

	return 0;
}

int main(void)
{
	struct sockaddr_nl addr;
	int sock, last_state;
	/* A uevent is bounded by one page in the kernel; this is comfortably
	 * larger so a record can never be truncated across reads.
	 */
	char buf[8192];

	memset(&addr, 0, sizeof(addr));
	addr.nl_family = AF_NETLINK;
	addr.nl_pid = 0;	/* let the kernel assign, so restarts never clash */
	addr.nl_groups = 1;	/* the uevent broadcast group */

	sock = socket(AF_NETLINK, SOCK_DGRAM | SOCK_CLOEXEC, NETLINK_KOBJECT_UEVENT);
	if (sock < 0) {
		ALOGE("netlink socket: %s", strerror(errno));
		return 1;
	}

	if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		ALOGE("netlink bind: %s", strerror(errno));
		close(sock);
		return 1;
	}

	/* Act on the state the device is already in. init starts this after
	 * boot_completed, so a cable present at boot still gets a mirror.
	 */
	{
		pthread_t t;
		if (pthread_create(&t, NULL, sleep_timer, NULL) == 0)
			pthread_detach(t);
		else
			ALOGE("sleep timer thread: %s", strerror(errno));
	}

	last_state = read_hdmi_state();
	ALOGI("started, hdmi switch state=%d", last_state);
	if (last_state == HDMI_ACTIVE)
		run_tweaks("hdmi_up");

	for (;;) {
		struct pollfd pfd = { .fd = sock, .events = POLLIN };
		ssize_t n;
		int state;
		int ret;
		int bounced;

		/* Block indefinitely: no wakeup until the kernel has something. */
		ret = TEMP_FAILURE_RETRY(poll(&pfd, 1, -1));
		if (ret < 0) {
			ALOGE("poll: %s", strerror(errno));
			break;
		}

		n = TEMP_FAILURE_RETRY(recv(sock, buf, sizeof(buf), 0));
		if (n <= 0)
			continue;

		if (!is_hdmi_switch_event(buf, n))
			continue;

		/* Drain the rest of the burst before reading the attribute, so a
		 * bouncing cable produces one action rather than one per bounce.
		 * Remember whether any record in it was a plug-out: the composer
		 * tears the external down on that, and only hdmi_up rebuilds it.
		 */
		bounced = has_state_zero(buf, n);
		for (;;) {
			ret = TEMP_FAILURE_RETRY(poll(&pfd, 1, DEBOUNCE_MS));
			if (ret <= 0)
				break;
			n = TEMP_FAILURE_RETRY(recv(sock, buf, sizeof(buf), 0));
			if (n <= 0)
				break;
			if (is_hdmi_switch_event(buf, n) && has_state_zero(buf, n))
				bounced = 1;
		}

		state = read_hdmi_state();
		if (state < 0) {
			ALOGE("cannot read %s: %s", HDMI_STATE_PATH, strerror(errno));
			continue;
		}

		if (state == last_state) {
			if (state == HDMI_ACTIVE && bounced) {
				ALOGI("hdmi bounced while plugged, rebuilding the mirror");
				run_tweaks("hdmi_up");
			}
			continue;
		}

		ALOGI("hdmi switch state %d -> %d", last_state, state);
		last_state = state;

		if (state == HDMI_ACTIVE)
			run_tweaks("hdmi_up");
		else if (state == HDMI_NO_DEVICE)
			run_tweaks("hdmi_down");
		else
			ALOGW("unexpected hdmi switch state %d, ignoring", state);
	}

	close(sock);
	return 1;
}
