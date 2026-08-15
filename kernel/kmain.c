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
#include "app.h"
#include "console.h"
#include "display.h"
#include "touch.h"
#include "critical.h"
#include "mutex.h"
#include "shell.h"
#include "vm.h"
#include "generated/demo.h"
#include "generated/spin.h"
#include "generated/app_a.h"
#include "generated/app_b.h"
#include "generated/app_rogue.h"
#include "generated/app_draw.h"
#include "generated/app_gfx_rogue.h"
#include "generated/app_paint.h"
#include "uart.h"
#include "timer.h"
#include "task.h"
#include "watchdog.h"
#include "xtensa.h"

#define TICK_INTERVAL_CYCLES  800000u   /* ~10 ms at the measured ~80 MHz */

extern char _vecbase;

static volatile uint32_t work_a_count, work_a_bad;
static volatile uint32_t work_b_count, work_b_bad;

static int id_report, id_a, id_b, id_vm, id_apps, id_shell, id_idle, id_disp, id_touch;

/* Last touch seen, latched so the reporter can show it whenever it happens to
 * run. Without this, confirming a real touch needs a serial capture to coincide
 * with a finger, which is not a test anyone can repeat reliably. */
static volatile uint32_t g_last_rawx, g_last_rawy, g_last_x, g_last_y, g_last_z;

/* Defined below with the other self-tests, but called from the reporter, which
 * is the only context where a running tick makes it meaningful. */
static void m6_critical_test(void);

/* Contention target. The workers hammer this from two tasks, and the reporter
 * checks it against their own iteration counts. The read-modify-write below is
 * deliberately slow and deliberately non-atomic: without the mutex it would be
 * reliably corrupted rather than occasionally, which is what makes the result
 * meaningful instead of lucky. */
static mutex_t           g_shared_lock;
static volatile uint32_t g_shared;
static volatile uint32_t g_bumps_a, g_bumps_b;

/* Contend every Nth iteration rather than every one. Taking the lock on every
 * pass means a worker holds it for most of its iteration, so contention is
 * near-certain and each handoff costs a full scheduling round-trip — measured
 * at roughly a thirtyfold throughput collapse. That is a real property of a
 * blocking lock, but it is a pathological workload, not a representative one. */
#define BUMP_EVERY 32u

static void shared_bump(void)
{
    mutex_lock(&g_shared_lock);
    uint32_t v = g_shared;
    for (volatile int i = 0; i < 6; i++) {
        /* Widen the window between read and write. A tick landing here is the
         * whole point: unprotected, the other worker's increment is lost. */
    }
    g_shared = v + 1;
    mutex_unlock(&g_shared_lock);
}

/* Idle. Runs only when every other task is blocked. WAITI stops the core until
 * an interrupt arrives, so an idle system draws less power than one spinning —
 * and the tick is what wakes it. */
static void task_idle(void)
{
    for (;;) {
        __asm__ volatile ("waiti 0");
    }
}

/* ---- the VM hosted as a native task ------------------------------------
 *
 * Two independent preemption mechanisms stack here, and the point of this task
 * is that they compose:
 *
 *   - The timer interrupt suspends this task wherever it happens to be, which
 *     is usually somewhere inside the interpreter's own C code. The bytecode
 *     program cannot observe that; it is the M2 context switch doing its job.
 *
 *   - vm_run() returns after its instruction quantum, at a bytecode
 *     instruction boundary, so this task regains control on a schedule of its
 *     own regardless of what the program does — including never terminating.
 *
 * The arena is created in kmain BEFORE the scheduler starts, not here. The heap
 * has no locking (UM-CYDOS-010 §8) and allocating from task context would be
 * the first thing to break that. */
#define VM_TASK_QUANTUM 2000u

static vm_t     g_vm;
static int      g_vm_arena = -1;
static uint32_t g_vm_base;
static uint32_t g_vm_result;    /* last run outcome, for the reporter */

static void task_vm(void)
{
    uart_puts("  [task 3 entered] hosting bytecode\n");

    for (;;) {
        g_vm_result = (uint32_t)vm_run(&g_vm, VM_TASK_QUANTUM);

        /* A program that halts or faults must not spin the task at full tilt
         * forever. Nothing yet loads a replacement, so it simply stops asking
         * for cycles it cannot use. */
        if (g_vm_result != (uint32_t)VM_RUN_QUANTUM) {
            for (;;) {
                task_yield();
            }
        }
    }
}

/* The counter the bytecode publishes into its arena. Reading it from kernel
 * code is not a special capability — an arena is ordinary DRAM, and the
 * asymmetry is deliberate: the kernel can see into an arena, a program cannot
 * see out of one. */
static uint32_t vm_counter(void)
{
    if (g_vm_arena < 0) {
        return 0;
    }
    return *(volatile uint32_t *)(g_vm_base + VM_SPIN_AT_COUNTER);
}

/* Keeps several values live across the whole loop, so the compiler parks them
 * in callee-saved registers and spills the rest to this task's stack — exactly
 * the state a context switch must preserve. Registers are not pinned with
 * explicit __asm__("aN") bindings: that claims registers the compiler may
 * already be using, and writes then land on arbitrary memory. */
static void worker(volatile uint32_t *count, volatile uint32_t *bad,
                   volatile uint32_t *bumps, uint32_t seed)
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

        if ((n % BUMP_EVERY) == 0u) {
            shared_bump();
            (*bumps)++;
        }

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
static void task_a(void) { uart_puts("  [task 1 entered]\n"); worker(&work_a_count, &work_a_bad, &g_bumps_a, 0xA5A5A5A5u); }
static void task_b(void) { uart_puts("  [task 2 entered]\n"); worker(&work_b_count, &work_b_bad, &g_bumps_b, 0x5A5A5A5Au); }

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

    /* Deferred until here: it needs a running tick to mean anything. */
    m6_critical_test();

    for (;;) {
        uint32_t t = timer_ticks();
        if (t - reported < 200u) {
            continue;
        }
        reported = t;

        console_lock();

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

        /* Bytecode progress, alongside the native figures on the same line so
         * the two can be seen advancing together rather than in turn. */
        uart_puts("  | vm sw=");
        uart_put_dec(task_switch_count(id_vm));
        uart_puts(" insns=");
        uart_put_dec(g_vm.executed);
        uart_puts(" counter=");
        uart_put_dec(vm_counter());
        uart_puts(" fault=");
        uart_puts(vm_fault_name(vm_fault(&g_vm)));

        /* Mutual exclusion, measured. Each worker bumps g_shared once per
         * iteration under the lock, so g_shared must track their combined
         * counts. The residual is sampling skew — the three values are read at
         * three different instants while both workers run — not lost updates. */
        uint32_t bumps  = g_bumps_a + g_bumps_b;
        uint32_t shared = g_shared;
        uart_puts("\n        lock owner=");
        uart_put_dec((unsigned int)mutex_owner(&g_shared_lock));
        uart_puts(" waiters=");
        uart_put_hex(g_shared_lock.waiters);
        uart_puts(" acq=");
        uart_put_dec(g_shared_lock.acquisitions);
        uart_puts(" contended=");
        uart_put_dec(g_shared_lock.contentions);
        uart_puts(" err=");
        uart_put_dec(g_shared_lock.errors);
        uart_puts(" skew=");
        uart_put_dec(bumps > shared ? bumps - shared : shared - bumps);
        uart_puts("  | touch g/w=");
        uart_put_dec(vm_touch_given());
        uart_putc('/');
        uart_put_dec(vm_touch_withheld());
        uart_puts("  | vp calls=");
        uart_put_dec(vm_viewport_calls());
        uart_puts(" escapes=");
        uart_put_dec(vm_viewport_escapes());
        uart_puts(" maxy=");
        uart_put_dec(vm_viewport_max_y());
        uart_puts("/");
        uart_put_dec(DISP_H);
        uart_puts("  touch s/e=");
        uart_put_dec(touch_samples());
        uart_putc('/');
        uart_put_dec(touch_events());
        uart_puts(" irq=");
        uart_put_dec(touch_irq_lows());
        uart_puts(" z1max=");
        uart_put_dec(touch_max_z1());
        uart_puts(" z2min=");
        uart_put_dec(touch_min_z2());
        uart_puts(" zmax=");
        uart_put_dec(touch_max_z());
        uart_puts(" rx=");
        uart_put_dec(touch_rx_min());
        uart_putc('-');
        uart_put_dec(touch_rx_max());
        uart_puts(" ry=");
        uart_put_dec(touch_ry_min());
        uart_putc('-');
        uart_put_dec(touch_ry_max());
        uart_puts(" first=");
        uart_put_dec(touch_rx_first());
        uart_putc(',');
        uart_put_dec(touch_ry_first());
        uart_puts(" last=");
        uart_put_dec(g_last_rawx);
        uart_putc(',');
        uart_put_dec(g_last_rawy);
        uart_puts("->");
        uart_put_dec(g_last_x);
        uart_putc(',');
        uart_put_dec(g_last_y);
        uart_puts("  states=");
        for (int i = 0; i < 7; i++) {
            uart_put_dec((unsigned int)task_state_of(i));
        }
        uart_puts("\n");

        console_unlock();
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

/* ---- Milestone 4 self-test -------------------------------------------- */
/*
 * Hand-encoded probes. Each is a few instructions whose only purpose is to do
 * something illegal, so they are written as bytes rather than assembled: the
 * point is that a *malformed* program is contained, and routing them through
 * the assembler would only prove that well-formed programs are.
 *
 * Encoding is { opcode, a, b, c }; see vm.h.
 */
static const uint8_t probe_bounds[] = {
    0x03, 0x01, 0xF0, 0xFF,   /* ldi r1, 0xFFF0   — far outside any arena */
    0x42, 0x00, 0x01, 0x00,   /* stw r0, r1, 0    — must fault, not write */
};
static const uint8_t probe_div0[] = {
    0x03, 0x01, 0x0A, 0x00,   /* ldi r1, 10 */
    0x03, 0x02, 0x00, 0x00,   /* ldi r2, 0  */
    0x13, 0x03, 0x01, 0x02,   /* div r3, r1, r2 */
};
static const uint8_t probe_opcode[] = {
    0xFF, 0x00, 0x00, 0x00,   /* not an instruction */
};
static const uint8_t probe_reg[] = {
    0x02, 0x10, 0x00, 0x00,   /* mov r16, r0 — index out of range */
};
static const uint8_t probe_ret[] = {
    0x34, 0x00, 0x00, 0x00,   /* ret with an empty call stack */
};
static const uint8_t probe_align[] = {
    0x03, 0x01, 0x01, 0x00,   /* ldi r1, 1 */
    0x40, 0x02, 0x01, 0x00,   /* ldw r2, r1, 0 — offset 1, misaligned */
};
static const uint8_t probe_spin[] = {
    0x30, 0x00, 0xFF, 0xFF,   /* jmp -1 — branches to itself forever */
};

static uint8_t *arena_ptr(int id)
{
    uint32_t base = 0;
    return (arena_bounds(id, &base, 0) == 0) ? (uint8_t *)base : 0;
}

static void load_program(int id, const uint8_t *prog, uint32_t len)
{
    uint8_t *dst = arena_ptr(id);
    for (uint32_t i = 0; i < len; i++) {
        dst[i] = prog[i];
    }
}

/* Runs a probe to completion and reports whether it faulted as expected. */
static int expect_fault(int id, const uint8_t *prog, uint32_t len, int want)
{
    vm_t vm;
    load_program(id, prog, len);
    if (vm_init(&vm, id) != 0) {
        return 0;
    }
    int r = vm_run(&vm, 1000);
    return (r == VM_RUN_FAULTED) && (vm_fault(&vm) == want);
}

static void m4_selftest(void)
{
    int id = arena_create(2048);
    if (id < 0) {
        uart_puts("  [M4] arena_create failed\n");
        return;
    }

    /* --- the demonstration program --------------------------------------
     * Deliberately run with a small quantum so it must be resumed several
     * times. That is the preemption boundary the roadmap asks for: a host task
     * can be descheduled between any two instructions without the program
     * knowing. */
    vm_t vm;
    load_program(id, vm_demo, VM_DEMO_LEN);
    vm_init(&vm, id);

    uint32_t resumes = 0;
    int r;
    while ((r = vm_run(&vm, 16)) == VM_RUN_QUANTUM) {
        resumes++;
        if (resumes > 1000u) {
            break;                  /* runaway guard for the test itself */
        }
    }

    uart_puts("  [4a] program  : ");
    uart_puts(r == VM_RUN_HALTED ? "HALTED ok" : "DID NOT HALT");
    uart_puts("  insns=");
    uart_put_dec(vm.executed);
    uart_puts(" resumes=");
    uart_put_dec(resumes);
    uart_puts(" status=");
    uart_put_dec(vm.exit_status);
    uart_puts(" fault=");
    uart_puts(vm_fault_name(vm_fault(&vm)));
    uart_puts("\n");

    /* --- containment ------------------------------------------------------
     * Every one of these is a program doing something the kernel must survive.
     * If any of them took the board down, nothing after this line would print. */
    struct { const uint8_t *p; uint32_t n; int want; const char *name; } probes[] = {
        { probe_bounds, sizeof probe_bounds, VM_FAULT_BOUNDS,     "bounds" },
        { probe_div0,   sizeof probe_div0,   VM_FAULT_DIV0,       "div0"   },
        { probe_opcode, sizeof probe_opcode, VM_FAULT_OPCODE,     "opcode" },
        { probe_reg,    sizeof probe_reg,    VM_FAULT_REG,        "reg"    },
        { probe_ret,    sizeof probe_ret,    VM_FAULT_RET,        "ret"    },
        { probe_align,  sizeof probe_align,  VM_FAULT_ALIGN,      "align"  },
    };

    int all_ok = 1;
    uart_puts("  [4b] faults   : ");
    for (unsigned i = 0; i < sizeof probes / sizeof probes[0]; i++) {
        int ok = expect_fault(id, probes[i].p, probes[i].n, probes[i].want);
        all_ok &= ok;
        uart_puts(probes[i].name);
        uart_puts(ok ? "=ok " : "=FAIL ");
    }
    uart_puts(all_ok ? " PASS\n" : " FAIL\n");

    /* --- a program that never ends ---------------------------------------
     * The guarantee is that it costs its quantum and nothing more. */
    vm_t spin;
    load_program(id, probe_spin, sizeof probe_spin);
    vm_init(&spin, id);
    int sr = vm_run(&spin, 500);
    uart_puts("  [4c] runaway  : ");
    uart_puts((sr == VM_RUN_QUANTUM && spin.executed == 500u) ? "PASS" : "FAIL");
    uart_puts("  bounded at ");
    uart_put_dec(spin.executed);
    uart_puts(" insns, no fault, kernel alive\n");

    /* --- the two bounds predicates must agree ----------------------------
     * vm_in_bounds() duplicates arena_contains() for speed (UM-CYDOS-010 §5.2).
     * Duplicated logic drifts, so the agreement is tested rather than trusted. */
    uint32_t abase = 0, alen = 0;
    arena_bounds(id, &abase, &alen);
    const uint32_t offs[] = { 0u, 1u, 4u, alen - 1u, alen, alen + 1u, 0xFFFFFFF0u };
    const uint32_t lens[] = { 0u, 1u, 4u, alen, 0xFFFFFFFFu };
    int agree = 1;
    for (unsigned i = 0; i < sizeof offs / sizeof offs[0]; i++) {
        for (unsigned j = 0; j < sizeof lens / sizeof lens[0]; j++) {
            int v = vm_in_bounds(&vm, offs[i], lens[j]);
            int a = arena_contains(id, abase + offs[i], lens[j]);
            /* arena_contains takes an absolute address; for offsets that would
             * wrap when added to the base the two are not comparable, so skip
             * only that case and compare everywhere else. */
            if (offs[i] <= alen && v != a) {
                agree = 0;
            }
        }
    }
    uart_puts("  [4d] predicate: ");
    uart_puts(agree ? "PASS" : "FAIL");
    uart_puts("  vm_in_bounds agrees with arena_contains over ");
    uart_put_dec((unsigned)(sizeof offs / sizeof offs[0] * sizeof lens / sizeof lens[0]));
    uart_puts(" cases\n");

    arena_destroy(id);
}

/* ---- Milestone 5 self-test -------------------------------------------- */
/*
 * Runs single-threaded before the scheduler starts, so the results are
 * deterministic and a failure cannot be blamed on task switching. The live,
 * interactive version of the same thing runs afterwards under the shell.
 *
 * Each block is one exit criterion from UM-CYDOS-007 §7.
 */

static const shell_program_t PROGRAMS[] = {
    { "counter", vm_app_a,     VM_APP_A_LEN,     512u, VM_APP_A_AT_COUNTER  },
    { "squares", vm_app_b,     VM_APP_B_LEN,     512u, VM_APP_B_AT_SQUARE   },
    { "rogue",   vm_app_rogue, VM_APP_ROGUE_LEN, 256u, VM_APP_ROGUE_AT_COUNTER },
    { "draw",    vm_app_draw,  VM_APP_DRAW_LEN,  512u, VM_APP_DRAW_AT_NAME },
    { "gfxrogue", vm_app_gfx_rogue, VM_APP_GFX_ROGUE_LEN, 256u, 0u },
    { "paint",   vm_app_paint, VM_APP_PAINT_LEN, 512u, 0u },
};
#define PROGRAM_COUNT ((int)(sizeof PROGRAMS / sizeof PROGRAMS[0]))

static int str_same(const char *a, const char *b)
{
    while (*a && *b) {
        if (*a++ != *b++) {
            return 0;
        }
    }
    return *a == *b;
}

/* Start a registered program by NAME.
 *
 * The table is indexed by position everywhere else and that is exactly how the
 * paint application failed to launch: PROGRAMS grew from three entries to six,
 * a hard-coded [4] silently became gfxrogue instead of paint, and the symptom
 * was a screen flickering red and white with no obvious connection to the
 * indexing. A name cannot drift when the table is reordered. */
static int start_program(const char *name)
{
    for (int i = 0; i < PROGRAM_COUNT; i++) {
        if (str_same(PROGRAMS[i].name, name)) {
            return app_start(PROGRAMS[i].name, PROGRAMS[i].img, PROGRAMS[i].len,
                             PROGRAMS[i].arena_bytes, PROGRAMS[i].publish_off);
        }
    }
    uart_puts("  [boot] no such program: ");
    uart_puts(name);
    uart_puts("\n");
    return -1;
}


static void m5_selftest(void)
{
    uint32_t base_free = heap_free_bytes();

    /* --- Criterion 1: two applications interleave ------------------------ */
    int a = app_start(PROGRAMS[0].name, PROGRAMS[0].img, PROGRAMS[0].len,
                      PROGRAMS[0].arena_bytes, PROGRAMS[0].publish_off);
    int b = app_start(PROGRAMS[1].name, PROGRAMS[1].img, PROGRAMS[1].len,
                      PROGRAMS[1].arena_bytes, PROGRAMS[1].publish_off);

    for (int i = 0; i < 200; i++) {
        app_tick(300);
    }

    uint32_t ia = app_instructions(a), ib = app_instructions(b);
    uint32_t pa = app_published(a),    pb = app_published(b);

    /* Both must have run, and both must have run the SAME amount: app_tick
     * hands out an identical quantum to each. Equal instruction counts with
     * unequal published values is exactly right — B does more work per
     * iteration, so it advances more slowly in its own terms. */
    int c1 = (app_state(a) == APP_RUNNING) && (app_state(b) == APP_RUNNING) &&
             (ia == ib) && (ia > 0u) && (pa > 0u) && (pb > 0u) && (pa != pb);

    uart_puts("  [1] interleave: ");
    uart_puts(c1 ? "PASS" : "FAIL");
    uart_puts("  a insns=");
    uart_put_dec(ia);
    uart_puts(" count=");
    uart_put_dec(pa);
    uart_puts("  |  b insns=");
    uart_put_dec(ib);
    uart_puts(" square=");
    uart_put_dec(pb);
    uart_puts("\n");

    /* --- Criterion 2: a rogue application is terminated, alone ----------- */
    uint32_t a_before = app_published(a), b_before = app_published(b);

    int rg = app_start(PROGRAMS[2].name, PROGRAMS[2].img, PROGRAMS[2].len,
                       PROGRAMS[2].arena_bytes, PROGRAMS[2].publish_off);

    for (int i = 0; i < 100 && app_state(rg) == APP_RUNNING; i++) {
        app_tick(300);
    }
    for (int i = 0; i < 50; i++) {
        app_tick(300);      /* let the survivors keep running afterwards */
    }

    /* The rogue must be gone, its neighbours must be untouched and still
     * advancing, and the fault must name the arena boundary exactly. */
    int c2 = (app_state(rg) == APP_FAULTED) &&
             (app_fault(rg) == VM_FAULT_BOUNDS) &&
             (app_fault_detail(rg) == PROGRAMS[2].arena_bytes) &&
             (app_state(a) == APP_RUNNING) && (app_state(b) == APP_RUNNING) &&
             (app_published(a) > a_before) && (app_published(b) > b_before);

    uart_puts("  [2] isolation : ");
    uart_puts(c2 ? "PASS" : "FAIL");
    uart_puts("  rogue ");
    uart_puts(app_state_name(rg));
    uart_puts(" at offset ");
    uart_put_dec(app_fault_detail(rg));
    uart_puts(" = arena size; neighbours still running and advancing\n");

    /* --- Criterion 3: termination releases the arena completely ---------- */
    app_kill(a);
    app_kill(b);

    uint32_t after = heap_free_bytes();
    int c3 = (after == base_free) && (heap_check() == 0) &&
             (app_live_count() == 0);

    uart_puts("  [3] release   : ");
    uart_puts(c3 ? "PASS" : "FAIL");
    uart_puts("  heap ");
    uart_put_dec(after);
    uart_puts("/");
    uart_put_dec(base_free);
    uart_puts(" B, live=");
    uart_put_dec((unsigned int)app_live_count());
    uart_puts(", check=");
    uart_put_dec((unsigned int)heap_check());
    uart_puts("\n");
}

/* ---- locking self-test -------------------------------------------------
 *
 * Single-threaded, before the scheduler starts. Contention itself cannot be
 * tested here — that needs two tasks, and the reporter measures it at runtime.
 * What CAN be established deterministically is that the primitives behave as
 * specified, including the cases a caller gets wrong.
 */
static void m6_selftest(void)
{
    /* --- mutex semantics ------------------------------------------------- */
    mutex_t m;
    mutex_init(&m);

    int free_at_start = (mutex_owner(&m) == MUTEX_FREE);

    mutex_lock(&m);
    int held      = (mutex_owner(&m) == task_current());
    mutex_lock(&m);                     /* recursive — must not deadlock */
    int recursed  = (m.depth == 2u);
    mutex_unlock(&m);
    int still_held = (mutex_owner(&m) == task_current()) && (m.depth == 1u);
    mutex_unlock(&m);
    int released  = (mutex_owner(&m) == MUTEX_FREE);

    /* Unlocking something this context does not hold must be refused and
     * counted, not acted on: clearing another owner would admit two holders. */
    uint32_t err_before = m.errors;
    m.owner = 99;                       /* pretend someone else holds it */
    mutex_unlock(&m);
    int refused = (m.errors == err_before + 1u) && (m.owner == 99);
    m.owner = MUTEX_FREE;

    int got  = mutex_try_lock(&m);
    m.owner  = 99;                      /* now genuinely held elsewhere */
    int denied = (mutex_try_lock(&m) == 0);
    m.owner  = MUTEX_FREE;
    m.depth  = 0;

    int mutex_ok = free_at_start && held && recursed && still_held && released &&
                   refused && got && denied;

    uart_puts("  [6b] mutex    : ");
    uart_puts(mutex_ok ? "PASS" : "FAIL");
    uart_puts("  recursive depth, ownership, non-owner unlock refused (");
    uart_put_dec(m.errors);
    uart_puts("), try_lock both ways\n");
}

/* Runs from the reporter task, NOT from m6_selftest(). The first version was
 * called before timer_start(), where timer_ticks() is 0 and stays 0 whatever
 * masking does — a test that could only ever report PASS by accident. It has to
 * run with the tick live to mean anything. */
static void m6_critical_test(void)
{
    /* --- a critical section really does mask the tick -------------------- */
    uint32_t t0 = timer_ticks();
    uint32_t crit = crit_enter();
    uint32_t start = xt_ccount();
    while ((xt_ccount() - start) < (TICK_INTERVAL_CYCLES * 2u)) {
        /* Two full tick periods. If masking does not work, g_ticks moves. */
    }
    uint32_t t_masked = timer_ticks();
    crit_exit(crit);

    /* The deadline passed while masked, so the interrupt is pending and fires
     * as soon as the level drops. Waiting for it proves the tick was deferred
     * rather than lost — masking that silently dropped ticks would keep the
     * scheduler running but make every timeout wrong. */
    uint32_t spin = xt_ccount();
    while (timer_ticks() == t_masked && (xt_ccount() - spin) < TICK_INTERVAL_CYCLES * 4u) {
    }
    uint32_t t_after = timer_ticks();

    int crit_ok = (t_masked == t0) && (t_after > t_masked);
    uart_puts("  [6a] critical : ");
    uart_puts(crit_ok ? "PASS" : "FAIL");
    uart_puts("  ticks held at ");
    uart_put_dec(t_masked);
    uart_puts(" across 2 periods, resumed at ");
    uart_put_dec(t_after);
    uart_puts("\n");
}


/* ---- status display -----------------------------------------------------
 * Draws what the kernel knows about itself onto the panel. Everything is drawn
 * through a 480-byte span buffer; there is no framebuffer anywhere in the
 * system (UM-CYDOS-010 §7.2).
 *
 * Redrawing only the value fields rather than the whole screen keeps each
 * update to a few hundred spans instead of 76,800 pixels, which matters when
 * the SPI is bit-banged.
 */
static void draw_num(uint32_t x, uint32_t y, uint32_t v, uint16_t fg)
{
    char buf[12];
    int  i = 0;
    if (v == 0u) {
        buf[i++] = '0';
    }
    while (v > 0u && i < 11) {
        buf[i++] = (char)('0' + (v % 10u));
        v /= 10u;
    }
    char out[12];
    int  n = 0;
    while (i-- > 0) {
        out[n++] = buf[i];
    }
    while (n < 10) {
        out[n++] = ' ';         /* pad, so a shrinking number leaves no residue */
    }
    out[n] = 0;
    display_text(x, y, out, fg, COLOR_BLACK, 1);
}

static void task_display(void)
{
    display_fill_rect(0, 0, DISP_W, 22, COLOR_BLUE);
    display_text(6, 7, "cyd-os", COLOR_WHITE, COLOR_BLUE, 2);

    display_text(6,  40, "ticks",  COLOR_GREY, COLOR_BLACK, 1);
    display_text(6,  56, "tasks",  COLOR_GREY, COLOR_BLACK, 1);
    display_text(6,  72, "apps",   COLOR_GREY, COLOR_BLACK, 1);
    display_text(6,  88, "heap",   COLOR_GREY, COLOR_BLACK, 1);
    display_text(6, 104, "bytecode", COLOR_GREY, COLOR_BLACK, 1);
    display_text(6, 120, "locks",  COLOR_GREY, COLOR_BLACK, 1);

    /* A strip of the panel's colour primaries. If these are wrong, the pixel
     * format or the byte order is wrong, and that is worth seeing immediately
     * rather than inferring from a garbled photograph later. */
    const uint16_t bars[] = { COLOR_RED, COLOR_GREEN, COLOR_BLUE, COLOR_YELLOW,
                              COLOR_CYAN, COLOR_MAGENTA, COLOR_WHITE, COLOR_GREY };
    for (uint32_t i = 0; i < 8; i++) {
        display_fill_rect(i * 30u, DISP_H - 32u, 30u, 32u, bars[i]);
    }

    uint32_t frame = 0;
    for (;;) {
        draw_num(90,  40, timer_ticks(),        COLOR_WHITE);
        draw_num(90,  56, (uint32_t)7,          COLOR_WHITE);
        draw_num(90,  72, (uint32_t)app_live_count(), COLOR_WHITE);
        draw_num(90,  88, heap_free_bytes(),    COLOR_WHITE);
        draw_num(90, 104, g_vm.executed,        COLOR_GREEN);
        draw_num(90, 120, g_shared_lock.acquisitions, COLOR_CYAN);

        /* A marker that moves every frame, so a frozen display is obvious even
         * when every number happens to look plausible. */
        display_fill_rect((frame % 12u) * 20u, 150u, 20u, 6u, COLOR_YELLOW);
        display_fill_rect(((frame + 11u) % 12u) * 20u, 150u, 20u, 6u, COLOR_BLACK);
        frame++;

        uint32_t until = timer_ticks() + 25u;
        while (timer_ticks() < until) {
            task_yield();
        }
    }
}


/* ---- touch -------------------------------------------------------------
 *
 * Reports every reading over UART for the first few touches and draws a
 * crosshair where it thinks the finger is.
 *
 * The UART trace is the actual verification. A crosshair in the wrong place and
 * a controller returning nothing look identical on the glass, and the display
 * driver already cost three commits to a defect that was invisible because only
 * the picture was being checked (UM-CYDOS-016 §3.4). Raw ADC values distinguish
 * "not answering" from "answering, mapped wrongly".
 */
static void task_touch(void)
{
    touch_state_t t;
    uint32_t traced = 0;

    for (;;) {
        int down = touch_read(&t);

        /* Trace the first few samples whether or not anything is touching.
         * "no events" is produced both by an untouched panel and by a bus that
         * never answers, and only the raw channels tell them apart. */
        if (traced < 4u) {
            traced++;
            console_lock();
            uart_puts("  [touch probe] z1=");
            uart_put_dec(t.z1);
            uart_puts(" z2=");
            uart_put_dec(t.z2);
            uart_puts(" rawx=");
            uart_put_dec(t.raw_x);
            uart_puts(" rawy=");
            uart_put_dec(t.raw_y);
            uart_puts(down ? "  DOWN\n" : "  up\n");
            console_unlock();
        }

        if (down) {
            g_last_rawx = t.raw_x;
            g_last_rawy = t.raw_y;
            g_last_x    = t.x;
            g_last_y    = t.y;
            g_last_z    = t.z;

            if (traced < 24u) {
                traced++;
                console_lock();
                uart_puts("  [touch] X=");
                for (int i = 0; i < 4; i++) {
                    uart_put_dec(t.sx[i]);
                    uart_putc(i == 3 ? ' ' : '/');
                }
                uart_puts(" Y=");
                for (int i = 0; i < 4; i++) {
                    uart_put_dec(t.sy[i]);
                    uart_putc(i == 3 ? ' ' : '/');
                }
                uart_puts(" raw=");
                uart_put_dec(t.raw_x);
                uart_putc(',');
                uart_put_dec(t.raw_y);
                uart_puts("  z=");
                uart_put_dec(t.z);
                uart_puts("  ->  x=");
                uart_put_dec(t.x);
                uart_putc(',');
                uart_put_dec(t.y);
                uart_puts("\n");
                console_unlock();
            }

            /* No cursor. The kernel used to draw one here, which meant it
             * was painting over application viewports — exactly what it
             * forbids applications from doing to each other. Not unsafe, since
             * the kernel is trusted, but wrong about ownership: those pixels
             * belong to whoever owns that strip.
             *
             * It also destroyed what it drew over. An application's yellow dots
             * were being replaced by the black squares this cursor left behind
             * as it erased its own previous position.
             *
             * An application that wants pointer feedback now draws it itself,
             * through SYS TOUCH and SYS FILL, in its own coordinates and inside
             * its own viewport. That is the whole point of the syscall.
             */
        }

        /* Polled at roughly 30 Hz. Fast enough to track a finger, slow enough
         * that the panel is not being clocked continuously for nothing. */
        uint32_t until = timer_ticks() + 1u;
        while (timer_ticks() < until) {
            task_yield();
        }
    }
}

/* Hosts every application. One native task drives the third scheduling level;
 * the applications inside it are preempted by their quantum, and this task is
 * itself preempted by the timer. */
static void task_apps(void)
{
    for (;;) {
        app_tick(2000);
        if (app_live_count() == 0) {
            task_yield();       /* nothing to run — do not spin at full tilt */
        }
    }
}

static void task_shell(void)
{
    shell_begin();
    for (;;) {
        shell_poll();
        task_yield();
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

    console_init();
    mutex_init(&g_shared_lock);

    touch_init();

    uart_puts("  display      : ");
    display_init();
    uart_puts("init ok, bytes=");
    uart_put_dec(display_bytes_written());
    uart_puts(" fullscreen=");
    uart_put_dec(display_fill_cycles() / 80000u);
    uart_puts(" ms clk=");
    uart_put_hex(display_spi_clock_reg());
    uart_puts(" dport=");
    uart_put_hex(display_dport_reg());
    uart_puts("\n");

    m3_selftest();
    m4_selftest();
    m5_selftest();
    m6_selftest();

    /* The VM's arena is created here, on the boot path, because the heap has no
     * locking and this is the last moment at which exactly one context exists
     * (UM-CYDOS-010 §8). The task only ever runs an already-initialised VM. */
    g_vm_arena = arena_create(1024);
    if (g_vm_arena >= 0 && arena_bounds(g_vm_arena, &g_vm_base, 0) == 0) {
        load_program(g_vm_arena, vm_spin, VM_SPIN_LEN);
        if (vm_init(&g_vm, g_vm_arena) != 0) {
            g_vm_arena = -1;
        }
    }
    uart_puts("  vm arena     : id=");
    uart_put_dec((unsigned int)g_vm_arena);
    uart_puts(" base=");
    uart_put_hex(g_vm_base);
    uart_puts(" program=");
    uart_put_dec(VM_SPIN_LEN);
    uart_puts(" B\n");

    /* Start the two well-behaved applications for the live system. The rogue is
     * left for the operator to launch from the shell — it is a demonstration,
     * not something that should be running by default. */
    shell_register(PROGRAMS, PROGRAM_COUNT);
    start_program("paint");
    start_program("draw");

    id_report = task_create("report", task_report);
    id_a      = task_create("worker-a", task_a);
    id_b      = task_create("worker-b", task_b);
    id_vm     = task_create("vm-host", task_vm);
    id_apps   = task_create("app-host", task_apps);
    id_shell  = task_create("shell", task_shell);

    /* Created last and registered as idle, so it is outside the round robin and
     * chosen only when every other task is blocked. Without it, a moment where
     * all tasks are waiting has nothing to switch to. */
    id_disp   = task_create("display", task_display);
    id_touch  = task_create("touch", task_touch);
    id_idle   = task_create("idle", task_idle);
    task_set_idle(id_idle);
    uart_puts("  tasks        : report=");
    uart_put_dec((unsigned int)id_report);
    uart_puts(" a=");
    uart_put_dec((unsigned int)id_a);
    uart_puts(" b=");
    uart_put_dec((unsigned int)id_b);
    uart_puts(" vm=");
    uart_put_dec((unsigned int)id_vm);
    uart_puts(" apps=");
    uart_put_dec((unsigned int)id_apps);
    uart_puts(" shell=");
    uart_put_dec((unsigned int)id_shell);
    uart_puts(" disp=");
    uart_put_dec((unsigned int)id_disp);
    uart_puts(" touch=");
    uart_put_dec((unsigned int)id_touch);
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
