/* nat-os — the multi-tap keyboard. See keyboard.h. */

#include "keyboard.h"
#include "display.h"
#include "timer.h"

/* The same tables as term.c, with the bottom-right face supplied by the caller.
 * The sequences are phone-keypad order because that is what a person who has
 * ever texted already knows, and this interface should not have to be learned
 * twice. */
static const char *const KEYS[KB_ROWS][KB_COLS] = {
    { ".,-1",  "abc2", "def3"  },
    { "ghi4",  "jkl5", "mno6"  },
    { "pqrs7", "tuv8", "wxyz9" },
    { "<",     " 0",   ">"     },   /* delete, space/zero, submit */
};

static const char *const FACES[KB_ROWS][KB_COLS] = {
    { "1 .,-", "2 abc", "3 def"  },
    { "4 ghi", "5 jkl", "6 mno"  },
    { "7 pqrs","8 tuv", "9 wxyz" },
    { "del",   "space", 0        },  /* [2][2] comes from keyboard_reset */
};

#define CYCLE_TICKS 80u             /* ~800 ms, as both existing copies */

#define KB_BG   COLOR_BLACK
#define KB_FACE 0x2104u             /* the key body: barely lighter than black */
#define KB_FG   COLOR_WHITE
#define KB_LIVE COLOR_BLUE

static char        g_text[KB_TEXT_MAX];
static uint32_t    g_len;
static const char *g_submit = "ok";

static int      g_live_row = -1, g_live_col = -1;
static uint32_t g_live_index;
static uint32_t g_live_tick;

static const char *face(uint32_t r, uint32_t c)
{
    if (r == KB_ROWS - 1u && c == KB_COLS - 1u) { return g_submit; }
    return FACES[r][c];
}

static void draw_key(uint32_t r, uint32_t c, int live)
{
    uint32_t x = c * KB_KEY_W;
    uint32_t y = KB_TOP + r * KB_KEY_H;
    uint16_t bg = live ? KB_LIVE : KB_FACE;

    display_fill_rect(x + 1u, y + 1u, KB_KEY_W - 2u, KB_KEY_H - 2u, bg);

    const char *l = face(r, c);
    uint32_t n = 0u;
    while (l[n]) { n++; }
    uint32_t tx = x + (KB_KEY_W - n * 6u) / 2u;
    display_text(tx, y + (KB_KEY_H - 8u) / 2u, l, KB_FG, bg, 1u);
}

void keyboard_draw(void)
{
    display_fill_rect(0, KB_TOP, DISP_W, SPEC_Y - KB_TOP, KB_BG);
    for (uint32_t r = 0u; r < KB_ROWS; r++) {
        for (uint32_t c = 0u; c < KB_COLS; c++) {
            draw_key(r, c, (g_live_row == (int)r && g_live_col == (int)c));
        }
    }
}

void keyboard_reset(const char *submit_label)
{
    g_submit = submit_label ? submit_label : "ok";
    for (uint32_t i = 0u; i < KB_TEXT_MAX; i++) { g_text[i] = 0; }
    g_len = 0u;
    g_live_row = -1;
    g_live_col = -1;
}

const char *keyboard_text(void) { return g_text; }
uint32_t    keyboard_len(void)  { return g_len; }

/* The live character can no longer change. */
static void settle(void)
{
    if (g_live_row >= 0) {
        uint32_t r = (uint32_t)g_live_row, c = (uint32_t)g_live_col;
        g_live_row = -1;
        g_live_col = -1;
        draw_key(r, c, 0);
    }
}

int keyboard_tick(void)
{
    if (g_live_row < 0) { return 0; }
    if ((timer_ticks() - g_live_tick) < CYCLE_TICKS) { return 0; }
    settle();
    return 1;
}

int keyboard_touch(uint32_t x, uint32_t y)
{
    if (y < KB_TOP || y >= SPEC_Y || x >= DISP_W) { return KB_NONE; }

    uint32_t r = (y - KB_TOP) / KB_KEY_H;
    uint32_t c = x / KB_KEY_W;
    if (r >= KB_ROWS || c >= KB_COLS) { return KB_NONE; }

    const char *seq = KEYS[r][c];

    /* [step 289] No click. Multi-tap's worst property is that a press
     * registering is invisible (UM-NATOS-022 3.4) -- the press that "did not
     * register" is usually one that did, which then replaces the letter you
     * wanted -- and the two existing copies answer that with audio.
     *
     * This one answers it with the LIVE KEY HIGHLIGHT: the key being cycled is
     * drawn blue for as long as another tap can still change it, so the
     * feedback says WHICH key and FOR HOW LONG, where a beep said only that
     * something had happened. Audio is off in this view by request.
     *
     * term.c and notes.c keep their click. If they migrate here (285a), this
     * becomes a flag rather than a decision made on their behalf. */

    if (seq[0] == '<' && seq[1] == 0) {
        settle();
        if (g_len) { g_text[--g_len] = 0; }
        return KB_EDIT;
    }
    if (seq[0] == '>' && seq[1] == 0) {
        settle();
        return KB_SUBMIT;
    }

    int same = (g_live_row == (int)r && g_live_col == (int)c);
    if (same) {
        uint32_t n = 0u;
        while (seq[n]) { n++; }
        g_live_index = (g_live_index + 1u) % n;
        if (g_len) { g_text[g_len - 1u] = seq[g_live_index]; }
    } else {
        settle();
        if (g_len + 1u >= KB_TEXT_MAX) {
            return KB_NONE;     /* full: refuse rather than silently drop */
        }
        g_live_index = 0u;
        g_text[g_len++] = seq[0];
        g_text[g_len]   = 0;
        g_live_row = (int)r;
        g_live_col = (int)c;
        draw_key(r, c, 1);
    }
    g_live_tick = timer_ticks();
    return KB_EDIT;
}
