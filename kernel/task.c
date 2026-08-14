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
    for (int i = 1; i <= TASK_MAX; i++) {
        int candidate = (g_current + i) % TASK_MAX;
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

    g_current = next;
    g_tasks[next].switches++;
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
