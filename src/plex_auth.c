#define _GNU_SOURCE
#include "plex_auth.h"
#include "plex_net.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "include/parson/parson.h"
#include "api.h"
#include "plex_config.h"
#include "plex_log.h"

#define PLEX_TV_API "https://plex.tv/api/v2"
/* resources lists every device on the account, with relay entries */
#define RESP_BUF_SIZE (256 * 1024)

/* ------------------------------------------------------------------
 * plex_auth_create_pin
 * POST https://plex.tv/api/v2/pins
 * body: strong=true
 * Response: { "id": 12345, "code": "ABCD" }
 * ------------------------------------------------------------------ */
int plex_auth_create_pin(PlexPin *pin)
{
    if (!pin) return -1;
    memset(pin, 0, sizeof(*pin));

    uint8_t *buf = (uint8_t *)malloc(RESP_BUF_SIZE);
    if (!buf) return -1;

    PlexNetOptions opts = {
        .method      = PLEX_HTTP_POST,
        .body        = "strong=false",
        .token       = NULL,
        .timeout_sec = 15
    };

    int n = plex_net_fetch(PLEX_TV_API "/pins", buf, RESP_BUF_SIZE, &opts);
    if (n <= 0) {
        PLEX_LOG_ERROR("[PlexAuth] create_pin: fetch failed\n");
        free(buf);
        return -1;
    }
    buf[n] = '\0';

    JSON_Value  *root = json_parse_string((const char *)buf);
    free(buf);
    if (!root) {
        PLEX_LOG_ERROR("[PlexAuth] create_pin: JSON parse failed\n");
        return -1;
    }

    JSON_Object *obj = json_value_get_object(root);
    if (!obj) {
        json_value_free(root);
        return -1;
    }

    double id_d       = json_object_get_number(obj, "id");
    const char *code  = json_object_get_string(obj, "code");

    if (id_d == 0.0 || !code || !*code) {
        PLEX_LOG_ERROR("[PlexAuth] create_pin: missing id or code in response\n");
        json_value_free(root);
        return -1;
    }

    pin->pin_id = (int)id_d;
    strncpy(pin->pin_code, code, sizeof(pin->pin_code) - 1);

    json_value_free(root);
    return 0;
}

/* ------------------------------------------------------------------
 * plex_auth_check_pin
 * GET https://plex.tv/api/v2/pins/{pin_id}
 * Response: { "authToken": "TOKEN" }  (null/empty while pending)
 * Returns:  1 = authorized, 0 = pending, -1 = error
 * ------------------------------------------------------------------ */
int plex_auth_check_pin(PlexPin *pin)
{
    if (!pin || pin->pin_id == 0) return -1;

    uint8_t *buf = (uint8_t *)malloc(RESP_BUF_SIZE);
    if (!buf) return -1;

    char url[256];
    snprintf(url, sizeof(url), "%s/pins/%d", PLEX_TV_API, pin->pin_id);

    PlexNetOptions opts = {
        .method      = PLEX_HTTP_GET,
        .body        = NULL,
        .token       = NULL,
        .timeout_sec = 15
    };

    int n = plex_net_fetch(url, buf, RESP_BUF_SIZE, &opts);
    if (n <= 0) {
        PLEX_LOG_ERROR("[PlexAuth] check_pin: fetch failed\n");
        free(buf);
        return -1;
    }
    buf[n] = '\0';

    JSON_Value  *root = json_parse_string((const char *)buf);
    free(buf);
    if (!root) {
        PLEX_LOG_ERROR("[PlexAuth] check_pin: JSON parse failed\n");
        return -1;
    }

    JSON_Object *obj = json_value_get_object(root);
    if (!obj) { json_value_free(root); return -1; }

    const char *auth_token = json_object_get_string(obj, "authToken");
    int result = 0;

    if (auth_token && *auth_token) {
        strncpy(pin->token, auth_token, sizeof(pin->token) - 1);
        result = 1;
    }

    json_value_free(root);
    return result;
}

/* ------------------------------------------------------------------
 * conn_rank — lower is better
 * Tier 0: local=true
 * Tier 1: local=false, relay=false
 * Tier 2: relay=true
 * Within a tier, prefer non-plex.direct (direct IP) over plex.direct —
 * plex.direct requires Plex's DNS to resolve and may fail on some networks.
 * Within each (tier, plex_direct) pair, prefer HTTPS over HTTP.
 * Final rank = tier*4 + plex_direct*2 + https_penalty
 * ------------------------------------------------------------------ */
static int conn_rank(JSON_Object *conn)
{
    int local = json_object_get_boolean(conn, "local");
    int relay = json_object_get_boolean(conn, "relay");
    const char *proto = json_object_get_string(conn, "protocol");
    const char *uri   = json_object_get_string(conn, "uri");

    int tier;
    if (local == 1)
        tier = 0;
    else if (relay == 1)
        tier = 2;
    else
        tier = 1;

    int plex_direct   = (uri && strstr(uri, "plex.direct")) ? 1 : 0;
    int https_penalty = (proto && strcmp(proto, "https") == 0) ? 0 : 1;
    return tier * 4 + plex_direct * 2 + https_penalty;
}

static PlexConnKind conn_kind(JSON_Object *conn)
{
    if (json_object_get_boolean(conn, "local") == 1) return PLEX_CONN_LOCAL;
    if (json_object_get_boolean(conn, "relay") == 1) return PLEX_CONN_RELAY;
    return PLEX_CONN_REMOTE;
}

/* ------------------------------------------------------------------
 * ranked_conn_uris — copy the connection URIs into out[] (and their kinds
 * into kinds[]), best rank first.
 * Equal ranks keep plex.tv's order; duplicate URIs are dropped.
 * Returns the number of URIs written (0 if none).
 * ------------------------------------------------------------------ */
static int ranked_conn_uris(JSON_Array *conns, char out[][PLEX_MAX_URL],
                            PlexConnKind kinds[], int max)
{
    if (!conns) return 0;

    int ranks[PLEX_MAX_CONNS];
    int count = 0;
    size_t n = json_array_get_count(conns);

    for (size_t i = 0; i < n; i++) {
        JSON_Object *c = json_array_get_object(conns, i);
        if (!c) continue;
        const char *uri = json_object_get_string(c, "uri");
        if (!uri || !*uri || strlen(uri) >= PLEX_MAX_URL) continue;

        bool dup = false;
        for (int j = 0; j < count; j++)
            if (strcmp(out[j], uri) == 0) { dup = true; break; }
        if (dup) continue;

        /* Insertion sort: find the slot after every entry ranked <= this one */
        int rank = conn_rank(c);
        int pos = count;
        while (pos > 0 && ranks[pos - 1] > rank) pos--;
        if (pos >= max) continue;   /* list full of better-ranked URIs */

        int last = (count < max) ? count : max - 1;
        for (int j = last; j > pos; j--) {
            memcpy(out[j], out[j - 1], PLEX_MAX_URL);
            ranks[j] = ranks[j - 1];
            kinds[j] = kinds[j - 1];
        }
        strcpy(out[pos], uri);
        ranks[pos] = rank;
        kinds[pos] = conn_kind(c);
        if (count < max) count++;
    }
    return count;
}

/* ------------------------------------------------------------------
 * plex_auth_get_servers
 * GET https://plex.tv/api/v2/resources?includeHttps=1&includeRelay=1
 * Filters for provides containing "server".
 * Keeps every connection URI, ranked local > remote > relay (HTTPS
 * preferred within tier).
 * ------------------------------------------------------------------ */
int plex_auth_get_servers(const char *token, PlexServer servers[], int *count)
{
    if (!token || !servers || !count) return -1;
    *count = 0;

    uint8_t *buf = (uint8_t *)malloc(RESP_BUF_SIZE);
    if (!buf) return -1;

    PlexNetOptions opts = {
        .method      = PLEX_HTTP_GET,
        .body        = NULL,
        .token       = token,
        .timeout_sec = 15
    };

    int n = plex_net_fetch(PLEX_TV_API "/resources?includeHttps=1&includeRelay=1",
                           buf, RESP_BUF_SIZE, &opts);
    if (n <= 0) {
        PLEX_LOG_ERROR("[PlexAuth] get_servers: fetch failed\n");
        free(buf);
        return -1;
    }
    buf[n] = '\0';

    JSON_Value *root = json_parse_string((const char *)buf);
    free(buf);
    if (!root) {
        PLEX_LOG_ERROR("[PlexAuth] get_servers: JSON parse failed\n");
        return -1;
    }

    /* Response is a JSON array */
    JSON_Array *arr = json_value_get_array(root);
    if (!arr) {
        json_value_free(root);
        return -1;
    }

    size_t total = json_array_get_count(arr);
    for (size_t i = 0; i < total && *count < PLEX_MAX_SERVERS; i++) {
        JSON_Object *res = json_array_get_object(arr, i);
        if (!res) continue;

        const char *provides = json_object_get_string(res, "provides");
        if (!provides || !strstr(provides, "server")) continue;

        PlexServer *sv = &servers[*count];
        memset(sv, 0, sizeof(*sv));

        JSON_Array *conns = json_object_get_array(res, "connections");
        sv->conn_count = ranked_conn_uris(conns, sv->conn_urls, sv->conn_kinds,
                                          PLEX_MAX_CONNS);
        if (sv->conn_count == 0) continue;

        const char *name = json_object_get_string(res, "name");
        const char *cid  = json_object_get_string(res, "clientIdentifier");
        int owned        = json_object_get_boolean(res, "owned");  /* -1 = missing */

        if (name) strncpy(sv->name, name, sizeof(sv->name) - 1);
        if (cid)  strncpy(sv->id,   cid,  sizeof(sv->id) - 1);
        sv->owned = (owned == 1);

        PLEX_LOG("[PlexAuth] server[%d]: name=%s conns=%d\n",
                 *count, sv->name, sv->conn_count);
        for (int c = 0; c < sv->conn_count; c++)
            PLEX_LOG("[PlexAuth]   conn[%d]: %s\n", c, sv->conn_urls[c]);

        (*count)++;
    }

    json_value_free(root);
    return 0;
}

/* ------------------------------------------------------------------
 * plex_auth_apply_server
 * ------------------------------------------------------------------ */
void plex_auth_apply_server(PlexConfig *cfg, const PlexServer *sv)
{
    if (!cfg || !sv) return;

    strncpy(cfg->server_name, sv->name, PLEX_MAX_STR - 1);
    cfg->server_name[PLEX_MAX_STR - 1] = '\0';
    strncpy(cfg->server_id, sv->id, PLEX_MAX_STR - 1);
    cfg->server_id[PLEX_MAX_STR - 1] = '\0';

    memcpy(cfg->conn_urls, sv->conn_urls, sizeof(cfg->conn_urls));
    memcpy(cfg->conn_kinds, sv->conn_kinds, sizeof(cfg->conn_kinds));
    cfg->conn_count = sv->conn_count;
    if (cfg->conn_count > 0) {
        strncpy(cfg->server_url, cfg->conn_urls[0], PLEX_MAX_URL - 1);
        cfg->server_url[PLEX_MAX_URL - 1] = '\0';
        cfg->server_kind = cfg->conn_kinds[0];
    }
}

/* ------------------------------------------------------------------
 * plex_auth_refresh_server_urls
 * GET https://plex.tv/api/v2/resources?includeHttps=1&includeRelay=1
 * Finds the resource matching cfg->server_id, then replaces
 * cfg->conn_urls with fresh connection URIs.
 * Returns 0 on success, -1 on failure (cfg unchanged).
 * ------------------------------------------------------------------ */
int plex_auth_refresh_server_urls(PlexConfig *cfg)
{
    if (!cfg || cfg->token[0] == '\0' || cfg->server_id[0] == '\0')
        return -1;

    uint8_t *buf = (uint8_t *)malloc(RESP_BUF_SIZE);
    if (!buf) return -1;

    PlexNetOptions opts = {
        .method      = PLEX_HTTP_GET,
        .body        = NULL,
        .token       = cfg->token,
        .timeout_sec = 5,
        .no_persist  = true
    };

    int n = plex_net_fetch(PLEX_TV_API "/resources?includeHttps=1&includeRelay=1",
                           buf, RESP_BUF_SIZE, &opts);
    if (n <= 0) {
        PLEX_LOG("[Auth] server URL refresh failed (using cached)\n");
        free(buf);
        return -1;
    }
    buf[n] = '\0';

    JSON_Value *root = json_parse_string((const char *)buf);
    free(buf);
    if (!root) {
        PLEX_LOG("[Auth] server URL refresh failed (using cached)\n");
        return -1;
    }

    JSON_Array *arr = json_value_get_array(root);
    if (!arr) {
        json_value_free(root);
        PLEX_LOG("[Auth] server URL refresh failed (using cached)\n");
        return -1;
    }

    size_t total = json_array_get_count(arr);
    int found = 0;
    for (size_t i = 0; i < total; i++) {
        JSON_Object *res = json_array_get_object(arr, i);
        if (!res) continue;

        /* v2 resources identify servers by clientIdentifier */
        const char *cid = json_object_get_string(res, "clientIdentifier");
        if (!cid) cid = json_object_get_string(res, "machineIdentifier");
        if (!cid || strcmp(cid, cfg->server_id) != 0) continue;

        char conns_buf[PLEX_MAX_CONNS][PLEX_MAX_URL];
        PlexConnKind kinds_buf[PLEX_MAX_CONNS];
        int conn_count = ranked_conn_uris(json_object_get_array(res, "connections"),
                                          conns_buf, kinds_buf, PLEX_MAX_CONNS);
        if (conn_count > 0) {
            memcpy(cfg->conn_urls, conns_buf, sizeof(cfg->conn_urls));
            memcpy(cfg->conn_kinds, kinds_buf, sizeof(cfg->conn_kinds));
            cfg->conn_count = conn_count;
            plex_config_save(cfg);
            PLEX_LOG("[Auth] server URLs refreshed: %d connections\n", conn_count);
        }

        found = 1;
        break;
    }

    json_value_free(root);

    if (!found) {
        PLEX_LOG("[Auth] server URL refresh failed (server not in resources, using cached)\n");
        return -1;
    }

    return 0;
}

/* ------------------------------------------------------------------
 * plex_auth_select_connection
 * Probe {conn}/identity for each cfg->conn_urls entry in rank order and
 * use the first that answers as cfg->server_url for this session.
 * ------------------------------------------------------------------ */
int plex_auth_select_connection(PlexConfig *cfg)
{
    if (!cfg || cfg->conn_count <= 0) return -1;

    uint8_t probe_buf[4096];
    /* Short timeout and no retries: an unreachable address should cost
     * seconds, not the full 15s default with retries. */
    PlexNetOptions probe_opts = {
        .method      = PLEX_HTTP_GET,
        .body        = NULL,
        .token       = cfg->token,
        .timeout_sec = 2,
        .no_persist  = true,
        .no_retry    = true
    };

    for (int i = 0; i < cfg->conn_count; i++) {
        char identity_url[PLEX_MAX_URL + 16];
        snprintf(identity_url, sizeof(identity_url), "%s/identity", cfg->conn_urls[i]);

        if (plex_net_fetch(identity_url, probe_buf, (int)sizeof(probe_buf), &probe_opts) > 0) {
            strncpy(cfg->server_url, cfg->conn_urls[i], PLEX_MAX_URL - 1);
            cfg->server_url[PLEX_MAX_URL - 1] = '\0';
            cfg->server_kind = cfg->conn_kinds[i];
            PLEX_LOG("[Auth] using connection %d/%d (%s): %s\n",
                     i + 1, cfg->conn_count, plex_conn_kind_label(cfg->server_kind),
                     cfg->server_url);
            return 0;
        }
        PLEX_LOG("[Auth] connection %d/%d unreachable: %s\n",
                 i + 1, cfg->conn_count, cfg->conn_urls[i]);
    }

    /* Nothing answered: fall back to the best-ranked URL so browse shows its
     * normal network error and retry flow. */
    strncpy(cfg->server_url, cfg->conn_urls[0], PLEX_MAX_URL - 1);
    cfg->server_url[PLEX_MAX_URL - 1] = '\0';
    cfg->server_kind = cfg->conn_kinds[0];
    PLEX_LOG_ERROR("[Auth] no connection reachable; using %s\n", cfg->server_url);
    return -1;
}
