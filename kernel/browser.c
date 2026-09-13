/* nat-os — the web view. See browser.h for what this is and is not. */

#include "browser.h"
#include "webfetch.h"
#include "html.h"
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
/* [step 300] 30, was 16. Step 286c enlarged the wifi view's button from 16 px
 * to 28 because a small target at the edge of an uncalibrated resistive panel
 * is not a target -- and this view was then built with 14-pixel controls, in
 * the same session, by the same hand. The lesson was written down and not
 * applied to the next thing built. */
#define URL_H     30u
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
/* [step 390] A link has to be distinguishable from prose at 6x8 pixels with no
 * underline available, so it is colour or it is nothing. Cyan for a link, and
 * the selected one inverts to yellow rather than changing shape -- a target
 * that moves when you touch it is worse than one that does not. */
#define LINK  COLOR_CYAN
#define SELFG COLOR_YELLOW
#define CHAR_W 6u                       /* display_text at scale 1 */

/* ---- state --------------------------------------------------------------- */

/* [step 390] The path, which this view never had. Every fetch was of "/", so a
 * link could not have been followed even if one had been found -- and that is
 * the other half of why links were not worth extracting before now. */
static char     g_host[WEB_HOST_MAX] = "example.com";
static char     g_path[WEB_HOST_MAX] = "/";
static int      g_raw;                  /* show the response, not the page */
static int      g_sel = -1;             /* the highlighted link, or none */
static int      g_editing;
/* [step 306] A sequence, not a flag, and volatile -- step 303's fault carried
 * across before it could be reported a second time. g_dirty is written by the
 * touch task and read by the display task, and a test-then-clear across that
 * boundary loses any set that lands between the two lines. */
static volatile uint32_t g_dirty;
static uint32_t          g_drawn;
static int      g_was_down;
static uint32_t g_scroll;
static int      g_want_fetch;
static int      g_last_state = -1;
static uint32_t g_last_len;     /* [step 301] redraw on CHANGE, not on state */
static int      g_full = 1;     /* the next paint must clear the whole view */

/* [step 331] The same on-screen log the wifi view got at step 324, for the same
 * reason: "it doesn't do anything" cannot be acted on, and every guess made
 * without one today has been wrong. The fetch has four stages and a dozen ways
 * to stop; the bar shows a state, which is not the same as showing what
 * happened. */
#define LOG_LINES 9u
#define LOG_COLS  38u
static char     g_log[LOG_LINES][LOG_COLS + 1u];
static volatile uint32_t g_log_n;

void browser_note(const char *a, uint32_t v, int has_v);
void browser_note(const char *a, uint32_t v, int has_v)
{
    char *d = g_log[g_log_n % LOG_LINES];
    uint32_t k = 0u;
    while (*a && k < LOG_COLS) { d[k++] = *a++; }
    if (has_v) {
        if (k < LOG_COLS) { d[k++] = ' '; }
        char t[11]; uint32_t n = 0u;
        do { t[n++] = (char)('0' + v % 10u); v /= 10u; } while (v && n < 10u);
        while (n && k < LOG_COLS) { d[k++] = t[--n]; }
    }
    d[k] = 0;
    g_log_n++;
    g_dirty++;
}
#define BLOG(a)     browser_note((a), 0u, 0)
#define BLOGV(a,v)  browser_note((a), (uint32_t)(v), 1)

static void put(uint32_t x, uint32_t y, const char *s, uint16_t fg, uint16_t bg)
{
    display_text(x, y, s, fg, bg, 1u);
}

/* ---- following a link ----------------------------------------------------
 *
 * [step 390] An href is one of four things, and three of them are reachable:
 *
 *   http://host/path   absolute -- replaces both
 *   /path              rooted   -- same host, new path
 *   path               relative -- same host, relative to the current directory
 *   https://...        NOT REACHABLE, and it is most of the web
 *
 * The last one is refused OUT LOUD rather than attempted and left to time out.
 * webfetch.h explains why there is no TLS: the handshake wants more heap than
 * this board has in total. A link this machine cannot follow should say so in
 * the one place the person tapping it is looking.
 */
static int follow(const char *href)
{
    uint32_t i = 0u;

    if (href[0] == 'h' && href[1] == 't' && href[2] == 't' && href[3] == 'p') {
        if (href[4] == 's') {
            BLOG("https -- no TLS on this board");
            return 0;
        }
        if (href[4] == ':' && href[5] == '/' && href[6] == '/') {
            i = 7u;
            uint32_t h = 0u;
            while (href[i] && href[i] != '/' && h + 1u < WEB_HOST_MAX) {
                g_host[h++] = href[i++];
            }
            g_host[h] = 0;
            uint32_t p = 0u;
            if (!href[i]) { g_path[p++] = '/'; }
            while (href[i] && p + 1u < WEB_HOST_MAX) { g_path[p++] = href[i++]; }
            g_path[p] = 0;
            return 1;
        }
        /* "http" beginning a relative path, e.g. "httpd.html". Falls through. */
    }

    /* A fragment alone goes nowhere: it is a position in the page already
     * shown, and this view has no anchors to scroll to. */
    if (href[0] == '#' || !href[0]) { return 0; }

    if (href[0] == '/') {
        uint32_t p = 0u;
        while (href[p] && p + 1u < WEB_HOST_MAX) { g_path[p] = href[p]; p++; }
        g_path[p] = 0;
        return 1;
    }

    /* Relative: keep everything up to and including the last '/' of the
     * current path, and append. */
    uint32_t cut = 0u;
    for (uint32_t k = 0u; g_path[k]; k++) {
        if (g_path[k] == '/') { cut = k + 1u; }
    }
    uint32_t p = cut;
    for (uint32_t k = 0u; href[k] && p + 1u < WEB_HOST_MAX; k++) {
        g_path[p++] = href[k];
    }
    g_path[p] = 0;
    return 1;
}

/* ---- the page ------------------------------------------------------------ */

/* [step 390] The page, as words.
 *
 * Until now this wrapped the RAW response into fixed columns -- status line,
 * headers, then whatever markup arrived, angle brackets and all. That was
 * honest about what the machine held and useless as a page: on any real
 * document the eight visible lines were a doctype and the opening of a <head>.
 *
 * html.c turns the response into text and a table of links. This lays that out
 * and REMEMBERS WHERE EACH ROW CAME FROM, because a tap has to be turned back
 * into an offset in the rendered text to know which link it landed on.
 *
 * The raw view is still one tap away -- the header toggles it -- because "show
 * exactly what came back" is a diagnostic this view has needed more than once,
 * and it should not be lost to a prettier default.
 */

/* Where each drawn row begins and ends in the text being shown, so a tap can be
 * mapped back to an offset. Only the VISIBLE rows: nothing off-screen is
 * tappable, so nothing off-screen needs remembering. */
static uint32_t g_row_a[ROWS];
static uint32_t g_row_b[ROWS];
static uint32_t g_rows_drawn;

/* One display row: at most COLS characters, broken at a space rather than
 * mid-word where there is one. Returns the offset to continue from. */
static uint32_t wrap_one(const char *t, uint32_t n, uint32_t i, uint32_t *end)
{
    uint32_t start = i, last_space = 0u;
    uint32_t e = start;
    while (e < n && (e - start) < COLS && t[e] != '\n') {
        if (t[e] == ' ') { last_space = e; }
        e++;
    }
    /* Broke mid-word: retreat to the last space on the row -- but only when
     * there is one. A single word longer than the row has to be cut somewhere. */
    if (e < n && t[e] != '\n' && (e - start) == COLS && last_space > start) {
        e = last_space;
    }
    *end = e;
    if (e < n && (t[e] == '\n' || t[e] == ' ')) { return e + 1u; }
    return e;
}

/* [step 390] How many rows the whole text lays out to.
 *
 * Needed because the scroll could run off the end: ROWS is 24 and a rendered
 * page is often six, so one tap in the lower half left a BLANK VIEW with no
 * indication that anything was wrong and no way back except guessing that the
 * upper half scrolls up. The raw dump was twenty-odd rows and hid this; turning
 * markup into words is what made it reachable. Same wrap_one() as the draw, so
 * the count cannot disagree with the layout. */
static uint32_t count_rows(const char *t, uint32_t n)
{
    uint32_t i = 0u, rows = 0u;
    while (i < n) {
        uint32_t end = 0u;
        i = wrap_one(t, n, i, &end);
        rows++;
        if (rows > 4096u) { break; }        /* nothing sane reaches this */
    }
    return rows;
}

static uint32_t max_scroll(void)
{
    const char *t = g_raw ? webfetch_body() : html_text();
    uint32_t    n = g_raw ? webfetch_len()  : html_len();
    uint32_t rows = count_rows(t, n);
    return rows > ROWS ? rows - ROWS : 0u;
}

static void draw_text(void)
{
    display_fill_rect(0, TXT_Y, DISP_W, TXT_H, BG);
    g_rows_drawn = 0u;

    const char *t = g_raw ? webfetch_body() : html_text();
    uint32_t    n = g_raw ? webfetch_len()  : html_len();
    uint32_t    i = 0u, line = 0u, skipped = 0u;
    char        buf[COLS + 1u];

    while (i < n && line < ROWS) {
        uint32_t end  = 0u;
        uint32_t next = wrap_one(t, n, i, &end);

        if (skipped < g_scroll) { skipped++; i = next; continue; }

        uint32_t y = TXT_Y + line * LINE_H;
        g_row_a[line] = i;
        g_row_b[line] = end;

        if (g_raw) {
            uint32_t c = 0u;
            for (uint32_t p = i; p < end && c < COLS; p++) { buf[c++] = t[p]; }
            buf[c] = 0;
            put(2u, y, buf, line == 0u && !g_scroll ? FG : DIM, BG);
        } else {
            /* Draw the row in RUNS of one colour: a link can start or end
             * anywhere in it, and a link the reader cannot tell from prose is
             * not a link anybody will ever tap. */
            uint32_t p = i;
            while (p < end) {
                int who = html_link_at(p);
                uint32_t q = p, c = 0u;
                while (q < end && html_link_at(q) == who && c < COLS) {
                    buf[c++] = t[q++];
                }
                buf[c] = 0;
                put(2u + (p - i) * CHAR_W, y, buf,
                    who >= 0 ? (who == g_sel ? SELFG : LINK) : FG, BG);
                p = q;
            }
        }
        i = next;
        line++;
    }
    g_rows_drawn = line;

    if (n == 0u) {
        uint32_t total = g_log_n;
        uint32_t show  = total < LOG_LINES ? total : LOG_LINES;
        for (uint32_t r = 0u; r < show; r++) {
            uint32_t idx = (total - show + r) % LOG_LINES;
            put(2u, TXT_Y + 2u + r * LINE_H, g_log[idx],
                r + 1u == show ? FG : DIM, BG);
        }
        if (!total) { put(2u, TXT_Y + 2u, "tap go", DIM, BG); }
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

    /* [step 330] While resolving, say WHO is being asked and how many times.
     * A DNS server of 0.0.0.0 is a query addressed to nobody, and it looks
     * exactly like a slow one. */
    if (webfetch_state() == WEB_RESOLVING) {
        extern uint32_t webfetch_dns_server(void);
        extern uint32_t webfetch_tries(void);
        uint32_t srv = webfetch_dns_server();
        static char dm[34];
        uint32_t at = 0u;
        const char *q = "dns ";
        while (*q) { dm[at++] = *q++; }
        for (uint32_t b = 0u; b < 4u; b++) {
            uint32_t v = (srv >> (8u * b)) & 0xFFu;
            if (v >= 100u) { dm[at++] = (char)('0' + v / 100u); }
            if (v >= 10u)  { dm[at++] = (char)('0' + (v / 10u) % 10u); }
            dm[at++] = (char)('0' + v % 10u);
            if (b < 3u) { dm[at++] = '.'; }
        }
        q = " try ";
        while (*q) { dm[at++] = *q++; }
        dm[at++] = (char)('0' + (webfetch_tries() % 10u));
        dm[at] = 0;
        put(2u, BAR_Y + 3u, dm, srv ? BUSY : BAD, BG);
    }

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
    put(6u, 8u, g_editing ? "url" : (g_raw ? "raw" : "web"), FG, COLOR_BLUE);
    /* The way out, drawn -- step 281a: the handler was always there and a
     * header painted over it. */
    display_fill_rect(DISP_W - 22u, 0, 22u, HDR_H, COLOR_RED);
    put(DISP_W - 14u, 8u, "x", FG, COLOR_RED);
}

static void draw_url(uint16_t bg)
{
    display_fill_rect(0, URL_Y, DISP_W, URL_H, BG);
    display_fill_rect(2u, URL_Y + 2u, DISP_W - 100u, URL_H - 4u, bg);
    /* [step 390] host AND path: a view that can follow links has to say where
     * it actually is, and after one tap the host alone no longer does. */
    static char shown[WEB_HOST_MAX * 2u];
    uint32_t k = 0u;
    for (uint32_t i = 0u; g_host[i] && k + 1u < sizeof shown; i++) { shown[k++] = g_host[i]; }
    for (uint32_t i = 0u; g_path[i] && k + 1u < sizeof shown; i++) { shown[k++] = g_path[i]; }
    shown[k] = 0;
    /* The field holds 22 characters; a long path is shown by its TAIL, because
     * the end of a path is what distinguishes two pages and the start is what
     * they have in common. */
    const char *t = shown;
    if (k > 22u) { t = &shown[k - 22u]; }
    put(5u, URL_Y + 12u, t, FG, bg);

    /* Two 46-wide, 26-tall targets. A fingertip on this panel is wider than
     * the old 26x14 was in either direction. */
    display_fill_rect(DISP_W - 96u, URL_Y + 2u, 46u, URL_H - 4u, COLOR_BLUE);
    put(DISP_W - 79u, URL_Y + 12u, "go", FG, COLOR_BLUE);
    display_fill_rect(DISP_W - 48u, URL_Y + 2u, 46u, URL_H - 4u, FIELD);
    put(DISP_W - 34u, URL_Y + 12u, "ed", FG, FIELD);
}

static void draw_all(void)
{
    if (g_editing) {
        if (g_full) { display_fill_rect(0, 0, DISP_W, SPEC_Y, BG); g_full = 0; }
        display_fill_rect(0, HDR_H, DISP_W, KB_TOP - HDR_H, BG);
        draw_chrome();
        display_fill_rect(4u, HDR_H + 10u, DISP_W - 8u, 14u, FIELD);
        const char *t = keyboard_text();
        put(6u, HDR_H + 13u, t[0] ? t : "type a host, then go", t[0] ? FG : DIM, FIELD);
        keyboard_draw();
        return;
    }

    /* [step 301] Clear the WHOLE view only when entering it or leaving the
     * editor. Every draw_* below fills its own region, so a repaint does not
     * need the screen blanked first -- and blanking it on every frame is what
     * the flicker was: black, then content, eight times a second. */
    if (g_full) {
        display_fill_rect(0, 0, DISP_W, VIEW_H, BG);
        g_full = 0;
    }
    draw_chrome();
    draw_url(FIELD);
    draw_text();
    draw_bar();
}

void browser_dump_rows(void)
{
    extern void uart_puts(const char *);
    extern void uart_putc(char);
    extern void uart_put_dec(unsigned int);

    uart_puts("   web      mode=");
    uart_puts(g_raw ? "raw" : "page");
    uart_puts("  at=");
    uart_puts(g_host);
    uart_puts(g_path);
    uart_puts("  sel=");
    if (g_sel >= 0) { uart_put_dec((unsigned)g_sel); } else { uart_puts("none"); }
    uart_puts("  text=");
    uart_put_dec(html_len());
    uart_puts("B  links=");
    uart_put_dec(html_link_count());
    if (html_dropped()) {
        uart_puts("  TEXT DROPPED=");
        uart_put_dec(html_dropped());
    }
    if (html_links_dropped()) {
        uart_puts("  LINKS DROPPED=");
        uart_put_dec(html_links_dropped());
    }
    uart_puts("\n");

    const char *t = g_raw ? webfetch_body() : html_text();
    for (uint32_t r = 0u; r < g_rows_drawn; r++) {
        uart_puts("     ");
        uart_put_dec(r);
        uart_puts(g_row_a[r] < 10u ? " |" : " |");
        /* A link run is bracketed, so the ROWS themselves say where a tap
         * would land rather than a separate table that could disagree. */
        int was = -1;
        for (uint32_t p = g_row_a[r]; p < g_row_b[r]; p++) {
            int who = g_raw ? -1 : html_link_at(p);
            if (who != was) {
                if (was >= 0) { uart_putc(']'); }
                if (who >= 0) { uart_putc('['); }
                was = who;
            }
            uart_putc(t[p]);
        }
        if (was >= 0) { uart_putc(']'); }
        uart_puts("|\n");
    }
    for (uint32_t i = 0u; i < html_link_count(); i++) {
        const html_link_t *L = html_link(i);
        uart_puts("     link ");
        uart_put_dec(i);
        uart_puts(" -> ");
        uart_puts(L->href);
        uart_puts("\n");
    }
}

/* ---- the view ------------------------------------------------------------ */

void browser_open(void)
{
    g_editing  = 0;
    /* [step 306] The launcher opens on a PRESS and the touch task keeps
     * reporting it to whoever owns the screen next. Clearing this armed the
     * handler for a press already spent, so the first thing done in the view
     * was acted on at the ICON's coordinates -- step 305a, in the view built
     * after it. */
    /* [step 310] ZERO, not one. Step 305a set this to 1 on the reasoning that
     * the launcher opens on a PRESS still being held -- and it does not:
     * desktop_touch() calls open_selected() from its RELEASE branch, after
     * clearing its own g_was_down. The finger is already up when this runs.
     *
     * So arming the handler here made it wait for a release that had already
     * happened: the next press was ignored, its release cleared the flag, and
     * the press after that worked. That is "only works the second time" --
     * introduced by the fix aimed at "only works the second time", from a
     * premise about the launcher that was never checked against the launcher.
     *
     * The check is one line away in desktop.c and would have cost nothing. */
    g_was_down = 0;
    g_scroll   = 0u;
    g_full     = 1;
    g_dirty++;
}

void browser_service(void)
{
    if (g_want_fetch) {
        g_want_fetch = 0;
        g_scroll     = 0u;
        BLOG("starting fetch");
        /* [step 390] g_path, not "/". Every fetch this view made was of the
         * root, which is the other half of why extracting links was not worth
         * doing before: there was nowhere to put the one you had found. */
        if (webfetch_start(g_host, g_path) != 0) { BLOG("start refused"); }
    }
    webfetch_service();
}

void browser_frame(void)
{
    if (g_editing && keyboard_tick()) { g_dirty++; }

    /* A fetch changes state on the net task; the screen has to notice. */
    /* [step 301] A fetch changes state on the net task and the screen has to
     * notice -- but "notice" means when something CHANGED. Marking the view
     * dirty every frame while REQUESTING repainted it eight times a second
     * whether or not a byte had arrived. The response length is the thing that
     * actually moves, so that is what is watched. */
    int      st = webfetch_state();
    uint32_t ln = webfetch_len();
    if (st != g_last_state) { g_last_state = st; g_dirty++; }
    if (ln != g_last_len)   { g_last_len   = ln; g_dirty++; }

    /* [step 390] Re-render when the response CHANGES, not on every frame.
     * html_render() is a full pass over the body and this runs eight times a
     * second; rendering unconditionally would spend most of the view's budget
     * re-deriving text that had not moved. The length is what actually moves
     * -- the same reasoning step 301 applied to the repaint above. */
    static uint32_t rendered_len = 0xFFFFFFFFu;
    static int      rendered_st  = -1;
    if (ln != rendered_len || st != rendered_st) {
        rendered_len = ln;
        rendered_st  = st;
        html_render(webfetch_body(), ln);
        g_sel = -1;
    }

    uint32_t seq = g_dirty;
    if (seq == g_drawn) { return; }
    g_drawn = seq;
    draw_all();
}

void browser_touch(uint32_t x, uint32_t y, int down)
{
    if (!down) { g_was_down = 0; return; }
    if (g_was_down) { return; }
    g_was_down = 1;

    if (g_editing) {
        int r = keyboard_touch(x, y);
        if (r == KB_EDIT) { g_dirty++; return; }
        if (r != KB_SUBMIT) { return; }
        const char *t = keyboard_text();
        if (t[0]) {
            /* "host" or "host/path" -- a typed URL that carries a path should
             * reach it, now that there is somewhere to keep one. */
            uint32_t i = 0u, h = 0u;
            while (t[i] && t[i] != '/' && h + 1u < WEB_HOST_MAX) { g_host[h++] = t[i++]; }
            g_host[h] = 0;
            uint32_t p = 0u;
            if (!t[i]) { g_path[p++] = '/'; }
            while (t[i] && p + 1u < WEB_HOST_MAX) { g_path[p++] = t[i++]; }
            g_path[p] = 0;
        }
        g_editing    = 0;
        g_want_fetch = 1;       /* the net task performs it */
        g_full       = 1;       /* back to the page layout; clear once */
        g_dirty++;
        return;
    }

    /* [step 390] The header toggles the raw response. The exit is at the right
     * and keeps its 22 pixels; the rest of the bar is the toggle. "Show exactly
     * what came back" is a diagnostic this view has needed more than once, and
     * a rendered page must not be the only thing it can show. */
    if (y < HDR_H && x < DISP_W - 22u) {
        g_raw    = !g_raw;
        g_scroll = 0u;
        g_sel    = -1;
        g_dirty++;
        return;
    }

    if (y >= URL_Y && y < URL_Y + URL_H) {
        if (x >= DISP_W - 48u) {                /* ed */
            g_editing = 1;
            keyboard_reset("go");
            g_full  = 1;        /* the layout changes; clear once */
            g_dirty++;
        } else if (x >= DISP_W - 96u) {         /* go */
            BLOG("go tapped");
            g_want_fetch = 1;
            g_dirty++;
        }
        return;
    }

    /* [step 390] A tap in the text is either a LINK or a scroll.
     *
     * Links first, and only on an exact hit: the row the finger landed on, the
     * column it landed in, mapped back through g_row_a[] to an offset in the
     * rendered text. Everything else still scrolls, so the gesture this view
     * has always had keeps working where there is no link to take it.
     *
     * Two taps, not one. The first SELECTS -- the link turns yellow -- and the
     * second follows it. A resistive panel with default calibration puts the
     * reported point some way from the finger (touch.h), and a single tap that
     * navigates would send the reader somewhere they did not choose with no way
     * to see it coming. Selecting first makes the machine say what it thinks
     * was tapped before it acts on it. */
    if (y >= TXT_Y && y < BAR_Y) {
        if (!g_raw && g_rows_drawn) {
            uint32_t row = (y - TXT_Y) / LINE_H;
            if (row < g_rows_drawn && x >= 2u) {
                uint32_t col = (x - 2u) / CHAR_W;
                uint32_t off = g_row_a[row] + col;
                if (off < g_row_b[row]) {
                    int hit = html_link_at(off);
                    if (hit >= 0) {
                        if (hit == g_sel) {
                            const html_link_t *L = html_link((uint32_t)hit);
                            if (L && follow(L->href)) {
                                g_sel        = -1;
                                g_want_fetch = 1;
                            }
                        } else {
                            g_sel = hit;
                        }
                        g_dirty++;
                        return;
                    }
                }
            }
        }
        /* A tap that missed every link clears the selection as well as
         * scrolling -- a highlight left behind after the reader moved on is a
         * target they did not choose sitting armed. */
        g_sel = -1;
        if (y > TXT_Y + TXT_H / 2u) {
            uint32_t cap = max_scroll();
            g_scroll = (g_scroll + ROWS / 2u > cap) ? cap : g_scroll + ROWS / 2u;
        } else if (g_scroll >= ROWS / 2u) {
            g_scroll -= ROWS / 2u;
        } else {
            g_scroll = 0u;
        }
        g_dirty++;
    }
}
