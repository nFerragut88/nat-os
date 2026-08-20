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

/* The one vendor function this exists to reach. */
extern int esp_wifi_80211_tx(int ifx, const void *buffer, int len, int seq);

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

    /* Callable entry points. Windowed ABI: the call0 kernel must reach these
     * through the bridges in kernel/window.S, exactly as it reaches libphy. */
    int (*wifi_80211_tx)(int ifx, const void *buffer, int len, int seq);
};

/* `used` because nothing in this translation unit references it and
 * --gc-sections would otherwise be entitled to drop the whole thing. KEEP() in
 * blob.ld covers the section; this covers the object. */
__attribute__((section(".blob_entry"), used))
const struct blob_entry blob_entry = {
    .magic       = 0x3230384Eu,          /* "N802" little-endian */
    .version     = 1u,
    .image_size  = (uint32_t)&_blob_image_size,
    .text_end    = (uint32_t)&_blob_text_end,

    .data_lma    = (uint32_t)&_blob_data_lma,
    .data_vma    = (uint32_t)&_blob_data_vma,
    .data_size   = (uint32_t)&_blob_data_size,
    .bss_start   = (uint32_t)&_blob_bss_start,
    .bss_end     = (uint32_t)&_blob_bss_end,

    .wifi_80211_tx = esp_wifi_80211_tx,
};
