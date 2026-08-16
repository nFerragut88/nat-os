/* nat-os — the shell, on the panel.
 *
 * The shell has always been a front end that happened to read a serial port.
 * This gives it a second front end and changes nothing behind it: a command
 * typed on the glass goes through `shell_run_line()` into the same `execute()`
 * that a typed line reaches, with the same parsing and the same output.
 *
 * That is the whole design claim, and it is worth stating because the tempting
 * shortcut — a small on-screen menu of the popular commands — would have been
 * easier and would have created a second, quietly diverging command set.
 *
 * ---- what this actually buys ---------------------------------------------
 *
 * The device stops needing a computer attached to be inspected. `mem`, `ps`,
 * `stacks`, `adc`, `i2c` and `intr` were all reachable only from a host over
 * USB; the launcher exists to avoid needing one, and the shell was the largest
 * remaining reason to plug it in.
 *
 * ---- and what it costs ----------------------------------------------------
 *
 * Typing on a multi-tap keypad. `stacks` is fifteen taps. This is not a good
 * way to type and is not trying to be — it is a way to read the machine's own
 * state without a host, and the commands worth typing on it are short.
 */

#ifndef NATOS_TERM_H
#define NATOS_TERM_H

#include <stdint.h>

void term_open(void);                                   /* entering the app  */
void term_frame(void);                                  /* per-frame redraw  */
void term_touch(uint32_t x, uint32_t y, int down);      /* routed by kmain   */

uint32_t term_commands(void);   /* lines run from the panel, for telemetry */

#endif /* NATOS_TERM_H */
