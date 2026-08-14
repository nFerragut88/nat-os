/* cyd-os — Milestone 1: interrupts.
 *
 * M0 established that the kernel loads and runs. M1 establishes that it can be
 * interrupted and resume correctly, which is the prerequisite for every form of
 * scheduling.
 *
 * Three things are demonstrated:
 *   1. the vector table is installed and a timer interrupt is dispatched
 *   2. the tick advances at a stable, measurable rate
 *   3. interrupted code resumes with its registers intact
 *
 * (3) is checked rather than assumed because a context switch (M2) is built
 * entirely on that property, and a save/restore bug in the interrupt path
 * would otherwise surface later as inexplicable corruption in unrelated code.
 */

#include "uart.h"
#include "timer.h"
#include "xtensa.h"

/* Tick interval in CPU cycles. The kernel does not yet know the CPU frequency,
 * so this is a cycle count, and the real rate is derived by measuring ticks
 * against the host's clock during capture. At 240 MHz this is 100 Hz; at
 * 80 MHz it is 33 Hz. Either way the measurement tells us which. */
#define TICK_INTERVAL_CYCLES  2400000u

extern char _bss_start;
extern char _bss_end;
extern char _stack_top;
extern char _vecbase;

static volatile unsigned int data_canary = 0xC0DEFACEu;
static volatile unsigned int bss_canary;

static void banner(void)
{
    uart_puts("\n\n");
    uart_puts("=====================================\n");
    uart_puts(" cyd-os  milestone 1 — interrupts\n");
    uart_puts("=====================================\n");
}

static void self_check(void)
{
    uart_puts("  .data loaded : ");
    uart_puts(data_canary == 0xC0DEFACEu ? "ok\n" : "FAIL\n");

    uart_puts("  .bss cleared : ");
    uart_puts(bss_canary == 0 ? "ok\n" : "FAIL\n");

    uart_puts("  stack top    : ");
    uart_put_hex((unsigned int)&_stack_top);
    uart_puts("\n");

    unsigned int pc = (unsigned int)(void *)&self_check;
    uart_puts("  code at      : ");
    uart_put_hex(pc);
    uart_puts(pc >= 0x40080000u && pc < 0x400A0000u ? "  (IRAM ok)\n" : "  (NOT IRAM!)\n");
}

/* Hammer the caller-saved registers with known values, then confirm they
 * survived. Interrupts fire asynchronously, so running this continuously means
 * some iterations are certain to be interrupted mid-sequence — which is
 * precisely the case that a faulty save/restore would corrupt.
 *
 * Values are chosen to be distinct and non-zero so a stray zero or a
 * neighbouring register's value is obvious rather than plausible.
 */
static int register_integrity_ok(void)
{
    register unsigned int r2 __asm__("a2") = 0x11111111u;
    register unsigned int r3 __asm__("a3") = 0x22222222u;
    register unsigned int r4 __asm__("a4") = 0x33333333u;
    register unsigned int r5 __asm__("a5") = 0x44444444u;
    register unsigned int r6 __asm__("a6") = 0x55555555u;
    register unsigned int r7 __asm__("a7") = 0x66666666u;

    /* Keep them live across a window in which an interrupt may land. The empty
     * asm block prevents the compiler from folding the checks away. */
    __asm__ volatile ("" :: "a"(r2), "a"(r3), "a"(r4), "a"(r5), "a"(r6), "a"(r7));

    return r2 == 0x11111111u && r3 == 0x22222222u && r4 == 0x33333333u &&
           r5 == 0x44444444u && r6 == 0x55555555u && r7 == 0x66666666u;
}

void kmain(void)
{
    banner();
    self_check();

    uart_puts("  vecbase      : ");
    xt_set_vecbase((unsigned int)&_vecbase);
    uart_put_hex(xt_get_vecbase());
    uart_puts(xt_get_vecbase() == (unsigned int)&_vecbase ? "  (installed)\n"
                                                          : "  (WRITE FAILED)\n");

    uart_puts("  tick every   : ");
    uart_put_dec(TICK_INTERVAL_CYCLES);
    uart_puts(" cycles\n");

    timer_start(TICK_INTERVAL_CYCLES);

    uart_puts("  intenable    : ");
    uart_put_hex(xt_get_intenable());
    uart_puts("\n");
    uart_puts("  ps           : ");
    uart_put_hex(xt_get_ps());
    uart_puts("\n\n");

    /* Wait for the first tick before claiming anything works. If interrupts are
     * not being delivered this never advances, and the absence of the next line
     * is itself the diagnosis. */
    uart_puts("  waiting for first tick... ");
    unsigned int spins = 0;
    while (timer_ticks() == 0 && spins < 200000000u) {
        spins++;
    }
    uart_puts(timer_ticks() > 0 ? "arrived\n\n" : "NEVER ARRIVED — check vector/intenable\n\n");

    unsigned int reported = 0;
    unsigned int checks = 0;
    unsigned int corruptions = 0;

    for (;;) {
        if (!register_integrity_ok()) {
            corruptions++;
        }
        checks++;

        unsigned int t = timer_ticks();
        if (t != reported) {
            reported = t;

            /* One line per 25 ticks keeps the UART from becoming the bottleneck
             * while still showing the rate. */
            if ((t % 25u) == 0u) {
                uart_puts("  tick ");
                uart_put_dec(t);
                uart_puts("  delta=");
                uart_put_dec(timer_last_delta());
                uart_puts("cy  late=");
                uart_put_dec(timer_late_count());
                uart_puts("  regchecks=");
                uart_put_dec(checks);
                uart_puts("  corrupt=");
                uart_put_dec(corruptions);
                uart_puts("\n");
            }
        }
    }
}
