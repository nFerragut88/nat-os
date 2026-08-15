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
 * the supply variation that single-ended mode is sensitive to. */
#define CMD_Y   0x90u
#define CMD_X   0xD0u
#define CMD_Z1  0xB0u
#define CMD_Z2  0xC0u

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

/* Maps a raw reading onto the panel, clamping rather than wrapping. The axes
 * are swapped: the controller's X runs along the panel's long edge, because the
 * touch layer is oriented to the glass and the display is driven in portrait.
 * Reading raw values before trusting the mapping is what showed which way round
 * they went. */
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

    gpio_clear(PIN_CS);

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

    /* Two samples per axis. The panel is noisy and a single reading regularly
     * lands far from the others; averaging two is enough to stop a cursor
     * jittering without pretending this is a filtered signal. */
    uint32_t rx = (convert(CMD_X) + convert(CMD_X)) / 2u;
    uint32_t ry = (convert(CMD_Y) + convert(CMD_Y)) / 2u;

    gpio_set(PIN_CS);

    out->raw_x = rx;
    out->raw_y = ry;
    out->z     = z;
    out->z1    = z1;
    out->z2    = z2;
    out->down  = answered && (z > Z_THRESHOLD);

    if (out->down) {
        g_events++;
        out->x = map_axis(ry, CAL_Y_MIN, CAL_Y_MAX, DISP_W);
        out->y = map_axis(rx, CAL_X_MIN, CAL_X_MAX, DISP_H);
    } else {
        out->x = 0;
        out->y = 0;
    }

    return out->down;
}

uint32_t touch_samples(void) { return g_samples; }
uint32_t touch_events(void)  { return g_events; }
