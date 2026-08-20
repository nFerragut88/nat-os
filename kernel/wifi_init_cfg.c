/* nat-os -- building the WiFi init config. See wifi_init_cfg.h. */

#include "wifi_init_cfg.h"
#include "wifi_osi_table.h"

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

const void *wifi_init_cfg(void)
{
    g_cfg.osi_funcs = (wifi_osi_funcs_t *)wifi_osi_table();

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
    g_cfg.ampdu_rx_enable        = 1;
    g_cfg.ampdu_tx_enable        = 1;
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

