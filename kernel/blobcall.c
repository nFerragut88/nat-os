/* nat-os — calling the vendor blob with the scheduler still running.
 *
 * next_moves/08 step 11. phy_stack_call() masked interrupts for the whole
 * blob call, and the WiFi driver has now reached the point where that cannot
 * work: it wants _task_create_pinned_to_core, and a task created inside a
 * masked call can never run. A _semphr_take waiting on that task can never
 * return either.
 *
 * ---- what the masking was actually protecting -------------------------
 *
 * Two things, and only one of them needed interrupts off.
 *
 *   1. WINDOW STATE. nat-os's level-3 handler saves a0..a15 and not
 *      WINDOWBASE/WINDOWSTART, so the concern was a context switch landing
 *      while windowed frames were live.
 *
 *      That concern was MEASURED and does not hold. `wintorture` holds eight
 *      live windowed frames across real context switches -- confirmed by the
 *      switch counter, which is the control -- and the checksum is correct
 *      6/6. A call0 task cannot disturb them: the handler saves and restores
 *      the sixteen registers at the current WINDOWBASE and never moves
 *      WINDOWBASE, and call0 code never rotates the window, so a windowed
 *      task's caller frames sit at window positions no other task can reach.
 *
 *      The real hazard is TWO CONTEXTS inside windowed code at once, which is
 *      an exclusion problem, not a preemption problem.
 *
 *   2. THE PRIVATE STACK. `_phy_stack` is a single shared 6 KB buffer. Two
 *      contexts entering phy_stack_call would corrupt each other whatever the
 *      window did.
 *
 * A mutex answers both, and unlike a masked interrupt it lets the scheduler
 * keep running -- which is the whole point.
 *
 * ---- what this does NOT cover -----------------------------------------
 *
 * An interrupt handler cannot take a mutex. If a WiFi ISR ever calls into the
 * blob, this is not enough and the exclusion has to be reconsidered. Nothing
 * does that today; `_set_intr` clamps priorities to CRIT_LEVEL and counts it,
 * so the day it matters is visible rather than silent.
 */

#include <stdint.h>
#include "blobcall.h"
#include "mutex.h"
#include "window.h"
#include "critical.h"

extern uint32_t g_phy_call_mask;     /* window.S */

static mutex_t g_blob_mutex;
static int     g_ready;

static uint32_t g_calls;             /* blob entries made through here */
static uint32_t g_contended;         /* times a second context had to wait */

void blob_call_init(void)
{
    uint32_t crit = crit_enter();
    if (!g_ready) {
        mutex_init(&g_blob_mutex);
        g_ready = 1;
    }
    crit_exit(crit);
}

uint32_t blob_call(uint32_t fn, uint32_t a, uint32_t b, uint32_t c, uint32_t d)
{
    blob_call_init();

    /* Contention is counted rather than assumed away. Today there is exactly
     * one caller, so this should stay zero; if it does not, something has
     * started entering the blob from a second context and the assumptions
     * above are worth re-reading. */
    if (mutex_owner(&g_blob_mutex) >= 0) {
        g_contended++;
    }
    mutex_lock(&g_blob_mutex);

    /* Scheduler stays alive for the duration. The mutex is what keeps a
     * second context out; interrupts no longer have to be. */
    g_phy_call_mask = 0u;
    uint32_t r = phy_stack_call(fn, a, b, c, d);
    g_phy_call_mask = 1u;

    g_calls++;
    mutex_unlock(&g_blob_mutex);
    return r;
}

uint32_t blob_call_count(void)     { return g_calls; }
uint32_t blob_call_contended(void) { return g_contended; }
