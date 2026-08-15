/* nat-os — periodic tick. See timer.c for why CCOMPARE1 is the source. */
#ifndef NATOS_TIMER_H
#define NATOS_TIMER_H

#include <stdint.h>

/* Arm the tick. interval_cycles is in CPU cycles, not microseconds — the
 * kernel does not yet know the CPU frequency, so callers work in cycles and
 * the rate is derived by measurement (see UM-NATOS-008). */
void timer_start(uint32_t interval_cycles);

uint32_t timer_ticks(void);
uint32_t timer_last_delta(void);   /* cycles between the last two ticks */
uint32_t timer_late_count(void);   /* deadlines missed and skipped */

/* Defined in vectors.S's handler path; declared here so the compiler checks
 * the signature the assembly calls. */
void timer_isr(void);

#endif /* NATOS_TIMER_H */
