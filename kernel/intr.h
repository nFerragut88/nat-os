/* nat-os — the interrupt matrix.
 *
 * Until this file existed, every peripheral in this kernel was polled. The
 * Level-3 vector served exactly one source — CCOMPARE1, which is *internal* to
 * the Xtensa core and reaches the CPU without passing through anything. No
 * peripheral interrupt had ever been routed, which is invisible while the only
 * peripherals are a display written on demand and a touch panel sampled at
 * 100 Hz, and is a hard wall for anything that must be serviced on the
 * hardware's schedule rather than on ours.
 *
 * On the ESP32 a peripheral cannot reach the CPU by itself. Each of ~70
 * peripheral interrupt SOURCES is routed, through a DPORT map register, onto
 * one of 32 CPU interrupt LINES. Those are different numbering spaces and
 * confusing them is the classic failure: writing the source number into
 * INTENABLE enables an unrelated line, and the peripheral stays silent while
 * every register involved reads back exactly as intended.
 *
 *   source  (0..69)  what raised it     — GPIO, I2S0, UART1, TG0_T0 ...
 *   line    (0..31)  what the CPU sees  — fixed priority level, fixed type
 *
 * A line's priority level and edge/level type are properties of the silicon,
 * not choices. Line 23 is used here because it is level-triggered at level 3,
 * so it shares the existing Level-3 vector and its proven context save rather
 * than needing a second one.
 */

#ifndef NATOS_INTR_H
#define NATOS_INTR_H

#include <stdint.h>

/* Peripheral interrupt sources, by their index in the silicon's table. The map
 * register address is derived from the index, so these must be the real ones —
 * a wrong index silently programs a different peripheral's routing. */
#define INTR_SRC_GPIO_PRO   22u

/* The WiFi MAC is source 0 -- the first entry in the silicon's table, which is
 * why its map register is the first one. Verified only by that structural fact
 * so far; nothing has been observed to arrive on it yet. */
#define INTR_SRC_WIFI_MAC    0u

/* CPU interrupt lines this kernel uses. Both are level 3. */
#define INTR_LINE_TIMER1    15u     /* internal CCOMPARE1; not from the matrix */
#define INTR_LINE_GPIO      23u

/* Line 27 is level 3 and level-triggered, which is what a peripheral that
 * holds its interrupt asserted needs. Lines 22 and 29 are also level 3 but are
 * edge and software respectively. */
#define INTR_LINE_WIFI_MAC  27u

/* [step 191] The line the BLOB asks for: ESP-IDF's ETS_WMAC_INUM. It is
 * priority 1, and nat-os has no priority-1 handler, so osi_impl_set_intr()
 * remaps it onto INTR_LINE_WIFI_MAC above. Named rather than written as 0 in
 * three places. */
#define INTR_LINE_WIFI_MAC_BLOB  0u

typedef void (*intr_handler_fn)(void);

/* Routes a peripheral source onto a CPU line, installs its handler and enables
 * the line. The handler runs at level 3 with interrupts masked, on the
 * interrupted task's stack.
 *
 * A handler for a LEVEL-triggered line must clear the condition at the
 * peripheral before returning. Nothing else can: the line stays asserted for as
 * long as the peripheral says so, and returning without clearing it re-enters
 * the handler immediately and forever. That failure mode is a hang with no
 * output, so intr_dispatch() defends against it — see intr.c. */
void intr_route(uint32_t source, uint32_t line, intr_handler_fn fn);

/* Installs a handler for a line without touching the matrix, for sources that
 * do not come through it. */
void intr_install(uint32_t line, intr_handler_fn fn);

/* Called from _handler_level3 in vectors.S. Not static — assembly names it. */
void intr_dispatch(void);

/* Observability. Counters rather than prints: an interrupt handler that writes
 * to the UART changes the timing of the thing it is reporting on, and this
 * project has already lost three separate measurements to being printed into a
 * stream nobody was capturing. */
uint32_t intr_count(uint32_t line);      /* times this line has been serviced */
uint32_t intr_spurious(void);            /* pending, enabled, and unhandled */
uint32_t intr_disabled_mask(void);       /* lines shut off by the defence below */

/* Reads back the registers that make routing work, for when it does not. */
void intr_dump(void);

/* Injects an edge for each candidate INT_ENA bit and reports which one this
 * CPU actually sees. Leaves the working bit installed. */
void intr_selftest(void);

/* Injects one edge into the live configuration. */
void intr_poke(void);

#endif /* NATOS_INTR_H */
