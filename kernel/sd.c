/* nat-os — microSD over SPI. See sd.h for the mode and pin reasoning. */

#include "sd.h"
#include "gpio.h"
#include "xtensa.h"
#include "spi3.h"

#define CPU_HZ 80000000u

/* IO_MUX pad registers for the four SD pins.
 *
 * The table is NOT in pin order, and the trap is specific: the two UART0 pads
 * sit between GPIO22 and GPIO23.
 *
 *      0x7C GPIO21   0x80 GPIO22   0x84 U0RXD(GPIO3)   0x88 U0TXD(GPIO1)
 *      0x8C GPIO23
 *
 * Counting up from GPIO21 while forgetting those two puts GPIO23 at 0x84,
 * which is the UART's receive pad. Configuring it as a GPIO output silently
 * killed the console's receive path while transmit kept working, so the board
 * still printed telemetry and simply stopped answering — a shell that echoes
 * nothing looks like a hung shell, not a repointed pin.
 *
 * Cross-checking against gpio.h confirms the indexing but NOT this entry: GPIO2
 * at 0x40, GPIO12 at 0x34, GPIO14 at 0x30 and GPIO21 at 0x7C are all below the
 * UART pads, so every one of them agreed with the wrong answer. */

/* Half a bit period. Cards must accept 400 kHz or slower until initialisation
 * completes, and many refuse to identify at full speed — so the clock is slow
 * for identification and raised only once the card has been accepted. */
#define CLK_SLOW_US 2u          /* ~250 kHz */
#define CLK_FAST_US 0u          /* as fast as the loop runs */

static uint32_t g_half_us = CLK_SLOW_US;

/* SPI3 divider to use after identification, and whether the pins are on SPI3
 * right now. Inside the 25 MHz SPI-mode limit and the ~26 MHz full-duplex
 * ceiling of a GPIO-matrix input; raised only on evidence.
 *
 * [next_moves/12 step 5] 4 (20 MHz), was 8 (10 MHz). Video wants 340-430 KB/s,
 * and a frame's read cost fell from 58.4 ms to 42.2 ms at div 4, measured
 * during playback. div 2 remains refused below -- that is the one that
 * corrupted transfers and left the card unresponsive (11 step 3d). */
static uint32_t g_div_wanted = 4u;
static int      g_hw;
static uint32_t g_token_polls;  /* bytes read waiting for a data token */

/* [next_moves/12 step 8] Where a block read's time goes, in CPU cycles: the
 * command, the wait for the card's data token, and the data itself. Video
 * spends 80 ms a frame reading 61 KB that is only ~25 ms of wire time at
 * 20 MHz, so the rest is somewhere, and guessing which is how the last three
 * days went wrong. */
static uint32_t g_cc_cmd, g_cc_token, g_cc_data, g_blocks;

uint32_t sd_cc_cmd(void)   { return g_cc_cmd; }
uint32_t sd_cc_token(void) { return g_cc_token; }
uint32_t sd_cc_data(void)  { return g_cc_data; }
uint32_t sd_blocks(void)   { return g_blocks; }
static sd_type_t g_type;
static uint32_t g_last_r1 = 0xFF;
static uint32_t g_attempts;

/* SD commands used here. R1 is a single byte on all of them except CMD8 and
 * CMD58, which append a 32-bit payload. */
#define CMD0_GO_IDLE        0u
#define CMD8_SEND_IF_COND   8u
#define CMD16_SET_BLOCKLEN 16u
#define CMD12_STOP         12u
#define CMD17_READ_SINGLE  17u
#define CMD18_READ_MULTI   18u
#define CMD55_APP_CMD      55u
#define CMD58_READ_OCR     58u
#define ACMD41_SEND_OP_COND 41u

#define R1_IDLE             0x01u
#define R1_ILLEGAL_COMMAND  0x04u

#define DATA_TOKEN          0xFEu

static void delay_us(uint32_t us)
{
    if (!us) {
        return;
    }
    uint32_t start = xt_ccount();
    uint32_t want  = us * (CPU_HZ / 1000000u);
    while ((xt_ccount() - start) < want) {
    }
}

/* One byte out, one byte in. SD SPI is mode 0 and full duplex: every exchange
 * clocks a byte in each direction, and a "read" is a write of 0xFF. */
static uint8_t sd_xfer(uint8_t out)
{
    uint8_t in = 0;

    if (g_hw) {
        spi3_xfer(&out, &in, 1u);
        return in;
    }

    for (int i = 7; i >= 0; i--) {
        if ((out >> i) & 1u) {
            gpio_set(SD_PIN_MOSI);
        } else {
            gpio_clear(SD_PIN_MOSI);
        }

        delay_us(g_half_us);
        gpio_set(SD_PIN_SCK);           /* card samples MOSI on the rising edge */

        in = (uint8_t)((in << 1) | (uint8_t)gpio_read(SD_PIN_MISO));

        delay_us(g_half_us);
        gpio_clear(SD_PIN_SCK);
    }

    return in;
}

static uint8_t sd_rx(void)
{
    return sd_xfer(0xFFu);
}

/* CRC7 is checked by the card only while it is still in SPI-idle, which in
 * practice means CMD0 and CMD8. Rather than carry a table for two constants,
 * those two commands' CRCs are baked in and everything else sends a stop bit
 * with a dummy CRC, which the card ignores once running. */
static uint8_t crc_for(uint8_t cmd, uint32_t arg)
{
    if (cmd == CMD0_GO_IDLE) {
        return 0x95u;
    }
    if (cmd == CMD8_SEND_IF_COND && arg == 0x1AAu) {
        return 0x87u;
    }
    return 0x01u;                       /* stop bit only */
}

/* Sends a command and returns R1. 0xFF means the card never answered, which is
 * distinct from any legal R1 because bit 7 of R1 is always zero. */
static uint8_t sd_command(uint8_t cmd, uint32_t arg)
{
    /* A card may still be busy from the previous command. */
    for (int i = 0; i < 10; i++) {
        if (sd_rx() == 0xFFu) {
            break;
        }
    }

    sd_xfer((uint8_t)(0x40u | cmd));
    sd_xfer((uint8_t)(arg >> 24));
    sd_xfer((uint8_t)(arg >> 16));
    sd_xfer((uint8_t)(arg >> 8));
    sd_xfer((uint8_t)arg);
    sd_xfer(crc_for(cmd, arg));

    /* R1 arrives within 8 bytes on any conforming card. Bounded, because an
     * absent card holds MISO high forever and an unbounded loop here would
     * hang the boot on a machine with an empty slot — which is the normal
     * case, not the exceptional one. */
    uint8_t r1 = 0xFFu;
    for (int i = 0; i < 16; i++) {
        r1 = sd_rx();
        if ((r1 & 0x80u) == 0u) {
            break;
        }
    }

    g_last_r1 = r1;
    return r1;
}

static void cs_low(void)  { gpio_clear(SD_PIN_CS); }
static void cs_high(void)
{
    gpio_set(SD_PIN_CS);
    /* Eight extra clocks after deselect. The card needs them to finish its
     * internal work, and omitting them is a classic source of a card that
     * works for one command and then stops. */
    sd_rx();
}

int sd_init(void)
{
    g_attempts++;
    g_type    = SD_TYPE_NONE;
    g_half_us = CLK_SLOW_US;
    g_hw      = 0;              /* gpio_out_init below takes the pins back */

    gpio_out_init(SD_PIN_CS);
    gpio_out_init(SD_PIN_SCK);
    gpio_out_init(SD_PIN_MOSI);
    gpio_in_init(SD_PIN_MISO);

    gpio_set(SD_PIN_CS);
    gpio_clear(SD_PIN_SCK);
    gpio_set(SD_PIN_MOSI);

    /* At least 74 clocks with CS high and MOSI high, so the card can bring its
     * own supply up before it is addressed. */
    for (int i = 0; i < 10; i++) {
        sd_rx();
    }

    /* CMD0: enter SPI mode. The card answers 0x01 — idle, in SPI mode. This is
     * the step that fails when no card is present, and it is why every wait
     * above it is bounded. */
    cs_low();
    uint8_t r1 = 0xFFu;
    for (int tries = 0; tries < 8; tries++) {
        r1 = sd_command(CMD0_GO_IDLE, 0);
        if (r1 == R1_IDLE) {
            break;
        }
    }
    if (r1 != R1_IDLE) {
        cs_high();
        return SD_ERR_IDLE;
    }

    /* CMD8: declare a 2.7-3.6 V supply and a check pattern the card must echo.
     * A v2 card returns R1=0x01 plus four bytes ending in the pattern. */
    r1 = sd_command(CMD8_SEND_IF_COND, 0x1AAu);
    if (r1 & R1_ILLEGAL_COMMAND) {
        /* Pre-2.0 card. Not supported here rather than silently half-working:
         * the addressing and initialisation differ, and no such card has been
         * available to test against. Saying so beats guessing. */
        cs_high();
        return SD_ERR_IFCOND;
    }
    uint32_t ifcond = 0;
    for (int i = 0; i < 4; i++) {
        ifcond = (ifcond << 8) | sd_rx();
    }
    if ((ifcond & 0xFFFu) != 0x1AAu) {
        cs_high();
        return SD_ERR_IFCOND;
    }

    /* ACMD41 with the high-capacity bit, repeated until the card leaves idle.
     * Cards routinely take hundreds of milliseconds; the bound is generous and
     * finite. */
    int ready = 0;
    for (int tries = 0; tries < 2000; tries++) {
        sd_command(CMD55_APP_CMD, 0);
        r1 = sd_command(ACMD41_SEND_OP_COND, 0x40000000u);
        if (r1 == 0u) {
            ready = 1;
            break;
        }
        delay_us(1000);
    }
    if (!ready) {
        cs_high();
        return SD_ERR_READY;
    }

    /* CMD58: the OCR's CCS bit says whether the card is block-addressed. Get
     * this wrong and every read lands 512 times too far into the card, which
     * looks like corrupt data rather than a wrong address. */
    r1 = sd_command(CMD58_READ_OCR, 0);
    if (r1 != 0u) {
        cs_high();
        return SD_ERR_OCR;
    }
    uint32_t ocr = 0;
    for (int i = 0; i < 4; i++) {
        ocr = (ocr << 8) | sd_rx();
    }
    g_type = (ocr & 0x40000000u) ? SD_TYPE_SDHC : SD_TYPE_SDSC;

    if (g_type == SD_TYPE_SDSC) {
        r1 = sd_command(CMD16_SET_BLOCKLEN, SD_BLOCK_SIZE);
        if (r1 != 0u) {
            cs_high();
            return SD_ERR_BLOCKLEN;
        }
    }

    cs_high();
    g_half_us = CLK_FAST_US;    /* identification done; the bus can run up */

    if (g_div_wanted) {
        /* Hand SCK/MOSI/MISO to SPI3 through the matrix. CS stays a GPIO: a
         * block read holds it low across many transfers. */
        spi3_init();
        spi3_set_div(g_div_wanted);
        spi3_route(SD_PIN_SCK, SD_PIN_MOSI, SD_PIN_MISO);
        /* After spi3_init(), which resets the peripheral. Re-running it on a
         * re-init is deliberate: a hot-swapped card comes back through here. */
        spi3_dma_init();
        g_hw = 1;
    }
    return SD_OK;
}

void sd_set_speed(uint32_t div)
{
    /* Not below 4 (20 MHz). div 2 was tried: identification failed, and the
     * card stayed unresponsive across the next two re-inits. Garbled bits on
     * MOSI can decode as ANY command, a write included, so a speed the bus
     * cannot carry is a risk to the card's contents, not only a failed read. */
    if (div != 0u && div < 4u) {
        div = 4u;
    }
    g_div_wanted = div;
}

uint32_t sd_token_polls(void) { return g_token_polls; }

uint32_t sd_speed(void)
{
    return g_hw ? g_div_wanted : 0u;
}

/* ---- one block's two waits, shared by the single and multi-block reads ------
 *
 * Waits for a data token on the hardware bus. A batch that contains the token
 * also contains the data bytes behind it, so those are kept in `dst` and
 * reported in `*have` rather than being re-read. Returns the token byte, or
 * 0xFF if the card never sent one. */
static uint8_t token_wait_hw(uint8_t *dst, uint32_t *have)
{
    uint8_t batch[16];
    *have = 0;
    for (int i = 0; i < 300; i++) {
        if (!spi3_read(batch, sizeof batch)) {
            return 0xFFu;
        }
        g_token_polls += sizeof batch;
        for (uint32_t k = 0; k < sizeof batch; k++) {
            if (batch[k] != 0xFFu) {
                uint32_t n = (uint32_t)(sizeof batch - (k + 1u));
                for (uint32_t j = 0; j < n; j++) {
                    dst[j] = batch[k + 1u + j];
                }
                *have = n;
                return batch[k];
            }
        }
    }
    return 0xFFu;
}

/* The rest of a block, then its two discarded CRC bytes.
 *
 * By DMA where the destination allows it, which is nearly always and is worth
 * arranging for: the engine costs the CPU nothing per byte, against ~0.36 us a
 * byte through the W registers. Otherwise 64 bytes a transaction, the W
 * registers' whole capacity. */
static int data_read_hw(uint8_t *dst, uint32_t have)
{
    /* The token batch left `have` bytes in dst, so the rest starts at an
     * arbitrary offset -- and the DMA engine writes WORDS at a word boundary.
     * Three bytes on the slow path buy alignment for the other five hundred.
     * (512 minus a multiple of four is a multiple of four, so the length the
     * engine needs comes out right on its own.) */
    uint32_t pad = spi3_dma_enabled() ? ((4u - (have & 3u)) & 3u) : 0u;
    if (pad != 0u && have + pad <= SD_BLOCK_SIZE) {
        if (!spi3_read(dst + have, pad)) {
            return 0;
        }
        have += pad;
    }

    while (have < SD_BLOCK_SIZE) {
        uint32_t left = SD_BLOCK_SIZE - have;
        /* DMA is OFF by default here and the reason is in spi3_read_dma: this
         * function has just polled for the data token through the W registers,
         * and on this peripheral a W-register read permanently costs every
         * later DMA transfer 16 bytes. `spidma 1` turns it on for measuring.
         *
         * So the block is moved by the W registers, refilling the transmit side
         * with 0xFF for every chunk -- which is half the peripheral-bus traffic
         * and the reason a block costs 390 us against 205 us of wire time.
         *
         * Not refilling was tried, on the theory that a card streaming a block
         * cannot see MOSI. It can, and it must: during a MULTI-BLOCK read the
         * card watches MOSI for CMD12, which is the only way the stream ever
         * stops. Sending the previous chunk's bytes instead of ones put a
         * 0x40-prefixed byte in front of it soon enough to break the very first
         * read -- "mount failed: no FAT boot sector" on a healthy card. */
        if (spi3_dma_enabled()) {
            uint32_t n = (left > SPI3_DMA_MAX) ? SPI3_DMA_MAX : left;
            if (!spi3_read_dma(dst + have, n)) {
                /* ABANDON the block. Do not re-read it on the slow path.
                 *
                 * A DMA transfer that failed still CLOCKED: those bytes have
                 * left the card and are gone. Reading them again returns the
                 * NEXT n bytes, so the block ends up assembled from two
                 * different places -- which is what "mount failed: no FAT boot
                 * sector" was, on a card whose block 0 is perfectly fine.
                 *
                 * The same mistake display.c records for its transmit path
                 * (UM-NATOS-031 §2), made again on the receive side. The
                 * failure also disabled DMA, so the caller's retry runs
                 * entirely on the W-register path and succeeds. */
                return 0;
            }
            have += n;
        } else {
            uint32_t n = (left > SPI3_XFER_MAX) ? SPI3_XFER_MAX : left;
            if (!spi3_read(dst + have, n)) {
                return 0;
            }
            have += n;
        }
    }

    /* The two CRC bytes. 0xFF again from here: the card starts listening for
     * the next command as soon as the block ends. */
    uint8_t crc[2];
    return spi3_read(crc, sizeof crc);
}

/* ---- multi-block reads (next_moves/12 step 8) -------------------------------
 *
 * Measured per block on this card at 20 MHz: command 39 us, waiting for the
 * data token 347 us, moving the 512 bytes 405 us. The middle one is the card's
 * own access latency and it is paid once per COMMAND, not once per sector --
 * so a run of consecutive sectors read with CMD18 pays it once instead of once
 * each. That is where video's read budget was going: 120 sectors a frame.
 *
 * The stream is open-ended until CMD12 stops it, so every early return has to
 * send the stop -- a card left streaming answers the next command with data. */
static uint32_t g_multi_bursts, g_multi_blocks;
static uint32_t g_dma_retries;      /* blocks re-read after a DMA failure */

uint32_t sd_multi_bursts(void) { return g_multi_bursts; }
uint32_t sd_multi_blocks(void) { return g_multi_blocks; }

static void multi_stop(void)
{
    sd_command(CMD12_STOP, 0);
    /* R1 for CMD12 is followed by a busy stretch of 0x00 while the card tidies
     * up. Its VALUE is not checked: the card may still have been sending the
     * block after the last one we wanted, and a data byte can look like any
     * R1. What matters is that the bus is idle (0xFF) again before the next
     * command, which is what this waits for. */
    for (int i = 0; i < 1000; i++) {
        if (sd_rx() == 0xFFu) {
            break;
        }
    }
    cs_high();
}

static int read_blocks_once(uint32_t lba, uint32_t count, uint8_t *dst)
{
    if (count == 0u) {
        return SD_OK;
    }
    /* One block, or the bit-banged bus, goes the proven way. Below three
     * blocks CMD18 saves one latency and costs a CMD12, which is close enough
     * to a wash that it is not worth a second code path being exercised. */
    if (count < 3u || !g_hw) {
        for (uint32_t i = 0; i < count; i++) {
            int rc = sd_read_block(lba + i, dst + i * SD_BLOCK_SIZE);
            if (rc != SD_OK) {
                return rc;
            }
        }
        return SD_OK;
    }
    if (g_type == SD_TYPE_NONE) {
        return SD_ERR_IDLE;
    }

    uint32_t addr = (g_type == SD_TYPE_SDHC) ? lba : lba * SD_BLOCK_SIZE;

    uint32_t t0 = xt_ccount();
    cs_low();
    if (sd_command(CMD18_READ_MULTI, addr) != 0u) {
        multi_stop();
        return SD_ERR_READ;
    }
    g_cc_cmd += xt_ccount() - t0;
    g_multi_bursts++;

    for (uint32_t b = 0; b < count; b++) {
        uint8_t *p = dst + b * SD_BLOCK_SIZE;
        uint32_t t1 = xt_ccount(), have = 0;
        uint8_t token = token_wait_hw(p, &have);
        uint32_t t2 = xt_ccount();
        g_cc_token += t2 - t1;
        if (token != DATA_TOKEN) {
            multi_stop();
            return SD_ERR_TOKEN;
        }
        if (!data_read_hw(p, have)) {
            multi_stop();
            return SD_ERR_TOKEN;
        }
        g_cc_data += xt_ccount() - t2;
        g_blocks++;
        g_multi_blocks++;
    }

    multi_stop();
    return SD_OK;
}

static int read_block_once(uint32_t lba, uint8_t *dst)
{
    if (g_type == SD_TYPE_NONE) {
        return SD_ERR_IDLE;
    }

    /* SDSC addresses bytes, SDHC addresses blocks. The caller always speaks in
     * blocks, so the conversion lives here and cannot be forgotten at a call
     * site. */
    uint32_t addr = (g_type == SD_TYPE_SDHC) ? lba : lba * SD_BLOCK_SIZE;

    uint32_t t0 = xt_ccount();
    cs_low();
    if (sd_command(CMD17_READ_SINGLE, addr) != 0u) {
        cs_high();
        return SD_ERR_READ;
    }
    uint32_t t1 = xt_ccount();

    /* The card sends 0xFF until its data token. Bounded for the usual reason.
     *
     * [next_moves/12 step 8] In BATCHES on the hardware bus. This card takes
     * ~60 bytes to answer, and one byte per transfer cost 343 us a block --
     * half the time a block took, for bytes that are all 0xFF. A batch that
     * contains the token also contains the first data bytes behind it, so
     * they are kept rather than re-read. */
    uint8_t token = 0xFFu;
    uint32_t have = 0;                  /* data bytes already in dst */
    if (g_hw) {
        token = token_wait_hw(dst, &have);
    } else {
        for (int i = 0; i < 4000; i++) {
            token = sd_rx();
            g_token_polls++;
            if (token != 0xFFu) {
                break;
            }
        }
    }
    uint32_t t2 = xt_ccount();
    g_cc_cmd += t1 - t0;
    g_cc_token += t2 - t1;
    if (token != DATA_TOKEN) {
        cs_high();
        return SD_ERR_TOKEN;
    }

    if (g_hw) {
        /* `have` bytes already arrived in the batch that carried the token.
         * The two CRC bytes at the end are read and discarded: the SPI-mode
         * CRC is off by default and the bytes are still sent, so not consuming
         * them leaves the bus out of step for the next command. */
        if (!data_read_hw(dst, have)) {
            cs_high();
            return SD_ERR_TOKEN;
        }
    } else {
        for (uint32_t i = 0; i < SD_BLOCK_SIZE; i++) {
            dst[i] = sd_rx();
        }
        sd_rx();
        sd_rx();
    }
    g_cc_data += xt_ccount() - t2;
    g_blocks++;

    cs_high();
    return SD_OK;
}

/* ---- one retry, and only for the one cause ---------------------------------
 *
 * A DMA transfer that fails takes its bytes with it (see data_read_hw), so the
 * block is abandoned rather than patched -- and the same failure disables the
 * engine. That makes exactly one retry worth having: it runs on the W-register
 * path that has read this card for two weeks.
 *
 * Conditional on the engine having JUST been disabled, not on failure alone. A
 * card that is absent, or wedged, must fail at the same speed it always did;
 * blanket retries are how a driver turns a clear failure into a slow one. */
static int retried(int rc, int dma_before)
{
    return rc != SD_OK && dma_before && !spi3_dma_enabled();
}

int sd_read_block(uint32_t lba, uint8_t *dst)
{
    int dma = spi3_dma_enabled();
    int rc = read_block_once(lba, dst);
    if (retried(rc, dma)) {
        g_dma_retries++;
        rc = read_block_once(lba, dst);
    }
    return rc;
}

int sd_read_blocks(uint32_t lba, uint32_t count, uint8_t *dst)
{
    int dma = spi3_dma_enabled();
    int rc = read_blocks_once(lba, count, dst);
    if (retried(rc, dma)) {
        g_dma_retries++;
        rc = read_blocks_once(lba, count, dst);
    }
    return rc;
}

uint32_t sd_dma_retries(void) { return g_dma_retries; }

sd_type_t sd_type(void)          { return g_type; }
uint32_t  sd_last_r1(void)       { return g_last_r1; }
uint32_t  sd_init_attempts(void) { return g_attempts; }
