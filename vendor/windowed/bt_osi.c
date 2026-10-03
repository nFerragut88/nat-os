/* nat-os — the host services Espressif's Bluetooth controller asks for.
 *
 * Compiled -mabi=windowed, because the blob calls these directly. Everything
 * real is forwarded into the call0 kernel through kernel/window.S, exactly as
 * vendor/windowed/wifi_osi_stubs.c does for WiFi; vendor/phy/README.md records
 * what calling across that boundary by accident looks like (an
 * IllegalInstruction in padding, from code that linked cleanly).
 *
 * ---- where this list came from ---------------------------------------------
 *
 * Not from reading documentation. `vendor/bt/patch_bt.py` takes the symbols the
 * four archives need, subtracts what they answer among themselves, subtracts
 * esp32.rom.ld (already linked, every entry PROVIDE), subtracts what it renames
 * to ROM addresses, and prints the remainder. That remainder is this file:
 *
 *     symbols needed       3608
 *     answered internally  3218
 *     from esp32.rom.ld     244
 *     renamed to the ROM     32
 *     LEFT -- this file     120
 *
 * Re-run it after any SDK change and the difference is the work, stated as a
 * measurement rather than a guess.
 *
 * ---- what is honest and what is a stub -------------------------------------
 *
 * REAL: queues, semaphores, mutexes, event groups, software timers, tasks, the
 * heap, interrupts, the microsecond clock, logging, the PHY. All of those exist
 * already -- the WiFi blob needed the same things and kernel/wifi_osi_impl.c is
 * compiled into every build.
 *
 * STUBBED, deliberately and visibly: NVS (the controller stores nothing a BLE
 * scanner needs), AES (nothing is encrypted until pairing exists), the VFS,
 * inter-processor calls (this kernel runs the controller on one core), BT mesh
 * provisioning, and the WiFi-coexistence hooks. Each returns the "nothing
 * there" answer its caller expects rather than zero-by-default, and each counts
 * its calls, so a stub that turns out to matter shows up as a number instead of
 * as a mystery.
 *
 * [next_moves/13 step 1]
 */

#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>

/* ---- the bridge into call0 --------------------------------------------------
 *
 * Macros only. No call0 header may be included here: one declaration pulled in
 * is one function the compiler will emit a windowed call to. */
extern uint32_t w2c_call0f(uint32_t fn);
extern uint32_t w2c_call1(uint32_t fn, uint32_t a);
extern uint32_t w2c_call2(uint32_t fn, uint32_t a, uint32_t b);
extern uint32_t w2c_call3(uint32_t fn, uint32_t a, uint32_t b, uint32_t c);

#define FWD0(f)         w2c_call0f((uint32_t)&f)
#define FWD1(f,a)       w2c_call1((uint32_t)&f,(uint32_t)(a))
#define FWD2(f,a,b)     w2c_call2((uint32_t)&f,(uint32_t)(a),(uint32_t)(b))
#define FWD3(f,a,b,c)   w2c_call3((uint32_t)&f,(uint32_t)(a),(uint32_t)(b),(uint32_t)(c))

/* ---- the shim trace --------------------------------------------------------
 *
 * TEMPORARY, and off unless BT_TRACE is defined at build time.
 *
 * esp_bt_controller_init() freezes the board completely -- no ticks, no task
 * switches, nothing but the hang detector thirty seconds later. A freeze that
 * total cannot be inspected after the fact: the reset erases it, and the
 * watchdog breadcrumb only updates on ticks, which have stopped.
 *
 * So the instrument has to print as it goes. The UART here is polled, not
 * interrupt-driven, so it still works with interrupts masked -- which is the
 * state this is meant to diagnose. Two outcomes are both informative: a
 * repeating line names the loop the controller is spinning in, and a sudden
 * silence names the last shim it entered and did not leave. */
#ifdef BT_TRACE
#define BT_TR(s) do { extern void bt_host_trace(uint32_t);                       FWD1(bt_host_trace, (uint32_t)(s)); } while (0)
#else
#define BT_TR(s) do { } while (0)
#endif

/* The call0 side. Declared, never called directly -- only their addresses are
 * taken, which is safe across the ABI. */
extern void *osi_impl_sem_create(uint32_t max, uint32_t init);
extern void osi_impl_sem_delete(void *h);
extern int32_t osi_impl_sem_take(void *h, uint32_t ticks);
extern int32_t osi_impl_sem_give(void *h);
extern void *osi_impl_recursive_mutex_create(void);
extern void *osi_impl_queue_create(uint32_t len, uint32_t item_size);
extern void osi_impl_queue_delete(void *h);
extern int32_t osi_impl_queue_send(void *h, void *item, uint32_t ticks, int to_front);
extern int32_t osi_impl_queue_send_isr(void *h, void *item);
extern int32_t osi_impl_queue_recv(void *h, void *item, uint32_t ticks);
extern uint32_t osi_impl_queue_waiting(void *h);
extern void *osi_impl_evt_create(void);
extern void osi_impl_evt_delete(void *h);
extern uint32_t osi_impl_evt_set(void *h, uint32_t bits);
extern uint32_t osi_impl_evt_clear(void *h, uint32_t bits);
extern uint32_t osi_impl_in_isr(void);
extern int32_t osi_impl_get_random(uint8_t *buf, uint32_t len);
extern int32_t osi_impl_read_mac(uint8_t *mac, uint32_t type);
extern void osi_impl_timer_setfn(void *p, void *fn, void *arg);
extern void osi_impl_timer_arm_us(void *p, uint32_t us, int periodic);
extern void osi_impl_timer_disarm(void *p);
extern void osi_impl_timer_done(void *p);

extern void *bt_host_malloc(uint32_t bytes);
extern void bt_host_free(void *p);
extern void bt_host_us(uint32_t *out);
extern uint32_t bt_host_intr_alloc(uint32_t source, uint32_t fn, uint32_t arg);
extern void bt_host_intr_free(uint32_t handle);
extern void bt_host_log(uint32_t str);
extern int bt_host_phy_enable(void);

/* Wider than the bridge carries. Three arguments is the widest w2c_ bridge, so
 * anything wider is packed into a frame on this stack and passed by pointer.
 * The call0 side reads it before returning, so the frame stays live. */
typedef struct {
    uint32_t a, b, c, d, e;
} bt_args_t;

extern uint32_t bt_host_evt_wait(bt_args_t *a);     /* group, bits, clear, all, ticks */
extern uint32_t bt_host_task_create(bt_args_t *a);  /* fn, arg, stack, prio, name */

/* ---- counters ---------------------------------------------------------------
 *
 * Not static: the call0 side prints them, and a shim that is never called is
 * a shim whose contract was misread. */
uint32_t bt_shim_stub_calls[8];
#define STUB_NVS 0
#define STUB_AES 1
#define STUB_VFS 2
#define STUB_IPC 3
#define STUB_MESH 4
#define STUB_COEX 5
#define STUB_SHUT 6
#define STUB_MISC 7

/* ---- FreeRTOS: queues, semaphores, mutexes ---------------------------------
 *
 * The controller uses one API for all three, distinguished by `type`:
 * 0 base queue, 1 mutex, 2 counting semaphore, 3 binary semaphore,
 * 4 recursive mutex. The kernel has all of them under other names. */
#define pdFALSE 0
#define pdTRUE  1
#define pdPASS  1

/* Counted and reported, because this is where the memory went.
 *
 * The controller refused its configuration with ESP_ERR_NO_MEM against a heap
 * that had 29,320 bytes free, while the largest block it asked bt_host_malloc
 * for was 248 bytes and nothing was refused. Queues do not go through that
 * allocator -- osi_impl_queue_create calls the kernel's heap directly -- so
 * they were invisible. A number that is not printed is a number nobody has. */
uint32_t bt_queue_bytes;
uint32_t bt_queues_made;
uint32_t bt_queues_failed;

void *xQueueGenericCreate(uint32_t len, uint32_t item_size, uint8_t type)
{
    BT_TR("xQueueGenericCreate");
    if (type == 0u) {
        bt_queue_bytes += len * item_size;
        bt_queues_made++;
    }
    if (type == 1u || type == 3u) {
        return (void *)FWD2(osi_impl_sem_create, 1u, 1u);
    }
    if (type == 4u) {
        return (void *)FWD0(osi_impl_recursive_mutex_create);
    }
    void *q = (void *)FWD2(osi_impl_queue_create, len, item_size);
    if (!q) {
        bt_queues_failed++;
        extern void bt_host_queue_fail(uint32_t len, uint32_t item_size);
        FWD2(bt_host_queue_fail, len, item_size);
    }
    return q;
}

/* The static variants ignore the caller's storage.
 *
 * FreeRTOS lets a caller supply the queue's memory so it can live in a known
 * place; this kernel's queues carry their own. Ignoring the buffer is safe --
 * nothing reads it -- and the alternative is a second queue implementation. The
 * controller allocates that storage in ITS static data, so nothing leaks. */
void *xQueueGenericCreateStatic(uint32_t len, uint32_t item_size,
                                uint8_t *storage, void *staticq, uint8_t type)
{
    (void)storage; (void)staticq;
    return xQueueGenericCreate(len, item_size, type);
}

void *xQueueCreateMutex(uint8_t type)
{
    BT_TR("xQueueCreateMutex");
    return (type == 4u) ? (void *)FWD0(osi_impl_recursive_mutex_create)
                        : (void *)FWD2(osi_impl_sem_create, 1u, 1u);
}

void *xQueueCreateCountingSemaphore(uint32_t max, uint32_t init)
{
    BT_TR("xQueueCreateCountingSemaphore");
    return (void *)FWD2(osi_impl_sem_create, max, init);
}

void *xQueueCreateCountingSemaphoreStatic(uint32_t max, uint32_t init, void *q)
{
    (void)q;
    return (void *)FWD2(osi_impl_sem_create, max, init);
}

void vQueueDelete(void *q)
{
    BT_TR("vQueueDelete");
    FWD1(osi_impl_queue_delete, q);
}

int32_t xQueueGenericSend(void *q, const void *item, uint32_t ticks, int32_t pos)
{
    BT_TR("xQueueGenericSend");
    return (int32_t)FWD3(osi_impl_queue_send, q, item, ticks) ? pdTRUE : pdFALSE;
}

int32_t xQueueGenericSendFromISR(void *q, const void *item, int32_t *woken, int32_t pos)
{
    BT_TR("xQueueGenericSendFromISR");
    (void)pos;
    if (woken) {
        *woken = pdFALSE;
    }
    return FWD2(osi_impl_queue_send_isr, q, item) ? pdTRUE : pdFALSE;
}

int32_t xQueueReceive(void *q, void *buf, uint32_t ticks)
{
    BT_TR("xQueueReceive");
    return FWD3(osi_impl_queue_recv, q, buf, ticks) ? pdTRUE : pdFALSE;
}

int32_t xQueueSemaphoreTake(void *q, uint32_t ticks)
{
    BT_TR("xQueueSemaphoreTake");
    return FWD2(osi_impl_sem_take, q, ticks) ? pdTRUE : pdFALSE;
}

int32_t xQueueGiveFromISR(void *q, int32_t *woken)
{
    BT_TR("xQueueGiveFromISR");
    if (woken) {
        *woken = pdFALSE;
    }
    return FWD1(osi_impl_sem_give, q) ? pdTRUE : pdFALSE;
}

int32_t xQueueGiveMutexRecursive(void *q)
{
    BT_TR("xQueueGiveMutexRecursive");
    return FWD1(osi_impl_sem_give, q) ? pdTRUE : pdFALSE;
}

int32_t xQueueTakeMutexRecursive(void *q, uint32_t ticks)
{
    BT_TR("xQueueTakeMutexRecursive");
    return FWD2(osi_impl_sem_take, q, ticks) ? pdTRUE : pdFALSE;
}

uint32_t uxQueueSpacesAvailable(void *q)
{
    BT_TR("uxQueueSpacesAvailable");
    /* The kernel reports what is WAITING; spaces is the complement, and the
     * capacity is not exposed. The controller uses this only to decide whether
     * to post, so "some" is the answer that keeps it moving; a wrong zero would
     * stall it permanently and a wrong large number costs one failed send. */
    uint32_t waiting = FWD1(osi_impl_queue_waiting, q);
    return waiting ? 1u : 8u;
}

/* ---- FreeRTOS: event groups ------------------------------------------------ */

void *xEventGroupCreate(void)
{
    BT_TR("xEventGroupCreate");
    return (void *)FWD0(osi_impl_evt_create);
}

void vEventGroupDelete(void *h)
{
    BT_TR("vEventGroupDelete");
    FWD1(osi_impl_evt_delete, h);
}

uint32_t xEventGroupSetBits(void *h, uint32_t bits)
{
    BT_TR("xEventGroupSetBits");
    return FWD2(osi_impl_evt_set, h, bits);
}

uint32_t xEventGroupClearBits(void *h, uint32_t bits)
{
    BT_TR("xEventGroupClearBits");
    return FWD2(osi_impl_evt_clear, h, bits);
}

uint32_t xEventGroupWaitBits(void *h, uint32_t bits, int32_t clear_on_exit,
                             int32_t wait_all, uint32_t ticks)
{
    BT_TR("xEventGroupWaitBits");
    bt_args_t a = { (uint32_t)h, bits, (uint32_t)clear_on_exit,
                    (uint32_t)wait_all, ticks };
    return FWD1(bt_host_evt_wait, &a);
}

/* ---- FreeRTOS: tasks -------------------------------------------------------- */

int32_t xTaskCreatePinnedToCore(void *fn, const char *name, uint32_t stack,
                                void *arg, uint32_t prio, void **handle,
                                int32_t core)
{
    BT_TR("xTaskCreatePinnedToCore");
    (void)core;                 /* one core runs the controller here */
    bt_args_t a = { (uint32_t)fn, (uint32_t)arg, stack, prio, (uint32_t)name };
    uint32_t h = FWD1(bt_host_task_create, &a);
    if (handle) {
        *handle = (void *)h;
    }
    return h ? pdPASS : pdFALSE;
}

void vTaskDelete(void *h)
{
    BT_TR("vTaskDelete");
    /* Not supported: this kernel's tasks are created once and live for the run.
     * The controller deletes its task only on esp_bt_controller_deinit, which
     * nothing calls, so refusing is honest and visible rather than silent. */
    (void)h;
    bt_shim_stub_calls[STUB_MISC]++;
}

void vTaskDelay(uint32_t ticks)
{
    BT_TR("vTaskDelay");
    extern void task_sleep(uint32_t ticks);
    FWD1(task_sleep, ticks ? ticks : 1u);
}

void vTaskPrioritySet(void *h, uint32_t prio)
{
    BT_TR("vTaskPrioritySet");
    (void)h; (void)prio;
    bt_shim_stub_calls[STUB_MISC]++;
}

char *pcTaskGetName(void *h)
{
    (void)h;
    static char n[8] = { 'b', 't', 0 };
    return n;
}

/* ---- FreeRTOS: the port ---------------------------------------------------- */

int32_t xPortInIsrContext(void)
{
    BT_TR("xPortInIsrContext");
    return (int32_t)FWD0(osi_impl_in_isr);
}

void vPortYield(void)
{
    BT_TR("vPortYield");
    extern void task_yield(void);
    FWD0(task_yield);
}

void vPortEvaluateYieldFromISR(void)
{
    BT_TR("vPortEvaluateYieldFromISR");
    /* Nothing: this scheduler reschedules at the tick, and yielding from an
     * interrupt is not something it offers. The controller uses this as a hint,
     * not a requirement. */
    bt_shim_stub_calls[STUB_MISC]++;
}

uint32_t xPortEnterCriticalTimeout(void *mux, uint32_t timeout)
{
    BT_TR("xPortEnterCriticalTimeout");
    /* The kernel's crit_enter() returns the previous interrupt state and
     * crit_exit() needs it back; FreeRTOS's API carries no such value, so the
     * call0 side keeps it. One level deep is all the controller uses, and
     * deeper nesting is counted rather than silently losing the outer state. */
    (void)mux; (void)timeout;
    extern uint32_t bt_crit_enter(void);
    FWD0(bt_crit_enter);
    return pdPASS;
}

void vPortExitCritical(void *mux)
{
    BT_TR("vPortExitCritical");
    (void)mux;
    extern void bt_crit_exit(void);
    FWD0(bt_crit_exit);
}

/* ---- interrupts ------------------------------------------------------------- */

int32_t esp_intr_alloc(int32_t source, int32_t flags, void *handler, void *arg,
                       void **ret_handle)
{
    BT_TR("esp_intr_alloc");
    (void)flags;
    uint32_t h = FWD3(bt_host_intr_alloc, (uint32_t)source, (uint32_t)handler,
                      (uint32_t)arg);
    if (ret_handle) {
        *ret_handle = (void *)h;
    }
    return h ? 0 : -1;          /* ESP_OK / ESP_FAIL */
}

int32_t esp_intr_free(void *handle)
{
    BT_TR("esp_intr_free");
    FWD1(bt_host_intr_free, (uint32_t)handle);
    return 0;
}

void xt_ints_on(uint32_t mask)
{
    BT_TR("xt_ints_on");
    extern void intr_enable_mask(uint32_t mask);
    FWD1(intr_enable_mask, mask);
}

void xt_ints_off(uint32_t mask)
{
    BT_TR("xt_ints_off");
    extern void intr_disable_mask(uint32_t mask);
    FWD1(intr_disable_mask, mask);
}

void xt_set_interrupt_handler(int32_t n, void *fn, void *arg)
{
    BT_TR("xt_set_interrupt_handler");
    /* The line is the CALLER's choice here, unlike esp_intr_alloc where the
     * host picks one for a peripheral source. Refusing this was a kernel panic:
     * exccause 4, an interrupt with no handler behind it, because the
     * controller enabled the line regardless -- see bt_host_set_handler. */
    extern uint32_t bt_host_set_handler(uint32_t line, uint32_t fn, uint32_t arg);
    FWD3(bt_host_set_handler, (uint32_t)n, (uint32_t)fn, (uint32_t)arg);
}

/* ---- esp_timer -------------------------------------------------------------
 *
 * The controller creates timers, arms them in microseconds, and reads a 64-bit
 * microsecond clock. kernel/wifi_osi_impl.c has all of that except the clock,
 * whose resolution is this kernel's 10 ms tick -- stated plainly in
 * bt_host_us(), and the first thing to suspect if the controller misbehaves
 * around sleep.
 *
 * esp_timer_create's args struct is { callback, arg, dispatch, name, skip }.
 * The handle it returns is the caller's own storage, which the kernel's timers
 * accept as an identity. */
typedef struct {
    void *callback;
    void *arg;
    int dispatch_method;
    const char *name;
    uint32_t skip_unhandled_events;
} esp_timer_create_args_t;

static uint8_t g_timer_slots[8][4];     /* identities, one per created timer */
static uint32_t g_timers_used;

int32_t esp_timer_create(const esp_timer_create_args_t *args, void **out)
{
    BT_TR("esp_timer_create");
    if (!args || !out || g_timers_used >= 8u) {
        return -1;
    }
    void *id = &g_timer_slots[g_timers_used++][0];
    FWD3(osi_impl_timer_setfn, id, args->callback, args->arg);
    *out = id;
    return 0;
}

int32_t esp_timer_start_once(void *h, uint64_t us)
{
    BT_TR("esp_timer_start_once");
    FWD3(osi_impl_timer_arm_us, h, (uint32_t)us, 0);
    return 0;
}

int32_t esp_timer_start_periodic(void *h, uint64_t us)
{
    BT_TR("esp_timer_start_periodic");
    FWD3(osi_impl_timer_arm_us, h, (uint32_t)us, 1);
    return 0;
}

int32_t esp_timer_stop(void *h)
{
    BT_TR("esp_timer_stop");
    FWD1(osi_impl_timer_disarm, h);
    return 0;
}

int32_t esp_timer_delete(void *h)
{
    BT_TR("esp_timer_delete");
    FWD1(osi_impl_timer_done, h);
    return 0;
}

int32_t esp_timer_is_active(void *h)
{
    BT_TR("esp_timer_is_active");
    (void)h;
    return 0;                   /* the controller uses this only as a hint */
}

uint64_t esp_timer_get_time(void)
{
    BT_TR("esp_timer_get_time");
    uint32_t t[2] = { 0, 0 };
    FWD1(bt_host_us, t);
    return ((uint64_t)t[1] << 32) | t[0];
}

uint64_t esp_system_get_time(void)
{
    BT_TR("esp_system_get_time");
    return esp_timer_get_time();
}

int32_t clock_gettime(int32_t clk, void *tp)
{
    /* struct timespec { long tv_sec; long tv_nsec; } -- the controller reads it
     * for logging only. */
    (void)clk;
    if (tp) {
        uint32_t t[2] = { 0, 0 };
        FWD1(bt_host_us, t);
        ((int32_t *)tp)[0] = (int32_t)(t[0] / 1000000u);
        ((int32_t *)tp)[1] = 0;
    }
    return 0;
}

/* ---- the heap --------------------------------------------------------------- */

void *malloc(size_t n)              { return (void *)FWD1(bt_host_malloc, n); }
void free(void *p)                  { FWD1(bt_host_free, p); }

void *calloc(size_t n, size_t sz)
{
    BT_TR("calloc");
    uint32_t bytes = (uint32_t)n * (uint32_t)sz;
    uint8_t *p = (uint8_t *)FWD1(bt_host_malloc, bytes);
    if (p) {
        for (uint32_t i = 0; i < bytes; i++) {
            p[i] = 0;
        }
    }
    return p;
}

void *heap_caps_malloc(size_t n, uint32_t caps)
{
    BT_TR("heap_caps_malloc");
    (void)caps;                 /* one kind of memory here, and it is DRAM */
    return (void *)FWD1(bt_host_malloc, n);
}

void *heap_caps_calloc(size_t n, size_t sz, uint32_t caps)
{
    BT_TR("heap_caps_calloc");
    (void)caps;
    return calloc(n, sz);
}

void heap_caps_free(void *p)        { FWD1(bt_host_free, p); }

int32_t heap_caps_add_region(uint32_t start, uint32_t end)
{
    /* Refused. This kernel's heap owns one region, fixed at link time; handing
     * it the controller's leftovers would mean a second allocator agreeing
     * about the same bytes. */
    (void)start; (void)end;
    bt_shim_stub_calls[STUB_MISC]++;
    return -1;
}

/* ---- randomness and the MAC ------------------------------------------------ */

uint32_t esp_random(void)
{
    BT_TR("esp_random");
    uint32_t v = 0;
    FWD2(osi_impl_get_random, &v, 4u);
    return v;
}

void esp_fill_random(void *buf, size_t len)
{
    BT_TR("esp_fill_random");
    FWD2(osi_impl_get_random, buf, (uint32_t)len);
}

int32_t esp_read_mac(uint8_t *mac, uint32_t type)
{
    BT_TR("esp_read_mac");
    return (int32_t)FWD2(osi_impl_read_mac, mac, type);
}

int32_t esp_efuse_mac_get_default(uint8_t *mac)
{
    return (int32_t)FWD2(osi_impl_read_mac, mac, 0u);
}

/* ---- the radio and its clocks ----------------------------------------------
 *
 * The PHY is shared hardware and its bring-up is kernel/phyinit.c, written for
 * WiFi and needed identically here. Everything else in this group is a register
 * poke the blob does itself once the PHY is up; the hooks exist because IDF
 * splits them across components. */
int32_t esp_phy_enable(uint32_t modem)
{
    BT_TR("esp_phy_enable");
    (void)modem;
    return FWD0(bt_host_phy_enable);
}

void esp_phy_disable(uint32_t modem)       { (void)modem; }
void esp_phy_modem_init(void)              { }
void esp_phy_modem_deinit(void)            { }
int64_t esp_phy_rf_get_on_ts(void)         { return (int64_t)esp_timer_get_time(); }

void esp_wifi_bt_power_domain_on(void)     { }
void esp_wifi_bt_power_domain_off(void)    { }

/* Not a no-op, which is what these were: the BT clock stayed gated and the BT
 * resets stayed asserted through the whole of esp_bt_controller_init, and it
 * spun on a register that could never change until the TG0 watchdog reset the
 * chip. The register work is in bt_host.c, next to the WiFi side's. */
void periph_module_enable(uint32_t module)
{
    BT_TR("periph_module_enable");
    extern void bt_host_periph(uint32_t module, uint32_t on);
    FWD2(bt_host_periph, module, 1u);
}

void periph_module_disable(uint32_t module)
{
    extern void bt_host_periph(uint32_t module, uint32_t on);
    FWD2(bt_host_periph, module, 0u);
}

uint32_t rtc_clk_xtal_freq_get(void)
{
    BT_TR("rtc_clk_xtal_freq_get");
    return 40u;                 /* MHz; this board's crystal, per board.h */
}

/* Renamed by patch_bt.py: vendor/windowed/phy_host.c already defines this one
 * for the WiFi path, in call0. */
uint32_t bt_dport_read(uint32_t reg)
{
    return *(volatile uint32_t *)reg;
}

void esp_rom_delay_us(uint32_t us)
{
    BT_TR("esp_rom_delay_us");
    extern void bt_host_delay_us(uint32_t us);
    FWD1(bt_host_delay_us, us);
}

/* The BT baseband hooks -- bt_bb_init_cmplx, bt_track_pll_cap, phy_bt_ifs_set,
 * phy_bt_pll_track, phy_bt_power_track -- are NOT here.
 *
 * They were stubbed first, on the assumption that libesp_phy supplied them and
 * was not linked. libphy_natos.a defines all of them, in the windowed ABI the
 * blob calls with, and the linker said so: "multiple definition of phy_bt_...".
 * Real radio code beats a counted stub, so the stubs are gone and the archive
 * answers them.
 *
 * TWO of the six are not in it: bt_bb_init_cmplx and its _reg variant live in
 * libesp_phy.a, which this build does not link (it brings ESP-IDF's own
 * expectations with it). They configure the BT baseband's complex filter, the
 * controller calls them once during init, and whether BLE works without them is
 * one of the things stage 1 is for. Counted, so the answer is a number. */
void bt_bb_init_cmplx(void)     { bt_shim_stub_calls[STUB_MISC]++; }
void bt_bb_init_cmplx_reg(void) { bt_shim_stub_calls[STUB_MISC]++; }

/* ---- WiFi coexistence ------------------------------------------------------
 *
 * There is no WiFi in a -BT build, so there is nothing to coexist with. These
 * are the hooks libcoexist expects from the WiFi side. */
void force_wifi_mode(int32_t m)     { (void)m; bt_shim_stub_calls[STUB_COEX]++; }
void unforce_wifi_mode(void)        { bt_shim_stub_calls[STUB_COEX]++; }

int32_t esp_coex_version_get(const char **v)
{
    BT_TR("esp_coex_version_get");
    static const char s[] = "nat-os, no coexistence";
    if (v) { *v = s; }
    return 0;
}

/* ---- logging --------------------------------------------------------------- */

void esp_log_write(uint32_t level, const char *tag, const char *fmt, ...)
{
    (void)level; (void)tag;
    FWD1(bt_host_log, fmt);     /* not expanded -- see bt_host_log */
}

uint32_t esp_log_timestamp(void)
{
    uint32_t t[2] = { 0, 0 };
    FWD1(bt_host_us, t);
    return t[0] / 1000u;
}

int32_t esp_rom_printf(const char *fmt, ...)
{
    FWD1(bt_host_log, fmt);
    return 0;
}

int32_t coexist_printf(const char *fmt, ...)
{
    FWD1(bt_host_log, fmt);
    return 0;
}

int32_t puts(const char *s)
{
    FWD1(bt_host_log, s);
    return 0;
}

/* ---- NVS, AES, VFS, IPC, mesh: stubs that say so --------------------------- */

#define ESP_ERR_NVS_NOT_FOUND 0x1102

int32_t nvs_open(const char *name, uint32_t mode, uint32_t *out)
{
    BT_TR("nvs_open");
    (void)name; (void)mode;
    bt_shim_stub_calls[STUB_NVS]++;
    if (out) { *out = 1u; }
    return 0;
}

void nvs_close(uint32_t h) { (void)h; bt_shim_stub_calls[STUB_NVS]++; }

int32_t nvs_commit(uint32_t h) { (void)h; return 0; }
int32_t nvs_erase_all(uint32_t h) { (void)h; return 0; }

int32_t nvs_get_blob(uint32_t h, const char *key, void *buf, size_t *len)
{
    BT_TR("nvs_get_blob");
    (void)h; (void)key; (void)buf; (void)len;
    bt_shim_stub_calls[STUB_NVS]++;
    return ESP_ERR_NVS_NOT_FOUND;   /* "nothing stored" is a legal answer */
}

int32_t nvs_set_blob(uint32_t h, const char *key, const void *buf, size_t len)
{
    (void)h; (void)key; (void)buf; (void)len;
    bt_shim_stub_calls[STUB_NVS]++;
    return 0;                   /* accepted and dropped: nothing reads it back */
}

int32_t esp_aes_init(void *ctx)                 { (void)ctx; bt_shim_stub_calls[STUB_AES]++; return 0; }
void    esp_aes_free(void *ctx)                 { (void)ctx; }
int32_t esp_aes_setkey(void *c, const void *k, uint32_t bits) { (void)c; (void)k; (void)bits; return 0; }
int32_t esp_aes_crypt_ecb(void *c, int32_t mode, const void *in, void *out)
{
    /* Refused rather than faked. This is link-crypto for pairing, which a
     * scanner and an advertiser never reach; a zeroed "encryption" that looked
     * like it worked would be worse than an error. */
    (void)c; (void)mode; (void)in; (void)out;
    bt_shim_stub_calls[STUB_AES]++;
    return -1;
}

int32_t esp_vfs_register_fd(void *vfs, int32_t *fd)        { (void)vfs; (void)fd; bt_shim_stub_calls[STUB_VFS]++; return -1; }
int32_t esp_vfs_unregister_fd(void *vfs, int32_t fd)       { (void)vfs; (void)fd; return -1; }
int32_t esp_vfs_register_with_id(void *vfs, void *ctx, void *id) { (void)vfs; (void)ctx; (void)id; bt_shim_stub_calls[STUB_VFS]++; return -1; }
int32_t esp_vfs_unregister_with_id(void *id)               { (void)id; return -1; }

int32_t esp_ipc_call(uint32_t cpu, void (*fn)(void *), void *arg)
{
    /* One core runs the controller, so "call on the other CPU" is "call". */
    (void)cpu;
    bt_shim_stub_calls[STUB_IPC]++;
    if (fn) { fn(arg); }
    return 0;
}

int32_t esp_ipc_call_blocking(uint32_t cpu, void (*fn)(void *), void *arg)
{
    return esp_ipc_call(cpu, fn, arg);
}

int32_t esp_register_shutdown_handler(void *fn)   { (void)fn; bt_shim_stub_calls[STUB_SHUT]++; return 0; }
int32_t esp_unregister_shutdown_handler(void *fn) { (void)fn; return 0; }

void bt_mesh_prov_complete(uint16_t net_idx, uint16_t addr) { (void)net_idx; (void)addr; bt_shim_stub_calls[STUB_MESH]++; }
void bt_mesh_prov_reset(void)                               { bt_shim_stub_calls[STUB_MESH]++; }

/* ---- byte order and the odds and ends -------------------------------------- */

/* Renamed by patch_bt.py: lwIP defines these, in call0. */
uint32_t bt_htonl(uint32_t v)
{
    return ((v & 0xFFu) << 24) | ((v & 0xFF00u) << 8)
         | ((v >> 8) & 0xFF00u) | ((v >> 24) & 0xFFu);
}

uint16_t bt_htons(uint16_t v)
{
    return (uint16_t)((v << 8) | (v >> 8));
}

/* The compiler's and the blob's safety nets. A failed assertion inside a blob
 * is not recoverable, so it is reported and the caller is left to hang rather
 * than returned into with corrupt state. */
void __assert_func(const char *file, int line, const char *fn, const char *expr)
{
    (void)file; (void)line; (void)fn;
    FWD1(bt_host_log, expr ? expr : "assert");
    for (;;) { }
}

void _esp_error_check_failed(int32_t rc, const char *file, int line,
                             const char *fn, const char *expr)
{
    (void)rc; (void)file; (void)line; (void)fn;
    FWD1(bt_host_log, expr ? expr : "ESP_ERROR_CHECK");
    for (;;) { }
}

static int g_errno;
int *__errno(void) { return &g_errno; }

uint32_t __stack_chk_guard = 0xDEADBEEFu;
void __stack_chk_fail(void)
{
    static const char s[] = "stack check failed inside the BT blob";
    FWD1(bt_host_log, s);
    for (;;) { }
}

/* ---- float division, without the FPU ---------------------------------------
 *
 * The archives call __divsf3. kernel/mp3.c defines one -- and mp3.c is the only
 * file in this project permitted to use the FPU, because FP registers are not
 * saved across a task switch and the build REFUSES any other file that touches
 * them. That guard caught the first version of this function, which was
 * mp3.c's Newton-Raphson copied across.
 *
 * So the blob's reference is renamed to btsf_divsf3 (vendor/bt/patch_bt.py) and
 * answered here in integer arithmetic: IEEE-754 decomposition, a 24-step
 * restoring division, round to nearest. No FP register is touched, so this is
 * safe on any task.
 *
 * Infinities and NaNs are handled as the blob's uses require -- timing and
 * power arithmetic, never a limit case -- and a division by zero returns
 * infinity rather than trapping, because there is nothing here to trap to. */
float btsf_divsf3(float fa, float fb)
{
    union { float f; uint32_t u; } A, B, R;
    A.f = fa;
    B.f = fb;

    uint32_t sign = (A.u ^ B.u) >> 31;
    int32_t  ea = (int32_t)((A.u >> 23) & 0xFFu);
    int32_t  eb = (int32_t)((B.u >> 23) & 0xFFu);
    uint32_t ma = A.u & 0x7FFFFFu;
    uint32_t mb = B.u & 0x7FFFFFu;

    if (eb == 0xFF || (ea == 0 && ma == 0)) {       /* b infinite, or a zero */
        R.u = sign << 31;                           /* signed zero */
        return R.f;
    }
    if (ea == 0xFF || (eb == 0 && mb == 0)) {       /* a infinite, or b zero */
        R.u = (sign << 31) | 0x7F800000u;           /* signed infinity */
        return R.f;
    }
    if (ea == 0 || eb == 0) {                       /* denormal: treated as 0 */
        R.u = (ea == 0) ? (sign << 31) : ((sign << 31) | 0x7F800000u);
        return R.f;
    }

    ma |= 0x800000u;                                /* the implicit 1 */
    mb |= 0x800000u;
    int32_t e = ea - eb + 127;

    /* Line the numerator up so the quotient's leading bit lands at bit 23. */
    uint32_t num = ma;
    if (num < mb) {
        num <<= 1;
        e--;
    }

    uint32_t q = 0;
    for (int i = 0; i < 24; i++) {
        q <<= 1;
        if (num >= mb) {
            num -= mb;
            q |= 1u;
        }
        num <<= 1;                  /* stays below 2^26: mb < 2^24 */
    }

    if (num >= mb) {                /* round to nearest, ties away from zero */
        q++;
        if (q == 0x1000000u) {
            q >>= 1;
            e++;
        }
    }

    if (e <= 0) {
        R.u = sign << 31;                           /* underflow to zero */
        return R.f;
    }
    if (e >= 0xFF) {
        R.u = (sign << 31) | 0x7F800000u;           /* overflow to infinity */
        return R.f;
    }
    R.u = (sign << 31) | ((uint32_t)e << 23) | (q & 0x7FFFFFu);
    return R.f;
}

/* ---- the VHCI side: talking to the controller ------------------------------
 *
 * The controller's host interface is three functions in the blob and a callback
 * struct it calls back through. The callbacks are WINDOWED, which is the whole
 * reason they live in this file: the blob invokes them directly, and a call0
 * function in that slot would execute ENTRY with no rotated window behind it
 * (vendor/phy/README.md, the second of its two failures).
 *
 * What arrives is buffered here, as DATA, and read by the call0 side. A pointer
 * crossing the boundary is safe where a call would not be. */
extern int  API_vhci_host_check_send_available(void);
extern void API_vhci_host_send_packet(uint8_t *data, uint16_t len);
extern int  API_vhci_host_register_callback(const void *cb);

/* Not static: kernel/bt.c reads all of these. */
uint8_t  bt_vhci_evt[64];
uint32_t bt_vhci_evt_len;
uint32_t bt_vhci_events;        /* events received since boot   */
uint32_t bt_vhci_ready;         /* "you may send" notifications */
uint32_t bt_vhci_dropped;       /* events longer than the buffer */

static void vhci_send_available(void)
{
    bt_vhci_ready++;
}

static int vhci_recv(uint8_t *data, uint16_t len)
{
    bt_vhci_events++;
    if (len > sizeof bt_vhci_evt) {
        bt_vhci_dropped++;
        len = sizeof bt_vhci_evt;
    }
    for (uint16_t i = 0; i < len; i++) {
        bt_vhci_evt[i] = data[i];
    }
    bt_vhci_evt_len = len;
    return 0;
}

/* The order of these two is the contract, and it is not checkable from here:
 * vhci_host_callback_t is { notify_host_send_available, notify_host_recv }.
 * One swapped and the controller calls a receive handler with no arguments. */
static const struct {
    void (*notify_host_send_available)(void);
    int  (*notify_host_recv)(uint8_t *data, uint16_t len);
} g_vhci_cb = { vhci_send_available, vhci_recv };

/* Windowed entry points for the call0 side, reached through phy_stack_call. */
uint32_t bt_vhci_register(void)
{
    return (uint32_t)API_vhci_host_register_callback(&g_vhci_cb);
}

uint32_t bt_vhci_can_send(void)
{
    return (uint32_t)API_vhci_host_check_send_available();
}

uint32_t bt_vhci_send(uint32_t data, uint32_t len)
{
    API_vhci_host_send_packet((uint8_t *)data, (uint16_t)len);
    return 1;
}
