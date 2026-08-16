/* nat-os — note pad and on-screen keyboard. See notes.h. */

#include "notes.h"
#include "desktop.h"
#include "display.h"
#include "timer.h"

/* ---- layout --------------------------------------------------------------
 *
 * The region is DESK_H tall and shared with the launcher, so everything here is
 * derived from it rather than written as absolute rows. The keyboard takes the
 * bottom four rows and the text area whatever is left.
 *
 * Keys are 80 x 26, a third of the panel wide. See the keypad note below for
 * why they are that size. */
#define KEY_ROWS   4u
#define KEY_H      26u
#define KB_Y       (DESK_H - KEY_ROWS * KEY_H)      /* 168 - 104 = 64 */

#define TEXT_X     2u
#define TEXT_Y     2u
#define LINE_H     9u
#define COLS       39u                              /* 240 / 6, less a margin */

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
static const char *const KEYS[KEY_ROWS][KEY_COLS] = {
    { ".,?!1", "abc2", "def3"  },
    { "ghi4",  "jkl5", "mno6"  },
    { "pqrs7", "tuv8", "wxyz9" },
    { "<",     " 0",   ">"     },   /* delete, space/zero, done */
};

/* Face labels. Drawn instead of the raw sequence so a key reads as a key
 * rather than as a string of characters. */
static const char *const FACES[KEY_ROWS][KEY_COLS] = {
    { "1 .,?!", "2 abc", "3 def"  },
    { "4 ghi",  "5 jkl", "6 mno"  },
    { "7 pqrs", "8 tuv", "9 wxyz" },
    { "del",    "space", "clear"  },
};

/* How long a key stays "live" for cycling. 80 ticks is about 800 ms — long
 * enough to reach the fourth letter of `pqrs` without hurrying, short enough
 * that two different letters from the same key do not need a deliberate wait
 * most of the time. */
#define CYCLE_TICKS 80u

static int      g_live_row = -1, g_live_col = -1;
static uint32_t g_live_index;
static uint32_t g_live_tick;

static char     g_text[NOTES_MAX];
static uint32_t g_len;
static uint32_t g_keys;

static int      g_kb_drawn;         /* the keyboard is static once painted */
static int      g_text_dirty = 1;

static int      g_was_down;

uint32_t notes_length(void) { return g_len; }
uint32_t notes_keys(void)   { return g_keys; }

void notes_open(void)
{
    g_kb_drawn   = 0;
    g_text_dirty = 1;
    g_was_down   = 0;
}

static void draw_key(uint32_t r, uint32_t c, int live)
{
    uint32_t x = c * KEY_W;
    uint32_t y = KB_Y + r * KEY_H;

    /* A live key — the one currently being cycled — is drawn back-lit, so the
     * letter about to be replaced by the next tap is visible. Without it,
     * multi-tap is guesswork about whether the last press registered. */
    uint16_t bg = live ? COLOR_CYAN : COLOR_GREY;

    display_fill_rect(x + 1u, y + 1u, KEY_W - 2u, KEY_H - 2u, bg);

    const char *label = FACES[r][c];
    uint32_t tw = 0;
    for (const char *p = label; *p; p++) {
        tw += 6u;
    }
    uint32_t tx = x + (KEY_W > tw ? (KEY_W - tw) / 2u : 1u);
    display_text(tx, y + (KEY_H - 8u) / 2u, label, COLOR_BLACK, bg, 1u);
}

static void draw_keyboard(void)
{
    display_fill_rect(0, KB_Y, DISP_W, DESK_H - KB_Y, COLOR_BLACK);
    for (uint32_t r = 0; r < KEY_ROWS; r++) {
        for (uint32_t c = 0; c < KEY_COLS; c++) {
            draw_key(r, c, 0);
        }
    }
}

/* Redraws the note itself, wrapped. Only the text area, so typing does not
 * repaint the keyboard — twelve keys is twenty-four drawing calls and each one
 * takes the draw lock. */
static void draw_text(void)
{
    display_fill_rect(0, 0, DISP_W, KB_Y, COLOR_BLACK);

    uint32_t line = 0;
    uint32_t col  = 0;
    char     buf[COLS + 1u];

    for (uint32_t i = 0; i <= g_len; i++) {
        int end = (i == g_len);

        if (!end && col < COLS) {
            buf[col++] = g_text[i];
            continue;
        }

        buf[col] = 0;
        if ((line + 1u) * LINE_H < KB_Y) {
            display_text(TEXT_X, TEXT_Y + line * LINE_H, buf,
                         COLOR_WHITE, COLOR_BLACK, 1u);
        }

        if (end) {
            if ((line + 1u) * LINE_H < KB_Y) {
                display_fill_rect(TEXT_X + col * 6u, TEXT_Y + line * LINE_H + 8u,
                                  5u, 1u, COLOR_CYAN);
            }
            break;
        }

        line++;
        col = 0;
        buf[col++] = g_text[i];
    }
}

/* Ends the cycle: the character in the buffer is final and the next tap on the
 * same key starts a new one. */
static void commit(void)
{
    if (g_live_row >= 0) {
        int r = g_live_row, c = g_live_col;
        g_live_row = g_live_col = -1;
        display_lock();
        draw_key((uint32_t)r, (uint32_t)c, 0);
        display_unlock();
    }
}

void notes_frame(void)
{
    if (!g_kb_drawn) {
        display_lock();
        draw_keyboard();
        display_unlock();
        g_kb_drawn = 1;
    }

    /* Time out the live key. This is what lets two letters from the same key be
     * typed in a row: wait, and the next tap starts fresh instead of cycling. */
    if (g_live_row >= 0 && (timer_ticks() - g_live_tick) > CYCLE_TICKS) {
        commit();
    }

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

    if (y < KB_Y || y >= DESK_H || x >= DISP_W) {
        return;
    }
    uint32_t r = (y - KB_Y) / KEY_H;
    uint32_t c = x / KEY_W;
    if (r >= KEY_ROWS || c >= KEY_COLS) {
        return;
    }

    const char *seq = KEYS[r][c];
    g_keys++;

    /* Delete and clear end any cycle first: they act on the committed text, not
     * on the character being chosen. */
    if (seq[0] == '<' && seq[1] == 0) {
        commit();
        if (g_len) {
            g_text[--g_len] = 0;
            g_text_dirty = 1;
        }
        return;
    }
    if (seq[0] == '>' && seq[1] == 0) {
        commit();
        g_len = 0;
        g_text[0] = 0;
        g_text_dirty = 1;
        return;
    }

    /* Same key, still live: advance to the next letter and REPLACE the one
     * already in the buffer. Wrapping is deliberate — tapping past the end of
     * `abc2` returns to `a` rather than sticking. */
    if ((int)r == g_live_row && (int)c == g_live_col) {
        g_live_index++;
        if (seq[g_live_index] == 0) {
            g_live_index = 0;
        }
        g_text[g_len - 1u] = seq[g_live_index];
        g_live_tick  = timer_ticks();
        g_text_dirty = 1;
        return;
    }

    /* A different key: whatever was live is now final. */
    commit();

    if (g_len + 1u >= NOTES_MAX) {
        return;                     /* full: refuse rather than overwrite */
    }
    g_text[g_len++] = seq[0];
    g_text[g_len]   = 0;

    g_live_row   = (int)r;
    g_live_col   = (int)c;
    g_live_index = 0;
    g_live_tick  = timer_ticks();
    g_text_dirty = 1;

    display_lock();
    draw_key(r, c, 1);
    display_unlock();
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
