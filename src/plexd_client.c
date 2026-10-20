#define _GNU_SOURCE
#include "plexd_client.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#include "include/parson/parson.h"
#include "plex_log.h"
#include "plex_queue.h"

static PlexdStatus s_cached;
static long long   s_last_tick_ms = 0;

static long long now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static const char *socket_path(void)
{
    const char *p = getenv("PLEXD_SOCKET");
    return (p && p[0]) ? p : "/tmp/plexd.sock";
}

static void set_err(char *err, int errlen, const char *msg)
{
    if (err && errlen > 0) snprintf(err, errlen, "%s", msg);
}

/* Minimal HTTP/1.0 over the unix socket (same as Audiobookshelf.pak's UI).
 * body NULL = GET. Returns the parsed JSON body of a 200 response. */
static JSON_Value *plexd_call(const char *path, const char *body, int timeout_sec,
                              char *err, int errlen)
{
    if (err && errlen > 0) err[0] = '\0';

    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        set_err(err, errlen, "socket() failed");
        return NULL;
    }
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", socket_path());
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        close(fd);
        set_err(err, errlen, "Player service is not running");
        return NULL;
    }
    struct timeval tv = { .tv_sec = timeout_sec, .tv_usec = 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    size_t blen = body ? strlen(body) : 0;
    char head[256];
    int hlen = snprintf(head, sizeof(head),
                        "%s %s HTTP/1.0\r\nHost: plexd\r\nContent-Type: application/json\r\n"
                        "Content-Length: %zu\r\n\r\n",
                        body ? "POST" : "GET", path, blen);
    if (write(fd, head, (size_t)hlen) != hlen ||
        (blen && write(fd, body, blen) != (ssize_t)blen)) {
        close(fd);
        set_err(err, errlen, "Could not talk to player service");
        return NULL;
    }

    size_t cap = 16384, len = 0;
    char *buf = malloc(cap);
    if (!buf) {
        close(fd);
        set_err(err, errlen, "Out of memory");
        return NULL;
    }
    for (;;) {
        if (len + 4096 > cap) {
            cap *= 2;
            char *nb = realloc(buf, cap);
            if (!nb) {
                free(buf);
                close(fd);
                set_err(err, errlen, "Out of memory");
                return NULL;
            }
            buf = nb;
        }
        ssize_t n = read(fd, buf + len, cap - len - 1);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        len += (size_t)n;
    }
    close(fd);
    buf[len] = '\0';

    int status = 0;
    sscanf(buf, "HTTP/%*s %d", &status);
    char *json = strstr(buf, "\r\n\r\n");
    JSON_Value *root = json ? json_parse_string(json + 4) : NULL;
    free(buf);

    if (status != 200) {
        const char *msg = root ? json_object_get_string(json_value_get_object(root), "error") : NULL;
        set_err(err, errlen, (msg && msg[0]) ? msg
                : (status ? "Request failed" : "No response from player service"));
        json_value_free(root);
        return NULL;
    }
    if (!root) set_err(err, errlen, "Bad response from player service");
    return root;
}

static void copy_str(char *dst, size_t size, const char *src)
{
    snprintf(dst, size, "%s", src ? src : "");
}

static PlexdState parse_state(const char *s)
{
    if (!s) return PLEXD_IDLE;
    if (!strcmp(s, "loading")) return PLEXD_LOADING;
    if (!strcmp(s, "playing")) return PLEXD_PLAYING;
    if (!strcmp(s, "paused"))  return PLEXD_PAUSED;
    if (!strcmp(s, "ended"))   return PLEXD_ENDED;
    if (!strcmp(s, "error"))   return PLEXD_ERROR;
    return PLEXD_IDLE;
}

static void parse_track(JSON_Object *t, PlexTrack *out)
{
    memset(out, 0, sizeof(*out));
    out->rating_key         = (int)json_object_get_number(t, "ratingKey");
    out->duration_ms        = (int)json_object_get_number(t, "durationMs");
    out->track_number       = (int)json_object_get_number(t, "trackNumber");
    out->audio_bitrate_kbps = (int)json_object_get_number(t, "bitrateKbps");
    copy_str(out->title,       sizeof(out->title),       json_object_get_string(t, "title"));
    copy_str(out->artist,      sizeof(out->artist),      json_object_get_string(t, "artist"));
    copy_str(out->album,       sizeof(out->album),       json_object_get_string(t, "album"));
    copy_str(out->media_key,   sizeof(out->media_key),   json_object_get_string(t, "mediaKey"));
    copy_str(out->thumb,       sizeof(out->thumb),       json_object_get_string(t, "thumb"));
    copy_str(out->local_path,  sizeof(out->local_path),  json_object_get_string(t, "localPath"));
    copy_str(out->audio_codec, sizeof(out->audio_codec), json_object_get_string(t, "audioCodec"));
}

static void parse_status(JSON_Value *root, PlexdStatus *out)
{
    memset(out, 0, sizeof(*out));
    JSON_Object *o = json_value_get_object(root);
    if (!o) return;
    out->ok = true;
    copy_str(out->connection, sizeof(out->connection), json_object_get_string(o, "connection"));

    JSON_Object *set = json_object_get_object(o, "settings");
    if (set) {
        out->mute_games = json_object_get_boolean(set, "muteGames") == 1;
        out->hotkeys    = json_object_dotget_boolean(set, "hotkeys.enabled") == 1;
    }

    JSON_Object *p = json_object_get_object(o, "player");
    if (!p) return;
    out->state          = parse_state(json_object_get_string(p, "state"));
    out->queue_id       = (int)json_object_get_number(p, "queueId");
    out->index          = (int)json_object_get_number(p, "index");
    out->count          = (int)json_object_get_number(p, "count");
    out->position_ms    = (int)json_object_get_number(p, "positionMs");
    out->duration_ms    = (int)json_object_get_number(p, "durationMs");
    out->shuffle        = json_object_get_boolean(p, "shuffle") == 1;
    out->has_next       = json_object_get_boolean(p, "hasNext") == 1;
    out->has_prev       = json_object_get_boolean(p, "hasPrev") == 1;
    out->local          = json_object_get_boolean(p, "local") == 1;
    out->transcode_kbps = (int)json_object_get_number(p, "transcodeKbps");
    copy_str(out->error, sizeof(out->error), json_object_get_string(p, "error"));

    const char *rep = json_object_get_string(p, "repeat");
    out->repeat = (rep && !strcmp(rep, "all")) ? REPEAT_ALL
                : (rep && !strcmp(rep, "one")) ? REPEAT_ONE : REPEAT_OFF;

    JSON_Object *t = json_object_get_object(p, "track");
    if (t) {
        out->has_track = true;
        parse_track(t, &out->track);
    }
}

bool plexd_get_status(PlexdStatus *out)
{
    JSON_Value *root = plexd_call("/status", NULL, 3, NULL, 0);
    if (!root) {
        memset(&s_cached, 0, sizeof(s_cached));
        if (out) *out = s_cached;
        return false;
    }
    parse_status(root, &s_cached);
    json_value_free(root);
    if (out) *out = s_cached;
    return true;
}

static void heartbeat(bool foreground)
{
    char body[32];
    snprintf(body, sizeof(body), "{\"foreground\":%s}", foreground ? "true" : "false");
    JSON_Value *root = plexd_call("/ui", body, 3, NULL, 0);
    if (!root) {
        memset(&s_cached, 0, sizeof(s_cached));
        return;
    }
    parse_status(root, &s_cached);
    json_value_free(root);
}

void plexd_tick(void)
{
    long long now = now_ms();
    if (now - s_last_tick_ms < 1000) return;
    s_last_tick_ms = now;
    heartbeat(true);
}

void plexd_set_foreground(bool foreground)
{
    s_last_tick_ms = now_ms();
    heartbeat(foreground);
}

const PlexdStatus *plexd_cached_status(void)
{
    return &s_cached;
}

bool plexd_has_queue(void)
{
    return s_cached.ok && s_cached.has_track &&
           (s_cached.state == PLEXD_PLAYING || s_cached.state == PLEXD_PAUSED ||
            s_cached.state == PLEXD_LOADING || s_cached.state == PLEXD_ERROR);
}

bool plexd_play_queue(const PlexTrack *tracks, int count, int start, char *err, int errlen)
{
    JSON_Value  *root = json_value_init_object();
    JSON_Object *o    = json_value_get_object(root);
    JSON_Value  *arr_v = json_value_init_array();
    JSON_Array  *arr   = json_value_get_array(arr_v);
    for (int i = 0; i < count; i++) {
        const PlexTrack *t = &tracks[i];
        JSON_Value  *tv = json_value_init_object();
        JSON_Object *to = json_value_get_object(tv);
        json_object_set_number(to, "ratingKey",   t->rating_key);
        json_object_set_number(to, "durationMs",  t->duration_ms);
        json_object_set_number(to, "trackNumber", t->track_number);
        json_object_set_string(to, "title",       t->title);
        json_object_set_string(to, "artist",      t->artist);
        json_object_set_string(to, "album",       t->album);
        json_object_set_string(to, "mediaKey",    t->media_key);
        json_object_set_string(to, "thumb",       t->thumb);
        json_object_set_string(to, "localPath",   t->local_path);
        json_object_set_string(to, "audioCodec",  t->audio_codec);
        json_object_set_number(to, "bitrateKbps", t->audio_bitrate_kbps);
        json_array_append_value(arr, tv);
    }
    json_object_set_value(o, "tracks", arr_v);
    json_object_set_number(o, "start", start);
    char *body = json_serialize_to_string(root);
    json_value_free(root);
    if (!body) {
        set_err(err, errlen, "Out of memory");
        return false;
    }

    JSON_Value *resp = plexd_call("/queue", body, 20, err, errlen);
    json_free_serialized_string(body);
    if (!resp) {
        PLEX_LOG_ERROR("[plexd] /queue: %s\n", err ? err : "");
        return false;
    }
    parse_status(resp, &s_cached);
    json_value_free(resp);
    return true;
}

bool plexd_cmd(const char *path)
{
    char err[160];
    JSON_Value *r = plexd_call(path, "{}", 10, err, sizeof(err));
    if (!r) {
        PLEX_LOG_ERROR("[plexd] %s: %s\n", path, err);
        return false;
    }
    json_value_free(r);
    return true;
}

bool plexd_seek(int position_ms)
{
    char body[48];
    if (position_ms < 0) position_ms = 0;
    snprintf(body, sizeof(body), "{\"positionMs\":%d}", position_ms);
    JSON_Value *r = plexd_call("/seek", body, 10, NULL, 0);
    if (!r) return false;
    json_value_free(r);
    return true;
}

static bool post_settings(const char *json)
{
    char body[128];
    snprintf(body, sizeof(body), "{\"settings\":%s}", json);
    JSON_Value *r = plexd_call("/settings", body, 5, NULL, 0);
    if (!r) return false;
    json_value_free(r);
    plexd_get_status(NULL);
    return true;
}

bool plexd_set_mute_games(bool on)
{
    return post_settings(on ? "{\"muteGames\":true}" : "{\"muteGames\":false}");
}

bool plexd_set_hotkeys(bool on)
{
    return post_settings(on ? "{\"hotkeys\":{\"enabled\":true}}" : "{\"hotkeys\":{\"enabled\":false}}");
}

void plexd_set_volume(float volume)
{
    char body[48];
    snprintf(body, sizeof(body), "{\"volume\":%.4f}", volume);
    JSON_Value *r = plexd_call("/volume", body, 3, NULL, 0);
    if (r) json_value_free(r);
}

/* Start plexd ourselves, detached, in case launch.sh couldn't. */
static void spawn_daemon(void)
{
    const char *bin = getenv("PLEXD_BIN");
    char path[1024];
    if (!bin || !bin[0]) {
        ssize_t n = readlink("/proc/self/exe", path, sizeof(path) - 8);
        if (n <= 0) return;
        path[n] = '\0';
        char *slash = strrchr(path, '/');
        if (!slash) return;
        strcpy(slash + 1, "plexd");
        bin = path;
    }
    const char *logf = getenv("PLEXD_LOG_FILE");
    PLEX_LOG("[plexd] starting player service: %s\n", bin);
    pid_t pid = fork();
    if (pid != 0) return;
    /* child: new session so it outlives the UI */
    setsid();
    int fd = open(logf && logf[0] ? logf : "/dev/null", O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd >= 0) {
        dup2(fd, 1);
        dup2(fd, 2);
    }
    int nul = open("/dev/null", O_RDONLY);
    if (nul >= 0) dup2(nul, 0);
    execl(bin, "plexd", "serve", (char *)NULL);
    _exit(127);
}

bool plexd_ensure_running(void)
{
    for (int i = 0; i < 30; i++) {      /* launch.sh should have started it */
        if (plexd_get_status(NULL)) return true;
        usleep(100 * 1000);
    }
    spawn_daemon();
    for (int i = 0; i < 50; i++) {
        if (plexd_get_status(NULL)) return true;
        usleep(100 * 1000);
    }
    PLEX_LOG_ERROR("[plexd] player service did not start\n");
    return false;
}
