/* nat-os — XPT2046 touchscreen. See touch.h for the pin map and rationale. */

#include "touch.h"
#include "display.h"
#include "gpio.h"
#include "xtensa.h"
#include "intr.h"
#include "task.h"
#include "critical.h"

#define PIN_CLK   25u
#define PIN_MOSI  32u
#define PIN_MISO  39u
#define PIN_CS    33u
#define PIN_IRQ   36u

/* Control bytes. Bit 7 starts a conversion; bits 6:4 select the channel; bit 3
 * clear selects 12-bit; bit 2 clear selects differential mode, which cancels
 * the supply variation single-ended mode is sensitive to.
 *
 * Bits 1:0 are PD1:PD0, the power-down mode, and they were the defect. With
 * PD=00 the ADC and its reference power down between every conversion, and the
 * first conversion after power-up is specified as inaccurate. Z1 is the first
 * conversion after CS goes low on every single read, so the one channel that
 * decides whether a touch exists was always the throwaway sample: z1 never
 * exceeded 1 across 150 samples including a firm held press.
 *
 * PD=11 keeps the reference and ADC powered for the whole burst. The final
 * command uses PD=00 to power down again and re-enable PENIRQ, which is what
 * makes the IRQ line usable later. */
#define PD_ON   0x03u
#define CMD_Y   (0x90u | PD_ON)
#define CMD_X   (0xD0u | PD_ON)
#define CMD_Z1  (0xB0u | PD_ON)
#define CMD_Z2  (0xC0u | PD_ON)
#define CMD_IDLE 0x90u              /* PD=00: power down, PENIRQ back on */

/* Calibration, measured on this board by tapping the four corners.
 *
 *      corner          raw_x   raw_y
 *      top-left         3360     416
 *      top-right         591     376
 *      bottom-left      3258    3518
 *      bottom-right      376    3462
 *
 * ---- the X axis runs backwards --------------------------------------------
 *
 * raw_x DECREASES as the finger moves right. Left reads ~3300, right reads
 * ~480. The mapping did not account for that, so the top-left corner reported
 * x=216 of 240 — the far right — and every touch landed on the wrong side of
 * the screen. TOUCH_X_INVERTED below is the whole fix.
 *
 * ---- why the original calibration passed ----------------------------------
 *
 * This axis has been backwards since the touch driver was written, and the
 * calibration in UM-NATOS-017 §7 did not catch it — but NOT because it ignored
 * direction. It tested direction explicitly, and concluded the opposite:
 *
 *     "the horizontal drag ended at rx=3527 against a maximum of 3536,
 *      so raw X increases left to right"
 *
 * The flaw is which sample it trusted. Direction was inferred from where a drag
 * ENDED, and the last sample of a drag is the release sample — taken as the
 * finger lifts, when PENIRQ still reads pressed and the ADC reads its rail.
 * A rail reading is 4095, near the top of the range, so a drag ending anywhere
 * at all "ends near the maximum" and every axis appears to increase.
 *
 * The same corrupted sample later broke the launcher's selection. One defect,
 * two symptoms, three months apart.
 *
 * Nothing downstream noticed for the same reason the defect survived: the
 * application strips are full width, so horizontal position never mattered,
 * and the raycaster's left/right steering was simply reversed — which reads as
 * a control preference, not a fault. The launcher is the first consumer that
 * depends on WHERE across the screen a finger is, and it failed immediately.
 *
 * The corner values above are what a calibration must record: four labelled
 * points, not two unlabelled extremes. */
/* Runtime, not compile-time, so a calibration routine can replace them and a
 * saved result can be restored at boot. The values below are the defaults from
 * the corner measurement described above — usable, and wrong by about 24 px on
 * X for the reason in the next paragraph. */
static uint32_t g_cal_x_min = 380u;     /* right edge — the LOW end of raw_x */
static uint32_t g_cal_x_max = 3380u;    /* left edge                         */
static uint32_t g_cal_y_min = 380u;     /* top                               */
static uint32_t g_cal_y_max = 3520u;    /* bottom                            */

void touch_set_calibration(uint32_t xmin, uint32_t xmax,
                           uint32_t ymin, uint32_t ymax)
{
    /* Refuse a degenerate or inverted range rather than installing it. A bad
     * calibration makes the panel unusable, which also makes it impossible to
     * run the calibration routine again — the failure would be
     * self-perpetuating. */
    if (xmax <= xmin + 100u || ymax <= ymin + 100u) {
        return;
    }
    g_cal_x_min = xmin;
    g_cal_x_max = xmax;
    g_cal_y_min = ymin;
    g_cal_y_max = ymax;
}

void touch_get_calibration(uint32_t *xmin, uint32_t *xmax,
                           uint32_t *ymin, uint32_t *ymax)
{
    *xmin = g_cal_x_min;
    *xmax = g_cal_x_max;
    *ymin = g_cal_y_min;
    *ymax = g_cal_y_max;
}

#define TOUCH_X_INVERTED 1

#define Z_THRESHOLD 300u

/* Latest reading, published for consumers that must not touch the bus
 * themselves. A syscall doing its own SPI would cost milliseconds per call and
 * contend with the polling task for the controller. */
static touch_state_t g_latest;

void touch_latest(touch_state_t *out) { *out = g_latest; }

static uint32_t g_samples;
static uint32_t g_events;

/* Extremes since boot. Confirming what the pressure channels do under a finger
 * needs a capture to coincide with the press, which is not something that can
 * be arranged reliably. Latching the extremes makes the question answerable at
 * any later moment instead. */
/* PENIRQ observations. The pin idles high and is pulled low by the panel
 * itself when it is touched — no ADC involved, which is why it is trusted over
 * the Z channels here. */
static uint32_t g_irq_low;
static uint32_t g_max_z1;
static uint32_t g_min_z2 = 0xFFFFFFFFu;
static uint32_t g_max_z;

/* Span of each raw axis while touched. A single drag along one screen axis
 * makes the mapping self-evident: whichever raw channel sweeps a wide range is
 * the one that tracks that screen axis, and the direction of travel falls out
 * of which end it started from. Guessing at swaps and flips costs a flash cycle
 * per guess; one controlled drag answers both at once. */
static uint32_t g_rx_min = 0xFFFFFFFFu, g_rx_max;
static uint32_t g_ry_min = 0xFFFFFFFFu, g_ry_max;
static uint32_t g_rx_first, g_ry_first, g_first_set;

uint32_t touch_rx_min(void)  { return g_rx_min; }
uint32_t touch_rx_max(void)  { return g_rx_max; }
uint32_t touch_ry_min(void)  { return g_ry_min; }
uint32_t touch_ry_max(void)  { return g_ry_max; }
uint32_t touch_rx_first(void){ return g_rx_first; }
uint32_t touch_ry_first(void){ return g_ry_first; }

uint32_t touch_max_z1(void) { return g_max_z1; }
uint32_t touch_min_z2(void) { return g_min_z2; }
uint32_t touch_max_z(void)  { return g_max_z; }
uint32_t touch_irq_lows(void) { return g_irq_low; }

/* The controller is specified to about 2 MHz and a GPIO loop runs faster than
 * that, so each edge is held briefly. Cheaper and more predictable than tuning
 * a cycle count, and touch is polled at tens of hertz — the cost is invisible. */
static inline void settle(void)
{
    for (volatile int i = 0; i < 3; i++) {
    }
}

/* SPI mode 0: data presented while the clock is low, sampled on the rising
 * edge. MISO is read while the clock is high, before it falls again. */
static uint32_t xfer_bits(uint32_t out, int nbits)
{
    uint32_t in = 0;

    for (int i = nbits - 1; i >= 0; i--) {
        if ((out >> i) & 1u) {
            gpio_set(PIN_MOSI);
        } else {
            gpio_clear(PIN_MOSI);
        }
        settle();
        gpio_set(PIN_CLK);
        settle();
        in = (in << 1) | gpio_read(PIN_MISO);
        gpio_clear(PIN_CLK);
    }
    return in;
}

/* One conversion: eight command bits, then sixteen clocks during which the
 * controller returns a busy bit, the twelve data bits, and three trailing
 * zeros. Shifting right by three leaves the 12-bit result. */
static uint32_t convert(uint8_t cmd)
{
    xfer_bits(cmd, 8);
    return (xfer_bits(0, 16) >> 3) & 0xFFFu;
}

void touch_init(void)
{
    gpio_out_init(PIN_CLK,  IO_MUX_GPIO25);
    gpio_out_init(PIN_MOSI, IO_MUX_GPIO32);
    gpio_out_init(PIN_CS,   IO_MUX_GPIO33);
    gpio_in_init(PIN_MISO,  IO_MUX_GPIO39);
    gpio_in_init(PIN_IRQ,   IO_MUX_GPIO36);

    gpio_set(PIN_CS);           /* idle high */
    gpio_clear(PIN_CLK);        /* mode 0 idles low */
}

/* ---- PENIRQ as an interrupt --------------------------------------------
 *
 * The pin idles high and is pulled low by the panel while a finger bridges it,
 * so a press is a FALLING edge. Edge rather than level, deliberately: a level
 * trigger stays asserted for the whole time a finger is down, and since the
 * handler cannot clear a condition the finger is causing, the machine would
 * re-enter this handler until the finger lifted and never run a task again.
 * That is the failure intr_dispatch() defends against, and it is better not to
 * arm it in the first place.
 *
 * Only the press is caught. Releases are found by the sampling that the press
 * starts — see the wait protocol below — because a resistive panel's release is
 * exactly the transition it reports least reliably (UM-NATOS-017 §7.1), and a
 * missed release edge would leave the pointer stuck down forever.
 */
static volatile int      g_irq_waiter = -1;   /* task to wake, -1 = nobody */
static volatile uint32_t g_irq_fires;
static volatile uint32_t g_irq_wakes;
static volatile uint32_t g_irq_waits;   /* times a task actually waited */
static volatile int      g_irq_flag;    /* an edge arrived with nobody waiting */
static volatile int      g_irq_last_reg = -2;  /* what the waiter registered AS */
static volatile int      g_irq_seen_by_isr = -2; /* what the ISR read back */
static volatile uint32_t g_irq_armed_rb;    /* PIN_REG read back after arming */

static void touch_isr(void)
{
    /* Clear FIRST. The pin is shared with every other GPIO on the same
     * interrupt source, and a status bit left set holds that source asserted. */
    gpio_int_clear(PIN_IRQ);
    g_irq_fires++;

    int w = g_irq_waiter;
    g_irq_seen_by_isr = w;
    if (w >= 0) {
        g_irq_waiter = -1;
        g_irq_wakes++;
        task_wake(w);
    } else {
        /* Nobody was waiting. That is usually this driver interrupting itself
         * — the controller drives PENIRQ during a conversion, so every read
         * generates edges while the task is awake — but it is also what a
         * genuine edge looks like if it lands in the gap between registering as
         * the waiter and actually going to sleep.
         *
         * Latching it costs nothing and closes that gap: the next wait sees the
         * flag and returns immediately instead of sleeping through an event
         * that already happened. */
        g_irq_flag = 1;
    }
}

void touch_irq_init(void)
{
    intr_route(INTR_SRC_GPIO_PRO, INTR_LINE_GPIO, touch_isr);
    gpio_int_enable(PIN_IRQ, GPIO_INT_FALLING);
}

/* Waits for a press, or for `timeout_ticks` to pass, whichever comes first.
 *
 * The registration and the sleep must not be separated by an interrupt: if
 * PENIRQ fired between "I am the waiter" and "I am asleep", the ISR would wake a
 * task that is still running and the sleep that followed would run its full
 * timeout with the event already gone. Registering inside a critical section
 * closes that window — the same reasoning task_block()'s comment gives for not
 * yielding itself.
 */
void touch_irq_wait(uint32_t timeout_ticks)
{
    uint32_t crit = crit_enter();
    if (g_irq_flag) {
        g_irq_flag = 0;
        crit_exit(crit);
        return;                     /* an edge is already outstanding */
    }
    g_irq_waiter = task_current();
    g_irq_last_reg = g_irq_waiter;
    g_irq_waits++;

    /* Arm here and nowhere else: the pen is up, no conversion is in flight, and
     * a waiter is registered, so the only edge that can arrive is a finger. */
    gpio_int_enable(PIN_IRQ, GPIO_INT_FALLING);
    /* Read back what the arm actually left in the register, from inside the
     * window itself. Sampling it from the shell showed zero three times running,
     * which cannot be chance when the window is 93% of the cycle — so the
     * question is whether the arm sticks, and only the armed task can answer
     * without the sampling being correlated with something. */
    g_irq_armed_rb = GPIO_REG(GPIO_PIN_REG(PIN_IRQ));
    crit_exit(crit);

    task_sleep(timeout_ticks);      /* cut short by touch_isr via task_wake() */

    crit = crit_enter();
    gpio_int_disable(PIN_IRQ);      /* disarm before any SPI can run */
    g_irq_waiter = -1;              /* timed out, or already woken */
    crit_exit(crit);
}

uint32_t touch_irq_waits(void) { return g_irq_waits; }
int      touch_irq_last_reg(void) { return g_irq_last_reg; }
int      touch_irq_seen(void)     { return g_irq_seen_by_isr; }
uint32_t touch_irq_armed_rb(void) { return g_irq_armed_rb; }
uint32_t touch_irq_fires(void) { return g_irq_fires; }
uint32_t touch_irq_wakes(void) { return g_irq_wakes; }

/* Maps a raw reading onto the panel, clamping rather than wrapping.
 *
 * MEASURED, not assumed. A single left-to-right drag swept raw X across
 * 486-3536 while raw Y moved only 2499-2862 — so raw X tracks the screen's
 * horizontal axis, and the drag ending at rx=3527 against a maximum of 3536
 * shows it increases left to right, needing no flip.
 *
 * This code originally had them the other way round on the reasoning that a
 * portrait display must transpose a landscape-calibrated panel. One drag
 * settled it; the reasoning had been wrong and no amount of staring at the
 * trail would have said which of swap-or-flip was at fault. */
static touch_log_t g_log[TOUCH_LOG_MAX];
static uint32_t    g_log_count;
static int         g_was_down;

uint32_t touch_log_count(void) { return g_log_count; }
void     touch_log_clear(void) { g_log_count = 0; g_was_down = 0; }

const touch_log_t *touch_log_entry(uint32_t i)
{
    return (i < g_log_count) ? &g_log[i] : 0;
}

static uint32_t map_axis(uint32_t raw, uint32_t lo, uint32_t hi, uint32_t span)
{
    if (raw <= lo) {
        return 0;
    }
    if (raw >= hi) {
        return span - 1u;
    }
    return ((raw - lo) * span) / (hi - lo);
}

int touch_read(touch_state_t *out)
{
    g_samples++;

    /* Read PENIRQ BEFORE asserting CS. The pin is only valid while the
     * controller is idle and powered down; a conversion in progress drives it
     * regardless of whether anything is touching. */
    int pen = (gpio_read(PIN_IRQ) == 0u);
    if (pen) {
        g_irq_low++;
    }

    /* Mask PENIRQ for the duration of the burst.
     *
     * The comment above says a conversion drives this pin regardless of whether
     * anything is touching, and that is exactly as true of the INTERRUPT as of
     * the level. Every read therefore manufactured its own edges: 3900 of them
     * across a handful of taps, none caused by a finger, each one indexing the
     * "an edge arrived with nobody waiting" latch and making the idle wait
     * return immediately forever. The driver was interrupting itself into a
     * busy loop.
     *
     * Masking here means the only edges that survive are the ones a finger
     * caused. An edge genuinely missed during the burst costs nothing: the burst
     * only runs while the task is awake and already sampling. */
    gpio_int_disable(PIN_IRQ);

    gpio_clear(PIN_CS);

    /* Throwaway. Powers the reference up and absorbs the inaccurate first
     * conversion so it cannot land on a channel whose value is load-bearing. */
    (void)convert(CMD_Z1);

    uint32_t z1 = convert(CMD_Z1);
    uint32_t z2 = convert(CMD_Z2);

    /* Pressure from the two Z channels. A light touch gives a small z1 and a
     * large z2; the difference is what separates contact from noise.
     *
     * A controller that is not answering returns 0 on every channel, and that
     * formula turns 0,0 into 4095 — maximum pressure. Silence would read as a
     * permanent hard press, which is how a dead bus first presented here. An
     * all-zero reading is therefore rejected outright: no touch produces a
     * z1 of exactly zero. */
    int answered = (z1 != 0u) || (z2 != 0u);
    uint32_t z = answered ? ((z1 + 4095u) - z2) : 0u;

    /* Four conversions per axis, kept individually. */
    uint32_t sx[4], sy[4];
    for (int i = 0; i < 4; i++) {
        sx[i] = convert(CMD_X);
    }
    for (int i = 0; i < 4; i++) {
        sy[i] = convert(CMD_Y);
    }
    (void)convert(CMD_IDLE);        /* power down, re-enable PENIRQ */
    uint32_t rx = (sx[1] + sx[2] + sx[3]) / 3u;   /* first discarded */
    uint32_t ry = (sy[1] + sy[2] + sy[3]) / 3u;

    gpio_set(PIN_CS);

    /* Deliberately does NOT re-arm. Arming happens in touch_irq_wait().
     *
     * Re-arming here looked right and was the second version of this bug. The
     * pad is still held LOW by a finger at this point, and enabling falling-edge
     * detection against an already-low pin latches an edge immediately — so
     * every read produced an interrupt, always while the task was awake and had
     * no waiter registered. The counters said 100 edges and 0 wakes, which is
     * exactly what a self-inflicted edge looks like from the outside.
     *
     * The interrupt is only ever useful while the task is idle and the pen is
     * up. Arming it anywhere else can only manufacture events. */

    for (int i = 0; i < 4; i++) {
        out->sx[i] = sx[i];
        out->sy[i] = sy[i];
    }
    if (z1 > g_max_z1) { g_max_z1 = z1; }
    if (z2 < g_min_z2) { g_min_z2 = z2; }
    if (answered && z > g_max_z) { g_max_z = z; }

    out->raw_x = rx;
    out->raw_y = ry;
    out->z     = z;
    out->z1    = z1;
    out->z2    = z2;
    /* PENIRQ decides, not pressure.
     *
     * The Z channels were measured across 150 samples including a firm held
     * press: z1 never exceeded 1 and the computed pressure peaked at 52 against
     * a threshold of 300. Whatever those channels are reporting on this board,
     * it is not contact. PENIRQ is a direct electrical indication from the
     * panel and needs no calibration at all.
     *
     * Pressure is still recorded, because a value that never moves is itself
     * evidence and hiding it would only make the next person repeat this. */
    /* PENIRQ says a finger is NEAR; pressure says it has actually landed.
     *
     * Both are now required, and the reason is not that PENIRQ was wrong.
     *
     * UM-NATOS-017 §4 chose PENIRQ over pressure with pressure working fine —
     * it measured a peak of 2065 against a threshold of 300 — on the grounds
     * that PENIRQ is a direct electrical signal needing no ADC, no threshold
     * and no calibration. That reasoning is still correct for the question it
     * answered.
     *
     * It answered the wrong question. PENIRQ reports whether a finger is
     * THERE; it says nothing about whether the position sample taken alongside
     * it is VALID. The pin asserts on approach and stays asserted through
     * release, and during both the panel is not resistively bridged, so the
     * position channels read their rail. Measured on the launcher, one tap:
     *
     *     z=7     raw_x=4095   -> x=0     approach, input floating
     *     z=2025  raw_x=1280   -> x=167   the actual finger
     *     z=20    raw_x=4095   -> x=0     release
     *
     * The rail maps to x=0, so every spurious sample votes for the leftmost
     * column — which is exactly what the launcher did once the axis inversion
     * stopped masking it. Two populations separated by a factor of a hundred,
     * and a threshold that has been sitting unused in this file the whole
     * time. */
    out->down  = pen && (z > Z_THRESHOLD);

    if (out->down) {
        g_events++;

        if (!g_first_set) {
            g_first_set = 1u;
            g_rx_first = rx;
            g_ry_first = ry;
        }
        if (rx < g_rx_min) { g_rx_min = rx; }
        if (rx > g_rx_max) { g_rx_max = rx; }
        if (ry < g_ry_min) { g_ry_min = ry; }
        if (ry > g_ry_max) { g_ry_max = ry; }

        out->x = map_axis(rx, g_cal_x_min, g_cal_x_max, DISP_W);
#if TOUCH_X_INVERTED
        out->x = (DISP_W - 1u) - out->x;
#endif
        out->y = map_axis(ry, g_cal_y_min, g_cal_y_max, DISP_H);

        /* Record the FIRST sample of each press. A held finger yields hundreds
         * of samples and only the moment of contact is a known screen
         * position; recording every sample would fill the log with the drift
         * of a finger settling. */
        if (!g_was_down && g_log_count < TOUCH_LOG_MAX) {
            touch_log_t *e = &g_log[g_log_count++];
            e->raw_x = rx;
            e->raw_y = ry;
            e->x     = out->x;
            e->y     = out->y;
            e->z     = out->z;
        }
        g_was_down = 1;
    } else {
        g_was_down = 0;
        out->x = 0;
        out->y = 0;
    }

    g_latest = *out;
    return out->down;
}

uint32_t touch_samples(void) { return g_samples; }
uint32_t touch_events(void)  { return g_events; }
