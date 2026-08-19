/* nat-os — SPI3 (VSPI) master. See spi3.h for why this exists separately. */

#include "spi3.h"
#include "gpio.h"
#include "xtensa.h"

/* ---- register map -------------------------------------------------------
 *
 * Every constant below was read out of soc/spi_reg.h, soc/gpio_reg.h,
 * soc/gpio_sig_map.h, soc/gpio_pins.h and soc/dport_reg.h rather than recalled.
 *
 * That is not caution for its own sake. This project has now lost a day to
 * OUTLINK_START being one bit from OUTLINK_RESTART (UM-NATOS-030), and then
 * found DMA_OUT_RST sitting on the inbound channel and OUTDSCR_BURST on the
 * inbound descriptor burst (UM-NATOS-033) -- three wrong bits in one peripheral,
 * every one of them a real neighbouring bit the hardware accepted in silence.
 * A fourth would be a pattern rather than bad luck.
 *
 * Two in particular are worth naming because they are not what a reasonable
 * person would guess:
 *
 *   DPORT_SPI3_CLK_EN is BIT(16), not the bit after SPI2's BIT(6).
 *   VSPIQ and VSPID share index numbers between their _IN and _OUT forms, so
 *   the direction is carried by which matrix register you write, not by the
 *   index.
 */
#define SPI3_BASE          0x3FF65000u
#define SPI3_CMD           (SPI3_BASE + 0x00u)
#define SPI3_CTRL          (SPI3_BASE + 0x08u)
#define SPI3_CLOCK         (SPI3_BASE + 0x18u)
#define SPI3_USER          (SPI3_BASE + 0x1Cu)
#define SPI3_USER1         (SPI3_BASE + 0x20u)
#define SPI3_USER2         (SPI3_BASE + 0x24u)
#define SPI3_MOSI_DLEN     (SPI3_BASE + 0x28u)
#define SPI3_MISO_DLEN     (SPI3_BASE + 0x2Cu)
#define SPI3_PIN           (SPI3_BASE + 0x34u)
#define SPI3_W(n)          (SPI3_BASE + 0x80u + 4u * (n))

#define SPI_USR_BIT        (1u << 18)
#define SPI_DOUTDIN_BIT    (1u << 0)    /* full duplex                        */
#define SPI_USR_MOSI_BIT   (1u << 27)
#define SPI_USR_MISO_BIT   (1u << 28)
#define SPI_CS0_DIS_BIT    (1u << 0)    /* PIN: hardware CS0 off              */
#define SPI_CS1_DIS_BIT    (1u << 1)
#define SPI_CS2_DIS_BIT    (1u << 2)

#define DPORT_PERIP_CLK_EN 0x3FF000C0u
#define DPORT_PERIP_RST_EN 0x3FF000C4u
#define DPORT_SPI3_BIT     (1u << 16)

/* 80 MHz / ((0+1) * (39+1)) = 2 MHz. pre=0 n=39 h=19 l=39.
 *
 * Deliberately slow to begin with. The SX1262 is specified to 16 MHz and this
 * has room to rise later, but a bring-up that fails at speed is indisputable
 * about the peripheral and ambiguous about the wiring, and the wiring does not
 * exist yet. */
#define SPI3_CLKDIV        0x000274E7u

/* GPIO matrix. FUNC_OUT_SEL is already in gpio.h; the input half is not. */
#define GPIO_FUNC_IN_SEL(sig)  (0x3FF44130u + 4u * (sig))
#define GPIO_SIG_IN_SEL_BIT    (1u << 7)   /* take the signal from the matrix */

#define VSPICLK_OUT_IDX   63u
#define VSPIQ_IN_IDX      64u    /* MISO into the peripheral */
#define VSPID_OUT_IDX     65u    /* MOSI out of the peripheral */

/* Tie a peripheral input to a level without involving a pin. */
#define MATRIX_CONST_ONE   0x38u
#define MATRIX_CONST_ZERO  0x30u

static uint32_t g_transfers;
static uint32_t g_timeouts;

uint32_t spi3_transfers(void) { return g_transfers; }
uint32_t spi3_timeouts(void)  { return g_timeouts; }

void spi3_init(void)
{
    /* Read-modify-write, never a plain store: this register also gates the
     * flash controller this code is executing from. */
    GPIO_REG(DPORT_PERIP_CLK_EN) |= DPORT_SPI3_BIT;
    GPIO_REG(DPORT_PERIP_RST_EN) &= ~DPORT_SPI3_BIT;

    GPIO_REG(SPI3_CLOCK) = SPI3_CLKDIV;

    /* Full duplex, both phases enabled. DOUTDIN is the bit that makes the
     * received bits land in the W registers alongside the transmitted ones;
     * without it this is a write-only port with a read that always returns
     * whatever was last written -- which is the single most convincing wrong
     * answer an SPI driver can give, because it looks exactly like an echo. */
    GPIO_REG(SPI3_USER)  = SPI_DOUTDIN_BIT | SPI_USR_MOSI_BIT | SPI_USR_MISO_BIT;
    GPIO_REG(SPI3_USER1) = 0;
    GPIO_REG(SPI3_USER2) = 0;
    GPIO_REG(SPI3_CTRL)  = 0;

    /* All three hardware chip selects off. CS is a plain GPIO here, for the
     * same reason display.c made that choice: a transaction spans several
     * transfers and the peripheral's CS automation does not express that. */
    GPIO_REG(SPI3_PIN) = SPI_CS0_DIS_BIT | SPI_CS1_DIS_BIT | SPI_CS2_DIS_BIT;
}

/* ---- pads ---------------------------------------------------------------
 *
 * gpio.h's helpers are not usable here and the reason is worth stating: its
 * gpio_in_init() CLEARS the output enable, and the loopback test below needs one
 * pad driven and read at the same instant. A pad that is only ever one or the
 * other is the normal case and those helpers serve it; this is the other case.
 *
 * IO_MUX registers are not ordered by pin number -- GPIO25 is at +0x24 and
 * GPIO26 at +0x28, but GPIO18 is at +0x70 -- so a table is the only honest way
 * to do this. Addresses read out of soc/io_mux_reg.h against a base of
 * 0x3FF49000; the six pins gpio.h already declares were cross-checked against it
 * and all six agree. */
#define IO_MUX_BASE 0x3FF49000u

/* MCU_SEL = 2 selects plain GPIO on these pads (FUNC_*_GPIOn is 2, while
 * function 1 is the JTAG/HSPI alternate -- the reverse of what it looks like,
 * confirmed when checking GPIO12 for the panel read path). */
#define PAD_DRIVE   ((2u << 12) | (2u << 10))               /* out, drive 2   */
#define PAD_DRIVE_IE ((2u << 12) | (2u << 10) | (1u << 9))  /* out + readable */
#define PAD_READ     ((2u << 12) | (1u << 9))               /* in only        */

static uint32_t io_mux_for(uint8_t pin)
{
    switch (pin) {
    case 0:  return IO_MUX_BASE + 0x44u;
    case 2:  return IO_MUX_BASE + 0x40u;
    case 4:  return IO_MUX_BASE + 0x48u;
    case 5:  return IO_MUX_BASE + 0x6Cu;
    case 16: return IO_MUX_BASE + 0x4Cu;
    case 17: return IO_MUX_BASE + 0x50u;
    case 18: return IO_MUX_BASE + 0x70u;
    case 19: return IO_MUX_BASE + 0x74u;
    case 21: return IO_MUX_BASE + 0x7Cu;
    case 22: return IO_MUX_BASE + 0x80u;
    case 23: return IO_MUX_BASE + 0x8Cu;
    case 25: return IO_MUX_BASE + 0x24u;
    case 26: return IO_MUX_BASE + 0x28u;
    case 27: return IO_MUX_BASE + 0x2Cu;
    case 32: return IO_MUX_BASE + 0x1Cu;
    case 33: return IO_MUX_BASE + 0x20u;
    case 34: return IO_MUX_BASE + 0x14u;   /* input only */
    case 35: return IO_MUX_BASE + 0x18u;   /* input only */
    default: return 0;                     /* refuse rather than guess */
    }
}

static void pad_out_enable(uint8_t pin)
{
    if (pin < 32u) {
        GPIO_REG(GPIO_ENABLE_W1TS_REG)  = 1u << pin;
    } else {
        GPIO_REG(GPIO_ENABLE1_W1TS_REG) = 1u << (pin - 32u);
    }
}

static void pad_out_disable(uint8_t pin)
{
    if (pin < 32u) {
        GPIO_REG(GPIO_ENABLE_W1TC_REG)  = 1u << pin;
    } else {
        GPIO_REG(GPIO_ENABLE1_W1TC_REG) = 1u << (pin - 32u);
    }
}

/* Drive `pin` from peripheral output `sig`. `readable` also leaves the input
 * buffer on, which is only wanted for the loopback. */
static int pad_drive(uint8_t pin, uint32_t sig, int readable)
{
    uint32_t mux = io_mux_for(pin);
    if (!mux) {
        return 0;
    }
    GPIO_REG(mux) = readable ? PAD_DRIVE_IE : PAD_DRIVE;
    GPIO_REG(GPIO_FUNC_OUT_SEL(pin)) = sig;
    pad_out_enable(pin);
    return 1;
}

/* Feed peripheral input `sig` from `pin`. */
static int pad_capture(uint8_t pin, uint32_t sig, int keep_output)
{
    uint32_t mux = io_mux_for(pin);
    if (!mux) {
        return 0;
    }
    if (!keep_output) {
        GPIO_REG(mux) = PAD_READ;
        pad_out_disable(pin);
    }
    GPIO_REG(GPIO_FUNC_IN_SEL(sig)) = (uint32_t)pin | GPIO_SIG_IN_SEL_BIT;
    return 1;
}

void spi3_route(uint8_t sck, uint8_t mosi, uint8_t miso)
{
    if (sck != SPI3_PIN_NONE) {
        pad_drive(sck, VSPICLK_OUT_IDX, 0);
    }
    if (mosi != SPI3_PIN_NONE) {
        pad_drive(mosi, VSPID_OUT_IDX, 0);
    }
    if (miso != SPI3_PIN_NONE) {
        pad_capture(miso, VSPIQ_IN_IDX, 0);
    }
}

int spi3_xfer(const uint8_t *tx, uint8_t *rx, uint32_t n)
{
    if (!tx || n == 0u || n > SPI3_XFER_MAX) {
        return 0;
    }

    for (uint32_t w = 0; w < (n + 3u) / 4u; w++) {
        uint32_t word = 0;
        for (uint32_t b = 0; b < 4u; b++) {
            uint32_t idx = w * 4u + b;
            if (idx < n) {
                word |= (uint32_t)tx[idx] << (8u * b);
            }
        }
        GPIO_REG(SPI3_W(w)) = word;
    }

    /* Both lengths, because both phases run. Setting only MOSI_DLEN clocks the
     * right number of bits and captures nothing. */
    GPIO_REG(SPI3_MOSI_DLEN) = n * 8u - 1u;
    GPIO_REG(SPI3_MISO_DLEN) = n * 8u - 1u;
    GPIO_REG(SPI3_CMD)       = SPI_USR_BIT;

    /* Bounded, and bounded by wall clock with the lesson of UM-NATOS-030
     * applied: the old display bound was ~25 ms, which is shorter than one
     * scheduling round trip, so a preempted task timed out on hardware that was
     * working perfectly. 40,000,000 cycles is ~500 ms at 80 MHz -- far beyond
     * the ~260 us a 64-byte transfer needs at 2 MHz, and still an actual bound. */
    uint32_t start = xt_ccount();
    while (GPIO_REG(SPI3_CMD) & SPI_USR_BIT) {
        if ((xt_ccount() - start) > 40000000u) {
            g_timeouts++;
            return 0;
        }
    }

    if (rx) {
        for (uint32_t i = 0; i < n; i++) {
            rx[i] = (uint8_t)(GPIO_REG(SPI3_W(i / 4u)) >> (8u * (i % 4u)));
        }
    }

    g_transfers++;
    return 1;
}

/* ---- bring-up ------------------------------------------------------------ */

int spi3_selftest_const(int level)
{
    /* GPIO_SIG_IN_SEL_BIT is required for the CONSTANTS too, not only for real
     * pins. Without it the peripheral takes its input straight from IO_MUX and
     * the index field is ignored entirely -- so both constants read whatever the
     * unrouted pad happens to be.
     *
     * That is exactly what happened on the first run of this test, and it is why
     * the test is a PAIR. Tie-high passed, because an unrouted input floats
     * high and 0xFF is what tie-high expects. On its own that reads as a clean
     * bring-up. Only tie-low disagreed, and the disagreement is the entire
     * signal: a bus that answers 0xFF to everything is indistinguishable from a
     * working one until you ask it for zero. */
    GPIO_REG(GPIO_FUNC_IN_SEL(VSPIQ_IN_IDX)) =
        (level ? MATRIX_CONST_ONE : MATRIX_CONST_ZERO) | GPIO_SIG_IN_SEL_BIT;

    /* A pattern that is neither all-ones nor all-zeros, so a driver that simply
     * hands back what it was given cannot pass either half of this test. */
    static const uint8_t PATTERN[8] = { 0x5Au, 0xA5u, 0x00u, 0xFFu,
                                        0x12u, 0x34u, 0x56u, 0x78u };
    uint8_t got[8];

    if (!spi3_xfer(PATTERN, got, 8u)) {
        return 0;
    }

    uint8_t want = level ? 0xFFu : 0x00u;
    for (int i = 0; i < 8; i++) {
        if (got[i] != want) {
            return 0;
        }
    }
    return 1;
}

int spi3_selftest_loopback(uint8_t pin)
{
    /* One pad doing both jobs: driven by the peripheral's MOSI output and read
     * straight back into its MISO input through the matrix. That needs the
     * output enable AND the input buffer on at once, which is why the pad code
     * above exists instead of gpio.h's helpers.
     *
     * Order matters. pad_capture() is told to keep the output, because clearing
     * it would leave a pin that reads its own undriven self -- which returns a
     * plausible, stable, entirely meaningless answer. */
    if (!pad_drive(pin, VSPID_OUT_IDX, 1)) {
        return 0;
    }
    if (!pad_capture(pin, VSPIQ_IN_IDX, 1)) {
        return 0;
    }

    static const uint8_t PATTERN[8] = { 0x5Au, 0xA5u, 0x00u, 0xFFu,
                                        0x12u, 0x34u, 0x56u, 0x78u };
    uint8_t got[8];

    if (!spi3_xfer(PATTERN, got, 8u)) {
        return 0;
    }
    for (int i = 0; i < 8; i++) {
        if (got[i] != PATTERN[i]) {
            return 0;
        }
    }
    return 1;
}
