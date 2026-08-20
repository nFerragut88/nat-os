/* nat-os -- building the WiFi init config. See wifi_init_cfg.h. */

#include "wifi_init_cfg.h"
#include "wifi_osi_table.h"

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
    g_cfg.ampdu_rx_enable        = 0;
    g_cfg.ampdu_tx_enable        = 0;
    g_cfg.amsdu_tx_enable        = 0;
    g_cfg.nvs_enable             = 0;
    g_cfg.nano_enable            = 0;
    g_cfg.rx_ba_win              = 0;      /* meaningless with ampdu off */
    g_cfg.wifi_task_core_id      = 0;
    g_cfg.beacon_max_len         = 752;
    g_cfg.mgmt_sbuf_num          = 32;
    g_cfg.feature_caps           = 0;
    g_cfg.sta_disconnected_pm    = false;
    g_cfg.espnow_max_encrypt_num = 7;

    /* Last field, and the blob checks it. */
    g_cfg.magic = WIFI_INIT_CONFIG_MAGIC;
    return &g_cfg;
}

uint32_t wifi_init_cfg_size(void) { return (uint32_t)sizeof g_cfg; }
