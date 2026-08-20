/* nat-os — SPI flash read/erase/write.
 *
 * The first thing in this kernel that survives a power cycle.
 *
 * ---- the cache hazard ---------------------------------------------------
 *
 * `.rodata` is mapped from flash through the data cache (UM-NATOS-011). A flash
 * chip cannot serve reads while it is erasing or programming, so any cache miss
 * during an operation would read a busy chip. That is why ESP-IDF disables the
 * cache around flash writes.
 *
 * This driver does not disable the cache. It makes the reads impossible
 * instead:
 *
 *   - all kernel code executes from IRAM, so instruction fetch never touches
 *     flash
 *   - flash.o, panic.o, uart.o and watchdog.o have their .rodata placed in DRAM
 *     by the linker, so nothing on this path reads a flash-mapped address
 *   - interrupts are masked for the whole operation, so no handler can run and
 *     touch one either
 *
 * A cache HIT is harmless — it never reaches the chip. Only a miss would, and
 * with no flash-mapped address referenced there is nothing to miss on.
 *
 * The residual risk is honest and worth stating: this argument depends on every
 * function reachable from here keeping its read-only data out of flash. The
 * linker rule is by OBJECT FILE rather than by annotation precisely so that
 * adding a string to one of these files cannot quietly break it.
 *
 * ---- where it writes ----------------------------------------------------
 *
 * A region at 2 MB, far from the bootloader at 0x1000, the partition table at
 * 0x8000, and the application image at 0x10000. A wrong address inside this
 * driver therefore cannot stop the board from booting, which keeps every
 * failure recoverable over serial rather than requiring a full erase.
 */

#ifndef NATOS_FLASH_H
#define NATOS_FLASH_H

#include <stdint.h>

/* Compile-time disable, kept because a flash driver is the one thing here that
 * can damage state a reset does not clear. */
#define FLASH_ENABLE 1

#define FLASH_SECTOR     4096u
#define FLASH_DATA_ADDR  0x200000u      /* 2 MB in; clear of everything */

/* ---- reserved region for the pre-linked vendor 802.11 blob ---------------
 *
 * next_moves/08. Nothing writes here yet; the reservation exists so that the
 * address is decided ONCE, before a loader, an installer or a linker script
 * hard-codes a different guess.
 *
 * The flash side and the virtual side are both 64 KB aligned because the flash
 * MMU maps 64 KB pages (boot/boot.c, MMU_PAGE_SIZE) -- a request for, say,
 * 700 KB is not expressible and would silently become 704.
 *
 * The offset matters more than the size. 0x200000 was proposed first and is
 * exactly FLASH_DATA_ADDR: installing there would have destroyed the
 * persistence record and the message sector on the first write. 0x220000
 * clears MSG_ADDR (0x201000 + one sector) by 120 KB.
 *
 *   flash   0x220000 .. 0x320000     1 MB, ends 896 KB below the 4 MB top
 *   vaddr 0x40300000 .. 0x40400000   the top of the IRAM0_CACHE window
 *   mmu entries 112 .. 127           64 + (0x300000 >> 16), 16 pages
 *
 * 1 MB against a measured 545 KB closure is deliberate headroom, not waste:
 * that figure is transmit-only with stubbed events, and scan or association
 * pull more of net80211 in. Flash here is not scarce -- 1.8 MB above the
 * message sector is otherwise unused. */
#define BLOB_FLASH_ADDR  0x220000u
#define BLOB_FLASH_SIZE  0x100000u
#define BLOB_IROM_ADDR   0x40300000u
#define BLOB_IROM_SIZE   0x100000u

/* The blob's writable memory. The MMU maps instructions; it does not populate
 * RAM, so .data must be COPIED here from inside the mapped image and .bss
 * zeroed, both at install time. The blob's own entry table carries the
 * addresses -- see vendor/net80211/blob_entry.c.
 *
 * This comes out of the kernel's dram one-for-one, and therefore out of the
 * heap, which is sized as whatever is left between _bss_end and the stack.
 * Measured need is 20,720 B (3,588 data + 16,600 bss); 32 KB leaves 36%
 * headroom for the event and scan paths that are still stubbed. */
#define BLOB_DRAM_ADDR   0x3FFD4000u
#define BLOB_DRAM_SIZE   0x8000u

/* The blob's read-only data, mapped through the DATA cache.
 *
 * This is not a refinement, it is required. .rodata placed in the instruction
 * window (0x40300000) serves 32-bit aligned accesses only, and vendor code
 * reads bytes and shorts out of its own rodata constantly. The first call into
 * the blob died with LoadStoreError at epc 0x40362b28 reaching excvaddr
 * 0x4037bd0c -- squarely inside .rodata.
 *
 * Splitting flash into IROM for code and DROM for rodata is exactly why
 * ESP-IDF does it, and boot.c already maps the kernel's own .flash.rodata this
 * way at MMU entry offset 0. This is the same trick for the blob.
 *
 *   rodata lives at image offset BLOB_RODATA_OFF, fixed and 64 KB aligned so
 *   the flash page and the virtual page line up with no fixup arithmetic.
 *
 *   flash 0x2A0000            = BLOB_FLASH_ADDR + BLOB_RODATA_OFF
 *   vaddr 0x3F700000          mmu entries 48.., offset 0 (DROM0)
 *
 * The kernel's own drom sits at 0x3F400020, entries 0-1, so there is no
 * collision. */
#define BLOB_RODATA_OFF   0x80000u
#define BLOB_DROM_ADDR    0x3F700000u
#define BLOB_DROM_SIZE    0x40000u

/* All three return 0 on success. Each masks interrupts for its duration; an
 * erase takes tens of milliseconds and will visibly delay the scheduler, so
 * they are not for calling from a hot path. */
int flash_read(uint32_t addr, void *dst, uint32_t len);
int flash_erase_sector(uint32_t addr);
int flash_write(uint32_t addr, const void *src, uint32_t len);

/* Chip identification, as a cheap proof the controller is actually talking:
 * a bus that is not working returns 0x000000 or 0xFFFFFF. */
uint32_t flash_read_id(void);

#endif /* NATOS_FLASH_H */
