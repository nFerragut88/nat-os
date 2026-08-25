/* nat-os — mapping and starting the pre-linked vendor 802.11 blob.
 *
 * next_moves/08 step 4. The blob is pre-linked to fixed addresses at build
 * time (vendor/net80211/blob.ld), so there is no runtime linker here: no
 * relocation, no symbol resolution, no ELF parsing. Starting it is four steps.
 *
 *   1. map    program 16 flash MMU entries, invalidate the cache
 *   2. check  read the table at BLOB_IROM_ADDR, verify magic and version
 *   3. init   copy .data out of the mapped image, zero .bss
 *   4. call   through the table, across the windowed/call0 bridge
 *
 * How the image GETS to flash is deliberately not this file's problem. During
 * development esptool writes it to BLOB_FLASH_ADDR directly, exactly as it
 * writes the kernel. SD or serial delivery is a convenience for a board with
 * no computer attached, and is a separate decision -- nat-os has no
 * filesystem, so SD delivery means either a FAT reader or raw LBAs, and
 * neither is a prerequisite for finding out whether the blob runs.
 */
#ifndef NATOS_BLOB_H
#define NATOS_BLOB_H

#include <stdint.h>
#include "flash.h"   /* BLOB_* reservations; this API is defined in terms of them */

#define BLOB_MAGIC   0x3230384Eu     /* "N802" */
#define BLOB_VERSION 18u

/* Mirrors vendor/net80211/blob_entry.c. If one changes, the other must. The
 * magic and version exist so a stale image in flash is REJECTED rather than
 * called -- a wrong function pointer here would jump into whatever bytes are
 * at that address, which is the least debuggable failure available. */
struct blob_entry {
    uint32_t magic;
    uint32_t version;
    uint32_t image_size;
    uint32_t text_end;
    uint32_t data_lma;
    uint32_t data_vma;
    uint32_t data_size;
    uint32_t bss_start;
    uint32_t bss_end;
    uint32_t rodata_lma;         /* mapped, not copied -- see blob_map()     */
    uint32_t rodata_vma;
    uint32_t rodata_size;
    uint32_t wifi_80211_tx;      /* function pointer, called via the bridge */
    uint32_t phy_init;           /* the blob's OWN register_chipv7_phy       */
    uint32_t osi_register;       /* wifi_osi_funcs_register                  */
    uint32_t wifi_init;          /* esp_wifi_init_internal                   */
    uint32_t wifi_start;         /* esp_wifi_start                           */
    /* [step 195] version 5. Appended; a version-4 image is rejected. */
    uint32_t wifi_set_mode;      /* esp_wifi_set_mode(int)                   */
    uint32_t wifi_get_mode;      /* esp_wifi_get_mode(int *)                 */
    /* [step 197] version 6. */
    uint32_t wifi_set_channel;   /* esp_wifi_set_channel(u8, int)  RX only */
    uint32_t wifi_scan_start;    /* esp_wifi_scan_start(cfg, block)        */
    uint32_t wifi_promiscuous;   /* esp_wifi_set_promiscuous(int)  RX only */
    uint32_t wifi_set_ps;        /* esp_wifi_set_ps(int)           RX only */
    uint32_t phy_wakeup;         /* phy_wakeup_init(void)          RX only */
    uint32_t wifi_scan_ap_num;   /* esp_wifi_scan_get_ap_num(u16*)  RX only */
    /* [step 205] version 11. */
    uint32_t wifi_register_wpa_cb; /* esp_wifi_register_wpa_cb_internal(v*) */
    /* [step 206] version 12. */
    uint32_t wifi_scan_ap_recs;  /* esp_wifi_scan_get_ap_records(u16*, v*) */
    /* [step 217] version 13. */
    uint32_t wifi_set_config;    /* esp_wifi_set_config(ifx, conf)         */
    uint32_t wifi_connect;       /* esp_wifi_connect()                     */
    uint32_t wifi_disconnect;    /* esp_wifi_disconnect()                  */
    /* [step 219] version 14. */
    uint32_t sta_connect_internal; /* esp_wifi_sta_connect_internal(bssid) */
    /* [step 222] version 15 -- the data path. */
    uint32_t reg_rxcb;           /* esp_wifi_internal_reg_rxcb(ifx, fn)    */
    uint32_t internal_tx;        /* esp_wifi_internal_tx(ifx, buf, len)    */
    uint32_t free_rx_buffer;     /* esp_wifi_internal_free_rx_buffer(eb)   */
    /* [step 236] version 16. */
    uint32_t set_appie;          /* esp_wifi_set_appie_internal(t,ie,l,a)  */
    /* [step 241] version 17 -- the four-way handshake. */
    uint32_t set_sta_key;        /* esp_wifi_set_sta_key_internal, 9 args  */
    uint32_t ptk_init_done;      /* esp_wifi_wpa_ptk_init_done_internal    */
    uint32_t auth_done;          /* esp_wifi_auth_done_internal            */
    uint32_t get_macaddr;        /* esp_wifi_get_macaddr_internal(ifx,mac) */
    /* [step 244] version 18 -- what the driver believes about the network. */
    uint32_t prof_authmode;      /* esp_wifi_sta_get_prof_authmode_internal */
    uint32_t prof_is_rsn;        /* esp_wifi_sta_prof_is_rsn_internal       */
};

/* Programs the MMU and returns the table, or 0 if the region does not hold a
 * valid image. Safe to call when nothing has been installed: the magic check
 * is what makes an unprogrammed region a clean negative rather than a crash. */
const struct blob_entry *blob_map(void);

/* Copies .data and zeroes .bss. Must run after blob_map() and before any call
 * into the blob. Returns 0 on success. Refuses if the ranges do not lie inside
 * the reservations, because a bad image would otherwise be allowed to write
 * over the kernel's own DRAM. */
int blob_init(const struct blob_entry *e);

/* 0 = not mapped, 1 = mapped and initialised. */
int blob_ready(void);

#endif /* NATOS_BLOB_H */
