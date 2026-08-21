/* nat-os — native task control.
 *
 * These are kernel-level tasks: drivers, the eventual VM host, and any work
 * that must run as real machine code. Applications will NOT be tasks of this
 * kind — they run inside the bytecode interpreter, which schedules them at
 * instruction boundaries (UM-NATOS-001 §4.2). The number of native tasks is
 * expected to stay small.
 *
 * There is no memory protection between them. The ESP32 has no MMU paging, so
 * a native task can corrupt any other. Stack guards catch the most common case
 * (overflow) but nothing catches a wild pointer.
 */

#ifndef NATOS_TASK_H
#define NATOS_TASK_H

#include <stdint.h>

/* 12, not 8. The kernel creates nine tasks and TASK_MAX was 8, so the ninth —
 * the idle task — failed to be created and nobody noticed: task_create()
 * returns -1, task_set_idle(-1) quietly does nothing, and the return value was
 * never checked or printed. The system ran without an idle task for as long as
 * priorities have existed, which cost the WAITI power-saving path entirely and
 * meant that with every task asleep the scheduler resumed a SLEEPING task
 * rather than idling.
 *
 * The headroom is for the desktop and whatever follows it. The cost is
 * TASK_STACK_WORDS * 4 bytes of DRAM per slot, which the measured margins in
 * UM-NATOS-019 §3.1 say is affordable. */
#define TASK_MAX          12
#define TASK_STACK_WORDS  512          /* 2 KB per task */
#define TASK_NAME_MAX     12

/* Saved-context frame, in 32-bit words. Assembly in vectors.S writes this
 * layout by hand; the two MUST agree. A static assertion in task.c checks the
 * size, but the field order is only guaranteed by keeping these in step.
 *
 *   0    a0            1..14  a2..a15
 *   15   SAR           16     EPC3          17  EPS3
 *   18   LBEG          19     LEND          20  LCOUNT
 *
 * LBEG/LEND/LCOUNT back the Xtensa zero-overhead LOOP instruction, and they are
 * part of a task's context whether or not that task ever knows it. GCC emits
 * LOOP for ordinary counted C loops, so any task can be suspended mid-loop with
 * LCOUNT non-zero. Omitting these three words cost a full debugging session:
 * the scheduler's own round robin compiled to a LOOP, stale LCOUNT survived
 * into the handler, the hardware branched back to LEND, and the "no candidate
 * matched" fallback overwrote a selection that had in fact matched. It presented
 * as a task switching to itself forever. Adding a uart_puts() inside the loop
 * made the symptom disappear, because a loop body containing a call cannot be a
 * zero-overhead loop and GCC emitted a plain branch instead.
 */
#define TASK_FRAME_WORDS  23
#define TASK_FRAME_BYTES  112          /* 23 words, padded to 16-byte alignment */

#define TASK_FRAME_IDX_SAR    15
#define TASK_FRAME_IDX_EPC3   16
#define TASK_FRAME_IDX_EPS3   17
#define TASK_FRAME_IDX_LBEG   18
#define TASK_FRAME_IDX_LEND   19
#define TASK_FRAME_IDX_LCOUNT 20

/* WINDOWBASE and WINDOWSTART, per task.
 *
 * These are GLOBAL processor state, and leaving them out is what step 29
 * finally isolated: the handler put a task's sixteen registers back at whatever
 * WINDOWBASE happened to be at that moment, which after another context had run
 * windowed code was not the base the task was saved at. The frame in memory was
 * perfect and the live registers were zeros.
 *
 * Only ONE case needs this, which is why two words are enough rather than a
 * whole spilled window: the pin means an involuntary switch never lands while a
 * task holds live windowed frames, and the blocking adapter path spills down to
 * a single frame before it waits. */
#define TASK_FRAME_IDX_WBASE  21
#define TASK_FRAME_IDX_WSTART 22

/* Test hook — see task.c. Runs the selection loop single-threaded. */
int task_select_probe(int current);

typedef void (*task_entry_fn)(void);

typedef enum {
    TASK_UNUSED = 0,
    TASK_READY,
    TASK_BLOCKED,       /* waiting on something; the scheduler will not pick it */
    TASK_SLEEPING,      /* waiting for a tick deadline                          */
} task_state_t;

/* ---- priorities --------------------------------------------------------
 *
 * Strictly ordered: the highest-priority runnable task always runs, and equal
 * priorities round-robin among themselves.
 *
 * Strict ordering is only safe because tasks SLEEP. A high-priority task that
 * spins instead of sleeping starves everything beneath it completely — this
 * kernel's previous `while (...) task_yield();` idiom would do exactly that.
 * task_sleep() is therefore not a convenience here; it is what makes priorities
 * usable at all.
 */
#define TASK_PRIO_LOW    0
#define TASK_PRIO_NORMAL 1
#define TASK_PRIO_HIGH   2
#define TASK_PRIO_LEVELS 3

/* ---- fairness by ageing --------------------------------------------------
 *
 * Strict priority has no upper bound on how long a ready task may wait. The
 * display task runs at HIGH, and shortening its sleep so it was ready ~61% of
 * the time starved the reporter to complete silence — twenty seconds, no
 * output, no crash, and no watchdog reset, because the hang detector asks
 * whether ANY distinct switch happened, not whether every ready task got a
 * turn. A quieter victim would have produced no symptom at all.
 *
 * Ageing bounds it. A ready task that is passed over gains effective priority
 * with time, so any runnable task reaches the front within a known interval
 * regardless of what sits above it. Priority still decides who runs FIRST;
 * ageing decides that "later" is finite.
 *
 * TASK_AGE_TICKS is the wait that buys one level. At 10 ms per tick and two
 * levels available above NORMAL, the worst case for a ready NORMAL task behind
 * a permanently-ready HIGH task is TASK_AGE_TICKS * 2 — currently 600 ms. That
 * is a latency, not a guarantee of throughput: a task that is aged in and then
 * immediately blocks still makes no progress, and that is the caller's
 * problem, not the scheduler's. */
#define TASK_AGE_TICKS  30u
#define TASK_AGE_MAX    3u      /* cap, so ageing cannot invert LOW past HIGH twice over */

/* NA-006. The cap is EXACTLY sized for the current priority range, and that is
 * load-bearing rather than incidental.
 *
 * For ageing to bound starvation, a task at the bottom priority must eventually
 * out-score a permanently-ready task at the top. The freshly-run top task has
 * waiting == 0, so its effective priority is (TASK_PRIO_LEVELS - 1); the bottom
 * task's best possible score is TASK_AGE_MAX. Selection is on STRICTLY greater
 * priority, so the guarantee needs
 *
 *     TASK_AGE_MAX > TASK_PRIO_LEVELS - 1
 *
 * With 3 levels and a cap of 3 that holds by exactly one. Adding a fourth
 * priority level without raising the cap would not produce a slow task or a
 * warning -- it would produce a LOW task that never runs again, presenting as a
 * hang in something unrelated. The assertion turns that into a build failure at
 * the moment the level is added, which is the only moment anyone is looking. */
_Static_assert(TASK_AGE_MAX >= TASK_PRIO_LEVELS,
               "ageing cap too small to bound starvation across the priority range");

typedef struct {
    uint32_t      sp;                  /* saved stack pointer when not running */
    task_state_t  state;
    uint8_t       prio;                /* effective, possibly boosted          */
    uint8_t       base_prio;           /* as created                           */
    uint32_t      wake_tick;           /* deadline while TASK_SLEEPING         */
    /* Set by task_wake(), cleared by task_sleep(). Distinguishes "an interrupt
     * cut this sleep short" from "the deadline has not arrived yet", which
     * task_sleep must tell apart to re-arm without defeating touch_irq_wait. */
    volatile uint8_t woken;
    uint32_t      switches;            /* times a DIFFERENT task was switched to */
    uint32_t      resumes;             /* ticks this task was selected, incl. itself */
    uint32_t      waiting;             /* ticks READY but not selected      */
    const char   *name;
    uint32_t     *stack_base;          /* for guard checking; NULL for boot task */
    uint32_t      stack_words;         /* size of that stack; 0 for boot task  */
} task_t;

/* Create a runnable task. Returns its id, or -1 if the table is full.
 * The entry function must not return — there is nowhere to return to. */
int task_create(const char *name, task_entry_fn entry);

/* The same, but on a stack the caller supplies.
 *
 * TASK_STACK_WORDS is one size for every task, and that stopped being workable
 * the moment a vendor task asked for 6656 bytes: scaling all twelve slots to
 * fit it would cost 78 KB against a heap of 84. Rather than raise the fixed
 * size for everyone, a task that genuinely needs more brings its own.
 *
 * `stack` must stay alive for the life of the task, so in practice it is a
 * static buffer rather than anything from the heap. The guard word, the fill
 * pattern and the headroom report all work exactly as they do for pool tasks,
 * because they read the size from the task rather than from the constant. */
int task_create_with_stack(const char *name, task_entry_fn entry,
                           uint32_t *stack, uint32_t words);

/* Called from the interrupt handler with the interrupted stack pointer.
 * Saves it against the current task, selects the next, and returns the stack
 * pointer to resume. Round-robin over ready tasks.
 *
 * The boot context is task 0: its stack pointer is captured on the first call
 * rather than being fabricated, so no special "start scheduling" step exists. */
uint32_t task_schedule(uint32_t current_sp);

int      task_current(void);
uint32_t task_switch_count(int id);
uint32_t task_multiframe_count(void);  /* switch-outs with >1 live frame */
uint32_t task_multiframe_worst(void);
uint32_t task_multiframe_ws(void);
int      task_multiframe_task(void);
uint32_t task_win_union(void);         /* live window positions, by owner */
uint32_t task_win_mask(int id);
uint32_t task_win_base(int id);
uint32_t task_saved_sp(int id);        /* saved sp of a non-running task */
uint32_t task_stack_span(int id, uint32_t *words);  /* base; size via out-param */
int      task_stack_intact(int id);    /* guard word still present? */
uint32_t task_stack_headroom(int id);  /* untouched words remaining */
int      task_stack_broken(void);      /* id of a task with a clobbered guard, or -1 */

/* Longest any ready task has waited without being scheduled, in ticks, and the
 * number of times ageing changed the decision. Reported because a fairness
 * policy nobody measures is a fairness policy nobody has. */
uint32_t task_max_wait(void);

/* Ready-to-running wait as a HISTOGRAM, not a high-water mark. Buckets in
 * ticks: 0, 1, 2-3, 4-7, 8-15, 16-31, 32-63, 64+. next_moves/04 wants the tail,
 * because one long wait per hour and one per second are the same `maxwait` and
 * completely different systems. */
uint32_t task_wait_hist(uint32_t bucket);
void     task_wait_hist_reset(void);
uint32_t task_age_rescues(void);
int      task_stack_tightest(void);    /* id with the least headroom */
const char *task_name(int id);         /* creation name, or "?" */
int      task_exists(int id);          /* slot is occupied by a real task */
/* Cycles the calling task has actually run, current slice included.
 *
 * The clock to time work with. xt_ccount() measures wall-clock and therefore
 * counts every preemption as though it were the work — the same full-screen
 * fill measures 43 ms single-threaded and 249-362 ms from a task. Two reads of
 * this around a section give what the section really cost. */
uint32_t task_cpu_cycles(void);
uint32_t task_cpu_cycles_of(int id);

void     task_smash_guard(void);       /* test hook: break the running guard */

/* Give up the rest of the current slice by bringing the tick forward.
 * Cooperative yield reusing the timer path rather than a second interrupt
 * source — crude, but it exercises exactly the same switch. */
void task_yield(void);

/* ---- blocking ----------------------------------------------------------
 *
 * task_block() marks the calling task not-runnable. It does NOT yield — the
 * caller is expected to hold a critical section (critical.h) while blocking so
 * that the decision to block and the record of what it is waiting for cannot be
 * split by a tick, then to leave that section and call task_yield().
 *
 *     uint32_t s = crit_enter();
 *     wait_list |= 1u << task_current();
 *     task_block();
 *     crit_exit(s);
 *     task_yield();
 *
 * Nothing here knows what a task is waiting for. That belongs to whatever built
 * the wait — see mutex.c. */
void task_block(void);
void task_unblock(int id);
int  task_state_of(int id);

/* Like task_unblock(), but also releases a SLEEPING task — so a sleep becomes a
 * timeout that an interrupt can cut short. Safe to call from an ISR. See the
 * comment at the definition for why a peripheral wait needs both halves. */
void task_wake(int id);

/* Registers a task as the idle task: it is chosen only when nothing else is
 * runnable, rather than taking an equal share of the round robin. Without one,
 * a system where every task blocks has nothing to run. */
void task_set_idle(int id);

/* Priority, and the boost used for inheritance. task_boost() only ever raises;
 * task_unboost() returns to the priority the task was created with. */
void task_set_priority(int id, int prio);
int  task_priority(int id);
void task_boost(int id, int prio);
void task_unboost(int id);

/* Blocks the caller until `ticks` scheduler ticks have elapsed. Unlike a yield
 * loop this leaves the task unrunnable in the meantime, so lower-priority work
 * actually gets the CPU. */
/* The longest sleep this kernel can express: 2^31-1 ticks, about 248 days at
 * 100 Hz. Deadlines are compared wrap-safely as (int32_t)(now - deadline) >= 0,
 * which spans half the 32-bit range and no more. A request above this is
 * clamped to it and counted by task_sleep_clamped() -- see NA-001 in task.c. */
#define TASK_SLEEP_MAX 0x7FFFFFFFu

void task_sleep(uint32_t ticks);

/* ---- sleeping after arming something that will wake you -----------------
 *
 * NA-005. The plain sequence is racy and the race is silent:
 *
 *     crit_enter(); arm_the_interrupt(); crit_exit();
 *     task_sleep(timeout);          <-- an edge landing HERE is lost
 *
 * The edge finds the task still RUNNING, so nothing is asleep to wake, and the
 * sleep then runs its full timeout with the event already gone. Use instead:
 *
 *     crit_enter(); task_arm_wake(); arm_the_interrupt(); crit_exit();
 *     task_sleep_armed(timeout);    <-- an edge landing HERE returns at once
 */
void task_arm_wake(void);
void task_sleep_armed(uint32_t ticks);

/* Times a sleep request exceeded TASK_SLEEP_MAX and was clamped. Should be zero
 * forever; anything else means a caller's arithmetic overflowed. */
uint32_t task_sleep_clamped(void);

#endif /* NATOS_TASK_H */
