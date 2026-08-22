/* nat-os — the second core, used as an instrument. See appcpu.c.
 *
 * This is not SMP and is not a step toward it. Core 1 exists here to sample the
 * regi2c host while core 0 is inside register_chipv7_phy(), which is
 * synchronous and therefore unobservable on one core (UM-NATOS-034 §27).
 */
#ifndef NATOS_APPCPU_H
#define NATOS_APPCPU_H

#include <stdint.h>

/* 6144 transactions. Sized against two hard constraints. Lower bound: the
 * reference board captured 5,095 during PHY init, so anything smaller
 * truncates a sequence this instrument exists to record. Upper bound: these
 * two arrays are static DRAM, and at 8192 they cost 64 KB of a 140 KB budget
 * -- on 2026-08-22 that pushed _bss_end past _heap_end (linker places bss
 * into the boot-stack reservation without complaint), heap_init() saw an
 * inverted range and returned a zero-byte heap, and m3_selftest() faulted
 * dereferencing an arena base that never existed. 6144 keeps the measured
 * maximum plus ~20% for 32 KB. If the count ever saturates, that saturation
 * is the report; raise the cap only together with the map arithmetic that
 * has to fund it. */
#define APPCPU_CAP_MAX 6144u

/* Brings core 1 out of stall and reset and points it at appcpu_entry.
 * Returns 0 if started, 1 if it was already running. */
int      appcpu_start(void);

/* 0xC0DEA11E once core 1 has reached C. Anything else means it never arrived,
 * and the distinction matters: a core that is stalled and a core that faulted
 * on arrival look identical from here except through this word. */
uint32_t appcpu_alive(void);

/* Rising means core 1 is executing, not merely alive. */
uint32_t appcpu_spins(void);

/* Arm (1) clears the buffer and starts recording; disarm (0) stops. */
void     appcpu_arm(int on);

uint32_t appcpu_cap_count(void);
uint32_t appcpu_cap_val(uint32_t i);
uint32_t appcpu_cap_ts(uint32_t i);

#endif /* NATOS_APPCPU_H */
