/* nat-os — touch-driven launcher. See desktop.h for what this is and is not. */

#include "desktop.h"
#include "display.h"
#include "raycast.h"
#include "notes.h"
#include "shell.h"
#include "app.h"
#include "timer.h"

/* The screen layout is agreed across four files — desktop.h sizes the launcher
 * region, raycast.h sizes the 3D view, app.h places the application strips, and
 * kmain.c places the colour strip. Nothing enforced that agreement, and a
 * session spent reshuffling it produced a screen that looked broken in ways no
 * test could see: every self-test passed throughout.
 *
 * These do not check that the layout is GOOD. They check that its pieces do not
 * overlap or fall off the panel, which is the part a compiler can know. */
_Static_assert(RAY_VIEW_H <= DESK_H,
               "the 3D view must fit inside the launcher region");
_Static_assert(APP_VIEW_Y0 >= DESK_H,
               "application strips must start at or below the launcher region");
_Static_assert(APP_VIEW_Y0 + APP_MAX * APP_VIEW_PITCH <= DISP_H,
               "application strips must fit on the panel");
_Static_assert(APP_CHROME_W < DISP_W,
               "the kernel column must leave an application something to draw in");

/* 3 x 3 over a 240 x 168 region. The cell is what constrains the label length,
 * not the other way round: at 6 px per character a 78 px cell holds 13, so the
 * labels below are kept to that rather than being clipped at draw time into
 * something unreadable. */
#define COLS 3u
#define ROWS 3u

/* The bottom of the region is a status strip, not grid.
 *
 * It used to be neither: the launch message was drawn at DESK_H-10, which is
 * inside the bottom-left cell's label. So "started" appeared over pong's name
 * and stayed there permanently, and the obvious reading — that pong was what
 * had been selected — was wrong but entirely reasonable. Text that overlaps a
 * control is not a cosmetic problem; it makes the interface lie about its own
 * state. */
#define STATUS_H 14u
#define GRID_H  (DESK_H - STATUS_H)

#define CELL_W (DISP_W / COLS)          /* 80 */
#define CELL_H (GRID_H / ROWS)          /* 51 */

/* How long a launch message stays up. It expires rather than persisting,
 * because a status line that never clears stops describing the present and
 * becomes decoration. */
#define MSG_TICKS 200u                  /* ~2 s */

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

/* ---- icons ---------------------------------------------------------------
 *
 * 8x8 monochrome glyphs, one byte per row, MSB leftmost, drawn at 3x.
 *
 * A bitmap rather than a routine per icon: nine bespoke drawing functions is
 * more code than nine constants and makes every icon a place a bug can hide.
 * A bitmap is also editable by anyone who can count to eight, which matters
 * more than elegance for something whose only requirement is being recognisable
 * at 24 pixels.
 *
 * Still not an asset pipeline (UM-NATOS-011 §6). These are in the image because
 * there is nowhere else to put them yet. */
#define GLYPH_PX 3u                     /* scale: 8x8 -> 24x24 */

static const uint8_t GLYPHS[COLS * ROWS][8] = {
    /* counter — stacked bars, tallest last */
    { 0x00, 0x08, 0x18, 0x38, 0x78, 0xF8, 0xF8, 0x00 },
    /* squares — a grid */
    { 0x00, 0x66, 0x66, 0x00, 0x00, 0x66, 0x66, 0x00 },
    /* draw — a pencil on the diagonal */
    { 0x03, 0x07, 0x0E, 0x1C, 0x38, 0x70, 0xE0, 0xC0 },
    /* paint — a brush with a broad head */
    { 0x18, 0x18, 0x18, 0x3C, 0x7E, 0x7E, 0x3C, 0x18 },
    /* notes — a page with ruled lines */
    { 0x7E, 0x42, 0x7A, 0x42, 0x7A, 0x42, 0x7E, 0x00 },
    /* ping — a ball travelling right */
    { 0x00, 0x00, 0x30, 0x78, 0x78, 0x30, 0x0C, 0x06 },
    /* pong — paddle and ball */
    { 0xC0, 0xC0, 0xC6, 0xCF, 0xCF, 0xC6, 0xC0, 0xC0 },
    /* rogue — a wall */
    { 0xFF, 0x91, 0x91, 0xFF, 0x19, 0x19, 0xFF, 0x00 },
    /* 3D view — an isometric cube */
    { 0x18, 0x3C, 0x7E, 0xFF, 0xFF, 0x7E, 0x3C, 0x18 },
};

/* One draw call per run of set pixels, not one per pixel. A row of eight
 * becomes a single 24-pixel rectangle instead of eight 3-pixel ones, which
 * matters because every one of them takes the draw lock. */
static void draw_glyph(const uint8_t *g, uint32_t x, uint32_t y, uint16_t fg)
{
    for (uint32_t row = 0; row < 8u; row++) {
        uint8_t bits = g[row];
        uint32_t col = 0;
        while (col < 8u) {
            if (!(bits & (0x80u >> col))) {
                col++;
                continue;
            }
            uint32_t run = 0;
            while (col + run < 8u && (bits & (0x80u >> (col + run)))) {
                run++;
            }
            display_fill_rect(x + col * GLYPH_PX, y + row * GLYPH_PX,
                              run * GLYPH_PX, GLYPH_PX, fg);
            col += run;
        }
    }
}

static const desk_icon_t ICONS[COLS * ROWS] = {
    { "counter",  "counter",  COLOR_CYAN,    DESK_ACTION_NONE },
    { "squares",  "squares",  COLOR_GREEN,   DESK_ACTION_NONE },
    { "draw",     "draw",     COLOR_YELLOW,  DESK_ACTION_NONE },
    { "paint",    "paint",    COLOR_MAGENTA, DESK_ACTION_NONE },
    { "notes",    0,          COLOR_WHITE,   DESK_ACTION_NOTES },
    { "ping",     "ping",     COLOR_CYAN,    DESK_ACTION_NONE },
    { "pong",     "pong",     COLOR_GREEN,   DESK_ACTION_NONE },
    { "rogue",    "gfxrogue", COLOR_YELLOW,  DESK_ACTION_NONE },
    { "3D view",  0,          COLOR_RED,     DESK_ACTION_3D   },
};

/* Which view owns the region. A mode rather than a set of booleans, so "who is
 * drawing" cannot have two answers at once. */
#define MODE_LAUNCHER 0
#define MODE_3D       1
#define MODE_NOTES    2
static int      g_mode = MODE_LAUNCHER;
#define g_active (g_mode == MODE_LAUNCHER)
static int      g_sel = 0;              /* cell under the cursor */
static uint32_t g_cx, g_cy;             /* cursor, in region coordinates */
static int      g_dirty = 1;

static int      g_was_down;
static uint32_t g_press_tick;
static uint32_t g_last_tap_tick;
static int      g_last_tap_sel = -1;

static uint32_t g_taps, g_opens, g_closes;

uint32_t desktop_closes(void) { return g_closes; }

/* Diagnostics for the selection itself.
 *
 * A tap that selects the wrong icon has two candidate causes that look
 * identical from the chair: the touch layer reporting the wrong place, or this
 * file sampling the right place at the wrong moment. Recording the FIRST and
 * LAST sample of each press separates them — if they disagree, the fault is
 * when we look, not where the finger was. */
static uint32_t g_first_x, g_first_y; static int g_first_cell = -1;
static uint32_t g_last_x,  g_last_y;  static int g_last_cell  = -1;
static uint32_t g_samples;

/* Set when a launch happens, so the next frame can say what it did. Kept as an
 * index rather than a pointer so nothing here outlives the table. */
static int      g_msg_sel = -1;
static int      g_msg_ok;
static uint32_t g_msg_tick;

int      desktop_active(void) { return g_mode == MODE_LAUNCHER; }
int      desktop_notes(void)  { return g_mode == MODE_NOTES; }

void desktop_set_active(int on)
{
    g_mode  = on ? MODE_LAUNCHER : MODE_3D;
    g_dirty  = 1;
}
uint32_t desktop_taps(void)   { return g_taps; }
uint32_t desktop_opens(void)  { return g_opens; }

void desktop_init(void)
{
    g_cx = CELL_W / 2u;
    g_cy = CELL_H / 2u;
    g_dirty = 1;
}

/* Full comparison, not a first-character test.
 *
 * This compared app_name(id)[0] against ic->prog[0], which is wrong on the
 * current table rather than merely fragile: paint, ping and pong all begin with
 * 'p', so running any one of them put the running-marker on all three. Found by
 * writing the documentation, not by looking at the screen — the marker is four
 * pixels and three of them being wrong looks like a rendering artefact. */
static int name_eq(const char *a, const char *b)
{
    if (!a || !b) {
        return 0;
    }
    while (*a && *b) {
        if (*a++ != *b++) {
            return 0;
        }
    }
    return *a == *b;
}

static uint32_t cell_x(int i) { return ((uint32_t)i % COLS) * CELL_W; }
static uint32_t cell_y(int i) { return ((uint32_t)i / COLS) * CELL_H; }

/* Which cell contains a point. Returns -1 outside the grid, so a touch in the
 * application strips below cannot select anything up here. */
static int cell_at(uint32_t x, uint32_t y)
{
    if (y >= GRID_H || x >= DISP_W) {
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
 * there is still no asset pipeline (UM-NATOS-011 §6), and eight fill_rect calls
 * cost less than inventing one for a single glyph. */
static void draw_cursor(uint32_t x, uint32_t y)
{
    for (uint32_t i = 0; i < 8u; i++) {
        uint32_t w = 8u - i;
        if (x + w > DISP_W) {
            w = DISP_W - x;
        }
        if (y + i >= GRID_H) {
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

    uint32_t gw = 8u * GLYPH_PX;
    uint32_t ix = x + (CELL_W - gw) / 2u;
    uint32_t iy = y + 5u;

    /* Selected cell gets a filled backing rather than a border: a one-pixel
     * outline on this panel is legible only if you already know it is there. */
    uint16_t bg = (i == g_sel) ? COLOR_GREY : COLOR_BLACK;
    if (i == g_sel) {
        display_fill_rect(x + 1u, y + 1u, CELL_W - 2u, CELL_H - 2u, COLOR_GREY);
    }

    draw_glyph(GLYPHS[i], ix, iy, ic->colour);

    /* Running programs are marked, so the launcher shows state rather than just
     * offering actions. Without it, double-tapping a program already running
     * looks like nothing happened. */
    int running = 0;
    if (ic->prog) {
        for (int id = 0; id < APP_MAX; id++) {
            if (app_state(id) == APP_RUNNING &&
                name_eq(app_name(id), ic->prog)) {
                running = 1;
                break;
            }
        }
    }
    /* A running program is marked by underlining its label rather than by a dot
     * in the corner of the icon. The dot was four pixels and read as a
     * rendering artefact; a full-width rule under the name cannot. */
    uint32_t lx = x + 4u;
    uint32_t ly = y + 5u + gw + 4u;
    display_text(lx, ly, ic->label, COLOR_WHITE, bg, 1u);
    if (running) {
        display_fill_rect(lx, ly + 9u, CELL_W - 8u, 1u, ic->colour);
    }
}

/* ---- close buttons -------------------------------------------------------
 *
 * One per running application, drawn in the column app.h reserves outside every
 * viewport, plus one for the 3D view.
 *
 * Drawn every frame rather than on change. The X sits outside the application's
 * viewport so the application cannot paint over it, but the cost of being wrong
 * about that is a user who cannot close a program, and repainting sixteen
 * pixels four times a frame is cheaper than being sure. */
#define CHROME_X (DISP_W - APP_CHROME_W)
#define CLOSE_X  (DISP_W - APP_CLOSE_W)

static void draw_close(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint16_t fg)
{
    display_fill_rect(x, y, w, h, COLOR_BLACK);

    /* A diagonal cross, one square per step, so it reads as an X rather than a
     * smear at this size. */
    uint32_t n = (w < h ? w : h) - 6u;
    for (uint32_t i = 0; i < n; i++) {
        display_fill_rect(x + 3u + i, y + 3u + i, 2u, 2u, fg);
        display_fill_rect(x + 3u + (n - 1u - i), y + 3u + i, 2u, 2u, fg);
    }
}

void desktop_chrome(void)
{
    for (int id = 0; id < APP_MAX; id++) {
        uint32_t y = APP_VIEW_Y0 + (uint32_t)id * APP_VIEW_PITCH;

        if (app_state(id) == APP_RUNNING) {
            /* Name, then the X. The name is what makes the strip legible: a
             * program that draws nothing is otherwise indistinguishable from an
             * empty slot that somehow grew a button. */
            display_fill_rect(CHROME_X, y, APP_NAME_W, APP_VIEW_H, COLOR_BLACK);
            display_text(CHROME_X + 1u, y + 9u, app_name(id),
                         COLOR_GREY, COLOR_BLACK, 1u);
            draw_close(CLOSE_X, y, APP_CLOSE_W, APP_VIEW_H, COLOR_RED);
        } else {
            /* Clear the whole column when the slot empties, or the name and X
             * outlive the program and offer to close something already gone. */
            display_fill_rect(CHROME_X, y, APP_CHROME_W, APP_VIEW_H, COLOR_BLACK);
        }
    }

    /* The 3D view's close button is normally NOT drawn here — see
     * desktop_overlay_into(). Drawing it over a view that repaints every pixel
     * every frame makes it visible only in the gap between one repaint and the
     * next, which strobes. It is stamped into that view's framebuffer instead,
     * so it goes out as part of the same blit.
     *
     * The exception is the framebuffer being off, where there is no buffer to
     * stamp into: that path blits column by column straight to the panel. The
     * button is drawn here in that case and DOES flicker. An invisible control
     * is worse than a flickering one — the way out of the view would exist and
     * be unfindable — and `fb off` is a diagnostic mode, not how it runs.
     *
     * The application close buttons above are unaffected either way: they sit in
     * the strips, which no full-region view touches. */
    /* Who draws the close button depends on who owns the region:
     *
     *   3D view, framebuffer on   the raycaster stamps it into the buffer, so
     *                             it and the frame arrive in one transfer
     *   3D view, framebuffer off  drawn here, and it flickers; fb off is a
     *                             diagnostic mode
     *   note pad                  the app draws it in its own header
     *
     * The middle case is the only one this branch is for. It used to be the
     * only case considered, which is why the note pad's button was invisible:
     * present, hit-testable, and drawn by nobody. */
    if (g_mode == MODE_3D && !raycast_framebuffer()) {
        draw_close(DISP_W - 20u, 2u, 18u, 18u, COLOR_WHITE);
    }
}

/* Stamps the close button into a view's framebuffer, in the buffer's own
 * coordinates. Pure: it writes pixels and calls nothing.
 *
 * Called by whichever view owns the region, immediately before it blits, so the
 * button and the frame beneath it reach the panel in one transfer. That is the
 * whole fix for the flicker — not ordering the draws differently, but making
 * them the same draw. */
void desktop_overlay_into(uint16_t *fb, uint32_t w, uint32_t h)
{
    if (g_active || !fb) {
        return;                 /* the launcher draws its own chrome normally */
    }

    const uint32_t bw = 18u, bh = 18u;
    if (w < bw + 2u || h < bh + 2u) {
        return;
    }
    const uint32_t bx = w - bw - 2u;     /* matches the hit test in
                                          * desktop_chrome_touch() */
    const uint32_t by = 2u;

    for (uint32_t row = 0; row < bh; row++) {
        uint16_t *line = fb + (by + row) * w + bx;
        for (uint32_t col = 0; col < bw; col++) {
            line[col] = COLOR_BLACK;
        }
    }

    /* Same diagonal cross as draw_close(), one 2x2 square per step. */
    uint32_t n = bw - 6u;
    for (uint32_t i = 0; i < n; i++) {
        for (uint32_t dy = 0; dy < 2u; dy++) {
            uint16_t *line = fb + (by + 3u + i + dy) * w + bx;
            line[3u + i]           = COLOR_WHITE;
            line[3u + i + 1u]      = COLOR_WHITE;
            line[3u + (n - 1u - i)]      = COLOR_WHITE;
            line[3u + (n - 1u - i) + 1u] = COLOR_WHITE;
        }
    }
}

/* Non-zero if the touch was consumed by a close button. */
int desktop_chrome_touch(uint32_t x, uint32_t y)
{
    if (!g_active && x >= DISP_W - 22u && y < 22u) {
        g_mode  = MODE_LAUNCHER;    /* leave whichever view is open */
        g_dirty  = 1;
        return 1;
    }

    if (x < CLOSE_X) {
        return 0;               /* the name is a label, not a button */
    }
    for (int id = 0; id < APP_MAX; id++) {
        uint32_t top = APP_VIEW_Y0 + (uint32_t)id * APP_VIEW_PITCH;
        if (y >= top && y < top + APP_VIEW_H && app_state(id) == APP_RUNNING) {
            app_kill(id);
            g_closes++;
            g_dirty = 1;            /* the launcher's running marks changed */
            return 1;
        }
    }
    return 0;
}

void desktop_frame(void)
{
    if (!g_active) {
        return;
    }

    /* Expire the launch message. Checked before the dirty gate, because
     * expiry is itself a reason to repaint. */
    if (g_msg_sel >= 0 && (timer_ticks() - g_msg_tick) > MSG_TICKS) {
        g_msg_sel = -1;
        g_dirty   = 1;
    }

    if (!g_dirty) {
        return;                 /* an idle launcher costs no SPI at all */
    }
    g_dirty = 0;

    display_lock();

    display_fill_rect(0, 0, DISP_W, DESK_H, COLOR_BLACK);
    for (int i = 0; i < (int)(COLS * ROWS); i++) {
        draw_icon(i);
    }

    /* Status strip, below the grid and never over it. Named rather than bare:
     * "started" alone cannot say WHICH program started, which is the only
     * question the message exists to answer. */
    if (g_msg_sel >= 0) {
        display_fill_rect(0, GRID_H, DISP_W, STATUS_H, COLOR_BLACK);
        display_text(3, GRID_H + 3u, g_msg_ok ? "started" : "no slot:",
                     g_msg_ok ? COLOR_GREEN : COLOR_RED, COLOR_BLACK, 1u);
        display_text(54, GRID_H + 3u, ICONS[g_msg_sel].label,
                     COLOR_WHITE, COLOR_BLACK, 1u);
    }

    draw_cursor(g_cx, g_cy);

    display_unlock();
}

static void open_selected(void)
{
    const desk_icon_t *ic = &ICONS[g_sel];
    g_opens++;
    g_msg_sel  = g_sel;
    g_msg_tick = timer_ticks();

    if (ic->action == DESK_ACTION_NOTES) {
        g_mode    = MODE_NOTES;
        notes_open();
        g_msg_ok  = 1;
        g_msg_sel = -1;
        return;
    }

    if (ic->action == DESK_ACTION_3D) {
        /* Hand the region over. The raycaster repaints every frame, so nothing
         * needs erasing first. */
        g_mode    = MODE_3D;
        g_msg_ok  = 1;
        g_msg_sel = -1;         /* nothing to report over a view we just left */
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
            g_mode  = MODE_LAUNCHER;
            g_dirty = 1;
        }
        return;
    }

    if (down) {
        /* The selection is latched from the FIRST sample of a press and never
         * moved again until the finger lifts.
         *
         * It used to follow every sample, which made a drag move the cursor —
         * pleasant in principle, and wrong in practice. A resistive panel's
         * worst readings come at contact and release, and the last sample
         * before release is precisely the one that decided the selection.
         * Measured: a tap on the top-right icon reported cell 2 on its first
         * sample and cell 3 on its last, so the correct target was read,
         * recorded, and then thrown away microseconds later.
         *
         * The first sample is the one taken while the finger is where the user
         * put it deliberately. Everything after it describes a finger being
         * lifted, which is not an instruction. */
        if (!g_was_down) {
            g_press_tick = now;

            int first = cell_at(x, y);
            if (first >= 0) {
                g_cx  = (x < DISP_W - 8u) ? x : DISP_W - 8u;
                g_cy  = (y < GRID_H - 8u) ? y : GRID_H - 8u;
                g_sel = first;
                g_dirty = 1;
            }
        }
        g_was_down = 1;

        /* Every later sample is recorded but acts on nothing. Kept because the
         * first/last pair is what diagnosed this: identical values mean a
         * steady press, and a divergence means the panel is smearing on
         * release. Deleting the instrument once it has found its bug is how
         * the bug comes back unnoticed. */
        int c = cell_at(x, y);
        if (g_samples == 0u) {
            g_first_x = x; g_first_y = y; g_first_cell = c;
        }
        g_last_x = x; g_last_y = y; g_last_cell = c;
        g_samples++;
        return;
    }

    /* Release. */
    if (!g_was_down) {
        return;
    }
    g_was_down = 0;
    uint32_t samples = g_samples;
    g_samples = 0;
    (void)samples;

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

uint32_t desktop_first_x(void)  { return g_first_x; }
uint32_t desktop_first_y(void)  { return g_first_y; }
int      desktop_first_cell(void) { return g_first_cell; }
uint32_t desktop_last_x(void)   { return g_last_x; }
uint32_t desktop_last_y(void)   { return g_last_y; }
int      desktop_last_cell(void)  { return g_last_cell; }
int      desktop_sel(void)      { return g_sel; }
