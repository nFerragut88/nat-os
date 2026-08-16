/* nat-os — register-window support. See window.S.
 *
 * The kernel is -mabi=call0 and does not need windows. These exist so that
 * WINDOWED code can run at all, which is the gate in front of every
 * precompiled Espressif library (UM-NATOS-003 §5.1).
 */

#ifndef NATOS_WINDOW_H
#define NATOS_WINDOW_H

#include <stdint.h>

/* Calls a windowed-ABI function from call0 code and returns what it returned.
 *
 * win_probe(n) recurses n deep and returns n. With CALL8 the 64 physical
 * registers are exhausted after 8 frames, so any depth above that MUST take
 * window overflow exceptions on the way down and underflows on the way back.
 * The return value is therefore a checksum over every handler invocation: a
 * handler that reloads the wrong register returns the wrong number rather than
 * merely failing to crash. */
uint32_t win_call_probe(uint32_t depth);

#endif /* NATOS_WINDOW_H */
