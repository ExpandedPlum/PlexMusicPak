#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdbool.h>
#include <signal.h>
#include <msettings.h>

#include "api.h"
#include "utils.h"
#include "config.h"

#include "plex_log.h"
#include "plex_auth.h"
#include "plex_config.h"
#include "plex_net.h"
#include "plex_art.h"
#include "audio_out.h"
#include "ui_fonts.h"
#include "ui_utils.h"
#include "module_auth.h"
#include "module_browse.h"
#include "module_player.h"
#include "module_settings.h"
#include "plex_queue.h"
#include "plex_downloads.h"
#include "plex_favorites.h"
#include "plexd_client.h"

/* Global quit flag, set by signal handler */
static volatile bool g_quit = false;

/* Global application config — accessible to future modules via plex_config_get() */
static PlexConfig g_config;

/* Provide read-only access to the global config for other modules */
const PlexConfig *plex_config_get(void)
{
    return &g_config;
}

/* Provide mutable access to the global config — used by module_auth after save */
PlexConfig *plex_config_get_mutable(void)
{
    return &g_config;
}

static void sig_handler(int sig) {
    switch (sig) {
    case SIGINT:
    case SIGTERM:
        g_quit = true;
        break;
    default:
        break;
    }
}

int main(int argc, char *argv[]) {
    (void)argc;
    (void)argv;

    plex_log_init();

    /* --- Platform / SDL init (mirror musicplayer.c sequence) --- */
    SDL_Surface *screen = GFX_init(MODE_MAIN);
    PWR_pinToCores(CPU_CORE_PERFORMANCE);

    InitSettings();
    PAD_init();
    PWR_init();
    PWR_setCPUSpeed(CPU_SPEED_POWERSAVE);
    PWR_disableAutosleep();
    WIFI_init();
    Fonts_load();
    plex_art_init();
    AudioOut_init();

    plex_downloads_init();

    /* Cover art cache: prune to 120 MB once past 150 MB, keeping art for
     * downloaded albums so offline browsing still has covers. */
    {
        PlexAlbum *albums = malloc(PLEX_MAX_OFFLINE_ITEMS * sizeof(PlexAlbum));
        const char **keep = malloc(PLEX_MAX_OFFLINE_ITEMS * sizeof(char *));
        int n = (albums && keep) ? plex_downloads_get_all_albums(albums, PLEX_MAX_OFFLINE_ITEMS) : 0;
        for (int i = 0; i < n; i++) keep[i] = albums[i].thumb;
        plex_art_prune_cache_async(150L << 20, 120L << 20, keep, n);
        free(keep);
        free(albums);
    }

    plex_favorites_init();

    signal(SIGINT, sig_handler);
    signal(SIGTERM, sig_handler);
    signal(SIGPIPE, SIG_IGN);      /* prevent crash when writing to dead sockets */

    /*
     * Load config from $SHARED_USERDATA_PATH/plexmusic/config.json.
     * Fallback path is /mnt/SDCARD/.userdata/shared/plexmusic/config.json.
     */
    memset(&g_config, 0, sizeof(g_config));
    plex_config_load(&g_config);
    plex_config_ensure_client_id(&g_config);
    plex_net_set_client_id(g_config.client_id);

    /* plexd plays the music and makes our network requests (verifying HTTPS
     * certificates); launch.sh starts it, this covers it not having done so.
     * Without it, requests connect directly. */
    plexd_ensure_running();

    if (!g_config.offline_mode) {
        render_status_message(screen, "Connecting to server...");
        if (plex_config_is_valid(&g_config)) {
            plex_auth_refresh_server_urls(&g_config); /* best-effort; failure falls through to cached */
            plex_auth_select_connection(&g_config);
        }
    }

    /*
     * Determine starting module.
     * If the config has a valid token and server URL, go straight to browse.
     * Otherwise begin at auth.
     */
    bool has_token = plex_config_is_valid(&g_config);
    AppModule current = has_token ? MODULE_BROWSE : MODULE_AUTH;

    /* --- Main module loop --- */
    while (!g_quit) {
        AppModule next;

        switch (current) {
        case MODULE_AUTH:
            next = module_auth_run(screen);
            if (next == MODULE_BROWSE && !g_config.offline_mode) {
                render_status_message(screen, "Connecting to server...");
                plex_auth_select_connection(&g_config);
            }
            break;
        case MODULE_BROWSE:
            next = module_browse_run(screen);
            break;
        case MODULE_PLAYER:
            next = module_player_run(screen);
            break;
        case MODULE_SETTINGS:
            next = module_settings_run(screen);
            break;
        case MODULE_QUIT:
        default:
            g_quit = true;
            next = MODULE_QUIT;
            break;
        }

        if (next == MODULE_QUIT) {
            g_quit = true;
        } else {
            current = next;
        }
    }

    /* Music keeps playing in plexd after we exit; let its in-game buttons
     * take over right away. */
    plexd_set_foreground(false);

    /* --- Clean shutdown --- */
    PWR_enableAutosleep();
    plex_downloads_quit();
    plex_favorites_quit();
    plex_art_cleanup();
    AudioOut_quit();
    Fonts_unload();
    PWR_quit();
    PAD_quit();
    GFX_quit();
    plex_log_flush();
    return EXIT_SUCCESS;
}
