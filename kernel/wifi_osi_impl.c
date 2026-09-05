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
#include "xtensa.h"
#include "window.h"
#include "intr.h"
#include "timer.h"
#include "blobcall.h"
#include "blob.h"
#include "wifiapp.h"
#include "heap.h"
#include "critical.h"

#define OSI_SEM_MAX     12u
#define OSI_QUEUE_MAX    8u
#define OSI_EVT_MAX      4u
/* [step 191] 24, was 12. With the ETS entries actually bound, esp_wifi_start
 * arms sixteen timers and the pool refused four of them -- measured,
 * "timers=12 refused=4". Refusing a timer the driver believes it armed is
 * exactly the class of silent failure this investigation keeps finding, so
 * the pool is sized above the observed need and g_timer_short stays
 * reported. Each slot is a little over thirty bytes of bss. */
#define OSI_TIMER_MAX 24u
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
    /* [step 209] Recursion, for the mutexes the driver creates with
     * _recursive_mutex_create. A recursive mutex re-entered by the task that
     * already holds it MUST succeed; a binary semaphore re-entered by its own
     * holder deadlocks. nat-os handed the driver a binary semaphore for both,
     * and esp_wifi_80211_tx -- which takes g_wifi_global_lock on a task that
     * was already holding it -- hung on the very first call, forever, with the
     * pool showing "#2=0/1 heldByTask5" and task 5 being the caller itself. */
    int      recursive;
    int      owner;              /* task id holding it, -1 when free */
    uint32_t depth;              /* nesting level of that owner */
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
    /* [step 191] The blob's own ETSTimer *, when this slot is standing in for
     * one. The ETS contract is that the CALLER owns the structure and passes
     * its address, so the handle-shaped lookup below never matched anything the
     * driver passed and every timer call was a silent no-op. */
    uint32_t owner;
} osi_timer_t;

static osi_sem_t   g_sem[OSI_SEM_MAX];
/* NOT static: vendor/windowed/wifi_osi_queue.c polls it from windowed code,
 * which is how the queue receive avoids a call0 bridge entirely. See that file. */
osi_queue_t g_queue[OSI_QUEUE_MAX];
static osi_evt_t   g_evt[OSI_EVT_MAX];
static osi_timer_t g_timer[OSI_TIMER_MAX];
uint32_t g_timer_short;   /* [step 191] ETS timers refused: pool full */
static uint32_t    g_rng = 0x12345678u;
uint32_t g_sem_owner[OSI_SEM_MAX];   /* [step 209] task id + 1 of last taker */

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
/* 400. The value is arbitrary scaffolding, but it is also a probe: UM-NATOS-042
 * section 7 predicted that the underflow's faulting address tracks this constant,
 * and setting it to 460 moved excvaddr from 0x170 to 0x1ac exactly. That
 * identified the word the window handler mistakes for a stack pointer as `spent`,
 * the loop counter below. Changing this constant changes that fault address. */
/* Derived from OSI_FOREVER_CAP_MS. Still 400 ticks at the current tick
 * period, so nothing about this path changes; what changes is that the
 * windowed copy in vendor/windowed/wifi_osi_stubs.c now derives its own
 * bound from the same milliseconds instead of copying these digits into a
 * loop with a different period. See kernel/osi_wait.h. */
#include "osi_wait.h"

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

/* [step 182] _wifi_thread_semphr_get: one semaphore per calling task, made on
 * first use.
 *
 * IDF keeps it in FreeRTOS thread-local storage -- pvTaskGetThreadLocalStorage
 * Pointer(NULL, 0), and xSemaphoreCreateCounting(1, 0) if the slot is empty.
 * nat-os has no TLS, but the slot is only ever indexed by "the current task",
 * so an array indexed by task id is the same thing.
 *
 * It returned NULL until now, and the trace showed the blob asking for it,
 * being refused, and freeing its way back out one call later. */
static void *g_thread_sem[TASK_MAX];

/* [step 186] _read_mac, from eFuse.
 *
 * The stub returned ESP_OK and never wrote the caller's buffer. The blob asked
 * twice, believed it, and faulted in ROM one allocation later (step 185).
 * Reporting success without doing the work is worse than failing: the caller
 * has no way to find out.
 *
 * Layout is ESP-IDF's esp_efuse_table.c, MAC_FACTORY, verbatim -- BLK0 bit
 * offsets, most significant byte first:
 *
 *   mac[0] = bits 72..79     mac[3] = bits 48..55
 *   mac[1] = bits 64..71     mac[4] = bits 40..47
 *   mac[2] = bits 56..63     mac[5] = bits 32..39
 *
 * BLK0 word N covers bits 32N..32N+31, so bits 32..63 are RDATA1 and 64..95 are
 * RDATA2. Read-only registers; no eFuse programming is performed or possible
 * from here.
 *
 * Type derivation is esp_read_mac()'s generate_mac(), for the universes ESP-IDF
 * enables by default on this part. */
#define EFUSE_BLK0_RDATA1  0x3FF5A004u
#define EFUSE_BLK0_RDATA2  0x3FF5A008u

int32_t osi_impl_read_mac(uint8_t *mac, uint32_t type);
int32_t osi_impl_read_mac(uint8_t *mac, uint32_t type)
{
    if (!mac) {
        return -1;                       /* ESP_ERR_INVALID_ARG */
    }
    uint32_t r1 = *(volatile uint32_t *)EFUSE_BLK0_RDATA1;
    uint32_t r2 = *(volatile uint32_t *)EFUSE_BLK0_RDATA2;

    mac[0] = (uint8_t)(r2 >> 8);
    mac[1] = (uint8_t)(r2);
    mac[2] = (uint8_t)(r1 >> 24);
    mac[3] = (uint8_t)(r1 >> 16);
    mac[4] = (uint8_t)(r1 >> 8);
    mac[5] = (uint8_t)(r1);

    switch (type) {
    case 0u: break;                      /* ESP_MAC_WIFI_STA    -- the base   */
    case 1u: mac[5] += 1u; break;        /* ESP_MAC_WIFI_SOFTAP               */
    case 2u: mac[5] += 2u; break;        /* ESP_MAC_BT                        */
    case 3u: mac[5] += 3u; break;        /* ESP_MAC_ETH                       */
    default: return -1;                  /* ESP_ERR_NOT_SUPPORTED             */
    }
    return 0;                            /* ESP_OK, and now it means it       */
}

/* Printed once at init so a wrong MAC is visible rather than inferred. */
void osi_impl_mac_report(void);
void osi_impl_mac_report(void)
{
    uint8_t m[6];
    if (osi_impl_read_mac(m, 0u) != 0) { return; }
    static const char hexd[] = "0123456789abcdef";
    char line[20];
    int  o = 0;
    for (int i = 0; i < 6; i++) {
        if (i) { line[o++] = ':'; }
        line[o++] = hexd[(m[i] >> 4) & 0xF];
        line[o++] = hexd[m[i] & 0xF];
    }
    line[o++] = '\n';
    line[o]   = 0;
    /* [step 186] ROM newlib's syscall stub table pointer. ROM __getreent reads
     * it; the LoadProhibited at __getreent+0x8 with excvaddr 0 says it is not
     * usable. Printed so "nat-os never writes it" stops being an inference. */
    uart_puts("   rom stubs : pro=");
    uart_put_hex(*(volatile uint32_t *)0x3FFAE024u);
    uart_puts(" app=");
    uart_put_hex(*(volatile uint32_t *)0x3FFAE020u);
    uart_puts(line + 17);          /* just the newline */
    uart_puts("   base mac  : ");
    uart_puts(line);
}

/* [step 193] The hardware random number generator.
 *
 * _rand, _random and _get_random all answered 0, and _get_random did it while
 * leaving the caller's buffer untouched -- the same shape as _read_mac before
 * step 186: success reported for work never done, which is worse than failing
 * because the caller has no way to find out. It is called during
 * esp_wifi_start.
 *
 * WDEV_RND_REG is Espressif's own constant, from
 * soc/esp32/include/soc/wdev_reg.h, not a derived one. ESP32 decodes its
 * peripherals at both 0x3FF4xxxx and 0x6000xxxx and the SDK uses the latter for
 * this register; the offset 0x35144 is the same either way.
 *
 * ENTROPY, stated rather than assumed. ESP-IDF documents that this is a true
 * random number generator only while the RF subsystem is running, and a much
 * weaker one otherwise. The WiFi driver's calls arrive after esp_wifi_start,
 * so they are on the good side of that -- but nothing here enforces it, and
 * these entries must not be treated as a cryptographic source on that basis
 * alone. Named so the next person does not have to rediscover it.
 *
 * Read one word per 32 bits, as esp_fill_random does. */
#define WDEV_RND_REG  0x60035144u

int32_t osi_impl_get_random(uint8_t *buf, uint32_t len);
int32_t osi_impl_get_random(uint8_t *buf, uint32_t len)
{
    if (!buf) {
        return -1;                          /* ESP_ERR_INVALID_ARG */
    }
    while (len >= 4u) {
        uint32_t w = *(volatile uint32_t *)WDEV_RND_REG;
        buf[0] = (uint8_t)w;
        buf[1] = (uint8_t)(w >> 8);
        buf[2] = (uint8_t)(w >> 16);
        buf[3] = (uint8_t)(w >> 24);
        buf += 4;
        len -= 4u;
    }
    if (len) {
        uint32_t w = *(volatile uint32_t *)WDEV_RND_REG;
        for (uint32_t i = 0; i < len; i++) {
            buf[i] = (uint8_t)(w >> (8u * i));
        }
    }
    return 0;                               /* ESP_OK, and it means it */
}

/* [step 197] The entries that actually turn the radio on.
 *
 * _wifi_clock_enable, _wifi_clock_disable and _wifi_reset_mac were empty.
 * The driver calls them to ungate the WiFi clock and pulse the MAC out of
 * reset, and neither happened -- the same "success reported for work never
 * done" as _read_mac and _get_random, except here the work is powering the
 * radio. Measured before this: intenable had bit 27 set and the pending
 * register never showed it, so the routing was right and the MAC was off.
 *
 * Constants are Espressif's, from soc/esp32/dport_reg.h:
 *   DPORT_WIFI_CLK_EN_REG  0x3FF000CC   DPORT_WIFI_CLK_WIFI_EN 0x406
 *   DPORT_CORE_RST_EN_REG  0x3FF000D0   DPORT_WIFIMAC_RST      BIT(2)
 *
 * phyinit_run_at() already ungates the shared WIFI_BT_COMMON bits once per
 * boot; these are the WiFi-specific ones the driver expects to control. */
#define DP_WIFI_CLK_EN   0x3FF000CCu
#define DP_WIFI_CLK_BITS 0x00000406u
#define DP_CORE_RST_EN   0x3FF000D0u
#define DP_WIFIMAC_RST   0x00000004u

/* [step 198] _phy_enable: wake the PHY, do not just count the call.
 *
 * ESP-IDF esp_phy_enable() calibrates on the FIRST call and calls
 * phy_wakeup_init() on every one after -- and _phy_disable() is
 * phy_close_rf(), which puts the PHY to sleep. Ours were empty, so once
 * anything slept the radio it was never woken, and no error was reported
 * by anybody, which is precisely the failure this looks like.
 *
 * phyinit_run_at() does the one-time calibration and guards itself, so
 * this is the "every call after" half. */
/* [step 198] The address only, cached. NOT blob_map(): that function
 * reprograms the flash MMU with the cache off, and calling it from inside a
 * blob call -- while executing out of the mapping it is rewriting -- is an
 * IllegalInstruction, measured. wifi_bringup() records the pointer once,
 * while nothing is running out of the blob. */
uint32_t g_phy_wakeup_fn;

/* [step 200] osi_impl_evt_wait takes five arguments and the widest bridge
 * carries three. The stub was calling it with three, so block_time_tick
 * landed where clear_on_exit belongs and wait_for_all and ticks were
 * whatever the registers held.
 *
 * Rather than add a w2c_call5 to window.S -- the file step 194 showed is
 * sensitive to its own size -- the three flag arguments are handed over
 * first and the wait then needs only two. No packing, so nothing is lost:
 * ticks may legitimately be 0xFFFFFFFF. */
static uint32_t g_evt_clear, g_evt_all, g_evt_ticks;

void osi_impl_evt_wait_args(uint32_t clear, uint32_t all, uint32_t ticks);
void osi_impl_evt_wait_args(uint32_t clear, uint32_t all, uint32_t ticks)
{
    g_evt_clear = clear; g_evt_all = all; g_evt_ticks = ticks;
}

uint32_t osi_impl_evt_wait2(void *h, uint32_t bits);
uint32_t osi_impl_evt_wait2(void *h, uint32_t bits)
{
    return osi_impl_evt_wait(h, bits, (int)g_evt_clear, (int)g_evt_all,
                             g_evt_ticks);
}

/* [step 202] The ISR side of the receive path.
 *
 * _queue_send_from_isr returned 0 -- failure -- and posted nothing. The MAC
 * ISR hands each received frame to the driver task through this entry, so
 * every frame was dropped and reported as a failed post. Measured before
 * this: the MAC asserts twice after start and then never again, and the
 * pending register never shows bit 27 thereafter.
 *
 * osi_impl_queue_send takes four arguments and the widest bridge carries
 * three, so this fixes ticks at 0 -- correct from an ISR, which must never
 * block -- and to_front at 0. */
int32_t osi_impl_queue_send_isr(void *h, void *item);
int32_t osi_impl_queue_send_isr(void *h, void *item)
{
    return osi_impl_queue_send(h, item, 0u, 0);
}

/* [step 202] Whether the caller is inside our interrupt trampoline.
 * _is_from_isr answered false unconditionally, which tells the driver it
 * may use the blocking variants from an interrupt. */
volatile uint32_t g_blob_in_isr;

uint32_t osi_impl_in_isr(void);
uint32_t osi_impl_in_isr(void) { return g_blob_in_isr; }

uint32_t osi_impl_phy_wakeup_addr(void);
uint32_t osi_impl_phy_wakeup_addr(void) { return g_phy_wakeup_fn; }

void osi_impl_wifi_clock_enable(void);
void osi_impl_wifi_clock_enable(void)
{
    *(volatile uint32_t *)DP_WIFI_CLK_EN |= DP_WIFI_CLK_BITS;
}

void osi_impl_wifi_clock_disable(void);
void osi_impl_wifi_clock_disable(void)
{
    *(volatile uint32_t *)DP_WIFI_CLK_EN &= ~DP_WIFI_CLK_BITS;
}

void osi_impl_wifi_reset_mac(void);
void osi_impl_wifi_reset_mac(void)
{
    volatile uint32_t *r = (volatile uint32_t *)DP_CORE_RST_EN;
    *r |= DP_WIFIMAC_RST;
    *r &= ~DP_WIFIMAC_RST;
}

void *osi_impl_thread_sem_get(void);
void *osi_impl_thread_sem_get(void)
{
    int me = task_current();
    if (me < 0 || me >= TASK_MAX) {
        return 0;
    }
    if (!g_thread_sem[me]) {
        g_thread_sem[me] = osi_impl_sem_create(1u, 0u);   /* counting(1, 0) */
    }
    return g_thread_sem[me];
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
        if (s->recursive && s->depth && s->owner == task_current()) {
            s->depth++;                      /* already ours: re-enter */
            crit_exit(crit);
            return 1;
        }
        if (s->count) {
            s->count--;
            if (s->recursive) {
                s->owner = task_current();
                s->depth = 1u;
            }
            /* [step 209] Who took it. A count of 0 says a mutex is held; it
             * does not say by whom, and "whom" is the whole question when the
             * holder never gives it back. */
            g_sem_owner[H_INDEX(h)] = (uint32_t)(task_current() + 1);
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
    /* [step 209] Unwind one level of recursion. The mutex only becomes
     * available again at depth zero -- releasing it on the inner unlock would
     * hand it to another task while this one is still inside the region it
     * thinks it owns, which is worse than the deadlock it replaces. */
    if (s->recursive && s->depth && s->owner == task_current()) {
        s->depth--;
        if (s->depth) {
            crit_exit(crit);
            return 1;
        }
        s->owner = -1;
    }
    if (s->count < s->max) {
        s->count++;
    }
    crit_exit(crit);
    wake_all(&s->waiters);
    return 1;
}

/* [step 209] The recursive variant the driver actually asked for.
 * _recursive_mutex_create has been aliased to a plain binary semaphore since
 * the table was written -- the same shape of defect as the timer entries and
 * the event groups: an entry that returns a plausible handle and has the wrong
 * semantics, which nothing in a build or a stub audit can see. */
void *osi_impl_recursive_mutex_create(void);
void *osi_impl_recursive_mutex_create(void)
{
    void *h = osi_impl_sem_create(1u, 1u);
    if (h) {
        osi_sem_t *s = &g_sem[H_INDEX(h)];
        s->recursive = 1;
        s->owner = -1;
        s->depth = 0u;
    }
    return h;
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

/* Wake senders blocked on a queue.
 *
 * Split out because vendor/windowed/wifi_osi_queue.c does the dequeue in
 * windowed code and must not call task_wake() -- that is call0, and calling it
 * from there reintroduces the very boundary that file exists to remove. The
 * windowed poll reports that a wake is owed; this does it, from call0, with the
 * caller pinned so no switch can land inside. */
void osi_impl_wake_senders(void *h);
void osi_impl_wake_senders(void *h)
{
    if (!H_OK(h, OSI_QUEUE_MAX)) { return; }
    wake_all(&g_queue[H_INDEX(h)].waiters_send);
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
    if (!p) { return 0; }
    for (uint32_t i = 0; i < OSI_TIMER_MAX; i++) {
        if (g_timer[i].used && g_timer[i].owner == (uint32_t)p) {
            return &g_timer[i];              /* an ETSTimer the blob owns */
        }
        if ((void *)&g_timer[i] == p) {
            return &g_timer[i];              /* a handle from timer_alloc()  */
        }
    }
    return 0;
}

/* [step 191] Find the slot standing in for this ETSTimer, or take one.
 *
 * ets_timer_setfn()/ets_timer_arm() are both valid first calls on a timer the
 * caller has just allocated, so either has to be able to bind it. */
static osi_timer_t *timer_bind(void *p)
{
    if (!p) { return 0; }
    osi_timer_t *t = timer_of(p);
    if (t) { return t; }

    uint32_t crit = crit_enter();
    for (uint32_t i = 0; i < OSI_TIMER_MAX; i++) {
        if (!g_timer[i].used) {
            g_timer[i].used     = 1;
            g_timer[i].armed    = 0;
            g_timer[i].periodic = 0;
            g_timer[i].fn       = 0;
            g_timer[i].arg      = 0;
            g_timer[i].owner    = (uint32_t)p;
            crit_exit(crit);
            return &g_timer[i];
        }
    }
    crit_exit(crit);
    g_timer_short++;
    return 0;
}

void osi_impl_timer_setfn(void *p, void *fn, void *arg)
{
    osi_timer_t *t = timer_bind(p);
    if (t) {
        t->fn = (void (*)(void *))fn;
        t->arg = arg;
    }
}

static void arm_ticks(void *p, uint32_t ticks, int periodic)
{
    (void)osi_impl_service_start();     /* [step 191] nothing else starts it */
    osi_timer_t *t = timer_bind(p);
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
            /* [step 191] Through blob_call, not directly.
             *
             * The handler is windowed vendor code. A direct call from here --
             * call0 -- is the ABI mismatch step 186 measured twice: the callee
             * never executes ENTRY, so it returns through an a0 the caller
             * never set. blob_call also takes the blob mutex, which the handler
             * needs and a bare call would not provide. */
            (void)blob_call((uint32_t)t->fn, (uint32_t)t->arg, 0u, 0u, 0u);
        }
    }
}

/* ---- memory ------------------------------------------------------------- */

/* [step 127] What the radio actually asks for.
 *
 * The question is whether 79,680 B of heap is enough for an initialised WiFi
 * stack, and it has never been answerable: init stops at OSI call 21, long
 * before the driver allocates in earnest. These make the answer accumulate as
 * init gets further, so the day it does complete the number is already there.
 *
 * Requests are recorded separately from the heap's own accounting on purpose.
 * heap_high_water() is the truth about occupancy; g_osi_alloc_bytes is the
 * truth about demand, and a large gap between them is fragmentation or churn
 * rather than pressure. Neither substitutes for the other.
 *
 * The heap snapshot is taken inside the allocator rather than sampled later,
 * because the peak this is looking for may not survive to any point a shell
 * command could read it. */
uint32_t g_osi_alloc_calls, g_osi_alloc_bytes, g_osi_alloc_max, g_osi_alloc_fails;
uint32_t g_osi_free_calls;
uint32_t g_osi_heap_used, g_osi_heap_hw, g_osi_heap_largest, g_osi_heap_minfree;

static void osi_alloc_note(uint32_t n, const void *p)
{
    g_osi_alloc_calls++;
    g_osi_alloc_bytes += n;
    if (n > g_osi_alloc_max) { g_osi_alloc_max = n; }
    if (!p) { g_osi_alloc_fails++; }

    g_osi_heap_used    = heap_used_bytes();
    g_osi_heap_hw      = heap_high_water();
    g_osi_heap_largest = heap_largest_free();
    {
        uint32_t f = heap_free_bytes();
        if (!g_osi_heap_minfree || f < g_osi_heap_minfree) { g_osi_heap_minfree = f; }
    }
}

/* [step 175] The blob's blocking wait, parking WITHOUT a spill.
 *
 * task_sleep() calls spill_before_parking() as its first act. That is the whole
 * defect on this path, and step 172 missed it by removing the wrong spill:
 *
 *   the bridge's `entry` puts its frame at bridge_sp;
 *   `callx0` into call0 code puts THAT frame at [bridge_sp-16, bridge_sp) --
 *     which is where the ABI keeps the bridge's caller's base save area;
 *   spill_before_parking() then writes the stub's a0..a3 into exactly that
 *     range, on top of live call0 locals, and they overwrite each other;
 *   the bridge's `retw` underflows and reads the wreckage.
 *
 * Measured at step 170: park a1 = 0x3ffb27c0 against a save area of
 * [0x3ffb27c0, 0x3ffb27d0). Exactly coincident.
 *
 * The spill exists because the pin made preemption with live windowed frames
 * fatal. Tier B removed that (step 162) -- the register file is saved and
 * restored per task, so parking with several frames live is safe. So this parks
 * through task_sleep_armed() directly and never spills.
 *
 * That also explains why step 113's spin works: it makes no call0 call after the
 * spill, so nothing is ever allocated over the save area. */
void osi_impl_park(int me, uint32_t ticks);

void osi_impl_park(int me, uint32_t ticks)
{
    extern volatile int g_pinned;

    g_pinned = -1;
    task_arm_wake();
    task_sleep_armed(ticks ? ticks : 1u);
    g_pinned = me;
}

/* ==== [step 177] The radio's interrupts, wired ==========================
 *
 * _set_intr, _set_isr, _ints_on and _ints_off have counted their calls and done
 * nothing since the OSI table was written. UM-NATOS-042 section 9.5 lists them
 * first among what stands between here and a working radio, and nothing above
 * the MAC can work until they do something.
 *
 * The kernel already has the machinery, and the constants were reserved for this
 * in intr.h: INTR_SRC_WIFI_MAC and INTR_LINE_WIFI_MAC.
 *
 * ---- the shape of the problem -------------------------------------------
 *
 * intr_dispatch() calls handlers as `void (*)(void)`, call0, no argument. The
 * blob's ISR is WINDOWED and takes one. So each line gets a call0 trampoline
 * that knows its own number, looks the ISR up, and crosses the ABI boundary.
 *
 * rom_call4 is the crossing, chosen over rom_call3 for one reason: rom_call3
 * takes the blob mutex, and a mutex cannot be taken from an interrupt handler
 * (UM-NATOS-042 section 2.4; UM-NATOS-041 section 5.3 is the same point). It
 * also reserves 32 bytes below its frame and writes its own base save area,
 * which the w2c_* bridges do not -- see UM-NATOS-045 section 8, where that gap
 * is recorded as dormant.
 *
 * ---- what this deliberately does NOT do ----------------------------------
 *
 * No mutual exclusion against the blob task. An ISR firing while the blob task
 * is inside vendor code puts two contexts in windowed vendor code at once, which
 * the blob mutex exists to prevent. The blob has its own answer -- it wraps its
 * critical regions in _wifi_int_disable/_wifi_int_restore, which mask interrupts
 * globally -- and those are already implemented here. Whether that is sufficient
 * is not yet measured, and this comment is the marker for it. */

typedef struct { uint32_t fn, arg; } blob_isr_t;

static volatile blob_isr_t g_blob_isr[32];
volatile uint32_t g_blob_isr_calls[32];
volatile uint32_t g_blob_isr_nofn;      /* line fired with no ISR recorded */
volatile uint32_t g_blob_intr_routed;   /* _set_intr calls that reached the matrix */
volatile uint32_t g_blob_intr_src, g_blob_intr_line, g_blob_intr_prio;

static void blob_isr_run(uint32_t line)
{
    uint32_t fn  = g_blob_isr[line].fn;
    uint32_t arg = g_blob_isr[line].arg;

    if (!fn) {
        g_blob_isr_nofn++;
        return;
    }
    g_blob_isr_calls[line]++;
    g_blob_in_isr++;
    (void)rom_call4(fn, arg, 0u, 0u, 0u);
    g_blob_in_isr--;
}

static void blob_isr_0(void) { blob_isr_run(0u); }
static void blob_isr_1(void) { blob_isr_run(1u); }
static void blob_isr_2(void) { blob_isr_run(2u); }
static void blob_isr_3(void) { blob_isr_run(3u); }
static void blob_isr_4(void) { blob_isr_run(4u); }
static void blob_isr_5(void) { blob_isr_run(5u); }
static void blob_isr_6(void) { blob_isr_run(6u); }
static void blob_isr_7(void) { blob_isr_run(7u); }
static void blob_isr_8(void) { blob_isr_run(8u); }
static void blob_isr_9(void) { blob_isr_run(9u); }
static void blob_isr_10(void) { blob_isr_run(10u); }
static void blob_isr_11(void) { blob_isr_run(11u); }
static void blob_isr_12(void) { blob_isr_run(12u); }
static void blob_isr_13(void) { blob_isr_run(13u); }
static void blob_isr_14(void) { blob_isr_run(14u); }
static void blob_isr_15(void) { blob_isr_run(15u); }
static void blob_isr_16(void) { blob_isr_run(16u); }
static void blob_isr_17(void) { blob_isr_run(17u); }
static void blob_isr_18(void) { blob_isr_run(18u); }
static void blob_isr_19(void) { blob_isr_run(19u); }
static void blob_isr_20(void) { blob_isr_run(20u); }
static void blob_isr_21(void) { blob_isr_run(21u); }
static void blob_isr_22(void) { blob_isr_run(22u); }
static void blob_isr_23(void) { blob_isr_run(23u); }
static void blob_isr_24(void) { blob_isr_run(24u); }
static void blob_isr_25(void) { blob_isr_run(25u); }
static void blob_isr_26(void) { blob_isr_run(26u); }
static void blob_isr_27(void) { blob_isr_run(27u); }
static void blob_isr_28(void) { blob_isr_run(28u); }
static void blob_isr_29(void) { blob_isr_run(29u); }
static void blob_isr_30(void) { blob_isr_run(30u); }
static void blob_isr_31(void) { blob_isr_run(31u); }

static const intr_handler_fn g_blob_tramp[32] = {
    blob_isr_0,
    blob_isr_1,
    blob_isr_2,
    blob_isr_3,
    blob_isr_4,
    blob_isr_5,
    blob_isr_6,
    blob_isr_7,
    blob_isr_8,
    blob_isr_9,
    blob_isr_10,
    blob_isr_11,
    blob_isr_12,
    blob_isr_13,
    blob_isr_14,
    blob_isr_15,
    blob_isr_16,
    blob_isr_17,
    blob_isr_18,
    blob_isr_19,
    blob_isr_20,
    blob_isr_21,
    blob_isr_22,
    blob_isr_23,
    blob_isr_24,
    blob_isr_25,
    blob_isr_26,
    blob_isr_27,
    blob_isr_28,
    blob_isr_29,
    blob_isr_30,
    blob_isr_31
};

/* _set_isr(n, f, arg). Recorded rather than installed: the blob may call this
 * before or after _set_intr, and the trampoline reads the record when it
 * fires, so either order works. */
/* [step 272] Times the blob asked to disable or re-route the scheduler tick.
 * INTR_LINE_TIMER1 is intr.h's own name for it -- internal CCOMPARE1, line 15
 * -- so the guard is written against the kernel's constant rather than a
 * number repeated here. */
uint32_t g_blob_tick_guard;

static uint32_t blob_line_map(uint32_t num);   /* [step 196] defined below */
void osi_impl_set_isr(int32_t n, void *f, void *arg);

void osi_impl_set_isr(int32_t n, void *f, void *arg)
{
    if (n < 0 || n >= 32) {
        return;
    }
    /* [step 196] The remap belongs here too. Step 191 said three places --
     * _set_intr, _ints_on, _ints_off -- and it is four. The driver files its
     * handler under the line IT asked for (0), while the trampoline that
     * actually runs is the one for the line we routed it to (27). Without
     * this the handler is stored where nothing looks, and a MAC interrupt
     * would find g_blob_isr[27].fn == 0 and count itself as nofn. */
    n = (int32_t)blob_line_map((uint32_t)n);
    g_blob_isr[n].fn  = (uint32_t)f;
    g_blob_isr[n].arg = (uint32_t)arg;
}

/* _set_intr(cpu_no, source, num, prio). cpu_no is dropped: this is a
 * single-core kernel and the app CPU is not started. */
void osi_impl_set_intr(uint32_t source, uint32_t num, uint32_t prio);

/* [step 191] Translate the blob's CPU interrupt line onto one nat-os can
 * actually service.
 *
 * The driver asks for what ESP-IDF reserves -- measured, `src=0 line=0 prio=1`,
 * which is ETS_WIFI_MAC_INTR_SOURCE on ETS_WMAC_INUM. nat-os installs ONE
 * interrupt handler, at level 3. Routing source 0 to line 0 faithfully and then
 * unmasking it produced exactly what that implies:
 *
 *     exccause 4  Level1Interrupt   epc 0x40083933  (spi_tx)
 *
 * a priority-1 interrupt taken asynchronously with nothing to service it.
 *
 * The interrupt matrix does not care which line a source lands on, so the line
 * is remapped to INTR_LINE_WIFI_MAC -- 27, priority 3, extern level, which the
 * existing _handler_level3 serves and which UM-NATOS-042 reserved for this and
 * never used. The blob's own number is kept only for reporting.
 *
 * The remap has to be applied in three places or it is worse than useless:
 * here, and in _ints_on/_ints_off, which are handed a MASK of the blob's line
 * numbers. Enabling bit 0 while the handler sits on 27 would arm an unserviced
 * line and disarm nothing. */
static uint32_t blob_line_map(uint32_t num)
{
    return (num == INTR_LINE_WIFI_MAC_BLOB) ? INTR_LINE_WIFI_MAC : num;
}

void osi_impl_set_intr(uint32_t source, uint32_t num, uint32_t prio)
{
    (void)prio;                     /* clamped by the caller; see the stub */
    if (num >= 32u) {
        return;
    }
    num = blob_line_map(num);
    /* [step 272] And it does not get to route its handler onto the tick
     * either, which would replace the scheduler's trampoline. */
    if (num == INTR_LINE_TIMER1) { g_blob_tick_guard++; return; }
    /* [step 191] Record WHAT was routed. nat-os installs only a level-3
     * handler, and ESP-IDF's convention puts the WiFi MAC on CPU interrupt 0,
     * which is priority 1 -- so the line the blob asks for decides whether
     * anything can service it. */
    g_blob_intr_src  = source;
    g_blob_intr_line = num;
    g_blob_intr_prio = prio;
    g_blob_intr_routed++;
    intr_route(source, num, g_blob_tramp[num]);
}

void osi_impl_ints_on(uint32_t mask);
void osi_impl_ints_off(uint32_t mask);

void osi_impl_ints_on(uint32_t mask)
{
    for (uint32_t line = 0u; line < 32u; line++) {
        if (mask & (1u << line)) {
            xt_enable_interrupt(blob_line_map(line));
        }
    }
}

/* [step 272] THE TICK'S LINE IS NOT THE BLOB'S TO TOUCH.
 *
 * timer.c drives the scheduler from CCOMPARE1, which raises internal interrupt
 * 15 at level 3. blob_line_map() remaps only the WiFi MAC line and passes
 * everything else through unchanged, so a mask from the blob with bit 15 set
 * reaches xt_disable_interrupt(15) and stops the tick.
 *
 * That is exactly the fault step 271 left open. The breadcrumb at a reset
 * reads: comparator armed 789738 cycles ahead -- one interval, correct --
 * interrupt level 0, nothing held, watchdog config untouched, and the tick
 * never arrives. A correctly armed one-shot whose interrupt does not come is a
 * masked or disabled line, and this is the only path by which the blob can
 * disable one.
 *
 * Guarded rather than trusted, and COUNTED: if the count is ever non-zero the
 * blob really does ask, and the guard is both the evidence and the fix. */
void osi_impl_ints_off(uint32_t mask)
{
    for (uint32_t line = 0u; line < 32u; line++) {
        if (mask & (1u << line)) {
            uint32_t l = blob_line_map(line);
            if (l == INTR_LINE_TIMER1) { g_blob_tick_guard++; continue; }
            xt_disable_interrupt(l);
        }
    }
}

void *osi_impl_malloc(uint32_t n)
{
    void *p = heap_alloc(n ? n : 1u);
    osi_alloc_note(n, p);
    return p;
}

void  osi_impl_free(void *p)      { g_osi_free_calls++; heap_free(p); }

void *osi_impl_calloc(uint32_t count, uint32_t size)
{
    uint32_t n = count * size;
    uint8_t *p = heap_alloc(n ? n : 1u);
    osi_alloc_note(n, p);
    if (p) {
        for (uint32_t i = 0; i < n; i++) {
            p[i] = 0;
        }
    }
    return p;
}

uint32_t osi_impl_free_heap(void) { return heap_free_bytes(); }

/* ---- misc --------------------------------------------------------------- */

/* [step 193] The hardware RNG, as this function asked to be.
 *
 * It was an xorshift32, and its own comment set the condition for replacing
 * it: "should be replaced once the PHY is live". Step 190 called _phy_enable,
 * so it is.
 *
 * ENTROPY, stated rather than assumed. ESP-IDF documents this register as a
 * true random number generator only while the RF subsystem is running, and a
 * much weaker one otherwise. The driver calls arrive after esp_wifi_start,
 * which is the good side of that -- but nothing here enforces it, and these
 * entries must not be treated as a cryptographic source on that basis alone.
 *
 * g_rng is kept and still stirred, so the xorshift can be restored by
 * reverting one line if the hardware read ever proves unavailable. */
uint32_t osi_impl_random(void)
{
    uint32_t hw = *(volatile uint32_t *)WDEV_RND_REG;
    g_rng ^= hw;
    return hw;
}

uint32_t osi_impl_ms_to_tick(uint32_t ms) { return (ms / 10u) ? (ms / 10u) : 1u; }
void     osi_impl_delay(uint32_t ticks)   { task_sleep(ticks ? ticks : 1u); }
int32_t  osi_impl_current_task(void)      { return task_current(); }
/* [step 210] Was `timer_ticks() * 10000u` -- correct but 10 ms granular and
 * 32-bit, so it wrapped every 71 minutes and a driver polling a deadline saw
 * time frozen for a whole tick. osi_impl_time_us() below is microsecond
 * resolution and 64-bit; _lo latches the high half so both halves come from ONE
 * reading. Sampling the clock twice could straddle a carry and produce a
 * timestamp that never existed, so call order is load-bearing: _lo first. */
static uint32_t g_time_us_hi_latch;

uint32_t osi_impl_time_us_lo(void)
{
    uint64_t v = (uint64_t)osi_impl_time_us();
    g_time_us_hi_latch = (uint32_t)(v >> 32);
    return (uint32_t)v;
}

uint32_t osi_impl_time_us_hi(void)        { return g_time_us_hi_latch; }

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

/* ---- WPA supplicant callback table -- next_moves/08 step 205 ----
 *
 * ESP-IDF's esp_wifi_init() WRAPPER calls esp_supplicant_init(), which calls
 * esp_wifi_register_wpa_cb_internal() with a `struct wpa_funcs`. That wrapper
 * is open-source IDF code and is NOT in the blob: the symbol is an EXPORT with
 * no caller anywhere in its 180k instructions. nat-os calls
 * esp_wifi_init_internal() directly, so g_ic->wpa_cb (+0x1b4) has been NULL
 * since the driver first initialised.
 *
 * A NULL table and an all-zero table each fault, in different places:
 *
 *   cannel_scan_connect_state          wifi_station_start
 *     l32i  a6, a5, 0x1b4                l32i   a9, a3, 0x1b4
 *     l32i  a6, a6, 84                   beqz.n a9, skip     <- TABLE checked
 *     beqz.n a6, skip   <- FN checked    l32i   a10, a9, 0
 *                                        callx8 a10          <- FN not checked
 *
 * So this does not guess which entries matter. Every slot points at one
 * windowed stub that records WHERE IT WAS CALLED FROM, and the driver names
 * the entries it needs. A static scan of the 41 read sites could not: following
 * the destination register forward runs into the register being reused, and it
 * reported offsets like 18 and 22 that cannot be function-pointer slots at all.
 *
 * CALL0, deliberately. Only wpa_cb_stub is windowed, because only wpa_cb_stub
 * is called by the blob. 128 slots is generous; the largest offset read off the
 * table anywhere in the blob is 92. */
extern int wpa_cb_stub(void);            /* windowed; only its address crosses */
extern uint32_t g_wpa_calls, g_wpa_ra[12];

uint32_t g_wpa_table[128];

uint32_t g_appie_pending;

/* [step 241] The handshake needs the passphrase and SSID to derive the PMK.
 * Same gitignored header the connect already uses; accessors so only these two
 * lines ever see the macros. */
#if defined(__has_include)
#  if __has_include("wifi_secrets.h")
#    include "wifi_secrets.h"
#  endif
#endif
const char *wifi_sta_ssid(void);
const char *wifi_sta_ssid(void)
{
#ifdef WIFI_STA_SSID
    return WIFI_STA_SSID;
#else
    return 0;
#endif
}
const char *wifi_sta_pass(void);
const char *wifi_sta_pass(void)
{
#ifdef WIFI_STA_PASS
    return WIFI_STA_PASS;
#else
    return 0;
#endif
}
uint32_t wpa_cb_table_fill(uint32_t sta_connect);
uint32_t wpa_cb_table_fill(uint32_t sta_connect)
{
    for (uint32_t i = 0u; i < 128u; i++) {
        g_wpa_table[i] = (uint32_t)&wpa_cb_stub;
    }
    /* [step 219] Slot 2 -- byte offset 8 -- is wpa_sta_connect, and it is the
     * one entry that stalls an association. Everything else stays a recording
     * stub, because everything else is still only observed and not needed. */
    {
        /* The address is passed IN. blob_map() is not a getter -- it
         * reprograms the flash MMU with the cache off (step 198) -- so the
         * caller, which already holds the entry table, hands it over. */
        extern int wpa_sta_connect_impl(void *bssid);
        extern int wpa_cb_true_stub(void);
        extern uint32_t g_sta_connect_fn;
        /* [step 220] PER ENTRY, from struct wpa_funcs in esp_wifi_driver.h.
         *
         * No single constant works, and both blanket answers were measured
         * wrong. Returning 1 everywhere crashed against a WPA3-capable access
         * point -- LoadProhibited, excvaddr 0x00000001, inside a ROM copy
         * routine -- because entries 7, 11, 14, 18 and 23 return POINTERS and
         * the driver dereferenced the 1. Returning 0 everywhere then hung
         * before set_mode and watchdog-reset the board, because the bool
         * entries treat false as refusal.
         *
         *   bool, must be TRUE : 0 sta_init, 1 sta_deinit, 8 ap_deinit,
         *                        9 ap_join, 10 ap_remove, 12 ap_rx_eapol
         *   bool, must be FALSE: 6 sta_in_4way_handshake -- we are never in
         *                        one, and true would have the driver wait for
         *                        a handshake that cannot complete
         *   pointer, must be 0 : 7 ap_init, 11 ap_get_wpa_ie,
         *                        14 config_parse_string, 18 wpa3_build_sae_msg,
         *                        23 owe_build_dhie
         *   int / void         : 0
         *
         * The header is on this machine. Reading it beat guessing twice. */
        /* [step 247] The per-entry return values moved INTO the trampolines
         * in vendor/windowed/wifi_glue.c, one per slot, so each call can
         * record which slot it was. The values are unchanged -- 1 for
         * slots 0, 1, 8, 9, 10, 12 and 0 for the rest -- and the table above
         * is now their documentation rather than their implementation.
         *
         * Only ADDRESSES cross, and they cross as DATA. Slots 32..127 keep
         * the shared stub. */
        {
            extern uint32_t g_wpa_slot_fn[32];
            for (uint32_t i = 0u; i < 32u; i++) {
                g_wpa_table[i] = g_wpa_slot_fn[i];
            }
        }
        (void)wpa_cb_true_stub;
        g_sta_connect_fn = sta_connect;
        {   /* [step 236] and the appie entry, for the RSN IE. */
            extern uint32_t g_appie_fn;
            g_appie_fn = g_appie_pending;
        }
        g_wpa_table[2] = (uint32_t)&wpa_sta_connect_impl;
        {   /* [step 241] slot 5 is wpa_sta_rx_eapol -- the handshake itself --
             * and slot 6 is wpa_sta_in_4way_handshake. Both were recording
             * stubs; slot 6 answered a constant 0, which was correct only
             * because no handshake could ever be in flight. */
            extern int wpa_sta_rx_eapol_impl(unsigned char *, unsigned char *, unsigned int);
            extern int wpa_sta_in_4way_impl(void);
            g_wpa_table[5] = (uint32_t)&wpa_sta_rx_eapol_impl;
            g_wpa_table[6] = (uint32_t)&wpa_sta_in_4way_impl;
        }
        {   /* [step 246] Slots 3 and 4 -- wpa_sta_connected_cb and
             * wpa_sta_disconnected_cb -- named rather than pooled into the
             * shared recording stub. Instrumentation: it decides whether the
             * association of steps 237-245 ever actually happened. */
            extern void wpa_sta_connected_cb_impl(unsigned char *);
            extern void wpa_sta_disconnected_cb_impl(unsigned char);
            g_wpa_table[3] = (uint32_t)&wpa_sta_connected_cb_impl;
            g_wpa_table[4] = (uint32_t)&wpa_sta_disconnected_cb_impl;
        }
        {   /* [step 248] Slot 15 is wpa_parse_wpa_ie. Step 247 measured it
             * called three times on the failing connect while it answered a
             * confident zero. It now parses the access point RSN element it
             * is handed. */
            extern int wpa_parse_wpa_ie_impl(const unsigned char *,
                                             unsigned int, void *);
            g_wpa_table[15] = (uint32_t)&wpa_parse_wpa_ie_impl;
        }
        {   /* [step 249] Slot 21 is wpa_sta_rx_mgmt, called seven times on
             * every failing connect. Reads its subtype and sender; changes
             * nothing. */
            extern int wpa_sta_rx_mgmt_impl(unsigned char, unsigned char *,
                                            unsigned int, unsigned char *,
                                            signed char, unsigned char);
            g_wpa_table[21] = (uint32_t)&wpa_sta_rx_mgmt_impl;
        }
    }
    g_wpa_calls = 0u;
    return (uint32_t)g_wpa_table;
}

/* Formatted here rather than at the call site: that is wifi_init_cfg.c, where
 * the layout sensitivity was measured, and it gets one call instead of a loop.
 * a0 carries the call-size encoding in its top two bits; the blob runs from
 * 0x403xxxxx. */
/* [step 250] The sniffer's log. Lives here because this is call0 and can
 * print; the callback is windowed and cannot. Same discipline as the crypto
 * self-test: results cross as data. */
/* [step 252] The RSN IE A/B, set from the shell. A GLOBAL, written as data:
 * the flag lives in the windowed file that uses it and only its value
 * crosses. */
void wifi_rsn_ie_enable(int on);
void wifi_rsn_ie_enable(int on)
{
    extern uint32_t g_rsn_ie_enable;
    g_rsn_ie_enable = on ? 1u : 0u;
}

/* [step 256] The passive-EAPOL switch, set from the shell. Data, not a call. */
void wifi_hs_passive(int on);
void wifi_hs_passive(int on)
{
    extern uint32_t g_hs_passive;
    g_hs_passive = on ? 1u : 0u;
}

/* [step 255] The appie type and flag, set from the shell. Data, not a call. */
void wifi_appie_shape(unsigned int type, unsigned int flag);
void wifi_appie_shape(unsigned int type, unsigned int flag)
{
    extern uint32_t g_appie_type, g_appie_flag;
    g_appie_type = type;
    g_appie_flag = flag;
}

/* [step 254] The AKM A/B, set from the shell. Data, not a call. */
void wifi_rsn_akm_set(unsigned int t);
void wifi_rsn_akm_set(unsigned int t)
{
    extern uint32_t g_rsn_akm_type;
    g_rsn_akm_type = t;
}

void wifi_sniff_report(void);
void wifi_sniff_report(void)
{
    extern uint32_t g_snf_total, g_snf_drop_bcn, g_snf_layout, g_snf_kept;
    extern uint8_t  g_snf_fc[24], g_snf_ch[24], g_snf_a1[24][3], g_snf_a2[24][3];
    extern signed char g_snf_rssi[24];
    static const char hx[] = "0123456789abcdef";

    uart_puts("   sniff     seen ");
    uart_put_dec(g_snf_total);
    uart_puts("  beacons/probes ");
    uart_put_dec(g_snf_drop_bcn);
    uart_puts("  layout-miss ");
    uart_put_dec(g_snf_layout);
    uart_puts("  KEPT ");
    uart_put_dec(g_snf_kept);
    if (g_snf_total == 0u) {
        uart_puts("\n             nothing at all -- the callback never fired\n");
        return;
    }
    /* [step 271] The per-frame decode is removed. It did decisive work --
     * steps 250-252 read AUTH status 0 and ASSOC_RESP status 0 off the air and
     * located the fault in the RSN element -- and that is finished. The counts
     * stay, so the sniffer is still visibly wired, and its iram bought the net
     * task that keeps the stack up after the command returns. */
    uart_puts("\n");
}

void wpa_cb_report(void);
void wpa_cb_report(void)
{
    uint32_t n = g_wpa_calls;
    /* [step 246] The driver's own answer to "did it associate". Printed first
     * because it is the result; the return-address list below it is context. */
    {
        extern uint32_t g_wpa_conn_cb, g_wpa_disc_cb, g_wpa_disc_reason;
        uart_puts("  wpa conn=");
        uart_put_dec(g_wpa_conn_cb);
        uart_puts(" disc=");
        uart_put_dec(g_wpa_disc_cb);
        uart_puts(" cbreason=");
        if (g_wpa_disc_reason == 0xFFFFFFFFu) { uart_puts("none"); }
        else { uart_put_dec(g_wpa_disc_reason); }
        uart_puts("\n");
    }
    /* [step 249] The per-frame subtype list is GONE, and step 250's sniffer
     * is why: it reads the same frames off the air with addresses and status
     * codes, so this was printing a worse version of a better measurement.
     * The COUNT stays -- it is the cheap check that slot 21 is still wired --
     * and its iram paid for step 259's starvation report. */
    {
        extern uint32_t g_mgmt_calls;
        extern uint32_t g_sta_connect_calls;
        uart_puts("  wpa mgmt calls=");
        uart_put_dec(g_mgmt_calls);
        uart_puts(" connect=");
        uart_put_dec(g_sta_connect_calls);
        uart_puts("\n");
    }

    /* [step 248] What slot 15 made of the access point's RSN element.
     * Printed as the DECODE, not as a rc: "ok 3" only says it was called. */
    {
        extern uint32_t g_pie_calls, g_pie_ok, g_pie_bad, g_pie_len;
        extern uint32_t g_pie_group, g_pie_pair, g_pie_akm, g_pie_caps;
        uart_puts("  wpa ie   calls=");
        uart_put_dec(g_pie_calls);
        uart_puts(" ok=");
        uart_put_dec(g_pie_ok);
        uart_puts(" bad=");
        uart_put_dec(g_pie_bad);
        uart_puts(" body=");
        uart_put_dec(g_pie_len);
        uart_puts("B  group=");
        uart_put_hex(g_pie_group);
        uart_puts(" pair=");
        uart_put_hex(g_pie_pair);
        uart_puts(" akm=");
        uart_put_hex(g_pie_akm);
        uart_puts(" caps=");
        uart_put_hex(g_pie_caps);
        uart_puts("   (cipher CCMP=0x8, akm PSK=0x2)\n");
    }
    /* [step 265] The step-247 slot NAME table and its 32-slot loop are
     * removed. They did their job: the slots are identified and the ones that
     * matter -- 2, 3, 4, 5, 6, 15, 21 -- are implemented, so the report was
     * printing a list that no longer changes. iram bought the watchdog
     * breadcrumb history with it. The raw hit addresses below still say
     * whether anything unexpected is being called. */
    uart_puts("  wpa hits ");
    uart_put_dec(n);
    if (n > 12u) { n = 12u; }
    for (uint32_t i = 0u; i < n; i++) {
        uart_puts(" ");
        uart_put_hex((g_wpa_ra[i] & 0x3FFFFFFFu) | 0x40000000u);
    }
}

/* [step 206] Print what the scan actually heard, by NAME.
 *
 * ap_num returning 1 is the driver's own count and it is good evidence, but it
 * is still the driver marking its own homework. An SSID is a string that was
 * transmitted by somebody else's hardware and can only have arrived through the
 * antenna, the PHY, the MAC, the interrupt, the queue and the worker. If one
 * comes out of here, every one of those works.
 *
 * Only record ZERO is read, and only its first two fields. wifi_ap_record_t has
 * begun with bssid[6] then ssid[33] in every ESP-IDF version, but the stride
 * between records does NOT come from a header this project can check -- the
 * blob decides it inside wifi_get_ap_list_process. Reading one record needs no
 * stride; reading two would need one that is not in evidence, so it reads one.
 *
 * Lives here rather than at the call site because that is wifi_init_cfg.c,
 * where the layout sensitivity was measured. It gets a single call. */
uint32_t g_ap_expect_ch;   /* [step 208] the channel the sweep asked for */
uint32_t g_assoc_ssid_len; /* [step 219] length of the SSID we tried to join */

void wifi_ap_report(uint32_t fn, uint32_t count);
void wifi_ap_report(uint32_t fn, uint32_t count)
{
    static volatile unsigned short want;
    static uint8_t rec[512];

    if (!fn || !count) { return; }
    /* [step 212] Ask for up to four now, not one. Step 206 read only record
     * zero because the STRIDE between records is not stated by any header this
     * project can check -- the blob decides it inside wifi_get_ap_list_process
     * -- and reading a second record at a guessed offset would have been the
     * exact error step 199 refused to make with scan_type.
     *
     * It is measurable now: a scan finally reported TWO access points, so
     * asking for both and looking at where the second lands in a zeroed buffer
     * measures the stride instead of assuming it. */
    want = (unsigned short)(count > 4u ? 4u : count);
    for (uint32_t i = 0u; i < sizeof rec; i++) { rec[i] = 0u; }

    uint32_t rc = blob_call(fn, (uint32_t)&want, (uint32_t)rec, 0u, 0u);
    uart_puts("   ap[0] rc ");
    uart_put_hex(rc);
    if (rc != 0u) { uart_puts("\n"); return; }

    static const char hex[] = "0123456789abcdef";
    /* [step 212] Every record, at the measured stride of 84. */
    for (uint32_t r = 0u; r < want && r < 4u; r++) {
    const uint8_t *q = &rec[r * 84u];
    uart_puts(r ? "\n            ap[+] bssid " : "  bssid ");
    for (uint32_t i = 0u; i < 6u; i++) {
        uart_putc(hex[(q[i] >> 4) & 0xFu]);
        uart_putc(hex[q[i] & 0xFu]);
        if (i != 5u) { uart_putc(58); }
    }
    uart_puts("  ssid [");
    for (uint32_t i = 6u; i < 38u && q[i]; i++) {
        uart_putc((q[i] >= 32u && q[i] < 127u) ? (char)q[i] : 63);
    }
    uart_puts("]");

    /* [step 212] THE STRIDE IS 84, and it was read out of the blob rather
     * than guessed from a header or inferred from one lucky scan.
     * wifi_get_ap_list_process computes each record's address as:
     *
     *     addx2  a10, a8, a8     ; a8 * 3
     *     subx8  a10, a10, a10   ; * 7        -> a8 * 21
     *     addx4  a10, a10, a7    ; * 4 + base -> base + a8 * 84
     *
     * so sizeof(wifi_ap_record_t) is 84 in THIS blob. That is a deterministic
     * fact about the binary, not an observation that needed two access points
     * to be in range -- which matters, because the second one comes and goes.
     *
     * Still falsified for free, per 7.1: every record returned by a
     * single-channel scan must carry that channel at +39. */
    /* [step 208] Channel and RSSI -- and the layout is CHECKED, not assumed.
     *
     * After ssid[33] the IDF struct is: primary(u8) at +39, second(enum,
     * 4-aligned) at +40, rssi(int8) at +44. Those offsets come from a header
     * whose vintage does not provably match this blob, which is the same
     * reasoning that made step 199 refuse to trust scan_type.
     *
     * But this one is falsifiable for free: +39 should hold the channel this
     * scan was told to dwell on, and that value is already known. If it
     * matches, the layout is right and the byte at +44 really is the RSSI. If
     * it does not, the line says so and no dBm figure is printed. */
    uart_puts(" ch@39=");
    uart_put_dec(q[39]);
    if (q[39] == g_ap_expect_ch) {
        int32_t rssi = (int32_t)(int8_t)q[44];
        uart_puts(" layoutOK rssi -");
        uart_put_dec((uint32_t)(0 - rssi));
        uart_puts("dBm");
    } else {
        uart_puts(" MISMATCH want ");
        uart_put_dec(g_ap_expect_ch);
        uart_puts(" -- layout unconfirmed, rssi NOT read");
    }
    }
    uart_puts("\n");
}

/* [step 207] The channel sweep, moved out of wifi_init_cfg.c.
 *
 * PASSES > 1 because "ch 6 found 1 at 150 ms, found 0 at 600 ms" is not a
 * result, it is a single sample of something that varies. One pass cannot tell
 * marginal reception from an environment that genuinely holds two access
 * points. Repeating the same channel and counting how often it answers can.
 *
 * Ordered pass-major (all channels, then all channels again) rather than
 * channel-major on purpose: consecutive scans of one channel would share
 * whatever transient state a single scan leaves behind, and the question is
 * about the radio, not about back-to-back calls. */
#define SWEEP_PASSES 1u   /* [step 227] one pass: evidence, not a survey */
#define SWEEP_DWELL  400u

/* [step 278] Scan results AS DATA, for the wifi view.
 *
 * wifi_ap_report() prints them, which was right when the only consumer was a
 * serial console and is not enough for something that has to lay them out.
 * Same blob calls and the same record layout -- stride 84, ssid at +6, channel
 * at +39, authmode at +40, rssi (int8) at +44 -- every one of which step 212
 * measured out of the blob rather than taking from a header.
 *
 * ONE CHANNEL PER CALL, so the caller can paint progress across a sweep
 * instead of freezing for five seconds. */
uint32_t g_scan_refused;    /* [step 288] channels the driver would not scan */

uint32_t wifi_scan_channel(uint32_t scan_fn, uint32_t num_fn, uint32_t recs_fn,
                           uint32_t ch, wifi_ap_t *out, uint32_t max)
{
    static uint32_t cfg[8] = { 0u, 0u, 1u, 1u, 0u, 0u, SWEEP_DWELL, 0u };
    static volatile unsigned short n;
    static uint8_t rec[512];

    if (!scan_fn || !num_fn || !recs_fn || !out || !max) { return 0u; }

    cfg[2] = ch;
    /* [step 288] A channel the driver REFUSED and a channel that was quiet are
     * both "no results", and telling them apart is the whole difference between
     * "nothing is on the air" and "the radio would not look". Counted, because
     * the reported symptom is inconsistency and inconsistency is a rate. */
    if (blob_call(scan_fn, (uint32_t)cfg, 1u, 0u, 0u) != 0u) {
        g_scan_refused++;
        return 0u;
    }

    n = 0xFFFFu;
    if (blob_call(num_fn, (uint32_t)&n, 0u, 0u, 0u) != 0u) {
        g_scan_refused++;
        return 0u;
    }
    if (n == 0xFFFFu || n == 0u) { return 0u; }

    unsigned short want = (unsigned short)(n > 6u ? 6u : n);
    for (uint32_t i = 0u; i < sizeof rec; i++) { rec[i] = 0u; }
    if (blob_call(recs_fn, (uint32_t)&want, (uint32_t)rec, 0u, 0u) != 0u) {
        return 0u;
    }

    uint32_t got = 0u;
    for (uint32_t r = 0u; r < want && got < max; r++) {
        const uint8_t *q = &rec[r * 84u];
        if (q[6] == 0u) { continue; }        /* hidden: no name to show */
        uint32_t i = 0u;
        for (; i < 32u && q[6u + i]; i++) {
            uint8_t c = q[6u + i];
            out[got].ssid[i] = (c >= 32u && c < 127u) ? (char)c : '?';
        }
        out[got].ssid[i] = 0;
        out[got].ch   = q[39];
        out[got].auth = q[40];
        out[got].rssi = (signed char)q[44];
        got++;
    }
    return got;
}

void wifi_scan_sweep(uint32_t scan_fn, uint32_t num_fn, uint32_t recs_fn);
void wifi_scan_sweep(uint32_t scan_fn, uint32_t num_fn, uint32_t recs_fn)
{
    static uint32_t cfg[8] = { 0u, 0u, 1u, 1u, 0u, 0u, SWEEP_DWELL, 0u };
    static volatile unsigned short n;
    static uint8_t hits[14];
    static uint8_t best[14];

    if (!scan_fn || !num_fn) { return; }
    for (uint32_t i = 0u; i < 14u; i++) { hits[i] = 0u; best[i] = 0u; }

    uart_puts("   scan      passive, 13 channels x ");
    uart_put_dec(SWEEP_PASSES);
    uart_puts(" passes, ");
    uart_put_dec(SWEEP_DWELL);
    uart_puts(" ms dwell\n");

    for (uint32_t p = 0u; p < SWEEP_PASSES; p++) {
        for (uint32_t ch = 1u; ch <= 13u; ch++) {
            cfg[2] = ch;
            uint32_t sc = blob_call(scan_fn, (uint32_t)cfg, 1u, 0u, 0u);
            n = 0xFFFFu;
            (void)blob_call(num_fn, (uint32_t)&n, 0u, 0u, 0u);
            if (sc == 0u && n != 0xFFFFu && n > 0u) {
                hits[ch]++;
                if (n > best[ch]) { best[ch] = (uint8_t)n; }
                uart_puts("   p");
                uart_put_dec(p);
                uart_puts(" ch ");
                uart_put_dec(ch);
                uart_puts(" found ");
                uart_put_dec(n);
                g_ap_expect_ch = ch;
                wifi_ap_report(recs_fn, n);
            }
        }
    }

    /* The whole point of the exercise: how RELIABLY does each channel answer,
     * not whether it answered once. */
    uart_puts("   summary   ");
    for (uint32_t ch = 1u; ch <= 13u; ch++) {
        if (hits[ch]) {
            uart_puts("ch");
            uart_put_dec(ch);
            uart_puts("=");
            uart_put_dec(hits[ch]);
            uart_puts("/");
            uart_put_dec(SWEEP_PASSES);
            uart_puts(" max");
            uart_put_dec(best[ch]);
            uart_puts("  ");
        }
    }
    uart_puts("\n");
    /* [step 211] Again here: SCAN_DONE is posted by the scans, which run after
     * the transmit, so the report before tx could never have contained one. */
    wifi_event_report();
    wifi_rx_report();      /* [step 222] and what the data path caught */
}

/* ---- TRANSMIT -- next_moves/08 step 209 ---------------------------------
 *
 * Every report in this investigation has ended "nothing has been transmitted",
 * and every one of them meant it: the scans were proven passive at step 201 by
 * changing the dwell and watching the duration track it one-for-one. This is
 * the code that ends that sentence, so it is worth being exact about what it
 * does and how it is checked.
 *
 * HOW IT IS VERIFIED, and why not by the return code. esp_wifi_80211_tx
 * returning ESP_OK says the driver accepted a buffer. Step 199 already learned
 * what a success code is worth here -- scan_start returned ESP_OK for six steps
 * while the radio decoded nothing. A transmission can only be confirmed by
 * something that is not this board.
 *
 * So it sends BEACONS. A beacon carries an SSID, and any phone or laptop
 * scanning nearby will list that SSID in its network picker. That is an
 * independent receiver, owned by someone else, displaying a string that this
 * code chose. Nothing about it can be faked by a mistaken return value.
 *
 * The name is deliberately unmistakable and deliberately not anyone else's.
 * Impersonating a real network would be trivial here and is not something this
 * project will do; the SSID says what it is.
 *
 * Channel 1, and not 6 or 11, because those are the two channels the sweep
 * found real access points on and there is no reason to sit on top of them.
 *
 * The frame is a textbook beacon:
 *   fc(2) dur(2) da(6) sa(6) bssid(6) seq(2)
 *   timestamp(8) beacon_interval(2) capability(2)
 *   SSID tag, supported-rates tag, DS-parameter tag
 * built by hand rather than by asking the driver for AP mode, because AP mode
 * would pull in esp_wifi_set_config and a second interface, and the point here
 * is to establish that the transmit path works at all. */

#define TX_BEACONS   20u           /* transmit is proven; keep the run short for RX work */
#define TX_CHANNEL   1u

static uint8_t g_tx_frame[128];

static uint32_t tx_build_beacon(const uint8_t *mac)
{
    static const char ssid[] = "nat-os-transmitting";
    uint32_t i = 0u;

    g_tx_frame[i++] = 0x80u; g_tx_frame[i++] = 0x00u;   /* beacon */
    g_tx_frame[i++] = 0x00u; g_tx_frame[i++] = 0x00u;   /* duration */
    for (uint32_t k = 0u; k < 6u; k++) { g_tx_frame[i++] = 0xFFu; }   /* DA */
    for (uint32_t k = 0u; k < 6u; k++) { g_tx_frame[i++] = mac[k]; }  /* SA */
    for (uint32_t k = 0u; k < 6u; k++) { g_tx_frame[i++] = mac[k]; }  /* BSSID */
    g_tx_frame[i++] = 0x00u; g_tx_frame[i++] = 0x00u;   /* seq: driver fills */

    for (uint32_t k = 0u; k < 8u; k++) { g_tx_frame[i++] = 0x00u; }   /* tsf */
    g_tx_frame[i++] = 0x64u; g_tx_frame[i++] = 0x00u;   /* 100 TU */
    g_tx_frame[i++] = 0x01u; g_tx_frame[i++] = 0x04u;   /* ESS, short preamble */

    g_tx_frame[i++] = 0x00u;                            /* tag 0: SSID */
    uint32_t n = (uint32_t)(sizeof ssid - 1u);
    g_tx_frame[i++] = (uint8_t)n;
    for (uint32_t k = 0u; k < n; k++) { g_tx_frame[i++] = (uint8_t)ssid[k]; }

    g_tx_frame[i++] = 0x01u; g_tx_frame[i++] = 0x08u;   /* tag 1: rates */
    g_tx_frame[i++] = 0x82u; g_tx_frame[i++] = 0x84u;
    g_tx_frame[i++] = 0x8Bu; g_tx_frame[i++] = 0x96u;
    g_tx_frame[i++] = 0x0Cu; g_tx_frame[i++] = 0x12u;
    g_tx_frame[i++] = 0x18u; g_tx_frame[i++] = 0x24u;

    g_tx_frame[i++] = 0x03u; g_tx_frame[i++] = 0x01u;   /* tag 3: DS param */
    g_tx_frame[i++] = (uint8_t)TX_CHANNEL;

    return i;
}

void wifi_tx_beacons(uint32_t tx_fn, uint32_t chan_fn);
void wifi_tx_beacons(uint32_t tx_fn, uint32_t chan_fn)
{
    uint8_t mac[6];

    if (!tx_fn) {
        uart_puts("   tx        : blob entry has no esp_wifi_80211_tx\n");
        return;
    }
    if (osi_impl_read_mac(mac, 0u) != 0) {
        uart_puts("   tx        : no MAC\n");
        return;
    }
    if (chan_fn) {
        (void)blob_call(chan_fn, TX_CHANNEL, 0u, 0u, 0u);
    }

    wifi_sem_dump("before tx");
    wifi_event_report();
    uint32_t len = tx_build_beacon(mac);
    uart_puts("   tx        beacon ");
    uart_put_dec(len);
    uart_puts(" B, ch ");
    uart_put_dec(TX_CHANNEL);
    uart_puts(", ssid [nat-os-transmitting] x");
    uart_put_dec(TX_BEACONS);
    uart_puts("\n   tx        LOOK FOR IT ON A PHONE. rc is not evidence.\n");

    uint32_t t_start = timer_ticks();
    uint32_t ok = 0u, first_bad = 0u, bad = 0u;
    for (uint32_t k = 0u; k < TX_BEACONS; k++) {
        /* [step 209] Progress, with a TICK COUNT. 900 beacons at an assumed
         * 100 ms each should have taken 90 seconds and had not finished after
         * 170. "assumed 100 ms" was the error -- task_sleep(10) is ten TICKS,
         * and the loop also contends with the driver task for the blob mutex.
         * So the loop reports its own rate instead of being predicted. */
        if ((k % 50u) == 0u) {
            uart_puts("   tx        ");
            uart_put_dec(k);
            uart_puts(" @tick ");
            uart_put_dec(timer_ticks() - t_start);
            uart_puts("\n");
        }
        uint32_t rc = blob_call(tx_fn, 0u, (uint32_t)g_tx_frame, len, 1u);
        if (rc == 0u) {
            ok++;
        } else {
            if (!bad) { first_bad = rc; }
            bad++;
        }
        /* [step 209] ONE tick, not ten. Measured: the loop ran at 22 ticks per
         * beacon with task_sleep(10), because the sleep is in TICKS and the
         * blob_call costs about twelve more on its own. A real AP beacons
         * every 102 ms; at 22 ticks this was sending less than half as often
         * as that, which is a thinner target for a phone's scan than it needs
         * to be. Dropping the sleep leaves the call overhead as the pace. */
        task_sleep(1u);
    }

    uart_puts("   tx        accepted ");
    uart_put_dec(ok);
    uart_puts("  refused ");
    uart_put_dec(bad);
    if (bad) {
        uart_puts("  first rc ");
        uart_put_hex(first_bad);
    }
    uart_puts("\n");
}

/* [step 209] The semaphore pool, printed. esp_wifi_80211_tx hangs in
 * _mutex_lock(g_wifi_global_lock) -- read out of the blob, offset 84 in
 * wifi_osi_funcs_t with g_wifi_global_lock as the argument -- and it does so
 * whether or not a scan has run first. That rules out the sweep leaking it and
 * leaves the question of who holds it, which the pool can answer directly.
 * count 0 with a nonzero waiters mask is a mutex somebody is sitting on. */
void wifi_sem_dump(const char *when);
void wifi_sem_dump(const char *when)
{
    uart_puts("   [sem] ");
    uart_puts(when);
    for (uint32_t i = 0u; i < OSI_SEM_MAX; i++) {
        if (!g_sem[i].used) { continue; }
        uart_puts("  #");
        uart_put_dec(i);
        uart_puts("=");
        uart_put_dec(g_sem[i].count);
        uart_puts("/");
        uart_put_dec(g_sem[i].max);
        /* [step 211] Recursive means it is a MUTEX. Without this the dump
         * cannot tell a mutex somebody is sitting on from a signalling
         * semaphore resting at zero, which is its correct state. */
        uart_puts(g_sem[i].recursive ? "R" : "s");
        /* [step 211] "held" ONLY for a recursive mutex. A signalling semaphore
         * resting at zero is not held by anyone, and labelling it that way is
         * what made step 209 report a leaked g_wifi_global_lock that does not
         * exist -- see UM-NATOS-048 rev 1.1. A diagnostic that overstates its
         * own certainty costs more than no diagnostic. */
        if (g_sem[i].recursive && g_sem[i].depth && g_sem_owner[i]) {
            uart_puts(" heldByTask");
            uart_put_dec(g_sem_owner[i] - 1u);
            uart_puts(" d");
            uart_put_dec(g_sem[i].depth);
        }
        if (g_sem[i].waiters) {
            uart_puts(" w");
            uart_put_hex(g_sem[i].waiters);
        }
    }
    /* [step 211] Anything still on the acquisition stack leaked. The address is
     * the blob's own call site, so it names the function that took it. */
    {
        extern uint32_t g_mtx_ra[8], g_mtx_h[8], g_mtx_sp, g_mtx_over;
        uart_puts("  held:");
        if (!g_mtx_sp) { uart_puts(" none"); }
        for (uint32_t i = 0u; i < g_mtx_sp && i < 8u; i++) {
            uart_puts(" h");
            uart_put_hex(g_mtx_h[i]);
            uart_puts("@");
            uart_put_hex((g_mtx_ra[i] & 0x3FFFFFFFu) | 0x40000000u);
        }
        if (g_mtx_over) { uart_puts(" +ovf"); }
    }
    uart_puts("\n");
}

/* [step 210] A monotonic microsecond clock.
 *
 * _esp_timer_get_time and _get_time both returned 0, so every "now" the driver
 * asked for was the same instant and every elapsed-time computation was zero.
 * That is the §10 shape again: a plausible value, wrong semantics, invisible to
 * any audit that looks for missing bodies.
 *
 * Ten-millisecond ticks are too coarse on their own -- a driver that polls a
 * deadline would see time frozen for a whole tick -- so CCOUNT provides the
 * sub-tick part. The (tick, ccount) pair is re-latched whenever the tick moves,
 * which makes it self-correcting: no drift accumulates and no wrap of the
 * 32-bit cycle counter can be missed, because the tick is the authority and
 * CCOUNT only ever interpolates INSIDE one tick.
 *
 * Monotonic across a tick boundary: sub is clamped below the tick length, so
 * t*10000+sub can never reach (t+1)*10000. */
#define OSI_US_PER_TICK   (OSI_TICK_CYCLES / (OSI_CPU_HZ / 1000000u))
#define OSI_CYCLES_PER_US (OSI_CPU_HZ / 1000000u)

int64_t osi_impl_time_us(void);
int64_t osi_impl_time_us(void)
{
    static uint32_t s_tick, s_cc;
    static int      s_have;
    uint32_t cc;
    __asm__ volatile ("rsr.ccount %0" : "=r"(cc));

    uint32_t crit = crit_enter();
    uint32_t t = timer_ticks();
    if (!s_have || t != s_tick) {
        s_tick = t;
        s_cc = cc;
        s_have = 1;
    }
    uint32_t sub = (cc - s_cc) / OSI_CYCLES_PER_US;
    crit_exit(crit);

    if (sub >= OSI_US_PER_TICK) { sub = OSI_US_PER_TICK - 1u; }
    return (int64_t)((uint64_t)t * OSI_US_PER_TICK + sub);
}

/* [step 211] _event_post, implemented.
 *
 * It returned 0 -- ESP_OK -- and delivered nothing, so the driver believed
 * every event it raised had been handled. That is harmless to the driver's own
 * state machine, which does not read its events back, and it is why scanning
 * and transmitting work without it. What it costs is US: SCAN_DONE, STA_START,
 * STA_CONNECTED and above all STA_DISCONNECTED-with-a-reason-code are the only
 * narration the driver offers, and association cannot be debugged blind.
 *
 * nat-os has no esp_event loop and does not need one. Events are recorded and
 * reported; nothing subscribes, because nothing in this kernel wants a
 * callback. The value is the record.
 *
 * event_base arrives as NULL, and that is not a defect here. WIFI_EVENT is a
 * `const char *` defined by the open-source esp_event component, which is not
 * in the blob -- the same absence that left esp_supplicant_init uncallable at
 * step 205. The blob references a symbol nothing defines and passes zero. The
 * ID is the information anyway, and it is unambiguous: measured, one id=2
 * (STA_START) after esp_wifi_start, then exactly one id=1 (SCAN_DONE) per scan,
 * 39 scans and 39 events. The pointer is still printed defensively and range-
 * checked before being followed, in case a build ever does resolve it. */
#define OSI_EVT_LOG 16u

/* [step 217] A COPY of the payload, taken at post time. The pointer alone is
 * useless -- the driver owns that memory and is free to release it the moment
 * the post returns, so reading it later would be reading whatever came next. */
static struct { uint32_t base, id, data; uint8_t d[48]; } g_evt_log[OSI_EVT_LOG];
static uint32_t g_evt_n;

int32_t osi_impl_event_post(uint32_t base, uint32_t id, uint32_t data);
int32_t osi_impl_event_post(uint32_t base, uint32_t id, uint32_t data)
{
    uint32_t crit = crit_enter();
    if (g_evt_n < OSI_EVT_LOG) {
        g_evt_log[g_evt_n].base = base;
        g_evt_log[g_evt_n].id   = id;
        g_evt_log[g_evt_n].data = data;
        for (uint32_t k = 0u; k < 48u; k++) { g_evt_log[g_evt_n].d[k] = 0u; }
        if (data >= 0x3F400000u && data < 0x40000000u) {
            const uint8_t *s = (const uint8_t *)data;
            for (uint32_t k = 0u; k < 48u; k++) { g_evt_log[g_evt_n].d[k] = s[k]; }
        }
    }
    g_evt_n++;
    crit_exit(crit);
    return 0;                       /* ESP_OK: it really has been handled now */
}

void wifi_event_report(void);
void wifi_event_report(void)
{
    uart_puts("   [evt] posted ");
    uart_put_dec(g_evt_n);
    uint32_t n = g_evt_n < OSI_EVT_LOG ? g_evt_n : OSI_EVT_LOG;
    for (uint32_t i = 0u; i < n; i++) {
        uart_puts("  ");
        const char *b = (const char *)g_evt_log[i].base;
        /* Only follow the pointer if it lands in mapped rodata or DRAM. */
        if (g_evt_log[i].base >= 0x3F400000u && g_evt_log[i].base < 0x40000000u) {
            for (uint32_t k = 0u; k < 14u && b[k]; k++) {
                uart_putc((b[k] >= 32 && b[k] < 127) ? b[k] : '?');
            }
        } else {
            /* Print the pointer, not a shrug. "?base" said the check failed
             * and not what it rejected, which is useless for fixing it. */
            uart_puts("base=");
            uart_put_hex(g_evt_log[i].base);
        }
        uart_puts(":");
        uart_put_dec(g_evt_log[i].id);
        /* [step 217] id 5 is WIFI_EVENT_STA_DISCONNECTED, whose payload is
         * ssid[32], ssid_len, bssid[6], reason -- so reason sits at +39.
         * CHECKED rather than assumed, the same way section 7.1 checked the
         * scan record: ssid_len at +32 must equal the length of the SSID we
         * asked for. If it does not, the offset is not trusted and no reason
         * is printed. */
        /* [step 221] id 4 is WIFI_EVENT_STA_CONNECTED. Its payload is
         * ssid[32], ssid_len, bssid[6], channel, authmode, aid -- so channel
         * sits at +39 and authmode at +40. Checked the same way everything
         * else has been: ssid_len at +32 must equal the SSID we asked to
         * join, and the channel must be one a scan actually found it on. */
        if (g_evt_log[i].id == 4u && g_evt_log[i].data) {
            uint32_t n = g_evt_log[i].d[32];
            uart_puts("(len");
            uart_put_dec(n);
            if (n == g_assoc_ssid_len) {
                uart_puts(" ch");
                uart_put_dec(g_evt_log[i].d[39]);
                uart_puts(" auth");
                uart_put_dec(g_evt_log[i].d[40]);
                uart_puts(" CONNECTED");
            } else {
                uart_puts(" layout?");
            }
            uart_puts(")");
        }
        if (g_evt_log[i].id == 5u && g_evt_log[i].data) {
            uint32_t n = g_evt_log[i].d[32];
            uart_puts("(len");
            uart_put_dec(n);
            /* [step 219] Compare against the SSID we actually asked for, not a
             * literal. It was hardcoded to 22 -- the length of step 217's
             * impossible SSID -- so a real network of a different length read
             * as "layout?" when the layout was in fact fine. A self-check that
             * only passes for one input is not a self-check. */
            if (n == g_assoc_ssid_len) {
                uart_puts(" reason");
                uint32_t rr = g_evt_log[i].d[39];
                uart_put_dec(rr);
                /* [step 219] Named, because a bare number sends the next
                 * reader to a header and the distinction between 201 and 203
                 * is the whole result. wifi_err_reason_t. */
                /* Codes under 200 are the 802.11 standard reasons sent by
                 * the ACCESS POINT; 200+ are Espressif's own. The distinction
                 * matters: reason 2 means the AP dropped us, not that the
                 * driver failed. */
                /* [step 237] The handshake range. 15 and 39 are what a
                 * station that ASSOCIATES and then cannot answer EAPOL looks
                 * like -- the distinction from 203 is the whole result. */
                /* [step 246] ELIMINATED, and the label above it was WRONG.
                 * 39 is WIFI_REASON_TIMEOUT; the code for "associated, then
                 * EAPOL never completed" is 204 HANDSHAKE_TIMEOUT (or 15).
                 * Measured: wpa_sta_connected_cb (slot 3) is NEVER called and
                 * no id-4 STA_CONNECTED event is ever posted. Two independent
                 * paths agree the station does not associate at all, so the
                 * missing EAPOL of steps 241-245 needs no explanation beyond
                 * this: the access point never had an associated station to
                 * send message one to. */
                uart_puts(rr == 15u  ? " 4WAY_HANDSHAKE_TIMEOUT"
                        : rr == 16u  ? " GROUP_KEY_UPDATE_TIMEOUT"
                        : rr == 17u  ? " IE_IN_4WAY_DIFFERS"
                        : rr == 39u  ? " TIMEOUT(NOT associated -- see step 246)"
                        : rr == 2u   ? " AUTH_EXPIRE(AP dropped us)"
                        : rr == 3u   ? " AUTH_LEAVE"
                        : rr == 4u   ? " ASSOC_EXPIRE"
                        : rr == 8u   ? " ASSOC_LEAVE"
                        : rr == 200u ? " BEACON_TIMEOUT"
                        : rr == 201u ? " NO_AP_FOUND"
                        : rr == 202u ? " AUTH_FAIL"
                        : rr == 203u ? " ASSOC_FAIL"
                        : rr == 204u ? " HANDSHAKE_TIMEOUT"
                        : rr == 205u ? " CONNECTION_FAIL"
                        : rr == 15u  ? " 4WAY_TIMEOUT"
                        : "");
            } else {
                uart_puts(" layout?");
            }
            uart_puts(")");
        }
    }
    uart_puts("\n");
}

/* ---- ASSOCIATION -- next_moves/08 step 217 ------------------------------
 *
 * The first attempt to join a network. It is deliberately aimed at an SSID
 * that DOES NOT EXIST, and that is the point: no credentials are needed, and
 * the driver still runs the whole path -- config, connect, scan for the
 * target, fail to find it -- and then says what happened through the event
 * stream that step 211 wired up. A reason code from a failed association is
 * worth more than a guess about a successful one.
 *
 * wifi_config_t is a union whose sta member begins ssid[32] then password[64].
 * Those two offsets have been stable across every IDF version; everything
 * after them has not, so the buffer is ZEROED and nothing else is set. Zero is
 * a sane default for all of it -- scan_method FAST, bssid_set 0, channel 0
 * (any), threshold rssi 0, authmode OPEN -- which is exactly the configuration
 * this test wants anyway. The same discipline as step 206: use the offsets
 * that are in evidence and leave the rest alone.
 *
 * This DOES transmit. A station looking for an SSID sends probe requests, and
 * if it found the network it would authenticate. Transmit was established at
 * step 209 and this is the same radio doing the same thing. */
/* [step 278] Join a NAMED network, for the wifi view.
 *
 * wifi_try_connect() joins the network compiled into wifi_secrets.h and takes
 * its name from there. This takes the name from the caller and keeps the
 * passphrase, which is the scope the view was built to: the stored password,
 * against whichever network you point it at.
 *
 * BLOCKING, and it is the PMK that blocks. PBKDF2 at 4096 iterations measures
 * ~15 s on this part, and the key is derived from the passphrase AND the SSID
 * together -- so a different network is a different key and there is no way to
 * skip it. Step 243 moved this off the driver's connect callback because
 * fifteen seconds there kills the association; it must stay off it here too,
 * which is why the derivation happens before esp_wifi_connect and not during.
 */
static char g_join_ssid[33];

/* [step 285] The passphrase the user typed, if they typed one. g_hs_pass is a
 * const char * held by the supplicant across the whole handshake, so it must
 * point at storage that outlives the caller -- a stack buffer here would be
 * read long after it went away. */
static char g_join_pass[64];
int g_used_cached;      /* [step 318] did this join use a cached PMK? */
static int  g_join_pass_set;

void wifi_join_ssid_pass(const char *ssid, const char *pass);
void wifi_join_ssid_pass(const char *ssid, const char *pass)
{
    g_join_pass_set = 0;
    if (pass && pass[0]) {
        uint32_t i = 0u;
        for (; i < 62u && pass[i]; i++) { g_join_pass[i] = pass[i]; }
        g_join_pass[i]  = 0;
        g_join_pass_set = 1;
    }
    wifi_join_ssid(ssid);
}

void wifi_join_ssid(const char *ssid);
void wifi_join_ssid(const char *ssid)
{
    const struct blob_entry *e = blob_map();
    if (!e || !blob_ready() || !ssid || !ssid[0]) { return; }

    uint32_t n = 0u;
    for (; n < 32u && ssid[n]; n++) { g_join_ssid[n] = ssid[n]; }
    g_join_ssid[n] = 0;

    {   /* Re-derive: a new SSID means a new key, and g_hs_pmk_ready guards
         * against doing it twice for the same one. */
        extern const char *g_hs_ssid;
        extern uint32_t g_hs_pmk_ready;
        extern int wpa_hs_derive_pmk(void);
        extern const char *g_hs_pass;
        extern unsigned char g_hs_pmk[32];
        extern uint32_t g_hs_pmk_ready;
        extern int  pmkcache_get(const char *ssid, unsigned char *out);
        extern int  pmkcache_put(const char *ssid, const unsigned char *pmk);
        g_hs_ssid = g_join_ssid;
        /* A typed passphrase wins over the compiled-in one. When none has been
         * typed the old behaviour stands, so the board still joins the network
         * it was built for with no credential saved. */
        if (g_join_pass_set) { g_hs_pass = g_join_pass; }
        g_hs_pmk_ready = 0u;    /* [step 292] a write, not a call -- see wifi_glue.c */

        /* [step 315] Fifteen seconds of PBKDF2, or none.
         *
         * The PMK is a pure function of the SSID and the passphrase, so a
         * cached one is not stale data -- it is the same arithmetic, already
         * done. Installing it is two memory operations and no call, for the
         * reason step 292 cost a crash to learn.
         *
         * A wrong cached key cannot do harm quietly: the four-way handshake
         * fails its MIC and the view reports a failed join, which is what a
         * wrong passphrase does anyway. The cache is dropped on a failure so
         * the next attempt derives. */
        extern int g_used_cached;
        if (pmkcache_get(g_join_ssid, g_hs_pmk)) {
            g_hs_pmk_ready = 1u;
            g_used_cached  = 1;
        } else {
            g_used_cached  = 0;
            (void)blob_call((uint32_t)&wpa_hs_derive_pmk, 0u, 0u, 0u, 0u);
            if (g_hs_pmk_ready) { (void)pmkcache_put(g_join_ssid, g_hs_pmk); }
        }
    }

    /* The station config: SSID at +0 and password at +32, the two fields step
     * 218 established are stable across every wifi_config_t this blob has
     * seen. Everything else stays zero. */
    {
        static uint8_t conf[256];
        extern const char *wifi_sta_pass(void);
        const char *pw = g_join_pass_set ? g_join_pass : wifi_sta_pass();
        for (uint32_t i = 0u; i < sizeof conf; i++) { conf[i] = 0u; }
        for (uint32_t i = 0u; i < n; i++) { conf[i] = (uint8_t)g_join_ssid[i]; }
        if (pw) {
            for (uint32_t i = 0u; i < 63u && pw[i]; i++) {
                conf[32u + i] = (uint8_t)pw[i];
            }
        }
        g_assoc_ssid_len = n;
        if (blob_call(e->wifi_set_config, 0u, (uint32_t)conf, 0u, 0u) != 0u) {
            return;
        }
    }

    (void)blob_call(e->wifi_connect, 0u, 0u, 0u, 0u);
}

/* Non-zero once the driver has told the supplicant the station is up. This is
 * wpa_sta_connected_cb, which step 246 named and measured at ZERO for thirteen
 * steps -- it is the one signal that means associated AND keyed, rather than
 * merely not yet failed. */
/* [step 319] Leave the network, so a scan can run without fighting it.
 *
 * An associated station is parked on its access point's channel. Sweeping the
 * other twelve from that state has been measured doing three different bad
 * things in this project: the driver refuses channels (288), the link drops
 * silently (293), and -- with the blob task live and being switched around a
 * retune it did not ask for -- it reaches the register-window fault of steps
 * 49-141 and takes the kernel with it (319).
 *
 * Disconnecting first is what a phone does, and it turns "scan while joined"
 * from a provocation into an ordinary sequence: leave, look, choose, rejoin. */
void wifi_leave(void);
void wifi_leave(void)
{
    const struct blob_entry *e = blob_map();
    if (!e || !blob_ready() || !e->wifi_disconnect) { return; }
    (void)blob_call(e->wifi_disconnect, 0u, 0u, 0u, 0u);

    /* The supplicant's connected flag is what wifi_joined() reads, and nothing
     * else clears it. A disconnect this code asked for is not a failure to be
     * reported later, so the flag goes with the association. */
    {
        extern uint32_t g_wpa_conn_cb;
        g_wpa_conn_cb = 0u;
    }
}

int wifi_joined(void);
int wifi_joined(void)
{
    extern uint32_t g_wpa_conn_cb;
    return g_wpa_conn_cb != 0u;
}

void wifi_try_connect(uint32_t cfg_fn, uint32_t conn_fn);
void wifi_try_connect(uint32_t cfg_fn, uint32_t conn_fn)
{
    static uint8_t conf[256];
    /* [step 218] Real credentials if they exist, the impossible SSID if not.
     * kernel/wifi_secrets.h is gitignored; wifi_secrets.h.example documents the
     * shape. Without it the tree still builds and still exercises the whole
     * association path -- the driver answers NO_AP_FOUND, which is what step
     * 217 measured -- so nothing here depends on a secret being present. */
#if defined(__has_include)
#  if __has_include("wifi_secrets.h")
#    include "wifi_secrets.h"
#  endif
#endif
#ifndef WIFI_STA_SSID
#  define WIFI_STA_SSID "nat-os-no-such-network"
#endif
    static const char ssid[] = WIFI_STA_SSID;
#ifdef WIFI_STA_PASS
    static const char pass[] = WIFI_STA_PASS;
#endif

    if (!cfg_fn || !conn_fn) {
        uart_puts("   assoc     : blob entry lacks set_config/connect\n");
        return;
    }
    for (uint32_t i = 0u; i < sizeof conf; i++) { conf[i] = 0u; }
    for (uint32_t i = 0u; i < sizeof ssid - 1u; i++) { conf[i] = (uint8_t)ssid[i]; }
#ifdef WIFI_STA_PASS
    /* Password at +32, the second stable field of wifi_config_t's sta member.
     * Capped at 63 so a long passphrase cannot run into whatever follows. */
    for (uint32_t i = 0u; i < sizeof pass - 1u && i < 63u; i++) {
        conf[32u + i] = (uint8_t)pass[i];
    }
#endif

    g_assoc_ssid_len = (uint32_t)(sizeof ssid - 1u);
    uint32_t cr = blob_call(cfg_fn, 0u /* WIFI_IF_STA */, (uint32_t)conf, 0u, 0u);
    uart_puts("   assoc     set_config rc ");
    uart_put_hex(cr);
    uart_puts("\n");
    if (cr != 0u) { return; }

    uint32_t nr = blob_call(conn_fn, 0u, 0u, 0u, 0u);
    uart_puts("   assoc     connect rc ");
    uart_put_hex(nr);
    uart_puts("  ssid [");
    uart_puts(ssid);
    /* The SSID is broadcast in the clear by the access point itself, so
     * printing it discloses nothing that an antenna would not. The PASSWORD is
     * never printed -- only its length, which shows the field was populated
     * without saying what is in it. A console log is a file, and files get
     * pasted into reports. */
    uart_puts("] pass ");
#ifdef WIFI_STA_PASS
    uart_put_dec((uint32_t)(sizeof pass - 1u));
    uart_puts(" chars\n");
#else
    uart_puts("none\n");
#endif

    /* Give the driver time to scan for a network that is not there and give
     * up. The answer arrives as an event, not as a return code. */
    /* [step 218] Thirty seconds, not six. A WPA2 association is scan, then
     * auth, then assoc, then a four-way handshake -- and the handshake is the
     * SUPPLICANT's work, which this project has only recording stubs for. If it
     * stalls there the driver times out rather than failing fast, so a short
     * wait sees nothing at all and proves nothing at all. */
    /* [step 223] Ten, was thirty. Measured: the connect result arrives within
     * a few seconds -- both CONNECTED and the AP's deauth -- so twenty of
     * those seconds were spent waiting for an event that had already been
     * posted, and they pushed the RX dwell past the capture window. */
    for (uint32_t k = 0u; k < 10u; k++) {
        task_sleep(100u);
    }
    wifi_event_report();
    /* Which supplicant callbacks the driver reached. If the handshake was
     * attempted, the entries it needed are named here rather than guessed. */
    wpa_cb_report();
    uart_puts("\n");
}

/* [step 222] Register the RX data path and report what arrives.
 *
 * Called after the association succeeds, because a station that is not
 * associated receives no data frames and registering earlier would prove
 * nothing either way. */
void wifi_dhcp_discover(uint32_t tx_fn);
uint32_t g_internal_tx_fn;
void wifi_rx_report(void);
void wifi_rx_start(uint32_t reg_fn, uint32_t free_fn, uint32_t promisc_fn,
                   uint32_t tx_fn);
void wifi_rx_start(uint32_t reg_fn, uint32_t free_fn, uint32_t promisc_fn,
                   uint32_t tx_fn)
{
    /* [step 223] PROMISCUOUS OFF FIRST.
     *
     * Step 197 turned it on to get the MAC raising interrupts at all, back
     * when the receiver was deaf, and nothing has turned it off since. But
     * promiscuous mode routes received frames to the PROMISCUOUS callback --
     * which nat-os has never registered -- and not to the station data path
     * that esp_wifi_internal_reg_rxcb feeds. A station that is associated and
     * listening on the right channel and still sees zero data frames is the
     * symptom that suggests it. */
    if (promisc_fn) {
        uint32_t pr = blob_call(promisc_fn, 0u, 0u, 0u, 0u);
        uart_puts("   rx        promiscuous off rc ");
        uart_put_hex(pr);
        uart_puts("\n");
    }
    extern int nat_rx_cb(void *buffer, unsigned short len, void *eb);
    extern uint32_t g_rx_free_fn;

    if (!reg_fn) {
        uart_puts("   rx        : blob entry has no reg_rxcb\n");
        return;
    }
    g_rx_free_fn = free_fn;
    g_internal_tx_fn = tx_fn;
    uint32_t rc = blob_call(reg_fn, 0u /* WIFI_IF_STA */, (uint32_t)&nat_rx_cb,
                            0u, 0u);
    uart_puts("   rx        reg_rxcb rc ");
    uart_put_hex(rc);
    uart_puts(free_fn ? "  (free wired)\n" : "  (NO FREE -- will leak)\n");
    if (rc != 0u) { return; }

    /* [step 222] DWELL ON THE AP'S CHANNEL. The first attempt registered the
     * callback and then immediately ran the thirteen-channel sweep, which
     * takes the radio off the access point for sixteen seconds -- so it
     * reported "frames 0" and that number meant nothing at all. A receiver
     * that is somewhere else is not evidence of a quiet network.
     *
     * Fifteen seconds of sitting still. An idle network still carries
     * broadcast traffic -- ARP, DHCP, mDNS -- and broadcasts are delivered to
     * every associated station. */
    /* [step 224] Provoke a reply before listening. Silence on its own could
     * not distinguish a quiet hotspot from a broken receive path. */
    wifi_dhcp_discover(g_internal_tx_fn);
    /* [step 226] Hand net.c the transmit entry and our MAC, then poll. The
     * poll both drives DHCP to completion and answers ARP and ICMP, so the
     * window has to be long enough for somebody to actually type `ping`. */
    {
        extern void net_set_tx(uint32_t tx_fn, const unsigned char *mac);
        extern void net_poll_for(uint32_t ticks);
        extern void net_report(void);
        uint8_t m[6];
        if (osi_impl_read_mac(m, 0u) == 0) { net_set_tx(tx_fn, m); }
        uart_puts("   net       polling 60 s, then the net task takes over\n");
        /* [step 260] 600 s, was 120. Step 227 sized this so a human could
         * type ping. The browser test races the operator in a way ping does
         * not: the address cannot be handed over until DHCP has bound, and
         * DHCP binds INSIDE this window. At 120 s the first attempt was simply
         * too late, and "the page did not load" then means nothing -- the
         * board recorded no ARP for its own address, so no browser had tried.
         * Ten minutes removes timing as a variable. */
        /* [step 271] 60 s, was 600. The long window existed because the poll
         * WAS the network: when it ended the stack went silent. The net task
         * now services lwIP for as long as the board is up, so this is only
         * the bring-up window -- long enough to reach a DHCP lease and report
         * it, and no longer a race against the operator. */
        /* [step 304] 150 ticks -- 1.5 s -- was 6000, sixty seconds.
         *
         * Step 271 gave the stack a task of its own, and step 271's own comment
         * said what that made this: "the net task now services lwIP for as long
         * as the board is up, so this is only the bring-up window". It was
         * still sixty seconds of blocking to do what the net task does anyway,
         * and it was two thirds of the ninety-second wait the user sits through
         * after tapping start.
         *
         * What the window still has to do is reach the handover at the end of
         * net_poll_for, which hands the ring to the task. That takes one pass,
         * not six thousand ticks. DHCP finishes afterwards, serviced by the
         * task, and the view watches for the address rather than the bring-up
         * waiting for it (283). */
        net_poll_for(150u);
        wifi_rx_report();
        net_report();
    }
}

void wifi_rx_report(void);
void wifi_rx_report(void)
{
    extern uint32_t g_rx_frames, g_rx_bytes, g_rx_len[6];
    extern uint8_t g_rx_snap[6][80];
    static const char hx[] = "0123456789abcdef";

    uart_puts("   rx        frames ");
    uart_put_dec(g_rx_frames);
    uart_puts(" bytes ");
    uart_put_dec(g_rx_bytes);
    uart_puts("\n");

    /* [step 267] The per-frame dump is removed. Step 222 needed it to prove
     * a DHCP OFFER had arrived and been decoded; that is long established,
     * the sniffer of step 250 reads the same traffic better, and its iram
     * bought the blob critical-section depth in the breadcrumb. Counts
     * stay. */
}

/* ---- DHCP DISCOVER -- next_moves/08 step 224 ----------------------------
 *
 * The first IP packet nat-os has ever built, and it exists to answer a
 * question that watching could not: fifteen seconds associated and on-channel
 * produced ZERO data frames, and that is consistent with two very different
 * worlds -- a hotspot with no other clients and therefore no broadcast traffic,
 * or an RX path that is not actually wired. Silence cannot tell them apart.
 *
 * A DHCP DISCOVER can. It is a broadcast the server is obliged to answer, so a
 * reply proves the transmit path, the receive path AND hands over the subnet
 * in one exchange. No reply, with the transmit reporting success, points at
 * the receive side.
 *
 * Built by hand because there is no stack: Ethernet II, IPv4, UDP, then the
 * BOOTP/DHCP body. The IPv4 header checksum is computed; the UDP checksum is
 * left zero, which IPv4 explicitly permits and every DHCP server accepts.
 */

static uint8_t  g_dhcp[300];
static uint32_t g_dhcp_len;

static void be16(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }

static uint32_t ip_checksum(const uint8_t *p, uint32_t len)
{
    uint32_t sum = 0u;
    for (uint32_t i = 0u; i + 1u < len; i += 2u) {
        sum += ((uint32_t)p[i] << 8) | p[i + 1u];
    }
    while (sum >> 16) { sum = (sum & 0xFFFFu) + (sum >> 16); }
    return (~sum) & 0xFFFFu;
}

static uint32_t dhcp_build(const uint8_t *mac, uint32_t xid)
{
    for (uint32_t i = 0u; i < sizeof g_dhcp; i++) { g_dhcp[i] = 0u; }
    uint8_t *e = g_dhcp;

    /* Ethernet II: broadcast, from us, IPv4. */
    for (uint32_t i = 0u; i < 6u; i++) { e[i] = 0xFFu; }
    for (uint32_t i = 0u; i < 6u; i++) { e[6u + i] = mac[i]; }
    be16(&e[12], 0x0800u);

    uint8_t *ip = e + 14;
    uint8_t *ud = ip + 20;
    uint8_t *bp = ud + 8;

    /* BOOTP/DHCP. op=BOOTREQUEST, htype=ethernet, hlen=6. */
    bp[0] = 1u; bp[1] = 1u; bp[2] = 6u; bp[3] = 0u;
    bp[4] = (uint8_t)(xid >> 24); bp[5] = (uint8_t)(xid >> 16);
    bp[6] = (uint8_t)(xid >> 8);  bp[7] = (uint8_t)xid;
    be16(&bp[10], 0x8000u);              /* BROADCAST: we have no address yet,
                                          * so a unicast reply could not reach
                                          * us -- this is not optional here. */
    for (uint32_t i = 0u; i < 6u; i++) { bp[28u + i] = mac[i]; }
    bp[236] = 99u; bp[237] = 130u; bp[238] = 83u; bp[239] = 99u;  /* magic */
    bp[240] = 53u; bp[241] = 1u; bp[242] = 1u;      /* option 53: DISCOVER */
    bp[243] = 55u; bp[244] = 3u;                    /* option 55: ask for   */
    bp[245] = 1u; bp[246] = 3u; bp[247] = 6u;       /* mask, router, DNS    */
    bp[248] = 255u;                                 /* end                  */
    uint32_t bootp_len = 249u;

    uint32_t udp_len = 8u + bootp_len;
    be16(&ud[0], 68u);                   /* client port */
    be16(&ud[2], 67u);                   /* server port */
    be16(&ud[4], udp_len);
    be16(&ud[6], 0u);                    /* checksum optional over IPv4 */

    uint32_t ip_len = 20u + udp_len;
    ip[0] = 0x45u; ip[1] = 0u;
    be16(&ip[2], ip_len);
    be16(&ip[4], 0u); be16(&ip[6], 0u);
    ip[8] = 64u; ip[9] = 17u;            /* TTL 64, UDP */
    be16(&ip[10], 0u);
    for (uint32_t i = 0u; i < 4u; i++) { ip[12u + i] = 0u; }       /* 0.0.0.0 */
    for (uint32_t i = 0u; i < 4u; i++) { ip[16u + i] = 0xFFu; }    /* 255.x   */
    be16(&ip[10], ip_checksum(ip, 20u));

    return 14u + ip_len;
}

void wifi_dhcp_discover(uint32_t tx_fn);
void wifi_dhcp_discover(uint32_t tx_fn)
{
    uint8_t mac[6];
    if (!tx_fn) { uart_puts("   dhcp      : no internal_tx entry\n"); return; }
    if (osi_impl_read_mac(mac, 0u) != 0) { uart_puts("   dhcp      : no MAC\n"); return; }

    g_dhcp_len = dhcp_build(mac, 0x6E61744Fu /* 'natO' -- recognisable in a capture */);

    uart_puts("   dhcp      DISCOVER ");
    uart_put_dec(g_dhcp_len);
    uart_puts(" B  ");
    uint32_t rc = blob_call(tx_fn, 0u /* WIFI_IF_STA */, (uint32_t)g_dhcp,
                            g_dhcp_len, 0u);
    uart_puts("tx rc ");
    uart_put_hex(rc);
    uart_puts(rc == 0u ? "  sent\n" : "  REFUSED\n");
}
