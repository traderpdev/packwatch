/*
 * packwatch — one camera, one recording.
 *
 * rc.d starts a FreeBSD daemon(8) --restart supervisor per camera that
 * should be recording. This process opens one MPEG-TS at its final path
 * and exits on SIGTERM. A crash or a stall exits non-zero. daemon starts
 * a new process, and that process opens a new start-named file. The old
 * file stays where it is. Nothing in this program renames, moves, or
 * copies a recording.
 *
 * The behavior matches the bench command:
 *
 *   ffmpeg -rtsp_transport tcp -use_wallclock_as_timestamps 1 \
 *     -i rtsp://USER:PASS@CAMERA:554/h264Preview_01_sub \
 *     -map 0:v:0 -an -c copy -f mpegts \
 *     /tank/video/<camera-id>/YYYYMMDDTHHMMSSZ.ts
 *
 * Linked against a private static FFmpeg with the 7.1 API: the RTSP
 * socket option is "timeout" and its unit is microseconds. The signal
 * handler only stores a flag. libavformat is called from the main thread.
 *
 * Exit status:
 *   0  SIGTERM, or a second start while this camera is already recording
 *   2  bad arguments or config
 *   3  NTP not synced; no file opened
 *   4  stream is not 640x480 H.264; no file opened
 *   5  output directory or file could not be prepared
 *   6  disk full; supervisor was signaled when the pidfile says it can be
 *   7  camera stalled or RTSP failed; daemon restarts
 *   8  mux error; daemon restarts
 */

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <time.h>
#include <unistd.h>

#include <sys/file.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/timex.h>
#include <netinet/in.h>

#include <libavcodec/avcodec.h>
#include <libavcodec/bsf.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/dict.h>
#include <libavutil/error.h>
#include <libavutil/mem.h>
#include <libavutil/time.h>

#define PW_OK      0
#define PW_USAGE   2
#define PW_NTP     3
#define PW_STREAM  4
#define PW_IO      5
#define PW_NOSPC   6
#define PW_STALL   7
#define PW_MUX     8

#define PW_WIDTH   640
#define PW_HEIGHT  480
#define PW_FAULT_SEC (8 * 60 * 60)
#define PW_IO_BUF  (32 * 1024)
#define PW_URL_MAX 2048
#define PW_LINE_MAX 1024
#define PW_RUN_DIR "/var/run/packwatch"

enum {
	PW_ID_MAX = 64,
	PW_USER_MAX = 64,
	PW_PASS_MAX = 128,
	PW_STREAM_PATH_MAX = 256,
	PW_ADDR_MAX = 16
};

struct config {
	char camera_id[PW_ID_MAX];
	char ip[PW_ADDR_MAX];
	int port;
	char stream_path[PW_STREAM_PATH_MAX];
	char user[PW_USER_MAX];
	char password[PW_PASS_MAX];
	char video_root[PATH_MAX];
	char supervisor_pidfile[PATH_MAX];
	int read_timeout_sec;
	int open_timeout_sec;
	int failure_delay_sec;
};

struct io_guard {
	int64_t deadline_us;
	volatile sig_atomic_t armed;
};

struct output {
	AVFormatContext *fmt;
	uint8_t *buf;
	int fd;
	int header_ok;
	int64_t origin_pts;
	int64_t start_pts;
	int64_t last_dts;
	char path[PATH_MAX];
};

static volatile sig_atomic_t stop_requested;
static struct io_guard io_guard;
static const char *g_camera_id = "packwatch";
static int g_failure_delay = 10;
static int g_lock_fd = -1;

static char av_line[1024];
static size_t av_line_len;

static void
on_signal(int signo)
{
	(void)signo;
	stop_requested = 1;
}

static void
install_signals(void)
{
	struct sigaction sa;

	memset(&sa, 0, sizeof sa);
	sa.sa_handler = on_signal;
	sigemptyset(&sa.sa_mask);
	sigaction(SIGTERM, &sa, NULL);
	sigaction(SIGINT, &sa, NULL);
	signal(SIGPIPE, SIG_IGN);
}

static void
pwlog(int pri, const char *fmt, ...)
{
	char msg[1024];
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(msg, sizeof msg, fmt, ap);
	va_end(ap);
	syslog(pri, "%s", msg);
}

/* av_log often prints an RTSP URL. Keep the password out of syslog. */
static void
redact(char *s)
{
	char *p = s;

	while ((p = strstr(p, "rtsp://")) != NULL) {
		char *at = strchr(p + 7, '@');
		char *slash = strchr(p + 7, '/');
		size_t tail;

		if (at == NULL || (slash != NULL && slash < at)) {
			p += 7;
			continue;
		}
		tail = strlen(at);
		memmove(p + 10, at, tail + 1);
		memcpy(p, "rtsp://***", 10);
		p += 10;
	}
}

static void
flush_av_line(int level)
{
	int pri;

	if (av_line_len == 0)
		return;
	av_line[av_line_len] = '\0';
	redact(av_line);
	if (level <= AV_LOG_ERROR)
		pri = LOG_ERR;
	else if (level <= AV_LOG_WARNING)
		pri = LOG_WARNING;
	else
		pri = LOG_INFO;
	syslog(pri, "%s: %s", g_camera_id, av_line);
	av_line_len = 0;
}

static void
av_to_syslog(void *avcl, int level, const char *fmt, va_list ap)
{
	char piece[512];
	size_t i;

	(void)avcl;
	if (level > av_log_get_level())
		return;
	vsnprintf(piece, sizeof piece, fmt, ap);
	for (i = 0; piece[i] != '\0'; i++) {
		if (piece[i] == '\n' || piece[i] == '\r') {
			flush_av_line(level);
			continue;
		}
		if (av_line_len + 1 < sizeof av_line)
			av_line[av_line_len++] = piece[i];
	}
}

static int
io_interrupt(void *opaque)
{
	struct io_guard *g = opaque;

	if (stop_requested)
		return 1;
	if (g->armed && av_gettime_relative() > g->deadline_us)
		return 1;
	return 0;
}

static void
arm_io(int seconds)
{
	io_guard.deadline_us = av_gettime_relative() + (int64_t)seconds * 1000000;
	io_guard.armed = 1;
}

static void
disarm_io(void)
{
	io_guard.armed = 0;
}

static void
delay_unless_stopping(int seconds)
{
	while (seconds > 0 && !stop_requested) {
		struct timespec ts;

		ts.tv_sec = 1;
		ts.tv_nsec = 0;
		nanosleep(&ts, NULL);
		seconds--;
	}
}

static int
finish(int status)
{
	if (status == PW_NOSPC)
		return PW_NOSPC;
	if (stop_requested)
		return PW_OK;
	if (status != PW_OK)
		delay_unless_stopping(g_failure_delay);
	if (stop_requested)
		return PW_OK;
	return status;
}

static void
config_scrub(struct config *cfg)
{
	explicit_bzero(cfg->password, sizeof cfg->password);
	explicit_bzero(cfg->user, sizeof cfg->user);
}

static const char *
skip_space(const char *s)
{
	while (*s == ' ' || *s == '\t')
		s++;
	return s;
}

static void
trim_end(char *s)
{
	size_t n = strlen(s);

	while (n > 0 && (s[n - 1] == ' ' || s[n - 1] == '\t' ||
	    s[n - 1] == '\n' || s[n - 1] == '\r')) {
		s[--n] = '\0';
	}
}

static int
copy_field(char *dst, size_t dstsz, const char *src, const char *what)
{
	size_t n = strlen(src);

	if (n == 0 || n >= dstsz) {
		pwlog(LOG_ERR, "%s: %s is missing or too long", g_camera_id, what);
		return -1;
	}
	memcpy(dst, src, n + 1);
	return 0;
}

static int
parse_int(const char *s, int min, int max, int *out)
{
	char *end;
	long v;

	errno = 0;
	v = strtol(s, &end, 10);
	if (s[0] == '\0' || *end != '\0' || errno != 0 || v < min || v > max)
		return -1;
	*out = (int)v;
	return 0;
}

static int
id_ok(const char *s)
{
	size_t i;

	if (s[0] == '\0' || strcmp(s, ".") == 0 || strcmp(s, "..") == 0)
		return 0;
	for (i = 0; s[i] != '\0'; i++) {
		char c = s[i];

		if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
		    (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-'))
			return 0;
	}
	return 1;
}

static int
stream_path_ok(const char *s)
{
	size_t i;

	if (s[0] == '\0' || strstr(s, "://") != NULL || strstr(s, "..") != NULL)
		return 0;
	for (i = 0; s[i] != '\0'; i++) {
		char c = s[i];

		if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
		    (c >= '0' && c <= '9') || c == '.' || c == '_' ||
		    c == '-' || c == '/'))
			return 0;
	}
	return 1;
}

static int
root_ok(const char *s)
{
	if (s[0] != '/')
		return 0;
	if (strstr(s, "/../") != NULL || strcmp(s, "/..") == 0)
		return 0;
	if (strlen(s) >= 3 && strcmp(s + strlen(s) - 3, "/..") == 0)
		return 0;
	return 1;
}

static int
apply_key(struct config *cfg, const char *key, const char *val, int *seen)
{
	if (strcmp(key, "camera_id") == 0) {
		if (copy_field(cfg->camera_id, sizeof cfg->camera_id, val, key) < 0)
			return -1;
		if (!id_ok(cfg->camera_id)) {
			pwlog(LOG_ERR, "%s: camera_id must be a single path token",
			    g_camera_id);
			return -1;
		}
		*seen |= 1;
		g_camera_id = cfg->camera_id;
		return 0;
	}
	if (strcmp(key, "ip") == 0) {
		unsigned char buf[4];

		if (inet_pton(AF_INET, val, buf) != 1) {
			pwlog(LOG_ERR, "%s: ip must be a numeric IPv4 address",
			    g_camera_id);
			return -1;
		}
		if (copy_field(cfg->ip, sizeof cfg->ip, val, key) < 0)
			return -1;
		*seen |= 2;
		return 0;
	}
	if (strcmp(key, "port") == 0) {
		if (parse_int(val, 1, 65535, &cfg->port) < 0) {
			pwlog(LOG_ERR, "%s: port must be 1-65535", g_camera_id);
			return -1;
		}
		return 0;
	}
	if (strcmp(key, "path") == 0) {
		if (copy_field(cfg->stream_path, sizeof cfg->stream_path, val, key) < 0)
			return -1;
		if (!stream_path_ok(cfg->stream_path)) {
			pwlog(LOG_ERR, "%s: path has characters this recorder will not put in a URL",
			    g_camera_id);
			return -1;
		}
		*seen |= 4;
		return 0;
	}
	if (strcmp(key, "user") == 0) {
		if (copy_field(cfg->user, sizeof cfg->user, val, key) < 0)
			return -1;
		*seen |= 8;
		return 0;
	}
	if (strcmp(key, "password") == 0) {
		if (copy_field(cfg->password, sizeof cfg->password, val, key) < 0)
			return -1;
		*seen |= 16;
		return 0;
	}
	if (strcmp(key, "video_root") == 0) {
		if (copy_field(cfg->video_root, sizeof cfg->video_root, val, key) < 0)
			return -1;
		if (!root_ok(cfg->video_root)) {
			pwlog(LOG_ERR, "%s: video_root must be an absolute path",
			    g_camera_id);
			return -1;
		}
		return 0;
	}
	if (strcmp(key, "supervisor_pidfile") == 0)
		return copy_field(cfg->supervisor_pidfile,
		    sizeof cfg->supervisor_pidfile, val, key);
	if (strcmp(key, "read_timeout_sec") == 0) {
		if (parse_int(val, 1, 300, &cfg->read_timeout_sec) < 0) {
			pwlog(LOG_ERR, "%s: read_timeout_sec must be 1-300",
			    g_camera_id);
			return -1;
		}
		return 0;
	}
	if (strcmp(key, "open_timeout_sec") == 0) {
		if (parse_int(val, 1, 300, &cfg->open_timeout_sec) < 0) {
			pwlog(LOG_ERR, "%s: open_timeout_sec must be 1-300",
			    g_camera_id);
			return -1;
		}
		return 0;
	}
	if (strcmp(key, "failure_delay_sec") == 0) {
		if (parse_int(val, 0, 300, &cfg->failure_delay_sec) < 0) {
			pwlog(LOG_ERR, "%s: failure_delay_sec must be 0-300",
			    g_camera_id);
			return -1;
		}
		g_failure_delay = cfg->failure_delay_sec;
		return 0;
	}
	pwlog(LOG_ERR, "%s: unknown config key %s", g_camera_id, key);
	return -1;
}

static int
load_config(const char *path, struct config *cfg)
{
	FILE *fp;
	struct stat st;
	int fd;
	int seen = 0;
	char line[PW_LINE_MAX];

	memset(cfg, 0, sizeof *cfg);
	cfg->port = 554;
	cfg->read_timeout_sec = 10;
	cfg->open_timeout_sec = 15;
	cfg->failure_delay_sec = 10;
	memcpy(cfg->video_root, "/tank/video", sizeof "/tank/video");

	fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
	if (fd < 0) {
		pwlog(LOG_ERR, "%s: open %s: %s", g_camera_id, path, strerror(errno));
		return -1;
	}
	if (fstat(fd, &st) < 0) {
		pwlog(LOG_ERR, "%s: fstat %s: %s", g_camera_id, path, strerror(errno));
		close(fd);
		return -1;
	}
	if (!S_ISREG(st.st_mode) ||
	    (st.st_mode & (S_IRWXG | S_IRWXO)) != 0) {
		pwlog(LOG_ERR, "%s: %s must be a regular file and mode 0600",
		    g_camera_id, path);
		close(fd);
		return -1;
	}
	fp = fdopen(fd, "r");
	if (fp == NULL) {
		pwlog(LOG_ERR, "%s: fdopen %s: %s", g_camera_id, path, strerror(errno));
		close(fd);
		return -1;
	}
	while (fgets(line, sizeof line, fp) != NULL) {
		char *key;
		char *val;
		size_t len = strlen(line);

		if (len > 0 && line[len - 1] != '\n' && !feof(fp)) {
			pwlog(LOG_ERR, "%s: config line is longer than %d",
			    g_camera_id, PW_LINE_MAX - 1);
			explicit_bzero(line, sizeof line);
			fclose(fp);
			return -1;
		}
		trim_end(line);
		key = (char *)skip_space(line);
		if (*key == '\0' || *key == '#')
			continue;
		val = strchr(key, '=');
		if (val == NULL) {
			pwlog(LOG_ERR, "%s: config line has no '='", g_camera_id);
			explicit_bzero(line, sizeof line);
			fclose(fp);
			return -1;
		}
		*val++ = '\0';
		trim_end(key);
		val = (char *)skip_space(val);
		trim_end(val);
		if (apply_key(cfg, key, val, &seen) < 0) {
			explicit_bzero(line, sizeof line);
			fclose(fp);
			return -1;
		}
	}
	fclose(fp);
	explicit_bzero(line, sizeof line);
	if ((seen & (1 | 2 | 4 | 8 | 16)) != (1 | 2 | 4 | 8 | 16)) {
		pwlog(LOG_ERR, "%s: config requires camera_id, ip, path, user, and password",
		    g_camera_id);
		return -1;
	}
	while (cfg->video_root[1] != '\0' &&
	    cfg->video_root[strlen(cfg->video_root) - 1] == '/')
		cfg->video_root[strlen(cfg->video_root) - 1] = '\0';
	g_failure_delay = cfg->failure_delay_sec;
	g_camera_id = cfg->camera_id;
	return 0;
}

static int
ntp_synced(void)
{
	struct ntptimeval ntv;
	int status;

	memset(&ntv, 0, sizeof ntv);
	status = ntp_gettime(&ntv);
	if (status < 0)
		return 0;
	switch (status) {
	case TIME_OK:
	case TIME_INS:
	case TIME_DEL:
	case TIME_OOP:
	case TIME_WAIT:
		return 1;
	default:
		return 0;
	}
}

static int
take_lock(const char *camera_id)
{
	char path[PATH_MAX];
	struct stat st;
	int fd;

	if (stat(PW_RUN_DIR, &st) < 0) {
		if (errno != ENOENT ||
		    (mkdir(PW_RUN_DIR, 0755) < 0 && errno != EEXIST)) {
			pwlog(LOG_ERR, "%s: %s: %s", g_camera_id, PW_RUN_DIR,
			    strerror(errno));
			return -1;
		}
	} else if (!S_ISDIR(st.st_mode)) {
		pwlog(LOG_ERR, "%s: %s is not a directory", g_camera_id, PW_RUN_DIR);
		return -1;
	}
	if (snprintf(path, sizeof path, "%s/%s.lock", PW_RUN_DIR, camera_id) >=
	    (int)sizeof path) {
		pwlog(LOG_ERR, "%s: lock path is too long", g_camera_id);
		return -1;
	}
	fd = open(path, O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0644);
	if (fd < 0) {
		pwlog(LOG_ERR, "%s: open %s: %s", g_camera_id, path, strerror(errno));
		return -1;
	}
	if (flock(fd, LOCK_EX | LOCK_NB) < 0) {
		if (errno == EWOULDBLOCK) {
			pwlog(LOG_NOTICE, "%s: already recording; not opening another file",
			    g_camera_id);
			close(fd);
			return 1;
		}
		pwlog(LOG_ERR, "%s: flock %s: %s", g_camera_id, path, strerror(errno));
		close(fd);
		return -1;
	}
	g_lock_fd = fd;
	return 0;
}

static int
append_userinfo(char *dst, size_t dstsz, size_t *used, const char *src)
{
	static const char hex[] = "0123456789ABCDEF";
	size_t n = *used;

	for (; *src != '\0'; src++) {
		unsigned char c = (unsigned char)*src;
		int raw = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
		    (c >= '0' && c <= '9') || c == '-' || c == '.' ||
		    c == '_' || c == '~';

		if (raw) {
			if (n + 1 >= dstsz)
				return -1;
			dst[n++] = (char)c;
		} else {
			if (n + 3 >= dstsz)
				return -1;
			dst[n++] = '%';
			dst[n++] = hex[c >> 4];
			dst[n++] = hex[c & 0x0f];
		}
	}
	*used = n;
	return 0;
}

static int
build_url(const struct config *cfg, char *url, size_t urlsz)
{
	const char *path = cfg->stream_path;
	size_t n = 0;
	int wrote;

	if (path[0] == '/')
		path++;
	wrote = snprintf(url, urlsz, "rtsp://");
	if (wrote < 0 || (size_t)wrote >= urlsz)
		return -1;
	n = (size_t)wrote;
	if (append_userinfo(url, urlsz, &n, cfg->user) < 0)
		return -1;
	if (n + 1 >= urlsz)
		return -1;
	url[n++] = ':';
	if (append_userinfo(url, urlsz, &n, cfg->password) < 0)
		return -1;
	wrote = snprintf(url + n, urlsz - n, "@%s:%d/%s", cfg->ip, cfg->port, path);
	if (wrote < 0 || (size_t)wrote >= urlsz - n)
		return -1;
	return 0;
}

static int
fd_write(void *opaque, const uint8_t *buf, int size)
{
	int fd = *(int *)opaque;
	int wrote = 0;

	while (wrote < size) {
		ssize_t n = write(fd, buf + wrote, (size_t)(size - wrote));

		if (n < 0) {
			if (errno == EINTR) {
				if (stop_requested)
					return wrote > 0 ? wrote : AVERROR_EXIT;
				continue;
			}
			if (wrote > 0)
				return wrote;
			return AVERROR(errno);
		}
		if (n == 0)
			return wrote > 0 ? wrote : AVERROR(EIO);
		wrote += (int)n;
	}
	return wrote;
}

static int
output_close(struct output *out, int do_fsync)
{
	int rc = 0;

	if (out->fmt != NULL && out->fmt->pb != NULL) {
		uint8_t *owned = out->buf;
		uint8_t *buf;

		avio_flush(out->fmt->pb);
		buf = out->fmt->pb->buffer;
		out->fmt->pb->buffer = NULL;
		avio_context_free(&out->fmt->pb);
		av_free(buf);
		if (owned != buf)
			av_free(owned);
		out->buf = NULL;
	} else {
		av_freep(&out->buf);
	}
	if (out->fd >= 0) {
		if (do_fsync && fsync(out->fd) < 0)
			rc = errno;
		close(out->fd);
		out->fd = -1;
	}
	if (out->fmt != NULL)
		avformat_free_context(out->fmt);
	out->fmt = NULL;
	return rc;
}

static int
output_open(struct output *out, const struct config *cfg, const AVCodecParameters *par,
    AVRational time_base)
{
	AVStream *st;
	char dir[PATH_MAX];
	struct stat stbuf;
	struct tm tm;
	time_t sec;
	int attempt;
	int ret;

	if (snprintf(dir, sizeof dir, "%s/%s", cfg->video_root, cfg->camera_id) >=
	    (int)sizeof dir) {
		pwlog(LOG_ERR, "%s: camera directory path is too long", g_camera_id);
		return PW_IO;
	}
	if (stat(dir, &stbuf) < 0 || !S_ISDIR(stbuf.st_mode)) {
		pwlog(LOG_ERR, "%s: camera directory %s is not ready (it is created with the placeholder, not by the recorder)",
		    g_camera_id, dir);
		return PW_IO;
	}

	out->fd = -1;
	for (attempt = 0; attempt < 3; attempt++) {
		struct timespec now;
		char name[32];
		int n;

		if (stop_requested)
			return PW_OK;
		if (clock_gettime(CLOCK_REALTIME, &now) < 0) {
			pwlog(LOG_ERR, "%s: clock_gettime: %s", g_camera_id,
			    strerror(errno));
			return PW_IO;
		}
		sec = now.tv_sec;
		if (gmtime_r(&sec, &tm) == NULL) {
			pwlog(LOG_ERR, "%s: gmtime_r failed", g_camera_id);
			return PW_IO;
		}
		if (strftime(name, sizeof name, "%Y%m%dT%H%M%SZ.ts", &tm) == 0) {
			pwlog(LOG_ERR, "%s: could not format the start time",
			    g_camera_id);
			return PW_IO;
		}
		n = snprintf(out->path, sizeof out->path, "%s/%s", dir, name);
		if (n < 0 || (size_t)n >= sizeof out->path) {
			pwlog(LOG_ERR, "%s: recording path is too long", g_camera_id);
			return PW_IO;
		}
		out->fd = open(out->path,
		    O_CREAT | O_EXCL | O_WRONLY | O_CLOEXEC | O_NOFOLLOW, 0644);
		if (out->fd >= 0)
			break;
		if (errno != EEXIST) {
			pwlog(LOG_ERR, "%s: create %s: %s", g_camera_id, out->path,
			    strerror(errno));
			return errno == ENOSPC ? PW_NOSPC : PW_IO;
		}
		/* Same UTC second as an existing file. Wait for the next name. */
		{
			struct timespec req;

			req.tv_sec = 0;
			req.tv_nsec = 1000000000L - now.tv_nsec;
			if (req.tv_nsec == 1000000000L) {
				req.tv_sec = 1;
				req.tv_nsec = 0;
			}
			nanosleep(&req, NULL);
		}
	}
	if (out->fd < 0) {
		pwlog(LOG_ERR, "%s: could not create a new start-named file",
		    g_camera_id);
		return PW_IO;
	}
	if (fchmod(out->fd, 0644) < 0) {
		pwlog(LOG_ERR, "%s: fchmod %s: %s", g_camera_id, out->path,
		    strerror(errno));
		return PW_IO;
	}

	if (avformat_alloc_output_context2(&out->fmt, NULL, "mpegts", out->path) < 0 ||
	    out->fmt == NULL) {
		pwlog(LOG_ERR, "%s: mpegts muxer is not available in this FFmpeg",
		    g_camera_id);
		return PW_MUX;
	}
	out->fmt->interrupt_callback.callback = io_interrupt;
	out->fmt->interrupt_callback.opaque = &io_guard;
	out->fmt->flush_packets = 1;
	out->fmt->flags |= AVFMT_FLAG_CUSTOM_IO;

	st = avformat_new_stream(out->fmt, NULL);
	if (st == NULL) {
		pwlog(LOG_ERR, "%s: could not add the output stream", g_camera_id);
		return PW_MUX;
	}
	if (avcodec_parameters_copy(st->codecpar, par) < 0) {
		pwlog(LOG_ERR, "%s: could not copy H.264 parameters", g_camera_id);
		return PW_MUX;
	}
	st->codecpar->codec_tag = 0;
	st->time_base = time_base;

	out->buf = av_malloc(PW_IO_BUF);
	if (out->buf == NULL)
		return PW_IO;
	out->fmt->pb = avio_alloc_context(out->buf, PW_IO_BUF, 1, &out->fd,
	    NULL, fd_write, NULL);
	if (out->fmt->pb == NULL) {
		pwlog(LOG_ERR, "%s: could not attach the output file", g_camera_id);
		return PW_IO;
	}
	out->fmt->pb->seekable = 0;
	out->fmt->pb->direct = 1;

	arm_io(cfg->open_timeout_sec);
	ret = avformat_write_header(out->fmt, NULL);
	disarm_io();
	if (ret < 0) {
		if (stop_requested)
			return PW_OK;
		pwlog(LOG_ERR, "%s: could not write the MPEG-TS header to %s",
		    g_camera_id, out->path);
		return ret == AVERROR(ENOSPC) ? PW_NOSPC : PW_MUX;
	}
	out->header_ok = 1;
	pwlog(LOG_INFO, "%s: recording %s from %s", g_camera_id, out->path, cfg->ip);
	return PW_OK;
}

static int
write_video(struct output *out, AVPacket *pkt, AVRational in_tb)
{
	AVRational out_tb = out->fmt->streams[0]->time_base;
	int ret;

	/*
	 * This camera's first picture has no timestamp. The MPEG-TS muxer
	 * would invent 0 for that packet and keep the later wall-clock
	 * values, so the file looks many hours long. Put that picture one
	 * tick before the stream start. av_gettime() is later than the
	 * pictures already read during probing, and using it as the origin
	 * pushes those pictures negative.
	 */
	if (pkt->pts == AV_NOPTS_VALUE ||
	    (pkt->pts == 0 && pkt->dts == AV_NOPTS_VALUE)) {
		if (out->last_dts != AV_NOPTS_VALUE)
			pkt->pts = out->last_dts + 1;
		else if (out->start_pts != AV_NOPTS_VALUE)
			pkt->pts = out->start_pts - 1;
		else
			pkt->pts = av_rescale_q(av_gettime(), AV_TIME_BASE_Q, in_tb);
	}
	if (pkt->dts == AV_NOPTS_VALUE || pkt->pts < pkt->dts)
		pkt->dts = pkt->pts;
	if (out->last_dts != AV_NOPTS_VALUE && pkt->dts <= out->last_dts) {
		pkt->dts = out->last_dts + 1;
		if (pkt->pts < pkt->dts)
			pkt->pts = pkt->dts;
	}
	out->last_dts = pkt->dts;

	av_packet_rescale_ts(pkt, in_tb, out_tb);
	if (out->origin_pts == AV_NOPTS_VALUE)
		out->origin_pts = pkt->dts;
	pkt->pts -= out->origin_pts;
	pkt->dts -= out->origin_pts;
	pkt->stream_index = 0;
	pkt->pos = -1;
	ret = av_interleaved_write_frame(out->fmt, pkt);
	if (ret < 0) {
		char err[AV_ERROR_MAX_STRING_SIZE];

		av_strerror(ret, err, sizeof err);
		pwlog(LOG_ERR, "%s: write %s: %s", g_camera_id, out->path, err);
	}
	return ret;
}

static int
forward_video(struct output *out, AVBSFContext *bsf, AVPacket *pkt, AVRational in_tb)
{
	int ret = av_bsf_send_packet(bsf, pkt);

	if (ret < 0)
		return ret;
	for (;;) {
		ret = av_bsf_receive_packet(bsf, pkt);
		if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
			return 0;
		if (ret < 0)
			return ret;
		ret = write_video(out, pkt, in_tb);
		if (ret < 0)
			return ret;
	}
}

static void
stop_supervisor(const char *pidfile)
{
	char buf[32];
	char *end;
	ssize_t n;
	long pid;
	int fd;

	if (pidfile[0] == '\0') {
		pwlog(LOG_ERR, "%s: disk full; no supervisor_pidfile, so daemon will restart this camera",
		    g_camera_id);
		return;
	}
	fd = open(pidfile, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
	if (fd < 0) {
		pwlog(LOG_ERR, "%s: disk full; open %s: %s", g_camera_id, pidfile,
		    strerror(errno));
		return;
	}
	n = read(fd, buf, sizeof buf - 1);
	close(fd);
	if (n <= 0) {
		pwlog(LOG_ERR, "%s: disk full; empty supervisor pidfile %s",
		    g_camera_id, pidfile);
		return;
	}
	buf[n] = '\0';
	errno = 0;
	pid = strtol(buf, &end, 10);
	if (errno != 0 || end == buf || pid <= 1 || pid > INT_MAX ||
	    pid == (long)getpid()) {
		pwlog(LOG_ERR, "%s: disk full; supervisor pid in %s is not usable",
		    g_camera_id, pidfile);
		return;
	}
	if (kill((pid_t)pid, SIGTERM) < 0) {
		pwlog(LOG_ERR, "%s: disk full; could not signal supervisor %ld: %s. The supervisor has to run as this user",
		    g_camera_id, pid, strerror(errno));
		return;
	}
	pwlog(LOG_ERR, "%s: disk full; signaled supervisor %ld to stop",
	    g_camera_id, pid);
}

static int
check_unused(AVDictionary *opts)
{
	const AVDictionaryEntry *ent = NULL;
	int bad = 0;

	while ((ent = av_dict_get(opts, "", ent, AV_DICT_IGNORE_SUFFIX)) != NULL) {
		pwlog(LOG_ERR, "%s: this FFmpeg did not accept option %s",
		    g_camera_id, ent->key);
		bad = 1;
	}
	return bad;
}

static int
record(struct config *cfg)
{
	AVFormatContext *in = NULL;
	AVBSFContext *bsf = NULL;
	AVPacket *pkt = NULL;
	AVDictionary *opts = NULL;
	AVStream *src;
	struct output out;
	char url[PW_URL_MAX];
	char err[AV_ERROR_MAX_STRING_SIZE];
	const AVBitStreamFilter *filter;
	int64_t opened_at = 0;
	int video_index;
	int status = PW_STALL;
	int fault_logged = 0;
	int ret;

	memset(&out, 0, sizeof out);
	out.fd = -1;
	out.origin_pts = AV_NOPTS_VALUE;
	out.start_pts = AV_NOPTS_VALUE;
	out.last_dts = AV_NOPTS_VALUE;

	if (build_url(cfg, url, sizeof url) < 0) {
		pwlog(LOG_ERR, "%s: RTSP URL does not fit", g_camera_id);
		status = PW_USAGE;
		goto done;
	}
	if (av_dict_set(&opts, "rtsp_transport", "tcp", 0) < 0 ||
	    av_dict_set(&opts, "use_wallclock_as_timestamps", "1", 0) < 0 ||
	    av_dict_set_int(&opts, "timeout",
	    (int64_t)cfg->open_timeout_sec * 1000000, 0) < 0) {
		pwlog(LOG_ERR, "%s: could not set RTSP options", g_camera_id);
		status = PW_IO;
		goto done;
	}

	in = avformat_alloc_context();
	if (in == NULL) {
		status = PW_IO;
		goto done;
	}
	in->interrupt_callback.callback = io_interrupt;
	in->interrupt_callback.opaque = &io_guard;
	/*
	 * Same clock as -use_wallclock_as_timestamps 1. write_video shifts
	 * those stamps so the file starts at the first picture. MPEG-TS
	 * stores 33 bits of PTS at 90 kHz, about 26.5 hours, so a session
	 * left open that long can wrap once. The filename is the absolute
	 * UTC start.
	 */
	in->use_wallclock_as_timestamps = 1;
	in->max_analyze_duration = 3 * AV_TIME_BASE;

	arm_io(cfg->open_timeout_sec);
	ret = avformat_open_input(&in, url, NULL, &opts);
	explicit_bzero(url, sizeof url);
	if (ret < 0) {
		disarm_io();
		av_strerror(ret, err, sizeof err);
		pwlog(LOG_ERR, "%s: RTSP open from %s failed: %s", g_camera_id,
		    cfg->ip, err);
		status = stop_requested ? PW_OK : PW_STALL;
		goto done;
	}
	if (check_unused(opts)) {
		disarm_io();
		status = PW_STALL;
		goto done;
	}
	av_dict_free(&opts);

	arm_io(cfg->open_timeout_sec);
	ret = avformat_find_stream_info(in, NULL);
	disarm_io();
	if (ret < 0) {
		av_strerror(ret, err, sizeof err);
		pwlog(LOG_ERR, "%s: could not read stream info from %s: %s",
		    g_camera_id, cfg->ip, err);
		status = stop_requested ? PW_OK : PW_STALL;
		goto done;
	}
	if (stop_requested) {
		status = PW_OK;
		goto done;
	}

	video_index = av_find_best_stream(in, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
	if (video_index < 0) {
		pwlog(LOG_ERR, "%s: no video stream from %s", g_camera_id, cfg->ip);
		status = PW_STREAM;
		goto done;
	}
	src = in->streams[video_index];
	if (src->codecpar->codec_id != AV_CODEC_ID_H264 ||
	    src->codecpar->width != PW_WIDTH ||
	    src->codecpar->height != PW_HEIGHT) {
		pwlog(LOG_ERR, "%s: refused %s %dx%d from %s; want H.264 %dx%d",
		    g_camera_id,
		    avcodec_get_name(src->codecpar->codec_id),
		    src->codecpar->width, src->codecpar->height,
		    cfg->ip, PW_WIDTH, PW_HEIGHT);
		status = PW_STREAM;
		goto done;
	}
	if (src->time_base.num <= 0 || src->time_base.den <= 0)
		src->time_base = (AVRational){1, 90000};
	if (src->codecpar->extradata_size <= 0) {
		pwlog(LOG_WARNING, "%s: H.264 parameter sets were not in the stream header; in-band sets must arrive with the pictures",
		    g_camera_id);
	}

	filter = av_bsf_get_by_name("h264_mp4toannexb");
	if (filter == NULL || av_bsf_alloc(filter, &bsf) < 0) {
		pwlog(LOG_ERR, "%s: h264_mp4toannexb is missing from this FFmpeg",
		    g_camera_id);
		status = PW_STREAM;
		goto done;
	}
	if (avcodec_parameters_copy(bsf->par_in, src->codecpar) < 0) {
		status = PW_STREAM;
		goto done;
	}
	bsf->time_base_in = src->time_base;
	ret = av_bsf_init(bsf);
	if (ret < 0) {
		av_strerror(ret, err, sizeof err);
		pwlog(LOG_ERR, "%s: could not prepare H.264 parameter sets: %s",
		    g_camera_id, err);
		status = PW_STREAM;
		goto done;
	}

	status = output_open(&out, cfg, bsf->par_out, src->time_base);
	if (status != PW_OK || out.header_ok == 0)
		goto done;
	out.start_pts = src->start_time;
	opened_at = av_gettime_relative();

	pkt = av_packet_alloc();
	if (pkt == NULL) {
		status = PW_IO;
		goto done;
	}

	status = PW_OK;
	for (;;) {
		if (stop_requested)
			break;
		if (!fault_logged && opened_at > 0 &&
		    av_gettime_relative() - opened_at >= (int64_t)PW_FAULT_SEC * 1000000) {
			pwlog(LOG_ERR, "%s: session still open after 8 hours; still writing %s",
			    g_camera_id, out.path);
			fault_logged = 1;
		}
		arm_io(cfg->read_timeout_sec);
		ret = av_read_frame(in, pkt);
		disarm_io();
		if (ret < 0) {
			if (stop_requested)
				break;
			av_strerror(ret, err, sizeof err);
			pwlog(LOG_ERR, "%s: read from %s stopped: %s", g_camera_id,
			    cfg->ip, err);
			status = PW_STALL;
			break;
		}
		if (pkt->stream_index != video_index) {
			av_packet_unref(pkt);
			continue;
		}
		ret = forward_video(&out, bsf, pkt, src->time_base);
		av_packet_unref(pkt);
		if (ret < 0) {
			status = (ret == AVERROR(ENOSPC)) ? PW_NOSPC : PW_MUX;
			break;
		}
	}

	if (out.header_ok && out.fmt != NULL &&
	    status != PW_NOSPC && status != PW_MUX && pkt != NULL && bsf != NULL) {
		arm_io(cfg->open_timeout_sec);
		av_bsf_send_packet(bsf, NULL);
		for (;;) {
			ret = av_bsf_receive_packet(bsf, pkt);
			if (ret < 0)
				break;
			ret = write_video(&out, pkt, src->time_base);
			av_packet_unref(pkt);
			if (ret < 0) {
				status = (ret == AVERROR(ENOSPC)) ? PW_NOSPC : PW_MUX;
				break;
			}
		}
		disarm_io();
	}
	if (out.header_ok && out.fmt != NULL && status != PW_NOSPC) {
		arm_io(cfg->open_timeout_sec);
		ret = av_write_trailer(out.fmt);
		disarm_io();
		if (ret < 0) {
			av_strerror(ret, err, sizeof err);
			pwlog(LOG_ERR, "%s: trailer %s: %s", g_camera_id, out.path, err);
			if (ret == AVERROR(ENOSPC))
				status = PW_NOSPC;
			else if (status == PW_OK)
				status = PW_MUX;
		}
		if (status == PW_OK)
			pwlog(LOG_INFO, "%s: stop %s", g_camera_id, out.path);
	}

done:
	disarm_io();
	av_packet_free(&pkt);
	av_bsf_free(&bsf);
	if (in != NULL) {
		arm_io(cfg->open_timeout_sec);
		avformat_close_input(&in);
		disarm_io();
	}
	av_dict_free(&opts);
	explicit_bzero(url, sizeof url);
	ret = output_close(&out, status == PW_OK && out.header_ok);
	if (ret != 0) {
		pwlog(LOG_ERR, "%s: fsync %s: %s", g_camera_id, out.path,
		    strerror(ret));
		if (ret == ENOSPC)
			status = PW_NOSPC;
	}
	if (status == PW_NOSPC)
		stop_supervisor(cfg->supervisor_pidfile);
	return status;
}

int
main(int argc, char **argv)
{
	struct config cfg;
	int status = PW_USAGE;

	memset(&cfg, 0, sizeof cfg);
	openlog("packwatch", LOG_PID | LOG_NDELAY, LOG_DAEMON);
	av_log_set_level(AV_LOG_WARNING);
	av_log_set_callback(av_to_syslog);
	install_signals();

	if (argc != 2 || argv[1][0] == '-') {
		pwlog(LOG_ERR, "usage: packwatch /usr/local/etc/packwatch/<camera-id>.conf");
		goto out;
	}
	if (load_config(argv[1], &cfg) < 0)
		goto out;

	status = take_lock(cfg.camera_id);
	if (status != 0) {
		status = (status < 0) ? PW_IO : PW_OK;
		goto out;
	}

	if (!ntp_synced()) {
		pwlog(LOG_ERR, "%s: NTP is not synced; not opening a file",
		    g_camera_id);
		status = PW_NTP;
		goto out;
	}

	status = record(&cfg);

out:
	if (av_line_len > 0)
		flush_av_line(AV_LOG_INFO);
	config_scrub(&cfg);
	status = finish(status);
	if (g_lock_fd >= 0)
		close(g_lock_fd);
	closelog();
	return status;
}
