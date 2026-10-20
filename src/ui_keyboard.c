#include "ui_keyboard.h"

#include <string.h>

#include "api.h"
#include "defines.h"
#include "module_common.h"
#include "ui_fonts.h"
#include "ui_utils.h"

#define KB_COLS 10
#define KB_ROWS 5   /* 4 rows of characters + the action row */

static const char KEYS[4][KB_COLS + 1] = {
    "abcdefghij",
    "klmnopqrst",
    "uvwxyz0123",
    "456789'-.&",
};

/* Action row: three wide keys spanning the columns */
enum { ACT_SPACE, ACT_DEL, ACT_OK };
static const char *ACT_LABELS[3] = { "SPACE", "DEL", "OK" };
static int act_for_col(int col) { return col < 4 ? ACT_SPACE : col < 7 ? ACT_DEL : ACT_OK; }
static int col_for_act(int act) { return act == ACT_SPACE ? 1 : act == ACT_DEL ? 5 : 8; }

static void draw_key(SDL_Surface *screen, int x, int y, int w, int h,
                     const char *label, bool sel)
{
    render_rounded_rect_bg(screen, x, y, w, h,
                           sel ? THEME_COLOR1 : SDL_MapRGB(screen->format, 0x30, 0x30, 0x30));
    SDL_Surface *t = TTF_RenderUTF8_Blended(Fonts_getMedium(), label,
                                             sel ? COLOR_BLACK : COLOR_WHITE);
    if (t) {
        SDL_BlitSurface(t, NULL, screen,
                        &(SDL_Rect){ x + (w - t->w) / 2, y + (h - t->h) / 2 });
        SDL_FreeSurface(t);
    }
}

static void render(SDL_Surface *screen, const char *title, const char *buf,
                   int row, int col, int show_setting)
{
    SDL_FillRect(screen, NULL, SDL_MapRGB(screen->format, 0x12, 0x12, 0x12));
    render_screen_header(screen, title, show_setting);

    int pad    = SCALE1(8);
    int field_y = SCALE1(PILL_SIZE) + SCALE1(12);
    int field_h = SCALE1(PILL_SIZE);
    int field_w = screen->w - 2 * pad;
    render_rounded_rect_bg(screen, pad, field_y, field_w, field_h,
                           SDL_MapRGB(screen->format, 0x26, 0x26, 0x26));
    char shown[300];
    snprintf(shown, sizeof(shown), "%s_", buf);
    SDL_Surface *t = TTF_RenderUTF8_Blended(Fonts_getMedium(), shown, COLOR_WHITE);
    if (t) {
        /* keep the end of long text visible */
        int maxw = field_w - 2 * pad;
        SDL_Rect src = { t->w > maxw ? t->w - maxw : 0, 0, t->w > maxw ? maxw : t->w, t->h };
        SDL_BlitSurface(t, &src, screen,
                        &(SDL_Rect){ pad * 2, field_y + (field_h - t->h) / 2 });
        SDL_FreeSurface(t);
    }

    int grid_y  = field_y + field_h + SCALE1(10);
    int bottom  = screen->h - SCALE1(PILL_SIZE) - SCALE1(10);   /* button hints */
    int gap     = SCALE1(4);
    int key_w   = (screen->w - 2 * pad - (KB_COLS - 1) * gap) / KB_COLS;
    int key_h   = (bottom - grid_y - (KB_ROWS - 1) * gap) / KB_ROWS;
    if (key_h > key_w) key_h = key_w;

    for (int r = 0; r < 4; r++) {
        for (int c = 0; c < KB_COLS; c++) {
            char label[2] = { KEYS[r][c], '\0' };
            draw_key(screen, pad + c * (key_w + gap), grid_y + r * (key_h + gap),
                     key_w, key_h, label, row == r && col == c);
        }
    }
    /* SPACE: cols 0-3, DEL: 4-6, OK: 7-9 */
    int ay = grid_y + 4 * (key_h + gap);
    int spans[3][2] = { {0, 4}, {4, 3}, {7, 3} };
    for (int a = 0; a < 3; a++) {
        int x = pad + spans[a][0] * (key_w + gap);
        int w = spans[a][1] * key_w + (spans[a][1] - 1) * gap;
        draw_key(screen, x, ay, w, key_h, ACT_LABELS[a], row == 4 && act_for_col(col) == a);
    }

    GFX_blitButtonGroup((char*[]){"X", "SPACE", "Y", "OK", NULL}, 0, screen, 0);
    GFX_blitButtonGroup((char*[]){"A", "TYPE", "B", "DEL", NULL}, 1, screen, 1);
    GFX_flip(screen);
}

static void type_char(char *buf, int buf_size, char c)
{
    int n = (int)strlen(buf);
    if (n < buf_size - 1) {
        buf[n] = c;
        buf[n + 1] = '\0';
    }
}

static void delete_char(char *buf)
{
    int n = (int)strlen(buf);
    if (n == 0) return;
    /* drop a whole UTF-8 sequence */
    do { n--; } while (n > 0 && ((unsigned char)buf[n] & 0xC0) == 0x80);
    buf[n] = '\0';
}

bool UI_keyboard(SDL_Surface *screen, const char *title, char *buf, int buf_size)
{
    int row = 0, col = 0;
    int dirty = 1, show_setting = 0;

    while (1) {
        GFX_startFrame();
        PAD_poll();
        SDL_Delay(16);
        ModuleCommon_PWR_update(&dirty, &show_setting);

        GlobalInputResult global = ModuleCommon_handleGlobalInput(screen, &show_setting, 0);
        if (global.should_quit) return false;
        if (global.input_consumed) {
            if (global.dirty) dirty = 1;
            GFX_sync();
            continue;
        }

        if (PAD_justRepeated(BTN_UP))    { row = (row + KB_ROWS - 1) % KB_ROWS; dirty = 1; }
        if (PAD_justRepeated(BTN_DOWN))  { row = (row + 1) % KB_ROWS; dirty = 1; }
        if (PAD_justRepeated(BTN_LEFT)) {
            if (row == 4) col = col_for_act((act_for_col(col) + 2) % 3);
            else          col = (col + KB_COLS - 1) % KB_COLS;
            dirty = 1;
        }
        if (PAD_justRepeated(BTN_RIGHT)) {
            if (row == 4) col = col_for_act((act_for_col(col) + 1) % 3);
            else          col = (col + 1) % KB_COLS;
            dirty = 1;
        }
        if (PAD_justPressed(BTN_A)) {
            if (row < 4) {
                type_char(buf, buf_size, KEYS[row][col]);
            } else {
                switch (act_for_col(col)) {
                    case ACT_SPACE: type_char(buf, buf_size, ' '); break;
                    case ACT_DEL:   delete_char(buf); break;
                    case ACT_OK:    if (buf[0]) return true; break;
                }
            }
            dirty = 1;
        }
        if (PAD_justPressed(BTN_X)) { type_char(buf, buf_size, ' '); dirty = 1; }
        if (PAD_justPressed(BTN_B)) {
            if (!buf[0]) return false;
            delete_char(buf);
            dirty = 1;
        }
        if (PAD_justPressed(BTN_Y) && buf[0]) return true;

        if (dirty) {
            render(screen, title, buf, row, col, show_setting);
            dirty = 0;
        } else {
            GFX_sync();
        }
    }
}
