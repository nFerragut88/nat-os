/* nat-os — second-stage bootloader.
 *
 * The ROM's first-stage loader reads this from flash 0x1000, copies its
 * segments into RAM and jumps here. This puts nat-os in memory and starts it.
 *
 * ---- what it must do ------------------------------------------------------
 *
 *   read the image header at APP_OFFSET
 *   for each segment:
 *       DROM  -> map flash pages into the data cache window; do not copy
 *       RAM   -> copy the bytes out of flash
 *   enable the cache, jump to the entry point
 *
 * ---- constraints that shape it -------------------------------------------
 *
 * This code runs with the flash cache DISABLED, so it cannot have any
 * flash-mapped rodata of its own: every constant is either an immediate or in
 * DRAM. That is what boot.ld enforces by giving this image only IRAM and DRAM.
 *
 * It also cannot call anything in nat-os. The kernel is not in memory yet --
 * putting it there is the entire job -- so this is a self-contained program
 * that happens to share two source files with the kernel by compiling them
 * again, not by linking against it.
 *
 * ---- reading flash without the cache -------------------------------------
 *
 * The one hard part is usually a chicken-and-egg: you need to read flash to
 * load the loader that sets up flash. This project already solved it.
 * kernel/flash.c drives SPI1 through its registers directly -- no ROM calls, no
 * vendor code -- and works with the cache off, because it never depends on the
 * cache. It is compiled into this image and used as-is.
 */

#include <stdint.h>
#include "flash.h"
#include "uart.h"

/* Where nat-os lives. Hardcoded rather than read from a partition table: this
 * kernel has exactly one application image and inventing a table it does not
 * use would be ceremony. The partition table stays at 0x8000 for esptool's
 * benefit; nothing here reads it. */
#define APP_OFFSET 0x10000u

/* ---- ESP32 image format, from the TRM and UM-NATOS-002 ------------------- */
typedef struct __attribute__((packed)) {
    uint8_t  magic;             /* 0xE9                                     */
    uint8_t  segment_count;
    uint8_t  spi_mode;
    uint8_t  spi_speed_size;
    uint32_t entry_addr;
    uint8_t  wp_pin;
    uint8_t  spi_pin_drv[3];
    uint16_t chip_id;
    uint8_t  min_chip_rev;
    uint8_t  reserved[8];
    uint8_t  hash_appended;
} img_header_t;

typedef struct __attribute__((packed)) {
    uint32_t load_addr;
    uint32_t data_len;
} seg_header_t;

/* ---- flash MMU ------------------------------------------------------------
 *
 * Constants read out of the SDK headers rather than recalled, for the reason
 * this project has now paid for four times: soc/dport_reg.h, ext_mem_defs.h and
 * hal/mmu_ll.h.
 *
 *   table         0x3FF10000, one 32-bit entry per 64 KB page
 *   DROM window   0x3F400000..0x3F800000, entry index = (vaddr & 0x3FFFFF) >> 16
 *   entry value   physical flash page number, i.e. paddr >> 16
 *   MMU_INVALID   bit 8 -- so a valid entry is simply a page number under 256
 */
#define MMU_TABLE       ((volatile uint32_t *)0x3FF10000u)
#define MMU_VADDR_MASK  0x3FFFFFu
#define DROM_LOW        0x3F400000u
#define DROM_HIGH       0x3F800000u
#define MMU_PAGE_SIZE   0x10000u

/* Flash-mapped EXECUTABLE code -- what an ESP32 application calls IROM.
 *
 * The window is 0x400D0000..0x40400000, which soc/ext_mem_defs.h names
 * IRAM0_CACHE. It is NOT IROM0_CACHE at 0x40800000; that is a different window
 * with a different entry range, and assuming otherwise was a wrong first guess
 * worth recording.
 *
 * The entry OFFSET is the part that differs from DROM. From
 * hal/esp32/mmu_ll.h, mmu_ll_get_entry_id():
 *
 *     DROM0_CACHE  -> offset   0
 *     IRAM0_CACHE  -> offset  64
 *     IRAM1_CACHE  -> offset 128
 *     IROM0_CACHE  -> offset 192
 *
 * with the same >> 16 shift and the same 0x3FFFFF mask in every case. Getting
 * the offset wrong writes a valid-looking entry for somebody else's window and
 * the CPU fetches whatever happens to be mapped there. */
#define IROM_LOW        0x400D0000u
#define IROM_HIGH       0x40400000u
#define MMU_IROM_OFFSET 64u

/* Anything at or above this is on the instruction bus. Data below it (DRAM at
 * 0x3FFBxxxx, DROM at 0x3F4xxxxx) is byte-accessible; instruction memory is
 * not, which is the distinction copy_to_iram() exists for. */
#define IBUS_LOW        0x40000000u

/* Cache control. Writing MMU entries with the cache enabled is undefined, so
 * the cache goes down, the table is written, and it comes back up. */
#define DPORT_PRO_CACHE_CTRL_REG   0x3FF00040u
#define DPORT_PRO_CACHE_CTRL1_REG  0x3FF00044u
#define PRO_CACHE_ENABLE           (1u << 3)
#define PRO_CACHE_FLUSH_ENA        (1u << 4)
#define PRO_CACHE_FLUSH_DONE       (1u << 5)
#define PRO_CACHE_MASK_DROM0       (1u << 4)
/* Cleared alongside DROM0 once code is fetched from flash too. */
#define PRO_CACHE_MASK_IRAM0       (1u << 0)

#define REG(a) (*(volatile uint32_t *)(a))

/* ---- the RTC watchdog ----------------------------------------------------
 *
 * The ROM arms this before jumping here, so that a second stage which hangs
 * still produces a reset rather than a dead board. nat-os disables it in
 * watchdog_disable_all(), but not until the kernel is running -- which leaves
 * the whole of this program depending on being fast enough.
 *
 * It nearly always is. "Nearly" is the problem: a marginal timing failure shows
 * up as a board that boots most of the time, and a boot that works most of the
 * time is worse to diagnose than one that never works. Three register writes
 * remove the question. The kernel disables it again; doing it twice costs
 * nothing.
 *
 * Same key and registers as kernel/watchdog.c, deliberately duplicated rather
 * than shared -- pulling in watchdog.c would drag the timer-group half of it
 * into an image that has no use for it.
 */
#define WDT_WKEY               0x50D83AA1u
#define RTC_CNTL_WDTCONFIG0    0x3FF4808Cu
#define RTC_CNTL_WDTWPROTECT   0x3FF480A4u

static void rtc_wdt_disable(void)
{
    REG(RTC_CNTL_WDTWPROTECT) = WDT_WKEY;
    REG(RTC_CNTL_WDTCONFIG0)  = 0;
    REG(RTC_CNTL_WDTWPROTECT) = 0;
}

static void cache_disable(void)
{
    REG(DPORT_PRO_CACHE_CTRL_REG) &= ~PRO_CACHE_ENABLE;
}

static void cache_enable_drom(void)
{
    /* Clear the DROM0 mask bit: on this part the MASK bits DISABLE a window,
     * so clearing is what makes the region cacheable. */
    REG(DPORT_PRO_CACHE_CTRL1_REG) &= ~(PRO_CACHE_MASK_DROM0 | PRO_CACHE_MASK_IRAM0);

    /* Flush, wait for the hardware to say it finished, then release. A flush
     * that is started and not waited for is the same shape of mistake as a
     * transfer that is started and not waited for, and this kernel has a
     * standing rule about those. */
    REG(DPORT_PRO_CACHE_CTRL_REG) |= PRO_CACHE_FLUSH_ENA;
    while (!(REG(DPORT_PRO_CACHE_CTRL_REG) & PRO_CACHE_FLUSH_DONE)) {
    }
    REG(DPORT_PRO_CACHE_CTRL_REG) &= ~PRO_CACHE_FLUSH_ENA;

    REG(DPORT_PRO_CACHE_CTRL_REG) |= PRO_CACHE_ENABLE;
}

/* Map `len` bytes of flash at `flash_off` to appear at `vaddr`. */
/* `entry_offset` selects which window's block of MMU entries to write: 0 for
 * DROM, 64 for the IRAM0 cache window that holds flash-executable code. The
 * arithmetic within a block is identical, which is why one function serves
 * both -- but the offset is not optional. Writing a DROM index for an IROM
 * address produces a perfectly valid entry pointing the wrong window at the
 * right flash page, and the CPU then executes whatever was already mapped
 * where the code should have been. */
static void mmu_map(uint32_t vaddr, uint32_t flash_off, uint32_t len,
                    uint32_t entry_offset)
{
    uint32_t page_v = vaddr & ~(MMU_PAGE_SIZE - 1u);
    uint32_t page_f = flash_off & ~(MMU_PAGE_SIZE - 1u);
    uint32_t end    = vaddr + len;

    while (page_v < end) {
        uint32_t entry = entry_offset + ((page_v & MMU_VADDR_MASK) >> 16);
        MMU_TABLE[entry] = page_f >> 16;
        page_v += MMU_PAGE_SIZE;
        page_f += MMU_PAGE_SIZE;
    }
}

/* kernel/uart.h already has uart_put_hex(); this file just uses a shorter
 * name for it, because half the lines below are hex. */
#define put_hex(v) uart_put_hex((unsigned int)(v))

/* ---- getting bytes into IRAM ---------------------------------------------
 *
 * IRAM is instruction memory and only answers aligned 32-bit accesses. A byte
 * store to it raises LoadStoreError, which is exactly how this was found: the
 * first three segments loaded, and the fourth -- IRAM at 0x40080000 -- faulted
 * with excvaddr pointing straight at its own load address.
 *
 * kernel/flash.c reassembles the SPI FIFO a byte at a time (`rx[i] = word >>
 * ...`), which is correct everywhere the kernel itself uses it and wrong only
 * here. Rather than change a file the kernel depends on to serve its one
 * unusual caller, the read lands in DRAM and this copies it across in words.
 *
 * 1 KB, out of the 24 KB this image has: large enough that the per-call
 * overhead is noise, small enough to be free.
 */
#define BOUNCE_LEN 1024u
static uint8_t g_bounce[BOUNCE_LEN] __attribute__((aligned(4)));

static int copy_to_iram(uint32_t off, uint32_t dst, uint32_t len)
{
    while (len) {
        uint32_t n = (len > BOUNCE_LEN) ? BOUNCE_LEN : len;

        if (flash_read(off, g_bounce, n) != 0) {
            return -1;
        }

        /* Rounding up can read up to three bytes past n -- out of our own
         * buffer, into our own buffer. Segment lengths are word multiples
         * anyway, so it does not happen; the round-up is there so a hypothetical
         * odd length short-writes garbage rather than dropping bytes. */
        const uint32_t *src = (const uint32_t *)(const void *)g_bounce;
        volatile uint32_t *d = (volatile uint32_t *)dst;
        for (uint32_t w = 0; w < (n + 3u) / 4u; w++) {
            d[w] = src[w];
        }

        off += n;
        dst += n;
        len -= n;
    }
    return 0;
}

static void fail(const char *why)
{
    uart_puts("\n[boot] FAILED: ");
    uart_puts(why);
    uart_puts("\n[boot] halted. reflash vendor/bootloader.bin at 0x1000 to recover.\n");
    for (;;) {
    }
}

void boot_main(void)
{
    /* No uart_init(). The ROM brought UART0 up to talk to esptool before it
     * jumped here, and kernel/uart.c likewise assumes a live port -- it has no
     * init function at all. Re-initialising would only risk dropping the
     * baud rate the ROM negotiated. */
    rtc_wdt_disable();
    uart_puts("\n[boot] nat-os second stage\n");

    /* No init call: kernel/flash.c reaches SPI1 through its registers and is
     * ready as soon as the chip is. Reading the JEDEC id is the cheap proof
     * that the controller answers before anything depends on it -- an id of 0
     * or 0xFFFFFF means the bus is dead, and every read after it would be
     * garbage that looks like data. */
    uint32_t id = flash_read_id();
    uart_puts("[boot] flash id ");
    put_hex(id);
    uart_puts("\n");
    if (id == 0u || (id & 0xFFFFFFu) == 0xFFFFFFu) {
        fail("flash did not answer");
    }

    img_header_t hdr;
    if (flash_read(APP_OFFSET, &hdr, sizeof hdr) != 0) {
        fail("header read");
    }
    if (hdr.magic != 0xE9u) {
        fail("bad magic at 0x10000");
    }

    uart_puts("[boot] segments ");
    put_hex(hdr.segment_count);
    uart_puts("  entry ");
    put_hex(hdr.entry_addr);
    uart_puts("\n");

    uint32_t off = APP_OFFSET + sizeof(img_header_t);

    for (uint32_t i = 0; i < hdr.segment_count; i++) {
        seg_header_t seg;
        if (flash_read(off, &seg, sizeof seg) != 0) {
            fail("segment header read");
        }
        off += sizeof(seg_header_t);

        uart_puts("[boot]   ");
        put_hex(seg.load_addr);
        uart_puts(" len ");
        put_hex(seg.data_len);

        if (seg.load_addr >= DROM_LOW && seg.load_addr < DROM_HIGH) {
            /* Flash-mapped: the bytes stay where they are and the address
             * space is pointed at them. Copying would need 27 KB of RAM this
             * board would rather keep.
             *
             * The MMU table is written here but the cache is NOT switched on
             * here. DROM is segment 1 of 4, and three more segments get copied
             * over SPI1 after this point -- with the cache live, the cache
             * controller is a second master on the same bus, and two masters
             * driving one flash chip is a corruption that would look like a
             * random bad byte somewhere in the kernel. It goes on once, after
             * the loop, when nothing else will touch SPI1 again. */
            uart_puts("  -> mmu drom\n");
            cache_disable();
            mmu_map(seg.load_addr, off, seg.data_len, 0u);
        } else if (seg.load_addr >= IROM_LOW && seg.load_addr < IROM_HIGH) {
            /* Flash-executable code. Same treatment as DROM, different window
             * and therefore a different block of MMU entries. Checked BEFORE
             * the instruction-bus case below, which would otherwise see an
             * address above 0x40000000 and copy it into RAM -- defeating the
             * point entirely and overwriting whatever was already there. */
            uart_puts("  -> mmu irom\n");
            cache_disable();
            mmu_map(seg.load_addr, off, seg.data_len, MMU_IROM_OFFSET);
        } else if (seg.load_addr >= IBUS_LOW) {
            /* Instruction bus: word stores only, via the bounce buffer. */
            uart_puts("  -> copy (iram)\n");
            if (copy_to_iram(off, seg.load_addr, seg.data_len) != 0) {
                fail("iram segment copy");
            }
        } else {
            /* DRAM: byte-accessible, so flash_read can write it directly. */
            uart_puts("  -> copy\n");
            if (flash_read(off, (void *)seg.load_addr, seg.data_len) != 0) {
                fail("segment copy");
            }
        }

        /* Segments are padded to a 4-byte boundary. */
        off += (seg.data_len + 3u) & ~3u;
    }

    /* Now, and only now: no further flash reads, so the cache can have the bus.
     * Everything the kernel reads from its DROM depends on this line. */
    cache_enable_drom();

    uart_puts("[boot] starting nat-os\n\n");

    void (*entry)(void) = (void (*)(void))hdr.entry_addr;
    entry();

    fail("kernel returned");
}
