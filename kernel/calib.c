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
#include "desktop.h"

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

/* The outcome of the last run, kept rather than only printed.
 *
 * The result line is emitted the instant the fourth target is tapped, which is
 * exactly when nobody has a capture attached — the same way the touch log's
 * values were lost before it was changed to hold them. A number that has
 * already scrolled past is not a measurement. */
static int      g_last_ok = -1;     /* -1 never run, 0 rejected, 1 installed */
static uint32_t g_taps_taken;
static int      g_disagree = -1;    /* index of the pair that failed, -1 = none */

/* How far two readings that should match may differ before the run is refused.
 * The usable raw span is about 3000 and a fingertip is worth a few hundred of
 * it, so 900 admits a sloppy tap and rejects a tap on the wrong cross. */
#define PAIR_TOLERANCE 900u

/* The four pairs that must agree: two targets share a screen x, two share a
 * screen y, so each reading has a partner that should nearly match it. */
static const uint32_t PAIR_A[4] = { 0u, 1u, 0u, 2u };
static const uint32_t PAIR_B[4] = { 2u, 3u, 1u, 3u };
static const char *const PAIR_AXIS[4] = { "x", "x", "y", "y" };

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

    /* Check the pairs before fitting anything.
     *
     * Averaging two readings guards against one sloppy tap, but it also hides a
     * tap on the wrong cross: 3453 and 353 average to 1903, which looks like a
     * perfectly ordinary number and is a measurement of nothing. The averaged
     * fit then fails its own sanity check and reports "readings too close
     * together", which is true of the averages and says nothing about the
     * mistake that produced them.
     *
     * Two targets share each screen coordinate, so every reading has a partner
     * it should nearly match. Comparing them first turns a vague rejection into
     * a specific one. */
    g_disagree = -1;
    for (int p = 0; p < 4; p++) {
        const uint32_t *v = (p < 2) ? g_raw_x : g_raw_y;
        uint32_t a = v[PAIR_A[p]], b = v[PAIR_B[p]];
        uint32_t d = (a > b) ? a - b : b - a;
        if (d > PAIR_TOLERANCE) {
            g_disagree = p;
            break;
        }
    }

    uint32_t xmin = 0, xmax = 0, ymin = 0, ymax = 0;
    int ok = (g_disagree < 0);

    /* X: screen = (W-1) - (raw - xmin) * W / (xmax - xmin).
     *
     * Two known screen positions give the span and the offset. Everything is
     * done in int32 with the span scaled up front; the raw range is a few
     * thousand and the screen a few hundred, so nothing here approaches the
     * limits that forced the scaled multiply in raycast.c. */
    if (ok && left_raw > right_raw + 100u) {
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
    desktop_invalidate();       /* give the panel back with something on it */

    uart_puts("\n  calibration: ");
    if (!ok) {
        g_last_ok = 0;              /* rejected is an outcome, not an absence */
        if (g_disagree >= 0) {
            uart_puts("REJECTED - targets ");
            uart_put_dec(PAIR_A[g_disagree] + 1u);
            uart_puts(" and ");
            uart_put_dec(PAIR_B[g_disagree] + 1u);
            uart_puts(" share a screen ");
            uart_puts(PAIR_AXIS[g_disagree]);
            uart_puts(" but read far apart.\n");
            uart_puts("             tap the cross that is SHOWING, one at a time.\n");
        } else {
            uart_puts("REJECTED - readings too close together, nothing changed\n");
        }
        uart_puts("             nothing changed; 'calshow' has the readings\n");
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

    g_last_ok = (ax == xmin);
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
    g_taps_taken++;

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

/* Reports what the last run measured and what is installed now. */
void calib_report(void)
{
    uart_puts("   taps taken   : ");
    uart_put_dec(g_taps_taken);
    uart_puts("\n   last run     : ");
    uart_puts(g_last_ok < 0 ? "never run\n" :
              g_last_ok ? "installed\n" : "rejected\n");

    if (g_disagree >= 0) {
        uart_puts("   disagreement : targets ");
        uart_put_dec(PAIR_A[g_disagree] + 1u);
        uart_puts(" and ");
        uart_put_dec(PAIR_B[g_disagree] + 1u);
        uart_puts(" share a screen ");
        uart_puts(PAIR_AXIS[g_disagree]);
        uart_puts("\n");
    }

    for (uint32_t i = 0; i < TARGETS; i++) {
        uart_puts("   target ");
        uart_put_dec(i + 1u);
        uart_puts(" at ");
        uart_put_dec(TX[i]);
        uart_putc(',');
        uart_put_dec(TY[i]);
        uart_puts("  raw ");
        uart_put_dec(g_raw_x[i]);
        uart_putc(',');
        uart_put_dec(g_raw_y[i]);
        uart_puts("\n");
    }

    uint32_t ax, bx, ay, by;
    touch_get_calibration(&ax, &bx, &ay, &by);
    uart_puts("   in use       : x ");
    uart_put_dec(ax);
    uart_puts("..");
    uart_put_dec(bx);
    uart_puts("  y ");
    uart_put_dec(ay);
    uart_puts("..");
    uart_put_dec(by);
    uart_puts("\n");
}
