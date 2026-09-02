/* nat-os — note pad and on-screen keyboard. See notes.h. */

#include "notes.h"
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
#define KB_Y       (SPEC_Y - KEY_ROWS * KEY_H)      /* 288 - 168 = 120 */

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
static const char *const KEYS[KEY_ROWS][KEY_COLS] = {
    { ".,?!1", "abc2", "def3"  },
    { "ghi4",  "jkl5", "mno6"  },
    { "pqrs7", "tuv8", "wxyz9" },
    { "<",     " 0",   ">"     },   /* delete, space/zero, save */
};

/* Face labels. Drawn instead of the raw sequence so a key reads as a key
 * rather than as a string of characters. */
static const char *const FACES[KEY_ROWS][KEY_COLS] = {
    { "1 .,?!", "2 abc", "3 def"  },
    { "4 ghi",  "5 jkl", "6 mno"  },
    { "7 pqrs", "8 tuv", "9 wxyz" },
    { "del",    "space", "save"   },
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

static void draw_key(uint32_t r, uint32_t c, int live)
{
    uint32_t x = c * KEY_W;
    uint32_t y = KB_Y + r * KEY_H;

    /* A live key — the one currently being cycled — is drawn back-lit, so the
     * letter about to be replaced by the next tap is visible. Without it,
     * multi-tap is guesswork about whether the last press registered. */
    uint16_t bg = live ? LCD_FG : LCD_DIM;
    uint16_t fg = live ? LCD_BG : LCD_FG;

    display_fill_rect(x + 1u, y + 1u, KEY_W - 2u, KEY_H - 2u, bg);

    const char *label = FACES[r][c];
    uint32_t tw = 0;
    for (const char *p = label; *p; p++) {
        tw += 6u;
    }
    uint32_t tx = x + (KEY_W > tw ? (KEY_W - tw) / 2u : 1u);
    display_text(tx, y + (KEY_H - 8u) / 2u, label, fg, bg, 1u);
}

static void draw_keyboard(void)
{
    display_fill_rect(0, KB_Y, DISP_W, SPEC_Y - KB_Y, LCD_FG);
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

    /* Expire the header note. */
    if (g_flash_msg && (timer_ticks() - g_flash_tick) > 200u) {
        g_flash_msg  = 0;
        g_text_dirty = 1;
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

    /* The header is the navigation control: tapping it swaps compose and
     * inbox. A phone with three buttons did it this way, and it costs no key. */
    if (y < HDR_H) {
        /* The close button occupies the right end of the header and is handled
         * by desktop_chrome_touch() before this is ever called. Anything else
         * in the header switches view. */
        commit();
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
    uint32_t r = (y - KB_Y) / KEY_H;
    uint32_t c = x / KEY_W;
    if (r >= KEY_ROWS || c >= KEY_COLS) {
        return;
    }

    const char *seq = KEYS[r][c];
    g_keys++;
    audio_click();          /* see term.c: multi-tap needs press feedback */

    /* While reading, the letter keys do nothing: an inbox is not an edit box,
     * and a stray tap should not silently start composing over a message the
     * user is looking at. del/space/save still act, so there is always a way
     * out. */
    if (g_view == VIEW_INBOX && seq[0] != '<' && seq[0] != '>') {
        return;
    }

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
        if (!g_len) {
            flash_note("empty");
            return;
        }
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
