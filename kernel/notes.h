/* nat-os — a note pad with an on-screen keyboard.
 *
 * The first thing here that takes TEXT from the user. Everything before it read
 * either a tap (the launcher) or a serial line (the shell), and a serial line
 * needs a computer attached — which the launcher exists to avoid needing.
 *
 * ---- why this is native and not a bytecode program -----------------------
 *
 * Every other application is VM bytecode confined to a 26-row strip
 * (UM-NATOS-016 §2). A keyboard does not fit in 26 rows, and the VM has no
 * syscall that returns a keypress. This owns the launcher's region instead, the
 * same way the 3D view does, and is native code for the same reason the
 * launcher is: it is part of the interface rather than something running inside
 * it.
 *
 * That is a real limitation and not a design win. It means text entry is a
 * kernel feature rather than a service applications can ask for. Giving the VM
 * a keyboard syscall is the version that would let any program read text, and
 * it is not built.
 */

#ifndef NATOS_NOTES_H
#define NATOS_NOTES_H

#include <stdint.h>

/* Longest note. Deliberately small: this lives in .bss, and the panel can show
 * about seven lines of forty characters at once, so a buffer much larger than
 * the screen would be text the user cannot see or reach. */
#define NOTES_MAX 256u

/* Called when the note pad takes the region, so the keyboard is redrawn from
 * scratch rather than assuming whatever was there before. */
void notes_open(void);

/* Draws whatever has changed. The keyboard is static once painted; only the
 * text area is repainted as characters arrive. */
void notes_frame(void);

/* Feeds a touch sample. Acts on the FIRST sample of a press, for the reason in
 * UM-NATOS-021 §4.2: the last sample before release is the one a resistive
 * panel gets wrong, and a keyboard that types the wrong letter is worse than
 * one that misses a press. */
void notes_touch(uint32_t x, uint32_t y, int down);

uint32_t notes_length(void);
uint32_t notes_keys(void);      /* keys accepted, for the reporter */

#endif /* NATOS_NOTES_H */
