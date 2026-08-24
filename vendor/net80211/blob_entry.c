/* The blob's entry table — next_moves/08 step 3.
 *
 * Placed at the very start of the mapped region (BLOB_IROM_ADDR) by blob.ld,
 * so the kernel can find everything it needs by reading one fixed address. No
 * ELF parsing, no symbol table, no relocation.
 *
 * Everything the LOADER needs is here too, not just the callable functions:
 * where to copy .data from and to, and what to zero for .bss. The MMU maps
 * instructions; it does not populate writable memory, so those two steps are
 * the loader's job and the blob is the thing that knows the numbers.
 */

#include <stdint.h>

/* Supplied by blob.ld. */
extern char _blob_start, _blob_text_end, _blob_image_end;
extern char _blob_data_lma, _blob_data_vma, _blob_data_end;
extern char _blob_bss_start, _blob_bss_end;
/* Absolute symbols whose ADDRESS is the size -- see the note in blob.ld. */
extern char _blob_image_size, _blob_data_size;
extern char _blob_rodata_lma, _blob_rodata_vma, _blob_rodata_size;

/* The one vendor function this exists to reach. */
extern int esp_wifi_80211_tx(int ifx, const void *buffer, int len, int seq);

/* The blob carries its OWN libphy. The kernel must therefore initialise THIS
 * copy, not the one its -WiFi build used to link into IRAM: two copies have
 * two sets of .bss, and calibrating one while transmitting through the other
 * would look like a PHY that initialised fine and a radio that stayed silent. */
extern int register_chipv7_phy(const void *init_data, void *cal_data, int mode);

/* Bring-up, in the order the driver requires: hand over the OS adapter table,
 * then init, then start. esp_wifi_80211_tx is the LAST step -- calling it cold
 * faulted on osi_funcs->_mutex_lock through a null table. */
extern int  wifi_osi_funcs_register(const void *funcs);
extern int  esp_wifi_init_internal(const void *cfg);
extern int  esp_wifi_start(void);
extern int  esp_wifi_set_mode(int mode);   /* [step 195] */
extern int  esp_wifi_get_mode(int *mode);
extern int  esp_wifi_set_channel(unsigned char pri, int sec);  /* [step 197] */
extern int  esp_wifi_scan_start(const void *cfg, int block);
extern int  esp_wifi_set_promiscuous(int en);   /* [step 197] RX only */
extern int  esp_wifi_set_ps(int type);   /* [step 198] RX only */
extern void phy_wakeup_init(void);   /* [step 198] wake the PHY from sleep */
extern int  esp_wifi_scan_get_ap_num(unsigned short *n);  /* [step 202] */
/* [step 205] The WPA supplicant callback table. In ESP-IDF this is registered
 * by esp_supplicant_init(), called from the esp_wifi_init() WRAPPER -- which
 * lives in open-source IDF code, not in this blob. nat-os calls
 * esp_wifi_init_internal() directly and so has never registered anything, and
 * g_ic->wpa_cb has been NULL the whole time. */
extern int  esp_wifi_register_wpa_cb_internal(void *cb);
/* [step 206] The scan RESULTS, not just the count. */
extern int  esp_wifi_scan_get_ap_records(unsigned short *n, void *recs);

struct blob_entry {
    uint32_t magic;          /* 'N','8','0','2' */
    uint32_t version;
    uint32_t image_size;     /* bytes to write to flash, from _blob_start   */
    uint32_t text_end;       /* end of executable content (diagnostics)     */

    uint32_t data_lma;       /* copy FROM here (inside the mapped region)   */
    uint32_t data_vma;       /* copy TO here (DRAM)                         */
    uint32_t data_size;
    uint32_t bss_start;      /* zero from here...                           */
    uint32_t bss_end;        /* ...to here                                  */

    /* Read-only data, mapped through the DATA cache at a DROM address. The
     * loader does not copy this -- it maps it -- but it needs the numbers to
     * know which pages, and reporting them makes a bad split visible. */
    uint32_t rodata_lma;
    uint32_t rodata_vma;
    uint32_t rodata_size;

    /* Callable entry points. Windowed ABI: the call0 kernel must reach these
     * through the bridges in kernel/window.S, exactly as it reaches libphy. */
    int (*wifi_80211_tx)(int ifx, const void *buffer, int len, int seq);
    int (*phy_init)(const void *init_data, void *cal_data, int mode);
    int (*osi_register)(const void *funcs);
    int (*wifi_init)(const void *cfg);
    int (*wifi_start)(void);
    /* [step 195] Present, but the version stays 4 and the kernel struct does
     * NOT declare them -- see docs/next_moves/08, step 195. Appending is
     * backward compatible: an older kernel simply never reads them. */
    int (*wifi_set_mode)(int mode);
    int (*wifi_get_mode)(int *mode);
    /* [step 197] version 6. set_channel is receive-side only. scan_start is
     * present but transmits when asked for an ACTIVE scan; see the kernel. */
    int (*wifi_set_channel)(unsigned char pri, int sec);
    int (*wifi_scan_start)(const void *cfg, int block);
    int (*wifi_promiscuous)(int en);
    int (*wifi_set_ps)(int type);
    void (*phy_wakeup)(void);
    int (*wifi_scan_ap_num)(unsigned short *n);
    /* [step 205] version 11. */
    int (*wifi_register_wpa_cb)(void *cb);
    /* [step 206] version 12. */
    int (*wifi_scan_ap_recs)(unsigned short *n, void *recs);
};

/* `used` because nothing in this translation unit references it and
 * --gc-sections would otherwise be entitled to drop the whole thing. KEEP() in
 * blob.ld covers the section; this covers the object. */
__attribute__((section(".blob_entry"), used))
const struct blob_entry blob_entry = {
    .magic       = 0x3230384Eu,          /* "N802" little-endian */
    .version     = 12u,
    .image_size  = (uint32_t)&_blob_image_size,
    .text_end    = (uint32_t)&_blob_text_end,

    .data_lma    = (uint32_t)&_blob_data_lma,
    .data_vma    = (uint32_t)&_blob_data_vma,
    .data_size   = (uint32_t)&_blob_data_size,
    .bss_start   = (uint32_t)&_blob_bss_start,
    .bss_end     = (uint32_t)&_blob_bss_end,

    .rodata_lma  = (uint32_t)&_blob_rodata_lma,
    .rodata_vma  = (uint32_t)&_blob_rodata_vma,
    .rodata_size = (uint32_t)&_blob_rodata_size,

    .wifi_80211_tx = esp_wifi_80211_tx,
    .phy_init      = register_chipv7_phy,
    .osi_register  = wifi_osi_funcs_register,
    .wifi_init     = esp_wifi_init_internal,
    .wifi_start    = esp_wifi_start,
    .wifi_set_mode = esp_wifi_set_mode,
    .wifi_get_mode = esp_wifi_get_mode,
    .wifi_set_channel = esp_wifi_set_channel,
    .wifi_scan_start  = esp_wifi_scan_start,
    .wifi_promiscuous = esp_wifi_set_promiscuous,
    .wifi_set_ps      = esp_wifi_set_ps,
    .phy_wakeup       = phy_wakeup_init,
    .wifi_scan_ap_num = esp_wifi_scan_get_ap_num,
    .wifi_register_wpa_cb = esp_wifi_register_wpa_cb_internal,
    .wifi_scan_ap_recs    = esp_wifi_scan_get_ap_records,
};
