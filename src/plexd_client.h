#ifndef PLEXD_CLIENT_H
#define PLEXD_CLIENT_H

/*
 * Client for plexd, the playback daemon (plexd/ in this repo). plexd owns the
 * play queue and the audio output so music keeps playing after the UI exits,
 * in the NextUI menu and inside games. The UI sends it the queue and button
 * presses over /tmp/plexd.sock and shows its status.
 */

#include <stdbool.h>
#include "plex_models.h"

typedef enum {
    PLEXD_IDLE = 0,   /* nothing queued */
    PLEXD_LOADING,
    PLEXD_PLAYING,
    PLEXD_PAUSED,
    PLEXD_ENDED,      /* queue finished */
    PLEXD_ERROR
} PlexdState;

typedef struct {
    bool       ok;              /* plexd answered */
    PlexdState state;
    int        queue_id;        /* changes whenever a new queue is set */
    int        index;           /* current track's index in the list sent */
    int        count;
    int        position_ms;
    int        duration_ms;
    bool       shuffle;
    int        repeat;          /* REPEAT_OFF / REPEAT_ALL / REPEAT_ONE (plex_queue.h) */
    bool       has_next;
    bool       has_prev;
    bool       has_track;
    PlexTrack  track;           /* valid when has_track */
    int        transcode_kbps;  /* > 0 when streaming an Opus transcode */
    bool       local;           /* playing a downloaded file */
    char       connection[16];  /* "local" / "remote" / "relay" / "" */
    bool       mute_games;      /* setting: silence games while music plays */
    bool       hotkeys;         /* setting: in-game buttons control playback */
    char       error[160];
} PlexdStatus;

/* Make sure plexd is running, starting it (detached) if launch.sh didn't.
 * Blocks up to ~8 s. Returns false if it can't be reached. */
bool plexd_ensure_running(void);

/* Fetch status. Returns false (and out->ok = false) if plexd didn't answer. */
bool plexd_get_status(PlexdStatus *out);

/* Tell plexd whether the UI is in front (its in-game buttons stay off while
 * it is), throttled to once a second; refreshes plexd_cached_status(). Call
 * every frame. */
void plexd_tick(void);

/* Same, but now: before exiting, or when the UI leaves the foreground. */
void plexd_set_foreground(bool foreground);

/* Last status fetched by plexd_tick() or plexd_get_status(). */
const PlexdStatus *plexd_cached_status(void);

/* True when plexd has a queue loaded (playing, paused or loading). */
bool plexd_has_queue(void);

/* Replace the queue and start tracks[start]. err gets a short message on
 * failure. Blocks while plexd picks a connection (a few seconds at most). */
bool plexd_play_queue(const PlexTrack *tracks, int count, int start,
                      char *err, int errlen);

/* Simple commands: "/toggle", "/pause", "/play", "/next", "/prev", "/stop",
 * "/shuffle", "/repeat". */
bool plexd_cmd(const char *path);

bool plexd_seek(int position_ms);

/* plexd settings ("muteGames", "hotkeys"); refreshes the cached status. */
bool plexd_set_mute_games(bool on);
bool plexd_set_hotkeys(bool on);

/* Software gain 0..1 for USB DAC / Bluetooth output. */
void plexd_set_volume(float volume);

#endif /* PLEXD_CLIENT_H */
