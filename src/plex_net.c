#define _GNU_SOURCE
#include "plex_net.h"

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#include "plex_log.h"
#include "plexd_client.h"

/*
 * Requests go to plexd's network socket as plain HTTP/1.1 with the full URL
 * in the request line (forward-proxy style); plexd makes the real request
 * (see plexd/internal/plex/forward.go) and streams the response back. A
 * response cut short upstream reaches us as a short Content-Length body or
 * a chunked body without its final chunk, and is reported as a failure.
 */

#define PLEX_NET_DEFAULT_TIMEOUT 10
#define HDR_LINE_MAX             8192
#define READ_BUF_SIZE            (32 * 1024)

#define PLEX_PRODUCT            "PlexMusic"
#define PLEX_VERSION            "0.1.0"
#define PLEX_PLATFORM           "Chrome"

/* Until set, the id every install shared before per-device ids */
static char s_client_id[64] = "nextui-plex-music";

void plex_net_set_client_id(const char *client_id)
{
    if (!client_id || !*client_id) return;
    strncpy(s_client_id, client_id, sizeof(s_client_id) - 1);
    s_client_id[sizeof(s_client_id) - 1] = '\0';
}

const char *plex_net_client_id(void)
{
    return s_client_id;
}

void plex_net_connection_close(void) {}

/* URL without its query string (which can hold the token), for logs. */
static void url_path_only(const char *url, char *out_buf, int out_size)
{
    const char *q = strchr(url, '?');
    int len = q ? (int)(q - url) : (int)strlen(url);
    if (len >= out_size) len = out_size - 1;
    memcpy(out_buf, url, len);
    out_buf[len] = '\0';
}

/* ------------------------------------------------------------------
 * Connection to plexd, with a read buffer
 * ------------------------------------------------------------------ */

typedef struct {
    int     fd;
    uint8_t buf[READ_BUF_SIZE];
    int     len, pos;
} Conn;

static const char *net_socket_path(void)
{
    const char *p = getenv("PLEXD_NET_SOCKET");
    return (p && p[0]) ? p : "/tmp/plexd-net.sock";
}

static int try_connect(int timeout_sec)
{
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", net_socket_path());
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        close(fd);
        return -1;
    }
    struct timeval tv = { timeout_sec, 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    return fd;
}

static int conn_open(Conn *c, int timeout_sec)
{
    static pthread_mutex_t restart_lock = PTHREAD_MUTEX_INITIALIZER;
    static time_t          last_restart = 0;

    c->len = c->pos = 0;
    c->fd = try_connect(timeout_sec);
    if (c->fd >= 0) return 0;

    /* plexd went away (crashed, or was never started): start it again, at
     * most every 10 s, then retry once. */
    pthread_mutex_lock(&restart_lock);
    time_t now = time(NULL);
    bool restart = now - last_restart >= 10;
    if (restart) last_restart = now;
    pthread_mutex_unlock(&restart_lock);
    if (restart) {
        PLEX_LOG_ERROR("[PlexNet] plexd not reachable at %s; starting it\n", net_socket_path());
        plexd_ensure_running();
        c->fd = try_connect(timeout_sec);
    }
    if (c->fd < 0) {
        PLEX_LOG_ERROR("[PlexNet] plexd not running; request not sent\n");
        return -1;
    }
    return 0;
}

static void conn_close(Conn *c)
{
    if (c->fd >= 0) close(c->fd);
    c->fd = -1;
}

static int conn_write_all(Conn *c, const char *data, size_t len)
{
    while (len > 0) {
        ssize_t n = write(c->fd, data, len);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return -1;
        data += n;
        len  -= (size_t)n;
    }
    return 0;
}

/* Fill the read buffer. Returns bytes available, 0 on EOF, -1 on error. */
static int conn_fill(Conn *c)
{
    if (c->pos < c->len) return c->len - c->pos;
    for (;;) {
        ssize_t n = read(c->fd, c->buf, sizeof(c->buf));
        if (n < 0 && errno == EINTR) continue;
        if (n < 0) return -1;
        c->pos = 0;
        c->len = (int)n;
        return (int)n;
    }
}

/* Read up to max bytes. Returns > 0, 0 on EOF, -1 on error/timeout. */
static int conn_read(Conn *c, uint8_t *dst, int max)
{
    int avail = conn_fill(c);
    if (avail <= 0) return avail;
    int n = avail < max ? avail : max;
    memcpy(dst, c->buf + c->pos, n);
    c->pos += n;
    return n;
}

/* Read one line, without its CRLF. Returns its length, or -1. */
static int conn_read_line(Conn *c, char *line, int max)
{
    int n = 0;
    for (;;) {
        if (conn_fill(c) <= 0) return -1;
        char ch = (char)c->buf[c->pos++];
        if (ch == '\n') break;
        if (ch != '\r' && n < max - 1) line[n++] = ch;
    }
    line[n] = '\0';
    return n;
}

/* ------------------------------------------------------------------
 * Request / response
 * ------------------------------------------------------------------ */

static int send_request(Conn *c, PlexHttpMethod method, const char *url,
                        const char *token, const char *body, const char *accept)
{
    const char *method_str = method == PLEX_HTTP_POST ? "POST"
                           : method == PLEX_HTTP_PUT  ? "PUT" : "GET";
    size_t blen = body ? strlen(body) : 0;
    size_t cap  = strlen(url) + 1024;
    char  *req  = malloc(cap);
    if (!req) return -1;

    int n = snprintf(req, cap,
        "%s %s HTTP/1.1\r\n"
        "Host: plexd\r\n"
        "Accept: %s\r\n"
        "X-Plex-Client-Identifier: %s\r\n"
        "X-Plex-Product: " PLEX_PRODUCT "\r\n"
        "X-Plex-Version: " PLEX_VERSION "\r\n"
        "X-Plex-Platform: " PLEX_PLATFORM "\r\n",
        method_str, url, accept, s_client_id);
    if (token && *token)
        n += snprintf(req + n, cap - n, "X-Plex-Token: %s\r\n", token);
    if (method != PLEX_HTTP_GET)
        n += snprintf(req + n, cap - n,
                      "Content-Type: application/x-www-form-urlencoded\r\n"
                      "Content-Length: %zu\r\n", blen);
    n += snprintf(req + n, cap - n, "Connection: close\r\n\r\n");

    int rc = (n > 0 && (size_t)n < cap) ? conn_write_all(c, req, (size_t)n) : -1;
    free(req);
    if (rc == 0 && blen) rc = conn_write_all(c, body, blen);
    return rc;
}

typedef struct {
    int  status;
    long content_length;   /* -1 = not given */
    bool chunked;
} RespHead;

static int read_head(Conn *c, RespHead *h)
{
    char line[HDR_LINE_MAX];
    h->status = 0;
    h->content_length = -1;
    h->chunked = false;

    if (conn_read_line(c, line, sizeof(line)) < 0) return -1;
    if (sscanf(line, "HTTP/%*s %d", &h->status) != 1) return -1;

    for (;;) {
        int n = conn_read_line(c, line, sizeof(line));
        if (n < 0) return -1;
        if (n == 0) break;   /* end of headers */
        if (strncasecmp(line, "Content-Length:", 15) == 0)
            h->content_length = atol(line + 15);
        else if (strncasecmp(line, "Transfer-Encoding:", 18) == 0 && strcasestr(line + 18, "chunked"))
            h->chunked = true;
    }
    return 0;
}

/* Receives body data; returns 0 to continue, -1 to abort. */
typedef int (*BodySink)(void *ctx, const uint8_t *data, int n);

static void report_progress(volatile int *progress, long done, long total)
{
    if (!progress || total <= 0) return;
    int pct = (int)(done * 100 / total);
    *progress = pct > 100 ? 100 : pct;
}

/* Read the whole body into sink. Returns its length, or -1 if it was cut
 * short, the sink refused it, or the caller cancelled. */
static long read_body(Conn *c, const RespHead *h, BodySink sink, void *ctx,
                      volatile bool *cancel, volatile int *progress)
{
    static __thread uint8_t chunk[READ_BUF_SIZE];
    long total = 0;

    if (h->chunked) {
        char line[128];
        for (;;) {
            if (conn_read_line(c, line, sizeof(line)) < 0) return -1;
            long size = strtol(line, NULL, 16);   /* stops at any ;extension */
            if (size < 0) return -1;
            if (size == 0) {
                /* trailers, then the blank line that ends the body */
                int n;
                while ((n = conn_read_line(c, line, sizeof(line))) > 0) {}
                return n == 0 ? total : -1;
            }
            while (size > 0) {
                if (cancel && *cancel) return -1;
                int want = size < (long)sizeof(chunk) ? (int)size : (int)sizeof(chunk);
                int n = conn_read(c, chunk, want);
                if (n <= 0 || sink(ctx, chunk, n) != 0) return -1;
                size  -= n;
                total += n;
            }
            if (conn_read_line(c, line, sizeof(line)) != 0) return -1;   /* CRLF after data */
        }
    }

    for (;;) {
        if (cancel && *cancel) return -1;
        long want = sizeof(chunk);
        if (h->content_length >= 0) {
            if (total >= h->content_length) return total;
            if (h->content_length - total < want) want = h->content_length - total;
        }
        int n = conn_read(c, chunk, (int)want);
        if (n < 0) return -1;
        if (n == 0) return h->content_length >= 0 ? -1 : total;   /* EOF */
        if (sink(ctx, chunk, n) != 0) return -1;
        total += n;
        report_progress(progress, total, h->content_length);
    }
}

/* Log an error response with the start of its body (plexd puts the reason
 * there, e.g. a failed certificate check). */
static void log_http_error(Conn *c, const RespHead *h, const char *url)
{
    char safe_url[512];
    url_path_only(url, safe_url, sizeof(safe_url));
    uint8_t msg[200];
    int n = 0;
    if (h->status == 502) {
        n = conn_read(c, msg, sizeof(msg) - 1);
        if (n < 0) n = 0;
        while (n > 0 && (msg[n - 1] == '\n' || msg[n - 1] == '\r')) n--;
    }
    msg[n] = '\0';
    PLEX_LOG_ERROR("[PlexNet] HTTP %d for URL: %s%s%s\n", h->status, safe_url,
                   n ? " — " : "", (char *)msg);
}

/* ------------------------------------------------------------------
 * plex_net_fetch
 * ------------------------------------------------------------------ */

typedef struct {
    uint8_t *buf;
    int      size;   /* capacity, keeping 1 byte for the caller's NUL */
    int      len;
    bool     truncated;
} MemSink;

static int mem_sink(void *ctx, const uint8_t *data, int n)
{
    MemSink *m = ctx;
    int room = m->size - 1 - m->len;
    if (n > room) {
        m->truncated = true;   /* keep reading so the response completes */
        n = room;
    }
    if (n > 0) {
        memcpy(m->buf + m->len, data, n);
        m->len += n;
    }
    return 0;
}

/* Returns bytes, -1 on a failure worth retrying, -2 on one that isn't. */
static int fetch_once(const char *url, uint8_t *buffer, int buffer_size,
                      const PlexNetOptions *opts)
{
    int timeout = (opts && opts->timeout_sec > 0) ? opts->timeout_sec : PLEX_NET_DEFAULT_TIMEOUT;
    Conn *c = malloc(sizeof(Conn));
    if (!c) return -1;
    if (conn_open(c, timeout) != 0) { free(c); return -1; }

    int result = -1;
    RespHead h;
    if (send_request(c, opts ? opts->method : PLEX_HTTP_GET, url, opts ? opts->token : NULL,
                     opts ? opts->body : NULL, "application/json") != 0 ||
        read_head(c, &h) != 0) {
        char safe_url[512];
        url_path_only(url, safe_url, sizeof(safe_url));
        PLEX_LOG_ERROR("[PlexNet] no response from plexd for %s\n", safe_url);
    } else if (h.status >= 400) {
        log_http_error(c, &h, url);
        result = (h.status >= 500) ? -1 : -2;
    } else {
        MemSink m = { buffer, buffer_size, 0, false };
        if (read_body(c, &h, mem_sink, &m, NULL, NULL) >= 0) {
            result = m.len;
            if (m.truncated) {
                char safe_url[512];
                url_path_only(url, safe_url, sizeof(safe_url));
                PLEX_LOG_ERROR("[PlexNet] response larger than %d bytes, truncated: %s\n",
                               buffer_size, safe_url);
            }
        } else {
            char safe_url[512];
            url_path_only(url, safe_url, sizeof(safe_url));
            PLEX_LOG_ERROR("[PlexNet] response cut short: %s\n", safe_url);
        }
    }
    conn_close(c);
    free(c);
    return result;
}

int plex_net_fetch(const char *url, uint8_t *buffer, int buffer_size,
                   const PlexNetOptions *opts)
{
    if (!url || !buffer || buffer_size <= 1) return -1;
    int r = fetch_once(url, buffer, buffer_size, opts);
    int retries = (opts && opts->no_retry) ? 0 : 2;
    for (int i = 0; i < retries && r == -1; i++) {
        usleep(1000000);
        r = fetch_once(url, buffer, buffer_size, opts);
    }
    return r < 0 ? -1 : r;
}

/* ------------------------------------------------------------------
 * plex_net_download_file
 * ------------------------------------------------------------------ */

typedef struct {
    FILE          *f;
    long           written;
    long           total;      /* expected size, -1 = unknown */
    volatile int  *progress;
} FileSink;

static int file_sink(void *ctx, const uint8_t *data, int n)
{
    FileSink *fs = ctx;
    if (fwrite(data, 1, (size_t)n, fs->f) != (size_t)n) return -1;   /* SD card full? */
    fs->written += n;
    report_progress(fs->progress, fs->written, fs->total);
    return 0;
}

int plex_net_download_file(const char *url, const char *filepath,
                           volatile int *progress_pct,
                           volatile bool *should_cancel,
                           const PlexNetOptions *opts)
{
    if (!url || !filepath) return -1;
    char safe_url[512];
    url_path_only(url, safe_url, sizeof(safe_url));

    int timeout = (opts && opts->timeout_sec > 0) ? opts->timeout_sec : PLEX_NET_DEFAULT_TIMEOUT;
    Conn *c = malloc(sizeof(Conn));
    if (!c) return -1;
    if (conn_open(c, timeout) != 0) { free(c); return -1; }

    int result = -1;
    RespHead h;
    if (send_request(c, PLEX_HTTP_GET, url, opts ? opts->token : NULL, NULL, "*/*") != 0 ||
        read_head(c, &h) != 0) {
        PLEX_LOG_ERROR("[PlexNet] download: no response from plexd for %s\n", safe_url);
    } else if (h.status >= 400) {
        log_http_error(c, &h, url);
    } else {
        FILE *f = fopen(filepath, "wb");
        if (!f) {
            PLEX_LOG_ERROR("[PlexNet] download: cannot write %s\n", filepath);
        } else {
            FileSink fs = { f, 0, h.chunked ? -1 : h.content_length, progress_pct };
            long n = read_body(c, &h, file_sink, &fs, should_cancel, progress_pct);
            /* fclose flushes; a failure there is a failed write too */
            bool closed_ok = fclose(f) == 0;
            if (n >= 0 && closed_ok && n > 0) {
                if (progress_pct) *progress_pct = 100;
                result = n > 0x7fffffff ? 0x7fffffff : (int)n;
            } else if (!(should_cancel && *should_cancel)) {
                PLEX_LOG_ERROR("[PlexNet] download incomplete (%ld bytes%s): %s\n", fs.written,
                               closed_ok ? "" : ", write failed", safe_url);
            }
        }
    }
    conn_close(c);
    free(c);
    return result;
}
