/* cyd-os — blocking mutex. See mutex.h for the design rationale. */

#include "mutex.h"
#include "critical.h"
#include "task.h"

void mutex_init(mutex_t *m)
{
    m->owner        = MUTEX_FREE;
    m->depth        = 0;
    m->waiters      = 0;
    m->granted      = 0;
    m->acquisitions = 0;
    m->contentions  = 0;
    m->errors       = 0;
}

int mutex_owner(const mutex_t *m) { return m->owner; }

int mutex_try_lock(mutex_t *m)
{
    uint32_t s  = crit_enter();
    int      me = task_current();

    if (m->owner == MUTEX_FREE) {
        m->owner = me;
        m->depth = 1;
        m->acquisitions++;
        crit_exit(s);
        return 1;
    }
    if (m->owner == me) {
        m->depth++;
        crit_exit(s);
        return 1;
    }

    crit_exit(s);
    return 0;
}

void mutex_lock(mutex_t *m)
{
    for (;;) {
        uint32_t s  = crit_enter();
        int      me = task_current();

        /* Checked first: a handoff already made this task the owner while it
         * was blocked, so it must return holding the lock without touching
         * depth. Testing owner == me first would read the handoff as a
         * recursive acquisition and leave depth permanently too high. */
        if (me >= 0 && (m->granted & (1u << me))) {
            m->granted &= ~(1u << me);
            crit_exit(s);
            return;
        }

        if (m->owner == MUTEX_FREE) {
            m->owner = me;
            m->depth = 1;
            m->acquisitions++;
            crit_exit(s);
            return;
        }
        if (m->owner == me) {
            m->depth++;                 /* recursive acquisition */
            crit_exit(s);
            return;
        }

        /* Held by someone else. Join the wait list and mark this task
         * unrunnable, both while still masked so a tick cannot land between
         * them — that split is the classic lost-wakeup: the holder releases,
         * sees an empty wait list, and this task blocks forever. */
        m->contentions++;
        if (me >= 0) {
            m->waiters |= 1u << me;
            task_block();
        }
        crit_exit(s);

        /* Only now allow the switch. If this runs before the scheduler has
         * started — me < 0 — there is no context to block, so the loop simply
         * spins, which is correct during single-threaded boot. */
        task_yield();
    }
}

void mutex_unlock(mutex_t *m)
{
    uint32_t s = crit_enter();

    /* Releasing a lock this task does not hold is a bug in the caller. Counted
     * and refused rather than acted on: clearing another task's ownership would
     * let two tasks into the section and corrupt whatever it protects, far from
     * where the mistake was made. */
    if (m->owner != task_current()) {
        m->errors++;
        crit_exit(s);
        return;
    }

    if (m->depth > 1) {
        m->depth--;
        crit_exit(s);
        return;
    }

    /* Hand the lock to the longest-waiting candidate rather than releasing it.
     * Ownership transfers here, atomically with the wakeup, so a task still
     * spinning in its own lock/unlock loop cannot take it first. */
    if (m->waiters != 0u) {
        int next = -1;
        for (int id = 0; id < TASK_MAX; id++) {
            if (m->waiters & (1u << id)) {
                next = id;
                break;
            }
        }
        m->waiters &= ~(1u << next);
        m->granted |= 1u << next;
        m->owner    = next;
        m->depth    = 1;
        m->acquisitions++;
        task_unblock(next);
        crit_exit(s);
        return;
    }

    m->depth = 0;
    m->owner = MUTEX_FREE;
    crit_exit(s);
}
