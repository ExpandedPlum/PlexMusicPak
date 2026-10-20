#ifndef PLEX_CONFIG_H
#define PLEX_CONFIG_H

#include <stdbool.h>
#include "plex_models.h"

/*
 * Load config from $SHARED_USERDATA_PATH/plexmusic/config.json.
 * Returns 0 on success, -1 if file doesn't exist or parse error.
 * cfg is zero-initialized on failure.
 */
int plex_config_load(PlexConfig *cfg);

/*
 * Save config to $SHARED_USERDATA_PATH/plexmusic/config.json.
 * Creates directory if needed. Returns 0 on success.
 */
int plex_config_save(const PlexConfig *cfg);

/* X-Plex-Client-Identifier every install sent before per-device ids */
#define PLEX_LEGACY_CLIENT_ID "nextui-plex-music"

/* Give cfg a new random client_id ("plexmusic-" + 16 hex digits). */
void plex_config_new_client_id(PlexConfig *cfg);

/*
 * Fill cfg->client_id if empty: the legacy shared id when already signed in
 * (the saved token belongs to it), otherwise a new random one.
 */
void plex_config_ensure_client_id(PlexConfig *cfg);

/* Zero cfg and set the defaults for a fresh install (screen timeout 30 s,
 * 128 kbps streaming and downloads, pocket lock on). */
void plex_config_defaults(PlexConfig *cfg);

/* Forget the sign-in, server and library; keep the user's preferences and
 * client_id. */
void plex_config_sign_out(PlexConfig *cfg);

/* Returns true if cfg has a non-empty token and server_url. */
bool plex_config_is_valid(const PlexConfig *cfg);

/* "local" / "remote" / "relay", as stored in config.json */
const char *plex_conn_kind_name(PlexConnKind kind);

/* "Local" / "Remote" / "Relay", for display */
const char *plex_conn_kind_label(PlexConnKind kind);

/*
 * Streaming bitrate to request for this session: stream_bitrate_kbps, except
 * Original (0) becomes PLEX_RELAY_STREAM_KBPS on a relay connection.
 */
int plex_config_stream_kbps(const PlexConfig *cfg);

/*
 * Return a mutable pointer to the application-global PlexConfig.
 * Defined in main.c. Modules call this to update the live config after
 * saving so the browse module sees the new values immediately.
 */
PlexConfig *plex_config_get_mutable(void);

#endif /* PLEX_CONFIG_H */
