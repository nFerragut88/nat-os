/* nat-os — Milestone 2: preemptive task switching.
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
#include "flash.h"
#include "store.h"
#include "sd.h"
#include "app.h"
#include "console.h"
#include "ipc.h"
#include "display.h"
#include "raycast.h"
#include "desktop.h"
#include "notes.h"
#include "term.h"
#include "audio.h"
#include "messages.h"
#include "calib.h"
#include "touch.h"
#include "intr.h"
#include "adc.h"
#include "i2c.h"
#include "critical.h"
#include "mutex.h"
#include "shell.h"
#include "vm.h"
#include "device.h"
#include "generated/demo.h"
#include "generated/spin.h"
#include "generated/app_a.h"
#include "generated/app_b.h"
#include "generated/app_rogue.h"
#include "generated/app_draw.h"
#include "generated/app_gfx_rogue.h"
#include "generated/app_paint.h"
#include "generated/app_blit.h"
#include "generated/app_dev.h"
#include "generated/app_evt.h"
#include "generated/app_ping.h"
#include "generated/app_pong.h"
#include "uart.h"
#include "timer.h"
#include "task.h"
#include "watchdog.h"
#include "clock.h"
#include "panic.h"
#include "xtensa.h"

#define TICK_INTERVAL_CYCLES  800000u   /* ~10 ms at the measured ~80 MHz */

/* osi_wait.h derives the OSI forever-cap in ticks from this period. It
 * cannot include kmain.c, so it restates the value and this checks it --
 * a tick change must not silently rescale how long the radio waits. */
#include "osi_wait.h"
_Static_assert(TICK_INTERVAL_CYCLES == OSI_TICK_CYCLES,
               "tick period changed; osi_wait.h still assumes the old one");

extern char _vecbase;

static volatile uint32_t work_a_count, work_a_bad;
static volatile uint32_t work_b_count, work_b_bad;

static int id_report, id_a, id_b, id_vm, id_apps, id_shell, id_idle, id_disp, id_touch;

/* Exposed so the shell can retune touch scheduling without a reflash. */
int kmain_touch_task_id(void) { return id_touch; }

/* Last touch seen, latched so the reporter can show it whenever it happens to
 * run. Without this, confirming a real touch needs a serial capture to coincide
 * with a finger, which is not a test anyone can repeat reliably. */
static volatile uint32_t g_last_rawx, g_last_rawy, g_last_x, g_last_y, g_last_z;

/* Emit one compact pressure line per report. Off by default: it exists to
 * answer the phantom-touch question over a long unattended run, and an
 * instrument that is always on is one more thing competing for the console.
 *
 * Defined here rather than beside g_touch_events_off further down, because
 * task_report() reads it and is above that point in the file. */
volatile int g_ztrack = 0;

/* Defined below with the other self-tests, but called from the reporter, which
 * is the only context where a running tick makes it meaningful. */
static void m6_critical_test(void);

/* Touch polling policy, tunable at runtime via the 'touchcfg' command. Default
 * matches the last configuration the 3D view was confirmed good in. */
volatile uint32_t g_touch_sleep_ticks = 1;      /* 1 tick = 10 ms, 100 Hz */

/* Stops every drawer in the display task, leaving whatever is on the panel and
 * in the framebuffer exactly as it stands.
 *
 * This is a measurement tool. The raycaster rewrites the entire framebuffer
 * every frame as the camera moves, so comparing the buffer before and after
 * some event always differs for innocent reasons. With the renderer frozen,
 * ANY change to the buffer was made by something else -- which is precisely
 * the open question about why launching a program repairs a garbled view. */
volatile int g_display_frozen = 0;

/* Creates a task or stops the kernel. See the note at the first call site. */
static int must_create(const char *name, task_entry_fn entry)
{
    int id = task_create(name, entry);
    if (id < 0) {
        kernel_panic_msg("task table full — raise TASK_MAX", 0);
    }
    return id;
}

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

/* How often each worker yields the CPU.
 *
 * These tasks are M2 artefacts: they prove a context switch preserves registers
 * across arbitrary suspension, and they have now done so 460,800 times with
 * corrupt=0. The claim is established; what remains is a REGRESSION CHECK, and
 * a regression check does not need most of the machine.
 *
 * At 2048 they were consuming the CPU budget that would otherwise be frame
 * rate — the renderer measured 31 ms of work inside a 320 ms frame, idle 90% of
 * the time and waiting for a share it could not get. Sleeping far more often
 * costs them iteration count and costs the invariant nothing: a worker that
 * yields more frequently is preempted MORE times per unit of CPU, which is
 * precisely the property under test.
 *
 * Deleting them was the alternative. Kept instead because guards=ok and
 * corrupt=0 are the only continuous evidence that the scheduler still does what
 * UM-NATOS-009 says it does. */
#define WORKER_SLEEP_EVERY 128u

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
 * has no locking (UM-NATOS-010 §8) and allocating from task context would be
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

        /* Yield the CPU periodically rather than spinning flat out.
         *
         * These tasks exist to prove the context switch preserves registers
         * across arbitrary suspension, and that claim needs them to run
         * CONTINUOUSLY, not CONSTANTLY. Spinning at full rate cost the renderer
         * more than half its frame rate and bought no extra evidence; sleeping
         * briefly every few thousand iterations still exercises thousands of
         * preemptions per second. */
        if ((n % WORKER_SLEEP_EVERY) == 0u) {
            task_sleep(1u);
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

        /* Guards are enforced in the scheduler now, so reaching this line at
         * all means every guard is intact — a broken one panics rather than
         * printing. What is worth reporting is the MARGIN, and specifically the
         * margin on the worst task rather than on the two that were easiest to
         * name. The old line covered three of the eight and omitted the display
         * task, which carries the deepest call chain in the kernel. */
        int tight = task_stack_tightest();
        uart_puts("  tightest stack=");
        uart_puts(task_name(tight));
        uart_putc(' ');
        uart_put_dec(task_stack_headroom(tight) * 4u);
        uart_puts("/2048 B free");

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
        uart_puts("  | ipc s/d/r=");
        uart_put_dec(ipc_sent());
        uart_putc('/');
        uart_put_dec(ipc_delivered());
        uart_putc('/');
        uart_put_dec(ipc_refused());
        uart_puts(" badbuf=");
        uart_put_dec(vm_ipc_bad_buffer());
        uart_puts("  | fb=");
        uart_puts(raycast_framebuffer() ? "on" : "off");
        uart_puts("  | ray us m/c/b=");
        uart_put_dec(raycast_us_march());
        uart_putc('/');
        uart_put_dec(raycast_us_compose());
        uart_putc('/');
        uart_put_dec(raycast_us_blit());
        uart_puts(" f/c=");
        uart_put_dec(raycast_frames());
        uart_putc('/');
        uart_put_dec(raycast_columns());
        uart_puts("  | cam=");
        uart_put_dec(raycast_cam_x());
        uart_putc(',');
        uart_put_dec(raycast_cam_y());

        uart_puts("  | blits=");
        uart_put_dec(vm_blits());
        /* Compact: the first/last cell pair is what distinguishes a bad touch
         * reading from sampling at the wrong moment, and it costs two numbers
         * to keep that question answerable without another build. */
        uart_puts("  | drawskip=");
        uart_put_dec(vm_draw_skipped());

        uart_puts("  | dblk disp/apps/touch/shell=");
        uart_put_dec(display_lock_blocked_of(id_disp));
        uart_putc('/');
        uart_put_dec(display_lock_blocked_of(id_apps));
        uart_putc('/');
        uart_put_dec(display_lock_blocked_of(id_touch));
        uart_putc('/');
        uart_put_dec(display_lock_blocked_of(id_shell));

        uart_puts("  | dlock blk/hold ms=");
        uart_put_dec(display_lock_blocked_ms());
        uart_putc('/');
        uart_put_dec(display_lock_hold_ms());
        uart_puts(" takes=");
        uart_put_dec(display_lock_takes());
        uart_puts(" cont=");
        uart_put_dec(display_lock_contentions());

        uart_puts("  | fair maxwait=");
        uart_put_dec(task_max_wait());
        uart_puts(" rescues=");
        uart_put_dec(task_age_rescues());

        uart_puts("  | desk sel=");
        uart_put_dec((unsigned int)desktop_sel());
        uart_puts(" cell f/l=");
        /* -1 means "no press recorded yet". Printed as a dash rather than cast
         * to unsigned, where it reads as 4294967295 and looks like corruption. */
        if (desktop_first_cell() < 0) { uart_putc('-'); }
        else { uart_put_dec((unsigned int)desktop_first_cell()); }
        uart_putc('/');
        if (desktop_last_cell() < 0) { uart_putc('-'); }
        else { uart_put_dec((unsigned int)desktop_last_cell()); }

        {
            extern uint32_t g_overlay_calls, g_overlay_skips;
            uart_puts("  | overlay c/s=");
            uart_put_dec(g_overlay_calls);
            uart_putc('/');
            uart_put_dec(g_overlay_skips);
        }
        uart_puts("  act/tap/open=");
        uart_put_dec((unsigned int)desktop_active());
        uart_putc('/');
        uart_put_dec(desktop_taps());
        uart_putc('/');
        uart_put_dec(desktop_opens());

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
        uart_puts(" wdt f/s=");
        uart_put_dec(watchdog_feeds());
        uart_putc('/');
        uart_put_dec(watchdog_starved());
        /* [step 260] cfg= readback REMOVED for the A/B. cap31 -- the first
         * run carrying it -- is the first run in which wifiinit stalls at
         * [blobtask], and every run since has stalled identically. */
        uart_puts("  states=");
        for (int i = 0; i < 7; i++) {
            uart_put_dec((unsigned int)task_state_of(i));
        }
        uart_puts("\n");

        /* One line per report, on its own, machine-parseable.
         *
         * Deliberately NOT appended to the status line above. That line is two
         * kilobytes of counters and a host parser reading a field out of it is
         * one field-order change away from silently reading the wrong number --
         * which is a mistake this project has already paid for twice. A prefix
         * nothing else emits can be grepped without understanding anything
         * else on the console. */
        if (g_ztrack) {
            touch_window_t w;
            touch_window(&w);
            uart_puts("ZTRK t=");
            uart_put_dec(timer_ticks());
            uart_puts(" n=");
            uart_put_dec(w.n);
            uart_puts(" min=");
            uart_put_dec(w.min);
            uart_puts(" max=");
            uart_put_dec(w.max);
            uart_puts(" over=");
            uart_put_dec(w.over);
            uart_puts(" pen=");
            uart_put_dec(w.pen);
            uart_puts(" thr=");
            uart_put_dec(touch_threshold());
            uart_puts(" blips=");
            uart_put_dec(touch_blips());
            uart_puts("\n");
        }

        console_unlock();
    }
}

/* ---- Milestone 3 self-test -------------------------------------------- */
/*
 * Runs single-threaded before the tick is armed, so nothing here can be
 * disturbed by a context switch and a failure cannot be blamed on M2.
 * Each block corresponds to one exit criterion in UM-NATOS-007 §5.
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

    /* NA-002: a size that wraps align_up() must fail like any other.
     *
     * Before the guard in heap.c this SUCCEEDED -- align_up(0xFFFFFFFF) is 0,
     * the search matched the first free block, and the caller got a valid
     * pointer to a few bytes while believing it held 4 GB. Tested here rather
     * than once by hand, because a bug that fails by succeeding is exactly the
     * kind that comes back. Two values: the boundary and the extreme. */
    uint32_t ovf_before = heap_fail_count();
    void *ovf1 = heap_alloc(0xFFFFFFFFu);
    void *ovf2 = heap_alloc(0xFFFFFFF9u);   /* first value that still wraps */
    int ovf_ok = (ovf1 == 0) && (ovf2 == 0) &&
                 (heap_fail_count() == ovf_before + 2u) &&
                 (heap_check() == 0) && (heap_free_bytes() == base_free);
    oom_ok = oom_ok && ovf_ok;

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
    uart_puts("  oversize=NULL overflow=");
    uart_puts(ovf_ok ? "NULL" : "LEAKED");
    uart_puts(" fails=");
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
     * vm_in_bounds() duplicates arena_contains() for speed (UM-NATOS-010 §5.2).
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
 * Each block is one exit criterion from UM-NATOS-007 §7.
 */

/* Device permission bits, by table position in device.c. Named here so the
 * grants below read as capabilities rather than as magic numbers. */
#define P_LIGHT (1u << 0)
#define P_BEEP  (1u << 1)
#define P_STORE (1u << 2)
#define P_I2C   (1u << 3)
#define P_KEYS  (1u << 4)
#define P_ECHO  (1u << 5)
#define P_SD    (1u << 6)

static const shell_program_t PROGRAMS[] = {
    /* Most programs draw, compute and exchange messages. None of that goes
     * through the device table, so most of these hold nothing -- which is the
     * point: a program that was never considered gets no hardware rather than
     * all of it. */
    { "counter", vm_app_a,     VM_APP_A_LEN,     512u, VM_APP_A_AT_COUNTER, DEV_PERM_NONE },
    { "squares", vm_app_b,     VM_APP_B_LEN,     512u, VM_APP_B_AT_SQUARE,  DEV_PERM_NONE },
    { "rogue",   vm_app_rogue, VM_APP_ROGUE_LEN, 256u, VM_APP_ROGUE_AT_COUNTER, DEV_PERM_NONE },
    { "draw",    vm_app_draw,  VM_APP_DRAW_LEN,  512u, VM_APP_DRAW_AT_NAME, DEV_PERM_NONE },
    { "gfxrogue", vm_app_gfx_rogue, VM_APP_GFX_ROGUE_LEN, 256u, 0u, DEV_PERM_NONE },
    { "paint",   vm_app_paint, VM_APP_PAINT_LEN, 512u, 0u, DEV_PERM_NONE },
    { "blit",    vm_app_blit,  VM_APP_BLIT_LEN,  512u, 0u, DEV_PERM_NONE },
    /* The first program that reaches a peripheral. Its arena is larger because
     * it holds a name buffer the kernel writes into. It enumerates the whole
     * table, reads the light sensor, claims a store slot and round-trips a
     * transfer through echo -- and needs exactly those. It does NOT get the
     * speaker, which it used to seize, or the SD card, or the bus. */
    { "dev",     vm_app_dev,   VM_APP_DEV_LEN,   768u, VM_APP_DEV_AT_PUBLISH,
      P_LIGHT | P_STORE | P_ECHO },
    /* The first program the kernel can call into. Its main flow is an empty
     * spin; everything it prints comes from handlers the kernel entered. It
     * needs no device at all -- events arrive without asking. */
    { "evt",     vm_app_evt,   VM_APP_EVT_LEN,   768u, VM_APP_EVT_AT_PUBLISH, DEV_PERM_NONE },
    { "ping",    vm_app_ping,  VM_APP_PING_LEN,  512u, 0u, DEV_PERM_NONE },
    { "pong",    vm_app_pong,  VM_APP_PONG_LEN,  512u, 0u, DEV_PERM_NONE },
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
            int id = app_start(PROGRAMS[i].name, PROGRAMS[i].img,
                               PROGRAMS[i].len, PROGRAMS[i].arena_bytes,
                               PROGRAMS[i].publish_off);
            /* Same grant the shell performs. The boot path and the shell path
             * must agree, or a program started at boot would behave differently
             * from the same program started by typing its name. */
            if (id >= 0) {
                device_grant((uint32_t)id, PROGRAMS[i].perms);
            }
            return id;
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
    /* t0 is printed too, because without it the two halves of this assertion
     * are indistinguishable in the output. "held at 31, resumed at 31" can mean
     * masking worked and the tick never resumed, or masking failed and the
     * second spin found nothing left to wait for — opposite faults, identical
     * line. That ambiguity cost a bisect. */
    uart_puts("  entered at ");
    uart_put_dec(t0);
    uart_puts(", held at ");
    uart_put_dec(t_masked);
    uart_puts(" across 2 periods, resumed at ");
    uart_put_dec(t_after);
    uart_puts("\n");
}



/* ---- animated spectrum strip -------------------------------------------
 *
 * Cycles the bottom band between the eight discrete primaries and a scrolling
 * hue sweep, crossfading between them.
 *
 * The bars started as a diagnostic and keep that job: distinct colours prove
 * the pixel format is right, and their ORDER is what confirms red and blue are
 * not transposed (UM-NATOS-015 §6). At full blend the bars are still there
 * underneath — the crossfade passes through them once per cycle, so the check
 * remains available to anyone watching rather than being traded away for the
 * effect.
 *
 * One row is composed and then blitted 32 times with a source stride of ZERO,
 * so every row reads the same 480 bytes. That costs 15,360 bytes over SPI — about
 * 4 ms at the DMA rate — instead of composing 7,680 pixels individually.
 */
#define SPEC_H     32u
#define SPEC_Y     (DISP_H - SPEC_H)

/* The colour strip must sit below every application strip. Checked here rather
 * than assumed: the strip geometry lives in app.h and this constant does not,
 * so the two can drift without either file looking wrong on its own. */
_Static_assert(SPEC_Y >= APP_VIEW_Y0 + APP_MAX * APP_VIEW_PITCH,
               "the colour strip must not overlap the application strips");
_Static_assert(SPEC_Y + SPEC_H <= DISP_H,
               "the colour strip must fit on the panel");
#define SPEC_STEPS 64u                  /* frames per half-cycle */

static uint16_t g_spec_row[DISP_W];

/* Hue sweep with saturation and value pinned at maximum. Integer only: six
 * linear segments around the colour wheel, which is what a floating-point
 * HSV conversion reduces to at full saturation anyway. */
static uint16_t spectrum_hue(uint32_t h)
{
    h %= 192u;
    uint32_t seg = h / 32u;
    uint32_t f   = (h % 32u) * 8u;      /* 0..248 */
    uint32_t r, g, b;

    switch (seg) {
    case 0:  r = 255;     g = f;       b = 0;       break;
    case 1:  r = 255 - f; g = 255;     b = 0;       break;
    case 2:  r = 0;       g = 255;     b = f;       break;
    case 3:  r = 0;       g = 255 - f; b = 255;     break;
    case 4:  r = f;       g = 0;       b = 255;     break;
    default: r = 255;     g = 0;       b = 255 - f; break;
    }
    return RGB(r, g, b);
}

/* Crossfade in RGB565's own channel widths — 5, 6, 5 bits. Unpacking to 8-bit
 * per channel and back would round twice and band visibly on a gradient. */
static uint16_t spectrum_mix(uint16_t a, uint16_t b, uint32_t t)
{
    uint32_t ar = (a >> 11) & 0x1Fu, ag = (a >> 5) & 0x3Fu, ab = a & 0x1Fu;
    uint32_t br = (b >> 11) & 0x1Fu, bg = (b >> 5) & 0x3Fu, bb = b & 0x1Fu;
    uint32_t u  = 16u - t;

    return (uint16_t)((((ar * u + br * t) / 16u) << 11) |
                      (((ag * u + bg * t) / 16u) <<  5) |
                       ((ab * u + bb * t) / 16u));
}

static void spectrum_region(uint32_t y, uint32_t h, uint32_t frame, uint32_t skew)
{
    static const uint16_t bars[8] = {
        COLOR_RED, COLOR_GREEN, COLOR_BLUE,  COLOR_YELLOW,
        COLOR_CYAN, COLOR_MAGENTA, COLOR_WHITE, COLOR_GREY
    };

    /* Triangle wave: 0 -> 16 -> 0, so the band returns to plain bars once per
     * cycle rather than settling on the gradient. */
    uint32_t pos = frame % (SPEC_STEPS * 2u);
    uint32_t t   = (pos < SPEC_STEPS) ? pos : (SPEC_STEPS * 2u - pos);
    t = (t * 16u) / SPEC_STEPS;

    /* `skew` offsets the hue per band, so the backdrop reads as one continuous
     * sweep running down the panel rather than several bands in lockstep. */
    uint32_t phase = frame * 3u + skew;

    for (uint32_t x = 0; x < DISP_W; x++) {
        uint16_t bar  = bars[(x * 8u) / DISP_W];
        uint16_t grad = spectrum_hue(phase + (x * 192u) / DISP_W);
        g_spec_row[x] = spectrum_mix(bar, grad, t);
    }

    /* Stride 0: every row of the blit reads the same composed row. */
    display_blit(0, y, DISP_W, h, g_spec_row, 0);
}

/* The whole backdrop: header, the status area behind the labels, and the strip
 * along the bottom. The application viewports at 168..280 are deliberately
 * skipped — those pixels belong to the applications, and the kernel painting
 * over them is the ownership mistake removed in UM-NATOS-017 §8.4. */
static void spectrum_backdrop(uint32_t frame)
{
    spectrum_region(0u,      22u,     frame, 0u);
    spectrum_region(22u,     146u,    frame, 48u);
    spectrum_region(SPEC_Y,  SPEC_H,  frame, 96u);
}

/* ---- status display -----------------------------------------------------
 * Draws what the kernel knows about itself onto the panel. Everything is drawn
 * through a 480-byte span buffer; there is no framebuffer anywhere in the
 * system (UM-NATOS-010 §7.2).
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
    /* Nothing static any more: the backdrop is repainted every frame and
     * everything else is drawn over it. */

    /* A strip of the panel's colour primaries. If these are wrong, the pixel
     * format or the byte order is wrong, and that is worth seeing immediately
     * rather than inferring from a garbled photograph later. */
    /* The strip is drawn every frame by spectrum_draw() now, so nothing is
     * painted here. */

    uint32_t frame = 0;
    for (;;) {
        /* The dungeon view owns the top 168 rows. The status text that used to
         * live here is gone from the panel — it is all still on the UART, and a
         * first-person view with numbers pasted over it reads as neither. */
        /* One owner at a time. The launcher repaints only when something
         * changed, so an idle desktop pushes no pixels at all; the raycaster
         * repaints unconditionally because every frame differs. */
        audio_service();    /* ends any beep whose deadline passed */

        /* Calibration owns the whole panel while it runs.
         *
         * calib.c draws its instructions and one cyan cross, then waits for a
         * tap. It had no way to say so, and this loop repainted over it on the
         * very next frame -- so the crosses existed for a fraction of a frame
         * and the user was asked to tap targets they could not see. The touch
         * task already checks calib_running() to feed it RAW coordinates
         * (below); the display task never did.
         *
         * Skipping every drawer here rather than teaching each one about
         * calibration: there are four of them, and a mode that owns the screen
         * should be expressed once. */
        if (calib_running() || g_display_frozen) {
            task_sleep(1u);
            continue;
        }

        if (desktop_active()) {
            desktop_frame();
        } else if (desktop_notes()) {
            notes_frame();
        } else if (desktop_term()) {
            term_frame();
        } else {
            raycast_frame();
        }

        /* Close buttons last, so they sit over whatever drew beneath them. */
        desktop_chrome();
        spectrum_region(SPEC_Y, SPEC_H, frame, 96u);

        frame++;

        /* Fold the frame count into the persistent record periodically.
         *
         * Not every frame: a save is an erase plus a write, which is tens of
         * milliseconds with interrupts masked and a flash-endurance cost besides.
         * At roughly eight frames a second this writes about once every eight
         * minutes, which is a few thousand cycles over a part rated for a
         * hundred thousand.
         *
         * The count is cumulative ACROSS boots, which is what makes it a real
         * test of persistence rather than a second boot counter: it can only be
         * right if the previous value was read back correctly and added to.
         *
         * 256 frames, chosen from the measured rate rather than a guess: the
         * renderer runs at 4.4 frames a second with the framebuffer on (81
         * frames in 18.3 s), so this saves about once a minute. An earlier 512
         * was picked assuming 8 fps and never fired inside a test window at all,
         * which is the whole argument for measuring — an interval nobody
         * exercises is an interval nobody has tested.
         *
         * Once a minute is a few thousand erases over a part rated for a
         * hundred thousand, and the sector is used by nothing else. */
#define STORE_EVERY_FRAMES 256u

#if FLASH_ENABLE
        if ((frame % STORE_EVERY_FRAMES) == 0u) {
            store_set_frames(store_frames() + STORE_EVERY_FRAMES);
            /* Asks before spending 125 ms with interrupts masked. Nothing
             * registers a predicate yet, so today this is store_save() with an
             * extra branch -- the point is that the call site is now the right
             * shape for when the radio can answer. See store.h. */
            store_save_if_allowed();
        }
#endif

        /* 2 ticks.
         *
         * This was 8, and shortening it used to break the system: at 1 tick the
         * watchdog reset the board, and at 2 the kernel stayed up, touch stayed
         * up, and the reporter task was starved to complete silence — twenty
         * seconds, no output, no reset, because the hang detector asks whether
         * ANY distinct switch happened rather than whether every ready task got
         * a turn.
         *
         * The constant was never the problem. It was the only thing bounding
         * how much of the machine a HIGH-priority task could take, and it did
         * that by simply not asking for it. Scheduler ageing (task.h) bounds it
         * properly now, so the value is no longer load-bearing.
         *
         * The value only started mattering once lock contention was fixed. While
         * applications blocked on the draw lock, the renderer lost ~200 ms a
         * frame to rescheduling and this constant was irrelevant — shortening it
         * to 2 changed 3.0 fps into 3.2. With best-effort drawing in place the
         * renderer became sleep-bound instead: 31 ms of work plus 80 ms of sleep
         * is 111 ms, and 9.9 fps was measured against 9.0 predicted.
         *
         * So this is now the limiter, and it is safe to shorten for two reasons
         * that did not hold before: scheduler ageing bounds the starvation it
         * used to cause, and applications no longer block waiting for the panel.
         *
         * Now a yield, for the same reason as the touch task: this was
         * task_sleep(2u) against a task_sleep that never slept, so the renderer
         * has always run flat out and every frame-rate figure above was
         * measured that way. A real 20 ms sleep on top of 31 ms of work took
         * the frame period from ~32 ms to ~51 ms — the fix was correct and the
         * consequence here was a slower view. The yield restores the timing
         * these numbers were taken under. */
        task_yield();
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
 * the picture was being checked (UM-NATOS-016 §3.4). Raw ADC values distinguish
 * "not answering" from "answering, mapped wrongly".
 */
/* How long the touch task waits on PENIRQ before sampling anyway. Three ticks
 * is 30 ms: brisk enough to be usable on its own if the interrupt never fires,
 * and rare enough that an idle panel is not being clocked for nothing. */
#define TOUCH_IDLE_TICKS 3u

/* Stops the touch task delivering events, without stopping the task.
 *
 * The unattended sweep recorded four taps and TWO OPENS at 390 s with nobody in
 * the room, and maxy moving 254 -> 270 -- which is slot 2's bottom edge, so a
 * third program was launched. ping and pong hold slots 0 and 1. Since a program
 * drawing continuously is the one thing known to repair the 3D view, "leave it
 * five minutes and it fixes itself" may simply be "leave it long enough for a
 * phantom tap to open something".
 *
 * That is testable by removing touch and sweeping again. Disabling delivery
 * rather than the task keeps the sampling, and therefore the touch counters,
 * running: if spurious presses are still being READ while nothing acts on them,
 * that is worth seeing rather than hiding. */
volatile int g_touch_events_off = 0;


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

        /* Routed on every sample, pressed or not: a double-tap is two
         * press-RELEASE pairs, so the launcher needs to see the releases. */
        /* Calibration takes every touch while it runs, and takes the RAW
         * channels: it is calibrating the mapping, so feeding it mapped
         * coordinates would fit the result to its own error. */
        if (calib_running()) {
            calib_touch(t.raw_x, t.raw_y, down);
            continue;
        }

        /* Suppress ROUTING only. The sample above still happened, the pressure
         * and raw channels below are still latched, and the touch counters
         * still move -- so a phantom press remains visible in `touch s/e`, rx
         * and ry while being incapable of selecting an icon or opening a
         * program. Skipping the read entirely would hide the very thing under
         * test. */
        if (g_touch_events_off) {
            goto latch;
        }

        /* Close buttons see the touch first. They occupy a column outside every
         * application viewport, so a press there is unambiguous — no consumer
         * below has a claim on those pixels. */
        /* The note pad takes touches before anything else while it owns the
         * region, except the close button — which is checked first below, so a
         * press on the X is never also a keypress. */
        if (down && desktop_chrome_touch(t.x, t.y)) {
            /* Consumed. Deliberately not passed on: a press that closes a
             * program must not also select an icon underneath it. */
        } else if (desktop_notes()) {
            notes_touch(t.x, t.y, down);
        } else if (desktop_term()) {
            term_touch(t.x, t.y, down);
        } else {
            desktop_touch(t.x, t.y, down);
        }

        /* Latch the raw and mapped values on EVERY press, whoever consumes it.
         *
         * These were moved inside the raycaster's branch when the desktop was
         * wired in, which meant that with the launcher active — the normal
         * state — the reporter showed last=0,0 forever. Diagnosing a touch
         * problem with the touch telemetry switched off is not a position to
         * be in twice. */
latch:
        if (down) {
            g_last_rawx = t.raw_x;
            g_last_rawy = t.raw_y;
            g_last_x    = t.x;
            g_last_y    = t.y;
            g_last_z    = t.z;
        }

        if (down && !desktop_active()) {
            /* Steer only from touches in the view itself, so the application
             * strips below keep their own input. */
            if (t.y < RAY_VIEW_H && !g_touch_events_off) {
                if (t.x < RAY_VIEW_W / 3u) {
                    raycast_turn(-2);
                } else if (t.x > (RAY_VIEW_W * 2u) / 3u) {
                    raycast_turn(2);
                }
            }

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

        /* Two rates, chosen by whether anything is happening.
         *
         * While a finger is DOWN, poll every tick. The press interrupt cannot
         * help here — it fired once, at the start — and the release has to be
         * found by sampling, so this is the rate that decides how a drag feels.
         *
         * While UP, wait on PENIRQ with a timeout. A press releases the wait
         * immediately, so idle costs nothing and response is better than the
         * poll ever gave. The timeout is what makes this safe rather than
         * clever: if PENIRQ does not fire — wrong edge, wrong routing, a pad
         * that never asserts — the sleep still expires and this becomes the
         * polling loop it replaced. The fallback IS the old behaviour, which is
         * why the interrupt could be wired in without risking input entirely.
         *
         * The comment this replaces claimed roughly 30 Hz. task_sleep(1) at a
         * 10 ms tick is 100 Hz; the figure had been wrong by 3x since it was
         * written, which is worth noting given how much of this session was
         * spent on frame rates measured against that same clock. */
        /* Polling, deliberately, despite PENIRQ now being a working interrupt.
         *
         * The matrix underneath is verified: an injected edge routes to CPU line
         * 23, reaches _handler_level3, and runs touch_isr (`irqtest`). What is
         * NOT verified is a finger doing the same thing. Real taps register in
         * the driver — 24 touch events, PENIRQ read low 30 times — while the
         * armed edge detector latched nothing, and that gap is unexplained.
         *
         * So the consumer is switched off and the infrastructure kept. Shipping
         * touch input on an interrupt whose end-to-end path has never once been
         * observed to work would be trading a mechanism that demonstrably works
         * for one that only should, and this project has spent enough of its
         * time on measurements that were assumed rather than taken.
         *
         * touch_irq_wait() is intact and is one line from being reinstated. See
         * UM-NATOS-023 for the four failures found on the way here and the
         * evidence for what is still wrong. */
        /* A real 10 ms sleep, at HIGH priority. Both halves matter.
         *
         * This was task_sleep(1u) originally, back when task_sleep neither
         * slept nor yielded -- so the loop polled repeatedly inside whatever
         * slice it got, which is why touch felt continuous. Replacing it with
         * task_yield() looked like a faithful restoration and was not: a yield
         * gives up the CPU after EVERY poll, so the task sampled once per
         * slice instead of many times, and a quick tap fell between samples.
         * The panel started needing a press-and-hold.
         *
         * Sleeping fixes the rate only if the wake is honoured promptly, and
         * at NORMAL it is not: the renderer is HIGH and never sleeps, so a
         * NORMAL task runs when ageing rescues it, about every 300 ms. Hence
         * HIGH here too -- see the priority table in kmain.
         *
         * 100 Hz sampling for one SPI transaction per pass is far less CPU
         * than the old flat-out polling, and unlike it, the rate is a property
         * of the clock rather than of whatever else happens to be running. */
        /* Runtime-tunable, so the two variables can be separated without a
         * reflash each. Touch went from NORMAL+yield to HIGH+sleep in one
         * step and the 3D view started tearing; which half did it is the
         * question, and guessing has already cost two rounds. */
        if (g_touch_sleep_ticks) {
            task_sleep(g_touch_sleep_ticks);
        } else {
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
    uart_puts(" nat-os  milestone 2 — task switching\n");
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

    /* Immediately after vecbase, and before anything that measures time.
     *
     * After vecbase because reaching the PLL means calling a windowed ROM
     * function, and windowed code needs the overflow handlers that line above
     * just installed. Before everything else because every duration this
     * kernel reports is derived from CCOUNT, so a clock changed later would
     * silently rescale measurements taken earlier.
     *
     * This used to be Espressif's bootloader's job, inherited without the
     * kernel knowing it depended on it. UM-NATOS-035's replacement loader does
     * not do it, and the board ran at 40 MHz while every cycle-derived
     * instrument agreed it had not. See clock.c. */
    int clk_rc = clock_init(BOARD_XTAL_MHZ);
    uart_puts("  cpu clock    : ");
    uart_put_dec(clock_cpu_mhz());
    uart_puts(" MHz ");
    if (clk_rc != 0) {
        uart_puts("(clock_init REFUSED - unsupported crystal)\n");
    } else if (clock_pll_switched()) {
        uart_puts("(PLL, switched by the kernel)\n");
    } else {
        uart_puts("(PLL, already set by the bootloader)\n");
    }
    /* The measurement, not the intent. If these disagree, believe this one. */
    if (clock_source() != 1u) {
        uart_puts("  *** SOC_CLK_SEL is not PLL - the board is running slow ***\n");
    }

    console_init();
    ipc_init();
    mutex_init(&g_shared_lock);

    touch_init();
    touch_irq_init();   /* PENIRQ -> matrix -> CPU line 23; see intr.h */
    device_init();      /* the device table; must precede anything that uses it */
    adc_init();         /* SAR ADC1; the light sensor is channel 6           */
    i2c_init();
    audio_init();       /* DAC2 on gpio26; tones only, see audio.h */         /* bit-banged, SDA gpio22 / SCL gpio27               */

    raycast_init();
    desktop_init();

    /* Probe the card at boot, so storage is ready before anything asks for it
     * and so an absent card is reported once rather than discovered later by
     * whatever needed it. A failure here is not fatal: the slot is normally
     * empty, and every wait inside sd_init() is bounded for exactly that
     * reason. */
    uart_puts("  sd           : ");
    {
        int sd_rc = sd_init();
        if (sd_rc == SD_OK) {
            uart_puts(sd_type() == SD_TYPE_SDHC ? "SDHC ready" : "SDSC ready");
        } else if (sd_rc == SD_ERR_IDLE) {
            uart_puts("no card");
        } else {
            uart_puts("present but init failed at stage ");
            uart_put_dec((unsigned int)(-sd_rc));
        }
        uart_puts("\n");
    }

    uart_puts("  messages     : ");
    {
        int found = (msg_load() == 0);
        uart_puts(found ? "loaded " : "empty (");
        uart_put_dec(msg_count());
        uart_puts(found ? " saved\n" : " )\n");
    }

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
    uart_puts(" dma=");
    uart_put_dec(display_dma_transfers());
    uart_puts("/");
    uart_put_dec(display_dma_timeouts());
    uart_puts("\n");

    m3_selftest();
    m4_selftest();
    m5_selftest();
    m6_selftest();

    /* Persistence. Loaded before the scheduler starts so the boot count is
     * settled before anything can race it, and saved immediately so a power
     * cut a second later still records that this boot happened. */
    extern void store_count_boot(void);
#if FLASH_ENABLE
    uart_puts("  flash id     : ");
    uart_put_hex(flash_read_id());
    uart_puts("\n  store        : ");
    int found = (store_load() == 0);
    store_count_boot();
    int saved = (store_save() == 0);
    uart_puts(found ? "loaded" : "initialised");
    uart_puts(", boot #");
    uart_put_dec(store_boots());
    uart_puts(", frames ");
    uart_put_dec(store_frames());
    uart_puts(saved ? ", saved\n" : ", SAVE FAILED\n");

    /* Restore a saved touch calibration before anything can be touched. */
    if (store_has_calibration()) {
        uint32_t cxa, cxb, cya, cyb;
        store_get_calibration(&cxa, &cxb, &cya, &cyb);
        touch_set_calibration(cxa, cxb, cya, cyb);
        uart_puts("  touch cal    : restored x ");
        uart_put_dec(cxa);
        uart_puts("..");
        uart_put_dec(cxb);
        uart_puts(" y ");
        uart_put_dec(cya);
        uart_puts("..");
        uart_put_dec(cyb);
        uart_puts("\n");
    } else {
        uart_puts("  touch cal    : defaults (run 'cal' to measure)\n");
    }

    /* Report a fault recorded by a previous boot.
     *
     * This is the only way a fault becomes visible on a board with nothing
     * attached. The panel freezes, the user power-cycles it, and this line is
     * what tells them why — a question that previously had no answer unless
     * someone happened to have a serial cable connected at the moment it died.
     *
     * Deliberately not cleared after printing. A fault stays on the record
     * until a different one replaces it, so power-cycling past this message
     * does not destroy it. `fault_boot` says which boot it happened on, which
     * is what stops an old fault from reading as a new one. */
    if (store_fault_kind() != STORE_FAULT_NONE) {
        uart_puts("  LAST FAULT   : ");
        if (store_fault_kind() == STORE_FAULT_GUARD) {
            /* NA-009. This said "stack guard overwritten, task N".
             *
             * STORE_FAULT_GUARD is written by kernel_panic_msg() for EVERY
             * kernel-detected failure, not just the stack-guard one -- the
             * "task table full" panic in must_create() lands here too, and was
             * reported on the next boot as a broken stack guard in task 0.
             * The record carries the kind and the detail but not the `why`
             * string, so the reason cannot be recovered after a reboot.
             *
             * Naming what is actually known is the fix. Guessing the most
             * common cause is what made this evidence worse than silence. */
            uart_puts("kernel-detected failure, detail ");
            uart_put_dec(store_fault_detail());
        } else {
            uart_puts("exception, exccause ");
            uart_put_dec(store_fault_detail());
            uart_puts(", epc ");
            uart_put_hex(store_fault_epc());
        }
        uart_puts("  (boot #");
        uart_put_dec(store_fault_boot());
        uart_puts(")\n");
    }
#else
    uart_puts("  store        : disabled, flash reads bit-shifted (flash.h)\n");
#endif

    /* After the self-tests, not before: heap_init() runs inside m3_selftest(),
     * and the leak test there checks free memory returns to its baseline — an
     * 80 KB allocation made earlier would fail for want of a heap and, once the
     * ordering was fixed, would break the baseline instead. */
    if (raycast_set_framebuffer(1) != 0) {
        uart_puts("  raycast fb   : allocation failed, using direct columns\n");
    }

    /* The VM's arena is created here, on the boot path, because the heap has no
     * locking and this is the last moment at which exactly one context exists
     * (UM-NATOS-010 §8). The task only ever runs an already-initialised VM. */
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
    /* Order matters: ping addresses application 1, so pong must take that
     * slot. Slots are handed out lowest-free-first. */
    start_program("ping");
    start_program("pong");

    /* Checked, because the unchecked version cost this kernel its idle task.
     * task_create() returns -1 when the table is full, kmain made nine calls
     * against a TASK_MAX of 8, and the ninth failed silently for months: the
     * return value was assigned to id_idle, passed to task_set_idle(), which
     * bounds-checks and ignores it, and never printed. A creation that cannot
     * fail quietly is worth the four lines. */
    id_report = must_create("report", task_report);
    id_a      = must_create("worker-a", task_a);
    id_b      = must_create("worker-b", task_b);
    id_vm     = must_create("vm-host", task_vm);
    id_apps   = must_create("app-host", task_apps);
    id_shell  = must_create("shell", task_shell);

    /* Created last and registered as idle, so it is outside the round robin and
     * chosen only when every other task is blocked. Without it, a moment where
     * all tasks are waiting has nothing to switch to. */
    id_disp   = must_create("display", task_display);
    id_touch  = must_create("touch", task_touch);
    id_idle   = must_create("idle", task_idle);
    task_set_idle(id_idle);

    /* Priorities. The renderer is the only HIGH task: it wakes, draws a frame,
     * and sleeps, so it takes what it needs and then stands aside. Strict
     * priority is only safe because of that sleep — a HIGH task that spun would
     * starve every level beneath it outright.
     *
     * Nothing sits at LOW. Strict priority starves it absolutely: the workers
     * were put there first and did exactly zero iterations, because the NORMAL
     * band never empties — the application host never sleeps. That killed the
     * M2 register-integrity evidence, since corrupt=0 means nothing when no
     * work is being done.
     *
     * The renderer's speedup came from being HIGH, not from anything being LOW,
     * so everything else shares NORMAL. LOW remains defined and usable, but any
     * task placed there must be genuinely discardable — there is no aging or
     * anti-starvation, and a LOW task will simply never run while any NORMAL
     * task is runnable. */
    task_set_priority(id_disp,   TASK_PRIO_HIGH);
    /* HIGH, with the renderer, and polling on a real 10 ms sleep.
     *
     * This was suspected of causing the 3D view tearing and reverted; it was
     * innocent. The tearing was desktop_chrome() painting over a full-width
     * view, and touch scheduling was identical across all four
     * priority/poll combinations. Confirmed on the panel afterwards.
     *
     * HIGH because touch is user INPUT: at NORMAL it runs only when ageing
     * rescues it from behind a renderer that never sleeps, roughly every
     * 300 ms, and a quick tap falls between samples. A real sleep rather than
     * a yield because a yield surrenders the CPU after EVERY poll, sampling
     * once per slice instead of many times. It costs one SPI read per tick.
     *
     * Measured: ~15 samples per reporter interval before, ~150 after. */
    task_set_priority(id_touch,  TASK_PRIO_HIGH);
    task_set_priority(id_shell,  TASK_PRIO_NORMAL);
    task_set_priority(id_apps,   TASK_PRIO_NORMAL);
    task_set_priority(id_report, TASK_PRIO_NORMAL);
    task_set_priority(id_a,      TASK_PRIO_NORMAL);
    task_set_priority(id_b,      TASK_PRIO_NORMAL);
    task_set_priority(id_vm,     TASK_PRIO_NORMAL);
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

    /* Armed last, immediately before the scheduler takes over. Arming earlier
     * would have the single-threaded boot path - which never switches tasks and
     * so never feeds - reset the board partway through its own self-tests. */
    watchdog_arm(3000u);
    uart_puts("  hang detector: armed, 3000 ms, fed on distinct task switches\n");

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
