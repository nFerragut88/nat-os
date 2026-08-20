/* nat-os — SPI flash read/erase/write. See flash.h for the cache argument. */

#include "flash.h"

/* The blob reservation is a set of addresses that must agree with each other,
 * with the store's sector, and with the MMU's page size. Checked here rather
 * than trusted, because the first proposal collided with FLASH_DATA_ADDR and
 * nothing in the build would have said so. */
_Static_assert((BLOB_FLASH_ADDR % 0x10000u) == 0u,
               "blob flash offset must be 64 KB aligned -- the MMU maps 64 KB pages");
_Static_assert((BLOB_FLASH_SIZE % 0x10000u) == 0u,
               "blob region size must be a whole number of 64 KB MMU pages");
_Static_assert((BLOB_IROM_ADDR % 0x10000u) == 0u,
               "blob virtual base must be 64 KB aligned");
_Static_assert(BLOB_IROM_SIZE == BLOB_FLASH_SIZE,
               "the mapped window and the flash region must be the same size");
_Static_assert(BLOB_FLASH_ADDR >= FLASH_DATA_ADDR + (2u * FLASH_SECTOR),
               "blob region overlaps the store record or the message sector");
_Static_assert(BLOB_FLASH_ADDR + BLOB_FLASH_SIZE <= 0x400000u,
               "blob region runs past the end of a 4 MB flash");
_Static_assert(BLOB_IROM_ADDR + BLOB_IROM_SIZE <= 0x40400000u,
               "blob window runs past the top of IRAM0_CACHE");
_Static_assert((BLOB_RODATA_OFF % 0x10000u) == 0u,
               "rodata image offset must be 64 KB aligned to map cleanly");
_Static_assert((BLOB_DROM_ADDR % 0x10000u) == 0u,
               "blob drom base must be 64 KB aligned");
_Static_assert(BLOB_DROM_ADDR >= 0x3F400000u &&
               BLOB_DROM_ADDR + BLOB_DROM_SIZE <= 0x3F800000u,
               "blob drom window must lie inside DROM0");
_Static_assert(BLOB_RODATA_OFF + BLOB_DROM_SIZE <= BLOB_FLASH_SIZE,
               "rodata would run past the flash reservation");
_Static_assert((BLOB_DRAM_ADDR % 4u) == 0u,
               "blob dram base must be word aligned");
_Static_assert(BLOB_DRAM_ADDR + BLOB_DRAM_SIZE <= 0x3FFE0000u,
               "blob dram region runs into the ROM's reserved area");
#include "critical.h"
#include "xtensa.h"

#define REG(a) (*(volatile uint32_t *)(a))

/* SPI1 is the flash controller. Same register layout as SPI2, which is why this
 * driver drives it in user mode rather than through the dedicated flash-opcode
 * bits: an arbitrary command/address/data sequence is exactly what a SPI NOR
 * chip wants anyway, and the user path is already proven by the display. */
#define SPI1_BASE          0x3FF42000u
#define SPI1_CMD           (SPI1_BASE + 0x00u)
#define SPI1_ADDR          (SPI1_BASE + 0x04u)
#define SPI1_CTRL          (SPI1_BASE + 0x08u)
#define SPI1_CTRL2         (SPI1_BASE + 0x14u)
#define SPI1_USER          (SPI1_BASE + 0x1Cu)
#define SPI1_USER1         (SPI1_BASE + 0x20u)
#define SPI1_USER2         (SPI1_BASE + 0x24u)
#define SPI1_MOSI_DLEN     (SPI1_BASE + 0x28u)
#define SPI1_MISO_DLEN     (SPI1_BASE + 0x2Cu)
#define SPI1_W(n)          (SPI1_BASE + 0x80u + 4u * (n))

#define SPI_USR_BIT        (1u << 18)
#define USR_COMMAND        (1u << 31)
#define USR_ADDR           (1u << 30)
#define USR_MISO           (1u << 28)
#define USR_MOSI           (1u << 27)

/* SPI NOR opcodes. Universal across the JEDEC-compatible parts these boards
 * carry, which is why they are not conditional on the chip id. */
#define CMD_WREN   0x06u
#define CMD_RDSR   0x05u
#define CMD_READ   0x03u
#define CMD_PP     0x02u        /* page program, 256 bytes max */
#define CMD_SE     0x20u        /* sector erase, 4 KB          */
#define CMD_RDID   0x9Fu

#define SR_WIP     0x01u        /* write in progress */

/* Bounded so a chip that never answers costs a delay rather than the system.
 * A sector erase is tens of milliseconds; this allows roughly a second. */
#define BUSY_TIMEOUT_CYCLES 80000000u

/* One user-mode transaction. Any of command, address, write data and read data
 * may be absent. Up to 64 bytes each way — the sixteen W registers are the
 * whole FIFO, which is also why a page program is split by the caller. */
/* Fast-read mode bits in SPI_CTRL. The bootloader leaves the controller in dual
 * or quad IO for cache reads; a user command issued in that mode goes out on
 * more than one line and the chip sees nonsense. */
#define CTRL_FASTRD_MODE   (1u << 13)
#define CTRL_FREAD_DUAL    (1u << 14)
#define CTRL_FREAD_QUAD    (1u << 20)
#define CTRL_FREAD_DIO     (1u << 23)
#define CTRL_FREAD_QIO     (1u << 24)
#define CTRL_FREAD_MASK    (CTRL_FASTRD_MODE | CTRL_FREAD_DUAL | CTRL_FREAD_QUAD |                             CTRL_FREAD_DIO | CTRL_FREAD_QIO)

#define SPI1_CLOCK         (SPI1_BASE + 0x18u)

/* 80 MHz / (pre+1) / (n+1), with the high phase at half. */
#define CLK_DIV(pre, n) (((pre) << 18) | ((n) << 12) | ((((n) + 1u) / 2u - 1u) << 6) | (n))

/* ~8 MHz for user commands, explicitly set rather than inherited.
 *
 * This was the whole of the read defect. The bootloader leaves SPI1's divider
 * configured for its own fast cache reads, and at that rate a user transaction
 * sampled MISO one clock early: every byte came back as the true value shifted
 * right once, with a spurious leading zero. RDID read 0x34200B where the part
 * reports 0x684016.
 *
 * Two things made this hard to see. The shift was perfectly consistent, so it
 * looked like a framing error in the command phase rather than a timing one —
 * and moving the command out of USR_COMMAND into the MOSI stream changed
 * nothing at all, which is what finally ruled that out. A sweep across four
 * dividers and four sampling edges settled it in one boot: every explicit
 * divider read correctly at every edge, and the inherited one failed at all of
 * them.
 *
 * These operations are rare and small, so there is nothing to be gained by
 * running the bus near its limit. */
#define FLASH_USER_CLOCK   CLK_DIV(0u, 9u)

static int spi1_xfer(uint8_t cmd, int use_addr, uint32_t addr,
                     const uint8_t *tx, uint32_t txlen,
                     uint8_t *rx, uint32_t rxlen)
{
    /* The command and address travel as ordinary MOSI bytes rather than through
     * USR_COMMAND / USR_ADDR.
     *
     * Those two phases have their own bit-length fields, and the command one was
     * off by a clock: every byte read back arrived as the true value shifted
     * right once (RDID gave 0x34200B where the part reports 0x684016), which is
     * what sampling MISO one clock early looks like — a spurious leading bit
     * pushing the real data down. Rather than guess whether that field counts
     * bits or bits-minus-one, this uses the MOSI length field instead, which the
     * display driver has already proven correct at every length it uses.
     *
     * A SPI NOR part cannot tell the difference. Command, address and data are
     * one continuous stream of bytes to the chip; the phase split is a
     * convenience of this controller, not something on the wire. */
    uint8_t hdr[8];
    uint32_t hlen = 0;
    hdr[hlen++] = cmd;
    if (use_addr) {
        hdr[hlen++] = (uint8_t)(addr >> 16);
        hdr[hlen++] = (uint8_t)(addr >> 8);
        hdr[hlen++] = (uint8_t)addr;
    }

    uint32_t user = 0;

    /* SPI1 shares the flash bus with the cache's SPI0, and these registers are
     * how the cache issues its own reads. Overwriting them and walking away
     * leaves the cache unable to read flash AT ALL — which presented as every
     * string literal in the kernel returning 0xFF immediately after the first
     * flash operation. Saved here, restored unconditionally on the way out. */
    uint32_t save_user  = REG(SPI1_USER);
    uint32_t save_user1 = REG(SPI1_USER1);
    uint32_t save_user2 = REG(SPI1_USER2);
    uint32_t save_ctrl  = REG(SPI1_CTRL);
    uint32_t save_ctrl2 = REG(SPI1_CTRL2);
    uint32_t save_clock = REG(SPI1_CLOCK);
    REG(SPI1_CLOCK) = FLASH_USER_CLOCK;

    REG(SPI1_CTRL) = save_ctrl & ~CTRL_FREAD_MASK;

    /* CTRL2 carries the MISO sampling delay the bootloader tuned for its own
     * fast reads. Zeroed here because this driver sets its own clock and wants
     * no compensation on top of it; restored on the way out. Zeroing it alone
     * does NOT fix the read shift — that was the divider, above. */
    REG(SPI1_CTRL2) = 0;

    /* Wait for any previous transaction to retire before touching the
     * registers underneath it. */
    uint32_t start = xt_ccount();
    while (REG(SPI1_CMD) & SPI_USR_BIT) {
        if ((xt_ccount() - start) > BUSY_TIMEOUT_CYCLES) {
            goto restore_fail;
        }
    }

    /* No command or address phase — both are in the MOSI stream below. */
    REG(SPI1_USER2) = 0;
    REG(SPI1_USER1) = 0;

    /* Header then payload, packed little-endian into the W registers because
     * that is the order the shifter reads them out. */
    uint32_t total = hlen + txlen;
    user |= USR_MOSI;
    for (uint32_t w = 0; w < (total + 3u) / 4u; w++) {
        uint32_t word = 0;
        for (uint32_t b = 0; b < 4u; b++) {
            uint32_t i = w * 4u + b;
            if (i < total) {
                uint8_t v = (i < hlen) ? hdr[i] : tx[i - hlen];
                word |= (uint32_t)v << (8u * b);
            }
        }
        REG(SPI1_W(w)) = word;
    }
    REG(SPI1_MOSI_DLEN) = total * 8u - 1u;

    if (rxlen) {
        user |= USR_MISO;
        REG(SPI1_MISO_DLEN) = rxlen * 8u - 1u;
    } else {
        REG(SPI1_MISO_DLEN) = 0;
    }

    REG(SPI1_USER) = user;
    REG(SPI1_CMD)  = SPI_USR_BIT;

    start = xt_ccount();
    while (REG(SPI1_CMD) & SPI_USR_BIT) {
        if ((xt_ccount() - start) > BUSY_TIMEOUT_CYCLES) {
            goto restore_fail;
        }
    }

    if (rxlen) {
        for (uint32_t i = 0; i < rxlen; i++) {
            uint32_t word = REG(SPI1_W(i / 4u));
            rx[i] = (uint8_t)(word >> (8u * (i % 4u)));
        }
    }

    REG(SPI1_USER)  = save_user;
    REG(SPI1_USER1) = save_user1;
    REG(SPI1_USER2) = save_user2;
    REG(SPI1_CTRL)  = save_ctrl;
    REG(SPI1_CTRL2) = save_ctrl2;
    REG(SPI1_CLOCK) = save_clock;
    return 0;

restore_fail:
    REG(SPI1_USER)  = save_user;
    REG(SPI1_USER1) = save_user1;
    REG(SPI1_USER2) = save_user2;
    REG(SPI1_CTRL)  = save_ctrl;
    REG(SPI1_CTRL2) = save_ctrl2;
    REG(SPI1_CLOCK) = save_clock;
    return -1;
}

static int flash_wait_ready(void)
{
    uint32_t start = xt_ccount();
    for (;;) {
        uint8_t sr = 0xFF;
        if (spi1_xfer(CMD_RDSR, 0, 0, 0, 0, &sr, 1) != 0) {
            return -1;
        }
        if ((sr & SR_WIP) == 0u) {
            return 0;
        }
        if ((xt_ccount() - start) > BUSY_TIMEOUT_CYCLES) {
            return -1;
        }
    }
}

uint32_t flash_read_id(void)
{
    uint8_t id[3] = { 0, 0, 0 };
    uint32_t crit = crit_enter();
    int rc = spi1_xfer(CMD_RDID, 0, 0, 0, 0, id, 3);
    crit_exit(crit);
    if (rc != 0) {
        return 0;
    }
    return ((uint32_t)id[0] << 16) | ((uint32_t)id[1] << 8) | id[2];
}

int flash_read(uint32_t addr, void *dst, uint32_t len)
{
    uint8_t *out = (uint8_t *)dst;
    uint32_t crit = crit_enter();

    while (len) {
        uint32_t chunk = (len > 64u) ? 64u : len;
        if (spi1_xfer(CMD_READ, 1, addr, 0, 0, out, chunk) != 0) {
            crit_exit(crit);
            return -1;
        }
        addr += chunk;
        out  += chunk;
        len  -= chunk;
    }

    crit_exit(crit);
    return 0;
}

int flash_erase_sector(uint32_t addr)
{
    /* Refuse anything outside the data region. The bootloader, the partition
     * table and the application image all live below it, and an erase is not an
     * operation to get wrong twice. */
    if (addr < FLASH_DATA_ADDR) {
        return -1;
    }
    addr &= ~(FLASH_SECTOR - 1u);

    /* ---- READ THIS BEFORE NARROWING THE CRITICAL SECTION -----------------
     *
     * This blocks for ~125 ms, measured (next_moves/04). Almost all of it is
     * flash_wait_ready() spinning on the status register while the chip erases;
     * the two SPI transactions above it are microseconds. Interrupts are masked
     * for the whole of it, so nothing else in the system runs.
     *
     * The obvious fix is to leave the critical section during the wait, since
     * polling a status register plainly does not require interrupts off. That
     * fix is WRONG AS IT STANDS, and the reason is not in this file:
     *
     *     While WIP is set, an SPI NOR chip answers RDSR and little else. It
     *     cannot serve a read. And since UM-NATOS-037, shell.c and kmain.c
     *     EXECUTE FROM FLASH -- so letting the scheduler run during an erase
     *     may enter a task whose instructions cannot be fetched.
     *
     * So this critical section is doing two jobs, and only one of them is
     * obvious. It stops preemption, and by stopping preemption it stops
     * flash-resident code from running while the chip is busy. Narrowing it
     * without addressing the second job produces a hang whose cause is three
     * files away from the change.
     *
     * Three ways out, none of them free, listed so the next person does not
     * have to re-derive them:
     *
     *   1. Every task that could run during an erase becomes IRAM-resident.
     *      Correct, and much larger than it sounds. You do not choose which
     *      task the scheduler picks, so "might run" means all of them -- and
     *      kmain.c, which is in flash since UM-NATOS-037, holds EVERY task's
     *      entry function. Today this means moving kmain.c back, undoing half
     *      of that report. Splitting it is possible but requires proving every
     *      reachable path stays in RAM, which is an audit and not an attribute.
     *
     *   2. Erase-suspend. RULED OUT, not merely uninvestigated. The cache
     *      fetches in hardware and there is no software hook on a miss, so
     *      suspend only helps if the flash CONTROLLER issues it automatically.
     *      Espressif built exactly that -- SOC_SPI_MEM_SUPPORT_AUTO_SUSPEND --
     *      and shipped it on the C2, C3, C6, H2, S2 and S3. The original ESP32
     *      is the one part that does not have it.
     *
     *   3. Do not erase while timing matters. Free, needs nothing in this file,
     *      and is the recommendation in next_moves/04 and /10. The work is
     *      elsewhere: store_save() currently fires blindly from the render loop
     *      every 256 frames and asks nobody whether now is a good moment. It
     *      needs a caller willing to be told when it may write.
     *
     * 2 is gone and 1 costs the IROM headroom UM-NATOS-037 just bought, to
     * solve a problem that only exists during a LoRa receive window. 3 is the
     * answer.
     *
     * Until one of those is done, 125 ms is the cost and it is a known one. */
    uint32_t crit = crit_enter();
    int rc = spi1_xfer(CMD_WREN, 0, 0, 0, 0, 0, 0);
    if (rc == 0) {
        rc = spi1_xfer(CMD_SE, 1, addr, 0, 0, 0, 0);
    }
    if (rc == 0) {
        rc = flash_wait_ready();
    }
    crit_exit(crit);
    return rc;
}

int flash_write(uint32_t addr, const void *src, uint32_t len)
{
    if (addr < FLASH_DATA_ADDR) {
        return -1;
    }

    const uint8_t *in = (const uint8_t *)src;
    uint32_t crit = crit_enter();

    while (len) {
        /* A page program may not cross a 256-byte page boundary, and this
         * controller's FIFO caps a transaction at 64 bytes regardless. */
        uint32_t page_left = 256u - (addr & 255u);
        uint32_t chunk = len;
        if (chunk > page_left) { chunk = page_left; }
        /* 60, not 64: the command and 24-bit address now ride in the same MOSI
         * stream as the data, and the sixteen W registers are the whole FIFO. */
        if (chunk > 60u)       { chunk = 60u; }

        int rc = spi1_xfer(CMD_WREN, 0, 0, 0, 0, 0, 0);
        if (rc == 0) {
            rc = spi1_xfer(CMD_PP, 1, addr, in, chunk, 0, 0);
        }
        if (rc == 0) {
            rc = flash_wait_ready();
        }
        if (rc != 0) {
            crit_exit(crit);
            return -1;
        }

        addr += chunk;
        in   += chunk;
        len  -= chunk;
    }

    crit_exit(crit);
    return 0;
}
