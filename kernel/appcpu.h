/* nat-os — the second core, used as an instrument. See appcpu.c.
 *
 * This is not SMP and is not a step toward it. Core 1 exists here to sample the
 * regi2c host while core 0 is inside register_chipv7_phy(), which is
 * synchronous and therefore unobservable on one core (UM-NATOS-034 §27).
 */
#ifndef NATOS_APPCPU_H
#define NATOS_APPCPU_H

#include <stdint.h>

/* 8192 transactions. The reference board captured 5,095 during PHY init, so
 * this holds a full sequence with headroom -- and if it ever fills, the count
 * saturating at the maximum is itself the report that it did. */
#define APPCPU_CAP_MAX 8192u

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
