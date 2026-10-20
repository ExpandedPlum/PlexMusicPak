#ifndef MODULE_PLAYER_H
#define MODULE_PLAYER_H

#include <SDL2/SDL.h>
#include "module_auth.h"  /* AppModule */

/*
 * Run the player module (now-playing screen).
 *
 * Playback itself runs in plexd (see plexd_client.h), so it carries on after
 * this screen or the whole app exits. If PlayerModule_queueChanged() was
 * called, the queue in plex_queue_get() is sent to plexd first; otherwise
 * this shows what plexd is already playing.
 *
 * Returns the next AppModule:
 *   MODULE_BROWSE — user pressed B (back) or X (stop), or the queue ended
 *   MODULE_QUIT   — user quit (music keeps playing)
 */
AppModule module_player_run(SDL_Surface *screen);

/*
 * Mark plex_queue_get() as a new queue for the next module_player_run() to
 * start. Call after plex_queue_set().
 */
void PlayerModule_queueChanged(void);

#endif /* MODULE_PLAYER_H */
