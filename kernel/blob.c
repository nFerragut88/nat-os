/* nat-os — mapping and starting the pre-linked vendor 802.11 blob. See blob.h.
 *
 * This file stays in IRAM. linker.ld only moves shell.c and kmain.c to flash,
 * so that happens by default -- but it is load-bearing here rather than
 * incidental: this code reprograms the flash MMU and invalidates the cache,
 * and it cannot be fetching its own next instruction through the thing it is
 * changing.
 */

#include "blob.h"
#include "flash.h"
#include "uart.h"
#include "critical.h"

/* Same table and geometry boot/boot.c uses. Duplicated rather than shared
 * because the bootloader is a separate binary with its own headers; the
 * _Static_asserts below are what keep the two from drifting. */
#define MMU_TABLE       ((volatile uint32_t *)0x3FF10000u)
#define MMU_VADDR_MASK  0x3FFFFFu
#define MMU_PAGE_SIZE   0x10000u
#define MMU_IROM_OFFSET 64u

/* Cache control. Flushing is not optional: the CPU may already hold entries
 * for these virtual addresses from before the region was programmed, and a
 * stale line means executing whatever used to be there. */
#define DPORT_PRO_CACHE_CTRL_REG  ((volatile uint32_t *)0x3FF00040u)
#define DPORT_PRO_CACHE_CTRL1_REG ((volatile uint32_t *)0x3FF00044u)
#define CACHE_FLUSH_ENA   (1u << 4)
#define CACHE_FLUSH_DONE  (1u << 5)

_Static_assert(BLOB_IROM_ADDR >= 0x400D0000u && BLOB_IROM_ADDR < 0x40400000u,
               "blob window must lie inside IRAM0_CACHE, which uses MMU offset 64");
_Static_assert((BLOB_IROM_SIZE % MMU_PAGE_SIZE) == 0u,
               "blob window must be a whole number of MMU pages");

static int g_ready;

static void cache_flush(void)
{
    uint32_t v = *DPORT_PRO_CACHE_CTRL_REG;
    *DPORT_PRO_CACHE_CTRL_REG = v | CACHE_FLUSH_ENA;
    /* Bounded. A flush that never completes must not become a hang in a
     * routine whose whole purpose is to make the system able to report. */
    for (uint32_t i = 0; i < 100000u; i++) {
        if (*DPORT_PRO_CACHE_CTRL_REG & CACHE_FLUSH_DONE) {
            break;
        }
    }
    *DPORT_PRO_CACHE_CTRL_REG = v;
    (void)*DPORT_PRO_CACHE_CTRL1_REG;
}

const struct blob_entry *blob_map(void)
{
    uint32_t crit = crit_enter();

    uint32_t vaddr = BLOB_IROM_ADDR;
    uint32_t foff  = BLOB_FLASH_ADDR;
    for (uint32_t n = 0; n < BLOB_IROM_SIZE / MMU_PAGE_SIZE; n++) {
        uint32_t entry = MMU_IROM_OFFSET + ((vaddr & MMU_VADDR_MASK) >> 16);
        MMU_TABLE[entry] = foff >> 16;
        vaddr += MMU_PAGE_SIZE;
        foff  += MMU_PAGE_SIZE;
    }
    cache_flush();
    crit_exit(crit);

    const struct blob_entry *e = (const struct blob_entry *)BLOB_IROM_ADDR;

    /* An unprogrammed flash region reads as 0xFF, so this is also the test for
     * "nothing has been installed" -- which must be a clean negative, not a
     * jump through a pointer made of erase pattern. */
    if (e->magic != BLOB_MAGIC) {
        return 0;
    }
    if (e->version != BLOB_VERSION) {
        return 0;
    }
    return e;
}

/* Every range the image asks us to touch is checked against the reservations
 * before anything is written.
 *
 * The image is not hostile, but it IS separately built and separately
 * installed, so a stale or truncated one is entirely ordinary. Without these
 * checks a wrong data_vma would have blob_init() write over the kernel's own
 * DRAM -- the heap, or another task's stack -- and the symptom would appear
 * somewhere with no connection to the blob at all. */
static int range_ok(uint32_t start, uint32_t end, uint32_t low, uint32_t high)
{
    if (end < start)   { return 0; }
    if (start < low)   { return 0; }
    if (end > high)    { return 0; }
    return 1;
}

int blob_init(const struct blob_entry *e)
{
    if (!e) {
        return -1;
    }

    const uint32_t irom_lo = BLOB_IROM_ADDR;
    const uint32_t irom_hi = BLOB_IROM_ADDR + BLOB_IROM_SIZE;
    const uint32_t dram_lo = BLOB_DRAM_ADDR;
    const uint32_t dram_hi = BLOB_DRAM_ADDR + BLOB_DRAM_SIZE;

    if (e->image_size > BLOB_FLASH_SIZE) {
        return -2;
    }
    if (!range_ok(e->data_lma, e->data_lma + e->data_size, irom_lo, irom_hi)) {
        return -3;      /* .data initialisers are not inside the mapped image */
    }
    if (!range_ok(e->data_vma, e->data_vma + e->data_size, dram_lo, dram_hi)) {
        return -4;      /* .data would land outside the reservation */
    }
    if (!range_ok(e->bss_start, e->bss_end, dram_lo, dram_hi)) {
        return -5;      /* .bss would land outside the reservation */
    }
    if (e->wifi_80211_tx < irom_lo || e->wifi_80211_tx >= irom_hi) {
        return -6;      /* the entry point is not in the blob */
    }

    /* WORD access only, and this is not a preference.
     *
     * data_lma points into the flash-mapped INSTRUCTION region, which on this
     * chip serves 32-bit aligned accesses and nothing else. A byte loop over
     * it raises LoadStoreError on the first read -- which is exactly what it
     * did, at 0x4008187f, before this was a word copy.
     *
     * That is the second time this class of fault has been paid for in one
     * day: boot.c hit it storing bytes INTO iram and needed a DRAM bounce
     * buffer. The rule worth carrying: if either end of a copy is instruction
     * memory, the copy is a word copy, and the sizes and addresses have to be
     * checked for alignment rather than assumed.
     *
     * blob.ld ALIGNs both to 4, so the guard below should never fire -- but a
     * separately built image is exactly the thing that can stop being true
     * without this file changing. */
    if ((e->data_lma % 4u) || (e->data_vma % 4u) || (e->data_size % 4u) ||
        (e->bss_start % 4u) || (e->bss_end % 4u)) {
        return -7;
    }

    /* The MMU maps instructions; it does not populate writable memory. These
     * two steps are what a loader exists for. */
    uart_puts("      [copy .data]\n");
    const volatile uint32_t *src = (const volatile uint32_t *)e->data_lma;
    volatile uint32_t *dst       = (volatile uint32_t *)e->data_vma;
    for (uint32_t i = 0; i < e->data_size / 4u; i++) {
        dst[i] = src[i];
    }
    uart_puts("      [zero .bss]\n");
    volatile uint32_t *bss = (volatile uint32_t *)e->bss_start;
    for (uint32_t i = 0; i < (e->bss_end - e->bss_start) / 4u; i++) {
        bss[i] = 0;
    }

    uart_puts("      [done]\n");
    g_ready = 1;
    return 0;
}

int blob_ready(void)
{
    return g_ready;
}
