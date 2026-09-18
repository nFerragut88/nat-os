/* nat-os — the music view. See player.h. */

#include "player.h"
#include "mp3.h"
#include "fat.h"
#include "display.h"
#include "uart.h"

#define SRAM1 __attribute__((section(".sram1")))

/* ---- geometry (240 x 288: the region plus the band, claimed like the web
 *      view claims it; the spectrum strip below SPEC_Y stays the kernel's) -- */
#define HDR_H     22u
#define LIST_Y    24u
#define ROW_H     18u
#define ROWS      9u
#define LIST_W    204u              /* rows; the scroll column is to the right */
#define SCROLL_X  208u
#define SCROLL_W  (DISP_W - SCROLL_X)
#define LIST_END  (LIST_Y + ROWS * ROW_H)           /* 186 */
#define NP_Y      190u              /* now playing: name, time, bar */
#define BAR_Y     (NP_Y + 30u)
#define BAR_H     8u
#define BTN_Y     234u
#define BTN_H     50u
#define BTN_W     (DISP_W / 4u)     /* prev, play/pause, stop, next */
#define LIST_CHARS 33u              /* 204 px at 6 px a glyph, less a margin */

#define BG      COLOR_BLACK
#define FG      COLOR_WHITE

/* ---- the song list --------------------------------------------------------- */
#define PL_MAX  32u

SRAM1 static char g_names[PL_MAX][FAT_NAME_MAX];
static uint32_t g_count;
static int      g_listed;           /* 0 = not yet, 1 = done, <0 = error */

static int      g_sel = -1;         /* highlighted row */
static uint32_t g_top;              /* first row shown */
static int      g_cur = -1;         /* song the player was last told to play */
static int      g_want = -1;        /* song to start once the decoder is free */
static uint32_t g_auto_seq;         /* last finished song already advanced from */
static int      g_start_err;        /* why the last start was refused, 0 if not */

static volatile uint32_t g_dirty;   /* bumped by touch, read by frame */
static uint32_t g_drawn;
static int      g_full = 1;
static int      g_was_down;
static mp3_status_t g_shown;        /* what the now-playing area last drew */

static int ends_mp3(const char *n)
{
    uint32_t len = 0;
    while (n[len]) { len++; }
    if (len < 4u) {
        return 0;
    }
    const char *e = n + len - 4u;
    return e[0] == '.' && (e[1] | 0x20) == 'm' && (e[2] | 0x20) == 'p' && e[3] == '3';
}

/* The card's root, .mp3 only. A name too long for FAT_NAME_MAX comes back
 * truncated; it is stored with its last character replaced by '*', which
 * fat.c matches as a prefix, so the song still opens. */
static void build_list(void)
{
    g_count = 0;
    if (!fat_mounted() && fat_mount() != FAT_OK) {
        g_listed = MP3_E_NOCARD;
        return;
    }
    static fat_dir_t d;
    static fat_dirent_t e;
    int rc = fat_dir_open(&d, "");
    if (rc) {
        g_listed = rc;
        return;
    }
    while (g_count < PL_MAX && (rc = fat_dir_next(&d, &e)) == 1) {
        if ((e.attr & FAT_ATTR_DIR) || !ends_mp3(e.name)) {
            continue;
        }
        uint32_t i = 0;
        for (; e.name[i] && i < FAT_NAME_MAX - 1u; i++) {
            g_names[g_count][i] = e.name[i];
        }
        g_names[g_count][i] = 0;
        if (e.truncated && i > 0u) {
            g_names[g_count][i - 1u] = '*';
        }
        g_count++;
    }
    g_listed = (rc < 0) ? rc : 1;
}

/* ---- transport --------------------------------------------------------------
 *
 * Starting a song while another is playing cannot be one call: the decoder
 * task has to notice the stop, drain and release the DMA first. So a start is
 * a request (g_want), carried out by player_service() once mp3_busy() is
 * false. The touch task only ever sets flags. */

static void start(int idx)
{
    if (g_count == 0u) {
        return;
    }
    idx = (int)(((uint32_t)idx + g_count) % g_count);
    mp3_set_pause(0);
    if (mp3_busy()) {
        mp3_request_stop();
    }
    g_want = idx;
    g_sel  = idx;
    /* Keep the song on screen. */
    if ((uint32_t)idx < g_top) {
        g_top = (uint32_t)idx;
    } else if ((uint32_t)idx >= g_top + ROWS) {
        g_top = (uint32_t)idx - ROWS + 1u;
    }
    g_dirty++;
}

void player_service(void)
{
    if (g_want >= 0 && !mp3_busy()) {
        int idx = g_want;
        g_want = -1;
        g_start_err = mp3_play(g_names[idx]);
        g_cur = idx;
        g_dirty++;
    }

    /* A song that reached its end -- not one that was stopped -- moves on. */
    mp3_status_t st;
    mp3_status(&st);
    if (st.state == MP3_ST_FINISHED && st.seq != g_auto_seq && g_cur >= 0 && g_want < 0) {
        g_auto_seq = st.seq;
        start(g_cur + 1);
    }
}

/* ---- drawing ----------------------------------------------------------------- */

static void put(uint32_t x, uint32_t y, const char *s, uint16_t fg, uint16_t bg)
{
    display_text(x, y, s, fg, bg, 1u);
}

static void clip(char *dst, const char *src, uint32_t max)
{
    uint32_t i = 0;
    for (; src[i] && i < max; i++) {
        dst[i] = src[i];
    }
    if (src[i] && i > 0u) {
        dst[i - 1u] = '~';                  /* there was more */
    }
    dst[i] = 0;
}

static void fmt_time(char *b, uint32_t s)
{
    uint32_t m = s / 60u;
    uint32_t k = 0;
    if (m >= 100u) { b[k++] = (char)('0' + (m / 100u) % 10u); }
    if (m >= 10u)  { b[k++] = (char)('0' + (m / 10u) % 10u); }
    b[k++] = (char)('0' + m % 10u);
    b[k++] = ':';
    b[k++] = (char)('0' + (s % 60u) / 10u);
    b[k++] = (char)('0' + s % 10u);
    b[k] = 0;
}

static void draw_header(void)
{
    display_fill_rect(0, 0, DISP_W, HDR_H, COLOR_BLUE);
    put(6u, 8u, "music", FG, COLOR_BLUE);
    /* The way out, drawn: desktop_chrome_touch() owns the top-right 22x22 of
     * every view and checks it before this view sees the press. */
    display_fill_rect(DISP_W - 22u, 0, 22u, HDR_H, COLOR_RED);
    put(DISP_W - 14u, 8u, "x", FG, COLOR_RED);
}

static void draw_list(void)
{
    display_fill_rect(0, LIST_Y, LIST_W + 2u, LIST_END - LIST_Y, BG);
    if (g_listed < 0) {
        put(6u, LIST_Y + 6u, "can't read the card:", COLOR_RED, BG);
        put(6u, LIST_Y + 18u, mp3_error_text(g_listed), COLOR_RED, BG);
        return;
    }
    if (g_count == 0u) {
        put(6u, LIST_Y + 6u, "no .mp3 files in the", FG, BG);
        put(6u, LIST_Y + 18u, "card's top folder", FG, BG);
        return;
    }
    char line[LIST_CHARS + 1u];
    for (uint32_t r = 0; r < ROWS && g_top + r < g_count; r++) {
        uint32_t i = g_top + r;
        uint32_t y = LIST_Y + r * ROW_H;
        uint16_t bg = ((int)i == g_sel) ? COLOR_BLUE : BG;
        uint16_t fg = ((int)i == g_cur) ? COLOR_GREEN : FG;
        if (bg != BG) {
            display_fill_rect(0, y, LIST_W, ROW_H - 1u, bg);
        }
        clip(line, g_names[i], LIST_CHARS);
        put(4u, y + 5u, line, fg, bg);
    }
}

static void draw_scroll(void)
{
    uint32_t mid = LIST_Y + (LIST_END - LIST_Y) / 2u;
    int up = g_top > 0u;
    int dn = g_top + ROWS < g_count;
    display_fill_rect(SCROLL_X, LIST_Y, SCROLL_W, mid - LIST_Y - 1u, up ? COLOR_GREY : BG);
    display_fill_rect(SCROLL_X, mid + 1u, SCROLL_W, LIST_END - mid - 1u, dn ? COLOR_GREY : BG);
    display_text(SCROLL_X + 10u, LIST_Y + 30u, "^", up ? FG : COLOR_GREY, up ? COLOR_GREY : BG, 2u);
    display_text(SCROLL_X + 10u, mid + 30u, "v", dn ? FG : COLOR_GREY, dn ? COLOR_GREY : BG, 2u);
}

static const char *state_word(const mp3_status_t *st)
{
    switch (st->state) {
    case MP3_ST_STARTING: return "loading";
    case MP3_ST_PLAYING:  return "playing";
    case MP3_ST_PAUSED:   return "paused";
    case MP3_ST_FINISHED: return "finished";
    case MP3_ST_STOPPED:  return "stopped";
    case MP3_ST_ERROR:    return "error";
    default:              return "";
    }
}

static void draw_now(const mp3_status_t *st)
{
    display_fill_rect(0, NP_Y, DISP_W, BTN_Y - NP_Y - 2u, BG);
    char line[40];
    if (g_cur >= 0) {
        clip(line, g_names[g_cur], 39u);
        put(4u, NP_Y, line, COLOR_GREEN, BG);
    } else {
        put(4u, NP_Y, "tap a song, then tap it again", COLOR_GREY, BG);
    }

    if (g_start_err) {
        put(4u, NP_Y + 14u, mp3_error_text(g_start_err), COLOR_RED, BG);
    } else if (st->state == MP3_ST_ERROR) {
        put(4u, NP_Y + 14u, mp3_error_text(st->err), COLOR_RED, BG);
    } else if (st->state != MP3_ST_IDLE) {
        char t[24];
        fmt_time(t, st->pos_s);
        uint32_t k = 0;
        while (t[k]) { k++; }
        t[k++] = ' '; t[k++] = '/'; t[k++] = ' ';
        if (st->total_s) {
            if (!st->exact) { t[k++] = '~'; }
            fmt_time(t + k, st->total_s);
        } else {
            t[k++] = '-'; t[k++] = ':'; t[k++] = '-'; t[k++] = '-'; t[k] = 0;
        }
        put(4u, NP_Y + 14u, t, FG, BG);
        put(DISP_W - 6u * 9u, NP_Y + 14u, state_word(st), COLOR_YELLOW, BG);
    }

    /* The bar: an outline, filled by elapsed / total. */
    display_fill_rect(4u, BAR_Y, DISP_W - 8u, BAR_H, COLOR_GREY);
    if (st->total_s) {
        uint32_t w = (DISP_W - 10u) * (st->pos_s < st->total_s ? st->pos_s : st->total_s)
                   / st->total_s;
        display_fill_rect(5u, BAR_Y + 1u, w, BAR_H - 2u, COLOR_GREEN);
    }
}

static void draw_buttons(const mp3_status_t *st)
{
    static const char *const label[4] = { "|<", ">", "[]", ">|" };
    for (uint32_t b = 0; b < 4u; b++) {
        uint32_t x = b * BTN_W;
        uint16_t c = (b == 1u) ? COLOR_GREEN : (b == 2u ? COLOR_RED : COLOR_GREY);
        display_fill_rect(x + 2u, BTN_Y, BTN_W - 4u, BTN_H, c);
        const char *l = label[b];
        if (b == 1u && st->state == MP3_ST_PLAYING) {
            l = "||";
        }
        uint32_t w = (l[1] ? 2u : 1u) * 12u;      /* 6x8 glyphs at scale 2 */
        display_text(x + (BTN_W - w) / 2u, BTN_Y + (BTN_H - 16u) / 2u, l, FG, c, 2u);
    }
}

void player_open(void)
{
    /* ZERO, for the reason wifiapp_open() and browser_open() give: the
     * launcher opens on RELEASE, so no press is in flight. */
    g_was_down = 0;
    g_full = 1;
    g_dirty++;
}

void player_frame(void)
{
    if (g_listed == 0) {
        build_list();
        g_full = 1;
    }

    mp3_status_t st;
    mp3_status(&st);
    int now_changed = st.state != g_shown.state || st.pos_s != g_shown.pos_s
                   || st.total_s != g_shown.total_s || st.seq != g_shown.seq
                   || st.err != g_shown.err;

    uint32_t seq = g_dirty;
    if (!g_full && seq == g_drawn && !now_changed) {
        return;                 /* nothing moved: no SPI at all */
    }
    if (g_full) {
        display_fill_rect(0, 0, DISP_W, SPEC_Y, BG);
        draw_header();
    }
    if (g_full || seq != g_drawn) {
        draw_list();
        draw_scroll();
    }
    draw_now(&st);
    /* Only the play/pause face depends on the status; repaint the buttons
     * when that could have changed, not every second. */
    if (g_full || seq != g_drawn || st.state != g_shown.state) {
        draw_buttons(&st);
    }
    g_shown = st;
    g_drawn = seq;
    g_full  = 0;
}

void player_touch(uint32_t x, uint32_t y, int down)
{
    if (!down) {
        g_was_down = 0;
        return;
    }
    if (g_was_down) {
        return;
    }
    g_was_down = 1;
    g_start_err = 0;

    if (y >= LIST_Y && y < LIST_END) {
        if (x >= SCROLL_X) {
            uint32_t mid = LIST_Y + (LIST_END - LIST_Y) / 2u;
            if (y < mid) {
                g_top = (g_top >= ROWS) ? g_top - ROWS : 0u;
            } else if (g_top + ROWS < g_count) {
                g_top += ROWS;
            }
        } else {
            uint32_t i = g_top + (y - LIST_Y) / ROW_H;
            if (i < g_count) {
                if ((int)i == g_sel) {
                    start((int)i);              /* second tap: play it */
                } else {
                    g_sel = (int)i;             /* first tap: select it */
                }
            }
        }
        g_dirty++;
        return;
    }

    if (y >= BTN_Y && y < BTN_Y + BTN_H) {
        mp3_status_t st;
        mp3_status(&st);
        uint32_t b = x / BTN_W;
        if (b == 0u) {                          /* previous, or restart */
            if (g_cur >= 0 && st.pos_s > 3u && st.state == MP3_ST_PLAYING) {
                start(g_cur);
            } else {
                start(g_cur >= 0 ? g_cur - 1 : (g_sel >= 0 ? g_sel : 0));
            }
        } else if (b == 1u) {                   /* play / pause */
            if (st.state == MP3_ST_PLAYING) {
                mp3_set_pause(1);
            } else if (st.state == MP3_ST_PAUSED) {
                mp3_set_pause(0);
            } else {
                start(g_sel >= 0 ? g_sel : (g_cur >= 0 ? g_cur : 0));
            }
        } else if (b == 2u) {                   /* stop */
            g_want = -1;
            mp3_request_stop();
        } else {                                /* next */
            start(g_cur >= 0 ? g_cur + 1 : (g_sel >= 0 ? g_sel : 0));
        }
        g_dirty++;
    }
}

void player_play_number(uint32_t n)
{
    if (g_listed == 0) {
        build_list();
    }
    if (n >= 1u && n <= g_count) {
        start((int)n - 1);
    }
}

void player_dump(void)
{
    mp3_status_t st;
    mp3_status(&st);
    uart_puts("   music view: listed=");
    uart_put_dec((uint32_t)(g_listed < 0 ? 0 : g_listed));
    if (g_listed < 0) {
        uart_puts(" (");
        uart_puts(mp3_error_text(g_listed));
        uart_puts(")");
    }
    uart_puts(" songs=");
    uart_put_dec(g_count);
    uart_puts(" top=");
    uart_put_dec(g_top);
    uart_puts(" sel=");
    uart_put_dec((uint32_t)(g_sel + 1));
    uart_puts(" cur=");
    uart_put_dec((uint32_t)(g_cur + 1));
    uart_puts(" want=");
    uart_put_dec((uint32_t)(g_want + 1));
    uart_puts("  (1-based, 0 = none)\n   state=");
    uart_puts(state_word(&st));
    uart_puts(" pos=");
    uart_put_dec(st.pos_s);
    uart_puts(" total=");
    uart_put_dec(st.total_s);
    uart_puts(st.exact ? " exact" : " est");
    uart_puts(" seq=");
    uart_put_dec(st.seq);
    uart_puts("\n");
    for (uint32_t i = 0; i < g_count; i++) {
        uart_puts((int)i == g_cur ? "   > " : "     ");
        uart_put_dec(i + 1u);
        uart_puts("  ");
        uart_puts(g_names[i]);
        uart_puts("\n");
    }
}
