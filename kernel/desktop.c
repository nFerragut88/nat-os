/* cyd-os — touch-driven launcher. See desktop.h for what this is and is not. */

#include "desktop.h"
#include "display.h"
#include "raycast.h"
#include "shell.h"
#include "app.h"
#include "timer.h"

/* 3 x 3 over a 240 x 168 region. The cell is what constrains the label length,
 * not the other way round: at 6 px per character a 78 px cell holds 13, so the
 * labels below are kept to that rather than being clipped at draw time into
 * something unreadable. */
#define COLS 3u
#define ROWS 3u
#define CELL_W (DISP_W / COLS)          /* 80 */
#define CELL_H (DESK_H / ROWS)          /* 56 */

#define ICON_W 44u
#define ICON_H 26u

/* Two taps inside this window, on the same cell, open it. 60 ticks is ~600 ms
 * at the 10 ms tick — slow enough for a deliberate double-tap on a panel that
 * needs a firm press, short enough that two unrelated taps on one icon do not
 * merge into an open. */
#define DOUBLE_TAP_TICKS 60u

/* A press must last at least one sample and release cleanly to count. Resistive
 * panels chatter on contact and release; without this, one press delivers
 * several taps and every single tap reads as a double. */
#define TAP_MIN_TICKS 2u

static const desk_icon_t ICONS[COLS * ROWS] = {
    { "counter",  "counter",  COLOR_CYAN,    DESK_ACTION_NONE },
    { "squares",  "squares",  COLOR_GREEN,   DESK_ACTION_NONE },
    { "draw",     "draw",     COLOR_YELLOW,  DESK_ACTION_NONE },
    { "paint",    "paint",    COLOR_MAGENTA, DESK_ACTION_NONE },
    { "blit",     "blit",     COLOR_WHITE,   DESK_ACTION_NONE },
    { "ping",     "ping",     COLOR_CYAN,    DESK_ACTION_NONE },
    { "pong",     "pong",     COLOR_GREEN,   DESK_ACTION_NONE },
    { "rogue",    "gfxrogue", COLOR_YELLOW,  DESK_ACTION_NONE },
    { "3D view",  0,          COLOR_RED,     DESK_ACTION_3D   },
};

static int      g_active = 1;
static int      g_sel = 0;              /* cell under the cursor */
static uint32_t g_cx, g_cy;             /* cursor, in region coordinates */
static int      g_dirty = 1;

static int      g_was_down;
static uint32_t g_press_tick;
static uint32_t g_last_tap_tick;
static int      g_last_tap_sel = -1;

static uint32_t g_taps, g_opens;

/* Set when a launch happens, so the next frame can say what it did. Kept as an
 * index rather than a pointer so nothing here outlives the table. */
static int      g_msg_sel = -1;
static int      g_msg_ok;

int      desktop_active(void) { return g_active; }
uint32_t desktop_taps(void)   { return g_taps; }
uint32_t desktop_opens(void)  { return g_opens; }

void desktop_init(void)
{
    g_cx = CELL_W / 2u;
    g_cy = CELL_H / 2u;
    g_dirty = 1;
}

static uint32_t cell_x(int i) { return ((uint32_t)i % COLS) * CELL_W; }
static uint32_t cell_y(int i) { return ((uint32_t)i / COLS) * CELL_H; }

/* Which cell contains a point. Returns -1 outside the grid, so a touch in the
 * application strips below cannot select anything up here. */
static int cell_at(uint32_t x, uint32_t y)
{
    if (y >= DESK_H || x >= DISP_W) {
        return -1;
    }
    uint32_t c = x / CELL_W;
    uint32_t r = y / CELL_H;
    if (c >= COLS || r >= ROWS) {
        return -1;
    }
    return (int)(r * COLS + c);
}

/* An arrow, drawn as a stack of horizontal runs. Deliberately not a bitmap:
 * there is still no asset pipeline (UM-CYDOS-011 §6), and eight fill_rect calls
 * cost less than inventing one for a single glyph. */
static void draw_cursor(uint32_t x, uint32_t y)
{
    for (uint32_t i = 0; i < 8u; i++) {
        uint32_t w = 8u - i;
        if (x + w > DISP_W) {
            w = DISP_W - x;
        }
        if (y + i >= DESK_H) {
            break;
        }
        /* Black underlay one pixel wider gives the arrow an outline, so it stays
         * visible over both the dark background and a bright icon. */
        display_fill_rect(x, y + i, w + 1u, 1u, COLOR_BLACK);
        display_fill_rect(x, y + i, w, 1u, COLOR_WHITE);
    }
}

static void draw_icon(int i)
{
    uint32_t x = cell_x(i);
    uint32_t y = cell_y(i);
    const desk_icon_t *ic = &ICONS[i];

    uint32_t ix = x + (CELL_W - ICON_W) / 2u;
    uint32_t iy = y + 6u;

    /* Selected cell gets a filled backing rather than a border: a one-pixel
     * outline on this panel is legible only if you already know it is there. */
    if (i == g_sel) {
        display_fill_rect(x + 1u, y + 1u, CELL_W - 2u, CELL_H - 2u, COLOR_GREY);
    }

    display_fill_rect(ix, iy, ICON_W, ICON_H, ic->colour);

    /* Running programs are marked, so the launcher shows state rather than just
     * offering actions. Without it, double-tapping a program already running
     * looks like nothing happened. */
    int running = 0;
    if (ic->prog) {
        for (int id = 0; id < APP_MAX; id++) {
            if (app_state(id) == APP_RUNNING && app_name(id) &&
                app_name(id)[0] == ic->prog[0]) {
                running = 1;
                break;
            }
        }
    }
    if (running) {
        display_fill_rect(ix + ICON_W - 6u, iy + 2u, 4u, 4u, COLOR_BLACK);
    }

    uint32_t lx = x + 4u;
    display_text(lx, y + ICON_H + 12u, ic->label, COLOR_WHITE,
                 (i == g_sel) ? COLOR_GREY : COLOR_BLACK, 1u);
}

void desktop_frame(void)
{
    if (!g_active || !g_dirty) {
        return;                 /* an idle launcher costs no SPI at all */
    }
    g_dirty = 0;

    display_lock();

    display_fill_rect(0, 0, DISP_W, DESK_H, COLOR_BLACK);
    for (int i = 0; i < (int)(COLS * ROWS); i++) {
        draw_icon(i);
    }

    if (g_msg_sel >= 0) {
        display_text(4, DESK_H - 10u,
                     g_msg_ok ? "started" : "no free slot",
                     g_msg_ok ? COLOR_GREEN : COLOR_RED, COLOR_BLACK, 1u);
    }

    draw_cursor(g_cx, g_cy);

    display_unlock();
}

static void open_selected(void)
{
    const desk_icon_t *ic = &ICONS[g_sel];
    g_opens++;
    g_msg_sel = g_sel;

    if (ic->action == DESK_ACTION_3D) {
        /* Hand the region over. The raycaster repaints every frame, so nothing
         * needs erasing first. */
        g_active = 0;
        g_msg_ok = 1;
        return;
    }

    g_msg_ok = (shell_launch(ic->prog) >= 0);
    g_dirty  = 1;
}

void desktop_touch(uint32_t x, uint32_t y, int down)
{
    uint32_t now = timer_ticks();

    if (!g_active) {
        /* The way back from the 3D view: a press in the top-left corner. A
         * corner rather than a gesture because the raycaster steers from the
         * left and right thirds of the same region, and anything subtler would
         * be indistinguishable from turning. */
        if (down && x < 36u && y < 18u) {
            g_active = 1;
            g_dirty  = 1;
        }
        return;
    }

    if (down) {
        if (!g_was_down) {
            g_press_tick = now;
        }
        g_was_down = 1;

        /* Track the cursor while the finger is down, so a drag moves it. */
        int c = cell_at(x, y);
        if (c >= 0) {
            g_cx = (x < DISP_W - 8u) ? x : DISP_W - 8u;
            g_cy = (y < DESK_H - 8u) ? y : DESK_H - 8u;
            if (c != g_sel) {
                g_sel = c;
            }
            g_dirty = 1;
        }
        return;
    }

    /* Release. */
    if (!g_was_down) {
        return;
    }
    g_was_down = 0;

    if ((now - g_press_tick) < TAP_MIN_TICKS) {
        return;                 /* contact chatter, not a press */
    }

    g_taps++;

    if (g_last_tap_sel == g_sel && (now - g_last_tap_tick) < DOUBLE_TAP_TICKS) {
        open_selected();
        g_last_tap_sel  = -1;   /* a third tap must not open again */
        g_last_tap_tick = 0;
        return;
    }

    g_last_tap_sel  = g_sel;
    g_last_tap_tick = now;
}
