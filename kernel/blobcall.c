/* nat-os — calling the vendor blob with the scheduler still running.
 *
 * next_moves/08 step 11. phy_stack_call() masked interrupts for the whole
 * blob call, and the WiFi driver has now reached the point where that cannot
 * work: it wants _task_create_pinned_to_core, and a task created inside a
 * masked call can never run. A _semphr_take waiting on that task can never
 * return either.
 *
 * ---- what the masking was actually protecting -------------------------
 *
 * Two things, and only one of them needed interrupts off.
 *
 *   1. WINDOW STATE. nat-os's level-3 handler saves a0..a15 and not
 *      WINDOWBASE/WINDOWSTART, so the concern was a context switch landing
 *      while windowed frames were live.
 *
 *      That concern was MEASURED and does not hold. `wintorture` holds eight
 *      live windowed frames across real context switches -- confirmed by the
 *      switch counter, which is the control -- and the checksum is correct
 *      6/6. A call0 task cannot disturb them: the handler saves and restores
 *      the sixteen registers at the current WINDOWBASE and never moves
 *      WINDOWBASE, and call0 code never rotates the window, so a windowed
 *      task's caller frames sit at window positions no other task can reach.
 *
 *      The real hazard is TWO CONTEXTS inside windowed code at once, which is
 *      an exclusion problem, not a preemption problem.
 *
 *   2. THE PRIVATE STACK. `_phy_stack` is a single shared 6 KB buffer. Two
 *      contexts entering phy_stack_call would corrupt each other whatever the
 *      window did.
 *
 * A mutex answers both, and unlike a masked interrupt it lets the scheduler
 * keep running -- which is the whole point.
 *
 * ---- what this does NOT cover -----------------------------------------
 *
 * An interrupt handler cannot take a mutex. If a WiFi ISR ever calls into the
 * blob, this is not enough and the exclusion has to be reconsidered. Nothing
 * does that today; `_set_intr` clamps priorities to CRIT_LEVEL and counts it,
 * so the day it matters is visible rather than silent.
 */

#include <stdint.h>
#include "blobcall.h"
#include "mutex.h"
#include "window.h"
#include "critical.h"
#include "task.h"

extern uint32_t g_phy_call_mask;     /* window.S */

static mutex_t g_blob_mutex;
static int     g_ready;

static uint32_t g_calls;             /* blob entries made through here */
static uint32_t g_contended;         /* times a second context had to wait */

void blob_call_init(void)
{
    uint32_t crit = crit_enter();
    if (!g_ready) {
        mutex_init(&g_blob_mutex);
        g_ready = 1;
    }
    crit_exit(crit);
}

uint32_t blob_call(uint32_t fn, uint32_t a, uint32_t b, uint32_t c, uint32_t d)
{
    blob_call_init();

    /* Contention is counted rather than assumed away. Today there is exactly
     * one caller, so this should stay zero; if it does not, something has
     * started entering the blob from a second context and the assumptions
     * above are worth re-reading. */
    if (mutex_owner(&g_blob_mutex) >= 0) {
        g_contended++;
    }
    mutex_lock(&g_blob_mutex);

    /* Scheduler stays alive for the duration. The mutex is what keeps a
     * second context out; interrupts no longer have to be. */
    g_phy_call_mask = 0u;
    uint32_t r = phy_stack_call(fn, a, b, c, d);
    g_phy_call_mask = 1u;

    g_calls++;
    mutex_unlock(&g_blob_mutex);
    return r;
}

/* Held while a context is executing blob code; released when that context is
 * about to BLOCK inside it. The adapter's blocking entries spill their window
 * first, so the blocked task is left with exactly one live frame -- the call0
 * steady state the existing context switch already handles correctly.
 *
 * That is the whole idea: rather than teach the scheduler about windows, make
 * a blocked windowed task stop looking like one. */
void blob_lock(void)   { blob_call_init(); mutex_lock(&g_blob_mutex); }
void blob_unlock(void) { mutex_unlock(&g_blob_mutex); }

uint32_t blob_call_count(void)     { return g_calls; }
uint32_t blob_call_contended(void) { return g_contended; }

/* ---- tasks the blob asks us to create ---------------------------------
 *
 * The blob hands over a function AND a parameter; task_create() takes a name
 * and a void entry. So each request gets a slot and a trampoline, and the
 * trampoline calls the real function once it can work out which slot is its
 * own.
 *
 * The lookup is by task id and it waits, because the created task may be
 * scheduled before task_create() has returned the id to store -- the scheduler
 * is live now, which is the entire point of blob_call(). Sleeping rather than
 * spinning keeps the race harmless.
 */
#define BLOB_TASK_MAX 4

struct blob_task {
    int      used;
    int      id;            /* nat-os task id, -1 until known */
    uint32_t fn;            /* windowed entry in the blob     */
    uint32_t arg;
    uint32_t want_stack;    /* what the blob asked for, in bytes */
};
static struct blob_task g_bt[BLOB_TASK_MAX];
static uint32_t g_bt_short;     /* times the request exceeded a nat-os stack */

/* Task creation is OPT-IN, and the reason is architectural rather than a bug.
 *
 * A created blob task runs blob code -- windowed -- on its own schedule, while
 * the caller is still inside blob code through phy_stack_call. That is TWO
 * CONTEXTS INSIDE WINDOWED CODE AT ONCE, which is the one case blob_call()'s
 * mutex cannot cover: the WiFi task cannot hold that mutex, because it holds
 * it forever by design.
 *
 * Measured, not predicted: enabling this panics with IllegalInstruction inside
 * osi_s_semphr_take -- a windowed function -- as soon as the new task blocks.
 * The window rotated under a second context and the frames collided.
 *
 * The fix is window-aware context switching: spill the window and save
 * WINDOWBASE/WINDOWSTART when switching away from a task with more than one
 * live frame. See next_moves/08. Until then this stays off, so the default
 * path fails cleanly instead of taking the board down. */
static int g_bt_enabled;
void blob_task_enable(int on) { g_bt_enabled = on; }

static void blob_task_entry(void)
{
    int me = task_current();
    int slot = -1;
    while (slot < 0) {
        for (int i = 0; i < BLOB_TASK_MAX; i++) {
            if (g_bt[i].used && g_bt[i].id == me) { slot = i; break; }
        }
        if (slot < 0) { task_sleep(1u); }
    }
    /* call0 -> windowed, one argument. rom_call3 passes three; the callee
     * takes one and ignores the rest. */
    /* The blob task holds the lock while it runs and releases it whenever it
     * blocks, so only one context is ever inside windowed code at a time
     * without the task having to hold it forever. */
    blob_lock();
    (void)rom_call3(g_bt[slot].fn, g_bt[slot].arg, 0u, 0u);
    blob_unlock();
}

/* Called from the windowed adapter stub. Arguments arrive in a small struct
 * because the w2c bridges carry at most three. */
struct blob_task_req { uint32_t fn, arg, prio, handle, stack_bytes; };

int blob_task_create(void *reqp, const char *name);
int blob_task_create(void *reqp, const char *name)
{
    struct blob_task_req *r = (struct blob_task_req *)reqp;

    if (!g_bt_enabled) {
        g_bt_short++;          /* counted so the refusal is visible */
        return 0;              /* pdFAIL -- driver reports NO_MEM and unwinds */
    }

    /* Record what was ASKED for. nat-os stacks are a fixed TASK_STACK_WORDS,
     * so a blob task wanting more is a real constraint rather than a detail --
     * and one that would present as a stack-guard panic much later. */
    if (r->stack_bytes > (uint32_t)(TASK_STACK_WORDS * 4)) { g_bt_short++; }

    uint32_t crit = crit_enter();
    int slot = -1;
    for (int i = 0; i < BLOB_TASK_MAX; i++) {
        if (!g_bt[i].used) { slot = i; break; }
    }
    if (slot < 0) { crit_exit(crit); return 0; }        /* pdFAIL */
    g_bt[slot].used = 1;
    g_bt[slot].id   = -1;
    g_bt[slot].fn   = r->fn;
    g_bt[slot].arg  = r->arg;
    g_bt[slot].want_stack = r->stack_bytes;
    crit_exit(crit);

    int id = task_create(name ? name : "blob", blob_task_entry);
    if (id < 0) {
        g_bt[slot].used = 0;
        return 0;                                        /* pdFAIL */
    }
    g_bt[slot].id = id;                                  /* trampoline unblocks */

    if (r->handle) { *(uint32_t *)r->handle = (uint32_t)(id + 1); }
    return 1;                                            /* pdPASS */
}

uint32_t blob_task_count(void)
{
    uint32_t n = 0;
    for (int i = 0; i < BLOB_TASK_MAX; i++) { if (g_bt[i].used) { n++; } }
    return n;
}
uint32_t blob_task_stack_short(void) { return g_bt_short; }
uint32_t blob_task_want_stack(void)  { return g_bt[0].want_stack; }
