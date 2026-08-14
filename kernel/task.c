/* cyd-os — native task control and round-robin scheduling.
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
        g_tasks[id].switches   = 0;
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
    if (g_current >= 0) {
        g_tasks[g_current].sp = current_sp;
    }

    /* Round robin from the one after current, so no task can starve another.
     * With g_current == -1 the first candidate is 0, so the first switch enters
     * whichever task was created first. */
    int next = g_current;
    /* Plain counted loop. GCC is free to emit a zero-overhead LOOP here, and
     * does; that is correct now that _handler_level3 clears PS.EXCM before
     * calling C. This loop previously needed a `volatile` counter to force
     * ordinary branches, which worked but treated the symptom. */
    for (int i = 1; i <= TASK_MAX; i++) {
        int candidate = (g_current + i) % TASK_MAX;
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
            next = candidate;
            break;
        }
    }
    if (next < 0) {
        /* Nothing runnable at all. Returning the interrupted stack pointer is
         * the only safe answer — resuming the boot context beats jumping to a
         * fabricated frame that does not exist. */
        return current_sp;
    }

#if TRACE_SWITCHES > 0
    /* switches == 0 means this task has never run, so its frame is still the
     * one task_create fabricated. Anything else is a frame the handler saved. */
    int fabricated = (g_tasks[next].switches == 0);
    int from = g_current;
#endif

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

void task_yield(void)
{
    /* Bring the comparator deadline forward so the tick — and therefore the
     * switch — happens almost immediately. Reuses the preemption path exactly
     * instead of introducing a second way to switch, which would be a second
     * thing that can be subtly wrong. */
    xt_set_ccompare1(xt_ccount() + 64u);
}
