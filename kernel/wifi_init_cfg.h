/* nat-os -- wifi_init_config_t, for esp_wifi_init_internal().
 *
 * next_moves/08. This struct is how the OS adapter table actually reaches the
 * blob: esp_wifi_init_internal() copies cfg->osi_funcs into the global that
 * ieee80211_raw_frame_sanity_check dereferences. wifi_osi_funcs_register()
 * validates a table but does not install it -- which is why transmit kept
 * faulting on a null pointer at offset 0x54 with a perfectly good table
 * registered.
 *
 * Copied VERBATIM from esp_wifi.h, for the same reason wifi_osi_table.c is
 * generated: the blob reads this by offset, and a hand-retyped layout that is
 * subtly wrong is not detectable by either side. In particular feature_caps is
 * a uint64_t and forces padding that is easy to get wrong by hand.
 */
#ifndef NATOS_WIFI_INIT_CFG_H
#define NATOS_WIFI_INIT_CFG_H

#include <stdint.h>
#include <stdbool.h>
#include "wifi_osi_table.h"   /* wifi_osi_funcs_t, referenced by the struct */

#define WIFI_INIT_CONFIG_MAGIC   0x1F2F3F4F
#define ESP_WIFI_CRYPTO_VERSION  0x00000001

typedef struct {
    wifi_osi_funcs_t*      osi_funcs;              /**< WiFi OS functions */
    /* wpa_crypto_funcs_t by VALUE, 30 members x 4 = 120 bytes. Kept as an
     * opaque block: nat-os supplies no crypto, so only its SIZE matters to
     * the layout of everything after it. size/version are written by hand
     * at offsets 0 and 4 -- see wifi_init_cfg.c. */
    uint8_t                wpa_crypto_funcs[120];
    int                    static_rx_buf_num;      /**< WiFi static RX buffer number */
    int                    dynamic_rx_buf_num;     /**< WiFi dynamic RX buffer number */
    int                    tx_buf_type;            /**< WiFi TX buffer type */
    int                    static_tx_buf_num;      /**< WiFi static TX buffer number */
    int                    dynamic_tx_buf_num;     /**< WiFi dynamic TX buffer number */
    int                    rx_mgmt_buf_type;       /**< WiFi RX MGMT buffer type */
    int                    rx_mgmt_buf_num;        /**< WiFi RX MGMT buffer number */
    int                    cache_tx_buf_num;       /**< WiFi TX cache buffer number */
    int                    csi_enable;             /**< WiFi channel state information enable flag */
    int                    ampdu_rx_enable;        /**< WiFi AMPDU RX feature enable flag */
    int                    ampdu_tx_enable;        /**< WiFi AMPDU TX feature enable flag */
    int                    amsdu_tx_enable;        /**< WiFi AMSDU TX feature enable flag */
    int                    nvs_enable;             /**< WiFi NVS flash enable flag */
    int                    nano_enable;            /**< Nano option for printf/scan family enable flag */
    int                    rx_ba_win;              /**< WiFi Block Ack RX window size */
    int                    wifi_task_core_id;      /**< WiFi Task Core ID */
    int                    beacon_max_len;         /**< WiFi softAP maximum length of the beacon */
    int                    mgmt_sbuf_num;          /**< WiFi management short buffer number, the minimum value is 6, the maximum value is 32 */
    uint64_t               feature_caps;           /**< Enables additional WiFi features and capabilities */
    bool                   sta_disconnected_pm;    /**< WiFi Power Management for station at disconnected status */
    int                    espnow_max_encrypt_num; /**< Maximum encrypt number of peers supported by espnow */
    int                    magic;                  /**< WiFi init magic number, it should be the last field */
} wifi_init_config_t;

/* Fills cfg with values chosen for a minimal bring-up and points osi_funcs at
 * nat-os's table. Returns the config to hand to esp_wifi_init_internal(). */
const void *wifi_init_cfg(void);
uint32_t    wifi_init_cfg_size(void);

#endif
