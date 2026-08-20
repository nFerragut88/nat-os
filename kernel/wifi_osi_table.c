/* nat-os -- the ESP-IDF WiFi OS adapter table.  GENERATED -- do not hand-edit.
 *
 * next_moves/08. The blob calls out through this table for every OS service it
 * needs. esp_wifi_80211_tx faulted reaching offset 0x54 -- _mutex_lock --
 * through a null one.
 *
 * ---- why every entry starts as an instrumented stub ---------------------
 *
 * nat-os has 24 primitives against 118 slots. Guessing which a bring-up needs
 * would be guessing, and this project has a specific history with stubs: one
 * that quietly returns success is indistinguishable from a working
 * implementation until the radio silently does nothing.
 *
 * So every entry RECORDS that it was called before returning. `osiused` then
 * reports which entries the driver reached, in what order and how often --
 * turning "what does it need?" from an argument into a measurement. Entries
 * get real bodies as the evidence demands them, not before.
 *
 * ---- order is the only thing that matters -------------------------------
 *
 * The blob indexes this by BYTE OFFSET. A misordered table calls the wrong
 * function through the right slot, which is the least debuggable failure
 * available -- so this is generated from esp_private/wifi_os_adapter.h rather
 * than typed.
 *
 * That header is #if'd per target, and it matters: on ESP32
 * _phy_common_clock_enable/_disable ARE members, and two other entries are
 * NOT. A first attempt ignored the conditionals and produced a table with the
 * wrong shape. CONFIG_IDF_TARGET_ESP32 is therefore defined in the header
 * before the struct is included -- the precompiled blob was built for ESP32
 * and the layouts must agree.
 */

#include <stdint.h>
#include <stdbool.h>
#include "uart.h"
#include "wifi_osi_table.h"

#define OSI_N 118u

static uint16_t g_calls[OSI_N];
static uint8_t  g_order[OSI_N];
static uint8_t  g_seq;

static void osi_hit(uint32_t i)
{
    if (i >= OSI_N) { return; }
    if (g_calls[i] == 0u && g_seq < 255u) { g_order[i] = ++g_seq; }
    if (g_calls[i] < 0xFFFFu) { g_calls[i]++; }
}

static bool osi_s_env_is_chip(void)
{
    osi_hit(1u);
    return false;
}

static void osi_s_set_intr(int32_t cpu_no, uint32_t intr_source, uint32_t intr_num, int32_t intr_prio)
{
    osi_hit(2u);
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
    return 0;
}

static void osi_s_spin_lock_delete(void *lock)
{
    osi_hit(9u);
}

static uint32_t osi_s_wifi_int_disable(void *wifi_int_mux)
{
    osi_hit(10u);
    return 0;
}

static void osi_s_wifi_int_restore(void *wifi_int_mux, uint32_t tmp)
{
    osi_hit(11u);
}

static void osi_s_task_yield_from_isr(void)
{
    osi_hit(12u);
}

static void * osi_s_semphr_create(uint32_t max, uint32_t init)
{
    osi_hit(13u);
    return 0;
}

static void osi_s_semphr_delete(void *semphr)
{
    osi_hit(14u);
}

static int32_t osi_s_semphr_take(void *semphr, uint32_t block_time_tick)
{
    osi_hit(15u);
    return 0;
}

static int32_t osi_s_semphr_give(void *semphr)
{
    osi_hit(16u);
    return 0;
}

static void * osi_s_wifi_thread_semphr_get(void)
{
    osi_hit(17u);
    return 0;
}

static void * osi_s_mutex_create(void)
{
    osi_hit(18u);
    return 0;
}

static void * osi_s_recursive_mutex_create(void)
{
    osi_hit(19u);
    return 0;
}

static void osi_s_mutex_delete(void *mutex)
{
    osi_hit(20u);
}

static int32_t osi_s_mutex_lock(void *mutex)
{
    osi_hit(21u);
    return 0;
}

static int32_t osi_s_mutex_unlock(void *mutex)
{
    osi_hit(22u);
    return 0;
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
    return 0;
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
    return 0;
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
    return 0;
}

static int32_t osi_s_task_create_pinned_to_core(void *task_func, const char *name, uint32_t stack_depth, void *param, uint32_t prio, void *task_handle, uint32_t core_id)
{
    osi_hit(36u);
    return 0;
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
    return 0;
}

static int32_t osi_s_task_get_max_priority(void)
{
    osi_hit(42u);
    return 0;
}

static void * osi_s_malloc(size_t size)
{
    osi_hit(43u);
    return 0;
}

static void osi_s_free(void *p)
{
    osi_hit(44u);
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
    return 0;
}

static void * osi_s_realloc_internal(void *ptr, size_t size)
{
    osi_hit(86u);
    return 0;
}

static void * osi_s_calloc_internal(size_t n, size_t size)
{
    osi_hit(87u);
    return 0;
}

static void * osi_s_zalloc_internal(size_t size)
{
    osi_hit(88u);
    return 0;
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
    return 0;
}

static void osi_s_wifi_delete_queue(void * queue)
{
    osi_hit(94u);
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

static const wifi_osi_funcs_t g_osi = {
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

const void *wifi_osi_table(void)       { return &g_osi; }
uint32_t    wifi_osi_entries(void)     { return OSI_N; }
uint16_t    wifi_osi_calls(uint32_t i) { return (i < OSI_N) ? g_calls[i] : 0u; }
uint8_t     wifi_osi_order(uint32_t i) { return (i < OSI_N) ? g_order[i] : 0u; }
const char *wifi_osi_name(uint32_t i)
{
    static const char *const NM[OSI_N] = {

        "_version",
        "_env_is_chip",
        "_set_intr",
        "_clear_intr",
        "_set_isr",
        "_ints_on",
        "_ints_off",
        "_is_from_isr",
        "_spin_lock_create",
        "_spin_lock_delete",
        "_wifi_int_disable",
        "_wifi_int_restore",
        "_task_yield_from_isr",
        "_semphr_create",
        "_semphr_delete",
        "_semphr_take",
        "_semphr_give",
        "_wifi_thread_semphr_get",
        "_mutex_create",
        "_recursive_mutex_create",
        "_mutex_delete",
        "_mutex_lock",
        "_mutex_unlock",
        "_queue_create",
        "_queue_delete",
        "_queue_send",
        "_queue_send_from_isr",
        "_queue_send_to_back",
        "_queue_send_to_front",
        "_queue_recv",
        "_queue_msg_waiting",
        "_event_group_create",
        "_event_group_delete",
        "_event_group_set_bits",
        "_event_group_clear_bits",
        "_event_group_wait_bits",
        "_task_create_pinned_to_core",
        "_task_create",
        "_task_delete",
        "_task_delay",
        "_task_ms_to_tick",
        "_task_get_current_task",
        "_task_get_max_priority",
        "_malloc",
        "_free",
        "_event_post",
        "_get_free_heap_size",
        "_rand",
        "_dport_access_stall_other_cpu_start_wrap",
        "_dport_access_stall_other_cpu_end_wrap",
        "_wifi_apb80m_request",
        "_wifi_apb80m_release",
        "_phy_disable",
        "_phy_enable",
        "_phy_update_country_info",
        "_read_mac",
        "_timer_arm",
        "_timer_disarm",
        "_timer_done",
        "_timer_setfn",
        "_timer_arm_us",
        "_wifi_reset_mac",
        "_wifi_clock_enable",
        "_wifi_clock_disable",
        "_wifi_rtc_enable_iso",
        "_wifi_rtc_disable_iso",
        "_esp_timer_get_time",
        "_nvs_set_i8",
        "_nvs_get_i8",
        "_nvs_set_u8",
        "_nvs_get_u8",
        "_nvs_set_u16",
        "_nvs_get_u16",
        "_nvs_open",
        "_nvs_close",
        "_nvs_commit",
        "_nvs_set_blob",
        "_nvs_get_blob",
        "_nvs_erase_key",
        "_get_random",
        "_get_time",
        "_random",
        "_log_write",
        "_log_writev",
        "_log_timestamp",
        "_malloc_internal",
        "_realloc_internal",
        "_calloc_internal",
        "_zalloc_internal",
        "_wifi_malloc",
        "_wifi_realloc",
        "_wifi_calloc",
        "_wifi_zalloc",
        "_wifi_create_queue",
        "_wifi_delete_queue",
        "_coex_init",
        "_coex_deinit",
        "_coex_enable",
        "_coex_disable",
        "_coex_status_get",
        "_coex_condition_set",
        "_coex_wifi_request",
        "_coex_wifi_release",
        "_coex_wifi_channel_set",
        "_coex_event_duration_get",
        "_coex_pti_get",
        "_coex_schm_status_bit_clear",
        "_coex_schm_status_bit_set",
        "_coex_schm_interval_set",
        "_coex_schm_interval_get",
        "_coex_schm_curr_period_get",
        "_coex_schm_curr_phase_get",
        "_coex_schm_process_restart",
        "_coex_schm_register_cb",
        "_coex_register_start_cb",
        "_coex_schm_flexible_period_set",
        "_coex_schm_flexible_period_get",
        "_magic"
    };
    return (i < OSI_N) ? NM[i] : "?";
}
