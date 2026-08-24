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

void osi_impl_ints_off(uint32_t mask)
{
    for (uint32_t line = 0u; line < 32u; line++) {
        if (mask & (1u << line)) {
            xt_disable_interrupt(blob_line_map(line));
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
