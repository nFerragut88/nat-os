/* nat-os — native task control and round-robin scheduling.
 *
 * Switching happens inside the level-3 timer interrupt. The handler saves the
 * full context onto the interrupted task's own stack, hands the stack pointer
 * to task_schedule(), and resumes on whatever stack it gets back. Because the
 * frame carries EPC3 and EPS3, restoring it restores the return address and
 * processor state of a *different* task — which is the whole trick.
 *
 * A new task is started by fabricating a frame that looks exactly as though it
 * had been interrupted at its entry point. No special "first switch" path is
 * needed, and the boot context needs no fabrication at all: its stack pointer
 * arrives as the argument on the first call.
 */

#include "task.h"
#include "timer.h"
#include "uart.h"
#include "critical.h"
#include "watchdog.h"
#include "panic.h"
#include "xtensa.h"

/* Written into the lowest stack word; if it changes, the task overflowed. */
#define STACK_GUARD 0x57ACC0DEu

/* Fill pattern, so untouched stack is distinguishable from used stack and
 * headroom can be measured rather than guessed. */
#define STACK_FILL  0xEEEEEEEEu

_Static_assert(TASK_FRAME_BYTES >= TASK_FRAME_WORDS * 4,
               "frame must hold every saved word");
_Static_assert((TASK_FRAME_BYTES % 16) == 0,
               "Xtensa requires a 16-byte aligned stack");

static task_t   g_tasks[TASK_MAX];
static uint32_t g_stacks[TASK_MAX][TASK_STACK_WORDS];

/* -1 means "no task is running yet": the boot context is about to be abandoned
 * and its stack pointer must NOT be saved, because doing so would overwrite the
 * fabricated frame of whichever task occupies that slot.
 *
 * An earlier design adopted the boot context as task 0 instead, capturing its
 * stack pointer on the first interrupt. That created two ways for a task to
 * come into existence — fabricated and captured — and only the fabricated path
 * worked: tasks 1 and 2 ran, task 0 never resumed. Deleting the second path was
 * cheaper than debugging it, and leaves one code path to be correct about. */
static int g_current = -1;

/* ---- CPU time accounting ------------------------------------------------
 *
 * Cycles each task has actually RUN, as opposed to how much wall-clock passed
 * while it was trying to.
 *
 * Every timing figure in this kernel used to be a pair of xt_ccount() reads
 * around some work, which counts every tick, every other task's slice and every
 * interrupt that landed in the middle as though it were the work itself. The
 * measured cost of a full-screen fill was 43 ms before the scheduler existed
 * and 249-362 ms from a task afterwards — the same bytes to the same panel,
 * differing by eight times in nothing but bookkeeping.
 *
 * Charged at the switch, which is the only place that knows a task stopped
 * running. Time spent inside the level-3 handler is charged to whichever task
 * it interrupted; that is a small and deliberate inaccuracy, and the
 * alternative is accounting inside the ISR that measures the ISR.
 *
 * MEASURED, and the correction is not uniform. A full-screen fill from the
 * SHELL task read 249-362 ms wall-clock and 63-79 ms of work — the shell runs
 * at NORMAL and is preempted constantly, so nearly all of that was other tasks.
 * The raycaster's blit did not move at all: 55.5 ms either way, because the
 * display task runs at HIGH and is barely interrupted, so wall-clock was
 * already very close to its work.
 *
 * Which means the fix matters most exactly where the old numbers were least
 * trusted, and changes nothing where they were quietly correct. A conclusion
 * drawn from one task's timings could not have been carried to another's. */
static uint32_t g_run_cycles[TASK_MAX];
static uint32_t g_slice_start;      /* ccount when the running task got the CPU */

/* -1 until a task registers as idle. Kept out of the round robin and used only
 * when nothing else can run. */
static int g_idle_id = -1;

/* Fairness telemetry. g_max_wait is the worst wait ever observed by a task that
 * eventually ran, in ticks; g_age_rescues counts decisions that went to a task
 * only because ageing had lifted it. If the second is zero the policy is inert,
 * which is itself worth knowing. */
static uint32_t g_max_wait;
static uint32_t g_age_rescues;

uint32_t task_max_wait(void)    { return g_max_wait; }
uint32_t task_age_rescues(void) { return g_age_rescues; }

/* Wakes any sleeping task whose deadline has passed. Called from the scheduler,
 * which runs every tick, so a sleep resolves to one tick. */
static void wake_sleepers(void)
{
    uint32_t now = timer_ticks();
    for (int i = 0; i < TASK_MAX; i++) {
        if (g_tasks[i].state == TASK_SLEEPING &&
            (int32_t)(now - g_tasks[i].wake_tick) >= 0) {
            g_tasks[i].state = TASK_READY;
        }
    }
}

int task_create(const char *name, task_entry_fn entry)
{
    for (int id = 0; id < TASK_MAX; id++) {
        if (g_tasks[id].state != TASK_UNUSED) {
            continue;
        }

        uint32_t *stack = g_stacks[id];
        for (int i = 0; i < TASK_STACK_WORDS; i++) {
            stack[i] = STACK_FILL;
        }
        stack[0] = STACK_GUARD;

        /* Frame sits at the top of the stack, 16-byte aligned. */
        uint32_t top = (uint32_t)&stack[TASK_STACK_WORDS];
        top &= ~15u;
        uint32_t *frame = (uint32_t *)(top - TASK_FRAME_BYTES);

        for (int i = 0; i < TASK_FRAME_WORDS; i++) {
            frame[i] = 0;
        }

        /* Resume at the entry point, with interrupts admitted. PS is taken
         * from the running kernel with INTLEVEL forced to 0, so the task
         * inherits the same execution mode rather than a guessed one — the
         * ROM leaves WOE and CALLINC set, and fabricating a different PS would
         * put the task in a subtly different state from its creator. */
        frame[TASK_FRAME_IDX_EPC3] = (uint32_t)entry;
        frame[TASK_FRAME_IDX_EPS3] = xt_get_ps() & ~0xFu;
        frame[TASK_FRAME_IDX_SAR]  = 0;

        g_tasks[id].sp         = (uint32_t)frame;
        g_tasks[id].state      = TASK_READY;
        g_tasks[id].prio       = TASK_PRIO_NORMAL;
        g_tasks[id].base_prio  = TASK_PRIO_NORMAL;
        g_tasks[id].wake_tick  = 0;
        g_tasks[id].woken      = 0;
        g_tasks[id].switches   = 0;
        g_tasks[id].waiting    = 0;
        g_tasks[id].name       = name;
        g_tasks[id].stack_base = stack;
        return id;
    }
    return -1;
}

/* ---- switch tracing -------------------------------------------------- */
/*
 * Prints the frame the handler is about to restore. Ticks 1-3 restore frames
 * fabricated by task_create; tick 4 is the first restore of a frame the
 * handler itself saved. Dumping both means the saved frame can be read against
 * a known-good fabricated one instead of against expectations.
 *
 * This runs at interrupt level 3 and blocks on the UART for the length of the
 * dump, which is far longer than a tick period. Ticks are missed as a result
 * and timer_late_count() will climb — that is expected and harmless here,
 * because the question is what the frame CONTAINS, not when it arrives.
 */
/* Switch tracing. Set to 0 for a quiet boot; raise it to watch the first N
 * switches when touching the handler or the frame layout. Retained rather than
 * deleted because it is what turned M2's silence into a sequence. */
#define TRACE_SWITCHES 0

/* A/B switch. With the in-loop probes compiled in, the selection loop is
 * correct; with them out, switch 4 returned 2 -> 2 while the task table said
 * every task was READY. Same source otherwise. Flip this to reproduce. */
#define TRACE_PROBES 0

#if TRACE_SWITCHES > 0

static uint32_t g_trace_n;

static const char *const FRAME_REGS[TASK_FRAME_WORDS] = {
    "a0 ", "a2 ", "a3 ", "a4 ", "a5 ", "a6 ", "a7 ", "a8 ", "a9 ",
    "a10", "a11", "a12", "a13", "a14", "a15", "sar", "epc", "eps",
    "lbg", "lnd", "lct"
};

static void trace_frame(int from, int to, uint32_t in_sp, const uint32_t *frame,
                        int fabricated)
{
    uart_puts("\n-- switch ");
    uart_put_dec(g_trace_n);
    uart_puts(": ");
    uart_put_dec((unsigned int)from);
    uart_puts(" -> ");
    uart_put_dec((unsigned int)to);
    uart_puts(fabricated ? "  (fabricated frame)\n" : "  (SAVED frame)\n");

    uart_puts("   in_sp=");
    uart_put_hex(in_sp);
    uart_puts("  frame@");
    uart_put_hex((uint32_t)frame);
    uart_puts("  base=");
    uart_put_hex((uint32_t)g_tasks[to].stack_base);
    uart_puts("\n");

    /* The whole task table. If the round robin stops offering a task, the
     * question is whether the scheduler skipped it or whether its state field
     * stopped saying READY — and those are different bugs. */
    uart_puts("   table:");
    for (int i = 0; i < TASK_MAX; i++) {
        uart_puts(" [");
        uart_put_dec((unsigned int)i);
        uart_puts("] st=");
        uart_put_dec((unsigned int)g_tasks[i].state);
        uart_puts(" sp=");
        uart_put_hex(g_tasks[i].sp);
        uart_puts(" sw=");
        uart_put_dec(g_tasks[i].switches);
    }
    uart_puts("\n");

    for (int i = 0; i < TASK_FRAME_WORDS; i++) {
        uart_puts("   ");
        uart_puts(FRAME_REGS[i]);
        uart_putc('=');
        uart_put_hex(frame[i]);
        if ((i % 3) == 2) {
            uart_putc('\n');
        }
    }
    uart_puts("\n");
}

#endif /* TRACE_SWITCHES > 0 */

/* Test hook. Byte-for-byte the selection loop as it was WITHOUT the volatile
 * workaround, so GCC is free to emit a zero-overhead LOOP again. Called from
 * kmain before timer_start(), i.e. single-threaded with no interrupt source
 * armed and no context switching in existence.
 *
 * This separates two very different explanations for the M2 defect:
 *   - wrong answers here  => the loop is mis-executed on its own, and interrupts
 *                            were never involved
 *   - correct answers here => the loop is fine in isolation and something about
 *                            interrupt context corrupts it
 */
int task_select_probe(int current)
{
    int next = current;
    for (int i = 1; i <= TASK_MAX; i++) {
        int candidate = (current + i) % TASK_MAX;
        if (g_tasks[candidate].state == TASK_READY) {
            next = candidate;
            break;
        }
    }
    return next;
}

/* Called from _handler_level3. Must not be static — assembly names it. */
uint32_t task_schedule(uint32_t current_sp)
{
    /* On the very first switch there is no task to save — the interrupted
     * context is the boot path, which is deliberately discarded. */
    uint32_t now = xt_ccount();
    if (g_current >= 0) {
        g_tasks[g_current].sp = current_sp;
        g_run_cycles[g_current] += now - g_slice_start;
    }
    g_slice_start = now;

    /* Round robin from the one after current, so no task can starve another.
     * With g_current == -1 the first candidate is 0, so the first switch enters
     * whichever task was created first.
     *
     * The idle task is skipped here and used only as a fallback below. Leaving
     * it in the rotation would hand it an equal share of the CPU, which is a
     * seventh of the machine spent deliberately doing nothing. */
    wake_sleepers();

    /* Highest priority wins; equal priorities round-robin.
     *
     * Scanning from the task after the current one and taking a candidate only
     * on STRICTLY greater priority gives both at once: among equals the first
     * one met wins, and because the scan starts past the current task, that is
     * a different task each time. */
    /* Effective priority = base + ageing credit, computed per candidate below.
     * The base priority is never modified: ageing is a property of the
     * SELECTION, not of the task, so a task that finally runs returns to its
     * declared priority automatically rather than needing to be restored. That
     * distinction is what keeps this separate from priority inheritance, which
     * really does change a task's priority and really does have to undo it. */
    int next = -1;
    int best = -1;
    /* Plain counted loop. GCC is free to emit a zero-overhead LOOP here, and
     * does; that is correct now that _handler_level3 clears PS.EXCM before
     * calling C. This loop previously needed a `volatile` counter to force
     * ordinary branches, which worked but treated the symptom. */
    for (int i = 1; i <= TASK_MAX; i++) {
        int candidate = (g_current + i) % TASK_MAX;
        if (candidate == g_idle_id) {
            continue;
        }
#if TRACE_SWITCHES > 0 && TRACE_PROBES
        if (g_trace_n < TRACE_SWITCHES) {
            uart_puts("\n   probe i=");
            uart_put_dec((unsigned int)i);
            uart_puts(" cand=");
            uart_put_dec((unsigned int)candidate);
            uart_puts(" st=");
            uart_put_dec((unsigned int)g_tasks[candidate].state);
            uart_puts(g_tasks[candidate].state == TASK_READY ? " MATCH" : " skip");
        }
#endif
        if (g_tasks[candidate].state == TASK_READY) {
            uint32_t credit = g_tasks[candidate].waiting / TASK_AGE_TICKS;
            if (credit > TASK_AGE_MAX) {
                credit = TASK_AGE_MAX;
            }
            int eff = (int)g_tasks[candidate].prio + (int)credit;
            if (eff > best) {
                best = eff;
                next = candidate;
            }
        }
    }
    /* Nothing else runnable — fall back to idle. Resuming the interrupted task
     * is NOT an acceptable answer once blocking exists: that task may be the
     * one that just blocked, and running a blocked task defeats the whole
     * mechanism. */
    if (next < 0 && g_idle_id >= 0 && g_tasks[g_idle_id].state == TASK_READY) {
        next = g_idle_id;
    }

    if (next < 0) {
        /* No runnable task and no idle task. Before blocking existed this could
         * only happen at boot; it can now also mean every task is blocked with
         * nobody left to wake them, which is a deadlock the kernel cannot
         * resolve. Resuming the interrupted context is the least-bad answer and
         * at least keeps the console alive to say so. */
        return current_sp;
    }

#if TRACE_SWITCHES > 0
    /* switches == 0 means this task has never run, so its frame is still the
     * one task_create fabricated. Anything else is a frame the handler saved. */
    int fabricated = (g_tasks[next].switches == 0);
    int from = g_current;
#endif

    /* Liveness for the hang detector: a tick that resumes the SAME task is not
     * evidence the system is healthy — it is exactly what a monopoly looks
     * like. Only a switch between distinct tasks counts. */
    /* Ageing bookkeeping, done once the decision is final.
     *
     * Every READY task that was NOT chosen waits one more tick; the chosen one
     * resets. Sleeping and blocked tasks are untouched — a task waiting on a
     * deadline or a mutex is not being treated unfairly by the scheduler, and
     * crediting it would let it barge ahead the moment it becomes runnable. */
    for (int i = 0; i < TASK_MAX; i++) {
        if (g_tasks[i].state != TASK_READY) {
            continue;
        }
        if (i == next) {
            if (g_tasks[i].waiting > g_max_wait) {
                g_max_wait = g_tasks[i].waiting;
            }
            if (g_tasks[i].waiting >= TASK_AGE_TICKS) {
                g_age_rescues++;    /* it only ran because ageing lifted it */
            }
            g_tasks[i].waiting = 0;
        } else {
            g_tasks[i].waiting++;
        }
    }

    watchdog_liveness(next != g_current);

    /* A broken guard is fatal, not cosmetic.
     *
     * It used to print "BROKEN" beside the telemetry and carry on, which means
     * continuing to schedule tasks whose stacks have already written into a
     * neighbour's. Every number printed after that point is suspect, including
     * the ones that would be used to diagnose it. Stopping at the switch that
     * noticed keeps the damage bounded and the report honest. */
    int broken = task_stack_broken();
    if (broken >= 0) {
        kernel_panic_msg("stack guard overwritten", (unsigned int)broken);
    }

    g_current = next;
    g_tasks[next].switches++;

#if TRACE_SWITCHES > 0
    if (g_trace_n < TRACE_SWITCHES) {
        g_trace_n++;
        trace_frame(from, next, current_sp,
                    (const uint32_t *)g_tasks[next].sp, fabricated);
    }
#endif

    return g_tasks[next].sp;
}

int task_current(void) { return g_current; }

uint32_t task_switch_count(int id)
{
    return (id >= 0 && id < TASK_MAX) ? g_tasks[id].switches : 0;
}

int task_stack_intact(int id)
{
    if (id < 0 || id >= TASK_MAX || g_tasks[id].stack_base == 0) {
        return 1;   /* no stack of ours to check */
    }
    return g_tasks[id].stack_base[0] == STACK_GUARD;
}

/* First task whose guard word has been overwritten, or -1 if all are intact.
 *
 * Checked on every context switch rather than by the reporter, because the
 * reporter only covered three of the eight tasks and a broken guard means the
 * damage has ALREADY happened — a check that runs seconds later reports a
 * corruption whose cause is long gone. Eight word loads per tick is not a cost
 * worth optimising against that. */
/* Test hook: clobber the running task's guard word so the next context switch
 * finds it. Exists for the same reason `hang` and `fault` do — an enforcement
 * path that has never been seen to fire is an assumption, not a mechanism. */
/* Cycles the CALLING task has run, including the slice it is in right now.
 *
 * This is the clock to measure work with. Two reads around a section give the
 * cycles that section actually consumed, with every preemption in between
 * excluded — which is the whole difference from xt_ccount().
 *
 * The critical section is not optional: without it a tick landing between the
 * two loads returns an accumulated total from before the switch added to a
 * slice start from after it, which reads as a wildly negative interval. */
uint32_t task_cpu_cycles(void)
{
    uint32_t crit = crit_enter();
    uint32_t v = (g_current >= 0)
               ? g_run_cycles[g_current] + (xt_ccount() - g_slice_start)
               : xt_ccount();
    crit_exit(crit);
    return v;
}

uint32_t task_cpu_cycles_of(int id)
{
    return (id >= 0 && id < TASK_MAX) ? g_run_cycles[id] : 0u;
}

void task_smash_guard(void)
{
    if (g_current >= 0 && g_tasks[g_current].stack_base) {
        g_tasks[g_current].stack_base[0] = 0xDEADBEEFu;
    }
}

int task_exists(int id)
{
    return id >= 0 && id < TASK_MAX && g_tasks[id].state != TASK_UNUSED;
}

const char *task_name(int id)
{
    if (id < 0 || id >= TASK_MAX || g_tasks[id].name == 0) {
        return "?";
    }
    return g_tasks[id].name;
}

int task_stack_broken(void)
{
    for (int id = 0; id < TASK_MAX; id++) {
        if (g_tasks[id].stack_base && g_tasks[id].stack_base[0] != STACK_GUARD) {
            return id;
        }
    }
    return -1;
}

/* The task closest to overflowing. Reported so the margin is a number somebody
 * has seen, rather than an assumption that 2 KB was enough. */
int task_stack_tightest(void)
{
    int worst = -1;
    uint32_t least = 0xFFFFFFFFu;
    for (int id = 0; id < TASK_MAX; id++) {
        if (!g_tasks[id].stack_base) {
            continue;
        }
        uint32_t free_words = task_stack_headroom(id);
        if (free_words < least) {
            least = free_words;
            worst = id;
        }
    }
    return worst;
}

uint32_t task_stack_headroom(int id)
{
    if (id < 0 || id >= TASK_MAX || g_tasks[id].stack_base == 0) {
        return 0;
    }
    uint32_t untouched = 0;
    /* Walk up from just above the guard until the fill pattern stops. */
    for (int i = 1; i < TASK_STACK_WORDS; i++) {
        if (g_tasks[id].stack_base[i] != STACK_FILL) {
            break;
        }
        untouched++;
    }
    return untouched;
}

void task_set_idle(int id)
{
    if (id >= 0 && id < TASK_MAX) {
        g_idle_id = id;
    }
}

void task_block(void)
{
    if (g_current < 0) {
        return;                     /* boot context has nothing to block */
    }
    g_tasks[g_current].state = TASK_BLOCKED;
    /* Deliberately does NOT yield. The caller marks itself blocked while still
     * holding a critical section — so the decision to block and the record of
     * what it is waiting for cannot be split by a tick — then leaves the
     * critical section and yields. Yielding here instead would either fire the
     * tick with the wait-list half-updated, or be silently deferred until the
     * caller's crit_exit(), which reads as a bug at the call site. */
}

int task_state_of(int id)
{
    return (id >= 0 && id < TASK_MAX) ? (int)g_tasks[id].state : (int)TASK_UNUSED;
}

void task_unblock(int id)
{
    if (id >= 0 && id < TASK_MAX && g_tasks[id].state == TASK_BLOCKED) {
        g_tasks[id].state = TASK_READY;
    }
}

void task_wake(int id)
{
    if (id < 0 || id >= TASK_MAX) {
        return;
    }
    /* Deliberately takes SLEEPING as well as BLOCKED, which is the whole
     * difference from task_unblock().
     *
     * A task waiting on hardware wants two things at once: to be released the
     * instant the device speaks, and to be released anyway if it never does. A
     * plain block gives the first without the second, and a task blocked on a
     * peripheral that has failed is deaf forever. A plain sleep gives the second
     * without the first.
     *
     * Sleeping with a deadline and letting an interrupt cut it short gives both,
     * and the failure mode is the good one: if the interrupt never arrives, the
     * deadline still fires and the caller falls back to polling. That is how the
     * touch task survives a PENIRQ that does not work — it degrades to exactly
     * what it did before this existed rather than to nothing.
     *
     * Safe from an ISR. It only promotes a state to READY; wake_tick is left
     * alone, and the scheduler's sleep sweep ignores a task that is no longer
     * SLEEPING. */
    if (g_tasks[id].state == TASK_BLOCKED ||
        g_tasks[id].state == TASK_SLEEPING) {
        g_tasks[id].state = TASK_READY;
        /* Records that this was a DELIBERATE release, not a deadline expiring.
         * task_sleep re-arms until its deadline arrives, so without this it
         * would sleep the caller again and the early wake would be lost. Set
         * only here; wake_sleepers() leaves it alone, which is what makes the
         * two cases distinguishable. */
        g_tasks[id].woken = 1;
    }
}

void task_set_priority(int id, int prio)
{
    if (id < 0 || id >= TASK_MAX) {
        return;
    }
    if (prio < 0) { prio = 0; }
    if (prio >= TASK_PRIO_LEVELS) { prio = TASK_PRIO_LEVELS - 1; }
    g_tasks[id].base_prio = (uint8_t)prio;
    g_tasks[id].prio      = (uint8_t)prio;
}

int task_priority(int id)
{
    return (id >= 0 && id < TASK_MAX) ? (int)g_tasks[id].prio : -1;
}

void task_boost(int id, int prio)
{
    if (id >= 0 && id < TASK_MAX && prio > (int)g_tasks[id].prio) {
        g_tasks[id].prio = (uint8_t)prio;
    }
}

void task_unboost(int id)
{
    if (id >= 0 && id < TASK_MAX) {
        g_tasks[id].prio = g_tasks[id].base_prio;
    }
}

void task_sleep(uint32_t ticks)
{
    if (g_current < 0) {
        return;                     /* no context to sleep */
    }

    /* Loops, because task_yield() does not switch — it ARMS a switch.
     *
     * It writes CCOMPARE1 to ccount + 64 and returns; the context change
     * happens when that comparator fires, roughly sixty cycles later. So the
     * caller keeps running past this function for a short window, and whatever
     * it does in that window happens DURING what it believes is a sleep.
     *
     * Measured directly: task_sleep(50) — half a second — returned to its
     * caller in 107 cycles, which is the cost of the function body and nothing
     * else. The sleep did happen; it just started after the caller had already
     * read the clock and concluded no time had passed. Two separate
     * measurements this session were wrong because of it, including the first
     * TSF check, which is what exposed it.
     *
     * Re-arming until the deadline genuinely arrives closes that window and
     * costs one extra pass in the normal case.
     *
     * The `woken` flag is what keeps this from breaking touch_irq_wait. That
     * caller wants a sleep an interrupt can cut short, so "the deadline has not
     * arrived" must not be confused with "task_wake released me deliberately" —
     * without the flag, this loop would put the task straight back to sleep and
     * turn every early wake into a full-length one. */
    uint32_t deadline = timer_ticks() + ticks;

    uint32_t crit = crit_enter();
    g_tasks[g_current].woken = 0;
    crit_exit(crit);

    for (;;) {
        crit = crit_enter();
        if (g_tasks[g_current].woken) {
            g_tasks[g_current].woken = 0;
            crit_exit(crit);
            return;                 /* released early, on purpose */
        }
        if ((int32_t)(timer_ticks() - deadline) >= 0) {
            crit_exit(crit);
            return;                 /* the time really has passed */
        }
        g_tasks[g_current].wake_tick = deadline;
        g_tasks[g_current].state     = TASK_SLEEPING;
        crit_exit(crit);
        task_yield();
    }
}

void task_yield(void)
{
    /* Bring the comparator deadline forward so the tick — and therefore the
     * switch — happens almost immediately. Reuses the preemption path exactly
     * instead of introducing a second way to switch, which would be a second
     * thing that can be subtly wrong.
     *
     * ONLY EVER EARLIER, NEVER LATER. Writing ccount + 64 unconditionally means
     * a loop that yields faster than 64 cycles pushes the deadline ahead of
     * CCOUNT on every pass, so the comparator is never reached and the timer
     * interrupt stops firing altogether. That is not a slowdown — it is a total
     * system freeze, because the tick is what drives every context switch, and
     * the task doing the yielding spins forever waiting for a clock it is
     * itself preventing.
     *
     * It cost a full debugging session: `while (timer_ticks() < until)
     * task_yield();` in the display task halted the entire kernel, and looked
     * exactly like a hung display driver. */
    uint32_t soon = xt_ccount() + 64u;
    if ((int32_t)(soon - xt_get_ccompare1()) < 0) {
        xt_set_ccompare1(soon);
    }
}
