/* nat-os — periodic tick from the core's CCOMPARE1 comparator.
 *
 * CCOUNT is a free-running cycle counter; when it equals CCOMPARE1 the core
 * raises internal interrupt 15 (level 3). This source was chosen over the
 * TIMG peripherals because it needs no clock gating, no pin muxing and no
 * interrupt-matrix routing — it is inside the core. For a first interrupt on a
 * kernel with no drivers yet, that removes almost every way to be wrong about
 * something other than interrupts themselves.
 *
 * The comparator is one-shot: it must be re-armed inside the handler, and the
 * write is also what acknowledges the interrupt.
 */

#include "timer.h"
#include "xtensa.h"

/* Written by the ISR, read by the main context — volatile, and read/written
 * as a single 32-bit word so no lock is needed on this core. */
static volatile uint32_t g_ticks;
static volatile uint32_t g_last_delta;   /* cycles between the last two ticks */
static volatile uint32_t g_late;         /* re-arms that had already elapsed */

static uint32_t g_interval;              /* cycles between ticks */
static uint32_t g_next;                  /* CCOUNT value of the next tick */

/* Called from _handler_level3 in vectors.S. Must not be static — assembly
 * references it by name. Keep it short: interrupts at level 3 and below are
 * masked for its duration. */
void timer_isr(void)
{
    uint32_t now = xt_ccount();
    g_last_delta = now - (g_next - g_interval);

    g_next += g_interval;

    /* If the handler was delayed past the next deadline, skipping ahead avoids
     * a storm of immediate re-entries trying to catch up. Counted, because a
     * rising value means the tick rate is too aggressive for the work being
     * done in the handler. */
    /* The deadline must end up within one interval of now, in BOTH directions.
     *
     * Too late was the only case this handled, and it is the obvious one. Too
     * EARLY is the one that actually bit: task_yield() ends a slice by pulling
     * CCOMPARE1 back to ccount+64 without telling this file, so g_next still
     * holds the original deadline. The handler then fires 64 cycles later and
     * adds a WHOLE interval to a deadline that was already a whole interval
     * away — and every yield pushes the tick further into the future.
     *
     * Measured after the display task's sleep was shortened, which multiplied
     * the yield rate: CCOMPARE1 ended up 14,630,119 cycles — 18 tick periods,
     * 183 ms — ahead of ccount, and the tick simply stopped for that long. The
     * self-test that caught it asserts a tick resumes promptly after a critical
     * section; it had been passing only because yields were rarer.
     *
     * Nothing else showed a symptom, which is the uncomfortable part. Sleeps
     * were silently running long, timeouts were silently loose, and the frame
     * rate this session spent so long measuring was taken against a clock that
     * intermittently stalled. */
    int32_t ahead = (int32_t)(g_next - xt_ccount());
    if (ahead <= 0 || ahead > (int32_t)g_interval) {
        g_next = xt_ccount() + g_interval;
        g_late++;
    }

    xt_set_ccompare1(g_next);   /* re-arm and acknowledge */
    g_ticks++;
}

void timer_start(uint32_t interval_cycles)
{
    g_interval = interval_cycles;
    g_ticks = 0;
    g_late = 0;
    g_last_delta = 0;

    g_next = xt_ccount() + interval_cycles;
    xt_set_ccompare1(g_next);

    xt_enable_interrupt(XT_TIMER1_INTERRUPT);
    xt_set_intlevel(0);          /* admit all levels; nothing is masked now */
}

uint32_t timer_ticks(void)      { return g_ticks; }
uint32_t timer_last_delta(void) { return g_last_delta; }
uint32_t timer_late_count(void) { return g_late; }
