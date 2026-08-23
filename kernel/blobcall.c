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
 *      THAT CLAIM WAS WRONG, and stood here for many steps. It read: "the
 *      concern was MEASURED and does not hold", citing wintorture's switch
 *      counter as the control.
 *
 *      The counter was not a control. It incremented on every tick, including
 *      the ones where the scheduler resumed the SAME task -- and `rom_call3`
 *      takes the blob lock, which pins, so during wintorture the scheduler
 *      cannot switch away at all. Six "switches" were six ticks of one task
 *      resuming itself. See next_moves/08 steps 54-55.
 *
 *      With the counter fixed, wintorture reports 0 switches and prints its own
 *      "NONE, so this proves nothing". With the pin disabled so preemption can
 *      actually occur, it PANICS -- StoreProhibited inside _WindowOverflow12.
 *
 *      So windowed frames do NOT survive preemption on this kernel. The pin is
 *      not an optimisation; it is the only thing keeping windowed code alive,
 *      and every result that looked like surviving preemption was obtained
 *      while the pin silently prevented preemption from happening.
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
#include "uart.h"

extern uint32_t g_phy_call_mask;     /* window.S */


/* Written by w2c_call2 in window.S -- see there. */
volatile uint32_t g_win_a0;
volatile uint32_t g_win_sp;
volatile uint32_t g_win_ps[3];       /* [X16/X17] PS, WINDOWSTART, WINDOWBASE just before retw.n */
volatile uint32_t g_win_in[3];       /* [X18] same triple right after entry */
volatile uint32_t g_win_mid[3];      /* [X19] same triple right before callx0 */
volatile uint32_t g_win_seq;         /* [X18] crossings of w2c_call2 */

/* NOT static: vendor/windowed/blob_lock_w.c takes and releases it from windowed
 * code, which is how the blocking path avoids a call0 bridge. See that file. */
mutex_t g_blob_mutex;
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

    /* rom_call4, NOT phy_stack_call -- this runs on the CALLER'S OWN task
     * stack, and that is the whole point of step 27.
     *
     * phy_stack_call moves execution onto `_phy_stack`, a single shared 6 KB
     * buffer. That was safe while the mutex was held for the entire call. It
     * stopped being safe in step 24, when the blocking adapter entries began
     * releasing the lock in order to wait: the waiting context stays PARKED on
     * the shared buffer, with live frames and a saved switch frame on it, while
     * a second context is now free to enter and run on the same 6 KB.
     *
     * Measured. The shell blocked inside esp_wifi_init_internal with sp
     * 0x3ffbfe00 -- inside _phy_stack (0x3ffbe800..0x3ffc0000) -- the blob task
     * ran, and the shell resumed with a0, a1 and WINDOWSTART all zero. Every
     * task stack guard was intact, because the buffer that was overwritten is
     * the one thing with no guard on it.
     *
     * A task stack is private by construction, so a context that blocks on one
     * leaves nothing another context can reach. The private stack keeps the job
     * it was built for -- PHY init, which does not block -- and blockable driver
     * code now runs where the driver's own task already runs. */

    blob_pin();                      /* not preemptible from here */
    uint32_t r = rom_call4(fn, a, b, c, d);
    blob_unpin();

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
/* Which task is currently executing windowed vendor code, or -1.
 *
 * task_schedule() refuses to switch away from it. Set on entry, cleared on
 * exit and around any voluntary block -- the two moments at which switching
 * away is safe, because the window is either not in use or has been spilled. */
/* NOT static: windowed code writes this directly.
 *
 * next_moves/08 step 106. Pinning through a call0 helper is self-defeating on
 * the blocking path -- the helper is itself a call0 frame that can be switched
 * away from with the windowed frame's a1 moved, which is the whole defect. A
 * single store issued from windowed code has no frame and no return sequence,
 * so there is no window in which it can be caught half-done. */
volatile int g_pinned = -1;

/* Bumped by every pin so the scheduler's runaway bound can be PER PIN.
 *
 * Measured, not assumed: the bound was first written as consecutive ticks on
 * which the current task happened to be pinned, and two tasks taking short
 * turns inside the blob accumulate that just as fast as one wedged task does.
 * wincollide reset the board on TG0WDT after two seconds while making perfectly
 * good progress. The counter has to reset when the pin CHANGES, not when a tick
 * happens to land on an unpinned task. */
static volatile uint32_t g_pin_seq;

/* Set at build time to run the experiment wintorture was always supposed to be:
 * windowed frames held across a GENUINE preemption. With the pin in force the
 * scheduler refuses to switch away, so the test never tested anything. */
#ifndef BLOB_PIN_DISABLE
/* [step 163] OFF BY DEFAULT NOW.
 *
 * The pin existed for one reason: windowed frames did not survive preemption
 * (UM-NATOS-038 section 12.3, measured by disabling it and watching wintorture
 * panic). Tier B changed that. Step 162 ran the same bench and got ten genuine
 * preemptions with eight windowed frames live and the checksum correct every
 * time, plus wifiinit clean unpinned.
 *
 * So the reason is gone, and what the pin costs is not small: it forbids
 * preemption inside vendor code, which forces every blocking wait in the radio
 * path to be step 113's busy-spin -- 600 ms of CPU per _queue_recv, and no way
 * to write a wait that sleeps until an interrupt arrives.
 *
 * Kept as a switch rather than deleted. It is the control for every windowed
 * measurement in this log, and step 121's baseline is only reproducible with it
 * back at 0. */
#define BLOB_PIN_DISABLE 1
#endif

int      blob_pinned_task(void) { return BLOB_PIN_DISABLE ? -1 : g_pinned; }
uint32_t blob_pin_seq(void)     { return g_pin_seq; }
void     blob_pin(void)         { g_pinned = task_current(); g_pin_seq++; }
void     blob_unpin(void)       { g_pinned = -1; g_pin_seq++; }

void blob_lock(void)   { blob_call_init(); mutex_lock(&g_blob_mutex); blob_pin(); }

/* Non-blocking halves, for callers that must not block inside call0.
 *
 * blob_lock() reaches mutex_lock(), which calls task_block() and task_yield() --
 * a call0 function that blocks, and therefore the very condition step 104
 * identified. These let windowed code do the waiting itself: try, and if it
 * fails, go back to windowed frames and try again. Neither blocks, so both are
 * safe to call while pinned. */
/* Wake the tasks blob_unlock_w() reported as owed.
 *
 * Split out for the same reason as osi_impl_wake_senders: task_unblock() is
 * call0 and reaches the scheduler, so the windowed release cannot do it. This
 * runs from call0 with the caller pinned, where no switch can land inside. */
void blob_wake_waiters(uint32_t owed);
void blob_wake_waiters(uint32_t owed)
{
    for (int id = 0; id < TASK_MAX && owed; id++, owed >>= 1) {
        if (owed & 1u) { task_unblock(id); }
    }
}

int  blob_trylock(void)      { blob_call_init(); return mutex_try_lock(&g_blob_mutex); }
void blob_unlock_only(void)  { mutex_unlock(&g_blob_mutex); }
void blob_unlock(void) { blob_unpin(); mutex_unlock(&g_blob_mutex); }

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

/* One big stack, for the one task the driver actually creates.
 *
 * Measured, not guessed: esp_wifi_init_internal asks for 6656 bytes. nat-os
 * pool stacks are 2048, and raising the pool to fit would cost 12 * 6656 = 78 KB
 * against a heap of 84 -- so the size lives here, with the one caller that needs
 * it, instead of being charged to every task in the system.
 *
 * A second concurrent blob task wanting more than a pool stack is refused
 * rather than silently squeezed; if that ever happens the counter says so. */
#define BLOB_TASK_STACK_WORDS 1792u             /* 7168 B */
_Static_assert(BLOB_TASK_STACK_WORDS * 4u >= 6656u,
               "blob task stack smaller than the 6656 B the WiFi task requests");
static uint32_t g_blob_stack[BLOB_TASK_STACK_WORDS];
static int      g_blob_stack_taken;

struct blob_task {
    int      used;
    int      id;            /* nat-os task id, -1 until known */
    uint32_t fn;            /* windowed entry in the blob     */
    uint32_t arg;
    uint32_t prio;          /* what the blob asked for, on ITS scale */
    uint32_t want_stack;    /* what the blob asked for, in bytes */
};
static struct blob_task g_bt[BLOB_TASK_MAX];
/* [step 179, Tortoise] Where the worker actually is.
 *
 * blob_task_entry() takes blob_lock() before it may run any windowed code. If
 * the worker never gets past that, it can never send the message the init
 * context is waiting for in _queue_recv, and the two counters below say so
 * without any inference: reached != running means it is stuck on the mutex. */
static uint32_t g_bt_reached, g_bt_running, g_bt_returned;
uint32_t blob_task_reached(void);
uint32_t blob_task_running(void);
uint32_t blob_task_returned(void);
uint32_t blob_task_reached(void)  { return g_bt_reached; }
uint32_t blob_task_running(void)  { return g_bt_running; }
uint32_t blob_task_returned(void) { return g_bt_returned; }
/* [step 179] the blob mutex, read from call0 so the windowed report needs
 * neither the type nor the symbol. */
uint32_t blob_mutex_owner(void);
uint32_t blob_mutex_acq(void);
uint32_t blob_mutex_cont(void);
uint32_t blob_mutex_err(void);
uint32_t blob_mutex_owner(void) { return (uint32_t)g_blob_mutex.owner; }
uint32_t blob_mutex_acq(void)   { return g_blob_mutex.acquisitions; }
uint32_t blob_mutex_cont(void)  { return g_blob_mutex.contentions; }
uint32_t blob_mutex_err(void)   { return g_blob_mutex.errors; }
uint32_t blob_mutex_depth(void);
uint32_t blob_mutex_waiters(void);
uint32_t blob_mutex_granted(void);
uint32_t blob_mutex_depth(void)   { return g_blob_mutex.depth; }
uint32_t blob_mutex_waiters(void) { return g_blob_mutex.waiters; }
uint32_t blob_mutex_granted(void) { return g_blob_mutex.granted; }
static uint32_t g_bt_short;     /* times the request exceeded a nat-os stack */
static uint32_t g_bt_last_want; /* the largest such request, in bytes */
/* [step 181] what the blob last asked for, and what it was mapped to. */
static uint32_t g_bt_last_prio, g_bt_last_lvl;
#define OSI_MAX_PRIO 25u        /* must match osi_s_task_get_max_priority() */

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
    g_bt_reached++;
    blob_lock();
    g_bt_running++;
    (void)rom_call3(g_bt[slot].fn, g_bt[slot].arg, 0u, 0u);
    g_bt_returned++;
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

    /* REFUSED, not merely counted. nat-os stacks are a fixed TASK_STACK_WORDS,
     * and a blob task handed less than it asked for does not fail where the
     * mistake is: it overruns its slot and writes through whatever follows.
     *
     * Measured. Creating the task anyway zeroed a BLOCKED task's saved context
     * three slots away -- the shell came back from a wait with a0, a1 and
     * WINDOWSTART all zero and died on the retw in w2c_call2, an
     * IllegalInstruction that looks like a register-window bug and is not one.
     *
     * Refusing turns that into pdFAIL, which the driver already handles: it
     * unwinds and reports NO_MEM. A shortfall is a resource limit, and it
     * should read like one. */
    /* Big enough for the pool? Then nothing special is needed. Otherwise the
     * dedicated stack above, once. */
    uint32_t *big = 0; uint32_t big_words = 0u;
    if (r->stack_bytes > (uint32_t)(TASK_STACK_WORDS * 4)) {
        if (!g_blob_stack_taken && r->stack_bytes <= BLOB_TASK_STACK_WORDS * 4u) {
            big = g_blob_stack; big_words = BLOB_TASK_STACK_WORDS;
        }
    }

    if (r->stack_bytes > (uint32_t)(TASK_STACK_WORDS * 4) && !big) {
        g_bt_short++;
        g_bt_last_want = r->stack_bytes;
        /* Reported from HERE rather than from the shell. shell.c is the first
         * object in .flash.text (kernel/linker.ld), so anything added to it
         * shifts everything the flash MMU maps and walks into the step-7 layout
         * band -- measured: nine lines of uart_puts there hung blob_map. This
         * file is not flash-resident, so the same print is free. */
        uart_puts("   [blobtask] refused: wants ");
        uart_put_dec(r->stack_bytes);
        uart_puts(" B, nat-os task stacks are ");
        uart_put_dec((unsigned int)(TASK_STACK_WORDS * 4));
        uart_puts(" B\n");
        return 0;                                        /* pdFAIL */
    }

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
    g_bt[slot].prio = r->prio;
    g_bt[slot].want_stack = r->stack_bytes;
    crit_exit(crit);

    int id = big ? task_create_with_stack(name ? name : "blob", blob_task_entry,
                                         big, big_words)
                 : task_create(name ? name : "blob", blob_task_entry);
    if (id < 0) {
        g_bt[slot].used = 0;
        return 0;                                        /* pdFAIL */
    }
    if (big) { g_blob_stack_taken = 1; }

    /* [step 181] Honour the requested priority.
     *
     * _task_get_max_priority answers 25 -- ESP-IDF's configMAX_PRIORITIES --
     * because the blob does arithmetic on it, and IDF's own wrapper hands the
     * result straight to xTaskCreatePinnedToCore. Espressif's WiFi task sits
     * near the top of that scale deliberately: their documentation warns that
     * starving the low-level WiFi work destabilises the system.
     *
     * Until now r->prio was read into the request struct and then dropped, so
     * every blob task ran at TASK_PRIO_NORMAL alongside the shell and display.
     * The mapping was always meant to live here -- osi_s_task_get_max_priority
     * says so in as many words, and declines to rescale at its own end because
     * the blob also uses the constant for arithmetic.
     *
     * Three levels against twenty-five, split in thirds. The WiFi task's ~23
     * lands in HIGH, which is the point of the exercise. */
    {
        uint32_t p = r->prio;
        int lvl = (p >= (OSI_MAX_PRIO * 2u) / 3u) ? TASK_PRIO_HIGH
                : (p >= OSI_MAX_PRIO / 3u)        ? TASK_PRIO_NORMAL
                                                  : TASK_PRIO_LOW;
        task_set_priority(id, lvl);
        g_bt_last_prio = p;
        g_bt_last_lvl  = (uint32_t)lvl;
        /* Printed from here for the same reason the refusal is: this file is
         * not flash-resident, and wifiinit no longer reaches the [qr] report. */
        uart_puts("   [blobtask] ");
        uart_puts(name ? name : "blob");
        uart_puts(" prio ");
        uart_put_dec(p);
        uart_puts("/25 -> nat-os level ");
        uart_put_dec((unsigned int)lvl);
        uart_puts(lvl == TASK_PRIO_HIGH ? " (HIGH)\n" : " (not high)\n");
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
uint32_t blob_task_last_prio(void);
uint32_t blob_task_last_lvl(void);
uint32_t blob_task_last_prio(void) { return g_bt_last_prio; }
uint32_t blob_task_last_lvl(void)  { return g_bt_last_lvl; }
uint32_t blob_task_want_stack(void)  { return g_bt_last_want ? g_bt_last_want
                                                            : g_bt[0].want_stack; }
