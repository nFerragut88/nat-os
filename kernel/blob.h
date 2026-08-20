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
#define BLOB_VERSION 4u

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
