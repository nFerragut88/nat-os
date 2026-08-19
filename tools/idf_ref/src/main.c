/* A deliberately boring SoftAP: beacon continuously, say who you are, and dump
 * the registers that matter while doing it.
 *
 * Espressif's stack, unmodified, so that "does nat-os's receiver work" and
 * "does nat-os's transmitter work" stop being entangled. AP mode because a
 * beacon is periodic, broadcast and self-identifying -- exactly what nat-os's
 * `scan` already decodes into a BSSID and an SSID.
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

/* ---- the register dump, for the differential ----------------------------
 *
 * Both stacks call the same PHY blob, so whatever it does internally is
 * identical in both. The difference between a radio that transmits and one that
 * does not therefore has to show in the registers the SURROUNDING code touches
 * -- and that is a layer both sides can dump.
 *
 * Ranges: the WiFi MAC block nat-os writes to, DPORT's clock and reset
 * registers, and RTC_CNTL's power and isolation registers.
 *
 * Printed TWICE, a second apart. Anything differing between a board's own two
 * dumps is a counter -- the TSF timer, statistics -- and the host tool discards
 * it. What survives is stable state, the only kind worth comparing across
 * boards. Without that filter the diff is almost entirely free-running clocks.
 *
 * Caveat kept in view: some MAC registers may be read-to-clear, so dumping is
 * not perfectly passive. Both sides perform identical reads, so the comparison
 * stays fair even if looking disturbs something.
 */
static void dump_range(const char *tag, uint32_t base, uint32_t words)
{
    for (uint32_t i = 0; i < words; i += 8) {
        printf("REG %s %08x", tag, (unsigned)(base + i * 4));
        for (uint32_t j = 0; j < 8 && (i + j) < words; j++) {
            printf(" %08x",
                   (unsigned)(*(volatile uint32_t *)(base + (i + j) * 4)));
        }
        printf("\n");
    }
}

static void dump_all(void)
{
    dump_range("dport", 0x3FF00000u, 64u);
    dump_range("rtc",   0x3FF48000u, 64u);
    dump_range("mac",   0x3FF73000u, 1280u);
    printf("REGEND\n");
}

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

    printf("REF-AP ssid=%s channel=%d mac=%02x:%02x:%02x:%02x:%02x:%02x\n",
           REF_SSID, REF_CHANNEL,
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    /* Let the AP settle and start beaconing before anything is read: the point
     * is to capture a radio that is actively transmitting, not one mid-init. */
    vTaskDelay(pdMS_TO_TICKS(3000));

    dump_all();
    vTaskDelay(pdMS_TO_TICKS(1000));
    dump_all();

    for (;;) {
        printf("REF-AP ssid=%s channel=%d mac=%02x:%02x:%02x:%02x:%02x:%02x\n",
               REF_SSID, REF_CHANNEL,
               mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
}
