#define _GNU_SOURCE
#include "plex_config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <errno.h>
#include <time.h>

#include "include/parson/parson.h"
#include "api.h"
#include "plex_log.h"
#include "file_util.h"

#define PLEX_CONFIG_FALLBACK_DIR "/mnt/SDCARD/.userdata/shared/plexmusic"
#define PLEX_CONFIG_FILENAME     "config.json"

/* Build the path to config.json into `out` (caller supplies buffer). */
static void config_path(char *out, int out_size)
{
    const char *base = getenv("SHARED_USERDATA_PATH");
    if (base && *base) {
        snprintf(out, out_size, "%s/plexmusic/%s", base, PLEX_CONFIG_FILENAME);
    } else {
        snprintf(out, out_size, "%s/%s",
                 PLEX_CONFIG_FALLBACK_DIR, PLEX_CONFIG_FILENAME);
    }
}

/* Ensure the directory portion of a path exists (one level only). */
static void ensure_config_dir(const char *file_path)
{
    char dir[640];
    strncpy(dir, file_path, sizeof(dir) - 1);
    dir[sizeof(dir) - 1] = '\0';

    char *slash = strrchr(dir, '/');
    if (!slash) return;
    *slash = '\0';

    /* Create parent of directory first (best-effort) */
    char parent[640];
    strncpy(parent, dir, sizeof(parent) - 1);
    parent[sizeof(parent) - 1] = '\0';
    char *pslash = strrchr(parent, '/');
    if (pslash) {
        *pslash = '\0';
        mkdir(parent, 0755);
    }

    mkdir(dir, 0755);
}

/* conn_kinds as stored in config.json */
static const char *const CONN_KIND_NAMES[] = { "local", "remote", "relay" };

static PlexConnKind conn_kind_from_name(const char *name)
{
    for (int i = 0; i < 3; i++)
        if (name && strcmp(name, CONN_KIND_NAMES[i]) == 0) return (PlexConnKind)i;
    return PLEX_CONN_REMOTE;
}

/* ------------------------------------------------------------------
 * Public API
 * ------------------------------------------------------------------ */

const char *plex_conn_kind_name(PlexConnKind kind)
{
    return ((unsigned)kind < 3) ? CONN_KIND_NAMES[kind] : "remote";
}

const char *plex_conn_kind_label(PlexConnKind kind)
{
    switch (kind) {
        case PLEX_CONN_LOCAL: return "Local";
        case PLEX_CONN_RELAY: return "Relay";
        default:              return "Remote";
    }
}

int plex_config_stream_kbps(const PlexConfig *cfg)
{
    if (cfg->stream_bitrate_kbps == 0 && cfg->server_kind == PLEX_CONN_RELAY)
        return PLEX_RELAY_STREAM_KBPS;
    return cfg->stream_bitrate_kbps;
}

int plex_config_load(PlexConfig *cfg)
{
    if (!cfg) return -1;
    memset(cfg, 0, sizeof(*cfg));

    char path[640];
    config_path(path, sizeof(path));

    JSON_Value *root = json_parse_file(path);
    if (!root) {
        PLEX_LOG_ERROR("[PlexConfig] Could not parse: %s\n", path);
        return -1;
    }

    JSON_Object *obj = json_value_get_object(root);
    if (!obj) {
        json_value_free(root);
        return -1;
    }

    const char *token       = json_object_get_string(obj, "token");
    const char *server_url  = json_object_get_string(obj, "server_url");
    const char *server_name = json_object_get_string(obj, "server_name");
    const char *server_id   = json_object_get_string(obj, "server_id");
    const char *client_id   = json_object_get_string(obj, "client_id");

    if (token)
        strncpy(cfg->token, token, sizeof(cfg->token) - 1);
    if (server_url)
        strncpy(cfg->server_url, server_url, sizeof(cfg->server_url) - 1);

    JSON_Array *conns = json_object_get_array(obj, "connections");
    if (conns) {
        size_t n = json_array_get_count(conns);
        for (size_t i = 0; i < n && cfg->conn_count < PLEX_MAX_CONNS; i++) {
            /* {"uri","kind"} objects; bare URI strings from earlier builds */
            JSON_Object *c  = json_array_get_object(conns, i);
            const char *uri = c ? json_object_get_string(c, "uri")
                                : json_array_get_string(conns, i);
            if (!uri || !*uri) continue;
            strncpy(cfg->conn_urls[cfg->conn_count], uri, PLEX_MAX_URL - 1);
            cfg->conn_kinds[cfg->conn_count] =
                conn_kind_from_name(c ? json_object_get_string(c, "kind") : NULL);
            cfg->conn_count++;
        }
    } else {
        /* Configs from before "connections" only kept server_url + relay_url.
         * Their kinds are unknown; treat both as remote until the startup
         * refresh replaces the list. */
        const char *relay_url = json_object_get_string(obj, "relay_url");
        if (server_url && *server_url) {
            strncpy(cfg->conn_urls[cfg->conn_count], server_url, PLEX_MAX_URL - 1);
            cfg->conn_kinds[cfg->conn_count++] = PLEX_CONN_REMOTE;
        }
        if (relay_url && *relay_url &&
            !(server_url && strcmp(relay_url, server_url) == 0)) {
            strncpy(cfg->conn_urls[cfg->conn_count], relay_url, PLEX_MAX_URL - 1);
            cfg->conn_kinds[cfg->conn_count++] = PLEX_CONN_REMOTE;
        }
    }

    if (server_name)
        strncpy(cfg->server_name, server_name, sizeof(cfg->server_name) - 1);
    if (server_id)
        strncpy(cfg->server_id, server_id, sizeof(cfg->server_id) - 1);
    if (client_id)
        strncpy(cfg->client_id, client_id, sizeof(cfg->client_id) - 1);

    cfg->offline_mode = (json_object_get_boolean(obj, "offline_mode") == 1);

    {
        int st = (int)json_object_get_number(obj, "screen_timeout");
        cfg->screen_timeout = (st > 0) ? st : 30;
    }

    cfg->library_id = (int)json_object_get_number(obj, "library_id");
    const char *library_name = json_object_get_string(obj, "library_name");
    if (library_name)
        strncpy(cfg->library_name, library_name, sizeof(cfg->library_name) - 1);

    if (json_object_has_value(obj, "stream_bitrate_kbps"))
        cfg->stream_bitrate_kbps = (int)json_object_get_number(obj, "stream_bitrate_kbps");
    else
        cfg->stream_bitrate_kbps = 128;

    if (json_object_has_value(obj, "download_bitrate_kbps"))
        cfg->download_bitrate_kbps = (int)json_object_get_number(obj, "download_bitrate_kbps");
    else
        cfg->download_bitrate_kbps = 128;

    /* Default true: pocket lock on unless explicitly saved as false */
    if (json_object_has_value(obj, "pocket_lock_enabled"))
        cfg->pocket_lock_enabled = (json_object_get_boolean(obj, "pocket_lock_enabled") == 1);
    else
        cfg->pocket_lock_enabled = true;

    if (json_object_has_value(obj, "preload_count")) {
        cfg->preload_count = (int)json_object_get_number(obj, "preload_count");
        if (cfg->preload_count < 0)  cfg->preload_count = 0;
        if (cfg->preload_count > 10) cfg->preload_count = 10;
    } else {
        cfg->preload_count = 2;
    }

    json_value_free(root);
    return 0;
}

int plex_config_save(const PlexConfig *cfg)
{
    if (!cfg) return -1;

    char path[640];
    config_path(path, sizeof(path));
    ensure_config_dir(path);

    JSON_Value  *root = json_value_init_object();
    JSON_Object *obj  = json_value_get_object(root);

    json_object_set_string(obj, "token",        cfg->token);
    json_object_set_string(obj, "server_url",   cfg->server_url);
    JSON_Value *conns_val = json_value_init_array();
    JSON_Array *conns     = json_value_get_array(conns_val);
    for (int i = 0; i < cfg->conn_count; i++) {
        JSON_Value  *c_val = json_value_init_object();
        JSON_Object *c     = json_value_get_object(c_val);
        json_object_set_string(c, "uri",  cfg->conn_urls[i]);
        json_object_set_string(c, "kind", plex_conn_kind_name(cfg->conn_kinds[i]));
        json_array_append_value(conns, c_val);
    }
    json_object_set_value(obj, "connections", conns_val);
    json_object_set_string(obj, "server_name",  cfg->server_name);
    json_object_set_string(obj, "server_id",    cfg->server_id);
    json_object_set_string(obj, "client_id",    cfg->client_id);
    json_object_set_boolean(obj, "offline_mode", cfg->offline_mode ? 1 : 0);
    json_object_set_number(obj, "screen_timeout", cfg->screen_timeout);
    json_object_set_number(obj, "library_id",     cfg->library_id);
    json_object_set_string(obj, "library_name",   cfg->library_name);
    json_object_set_number(obj, "stream_bitrate_kbps",   cfg->stream_bitrate_kbps);
    json_object_set_number(obj, "download_bitrate_kbps", cfg->download_bitrate_kbps);
    json_object_set_boolean(obj, "pocket_lock_enabled",  cfg->pocket_lock_enabled ? 1 : 0);
    json_object_set_number(obj, "preload_count",         cfg->preload_count);

    char *json_str = json_serialize_to_string_pretty(root);
    json_value_free(root);
    if (!json_str) return -1;

    int rc = write_file_atomic(path, json_str, strlen(json_str));
    json_free_serialized_string(json_str);
    if (rc != 0) {
        PLEX_LOG_ERROR("[PlexConfig] Failed to write: %s\n", path);
        return -1;
    }
    return 0;
}

void plex_config_new_client_id(PlexConfig *cfg)
{
    unsigned char rnd[8] = {0};
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd < 0 || read(fd, rnd, sizeof(rnd)) != (ssize_t)sizeof(rnd)) {
        /* No urandom: still unique enough per device */
        unsigned long seed = (unsigned long)time(NULL) ^ ((unsigned long)getpid() << 16);
        for (size_t i = 0; i < sizeof(rnd); i++) {
            seed = seed * 1103515245UL + 12345UL;
            rnd[i] = (unsigned char)(seed >> 16);
        }
    }
    if (fd >= 0) close(fd);

    int n = snprintf(cfg->client_id, sizeof(cfg->client_id), "plexmusic-");
    for (size_t i = 0; i < sizeof(rnd); i++)
        n += snprintf(cfg->client_id + n, sizeof(cfg->client_id) - n, "%02x", rnd[i]);
}

void plex_config_ensure_client_id(PlexConfig *cfg)
{
    if (cfg->client_id[0]) return;
    if (cfg->token[0])
        /* Signed in before per-device ids: the token was issued to the old
         * shared id, so keep sending it until the next sign-in. */
        snprintf(cfg->client_id, sizeof(cfg->client_id), "%s", PLEX_LEGACY_CLIENT_ID);
    else
        plex_config_new_client_id(cfg);
}

bool plex_config_is_valid(const PlexConfig *cfg)
{
    return cfg &&
           cfg->token[0]      != '\0' &&
           cfg->server_url[0] != '\0';
}
