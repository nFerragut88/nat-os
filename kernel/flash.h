/* cyd-os — SPI flash read/erase/write.
 *
 * The first thing in this kernel that survives a power cycle.
 *
 * ---- the cache hazard ---------------------------------------------------
 *
 * `.rodata` is mapped from flash through the data cache (UM-CYDOS-011). A flash
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

#ifndef CYDOS_FLASH_H
#define CYDOS_FLASH_H

#include <stdint.h>

/* Compile-time disable, kept because a flash driver is the one thing here that
 * can damage state a reset does not clear. */
#define FLASH_ENABLE 1

#define FLASH_SECTOR     4096u
#define FLASH_DATA_ADDR  0x200000u      /* 2 MB in; clear of everything */

/* All three return 0 on success. Each masks interrupts for its duration; an
 * erase takes tens of milliseconds and will visibly delay the scheduler, so
 * they are not for calling from a hot path. */
int flash_read(uint32_t addr, void *dst, uint32_t len);
int flash_erase_sector(uint32_t addr);
int flash_write(uint32_t addr, const void *src, uint32_t len);

/* Chip identification, as a cheap proof the controller is actually talking:
 * a bus that is not working returns 0x000000 or 0xFFFFFF. */
uint32_t flash_read_id(void);

#endif /* CYDOS_FLASH_H */
