/* nat-os — microSD over SPI. See sd.h for the mode and pin reasoning. */

#include "sd.h"
#include "gpio.h"
#include "xtensa.h"

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
#define IO_MUX_GPIO5   0x3FF4906Cu
#define IO_MUX_GPIO18  0x3FF49070u
#define IO_MUX_GPIO19  0x3FF49074u
#define IO_MUX_GPIO23  0x3FF4908Cu

/* Half a bit period. Cards must accept 400 kHz or slower until initialisation
 * completes, and many refuse to identify at full speed — so the clock is slow
 * for identification and raised only once the card has been accepted. */
#define CLK_SLOW_US 2u          /* ~250 kHz */
#define CLK_FAST_US 0u          /* as fast as the loop runs */

static uint32_t g_half_us = CLK_SLOW_US;
static sd_type_t g_type;
static uint32_t g_last_r1 = 0xFF;
static uint32_t g_attempts;

/* SD commands used here. R1 is a single byte on all of them except CMD8 and
 * CMD58, which append a 32-bit payload. */
#define CMD0_GO_IDLE        0u
#define CMD8_SEND_IF_COND   8u
#define CMD16_SET_BLOCKLEN 16u
#define CMD17_READ_SINGLE  17u
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

    gpio_out_init(SD_PIN_CS,   IO_MUX_GPIO5);
    gpio_out_init(SD_PIN_SCK,  IO_MUX_GPIO18);
    gpio_out_init(SD_PIN_MOSI, IO_MUX_GPIO23);
    gpio_in_init (SD_PIN_MISO, IO_MUX_GPIO19);

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
    return SD_OK;
}

int sd_read_block(uint32_t lba, uint8_t *dst)
{
    if (g_type == SD_TYPE_NONE) {
        return SD_ERR_IDLE;
    }

    /* SDSC addresses bytes, SDHC addresses blocks. The caller always speaks in
     * blocks, so the conversion lives here and cannot be forgotten at a call
     * site. */
    uint32_t addr = (g_type == SD_TYPE_SDHC) ? lba : lba * SD_BLOCK_SIZE;

    cs_low();
    if (sd_command(CMD17_READ_SINGLE, addr) != 0u) {
        cs_high();
        return SD_ERR_READ;
    }

    /* The card sends 0xFF until its data token. Bounded for the usual reason. */
    uint8_t token = 0xFFu;
    for (int i = 0; i < 4000; i++) {
        token = sd_rx();
        if (token != 0xFFu) {
            break;
        }
    }
    if (token != DATA_TOKEN) {
        cs_high();
        return SD_ERR_TOKEN;
    }

    for (uint32_t i = 0; i < SD_BLOCK_SIZE; i++) {
        dst[i] = sd_rx();
    }

    /* Two CRC bytes, discarded. The SPI-mode CRC is off by default and the
     * bytes are still sent; not consuming them leaves the bus out of step for
     * the next command. */
    sd_rx();
    sd_rx();

    cs_high();
    return SD_OK;
}

sd_type_t sd_type(void)          { return g_type; }
uint32_t  sd_last_r1(void)       { return g_last_r1; }
uint32_t  sd_init_attempts(void) { return g_attempts; }
