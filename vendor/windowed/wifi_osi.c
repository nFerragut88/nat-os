/* nat-os - the WiFi OS Interface table. Compiled -mabi=windowed.
 *
 * libpp.a reaches its host through exactly one symbol, g_osi_funcs_p, pointing
 * at a struct of 116 function pointers. Everything the blob needs from an
 * operating system arrives through here: tasks, queues, semaphores, timers,
 * memory, interrupts, NVS.
 *
 * That is the whole remaining contract for MAC hardware init, and its shape
 * decides the work:
 *
 *    34  pure stubs      19 Bluetooth coexistence (no BT on this build),
 *                        12 NVS, 3 logging
 *    50  thin mappings   onto nat-os's tasks, heap, mutexes, interrupt matrix
 *    26  real work       queues, semaphores, event groups, software timers,
 *                        none of which nat-os has today
 *
 * GENERATED SKELETON. Every entry is present and correctly typed; the bodies
 * are stubs. That is deliberate. A table with the right SHAPE links, and
 * linking is what proves the contract is satisfied before any behaviour is
 * written. The bodies then get filled in against a frame that is already known
 * to be correct.
 *
 * The declaration ORDER is load-bearing. The blob indexes this struct by
 * layout, not by name, so one member out of position is a call to the wrong
 * function with the wrong arguments, and nothing in the system would diagnose
 * it. The order here is generated from Espressif's header rather than typed.
 *
 * Windowed, because the blob calls these. vendor/windowed/phy_host.c records
 * what happens when that is got wrong, in both directions.
 */

#include <stdint.h>
#include <stdbool.h>
#include <stdarg.h>
#include <stddef.h>

#define ESP_WIFI_OS_ADAPTER_VERSION  0x00000008
#define ESP_WIFI_OS_ADAPTER_MAGIC    0xDEADBEAF


/* ---- forwarding into the call0 kernel -----------------------------------
 *
 * These bodies do no work. Every one hands off through w2c_callN() in
 * window.S, which establishes a windowed frame, invokes the call0 target under
 * its own convention, and unwinds. The implementations live in
 * kernel/wifi_osi_impl.c where the heap, scheduler and tick are reachable.
 *
 * Taking the ADDRESS of a call0 function from windowed code is safe -- it is
 * just a symbol. CALLING it directly would not be, and is the fault this
 * project has now met from both directions.
 */
extern uint32_t w2c_call0f(uint32_t fn);
extern uint32_t w2c_call1(uint32_t fn, uint32_t a);
extern uint32_t w2c_call2(uint32_t fn, uint32_t a, uint32_t b);
extern uint32_t w2c_call3(uint32_t fn, uint32_t a, uint32_t b, uint32_t c);

extern void osi_impl_sem_create(void);      /* addresses only; see above */
extern void osi_impl_sem_delete(void);
extern void osi_impl_sem_take(void);
extern void osi_impl_sem_give(void);
extern void osi_impl_queue_create(void);
extern void osi_impl_queue_delete(void);
extern void osi_impl_queue_send(void);
extern void osi_impl_queue_recv(void);
extern void osi_impl_queue_waiting(void);
extern void osi_impl_evt_create(void);
extern void osi_impl_evt_delete(void);
extern void osi_impl_evt_set(void);
extern void osi_impl_evt_clear(void);
extern void osi_impl_evt_wait(void);
extern void osi_impl_timer_alloc(void);
extern void osi_impl_timer_setfn(void);
extern void osi_impl_timer_arm(void);
extern void osi_impl_timer_arm_us(void);
extern void osi_impl_timer_disarm(void);
extern void osi_impl_timer_done(void);
extern void osi_impl_malloc(void);
extern void osi_impl_free(void);
extern void osi_impl_calloc(void);
extern void osi_impl_free_heap(void);
extern void osi_impl_random(void);
extern void osi_impl_ms_to_tick(void);
extern void osi_impl_delay(void);
extern void osi_impl_current_task(void);
extern void osi_impl_time_us_lo(void);

/* Instrumentation, for the price of four macros.
 *
 * Every forwarded entry goes through one of these, so recording the ADDRESS of
 * the call0 implementation here gives "what did the driver last ask for"
 * without touching 118 bodies. The address maps back to a symbol with nm.
 *
 * Not static: kernel-side call0 code reads them as data. */
uint32_t g_osi_last;      /* address of the last osi_impl_* forwarded to */
uint32_t g_osi_hits;      /* how many forwarded calls have happened      */

#define OSI_NOTE(f)      (g_osi_last = (uint32_t)&f, g_osi_hits++)
#define FWD0(f)          (OSI_NOTE(f), w2c_call0f((uint32_t)&f))
#define FWD1(f,a)        (OSI_NOTE(f), w2c_call1((uint32_t)&f,(uint32_t)(a)))
#define FWD2(f,a,b)      (OSI_NOTE(f), w2c_call2((uint32_t)&f,(uint32_t)(a),(uint32_t)(b)))
#define FWD3(f,a,b,c)    (OSI_NOTE(f), w2c_call3((uint32_t)&f,(uint32_t)(a),(uint32_t)(b),(uint32_t)(c)))

typedef struct {
    int32_t _version;
    bool (*_env_is_chip)(void);
    void (*_set_intr)(int32_t cpu_no, uint32_t intr_source, uint32_t intr_num, int32_t intr_prio);
    void (*_clear_intr)(uint32_t intr_source, uint32_t intr_num);
    void (*_set_isr)(int32_t n, void *f, void *arg);
    void (*_ints_on)(uint32_t mask);
    void (*_ints_off)(uint32_t mask);
    bool (*_is_from_isr)(void);
    void * (*_spin_lock_create)(void);
    void (*_spin_lock_delete)(void *lock);
    uint32_t (*_wifi_int_disable)(void *wifi_int_mux);
    void (*_wifi_int_restore)(void *wifi_int_mux, uint32_t tmp);
    void (*_task_yield_from_isr)(void);
    void * (*_semphr_create)(uint32_t max, uint32_t init);
    void (*_semphr_delete)(void *semphr);
    int32_t (*_semphr_take)(void *semphr, uint32_t block_time_tick);
    int32_t (*_semphr_give)(void *semphr);
    void * (*_wifi_thread_semphr_get)(void);
    void * (*_mutex_create)(void);
    void * (*_recursive_mutex_create)(void);
    void (*_mutex_delete)(void *mutex);
    int32_t (*_mutex_lock)(void *mutex);
    int32_t (*_mutex_unlock)(void *mutex);
    void * (*_queue_create)(uint32_t queue_len, uint32_t item_size);
    void (*_queue_delete)(void *queue);
    int32_t (*_queue_send)(void *queue, void *item, uint32_t block_time_tick);
    int32_t (*_queue_send_from_isr)(void *queue, void *item, void *hptw);
    int32_t (*_queue_send_to_back)(void *queue, void *item, uint32_t block_time_tick);
    int32_t (*_queue_send_to_front)(void *queue, void *item, uint32_t block_time_tick);
    int32_t (*_queue_recv)(void *queue, void *item, uint32_t block_time_tick);
    uint32_t (*_queue_msg_waiting)(void *queue);
    void * (*_event_group_create)(void);
    void (*_event_group_delete)(void *event);
    uint32_t (*_event_group_set_bits)(void *event, uint32_t bits);
    uint32_t (*_event_group_clear_bits)(void *event, uint32_t bits);
    uint32_t (*_event_group_wait_bits)(void *event, uint32_t bits_to_wait_for, int clear_on_exit, int wait_for_all_bits, uint32_t block_time_tick);
    int32_t (*_task_create_pinned_to_core)(void *task_func, const char *name, uint32_t stack_depth, void *param, uint32_t prio, void *task_handle, uint32_t core_id);
    int32_t (*_task_create)(void *task_func, const char *name, uint32_t stack_depth, void *param, uint32_t prio, void *task_handle);
    void (*_task_delete)(void *task_handle);
    void (*_task_delay)(uint32_t tick);
    int32_t (*_task_ms_to_tick)(uint32_t ms);
    void * (*_task_get_current_task)(void);
    int32_t (*_task_get_max_priority)(void);
    void * (*_malloc)(unsigned int size);
    void (*_free)(void *p);
    int32_t (*_event_post)(const char* event_base, int32_t event_id, void* event_data, size_t event_data_size, uint32_t ticks_to_wait);
    uint32_t (*_get_free_heap_size)(void);
    uint32_t (*_rand)(void);
    void (*_dport_access_stall_other_cpu_start_wrap)(void);
    void (*_dport_access_stall_other_cpu_end_wrap)(void);
    void (*_wifi_apb80m_request)(void);
    void (*_wifi_apb80m_release)(void);
    void (*_phy_disable)(void);
    void (*_phy_enable)(void);
    void (*_phy_common_clock_enable)(void);
    void (*_phy_common_clock_disable)(void);
    int (*_phy_update_country_info)(const char* country);
    int (*_read_mac)(uint8_t* mac, uint32_t type);
    void (*_timer_arm)(void *timer, uint32_t tmout, bool repeat);
    void (*_timer_disarm)(void *timer);
    void (*_timer_done)(void *ptimer);
    void (*_timer_setfn)(void *ptimer, void *pfunction, void *parg);
    void (*_timer_arm_us)(void *ptimer, uint32_t us, bool repeat);
    void (*_wifi_reset_mac)(void);
    void (*_wifi_clock_enable)(void);
    void (*_wifi_clock_disable)(void);
    void (*_wifi_rtc_enable_iso)(void);
    void (*_wifi_rtc_disable_iso)(void);
    int64_t (*_esp_timer_get_time)(void);
    int (*_nvs_set_i8)(uint32_t handle, const char* key, int8_t value);
    int (*_nvs_get_i8)(uint32_t handle, const char* key, int8_t* out_value);
    int (*_nvs_set_u8)(uint32_t handle, const char* key, uint8_t value);
    int (*_nvs_get_u8)(uint32_t handle, const char* key, uint8_t* out_value);
    int (*_nvs_set_u16)(uint32_t handle, const char* key, uint16_t value);
    int (*_nvs_get_u16)(uint32_t handle, const char* key, uint16_t* out_value);
    int (*_nvs_open)(const char* name, uint32_t open_mode, uint32_t *out_handle);
    void (*_nvs_close)(uint32_t handle);
    int (*_nvs_commit)(uint32_t handle);
    int (*_nvs_set_blob)(uint32_t handle, const char* key, const void* value, size_t length);
    int (*_nvs_get_blob)(uint32_t handle, const char* key, void* out_value, size_t* length);
    int (*_nvs_erase_key)(uint32_t handle, const char* key);
    int (*_get_random)(uint8_t *buf, size_t len);
    int (*_get_time)(void *t);
    unsigned long (*_random)(void);
    uint32_t (*_slowclk_cal_get)(void);
    void (*_log_write)(uint32_t level, const char* tag, const char* format, ...);
    void (*_log_writev)(uint32_t level, const char* tag, const char* format, va_list args);
    uint32_t (*_log_timestamp)(void);
    void * (*_malloc_internal)(size_t size);
    void * (*_realloc_internal)(void *ptr, size_t size);
    void * (*_calloc_internal)(size_t n, size_t size);
    void * (*_zalloc_internal)(size_t size);
    void * (*_wifi_malloc)(size_t size);
    void * (*_wifi_realloc)(void *ptr, size_t size);
    void * (*_wifi_calloc)(size_t n, size_t size);
    void * (*_wifi_zalloc)(size_t size);
    void * (*_wifi_create_queue)(int queue_len, int item_size);
    void (*_wifi_delete_queue)(void * queue);
    int (*_coex_init)(void);
    void (*_coex_deinit)(void);
    int (*_coex_enable)(void);
    void (*_coex_disable)(void);
    uint32_t (*_coex_status_get)(void);
    void (*_coex_condition_set)(uint32_t type, bool dissatisfy);
    int (*_coex_wifi_request)(uint32_t event, uint32_t latency, uint32_t duration);
    int (*_coex_wifi_release)(uint32_t event);
    int (*_coex_wifi_channel_set)(uint8_t primary, uint8_t secondary);
    int (*_coex_event_duration_get)(uint32_t event, uint32_t *duration);
    int (*_coex_pti_get)(uint32_t event, uint8_t *pti);
    void (*_coex_schm_status_bit_clear)(uint32_t type, uint32_t status);
    void (*_coex_schm_status_bit_set)(uint32_t type, uint32_t status);
    int (*_coex_schm_interval_set)(uint32_t interval);
    uint32_t (*_coex_schm_interval_get)(void);
    uint8_t (*_coex_schm_curr_period_get)(void);
    void * (*_coex_schm_curr_phase_get)(void);
    int (*_coex_schm_curr_phase_idx_set)(int idx);
    int (*_coex_schm_curr_phase_idx_get)(void);
    int32_t _magic;
} wifi_osi_funcs_t;

static bool osi_env_is_chip(void)
{
    return false;
}

static void osi_set_intr(int32_t cpu_no, uint32_t intr_source, uint32_t intr_num, int32_t intr_prio)
{
    (void)cpu_no;
    (void)intr_source;
    (void)intr_num;
    (void)intr_prio;
    /* stub */
}

static void osi_clear_intr(uint32_t intr_source, uint32_t intr_num)
{
    (void)intr_source;
    (void)intr_num;
    /* stub */
}

static void osi_set_isr(int32_t n, void *f, void *arg)
{
    (void)n;
    (void)f;
    (void)arg;
    /* stub */
}

static void osi_ints_on(uint32_t mask)
{
    (void)mask;
    /* stub */
}

static void osi_ints_off(uint32_t mask)
{
    (void)mask;
    /* stub */
}

static bool osi_is_from_isr(void)
{
    return false;
}

static void * osi_spin_lock_create(void)
{
    return 0;
}

static void osi_spin_lock_delete(void *lock)
{
    (void)lock;
    /* stub */
}

static uint32_t osi_wifi_int_disable(void *wifi_int_mux)
{
    (void)wifi_int_mux;
    return 0;
}

static void osi_wifi_int_restore(void *wifi_int_mux, uint32_t tmp)
{
    (void)wifi_int_mux;
    (void)tmp;
    /* stub */
}

static void osi_task_yield_from_isr(void)
{
    /* stub */
}

static void * osi_semphr_create(uint32_t max, uint32_t init)
{
    return (void *)FWD2(osi_impl_sem_create, max, init);
}

static void osi_semphr_delete(void *semphr)
{
    FWD1(osi_impl_sem_delete, semphr);
}

static int32_t osi_semphr_take(void *semphr, uint32_t block_time_tick)
{
    return (int32_t)FWD2(osi_impl_sem_take, semphr, block_time_tick);
}

static int32_t osi_semphr_give(void *semphr)
{
    return (int32_t)FWD1(osi_impl_sem_give, semphr);
}

static void * osi_wifi_thread_semphr_get(void)
{
    return 0;
}

static void * osi_mutex_create(void)
{
    return (void *)FWD2(osi_impl_sem_create, 1u, 1u);
}

static void * osi_recursive_mutex_create(void)
{
    return (void *)FWD2(osi_impl_sem_create, 1u, 1u);
}

static void osi_mutex_delete(void *mutex)
{
    FWD1(osi_impl_sem_delete, mutex);
}

static int32_t osi_mutex_lock(void *mutex)
{
    return (int32_t)FWD2(osi_impl_sem_take, mutex, 0xFFFFFFFFu);
}

static int32_t osi_mutex_unlock(void *mutex)
{
    return (int32_t)FWD1(osi_impl_sem_give, mutex);
}

static void * osi_queue_create(uint32_t queue_len, uint32_t item_size)
{
    return (void *)FWD2(osi_impl_queue_create, queue_len, item_size);
}

static void osi_queue_delete(void *queue)
{
    FWD1(osi_impl_queue_delete, queue);
}

static int32_t osi_queue_send(void *queue, void *item, uint32_t block_time_tick)
{
    return (int32_t)FWD3(osi_impl_queue_send, queue, item, block_time_tick);
}

static int32_t osi_queue_send_from_isr(void *queue, void *item, void *hptw)
{
    (void)queue;
    (void)item;
    (void)hptw;
    return 0;
}

static int32_t osi_queue_send_to_back(void *queue, void *item, uint32_t block_time_tick)
{
    return (int32_t)FWD3(osi_impl_queue_send, queue, item, block_time_tick);
}

static int32_t osi_queue_send_to_front(void *queue, void *item, uint32_t block_time_tick)
{
    (void)queue;
    (void)item;
    (void)block_time_tick;
    return 0;
}

static int32_t osi_queue_recv(void *queue, void *item, uint32_t block_time_tick)
{
    return (int32_t)FWD3(osi_impl_queue_recv, queue, item, block_time_tick);
}

static uint32_t osi_queue_msg_waiting(void *queue)
{
    return FWD1(osi_impl_queue_waiting, queue);
}

static void * osi_event_group_create(void)
{
    return (void *)FWD0(osi_impl_evt_create);
}

static void osi_event_group_delete(void *event)
{
    FWD1(osi_impl_evt_delete, event);
}

static uint32_t osi_event_group_set_bits(void *event, uint32_t bits)
{
    return FWD2(osi_impl_evt_set, event, bits);
}

static uint32_t osi_event_group_clear_bits(void *event, uint32_t bits)
{
    return FWD2(osi_impl_evt_clear, event, bits);
}

static uint32_t osi_event_group_wait_bits(void *event, uint32_t bits_to_wait_for, int clear_on_exit, int wait_for_all_bits, uint32_t block_time_tick)
{
    (void)event;
    (void)bits_to_wait_for;
    (void)clear_on_exit;
    (void)wait_for_all_bits;
    (void)block_time_tick;
    return 0;
}

static int32_t osi_task_create_pinned_to_core(void *task_func, const char *name, uint32_t stack_depth, void *param, uint32_t prio, void *task_handle, uint32_t core_id)
{
    (void)task_func;
    (void)name;
    (void)stack_depth;
    (void)param;
    (void)prio;
    (void)task_handle;
    (void)core_id;
    return 0;
}

static int32_t osi_task_create(void *task_func, const char *name, uint32_t stack_depth, void *param, uint32_t prio, void *task_handle)
{
    (void)task_func;
    (void)name;
    (void)stack_depth;
    (void)param;
    (void)prio;
    (void)task_handle;
    return 0;
}

static void osi_task_delete(void *task_handle)
{
    (void)task_handle;
    /* stub */
}

static void osi_task_delay(uint32_t tick)
{
    FWD1(osi_impl_delay, tick);
}

static int32_t osi_task_ms_to_tick(uint32_t ms)
{
    return (int32_t)FWD1(osi_impl_ms_to_tick, ms);
}

static void * osi_task_get_current_task(void)
{
    return (void *)FWD0(osi_impl_current_task);
}

static int32_t osi_task_get_max_priority(void)
{
    return 0;
}

static void * osi_malloc(unsigned int size)
{
    return (void *)FWD1(osi_impl_malloc, size);
}

static void osi_free(void *p)
{
    FWD1(osi_impl_free, p);
}

static int32_t osi_event_post(const char* event_base, int32_t event_id, void* event_data, size_t event_data_size, uint32_t ticks_to_wait)
{
    (void)event_base;
    (void)event_id;
    (void)event_data;
    (void)event_data_size;
    (void)ticks_to_wait;
    return 0;
}

static uint32_t osi_get_free_heap_size(void)
{
    return FWD0(osi_impl_free_heap);
}

static uint32_t osi_rand(void)
{
    return FWD0(osi_impl_random);
}

static void osi_dport_access_stall_other_cpu_start_wrap(void)
{
    /* stub */
}

static void osi_dport_access_stall_other_cpu_end_wrap(void)
{
    /* stub */
}

static void osi_wifi_apb80m_request(void)
{
    /* stub */
}

static void osi_wifi_apb80m_release(void)
{
    /* stub */
}

static void osi_phy_disable(void)
{
    /* stub */
}

static void osi_phy_enable(void)
{
    /* stub */
}

static void osi_phy_common_clock_enable(void)
{
    /* stub */
}

static void osi_phy_common_clock_disable(void)
{
    /* stub */
}

static int osi_phy_update_country_info(const char* country)
{
    (void)country;
    return 0;
}

static int osi_read_mac(uint8_t* mac, uint32_t type)
{
    (void)mac;
    (void)type;
    return 0;
}

static void osi_timer_arm(void *timer, uint32_t tmout, bool repeat)
{
    FWD3(osi_impl_timer_arm, timer, tmout, repeat);
}

static void osi_timer_disarm(void *timer)
{
    FWD1(osi_impl_timer_disarm, timer);
}

static void osi_timer_done(void *ptimer)
{
    FWD1(osi_impl_timer_done, ptimer);
}

static void osi_timer_setfn(void *ptimer, void *pfunction, void *parg)
{
    FWD3(osi_impl_timer_setfn, ptimer, pfunction, parg);
}

static void osi_timer_arm_us(void *ptimer, uint32_t us, bool repeat)
{
    FWD3(osi_impl_timer_arm_us, ptimer, us, repeat);
}

static void osi_wifi_reset_mac(void)
{
    /* stub */
}

static void osi_wifi_clock_enable(void)
{
    /* stub */
}

static void osi_wifi_clock_disable(void)
{
    /* stub */
}

static void osi_wifi_rtc_enable_iso(void)
{
    /* stub */
}

static void osi_wifi_rtc_disable_iso(void)
{
    /* stub */
}

static int64_t osi_esp_timer_get_time(void)
{
    return (int64_t)FWD0(osi_impl_time_us_lo);
}

static int osi_nvs_set_i8(uint32_t handle, const char* key, int8_t value)
{
    (void)handle;
    (void)key;
    (void)value;
    return 0;
}

static int osi_nvs_get_i8(uint32_t handle, const char* key, int8_t* out_value)
{
    (void)handle;
    (void)key;
    (void)out_value;
    return 0;
}

static int osi_nvs_set_u8(uint32_t handle, const char* key, uint8_t value)
{
    (void)handle;
    (void)key;
    (void)value;
    return 0;
}

static int osi_nvs_get_u8(uint32_t handle, const char* key, uint8_t* out_value)
{
    (void)handle;
    (void)key;
    (void)out_value;
    return 0;
}

static int osi_nvs_set_u16(uint32_t handle, const char* key, uint16_t value)
{
    (void)handle;
    (void)key;
    (void)value;
    return 0;
}

static int osi_nvs_get_u16(uint32_t handle, const char* key, uint16_t* out_value)
{
    (void)handle;
    (void)key;
    (void)out_value;
    return 0;
}

static int osi_nvs_open(const char* name, uint32_t open_mode, uint32_t *out_handle)
{
    (void)name;
    (void)open_mode;
    (void)out_handle;
    return 0;
}

static void osi_nvs_close(uint32_t handle)
{
    (void)handle;
    /* stub */
}

static int osi_nvs_commit(uint32_t handle)
{
    (void)handle;
    return 0;
}

static int osi_nvs_set_blob(uint32_t handle, const char* key, const void* value, size_t length)
{
    (void)handle;
    (void)key;
    (void)value;
    (void)length;
    return 0;
}

static int osi_nvs_get_blob(uint32_t handle, const char* key, void* out_value, size_t* length)
{
    (void)handle;
    (void)key;
    (void)out_value;
    (void)length;
    return 0;
}

static int osi_nvs_erase_key(uint32_t handle, const char* key)
{
    (void)handle;
    (void)key;
    return 0;
}

static int osi_get_random(uint8_t *buf, size_t len)
{
    (void)buf;
    (void)len;
    return 0;
}

static int osi_get_time(void *t)
{
    (void)t;
    return 0;
}

static unsigned long osi_random(void)
{
    return FWD0(osi_impl_random);
}

static uint32_t osi_slowclk_cal_get(void)
{
    return 0;
}

static void osi_log_write(uint32_t level, const char* tag, const char* format, ...)
{
    (void)level; (void)tag; (void)format;
    /* stub */
}

static void osi_log_writev(uint32_t level, const char* tag, const char* format, va_list args)
{
    (void)level;
    (void)tag;
    (void)format;
    (void)args;
    /* stub */
}

static uint32_t osi_log_timestamp(void)
{
    return 0;
}

static void * osi_malloc_internal(size_t size)
{
    return (void *)FWD1(osi_impl_malloc, size);
}

static void * osi_realloc_internal(void *ptr, size_t size)
{
    (void)ptr;
    (void)size;
    return 0;
}

static void * osi_calloc_internal(size_t n, size_t size)
{
    return (void *)FWD2(osi_impl_calloc, n, size);
}

static void * osi_zalloc_internal(size_t size)
{
    return (void *)FWD2(osi_impl_calloc, 1u, size);
}

static void * osi_wifi_malloc(size_t size)
{
    return (void *)FWD1(osi_impl_malloc, size);
}

static void * osi_wifi_realloc(void *ptr, size_t size)
{
    (void)ptr;
    (void)size;
    return 0;
}

static void * osi_wifi_calloc(size_t n, size_t size)
{
    return (void *)FWD2(osi_impl_calloc, n, size);
}

static void * osi_wifi_zalloc(size_t size)
{
    return (void *)FWD2(osi_impl_calloc, 1u, size);
}

static void * osi_wifi_create_queue(int queue_len, int item_size)
{
    (void)queue_len;
    (void)item_size;
    return 0;
}

static void osi_wifi_delete_queue(void * queue)
{
    (void)queue;
    /* stub */
}

static int osi_coex_init(void)
{
    return 0;
}

static void osi_coex_deinit(void)
{
    /* stub */
}

static int osi_coex_enable(void)
{
    return 0;
}

static void osi_coex_disable(void)
{
    /* stub */
}

static uint32_t osi_coex_status_get(void)
{
    return 0;
}

static void osi_coex_condition_set(uint32_t type, bool dissatisfy)
{
    (void)type;
    (void)dissatisfy;
    /* stub */
}

static int osi_coex_wifi_request(uint32_t event, uint32_t latency, uint32_t duration)
{
    (void)event;
    (void)latency;
    (void)duration;
    return 0;
}

static int osi_coex_wifi_release(uint32_t event)
{
    (void)event;
    return 0;
}

static int osi_coex_wifi_channel_set(uint8_t primary, uint8_t secondary)
{
    (void)primary;
    (void)secondary;
    return 0;
}

static int osi_coex_event_duration_get(uint32_t event, uint32_t *duration)
{
    (void)event;
    (void)duration;
    return 0;
}

static int osi_coex_pti_get(uint32_t event, uint8_t *pti)
{
    (void)event;
    (void)pti;
    return 0;
}

static void osi_coex_schm_status_bit_clear(uint32_t type, uint32_t status)
{
    (void)type;
    (void)status;
    /* stub */
}

static void osi_coex_schm_status_bit_set(uint32_t type, uint32_t status)
{
    (void)type;
    (void)status;
    /* stub */
}

static int osi_coex_schm_interval_set(uint32_t interval)
{
    (void)interval;
    return 0;
}

static uint32_t osi_coex_schm_interval_get(void)
{
    return 0;
}

static uint8_t osi_coex_schm_curr_period_get(void)
{
    return 0;
}

static void * osi_coex_schm_curr_phase_get(void)
{
    return 0;
}

static int osi_coex_schm_curr_phase_idx_set(int idx)
{
    (void)idx;
    return 0;
}

static int osi_coex_schm_curr_phase_idx_get(void)
{
    return 0;
}


/* Espressif's declaration order, generated from their header. */
wifi_osi_funcs_t g_wifi_osi_funcs = {
    ._version = ESP_WIFI_OS_ADAPTER_VERSION,
    ._env_is_chip = osi_env_is_chip,
    ._set_intr = osi_set_intr,
    ._clear_intr = osi_clear_intr,
    ._set_isr = osi_set_isr,
    ._ints_on = osi_ints_on,
    ._ints_off = osi_ints_off,
    ._is_from_isr = osi_is_from_isr,
    ._spin_lock_create = osi_spin_lock_create,
    ._spin_lock_delete = osi_spin_lock_delete,
    ._wifi_int_disable = osi_wifi_int_disable,
    ._wifi_int_restore = osi_wifi_int_restore,
    ._task_yield_from_isr = osi_task_yield_from_isr,
    ._semphr_create = osi_semphr_create,
    ._semphr_delete = osi_semphr_delete,
    ._semphr_take = osi_semphr_take,
    ._semphr_give = osi_semphr_give,
    ._wifi_thread_semphr_get = osi_wifi_thread_semphr_get,
    ._mutex_create = osi_mutex_create,
    ._recursive_mutex_create = osi_recursive_mutex_create,
    ._mutex_delete = osi_mutex_delete,
    ._mutex_lock = osi_mutex_lock,
    ._mutex_unlock = osi_mutex_unlock,
    ._queue_create = osi_queue_create,
    ._queue_delete = osi_queue_delete,
    ._queue_send = osi_queue_send,
    ._queue_send_from_isr = osi_queue_send_from_isr,
    ._queue_send_to_back = osi_queue_send_to_back,
    ._queue_send_to_front = osi_queue_send_to_front,
    ._queue_recv = osi_queue_recv,
    ._queue_msg_waiting = osi_queue_msg_waiting,
    ._event_group_create = osi_event_group_create,
    ._event_group_delete = osi_event_group_delete,
    ._event_group_set_bits = osi_event_group_set_bits,
    ._event_group_clear_bits = osi_event_group_clear_bits,
    ._event_group_wait_bits = osi_event_group_wait_bits,
    ._task_create_pinned_to_core = osi_task_create_pinned_to_core,
    ._task_create = osi_task_create,
    ._task_delete = osi_task_delete,
    ._task_delay = osi_task_delay,
    ._task_ms_to_tick = osi_task_ms_to_tick,
    ._task_get_current_task = osi_task_get_current_task,
    ._task_get_max_priority = osi_task_get_max_priority,
    ._malloc = osi_malloc,
    ._free = osi_free,
    ._event_post = osi_event_post,
    ._get_free_heap_size = osi_get_free_heap_size,
    ._rand = osi_rand,
    ._dport_access_stall_other_cpu_start_wrap = osi_dport_access_stall_other_cpu_start_wrap,
    ._dport_access_stall_other_cpu_end_wrap = osi_dport_access_stall_other_cpu_end_wrap,
    ._wifi_apb80m_request = osi_wifi_apb80m_request,
    ._wifi_apb80m_release = osi_wifi_apb80m_release,
    ._phy_disable = osi_phy_disable,
    ._phy_enable = osi_phy_enable,
    ._phy_common_clock_enable = osi_phy_common_clock_enable,
    ._phy_common_clock_disable = osi_phy_common_clock_disable,
    ._phy_update_country_info = osi_phy_update_country_info,
    ._read_mac = osi_read_mac,
    ._timer_arm = osi_timer_arm,
    ._timer_disarm = osi_timer_disarm,
    ._timer_done = osi_timer_done,
    ._timer_setfn = osi_timer_setfn,
    ._timer_arm_us = osi_timer_arm_us,
    ._wifi_reset_mac = osi_wifi_reset_mac,
    ._wifi_clock_enable = osi_wifi_clock_enable,
    ._wifi_clock_disable = osi_wifi_clock_disable,
    ._wifi_rtc_enable_iso = osi_wifi_rtc_enable_iso,
    ._wifi_rtc_disable_iso = osi_wifi_rtc_disable_iso,
    ._esp_timer_get_time = osi_esp_timer_get_time,
    ._nvs_set_i8 = osi_nvs_set_i8,
    ._nvs_get_i8 = osi_nvs_get_i8,
    ._nvs_set_u8 = osi_nvs_set_u8,
    ._nvs_get_u8 = osi_nvs_get_u8,
    ._nvs_set_u16 = osi_nvs_set_u16,
    ._nvs_get_u16 = osi_nvs_get_u16,
    ._nvs_open = osi_nvs_open,
    ._nvs_close = osi_nvs_close,
    ._nvs_commit = osi_nvs_commit,
    ._nvs_set_blob = osi_nvs_set_blob,
    ._nvs_get_blob = osi_nvs_get_blob,
    ._nvs_erase_key = osi_nvs_erase_key,
    ._get_random = osi_get_random,
    ._get_time = osi_get_time,
    ._random = osi_random,
    ._slowclk_cal_get = osi_slowclk_cal_get,
    ._log_write = osi_log_write,
    ._log_writev = osi_log_writev,
    ._log_timestamp = osi_log_timestamp,
    ._malloc_internal = osi_malloc_internal,
    ._realloc_internal = osi_realloc_internal,
    ._calloc_internal = osi_calloc_internal,
    ._zalloc_internal = osi_zalloc_internal,
    ._wifi_malloc = osi_wifi_malloc,
    ._wifi_realloc = osi_wifi_realloc,
    ._wifi_calloc = osi_wifi_calloc,
    ._wifi_zalloc = osi_wifi_zalloc,
    ._wifi_create_queue = osi_wifi_create_queue,
    ._wifi_delete_queue = osi_wifi_delete_queue,
    ._coex_init = osi_coex_init,
    ._coex_deinit = osi_coex_deinit,
    ._coex_enable = osi_coex_enable,
    ._coex_disable = osi_coex_disable,
    ._coex_status_get = osi_coex_status_get,
    ._coex_condition_set = osi_coex_condition_set,
    ._coex_wifi_request = osi_coex_wifi_request,
    ._coex_wifi_release = osi_coex_wifi_release,
    ._coex_wifi_channel_set = osi_coex_wifi_channel_set,
    ._coex_event_duration_get = osi_coex_event_duration_get,
    ._coex_pti_get = osi_coex_pti_get,
    ._coex_schm_status_bit_clear = osi_coex_schm_status_bit_clear,
    ._coex_schm_status_bit_set = osi_coex_schm_status_bit_set,
    ._coex_schm_interval_set = osi_coex_schm_interval_set,
    ._coex_schm_interval_get = osi_coex_schm_interval_get,
    ._coex_schm_curr_period_get = osi_coex_schm_curr_period_get,
    ._coex_schm_curr_phase_get = osi_coex_schm_curr_phase_get,
    ._coex_schm_curr_phase_idx_set = osi_coex_schm_curr_phase_idx_set,
    ._coex_schm_curr_phase_idx_get = osi_coex_schm_curr_phase_idx_get,
    ._magic = ESP_WIFI_OS_ADAPTER_MAGIC,
};

wifi_osi_funcs_t *g_osi_funcs_p = &g_wifi_osi_funcs;

/* ---- self-test ----------------------------------------------------------
 *
 * WINDOWED, and reached from the call0 shell through rom_call3(). It has to
 * live on this side: it exercises the vtable the way libpp will, by calling
 * through the function pointers rather than the static functions, so the whole
 * path -- pointer, windowed prologue, w2c bridge, call0 body, unwind -- is
 * what gets tested. A call0 test could only reach the implementations, which
 * is the half that was never in doubt.
 *
 * Returns a bitmask of PASSED checks so a partial result is still readable;
 * 0x3F is everything.
 */
uint32_t osi_selftest(void)
{
    const wifi_osi_funcs_t *f = g_osi_funcs_p;
    uint32_t pass = 0;

    void *m = f->_malloc(64);
    if (m) { pass |= 0x01; f->_free(m); }

    void *s = f->_semphr_create(1, 0);
    if (s) {
        /* give then take must succeed immediately; take on an empty count with
         * a zero timeout must fail. A semaphore that always succeeds would
         * satisfy the first check alone. */
        if (f->_semphr_give(s) && f->_semphr_take(s, 0) && !f->_semphr_take(s, 0)) {
            pass |= 0x02;
        }
        f->_semphr_delete(s);
    }

    void *q = f->_queue_create(4, 4);
    if (q) {
        uint32_t in = 0xA5A51234u, out = 0;
        if (f->_queue_send(q, &in, 0) && f->_queue_msg_waiting(q) == 1 &&
            f->_queue_recv(q, &out, 0) && out == in) {
            pass |= 0x04;
        }
        f->_queue_delete(q);
    }

    void *e = f->_event_group_create();
    if (e) {
        if (f->_event_group_set_bits(e, 0x0Fu) == 0x0Fu &&
            f->_event_group_clear_bits(e, 0x0Au) == 0x05u) {
            pass |= 0x08;
        }
        f->_event_group_delete(e);
    }

    if (f->_get_free_heap_size() > 1024u) { pass |= 0x10; }

    /* Two draws differing proves the generator advances rather than returning
     * a constant, which a stub would also do. */
    if (f->_random() != f->_random()) { pass |= 0x20; }

    return pass;
}
