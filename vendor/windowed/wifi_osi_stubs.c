/* nat-os -- the WiFi OS adapter stubs.  GENERATED -- do not hand-edit.
 *
 * COMPILED -mabi=windowed, and that is the whole reason this file lives in
 * vendor/windowed/ rather than kernel/.
 *
 * The blob is windowed and calls these through the table with CALL8. When they
 * were ordinary call0 kernel functions, the window rotated forward on entry,
 * the callee used the rotated registers as its own, and its RET did not rotate
 * back -- so a0 still held the windowed return encoding and the CPU jumped
 * straight to it. That is exactly what happened: IllegalInstruction at
 * epc 0x803014fd, an address with bit 31 set, which is a (2<<30)|offset
 * return encoding rather than any real code address. window.S records the
 * same fault from the first time it was hit.
 *
 * Counters are NOT static: kernel/wifi_osi_table.c is call0 and reads them
 * directly as DATA, which crosses the ABI boundary safely because data has no
 * calling convention. Only the CALLS had to move.
 */

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdarg.h>

#define CONFIG_IDF_TARGET_ESP32 1
#define ESP_WIFI_OS_ADAPTER_VERSION  0x00000008
#define ESP_WIFI_OS_ADAPTER_MAGIC    0xDEADBEAF
typedef struct {
    int32_t _version;
    bool (* _env_is_chip)(void);
    void (*_set_intr)(int32_t cpu_no, uint32_t intr_source, uint32_t intr_num, int32_t intr_prio);
    void (*_clear_intr)(uint32_t intr_source, uint32_t intr_num);
    void (*_set_isr)(int32_t n, void *f, void *arg);
    void (*_ints_on)(uint32_t mask);
    void (*_ints_off)(uint32_t mask);
    bool (* _is_from_isr)(void);
    void *(* _spin_lock_create)(void);
    void (* _spin_lock_delete)(void *lock);
    uint32_t (*_wifi_int_disable)(void *wifi_int_mux);
    void (*_wifi_int_restore)(void *wifi_int_mux, uint32_t tmp);
    void (*_task_yield_from_isr)(void);
    void *(*_semphr_create)(uint32_t max, uint32_t init);
    void (*_semphr_delete)(void *semphr);
    int32_t (*_semphr_take)(void *semphr, uint32_t block_time_tick);
    int32_t (*_semphr_give)(void *semphr);
    void *(*_wifi_thread_semphr_get)(void);
    void *(*_mutex_create)(void);
    void *(*_recursive_mutex_create)(void);
    void (*_mutex_delete)(void *mutex);
    int32_t (*_mutex_lock)(void *mutex);
    int32_t (*_mutex_unlock)(void *mutex);
    void *(* _queue_create)(uint32_t queue_len, uint32_t item_size);
    void (* _queue_delete)(void *queue);
    int32_t (* _queue_send)(void *queue, void *item, uint32_t block_time_tick);
    int32_t (* _queue_send_from_isr)(void *queue, void *item, void *hptw);
    int32_t (* _queue_send_to_back)(void *queue, void *item, uint32_t block_time_tick);
    int32_t (* _queue_send_to_front)(void *queue, void *item, uint32_t block_time_tick);
    int32_t (* _queue_recv)(void *queue, void *item, uint32_t block_time_tick);
    uint32_t (* _queue_msg_waiting)(void *queue);
    void *(* _event_group_create)(void);
    void (* _event_group_delete)(void *event);
    uint32_t (* _event_group_set_bits)(void *event, uint32_t bits);
    uint32_t (* _event_group_clear_bits)(void *event, uint32_t bits);
    uint32_t (* _event_group_wait_bits)(void *event, uint32_t bits_to_wait_for, int clear_on_exit, int wait_for_all_bits, uint32_t block_time_tick);
    int32_t (* _task_create_pinned_to_core)(void *task_func, const char *name, uint32_t stack_depth, void *param, uint32_t prio, void *task_handle, uint32_t core_id);
    int32_t (* _task_create)(void *task_func, const char *name, uint32_t stack_depth, void *param, uint32_t prio, void *task_handle);
    void (* _task_delete)(void *task_handle);
    void (* _task_delay)(uint32_t tick);
    int32_t (* _task_ms_to_tick)(uint32_t ms);
    void *(* _task_get_current_task)(void);
    int32_t (* _task_get_max_priority)(void);
    void *(* _malloc)(size_t size);
    void (* _free)(void *p);
    int32_t (* _event_post)(const char* event_base, int32_t event_id, void* event_data, size_t event_data_size, uint32_t ticks_to_wait);
    uint32_t (* _get_free_heap_size)(void);
    uint32_t (* _rand)(void);
    void (* _dport_access_stall_other_cpu_start_wrap)(void);
    void (* _dport_access_stall_other_cpu_end_wrap)(void);
    void (* _wifi_apb80m_request)(void);
    void (* _wifi_apb80m_release)(void);
    void (* _phy_disable)(void);
    void (* _phy_enable)(void);
#if CONFIG_IDF_TARGET_ESP32 || CONFIG_IDF_TARGET_ESP32S2
    void (* _phy_common_clock_enable)(void);
    void (* _phy_common_clock_disable)(void);
#endif
    int (* _phy_update_country_info)(const char* country);
    int (* _read_mac)(uint8_t* mac, unsigned int type);
    void (* _timer_arm)(void *timer, uint32_t tmout, bool repeat);
    void (* _timer_disarm)(void *timer);
    void (* _timer_done)(void *ptimer);
    void (* _timer_setfn)(void *ptimer, void *pfunction, void *parg);
    void (* _timer_arm_us)(void *ptimer, uint32_t us, bool repeat);
    void (* _wifi_reset_mac)(void);
    void (* _wifi_clock_enable)(void);
    void (* _wifi_clock_disable)(void);
    void (* _wifi_rtc_enable_iso)(void);
    void (* _wifi_rtc_disable_iso)(void);
    int64_t (* _esp_timer_get_time)(void);
    int (* _nvs_set_i8)(uint32_t handle, const char* key, int8_t value);
    int (* _nvs_get_i8)(uint32_t handle, const char* key, int8_t* out_value);
    int (* _nvs_set_u8)(uint32_t handle, const char* key, uint8_t value);
    int (* _nvs_get_u8)(uint32_t handle, const char* key, uint8_t* out_value);
    int (* _nvs_set_u16)(uint32_t handle, const char* key, uint16_t value);
    int (* _nvs_get_u16)(uint32_t handle, const char* key, uint16_t* out_value);
    int (* _nvs_open)(const char* name, unsigned int open_mode, uint32_t *out_handle);
    void (* _nvs_close)(uint32_t handle);
    int (* _nvs_commit)(uint32_t handle);
    int (* _nvs_set_blob)(uint32_t handle, const char* key, const void* value, size_t length);
    int (* _nvs_get_blob)(uint32_t handle, const char* key, void* out_value, size_t* length);
    int (* _nvs_erase_key)(uint32_t handle, const char* key);
    int (* _get_random)(uint8_t *buf, size_t len);
    int (* _get_time)(void *t);
    unsigned long (* _random)(void);
#if !CONFIG_IDF_TARGET_ESP32
    uint32_t (* _slowclk_cal_get)(void);
#endif
    void (* _log_write)(unsigned int level, const char* tag, const char* format, ...);
    void (* _log_writev)(unsigned int level, const char* tag, const char* format, va_list args);
    uint32_t (* _log_timestamp)(void);
    void * (* _malloc_internal)(size_t size);
    void * (* _realloc_internal)(void *ptr, size_t size);
    void * (* _calloc_internal)(size_t n, size_t size);
    void * (* _zalloc_internal)(size_t size);
    void * (* _wifi_malloc)(size_t size);
    void * (* _wifi_realloc)(void *ptr, size_t size);
    void * (* _wifi_calloc)(size_t n, size_t size);
    void * (* _wifi_zalloc)(size_t size);
    void * (* _wifi_create_queue)(int queue_len, int item_size);
    void (* _wifi_delete_queue)(void * queue);
    int (* _coex_init)(void);
    void (* _coex_deinit)(void);
    int (* _coex_enable)(void);
    void (* _coex_disable)(void);
    uint32_t (* _coex_status_get)(void);
    void (* _coex_condition_set)(uint32_t type, bool dissatisfy);
    int (* _coex_wifi_request)(uint32_t event, uint32_t latency, uint32_t duration);
    int (* _coex_wifi_release)(uint32_t event);
    int (* _coex_wifi_channel_set)(uint8_t primary, uint8_t secondary);
    int (* _coex_event_duration_get)(uint32_t event, uint32_t *duration);
    int (* _coex_pti_get)(uint32_t event, uint8_t *pti);
    void (* _coex_schm_status_bit_clear)(uint32_t type, uint32_t status);
    void (* _coex_schm_status_bit_set)(uint32_t type, uint32_t status);
    int (* _coex_schm_interval_set)(uint32_t interval);
    uint32_t (* _coex_schm_interval_get)(void);
    uint8_t (* _coex_schm_curr_period_get)(void);
    void * (* _coex_schm_curr_phase_get)(void);
    int (* _coex_schm_process_restart)(void);
    int (* _coex_schm_register_cb)(int, int (* cb)(int));
    int (* _coex_register_start_cb)(int (* cb)(void));
#if CONFIG_IDF_TARGET_ESP32C6
    void (* _regdma_link_set_write_wait_content)(void *, uint32_t, uint32_t);
    void * (* _sleep_retention_find_link_by_id)(int);
#endif
    int (*_coex_schm_flexible_period_set)(uint8_t);
    uint8_t (*_coex_schm_flexible_period_get)(void);
    int32_t _magic;
} wifi_osi_funcs_t;

#define OSI_N 118u

uint16_t g_osi_calls[OSI_N];
uint8_t  g_osi_order[OSI_N];
uint8_t  g_osi_seq;
uint32_t g_osi_intr_clamped;   /* interrupts asked for above CRIT_LEVEL */

/* Exact call SEQUENCE, with repeats -- g_osi_order only records the first time
 * each entry is touched, which cannot show a loop or a retry. Paired with the
 * argument where the entry has one worth seeing (allocation sizes). */
#define OSI_TRACE_MAX 48u
uint8_t  g_osi_trace[OSI_TRACE_MAX];
uint32_t g_osi_trace_arg[OSI_TRACE_MAX];
uint32_t g_osi_trace_n;

static void osi_trace(uint32_t i, uint32_t arg)
{
    if (g_osi_trace_n < OSI_TRACE_MAX) {
        g_osi_trace[g_osi_trace_n] = (uint8_t)i;
        g_osi_trace_arg[g_osi_trace_n] = arg;
        g_osi_trace_n++;
    }
}

/* Bridges into nat-os's call0 side. The blob calls us windowed; the kernel's
 * heap, mutexes and scheduler are call0, so every real body hands off through
 * these. See window.S. */
extern uint32_t w2c_call0f(uint32_t fn);
extern uint32_t w2c_call1(uint32_t fn, uint32_t a);
extern uint32_t w2c_call2(uint32_t fn, uint32_t a, uint32_t b);
extern void osi_impl_sem_create(void);   /* address only -- see w2c_call2 */
extern void osi_impl_sem_delete(void);
extern void osi_impl_sem_take(void);
extern void osi_impl_sem_give(void);
extern void osi_impl_malloc(void);
extern void osi_impl_calloc(void);
extern void osi_impl_free(void);
extern void task_current(void);
extern void osi_impl_queue_create(void);
extern void osi_impl_queue_delete(void);
extern void blob_task_create(void);
extern void blob_lock(void);
extern void blob_unlock(void);
extern void win_spill_all(void);   /* windowed; callable directly from here */
extern void osi_impl_queue_recv(void);
extern void osi_impl_delay(void);
extern void osi_impl_evt_wait(void);
extern void osi_impl_queue_send(void);

/* WOE watch. next_moves/08 step 35.
 *
 * PS.WOE clear makes every windowed instruction illegal, and something on this
 * path clears it -- the fault landed inside win_spill_all, a routine measured
 * correct in isolation, with ps 0x00030210.
 *
 * Every one of the 118 adapter entries passes through osi_hit(), so this is the
 * one place that sees the whole blob/kernel boundary. It records the FIRST
 * crossing at which WOE is already clear: that entry is the first witness, and
 * whatever ran between it and the previously recorded entry is the writer.
 *
 * Recorded once and never overwritten -- the first transition is the evidence,
 * every later one is a consequence. */
/* PS as it stands on entry to the blocking path, BEFORE the spill runs.
 *
 * Step 37. Everything since step 30 assumed the spill causes the exception
 * state. If EXCM is already set here, the spill is only the first windowed
 * instruction to notice -- the same mistake made with WOE one step earlier. */
volatile uint32_t g_stub_ps_pre_spill = 0xFFFFFFFFu;

/* The stack pointer at the same moment. win_spill_all nests six CALL12 frames
 * of 48 bytes plus whatever the spills themselves write, on the CALLER'S task
 * stack -- 2048 bytes for the shell, already 1344 deep at its tightest. If the
 * spill runs off the end, the window handlers read frames back from memory that
 * was never a save area, which is step 40's return address pointing into the
 * blob's .data. */
volatile uint32_t g_stub_sp_pre_spill;
volatile uint32_t g_stub_sp_min = 0xFFFFFFFFu;

/* First underflow recovery that is not a code address, and which side of the
 * spill it appeared on. EXCSAVE_4/5 are written by _WindowUnderflow* on every
 * underflow (step 42), so they must be filtered here rather than there -- the
 * vectors have no room for a compare. Sticky: the first one is the evidence. */
volatile uint32_t g_uf_bad_a0;
volatile uint32_t g_uf_bad_base;
volatile uint32_t g_uf_bad_when;      /* 1 = before the spill, 2 = after */

/* Same idea for the OVERFLOW side: a recovered caller sp that is not a plausible
 * DRAM stack address is a frame whose base save area was never written. */
volatile uint32_t g_of_bad_base;
volatile uint32_t g_of_bad_frame;
volatile uint32_t g_of_bad_when;

/* [step 99] the blob's return address into osi_s_queue_recv. */
volatile uint32_t g_qr_caller, g_qr_caller_raw;

/* [X4 experiment] Window-state coherence across the voluntary block.
 *
 * H1 is eliminated: no tick ever lands mid-window-handler. The only remaining
 * window-state transitions on the failing path are the ones this task performs
 * itself around a contended take: the pre-block spill, and whatever the
 * block/wake round trip does to WINDOWBASE/WINDOWSTART. Sampled at three
 * points of every blocking excursion:
 *   0 = before win_spill_all, 1 = after it, 2 = after the block returns.
 *
 * Overwritten on every excursion, so after a fault these describe the LAST
 * excursion -- the one nearest the event. Post-spill must be exactly one live
 * frame; anything else means the sweep finished over frames that do not belong
 * to this task, whose register slots no longer hold coherent stack pointers --
 * which is what an overflow faulting on a13 ~= 25 inside win_spill_all reads. */
/* [X5 experiment] 0xDEADBEEF sentinels: a slot still holding it after a run
 * was never sampled, distinguishing "phase not reached" from a genuine
 * zero/zero window state (which is unexecutable and was ambiguous in X4). */
volatile uint32_t g_blk_ws[3] = {0xDEADBEEFu, 0xDEADBEEFu, 0xDEADBEEFu};
volatile uint32_t g_blk_wb[3] = {0xDEADBEEFu, 0xDEADBEEFu, 0xDEADBEEFu};
volatile uint32_t g_blk_union;    /* g_win_union as it stood at post-spill */

static void blk_sample(uint32_t phase)
{
    uint32_t wb, ws;
    __asm__ volatile ("rsr.windowbase %0" : "=r"(wb));
    __asm__ volatile ("rsr.windowstart %0" : "=r"(ws));
    g_blk_wb[phase] = wb;
    g_blk_ws[phase] = ws;
    if (phase == 1u) {
        extern volatile uint32_t g_win_union;   /* task.c */
        g_blk_union = g_win_union;
    }
}

static void of_sample(uint32_t when)
{
    uint32_t base, frame;
    __asm__ volatile ("rsr.excsave6 %0" : "=r"(base));
    __asm__ volatile ("rsr.excsave7 %0" : "=r"(frame));
    /* 0xFFFFFFFF is the seed (win_probe_seed) meaning "no overflow yet", and a
     * base at or above DRAM is a legitimate recovery. Only what is neither is
     * worth reporting. */
    if (g_of_bad_when == 0u
        && ((base != 0xFFFFFFFFu && base < 0x3ff00000u)
            || (frame != 0xFFFFFFFFu && frame < 0x3ff00000u))) {
        g_of_bad_base  = base;
        g_of_bad_frame = frame;
        g_of_bad_when  = when;
    }
}

static void uf_sample(uint32_t when)
{
    uint32_t a0, base;
    __asm__ volatile ("rsr.excsave4 %0" : "=r"(a0));
    __asm__ volatile ("rsr.excsave5 %0" : "=r"(base));
    if (g_uf_bad_when == 0u && a0 != 0u && a0 < 0x40000000u) {
        g_uf_bad_a0   = a0;
        g_uf_bad_base = base;
        g_uf_bad_when = when;
    }
}

volatile uint32_t g_woe_lost_ps;
volatile uint32_t g_woe_lost_at   = 0xFFFFFFFFu;
volatile uint32_t g_woe_prev_hit  = 0xFFFFFFFFu;
volatile uint32_t g_woe_seen_ok;

static void osi_hit(uint32_t i)
{
    {
        uint32_t ps;
        __asm__ volatile ("rsr.ps %0" : "=r"(ps));
        if (ps & (1u << 18)) {
            g_woe_seen_ok++;                 /* WOE still set here */
            g_woe_prev_hit = i;              /* last entry known good */
        } else if (g_woe_lost_at == 0xFFFFFFFFu) {
            g_woe_lost_at = i;               /* first entry seen with WOE clear */
            g_woe_lost_ps = ps;
        }
    }

    if (i >= OSI_N) { return; }
    if (g_osi_calls[i] == 0u && g_osi_seq < 255u) { g_osi_order[i] = ++g_osi_seq; }
    if (g_osi_calls[i] < 0xFFFFu) { g_osi_calls[i]++; }
    osi_trace(i, 0u);
}

static bool osi_s_env_is_chip(void)
{
    osi_hit(1u);
    return false;
}

static void osi_s_set_intr(int32_t cpu_no, uint32_t intr_source, uint32_t intr_num, int32_t intr_prio)
{
    osi_hit(2u);
    /* CLAMP THE PRIORITY, and this is load-bearing rather than tidy.
     *
     * The blob's *iram sections -- 53.7 KB of .wifi0iram, .iram1, .phyiram and
     * friends -- are in FLASH here, not RAM. ESP-IDF keeps them in RAM because
     * it leaves interrupts ENABLED during flash writes, so its WiFi ISRs must
     * stay executable with the cache off.
     *
     * nat-os made the opposite trade: flash_erase_sector() holds crit_enter()
     * across the whole erase, so nothing at level <= CRIT_LEVEL runs and
     * nothing fetches. Flash placement is therefore safe -- but ONLY for
     * interrupts the critical section actually masks.
     *
     * An interrupt allocated above CRIT_LEVEL would fire mid-erase and fetch
     * from a flash chip that cannot answer. Clamping here closes that, and
     * costs a WiFi ISR up to 125 ms of delay during an erase -- which is the
     * next_moves/04 problem, already answered by write policy rather than by
     * scheduler or placement changes.
     *
     * The alternative is a fourth window: 53.7 KB of real IRAM against 86.4 KB
     * free, so it does fit. Not spent, because masking already covers it. */
    if (intr_prio > 3) {            /* CRIT_LEVEL */
        g_osi_intr_clamped++;
        intr_prio = 3;
    }
    (void)cpu_no; (void)intr_source; (void)intr_num; (void)intr_prio;
}

static void osi_s_clear_intr(uint32_t intr_source, uint32_t intr_num)
{
    osi_hit(3u);
}

static void osi_s_set_isr(int32_t n, void *f, void *arg)
{
    osi_hit(4u);
}

static void osi_s_ints_on(uint32_t mask)
{
    osi_hit(5u);
}

static void osi_s_ints_off(uint32_t mask)
{
    osi_hit(6u);
}

static bool osi_s_is_from_isr(void)
{
    osi_hit(7u);
    return false;
}

static void * osi_s_spin_lock_create(void)
{
    osi_hit(8u);
    /* The blob only needs an opaque non-NULL handle: it passes this back
     * to _wifi_int_disable/_wifi_int_restore, which mask interrupts
     * globally rather than per-lock. One word is enough to be a handle,
     * and allocating it keeps delete symmetric. */
    return (void *)w2c_call1((uint32_t)&osi_impl_malloc, 4u);
}

static void osi_s_spin_lock_delete(void *lock)
{
    osi_hit(9u);
    (void)w2c_call1((uint32_t)&osi_impl_free, (uint32_t)lock);
}

static uint32_t osi_s_wifi_int_disable(void *wifi_int_mux)
{
    osi_hit(10u);
    /* Interrupts off, returning the previous level. Done inline rather
     * than through a bridge, exactly as phy_host.c does -- a bridge here
     * would itself be interruptible. */
    (void)wifi_int_mux;
    uint32_t ps;
    __asm__ volatile ("rsil %0, 3" : "=r"(ps));
    return ps;
}

static void osi_s_wifi_int_restore(void *wifi_int_mux, uint32_t tmp)
{
    osi_hit(11u);
    (void)wifi_int_mux;

    /* ONLY the interrupt level, which is what IDF's counterpart does.
     *
     * portEXIT_CRITICAL_NESTED reaches XTOS_RESTORE_JUST_INTLEVEL -- the name
     * is the specification. Writing the whole word into PS instead trusts the
     * driver to hand back a well-formed PS, and nothing makes that true: a
     * stale or differently-shaped value clears PS.WOE, after which EVERY
     * windowed instruction raises IllegalInstruction.
     *
     * Measured. The fault landed inside win_spill_all -- a routine the
     * standalone probe proves correct, spilling 7 frames to 1 -- with
     * ps 0x00030210: WOE clear. The spill was never wrong; it was being run in
     * a processor state where it could not be right.
     *
     * Reading PS and merging preserves WOE, UM, CALLINC and OWB, which belong
     * to the kernel's execution mode and were never the driver's to set. */
    uint32_t ps;
    __asm__ volatile ("rsr.ps %0" : "=r"(ps));
    ps = (ps & ~0xFu) | (tmp & 0xFu);
    __asm__ volatile ("wsr.ps %0; rsync" :: "r"(ps));
}

static void osi_s_task_yield_from_isr(void)
{
    osi_hit(12u);
}

static void * osi_s_semphr_create(uint32_t max, uint32_t init)
{
    osi_hit(13u);
    /* nat-os's semaphores already back the mutex entries; these are the same
     * primitive exposed under its own name. */
    return (void *)w2c_call2((uint32_t)&osi_impl_sem_create,
                             (uint32_t)max, (uint32_t)init);
}

static void osi_s_semphr_delete(void *semphr)
{
    osi_hit(14u);
    (void)w2c_call1((uint32_t)&osi_impl_sem_delete, (uint32_t)semphr);
}

static int32_t osi_s_semphr_take(void *semphr, uint32_t block_time_tick)
{
    osi_hit(15u);
    /* Uncontended first, without blocking. This is the common case and it
     * must stay cheap -- no spill, no lock traffic. */
    if (w2c_call2((uint32_t)&osi_impl_sem_take, (uint32_t)semphr, 0u)) {
        return 1;
    }

    /* Contended: we are about to block INSIDE windowed blob code, which is
     * exactly the state nat-os cannot preserve across a context switch.
     *
     * So stop being in that state. Spilling pushes every live frame out to
     * this task's own stack and leaves one -- the call0 steady state the
     * existing switch already handles. The blob lock is then released so
     * another context may enter windowed code while we wait.
     *
     * On the way back the frames reload through _WindowUnderflow* as we
     * return, which is where the hardware put them.
     *
     * Spilling here works because this is TASK context. Every attempt to do it
     * inside _handler_level3 failed; see next_moves/08 steps 14-18. */
    if (g_stub_ps_pre_spill == 0xFFFFFFFFu) {
        uint32_t ps;
        __asm__ volatile ("rsr.ps %0" : "=r"(ps));
        g_stub_ps_pre_spill = ps;
    }
    {
        uint32_t sp;
        __asm__ volatile ("mov %0, a1" : "=r"(sp));
        g_stub_sp_pre_spill = sp;
        if (sp < g_stub_sp_min) { g_stub_sp_min = sp; }
    }
    blk_sample(0u); uf_sample(1u); of_sample(1u);
    win_spill_all();
    blk_sample(1u); uf_sample(2u); of_sample(2u);
    /* Through the bridge: blob_lock/blob_unlock are call0 kernel functions and
     * this file is windowed. Calling them directly rotates the window and
     * their RET does not rotate back -- which is exactly what happened, an
     * IllegalInstruction at epc 0x80247feb, bit 31 set, a return encoding
     * jumped to raw. Same fault window.S records from the first time. */
    (void)w2c_call0f((uint32_t)&blob_unlock);
    int32_t r = (int32_t)w2c_call2((uint32_t)&osi_impl_sem_take,
                                   (uint32_t)semphr, (uint32_t)block_time_tick);
    blk_sample(2u);
    (void)w2c_call0f((uint32_t)&blob_lock);
    return r;
}

static int32_t osi_s_semphr_give(void *semphr)
{
    osi_hit(16u);
    return (int32_t)w2c_call1((uint32_t)&osi_impl_sem_give, (uint32_t)semphr);
}

static void * osi_s_wifi_thread_semphr_get(void)
{
    osi_hit(17u);
    return 0;
}

static void * osi_s_mutex_create(void)
{
    osi_hit(18u);
    return (void *)w2c_call2((uint32_t)&osi_impl_sem_create, 1u, 1u);
}

static void * osi_s_recursive_mutex_create(void)
{
    osi_hit(19u);
    /* nat-os's mutex is recursive already -- [6b] verifies depth. A
     * binary semaphore stands in, which is what the older table did. */
    return (void *)w2c_call2((uint32_t)&osi_impl_sem_create, 1u, 1u);
}

static void osi_s_mutex_delete(void *mutex)
{
    osi_hit(20u);
    (void)w2c_call1((uint32_t)&osi_impl_sem_delete, (uint32_t)mutex);
}

static int32_t osi_s_mutex_lock(void *mutex)
{
    osi_hit(21u);
    /* Try without blocking. */
    { uint32_t r = w2c_call2((uint32_t)&osi_impl_sem_take, (uint32_t)mutex, 0u); if (r) { return (int32_t)r; } }
    /* About to block inside windowed code. Spill first so this task is left
     * with exactly ONE live frame -- the state the ordinary context switch
     * already handles -- then unpin and release so another context may run. */
    if (g_stub_ps_pre_spill == 0xFFFFFFFFu) {
        uint32_t ps;
        __asm__ volatile ("rsr.ps %0" : "=r"(ps));
        g_stub_ps_pre_spill = ps;
    }
    {
        uint32_t sp;
        __asm__ volatile ("mov %0, a1" : "=r"(sp));
        g_stub_sp_pre_spill = sp;
        if (sp < g_stub_sp_min) { g_stub_sp_min = sp; }
    }
    blk_sample(0u); uf_sample(1u); of_sample(1u);
    win_spill_all();
    blk_sample(1u); uf_sample(2u); of_sample(2u);
    (void)w2c_call0f((uint32_t)&blob_unlock);
    uint32_t r2 = w2c_call2((uint32_t)&osi_impl_sem_take, (uint32_t)mutex, 0xFFFFFFFFu);
    blk_sample(2u);
    (void)w2c_call0f((uint32_t)&blob_lock);
    return (int32_t)r2;
}

static int32_t osi_s_mutex_unlock(void *mutex)
{
    osi_hit(22u);
    return (int32_t)w2c_call1((uint32_t)&osi_impl_sem_give, (uint32_t)mutex);
}

static void * osi_s_queue_create(uint32_t queue_len, uint32_t item_size)
{
    osi_hit(23u);
    return 0;
}

static void osi_s_queue_delete(void *queue)
{
    osi_hit(24u);
}

static int32_t osi_s_queue_send(void *queue, void *item, uint32_t block_time_tick)
{
    osi_hit(25u);
    /* Try without blocking. */
    { uint32_t r = w2c_call3((uint32_t)&osi_impl_queue_send, (uint32_t)queue, (uint32_t)item, 0u); if (r) { return (int32_t)r; } }
    /* About to block inside windowed code. Spill first so this task is left
     * with exactly ONE live frame -- the state the ordinary context switch
     * already handles -- then unpin and release so another context may run. */
    if (g_stub_ps_pre_spill == 0xFFFFFFFFu) {
        uint32_t ps;
        __asm__ volatile ("rsr.ps %0" : "=r"(ps));
        g_stub_ps_pre_spill = ps;
    }
    {
        uint32_t sp;
        __asm__ volatile ("mov %0, a1" : "=r"(sp));
        g_stub_sp_pre_spill = sp;
        if (sp < g_stub_sp_min) { g_stub_sp_min = sp; }
    }
    blk_sample(0u); uf_sample(1u); of_sample(1u);
    win_spill_all();
    blk_sample(1u); uf_sample(2u); of_sample(2u);
    (void)w2c_call0f((uint32_t)&blob_unlock);
    uint32_t r2 = w2c_call3((uint32_t)&osi_impl_queue_send, (uint32_t)queue, (uint32_t)item, (uint32_t)block_time_tick);
    blk_sample(2u);
    (void)w2c_call0f((uint32_t)&blob_lock);
    return (int32_t)r2;
}

static int32_t osi_s_queue_send_from_isr(void *queue, void *item, void *hptw)
{
    osi_hit(26u);
    return 0;
}

static int32_t osi_s_queue_send_to_back(void *queue, void *item, uint32_t block_time_tick)
{
    osi_hit(27u);
    return 0;
}

static int32_t osi_s_queue_send_to_front(void *queue, void *item, uint32_t block_time_tick)
{
    osi_hit(28u);
    return 0;
}

static int32_t osi_s_queue_recv(void *queue, void *item, uint32_t block_time_tick)
{
    osi_hit(29u);
    /* [step 99] Who in the blob calls us, and where does it expect to return?
     *
     * a0 here is the windowed return encoding the blob supplied. Masking to 30
     * bits and OR-ing the PC region gives the actual blob address, which names
     * the calling function in the image -- the starting point step 98 asked
     * for. Latched once; every later call would overwrite it. */
    if (!g_qr_caller) {
        uint32_t a0;
        __asm__ volatile ("mov %0, a0" : "=r"(a0));
        g_qr_caller_raw = a0;
        g_qr_caller = 0x40000000u | (a0 & 0x3FFFFFFFu);
    }
    /* Same shape as _semphr_take: try without blocking, and only if that
     * fails spill the window, unpin and release so another context -- notably
     * the blob's own task -- can run while we wait. Without this the pinned
     * caller waits for a task the pin itself is preventing from running. */
    if (w2c_call3((uint32_t)&osi_impl_queue_recv, (uint32_t)queue, (uint32_t)item, 0u)) {
        return 1;
    }
    if (g_stub_ps_pre_spill == 0xFFFFFFFFu) {
        uint32_t ps;
        __asm__ volatile ("rsr.ps %0" : "=r"(ps));
        g_stub_ps_pre_spill = ps;
    }
    {
        uint32_t sp;
        __asm__ volatile ("mov %0, a1" : "=r"(sp));
        g_stub_sp_pre_spill = sp;
        if (sp < g_stub_sp_min) { g_stub_sp_min = sp; }
    }
    blk_sample(0u); uf_sample(1u); of_sample(1u);
    win_spill_all();
    blk_sample(1u); uf_sample(2u); of_sample(2u);
    (void)w2c_call0f((uint32_t)&blob_unlock);
    int32_t r = (int32_t)w2c_call3((uint32_t)&osi_impl_queue_recv, (uint32_t)queue,
                                   (uint32_t)item, (uint32_t)block_time_tick);
    blk_sample(2u);
    (void)w2c_call0f((uint32_t)&blob_lock);
    return r;
}

static uint32_t osi_s_queue_msg_waiting(void *queue)
{
    osi_hit(30u);
    return 0;
}

static void * osi_s_event_group_create(void)
{
    osi_hit(31u);
    return 0;
}

static void osi_s_event_group_delete(void *event)
{
    osi_hit(32u);
}

static uint32_t osi_s_event_group_set_bits(void *event, uint32_t bits)
{
    osi_hit(33u);
    return 0;
}

static uint32_t osi_s_event_group_clear_bits(void *event, uint32_t bits)
{
    osi_hit(34u);
    return 0;
}

static uint32_t osi_s_event_group_wait_bits(void *event, uint32_t bits_to_wait_for, int clear_on_exit, int wait_for_all_bits, uint32_t block_time_tick)
{
    osi_hit(35u);
    /* Try without blocking. */
    { uint32_t r = 0u; if (r) { return (uint32_t)r; } }
    /* About to block inside windowed code. Spill first so this task is left
     * with exactly ONE live frame -- the state the ordinary context switch
     * already handles -- then unpin and release so another context may run. */
    if (g_stub_ps_pre_spill == 0xFFFFFFFFu) {
        uint32_t ps;
        __asm__ volatile ("rsr.ps %0" : "=r"(ps));
        g_stub_ps_pre_spill = ps;
    }
    {
        uint32_t sp;
        __asm__ volatile ("mov %0, a1" : "=r"(sp));
        g_stub_sp_pre_spill = sp;
        if (sp < g_stub_sp_min) { g_stub_sp_min = sp; }
    }
    blk_sample(0u); uf_sample(1u); of_sample(1u);
    win_spill_all();
    blk_sample(1u); uf_sample(2u); of_sample(2u);
    (void)w2c_call0f((uint32_t)&blob_unlock);
    uint32_t r2 = w2c_call3((uint32_t)&osi_impl_evt_wait, (uint32_t)event, (uint32_t)bits_to_wait_for, (uint32_t)block_time_tick);
    blk_sample(2u);
    (void)w2c_call0f((uint32_t)&blob_lock);
    return (uint32_t)r2;
}

static int32_t osi_s_task_create_pinned_to_core(void *task_func, const char *name, uint32_t stack_depth, void *param, uint32_t prio, void *task_handle, uint32_t core_id)
{
    osi_hit(36u);
    /* Record the requested stack, then hand off. The w2c bridges carry at
     * most three arguments and this has seven, so the request travels as a
     * struct on this (windowed) stack -- same address space, so the call0 side
     * can read it directly. */
    g_osi_trace_arg[g_osi_trace_n ? g_osi_trace_n - 1u : 0u] = stack_depth;
    (void)core_id;

    struct { uint32_t fn, arg, prio, handle, stack_bytes; } req;
    req.fn          = (uint32_t)task_func;
    req.arg         = (uint32_t)param;
    req.prio        = prio;
    req.handle      = (uint32_t)task_handle;
    req.stack_bytes = stack_depth;

    return (int32_t)w2c_call2((uint32_t)&blob_task_create,
                              (uint32_t)&req, (uint32_t)name);
}

static int32_t osi_s_task_create(void *task_func, const char *name, uint32_t stack_depth, void *param, uint32_t prio, void *task_handle)
{
    osi_hit(37u);
    return 0;
}

static void osi_s_task_delete(void *task_handle)
{
    osi_hit(38u);
}

static void osi_s_task_delay(uint32_t tick)
{
    osi_hit(39u);
}

static int32_t osi_s_task_ms_to_tick(uint32_t ms)
{
    osi_hit(40u);
    return 0;
}

static void * osi_s_task_get_current_task(void)
{
    osi_hit(41u);
    /* nat-os task ids are small ints; the blob only ever compares this
     * handle for equality, so the id+1 doubles as a non-NULL handle. */
    return (void *)(w2c_call0f((uint32_t)&task_current) + 1u);
}

static int32_t osi_s_task_get_max_priority(void)
{
    osi_hit(42u);
    /* What ESP-IDF returns: configMAX_PRIORITIES. The blob was compiled
     * against that scale and derives its task priorities from it, so
     * answering with nat-os's own three levels would have it asking for
     * numbers that mean something different. The mapping down to
     * TASK_PRIO_LOW/NORMAL/HIGH belongs in task creation, where the number is
     * actually used -- not here, where it would silently rescale a constant
     * the blob also uses for arithmetic. */
    return 25;
}

static void * osi_s_malloc(size_t size)
{
    osi_hit(43u);
    g_osi_trace_arg[g_osi_trace_n ? g_osi_trace_n - 1u : 0u] = (uint32_t)size;
    return (void *)w2c_call1((uint32_t)&osi_impl_malloc, (uint32_t)size);
}

static void osi_s_free(void *p)
{
    osi_hit(44u);
    (void)w2c_call1((uint32_t)&osi_impl_free, (uint32_t)p);
}

static int32_t osi_s_event_post(const char* event_base, int32_t event_id, void* event_data, size_t event_data_size, uint32_t ticks_to_wait)
{
    osi_hit(45u);
    return 0;
}

static uint32_t osi_s_get_free_heap_size(void)
{
    osi_hit(46u);
    return 0;
}

static uint32_t osi_s_rand(void)
{
    osi_hit(47u);
    return 0;
}

static void osi_s_dport_access_stall_other_cpu_start_wrap(void)
{
    osi_hit(48u);
}

static void osi_s_dport_access_stall_other_cpu_end_wrap(void)
{
    osi_hit(49u);
}

static void osi_s_wifi_apb80m_request(void)
{
    osi_hit(50u);
}

static void osi_s_wifi_apb80m_release(void)
{
    osi_hit(51u);
}

static void osi_s_phy_disable(void)
{
    osi_hit(52u);
}

static void osi_s_phy_enable(void)
{
    osi_hit(53u);
}

static int osi_s_phy_update_country_info(const char* country)
{
    osi_hit(54u);
    return 0;
}

static int osi_s_read_mac(uint8_t* mac, unsigned int type)
{
    osi_hit(55u);
    return 0;
}

static void osi_s_timer_arm(void *timer, uint32_t tmout, bool repeat)
{
    osi_hit(56u);
}

static void osi_s_timer_disarm(void *timer)
{
    osi_hit(57u);
}

static void osi_s_timer_done(void *ptimer)
{
    osi_hit(58u);
}

static void osi_s_timer_setfn(void *ptimer, void *pfunction, void *parg)
{
    osi_hit(59u);
}

static void osi_s_timer_arm_us(void *ptimer, uint32_t us, bool repeat)
{
    osi_hit(60u);
}

static void osi_s_wifi_reset_mac(void)
{
    osi_hit(61u);
}

static void osi_s_wifi_clock_enable(void)
{
    osi_hit(62u);
}

static void osi_s_wifi_clock_disable(void)
{
    osi_hit(63u);
}

static void osi_s_wifi_rtc_enable_iso(void)
{
    osi_hit(64u);
}

static void osi_s_wifi_rtc_disable_iso(void)
{
    osi_hit(65u);
}

static int64_t osi_s_esp_timer_get_time(void)
{
    osi_hit(66u);
    return 0;
}

static int osi_s_nvs_set_i8(uint32_t handle, const char* key, int8_t value)
{
    osi_hit(67u);
    return 0;
}

static int osi_s_nvs_get_i8(uint32_t handle, const char* key, int8_t* out_value)
{
    osi_hit(68u);
    return 0;
}

static int osi_s_nvs_set_u8(uint32_t handle, const char* key, uint8_t value)
{
    osi_hit(69u);
    return 0;
}

static int osi_s_nvs_get_u8(uint32_t handle, const char* key, uint8_t* out_value)
{
    osi_hit(70u);
    return 0;
}

static int osi_s_nvs_set_u16(uint32_t handle, const char* key, uint16_t value)
{
    osi_hit(71u);
    return 0;
}

static int osi_s_nvs_get_u16(uint32_t handle, const char* key, uint16_t* out_value)
{
    osi_hit(72u);
    return 0;
}

static int osi_s_nvs_open(const char* name, unsigned int open_mode, uint32_t *out_handle)
{
    osi_hit(73u);
    return 0;
}

static void osi_s_nvs_close(uint32_t handle)
{
    osi_hit(74u);
}

static int osi_s_nvs_commit(uint32_t handle)
{
    osi_hit(75u);
    return 0;
}

static int osi_s_nvs_set_blob(uint32_t handle, const char* key, const void* value, size_t length)
{
    osi_hit(76u);
    return 0;
}

static int osi_s_nvs_get_blob(uint32_t handle, const char* key, void* out_value, size_t* length)
{
    osi_hit(77u);
    return 0;
}

static int osi_s_nvs_erase_key(uint32_t handle, const char* key)
{
    osi_hit(78u);
    return 0;
}

static int osi_s_get_random(uint8_t *buf, size_t len)
{
    osi_hit(79u);
    return 0;
}

static int osi_s_get_time(void *t)
{
    osi_hit(80u);
    return 0;
}

static unsigned long osi_s_random(void)
{
    osi_hit(81u);
    return 0;
}

static void osi_s_log_write(unsigned int level, const char* tag, const char* format, ...)
{
    osi_hit(82u);
}

static void osi_s_log_writev(unsigned int level, const char* tag, const char* format, va_list args)
{
    osi_hit(83u);
}

static uint32_t osi_s_log_timestamp(void)
{
    osi_hit(84u);
    return 0;
}

static void * osi_s_malloc_internal(size_t size)
{
    osi_hit(85u);
    g_osi_trace_arg[g_osi_trace_n ? g_osi_trace_n - 1u : 0u] = (uint32_t)size;
    return (void *)w2c_call1((uint32_t)&osi_impl_malloc, (uint32_t)size);
}

static void * osi_s_realloc_internal(void *ptr, size_t size)
{
    osi_hit(86u);
    return 0;
}

static void * osi_s_calloc_internal(size_t n, size_t size)
{
    osi_hit(87u);
    g_osi_trace_arg[g_osi_trace_n ? g_osi_trace_n - 1u : 0u] = (uint32_t)(n * size);
    return (void *)w2c_call2((uint32_t)&osi_impl_calloc,
                             (uint32_t)n, (uint32_t)size);
}

static void * osi_s_zalloc_internal(size_t size)
{
    osi_hit(88u);
    return (void *)w2c_call2((uint32_t)&osi_impl_calloc, 1u, (uint32_t)size);
}

static void * osi_s_wifi_malloc(size_t size)
{
    osi_hit(89u);
    return 0;
}

static void * osi_s_wifi_realloc(void *ptr, size_t size)
{
    osi_hit(90u);
    return 0;
}

static void * osi_s_wifi_calloc(size_t n, size_t size)
{
    osi_hit(91u);
    return 0;
}

static void * osi_s_wifi_zalloc(size_t size)
{
    osi_hit(92u);
    return 0;
}

static void * osi_s_wifi_create_queue(int queue_len, int item_size)
{
    osi_hit(93u);
    /* ESP-IDF hands back a wifi_static_queue_t { handle; storage; } -- 8
     * bytes -- not a bare queue handle. The blob then uses ->handle for the
     * queue operations itself, so the wrapper only has to exist and be the
     * right shape. storage stays NULL: nat-os's queues allocate their own. */
    /* Record what was ASKED for. osi_impl_queue_create refuses anything over
     * OSI_QUEUE_BYTES, so a NULL return is ambiguous between "pool exhausted"
     * and "too big" until the request itself is visible. */
    g_osi_trace_arg[g_osi_trace_n ? g_osi_trace_n - 1u : 0u] =
        ((uint32_t)queue_len << 16) | ((uint32_t)item_size & 0xFFFFu);

    uint32_t *q = (uint32_t *)w2c_call1((uint32_t)&osi_impl_malloc, 8u);
    if (!q) { return 0; }
    q[0] = w2c_call2((uint32_t)&osi_impl_queue_create,
                     (uint32_t)queue_len, (uint32_t)item_size);
    q[1] = 0u;
    if (!q[0]) {
        (void)w2c_call1((uint32_t)&osi_impl_free, (uint32_t)q);
        return 0;
    }
    return (void *)q;
}

static void osi_s_wifi_delete_queue(void * queue)
{
    osi_hit(94u);
    uint32_t *q = (uint32_t *)queue;
    if (!q) { return; }
    if (q[0]) { (void)w2c_call1((uint32_t)&osi_impl_queue_delete, q[0]); }
    (void)w2c_call1((uint32_t)&osi_impl_free, (uint32_t)q);
}

static int osi_s_coex_init(void)
{
    osi_hit(95u);
    return 0;
}

static void osi_s_coex_deinit(void)
{
    osi_hit(96u);
}

static int osi_s_coex_enable(void)
{
    osi_hit(97u);
    return 0;
}

static void osi_s_coex_disable(void)
{
    osi_hit(98u);
}

static uint32_t osi_s_coex_status_get(void)
{
    osi_hit(99u);
    return 0;
}

static void osi_s_coex_condition_set(uint32_t type, bool dissatisfy)
{
    osi_hit(100u);
}

static int osi_s_coex_wifi_request(uint32_t event, uint32_t latency, uint32_t duration)
{
    osi_hit(101u);
    return 0;
}

static int osi_s_coex_wifi_release(uint32_t event)
{
    osi_hit(102u);
    return 0;
}

static int osi_s_coex_wifi_channel_set(uint8_t primary, uint8_t secondary)
{
    osi_hit(103u);
    return 0;
}

static int osi_s_coex_event_duration_get(uint32_t event, uint32_t *duration)
{
    osi_hit(104u);
    return 0;
}

static int osi_s_coex_pti_get(uint32_t event, uint8_t *pti)
{
    osi_hit(105u);
    return 0;
}

static void osi_s_coex_schm_status_bit_clear(uint32_t type, uint32_t status)
{
    osi_hit(106u);
}

static void osi_s_coex_schm_status_bit_set(uint32_t type, uint32_t status)
{
    osi_hit(107u);
}

static int osi_s_coex_schm_interval_set(uint32_t interval)
{
    osi_hit(108u);
    return 0;
}

static uint32_t osi_s_coex_schm_interval_get(void)
{
    osi_hit(109u);
    return 0;
}

static uint8_t osi_s_coex_schm_curr_period_get(void)
{
    osi_hit(110u);
    return 0;
}

static void * osi_s_coex_schm_curr_phase_get(void)
{
    osi_hit(111u);
    return 0;
}

static int osi_s_coex_schm_process_restart(void)
{
    osi_hit(112u);
    return 0;
}

static int osi_s_coex_schm_register_cb(int a0, int (* cb)(int))
{
    osi_hit(113u);
    return 0;
}

static int osi_s_coex_register_start_cb(int (* cb)(void))
{
    osi_hit(114u);
    return 0;
}

static int osi_s_coex_schm_flexible_period_set(uint8_t a0)
{
    osi_hit(115u);
    return 0;
}

static uint8_t osi_s_coex_schm_flexible_period_get(void)
{
    osi_hit(116u);
    return 0;
}

const wifi_osi_funcs_t g_osi = {
    ._version = ESP_WIFI_OS_ADAPTER_VERSION,
    ._env_is_chip = osi_s_env_is_chip,
    ._set_intr = osi_s_set_intr,
    ._clear_intr = osi_s_clear_intr,
    ._set_isr = osi_s_set_isr,
    ._ints_on = osi_s_ints_on,
    ._ints_off = osi_s_ints_off,
    ._is_from_isr = osi_s_is_from_isr,
    ._spin_lock_create = osi_s_spin_lock_create,
    ._spin_lock_delete = osi_s_spin_lock_delete,
    ._wifi_int_disable = osi_s_wifi_int_disable,
    ._wifi_int_restore = osi_s_wifi_int_restore,
    ._task_yield_from_isr = osi_s_task_yield_from_isr,
    ._semphr_create = osi_s_semphr_create,
    ._semphr_delete = osi_s_semphr_delete,
    ._semphr_take = osi_s_semphr_take,
    ._semphr_give = osi_s_semphr_give,
    ._wifi_thread_semphr_get = osi_s_wifi_thread_semphr_get,
    ._mutex_create = osi_s_mutex_create,
    ._recursive_mutex_create = osi_s_recursive_mutex_create,
    ._mutex_delete = osi_s_mutex_delete,
    ._mutex_lock = osi_s_mutex_lock,
    ._mutex_unlock = osi_s_mutex_unlock,
    ._queue_create = osi_s_queue_create,
    ._queue_delete = osi_s_queue_delete,
    ._queue_send = osi_s_queue_send,
    ._queue_send_from_isr = osi_s_queue_send_from_isr,
    ._queue_send_to_back = osi_s_queue_send_to_back,
    ._queue_send_to_front = osi_s_queue_send_to_front,
    ._queue_recv = osi_s_queue_recv,
    ._queue_msg_waiting = osi_s_queue_msg_waiting,
    ._event_group_create = osi_s_event_group_create,
    ._event_group_delete = osi_s_event_group_delete,
    ._event_group_set_bits = osi_s_event_group_set_bits,
    ._event_group_clear_bits = osi_s_event_group_clear_bits,
    ._event_group_wait_bits = osi_s_event_group_wait_bits,
    ._task_create_pinned_to_core = osi_s_task_create_pinned_to_core,
    ._task_create = osi_s_task_create,
    ._task_delete = osi_s_task_delete,
    ._task_delay = osi_s_task_delay,
    ._task_ms_to_tick = osi_s_task_ms_to_tick,
    ._task_get_current_task = osi_s_task_get_current_task,
    ._task_get_max_priority = osi_s_task_get_max_priority,
    ._malloc = osi_s_malloc,
    ._free = osi_s_free,
    ._event_post = osi_s_event_post,
    ._get_free_heap_size = osi_s_get_free_heap_size,
    ._rand = osi_s_rand,
    ._dport_access_stall_other_cpu_start_wrap = osi_s_dport_access_stall_other_cpu_start_wrap,
    ._dport_access_stall_other_cpu_end_wrap = osi_s_dport_access_stall_other_cpu_end_wrap,
    ._wifi_apb80m_request = osi_s_wifi_apb80m_request,
    ._wifi_apb80m_release = osi_s_wifi_apb80m_release,
    ._phy_disable = osi_s_phy_disable,
    ._phy_enable = osi_s_phy_enable,
    ._phy_update_country_info = osi_s_phy_update_country_info,
    ._read_mac = osi_s_read_mac,
    ._timer_arm = osi_s_timer_arm,
    ._timer_disarm = osi_s_timer_disarm,
    ._timer_done = osi_s_timer_done,
    ._timer_setfn = osi_s_timer_setfn,
    ._timer_arm_us = osi_s_timer_arm_us,
    ._wifi_reset_mac = osi_s_wifi_reset_mac,
    ._wifi_clock_enable = osi_s_wifi_clock_enable,
    ._wifi_clock_disable = osi_s_wifi_clock_disable,
    ._wifi_rtc_enable_iso = osi_s_wifi_rtc_enable_iso,
    ._wifi_rtc_disable_iso = osi_s_wifi_rtc_disable_iso,
    ._esp_timer_get_time = osi_s_esp_timer_get_time,
    ._nvs_set_i8 = osi_s_nvs_set_i8,
    ._nvs_get_i8 = osi_s_nvs_get_i8,
    ._nvs_set_u8 = osi_s_nvs_set_u8,
    ._nvs_get_u8 = osi_s_nvs_get_u8,
    ._nvs_set_u16 = osi_s_nvs_set_u16,
    ._nvs_get_u16 = osi_s_nvs_get_u16,
    ._nvs_open = osi_s_nvs_open,
    ._nvs_close = osi_s_nvs_close,
    ._nvs_commit = osi_s_nvs_commit,
    ._nvs_set_blob = osi_s_nvs_set_blob,
    ._nvs_get_blob = osi_s_nvs_get_blob,
    ._nvs_erase_key = osi_s_nvs_erase_key,
    ._get_random = osi_s_get_random,
    ._get_time = osi_s_get_time,
    ._random = osi_s_random,
    ._log_write = osi_s_log_write,
    ._log_writev = osi_s_log_writev,
    ._log_timestamp = osi_s_log_timestamp,
    ._malloc_internal = osi_s_malloc_internal,
    ._realloc_internal = osi_s_realloc_internal,
    ._calloc_internal = osi_s_calloc_internal,
    ._zalloc_internal = osi_s_zalloc_internal,
    ._wifi_malloc = osi_s_wifi_malloc,
    ._wifi_realloc = osi_s_wifi_realloc,
    ._wifi_calloc = osi_s_wifi_calloc,
    ._wifi_zalloc = osi_s_wifi_zalloc,
    ._wifi_create_queue = osi_s_wifi_create_queue,
    ._wifi_delete_queue = osi_s_wifi_delete_queue,
    ._coex_init = osi_s_coex_init,
    ._coex_deinit = osi_s_coex_deinit,
    ._coex_enable = osi_s_coex_enable,
    ._coex_disable = osi_s_coex_disable,
    ._coex_status_get = osi_s_coex_status_get,
    ._coex_condition_set = osi_s_coex_condition_set,
    ._coex_wifi_request = osi_s_coex_wifi_request,
    ._coex_wifi_release = osi_s_coex_wifi_release,
    ._coex_wifi_channel_set = osi_s_coex_wifi_channel_set,
    ._coex_event_duration_get = osi_s_coex_event_duration_get,
    ._coex_pti_get = osi_s_coex_pti_get,
    ._coex_schm_status_bit_clear = osi_s_coex_schm_status_bit_clear,
    ._coex_schm_status_bit_set = osi_s_coex_schm_status_bit_set,
    ._coex_schm_interval_set = osi_s_coex_schm_interval_set,
    ._coex_schm_interval_get = osi_s_coex_schm_interval_get,
    ._coex_schm_curr_period_get = osi_s_coex_schm_curr_period_get,
    ._coex_schm_curr_phase_get = osi_s_coex_schm_curr_phase_get,
    ._coex_schm_process_restart = osi_s_coex_schm_process_restart,
    ._coex_schm_register_cb = osi_s_coex_schm_register_cb,
    ._coex_register_start_cb = osi_s_coex_register_start_cb,
    ._coex_schm_flexible_period_set = osi_s_coex_schm_flexible_period_set,
    ._coex_schm_flexible_period_get = osi_s_coex_schm_flexible_period_get,
    ._magic = ESP_WIFI_OS_ADAPTER_MAGIC,
};