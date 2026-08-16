/* nat-os — bit-banged I2C master.
 *
 * The ADC turned one pin into one sensor. This turns two pins into most
 * sensors: temperature, humidity, pressure, accelerometers, magnetometers,
 * real-time clocks and displays are nearly all I2C, and they share a bus, so
 * the pin cost does not grow with the sensor count.
 *
 * ---- why bit-banged ------------------------------------------------------
 *
 * This part has two hardware I2C controllers and neither is used. Two reasons,
 * and the second is the real one:
 *
 *   - the display and touch drivers are both bit-banged already, so the
 *     technique is proven here and the failure modes are understood
 *   - **clock stretching is in the I2C specification.** A slave may hold SCL
 *     low to buy time, and every master must tolerate it. That means a bus
 *     that is legal to stall is also a bus where a preempted master is legal —
 *     so a task switch in the middle of a transaction stretches the clock
 *     rather than corrupting it.
 *
 * That last property is what makes bit-banging safe under a preemptive
 * scheduler, and it is exactly what bit-banged I2S would not have. It is a
 * property of the protocol, not of this code being careful.
 *
 * ---- pins ----------------------------------------------------------------
 *
 * SDA on GPIO 22, SCL on GPIO 27 — the two general-purpose pins this board
 * brings out to headers that are not already claimed by the display, the touch
 * controller, the SD slot, the light sensor or the speaker.
 *
 * ---- pull-ups ------------------------------------------------------------
 *
 * I2C is open-drain: devices only ever pull DOWN, and something must pull up.
 * The internal pull-ups are used, which is a compromise worth stating — they
 * are roughly 45 kOhm against the 2.2-10 kOhm a real bus wants, so rise times
 * are slow and the clock here is correspondingly slow. A module with its own
 * pull-ups fitted (most breakouts have them) will be faster and happier.
 *
 * A bus with NO pull-up does not fail cleanly: SDA floats, reads as whatever,
 * and a scan can report every address as present. i2c_selftest() exists to
 * distinguish that from a working bus before any of it is believed.
 */

#ifndef NATOS_I2C_H
#define NATOS_I2C_H

#include <stdint.h>

#define I2C_PIN_SDA  22u
#define I2C_PIN_SCL  27u

void i2c_init(void);

/* Returns 0 on success, negative on NAK or timeout. A NAK from a device that
 * is not there and a timeout from a bus that is stuck are different failures
 * and are reported as such. */
#define I2C_OK        0
#define I2C_ENAK     -1     /* addressed, nobody answered */
#define I2C_ETIMEOUT -2     /* SCL never rose: stretched too long, or shorted */

int i2c_write(uint8_t addr7, const uint8_t *data, uint32_t len);
int i2c_read(uint8_t addr7, uint8_t *buf, uint32_t len);

/* Write then read without releasing the bus, which is how nearly every I2C
 * sensor is addressed: write the register number, repeated START, read back. */
int i2c_write_read(uint8_t addr7, const uint8_t *tx, uint32_t txlen,
                   uint8_t *rx, uint32_t rxlen);

/* Addresses a device and sends nothing. The cheapest question that has a real
 * answer: is anything at this address. */
int i2c_probe(uint8_t addr7);

void i2c_scan(void);        /* probe 0x08..0x77 and report */

/* Drives each line low and releases it, checking the level read back both
 * times. Proves the drive path and the pull-up per line, with no device
 * attached and nothing to believe on faith. Run this before trusting a scan. */
void i2c_selftest(void);

#endif /* NATOS_I2C_H */
