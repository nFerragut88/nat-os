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

/* [step 353] What the last press actually DID, for apps that own their own text.
 *
 * The built-in buffer below is right for a field -- a passphrase, a host name --
 * and wrong for everything else. `notes` edits a 256-byte document and `term`
 * keeps a command line AND feeds every settled character to term_key_pop(),
 * which vm.c and device.c read so bytecode programs can take keystrokes.
 * Neither can hand its storage to a module with a 64-byte array.
 *
 * So the keyboard reports the edit and the app applies it. Same layout, same
 * cycling, same 800 ms settle; three different owners of three different texts.
 *
 * APPEND and REPLACE describe multi-tap: the first press on a key appends its
 * first letter, a repeat press replaces that letter with the next one. SETTLE
 * says the live character can no longer change, which is the moment `term` has
 * always used to push into its queue. */
enum {
    KB_ACT_NONE = 0,
    KB_ACT_APPEND,      /* a new character, `ch`                    */
    KB_ACT_REPLACE,     /* the last character becomes `ch`          */
    KB_ACT_SETTLE,      /* `ch` is final and can no longer change   */
    KB_ACT_BACKSPACE,   /* remove the last character                */
    KB_ACT_SUBMIT       /* the terminating key                      */
};

typedef struct {
    int  action;
    char ch;
} kb_event_t;

/* The event produced by the most recent keyboard_touch() or keyboard_tick().
 * KB_ACT_NONE if that press did nothing. Apps that use keyboard_text() may
 * ignore this entirely. */
kb_event_t keyboard_event(void);

/* [step 354] The character that just became FINAL, or 0. Read-and-clear.
 *
 * A press can do two things at once -- settling the previous character and
 * starting a new one -- and a single event field can only report the later of
 * them. `term` needs both: every settled character goes to term_key_pop(),
 * which vm.c and device.c read.
 *
 * Deleting is NOT settling. Backspace ends the cycle without delivering,
 * because the character being cycled is the one about to be removed; term's own
 * code drew that distinction (`commit` vs `settle`) and it belongs here now. */
char keyboard_settled(void);

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
