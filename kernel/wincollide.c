/* nat-os -- two tasks inside windowed code at once. A reproducer, built to fail.
 *
 * next_moves/08 step 13 panicked with a WiFi task and a shell call both inside
 * windowed blob code. That was one observation with a blob, a driver, a mutex
 * and an adapter table all in frame. This strips it to the claim itself: two
 * ordinary nat-os tasks calling the same windowed function.
 *
 * rom_call3 is used deliberately -- unlike phy_stack_call it does not mask
 * interrupts, so both tasks really can be inside windowed code simultaneously.
 *
 * If this passes, the step-13 diagnosis is wrong and the scheduler work is not
 * justified. It is worth nothing until it fails.
 */

#include <stdint.h>
#include "task.h"
#include "window.h"

extern unsigned int vendor_torture(unsigned int, unsigned int);

static volatile uint32_t g_runs, g_bad;

uint32_t wincollide_runs(void) { return g_runs; }
uint32_t wincollide_bad(void)  { return g_bad; }

void wincollide_entry(void);
void wincollide_entry(void)
{
    /* Same derivation the shell prints, kept here so the task does not depend
     * on a value passed through a global that the other task also writes. */
    uint32_t want = 0;
    for (uint32_t d = 1; d <= 8u; d++) {
        for (uint32_t i = 0; i < 6u; i++) { want += (d * 7u) + i; }
    }

    for (;;) {
        /* depth 8 fits a 2 KB task stack; the 5 ms spin at the bottom is what
         * makes the two tasks overlap rather than merely alternate. */
        uint32_t got = rom_call3((uint32_t)&vendor_torture, 8u, 5u, 0u);
        if (got != want) { g_bad++; }
        g_runs++;
        task_sleep(1u);
    }
}
