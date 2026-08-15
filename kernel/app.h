/* nat-os — application table and lifecycle.
 *
 * An application is a bytecode program, an arena it is confined to, and a VM
 * executing it. This layer owns their lifecycle: starting one allocates an
 * arena and loads an image into it; stopping one releases the arena completely,
 * whether it stopped because it finished, because it faulted, or because it was
 * killed.
 *
 * Scheduling here is a THIRD level, above the two that already exist:
 *
 *   1. The timer interrupt preempts native tasks (M2).
 *   2. vm_run()'s quantum returns control at a bytecode boundary (M4).
 *   3. app_tick() round-robins that quantum across every live application.
 *
 * Only the third is aware of applications. The other two are unchanged and do
 * not know this layer exists, which is why adding it required no modification
 * to the scheduler or the interpreter.
 *
 * A faulting application is terminated. It cannot damage another because it
 * cannot address another: a VM offset is arena-relative and bounds-checked, so
 * an address outside its own arena is not merely refused, it is inexpressible.
 */

#ifndef NATOS_APP_H
#define NATOS_APP_H

#include <stdint.h>

#define APP_MAX 4

typedef enum {
    APP_FREE = 0,
    APP_RUNNING,
    APP_HALTED,      /* ran to completion                       */
    APP_FAULTED,     /* violated a rule and was terminated       */
    APP_KILLED       /* stopped from the shell                   */
} app_state_t;

/* Loads `img` into a fresh arena of `arena_bytes` and starts it. `publish_off`
 * is the byte offset within the arena at which the program publishes a progress
 * word, so the kernel can observe it without the program cooperating.
 *
 * Returns an application id, or -1 if no slot or no memory. */
int app_start(const char *name, const uint8_t *img, uint32_t len,
              uint32_t arena_bytes, uint32_t publish_off);

/* Stops an application and releases its arena. Safe on an already-stopped id. */
void app_kill(int id);

/* Gives every RUNNING application one quantum of bytecode. Applications that
 * halt or fault during it are reported and terminated here. */
void app_tick(uint32_t quantum);

int         app_state(int id);
const char *app_state_name(int id);
const char *app_name(int id);
uint32_t    app_instructions(int id);
uint32_t    app_published(int id);
uint32_t    app_arena_bytes(int id);
int         app_fault(int id);
uint32_t    app_fault_detail(int id);
int         app_live_count(void);

#endif /* NATOS_APP_H */
