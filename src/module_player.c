#define _GNU_SOURCE
#include "module_player.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <ctype.h>

#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>

#include "api.h"
#include "msettings.h"
#include "module_common.h"
#include "plex_log.h"
#include "defines.h"
#include "plex_art.h"
#include "plex_config.h"
#include "plex_models.h"
#include "plex_favorites.h"
#include "plex_queue.h"
#include "plexd_client.h"
#include "ui_fonts.h"
#include "ui_utils.h"

extern void PLAT_enableBacklight(int enable);

/* ------------------------------------------------------------------
 * Constants
 * ------------------------------------------------------------------ */

#define STATUS_POLL_MS        250    /* plexd status refresh while awake */
#define STATUS_POLL_SLEEP_MS  1000   /* ... and with the screen off */
#define SEEK_HOLD_STEP_MS     5000   /* LEFT/RIGHT held: per repeat tick */
#define SEEK_JUMP_MS          30000  /* UP/DOWN */

/* ------------------------------------------------------------------
 * Sleep state
 * ------------------------------------------------------------------ */

static bool          s_screen_sleeping   = false;
static Uint32        s_last_activity_ms  = 0;
static volatile bool s_pwr_just_woke     = false;

static void player_after_sleep_cb(void) { s_pwr_just_woke = true; }

/* ------------------------------------------------------------------
 * Module state
 * ------------------------------------------------------------------ */

static bool s_queue_pending = false;   /* plex_queue_get() not yet sent to plexd */

static struct {
    PlexdStatus st;
    Uint32      polled_at;      /* SDL_GetTicks() of st */
    int         shown_rk;       /* rating key whose art/badge is loaded */
    char        badge_str[32];  /* e.g. "FLAC", "OPUS 128", "MP3 320"; "" = hide */
    char        error[160];     /* failure starting the queue */
} s_ui;

void PlayerModule_queueChanged(void) { s_queue_pending = true; }

/* ------------------------------------------------------------------
 * Codec badge helper
 * ------------------------------------------------------------------ */

static void build_badge_str(const PlexdStatus *st, char *out, int out_size)
{
    out[0] = '\0';
    const PlexTrack *t = &st->track;

    if (st->transcode_kbps > 0) {
        snprintf(out, out_size, "OPUS %d", st->transcode_kbps);
        return;
    }

    char codec[32] = "";
    for (int i = 0; t->audio_codec[i] && i < (int)sizeof(codec) - 1; i++)
        codec[i] = (char)toupper((unsigned char)t->audio_codec[i]);
    if (!codec[0]) return;

    bool lossless = (strcmp(t->audio_codec, "flac") == 0 ||
                     strcmp(t->audio_codec, "wav")  == 0 ||
                     strcmp(t->audio_codec, "alac") == 0 ||
                     strcmp(t->audio_codec, "aiff") == 0 ||
                     strcmp(t->audio_codec, "pcm")  == 0);

    if (!lossless && t->audio_bitrate_kbps > 0)
        snprintf(out, out_size, "%s %d", codec, t->audio_bitrate_kbps);
    else
        snprintf(out, out_size, "%s", codec);
}

/* ------------------------------------------------------------------
 * Status polling
 * ------------------------------------------------------------------ */

static void poll_status(const PlexConfig *cfg)
{
    plexd_get_status(&s_ui.st);
    s_ui.polled_at = SDL_GetTicks();

    /* Track changed (auto-advance, or a skip from in-game buttons): fetch its
     * art and badge. */
    int rk = s_ui.st.has_track ? s_ui.st.track.rating_key : 0;
    if (rk != s_ui.shown_rk) {
        s_ui.shown_rk = rk;
        plex_art_clear();
        if (rk && s_ui.st.track.thumb[0]) plex_art_fetch(cfg, s_ui.st.track.thumb);
    }
    if (rk) build_badge_str(&s_ui.st, s_ui.badge_str, sizeof(s_ui.badge_str));
}

/* Position now, moving on from the last poll while playing. */
static int display_position_ms(void)
{
    int pos = s_ui.st.position_ms;
    if (s_ui.st.state == PLEXD_PLAYING)
        pos += (int)(SDL_GetTicks() - s_ui.polled_at);
    if (s_ui.st.duration_ms > 0 && pos > s_ui.st.duration_ms) pos = s_ui.st.duration_ms;
    return pos < 0 ? 0 : pos;
}

/* ------------------------------------------------------------------
 * Rendering helpers
 * ------------------------------------------------------------------ */

#define COVER_SIZE    (is_brick ? SCALE1(100) : SCALE1(160))
#define PROGRESS_H    SCALE1(8)
#define NP_PADDING       SCALE1(12)

static void render_cover_art(SDL_Surface *screen, SDL_Surface *art, int x, int y)
{
    if (!art) return;
    SDL_Rect src = { 0, 0, art->w, art->h };
    SDL_Rect dst = { x, y, COVER_SIZE, COVER_SIZE };
    SDL_BlitScaled(art, &src, screen, &dst);
}

static void render_progress_bar(SDL_Surface *screen, int y, float frac,
                                 uint32_t bg_color, uint32_t fg_color)
{
    int x = NP_PADDING;
    int w = screen->w - 2 * NP_PADDING;

    SDL_Rect bg = { x, y, w, PROGRESS_H };
    SDL_FillRect(screen, &bg, bg_color);

    if (frac > 0.0f) {
        int fw = (int)(w * frac);
        if (fw < 1) fw = 1;
        if (fw > w) fw = w;
        SDL_Rect fg = { x, y, fw, PROGRESS_H };
        SDL_FillRect(screen, &fg, fg_color);
    }
}

static void render_text_centered(SDL_Surface *screen, TTF_Font *font,
                                  SDL_Color color, const char *text, int y)
{
    if (!text || !text[0]) return;
    SDL_Surface *surf = TTF_RenderUTF8_Blended(font, text, color);
    if (!surf) return;
    SDL_Rect dst = { (screen->w - surf->w) / 2, y, surf->w, surf->h };
    SDL_BlitSurface(surf, NULL, screen, &dst);
    SDL_FreeSurface(surf);
}

/* ------------------------------------------------------------------
 * Render: loading state
 * ------------------------------------------------------------------ */

static void render_loading(SDL_Surface *screen, const PlexTrack *track)
{
    SDL_FillRect(screen, NULL, SDL_MapRGB(screen->format, 0x12, 0x12, 0x12));

    SDL_Surface *art = plex_art_get();
    int art_x = (screen->w - COVER_SIZE) / 2;
    render_cover_art(screen, art, art_x, NP_PADDING);

    int text_y = COVER_SIZE + NP_PADDING * 2;

    SDL_Color white = COLOR_WHITE;
    SDL_Color gray  = { 0xAA, 0xAA, 0xAA, 0xFF };

    if (track) {
        TTF_Font *title_font = is_brick ? Fonts_getLarge() : Fonts_getTitle();
        render_text_centered(screen, title_font,        white, track->title,  text_y);
        text_y += TTF_FontHeight(title_font) + SCALE1(4);
        render_text_centered(screen, Fonts_getArtist(), gray,  track->artist, text_y);
        text_y += TTF_FontHeight(Fonts_getArtist()) + SCALE1(4);
        render_text_centered(screen, Fonts_getAlbum(),  gray,  track->album,  text_y);
        text_y += TTF_FontHeight(Fonts_getAlbum())  + NP_PADDING;
    }

    uint32_t bg_col = SDL_MapRGB(screen->format, 0x40, 0x40, 0x40);
    render_progress_bar(screen, text_y, 0.0f, bg_col, bg_col);
    text_y += PROGRESS_H + SCALE1(4);
    render_text_centered(screen, Fonts_getSmall(), gray, "Loading...", text_y);

    GFX_flip(screen);
}

/* ------------------------------------------------------------------
 * Render: playing state
 * ------------------------------------------------------------------ */

static void render_icon_row(SDL_Surface *screen, const PlexdStatus *st, int icons_y)
{
    SDL_Color white = COLOR_WHITE;
    SDL_Color gray  = { 0xAA, 0xAA, 0xAA, 0xFF };

    /* Shuffle label/color */
    const char *shuffle_label = st->shuffle ? "[L2] Shuffle: On" : "[L2] Shuffle: Off";
    SDL_Color   shuffle_col   = st->shuffle ? white : gray;

    /* Repeat label/color */
    const char *repeat_label;
    SDL_Color   repeat_col;
    if (st->repeat == REPEAT_ALL) {
        repeat_label = "[R2] Repeat: All";
        repeat_col   = white;
    } else if (st->repeat == REPEAT_ONE) {
        repeat_label = "[R2] Repeat: One";
        repeat_col   = white;
    } else {
        repeat_label = "[R2] Repeat: Off";
        repeat_col   = gray;
    }

    /* Favorite label/color */
    bool fav = st->has_track ? plex_favorites_contains(st->track.rating_key) : false;
    const char *fav_label = fav ? "[Y] \xe2\x99\xa5" : "[Y] \xe2\x99\xa1";
    SDL_Color   fav_col   = fav ? white : gray;

    /* Render all three surfaces to measure widths */
    SDL_Surface *s0 = TTF_RenderUTF8_Blended(Fonts_getSmall(), shuffle_label, shuffle_col);
    SDL_Surface *s1 = TTF_RenderUTF8_Blended(Fonts_getSmall(), repeat_label,  repeat_col);
    SDL_Surface *s2 = TTF_RenderUTF8_Blended(Fonts_getSmall(), fav_label,     fav_col);

    if (s0 && s1 && s2) {
        int gap     = SCALE1(16);
        int total_w = s0->w + gap + s1->w + gap + s2->w;
        int x0      = (screen->w - total_w) / 2;

        SDL_Rect d0 = { x0,                            icons_y, s0->w, s0->h };
        SDL_Rect d1 = { x0 + s0->w + gap,              icons_y, s1->w, s1->h };
        SDL_Rect d2 = { x0 + s0->w + gap + s1->w + gap, icons_y, s2->w, s2->h };

        SDL_BlitSurface(s0, NULL, screen, &d0);
        SDL_BlitSurface(s1, NULL, screen, &d1);
        SDL_BlitSurface(s2, NULL, screen, &d2);
    }

    if (s0) SDL_FreeSurface(s0);
    if (s1) SDL_FreeSurface(s1);
    if (s2) SDL_FreeSurface(s2);
}

static void render_playing_screen(SDL_Surface *screen, const PlexdStatus *st,
                                   int pos_ms, int show_setting)
{
    const PlexTrack *track = &st->track;

    SDL_FillRect(screen, NULL, SDL_MapRGB(screen->format, 0x12, 0x12, 0x12));

    SDL_Color white = COLOR_WHITE;
    SDL_Color gray  = { 0xAA, 0xAA, 0xAA, 0xFF };

    TTF_Font *title_font = is_brick ? Fonts_getLarge() : Fonts_getTitle();

    /* Measure all element heights */
    int title_h  = TTF_FontHeight(title_font);
    int artist_h = TTF_FontHeight(Fonts_getArtist());
    int album_h  = TTF_FontHeight(Fonts_getAlbum());
    int small_h  = TTF_FontHeight(Fonts_getSmall());

    /* Fixed-height elements (no gaps) */
    int fixed_h = COVER_SIZE
                + title_h + artist_h + album_h
                + small_h            /* icons row */
                + PROGRESS_H
                + small_h;           /* timestamp */

    /* Space left over for all gaps, with NP_PADDING margin top and bottom */
    int gap_pool = screen->h - 2 * NP_PADDING - fixed_h;
    if (gap_pool < 0) gap_pool = 0;

    /* Distribute across 7 equal-weight slots:
     *   art->title (1), title->artist (1), artist->album (1),
     *   album->icons (1), icons->bar (1), bar->time (1),
     *   content centering top margin (1)
     * Use integer division; any remainder goes to the centering margin. */
    int gap_unit  = gap_pool / 7;
    int art_gap   = gap_unit;   /* art bottom -> title */
    int text_gap  = gap_unit;   /* between title/artist/album lines */
    int block_gap = gap_unit;   /* album->icons and icons->bar */
    int time_gap  = gap_unit;   /* bar->timestamp */
    int top_margin = gap_pool - gap_unit * 6;  /* remainder to centering */

    int y = NP_PADDING + top_margin;

    /* Art */
    SDL_Surface *art = plex_art_get();
    int art_x = (screen->w - COVER_SIZE) / 2;
    render_cover_art(screen, art, art_x, y);
    y += COVER_SIZE + art_gap;

    /* Metadata */
    render_text_centered(screen, title_font,        white, track->title,  y);
    y += title_h + text_gap;
    render_text_centered(screen, Fonts_getArtist(), gray,  track->artist, y);
    y += artist_h + text_gap;
    render_text_centered(screen, Fonts_getAlbum(),  gray,  track->album,  y);
    y += album_h + block_gap;

    /* Icons */
    render_icon_row(screen, st, y);
    y += small_h + block_gap;

    /* Progress bar and timestamp — anchored from the bottom so they are
     * never clipped regardless of font metric variance. */
    int time_y = screen->h - NP_PADDING - small_h;
    int bar_y  = time_y - time_gap - PROGRESS_H;

    int dur_ms = st->duration_ms;
    float frac = (dur_ms > 0) ? ((float)pos_ms / (float)dur_ms) : 0.0f;
    if (frac < 0.0f) frac = 0.0f;
    if (frac > 1.0f) frac = 1.0f;
    uint32_t bg_col = SDL_MapRGB(screen->format, 0x40, 0x40, 0x40);
    uint32_t fg_col = SDL_MapRGB(screen->format, 0x22, 0x88, 0xFF);
    render_progress_bar(screen, bar_y, frac, bg_col, fg_col);

    /* Timestamp */
    char pos_str[16], dur_str[16], time_label[48];
    format_time(pos_str, pos_ms);
    format_time(dur_str, dur_ms);
    snprintf(time_label, sizeof(time_label), "%s%s / %s",
             st->state == PLEXD_PAUSED ? "Paused  " : "", pos_str, dur_str);
    render_text_centered(screen, Fonts_getSmall(), gray, time_label, time_y);

    /* Codec/quality badge — top-left corner, pill background */
    if (s_ui.badge_str[0]) {
        SDL_Surface *badge = TTF_RenderUTF8_Blended(Fonts_getSmall(), s_ui.badge_str, white);
        if (badge) {
            int pill_w = badge->w + SCALE1(BUTTON_PADDING * 2);
            int pill_h = SCALE1(PILL_SIZE);
            int hw_pad = is_brick ? SCALE1(5) : SCALE1(10);
            GFX_blitPillLight(ASSET_WHITE_PILL, screen,
                              &(SDL_Rect){ hw_pad, hw_pad, pill_w, pill_h });
            int text_x = hw_pad + SCALE1(BUTTON_PADDING);
            int text_y = hw_pad + (pill_h - badge->h) / 2;
            SDL_BlitSurface(badge, NULL, screen,
                            &(SDL_Rect){ text_x, text_y, badge->w, badge->h });
            SDL_FreeSurface(badge);
        }
    }

    /* Hardware status (wifi / battery / time) — top-right */
    GFX_blitHardwareGroup(screen, show_setting);

    GFX_flip(screen);
}

/* ------------------------------------------------------------------
 * Render: error state
 * ------------------------------------------------------------------ */

static void render_error(SDL_Surface *screen, const char *detail)
{
    SDL_FillRect(screen, NULL, SDL_MapRGB(screen->format, 0x12, 0x12, 0x12));
    SDL_Color white = COLOR_WHITE;
    SDL_Color gray  = { 0xAA, 0xAA, 0xAA, 0xFF };
    int cy = screen->h / 2 - TTF_FontHeight(Fonts_getMedium());
    render_text_centered(screen, Fonts_getMedium(), white,
                         "Failed to load track.", cy);
    cy += TTF_FontHeight(Fonts_getMedium()) + SCALE1(8);
    if (detail && detail[0]) {
        render_text_centered(screen, Fonts_getSmall(), gray, detail, cy);
        cy += TTF_FontHeight(Fonts_getSmall()) + SCALE1(4);
    }
    render_text_centered(screen, Fonts_getSmall(), gray,
                         "Press A to retry, B to go back.", cy);
    GFX_flip(screen);
}

/* ------------------------------------------------------------------
 * Start the pending queue
 * ------------------------------------------------------------------ */

static bool start_pending_queue(SDL_Surface *screen, const PlexConfig *cfg)
{
    PlexQueue *queue = plex_queue_get();
    s_queue_pending = false;
    s_ui.error[0] = '\0';
    if (!queue || !queue->active || queue->count == 0) {
        PLEX_LOG_ERROR("[Player] no active queue\n");
        return false;
    }

    const PlexTrack *t = plex_queue_current_track();
    plex_art_clear();
    s_ui.shown_rk = 0;
    if (t && t->thumb[0]) plex_art_fetch(cfg, t->thumb);
    render_loading(screen, t);

    if (!plexd_ensure_running()) {
        snprintf(s_ui.error, sizeof(s_ui.error), "The player service isn't running.");
        return false;
    }
    if (!plexd_play_queue(queue->tracks, queue->count, queue->current_index,
                          s_ui.error, sizeof(s_ui.error)))
        return false;
    poll_status(cfg);
    return true;
}

/* ------------------------------------------------------------------
 * Public entry point
 * ------------------------------------------------------------------ */

AppModule module_player_run(SDL_Surface *screen)
{
    const PlexConfig *cfg = plex_config_get_mutable();

    if (s_queue_pending) {
        start_pending_queue(screen, cfg);
    } else {
        s_ui.error[0] = '\0';
        s_ui.shown_rk = 0;   /* refetch art: browse showed album art meanwhile */
        poll_status(cfg);
        if (!s_ui.st.has_track) return MODULE_BROWSE;
    }

    int dirty        = 1;
    int show_setting = 0;

    /* LEFT/RIGHT: tap = previous/next track, hold = seek. Held seeks add up
     * locally and are sent once on release (each seek restarts the stream). */
    bool left_armed  = false;
    bool right_armed = false;
    bool seek_held   = false;
    int  seek_target = 0;

    /* Sleep state init */
    s_screen_sleeping  = false;
    s_last_activity_ms = SDL_GetTicks();

    while (1) {
        GFX_startFrame();
        PAD_poll();
        SDL_Delay(16);

        Uint32 poll_every = s_screen_sleeping ? STATUS_POLL_SLEEP_MS : STATUS_POLL_MS;
        if (!s_ui.error[0] && SDL_GetTicks() - s_ui.polled_at >= poll_every) {
            poll_status(cfg);
            dirty = 1;
            /* Queue finished, or stopped from elsewhere: back to browsing */
            if (s_ui.st.state == PLEXD_ENDED || s_ui.st.state == PLEXD_IDLE) {
                if (s_screen_sleeping) { PLAT_enableBacklight(1); s_screen_sleeping = false; }
                return MODULE_BROWSE;
            }
        }

        /* ---- Screen sleep / wake ---- */
        if (cfg->screen_timeout > 0 && !s_screen_sleeping) {
            if (SDL_GetTicks() - s_last_activity_ms > (Uint32)(cfg->screen_timeout * 1000)) {
                SetRawBrightness(0);
                s_screen_sleeping = true;
            }
        }

        if (s_screen_sleeping) {
            ModuleCommon_setAfterSleepCallback(player_after_sleep_cb);
            GlobalInputResult global = ModuleCommon_handleGlobalInput(screen, &show_setting, 0);
            if (global.should_quit) {
                PLAT_enableBacklight(1);
                s_screen_sleeping = false;
                return MODULE_QUIT;
            }
            bool os_woke = s_pwr_just_woke;
            s_pwr_just_woke = false;

            bool woke = os_woke
                || (cfg->pocket_lock_enabled
                    ? (PAD_isPressed(BTN_MENU) && PAD_justPressed(BTN_SELECT))
                    : (PAD_anyPressed() && !PAD_isPressed(BTN_PLUS) && !PAD_isPressed(BTN_MINUS)));
            if (woke) {
                PLAT_enableBacklight(1);
                s_screen_sleeping  = false;
                s_last_activity_ms = SDL_GetTicks();
                dirty = 1;
            }

            SDL_Delay(34); /* combined with top-of-loop SDL_Delay(16) = ~50 ms (~15 Hz) */
            GFX_sync();
            continue;  /* consume all other input while sleeping */
        }

        /* Reset idle timer on any input while awake */
        if (PAD_anyPressed()) s_last_activity_ms = SDL_GetTicks();

        /* Power management heartbeat — must run every awake frame */
        ModuleCommon_PWR_update(&dirty, &show_setting);

        /* Global input (MENU+START = quit, START = help/confirm) */
        {
            GlobalInputResult global = ModuleCommon_handleGlobalInput(screen, &show_setting, 0);
            if (global.should_quit) {
                /* Music keeps playing in plexd; in-game buttons control it */
                return MODULE_QUIT;
            }
            if (global.input_consumed) {
                GFX_sync();
                continue;
            }
        }

        /* ---- Art async update ---- */
        if (plex_art_is_fetching()) dirty = 1;

        bool failed = s_ui.error[0] || s_ui.st.state == PLEXD_ERROR || !s_ui.st.ok;

        /* ====================================================
         * ERROR state
         * ==================================================== */
        if (failed) {
            if (PAD_justPressed(BTN_A)) {
                if (s_ui.error[0]) {
                    PlayerModule_queueChanged();
                    start_pending_queue(screen, cfg);
                } else {
                    plexd_cmd("/play");
                    poll_status(cfg);
                }
                dirty = 1;
            } else if (PAD_justPressed(BTN_B)) {
                return MODULE_BROWSE;
            }
        }

        /* ====================================================
         * PLAYING / PAUSED / LOADING
         * ==================================================== */
        else {
            if (PAD_justPressed(BTN_A)) {
                plexd_cmd("/toggle");
                poll_status(cfg);
                dirty = 1;
            }
            else if (PAD_justPressed(BTN_B)) {
                /* Keep playing in the background, return to browse */
                return MODULE_BROWSE;
            }
            else if (PAD_justPressed(BTN_X)) {
                /* Stop playback altogether */
                plexd_cmd("/stop");
                return MODULE_BROWSE;
            }
            else if (PAD_justPressed(BTN_L2)) {
                plexd_cmd("/shuffle");
                poll_status(cfg);
                dirty = 1;
            }
            else if (PAD_justPressed(BTN_R2)) {
                plexd_cmd("/repeat");
                poll_status(cfg);
                dirty = 1;
            }
            else if (PAD_justPressed(BTN_Y) && s_ui.st.has_track) {
                plex_favorites_toggle(&s_ui.st.track);
                dirty = 1;
            }

            /* BTN_LEFT: tap = prev track, hold = seek back */
            if (PAD_justPressed(BTN_LEFT)) {
                left_armed  = true;
                seek_held   = false;
                seek_target = display_position_ms();
            }
            if (PAD_justRepeated(BTN_LEFT) && left_armed && !PAD_justPressed(BTN_LEFT)) {
                seek_target -= SEEK_HOLD_STEP_MS;
                if (seek_target < 0) seek_target = 0;
                seek_held = true;
                dirty = 1;
            }
            if (PAD_justReleased(BTN_LEFT) && left_armed) {
                if (seek_held) plexd_seek(seek_target);
                else           plexd_cmd("/prev");
                poll_status(cfg);
                left_armed = seek_held = false;
                dirty = 1;
            }

            /* BTN_RIGHT: tap = next track, hold = seek forward */
            if (PAD_justPressed(BTN_RIGHT)) {
                right_armed = true;
                seek_held   = false;
                seek_target = display_position_ms();
            }
            if (PAD_justRepeated(BTN_RIGHT) && right_armed && !PAD_justPressed(BTN_RIGHT)) {
                seek_target += SEEK_HOLD_STEP_MS;
                if (s_ui.st.duration_ms > 0 && seek_target > s_ui.st.duration_ms - 1000)
                    seek_target = s_ui.st.duration_ms - 1000;
                seek_held = true;
                dirty = 1;
            }
            if (PAD_justReleased(BTN_RIGHT) && right_armed) {
                if (seek_held) plexd_seek(seek_target);
                else if (s_ui.st.has_next) plexd_cmd("/next");
                poll_status(cfg);
                right_armed = seek_held = false;
                dirty = 1;
            }

            /* Seek ±30s */
            if (PAD_justPressed(BTN_UP)) {
                plexd_seek(display_position_ms() + SEEK_JUMP_MS);
                poll_status(cfg);
                dirty = 1;
            }
            if (PAD_justPressed(BTN_DOWN)) {
                plexd_seek(display_position_ms() - SEEK_JUMP_MS);
                poll_status(cfg);
                dirty = 1;
            }

            /* Keep the time display moving */
            if (s_ui.st.state == PLEXD_PLAYING) dirty = 1;
        }

        /* ---- Render ---- */
        if (dirty) {
            if (s_ui.error[0]) {
                render_error(screen, s_ui.error);
            } else if (!s_ui.st.ok || s_ui.st.state == PLEXD_ERROR) {
                render_error(screen, s_ui.st.ok ? s_ui.st.error : "The player service stopped.");
            } else if (s_ui.st.state == PLEXD_LOADING || !s_ui.st.has_track) {
                render_loading(screen, s_ui.st.has_track ? &s_ui.st.track : NULL);
            } else {
                int pos = seek_held ? seek_target : display_position_ms();
                render_playing_screen(screen, &s_ui.st, pos, show_setting);
            }
            dirty = 0;
        }

        GFX_sync();
    }

    /* unreachable */
    return MODULE_BROWSE;
}
