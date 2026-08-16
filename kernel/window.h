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

/* Calls vendor_probe() in vendor/windowed/, which is compiled -mabi=windowed by
 * the same compiler that builds Espressif's libraries. Standing proof that this
 * kernel can call vendor-ABI code, not just hand-written windowed assembly. */
uint32_t win_call_vendor(uint32_t depth, uint32_t seed);

/* Calls a three-argument WINDOWED function at an arbitrary address.
 *
 * Espressif's ROM holds hundreds of routines at fixed addresses, all windowed,
 * needing no linking and no environment. This makes every one of them callable.
 * crc32_le lives at 0x4005CFEC and is a pure function, which is why it is the
 * first vendor code this kernel runs. */
uint32_t rom_call3(uint32_t fn, uint32_t a, uint32_t b, uint32_t c);

/* Calls a windowed function that calls BACK into call0 code on every
 * iteration -- proof the reverse bridge round-trips. */
uint32_t win_call_bridge(uint32_t fn_add, uint32_t depth);

#define ESP_ROM_CRC32_LE  0x4005CFECu

/* Output captured from libphy.a's own printf. The blob is windowed and cannot
 * call back into this call0 kernel, so its shim buffers instead — a data
 * pointer crosses the ABI boundary safely where a call would not. */
/* Windowed -> call0 bridges, for the OSI table's bodies. See window.S. */
uint32_t w2c_call0f(uint32_t fn);
uint32_t w2c_call1(uint32_t fn, uint32_t a);
uint32_t w2c_call2(uint32_t fn, uint32_t a, uint32_t b);
uint32_t w2c_call3(uint32_t fn, uint32_t a, uint32_t b, uint32_t c);

extern char     phy_host_log_buf[];
extern uint32_t phy_host_log_len;

/* Calls a WINDOWED function on a private 6 KB stack. The PHY nests far deeper
 * than a 2 KB nat-os task stack allows, and the overflow shows up as a
 * StoreProhibited inside the window spill rather than as anything resembling a
 * stack problem. See the note in window.S. */
uint32_t phy_stack_call(uint32_t fn, uint32_t a, uint32_t b);

#endif /* NATOS_WINDOW_H */
