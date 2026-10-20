#include "background.h"
#include "plexd_client.h"

/*
 * Music plays in plexd, which outlives the UI, so "background playback" is
 * whatever plexd has queued; these wrap its cached status (refreshed by
 * Background_tick / plexd_tick).
 */

void Background_setActive(BackgroundPlayerType type) {
    (void)type;   /* plexd's queue is the source of truth */
}

BackgroundPlayerType Background_getActive(void) {
    return plexd_has_queue() ? BG_MUSIC : BG_NONE;
}

void Background_stopAll(void) {
    if (plexd_has_queue()) plexd_cmd("/stop");
    plexd_tick();
}

bool Background_isPlaying(void) {
    const PlexdStatus *st = plexd_cached_status();
    return st->ok && (st->state == PLEXD_PLAYING || st->state == PLEXD_LOADING);
}

void Background_tick(void) {
    plexd_tick();
}
