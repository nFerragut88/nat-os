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

#include "arena.h"
#include "heap.h"
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

/* ---- Milestone 3 self-test -------------------------------------------- */
/*
 * Runs single-threaded before the tick is armed, so nothing here can be
 * disturbed by a context switch and a failure cannot be blamed on M2.
 * Each block corresponds to one exit criterion in UM-CYDOS-007 §5.
 */

#define LEAK_ITERS 10000u
#define LEAK_SLOTS 8u

static void *g_slots[LEAK_SLOTS];

static void m3_selftest(void)
{
    heap_init();

    uint32_t base_free   = heap_free_bytes();
    uint32_t base_large  = heap_largest_free();
    uint32_t base_blocks = heap_blocks();

    uart_puts("  heap         : ");
    uart_put_dec(heap_total());
    uart_puts(" B usable, largest ");
    uart_put_dec(base_large);
    uart_puts(" B, blocks ");
    uart_put_dec(base_blocks);
    uart_puts("\n");

    /* --- Criterion 1: 10,000 alloc/free cycles leave no leak --------------
     * Sizes vary so blocks are split and coalesced constantly; a missed merge
     * shows up as a largest-free that never recovers, which is the failure a
     * simple "free bytes match" test would miss entirely. */
    uint32_t seed = 0x1234567u;
    uint32_t oom  = 0;

    for (uint32_t i = 0; i < LEAK_ITERS; i++) {
        uint32_t s = i % LEAK_SLOTS;
        heap_free(g_slots[s]);          /* NULL on the first pass — a no-op */
        seed = seed * 1664525u + 1013904223u;
        uint32_t sz = 16u + ((seed >> 13) % 500u);
        g_slots[s] = heap_alloc(sz);
        if (!g_slots[s]) {
            oom++;
        }
    }
    for (uint32_t s = 0; s < LEAK_SLOTS; s++) {
        heap_free(g_slots[s]);
        g_slots[s] = 0;
    }

    int chk = heap_check();
    int leak_ok = (heap_free_bytes() == base_free) &&
                  (heap_largest_free() == base_large) &&
                  (heap_blocks() == base_blocks) &&
                  (heap_used_bytes() == 0u) && (chk == 0) && (oom == 0u);

    uart_puts("  [1] no leak  : ");
    uart_puts(leak_ok ? "PASS" : "FAIL");
    uart_puts("  after ");
    uart_put_dec(LEAK_ITERS);
    uart_puts(" cycles  free=");
    uart_put_dec(heap_free_bytes());
    uart_puts(" largest=");
    uart_put_dec(heap_largest_free());
    uart_puts(" blocks=");
    uart_put_dec(heap_blocks());
    uart_puts(" check=");
    uart_put_dec((unsigned int)chk);
    uart_puts("\n");

    /* --- Criterion 3: exhaustion fails cleanly ---------------------------- */
    uint32_t fails_before = heap_fail_count();
    void *huge = heap_alloc(heap_total() + 1u);
    int oom_ok = (huge == 0) && (heap_fail_count() == fails_before + 1u) &&
                 (heap_check() == 0) && (heap_free_bytes() == base_free);

    /* A refused free must not corrupt the list either. Both a wild pointer and
     * a double free are counted rather than acted on. */
    uint32_t bad_before = heap_bad_free_count();
    void *live = heap_alloc(64);
    heap_free(live);
    heap_free(live);                    /* double free */
    uint32_t stack_local = 0;
    heap_free(&stack_local);            /* not a heap pointer at all */
    int guard_ok = (heap_bad_free_count() == bad_before + 2u) &&
                   (heap_check() == 0) && (heap_free_bytes() == base_free);

    uart_puts("  [3] oom safe : ");
    uart_puts((oom_ok && guard_ok) ? "PASS" : "FAIL");
    uart_puts("  oversize=NULL fails=");
    uart_put_dec(heap_fail_count());
    uart_puts(" bad_frees=");
    uart_put_dec(heap_bad_free_count());
    uart_puts(" check=");
    uart_put_dec((unsigned int)heap_check());
    uart_puts("\n");

    /* --- Criterion 2: arena bounds are queryable -------------------------- */
    int a = arena_create(4096);
    int b = arena_create(1024);

    uint32_t abase = 0, alen = 0;
    int q = arena_bounds(a, &abase, &alen);

    /* The checks the interpreter will make on every load and store. The last
     * one is the reason arena_contains() works in the offset domain: addr+len
     * wraps the address space, and a naive comparison would admit it. */
    int bounds_ok =
        (q == 0) && (alen == 4096u) && (abase != 0u) &&
        arena_contains(a, abase, 4096u)             &&  /* exact fit      */
        arena_contains(a, abase + 4095u, 1u)        &&  /* last byte      */
        arena_contains(a, abase, 0u)                &&  /* empty access   */
        !arena_contains(a, abase + 4096u, 1u)       &&  /* one past end   */
        !arena_contains(a, abase - 1u, 1u)          &&  /* one before     */
        !arena_contains(a, abase, 4097u)            &&  /* one too long   */
        !arena_contains(a, abase + 8u, 0xFFFFFFF8u) &&  /* wrap attempt   */
        !arena_contains(b, abase, 1u)               &&  /* wrong arena    */
        !arena_contains(99, abase, 1u);                 /* bogus id       */

    /* Zeroed on creation: an application must not see the previous tenant. */
    int zero_ok = 1;
    for (uint32_t i = 0; i < 4096u / 4u; i++) {
        if (((volatile uint32_t *)abase)[i] != 0u) {
            zero_ok = 0;
            break;
        }
    }

    uart_puts("  [2] arenas   : ");
    uart_puts((bounds_ok && zero_ok) ? "PASS" : "FAIL");
    uart_puts("  live=");
    uart_put_dec(arena_count());
    uart_puts(" committed=");
    uart_put_dec(arena_bytes_committed());
    uart_puts(" B  base=");
    uart_put_hex(abase);
    uart_puts(" len=");
    uart_put_dec(alen);
    uart_puts("\n");

    arena_destroy(a);
    arena_destroy(b);
    arena_destroy(a);                   /* already gone — counted, not acted on */

    uart_puts("  arenas freed : check=");
    uart_put_dec((unsigned int)heap_check());
    uart_puts(" free=");
    uart_put_dec(heap_free_bytes());
    uart_puts("/");
    uart_put_dec(base_free);
    uart_puts(" rejects=");
    uart_put_dec(arena_reject_count());
    uart_puts(" high_water=");
    uart_put_dec(heap_high_water());
    uart_puts(" B\n");
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

    m3_selftest();

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
