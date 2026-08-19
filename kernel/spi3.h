/* nat-os — SPI3 (VSPI) master, full duplex.
 *
 * ---- why a second SPI at all -------------------------------------------
 *
 * display.c owns SPI2 and is write-only: it never reads a byte back, because
 * the panel's MISO is dead on this module (UM-NATOS-030 §7). That driver is
 * shaped entirely around streaming pixels out, and it is the wrong shape for a
 * peripheral you have to hold a conversation with.
 *
 * A radio is such a peripheral. Every SX126x command is "send these bytes and
 * read these back in the same transaction", which is full duplex, which SPI2's
 * driver has no path for. SPI3 is completely unclaimed, so it gets its own
 * driver rather than a mode flag bolted onto a write-only one.
 *
 * ---- the pin situation, stated plainly ---------------------------------
 *
 * SPI3's IO_MUX pins are 18, 19, 23 and 5, which on this board are the microSD
 * card's. "SPI3 is free" is true of the PERIPHERAL and false of its default
 * pins, and that distinction is worth stating before someone wires a radio to
 * 18 and wonders why the card stopped working.
 *
 * So this routes through the GPIO matrix instead, to whatever pins are actually
 * free. The matrix costs a ceiling of about 40 MHz, which would matter to the
 * display and does not matter here: an SX1262 is specified to 16 MHz and is
 * normally driven at 2-8.
 *
 * ---- what is verified before any radio exists --------------------------
 *
 * Nothing is wired yet, so the bring-up has to be answerable without hardware.
 * The GPIO matrix can tie a peripheral input to a constant, which makes two
 * tests possible that drive no pin at all:
 *
 *   MISO tied to constant 1 -> every byte read back must be 0xFF
 *   MISO tied to constant 0 -> every byte read back must be 0x00
 *
 * That is the "one counter that must be non-zero beside one that must be zero"
 * pattern the project already leans on: a driver that returns 0xFF for both is
 * not reading anything, and a stuck-at-zero result is exactly how a dead input
 * path has presented twice before in this kernel.
 *
 * Passing those proves the clock gate, the register block, the W registers and
 * the capture path. It does NOT prove a byte ever left the chip. The pin
 * loopback below is what proves that, and it needs one free GPIO.
 */

#ifndef NATOS_SPI3_H
#define NATOS_SPI3_H

#include <stdint.h>

/* Brings up the peripheral at ~2 MHz, full duplex, with CS left under manual
 * GPIO control. Routes nothing: call spi3_route() once the pins are known. */
void spi3_init(void);

/* Points the peripheral's signals at real pins through the GPIO matrix. Pass
 * SPI3_PIN_NONE for anything not wired yet. CS is deliberately NOT routed --
 * this driver drives it as a plain GPIO, the same choice display.c made, because
 * a radio transaction holds CS low across several transfers. */
#define SPI3_PIN_NONE 0xFFu
void spi3_route(uint8_t sck, uint8_t mosi, uint8_t miso);

/* One full-duplex burst, at most SPI3_XFER_MAX bytes.
 *
 * `rx` may be 0 if the reply is not wanted; `tx` may not, because the bus always
 * carries something outward and pretending otherwise would leave whatever the W
 * registers happened to hold on the wire. Returns 1, or 0 if the transaction did
 * not retire inside its bound. */
#define SPI3_XFER_MAX 64u
int spi3_xfer(const uint8_t *tx, uint8_t *rx, uint32_t n);

/* ---- bring-up diagnostics ---------------------------------------------- */

/* Tie MISO to a constant through the matrix and check what comes back. `level`
 * is 0 or 1. Returns 1 if every received byte matched. Drives no pin. */
int spi3_selftest_const(int level);

/* Route MISO from the same pin MOSI drives, transfer a pattern, and check it
 * returns unchanged. This is the first test that proves data actually leaves the
 * chip and comes back, and the only one that needs a pin to be safe to drive. */
int spi3_selftest_loopback(uint8_t pin);

/* Transfers attempted and transactions that did not retire in time. */
uint32_t spi3_transfers(void);
uint32_t spi3_timeouts(void);

#endif /* NATOS_SPI3_H */
