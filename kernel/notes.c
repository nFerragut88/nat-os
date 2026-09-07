/* nat-os — note pad and on-screen keyboard. See notes.h. */

#include "notes.h"
#include "keyboard.h"
#include "desktop.h"
#include "display.h"
#include "timer.h"
#include "messages.h"
#include "audio.h"

/* ---- layout --------------------------------------------------------------
 *
 * The region is shared with the launcher, so everything here is derived rather
 * than written as absolute rows. [step 277] The keyboard now runs from its own
 * top down to SPEC_Y -- the rainbow bar -- rather than to DESK_H, so it covers
 * the application band; the text area takes whatever is left above it.
 *
 * Keys are 80 x 26, a third of the panel wide. See the keypad note below for
 * why they are that size. */
#define KEY_ROWS   4u
/* [step 277] 42, was 26, matching the shell. The keyboard reaches the rainbow
 * bar rather than stopping at DESK_H, taking the 64 px that used to hold the
 * application strips -- see term.c for the same change and app_views_suspend()
 * for what happens to the programs that drew there. */
#define KEY_H      42u
#define KB_Y       KB_TOP    /* [step 353] the shared keyboard owns this */

#define TEXT_X     3u
#define LINE_H     9u
#define COLS       38u                              /* 240 / 6, less a margin */

/* ---- the look ------------------------------------------------------------
 *
 * A monochrome LCD: dark text on a green-grey field, the way a phone screen
 * looked before backlights were white. Two colours for the whole app, because
 * that is what the thing being imitated had — and because a 5x8 font on a busy
 * background is hard to read, which those screens solved by not having one.
 *
 * The header is inverted rather than a different hue, for the same reason. */
#define LCD_BG   0xAE54u        /* green-grey field  */
#define LCD_FG   0x1922u        /* dark ink          */
#define LCD_DIM  0x6B4Bu        /* half-tone, for the key faces */

/* 22, matching the close button's hit region in desktop_chrome_touch(). The
 * header draws that button itself: the launcher's chrome cannot, because it
 * only draws one when the framebuffer is off — the 3D view stamps its own into
 * the framebuffer instead, and this app has no framebuffer to stamp into. So
 * the button existed and was invisible.
 *
 * Drawing it here keeps one owner for those pixels, which is the rule that came
 * out of the flicker (UM-NATOS-021 §6.5): chrome drawn over something that
 * repaints is chrome that strobes. */
#define HDR_H    22u
#define TEXT_Y   (HDR_H + 3u)

/* ---- a multi-tap keypad --------------------------------------------------
 *
 * Twelve keys in a 3 x 4 grid, 80 x 26 each, letters reached by tapping a key
 * repeatedly — a phone keypad from before predictive text.
 *
 * This started as a joke and turned out to be the fix. The QWERTY layout it
 * replaces had 24 x 26 keys, and the touch mapping reads systematically about
 * one key to the left: tapping `e` produced `w`, consistently, which made
 * writing anything a chore. A key 3.3 times wider absorbs that error instead of
 * being destroyed by it.
 *
 * It does NOT fix the offset, and the offset is still there — see the note at
 * the end of this file. It makes the interface tolerant of it, which is a
 * different thing and worth not confusing.
 *
 * The trade is real: three taps for `c` against one for a key you cannot hit.
 * On a panel this imprecise, slow and correct beats fast and wrong. */
#define KEY_COLS   3u
#define KEY_W      (DISP_W / KEY_COLS)          /* 80 */

/* Letters per key, in tap order. The digit is last, exactly as a phone did it,
 * so a long press-through gives you the number. */
/* [step 353] The tables, the cycling state and draw_key/draw_keyboard were
 * here. They were a copy of term.c's, which said above its own copy: "If a
 * third consumer appears, factor it then." One did (285), and this is the
 * migration that was owed from that step.
 *
 * What kept it owed is that keyboard.c OWNS its text, in a 64-byte field --
 * right for a passphrase, wrong for a 256-byte note. Step 353 gave the module
 * an event API so the app can keep its own buffer and apply what the user did.
 * The layout, the cycling and the 800 ms settle are the module's; the document
 * is still this file's. */

static char     g_text[NOTES_MAX];
static uint32_t g_len;
static uint32_t g_keys;

static int      g_kb_drawn;         /* the keyboard is static once painted */
static int      g_text_dirty = 1;

/* Compose or inbox. The header is the control: tapping it switches, which is
 * how a phone with three buttons did it and means no key is spent on
 * navigation. */
#define VIEW_COMPOSE 0
#define VIEW_INBOX   1
static int      g_view = VIEW_COMPOSE;
static uint32_t g_read_index;       /* which saved message the inbox shows */
static const char *g_flash_msg;     /* transient header note: "saved" etc  */
static uint32_t g_flash_tick;

static int      g_was_down;

uint32_t notes_length(void) { return g_len; }
uint32_t notes_keys(void)   { return g_keys; }

void notes_open(void)
{
    g_kb_drawn   = 0;
    g_text_dirty = 1;
    g_was_down   = 0;
    g_view       = VIEW_COMPOSE;
    g_flash_msg  = 0;

    /* [step 353] The module owns the bottom-right key face; this app calls it
     * "save". */
    keyboard_reset("save");
}

/* A word in the header for a couple of seconds — "saved", "full". Transient
 * because it describes something that just happened rather than a state, and a
 * status line that never clears stops describing the present. */
static void flash_note(const char *m)
{
    g_flash_msg  = m;
    g_flash_tick = timer_ticks();
    g_text_dirty = 1;
}

static void draw_header(void)
{
    display_fill_rect(0, 0, DISP_W, HDR_H, LCD_FG);

    char count[4];
    uint32_t n = msg_count();
    count[0] = (char)('0' + (n / 10u) % 10u);
    count[1] = (char)('0' + n % 10u);
    count[2] = 0;

    if (g_flash_msg) {
        display_text(3, 7, g_flash_msg, LCD_BG, LCD_FG, 1u);
    } else if (g_view == VIEW_COMPOSE) {
        display_text(3, 7, "WRITE", LCD_BG, LCD_FG, 1u);
    } else {
        display_text(3, 7, "INBOX", LCD_BG, LCD_FG, 1u);
    }

    /* Saved count, then the close button at the far right. */
    display_text(DISP_W - 52u, 7, count, LCD_BG, LCD_FG, 1u);
    display_text(DISP_W - 64u, 7, "@", LCD_BG, LCD_FG, 1u);

    /* Close. Same coordinates desktop_chrome_touch() already tests, so the
     * drawing and the hit test cannot disagree. */
    for (uint32_t i = 0; i < 10u; i++) {
        display_fill_rect(DISP_W - 17u + i, 6u + i, 2u, 2u, LCD_BG);
        display_fill_rect(DISP_W - 17u + (9u - i), 6u + i, 2u, 2u, LCD_BG);
    }
}


/* Redraws the note itself, wrapped. Only the text area, so typing does not
 * repaint the keyboard — twelve keys is twenty-four drawing calls and each one
 * takes the draw lock. */
static void draw_text(void)
{
    display_fill_rect(0, HDR_H, DISP_W, KB_Y - HDR_H, LCD_BG);
    draw_header();

    /* Which text is on screen: the one being written, or the one being read. */
    const char *src = g_text;
    uint32_t    len = g_len;
    if (g_view == VIEW_INBOX) {
        src = msg_get(g_read_index);
        if (!src) {
            display_text(TEXT_X, TEXT_Y, "no messages yet", LCD_FG, LCD_BG, 1u);
            return;
        }
        len = 0;
        while (src[len]) {
            len++;
        }
    }

    uint32_t line = 0;
    uint32_t col  = 0;
    char     buf[COLS + 1u];

    for (uint32_t i = 0; i <= len; i++) {
        int end = (i == len);

        if (!end && col < COLS) {
            buf[col++] = src[i];
            continue;
        }

        buf[col] = 0;
        if (TEXT_Y + (line + 1u) * LINE_H < KB_Y) {
            display_text(TEXT_X, TEXT_Y + line * LINE_H, buf, LCD_FG, LCD_BG, 1u);
        }

        if (end) {
            /* Cursor only while writing. An inbox is not an edit box. */
            if (g_view == VIEW_COMPOSE &&
                TEXT_Y + (line + 1u) * LINE_H < KB_Y) {
                display_fill_rect(TEXT_X + col * 6u,
                                  TEXT_Y + line * LINE_H + 8u, 5u, 1u, LCD_FG);
            }
            break;
        }

        line++;
        col = 0;
        buf[col++] = src[i];
    }

    /* In the inbox, say which of how many is showing, so paging has a
     * position rather than being an endless cycle. */
    if (g_view == VIEW_INBOX) {
        char pos[8];
        pos[0] = (char)('0' + (g_read_index + 1u) % 10u);
        pos[1] = '/';
        pos[2] = (char)('0' + msg_count() % 10u);
        pos[3] = 0;
        display_text(DISP_W - 26u, KB_Y - 10u, pos, LCD_FG, LCD_BG, 1u);
    }
}

/* [step 353] commit() was here: it ended the multi-tap cycle and repainted the
 * key. Both belong to keyboard.c now -- the module settles on its own timeout
 * and repaints its own keys. What this file kept is the document. */

void notes_frame(void)
{
    if (!g_kb_drawn) {
        display_lock();
    keyboard_draw();
        display_unlock();
        g_kb_drawn = 1;
    }

    /* Expire the header note. */
    if (g_flash_msg && (timer_ticks() - g_flash_tick) > 200u) {
        g_flash_msg  = 0;
        g_text_dirty = 1;
    }

    /* [step 353] The module owns the settle timeout; ask it to run one. */
    if (keyboard_tick()) { g_text_dirty = 1; }

    if (g_text_dirty) {
        display_lock();
        draw_text();
        display_unlock();
        g_text_dirty = 0;
    }
}

void notes_touch(uint32_t x, uint32_t y, int down)
{
    if (!down) {
        g_was_down = 0;
        return;
    }
    if (g_was_down) {
        return;             /* one press, one action — not one per sample */
    }
    g_was_down = 1;

    /* The header is the navigation control: tapping it swaps compose and
     * inbox. A phone with three buttons did it this way, and it costs no key. */
    if (y < HDR_H) {
        /* The close button occupies the right end of the header and is handled
         * by desktop_chrome_touch() before this is ever called. Anything else
         * in the header switches view. */
        /* [step 353] End any live cycle before the view changes: a letter that
         * could still be replaced must not be replaceable from the next screen.
         * keyboard_reset() is the module's way to say "nothing is live". */
        keyboard_reset("save");
        g_view = (g_view == VIEW_COMPOSE) ? VIEW_INBOX : VIEW_COMPOSE;
        if (g_view == VIEW_INBOX && msg_count()) {
            g_read_index = msg_count() - 1u;    /* newest first */
        }
        g_text_dirty = 1;
        return;
    }

    /* In the inbox, the text area pages: left half back, right half forward. */
    if (g_view == VIEW_INBOX && y < KB_Y) {
        uint32_t n = msg_count();
        if (n) {
            if (x < DISP_W / 2u) {
                g_read_index = (g_read_index == 0u) ? n - 1u : g_read_index - 1u;
            } else {
                g_read_index = (g_read_index + 1u) % n;
            }
            g_text_dirty = 1;
        }
        return;
    }

    /* [step 277] SPEC_Y, not DESK_H -- the same bound that made the shell's
     * bottom two rows unresponsive when only the drawing was extended. */
    if (y < KB_Y || y >= SPEC_Y || x >= DISP_W) {
        return;
    }
    /* [step 353] The module reads the key; this file applies the edit.
     *
     * keyboard.c owns the layout, the multi-tap cycling and the settle timeout,
     * and reports what the press meant. The 256-byte document stays here,
     * because a module with a 64-byte field cannot hold it -- which is exactly
     * why this migration waited from step 285 until the event API existed. */
    int res = keyboard_touch(x, y);
    if (res == KB_NONE) { return; }

    kb_event_t ev = keyboard_event();
    g_keys++;

    /* While reading, the letter keys do nothing: an inbox is not an edit box,
     * and a stray tap should not silently start composing over a message the
     * user is looking at. Delete and save still act, so there is always a way
     * out. The module has already cycled its own buffer, which this file does
     * not read -- ignoring the event is enough. */
    if (g_view == VIEW_INBOX &&
        (ev.action == KB_ACT_APPEND || ev.action == KB_ACT_REPLACE)) {
        return;
    }

    switch (ev.action) {
    case KB_ACT_APPEND:
        if (g_len + 1u >= NOTES_MAX) { return; }   /* full: refuse, not overwrite */
        g_text[g_len++] = ev.ch;
        g_text[g_len]   = 0;
        g_text_dirty    = 1;
        break;

    case KB_ACT_REPLACE:
        if (g_len) { g_text[g_len - 1u] = ev.ch; g_text_dirty = 1; }
        break;

    case KB_ACT_BACKSPACE:
        if (g_len) { g_text[--g_len] = 0; g_text_dirty = 1; }
        break;

    case KB_ACT_SUBMIT:
        if (!g_len) { flash_note("empty"); return; }
        if (msg_save(g_text) == 0) {
            /* Cleared on success only. A save that failed must not look like
             * one that worked by leaving an empty box behind. */
            g_len = 0;
            g_text[0] = 0;
            flash_note("saved");
        } else {
            flash_note("save failed");
        }
        g_text_dirty = 1;
        break;

    default:
        break;                      /* SETTLE and NONE change no text here */
    }
}

/* ---- the offset this layout tolerates ------------------------------------
 *
 * The touch mapping reads systematically low on X: on the QWERTY layout this
 * replaced, tapping `e` reliably produced `w`, one 24-pixel key to the left.
 *
 * That fault is NOT fixed. An 80-pixel key absorbs a 24-pixel error, so this
 * layout works despite it, and the launcher's 80-pixel icon cells always did.
 * Anything finer will hit it again.
 *
 * The calibration constants in touch.c came from tapping the four corners of
 * the glass, and a finger cannot reach the extreme corner of a bezelled panel —
 * so the observed range is narrower than the true one and everything maps
 * inward. Fixing it properly means calibrating from targets in the middle of
 * the screen, where the error is actually measured rather than extrapolated. */
