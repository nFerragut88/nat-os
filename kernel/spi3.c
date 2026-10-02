/* nat-os — SPI3 (VSPI) master. See spi3.h for why this exists separately. */

#include "spi3.h"
#include "gpio.h"
#include "xtensa.h"
#include "task.h"
#include "uart.h"

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
#define SPI3_SLAVE         (SPI3_BASE + 0x38u)
#define SPI_SYNC_RESET     (1u << 31)   /* SPI_SLAVE_REG: reset the SPI logic */
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

/* W-register transfers, and the count as it stood when the DMA engine was last
 * armed. One of these poisons every later DMA transfer by 16 bytes, so the
 * difference between them is a yes/no answer to "is DMA still trustworthy". */
static uint32_t g_wreg_xfers;
static uint32_t g_wreg_at_init;
static uint32_t g_dma_poisoned;     /* DMA given up because the count moved */

uint32_t spi3_wreg_xfers(void)  { return g_wreg_xfers; }
uint32_t spi3_dma_poisoned(void) { return g_dma_poisoned; }

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

/* The clock word in force, so a peripheral reset can put it back. */
static uint32_t g_clk_reg = SPI3_CLKDIV;

/* Takes SPI3 through its DPORT reset and restores every register spi3_init()
 * sets. Heavier than SPI_SYNC_RESET, and for one specific reason:
 *
 * A W-register transfer leaves the peripheral counting 16 bytes that a
 * subsequent DMA transfer then spends out of its descriptor, PERMANENTLY and
 * cumulatively -- 512 of 512 cold, 496 after one FIFO read, 480 after two, on
 * every transfer thereafter. Resetting the DMA channel does not clear it and
 * neither does SPI_SYNC_RESET (both measured, next_moves/12 step 10). This is
 * the next-largest hammer: the peripheral itself, back to its power-on state.
 *
 * Costs ~10 register writes against the ~205 us a 512-byte transfer takes at
 * 20 MHz, so if it works it is cheap at the price. */
static void spi3_hard_reset(void)
{
    GPIO_REG(DPORT_PERIP_RST_EN) |= DPORT_SPI3_BIT;
    GPIO_REG(DPORT_PERIP_RST_EN) &= ~DPORT_SPI3_BIT;

    GPIO_REG(SPI3_CLOCK) = g_clk_reg;
    GPIO_REG(SPI3_USER)  = SPI_DOUTDIN_BIT | SPI_USR_MOSI_BIT | SPI_USR_MISO_BIT;
    GPIO_REG(SPI3_USER1) = 0;
    GPIO_REG(SPI3_USER2) = 0;
    GPIO_REG(SPI3_CTRL)  = 0;
    GPIO_REG(SPI3_PIN)   = SPI_CS0_DIS_BIT | SPI_CS1_DIS_BIT | SPI_CS2_DIS_BIT;
}

void spi3_set_div(uint32_t div)
{
    if (div < 2u)  { div = 2u; }
    if (div > 64u) { div = 64u; }
    /* SPI_CLOCK: pre[30:18]=0, n[17:12], h[11:6], l[5:0]; f = APB / (n+1),
     * high for h+1 of those cycles. Field positions from spi_reg.h; checked
     * against SPI3_CLKDIV above, which decodes to n=39 h=19 l=39: 2 MHz. */
    uint32_t n = div - 1u;
    uint32_t h = div / 2u - 1u;
    g_clk_reg = (n << 12) | (h << 6) | n;
    GPIO_REG(SPI3_CLOCK) = g_clk_reg;
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

/* The pins last routed, so the peripheral can be re-attached without the
 * caller having to remember them. */
static uint8_t g_sck = SPI3_PIN_NONE, g_mosi = SPI3_PIN_NONE,
               g_miso = SPI3_PIN_NONE;

void spi3_route(uint8_t sck, uint8_t mosi, uint8_t miso)
{
    g_sck = sck; g_mosi = mosi; g_miso = miso;
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

/* Re-attaches the peripheral: configure, set the clock, re-point the pads
 * through the GPIO matrix. Exactly what sd_init() does after identification.
 *
 * Worth having as one call because of an observation that contradicts the
 * "W-register reads poison DMA permanently" rule: the mixing probe's FIRST row
 * always delivered every byte, in sessions where the boot had already read
 * hundreds of blocks through the W registers. Something between those reads and
 * that row cleared the deficit, and the one thing sd_init() does that none of
 * the four resets did is take the pads away from the peripheral and give them
 * back. If that is what clears it, mixing is survivable and this is the price:
 * a dozen register writes before switching to DMA. */
void spi3_reattach(void)
{
    spi3_init();
    GPIO_REG(SPI3_CLOCK) = g_clk_reg;       /* spi3_init() reset it to 2 MHz */
    spi3_route(g_sck, g_mosi, g_miso);
}

/* Receive n bytes while sending 0xFF -- what every SD read is.
 *
 * The transmit side needs no packing loop: the W registers are simply filled
 * with ones. That and the word-wise readback are the whole of the difference
 * between 0.4 us of software per byte and the wire's own 0.4 us at 20 MHz. */
/* The transmit fill is NOT optional, and it is half of this function's cost.
 *
 * Every W-register access crosses the peripheral bus at ~50 CPU cycles: 16
 * writes and 16 reads per 64 bytes, which is the 0.36 us a byte this driver
 * spends above the 20 MHz wire's own 0.4 us. Skipping the fill -- sending the
 * previous chunk's received bytes instead of ones -- was tried and it broke the
 * first block read on the card. During a MULTI-BLOCK read the card watches MOSI
 * for CMD12, because that is the only thing that stops the stream, so arbitrary
 * bytes there are a command waiting to happen.
 *
 * Halving this traffic needs DMA, whose transmit side streams 0xFF from memory
 * at no CPU cost -- and DMA cannot be mixed with this path at all. See
 * spi3_read_dma. */
int spi3_read(uint8_t *rx, uint32_t n)
{
    if (!rx || n == 0u || n > SPI3_XFER_MAX) {
        return 0;
    }
    for (uint32_t w = 0; w < (n + 3u) / 4u; w++) {
        GPIO_REG(SPI3_W(w)) = 0xFFFFFFFFu;
    }
    GPIO_REG(SPI3_MOSI_DLEN) = n * 8u - 1u;
    GPIO_REG(SPI3_MISO_DLEN) = n * 8u - 1u;
    GPIO_REG(SPI3_CMD)       = SPI_USR_BIT;

    uint32_t start = xt_ccount();
    while (GPIO_REG(SPI3_CMD) & SPI_USR_BIT) {
        if ((xt_ccount() - start) > 40000000u) {
            g_timeouts++;
            return 0;
        }
    }
    for (uint32_t i = 0; i < n; i += 4u) {
        uint32_t w = GPIO_REG(SPI3_W(i / 4u));
        uint32_t k = n - i < 4u ? n - i : 4u;
        rx[i] = (uint8_t)w;
        if (k > 1u) { rx[i + 1u] = (uint8_t)(w >> 8); }
        if (k > 2u) { rx[i + 2u] = (uint8_t)(w >> 16); }
        if (k > 3u) { rx[i + 3u] = (uint8_t)(w >> 24); }
    }
    g_transfers++;
    g_wreg_xfers++;     /* the tripwire DMA watches; see spi3_xfer_dma */
    return 1;
}

/* ---- DMA reads ---------------------------------------------------------------
 *
 * [next_moves/12 step 10] Even word-wise, every byte of a block read costs the
 * CPU an APB register access: measured 0.36 us a byte on top of the 0.4 us the
 * 20 MHz wire itself takes. DMA removes the CPU from the data path entirely, so
 * a block read approaches its wire time.
 *
 * The register constants and the hard-won ordering both come from display.c's
 * SPI2 engine, which cost UM-NATOS-030 and UM-NATOS-033 to get right. Two
 * differences, and both matter:
 *
 *   - this is the INBOUND channel, which display.c never uses. The engine
 *     writes WORDS into DRAM, so a destination must be 4-byte aligned and in
 *     DRAM; anything else is refused and counted, not silently mangled.
 *   - a read still has to send something. SD expects all ones while it talks,
 *     so the outbound channel streams a buffer of 0xFF alongside.
 */
#define SPI3_DMA_CONF      (SPI3_BASE + 0x100u)
#define SPI3_DMA_OUT_LINK  (SPI3_BASE + 0x104u)
#define SPI3_DMA_IN_LINK   (SPI3_BASE + 0x108u)
#define SPI3_DMA_STATUS    (SPI3_BASE + 0x10Cu)
#define SPI3_DMA_INT_RAW   (SPI3_BASE + 0x114u)
#define SPI3_DMA_INT_CLR   (SPI3_BASE + 0x11Cu)

/* SPI_DMA_CONF_REG. The IN/OUT pairs are named together because the wrong one
 * of a pair is a real bit the hardware accepts in silence -- which is exactly
 * how DMA_OUT_RST spent a year acting on the receive channel in display.c. */
#define DMA_IN_RST          (1u << 2)
#define DMA_OUT_RST         (1u << 3)
#define DMA_AHBM_FIFO_RST   (1u << 4)
#define DMA_AHBM_RST        (1u << 5)
#define DMA_OUTDSCR_BURST   (1u << 10)
#define DMA_INDSCR_BURST    (1u << 11)
#define DMA_OUT_DATA_BURST  (1u << 12)

/* SPI_DMA_INT_RAW_REG: 3 IN_DONE, 4 IN_SUC_EOF, 5 IN_ERR_EOF, 6 OUT_DONE,
 * 7 OUT_EOF, 8 OUT_TOTAL_EOF. IN_SUC_EOF is the inbound analogue of the bit
 * display.c learned to wait for: the channel has retired the descriptor, which
 * is a different event from the SPI transaction ending. */
#define DMA_IN_SUC_EOF_INT  (1u << 4)
#define DMA_IN_ERR_EOF_INT  (1u << 5)
#define DMA_STATUS_TX_EN    (1u << 1)

#define DMA_LINK_START      (1u << 29)   /* NOT 30, which is RESTART */

/* Bits 5:4 are SPI3's channel; 3:2 are SPI2's and must survive untouched. */
#define DPORT_SPI_DMA_CHAN_SEL 0x3FF005A8u
#define DPORT_SPI_DMA_CLK_EN   (1u << 22)

/* DRAM, the only memory the engine can reach. A descriptor or buffer outside
 * this range is not slow, it is unreachable. */
#define DRAM_LO 0x3FFAE000u
#define DRAM_HI 0x40000000u

typedef struct {
    uint32_t flags;     /* size:12 | length:12 | offset:5 | sosf:1 | eof:1 | owner:1 */
    uint32_t buf;
    uint32_t next;
} dma_desc_t;

static dma_desc_t g_rx_desc __attribute__((aligned(4)));
static dma_desc_t g_tx_desc __attribute__((aligned(4)));
static uint8_t    g_ones[SPI3_DMA_MAX] __attribute__((aligned(4)));

static int      g_dma_ok;           /* usable right now                     */
static int      g_dma_allowed;      /* policy: off until asked, see below    */
static int      g_dma_reachable;    /* descriptors and buffers are in DRAM   */
static uint32_t g_dma_transfers, g_dma_timeouts, g_dma_refused;

/* Which wait gave up, and what the peripheral said at that moment. A timeout
 * counter alone says "DMA does not work", which is the one thing already
 * obvious; these say whether the shifter ran, whether the channel started, and
 * what the interrupt-raw bits were when it stopped. */
static uint32_t g_dma_stage;        /* see spi3.h */
static uint32_t g_dma_int, g_dma_status, g_dma_spins, g_dma_len;
static uint32_t g_dma_err_eofs;     /* IN_ERR_EOF on a transfer that was fine */
static uint32_t g_dma_flags;        /* the descriptor as the engine left it */
static uint32_t g_dma_late, g_dma_settle;   /* transfers whose count lagged */

/* The FIRST failure, kept unoverwritten. Every failure after the first is a
 * consequence of it -- DMA gives itself up, callers fall back, and the last
 * recorded failure describes the fallback rather than the fault. Reading the
 * last one cost a build cycle chasing a 1-byte transfer that only happened
 * because something else had already gone wrong. */
static uint32_t g_first_stage, g_first_len, g_first_flags, g_first_int;
static uint32_t g_first_at;         /* how many transfers had succeeded */

uint32_t spi3_dma_first_stage(void) { return g_first_stage; }
uint32_t spi3_dma_first_len(void)   { return g_first_len; }
uint32_t spi3_dma_first_flags(void) { return g_first_flags; }
uint32_t spi3_dma_first_int(void)   { return g_first_int; }
uint32_t spi3_dma_first_at(void)    { return g_first_at; }

static void dma_fail(uint32_t stage, uint32_t n, uint32_t flags, uint32_t raw)
{
    if (g_first_stage == 0u) {
        g_first_stage = stage;
        g_first_len   = n;
        g_first_flags = flags;
        g_first_int   = raw;
        g_first_at    = g_dma_transfers;
    }
}

uint32_t spi3_dma_late(void)   { return g_dma_late; }
uint32_t spi3_dma_settle(void) { return g_dma_settle; }

uint32_t spi3_dma_flags(void) { return g_dma_flags; }

uint32_t spi3_dma_err_eofs(void) { return g_dma_err_eofs; }

uint32_t spi3_dma_stage(void)  { return g_dma_stage; }
uint32_t spi3_dma_int(void)    { return g_dma_int; }
uint32_t spi3_dma_status(void) { return g_dma_status; }
uint32_t spi3_dma_spins(void)  { return g_dma_spins; }
uint32_t spi3_dma_len(void)    { return g_dma_len; }

uint32_t spi3_dma_transfers(void) { return g_dma_transfers; }
uint32_t spi3_dma_timeouts(void)  { return g_dma_timeouts; }
uint32_t spi3_dma_refused(void)   { return g_dma_refused; }
int      spi3_dma_enabled(void)   { return g_dma_ok; }
void spi3_dma_force_fifo(int on)
{
    g_dma_allowed = !on;
    g_dma_ok = g_dma_allowed && g_dma_reachable;
}

void spi3_dma_init(void)
{
    for (uint32_t i = 0; i < SPI3_DMA_MAX; i++) {
        g_ones[i] = 0xFFu;
    }
    GPIO_REG(DPORT_PERIP_CLK_EN) |= DPORT_SPI_DMA_CLK_EN;

    uint32_t sel = GPIO_REG(DPORT_SPI_DMA_CHAN_SEL);
    sel &= ~(3u << 4);
    sel |=  (2u << 4);                  /* channel 2; SPI2 keeps channel 1 */
    GPIO_REG(DPORT_SPI_DMA_CHAN_SEL) = sel;

    /* Reset ONCE, here. display.c proved that resetting a channel between
     * back-to-back transfers tears down the state that produces its completion
     * signal, and sd.c issues these back to back inside one card transaction. */
    GPIO_REG(SPI3_DMA_CONF) |= DMA_IN_RST | DMA_OUT_RST
                             | DMA_AHBM_FIFO_RST | DMA_AHBM_RST;
    GPIO_REG(SPI3_DMA_CONF) &= ~(DMA_IN_RST | DMA_OUT_RST
                               | DMA_AHBM_FIFO_RST | DMA_AHBM_RST);
    GPIO_REG(SPI3_DMA_CONF) |= DMA_OUTDSCR_BURST | DMA_INDSCR_BURST
                             | DMA_OUT_DATA_BURST;

    /* The descriptors themselves must be reachable too, and .bss placement is
     * the linker's business, not this file's. Checked rather than assumed. */
    if ((uint32_t)&g_rx_desc < DRAM_LO || (uint32_t)&g_rx_desc >= DRAM_HI
        || (uint32_t)&g_ones < DRAM_LO || (uint32_t)&g_ones >= DRAM_HI) {
        g_dma_reachable = 0;
        g_dma_ok = 0;
        return;
    }
    g_dma_reachable = 1;
    g_wreg_at_init  = g_wreg_xfers;
    /* ON, and the condition that makes it safe is absolute: NOTHING may have
     * moved bytes through the W registers since power-on.
     *
     * One such transfer costs every later DMA transfer 16 bytes, cumulatively
     * and permanently -- five mechanisms were measured against it (the channel
     * reset, the AHB-master FIFO reset, SPI_SYNC_RESET, the DPORT peripheral
     * reset, and re-attaching the pads through the GPIO matrix) and none of
     * them clears it. So the peripheral is a DMA port for the whole run or a
     * W-register port for the whole run.
     *
     * This is armed from sd_init(), whose identification is bit-banged GPIO and
     * touches no register, so in a fresh boot the count is zero here and every
     * SD transfer afterwards would be DMA. The tripwire in spi3_xfer_dma()
     * enforces the rest: if a diagnostic moves the count, DMA gives itself up
     * and reads fall back to the slow path for the remainder of the run.
     *
     * LEFT AT 0 because the word-aligned data path (step 11) is written but has
     * never completed a read: every attempt to test it needed the card
     * physically reseated first, for reasons that turned out to be the test
     * before it rather than the code. `1` here is the whole change needed to
     * try again from a fresh boot -- there is no runtime way in, because by the
     * time a shell command could ask, the W registers have already moved bytes
     * and the engine is poisoned for the rest of the run. */
    g_dma_allowed   = 1;        /* the line step 11 left to flip */
    /* Armed, but NOT permitted by default. sd.c mixes W-register transfers with
     * these inside one card transaction, and on this peripheral a W-register
     * read permanently costs every later DMA transfer 16 bytes (measured; see
     * spi3_read_dma). `spidma 1` allows it for measurement, and the probe
     * allows it around its own transfers. */
    g_dma_ok = g_dma_allowed;
}

/* Staging, for callers whose buffers the engine cannot use directly: the
 * command bytes sd.c sends, and the odd 1-3 byte reads that bring a block's
 * offset onto a word boundary. A 512-byte copy would cost ~12 us against the
 * 207 us the transfer itself takes, so the block data path must stay DIRECT and
 * these are only for the small transfers. */
static uint8_t g_dma_tx[SPI3_DMA_MAX] __attribute__((aligned(4)));
static uint8_t g_dma_rx[SPI3_DMA_MAX + 4u] __attribute__((aligned(4)));

/* What the engine will take DIRECTLY, measured rather than assumed:
 *
 *   destination alignment   MATTERS, and the probe said so in a line I read
 *                           past: 512 bytes to buf+2 reported "delivered 512"
 *                           while only 511 of them had actually changed, and
 *                           buf+3 lost two. The descriptor's length field is
 *                           not a count of bytes written. Trusting it cost a
 *                           read with the wrong CRC (0xf994d357).
 *   length                  must be a word multiple -- not because the data
 *                           fails to arrive (509 bytes all arrived) but because
 *                           the descriptor's length field is only written on a
 *                           word boundary, so a transfer of 509 can never be
 *                           CONFIRMED. An unconfirmable transfer is one this
 *                           driver will not build a filesystem on.
 *   memory                  DRAM only; the engine cannot reach IRAM.
 */
static int dma_usable(const void *p, uint32_t n)
{
    return ((uint32_t)p & 3u) == 0u && (n & 3u) == 0u
        && (uint32_t)p >= DRAM_LO && (uint32_t)p + n <= DRAM_HI;
}

int spi3_read_dma(uint8_t *rx, uint32_t n)
{
    return spi3_xfer_dma(0, rx, n);
}

static int xfer_dma(const uint8_t *tx, uint8_t *rx, uint32_t n, int force);

/* The probe's way in: the same transfer with the alignment and length refusals
 * SKIPPED, so the probe can find out what the engine actually tolerates rather
 * than what this file assumes. Nothing else may call it. */
int spi3_force_dma(uint8_t *rx, uint32_t n)
{
    return xfer_dma(0, rx, n, 1);
}

int spi3_xfer_dma(const uint8_t *tx, uint8_t *rx, uint32_t n)
{
    return xfer_dma(tx, rx, n, 0);
}

static int xfer_dma(const uint8_t *tx, uint8_t *rx, uint32_t n, int force)
{
    if (!g_dma_ok || n == 0u || n > SPI3_DMA_MAX) {
        g_dma_refused++;        /* COUNTED. Silence here hid a 12% retry rate */
        return 0;
    }
    /* ---- the tripwire -------------------------------------------------------
     *
     * A single W-register transfer permanently costs every later DMA transfer
     * 16 bytes on this peripheral, cumulatively, and nothing clears it short of
     * a power cycle (next_moves/12 step 10; four resets measured). sd.c no
     * longer makes any on this path -- but `spitest`, `spi3` and the pin
     * loopback still do, and they are one keystroke away.
     *
     * So the count is watched. If it moves, DMA is given up for the rest of the
     * run and reads fall back to the W registers, which is slower and correct.
     * The alternative is a driver that returns 496 bytes of a 512-byte block
     * because somebody ran a diagnostic. */
    if (g_wreg_xfers != g_wreg_at_init) {
        g_dma_poisoned++;
        g_dma_ok = 0;
        g_dma_allowed = 0;
        return 0;
    }

    const uint8_t *src = g_ones;            /* a read sends ones */
    if (tx) {
        if (dma_usable(tx, n)) {
            src = tx;
        } else {
            for (uint32_t i = 0; i < n; i++) {
                g_dma_tx[i] = tx[i];
            }
            src = g_dma_tx;
        }
    }

    /* The engine writes WORDS at a word boundary. A destination it cannot
     * honour is staged rather than refused, because refusing would push the
     * decision onto every caller; the copy is only paid by small transfers. */
    uint8_t *dst = rx;
    int staged = 0;
    if (!rx || (!dma_usable(rx, n) && !force)) {
        dst = g_dma_rx;
        staged = 1;
    }

    /* The AHB master FIFO only, and per transfer: this driver ALTERNATES
     * transports inside one card transaction -- commands and the token poll go
     * through the W registers, block data comes this way -- and the two share
     * that FIFO. Same reasoning as display.c, same bits. */
    /* The reset, in the order Espressif's driver does it -- and the ORDER is
     * the whole content of this comment, because two wrong versions of it came
     * first and each failed differently:
     *
     *   resetting nothing but the AHB FIFO   -> IN_ERR_EOF, every time
     *   adding IN_RST without clearing START -> both channels dead, int_raw 0
     *
     * A link register keeps its START bit after a transfer. Pulsing a channel
     * reset while START is still set leaves the engine believing it is running
     * a descriptor that has been torn out from under it, and nothing brings it
     * back -- which is why every attempt after the first read int_raw
     * 0x00000000: the peripheral had quietly reverted to its W-register FIFO.
     *
     * So: raise the resets, CLEAR BOTH LINKS, drop the resets. spi_dma_reset()
     * in spi_ll.h, and display.c's warning about per-transfer resets was the
     * same fault seen from the far side. */
    GPIO_REG(SPI3_DMA_CONF) |= DMA_IN_RST | DMA_OUT_RST
                             | DMA_AHBM_FIFO_RST | DMA_AHBM_RST;
    GPIO_REG(SPI3_DMA_OUT_LINK) = 0;
    GPIO_REG(SPI3_DMA_IN_LINK)  = 0;
    GPIO_REG(SPI3_DMA_CONF) &= ~(DMA_IN_RST | DMA_OUT_RST
                               | DMA_AHBM_FIFO_RST | DMA_AHBM_RST);
    GPIO_REG(SPI3_DMA_INT_CLR) = 0xFFFFFFFFu;

    g_tx_desc.flags = (n & 0xFFFu) | ((n & 0xFFFu) << 12)
                    | (1u << 30) | (1u << 31);      /* eof, owned by the engine */
    g_tx_desc.buf   = (uint32_t)src;
    g_tx_desc.next  = 0;

    /* Receive descriptor: SIZE, and EOF, and the owner bit. `length` is left 0
     * because the engine writes it -- but EOF is NOT the engine's to set here,
     * and leaving it clear is what made the first version fail:
     *
     *   int_raw 0x000001e8 -- OUT_DONE, OUT_EOF, OUT_TOTAL_EOF and IN_ERR_EOF,
     *   with IN_SUC_EOF absent. The transmit side was flawless; the inbound
     *   channel ran out of stream with no descriptor marked as its end and
     *   retired the descriptor as an ERROR.
     *
     * Espressif's lldesc_setup_link() marks the last descriptor eof=1 for
     * receive links exactly as it does for transmit ones. One descriptor per
     * transfer here, so this is that last one. */
    g_rx_desc.flags = (((n + 3u) & ~3u) & 0xFFFu) | (1u << 30) | (1u << 31);
    g_rx_desc.buf   = (uint32_t)dst;
    g_rx_desc.next  = 0;

    GPIO_REG(SPI3_DMA_OUT_LINK) = ((uint32_t)&g_tx_desc & 0xFFFFFu) | DMA_LINK_START;
    GPIO_REG(SPI3_DMA_IN_LINK)  = ((uint32_t)&g_rx_desc & 0xFFFFFu) | DMA_LINK_START;

    /* Let the channels fetch their descriptors before the shifter starts.
     * Waited on as a condition, bounded; falling through leaves the race this
     * replaces, not a hang. */
    uint32_t spins = 0;
    while (!(GPIO_REG(SPI3_DMA_STATUS) & DMA_STATUS_TX_EN) && spins < 1000u) {
        spins++;
    }
    g_dma_spins = spins;
    g_dma_len   = n;

    GPIO_REG(SPI3_MOSI_DLEN) = n * 8u - 1u;
    GPIO_REG(SPI3_MISO_DLEN) = n * 8u - 1u;
    GPIO_REG(SPI3_CMD)       = SPI_USR_BIT;

    /* Wall clock, and bounded far beyond one scheduling round trip: the wait
     * keeps running while this task does not, and a bound shorter than a
     * context switch times out on hardware that is working (UM-NATOS-030). */
    uint32_t start = xt_ccount();
    while (GPIO_REG(SPI3_CMD) & SPI_USR_BIT) {
        if ((xt_ccount() - start) > 40000000u) {
            g_dma_stage  = 2u;              /* the shifter never finished */
            g_dma_int    = GPIO_REG(SPI3_DMA_INT_RAW);
            g_dma_status = GPIO_REG(SPI3_DMA_STATUS);
            dma_fail(2u, n, g_rx_desc.flags, g_dma_int);
            g_dma_timeouts++;
            g_dma_ok = 0;
            return 0;
        }
    }
    /* And then the channel, which is a different question from the shifter --
     * returning here would hand back a buffer the engine is still filling.
     *
     * Asked of the DESCRIPTOR, not of an interrupt bit. The engine clears the
     * owner bit and writes the byte count it delivered into `length`, so this
     * waits for owner==0 and then checks length==n: the one condition that
     * actually means "n bytes are in that buffer".
     *
     * The first version waited for IN_SUC_EOF and failed every transfer with
     * IN_ERR_EOF -- while writing all 512 bytes correctly. In MASTER mode the
     * peripheral never produces a stream EOF for the inbound channel to match,
     * so a descriptor marked eof=1 retires as an "error" by definition. That
     * bit is not a verdict on the data here, and Espressif's own master driver
     * does not consult it either; it waits on the transaction. Counted anyway,
     * below, because a bit that fires on every good transfer is worth watching
     * in case it ever means something. */
    while (g_rx_desc.flags & (1u << 31)) {
        if ((xt_ccount() - start) > 40000000u) {
            g_dma_stage  = 3u;
            g_dma_int    = GPIO_REG(SPI3_DMA_INT_RAW);
            g_dma_status = GPIO_REG(SPI3_DMA_STATUS);
            dma_fail(3u, n, g_rx_desc.flags, g_dma_int);
            g_dma_timeouts++;
            g_dma_ok = 0;
            return 0;
        }
    }
    g_dma_int    = GPIO_REG(SPI3_DMA_INT_RAW);
    g_dma_status = GPIO_REG(SPI3_DMA_STATUS);
    /* The owner bit clearing is NOT "the bytes are in memory".
     *
     * Measured: at that instant the length field reads short, and by a margin
     * that grows with the transfer -- 49 of 64, 225 of 256, 449 of 512, always
     * 15, 31, 47 or 63 bytes behind. Counting the buffer's changed bytes a
     * moment later showed MORE than the length field had claimed, which is the
     * engine still retiring its last burst while the descriptor already says it
     * is done with it.
     *
     * So the wait is on the count, which is the only thing that means what this
     * function promises. `late` counts the transfers that needed it, because a
     * race that always resolves in a few spins and a race that occasionally
     * does not are different, and only a number tells them apart. */
    uint32_t spins2 = 0;
    while (((g_rx_desc.flags >> 12) & 0xFFFu) != n) {
        if ((xt_ccount() - start) > 40000000u) {
            g_dma_stage  = 5u;          /* short receive, and it stayed short */
            g_dma_flags  = g_rx_desc.flags;
            g_dma_int    = GPIO_REG(SPI3_DMA_INT_RAW);
            g_dma_status = GPIO_REG(SPI3_DMA_STATUS);
            dma_fail(5u, n, g_dma_flags, g_dma_int);
            g_dma_timeouts++;
            g_dma_ok = 0;
            return 0;
        }
        spins2++;
    }
    g_dma_flags = g_rx_desc.flags;
    g_dma_settle = spins2;
    if (spins2) {
        g_dma_late++;
    }
    if (staged && rx) {
        for (uint32_t i = 0; i < n; i++) {
            rx[i] = g_dma_rx[i];
        }
    }
    if (g_dma_int & DMA_IN_ERR_EOF_INT) {
        g_dma_err_eofs++;
    }

    g_dma_transfers++;
    return 1;
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
        /* One register read per WORD, not per byte. Each W access goes out on
         * the peripheral bus; four of them for four bytes was most of this
         * driver's 0.4 us/byte -- measured at 32 CPU cycles a byte, against
         * 0.4 us of actual wire time at 20 MHz (next_moves/12 step 8). */
        for (uint32_t i = 0; i < n; i += 4u) {
            uint32_t w = GPIO_REG(SPI3_W(i / 4u));
            uint32_t k = n - i < 4u ? n - i : 4u;
            rx[i] = (uint8_t)w;
            if (k > 1u) { rx[i + 1u] = (uint8_t)(w >> 8); }
            if (k > 2u) { rx[i + 2u] = (uint8_t)(w >> 16); }
            if (k > 3u) { rx[i + 3u] = (uint8_t)(w >> 24); }
        }
    }

    g_transfers++;
    g_wreg_xfers++;     /* ditto: any W-register traffic poisons the engine */
    return 1;
}

/* ---- bring-up ------------------------------------------------------------ */

/* [next_moves/12 step 8] Where a transfer's time goes, with no card in the
 * picture: the peripheral clocks whether or not anything listens.
 *
 * Timed in THIS TASK'S OWN CYCLES. The first version used xt_ccount() and
 * measured the same 64-byte transfer at 80 us and at 59 us, because a tick
 * lands inside a 12 ms loop and another task's slice goes on the bill --
 * task.h says exactly this, and it was ignored for one more round.
 *
 * Cost is measured against LENGTH so the fixed part of a transfer separates
 * from the per-byte part: only the second is helped by a faster clock. */
void spi3_probe_speed(void)
{
    static uint8_t ff[SPI3_XFER_MAX], rx[SPI3_XFER_MAX];
    for (uint32_t i = 0; i < SPI3_XFER_MAX; i++) {
        ff[i] = 0xFFu;
    }
    uint32_t saved = GPIO_REG(SPI3_CLOCK);
    static const uint32_t divs[] = { 2u, 4u, 8u };
    static const uint32_t lens[] = { 1u, 16u, 64u };

    for (uint32_t d = 0; d < 3u; d++) {
        spi3_set_div(divs[d]);
        uart_puts("   div ");
        uart_put_dec(divs[d]);
        uart_puts(" (");
        uart_put_dec(80u / divs[d]);
        uart_puts(" MHz, reg ");
        uart_put_hex(GPIO_REG(SPI3_CLOCK));
        uart_puts("):");
        for (uint32_t l = 0; l < 3u; l++) {
            uint32_t c0 = task_cpu_cycles();
            for (uint32_t i = 0; i < 400u; i++) {
                spi3_xfer(ff, rx, lens[l]);
            }
            uint32_t ns = (task_cpu_cycles() - c0) * 25u / 2u / 400u;
            uart_puts("  ");
            uart_put_dec(lens[l]);
            uart_puts(" B: ");
            uart_put_dec(ns / 1000u);
            uart_puts(".");
            uart_put_dec((ns / 100u) % 10u);
            uart_puts(" us");
        }
        uart_puts("\n");
    }
    GPIO_REG(SPI3_CLOCK) = saved;
    uart_puts("   wire time for 64 B: 25.6 us at 20 MHz, 12.8 at 40, 51.2 at 10\n");
}

/* [next_moves/12 step 10] Does the receive channel accept this LENGTH?
 *
 * The first DMA read failed with IN_ERR_EOF -- the inbound channel retiring its
 * descriptor with an error -- and the card was blamed for a whole build cycle
 * before this existed. It need not be: with CS high the card ignores the clock
 * entirely, so a transfer still exercises the peripheral, the channel and the
 * descriptor while reading nothing but an idle line. Every length gets its own
 * answer, printed with the interrupt-raw bits, because "DMA is broken" and
 * "DMA refuses 500 bytes" are different problems.
 *
 * Bytes are checked as well as timing: an idle MISO reads 0xFF, so a buffer
 * salted with a different value shows whether anything was written at all. */
void spi3_probe_dma(void)
{
    static uint8_t buf[SPI3_DMA_MAX] __attribute__((aligned(4)));
    /* One length throughout, so the only variable is what precedes each
     * transfer. A 16-byte W-register read goes in front of rows 1 and 4 only.
     *
     * If the deficit is 16 for rows 1-3 and 32 for rows 4-5, the offset a FIFO
     * read introduces is PERMANENT and every later DMA transfer inherits it. If
     * instead rows 2-3 and 5 come back clean, only the transfer immediately
     * after a FIFO read is affected, and that is a much cheaper thing to fix. */
    static const uint32_t lens[] = { 512u, 512u, 512u, 512u, 512u, 512u };
    static const uint32_t pre_fifo[] = { 0u, 1u, 0u, 0u, 1u, 0u };

    /* Establish the peripheral, do not assume it.
     *
     * The first three runs of this probe tested an SPI3 that had never been
     * initialised: nothing in those sessions had touched the card, and
     * spi3_init() is called from sd_init(), so SPI_USER sat at its power-on
     * default of 0x80000040 -- DOUTDIN, USR_MOSI and USR_MISO all clear, no
     * receive phase at all. Three builds were spent explaining a receive
     * channel that was never asked to receive.
     *
     * Reading the register back is what ended it, and USER is printed below so
     * the next reader gets the evidence rather than the conclusion.
     *
     * The CALLER brings the bus up -- the shell runs sd_init() first. An earlier
     * version initialised the peripheral here and then clocked 1.5 KB at a
     * chip-select nobody had driven high yet, and the card stopped identifying.
     * A diagnostic may disturb what it measures; it should not be the only
     * thing that configures it. */
    spi3_dma_init();
    uart_puts("   chan_sel=");
    uart_put_hex(GPIO_REG(DPORT_SPI_DMA_CHAN_SEL));
    uart_puts(" (bits 1:0 SPI1, 3:2 SPI2, 5:4 SPI3)  dma_conf=");
    uart_put_hex(GPIO_REG(SPI3_DMA_CONF));
    uart_puts("\n   user=");
    uart_put_hex(GPIO_REG(SPI3_USER));
    uart_puts(" status=");
    uart_put_hex(GPIO_REG(SPI3_DMA_STATUS));
    uart_puts(" rx_desc at ");
    uart_put_hex((uint32_t)&g_rx_desc);
    uart_puts(" buf at ");
    uart_put_hex((uint32_t)buf);
    uart_puts("\n");

    for (uint32_t i = 0; i < sizeof lens / sizeof lens[0]; i++) {
        uint32_t n = lens[i];
        for (uint32_t k = 0; k < n; k++) {
            buf[k] = 0x5Au;             /* neither 0xFF nor 0x00 */
        }
        /* Re-arm, not merely re-enable. A failed transfer leaves the channel
         * unattached -- every attempt after the first reported int_raw
         * 0x00000000, which is the peripheral quietly running the transfer
         * through the W-register FIFO instead, with no DMA involved at all.
         * Clearing the disable flag does not put the channel back; only the
         * init sequence does. */
        spi3_dma_force_fifo(0);     /* the probe is where DMA is allowed */
        /* HALF the lengths get a W-register read first, because that is what
         * sd.c does: it polls for the data token through the FIFO and only then
         * hands the rest of the block to the engine. In the real path the engine
         * reported delivering 149 bytes of 496 while this probe, transfer-cold,
         * reported every byte. If the transport switch is the difference, the
         * odd rows below are short and the even rows are not. */
        if (pre_fifo[i]) {
            static uint8_t pre[16] __attribute__((aligned(4)));
            spi3_read(pre, sizeof pre);
        }
        /* The experiment this probe now exists for: does re-attaching the
         * peripheral clear what the W-register read left behind? */
        spi3_reattach();
        /* AFTER the W-register read, not before it.
         *
         * The first version of this probe armed the engine and THEN did the
         * poisoning read, so it never asked the one question that matters: does
         * re-arming CLEAR the deficit? Four resets were measured against this
         * fault and all of them were the DMA_CONF resets -- none of them
         * rewrote DPORT_SPI_DMA_CHAN_SEL, which is the register that attaches
         * this peripheral to the channel in the first place. */
        spi3_dma_init();
        /* AFTER the init, which now leaves DMA disallowed by default (step 11).
         * Allowing first and arming second disabled the engine and every row
         * failed at the guard with no transfer attempted -- stage 0, int 0. */
        spi3_dma_force_fifo(0);
        int ok = spi3_read_dma(buf, n);
        uint32_t written = 0;
        for (uint32_t k = 0; k < n; k++) {
            if (buf[k] != 0x5Au) {
                written++;
            }
        }
        uart_puts("   ");
        uart_put_dec(n);
        uart_puts(pre_fifo[i] ? " B after a FIFO read: " : " B, no FIFO read: ");
        uart_puts(ok ? "ok  " : "FAIL");
        uart_puts(" stage=");
        uart_put_dec(spi3_dma_stage());
        uart_puts(" int=");
        uart_put_hex(spi3_dma_int());
        uart_puts(" bytes changed=");
        uart_put_dec(written);
        uart_puts(" of ");
        uart_put_dec(n);
        /* The engine writes `length` into the descriptor and clears its owner
         * bit when it retires one. Unchanged flags mean it never looked. */
        uart_puts("  rx_flags=");
        uart_put_hex(g_rx_desc.flags);
        uart_puts(" inlink=");
        uart_put_hex(GPIO_REG(SPI3_DMA_IN_LINK));
        uart_puts("\n");
    }
    spi3_dma_force_fifo(1);
}

/* ---- what a DMA transfer COSTS, back to back, DMA only --------------------
 *
 * The regime sd.c would live in if it gave up the W registers entirely: no
 * mixing, so no 16-byte deficit. What matters is the FIXED cost per transfer,
 * because an all-DMA command layer pays it several times per card command,
 * against the ~262 us of wire a 512-byte block needs at 20 MHz.
 *
 * In this task's own cycles, with correctness counted alongside: a cost per
 * transfer means nothing if the transfers are not delivering.
 *
 * SEPARATE from the mixing probe, and that is not cosmetic. Those rows leave a
 * 16-byte deficit behind, after which every DMA transfer fails and sits out its
 * 500 ms timeout -- timing them in the same run measured the timeout and
 * printed nothing for minutes. */
/* WHICH LENGTHS does the engine accept? The first real failure of the all-DMA
 * SD path was a two-byte transfer -- the block's trailing CRC bytes -- whose
 * descriptor the engine never touched at all (owner still set, length 0) after
 * 33 larger transfers had succeeded. It moves words, so a request below a word
 * leaves it waiting for data that will never fill one.
 *
 * What is NOT yet known is whether a length merely has to be >= 4 or has to be
 * a MULTIPLE of 4, and the data path's shape depends on the answer: a block
 * read that keeps the bytes trailing the data token has 497..511 bytes left,
 * which is neither. So: every interesting length, cold, with nothing mixed in.
 */
void spi3_probe_dma_lengths(void)
{
    static uint8_t buf[SPI3_DMA_MAX + 8u] __attribute__((aligned(4)));
    static const uint32_t ln[]  = { 4u, 8u, 509u, 510u, 511u, 512u,
                                    512u, 512u, 512u, 509u, 510u, 511u };
    /* The last six go to an UNALIGNED destination.
     *
     * Both of this wrapper's refusals -- a destination that is not word-aligned
     * and a length that is not a word multiple -- are ASSUMPTIONS about an
     * engine that "writes words", and together they cost a 500-byte
     * byte-at-a-time copy out of a staging buffer on every single block:
     * measured at 160 us against the 206 us the transfer itself needs, on a
     * kernel built -Os where memcpy is a byte loop. If either assumption is
     * wrong, that copy disappears and with it the reason DMA is currently
     * SLOWER than the registers it replaced. */
    static const uint32_t off[] = { 0u, 0u, 0u, 0u, 0u, 0u,
                                    1u, 2u, 3u, 1u, 2u, 3u };

    for (uint32_t i = 0; i < sizeof ln / sizeof ln[0]; i++) {
        uint32_t n = ln[i];
        uint8_t *dst = buf + off[i];
        for (uint32_t k = 0; k < n + 8u; k++) {
            buf[k] = 0x5Au;
        }
        spi3_dma_init();
        spi3_dma_force_fifo(0);
        int ok = spi3_force_dma(dst, n);

        uint32_t written = 0;
        for (uint32_t k = 0; k < n; k++) {
            if (dst[k] != 0x5Au) { written++; }
        }
        uart_puts("   ");
        uart_put_dec(n);
        uart_puts(" B at +");
        uart_put_dec(off[i]);
        uart_puts(": ");
        uart_puts(ok ? "ok   " : "FAIL ");
        uart_puts("delivered ");
        uart_put_dec((g_rx_desc.flags >> 12) & 0xFFFu);
        uart_puts(", written ");
        uart_put_dec(written);
        uart_puts(" of ");
        uart_put_dec(n);
        /* An idle MISO reads 0xFF, so every byte of the window should differ
         * from the salt -- and the byte in front of it must NOT. */
        if (off[i] != 0u && buf[off[i] - 1u] != 0x5Au) {
            uart_puts("  CLOBBERED the byte before");
        }
        uart_puts("\n");
    }
    spi3_dma_force_fifo(1);
}

void spi3_time_dma(void)
{
    static uint8_t buf[SPI3_DMA_MAX] __attribute__((aligned(4)));
    {
        static const uint32_t tl[] = { 16u, 64u, 512u };
        uart_puts("   cost per transfer, DMA only:");
        for (uint32_t t = 0; t < 3u; t++) {
            uint32_t n = tl[t], bad = 0;
            spi3_dma_init();
            spi3_dma_force_fifo(0);     /* in this order; see probe above */
            /* 40, not 200. Two hundred back-to-back 512-byte transfers is a
             * sustained 2.5 MB/s on a board whose supply already proved too
             * weak for a speaker at full volume (step 7), and the card wedged
             * after each long run of this loop. 40 is enough for a cost per
             * transfer that agrees with the wire time to 1%. */
            uint32_t c0 = task_cpu_cycles();
            for (uint32_t k = 0; k < 40u; k++) {
                if (!spi3_read_dma(buf, n)) {
                    bad++;
                    spi3_dma_force_fifo(0);     /* a failure disables it */
                }
            }
            uint32_t ns = (task_cpu_cycles() - c0) * 25u / 2u / 40u;
            uart_puts("  ");
            uart_put_dec(n);
            uart_puts(" B: ");
            uart_put_dec(ns / 1000u);
            uart_puts(".");
            uart_put_dec((ns / 100u) % 10u);
            uart_puts(" us");
            if (bad) {
                uart_puts(" (");
                uart_put_dec(bad);
                uart_puts(" of 40 FAILED)");
            }
        }
        uart_puts("\n   wire time at 20 MHz: 8.2 us for 16 B, 32.8 for 64,"
                  " 262 for 512\n");
    }
    spi3_dma_force_fifo(1);         /* and where it is put back */
}

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
