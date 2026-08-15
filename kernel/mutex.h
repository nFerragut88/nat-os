/* nat-os — blocking mutex.
 *
 * For sections too long to hold with interrupts masked. A task that cannot
 * acquire the lock blocks and is removed from the scheduler's rotation, so
 * waiting costs nothing beyond the switches in and out — unlike a spin, which
 * would burn the waiter's entire quantum discovering it still cannot proceed.
 *
 * Recursive. A task may take a mutex it already owns; the lock releases when
 * the matching number of unlocks have happened. That is not the purist choice,
 * but a console lock is exactly the kind of thing that acquires a nested
 * acquisition by accident, and self-deadlock on a kernel with no debugger is
 * an expensive way to learn about it.
 *
 * Release HANDS THE LOCK to a waiter rather than freeing it and letting
 * everyone race. An earlier version did the latter, reasoning that with at most
 * eight tasks a thundering herd was not worth managing. That reasoning was
 * wrong, and measurably so: worker-a acquired the lock 238,542 times while
 * worker-b, blocked and repeatedly woken, won it exactly zero times. A task in
 * a tight lock/unlock loop re-acquires long before a woken waiter is next
 * scheduled, so race-on-release is not merely unfair, it can starve a waiter
 * indefinitely.
 *
 * Handing off makes ownership transfer atomically with the wakeup: the
 * successor is the owner before it runs again, so nobody can cut in front. The
 * cost is that "owner" names a task that is not yet on the CPU, which is the
 * complication the first design was trying to avoid — and is worth paying.
 *
 * NOT usable from an interrupt handler. A handler cannot block, and there is no
 * context to block. Kernel structures that a handler touches must use a
 * critical section instead (critical.h).
 */

#ifndef NATOS_MUTEX_H
#define NATOS_MUTEX_H

#include <stdint.h>

/* Distinct from any task id INCLUDING the boot context's -1. Using -1 for
 * "free" would make an unheld mutex indistinguishable from one held by the
 * pre-scheduler boot context, and the free/recursive tests would both match. */
#define MUTEX_FREE (-2)

typedef struct {
    int      owner;         /* task id holding it, MUTEX_FREE if unheld */
    uint32_t depth;         /* recursion count */
    uint32_t waiters;       /* bitmask of blocked task ids */
    uint32_t granted;       /* handed the lock but not yet resumed */
    uint32_t acquisitions;  /* uncontended + contended */
    uint32_t contentions;   /* times a task had to block */
    uint32_t errors;        /* unlock by a non-owner */
} mutex_t;

void mutex_init(mutex_t *m);
void mutex_lock(mutex_t *m);
void mutex_unlock(mutex_t *m);

/* Never blocks. Returns non-zero if the lock was taken. */
int  mutex_try_lock(mutex_t *m);

int  mutex_owner(const mutex_t *m);

#endif /* NATOS_MUTEX_H */
