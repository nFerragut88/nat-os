/* nat-os — the WiFi OS Interface, implemented. See vendor/windowed/wifi_osi.c.
 *
 * call0, deliberately. The OSI table itself must be windowed because libpp
 * calls it, but the table's only job is to forward: the work happens here,
 * where the kernel's heap, scheduler and tick can be used directly. Each
 * windowed entry crosses back through w2c_callN() in window.S.
 *
 * That split is the design. Writing these bodies in the windowed file would
 * mean either duplicating the kernel's primitives or calling into them across
 * a boundary that does not permit it — the fault that put IllegalInstruction
 * at 0x4008a810 the first time it was tried.
 *
 * ---- static pools, not the heap ------------------------------------------
 *
 * Semaphores, queues, event groups and timers come from fixed arrays. The blob
 * creates its objects during init and keeps them, so a pool costs a known
 * amount of DRAM and cannot fragment, cannot fail late, and cannot leak.
 *
 * A handle is a tagged index rather than a pointer, so a stale or foreign
 * handle is detected instead of becoming a wild write. The blob is not code
 * this kernel can inspect, and it is the only caller.
 */

#include "wifi_osi_impl.h"
#include "task.h"
#include "timer.h"
#include "heap.h"
#include "critical.h"

#define OSI_SEM_MAX     12u
#define OSI_QUEUE_MAX    8u
#define OSI_EVT_MAX      4u
#define OSI_TIMER_MAX   12u
/* Queue storage is HEAP-ALLOCATED per queue, not a fixed array.
 *
 * It was uint8_t buf[512] inline, and osi_impl_queue_create refused anything
 * larger. The WiFi driver asks for 200 items of 8 bytes -- 1600 -- so every
 * bring-up ended at _wifi_create_queue returning NULL.
 *
 * Raising the constant would have cost OSI_QUEUE_MAX x the new size in .bss
 * whether or not the queues exist: 8 x 2 KB = 16 KB for one queue that needs
 * it. Allocating what is actually asked for costs the 1600 bytes the driver
 * wanted and nothing for the seven unused slots.
 *
 * The cap remains, moved from "per queue, static" to "per queue, sane" -- a
 * queue is still refused rather than truncated, because one silently shorter
 * than requested loses messages under load. */
#define OSI_QUEUE_BYTES 4096u       /* upper bound on a single queue */

#define OSI_MAX_DELAY   0xFFFFFFFFu

/* Tagged handles. The tag is checked on every use. */
#define H_TAG        0x05100000u
#define H_MASK       0xFFF00000u
#define H_MAKE(i)    ((void *)(uintptr_t)(H_TAG | (i)))
#define H_INDEX(h)   (((uintptr_t)(h)) & 0xFFFu)
#define H_OK(h, n)   ((((uintptr_t)(h)) & H_MASK) == H_TAG && H_INDEX(h) < (n))

/* Waiters are a bitmask of task ids. TASK_MAX is 12, so one word covers every
 * task and "wake everyone waiting" is a loop over set bits. */
typedef struct {
    int      used;
    uint32_t count, max, waiters;
} osi_sem_t;

typedef struct {
    int      used;
    uint32_t item_size, capacity, head, tail, len;
    uint32_t waiters_recv, waiters_send;
    uint8_t *buf;               /* heap, sized to len * item_size */
} osi_queue_t;

typedef struct {
    int      used;
    uint32_t bits, waiters;
} osi_evt_t;

typedef struct {
    int      used, armed, periodic;
    uint32_t period_ticks, due_tick;
    void   (*fn)(void *);
    void    *arg;
} osi_timer_t;

static osi_sem_t   g_sem[OSI_SEM_MAX];
static osi_queue_t g_queue[OSI_QUEUE_MAX];
static osi_evt_t   g_evt[OSI_EVT_MAX];
static osi_timer_t g_timer[OSI_TIMER_MAX];
static uint32_t    g_rng = 0x12345678u;

/* ---- blocking ----------------------------------------------------------- */

static void wake_all(uint32_t *mask)
{
    uint32_t crit = crit_enter();
    uint32_t m = *mask;
    *mask = 0;
    crit_exit(crit);
    for (int id = 0; m; id++, m >>= 1) {
        if (m & 1u) {
            task_wake(id);
        }
    }
}

/* Sleeps rather than blocks, even when the caller asked to wait forever.
 *
 * task_block() with a lost wake-up never runs again, and the blob is not code
 * this kernel can audit for that. A sleeping task woken early returns early; a
 * missed wake costs one tick of latency and the caller re-checks its condition
 * anyway. The pessimism is deliberate. */
/* A ceiling on "wait forever".
 *
 * The driver passes OSI_MAX_DELAY for its main queue, which is correct FreeRTOS
 * usage: the WiFi task sleeps until the ISR or a timer posts work. nat-os has
 * neither yet -- _set_intr clamps and counts, the timer entries are stubs -- so
 * nothing can ever post, and both the blob task and the caller that is waiting
 * on it block permanently. Measured: the shell stops answering entirely.
 *
 * An infinite wait for an event that cannot occur is a hang, and a hang reports
 * nothing. Capping it turns that into a timeout the driver already knows how to
 * handle, and lets esp_wifi_init_internal return an error we can read. The cap
 * is a bring-up scaffold, not a design: when interrupts are wired it should go,
 * and the counter below is what will say whether it still fires. */
#define OSI_FOREVER_CAP 400u        /* ~4 s at the current tick */

uint32_t g_osi_capped;              /* times a "forever" wait was cut short */
uint32_t g_osi_capped_where;        /* 1 = sem, 2 = queue_recv, 3 = evt, 4 = queue_send */

static void wait_on(uint32_t *mask, uint32_t ticks)
{
    int me = task_current();
    if (me < 0) {
        return;                     /* pre-scheduler; nothing can block */
    }
    uint32_t crit = crit_enter();
    *mask |= (1u << me);
    crit_exit(crit);

    task_sleep(ticks ? ticks : 1u);

    crit = crit_enter();
    *mask &= ~(1u << me);
    crit_exit(crit);
}

/* ---- semaphores --------------------------------------------------------- */

void *osi_impl_sem_create(uint32_t max, uint32_t init)
{
    uint32_t crit = crit_enter();
    for (uint32_t i = 0; i < OSI_SEM_MAX; i++) {
        if (!g_sem[i].used) {
            g_sem[i].used = 1;
            g_sem[i].count = init;
            g_sem[i].max = max ? max : 1u;
            g_sem[i].waiters = 0;
            crit_exit(crit);
            return H_MAKE(i);
        }
    }
    crit_exit(crit);
    return 0;
}

void osi_impl_sem_delete(void *h)
{
    if (H_OK(h, OSI_SEM_MAX)) {
        g_sem[H_INDEX(h)].used = 0;
    }
}

int32_t osi_impl_sem_take(void *h, uint32_t ticks)
{
    if (!H_OK(h, OSI_SEM_MAX)) {
        return 0;
    }
    osi_sem_t *s = &g_sem[H_INDEX(h)];
    for (uint32_t spent = 0; ; spent++) {
        uint32_t crit = crit_enter();
        if (s->count) {
            s->count--;
            crit_exit(crit);
            return 1;
        }
        crit_exit(crit);
        if (ticks != OSI_MAX_DELAY && spent >= ticks) {
            return 0;
        }
        wait_on(&s->waiters, 1u);
    }
}

int32_t osi_impl_sem_give(void *h)
{
    if (!H_OK(h, OSI_SEM_MAX)) {
        return 0;
    }
    osi_sem_t *s = &g_sem[H_INDEX(h)];
    uint32_t crit = crit_enter();
    if (s->count < s->max) {
        s->count++;
    }
    crit_exit(crit);
    wake_all(&s->waiters);
    return 1;
}

/* ---- queues ------------------------------------------------------------- */

void *osi_impl_queue_create(uint32_t len, uint32_t item_size)
{
    /* Refuse rather than truncate. A queue silently shorter than asked for
     * loses messages under load, which surfaces as the blob misbehaving. */
    if (!item_size || !len || len * item_size > OSI_QUEUE_BYTES) {
        return 0;
    }
    uint8_t *buf = (uint8_t *)heap_alloc(len * item_size);
    if (!buf) {
        return 0;
    }
    uint32_t crit = crit_enter();
    for (uint32_t i = 0; i < OSI_QUEUE_MAX; i++) {
        if (!g_queue[i].used) {
            osi_queue_t *q = &g_queue[i];
            q->used = 1;
            q->buf = buf;
            q->item_size = item_size;
            q->capacity = len;
            q->head = q->tail = q->len = 0;
            q->waiters_recv = q->waiters_send = 0;
            crit_exit(crit);
            return H_MAKE(i);
        }
    }
    crit_exit(crit);
    heap_free(buf);             /* no slot free -- do not leak the storage */
    return 0;
}

void osi_impl_queue_delete(void *h)
{
    if (!H_OK(h, OSI_QUEUE_MAX)) {
        return;
    }
    osi_queue_t *q = &g_queue[H_INDEX(h)];

    /* Free the storage, not just the slot. It became a heap allocation when
     * the fixed 512-byte array was removed; marking the slot unused and
     * walking away would leak 1600 bytes per WiFi bring-up. */
    uint32_t crit = crit_enter();
    uint8_t *buf = q->buf;
    q->buf  = 0;
    q->used = 0;
    crit_exit(crit);
    heap_free(buf);
}

static void copy_n(uint8_t *d, const uint8_t *s, uint32_t n)
{
    while (n--) {
        *d++ = *s++;
    }
}

int32_t osi_impl_queue_send(void *h, void *item, uint32_t ticks, int to_front)
{
    if (!H_OK(h, OSI_QUEUE_MAX) || !item) {
        return 0;
    }
    osi_queue_t *q = &g_queue[H_INDEX(h)];
    for (uint32_t spent = 0; ; spent++) {
        uint32_t crit = crit_enter();
        if (q->len < q->capacity) {
            uint32_t slot;
            if (to_front) {
                q->head = (q->head + q->capacity - 1u) % q->capacity;
                slot = q->head;
            } else {
                slot = q->tail;
                q->tail = (q->tail + 1u) % q->capacity;
            }
            copy_n(&q->buf[slot * q->item_size], item, q->item_size);
            q->len++;
            crit_exit(crit);
            wake_all(&q->waiters_recv);
            return 1;
        }
        crit_exit(crit);
        if (ticks != OSI_MAX_DELAY && spent >= ticks) {
            return 0;
        }
        wait_on(&q->waiters_send, 1u);
    }
}

int32_t osi_impl_queue_recv(void *h, void *item, uint32_t ticks)
{
    if (!H_OK(h, OSI_QUEUE_MAX) || !item) {
        return 0;
    }
    osi_queue_t *q = &g_queue[H_INDEX(h)];
    for (uint32_t spent = 0; ; spent++) {
        uint32_t crit = crit_enter();
        if (q->len) {
            copy_n(item, &q->buf[q->head * q->item_size], q->item_size);
            q->head = (q->head + 1u) % q->capacity;
            q->len--;
            crit_exit(crit);
            wake_all(&q->waiters_send);
            return 1;
        }
        crit_exit(crit);
        if (ticks != OSI_MAX_DELAY && spent >= ticks) {
            return 0;
        }
        if (ticks == OSI_MAX_DELAY && spent >= OSI_FOREVER_CAP) {
            g_osi_capped++;
            g_osi_capped_where = 2u;
            return 0;
        }
        wait_on(&q->waiters_recv, 1u);
    }
}

uint32_t osi_impl_queue_waiting(void *h)
{
    return H_OK(h, OSI_QUEUE_MAX) ? g_queue[H_INDEX(h)].len : 0u;
}

/* ---- event groups ------------------------------------------------------- */

void *osi_impl_evt_create(void)
{
    uint32_t crit = crit_enter();
    for (uint32_t i = 0; i < OSI_EVT_MAX; i++) {
        if (!g_evt[i].used) {
            g_evt[i].used = 1;
            g_evt[i].bits = 0;
            g_evt[i].waiters = 0;
            crit_exit(crit);
            return H_MAKE(i);
        }
    }
    crit_exit(crit);
    return 0;
}

void osi_impl_evt_delete(void *h)
{
    if (H_OK(h, OSI_EVT_MAX)) {
        g_evt[H_INDEX(h)].used = 0;
    }
}

uint32_t osi_impl_evt_set(void *h, uint32_t bits)
{
    if (!H_OK(h, OSI_EVT_MAX)) {
        return 0;
    }
    osi_evt_t *e = &g_evt[H_INDEX(h)];
    uint32_t crit = crit_enter();
    e->bits |= bits;
    uint32_t now = e->bits;
    crit_exit(crit);
    wake_all(&e->waiters);
    return now;
}

uint32_t osi_impl_evt_clear(void *h, uint32_t bits)
{
    if (!H_OK(h, OSI_EVT_MAX)) {
        return 0;
    }
    osi_evt_t *e = &g_evt[H_INDEX(h)];
    uint32_t crit = crit_enter();
    e->bits &= ~bits;
    uint32_t now = e->bits;
    crit_exit(crit);
    return now;
}

uint32_t osi_impl_evt_wait(void *h, uint32_t bits, int clear, int all,
                           uint32_t ticks)
{
    if (!H_OK(h, OSI_EVT_MAX)) {
        return 0;
    }
    osi_evt_t *e = &g_evt[H_INDEX(h)];
    for (uint32_t spent = 0; ; spent++) {
        uint32_t crit = crit_enter();
        uint32_t got = e->bits & bits;
        if (all ? (got == bits) : (got != 0u)) {
            if (clear) {
                e->bits &= ~bits;
            }
            crit_exit(crit);
            return got;
        }
        crit_exit(crit);
        if (ticks != OSI_MAX_DELAY && spent >= ticks) {
            return e->bits;
        }
        wait_on(&e->waiters, 1u);
    }
}

/* ---- software timers ---------------------------------------------------- */

void *osi_impl_timer_alloc(void)
{
    uint32_t crit = crit_enter();
    for (uint32_t i = 0; i < OSI_TIMER_MAX; i++) {
        if (!g_timer[i].used) {
            g_timer[i].used = 1;
            g_timer[i].armed = 0;
            g_timer[i].fn = 0;
            crit_exit(crit);
            return &g_timer[i];
        }
    }
    crit_exit(crit);
    return 0;
}

/* Validates by identity against the pool rather than trusting the pointer. */
static osi_timer_t *timer_of(void *p)
{
    for (uint32_t i = 0; i < OSI_TIMER_MAX; i++) {
        if ((void *)&g_timer[i] == p) {
            return &g_timer[i];
        }
    }
    return 0;
}

void osi_impl_timer_setfn(void *p, void *fn, void *arg)
{
    osi_timer_t *t = timer_of(p);
    if (t) {
        t->fn = (void (*)(void *))fn;
        t->arg = arg;
    }
}

static void arm_ticks(void *p, uint32_t ticks, int periodic)
{
    osi_timer_t *t = timer_of(p);
    if (!t) {
        return;
    }
    if (!ticks) {
        ticks = 1u;
    }
    t->period_ticks = ticks;
    t->due_tick = timer_ticks() + ticks;
    t->periodic = periodic;
    t->armed = 1;
}

void osi_impl_timer_arm(void *p, uint32_t ms, int periodic)
{
    arm_ticks(p, ms / 10u, periodic);           /* the tick is 10 ms */
}

/* Microsecond arming, rounded up to a tick.
 *
 * The tick is 10 ms, so anything shorter than that becomes one tick. The blob
 * asks for microsecond timers and will not get them; this is recorded rather
 * than hidden because a timer that fires late is a plausible cause of a
 * protocol timeout later, and a caller reading this should know. */
void osi_impl_timer_arm_us(void *p, uint32_t us, int periodic)
{
    arm_ticks(p, us / 10000u, periodic);
}

void osi_impl_timer_disarm(void *p)
{
    osi_timer_t *t = timer_of(p);
    if (t) {
        t->armed = 0;
    }
}

void osi_impl_timer_done(void *p)
{
    osi_timer_t *t = timer_of(p);
    if (t) {
        t->armed = 0;
        t->used = 0;
        t->fn = 0;
    }
}

/* The service task, created on demand.
 *
 * Not spawned at boot: nothing arms an OSI timer until the radio comes up, and
 * a permanently-resident task costs a stack out of a table that holds twelve.
 * Not folded into idle either — idle only runs when nothing else wants the CPU,
 * so a busy 3D view would starve every timer the blob depends on. That is the
 * kind of "works until it matters" wiring this kernel has already paid for
 * three times.
 *
 * Callbacks therefore run in ordinary task context and may block. */
static int g_service_task = -1;

static void osi_service_task(void)
{
    for (;;) {
        osi_impl_timer_service();
        task_sleep(1u);
    }
}

int osi_impl_service_start(void)
{
    if (g_service_task >= 0) {
        return g_service_task;
    }
    g_service_task = task_create("wifitmr", osi_service_task);
    return g_service_task;
}

void osi_impl_timer_service(void)
{
    uint32_t now = timer_ticks();
    for (uint32_t i = 0; i < OSI_TIMER_MAX; i++) {
        osi_timer_t *t = &g_timer[i];
        if (!t->used || !t->armed || !t->fn) {
            continue;
        }
        if ((int32_t)(now - t->due_tick) >= 0) {
            if (t->periodic) {
                t->due_tick = now + t->period_ticks;
            } else {
                t->armed = 0;
            }
            t->fn(t->arg);
        }
    }
}

/* ---- memory ------------------------------------------------------------- */

void *osi_impl_malloc(uint32_t n) { return heap_alloc(n ? n : 1u); }
void  osi_impl_free(void *p)      { heap_free(p); }

void *osi_impl_calloc(uint32_t count, uint32_t size)
{
    uint32_t n = count * size;
    uint8_t *p = heap_alloc(n ? n : 1u);
    if (p) {
        for (uint32_t i = 0; i < n; i++) {
            p[i] = 0;
        }
    }
    return p;
}

uint32_t osi_impl_free_heap(void) { return heap_free_bytes(); }

/* ---- misc --------------------------------------------------------------- */

/* xorshift32.
 *
 * The ESP32 has a hardware RNG, but it is only properly random while the radio
 * is running — which is the thing being brought up. This is deterministic on
 * purpose rather than by accident, and should be replaced once the PHY is live
 * if anything security-relevant ever depends on it. */
uint32_t osi_impl_random(void)
{
    uint32_t x = g_rng;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    g_rng = x;
    return x;
}

uint32_t osi_impl_ms_to_tick(uint32_t ms) { return (ms / 10u) ? (ms / 10u) : 1u; }
void     osi_impl_delay(uint32_t ticks)   { task_sleep(ticks ? ticks : 1u); }
int32_t  osi_impl_current_task(void)      { return task_current(); }
uint32_t osi_impl_time_us_lo(void)        { return timer_ticks() * 10000u; }

uint32_t osi_impl_sems_used(void)
{
    uint32_t n = 0;
    for (uint32_t i = 0; i < OSI_SEM_MAX; i++) { n += (uint32_t)g_sem[i].used; }
    return n;
}

uint32_t osi_impl_queues_used(void)
{
    uint32_t n = 0;
    for (uint32_t i = 0; i < OSI_QUEUE_MAX; i++) { n += (uint32_t)g_queue[i].used; }
    return n;
}

uint32_t osi_impl_timers_used(void)
{
    uint32_t n = 0;
    for (uint32_t i = 0; i < OSI_TIMER_MAX; i++) { n += (uint32_t)g_timer[i].used; }
    return n;
}
