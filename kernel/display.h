/* nat-os — ILI9341 display driver for the CYD 2.8".
 *
 * 240x320, 16-bit colour, driven over SPI. Pin assignment is the board's, taken
 * from the vendor project's TFT_eSPI setup:
 *
 *     SCLK 14   MOSI 13   MISO 12   CS 15   DC 2   BL 21 (active high)
 *     RST is not wired to a GPIO — it follows the ESP32's own reset line, so
 *     the panel can only be reset in software (0x01 SWRESET).
 *
 * NO FRAMEBUFFER. The ILI9341 holds the image in its own GRAM and drives the
 * glass from it, so the host never needs a second copy. UM-NATOS-010 §7.2
 * measured what one would cost: 153,600 B, 92% of the heap. Everything here
 * renders through a small line buffer instead — 480 B for a full-width span.
 *
 * SPI is bit-banged rather than driven by the SPI2 peripheral. That is a
 * deliberate first step: hardware SPI needs DPORT clock-enable bits and GPIO
 * matrix routing, and every mistake in either produces the same symptom as a
 * wiring fault or a bad init sequence — a black screen. Bit-banging needs only
 * the GPIO registers, so a failure here can only be the panel, the pins, or the
 * commands. A full-screen fill costs a measured 387 ms — 2.6 screens per
 * second, ample for a status display and far too slow for animation — and the
 * peripheral can replace it later without anything above this file changing.
 */

#ifndef NATOS_DISPLAY_H
#define NATOS_DISPLAY_H

#include <stdint.h>

/* 1 = SPI2 peripheral, 0 = bit-banged GPIO. The bit-banged path is retained
 * because a wrong DPORT clock bit or IOMUX selection fails exactly like a
 * wiring fault: a black screen with no diagnostic. */
#define DISPLAY_USE_SPI2 1

#define DISP_W 240u
#define DISP_H 320u

/* RGB565, the panel's native format. */
#define RGB(r, g, b) ((uint16_t)((((r) & 0xF8u) << 8) | (((g) & 0xFCu) << 3) | ((b) >> 3)))

#define COLOR_BLACK   0x0000u
#define COLOR_WHITE   0xFFFFu
#define COLOR_RED     0xF800u
#define COLOR_GREEN   0x07E0u
#define COLOR_BLUE    0x001Fu
#define COLOR_YELLOW  0xFFE0u
#define COLOR_CYAN    0x07FFu
#define COLOR_MAGENTA 0xF81Fu
#define COLOR_GREY    0x8410u

/* Brings up the panel: pins, software reset, init sequence, backlight on.
 * Returns 0 on success. Must be called before anything else here. */
int  display_init(void);

void display_backlight(int on);

/* Hold the draw lock across a batch of primitives.
 *
 * Every drawing call already locks, and the mutex is recursive, so this is only
 * about FREQUENCY. A caller issuing hundreds of small primitives pays a full
 * scheduling round-trip per contended acquisition — a raycaster taking the lock
 * once per column spent 25 seconds per frame on 37 ms of work. Holding it for
 * the batch turns that into one acquisition. UM-NATOS-014 §5.2. */
/* Non-blocking. Returns non-zero if the lock was taken; the caller must then
 * display_unlock(). For drawing that is better skipped than waited for — see
 * the note in display.c. */
int  display_try_lock(void);

void display_lock(void);
void display_unlock(void);

/* ---- panic mode --------------------------------------------------------
 *
 * Suspends the draw lock, DMA, and the unbounded wait for the SPI controller,
 * so the panic handler can put a fault on the panel without depending on a
 * scheduler that has stopped or a transfer that may never retire. One-way:
 * nothing turns it off, because nothing after a panic needs it off.
 *
 * display_ready() reports whether display_init() ever completed. A panic
 * before the panel exists must draw nothing rather than write to an
 * unconfigured controller. */
/* Lock timing.
 *
 * "Blocked" is time from asking for the lock to holding it. It is NOT time the
 * lock was busy: a task that cannot have it is descheduled, so the figure also
 * covers being selected again afterwards, and measurement showed that is the
 * larger part — 63 ms blocked against a 24 ms hold. The name matters, because
 * "wait" would imply the panel was the bottleneck and send the next person to
 * shorten holds, which was tried and changed nothing. */
uint32_t display_lock_blocked_ms(void);
uint32_t display_lock_hold_ms(void);
uint32_t display_lock_takes(void);
uint32_t display_lock_contentions(void);

/* Milliseconds a specific task has spent blocked on the draw lock. The
 * aggregate says the system is blocked; this says which task, which is what
 * decided the fix — both the renderer and the applications were blocked most of
 * the time, on a lock that was free 91% of it. */
uint32_t display_lock_blocked_of(int task_id);

void display_enter_panic_mode(void);
int  display_ready(void);

/* Fills the whole panel. */
void display_clear(uint16_t colour);

/* Axis-aligned rectangle, clipped to the panel. */
void display_fill_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint16_t colour);

/* 6x8 text, one byte per glyph column plus a blank spacer. `scale` multiplies
 * both axes. Characters outside 32..126 print as a blank. */
void display_text(uint32_t x, uint32_t y, const char *s, uint16_t fg, uint16_t bg,
                  uint32_t scale);

/* Copies a caller-supplied RGB565 image to the panel. `src_stride` is the
 * source row pitch in PIXELS, so a clipped blit can walk the original rows
 * without the caller having to repack. Clipped to the panel; callers wanting a
 * tighter boundary must clip before calling. */
void display_blit(uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                  const uint16_t *src, uint32_t src_stride);

/* Bytes pushed to the panel since init — the cheapest measure of whether the
 * driver is doing anything at all when nothing appears on screen. */
uint32_t display_bytes_written(void);

/* CCOUNT cycles taken by the full-screen clear during init — the driver's
 * throughput, measured on the real panel rather than estimated. */
uint32_t display_fill_cycles(void);

/* SPI2 registers as read back after configuration, for confirming the clock
 * divisor and the peripheral clock gate actually took. */
uint32_t display_spi_clock_reg(void);
uint32_t display_dport_reg(void);

/* DMA transfers completed, and transfers abandoned on timeout. A non-zero
 * timeout count means DMA disabled itself and the FIFO path took over. */
/* One full-screen fill, timed at runtime. See display.c. */
uint32_t display_fill_bench(void);

uint32_t display_dma_transfers(void);
uint32_t display_dma_timeouts(void);

/* Task id currently holding the draw lock, for deadlock diagnosis. */
int display_owner(void);

#endif /* NATOS_DISPLAY_H */
