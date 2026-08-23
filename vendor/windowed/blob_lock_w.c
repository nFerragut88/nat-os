/* nat-os — the blob lock's non-blocking halves, compiled WINDOWED.
 *
 * next_moves/08 step 111. Companion to vendor/windowed/wifi_osi_queue.c, and the
 * same reasoning: a bridge does `entry a1, N` then `callx0`, so the call0 callee
 * shares the bridge's register window and moves its `a1`. Step 110 removed that
 * bridge from the queue poll and the fault persisted, because three call0
 * excursions remain on the blocking path -- `blob_trylock`, `blob_unlock_only`
 * and the sender wake -- each reached through `w2c_call0f` or `w2c_call1`, which
 * have the identical shape.
 *
 * The bridges are the pattern, not any one callee. This removes the last three
 * from that path.
 *
 * ---- what this may and may not do ---------------------------------------
 *
 * It may take and release the lock, because those are a compare and a store
 * under a masked interrupt, and nothing else.
 *
 * It may NOT wake anybody. `task_wake()` and `task_unblock()` are call0 and
 * reach the scheduler; calling them from here reinstates exactly the boundary
 * this file exists to remove. So `blob_unlock_w()` reports which tasks are owed
 * a wake and the caller does it through a bridge, pinned, where no switch can
 * land inside.
 *
 * It also does NOT hand ownership over the way `mutex_unlock()` does. That
 * transfer exists so a spinning task cannot steal the lock from a waiter, and it
 * requires `task_unblock()`. Here the lock is simply released; a waiter takes it
 * on its next try. The cost is fairness, not correctness, and the blocking path
 * retries in windowed frames anyway.
 *
 * ---- layout agreement ----------------------------------------------------
 *
 * This must match kernel/mutex.h. The assert turns drift into a build error
 * rather than a silent wrong-offset write, which is the failure mode that cost
 * this project a full session on wifi_init_config_t.
 */

typedef unsigned int u32;

typedef struct {
    int  owner;
    u32  depth;
    u32  waiters;
    u32  granted;
    u32  acquisitions;
    u32  contentions;
    u32  errors;
} mutex_w_t;

_Static_assert(sizeof(mutex_w_t) == 28u,
               "mutex_w_t layout diverged from kernel/mutex.h");

/* [step 180] MUST equal kernel/mutex.h's MUTEX_FREE, which is -2, not -1.
 *
 * This mirror had -1, and the two halves of the same mutex therefore disagreed
 * about what "free" means:
 *
 *   blob_unlock_w() released by writing owner = -1;
 *   mutex_lock() waits for owner == MUTEX_FREE (-2), so it never saw the
 *     release, re-blocked, and the woken waiter went straight back to sleep;
 *   mutex_unlock() releases by writing -2, which blob_trylock_w() then read as
 *     still-held.
 *
 * mutex.h says why -1 is the wrong value: it is the pre-scheduler boot
 * context's task id, so "free" and "held by boot" would be indistinguishable.
 *
 * The static assert on sizeof(mutex_w_t) caught layout drift but could not
 * catch a value that had drifted. */
#define MUTEX_FREE_W  (-2)

extern mutex_w_t g_blob_mutex;      /* kernel/blobcall.c */
extern volatile int g_pinned;       /* kernel/blobcall.c, written directly */

/* Take the lock if it is free, or if we already hold it. Never blocks.
 * Returns non-zero on success. */
int blob_trylock_w(int me);

int blob_trylock_w(int me)
{
    u32 ps;
    int got = 0;

    __asm__ volatile ("rsil %0, 3" : "=r"(ps));
    if (g_blob_mutex.owner == MUTEX_FREE_W) {
        g_blob_mutex.owner = me;
        g_blob_mutex.depth = 1u;
        g_blob_mutex.acquisitions++;
        got = 1;
    } else if (g_blob_mutex.owner == me) {
        g_blob_mutex.depth++;
        g_blob_mutex.acquisitions++;
        got = 1;
    } else {
        g_blob_mutex.contentions++;
    }
    __asm__ volatile ("wsr.ps %0; rsync" :: "r"(ps));

    return got;
}

/* [step 180] Release EVERY level this task holds, and restore them on the way
 * back. A blocking wait must drop the mutex completely -- that is the whole
 * contract of blocking on a condition -- and blob_unlock_w() cannot, because it
 * unwinds one level of recursion.
 *
 * Measured: the worker sits at depth 2 (blob_task_entry()'s blob_lock(), plus
 * rom_call3()'s), so the wait loop's single release took it 2 -> 1 and the lock
 * was never freed. waiters held 0x20 -- task 5, registered and never served --
 * for the whole run.
 *
 * Full release is safe here and was not before Tier B: another context entering
 * windowed code while this task has live windowed frames is exactly what the
 * per-task register file (step 162) and per-task stack make legal. It is also
 * what blob_task_entry() already says it does: "holds the lock while it runs and
 * releases it whenever it blocks."
 *
 * Skips are counted apart from errors: the wait loop releases at the top of
 * every iteration, so after a lost re-acquire it legitimately arrives here not
 * holding the lock, and that is not a non-owner unlock bug. */
static u32 g_saved_depth[32];
u32 g_blob_relock_skips;

u32 blob_unlock_all_w(int me);
int blob_relock_all_w(int me);

u32 blob_unlock_all_w(int me)
{
    u32 ps;
    u32 owed = 0u;

    __asm__ volatile ("rsil %0, 3" : "=r"(ps));
    if (g_blob_mutex.owner != me) {
        g_blob_relock_skips++;              /* not ours to release; see above */
    } else {
        if (me >= 0 && me < 32) { g_saved_depth[me] = g_blob_mutex.depth; }
        g_blob_mutex.depth   = 0u;
        g_blob_mutex.owner   = MUTEX_FREE_W;
        owed                 = g_blob_mutex.waiters;
        g_blob_mutex.waiters = 0u;
    }
    __asm__ volatile ("wsr.ps %0; rsync" :: "r"(ps));

    return owed;
}

/* Re-acquire and put the recursion depth back exactly as it was. */
int blob_relock_all_w(int me)
{
    u32 ps;
    int got = 0;

    __asm__ volatile ("rsil %0, 3" : "=r"(ps));
    if (g_blob_mutex.owner == MUTEX_FREE_W) {
        u32 d = (me >= 0 && me < 32) ? g_saved_depth[me] : 0u;
        g_blob_mutex.owner = me;
        g_blob_mutex.depth = d ? d : 1u;
        g_blob_mutex.acquisitions++;
        got = 1;
    } else if (g_blob_mutex.owner == me) {
        g_blob_mutex.depth++;
        g_blob_mutex.acquisitions++;
        got = 1;
    } else {
        g_blob_mutex.contentions++;
    }
    __asm__ volatile ("wsr.ps %0; rsync" :: "r"(ps));

    return got;
}

/* Release. Returns the waiter bitmask the caller owes a wake to, or 0.
 * Refuses a release by a non-owner, counted rather than acted on -- clearing
 * somebody else's ownership would let two contexts into the blob, far from
 * where the mistake was made. */
u32 blob_unlock_w(int me);

u32 blob_unlock_w(int me)
{
    u32 ps;
    u32 owed = 0u;

    __asm__ volatile ("rsil %0, 3" : "=r"(ps));
    if (g_blob_mutex.owner != me) {
        g_blob_mutex.errors++;
    } else if (g_blob_mutex.depth > 1u) {
        g_blob_mutex.depth--;
    } else {
        g_blob_mutex.depth = 0u;
        g_blob_mutex.owner = MUTEX_FREE_W;
        owed = g_blob_mutex.waiters;
        g_blob_mutex.waiters = 0u;
    }
    __asm__ volatile ("wsr.ps %0; rsync" :: "r"(ps));

    return owed;
}
