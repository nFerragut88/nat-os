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
/* Macros only -- see the header. No call0 declaration may be pulled in here. */
#include "osi_wait.h"

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

/* [step 185] 120, not 118: the two phy_common_clock stubs take trace ids 117
 * and 118, outside the 1..116 field numbering. nat-os's osi_hit ids were never
 * struct indices (step 179) and 54/55 are already taken by
 * _phy_update_country_info and _read_mac, so appending is the only choice that
 * does not renumber the whole trace. This OSI_N sizes the trace arrays only --
 * wifi_osi_entries() reads a separate one in kernel/wifi_osi_table.h. */
#define OSI_N 120u

uint16_t g_osi_calls[OSI_N];
uint8_t  g_osi_order[OSI_N];
uint8_t  g_osi_seq;
uint32_t g_osi_intr_clamped;   /* interrupts asked for above CRIT_LEVEL */

/* Exact call SEQUENCE, with repeats -- g_osi_order only records the first time
 * each entry is touched, which cannot show a loop or a retry. Paired with the
 * argument where the entry has one worth seeing (allocation sizes). */
#define OSI_TRACE_MAX 48u
static uint32_t g_st_sem_done, g_st_sem_rc, g_st_relocked;
extern volatile int g_pinned;   /* kernel/blobcall.c -- needed by osi_trace */
extern volatile uint32_t g_romcall_prime[];   /* [step 188] window.S */
uint32_t g_rcz_seen, g_rcz_idx, g_rcz_call, g_rcz_who;
uint32_t g_rcz_site;
uint8_t  g_osi_trace[OSI_TRACE_MAX];
uint8_t  g_osi_trace_who[OSI_TRACE_MAX];
uint32_t g_osi_trace_arg[OSI_TRACE_MAX];
uint32_t g_osi_trace_n;

/* [step 188] Latch the FIRST site that sees rom_call4's saved return address
 * already zero. The sites are ordered along osi_s_semphr_take's blocking path,
 * so whichever fires first brackets the write to a single operation. One load
 * of a word whose address the kernel already recorded -- it cannot perturb what
 * it measures. */
/* [step 188] Window state either side of the spill, so "which frames existed"
 * is a measurement rather than a geometry argument. */
uint32_t g_rcz_ws[4], g_rcz_wb[4], g_rcz_sp[4], g_rcz_val[4];
/* [step 188] 16 words of task 5's stack around rom_call4's frame, before and
 * after the spill. Which words the spill actually writes is a measurement; the
 * geometry argument has been wrong twice. Anchored at rom_call4's sp - 16. */
uint32_t g_rcz_dump[2][16];

static void rcz_snap(uint32_t k)
{
    /* Only the FIRST pass through the blocking take. osi_s_semphr_take runs
     * more than once, and a singleton that keeps overwriting itself describes
     * the last call while the latch beside it describes the first -- the trap
     * step 183 caught in a0/sp out, one file over. */
    if (g_rcz_seen) { return; }
    uint32_t ws, wb, sp;
    __asm__ volatile ("rsr.windowstart %0" : "=r"(ws));
    __asm__ volatile ("rsr.windowbase  %0" : "=r"(wb));
    __asm__ volatile ("mov %0, a1" : "=r"(sp));
    if (k < 4u) {
        g_rcz_ws[k] = ws; g_rcz_wb[k] = wb; g_rcz_sp[k] = sp;
        g_rcz_val[k] = g_romcall_prime[2]
                     ? *(volatile uint32_t *)g_romcall_prime[2] : 0xDEADu;
        if (k < 2u && g_romcall_prime[2]) {
            const volatile uint32_t *b =
                (const volatile uint32_t *)(g_romcall_prime[2] - 16u);
            for (uint32_t q = 0; q < 16u; q++) { g_rcz_dump[k][q] = b[q]; }
        }
    }
}

static void rcz_check(uint32_t site)
{
    if (g_rcz_seen || !g_romcall_prime[2]) { return; }
    if (*(volatile uint32_t *)g_romcall_prime[2] == 0u) {
        g_rcz_seen = 1u;
        g_rcz_site = site;
        g_rcz_idx  = g_osi_trace_n;
        g_rcz_who  = (uint32_t)g_pinned;
    }
}

static void osi_trace(uint32_t i, uint32_t arg)
{
    if (g_osi_trace_n < OSI_TRACE_MAX) {
        g_osi_trace[g_osi_trace_n] = (uint8_t)i;
        /* [step 179, Tortoise] WHO made the call. g_pinned is the task that is
         * currently inside the blob and is already in memory here, so this is a
         * store, not a new read on the blocking path. Without it, "the worker
         * waits for itself" and "init waits for the worker" have the same trace. */
        /* [step 188] Bracket the corruption of rom_call4's saved return
         * address. One load of a word the kernel already recorded the address
         * of, latched once -- it cannot perturb what it measures, and it names
         * the interval the write happened in. */
        if (!g_rcz_seen && g_romcall_prime[2]) {
            if (*(volatile uint32_t *)g_romcall_prime[2] == 0u) {
                g_rcz_seen = 1u;
                g_rcz_site = 9u;              /* seen at an adapter call */
                g_rcz_idx  = g_osi_trace_n;   /* zero by this call */
                g_rcz_call = i;               /* which entry that is */
                g_rcz_who  = (uint32_t)g_pinned;
            }
        }
        g_osi_trace_who[g_osi_trace_n] = (uint8_t)(g_pinned < 0 ? 99 : g_pinned);
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
extern void osi_impl_free_heap(void);      /* [step 182] */
extern void osi_impl_queue_waiting(void); /* [step 182] */
extern void osi_impl_thread_sem_get(void); /* [step 182] */
extern void osi_impl_read_mac(void);       /* [step 186] */
extern void osi_impl_random(void);         /* [step 193] */
extern void osi_impl_get_random(void);
extern void osi_impl_timer_arm(void);      /* [step 191] */
extern void osi_impl_timer_arm_us(void);
extern void osi_impl_timer_disarm(void);
extern void osi_impl_timer_done(void);
extern void osi_impl_timer_setfn(void);
extern void osi_impl_free(void);
extern void task_current(void);
extern void osi_impl_queue_create(void);
extern void osi_impl_queue_delete(void);
extern void blob_task_create(void);
/* [step 178] read-only blob-task counters, taken via w2c_call0f. */
extern void blob_task_count(void);
extern void blob_task_stack_short(void);
extern void blob_task_last_prio(void);
extern void blob_task_last_lvl(void);
extern void blob_task_want_stack(void);
extern void blob_lock(void);
extern void blob_unlock(void);
extern void win_spill_all(void);
extern unsigned int osi_windowed_idle(unsigned int depth, unsigned int spin);
extern int  osi_qpoll_w(void *h, void *item, uint32_t *woke);   /* windowed: CALL8, no bridge */
/* [step 179, Tortoise] Who waits, and on what.
 *
 * _queue_recv is the stall. The trace says it is called and never returns, but
 * not by which task nor on which queue -- and "the worker waits for itself" and
 * "the init context waits for the worker" are different bugs with the same
 * trace. Both values are already in registers here; recording them adds no read
 * to the blocking path (step 167's rule). */
static uint32_t g_qrw_n;
static uint32_t g_qrw_task[4];
static uint32_t g_qrw_queue[4];
/* Every queue handle nat-os ever hands the blob, and which call made it. */
static uint32_t g_qmk_n;
static uint32_t g_qmk_h[8];
static uint32_t g_qmk_via[8];   /* 23 = _queue_create, 96 = _wifi_create_queue */
extern uint32_t blob_mutex_owner(void);
extern uint32_t blob_mutex_acq(void);
extern uint32_t blob_mutex_cont(void);
extern uint32_t blob_mutex_err(void);
extern uint32_t blob_mutex_depth(void);
extern uint32_t blob_mutex_waiters(void);
extern uint32_t blob_mutex_granted(void);
extern void uart_put_hex(unsigned int value);
extern uint32_t blob_task_reached(void);
extern uint32_t blob_task_running(void);
extern uint32_t blob_task_returned(void);
extern int      blob_trylock_w(int me);                        /* windowed */
/* [step 180] full-depth release/restore, for the blocking wait only. */
extern uint32_t blob_unlock_all_w(int me);                     /* windowed */
extern int      blob_relock_all_w(int me);                     /* windowed */
extern uint32_t g_blob_relock_skips;
extern uint32_t blob_unlock_w(int me);                         /* windowed */
extern void     blob_wake_waiters(void);                       /* address only */
extern void osi_impl_wake_senders(void);                        /* address only */
extern void blob_trylock(void);        /* address only -- through the bridge */
extern void blob_unlock_only(void);
extern volatile int g_pinned;          /* written DIRECTLY from here; see below */   /* windowed; callable directly from here */
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

/* [step 113] Total blocking budget, so `wifiinit` always returns to the shell.
 *
 * The leaf test removed the fault but init now runs without returning, and the
 * shell task is the one inside the blob -- so no counter can be read afterwards.
 * This bounds the SUM of blocking waits across all recv calls; past it, every
 * blocking recv reports empty immediately. The blob then either finishes init
 * with a return code or reveals an unbounded loop, and either way control comes
 * back and `osiused` can be read. Not a fix -- an instrument. */
/* [step 115] spill-on-preemption outcome, reported from the one place that
 * already prints during a live wifiinit. Data only -- no call0 call. */
extern volatile uint32_t g_pspill_count, g_pspill_bad, g_pspill_worst, g_pspill_post_ws;
/* [step 177] did the interrupts actually get wired, and do they fire? */
extern volatile uint32_t g_blob_isr_calls[32], g_blob_isr_nofn, g_blob_intr_routed;
/* [step 127] the radio's memory demand, read as data -- no call0 call. */
extern uint32_t g_osi_alloc_calls, g_osi_alloc_bytes, g_osi_alloc_max, g_osi_alloc_fails;
extern uint32_t g_osi_free_calls, g_osi_heap_used, g_osi_heap_hw;
extern uint32_t g_osi_heap_largest, g_osi_heap_minfree;
extern void osi_impl_park(int me, uint32_t ticks);
/* [step 177] the interrupt plumbing, call0 side. */
extern void osi_impl_set_isr(int32_t n, void *f, void *arg);
extern void osi_impl_set_intr(uint32_t source, uint32_t num, uint32_t prio);
extern void osi_impl_ints_on(uint32_t mask);
extern void osi_impl_ints_off(uint32_t mask);
extern void uart_puts(const char *s);
extern void uart_put_dec(unsigned int v);
volatile uint32_t g_qr_blk_calls, g_qr_blk_rounds, g_qr_timeouts;
/* Report once the blob has been waiting three full timeouts, measured the same
 * way the timeout is -- by the clock. g_qr_last_rounds records how many rounds
 * the last full wait actually took, which is the number that revealed a round
 * costs ~19 ms rather than the 1.5 ms it was assumed to. */
#define QR_BLK_REPORT_CALLS  3u
volatile uint32_t g_qr_last_rounds;

/* [step 112] The spill probe, pointed at the BLOCKING path.
 *
 * The same walk that step 98 ran on the synthetic chain -- follow the saved a1
 * links upward and check each frame's a0 is a windowed return encoding -- but
 * taken here, immediately after win_spill_all() on the path that actually
 * faults. Step 111 left the faulting save area reading STACK_FILL, i.e. never
 * written, so the question is whether the frame in question is even on the chain
 * the spill covers. Latched on the first blocking wait. */
volatile uint32_t g_qspill_have, g_qspill_walked, g_qspill_bad;
volatile uint32_t g_qspill_bad_a0, g_qspill_bad_at, g_qspill_top;

/* [step 102] The save-area word the underflow later reads, sampled where the
 * spill writes it. {addr, after_spill, latched} -- so "the spill wrote it wrong"
 * and "the spill wrote it right and something changed it" can be told apart. */
volatile uint32_t g_sa_addr, g_sa_after_spill, g_sa_have;

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
    /* [step 177] Now actually routed. cpu_no is dropped: single core, and the
     * app CPU is not started. */
    (void)cpu_no;
    (void)w2c_call3((uint32_t)&osi_impl_set_intr,
                    intr_source, intr_num, (uint32_t)intr_prio);
}

static void osi_s_clear_intr(uint32_t intr_source, uint32_t intr_num)
{
    osi_hit(3u);
}

static void osi_s_set_isr(int32_t n, void *f, void *arg)
{
    osi_hit(4u);
    /* [step 177] Recorded, not installed -- the trampoline reads it when the
     * line fires, so _set_isr and _set_intr may arrive in either order. */
    (void)w2c_call3((uint32_t)&osi_impl_set_isr,
                    (uint32_t)n, (uint32_t)f, (uint32_t)arg);
}

static void osi_s_ints_on(uint32_t mask)
{
    osi_hit(5u);
    (void)w2c_call1((uint32_t)&osi_impl_ints_on, mask);
}

static void osi_s_ints_off(uint32_t mask)
{
    osi_hit(6u);
    (void)w2c_call1((uint32_t)&osi_impl_ints_off, mask);
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
    rcz_snap(0u);
    rcz_check(1u);                  /* [step 188] before the spill */
    blk_sample(0u); uf_sample(1u); of_sample(1u);
    win_spill_all();
    rcz_snap(1u);
    rcz_check(2u);                  /* after win_spill_all */
    blk_sample(1u); uf_sample(2u); of_sample(2u);
    /* Through the bridge: blob_lock/blob_unlock are call0 kernel functions and
     * this file is windowed. Calling them directly rotates the window and
     * their RET does not rotate back -- which is exactly what happened, an
     * IllegalInstruction at epc 0x80247feb, bit 31 set, a return encoding
     * jumped to raw. Same fault window.S records from the first time. */
    (void)w2c_call0f((uint32_t)&blob_unlock);
    rcz_check(3u);                  /* after blob_unlock */
    int32_t r = (int32_t)w2c_call2((uint32_t)&osi_impl_sem_take,
                                   (uint32_t)semphr, (uint32_t)block_time_tick);
    blk_sample(2u);
    rcz_check(4u);                  /* after the block returns */
    /* [step 179, Tortoise] Three points on the blocking take, so "still in the
     * semaphore" and "has the semaphore, waiting for the blob lock back" stop
     * looking the same from outside. */
    g_st_sem_done++;
    g_st_sem_rc = (uint32_t)r;
    (void)w2c_call0f((uint32_t)&blob_lock);
    rcz_check(5u);                  /* after re-taking the blob lock */
    g_st_relocked++;
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
    return (void *)w2c_call0f((uint32_t)&osi_impl_thread_sem_get);
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

    /* [step 106] Complete form: no call0 frame is EVER live across a switch.
     *
     * Step 104 traced the fault to a call0 function blocking while it holds a
     * windowed frame's a1; step 105 moved the queue wait out of call0 and still
     * failed, because two call0 excursions remained -- blob_unlock returning
     * unpinned, and blob_lock reaching mutex_lock, which itself blocks.
     *
     * The invariant this enforces is exact:
     *
     *   every call0 excursion runs PINNED   -> no switch can land in one
     *   every wait runs UNPINNED in WINDOWED frames -> switches land where a1
     *                                                  belongs to a windowed
     *                                                  frame with a save area
     *   pin and unpin are DIRECT STORES from windowed code -> no frame, no
     *                                                          return sequence,
     *                                                          nothing to catch
     *   the lock is acquired by TRY, never by mutex_lock -> no call0 block
     *
     * We are pinned on entry (the blob was called with the lock held), so the
     * task id can be read safely once and reused for the direct stores. */
    if (g_qrw_n < 4u) {
        g_qrw_task[g_qrw_n] = (uint32_t)g_pinned;
        g_qrw_queue[g_qrw_n] = (uint32_t)queue;
        g_qrw_n++;
    }
    int me = g_pinned;
    if (me < 0) {
        /* Not pinned: nothing to protect, so the simple path is correct. */
        return (int32_t)w2c_call3((uint32_t)&osi_impl_queue_recv, (uint32_t)queue,
                                  (uint32_t)item, 0u);
    }

    /* [step 110] fix (2): the poll is WINDOWED and reached by CALL8.
     *
     * This was `w2c_call3(&osi_impl_queue_recv, ...)` -- a bridge that does
     * `entry a1, 32` then `callx0`, so the call0 callee shared this frame's
     * register window and moved its a1. Steps 103 and 109 established that as
     * the cause. Calling a windowed function instead rotates properly: the
     * callee gets its own frame and never touches ours.
     *
     * The wake goes back through a bridge, deliberately -- task_wake() is call0,
     * and this runs pinned, so no switch can land in it. */
    {
        uint32_t woke = 0u;
        if (osi_qpoll_w(queue, item, &woke)) {
            if (woke) { (void)w2c_call1((uint32_t)&osi_impl_wake_senders, (uint32_t)queue); }
            return 1;                   /* uncontended: no spill, no lock traffic */
        }
    }

    g_qr_blk_calls++;

    blk_sample(0u);
    uint32_t rounds = 0u;             /* diagnostics only -- never a bound */
    uint32_t wait_t0;
    __asm__ volatile ("rsr.ccount %0" : "=r"(wait_t0));
    for (;;) {
        /* [step 111] Release through WINDOWED code -- no bridge, no callx0, so
         * nothing moves this frame's a1. The wake it may owe goes back through
         * a bridge below, pinned. */
        {
            uint32_t owed = blob_unlock_all_w(me);   /* [step 180] every level */
            if (owed) { (void)w2c_call1((uint32_t)&blob_wake_waiters, owed); }
        }

        win_spill_all();                /* one live frame before we let go */

        if (!g_qspill_have) {           /* [step 112] walk what the spill wrote */
            uint32_t sp;
            __asm__ volatile ("mov %0, a1" : "=r"(sp));
            g_qspill_have = 1u;
            g_qspill_top  = sp;
            for (uint32_t k = 0; k < 12u; k++) {
                if (sp < 0x3ff00000u || sp >= 0x40000000u) { break; }
                uint32_t fa0 = ((volatile uint32_t *)(sp - 16u))[0];
                uint32_t fa1 = ((volatile uint32_t *)(sp - 12u))[0];
                g_qspill_walked++;
                if ((fa0 >> 30) == 0u) {
                    g_qspill_bad++;
                    if (!g_qspill_bad_at) { g_qspill_bad_a0 = fa0; g_qspill_bad_at = sp; }
                }
                if (fa1 <= sp) { break; }      /* the chain must ascend */
                sp = fa1;
            }
        }

        /* [step 109, Tortoise's H-windowed-reg-loss] Nothing live in a register
         * across the wait.
         *
         * Step 106 found `a6` holding &g_pinned across `call8 osi_windowed_idle`
         * and coming back 0x1000. This re-derives the address afterwards and
         * routes the value through memory, so if the fault is register loss it
         * must move or vanish. Three outcomes discriminate:
         *
         *   gone            -> register preservation across an unpinned call8 is
         *                      the mechanism
         *   moves to the next register used  -> the whole frame is clobbered
         *   reverts to excvaddr 0x170-shaped double fault -> a6 was a symptom and
         *                      w2c_call3's entry/callx0 moving a1 is the cause
         *
         * Volatile forces both to the stack, which the frame carries in memory
         * rather than in the register file. */
        volatile int me_v = me;
        volatile int * volatile pinp = &g_pinned;

        *pinp = -1;                     /* UNPIN */

        *pinp = me_v;
        (void)w2c_call2((uint32_t)&osi_impl_park, (uint32_t)me_v, 1u);

        pinp = &g_pinned;               /* RE-DERIVE, do not trust a register */
        *pinp = me_v;                   /* REPIN before any call0 excursion */

        if (!blob_relock_all_w(me)) {   /* [step 180] restores the depth */
            continue;                   /* someone else holds it; wait again */
        }
        {
            uint32_t woke = 0u;
            if (osi_qpoll_w(queue, item, &woke)) {
                if (woke) { (void)w2c_call1((uint32_t)&osi_impl_wake_senders, (uint32_t)queue); }
                blk_sample(2u);
                return 1;
            }
        }
        g_qr_blk_rounds++;
        if (g_qr_timeouts == QR_BLK_REPORT_CALLS && rounds == 1u) {
            /* [step 113] One-shot liveness report, bridged.
             *
             * The first budget attempt returned 0 immediately once spent, which
             * made the blob's retry a tight loop that never yielded and let the
             * TG0 watchdog reset the chip -- an artifact of the instrument, not
             * a finding. This reports and then keeps waiting normally, so the
             * blob's own pacing is undisturbed.
             *
             * Legal here: the lock is held and we are pinned, so no switch can
             * land inside the call0 excursion. */
            (void)w2c_call1((uint32_t)&uart_puts,
                            (uint32_t)"\n   [qr] budget spent, still waiting  calls=");
            (void)w2c_call1((uint32_t)&uart_put_dec, g_qr_blk_calls);
            (void)w2c_call1((uint32_t)&uart_puts, (uint32_t)" timeouts=");
            (void)w2c_call1((uint32_t)&uart_put_dec, g_qr_timeouts);
            (void)w2c_call1((uint32_t)&uart_puts, (uint32_t)" lastosi=");
            (void)w2c_call1((uint32_t)&uart_put_dec,
                            g_osi_trace_n ? g_osi_trace[g_osi_trace_n - 1u] : 0u);
            (void)w2c_call1((uint32_t)&uart_puts, (uint32_t)" rounds/wait=");
            (void)w2c_call1((uint32_t)&uart_put_dec, g_qr_last_rounds);
            (void)w2c_call1((uint32_t)&uart_puts, (uint32_t)" osin=");
            (void)w2c_call1((uint32_t)&uart_put_dec, g_osi_trace_n);
            (void)w2c_call1((uint32_t)&uart_puts,
                            (uint32_t)"\n   [pspill] sweeps=");
            (void)w2c_call1((uint32_t)&uart_put_dec, g_pspill_count);
            (void)w2c_call1((uint32_t)&uart_puts, (uint32_t)" bad=");
            (void)w2c_call1((uint32_t)&uart_put_dec, g_pspill_bad);
            (void)w2c_call1((uint32_t)&uart_puts, (uint32_t)" worst_frames=");
            (void)w2c_call1((uint32_t)&uart_put_dec, g_pspill_worst);
            (void)w2c_call1((uint32_t)&uart_puts, (uint32_t)" last_post_ws=");
            (void)w2c_call1((uint32_t)&uart_put_dec, g_pspill_post_ws);
            (void)w2c_call1((uint32_t)&uart_puts,
                            (uint32_t)"\n   [intr] routed=");
            (void)w2c_call1((uint32_t)&uart_put_dec, g_blob_intr_routed);
            (void)w2c_call1((uint32_t)&uart_puts, (uint32_t)" nofn=");
            (void)w2c_call1((uint32_t)&uart_put_dec, g_blob_isr_nofn);
            (void)w2c_call1((uint32_t)&uart_puts, (uint32_t)" fired:");
            {
                uint32_t k;
                for (k = 0u; k < 32u; k++) {
                    if (g_blob_isr_calls[k]) {
                        (void)w2c_call1((uint32_t)&uart_puts, (uint32_t)" L");
                        (void)w2c_call1((uint32_t)&uart_put_dec, k);
                        (void)w2c_call1((uint32_t)&uart_puts, (uint32_t)"=");
                        (void)w2c_call1((uint32_t)&uart_put_dec, g_blob_isr_calls[k]);
                    }
                }
            }
            /* [step 177] the OSI call trace, in order. Already recorded by
             * osi_hit(); never printed, because wifiinit does not return and
             * `osiused` cannot be reached. This says what the blob actually
             * asked for before it stalled. */
            (void)w2c_call1((uint32_t)&uart_puts,
                            (uint32_t)"\n   [who] ");
            {
                uint32_t k, n = g_osi_trace_n;
                if (n > 40u) { n = 40u; }
                for (k = 0u; k < n; k++) {
                    (void)w2c_call1((uint32_t)&uart_put_dec, g_osi_trace_who[k]);
                    (void)w2c_call1((uint32_t)&uart_puts, (uint32_t)" ");
                }
            }
            (void)w2c_call1((uint32_t)&uart_puts,
                            (uint32_t)"\n   [osi] ");
            {
                uint32_t k, n = g_osi_trace_n;
                if (n > 40u) { n = 40u; }
                for (k = 0u; k < n; k++) {
                    (void)w2c_call1((uint32_t)&uart_put_dec, g_osi_trace[k]);
                    (void)w2c_call1((uint32_t)&uart_puts, (uint32_t)" ");
                }
            }
            (void)w2c_call1((uint32_t)&uart_puts,
                            (uint32_t)"\n   [bt] created=");
            (void)w2c_call1((uint32_t)&uart_put_dec, w2c_call0f((uint32_t)&blob_task_count));
            (void)w2c_call1((uint32_t)&uart_puts, (uint32_t)" refused=");
            (void)w2c_call1((uint32_t)&uart_put_dec, w2c_call0f((uint32_t)&blob_task_stack_short));
            (void)w2c_call1((uint32_t)&uart_puts, (uint32_t)" prio=");
            (void)w2c_call1((uint32_t)&uart_put_dec, w2c_call0f((uint32_t)&blob_task_last_prio));
            (void)w2c_call1((uint32_t)&uart_puts, (uint32_t)"->lvl");
            (void)w2c_call1((uint32_t)&uart_put_dec, w2c_call0f((uint32_t)&blob_task_last_lvl));
            (void)w2c_call1((uint32_t)&uart_puts, (uint32_t)" want_stack=");
            (void)w2c_call1((uint32_t)&uart_put_dec, w2c_call0f((uint32_t)&blob_task_want_stack));
            /* [step 178] blob-task state. The OSI trace shows call 36
             * (_task_create_pinned_to_core) hit once, but a refused create and
             * a successful one look identical from the trace -- osi_hit() fires
             * before the decision. These counters are already maintained by
             * blobcall.c; nothing new is measured. */
            (void)w2c_call1((uint32_t)&uart_puts,
                            (uint32_t)"\n   [sem] blocking_take done=");
            (void)w2c_call1((uint32_t)&uart_put_dec, g_st_sem_done);
            (void)w2c_call1((uint32_t)&uart_puts, (uint32_t)" rc=");
            (void)w2c_call1((uint32_t)&uart_put_dec, g_st_sem_rc);
            (void)w2c_call1((uint32_t)&uart_puts, (uint32_t)" relocked=");
            (void)w2c_call1((uint32_t)&uart_put_dec, g_st_relocked);
            (void)w2c_call1((uint32_t)&uart_puts,
                            (uint32_t)"\n   [wrk] reached=");
            (void)w2c_call1((uint32_t)&uart_put_dec, w2c_call0f((uint32_t)&blob_task_reached));
            (void)w2c_call1((uint32_t)&uart_puts, (uint32_t)" running=");
            (void)w2c_call1((uint32_t)&uart_put_dec, w2c_call0f((uint32_t)&blob_task_running));
            (void)w2c_call1((uint32_t)&uart_puts, (uint32_t)" returned=");
            (void)w2c_call1((uint32_t)&uart_put_dec, w2c_call0f((uint32_t)&blob_task_returned));
            (void)w2c_call1((uint32_t)&uart_puts, (uint32_t)"  mutex owner=");
            (void)w2c_call1((uint32_t)&uart_put_dec, w2c_call0f((uint32_t)&blob_mutex_owner));
            (void)w2c_call1((uint32_t)&uart_puts, (uint32_t)" acq=");
            (void)w2c_call1((uint32_t)&uart_put_dec, w2c_call0f((uint32_t)&blob_mutex_acq));
            (void)w2c_call1((uint32_t)&uart_puts, (uint32_t)" cont=");
            (void)w2c_call1((uint32_t)&uart_put_dec, w2c_call0f((uint32_t)&blob_mutex_cont));
            (void)w2c_call1((uint32_t)&uart_puts, (uint32_t)" depth=");
            (void)w2c_call1((uint32_t)&uart_put_dec, w2c_call0f((uint32_t)&blob_mutex_depth));
            (void)w2c_call1((uint32_t)&uart_puts, (uint32_t)" waiters=");
            (void)w2c_call1((uint32_t)&uart_put_hex, w2c_call0f((uint32_t)&blob_mutex_waiters));
            (void)w2c_call1((uint32_t)&uart_puts, (uint32_t)" granted=");
            (void)w2c_call1((uint32_t)&uart_put_hex, w2c_call0f((uint32_t)&blob_mutex_granted));
            (void)w2c_call1((uint32_t)&uart_puts, (uint32_t)" err=");
            (void)w2c_call1((uint32_t)&uart_put_dec, w2c_call0f((uint32_t)&blob_mutex_err));
            /* [step 179] made queues, then who waited on what. */
            (void)w2c_call1((uint32_t)&uart_puts, (uint32_t)"\n   [q] made:");
            {   uint32_t k;
                for (k = 0u; k < g_qmk_n; k++) {
                    (void)w2c_call1((uint32_t)&uart_puts, (uint32_t)" via");
                    (void)w2c_call1((uint32_t)&uart_put_dec, g_qmk_via[k]);
                    (void)w2c_call1((uint32_t)&uart_puts, (uint32_t)"=");
                    (void)w2c_call1((uint32_t)&uart_put_hex, g_qmk_h[k]);
                }
                (void)w2c_call1((uint32_t)&uart_puts, (uint32_t)"   recv:");
                for (k = 0u; k < g_qrw_n; k++) {
                    (void)w2c_call1((uint32_t)&uart_puts, (uint32_t)" t");
                    (void)w2c_call1((uint32_t)&uart_put_dec, g_qrw_task[k]);
                    (void)w2c_call1((uint32_t)&uart_puts, (uint32_t)"@");
                    (void)w2c_call1((uint32_t)&uart_put_hex, g_qrw_queue[k]);
                }
            }
            (void)w2c_call1((uint32_t)&uart_puts,
                            (uint32_t)"\n   [mem] alloc calls=");
            (void)w2c_call1((uint32_t)&uart_put_dec, g_osi_alloc_calls);
            (void)w2c_call1((uint32_t)&uart_puts, (uint32_t)" bytes=");
            (void)w2c_call1((uint32_t)&uart_put_dec, g_osi_alloc_bytes);
            (void)w2c_call1((uint32_t)&uart_puts, (uint32_t)" largest_req=");
            (void)w2c_call1((uint32_t)&uart_put_dec, g_osi_alloc_max);
            (void)w2c_call1((uint32_t)&uart_puts, (uint32_t)" fails=");
            (void)w2c_call1((uint32_t)&uart_put_dec, g_osi_alloc_fails);
            (void)w2c_call1((uint32_t)&uart_puts, (uint32_t)" frees=");
            (void)w2c_call1((uint32_t)&uart_put_dec, g_osi_free_calls);
            (void)w2c_call1((uint32_t)&uart_puts,
                            (uint32_t)"\n   [mem] heap used=");
            (void)w2c_call1((uint32_t)&uart_put_dec, g_osi_heap_used);
            (void)w2c_call1((uint32_t)&uart_puts, (uint32_t)" high_water=");
            (void)w2c_call1((uint32_t)&uart_put_dec, g_osi_heap_hw);
            (void)w2c_call1((uint32_t)&uart_puts, (uint32_t)" min_free=");
            (void)w2c_call1((uint32_t)&uart_put_dec, g_osi_heap_minfree);
            (void)w2c_call1((uint32_t)&uart_puts, (uint32_t)" largest_free=");
            (void)w2c_call1((uint32_t)&uart_put_dec, g_osi_heap_largest);
            (void)w2c_call1((uint32_t)&uart_puts, (uint32_t)"\n");
        }
        rounds++;
        {
            uint32_t now;
            __asm__ volatile ("rsr.ccount %0" : "=r"(now));
            if ((now - wait_t0) >= OSI_FOREVER_CAP_CYCLES) {
                blk_sample(2u);
                g_qr_timeouts++;
                g_qr_last_rounds = rounds;
                return 0;
            }
        }
    }
}

static uint32_t osi_s_queue_msg_waiting(void *queue)
{
    osi_hit(30u);
    return w2c_call1((uint32_t)&osi_impl_queue_waiting, (uint32_t)queue);
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
    /* [step 182] Answering 0 collapsed every timeout the blob derived from
     * this to "do not block" -- and it derives the queue_send and semphr_take
     * timeouts on the init path from exactly this call. IDF's wrapper is
     * ms / portTICK_PERIOD_MS; nat-os's tick is OSI_TICK_CYCLES at
     * OSI_CYCLES_PER_MS, which is 10 ms. Round up, so a non-zero request never
     * becomes a zero wait. */
    uint32_t per = OSI_TICK_CYCLES / OSI_CYCLES_PER_MS;      /* 10 ms */
    return (int32_t)((ms + per - 1u) / per);
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
    /* [step 182] Reporting zero free heap tells a driver it has no memory.
     * osi_impl_free_heap() has always existed. */
    return w2c_call0f((uint32_t)&osi_impl_free_heap);
}

static uint32_t osi_s_rand(void)
{
    osi_hit(47u);
    return w2c_call0f((uint32_t)&osi_impl_random);
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

/* [step 185] Five slots the struct declared and the initializer never set.
 *
 * A designated initializer leaves omitted members ZERO, so these were NULL
 * function pointers in a table handed to a driver that calls them without
 * checking. ppTask does exactly that at offset 216 -- see the report -- and
 * jumped to address 0.
 *
 * The two phy_common_clock entries are NO-OPS, not implementations. IDF's
 * wrappers reach wifi_bt_common_module_enable/disable, the shared WiFi/BT
 * peripheral clock. phyinit already runs to completion here (blobphy rc=0), so
 * the clock is on before ppTask ever asks; what was missing was a valid address
 * to call, not the work. Wiring the real clock gating belongs with _wifi_clock_
 * enable/_disable, which are also still empty.
 *
 * These were the only two. A first scan reported five, but it did not honour
 * the preprocessor: _slowclk_cal_get is #if !CONFIG_IDF_TARGET_ESP32 and the
 * two regdma/sleep_retention entries are ESP32-C6 only, so none of the three
 * exists in this build. */
static void osi_s_phy_common_clock_enable(void)
{
    osi_hit(117u);
}

static void osi_s_phy_common_clock_disable(void)
{
    osi_hit(118u);
}

static int osi_s_phy_update_country_info(const char* country)
{
    osi_hit(54u);
    return 0;
}

static int osi_s_read_mac(uint8_t* mac, unsigned int type)
{
    osi_hit(55u);
    /* [step 186] Was `return 0` with the buffer untouched -- ESP_OK for work
     * never done. See osi_impl_read_mac() for the eFuse layout. */
    return (int)w2c_call2((uint32_t)&osi_impl_read_mac,
                          (uint32_t)mac, (uint32_t)type);
}

static void osi_s_timer_arm(void *timer, uint32_t tmout, bool repeat)
{
    osi_hit(56u);
    (void)w2c_call3((uint32_t)&osi_impl_timer_arm, (uint32_t)timer, tmout, (uint32_t)repeat);
}

static void osi_s_timer_disarm(void *timer)
{
    osi_hit(57u);
    (void)w2c_call1((uint32_t)&osi_impl_timer_disarm, (uint32_t)timer);
}

static void osi_s_timer_done(void *ptimer)
{
    osi_hit(58u);
    (void)w2c_call1((uint32_t)&osi_impl_timer_done, (uint32_t)ptimer);
}

static void osi_s_timer_setfn(void *ptimer, void *pfunction, void *parg)
{
    osi_hit(59u);
    (void)w2c_call3((uint32_t)&osi_impl_timer_setfn, (uint32_t)ptimer, (uint32_t)pfunction, (uint32_t)parg);
}

static void osi_s_timer_arm_us(void *ptimer, uint32_t us, bool repeat)
{
    osi_hit(60u);
    (void)w2c_call3((uint32_t)&osi_impl_timer_arm_us, (uint32_t)ptimer, us, (uint32_t)repeat);
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
    return (int)w2c_call2((uint32_t)&osi_impl_get_random, (uint32_t)buf, (uint32_t)len);
}

static int osi_s_get_time(void *t)
{
    osi_hit(80u);
    return 0;
}

static unsigned long osi_s_random(void)
{
    osi_hit(81u);
    return (unsigned long)w2c_call0f((uint32_t)&osi_impl_random);
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

/* [step 182] The WiFi-heap allocators were all NULL, and that is what made
 * esp_wifi_init_internal unwind.
 *
 * The trace showed task 5 calling _wifi_zalloc and then, having been told the
 * allocation failed, tearing the driver back down: delete the semaphore, post a
 * stop to the worker, and the worker deletes its queue and itself. The crash at
 * _task_delete was the last step of a shutdown, not the first step of a bug.
 *
 * ESP-IDF separates these from _malloc_internal only by which heap they draw
 * from -- MALLOC_CAP_INTERNAL either way on this part. nat-os has one heap, so
 * they route to the same place _malloc_internal already used successfully. */
static void * osi_s_wifi_malloc(size_t size)
{
    osi_hit(89u);
    return (void *)w2c_call1((uint32_t)&osi_impl_malloc, (uint32_t)size);
}

static void * osi_s_wifi_realloc(void *ptr, size_t size)
{
    osi_hit(90u);
    return 0;
}

static void * osi_s_wifi_calloc(size_t n, size_t size)
{
    osi_hit(91u);
    return (void *)w2c_call2((uint32_t)&osi_impl_calloc,
                             (uint32_t)n, (uint32_t)size);
}

static void * osi_s_wifi_zalloc(size_t size)
{
    osi_hit(92u);
    /* calloc(1, size) is zalloc; osi_impl_calloc already zeroes. */
    return (void *)w2c_call2((uint32_t)&osi_impl_calloc, 1u, (uint32_t)size);
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
    if (g_qmk_n < 8u) { g_qmk_h[g_qmk_n] = q[0]; g_qmk_via[g_qmk_n] = 96u; g_qmk_n++; }
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
    ._phy_common_clock_enable = osi_s_phy_common_clock_enable,
    ._phy_common_clock_disable = osi_s_phy_common_clock_disable,
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