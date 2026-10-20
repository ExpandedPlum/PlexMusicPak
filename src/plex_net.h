#ifndef PLEX_NET_H
#define PLEX_NET_H

/*
 * HTTP for the UI. Every request goes through plexd (/tmp/plexd-net.sock),
 * which makes it with Go's HTTP and TLS stack: certificates are verified,
 * redirects followed, and plex.direct names reach their encoded IP when DNS
 * won't resolve them. plexd must be running (plexd_ensure_running()).
 */

#include <stdint.h>
#include <stdbool.h>

/* HTTP methods */
typedef enum { PLEX_HTTP_GET, PLEX_HTTP_POST, PLEX_HTTP_PUT } PlexHttpMethod;

/* Per-request options */
typedef struct {
    PlexHttpMethod method;
    const char    *body;            /* POST/PUT body (NULL for none) */
    const char    *token;           /* X-Plex-Token value (NULL if not needed) */
    int            timeout_sec;     /* per read/write; 0 = default (10s) */
    bool           no_persist;      /* unused; kept for callers */
    bool           no_retry;        /* true = single attempt (plex_net_fetch only) */
} PlexNetOptions;

/*
 * X-Plex-Client-Identifier sent with every request. Set once at startup,
 * before any request; defaults to the legacy shared id.
 */
void        plex_net_set_client_id(const char *client_id);
const char *plex_net_client_id(void);

/*
 * Fetch a URL with Plex headers.
 *
 * Always adds: X-Plex-Client-Identifier, X-Plex-Product, X-Plex-Version,
 *              X-Plex-Platform, Accept: application/json
 * If opts->token is non-NULL, adds X-Plex-Token header.
 *
 * Returns bytes written to buffer (at most buffer_size - 1, so the caller can
 * NUL-terminate), or -1 on error, HTTP status >= 400, or a body cut short.
 * Retries twice on failure unless opts->no_retry.
 */
int plex_net_fetch(const char *url, uint8_t *buffer, int buffer_size,
                   const PlexNetOptions *opts);

/*
 * Download a URL to a local file path in streaming fashion.
 *
 * progress_pct: updated with 0-100 as download proceeds when the size is
 *               known; may be NULL.
 * should_cancel: if set to true by caller, abort download and return -1; may
 *                be NULL.
 *
 * Returns total bytes written on success, or -1 on error, cancellation, a
 * failed write (e.g. SD card full) or a download that ended early. On -1 the
 * file may hold partial data; callers write to a temp path and rename.
 */
int plex_net_download_file(const char *url, const char *filepath,
                           volatile int *progress_pct,
                           volatile bool *should_cancel,
                           const PlexNetOptions *opts);

/* No-op: plexd keeps connections alive. Kept for callers. */
void plex_net_connection_close(void);

#endif /* PLEX_NET_H */
