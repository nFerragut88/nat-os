/* cyd-os — Milestone 2: preemptive task switching.
 *
 * M1 proved the kernel can be interrupted and resume with its registers intact.
 * M2 uses that: the same interrupt saves the full context, asks the scheduler
 * for a different stack, and resumes somebody else.
 *
 * Every task is created the same way — including the reporter. kmain sets up,
 * starts the tick, and then never runs again; its stack is abandoned. There is
 * deliberately no path by which a running context becomes a task, because an
 * earlier design had exactly that and it was the one path that did not work.
 *
 * Two workers verify that their private state survives arbitrary suspension;
 * a third task reports. If the switch is wrong, the workers' invariants break
 * or the reporter never speaks.
 */

#include "uart.h"
#include "timer.h"
#include "task.h"
#include "watchdog.h"
#include "xtensa.h"

#define TICK_INTERVAL_CYCLES  800000u   /* ~10 ms at the measured ~80 MHz */

extern char _vecbase;

static volatile uint32_t work_a_count, work_a_bad;
static volatile uint32_t work_b_count, work_b_bad;

static int id_report, id_a, id_b;

/* Keeps several values live across the whole loop, so the compiler parks them
 * in callee-saved registers and spills the rest to this task's stack — exactly
 * the state a context switch must preserve. Registers are not pinned with
 * explicit __asm__("aN") bindings: that claims registers the compiler may
 * already be using, and writes then land on arbitrary memory. */
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

/* Entry markers, same purpose as task 0's: they distinguish "the round robin
 * reached this task" from "the switch died on the way there". Tick 2 enters
 * worker-a, tick 3 enters worker-b, and tick 4 is the first time any task is
 * RESUMED from a frame the handler actually saved rather than one fabricated
 * by task_create — three different mechanisms that all fail as silence. */
static void task_a(void) { uart_puts("  [task 1 entered]\n"); worker(&work_a_count, &work_a_bad, 0xA5A5A5A5u); }
static void task_b(void) { uart_puts("  [task 2 entered]\n"); worker(&work_b_count, &work_b_bad, 0x5A5A5A5Au); }

/* Reporter. A task like any other — it is suspended and resumed on the same
 * schedule as the workers, so the fact that its output stays coherent is
 * itself part of the test. */
static void task_report(void)
{
    uint32_t reported = 0;

    /* First thing a fabricated task ever does. If this appears, the RFI landed
     * on the entry point and the task owns the CPU; if it never appears, the
     * switch itself is where control is lost. Those two failures are otherwise
     * indistinguishable, because both present as silence. */
    uart_puts("\n  [task 0 entered]\n");

    for (;;) {
        uint32_t t = timer_ticks();
        if (t - reported < 200u) {
            continue;
        }
        reported = t;

        uart_puts("  t=");
        uart_put_dec(t);
        uart_puts("  switches r/a/b=");
        uart_put_dec(task_switch_count(id_report));
        uart_putc('/');
        uart_put_dec(task_switch_count(id_a));
        uart_putc('/');
        uart_put_dec(task_switch_count(id_b));

        uart_puts("  work a/b=");
        uart_put_dec(work_a_count);
        uart_putc('/');
        uart_put_dec(work_b_count);

        uart_puts("  guards=");
        uart_puts(task_stack_intact(id_report) && task_stack_intact(id_a) &&
                  task_stack_intact(id_b) ? "ok" : "BROKEN");

        uart_puts("  freew a/b=");
        uart_put_dec(task_stack_headroom(id_a));
        uart_putc('/');
        uart_put_dec(task_stack_headroom(id_b));

        uart_puts("  corrupt=");
        uart_put_dec(work_a_bad + work_b_bad);
        uart_puts("\n");
    }
}

void kmain(void)
{
    uart_puts("\n\n");
    uart_puts("=====================================\n");
    uart_puts(" cyd-os  milestone 2 — task switching\n");
    uart_puts("=====================================\n");

    /* Before anything can take long enough to trip it. The bootloader arms the
     * RTC watchdog and expects the application to take ownership. */
    watchdog_disable_all();
    uart_puts("  rtc wdt      : ");
    uart_puts(watchdog_rtc_config() == 0u ? "disarmed\n" : "STILL ARMED\n");

    xt_set_vecbase((unsigned int)&_vecbase);
    uart_puts("  vecbase      : ");
    uart_put_hex(xt_get_vecbase());
    uart_puts("\n");

    id_report = task_create("report", task_report);
    id_a      = task_create("worker-a", task_a);
    id_b      = task_create("worker-b", task_b);
    uart_puts("  tasks        : report=");
    uart_put_dec((unsigned int)id_report);
    uart_puts(" a=");
    uart_put_dec((unsigned int)id_a);
    uart_puts(" b=");
    uart_put_dec((unsigned int)id_b);
    uart_puts("\n");

    /* Single-threaded selection test. No interrupt source is armed yet, so a
     * wrong answer here cannot involve context switching at all. Expected, with
     * tasks 0/1/2 READY and 3 unused: -1->0, 0->1, 1->2, 2->0. */
    uart_puts("  select probe : ");
    for (int c = -1; c < TASK_MAX; c++) {
        int got = task_select_probe(c);
        int want = (c == 2 || c == 3) ? 0 : c + 1;
        uart_put_dec((unsigned int)c);
        uart_puts("->");
        uart_put_dec((unsigned int)got);
        uart_puts(got == want ? " ok  " : " WRONG  ");
    }
    uart_puts("\n");

    uart_puts("  tick every   : ");
    uart_put_dec(TICK_INTERVAL_CYCLES);
    uart_puts(" cycles\n");
    uart_puts("  handing off to the scheduler — kmain does not return\n\n");

    timer_start(TICK_INTERVAL_CYCLES);

    /* The first tick should switch into task 0 and never resume this context.
     * DIAGNOSTIC: if we are still here, report whether the tick is advancing —
     * that separates "interrupt not firing" from "switch not working", which
     * produce identical silence. */
    uint32_t spins = 0;
    for (;;) {
        if (++spins >= 600000u) {
            spins = 0;
            uart_puts("  [kmain still here] ticks=");
            uart_put_dec(timer_ticks());
            uart_puts(" ccount=");
            uart_put_hex(xt_ccount());
            uart_puts(" intenable=");
            uart_put_hex(xt_get_intenable());
            uart_puts(" ps=");
            uart_put_hex(xt_get_ps());
            uart_puts(" ccompare1=");
            uart_put_hex(xt_get_ccompare1());
            uart_puts("\n");
        }
    }
}
