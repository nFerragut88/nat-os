/* nat-os — the multi-tap keyboard, factored out.
 *
 * term.c said, above its own copy of this:
 *
 *     "This is a SECOND copy of the note pad's cycling logic. That is
 *      duplication and is recorded as such rather than pretended away...
 *      If a third consumer appears, factor it then."
 *
 * The wifi view needs to type a passphrase. That is the third consumer, so this
 * is that factoring, on the terms the comment set. It is written to match the
 * behaviour of the two existing copies exactly — same layout, same cycling,
 * same 800 ms settle — so that migrating them to it is a deletion rather than a
 * change in how either app feels.
 *
 * [step 285] term.c and notes.c are NOT migrated yet. Doing it in the same
 * change would mean two working apps riding on an untested module; they follow
 * once this one has been used in anger. Until then there are three copies,
 * which is worse than two and is the reason the migration is written down as
 * owed rather than left to be noticed.
 */

#ifndef NATOS_KEYBOARD_H
#define NATOS_KEYBOARD_H

#include <stdint.h>
#include "display.h"

#define KB_ROWS   4u
#define KB_COLS   3u
#define KB_KEY_H  42u
#define KB_KEY_W  (DISP_W / KB_COLS)
#define KB_TOP    (SPEC_Y - KB_ROWS * KB_KEY_H)

_Static_assert(KB_TOP + KB_ROWS * KB_KEY_H == SPEC_Y,
               "keyboard must end exactly at the rainbow bar");

#define KB_TEXT_MAX 64u         /* the WPA2 passphrase maximum, 63 + NUL */

enum {
    KB_NONE = 0,    /* the press was not the keyboard's */
    KB_EDIT,        /* the text changed; redraw it */
    KB_SUBMIT       /* the terminating key was pressed */
};

/* Empty the buffer and end any live cycle. `submit_label` is the bottom-right
 * key's face — "run" in a shell, "save" in an editor, "join" here. */
void        keyboard_reset(const char *submit_label);

void        keyboard_draw(void);
int         keyboard_touch(uint32_t x, uint32_t y);

/* Ends the cycle once the settle window has passed, so a repeat tap on the same
 * key after a pause starts a new character rather than replacing the old one.
 * Call once per frame. Returns 1 if something changed and a redraw is due. */
int         keyboard_tick(void);

const char *keyboard_text(void);
uint32_t    keyboard_len(void);

#endif /* NATOS_KEYBOARD_H */
