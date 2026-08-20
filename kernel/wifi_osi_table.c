/* nat-os -- call0 accessors for the WiFi OS adapter table.
 *
 * The stubs and the table itself are in vendor/windowed/wifi_osi_stubs.c,
 * compiled -mabi=windowed because the blob calls them with CALL8. This file
 * stays call0 so the shell can read the counters without a bridge -- data has
 * no calling convention, only calls do.
 */

#include <stdint.h>
#include "wifi_osi_table.h"

#define OSI_N 118u

extern const wifi_osi_funcs_t g_osi;
extern uint16_t g_osi_calls[OSI_N];
extern uint8_t  g_osi_order[OSI_N];
extern uint32_t g_osi_intr_clamped;
extern uint8_t  g_osi_trace[];
extern uint32_t g_osi_trace_arg[];
extern uint32_t g_osi_trace_n;

const void *wifi_osi_table(void)       { return &g_osi; }
uint32_t    wifi_osi_intr_clamped(void){ return g_osi_intr_clamped; }
uint32_t    wifi_osi_trace_len(void)   { return g_osi_trace_n; }
uint32_t    wifi_osi_trace_idx(uint32_t n) { return g_osi_trace[n]; }
uint32_t    wifi_osi_trace_arg(uint32_t n) { return g_osi_trace_arg[n]; }
uint32_t    wifi_osi_entries(void)     { return OSI_N; }
uint16_t    wifi_osi_calls(uint32_t i) { return (i < OSI_N) ? g_osi_calls[i] : 0u; }
uint8_t     wifi_osi_order(uint32_t i) { return (i < OSI_N) ? g_osi_order[i] : 0u; }

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
