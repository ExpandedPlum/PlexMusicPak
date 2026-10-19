#ifndef PLEX_AUTH_H
#define PLEX_AUTH_H

#include "plex_models.h"

typedef struct {
    int  pin_id;
    char pin_code[8];            /* 4-char code shown to user */
    char token[PLEX_MAX_STR];   /* filled in when auth completes */
} PlexPin;

/*
 * Create a new PIN. Returns 0 on success, -1 on error.
 * Calls POST https://plex.tv/api/v2/pins
 * Fills pin->pin_id and pin->pin_code.
 */
int plex_auth_create_pin(PlexPin *pin);

/*
 * Poll for PIN auth completion. Returns:
 *   1  — authorized (pin->token filled in)
 *   0  — not yet authorized
 *  -1  — error or expired
 * Calls GET https://plex.tv/api/v2/pins/{pin->pin_id}
 */
int plex_auth_check_pin(PlexPin *pin);

/*
 * Fetch list of available Plex servers for the authenticated token.
 * Calls GET https://plex.tv/api/v2/resources?includeHttps=1&includeRelay=1
 * Fills servers[] array (each with its ranked connection URIs), sets *count.
 * Returns 0 on success.
 */
#define PLEX_MAX_SERVERS 16
int plex_auth_get_servers(const char *token, PlexServer servers[], int *count);

/*
 * Copy a server's name, id and connection URIs into cfg.
 * Sets cfg->server_url to the best-ranked URI; call
 * plex_auth_select_connection() to pick one that is reachable.
 */
void plex_auth_apply_server(PlexConfig *cfg, const PlexServer *sv);

/*
 * Refresh cfg->conn_urls from Plex TV on startup.
 * Calls GET https://plex.tv/api/v2/resources?includeHttps=1&includeRelay=1
 * and finds the resource matching cfg->server_id.
 * Returns 0 on success, -1 if skipped (no token/server_id) or fetch failed.
 * On failure, cfg is unchanged (cached values remain intact).
 */
int plex_auth_refresh_server_urls(PlexConfig *cfg);

/*
 * Probe cfg->conn_urls in rank order (local, remote, relay) and set
 * cfg->server_url to the first that answers, for this session.
 * Returns 0 if one answered, -1 if none did (server_url is then the
 * best-ranked URI so browse shows its usual network error).
 */
int plex_auth_select_connection(PlexConfig *cfg);

#endif /* PLEX_AUTH_H */
