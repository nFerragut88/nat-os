/* nat-os — the video view. See vidlist.h. The file format is specified in
 * tools/vidconv.py's docstring; the offsets below are read from it. */

#include "vidlist.h"
#include "fat.h"
#include "mp3.h"            /* mp3_error_text(): the same words for card errors */
#include "display.h"
#include "uart.h"

#define SRAM1 __attribute__((section(".sram1")))

/* ---- .nvd header offsets (vidconv.py: HEADER, ICON) ------------------------- */
#define NV_W         8u
#define NV_H         10u
#define NV_FPS_NUM   12u
#define NV_FPS_DEN   14u
#define NV_PIX       16u
#define NV_FRAMES    20u
#define NV_COVER_OFF 48u
#define NV_COVER_W   52u
#define NV_COVER_H   54u
#define NV_DUR_MS    56u
#define NV_TITLE     64u
#define NV_TITLE_MAX 128u
#define NV_ICON_OFF  192u
#define NV_ICON_W    196u
#define NV_ICON_H    198u
#define NV_VERSION_1 1u

/* ---- geometry --------------------------------------------------------------- */
#define HDR_H     22u
#define LIST_Y    24u
#define ROW_H     40u
#define ROWS      6u
#define LIST_END  (LIST_Y + ROWS * ROW_H)       /* 264 */
#define BAR_Y     (LIST_END + 2u)               /* scroll bar, to SPEC_Y (288) */
#define BAR_H     (SPEC_Y - BAR_Y)
#define ICON_X    2u
#define TEXT_X    70u
#define TEXT_CH   28u                           /* (240 - 70) / 6, less a margin */

#define BG  COLOR_BLACK
#define FG  COLOR_WHITE

/* ---- the list ----------------------------------------------------------------- */
#define VL_MAX    16u
#define VL_TITLE  (2u * TEXT_CH + 1u)           /* two lines' worth */

typedef struct {
    char     name[FAT_NAME_MAX];                /* to open it again */
    char     title[VL_TITLE];
    uint32_t dur_ms, frames;
    uint16_t w, h, fps_num, fps_den;
    uint8_t  pix;
    uint32_t icon_off, cover_off;
    uint16_t icon_w, icon_h, cover_w, cover_h;
} vid_t;

SRAM1 static vid_t g_vids[VL_MAX];
static uint32_t g_count;
static int      g_listed;                       /* 0 not yet, 1 done, <0 error */
static uint32_t g_skipped;                      /* .nvd files that did not parse */

static int      g_sel = -1;
static uint32_t g_top;
static int      g_detail = -1;                  /* video whose detail is shown */

static volatile uint32_t g_dirty;
static uint32_t g_drawn;
static int      g_full = 1;
static int      g_was_down;

/* One sector's worth, word-aligned: display_blit() takes uint16_t pixels and
 * the file stores them little-endian, which is what this CPU reads. */
static uint16_t g_px[256];

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static int ends_nvd(const char *n)
{
    uint32_t len = 0;
    while (n[len]) { len++; }
    if (len < 4u) {
        return 0;
    }
    const char *e = n + len - 4u;
    return e[0] == '.' && (e[1] | 0x20) == 'n' && (e[2] | 0x20) == 'v' && (e[3] | 0x20) == 'd';
}

/* Reads a file's header sector into g_px and fills in `v`. 0 on success. A
 * file that is not a version-1 .nvd is skipped and counted, not shown half
 * broken. */
static int parse(vid_t *v)
{
    static fat_file_t f;
    if (fat_open(&f, v->name) != FAT_OK) {
        return -1;
    }
    uint8_t *h = (uint8_t *)g_px;
    if (fat_read(&f, h, 512u) != 512) {
        return -1;
    }
    if (h[0] != 'N' || h[1] != 'V' || h[2] != 'I' || h[3] != 'D' || rd16(h + 4) != NV_VERSION_1) {
        return -1;
    }
    v->w = rd16(h + NV_W);
    v->h = rd16(h + NV_H);
    v->fps_num = rd16(h + NV_FPS_NUM);
    v->fps_den = rd16(h + NV_FPS_DEN);
    v->pix = h[NV_PIX];
    v->frames = rd32(h + NV_FRAMES);
    v->dur_ms = rd32(h + NV_DUR_MS);
    v->cover_off = rd32(h + NV_COVER_OFF);
    v->cover_w = rd16(h + NV_COVER_W);
    v->cover_h = rd16(h + NV_COVER_H);
    v->icon_off = rd32(h + NV_ICON_OFF);
    v->icon_w = rd16(h + NV_ICON_W);
    v->icon_h = rd16(h + NV_ICON_H);
    uint32_t i = 0;
    for (; i < VL_TITLE - 1u && i < NV_TITLE_MAX && h[NV_TITLE + i]; i++) {
        char c = (char)h[NV_TITLE + i];
        v->title[i] = (c >= 32 && c < 127) ? c : '?';
    }
    v->title[i] = 0;
    /* Pictures wider than the space they are drawn in are not drawn: a bad
     * header must not become a blit off the edge of the panel. */
    if (v->icon_w > 64u || v->icon_h > 36u) { v->icon_off = 0; }
    if (v->cover_w > DISP_W || v->cover_h > 120u) { v->cover_off = 0; }
    return 0;
}

static void build_list(void)
{
    g_count = 0;
    g_skipped = 0;
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
    while (g_count < VL_MAX && (rc = fat_dir_next(&d, &e)) == 1) {
        if ((e.attr & FAT_ATTR_DIR) || !ends_nvd(e.name)) {
            continue;
        }
        vid_t *v = &g_vids[g_count];
        uint32_t i = 0;
        for (; e.name[i] && i < FAT_NAME_MAX - 1u; i++) {
            v->name[i] = e.name[i];
        }
        v->name[i] = 0;
        if (e.truncated && i > 0u) {
            v->name[i - 1u] = '*';              /* fat.c matches it as a prefix */
        }
        if (parse(v) == 0) {
            g_count++;
        } else {
            g_skipped++;
        }
    }
    g_listed = (rc < 0) ? rc : 1;
}

/* Streams an RGB565 picture from file offset `off` to the panel at (x, y),
 * a sector at a time: never more than 512 bytes of it in RAM. */
static void draw_picture(const char *name, uint32_t off, uint32_t x, uint32_t y,
                         uint32_t w, uint32_t h)
{
    static fat_file_t f;
    if (!off || !w || !h || fat_open(&f, name) != FAT_OK || fat_seek(&f, off) != FAT_OK) {
        display_fill_rect(x, y, w ? w : 64u, h ? h : 36u, COLOR_GREY);
        return;
    }
    uint32_t rows_per = sizeof g_px / 2u / w;           /* rows per 512 bytes */
    if (rows_per == 0u) {
        rows_per = 1u;
    }
    for (uint32_t r = 0; r < h; r += rows_per) {
        uint32_t n = (h - r < rows_per) ? h - r : rows_per;
        if (fat_read(&f, g_px, n * w * 2u) != (int32_t)(n * w * 2u)) {
            display_fill_rect(x, y + r, w, h - r, COLOR_GREY);
            return;
        }
        display_blit(x, y + r, w, n, g_px, w);
    }
}

/* ---- drawing ----------------------------------------------------------------- */

static void put(uint32_t x, uint32_t y, const char *s, uint16_t fg, uint16_t bg)
{
    display_text(x, y, s, fg, bg, 1u);
}

static void fmt_time(char *b, uint32_t s)
{
    uint32_t m = s / 60u, k = 0;
    if (m >= 10u) { b[k++] = (char)('0' + (m / 10u) % 10u); }
    b[k++] = (char)('0' + m % 10u);
    b[k++] = ':';
    b[k++] = (char)('0' + (s % 60u) / 10u);
    b[k++] = (char)('0' + s % 10u);
    b[k] = 0;
}

/* Up to `lines` lines of `cols` characters from `s`, broken at a space where
 * one is near the end of a line. */
static void put_wrapped(uint32_t x, uint32_t y, const char *s, uint32_t cols,
                        uint32_t lines, uint16_t fg, uint16_t bg)
{
    char line[48];
    for (uint32_t l = 0; l < lines && *s; l++) {
        uint32_t n = 0;
        while (s[n] && n < cols) { n++; }
        if (s[n] && n == cols) {
            uint32_t sp = n;
            while (sp > cols / 2u && s[sp] != ' ') { sp--; }
            if (s[sp] == ' ') { n = sp; }
        }
        for (uint32_t i = 0; i < n && i < sizeof line - 1u; i++) { line[i] = s[i]; }
        line[n < sizeof line ? n : sizeof line - 1u] = 0;
        put(x, y + l * 10u, line, fg, bg);
        s += n;
        while (*s == ' ') { s++; }
    }
}

static void draw_header(const char *label)
{
    display_fill_rect(0, 0, DISP_W, HDR_H, COLOR_BLUE);
    put(6u, 8u, label, FG, COLOR_BLUE);
    /* The way out, as every view draws it: desktop_chrome_touch() owns the
     * top-right 22x22 and sees the press first. */
    display_fill_rect(DISP_W - 22u, 0, 22u, HDR_H, COLOR_RED);
    put(DISP_W - 14u, 8u, "x", FG, COLOR_RED);
}

static void draw_list(void)
{
    display_fill_rect(0, LIST_Y, DISP_W, LIST_END - LIST_Y, BG);
    if (g_listed < 0) {
        put(6u, LIST_Y + 6u, "can't read the card:", COLOR_RED, BG);
        put(6u, LIST_Y + 18u, mp3_error_text(g_listed), COLOR_RED, BG);
        return;
    }
    if (g_count == 0u) {
        put(6u, LIST_Y + 6u, "no .nvd videos on the card", FG, BG);
        put(6u, LIST_Y + 18u, "make one on the PC with", COLOR_GREY, BG);
        put(6u, LIST_Y + 30u, "tools/vidconv.py", COLOR_GREY, BG);
        return;
    }
    for (uint32_t r = 0; r < ROWS && g_top + r < g_count; r++) {
        uint32_t i = g_top + r;
        const vid_t *v = &g_vids[i];
        uint32_t y = LIST_Y + r * ROW_H;
        uint16_t bg = ((int)i == g_sel) ? COLOR_BLUE : BG;
        if (bg != BG) {
            display_fill_rect(0, y, DISP_W, ROW_H - 1u, bg);
        }
        draw_picture(v->name, v->icon_off, ICON_X, y + 2u, v->icon_w, v->icon_h);
        put_wrapped(TEXT_X, y + 4u, v->title, TEXT_CH, 2u, FG, bg);
        char t[8];
        fmt_time(t, v->dur_ms / 1000u);
        put(TEXT_X, y + 27u, t, COLOR_GREY, bg);
    }
}

static void draw_bar(void)
{
    int up = g_top > 0u;
    int dn = g_top + ROWS < g_count;
    display_fill_rect(0, BAR_Y, DISP_W, BAR_H, BG);
    display_fill_rect(2u, BAR_Y, 76u, BAR_H, up ? COLOR_GREY : BG);
    display_fill_rect(DISP_W - 78u, BAR_Y, 76u, BAR_H, dn ? COLOR_GREY : BG);
    put(36u, BAR_Y + 7u, "^", up ? FG : COLOR_GREY, up ? COLOR_GREY : BG);
    put(DISP_W - 42u, BAR_Y + 7u, "v", dn ? FG : COLOR_GREY, dn ? COLOR_GREY : BG);
    char c[16];
    uint32_t k = 0, n = g_count;
    if (n >= 10u) { c[k++] = (char)('0' + n / 10u); }
    c[k++] = (char)('0' + n % 10u);
    const char *w = (n == 1u) ? " video" : " videos";
    while (*w) { c[k++] = *w++; }
    c[k] = 0;
    put(92u, BAR_Y + 7u, c, COLOR_GREY, BG);
}

static void draw_detail(const vid_t *v)
{
    display_fill_rect(0, HDR_H, DISP_W, SPEC_Y - HDR_H, BG);
    uint32_t cw = v->cover_off ? v->cover_w : 64u;
    draw_picture(v->name, v->cover_off ? v->cover_off : v->icon_off,
                 (DISP_W - cw) / 2u, HDR_H + 12u,
                 v->cover_off ? v->cover_w : v->icon_w,
                 v->cover_off ? v->cover_h : v->icon_h);
    uint32_t y = HDR_H + 12u + (v->cover_off ? v->cover_h : v->icon_h) + 12u;
    put_wrapped(6u, y, v->title, 38u, 4u, FG, BG);
    y += 46u;
    char t[8];
    fmt_time(t, v->dur_ms / 1000u);
    put(6u, y, t, COLOR_GREY, BG);
    char s[40];
    uint32_t k = 0;
    const char *p;
    #define PUTN(n) do { uint32_t _v = (n), _d = 1; while (_v / _d >= 10u) _d *= 10u; \
                         while (_d) { s[k++] = (char)('0' + _v / _d % 10u); _d /= 10u; } } while (0)
    PUTN(v->w); s[k++] = 'x'; PUTN(v->h); s[k++] = ' '; s[k++] = ' ';
    PUTN(v->fps_den ? v->fps_num / v->fps_den : 0u);
    for (p = " fps  "; *p; p++) { s[k++] = *p; }
    for (p = (v->pix == 1u) ? "256 colours" : "rgb565"; *p; p++) { s[k++] = *p; }
    s[k] = 0;
    #undef PUTN
    put(48u, y, s, COLOR_GREY, BG);
    put(6u, y + 22u, "playback is the next step --", COLOR_YELLOW, BG);
    put(6u, y + 32u, "this is the browser.", COLOR_YELLOW, BG);
    put(6u, SPEC_Y - 14u, "tap to go back", COLOR_GREY, BG);
}

/* ---- the view ------------------------------------------------------------------ */

void vidlist_open(void)
{
    /* ZERO: the launcher opens on RELEASE (wifiapp_open, browser_open). */
    g_was_down = 0;
    g_detail = -1;
    g_listed = 0;               /* re-read: files come and go with the card */
    g_full = 1;
    g_dirty++;
}

void vidlist_frame(void)
{
    uint32_t seq = g_dirty;
    if (!g_full && seq == g_drawn) {
        return;
    }
    if (g_listed == 0) {
        build_list();
    }
    if (g_detail >= 0) {
        draw_header("video");
        draw_detail(&g_vids[g_detail]);
    } else {
        if (g_full) {
            display_fill_rect(0, 0, DISP_W, SPEC_Y, BG);
        }
        draw_header("video");
        draw_list();
        draw_bar();
    }
    g_drawn = seq;
    g_full = 0;
}

void vidlist_touch(uint32_t x, uint32_t y, int down)
{
    if (!down) {
        g_was_down = 0;
        return;
    }
    if (g_was_down) {
        return;
    }
    g_was_down = 1;

    if (g_detail >= 0) {
        g_detail = -1;                          /* any tap: back to the list */
        g_full = 1;
        g_dirty++;
        return;
    }
    if (y >= LIST_Y && y < LIST_END) {
        uint32_t i = g_top + (y - LIST_Y) / ROW_H;
        if (i < g_count) {
            if ((int)i == g_sel) {
                g_detail = (int)i;              /* second tap: open it */
            } else {
                g_sel = (int)i;                 /* first tap: select it */
            }
            g_dirty++;
        }
        return;
    }
    if (y >= BAR_Y) {
        if (x < DISP_W / 3u && g_top > 0u) {
            g_top = (g_top >= ROWS) ? g_top - ROWS : 0u;
            g_dirty++;
        } else if (x >= DISP_W - DISP_W / 3u && g_top + ROWS < g_count) {
            g_top += ROWS;
            g_dirty++;
        }
    }
}

void vidlist_dump(void)
{
    if (g_listed == 0) {
        build_list();
    }
    uart_puts("   video view: listed=");
    uart_put_dec((uint32_t)(g_listed > 0 ? g_listed : 0));
    if (g_listed < 0) {
        uart_puts(" (");
        uart_puts(mp3_error_text(g_listed));
        uart_puts(")");
    }
    uart_puts(" videos=");
    uart_put_dec(g_count);
    uart_puts(" skipped=");
    uart_put_dec(g_skipped);
    uart_puts(" sel=");
    uart_put_dec((uint32_t)(g_sel + 1));
    uart_puts(" detail=");
    uart_put_dec((uint32_t)(g_detail + 1));
    uart_puts("\n");
    for (uint32_t i = 0; i < g_count; i++) {
        const vid_t *v = &g_vids[i];
        uart_puts("     ");
        uart_put_dec(i + 1u);
        uart_puts("  ");
        uart_puts(v->name);
        uart_puts("\n        '");
        uart_puts(v->title);
        uart_puts("'  ");
        uart_put_dec(v->w);
        uart_puts("x");
        uart_put_dec(v->h);
        uart_puts("  ");
        uart_put_dec(v->frames);
        uart_puts(" frames  ");
        uart_put_dec(v->dur_ms);
        uart_puts(" ms  icon ");
        uart_put_dec(v->icon_w);
        uart_puts("x");
        uart_put_dec(v->icon_h);
        uart_puts("@");
        uart_put_dec(v->icon_off);
        uart_puts("  cover ");
        uart_put_dec(v->cover_w);
        uart_puts("x");
        uart_put_dec(v->cover_h);
        uart_puts("@");
        uart_put_dec(v->cover_off);
        uart_puts("\n");
    }
}
