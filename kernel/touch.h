/* cyd-os — XPT2046 resistive touchscreen.
 *
 * Pins are the board's, from the vendor driver, and are deliberately NOT the
 * display's:
 *
 *     CLK 25   MOSI 32   MISO 39   CS 33   IRQ 36
 *
 * A separate bus is why touch can be polled without disturbing a half-finished
 * pixel stream. The display driver leaves CS asserted between a window command
 * and its pixel data, so sharing a bus would mean either interleaving into that
 * window or serialising every touch read behind a full-screen fill.
 *
 * GPIO 36 and 39 are input-only pins on this part, which makes MISO and IRQ
 * impossible to drive by accident.
 *
 * Bit-banged, like the display, and for the same reason: it needs only GPIO
 * registers, so a failure can only be the panel, the pins, or the protocol.
 * The XPT2046 tops out around 2 MHz, well under what a GPIO loop produces.
 *
 * Raw readings are exposed alongside mapped coordinates. Calibration is a
 * per-board property and a mapped coordinate that looks wrong is
 * indistinguishable from a controller that is not answering — the raw value
 * tells the two apart.
 */

#ifndef CYDOS_TOUCH_H
#define CYDOS_TOUCH_H

#include <stdint.h>

typedef struct {
    int      down;          /* pressure exceeded the threshold  */
    uint32_t x, y;          /* panel coordinates, valid if down */
    uint32_t raw_x, raw_y;  /* 12-bit ADC, always valid         */
    uint32_t z;             /* pressure                         */
    uint32_t z1, z2;        /* raw pressure channels — a controller that is
                             * not answering returns 0 on both, which is what
                             * distinguishes a dead bus from an untouched
                             * panel. Both look like "no events". */
    /* Individual conversions, not the average. Averaging a good reading with a
     * garbage one yields a plausible-looking wrong answer, so the spread has to
     * be visible to tell "noisy" from "not tracking position at all". */
    uint32_t sx[4], sy[4];
} touch_state_t;

void touch_init(void);

/* Samples the controller. Returns non-zero if a touch is present. Cheap enough
 * to call from a task loop; takes roughly 100 us. */
int touch_read(touch_state_t *out);

/* Total samples taken and touches seen, so "the driver is not running" is
 * distinguishable from "nobody has touched it". */
/* Extremes of the pressure channels since boot. A finger should drive z1 well
 * above zero and z2 well below its idle rail; if neither moves, the panel is
 * not reaching the controller regardless of what the coordinates say. */
uint32_t touch_max_z1(void);
uint32_t touch_min_z2(void);
uint32_t touch_max_z(void);

/* Samples in which PENIRQ was asserted. */
uint32_t touch_irq_lows(void);

uint32_t touch_samples(void);
uint32_t touch_events(void);

#endif /* CYDOS_TOUCH_H */
