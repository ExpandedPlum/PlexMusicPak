#ifndef UI_KEYBOARD_H
#define UI_KEYBOARD_H

#include <stdbool.h>
#include <SDL2/SDL.h>

/*
 * On-screen keyboard. Runs its own loop until the user confirms (Y, or the
 * OK key) or cancels (B on an empty field, or MENU+START quit).
 * buf holds the starting text and receives the result.
 * Returns true if confirmed with non-empty text.
 *
 * Controls: D-pad moves, A types the key, B deletes (cancels when empty),
 * X types a space, Y confirms. (START is the app-wide help/quit button.)
 */
bool UI_keyboard(SDL_Surface *screen, const char *title, char *buf, int buf_size);

#endif /* UI_KEYBOARD_H */
