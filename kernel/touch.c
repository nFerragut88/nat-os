/* cyd-os — XPT2046 touchscreen. See touch.h for the pin map and rationale. */

#include "touch.h"
#include "display.h"
#include "gpio.h"
#include "xtensa.h"

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

/* Vendor calibration for this board. Kept as the extremes of the usable ADC
 * range rather than as a scale factor, so the mapping stays readable and a
 * re-calibration is two numbers per axis. */
#define CAL_X_MIN  185u
#define CAL_X_MAX  3700u
#define CAL_Y_MIN  280u
#define CAL_Y_MAX  3850u

#define Z_THRESHOLD 300u

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
    out->down  = pen;

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

        out->x = map_axis(rx, CAL_X_MIN, CAL_X_MAX, DISP_W);
        out->y = map_axis(ry, CAL_Y_MIN, CAL_Y_MAX, DISP_H);
    } else {
        out->x = 0;
        out->y = 0;
    }

    return out->down;
}

uint32_t touch_samples(void) { return g_samples; }
uint32_t touch_events(void)  { return g_events; }
