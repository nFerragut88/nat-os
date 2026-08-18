/* nat-os — the shell, on the panel. See term.h. */

#include "term.h"
#include "shell.h"
#include "display.h"
#include "desktop.h"
#include "uart.h"
#include "timer.h"
#include "audio.h"

/* ---- layout -------------------------------------------------------------
 *
 * The same 224-row region the launcher and the note pad own, split three ways.
 * The keyboard is the fixed cost — four rows of 26 is 104 rows, nearly half the
 * region — and everything else fits in what is left.
 */
#define HDR_H      22u
#define KEY_ROWS   4u
#define KEY_COLS   3u
#define KEY_H      26u
#define KEY_W      (DISP_W / KEY_COLS)              /* 80 */
#define KB_Y       (DESK_H - KEY_ROWS * KEY_H)      /* 224 - 104 = 120 */

#define INPUT_H    11u
#define INPUT_Y    (KB_Y - INPUT_H)                 /* 109 */
#define OUT_Y      HDR_H
#define OUT_H      (INPUT_Y - OUT_Y)                /* 87 */

#define CHAR_W     6u
#define LINE_H     9u
#define TERM_COLS  39u                              /* 240/6, less a margin */
#define TERM_ROWS  (OUT_H / LINE_H)                 /* 9 */

_Static_assert(KB_Y + KEY_ROWS * KEY_H == DESK_H,
               "keyboard must end exactly at the region boundary");
_Static_assert(OUT_Y + OUT_H == INPUT_Y, "output pane must meet the input line");
_Static_assert(TERM_ROWS >= 6u, "an output pane under six lines is not worth having");

/* A terminal, deliberately not the note pad's LCD. Two full-region native apps
 * that look alike are two apps a user has to read the header to tell apart. */
#define TRM_BG   0x0000u        /* black                    */
#define TRM_FG   0x07E0u        /* phosphor green           */
#define TRM_DIM  0x03E0u        /* half-bright, key faces   */
#define TRM_KEY  0x2124u        /* key body                 */

/* ---- multi-tap keypad ---------------------------------------------------
 *
 * The same scheme and the same key sizes as the note pad (UM-NATOS-022 §3),
 * for the same reason: 80 px keys survive touch error that 24 px keys do not.
 * The calibration that made that necessary has since been fixed
 * (UM-NATOS-017 §7.4), so this is now a choice rather than a workaround — but
 * shell commands are lowercase words and multi-tap types those adequately.
 *
 * This is a SECOND copy of the note pad's cycling logic. That is duplication
 * and is recorded as such rather than pretended away: factoring it out means
 * changing a working app to serve a new one, which is a worse trade tonight
 * than two copies with a comment. If a third consumer appears, factor it then.
 *
 * The bottom row differs from the note pad's. There is no `save`; the third key
 * is `run`, because a shell's terminating key submits.
 */
static const char *const KEYS[KEY_ROWS][KEY_COLS] = {
    { ".,-1",  "abc2", "def3"  },
    { "ghi4",  "jkl5", "mno6"  },
    { "pqrs7", "tuv8", "wxyz9" },
    { "<",     " 0",   ">"     },   /* delete, space/zero, run */
};

static const char *const FACES[KEY_ROWS][KEY_COLS] = {
    { "1 .,-", "2 abc", "3 def"  },
    { "4 ghi", "5 jkl", "6 mno"  },
    { "7 pqrs","8 tuv", "9 wxyz" },
    { "del",   "space", "run"    },
};

#define CYCLE_TICKS 80u         /* ~800 ms, as the note pad */

static int      g_live_row = -1, g_live_col = -1;
static uint32_t g_live_index;
static uint32_t g_live_tick;

/* ---- state --------------------------------------------------------------- */

#define INPUT_MAX 40u

static char     g_input[INPUT_MAX];
static uint32_t g_inlen;

/* The output pane is a ring of fixed-width lines rather than a byte stream.
 * A stream would need re-wrapping on every draw, and the wrap is already
 * decided at capture time by where the newlines fall.
 *
 * The ring holds far more than the pane shows. Nine visible lines was the
 * original size and it was the wrong size for the two commands most worth
 * running: `help` produces about thirty lines and `adc` about fifteen, so the
 * pane showed the end of a list whose beginning was the part being asked for.
 *
 * 48 lines is a little over three screens of `help` and costs under 2 KB of
 * .bss. The bound is deliberate rather than generous — this is a fixed
 * allocation in a kernel with no paging, so scrollback that grew with output
 * would be an unbounded allocation driven by whatever the user typed. */
#define HIST_ROWS  48u

static char     g_out[HIST_ROWS][TERM_COLS + 1u];
static uint32_t g_row;          /* write cursor within the ring */
static uint32_t g_col;
static uint32_t g_filled = 1u;  /* lines ever written, capped at HIST_ROWS */
static uint32_t g_scroll;       /* lines scrolled back from the newest */

static uint32_t g_commands;
static int      g_kb_drawn;
static int      g_out_dirty = 1;
static int      g_in_dirty  = 1;
static int      g_was_down;

uint32_t term_commands(void) { return g_commands; }

static void out_newline(void)
{
    g_row = (g_row + 1u) % HIST_ROWS;
    g_col = 0;
    for (uint32_t i = 0; i <= TERM_COLS; i++) {
        g_out[g_row][i] = 0;
    }
    if (g_filled < HIST_ROWS) {
        g_filled++;
    }
}

/* Lines that could be scrolled back through: everything held, less a screenful
 * already visible. */
static uint32_t max_scroll(void)
{
    return (g_filled > TERM_ROWS) ? g_filled - TERM_ROWS : 0u;
}

/* Installed as the UART tee for exactly the duration of one command.
 *
 * Runs inside uart_putc(), so it must not print, must not lock, and must not be
 * slow — the shell calls it once per character of its own output. */
static void capture(char c)
{
    if (c == '\r') {
        return;
    }
    if (c == '\n') {
        out_newline();
        return;
    }
    if (g_col >= TERM_COLS) {
        out_newline();          /* hard wrap; the alternative is losing it */
    }
    g_out[g_row][g_col++] = c;
    g_out[g_row][g_col] = 0;
}

static void out_puts(const char *s)
{
    for (const char *p = s; *p; p++) {
        capture(*p);
    }
}

void term_open(void)
{
    g_kb_drawn  = 0;
    g_out_dirty = 1;
    g_in_dirty  = 1;
    g_was_down  = 0;
    g_live_row  = -1;

    if (g_commands == 0u) {
        out_puts("nat-os shell");
        out_newline();
        out_puts("type a command, then run");
        out_newline();
        out_puts("try: help  mem  ps  adc  i2c");
        out_newline();
    }
}

static void draw_header(void)
{
    display_fill_rect(0, 0, DISP_W, HDR_H, TRM_DIM);
    display_text(4, 7, "shell", TRM_BG, TRM_DIM, 1u);

    /* Same coordinates desktop_chrome_touch() tests, so the drawing and the hit
     * test sit in one file and cannot drift — the note pad's rule, and the
     * reason its close button was once invisible (UM-NATOS-022 §7). */
    for (uint32_t i = 0; i < 10u; i++) {
        display_fill_rect(DISP_W - 17u + i, 6u + i, 2u, 2u, TRM_BG);
        display_fill_rect(DISP_W - 17u + (9u - i), 6u + i, 2u, 2u, TRM_BG);
    }
}

static void draw_key(uint32_t r, uint32_t c, int live)
{
    uint32_t x = c * KEY_W;
    uint32_t y = KB_Y + r * KEY_H;

    uint16_t bg = live ? TRM_FG : TRM_KEY;
    uint16_t fg = live ? TRM_BG : TRM_FG;

    display_fill_rect(x + 1u, y + 1u, KEY_W - 2u, KEY_H - 2u, bg);

    const char *label = FACES[r][c];
    uint32_t tw = 0;
    for (const char *p = label; *p; p++) {
        tw += CHAR_W;
    }
    uint32_t tx = x + (KEY_W > tw ? (KEY_W - tw) / 2u : 1u);
    display_text(tx, y + (KEY_H - 8u) / 2u, label, fg, bg, 1u);
}

static void draw_keyboard(void)
{
    display_fill_rect(0, KB_Y, DISP_W, DESK_H - KB_Y, TRM_BG);
    for (uint32_t r = 0; r < KEY_ROWS; r++) {
        for (uint32_t c = 0; c < KEY_COLS; c++) {
            draw_key(r, c, 0);
        }
    }
}

/* Draws a window into the ring, newest line at the bottom where a terminal puts
 * it, offset back by g_scroll.
 *
 * Addressing is by distance BACK from the write cursor rather than forward from
 * an origin. The ring has no origin once it has wrapped, and counting forward
 * from one means recomputing where it moved to on every draw; counting back
 * from the cursor is the same arithmetic whether the ring has wrapped or not. */
static void draw_output(void)
{
    if (g_scroll > max_scroll()) {
        g_scroll = max_scroll();        /* history shrank under a held scroll */
    }

    display_fill_rect(0, OUT_Y, DISP_W, OUT_H, TRM_BG);

    for (uint32_t i = 0; i < TERM_ROWS; i++) {
        uint32_t back = (TERM_ROWS - 1u - i) + g_scroll;
        if (back >= g_filled) {
            continue;                   /* before anything was written */
        }
        uint32_t src = (g_row + HIST_ROWS - back) % HIST_ROWS;
        if (g_out[src][0]) {
            display_text(2, OUT_Y + i * LINE_H, g_out[src], TRM_FG, TRM_BG, 1u);
        }
    }

    /* Scrollbar, drawn only when there is something to scroll.
     *
     * It is the affordance as well as the indicator: nothing else on screen
     * says the pane can be scrolled, and a gesture with no visible cue is a
     * feature only its author knows about. */
    uint32_t span = max_scroll();
    if (span == 0u) {
        return;
    }

    uint32_t track_h = OUT_H;
    uint32_t thumb_h = (TERM_ROWS * track_h) / g_filled;
    if (thumb_h < 6u) {
        thumb_h = 6u;
    }
    /* g_scroll counts BACK from the newest, so the thumb runs bottom-to-top. */
    uint32_t travel = track_h - thumb_h;
    uint32_t thumb_y = OUT_Y + travel - (g_scroll * travel) / span;

    display_fill_rect(DISP_W - 3u, OUT_Y, 2u, track_h, TRM_KEY);
    display_fill_rect(DISP_W - 3u, thumb_y, 2u, thumb_h, TRM_FG);
}

static void draw_input(void)
{
    display_fill_rect(0, INPUT_Y, DISP_W, INPUT_H, TRM_BG);
    display_text(2, INPUT_Y + 2u, ">", TRM_DIM, TRM_BG, 1u);
    if (g_inlen) {
        display_text(2u + CHAR_W + 2u, INPUT_Y + 2u, g_input, TRM_FG, TRM_BG, 1u);
    }
    /* Block cursor, so an empty line still shows where typing will land. */
    display_fill_rect(2u + CHAR_W + 2u + g_inlen * CHAR_W, INPUT_Y + 2u,
                      CHAR_W - 1u, 8u, TRM_DIM);
}

/* ---- the key queue ------------------------------------------------------
 *
 * The keypad decoded taps into a command line FOR ITSELF and published nothing,
 * so an application could not read a keypress -- one of the eight things book
 * chapter 31 listed as needing a kernel edit and a hand-written syscall. It is
 * now a device, and this is where the characters come from.
 *
 * A character is queued when it SETTLES, not when it first appears. Multi-tap
 * means the letter under your finger is provisional: tapping the same key again
 * replaces it. Publishing on first appearance would send `a` then `b` then `c`
 * to an application that only ever saw one `c` typed.
 *
 * Overflow drops the OLDEST. A program that stops reading should lose the
 * beginning of what it missed rather than stop receiving anything new, and a
 * silent stall is harder to notice than a gap. */
#define KEYQ_MAX 16u

static char     g_keyq[KEYQ_MAX];
static uint32_t g_keyq_head, g_keyq_count, g_keyq_dropped;

/* Every character ever queued, never reset. `pending` and `dropped` cannot
 * distinguish "nothing was typed" from "something was typed and a program
 * consumed it" -- a total can. That ambiguity cost a debugging round. */
static uint32_t g_keyq_total;

static void keyq_push(char ch)
{
    g_keyq_total++;
    if (g_keyq_count == KEYQ_MAX) {
        g_keyq_head = (g_keyq_head + 1u) % KEYQ_MAX;
        g_keyq_count--;
        g_keyq_dropped++;
    }
    g_keyq[(g_keyq_head + g_keyq_count) % KEYQ_MAX] = ch;
    g_keyq_count++;
}

int term_key_pop(uint32_t *out)
{
    if (g_keyq_count == 0u) {
        return 0;
    }
    *out = (uint32_t)(uint8_t)g_keyq[g_keyq_head];
    g_keyq_head = (g_keyq_head + 1u) % KEYQ_MAX;
    g_keyq_count--;
    return 1;
}

uint32_t term_keys_pending(void) { return g_keyq_count; }
uint32_t term_keys_dropped(void) { return g_keyq_dropped; }

/* Put a character in as though it had been typed.
 *
 * The keypad only exists inside the terminal view and only settles a character
 * when a different key is pressed or the multi-tap cycle expires, so testing
 * anything downstream of it needs a person, a particular view, and correct
 * timing. That is three ways for a test to fail for reasons unrelated to what
 * it is testing -- and the `keys` device and the VM event path are both
 * downstream of here.
 *
 * Same reasoning as the `echo` device: a path whose only source needs hardware
 * or a human ships having exercised nothing but its refusal case. */
void term_key_inject(uint32_t ch)
{
    keyq_push((char)ch);
}

uint32_t term_keys_queued(void) { return g_keyq_total; }

/* Ends any live multi-tap cycle WITHOUT settling a character. Used by
 * backspace, where the live character is about to be deleted and must not be
 * delivered to anybody. */
static void commit(void)
{
    g_live_row = -1;
    g_live_col = -1;
}

/* Ends the cycle and DELIVERS the live character: it can no longer change, so
 * it is now what the user typed. Also clears the key's highlight, which every
 * caller previously did for itself. */
static void settle(void)
{
    if (g_live_row >= 0) {
        if (g_inlen) {
            keyq_push(g_input[g_inlen - 1u]);
        }
        draw_key((uint32_t)g_live_row, (uint32_t)g_live_col, 0);
    }
    commit();
}

static void submit(void)
{
    settle();                   /* the last character typed is now final */
    if (g_inlen == 0u) {
        return;
    }

    /* Snap to the newest before running.
     *
     * Done here rather than in out_newline() so that scrolling stays put while
     * reading — output only ever arrives because a command was submitted, and
     * a pane that jumped while being read would be worse than one that did not
     * follow. It also keeps the ring indices stable for the whole of a draw:
     * scrolled-back positions shift as lines are overwritten, and this makes
     * that unobservable. */
    g_scroll = 0;

    out_puts("> ");
    out_puts(g_input);
    out_newline();

    /* Tee installed for exactly this call.
     *
     * Left installed, the reporter task's telemetry — several lines a second —
     * would fill the pane with output nobody asked for. execute() takes the
     * console lock for its whole run, so while this is set the only writer is
     * the command itself. */
    uart_set_tee(capture);
    shell_run_line(g_input);
    uart_set_tee(0);

    g_commands++;
    g_inlen = 0;
    g_input[0] = 0;
    g_out_dirty = 1;
    g_in_dirty  = 1;
}

void term_frame(void)
{
    if (!g_kb_drawn) {
        display_fill_rect(0, 0, DISP_W, DESK_H, TRM_BG);
        draw_header();
        draw_keyboard();
        g_kb_drawn  = 1;
        g_out_dirty = 1;
        g_in_dirty  = 1;
    }

    /* A live key expires on its own, which is what lets two letters from one
     * key be typed without a different key in between. */
    if (g_live_row >= 0 && (timer_ticks() - g_live_tick) > CYCLE_TICKS) {
        settle();               /* the cycle expired; the character stands */
    }

    if (g_out_dirty) {
        draw_output();
        g_out_dirty = 0;
    }
    if (g_in_dirty) {
        draw_input();
        g_in_dirty = 0;
    }
}

void term_touch(uint32_t x, uint32_t y, int down)
{
    if (!down) {
        g_was_down = 0;
        return;
    }
    if (g_was_down) {
        return;                 /* one press, one action (UM-NATOS-021 §4.2) */
    }
    g_was_down = 1;

    /* The output pane scrolls: upper half back, lower half forward.
     *
     * Half a screen per tap rather than a line. Every tap here costs a press on
     * a panel this slow, and nine taps to move one screen is not a control, it
     * is a chore. Half-screens also keep two lines of overlap, so the reader
     * does not have to remember what the last line was. */
    if (y >= OUT_Y && y < INPUT_Y) {
        uint32_t step = TERM_ROWS / 2u;
        if (y < OUT_Y + OUT_H / 2u) {
            g_scroll += step;                       /* older */
            if (g_scroll > max_scroll()) {
                g_scroll = max_scroll();
            }
        } else {
            g_scroll = (g_scroll > step) ? g_scroll - step : 0u;
        }
        g_out_dirty = 1;
        return;
    }

    if (y < KB_Y || y >= DESK_H || x >= DISP_W) {
        return;                 /* header close is handled before this is called */
    }

    uint32_t r = (y - KB_Y) / KEY_H;
    uint32_t c = x / KEY_W;
    if (r >= KEY_ROWS || c >= KEY_COLS) {
        return;
    }

    const char *seq = KEYS[r][c];

    /* Click on every accepted press.
     *
     * This is what the audio was built for. Multi-tap's worst property is that
     * a press registering is INVISIBLE — UM-NATOS-022 §3.4 notes the press that
     * "did not register" is usually one that did, which then replaces the
     * letter you wanted. A click resolves that with feedback that costs none of
     * the 224 rows every other part of this interface is competing for. */
    audio_click();

    if (seq[0] == '<' && seq[1] == 0) {
        commit();
        if (g_inlen) {
            g_input[--g_inlen] = 0;
            g_in_dirty = 1;
        }
        return;
    }
    if (seq[0] == '>' && seq[1] == 0) {
        submit();
        return;
    }

    /* Cycling: a repeat tap on the live key replaces the character it just
     * produced; any other key commits the old one and starts fresh. */
    int same = (g_live_row == (int)r && g_live_col == (int)c);
    if (same) {
        uint32_t n = 0;
        while (seq[n]) {
            n++;
        }
        g_live_index = (g_live_index + 1u) % n;
        if (g_inlen) {
            g_input[g_inlen - 1u] = seq[g_live_index];
        }
    } else {
        /* A different key: whatever was live can no longer change. */
        settle();
        if (g_inlen + 1u >= INPUT_MAX) {
            return;             /* full: refuse rather than silently drop */
        }
        g_live_index = 0;
        g_input[g_inlen++] = seq[0];
        g_input[g_inlen] = 0;
        g_live_row = (int)r;
        g_live_col = (int)c;
        draw_key(r, c, 1);
    }

    g_live_tick = timer_ticks();
    g_in_dirty  = 1;
}
