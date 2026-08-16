/* nat-os — XPT2046 resistive touchscreen.
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

#ifndef NATOS_TOUCH_H
#define NATOS_TOUCH_H

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

/* Most recent reading, without touching the bus. */
void touch_latest(touch_state_t *out);

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

/* Raw-axis spans while touched, and the first point of the first touch. */
uint32_t touch_rx_min(void);
uint32_t touch_rx_max(void);
uint32_t touch_ry_min(void);
uint32_t touch_ry_max(void);
uint32_t touch_rx_first(void);
uint32_t touch_ry_first(void);

uint32_t touch_samples(void);
uint32_t touch_events(void);

/* ---- press log ----------------------------------------------------------
 *
 * The last few presses, kept in memory rather than printed.
 *
 * Printing them was the obvious thing and it was wrong: the lines appear while
 * the user's finger is on the glass, which is exactly when nobody is running a
 * capture. A value that has already scrolled past is not a measurement. Held
 * here, it can be read whenever somebody asks. */
/* Calibration, as runtime values. Set by the calibration routine and restored
 * from the persistent record at boot. touch_set_calibration() refuses a
 * degenerate range: a bad calibration makes the panel unusable, which would
 * make it impossible to run the calibration again. */
void touch_set_calibration(uint32_t xmin, uint32_t xmax,
                           uint32_t ymin, uint32_t ymax);
void touch_get_calibration(uint32_t *xmin, uint32_t *xmax,
                           uint32_t *ymin, uint32_t *ymax);

#define TOUCH_LOG_MAX 8u

typedef struct {
    uint32_t raw_x, raw_y;
    uint32_t x, y;
    uint32_t z;
} touch_log_t;

/* PENIRQ, wired as this kernel's first peripheral interrupt. See touch.c. */
void     touch_irq_init(void);
void     touch_irq_wait(uint32_t timeout_ticks);
uint32_t touch_irq_fires(void);     /* falling edges seen by the handler */
uint32_t touch_irq_waits(void);     /* times the task actually waited */
int      touch_irq_last_reg(void);  /* task id the waiter registered as */
int      touch_irq_seen(void);      /* waiter value the handler read back */
uint32_t touch_irq_armed_rb(void);  /* PIN_REG as it read back after arming */
uint32_t touch_irq_wakes(void);     /* those that released a waiting task */

uint32_t           touch_log_count(void);
const touch_log_t *touch_log_entry(uint32_t i);
void               touch_log_clear(void);

#endif /* NATOS_TOUCH_H */
