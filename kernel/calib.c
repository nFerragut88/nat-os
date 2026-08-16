/* nat-os — touch calibration by tapping targets. See calib.h.
 *
 * Four targets, INSET from the edges. That inset is the entire point.
 *
 * The calibration this replaces was taken by tapping the four corners of the
 * glass (UM-NATOS-017 §7.1). A finger cannot reach the extreme corner of a
 * bezelled panel, so every corner reading was short of the true extreme, the
 * derived range came out narrower than reality, and every mapped coordinate
 * landed inward of the finger — about 24 px on X by the time it reached the
 * middle of the screen. That was enough to make a 24 px keyboard key type its
 * neighbour (UM-NATOS-022 §3).
 *
 * Targets at 30 px in are reachable, so the readings are real positions rather
 * than the closest a fingertip could get to an unreachable one. The mapping is
 * then interpolated outward to the edges instead of extrapolated inward from a
 * guess.
 */

#include "calib.h"
#include "display.h"
#include "touch.h"
#include "uart.h"

#define INSET   30u
#define TARGETS 4u

/* Screen positions of the targets, in the order they are asked for. Two on each
 * side of both axes, so each axis is fitted from an average of two readings and
 * a single bad tap cannot define the whole scale. */
static const uint32_t TX[TARGETS] = { INSET, DISP_W - INSET, INSET,          DISP_W - INSET };
static const uint32_t TY[TARGETS] = { INSET, INSET,          DISP_H - INSET, DISP_H - INSET };

static uint32_t g_raw_x[TARGETS];
static uint32_t g_raw_y[TARGETS];
static uint32_t g_step;
static int      g_running;
static int      g_was_down;

int calib_running(void) { return g_running; }

static void draw_target(uint32_t x, uint32_t y, uint16_t colour)
{
    display_fill_rect(x - 10u, y, 21u, 1u, colour);
    display_fill_rect(x, y - 10u, 1u, 21u, colour);
    display_fill_rect(x - 2u, y - 2u, 5u, 5u, colour);
}

static void draw_screen(void)
{
    display_lock();
    display_clear(COLOR_BLACK);

    display_text(20, DISP_H / 2u - 20u, "TOUCH CALIBRATION",
                 COLOR_WHITE, COLOR_BLACK, 1u);
    display_text(20, DISP_H / 2u - 6u, "tap the centre of each cross",
                 COLOR_GREY, COLOR_BLACK, 1u);

    char n[8];
    n[0] = (char)('1' + g_step);
    n[1] = ' ';
    n[2] = 'o';
    n[3] = 'f';
    n[4] = ' ';
    n[5] = (char)('0' + TARGETS);
    n[6] = 0;
    display_text(20, DISP_H / 2u + 8u, n, COLOR_GREY, COLOR_BLACK, 1u);

    draw_target(TX[g_step], TY[g_step], COLOR_CYAN);
    display_unlock();
}

void calib_start(void)
{
    g_step     = 0;
    g_running  = 1;
    g_was_down = 0;
    draw_screen();
}

/* Fits both axes and installs the result.
 *
 * X is inverted (raw decreases as the finger moves right), so the fit is done
 * against the inverted screen coordinate and the sense falls out of the
 * arithmetic rather than being assumed. */
static void finish(void)
{
    /* Average the two readings on each side. */
    uint32_t left_raw   = (g_raw_x[0] + g_raw_x[2]) / 2u;   /* screen x = INSET */
    uint32_t right_raw  = (g_raw_x[1] + g_raw_x[3]) / 2u;   /* screen x = W-INSET */
    uint32_t top_raw    = (g_raw_y[0] + g_raw_y[1]) / 2u;   /* screen y = INSET */
    uint32_t bottom_raw = (g_raw_y[2] + g_raw_y[3]) / 2u;   /* screen y = H-INSET */

    uint32_t xmin = 0, xmax = 0, ymin = 0, ymax = 0;
    int ok = 0;

    /* X: screen = (W-1) - (raw - xmin) * W / (xmax - xmin).
     *
     * Two known screen positions give the span and the offset. Everything is
     * done in int32 with the span scaled up front; the raw range is a few
     * thousand and the screen a few hundred, so nothing here approaches the
     * limits that forced the scaled multiply in raycast.c. */
    if (left_raw > right_raw + 100u) {
        int32_t d_raw    = (int32_t)left_raw - (int32_t)right_raw;
        int32_t d_screen = (int32_t)(DISP_W - INSET) - (int32_t)INSET;
        int32_t span     = (d_raw * (int32_t)DISP_W) / d_screen;

        /* At screen x = W-INSET the inverted term is (W-1)-(W-INSET). */
        int32_t inv_right = (int32_t)(DISP_W - 1u) - (int32_t)(DISP_W - INSET);
        xmin = (uint32_t)((int32_t)right_raw - (inv_right * span) / (int32_t)DISP_W);
        xmax = xmin + (uint32_t)span;
        ok = 1;
    }

    /* Y: screen = (raw - ymin) * H / (ymax - ymin), no inversion. */
    if (bottom_raw > top_raw + 100u && ok) {
        int32_t d_raw    = (int32_t)bottom_raw - (int32_t)top_raw;
        int32_t d_screen = (int32_t)(DISP_H - INSET) - (int32_t)INSET;
        int32_t span     = (d_raw * (int32_t)DISP_H) / d_screen;

        ymin = (uint32_t)((int32_t)top_raw - ((int32_t)INSET * span) / (int32_t)DISP_H);
        ymax = ymin + (uint32_t)span;
    } else {
        ok = 0;
    }

    g_running = 0;

    uart_puts("\n  calibration: ");
    if (!ok) {
        uart_puts("REJECTED - readings too close together, nothing changed\n");
        return;
    }

    touch_set_calibration(xmin, xmax, ymin, ymax);

    /* Read back what was actually installed rather than what was computed:
     * touch_set_calibration() refuses a degenerate range, and reporting the
     * request instead of the result would hide that. */
    uint32_t ax, bx, ay, by;
    touch_get_calibration(&ax, &bx, &ay, &by);
    uart_puts("x ");
    uart_put_dec(ax);
    uart_puts("..");
    uart_put_dec(bx);
    uart_puts("   y ");
    uart_put_dec(ay);
    uart_puts("..");
    uart_put_dec(by);
    uart_puts(ax == xmin ? "   (installed)\n" : "   (REFUSED, kept previous)\n");

    calib_persist(ax, bx, ay, by);
}

void calib_touch(uint32_t raw_x, uint32_t raw_y, int down)
{
    if (!g_running) {
        return;
    }
    if (!down) {
        g_was_down = 0;
        return;
    }
    if (g_was_down) {
        return;                 /* one reading per press */
    }
    g_was_down = 1;

    g_raw_x[g_step] = raw_x;
    g_raw_y[g_step] = raw_y;

    /* Confirm the tap landed, so a press that produced no reading is visibly
     * different from one that did. */
    display_lock();
    draw_target(TX[g_step], TY[g_step], COLOR_GREEN);
    display_unlock();

    g_step++;
    if (g_step >= TARGETS) {
        finish();
        return;
    }
    draw_screen();
}
