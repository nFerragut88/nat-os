/* cyd-os — Milestone 2: preemptive task switching.
 *
 * M1 proved the kernel can be interrupted and resume with its registers
 * intact. M2 uses that: the same interrupt now saves the full context, asks
 * the scheduler for a different stack, and resumes somebody else.
 *
 * Two worker tasks each maintain private state that must survive being
 * suspended arbitrarily. If the context switch is wrong, that state diverges —
 * so each task verifies its own invariant continuously rather than trusting
 * that switching worked.
 */

#include "uart.h"
#include "timer.h"
#include "task.h"
#include "xtensa.h"

#define TICK_INTERVAL_CYCLES  800000u   /* ~10 ms at the measured ~80 MHz */

extern char _stack_top;
extern char _vecbase;

/* Each worker keeps a counter and a value derived from it. The pair is only
 * consistent if every switch preserved the task's registers and stack exactly.
 * Written from the tasks, read by the reporter — volatile, single words. */
static volatile uint32_t work_a_count, work_a_bad;
static volatile uint32_t work_b_count, work_b_bad;

/* Verifies that long-lived local state survives being suspended.
 *
 * Several values are kept live across the whole loop, so the compiler naturally
 * parks them in callee-saved registers (a12..a15) and spills the rest to this
 * task's stack — which is precisely the state a context switch must preserve.
 * A handler that saved only the caller-saved set, or that failed to give each
 * task its own stack, breaks this within a few switches.
 *
 * Registers are deliberately NOT pinned with explicit __asm__("aN") bindings.
 * Doing so claims a register the compiler may already be using (a15 in
 * particular can serve as a frame pointer); writes then land on arbitrary
 * memory. That is not a hypothetical — it corrupted the task table on the
 * first attempt at this test, and the scheduler stopped finding runnable
 * tasks. Let the compiler allocate; check the values, not their location. */
static void worker(volatile uint32_t *count, volatile uint32_t *bad, uint32_t seed)
{
    uint32_t n     = 0;
    uint32_t magic = seed;
    uint32_t alt   = ~seed;
    uint32_t acc   = seed ^ 0x9E3779B9u;

    for (;;) {
        n++;
        acc = acc * 1664525u + 1013904223u;

        /* Barrier only — keeps the values live across a point where an
         * interrupt can land, without dictating where they live. */
        __asm__ volatile ("" : "+r"(n), "+r"(magic), "+r"(alt), "+r"(acc));

        if (magic != seed || alt != ~seed) {
            (*bad)++;
            magic = seed;       /* repair, so one fault is not counted forever */
            alt   = ~seed;
        }
        *count = n;
    }
}

static void task_a(void) { worker(&work_a_count, &work_a_bad, 0xA5A5A5A5u); }
static void task_b(void) { worker(&work_b_count, &work_b_bad, 0x5A5A5A5Au); }

static void banner(void)
{
    uart_puts("\n\n");
    uart_puts("=====================================\n");
    uart_puts(" cyd-os  milestone 2 — task switching\n");
    uart_puts("=====================================\n");
}

void kmain(void)
{
    banner();

    xt_set_vecbase((unsigned int)&_vecbase);
    uart_puts("  vecbase      : ");
    uart_put_hex(xt_get_vecbase());
    uart_puts("\n");

    int a = task_create("worker-a", task_a);
    int b = task_create("worker-b", task_b);
    uart_puts("  tasks created: ");
    uart_put_dec((unsigned int)a);
    uart_puts(", ");
    uart_put_dec((unsigned int)b);
    uart_puts("  (0 = boot context)\n");

    timer_start(TICK_INTERVAL_CYCLES);
    uart_puts("  tick every   : ");
    uart_put_dec(TICK_INTERVAL_CYCLES);
    uart_puts(" cycles\n\n");

    /* The boot path continues as task 0 — its stack pointer is captured on the
     * first switch, so no explicit hand-off is needed. This loop is therefore
     * itself a scheduled task and will be suspended and resumed like the
     * others; the fact that it keeps printing coherently is part of the test. */
    uint32_t reported = 0;
    for (;;) {
        uint32_t t = timer_ticks();
        if (t - reported < 20u) {
            continue;
        }
        reported = t;

        uart_puts("  t=");
        uart_put_dec(t);
        uart_puts("  switches boot/a/b=");
        uart_put_dec(task_switch_count(0));
        uart_putc('/');
        uart_put_dec(task_switch_count(a));
        uart_putc('/');
        uart_put_dec(task_switch_count(b));

        uart_puts("  work a/b=");
        uart_put_dec(work_a_count);
        uart_putc('/');
        uart_put_dec(work_b_count);

        uart_puts("  guards=");
        uart_puts(task_stack_intact(a) && task_stack_intact(b) ? "ok" : "BROKEN");

        uart_puts("  free a/b=");
        uart_put_dec(task_stack_headroom(a));
        uart_putc('/');
        uart_put_dec(task_stack_headroom(b));

        uart_puts("w  corrupt=");
        uart_put_dec(work_a_bad + work_b_bad);
        uart_puts("\n");
    }
}
