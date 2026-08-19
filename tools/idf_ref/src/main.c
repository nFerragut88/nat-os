/* A deliberately boring SoftAP: beacon continuously, say who you are.
 *
 * Espressif's stack, unmodified, so that "does nat-os's receiver work" and
 * "does nat-os's transmitter work" stop being entangled. The AP mode is chosen
 * because a beacon is periodic, broadcast and self-identifying -- exactly what
 * nat-os's `scan` already decodes into a BSSID and an SSID.
 */
#include <stdio.h>
#include <string.h>
#include "nvs_flash.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_wifi.h"
#include "esp_mac.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define REF_SSID    "natref"
#define REF_CHANNEL 6

void app_main(void)
{
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_ap();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    wifi_config_t ap = { 0 };
    strcpy((char *)ap.ap.ssid, REF_SSID);
    ap.ap.ssid_len       = strlen(REF_SSID);
    ap.ap.channel        = REF_CHANNEL;
    ap.ap.max_connection = 4;
    ap.ap.authmode       = WIFI_AUTH_OPEN;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap));
    ESP_ERROR_CHECK(esp_wifi_start());

    uint8_t mac[6];
    ESP_ERROR_CHECK(esp_wifi_get_mac(WIFI_IF_AP, mac));

    for (;;) {
        /* Printed repeatedly so the host tool can pick it up whenever it
         * attaches, rather than only in the first moment after boot. */
        printf("REF-AP ssid=%s channel=%d mac=%02x:%02x:%02x:%02x:%02x:%02x\n",
               REF_SSID, REF_CHANNEL,
               mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
}
