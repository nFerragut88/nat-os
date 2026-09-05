/* nat-os — the web view. See browser.h for what this is and is not. */

#include "browser.h"
#include "webfetch.h"
#include "display.h"
#include "desktop.h"
#include "keyboard.h"
#include "timer.h"

/* ---- layout --------------------------------------------------------------
 *
 * Two screens sharing the top: the page, and the URL editor. The editor puts a
 * keyboard where the text is, so the geometry below the header differs; the
 * header and its exit are identical in both, because the way out must not move.
 */
#define HDR_H     22u
#define URL_H     16u
#define URL_Y     HDR_H
#define TXT_Y     (URL_Y + URL_H + 2u)
#define VIEW_H    SPEC_Y
#define BAR_H     14u
#define BAR_Y     (VIEW_H - BAR_H)
#define TXT_H     (BAR_Y - TXT_Y)

#define LINE_H    9u
#define COLS      39u                       /* 240 / 6, less a margin */
#define ROWS      (TXT_H / LINE_H)

_Static_assert(ROWS >= 8u, "a page view under eight lines is not worth having");
_Static_assert(BAR_Y + BAR_H == VIEW_H, "the status bar must meet the region end");

#define BG    COLOR_BLACK
#define FG    COLOR_WHITE
#define DIM   COLOR_GREY
#define OK    COLOR_GREEN
#define BUSY  COLOR_YELLOW
#define BAD   COLOR_RED
#define FIELD 0x2104u

/* ---- state --------------------------------------------------------------- */

static char     g_host[WEB_HOST_MAX] = "google.com";
static int      g_editing;
static int      g_dirty;
static int      g_was_down;
static uint32_t g_scroll;
static int      g_want_fetch;
static int      g_last_state = -1;

static void put(uint32_t x, uint32_t y, const char *s, uint16_t fg, uint16_t bg)
{
    display_text(x, y, s, fg, bg, 1u);
}

/* ---- the page ------------------------------------------------------------ */

/* Wrap the response into fixed columns, honouring the newlines that are already
 * in it. A response is not a paragraph -- it is a status line, headers, then
 * whatever the body is -- so the newlines carry real structure and re-flowing
 * across them would destroy the only formatting there is. */
static void draw_text(void)
{
    display_fill_rect(0, TXT_Y, DISP_W, TXT_H, BG);

    const char *b = webfetch_body();
    uint32_t    n = webfetch_len();
    uint32_t    i = 0u, line = 0u, skipped = 0u;
    char        buf[COLS + 1u];

    while (i < n && line < ROWS) {
        uint32_t c = 0u;
        while (i < n && c < COLS && b[i] != '\n') { buf[c++] = b[i++]; }
        buf[c] = 0;
        if (i < n && b[i] == '\n') { i++; }

        if (skipped < g_scroll) { skipped++; continue; }

        /* The status line is the answer; the rest is context. */
        put(2u, TXT_Y + line * LINE_H, buf, line == 0u && !g_scroll ? FG : DIM, BG);
        line++;
    }

    if (n == 0u) {
        put(2u, TXT_Y + 2u, "nothing fetched yet -- tap go", DIM, BG);
    }
}

static void draw_bar(void)
{
    display_fill_rect(0, BAR_Y, DISP_W, BAR_H, BG);

    const char *msg = webfetch_status();
    uint16_t    col = DIM;
    switch (webfetch_state()) {
    case WEB_RESOLVING:
    case WEB_CONNECTING:
    case WEB_REQUESTING: col = BUSY; break;
    case WEB_DONE:       col = OK;   break;
    case WEB_FAILED:     col = BAD;  break;
    default:             msg = "ready";  break;
    }
    put(2u, BAR_Y + 3u, msg, col, BG);

    /* [step 299] The board's OWN address, shown whenever there is no fetch in
     * flight. "no network" is this view reporting that netif_wifi_ip() returned
     * zero, and the only useful follow-up question is what it actually holds --
     * which the user could not see without opening a different app. A view that
     * reports a network failure should say what it believes about the network. */
    if (webfetch_state() == WEB_IDLE || webfetch_state() == WEB_FAILED) {
        extern uint32_t netif_wifi_ip(void);
        uint32_t ip = netif_wifi_ip();
        static char me[20];
        uint32_t at = 0u;
        if (ip) {
            for (uint32_t b = 0u; b < 4u; b++) {
                uint32_t v = (ip >> (8u * b)) & 0xFFu;
                if (v >= 100u) { me[at++] = (char)('0' + v / 100u); }
                if (v >= 10u)  { me[at++] = (char)('0' + (v / 10u) % 10u); }
                me[at++] = (char)('0' + v % 10u);
                if (b < 3u) { me[at++] = '.'; }
            }
        } else {
            const char *q = "no address -- run wifi";
            while (*q) { me[at++] = *q++; }
        }
        me[at] = 0;
        put(2u, BAR_Y + 3u, me, ip ? DIM : BAD, BG);
    }

    /* The HTTP code, right-aligned, because it is the one number that says what
     * happened and it should not be hunted for in wrapped text. */
    uint32_t code = webfetch_code();
    if (code) {
        char c[4];
        c[0] = (char)('0' + (code / 100u) % 10u);
        c[1] = (char)('0' + (code / 10u) % 10u);
        c[2] = (char)('0' + code % 10u);
        c[3] = 0;
        put(DISP_W - 24u, BAR_Y + 3u, c, code < 400u ? OK : BAD, BG);
    }
}

static void draw_chrome(void)
{
    display_fill_rect(0, 0, DISP_W, HDR_H, COLOR_BLUE);
    put(6u, 8u, g_editing ? "url" : "web", FG, COLOR_BLUE);
    /* The way out, drawn -- step 281a: the handler was always there and a
     * header painted over it. */
    display_fill_rect(DISP_W - 22u, 0, 22u, HDR_H, COLOR_RED);
    put(DISP_W - 14u, 8u, "x", FG, COLOR_RED);
}

static void draw_url(uint16_t bg)
{
    display_fill_rect(0, URL_Y, DISP_W, URL_H, BG);
    display_fill_rect(2u, URL_Y + 1u, DISP_W - 62u, URL_H - 2u, bg);
    put(4u, URL_Y + 5u, g_host, FG, bg);

    display_fill_rect(DISP_W - 56u, URL_Y + 1u, 26u, URL_H - 2u, COLOR_BLUE);
    put(DISP_W - 52u, URL_Y + 5u, "go", FG, COLOR_BLUE);
    display_fill_rect(DISP_W - 28u, URL_Y + 1u, 26u, URL_H - 2u, FIELD);
    put(DISP_W - 25u, URL_Y + 5u, "ed", DIM, FIELD);
}

static void draw_all(void)
{
    if (g_editing) {
        display_fill_rect(0, 0, DISP_W, KB_TOP, BG);
        draw_chrome();
        display_fill_rect(4u, HDR_H + 10u, DISP_W - 8u, 14u, FIELD);
        const char *t = keyboard_text();
        put(6u, HDR_H + 13u, t[0] ? t : "type a host, then go", t[0] ? FG : DIM, FIELD);
        keyboard_draw();
        return;
    }

    display_fill_rect(0, 0, DISP_W, VIEW_H, BG);
    draw_chrome();
    draw_url(FIELD);
    draw_text();
    draw_bar();
}

/* ---- the view ------------------------------------------------------------ */

void browser_open(void)
{
    g_editing  = 0;
    g_was_down = 0;
    g_scroll   = 0u;
    g_dirty    = 1;
}

void browser_service(void)
{
    if (g_want_fetch) {
        g_want_fetch = 0;
        g_scroll     = 0u;
        (void)webfetch_start(g_host, "/");
    }
    webfetch_service();
}

void browser_frame(void)
{
    if (g_editing && keyboard_tick()) { g_dirty = 1; }

    /* A fetch changes state on the net task; the screen has to notice. */
    int st = webfetch_state();
    if (st != g_last_state) { g_last_state = st; g_dirty = 1; }
    else if (st == WEB_REQUESTING)  { g_dirty = 1; }   /* body still growing */

    if (!g_dirty) { return; }
    g_dirty = 0;
    draw_all();
}

void browser_touch(uint32_t x, uint32_t y, int down)
{
    if (!down) { g_was_down = 0; return; }
    if (g_was_down) { return; }
    g_was_down = 1;

    if (g_editing) {
        int r = keyboard_touch(x, y);
        if (r == KB_EDIT) { g_dirty = 1; return; }
        if (r != KB_SUBMIT) { return; }
        const char *t = keyboard_text();
        if (t[0]) {
            uint32_t i = 0u;
            for (; i + 1u < WEB_HOST_MAX && t[i]; i++) { g_host[i] = t[i]; }
            g_host[i] = 0;
        }
        g_editing    = 0;
        g_want_fetch = 1;       /* the net task performs it */
        g_dirty      = 1;
        return;
    }

    if (y >= URL_Y && y < URL_Y + URL_H) {
        if (x >= DISP_W - 28u) {                /* ed */
            g_editing = 1;
            keyboard_reset("go");
            g_dirty = 1;
        } else if (x >= DISP_W - 56u) {         /* go */
            g_want_fetch = 1;
            g_dirty      = 1;
        }
        return;
    }

    /* Anywhere in the text scrolls it: a page longer than the screen needs a
     * way down, and a scrollbar would cost columns this view cannot spare. The
     * bottom half pages down, the top half pages up. */
    if (y >= TXT_Y && y < BAR_Y) {
        if (y > TXT_Y + TXT_H / 2u) { g_scroll += ROWS / 2u; }
        else if (g_scroll >= ROWS / 2u) { g_scroll -= ROWS / 2u; }
        else { g_scroll = 0u; }
        g_dirty = 1;
    }
}
