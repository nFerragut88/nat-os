/* nat-os -- building the WiFi init config. See wifi_init_cfg.h. */

#include "wifi_init_cfg.h"
#include "wifi_osi_table.h"
#include "uart.h"
#include "blob.h"
#include "blobcall.h"

/* THE REAL TABLE, in vendor/windowed/wifi_osi.c.
 *
 * That file predates this work: a generated, correctly-ordered, 118-entry
 * table whose bodies already forward into nat-os through w2c_callN. Its header
 * comment says 116, which is stale -- it has 118, the same shape the blob
 * accepted from the counting table in wifi_osi_stubs.c.
 *
 * ...and it is STALE. Its member ORDER diverges from the current IDF header
 * from index 54 onward, 63 positions differing: it still has
 * _phy_common_clock_enable/_disable where the header now has
 * _phy_update_country_info/_read_mac. Both are 118 entries, so nothing about
 * the size gives it away.
 *
 * The blob detects it and refuses: esp_wifi_init_internal returned
 * ESP_ERR_INVALID_ARG (0x102) having forwarded ZERO calls, where the generated
 * table gets as far as _recursive_mutex_create. That is the exact hazard both
 * files' own comments warn about -- "one member out of position is a call to
 * the wrong function with the wrong arguments, and nothing in the system would
 * diagnose it". Something did diagnose it: the blob.
 *
 * So wifi_osi_stubs.c is not a duplicate, it is the correct one, and this
 * declaration is kept only to document why the other must not be used until
 * it is regenerated.
 *
 * Declared as an opaque object because its typedef lives on the windowed side;
 * only the address is needed here. */
extern char g_wifi_osi_funcs;

/* Deliberately minimal. Every feature enabled here is more of the driver that
 * has to work before anything transmits, and more OS adapter entries that need
 * real bodies rather than the instrumented stubs they currently have.
 *
 *   nvs_enable   0  -- nat-os has no NVS at all. Enabling it would have the
 *                      driver reach for a key/value store that does not exist.
 *   ampdu rx/tx  0  -- block acknowledgement is an optimisation and a large
 *                      amount of state. Not needed to put one frame in the air.
 *   csi          0  -- channel state reporting, pure extra.
 *   feature_caps 0  -- WPA3, FTM, GCMP, 11R and enterprise all off.
 *
 * Buffer counts are ESP-IDF's defaults. They are the one group NOT trimmed:
 * they decide how much the driver allocates through _malloc at init, and
 * starving it is a good way to produce a failure that looks like something
 * else entirely. */
static wifi_init_config_t g_cfg;

/* Override nvs_enable after the struct is built. Exists to bisect
 * ESP_ERR_INVALID_ARG one field at a time rather than guess at 21 fields. */
static int g_nvs_override = -1;

/* [step 185] Refuse to hand the driver a table with a hole in it.
 *
 * The OS adapter is built with designated initializers, so a member the struct
 * declares and the initializer omits is silently NULL -- and the driver calls
 * these without checking. Five slots were NULL for the entire investigation;
 * ppTask reached one of them (offset 216, _phy_common_clock_enable) and jumped
 * to address 0. Nothing in the build, and no scan for unimplemented stubs, can
 * see that: there is no stub to find.
 *
 * The table is bounded by _version at word 0 and _magic at the end, so the
 * function pointers are exactly the words between, and a zero among them is
 * always a defect. Checked here because this is the last code that touches the
 * table before the blob does. */
static uint32_t osi_null_slots(uint32_t *first_off)
{
    const uint32_t *t = (const uint32_t *)wifi_osi_table();
    uint32_t words = wifi_osi_entries();
    uint32_t bad = 0;
    *first_off = 0;
    for (uint32_t i = 1; i + 1 < words; i++) {
        if (t[i] == 0u) {
            if (!bad) { *first_off = i * 4u; }
            bad++;
        }
    }
    return bad;
}

/* [step 190] Bring-up, moved out of shell.c.
 *
 * esp_wifi_init_internal returns ESP_OK as of step 189, so esp_wifi_start() is
 * reachable for the first time. It needs a call site, and shell.c is the one
 * file this project does not add to: it is first in .flash.text, so anything
 * appended shifts everything the flash MMU maps and walks into the step-7
 * layout band -- measured, nine lines of uart_puts there hung blob_map
 * (UM-NATOS-042 section 9.2).
 *
 * So the work moves here and shell.c gets SMALLER: a three-line blob_call
 * becomes a one-line wifi_bringup(). The rule is about growth, and this is the
 * opposite of growth.
 *
 * `wifiinit start` runs both; plain `wifiinit` runs init only, so every
 * measurement taken up to step 189 stays reproducible unchanged. */
static int g_want_start;

void wifi_start_enable(int on);
/* [step 214] Starting the driver REQUIRES its task, so enabling one enables
 * the other. Until now `wifiinit start` called blob_task_enable(0) -- the shell
 * gates it on the literal argument "task" -- and the driver's own task could
 * therefore never be created. It was created anyway, on every working build,
 * because a stray write had left a STACK POINTER in g_bt_enabled and a stack
 * pointer is not zero. That is what the "layout sensitivity" of step 194 has
 * been all along: remove nine bytes from window.S, the wild write lands
 * somewhere else, g_bt_enabled stays 0, and the driver correctly reports
 * ESP_ERR_NO_MEM. The build that failed was the honest one. */
void wifi_start_enable(int on) { g_want_start = on; blob_task_enable(on); }

uint32_t wifi_bringup(const struct blob_entry *e, int want_null);
uint32_t wifi_bringup(const struct blob_entry *e, int want_null)
{
    /* blob_call, not phy_stack_call: the driver reaches
     * _task_create_pinned_to_core, and a task created inside a masked call can
     * never run. Exclusion is the mutex; the scheduler keeps running. */
    uint32_t r = blob_call(e->wifi_init,
                           want_null ? 0u : (uint32_t)wifi_init_cfg(),
                           0u, 0u, 0u);
    if (r != 0u || !g_want_start) {
        return r;
    }
    /* [step 205] Register a WPA callback table. ESP-IDF's esp_wifi_init()
     * WRAPPER calls esp_supplicant_init() right here; nat-os calls
     * esp_wifi_init_internal() directly, so g_ic->wpa_cb (+0x1b4) has been
     * NULL since the driver first initialised. The scan/connect state machine
     * dereferences it -- that is the LoadProhibited at NULL+0x54.
     *
     * The table is STUBS, not zeros -- an all-zero table faults too, just in
     * wifi_station_start instead. It lives in kernel/wifi_osi_impl.c (call0)
     * and the one stub the blob calls lives in vendor/windowed/wifi_glue.c;
     * this does the registration only. */
    extern void wifi_scan_sweep(uint32_t s, uint32_t nfn, uint32_t rfn);
    extern void wifi_tx_beacons(uint32_t tx, uint32_t chan);
    extern void wifi_try_connect(uint32_t cfg, uint32_t conn);
    extern void wifi_rx_start(uint32_t reg, uint32_t freefn, uint32_t pr,
                              uint32_t tx);
    extern uint32_t wpa_cb_table_fill(uint32_t sta_connect);
    extern void wpa_cb_report(void);
    if (e->wifi_register_wpa_cb) {
        uint32_t wr = blob_call(e->wifi_register_wpa_cb,
                                wpa_cb_table_fill(e->sta_connect_internal),
                                0u, 0u, 0u);
        uart_puts("   wpa_cb    returned ");
        uart_put_hex(wr);
        uart_puts("\n");
    }
    /* [step 196] STA mode, between init and start, which is the order
     * ESP-IDF requires. No argument parsing: "wifiinit start" now means
     * init + set_mode(STA) + start, because a driver with no interface is
     * not a state worth having a command for.
     *
     * Setting a mode does NOT transmit. It tells the driver which interface
     * to build; a station only puts energy on air when it scans, associates
     * or is asked to send, and none of those is reachable from here.
     *
     * Deliberately as few instructions as possible: step 195 added about a
     * hundred bytes here to do the same thing and the board watchdog-reset
     * inside phyinit. */
    if (e->wifi_set_mode) {
        uint32_t mr = blob_call(e->wifi_set_mode, 1u, 0u, 0u, 0u);
        uart_puts("   set_mode  STA returned ");
        uart_put_hex(mr);
        uart_puts("\n");
    }

    if (!e->wifi_start) {
        uart_puts("   start     : blob entry has no esp_wifi_start\n");
        return r;
    }

    uart_puts("   calling esp_wifi_start at ");
    uart_put_hex(e->wifi_start);
    uart_puts("\n");
    uint32_t sr = blob_call(e->wifi_start, 0u, 0u, 0u, 0u);
    uart_puts("   start     returned ");
    uart_put_hex(sr);
    uart_puts(sr == 0u ? "  (ESP_OK)\n" : "  (an esp_err_t, not OK)\n");
    /* [step 197] Park the receiver on a channel.
     * esp_wifi_set_channel does not transmit. If the MAC starts taking
     * interrupts after this, the whole receive path works -- routing,
     * trampoline, handler, all of it -- without a frame having left. */
    /* [step 197] Promiscuous mode: hand us every frame on the channel.
     * Receive only -- it disables the address filter, it does not transmit.
     * If interrupts start arriving, the whole receive path is proven. */
    /* [step 198] Turn modem power save OFF.
     * esp_wifi.h: "Default power save type is WIFI_PS_MIN_MODEM" -- the
     * station powers the modem down and wakes only every DTIM period, and
     * an unassociated station has no DTIM to sync to. It reports no error
     * either way, which is exactly the shape of the low interrupt counts.
     * WIFI_PS_NONE is 0. */
    { extern uint32_t g_phy_wakeup_fn; g_phy_wakeup_fn = e->phy_wakeup; }
    if (e->wifi_set_ps) {
        uint32_t ps = blob_call(e->wifi_set_ps, 0u, 0u, 0u, 0u);
        uart_puts("   ps NONE   returned ");
        uart_put_hex(ps);
        uart_puts("\n");
    }
    if (e->wifi_promiscuous) {
        uint32_t pr = blob_call(e->wifi_promiscuous, 1u, 0u, 0u, 0u);
        uart_puts("   promisc   returned ");
        uart_put_hex(pr);
        uart_puts("\n");
    }
    if (e->wifi_set_channel) {
        uint32_t cr = blob_call(e->wifi_set_channel, 1u, 0u, 0u, 0u);
        uart_puts("   channel 1 returned ");
        uart_put_hex(cr);
        uart_puts("\n");
    }

    /* [step 199] A PASSIVE scan of ONE channel.
     *
     * Layout is wifi_scan_config_t from the Arduino-ESP32 esp_wifi_types.h,
     * the closest source of truth available -- but NOT provably the same IDF
     * vintage as this blob. If scan_type sits at a different offset the
     * field reads 0, which is WIFI_SCAN_TYPE_ACTIVE, and the scan
     * transmits. That is why channel is pinned to 1: the exposure if the
     * layout is wrong is one probe request on one channel, not a sweep.
     *
     *   +0 ssid   +4 bssid   +8 channel  +9 show_hidden
     *   +12 scan_type   +16 active.min  +20 active.max
     *   +24 passive_ms  +28 home_chan_dwell
     *
     * 1500 ms of passive dwell also makes the outcome observable: passive
     * takes about a second and a half, active about a tenth.
     *
     * block = 0, NOT 1. A blocking scan waits for WIFI_EVENT_SCAN_DONE, and
     * _event_post is still a stub returning 0 -- so nothing is ever posted and
     * the wait cannot end. Measured: with block=1 the call never returned and
     * the shell task stayed inside it. Retried after step 200 wired the event
     * groups to their implementation, on the theory that a NULL event group
     * was what it waited on. It hangs identically, so that was not it and
     * _event_post remains the suspect. */
    /* [step 204] SWEEP THE CHANNELS. Every scan this project has run was
     * pinned to channel 1, because step 199 pinned it to bound the exposure
     * if scan_type had been read at the wrong offset and the scan
     * transmitted. Step 201 proved the layout is right and the scan is
     * passive, so that reason expired -- and nobody moved the channel back.
     * If the AP is on 6 or 11, "found 0" on channel 1 is the CORRECT answer
     * and the radio was never the problem. channel=0 (all channels) still
     * panics, so this walks them one at a time. */
    /* [step 207] The sweep itself now lives in wifi_osi_impl.c. This file is
     * the position-sensitive one and it should be SHRINKING, not growing --
     * the same reasoning that moved wifi_bringup() out of shell.c at step
     * 190. Twenty-two lines become one. */
    wifi_tx_beacons(e->wifi_80211_tx, e->wifi_set_channel);

    /* [step 217] Then try to associate. One call; see wifi_osi_impl.c. */
    /* [step 227] SCAN BEFORE CONNECTING. The sweep used to run last, so a run
     * whose connect failed with NO_AP_FOUND produced no scan evidence at all
     * and could not distinguish "the network is gone" from "the driver did not
     * look properly". Evidence first, then the action it informs. */
    wifi_scan_sweep(e->wifi_scan_start, e->wifi_scan_ap_num,
                    e->wifi_scan_ap_recs);

    wifi_try_connect(e->wifi_set_config, e->wifi_connect);

    /* [step 222] The data path, after the association. */
    wifi_rx_start(e->reg_rxcb, e->free_rx_buffer, e->wifi_promiscuous,
                  e->internal_tx);


    /* [step 209] TRANSMIT. One call; the frame and the loop are in
     * wifi_osi_impl.c. This is the line that ends "nothing has been
     * transmitted", so it is deliberately after the sweep: the receive
     * evidence is gathered before the radio is asked to speak. */


    /* [step 190] Is the MAC actually armed? _set_intr/_set_isr/_ints_on are
     * reached for the first time here, so the step-177 wiring stops being
     * inert. routed says a line was routed; fired says one has been taken. */
    {
        extern volatile uint32_t g_blob_intr_routed, g_blob_isr_nofn;
        extern volatile uint32_t g_blob_isr_calls[];
        extern volatile uint32_t g_blob_intr_src, g_blob_intr_line, g_blob_intr_prio;
        uart_puts("   [intr]    src=");
        uart_put_dec(g_blob_intr_src);
        uart_puts(" line=");
        uart_put_dec(g_blob_intr_line);
        uart_puts(" prio=");
        uart_put_dec(g_blob_intr_prio);
        uart_puts(" routed=");
        uart_put_dec(g_blob_intr_routed);
        uart_puts(" nofn=");
        uart_put_dec(g_blob_isr_nofn);
        uart_puts(" fired:");
        uint32_t any = 0;
        for (uint32_t k = 0; k < 32u; k++) {
            if (g_blob_isr_calls[k]) {
                any = 1;
                uart_puts(" L");
                uart_put_dec(k);
                uart_puts("=");
                uart_put_dec(g_blob_isr_calls[k]);
            }
        }
        if (!any) { uart_puts(" none"); }
        {
            /* [step 191] Timer slots the driver actually bound. Zero here means
             * the ETS entries are still no-ops, which is what they were. */
            extern uint32_t osi_impl_timers_used(void);
            extern uint32_t g_timer_short;
            uart_puts("  timers=");
            uart_put_dec(osi_impl_timers_used());
            uart_puts(" refused=");
            uart_put_dec(g_timer_short);
            {
                /* [step 203] Did the driver get what it asked for? RX buffers
                 * are allocated at init and _wifi_malloc only started working
                 * at step 182; nothing has checked the result since. */
                extern uint32_t g_osi_alloc_calls, g_osi_alloc_bytes;
                extern uint32_t g_osi_alloc_fails;
                uart_puts("  alloc ");
                uart_put_dec(g_osi_alloc_calls);
                uart_puts("/");
                uart_put_dec(g_osi_alloc_bytes);
                uart_puts("B fails ");
                uart_put_dec(g_osi_alloc_fails);
                /* [step 205] wpa_cb call sites, printed by the windowed file
                 * that owns them. Kept to ONE call: this is wifi_init_cfg.c,
                 * where the layout sensitivity was measured, and a fifteen-line
                 * block here panicked inside esp_wifi_init_internal. */
                wpa_cb_report();
            }
            {
                /* [step 193] Two words from the hardware RNG. Proves the
                 * register decodes and is not stuck; it is not a test of
                 * randomness and must not be read as one. */
                extern uint32_t osi_impl_random(void);
                uart_puts("  rng=");
                uart_put_hex(osi_impl_random());
                uart_puts(",");
                uart_put_hex(osi_impl_random());
            }
        }
        uart_puts("\n");
    }
    return r;
}

const void *wifi_init_cfg(void)
{
    g_cfg.osi_funcs = (wifi_osi_funcs_t *)wifi_osi_table();

    {
        extern void osi_impl_mac_report(void);
        osi_impl_mac_report();
        uint32_t off = 0;
        uint32_t bad = osi_null_slots(&off);
        uart_puts("   osi table : ");
        uart_put_dec(wifi_osi_entries());
        uart_puts(" words, ");
        if (!bad) {
            uart_puts("no null slots\n");
        } else {
            uart_put_dec(bad);
            uart_puts(" NULL SLOTS -- first at offset ");
            uart_put_dec(off);
            uart_puts("  the driver will jump to 0\n");
        }
    }

    /* wpa_crypto_funcs is opaque here; only size and version are set, at
     * offsets 0 and 4, and every callback stays null. Nothing in a raw
     * transmit path should reach them -- if something does, it will fault on a
     * null pointer inside the blob rather than silently mis-encrypt, which is
     * the failure mode to prefer. */
    uint32_t *cr = (uint32_t *)g_cfg.wpa_crypto_funcs;
    cr[0] = sizeof g_cfg.wpa_crypto_funcs;
    cr[1] = ESP_WIFI_CRYPTO_VERSION;

    g_cfg.static_rx_buf_num      = 10;
    g_cfg.dynamic_rx_buf_num     = 32;
    g_cfg.tx_buf_type            = 1;      /* dynamic */
    g_cfg.static_tx_buf_num      = 0;
    g_cfg.dynamic_tx_buf_num     = 32;
    g_cfg.rx_mgmt_buf_type       = 0;
    g_cfg.rx_mgmt_buf_num        = 5;
    g_cfg.cache_tx_buf_num       = 0;
    g_cfg.csi_enable             = 0;
    /* ESP-IDF's DEFAULTS, not zero.
     *
     * These were zeroed to keep the driver simple, and esp_wifi_init_internal
     * answered ESP_ERR_INVALID_ARG (0x102). "Minimal" is not the same as
     * "valid": the driver validates this struct, and block-ack has a
     * consistency rule -- rx_ba_win must agree with ampdu_rx_enable. Matching
     * the configuration Espressif ships is the shape of argument the driver is
     * known to accept; trimming it is a later optimisation with a measurement
     * attached, not a starting point. */
    g_cfg.ampdu_rx_enable        = 0;   /* [step 203] was 1 */
    g_cfg.ampdu_tx_enable        = 0;   /* [step 203] was 1 */
    g_cfg.amsdu_tx_enable        = 0;
    g_cfg.nvs_enable             = 0;
    g_cfg.nano_enable            = 0;
    g_cfg.rx_ba_win              = 6;      /* WIFI_DEFAULT_RX_BA_WIN */
    g_cfg.wifi_task_core_id      = 0;
    g_cfg.beacon_max_len         = 752;
    g_cfg.mgmt_sbuf_num          = 32;
    /* 0xa1 = WPA3_SAE | GMAC | ENTERPRISE, read off the reference board.
     * Zero was a guess that the driver would treat "no features" as valid. */
    g_cfg.feature_caps           = 0xa1u;
    g_cfg.sta_disconnected_pm    = true;    /* reference value */
    g_cfg.espnow_max_encrypt_num = 7;

    /* Last field, and the blob checks it. */
    if (g_nvs_override >= 0) { g_cfg.nvs_enable = g_nvs_override; }

    g_cfg.magic = WIFI_INIT_CONFIG_MAGIC;
    return &g_cfg;
}

uint32_t wifi_init_cfg_size(void) { return (uint32_t)sizeof g_cfg; }

void wifi_init_cfg_nvs(int on) { g_nvs_override = on; }

