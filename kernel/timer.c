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

uint32_t g_timer_interval;              /* cycles between ticks */
uint32_t g_timer_next;                  /* CCOUNT value of the next tick */

/* Called from _handler_level3 in vectors.S. Must not be static — assembly
 * references it by name. Keep it short: interrupts at level 3 and below are
 * masked for its duration. */
void timer_isr(void)
{
    uint32_t now = xt_ccount();

    /* Not every entry here is a tick.
     *
     * task_yield() ends a slice by pulling CCOMPARE1 back to ccount+64, so this
     * handler runs on every yield as well as on every real deadline. Counting
     * entries therefore counts yields, and g_ticks++ used to sit at the bottom
     * of this function unconditionally.
     *
     * That made timer_ticks() a measure of scheduling activity rather than of
     * time, and every deadline expressed in ticks came due early in proportion
     * to how much yielding the system happened to be doing. Measured at 217
     * ticks per real second with the shell busy-waiting; far higher when a task
     * sits in an idle-yield loop, which is where task_sleep(50) was observed
     * returning in under a millisecond.
     *
     * The consequences were quiet and everywhere: sleeps ran short, timeouts
     * ran loose, and any measurement taken against this clock was wrong by a
     * factor that varied with load. The 3D view's own frame timing was among
     * them.
     *
     * So: advance the clock only when the deadline has actually passed. An
     * early entry falls through to the re-arm below, which restores CCOMPARE1
     * to the real deadline the yield displaced -- the yield still got its
     * context switch, because _handler_level3 calls the scheduler regardless of
     * what this function decides. */
    if ((int32_t)(now - g_timer_next) < 0) {
        xt_set_ccompare1(g_timer_next);
        return;
    }

    g_last_delta = now - (g_timer_next - g_timer_interval);

    g_timer_next += g_timer_interval;

    /* If the handler was delayed past the next deadline, skipping ahead avoids
     * a storm of immediate re-entries trying to catch up. Counted, because a
     * rising value means the tick rate is too aggressive for the work being
     * done in the handler. */
    /* The deadline must end up within one interval of now, in BOTH directions.
     *
     * Too late is the obvious case and the only one still reachable here: the
     * early-return above now catches every entry that arrives before the
     * deadline, so a yield can no longer reach this code at all. The too-EARLY
     * arm is kept because it costs nothing and this is the second bug in this
     * function to come from an assumption about who calls it.
     *
     * The history is worth keeping. task_yield() pulls CCOMPARE1 back to
     * ccount+64 without telling this file, so g_timer_next still held the original
     * deadline; the handler then fired 64 cycles later and added a WHOLE
     * interval to a deadline that was already a whole interval away — every
     * yield pushing the tick further into the future.
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
    int32_t ahead = (int32_t)(g_timer_next - xt_ccount());
    if (ahead <= 0 || ahead > (int32_t)g_timer_interval) {
        g_timer_next = xt_ccount() + g_timer_interval;
        g_late++;
    }

    xt_set_ccompare1(g_timer_next);   /* re-arm and acknowledge */
    g_ticks++;
}

/* [step 273] The rescue itself is a STATIC INLINE in timer.h, not a function
 * here. Its caller is osi_wifi_int_restore() in vendor/windowed/, and a
 * windowed call8 into a call0 function is the violation this project has paid
 * for five times. An inline crosses nothing. The state it needs is exported
 * above for that reason. */
unsigned int g_timer_rescues;
unsigned int timer_rescue_count(void) { return g_timer_rescues; }

void timer_start(uint32_t interval_cycles)
{
    g_timer_interval = interval_cycles;
    g_ticks = 0;
    g_late = 0;
    g_last_delta = 0;

    g_timer_next = xt_ccount() + interval_cycles;
    xt_set_ccompare1(g_timer_next);

    xt_enable_interrupt(XT_TIMER1_INTERRUPT);
    xt_set_intlevel(0);          /* admit all levels; nothing is masked now */
}

uint32_t timer_ticks(void)      { return g_ticks; }
uint32_t timer_last_delta(void) { return g_last_delta; }
uint32_t timer_late_count(void) { return g_late; }
