#define _GNU_SOURCE
#include "plex_favorites.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>

#include "include/parson/parson.h"
#include "plex_log.h"
#include "file_util.h"
#include "plex_api.h"
#include "plex_config.h"
#include <pthread.h>

/* ------------------------------------------------------------------
 * Constants
 * ------------------------------------------------------------------ */

#define FAVORITES_FILENAME  "favorites.json"
#define FALLBACK_BASE_DIR   "/mnt/SDCARD/.userdata/shared/plexmusic"

/* ------------------------------------------------------------------
 * Module state
 * ------------------------------------------------------------------ */

static PlexTrack g_favs[PLEX_MAX_FAVORITES];
static int       g_fav_count = 0;

/* Favorites are mirrored to Plex as five-star ratings ("loved" in Plexamp).
 * A change that couldn't reach the server (offline, or the request failed)
 * waits here, saved with the favorites, until the next sync. */
#define PLEX_LOVED_RATING 10
#define MAX_PENDING       PLEX_MAX_FAVORITES

typedef struct { int rating_key; bool loved; } PendingRating;

static pthread_mutex_t g_pending_lock = PTHREAD_MUTEX_INITIALIZER;
static PendingRating   g_pending[MAX_PENDING];
static int             g_pending_count = 0;

static void favorites_save(void);

/* Record (or replace) the latest wanted state for a track. Caller holds
 * g_pending_lock. */
static void pending_set_locked(int rating_key, bool loved)
{
    for (int i = 0; i < g_pending_count; i++) {
        if (g_pending[i].rating_key == rating_key) {
            g_pending[i].loved = loved;
            return;
        }
    }
    if (g_pending_count < MAX_PENDING)
        g_pending[g_pending_count++] = (PendingRating){ rating_key, loved };
}

static void pending_drop_locked(int rating_key, bool loved)
{
    for (int i = 0; i < g_pending_count; i++) {
        if (g_pending[i].rating_key == rating_key && g_pending[i].loved == loved) {
            g_pending[i] = g_pending[--g_pending_count];
            return;
        }
    }
}

/* ------------------------------------------------------------------
 * Internal helpers
 * ------------------------------------------------------------------ */

static void favorites_path(char *out, int out_size)
{
    const char *base = getenv("SHARED_USERDATA_PATH");
    if (base && *base) {
        snprintf(out, out_size, "%s/plexmusic/%s", base, FAVORITES_FILENAME);
    } else {
        snprintf(out, out_size, "%s/%s", FALLBACK_BASE_DIR, FAVORITES_FILENAME);
    }
}

/* Ensure the directory portion of a path exists (one level only). */
static void ensure_dir(const char *file_path)
{
    char dir[640];
    strncpy(dir, file_path, sizeof(dir) - 1);
    dir[sizeof(dir) - 1] = '\0';

    char *slash = strrchr(dir, '/');
    if (!slash) return;
    *slash = '\0';

    /* Create parent directory first (best-effort) */
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

static void favorites_save(void)
{
    char path[640];
    favorites_path(path, sizeof(path));
    ensure_dir(path);

    JSON_Value  *root     = json_value_init_object();
    JSON_Object *root_obj = json_value_get_object(root);
    JSON_Value  *tracks_v = json_value_init_array();
    JSON_Array  *tracks_a = json_value_get_array(tracks_v);

    for (int i = 0; i < g_fav_count; i++) {
        const PlexTrack *t = &g_favs[i];

        JSON_Value  *trk_v   = json_value_init_object();
        JSON_Object *trk_obj = json_value_get_object(trk_v);

        json_object_set_number(trk_obj, "track_id",     (double)t->rating_key);
        json_object_set_number(trk_obj, "track_number", (double)t->track_number);
        json_object_set_number(trk_obj, "duration_ms",  (double)t->duration_ms);
        json_object_set_string(trk_obj, "title",        t->title);
        json_object_set_string(trk_obj, "artist",       t->artist);
        json_object_set_string(trk_obj, "album",        t->album);
        json_object_set_string(trk_obj, "media_key",    t->media_key);
        json_object_set_string(trk_obj, "thumb",        t->thumb);
        json_object_set_string(trk_obj, "audio_codec",  t->audio_codec);
        json_object_set_number(trk_obj, "bitrate_kbps", (double)t->audio_bitrate_kbps);

        json_array_append_value(tracks_a, trk_v);
    }

    JSON_Value *pend_v = json_value_init_array();
    JSON_Array *pend_a = json_value_get_array(pend_v);
    pthread_mutex_lock(&g_pending_lock);
    for (int i = 0; i < g_pending_count; i++) {
        JSON_Value  *pv = json_value_init_object();
        JSON_Object *po = json_value_get_object(pv);
        json_object_set_number(po, "track_id", (double)g_pending[i].rating_key);
        json_object_set_boolean(po, "loved", g_pending[i].loved);
        json_array_append_value(pend_a, pv);
    }
    pthread_mutex_unlock(&g_pending_lock);
    json_object_set_value(root_obj, "pending_ratings", pend_v);

    json_object_set_value(root_obj, "tracks", tracks_v);

    char *json_str = json_serialize_to_string_pretty(root);
    json_value_free(root);
    if (!json_str) {
        PLEX_LOG_ERROR("[Favorites] Failed to serialise favorites\n");
        return;
    }

    if (write_file_atomic(path, json_str, strlen(json_str)) != 0)
        PLEX_LOG_ERROR("[Favorites] Failed to write favorites: %s\n", path);
    json_free_serialized_string(json_str);
}

static void favorites_load(void)
{
    char path[640];
    favorites_path(path, sizeof(path));

    JSON_Value *root = json_parse_file(path);
    if (!root) {
        /* Missing file is not an error — start empty */
        PLEX_LOG("[Favorites] No favorites file found, starting empty\n");
        return;
    }

    JSON_Object *root_obj = json_value_get_object(root);
    if (!root_obj) {
        json_value_free(root);
        return;
    }

    JSON_Array *tracks_a = json_object_get_array(root_obj, "tracks");
    if (!tracks_a) {
        json_value_free(root);
        return;
    }

    int count = (int)json_array_get_count(tracks_a);
    if (count > PLEX_MAX_FAVORITES)
        count = PLEX_MAX_FAVORITES;

    g_fav_count = 0;
    for (int i = 0; i < count; i++) {
        JSON_Object *trk_obj = json_array_get_object(tracks_a, i);
        if (!trk_obj) continue;

        PlexTrack *t = &g_favs[g_fav_count];
        memset(t, 0, sizeof(*t));

        t->rating_key   = (int)json_object_get_number(trk_obj, "track_id");
        t->track_number = (int)json_object_get_number(trk_obj, "track_number");
        t->duration_ms  = (int)json_object_get_number(trk_obj, "duration_ms");

        const char *title     = json_object_get_string(trk_obj, "title");
        const char *artist    = json_object_get_string(trk_obj, "artist");
        const char *album     = json_object_get_string(trk_obj, "album");
        const char *media_key = json_object_get_string(trk_obj, "media_key");
        const char *thumb     = json_object_get_string(trk_obj, "thumb");
        const char *codec     = json_object_get_string(trk_obj, "audio_codec");
        t->audio_bitrate_kbps = (int)json_object_get_number(trk_obj, "bitrate_kbps");
        if (codec)     strncpy(t->audio_codec, codec, sizeof(t->audio_codec) - 1);

        if (title)     strncpy(t->title,     title,     sizeof(t->title) - 1);
        if (artist)    strncpy(t->artist,    artist,    sizeof(t->artist) - 1);
        if (album)     strncpy(t->album,     album,     sizeof(t->album) - 1);
        if (media_key) strncpy(t->media_key, media_key, sizeof(t->media_key) - 1);
        if (thumb)     strncpy(t->thumb,     thumb,     sizeof(t->thumb) - 1);
        /* local_path is runtime-only — leave as empty string (memset above covers it) */

        g_fav_count++;
    }

    JSON_Array *pend_a = json_object_get_array(root_obj, "pending_ratings");
    size_t np = pend_a ? json_array_get_count(pend_a) : 0;
    pthread_mutex_lock(&g_pending_lock);
    g_pending_count = 0;
    for (size_t i = 0; i < np; i++) {
        JSON_Object *po = json_array_get_object(pend_a, i);
        if (po)
            pending_set_locked((int)json_object_get_number(po, "track_id"),
                               json_object_get_boolean(po, "loved") == 1);
    }
    pthread_mutex_unlock(&g_pending_lock);

    json_value_free(root);
    PLEX_LOG("[Favorites] Loaded %d favorites (%d ratings to send)\n", g_fav_count, g_pending_count);
}

/* ------------------------------------------------------------------
 * Public API
 * ------------------------------------------------------------------ */

void plex_favorites_init(void)
{
    g_fav_count = 0;
    favorites_load();
}

void plex_favorites_quit(void)
{
    /* No-op: static array, nothing to free */
}

/* ------------------------------------------------------------------
 * Mirroring to Plex ratings
 * ------------------------------------------------------------------ */

typedef struct {
    char server_url[PLEX_MAX_URL];
    char token[PLEX_MAX_STR];
    int  rating_key;
    bool loved;
} RateJob;

static void *rate_worker(void *arg)
{
    RateJob *job = arg;
    PlexConfig cfg;
    memset(&cfg, 0, sizeof(cfg));
    snprintf(cfg.server_url, sizeof(cfg.server_url), "%s", job->server_url);
    snprintf(cfg.token, sizeof(cfg.token), "%s", job->token);
    if (plex_api_rate(&cfg, job->rating_key, job->loved ? PLEX_LOVED_RATING : -1) == 0) {
        pthread_mutex_lock(&g_pending_lock);
        pending_drop_locked(job->rating_key, job->loved);
        pthread_mutex_unlock(&g_pending_lock);
    } else {
        PLEX_LOG_ERROR("[Favorites] rating %d not sent; will retry on sync\n", job->rating_key);
    }
    free(job);
    return NULL;
}

/* Queue the change, then send it in the background when online; it leaves
 * the queue once the server takes it. */
static void push_rating(int rating_key, bool loved)
{
    pthread_mutex_lock(&g_pending_lock);
    pending_set_locked(rating_key, loved);
    pthread_mutex_unlock(&g_pending_lock);

    const PlexConfig *cfg = plex_config_get_mutable();
    if (cfg->offline_mode || !plex_config_is_valid(cfg)) return;

    RateJob *job = calloc(1, sizeof(*job));
    if (!job) return;
    snprintf(job->server_url, sizeof(job->server_url), "%s", cfg->server_url);
    snprintf(job->token, sizeof(job->token), "%s", cfg->token);
    job->rating_key = rating_key;
    job->loved      = loved;
    pthread_t th;
    if (pthread_create(&th, NULL, rate_worker, job) == 0) pthread_detach(th);
    else free(job);
}

int plex_favorites_sync_with_plex(const PlexConfig *cfg)
{
    if (!cfg || cfg->offline_mode || !plex_config_is_valid(cfg)) return -1;

    /* 1. Send queued changes, so a favorite removed offline isn't pulled
     *    straight back in below. */
    PendingRating todo[MAX_PENDING];
    pthread_mutex_lock(&g_pending_lock);
    int n = g_pending_count;
    memcpy(todo, g_pending, n * sizeof(PendingRating));
    pthread_mutex_unlock(&g_pending_lock);
    for (int i = 0; i < n; i++) {
        if (plex_api_rate(cfg, todo[i].rating_key,
                          todo[i].loved ? PLEX_LOVED_RATING : -1) != 0)
            return -1;   /* server unreachable: keep everything for next time */
        pthread_mutex_lock(&g_pending_lock);
        pending_drop_locked(todo[i].rating_key, todo[i].loved);
        pthread_mutex_unlock(&g_pending_lock);
    }

    /* 2. Add tracks loved elsewhere (Plexamp, Plex Web). */
    int added = 0;
    if (cfg->library_id > 0) {
        static PlexTrack loved[PLEX_MAX_FAVORITES];
        int count = 0;
        if (plex_api_get_loved_tracks(cfg, cfg->library_id, loved, PLEX_MAX_FAVORITES, &count) == 0) {
            for (int i = 0; i < count && g_fav_count < PLEX_MAX_FAVORITES; i++) {
                if (plex_favorites_contains(loved[i].rating_key)) continue;
                g_favs[g_fav_count] = loved[i];
                g_favs[g_fav_count].local_path[0] = '\0';
                g_fav_count++;
                added++;
            }
        }
    }
    favorites_save();
    PLEX_LOG("[Favorites] synced with Plex: %d sent, %d added\n", n, added);
    return added;
}

bool plex_favorites_toggle(const PlexTrack *t)
{
    if (!t) return false;

    /* Linear scan: if found, remove by shifting left and save */
    for (int i = 0; i < g_fav_count; i++) {
        if (g_favs[i].rating_key == t->rating_key) {
            if (i < g_fav_count - 1)
                memmove(&g_favs[i], &g_favs[i + 1],
                        (g_fav_count - i - 1) * sizeof(PlexTrack));
            g_fav_count--;
            push_rating(t->rating_key, false);
            favorites_save();
            return false;
        }
    }

    /* Not found: append if room */
    if (g_fav_count >= PLEX_MAX_FAVORITES) {
        PLEX_LOG_ERROR("[Favorites] Favorites list full (%d), cannot add\n",
                       PLEX_MAX_FAVORITES);
        return false;
    }

    g_favs[g_fav_count] = *t;
    /* Clear runtime-only field */
    g_favs[g_fav_count].local_path[0] = '\0';
    g_fav_count++;
    push_rating(t->rating_key, true);
    favorites_save();
    return true;
}

bool plex_favorites_contains(int rating_key)
{
    for (int i = 0; i < g_fav_count; i++) {
        if (g_favs[i].rating_key == rating_key)
            return true;
    }
    return false;
}

int plex_favorites_get(PlexTrack *out, int max)
{
    if (!out || max <= 0) return 0;
    int n = g_fav_count < max ? g_fav_count : max;
    memcpy(out, g_favs, n * sizeof(PlexTrack));
    return n;
}

int plex_favorites_count(void)
{
    return g_fav_count;
}
