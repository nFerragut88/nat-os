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
#include "uart.h"

extern unsigned int vendor_torture(unsigned int, unsigned int);

static volatile uint32_t g_runs, g_bad;

/* Written by vendor_spilltest() in the windowed test file. */
volatile unsigned int g_spill_ws_before;
volatile unsigned int g_spill_ws_after;
static volatile int   g_spill_done;

static unsigned int bitcount(unsigned int v)
{
    unsigned int n = 0;
    for (; v; v &= v - 1u) { n++; }
    return n;
}
static volatile int      g_first = -1;

uint32_t wincollide_runs(void) { return g_runs; }
uint32_t wincollide_bad(void)  { return g_bad; }

void wincollide_entry(void);
void wincollide_entry(void)
{
    /* Same derivation the shell prints, kept here so the task does not depend
     * on a value passed through a global that the other task also writes. */
    if (g_first < 0) { g_first = task_current(); }

    uint32_t want = 0;
    for (uint32_t d = 1; d <= 8u; d++) {
        for (uint32_t i = 0; i < 6u; i++) { want += (d * 7u) + i; }
    }

    /* Once, from the first task in: measure the spill itself. */
    if (!g_spill_done) {
        extern unsigned int vendor_spilltest(unsigned int);
        g_spill_done = 1;
        (void)rom_call3((uint32_t)&vendor_spilltest, 8u, 0u, 0u);
        uart_puts("   spill: windowstart ");
        uart_put_hex(g_spill_ws_before);
        uart_puts(" (");
        uart_put_dec(bitcount(g_spill_ws_before));
        uart_puts(" frames) -> ");
        uart_put_hex(g_spill_ws_after);
        uart_puts(" (");
        uart_put_dec(bitcount(g_spill_ws_after));
        uart_puts(" frames)\n");
        uart_puts(bitcount(g_spill_ws_after) == 1u
                  ? "   spill reduces to ONE frame as designed\n"
                  : "   SPILL DOES NOT REDUCE TO ONE FRAME\n");
    }

    for (;;) {
        /* Progress trace. The shell is starved when this reproducer wedges, so
         * the tasks have to report for themselves: 'a'/'b' on entry to the
         * windowed call, 'A'/'B' on return. A run that shows the lower-case
         * letter and never its upper-case pair names the task that went in and
         * did not come out -- which is the whole question. */
        uart_putc(task_current() == g_first ? 'a' : 'b');

        /* depth 8 fits a 2 KB task stack; the 5 ms spin at the bottom is what
         * makes the two tasks overlap rather than merely alternate. */
        uint32_t got = rom_call3((uint32_t)&vendor_torture, 8u, 5u, 0u);
        uart_putc(task_current() == g_first ? 'A' : 'B');
        if (got != want) { g_bad++; }
        g_runs++;
        task_sleep(1u);
    }
}
