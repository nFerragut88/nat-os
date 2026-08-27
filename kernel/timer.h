/* nat-os — periodic tick. See timer.c for why CCOMPARE1 is the source. */
#ifndef NATOS_TIMER_H
#define NATOS_TIMER_H

#include "xtensa.h"
#include <stdint.h>

/* Arm the tick. interval_cycles is in CPU cycles, not microseconds — the
 * kernel does not yet know the CPU frequency, so callers work in cycles and
 * the rate is derived by measurement (see UM-NATOS-008). */
void timer_start(uint32_t interval_cycles);

uint32_t timer_ticks(void);
uint32_t timer_last_delta(void);   /* cycles between the last two ticks */
uint32_t timer_late_count(void);

/* [step 273] THE RESCUE. Re-arm the tick if its one-shot comparator has been
 * left in the past.
 *
 * CCOMPARE1 asserts on the single cycle CCOUNT equals it. If interrupts are
 * masked for that cycle the match passes and is GONE -- Xtensa does not latch
 * it -- and the next is a full 2^32 cycles away: 53.7 s at 80 MHz, against a
 * 3 s watchdog. Step 272 caught CCOMPARE1 sitting 86,569,830 cycles behind
 * CCOUNT with interrupts enabled and nothing held.
 *
 * timer_isr already guards a bad deadline and g_late counts it, but that guard
 * is INSIDE THE HANDLER and the handler is what is not running. So this runs
 * where the level is LOWERED -- the first moment code executes again after the
 * window that ate the match.
 *
 * STATIC INLINE deliberately: its caller is osi_wifi_int_restore(), which is
 * WINDOWED, and a windowed call8 into a call0 function is the violation this
 * project has paid for five times. An inline crosses nothing -- verified in
 * the disassembly: that function contains no call of any kind.
 *
 * NOT wired into phy_exit_critical(), though it lowers the level too:
 * phy_host.c is compiled into the pre-linked BLOB as well as the kernel, and
 * an inline there would reference kernel globals the blob cannot resolve. The
 * phy critical sections run during phyinit and little else; the blob's own,
 * which wrap everything the driver does, are covered. */
extern uint32_t g_timer_interval, g_timer_next;
extern unsigned int g_timer_rescues;
unsigned int timer_rescue_count(void);

static inline int timer_rescue(void)
{
    if (g_timer_interval == 0u) { return 0; }            /* not started */
    uint32_t now = xt_ccount();
    if ((int32_t)(g_timer_next - now) > 0) { return 0; } /* still ahead */
    /* The match is already lost; resume ticking rather than catch up. */
    g_timer_next = now + g_timer_interval;
    xt_set_ccompare1(g_timer_next);
    g_timer_rescues++;
    return 1;
}   /* deadlines missed and skipped */

/* Defined in vectors.S's handler path; declared here so the compiler checks
 * the signature the assembly calls. */
void timer_isr(void);

#endif /* NATOS_TIMER_H */
