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

/* Re-runs the configuration and routing with the pins last given to
 * spi3_route(). See the definition for why this is interesting. */
void spi3_reattach(void);

/* One full-duplex burst, at most SPI3_XFER_MAX bytes.
 *
 * `rx` may be 0 if the reply is not wanted; `tx` may not, because the bus always
 * carries something outward and pretending otherwise would leave whatever the W
 * registers happened to hold on the wire. Returns 1, or 0 if the transaction did
 * not retire inside its bound. */
#define SPI3_XFER_MAX 64u
int spi3_xfer(const uint8_t *tx, uint8_t *rx, uint32_t n);

/* Receives n bytes while sending 0xFF: an SD read, without the packing loop
 * a general transfer needs. Same bound, same limit. */
int spi3_read(uint8_t *rx, uint32_t n);


/* ---- DMA reads (next_moves/12 step 10) --------------------------------------
 *
 * The same read with the CPU out of the data path. Word-wise register access
 * still costs ~0.36 us a byte on top of the wire's 0.4 us at 20 MHz, and all of
 * that is APB accesses; the engine has none.
 *
 * spi3_dma_init() must be called after spi3_init(), which resets the
 * peripheral. A whole 512-byte block fits one descriptor.
 *
 * spi3_read_dma() returns 0 WITHOUT having moved anything if it cannot honour
 * the request -- the destination must be 4-byte aligned, `n` a multiple of 4,
 * and the buffer in DRAM, because the engine writes words and cannot reach
 * IRAM. Callers fall back to spi3_read(); the refusals are counted so a caller
 * whose buffers are never aligned shows up as a number rather than as the old
 * speed with no explanation.
 *
 * A timeout disables DMA for the rest of the run, as display.c does: an engine
 * that missed one completion has not earned the next block. */
/* 520, not 512: a block read asks for its remaining data AND the two CRC bytes
 * in one word-multiple transfer, which is up to 516 bytes when the data token
 * landed at the end of a poll batch. At 512 those transfers were REFUSED -- by
 * the one guard that returns without counting anything -- so the block failed,
 * the retry paid for it, and ~12% of blocks cost double. That is the 200 us of
 * unexplained overhead in the data phase, and the read that died at 3.1 MB. */
#define SPI3_DMA_MAX 520u
void spi3_dma_init(void);
int  spi3_read_dma(uint8_t *rx, uint32_t n);

/* Full duplex by DMA. `tx` of 0 sends 0xFF, which is what a read is; `rx` of 0
 * discards. Buffers the engine cannot use directly -- unaligned, not a multiple
 * of a word, or outside DRAM -- are staged through an internal copy, so callers
 * need not care, but a 512-byte copy costs ~12 us against the 207 us the
 * transfer itself takes: the block data path hands over aligned buffers and
 * pays nothing. */
int  spi3_xfer_dma(const uint8_t *tx, uint8_t *rx, uint32_t n);

/* For the probe only: a receive with the alignment and length checks skipped,
 * so what the engine tolerates can be measured instead of assumed. */
int  spi3_force_dma(uint8_t *rx, uint32_t n);

/* W-register transfers since boot, and the number of times DMA gave itself up
 * because that count moved after it was armed. The second must stay 0 in normal
 * operation: it means a diagnostic poisoned the engine and reads fell back to
 * the slow path for the rest of the run. */
uint32_t spi3_wreg_xfers(void);
uint32_t spi3_dma_poisoned(void);

/* Why the last DMA read gave up, and what the peripheral said at that instant.
 * stage: 0 none, 2 the shifter never finished, 3 the channel never signalled
 * its descriptor retired, 4 the channel reported a descriptor error.
 * `spins` is how long the outbound channel took to start (0 is normal, 1000 is
 * "it never did"), `len` the request's length. */
uint32_t spi3_dma_stage(void);          /* 3 stalled, 5 short receive */
/* Transfers that completed correctly while the channel raised IN_ERR_EOF, which
 * in master mode is what a descriptor marked eof=1 always does. Expect this to
 * equal the transfer count; a DIVERGENCE would be the interesting event. */
uint32_t spi3_dma_err_eofs(void);
/* The receive descriptor as the engine left it: size in 11:0, the length it
 * says it delivered in 23:12, owner at 31. The one place that distinguishes
 * "the data is there" from "the transfer ended". */
uint32_t spi3_dma_flags(void);
/* Transfers whose byte count had not caught up when the descriptor said the
 * engine was finished with it, and how many spins the last one needed. */
/* The first failure since boot, which is the only one that explains anything:
 * its stage, the length asked for, the descriptor, the interrupt bits, and how
 * many transfers had already succeeded. */
uint32_t spi3_dma_first_stage(void);
uint32_t spi3_dma_first_len(void);
uint32_t spi3_dma_first_flags(void);
uint32_t spi3_dma_first_int(void);
uint32_t spi3_dma_first_at(void);

uint32_t spi3_dma_late(void);
uint32_t spi3_dma_settle(void);
uint32_t spi3_dma_int(void);
uint32_t spi3_dma_status(void);
uint32_t spi3_dma_spins(void);
uint32_t spi3_dma_len(void);

uint32_t spi3_dma_transfers(void);
uint32_t spi3_dma_timeouts(void);
uint32_t spi3_dma_refused(void);
int      spi3_dma_enabled(void);
void     spi3_dma_force_fifo(int on);   /* for measuring the guard's absence */

/* SCK = APB (80 MHz) / div, div 2..64, 50% duty. The default after
 * spi3_init() is div 40, 2 MHz -- the radio's rate. The SD card uses this to
 * go faster once it has identified itself (next_moves/11 step 3). */
void spi3_set_div(uint32_t div);

/* ---- bring-up diagnostics ---------------------------------------------- */

/* Tie MISO to a constant through the matrix and check what comes back. `level`
 * is 0 or 1. Returns 1 if every received byte matched. Drives no pin. */
void spi3_probe_speed(void);    /* where a transfer's time goes */

/* Attempts a DMA read of each of several lengths with CS high -- the card
 * ignores the clock, so this asks the peripheral and the channel alone whether
 * a length is acceptable, and prints what the channel said. */
void spi3_probe_dma(void);

/* Times back-to-back DMA reads, which is the only regime in which they are
 * correct. Separate from the probe above, whose mixing rows poison the engine
 * for everything after them. */
void spi3_time_dma(void);

/* Which transfer lengths the receive channel will actually retire. Cold, DMA
 * only, no card traffic: CS stays high throughout. */
void spi3_probe_dma_lengths(void);

int spi3_selftest_const(int level);

/* Route MISO from the same pin MOSI drives, transfer a pattern, and check it
 * returns unchanged. This is the first test that proves data actually leaves the
 * chip and comes back, and the only one that needs a pin to be safe to drive. */
int spi3_selftest_loopback(uint8_t pin);

/* Transfers attempted and transactions that did not retire in time. */
uint32_t spi3_transfers(void);
uint32_t spi3_timeouts(void);

#endif /* NATOS_SPI3_H */
