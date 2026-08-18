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

/* ---- keypresses, for the `keys` device ---------------------------------
 *
 * The keypad decoded taps into a command line for itself and published nothing,
 * which is why an application could not read a key. A character is queued when
 * it SETTLES -- multi-tap makes the letter under your finger provisional until
 * the cycle ends, and publishing sooner would deliver every intermediate letter
 * of a cycle nobody typed.
 *
 * term_key_pop() returns 1 and the character, or 0 when the queue is empty.
 * An empty queue is not an error; it is the normal state. */
int      term_key_pop(uint32_t *out);
uint32_t term_keys_pending(void);
uint32_t term_keys_dropped(void);

/* Every character ever queued, never reset. `pending` and `dropped` cannot tell
 * "nothing was typed" apart from "something was typed and a program consumed
 * it"; a total can. */
uint32_t term_keys_queued(void);

/* Inject a character as though it had been typed. The keypad needs a person, a
 * particular view, and correct multi-tap timing, which is three ways for a test
 * of something DOWNSTREAM to fail for unrelated reasons. */
void     term_key_inject(uint32_t ch);

#endif /* NATOS_TERM_H */
