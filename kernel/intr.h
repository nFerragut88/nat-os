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

/* How many there are: ESP32 has 69 peripheral sources, numbered 0..68.
 *
 * intr_route() needs this because DPORT_PRO_MAP(src) is unbounded arithmetic --
 * a source outside the table does not fail, it writes into whatever register
 * lies that far from the array. See the bound check in intr_route(). */
#define INTR_SRC_COUNT      69u

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

/* The CPU lines that are LEVEL 1 on the ESP32.
 *
 *   0..10   level 1   (6 is internal timer0, 7 internal software, 10 edge)
 *   12, 13  level 1
 *   17, 18  level 1
 *
 * Everything else is another level and must not be enabled by a caller that
 * expects this dispatcher to service it: 11, 15, 22, 23, 27 and 29 are level 3,
 * 19..21 level 2, 24, 25, 28, 30 level 4, 16, 26, 31 level 5, and 14 is the
 * NMI. A line enabled at the wrong level is not a lost interrupt -- it is a
 * panic on the first arrival, which is how the Bluetooth controller's request
 * for lines 5, 7 and 8 was found. */
#define INTR_LEVEL1_MASK    0x000637FFu

/* Called from _handler_user in vectors.S when EXCCAUSE says 4 — a level-1
 * interrupt rather than a fault. Runs at INTLEVEL 3, non-reentrant, on the
 * interrupted context's stack; see the long note above the handler. */
void intr_dispatch_level1(void);

/* Level-1 counters, separate from the level-3 ones because the two paths are
 * separate and a single total would hide which vector is actually firing. */
uint32_t intr_level1_count(void);     /* level-1 interrupts serviced */
uint32_t intr_level1_spurious(void);  /* pending, enabled, unhandled */

/* Asserts CPU line 7 (the level-1 software interrupt) and reports whether the
 * handler ran. Proves the mechanism without needing a peripheral or the radio:
 * if this fails, nothing built on level-1 dispatch is worth debugging yet. */
int  intr_selftest_level1(void);

/* Observability. Counters rather than prints: an interrupt handler that writes
 * to the UART changes the timing of the thing it is reporting on, and this
 * project has already lost three separate measurements to being printed into a
 * stream nobody was capturing. */
uint32_t intr_count(uint32_t line);      /* times this line has been serviced */
uint32_t intr_spurious(void);            /* pending, enabled, and unhandled */
uint32_t intr_disabled_mask(void);       /* lines shut off by the defence below */

/* Calls to intr_route() refused because the source was outside 0..68. Any
 * non-zero value here is a caller bug that USED to be a stray DPORT write. */
uint32_t intr_bad_sources(void);

/* Reads back the registers that make routing work, for when it does not. */
void intr_dump(void);

/* Injects an edge for each candidate INT_ENA bit and reports which one this
 * CPU actually sees. Leaves the working bit installed. */
void intr_selftest(void);

/* Injects one edge into the live configuration. */
void intr_poke(void);

#endif /* NATOS_INTR_H */
