/* cyd-os — native task control.
 *
 * These are kernel-level tasks: drivers, the eventual VM host, and any work
 * that must run as real machine code. Applications will NOT be tasks of this
 * kind — they run inside the bytecode interpreter, which schedules them at
 * instruction boundaries (UM-CYDOS-001 §4.2). The number of native tasks is
 * expected to stay small.
 *
 * There is no memory protection between them. The ESP32 has no MMU paging, so
 * a native task can corrupt any other. Stack guards catch the most common case
 * (overflow) but nothing catches a wild pointer.
 */

#ifndef CYDOS_TASK_H
#define CYDOS_TASK_H

#include <stdint.h>

#define TASK_MAX          4
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
#define TASK_FRAME_WORDS  21
#define TASK_FRAME_BYTES  96           /* 21 words, padded to 16-byte alignment */

#define TASK_FRAME_IDX_SAR    15
#define TASK_FRAME_IDX_EPC3   16
#define TASK_FRAME_IDX_EPS3   17
#define TASK_FRAME_IDX_LBEG   18
#define TASK_FRAME_IDX_LEND   19
#define TASK_FRAME_IDX_LCOUNT 20

typedef void (*task_entry_fn)(void);

typedef enum {
    TASK_UNUSED = 0,
    TASK_READY,
} task_state_t;

typedef struct {
    uint32_t      sp;                  /* saved stack pointer when not running */
    task_state_t  state;
    uint32_t      switches;            /* times this task has been resumed */
    const char   *name;
    uint32_t     *stack_base;          /* for guard checking; NULL for boot task */
} task_t;

/* Create a runnable task. Returns its id, or -1 if the table is full.
 * The entry function must not return — there is nowhere to return to. */
int task_create(const char *name, task_entry_fn entry);

/* Called from the interrupt handler with the interrupted stack pointer.
 * Saves it against the current task, selects the next, and returns the stack
 * pointer to resume. Round-robin over ready tasks.
 *
 * The boot context is task 0: its stack pointer is captured on the first call
 * rather than being fabricated, so no special "start scheduling" step exists. */
uint32_t task_schedule(uint32_t current_sp);

int      task_current(void);
uint32_t task_switch_count(int id);
int      task_stack_intact(int id);    /* guard word still present? */
uint32_t task_stack_headroom(int id);  /* untouched words remaining */

/* Give up the rest of the current slice by bringing the tick forward.
 * Cooperative yield reusing the timer path rather than a second interrupt
 * source — crude, but it exercises exactly the same switch. */
void task_yield(void);

#endif /* CYDOS_TASK_H */
