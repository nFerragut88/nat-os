/* nat-os — grid raycaster. See raycast.h for the camera-plane argument. */

#include "raycast.h"
#include "timer.h"
#include "desktop.h"
#include "display.h"
#include "heap.h"
#include "xtensa.h"
#include "generated/sintab.h"

#define MAP_W 16
#define MAP_H 16

/* 1 is wall, 0 is floor. A ring with a few interior blocks, so walking it
 * produces corridors and openings rather than one empty box. */
static const uint8_t MAP[MAP_H][MAP_W] = {
    {1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1},
    {1,0,0,0,0,0,0,1,0,0,0,0,0,0,0,1},
    {1,0,1,1,0,1,0,1,0,1,1,1,0,1,0,1},
    {1,0,1,0,0,1,0,0,0,1,0,0,0,1,0,1},
    {1,0,1,0,1,1,1,1,1,1,0,1,1,1,0,1},
    {1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1},
    {1,1,1,1,0,1,1,1,0,1,1,1,1,1,0,1},
    {1,0,0,0,0,1,0,0,0,0,0,0,0,1,0,1},
    {1,0,1,1,0,1,0,1,1,1,1,1,0,1,0,1},
    {1,0,1,0,0,0,0,1,0,0,0,0,0,0,0,1},
    {1,0,1,0,1,1,1,1,0,1,1,1,1,1,0,1},
    {1,0,0,0,0,0,0,0,0,0,0,0,0,1,0,1},
    {1,1,1,1,1,0,1,1,1,1,1,0,0,1,0,1},
    {1,0,0,0,0,0,0,0,0,0,1,0,1,1,0,1},
    {1,0,1,1,1,1,1,1,1,0,0,0,0,0,0,1},
    {1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1},
};

#define FP        16
#define ONE       FP_ONE
#define STEP_SHIFT 3                    /* march 1/8 of a cell per step */
#define MAX_STEPS  160                  /* 20 cells before giving up    */

/* tan(30 deg) in 16.16 — half of a 60 degree field of view. */
#define PLANE_SCALE 37837

static int32_t  g_px, g_py;             /* position, 16.16, in cells */
static uint32_t g_angle;                /* 0..255, one turn          */
static uint32_t g_frames, g_columns;

/* Split the frame cost three ways: ray marching, column composition, and the
 * SPI transfer. Guessing which dominates has been wrong often enough in this
 * project that it is cheaper to measure. */
static uint32_t g_us_march, g_us_compose, g_us_blit;
static uint32_t g_last_move_tick;

/* ---- walk and steer ------------------------------------------------------
 *
 * MOVE_SHIFT  distance per tick, as a shift of the unit direction vector.
 *             6 is 1/64 of a cell per tick, so about 1.5 cells a second once
 *             MOVE_CATCHUP_MAX stopped throwing most of the ticks away.
 * LOOK_SHIFT  how far ahead a wall triggers a turn. 0 is a FULL cell — half a
 *             cell was not enough warning at this speed, which is what "runs
 *             into walls before turning" meant the second time.
 * STOP_SHIFT  how far ahead a wall stops the walk. 3 is an eighth of a cell,
 *             so the camera keeps advancing through a turn and only halts when
 *             it is genuinely about to embed.
 * TURN_UNITS  angle per tick while a turn is wanted, out of 256 for a full
 *             circle. 1 is 1.4 degrees — a sweep, where the 3 it replaced was
 *             a 4.2-degree jerk applied only on contact. */
#define MOVE_SHIFT  6
#define LOOK_SHIFT  0
#define STOP_SHIFT  3
#define TURN_UNITS  1

/* Ticks of movement a single frame may apply.
 *
 * This was 4, and it was the reason the camera crawled AND turned late. At
 * 9 fps about eleven ticks pass between frames, so the clamp discarded two
 * thirds of both the walking and the turning, every frame — a stall guard that
 * fired constantly during normal running.
 *
 * 20 is 200 ms, longer than any gap between frames the renderer produces, so it
 * now only bites on a genuine stall. The guard is still needed: without it, a
 * pause of a second would apply a second of movement in one step and walk
 * straight through a wall, because the collision probe only checks the
 * destination of each step and not the path to it. */
#define MOVE_CATCHUP_MAX 20u




/* Two pixels per row, so one composed buffer covers a 2-pixel-wide column.
 *
 * The data volume is identical either way — the panel receives the same 80,640
 * bytes. What halves is the number of address-window setups, and those measured
 * at ~450 us each against only ~94 us of pixel data per column. The overhead was
 * five times the payload. */
/* One ray per screen column.
 *
 * Was 2, which halved the cast cost at the price of halving horizontal detail.
 * That trade made sense when the renderer's cost was unknown; measurement says
 * it is not: marching is 0.2 ms of a 50 ms frame and the blit is 41.9 ms, so
 * the frame is bus-bound and casting twice as many rays is close to free. */
#define RAY_COLW  1u
#define RAY_COLS  (RAY_VIEW_W / RAY_COLW)

static uint16_t g_col[RAY_VIEW_H * RAY_COLW];

/* NULL when the direct path is in use. */
static uint16_t *g_fb;

static int32_t fsin(uint32_t a) { return SIN_TAB[a & (SIN_N - 1u)]; }
static int32_t fcos(uint32_t a) { return SIN_TAB[(a + 64u) & (SIN_N - 1u)]; }

static int wall_at(int32_t fx, int32_t fy)
{
    int32_t cx = fx >> FP;
    int32_t cy = fy >> FP;
    if (cx < 0 || cy < 0 || cx >= MAP_W || cy >= MAP_H) {
        return 1;                       /* outside the map counts as solid */
    }
    return MAP[cy][cx];
}

/* ---- navigation ----------------------------------------------------------
 *
 * Heading is chosen once per CELL, then held while the camera walks to the next
 * one. Steering is smooth: the angle turns toward the chosen heading a little
 * each tick, so the corner is rounded rather than snapped.
 *
 * The previous version steered reactively — probe ahead, turn while blocked —
 * and it could not traverse a maze. With corridors one cell wide, a probe
 * looking a full cell ahead is blocked almost all the time, so the camera
 * turned continuously and never committed to a direction. It covered four
 * distinct cells in twenty seconds and paced between them.
 *
 * Deciding per cell instead of per tick is the difference between a camera that
 * reacts to walls and one that follows corridors. The rule is left-first, which
 * is wall-following: with a consistent hand a connected maze gets walked
 * instead of paced.
 *
 * Headings are indices, not angles: 0 = +x, 1 = +y, 2 = -x, 3 = -y, matching
 * fcos/fsin at multiples of 64. */
static uint32_t g_heading;              /* 0..3 */
static uint32_t g_target_angle;         /* g_heading * 64 */
static int32_t  g_decided_x = -1, g_decided_y = -1;

static const int8_t HEAD_DX[4] = { 1, 0, -1, 0 };
static const int8_t HEAD_DY[4] = { 0, 1, 0, -1 };

static int cell_open(int32_t cx, int32_t cy, uint32_t h)
{
    int32_t nx = cx + HEAD_DX[h & 3u];
    int32_t ny = cy + HEAD_DY[h & 3u];
    if (nx < 0 || ny < 0 || nx >= MAP_W || ny >= MAP_H) {
        return 0;
    }
    return !MAP[ny][nx];
}

static void navigate_step(void)
{
    int32_t cx = g_px >> FP;
    int32_t cy = g_py >> FP;

    /* One decision per cell entered. Without this the heading would be
     * re-chosen every tick and the camera would dither on the boundary. */
    if (cx != g_decided_x || cy != g_decided_y) {
        g_decided_x = cx;
        g_decided_y = cy;

        /* Left, straight, right, back — the first that is open wins. */
        static const int8_t ORDER[4] = { 3, 0, 1, 2 };   /* -1, 0, +1, +2 */
        for (uint32_t i = 0; i < 4u; i++) {
            uint32_t h = (g_heading + (uint32_t)ORDER[i]) & 3u;
            if (cell_open(cx, cy, h)) {
                g_heading = h;
                break;
            }
        }
        g_target_angle = g_heading * 64u;
    }

    /* Turn toward the chosen heading, shortest way round, a little per tick. */
    uint32_t diff = (g_target_angle - g_angle) & (SIN_N - 1u);
    if (diff != 0u) {
        raycast_turn(diff < (SIN_N / 2u) ? TURN_UNITS : -TURN_UNITS);
    }

    /* Walk. The stop probe stays as a backstop: navigation should never aim at
     * a wall, and if it ever does this is what keeps the camera out of it. */
    int32_t dx = fcos(g_angle);
    int32_t dy = fsin(g_angle);
    int32_t nx = g_px + (dx >> MOVE_SHIFT);
    int32_t ny = g_py + (dy >> MOVE_SHIFT);
    if (!wall_at(nx + (dx >> STOP_SHIFT), ny + (dy >> STOP_SHIFT))) {
        g_px = nx;
        g_py = ny;
    }
}

void raycast_init(void)
{
    g_px = (1 * ONE) + (ONE / 2);       /* stand in the middle of a cell */
    g_py = (1 * ONE) + (ONE / 2);
    g_angle = 0;
    g_frames = g_columns = 0;
}

int raycast_framebuffer(void)   { return g_fb != 0; }
uint32_t raycast_fb_bytes(void) { return g_fb ? (RAY_VIEW_W * RAY_VIEW_H * 2u) : 0u; }

int raycast_set_framebuffer(int on)
{
    if (on && !g_fb) {
        g_fb = (uint16_t *)heap_alloc(RAY_VIEW_W * RAY_VIEW_H * 2u);
        return g_fb ? 0 : -1;
    }
    if (!on && g_fb) {
        heap_free(g_fb);
        g_fb = 0;
    }
    return 0;
}

void raycast_turn(int32_t units)
{
    g_angle = (uint32_t)((int32_t)g_angle + units) & (SIN_N - 1u);
}

/* Full-saturation hue, then scaled by `shade` (0..255) for distance falloff.
 * Integer only: six linear segments, which is what an HSV conversion collapses
 * to when saturation and value are pinned. */
static uint16_t hue_shaded(uint32_t h, uint32_t shade)
{
    h %= 192u;
    uint32_t seg = h / 32u;
    uint32_t f   = (h % 32u) * 8u;
    uint32_t r, g, b;

    switch (seg) {
    case 0:  r = 255;     g = f;       b = 0;       break;
    case 1:  r = 255 - f; g = 255;     b = 0;       break;
    case 2:  r = 0;       g = 255;     b = f;       break;
    case 3:  r = 0;       g = 255 - f; b = 255;     break;
    case 4:  r = f;       g = 0;       b = 255;     break;
    default: r = 255;     g = 0;       b = 255 - f; break;
    }

    r = (r * shade) >> 8;
    g = (g * shade) >> 8;
    b = (b * shade) >> 8;
    return RGB(r, g, b);
}

uint32_t raycast_us_march(void)   { return g_us_march; }
uint32_t raycast_us_compose(void) { return g_us_compose; }
uint32_t raycast_us_blit(void)    { return g_us_blit; }

void raycast_frame(void)
{
    uint32_t t_march = 0, t_compose = 0, t_blit = 0, t0;

    int32_t dirX = fcos(g_angle);
    int32_t dirY = fsin(g_angle);

    /* Perpendicular to dir, scaled by tan(FOV/2). Because dir is unit length
     * and plane is perpendicular to it, distance marched along a ray equals the
     * perpendicular distance to the camera plane — which is what makes the
     * fisheye correction unnecessary. */
    /* (a * b) >> 16 computed as (a/256) * (b/256).
     *
     * The direct form overflows: 65536 * 37837 is 2.48e9 against an int32 limit
     * of 2.14e9. Widening to int64 would emit calls to libgcc's 64-bit helpers,
     * which this kernel does not link (-nostdlib), so the multiply is scaled
     * down instead. It costs eight bits of fraction — about 1/256 of a cell,
     * far below one pixel at any distance drawn here. */
    /* The lock covers only what actually needs it.
     *
     * With a framebuffer, marching and composing write to g_fb — private to
     * this file, touching no shared hardware and no shared buffer. Holding the
     * draw lock across them served nothing except to block every application
     * for an extra 7.8 ms per frame, measured. Only the blit talks to the
     * panel, and only the blit is now inside the lock.
     *
     * Without a framebuffer the loop blits per column, so the lock has to span
     * the whole loop — that is UM-NATOS-014 §5.2's fix, replacing 240
     * acquisitions with one, and it still applies to that path.
     *
     * The general rule this project keeps relearning: a lock should cover the
     * shared thing, not the whole operation that happens to use it. */
    if (!g_fb) {
        display_lock();
    }

    int32_t planeX = (-dirY / 256) * (PLANE_SCALE / 256);
    int32_t planeY = ( dirX / 256) * (PLANE_SCALE / 256);

    for (uint32_t c = 0; c < RAY_COLS; c++) {
        uint32_t x = c * RAY_COLW;
        /* -1 at the left edge, +1 at the right. */
        int32_t cameraX = (int32_t)((2 * x * ONE) / RAY_VIEW_W) - ONE;

        int32_t rayX = dirX + (planeX / 256) * (cameraX / 256);
        int32_t rayY = dirY + (planeY / 256) * (cameraX / 256);

        /* March by a fixed fraction of a cell. Stepping incrementally avoids
         * multiplying the ray by a growing t, which would overflow 32 bits
         * within a few cells. */
        int32_t sx = rayX >> STEP_SHIFT;
        int32_t sy = rayY >> STEP_SHIFT;

        int32_t  fx = g_px, fy = g_py;
        int      hit = 0, steps = 0;
        int32_t  hx = 0, hy = 0;

        t0 = xt_ccount();
        while (steps < MAX_STEPS) {
            fx += sx;
            fy += sy;
            steps++;
            if (wall_at(fx, fy)) {
                hit = 1;
                hx = fx >> FP;
                hy = fy >> FP;
                break;
            }
        }

        t_march += xt_ccount() - t0;

        /* Perpendicular distance, in 16.16 cells. */
        int32_t dist = (int32_t)steps << (FP - STEP_SHIFT);
        if (dist < (ONE / 8)) {
            dist = ONE / 8;             /* never divide by ~zero */
        }

        int32_t h = hit ? (int32_t)((RAY_VIEW_H * ONE) / dist) : 0;
        if (h > (int32_t)RAY_VIEW_H) {
            h = RAY_VIEW_H;
        }

        int32_t top = ((int32_t)RAY_VIEW_H - h) / 2;
        int32_t bot = top + h;

        /* Distance falloff. Full brightness up close, floor of 40 so a far wall
         * stays visible rather than fading into the ceiling. */
        uint32_t shade = 255u;
        if (dist > ONE) {
            uint32_t d = (uint32_t)(dist >> FP);
            shade = (d >= 12u) ? 40u : (255u - d * 18u);
        }

        /* Hue from the cell, so each wall block is its own colour and the room
         * reads as a rainbow rather than one tinted box. */
        uint16_t wall = hue_shaded((uint32_t)(hx * 23 + hy * 41), shade);

        t0 = xt_ccount();
        for (int32_t y = 0; y < (int32_t)RAY_VIEW_H; y++) {
            uint16_t px;
            if (y < top) {
                /* Ceiling: darkens towards the horizon. */
                uint32_t s = 30u + (uint32_t)(y * 40) / (top > 0 ? top : 1);
                px = RGB(s / 3u, s / 3u, s);
            } else if (y < bot) {
                px = wall;
            } else {
                /* Floor: brightens towards the viewer. */
                int32_t below = y - bot;
                int32_t span  = (int32_t)RAY_VIEW_H - bot;
                uint32_t s = 20u + (uint32_t)(below * 50) / (span > 0 ? span : 1);
                px = RGB(s, s / 2u, s / 3u);
            }
            /* Written RAY_COLW times rather than twice: the column width is a
             * constant that has now changed once, and code that hardcodes it
             * is code that silently writes into the neighbouring column when it
             * changes again. */
            if (g_fb) {
                /* Row-major, so the finished view is one contiguous stream. */
                uint16_t *row = g_fb + (uint32_t)y * RAY_VIEW_W + x;
                for (uint32_t k = 0; k < RAY_COLW; k++) {
                    row[k] = px;
                }
            } else {
                for (uint32_t k = 0; k < RAY_COLW; k++) {
                    g_col[y * RAY_COLW + k] = px;
                }
            }
        }

        t_compose += xt_ccount() - t0;

        /* Direct path only: with a framebuffer the whole view goes out once,
         * after every column has been composed. */
        if (!g_fb) {
            t0 = xt_ccount();
            display_blit(x, 0, RAY_COLW, RAY_VIEW_H, g_col, RAY_COLW);
            t_blit += xt_ccount() - t0;
        }
        g_columns++;
    }

    if (g_fb) {
        /* One window for the entire view. Stride equals width, so this takes
         * the contiguous path and streams 80,640 bytes without another setup. */
        /* Chrome goes into the buffer, not on top of the panel afterwards.
         * This view repaints every pixel every frame, so anything drawn over it
         * survives only until the next frame begins — the close button drawn
         * that way strobed hard enough to make the view look broken. Stamping it
         * here makes the button and the frame one transfer. */
        desktop_overlay_into(g_fb, RAY_VIEW_W, RAY_VIEW_H);

        t0 = xt_ccount();
        display_lock();
        display_blit(0, 0, RAY_VIEW_W, RAY_VIEW_H, g_fb, RAY_VIEW_W);
        display_unlock();
        t_blit += xt_ccount() - t0;
    } else {
        display_unlock();
    }

    /* Walk forward; turn when something is close ahead. Probing half a cell in
     * front rather than at the feet stops the camera burying itself in a wall
     * before the turn takes effect. */
    /* Movement is per TICK, not per frame.
     *
     * A fixed step every frame made the camera's speed a function of the
     * renderer's throughput. Harmless at three frames a second; at sixteen the
     * camera advanced five times faster than the turn-away probe could react,
     * buried itself in a wall, and every column rendered as one flat colour —
     * measured as cam=6,1 hit0=0, a centre-column hit distance of zero cells.
     *
     * Speed and steering are four constants, all per tick — see their
     * definitions above. Keeping them per tick is what stops a faster renderer
     * becoming a faster camera, which it did once already. */
    uint32_t now = timer_ticks();
    uint32_t elapsed = now - g_last_move_tick;
    if (elapsed > MOVE_CATCHUP_MAX) {
        elapsed = MOVE_CATCHUP_MAX;
    }
    g_last_move_tick = now;

    for (uint32_t step = 0; step < elapsed; step++) {
        navigate_step();
    }

    g_us_march   = t_march   / 80u;     /* 80 MHz -> microseconds */
    g_us_compose = t_compose / 80u;
    g_us_blit    = t_blit    / 80u;
    g_frames++;
}

/* Camera cell, for the reporter. A view that looks wrong has two very
 * different causes — nothing drawn, or a correct view from inside a wall — and
 * only the camera position tells them apart. */
uint32_t raycast_cam_x(void) { return (uint32_t)(g_px >> FP); }
uint32_t raycast_cam_y(void) { return (uint32_t)(g_py >> FP); }

uint32_t raycast_frames(void)  { return g_frames; }
uint32_t raycast_columns(void) { return g_columns; }
