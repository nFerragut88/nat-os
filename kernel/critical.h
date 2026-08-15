/* cyd-os — critical sections.
 *
 * Raises PS.INTLEVEL to mask the scheduler tick, so a short sequence completes
 * without being preempted. On a single core with no other bus master, that is
 * sufficient for mutual exclusion: nothing else can be running.
 *
 * THIS IS THE WRONG TOOL FOR ANYTHING LONG. Interrupts are masked for the whole
 * section, so scheduling latency is degraded by exactly its duration. Writing a
 * 150-character line to UART0 at 115200 baud takes about 13 ms — masking the
 * tick for that long would wreck the scheduler to solve a cosmetic problem. Use
 * a mutex (mutex.h) for anything that is not a handful of memory operations.
 *
 * The saved level is returned and restored rather than being forced back to 0,
 * so nesting works and a section inside an interrupt handler does not silently
 * re-enable interrupts on the way out.
 */

#ifndef CYDOS_CRITICAL_H
#define CYDOS_CRITICAL_H

#include <stdint.h>
#include "xtensa.h"

/* Masks everything up to and including the timer's level 3. */
#define CRIT_LEVEL 3u

static inline uint32_t crit_enter(void)
{
    uint32_t ps = xt_get_ps();
    xt_set_intlevel(CRIT_LEVEL);
    return ps & 0xFu;
}

static inline void crit_exit(uint32_t saved_level)
{
    xt_set_intlevel(saved_level);
}

#endif /* CYDOS_CRITICAL_H */
