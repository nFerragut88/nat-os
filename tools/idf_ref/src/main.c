/* A reference that is doing NAT-OS'S JOB, not its own.
 *
 * The first version of this was a SoftAP. It proved nat-os's receiver works
 * (UM-NATOS-034 §12) and then produced a register differential that was mostly
 * useless: 342 differences, of which the great majority said only "a fully
 * configured access point and a minimal promiscuous sniffer are configured
 * differently". True, and not a bug. Applying them wholesale hung the board
 * (§14).
 *
 * A difference is not a defect until both sides are doing the same job. So this
 * version matches nat-os as closely as ESP-IDF allows:
 *
 *     promiscuous mode, no association, fixed channel, and a raw 802.11 frame
 *     pushed out by hand every 100 ms
 *
 * which is exactly what nat-os does. What is left in the diff after that should
 * be suspicious rather than merely different.
 *
 * The frame is a PROBE REQUEST because that is what nat-os's `probe` sends, it
 * is broadcast so any receiver logs it, and an access point in range answers it
 * -- which makes "did this actually radiate" checkable from three directions.
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

#define REF_CHANNEL 6

/* A probe request with a wildcard SSID and a basic rate set. Hand-built so the
 * bytes on the air are ours rather than the stack's, the same way nat-os builds
 * its own. Addresses are filled in at run time from the chip's own MAC. */
static uint8_t probe_req[] = {
    0x40, 0x00,                             /* frame control: mgmt, probe req */
    0x00, 0x00,                             /* duration                       */
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff,     /* addr1  destination: broadcast  */
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00,     /* addr2  source: filled below    */
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff,     /* addr3  bssid: broadcast        */
    0x00, 0x00,                             /* sequence control               */
    0x00, 0x00,                             /* IE 0: SSID, length 0, wildcard */
    0x01, 0x04, 0x82, 0x84, 0x8b, 0x96,     /* IE 1: rates 1, 2, 5.5, 11 Mbps */
};

/* ---- the register dump, for the differential ----------------------------
 *
 * Ranges match tools/serial/reg_diff.py and nat-os's `regdump` exactly, so one
 * host script parses both. Printed twice a second apart, so the host can throw
 * away anything that moves on its own -- the TSF timer and the statistics -- and
 * compare only stable state.
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

/* Promiscuous mode needs a callback registered even when nothing is done with
 * the frames: the point is to be in the same RECEIVE configuration as nat-os,
 * which is armed and promiscuous while it transmits. */
static void sniff(void *buf, wifi_promiscuous_pkt_type_t type)
{
    (void)buf;
    (void)type;
}

void app_main(void)
{
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));

    /* STA mode but never associated -- the interface exists so frames can be
     * pushed through it, and nothing connects to anything. */
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_ERROR_CHECK(esp_wifi_set_promiscuous_rx_cb(sniff));
    ESP_ERROR_CHECK(esp_wifi_set_promiscuous(true));
    ESP_ERROR_CHECK(esp_wifi_set_channel(REF_CHANNEL, WIFI_SECOND_CHAN_NONE));

    uint8_t mac[6];
    ESP_ERROR_CHECK(esp_wifi_get_mac(WIFI_IF_STA, mac));
    memcpy(&probe_req[10], mac, 6);          /* addr2: this chip */

    printf("REF-RAW mode=promiscuous channel=%d mac=%02x:%02x:%02x:%02x:%02x:%02x\n",
           REF_CHANNEL, mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    /* Transmit for a while before dumping: the registers wanted are those of a
     * radio that is actively transmitting, not one that has just been set up. */
    for (int i = 0; i < 30; i++) {
        esp_wifi_80211_tx(WIFI_IF_STA, probe_req, sizeof probe_req, true);
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    dump_all();
    for (int i = 0; i < 10; i++) {
        esp_wifi_80211_tx(WIFI_IF_STA, probe_req, sizeof probe_req, true);
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    dump_all();

    for (;;) {
        esp_wifi_80211_tx(WIFI_IF_STA, probe_req, sizeof probe_req, true);
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}
