/* nat-os — ILI9341 driver. See display.h for the pin map and the reasoning. */

#include "display.h"
#include "gpio.h"
#include "uart.h"
#include "mutex.h"
#include "task.h"
#include "xtensa.h"
#include "critical.h"

#define PIN_MOSI 13u
#define PIN_SCLK 14u
#define PIN_CS   15u
#define PIN_DC    2u
#define PIN_BL   21u

/* Derived in UM-NATOS-008 §5.2 from the measured tick rate. Only used for the
 * panel's reset and sleep-out delays, where being wrong by a factor of three
 * still leaves them long enough. */
#define CPU_HZ 80000000u

#define LINE_MAX DISP_W                 /* one full-width span of pixels */

static uint32_t g_bytes;
static uint32_t g_last_fill_cycles;      /* cost of the most recent full clear */
static uint16_t g_line[LINE_MAX];       /* 480 B — the whole "framebuffer" */

/* The span buffer and the panel's window state are both shared, so two tasks
 * drawing at once would interleave pixel streams into one window. Recursive,
 * because display_clear() draws through display_fill_rect(). */
static mutex_t g_lock;

/* Defined further down, next to the panic-mode note that explains why they
 * exist at all. Declared here because display_lock() is defined above them. */
static void draw_lock(void);
static void draw_unlock(void);

static void delay_us(uint32_t us)
{
    uint32_t start = xt_ccount();
    uint32_t want  = us * (CPU_HZ / 1000000u);
    while ((xt_ccount() - start) < want) {
    }
}

static void delay_ms(uint32_t ms)
{
    while (ms--) {
        delay_us(1000);
    }
}


/* ---- SPI2 (HSPI) hardware backend ---------------------------------------
 *
 * The CYD's display pins are the ESP32's native HSPI pads, so IOMUX routes them
 * directly: no GPIO matrix, and none of its 40 MHz ceiling. CS and DC stay
 * ordinary GPIOs, because the driver holds CS asserted across a window command
 * and its whole pixel stream, which is not a shape the peripheral's CS
 * automation expresses.
 *
 * Kept alongside the bit-banged path rather than replacing it. A wrong DPORT
 * clock bit or a wrong IOMUX selection produces a black screen, which is what a
 * wiring fault or a bad init sequence also produces, so the known-good path
 * stays one #define away (UM-NATOS-015 section 3).
 */
#define SPI2_BASE          0x3FF64000u
#define SPI2_CMD           (SPI2_BASE + 0x00u)
#define SPI2_CTRL          (SPI2_BASE + 0x08u)
#define SPI2_CTRL2         (SPI2_BASE + 0x14u)
#define SPI2_CLOCK         (SPI2_BASE + 0x18u)
#define SPI2_USER          (SPI2_BASE + 0x1Cu)
#define SPI2_USER1         (SPI2_BASE + 0x20u)
#define SPI2_USER2         (SPI2_BASE + 0x24u)
#define SPI2_MOSI_DLEN     (SPI2_BASE + 0x28u)
#define SPI2_PIN           (SPI2_BASE + 0x34u)
#define SPI2_SLAVE         (SPI2_BASE + 0x38u)
#define SPI2_W(n)          (SPI2_BASE + 0x80u + 4u * (n))

#define SPI_USR_BIT        (1u << 18)   /* CMD: start; self-clears when done */
#define SPI_USR_MOSI_BIT   (1u << 27)   /* USER: perform a write phase       */

/* DPORT peripheral clock gating. SPI2 is bit 6. Bit 1 in the same register
 * clocks the flash controller this code executes from, so the write is
 * read-modify-write and never a plain store. */
#define DPORT_PERIP_CLK_EN 0x3FF000C0u
#define DPORT_PERIP_RST_EN 0x3FF000C4u
#define DPORT_SPI2_BIT     (1u << 6)

/* 80 MHz / 2. This is the ceiling on THIS board, established by trying the
 * next step up and looking at the panel.
 *
 * Full APB (SPI_CLK_EQU_SYSCLK, 0x80000000) works electrically — the driver
 * reports, the DMA completes, no timeouts, every self-test passes, and the
 * full-screen fill drops from 44 ms to 29 ms. It also puts visible noise on the
 * glass. Nothing in the kernel can see that: every counter says success while
 * the pixels are wrong.
 *
 * That is the whole reason the conservative divisor was taken first. The limit
 * here is the panel and the flex, not the controller, and the only instrument
 * that can measure it is a person looking at the screen. */
#define SPI2_CLKDIV        0x00001001u   /* pre=0 n=1 h=0 l=1 -> sysclk/2 */

/* IOMUX function 1 is the HSPI peripheral on these pads; function 2 is plain
 * GPIO, which is what every other pin in this kernel uses. */
#define IO_MUX_HSPI_FUNC   ((1u << 12) | (2u << 10))

static uint32_t g_spi2_clk_reg;
static uint32_t g_spi2_dport;


/* ---- SPI2 DMA -----------------------------------------------------------
 *
 * The CPU-driven path costs 2,560 transactions per full screen because the FIFO
 * holds 64 bytes (UM-NATOS-015 §5.3). DMA takes a whole 480-byte span in one,
 * so a screen becomes 320 transfers instead.
 *
 * Everything here is guarded three ways, because a DMA engine that never
 * asserts completion would hang the display task forever and take the system
 * with it — the failure mode this project has already paid for twice:
 *
 *   1. every wait is bounded and counted, never a bare `while`
 *   2. a timeout permanently disables DMA and falls back to the FIFO path, so
 *      the display degrades instead of stopping
 *   3. the configuration registers are read back and reported
 *
 * Descriptors and buffers must live in DRAM. g_txbuf and g_desc are in .bss,
 * which the linker places in DRAM; a buffer in IRAM would be silently
 * unreachable by the DMA engine.
 */
#define SPI2_DMA_CONF      (SPI2_BASE + 0x100u)
#define SPI2_DMA_OUT_LINK  (SPI2_BASE + 0x104u)
#define SPI2_DMA_INT_CLR   (SPI2_BASE + 0x11Cu)

#define DMA_OUT_RST        (1u << 2)
#define DMA_AHBM_FIFO_RST  (1u << 4)
#define DMA_AHBM_RST       (1u << 5)
#define DMA_OUTDSCR_BURST  (1u << 11)
#define DMA_OUT_DATA_BURST (1u << 12)
#define DMA_OUTLINK_START  (1u << 30)

/* Which DMA channel serves which SPI peripheral. Bits 2:3 are SPI2's. */
#define DPORT_SPI_DMA_CHAN_SEL 0x3FF005A8u
#define DPORT_SPI_DMA_CLK_EN   (1u << 22)

/* Hardware descriptor. Written as explicit words rather than bitfields: the bit
 * order of a C bitfield is implementation-defined, and this layout is the
 * silicon's. */
typedef struct {
    uint32_t flags;     /* size:12 | length:12 | offset:5 | sosf:1 | eof:1 | owner:1 */
    uint32_t buf;
    uint32_t next;
} dma_desc_t;

static dma_desc_t g_desc  __attribute__((aligned(4)));
static dma_desc_t g_desc2 __attribute__((aligned(4)));   /* the pipeline's other half */
static int        g_dma_busy;   /* a transfer is in flight */

/* Set once by display_init(), so a panic before the panel exists draws nothing
 * rather than writing to an unconfigured controller. */
static int g_ready;

/* One-way. See the panic-mode note further down. */
static volatile int g_panic_mode;

static int      g_dma_ok;     /* 0 disables DMA for the rest of the run */
static uint32_t g_dma_timeouts;
static uint32_t g_dma_transfers;
static uint32_t g_took_dma;     /* spi_tx chose the DMA branch */
static uint32_t g_took_fifo;    /* spi_tx chose the FIFO branch */
static uint32_t g_wait_cycles;    /* spent waiting for the engine */
static uint32_t g_setup_cycles;   /* spent programming it */

static void spi2_dma_init(void)
{
    GPIO_REG(DPORT_PERIP_CLK_EN) |= DPORT_SPI_DMA_CLK_EN;

    /* Channel 1 for SPI2. */
    uint32_t sel = GPIO_REG(DPORT_SPI_DMA_CHAN_SEL);
    sel &= ~(3u << 2);
    sel |=  (1u << 2);
    GPIO_REG(DPORT_SPI_DMA_CHAN_SEL) = sel;

    /* Pulse the resets, then leave burst mode enabled. */
    GPIO_REG(SPI2_DMA_CONF) |= DMA_OUT_RST | DMA_AHBM_FIFO_RST | DMA_AHBM_RST;
    GPIO_REG(SPI2_DMA_CONF) &= ~(DMA_OUT_RST | DMA_AHBM_FIFO_RST | DMA_AHBM_RST);
    GPIO_REG(SPI2_DMA_CONF) |= DMA_OUTDSCR_BURST | DMA_OUT_DATA_BURST;

    g_dma_ok = 1;
}

/* Returns 1 if the transfer completed, 0 if it timed out. A timeout disables
 * DMA permanently rather than retrying: a DMA engine that missed one completion
 * has no reason to be trusted with the next, and the FIFO path still works. */
/* Starting and waiting are separate so a caller can do useful work in between.
 *
 * The blocking spi2_dma_tx() below is these two back to back, which is what
 * every caller wanted until the raycaster's full-screen blit made the gap worth
 * filling: 224 transfers per frame, each with the CPU idle for the whole of it.
 *
 * The descriptor is a parameter rather than a global because a pipelined caller
 * has one transfer in flight while it prepares the next, and the engine is
 * still reading the first descriptor at that moment. One descriptor would be
 * rewritten underneath a live transfer. */
static void spi2_dma_start(dma_desc_t *d, const uint8_t *data, uint32_t n)
{
    uint32_t t_enter = xt_ccount();
    GPIO_REG(SPI2_DMA_INT_CLR) = 0xFFFFFFFFu;

    d->flags = (n & 0xFFFu)              /* size   */
             | ((n & 0xFFFu) << 12)      /* length */
             | (1u << 30)                /* eof    */
             | (1u << 31);               /* owned by the DMA engine */
    d->buf   = (uint32_t)data;
    d->next  = 0;

    GPIO_REG(SPI2_DMA_OUT_LINK) = ((uint32_t)d & 0xFFFFFu) | DMA_OUTLINK_START;

    GPIO_REG(SPI2_MOSI_DLEN) = n * 8u - 1u;
    GPIO_REG(SPI2_CMD)       = SPI_USR_BIT;

    g_dma_busy = 1;
    g_bytes   += n;
    g_setup_cycles += xt_ccount() - t_enter;
}

/* Returns 0 if the engine stalled, having disabled DMA for the rest of the run.
 *
 * Bounded wait. 2,000,000 cycles is ~25 ms at 80 MHz, far beyond the ~100 us a
 * 480-byte transfer needs, and short enough that a stuck engine costs one frame
 * rather than the system. */
static int spi2_dma_wait(void)
{
    if (!g_dma_busy) {
        return 1;
    }

    uint32_t start = xt_ccount();
    uint32_t t_enter = start;
    while (GPIO_REG(SPI2_CMD) & SPI_USR_BIT) {
        if ((xt_ccount() - start) > 2000000u) {
            g_dma_timeouts++;
            g_dma_ok    = 0;
            g_dma_busy  = 0;

            /* Say so, once and loudly.
             *
             * Falling back to the FIFO path is the right call — a stalled
             * engine has not earned the next transfer. Doing it SILENTLY was
             * not: every span becomes 64-byte transactions, a full screen goes
             * from 320 transfers to 2,400, and the fill measured 43 ms at boot
             * takes 79 ms forever after. Nothing surfaced that. It was found by
             * chasing a 1.8x gap between init and runtime through preemption,
             * interrupt masking, CPU clock, SPI divider and transaction size,
             * all of which were innocent.
             *
             * A degradation nobody can see is indistinguishable from the
             * hardware simply being slow, which is exactly the conclusion it
             * led to.*
             * ISOLATED: about eight seconds of the 3D view is enough. Boot and
             * the launcher never trigger it; opening the raycaster reliably
             * does. It is NOT caused by the DMA pipelining added the same day
             * — yesterday's telemetry already showed the blit at 1.92 B/us,
             * which is this fallback's rate, so the 3D view has been running on
             * the FIFO path the whole time it has existed.
             *
             * Why the engine stalls is still unknown. Worth noting before
             * anyone acts on it: if the stall is spurious, resetting the engine
             * and retrying would restore 43 ms fills, and the permanent
             * disable is then costing 84% of the throughput to guard against a
             * fault that may not be real. */
            uart_puts("\n  !! display DMA timed out - FIFO path for the rest of"
                      " this run\n"
                      "     expect roughly half the throughput; 'bytes' shows"
                      " dma_on=0\n");
            return 0;
        }
    }

    g_dma_busy = 0;
    g_dma_transfers++;
    g_wait_cycles += xt_ccount() - t_enter;
    return 1;
}

static int spi2_dma_tx(const uint8_t *data, uint32_t n)
{
    spi2_dma_start(&g_desc, data, n);
    return spi2_dma_wait();
}

uint32_t display_dma_transfers(void) { return g_dma_transfers; }
uint32_t display_dma_timeouts(void)  { return g_dma_timeouts; }

static void spi2_init(void)
{
    GPIO_REG(DPORT_PERIP_CLK_EN) |= DPORT_SPI2_BIT;
    GPIO_REG(DPORT_PERIP_RST_EN) &= ~DPORT_SPI2_BIT;

    GPIO_REG(IO_MUX_GPIO14) = IO_MUX_HSPI_FUNC;   /* SCLK */
    GPIO_REG(IO_MUX_GPIO13) = IO_MUX_HSPI_FUNC;   /* MOSI */

    GPIO_REG(SPI2_SLAVE) = 0;                     /* master                   */
    GPIO_REG(SPI2_PIN)   = 0x7u;                  /* peripheral drives no CS  */
    GPIO_REG(SPI2_USER)  = SPI_USR_MOSI_BIT;      /* write phase only, mode 0 */
    GPIO_REG(SPI2_USER1) = 0;
    GPIO_REG(SPI2_USER2) = 0;
    GPIO_REG(SPI2_CTRL)  = 0;                     /* MSB first                */
    GPIO_REG(SPI2_CTRL2) = 0;
    GPIO_REG(SPI2_CLOCK) = SPI2_CLKDIV;

    /* Read back rather than assume. A clock register that did not take, or a
     * DPORT bit that did not stick, is the difference between a fast display
     * and a black one. */
    spi2_dma_init();

    g_spi2_clk_reg = GPIO_REG(SPI2_CLOCK);
    g_spi2_dport   = GPIO_REG(DPORT_PERIP_CLK_EN);
}

/* Up to 64 bytes per transaction: the sixteen W registers are the whole FIFO.
 * Bytes leave in address order within a word, MSB first within a byte. */
static void spi2_tx(const uint8_t *data, uint32_t n)
{
    while (n) {
        uint32_t chunk = (n > 64u) ? 64u : n;

        for (uint32_t w = 0; w < (chunk + 3u) / 4u; w++) {
            uint32_t word = 0;
            for (uint32_t b = 0; b < 4u; b++) {
                uint32_t idx = w * 4u + b;
                if (idx < chunk) {
                    word |= (uint32_t)data[idx] << (8u * b);
                }
            }
            GPIO_REG(SPI2_W(w)) = word;
        }

        GPIO_REG(SPI2_MOSI_DLEN) = chunk * 8u - 1u;
        GPIO_REG(SPI2_CMD)       = SPI_USR_BIT;

        /* Bounded only in panic mode. A healthy controller retires a 64-byte
         * transfer in microseconds, so the bound is never reached in normal
         * operation and costs a comparison; in a panic an unretired transfer
         * must not be allowed to consume the one chance to report a fault. */
        uint32_t spins = 0;
        while (GPIO_REG(SPI2_CMD) & SPI_USR_BIT) {
            if (g_panic_mode && ++spins > 4000000u) {
                break;
            }
        }

        data    += chunk;
        n       -= chunk;
        g_bytes += chunk;
    }
}

uint32_t display_spi_clock_reg(void) { return g_spi2_clk_reg; }
uint32_t display_dport_reg(void)     { return g_spi2_dport; }

/* ---- bit-banged SPI, mode 0 --------------------------------------------
 * Data is presented on MOSI while the clock is low and sampled by the panel on
 * the rising edge. No explicit delays: each GPIO write is a peripheral store
 * costing several cycles, which keeps the clock well inside the ILI9341's
 * limits without having to tune anything.
 *
 * MEASURED: 153,600 bytes in 387 ms, about 397 kB/s, an effective clock near
 * 3.2 MHz. The estimate written here first was ~150 ms — wrong by 2.6x, because
 * a peripheral-register store costs rather more than the few cycles assumed.
 * 2.6 full screens per second is ample for a status display and far too slow
 * for animation; that is the number that decides whether SPI2 is worth bringing
 * up, so it is measured rather than asserted.
 */
static void spi_write(uint8_t b)
{
    for (int i = 7; i >= 0; i--) {
        if (b & (1u << i)) {
            gpio_set(PIN_MOSI);
        } else {
            gpio_clear(PIN_MOSI);
        }
        gpio_set(PIN_SCLK);
        gpio_clear(PIN_SCLK);
    }
    g_bytes++;
}

/* Single egress point. Both backends implement this and nothing else touches
 * the transport, so switching them cannot leave a stray path behind. */
static void spi_tx(const uint8_t *data, uint32_t n)
{
#if DISPLAY_USE_SPI2
    /* Short transfers stay on the FIFO path: below roughly a FIFO's worth, the
     * descriptor setup costs more than it saves. Commands and their few
     * parameter bytes are all in that range. */
    if (g_dma_ok && !g_panic_mode && n > 64u) {
        g_took_dma++;
        if (spi2_dma_tx(data, n)) {
            return;
        }
        /* Fell through: DMA timed out and is now disabled. The FIFO path
         * below still works, so the display degrades rather than stopping. */
    }
    g_took_fifo++;
    spi2_tx(data, n);
#else
    for (uint32_t i = 0; i < n; i++) {
        spi_write(data[i]);
    }
#endif
}

static void write_cmd(uint8_t c)
{
    gpio_clear(PIN_DC);                 /* DC low = command */
    gpio_clear(PIN_CS);
    spi_tx(&c, 1);
    gpio_set(PIN_CS);
}

/* Command followed by its parameters, holding CS across the whole exchange —
 * the panel latches a command and its data as one transaction. */
static void write_cmd_data(uint8_t c, const uint8_t *data, uint32_t n)
{
    gpio_clear(PIN_DC);
    gpio_clear(PIN_CS);
    spi_tx(&c, 1);
    if (n) {
        gpio_set(PIN_DC);
        spi_tx(data, n);
    }
    gpio_set(PIN_CS);
}

/* ---- panel ------------------------------------------------------------- */

/* Sets the rectangle subsequent pixel writes fill, then leaves the panel in
 * RAMWR with CS asserted so the caller can stream pixels. */
static void set_window(uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1)
{
    uint8_t col[4] = { (uint8_t)(x0 >> 8), (uint8_t)x0, (uint8_t)(x1 >> 8), (uint8_t)x1 };
    uint8_t row[4] = { (uint8_t)(y0 >> 8), (uint8_t)y0, (uint8_t)(y1 >> 8), (uint8_t)y1 };

    write_cmd_data(0x2A, col, 4);       /* CASET — column address */
    write_cmd_data(0x2B, row, 4);       /* PASET — page address   */

    gpio_clear(PIN_DC);
    gpio_clear(PIN_CS);
    static const uint8_t ramwr = 0x2C;
    spi_tx(&ramwr, 1);                  /* RAMWR — memory write   */
    gpio_set(PIN_DC);                   /* everything after is data; CS stays low */
}

/* Byte-swapped copy of the span. RGB565 goes out high byte first, the opposite
 * of how it sits in memory. Sent as one transfer: with a hardware FIFO the
 * per-call overhead dominates if this is done a byte at a time. */
static uint8_t g_txbuf[LINE_MAX * 2u];
static uint8_t g_txbuf2[LINE_MAX * 2u];     /* filled while g_txbuf is in flight */

/* The panel wants big-endian; the framebuffer is little-endian. This is the
 * conversion that used to sit between every transfer and the next, with the bus
 * idle throughout. */
static void swap_into(uint8_t *dst, const uint16_t *px, uint32_t n)
{
    for (uint32_t i = 0; i < n; i++) {
        dst[i * 2u]      = (uint8_t)(px[i] >> 8);
        dst[i * 2u + 1u] = (uint8_t)px[i];
    }
}

static void push_pixels(const uint16_t *px, uint32_t n)
{
    swap_into(g_txbuf, px, n);
    spi_tx(g_txbuf, n * 2u);
}

/* Sends a contiguous run of pixels with the conversion overlapped against the
 * transfer.
 *
 * The serial version alternated: convert a span with the bus idle, transfer it
 * with the CPU idle, repeat. Two resources, each roughly half idle, for 224
 * spans a frame. Here span N+1 is converted into the other buffer while span N
 * is still going out, so the conversion disappears behind the transfer and the
 * bus stops waiting for it.
 *
 * Falls back to the blocking path if DMA is unavailable or stalls mid-run —
 * a degraded frame rate is a better outcome than a torn frame or a hang. *
 * MEASURED GAIN: inconclusive. The raycaster's blit went 55.9 ms to 55.5 ms,
 * which is inside the noise. The conversion this hides is only about 4 ms of
 * that total, and the figure it is measured against turns out to be wall-clock
 * across preemptions rather than transfer time — see display_fill_bench().
 * Kept because it is correct and costs one 480-byte buffer, not because it was
 * shown to be faster. */
static void push_pixels_stream(const uint16_t *px, uint32_t total)
{
    if (!g_dma_ok || g_panic_mode || total <= LINE_MAX) {
        while (total) {
            uint32_t chunk = (total > LINE_MAX) ? LINE_MAX : total;
            push_pixels(px, chunk);
            px    += chunk;
            total -= chunk;
        }
        return;
    }

    uint8_t     *buf[2]  = { g_txbuf, g_txbuf2 };
    dma_desc_t  *desc[2] = { &g_desc, &g_desc2 };
    int cur = 0;

    uint32_t chunk = (total > LINE_MAX) ? LINE_MAX : total;
    swap_into(buf[cur], px, chunk);
    spi2_dma_start(desc[cur], buf[cur], chunk * 2u);
    px    += chunk;
    total -= chunk;

    while (total) {
        int nxt = 1 - cur;
        uint32_t next_chunk = (total > LINE_MAX) ? LINE_MAX : total;

        /* The whole point: this runs while the previous span is on the wire. */
        swap_into(buf[nxt], px, next_chunk);

        if (!spi2_dma_wait()) {
            /* The engine stalled and DMA is now off for the run. Finish what is
             * left the slow way rather than leaving the window half written. */
            while (total) {
                uint32_t c = (total > LINE_MAX) ? LINE_MAX : total;
                push_pixels(px, c);
                px    += c;
                total -= c;
            }
            return;
        }

        spi2_dma_start(desc[nxt], buf[nxt], next_chunk * 2u);
        px    += next_chunk;
        total -= next_chunk;
        cur    = nxt;
    }

    (void)spi2_dma_wait();
}

static void push_end(void)
{
    gpio_set(PIN_CS);
}

int display_init(void)
{
    mutex_init(&g_lock);

    gpio_out_init(PIN_MOSI, IO_MUX_GPIO13);
    gpio_out_init(PIN_SCLK, IO_MUX_GPIO14);
#if DISPLAY_USE_SPI2
    spi2_init();                        /* re-routes SCLK and MOSI to HSPI */
#endif
    gpio_out_init(PIN_CS,   IO_MUX_GPIO15);
    gpio_out_init(PIN_DC,   IO_MUX_GPIO2);
    gpio_out_init(PIN_BL,   IO_MUX_GPIO21);

    gpio_set(PIN_CS);                   /* idle high */
    gpio_clear(PIN_SCLK);               /* mode 0 idles low */
    gpio_clear(PIN_BL);                 /* dark until there is something to show */

    /* No reset pin on this board — the panel's RST follows the ESP32's, so a
     * software reset is the only one available after boot. */
    write_cmd(0x01);                    /* SWRESET */
    delay_ms(150);

    static const uint8_t pwctr1[]  = { 0x23 };
    static const uint8_t pwctr2[]  = { 0x10 };
    static const uint8_t vmctr1[]  = { 0x3E, 0x28 };
    static const uint8_t vmctr2[]  = { 0x86 };
    /* MADCTL 0x48: column order flipped, BGR panel. CONFIRMED on hardware by
     * the colour strip rendering red-leftmost. Had the BGR bit been wrong, red
     * and blue would be transposed system-wide and every screen would still
     * look entirely plausible in isolation — only the strip's known ORDER
     * catches it. */
    static const uint8_t madctl[]  = { 0x48 };
    static const uint8_t pixfmt[]  = { 0x55 };   /* 16 bits per pixel */
    static const uint8_t frmctr1[] = { 0x00, 0x18 };
    static const uint8_t dfunctr[] = { 0x08, 0x82, 0x27 };

    write_cmd_data(0xC0, pwctr1,  sizeof pwctr1);
    write_cmd_data(0xC1, pwctr2,  sizeof pwctr2);
    write_cmd_data(0xC5, vmctr1,  sizeof vmctr1);
    write_cmd_data(0xC7, vmctr2,  sizeof vmctr2);
    write_cmd_data(0x36, madctl,  sizeof madctl);
    write_cmd_data(0x3A, pixfmt,  sizeof pixfmt);
    write_cmd_data(0xB1, frmctr1, sizeof frmctr1);
    write_cmd_data(0xB6, dfunctr, sizeof dfunctr);

    write_cmd(0x11);                    /* SLPOUT — leave sleep */
    delay_ms(120);
    write_cmd(0x29);                    /* DISPON */
    delay_ms(20);

    /* Measured rather than estimated: throughput is the first thing anyone
     * asks of a bit-banged driver, and it decides whether the SPI peripheral is
     * worth the risk of bringing up. */
    uint32_t t0 = xt_ccount();
    display_clear(COLOR_BLACK);
    g_last_fill_cycles = xt_ccount() - t0;

    gpio_set(PIN_BL);                   /* backlight on once the panel is clean */
    g_ready = 1;
    return 0;
}

/* Public batch lock, routed through the instrumented helpers so a caller
 * holding it across many primitives is measured the same as one that does
 * not — the raycaster is exactly such a caller. */
void display_lock(void)   { draw_lock(); }
void display_unlock(void) { draw_unlock(); }

/* ---- panic mode --------------------------------------------------------
 *
 * A panic cannot honour any of this driver's normal assumptions, so it
 * suspends three of them at once and never restores them. The kernel is on
 * its way to a halt; there is nothing to restore them for.
 *
 *   - THE LOCK. Blocking on a mutex means task_block() and a yield, and a
 *     panic runs with the scheduler stopped. Waiting for a lock whose owner
 *     will never run again is a hang, and a hang here costs the fault report.
 *   - DMA. The descriptors may describe a transfer already in flight, or a
 *     buffer belonging to whatever just died. The FIFO path touches nothing
 *     but the controller.
 *   - THE UNBOUNDED FIFO WAIT. spi2_tx() spins until the controller retires a
 *     transfer, which is correct while the system is healthy and an infinite
 *     loop once it is not.
 *
 * Deliberately one-way. There is no display_panic_clear(): a driver that can
 * be talked back out of panic mode invites somebody to try. */
/* Lock timing.
 *
 * The mutex already counts acquisitions and contentions, which answers "how
 * often" and not "for how long" — and a renderer that is promptly scheduled,
 * not sleeping, and still delivering three frames a second is a question about
 * duration.
 *
 * Only the OUTERMOST acquisition is timed. The lock is recursive by design
 * (display_clear draws through display_fill_rect), so counting every nested
 * take would measure the nesting rather than the hold. Depth reaching 1 is the
 * moment the lock actually changed hands. */
/* Wait attributed per task.
 *
 * The aggregate says the renderer is blocked; it cannot say blocked BY WHOM,
 * and the two readings suggest opposite fixes. Making application draws
 * non-blocking helps only if applications are the ones waiting; shortening
 * application holds helps only if the renderer is. Guessing between them has
 * already been wrong three times this session. */
static uint32_t g_blocked_by_task[TASK_MAX];

/* Named "blocked", not "wait", and the distinction is the finding.
  *
  * This is measured from asking for the lock to holding it, and a task that
  * cannot have it is DESCHEDULED for the interval. So the number includes the
  * time after the lock became free but before the scheduler picked this task up
  * again — which turned out to be most of it: 63 ms per contention against a
  * 24 ms hold.
  *
  * Calling it "lock wait" would say the panel was busy for 63 ms, which is
  * false and would send the next person to shorten holds. Shortening the hold
  * 25% moved this number by 0%. */
static uint32_t g_lock_blocked_cy;     /* cycles between asking and holding */
static uint32_t g_lock_hold_cy;     /* cycles spent holding             */
static uint32_t g_lock_takes;       /* outermost acquisitions           */
static uint32_t g_hold_start;

static void draw_lock(void)
{
    if (g_panic_mode) {
        return;
    }
    uint32_t t0 = xt_ccount();
    mutex_lock(&g_lock);
    uint32_t t1 = xt_ccount();
    if (g_lock.depth == 1u) {
        g_lock_blocked_cy += t1 - t0;
        g_lock_takes++;
        g_hold_start = t1;

        int me = task_current();
        if (me >= 0 && me < TASK_MAX) {
            g_blocked_by_task[me] += (t1 - t0) / 80000u;   /* ms */
        }
    }
}

static void draw_unlock(void)
{
    if (g_panic_mode) {
        return;
    }
    if (g_lock.depth == 1u) {
        g_lock_hold_cy += xt_ccount() - g_hold_start;
    }
    mutex_unlock(&g_lock);
}

uint32_t display_lock_blocked_ms(void)     { return g_lock_blocked_cy / 80000u; }
uint32_t display_lock_hold_ms(void)     { return g_lock_hold_cy / 80000u; }
uint32_t display_lock_takes(void)       { return g_lock_takes; }
uint32_t display_lock_contentions(void) { return g_lock.contentions; }

/* Non-blocking acquisition, for callers whose drawing is optional.
 *
 * Blocking on this mutex is expensive out of all proportion to the lock:
 * measured, a contended acquisition costs ~63 ms while the lock itself is only
 * held ~24 ms, because the blocked task is descheduled and must be selected
 * again. The renderer and the applications were each blocked most of the time
 * waiting for a lock that was actually free 91% of the time.
 *
 * An application that cannot draw right now should skip the primitive, not
 * stop. A dropped fill costs one frame of one application; a blocked task costs
 * the whole system a scheduling round trip. */
int display_try_lock(void)
{
    if (g_panic_mode) {
        return 1;
    }
    return mutex_try_lock(&g_lock);
}

uint32_t display_lock_blocked_of(int task_id)
{
    return (task_id >= 0 && task_id < TASK_MAX) ? g_blocked_by_task[task_id] : 0;
}

void display_enter_panic_mode(void) { g_panic_mode = 1; }
int  display_ready(void)            { return g_ready; }

void display_backlight(int on)
{
    if (on) {
        gpio_set(PIN_BL);
    } else {
        gpio_clear(PIN_BL);
    }
}

void display_fill_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint16_t colour)
{
    if (x >= DISP_W || y >= DISP_H || w == 0u || h == 0u) {
        return;
    }
    draw_lock();
    if (x + w > DISP_W) { w = DISP_W - x; }
    if (y + h > DISP_H) { h = DISP_H - y; }

    for (uint32_t i = 0; i < w && i < LINE_MAX; i++) {
        g_line[i] = colour;
    }

    set_window(x, y, x + w - 1u, y + h - 1u);
    for (uint32_t row = 0; row < h; row++) {
        push_pixels(g_line, w);         /* one span at a time — no framebuffer */
    }
    push_end();
    draw_unlock();
}

void display_blit(uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                  const uint16_t *src, uint32_t src_stride)
{
    if (x >= DISP_W || y >= DISP_H || w == 0u || h == 0u || w > LINE_MAX) {
        return;
    }
    if (w > DISP_W - x) { w = DISP_W - x; }
    if (h > DISP_H - y) { h = DISP_H - y; }

    draw_lock();
    set_window(x, y, x + w - 1u, y + h - 1u);

    if (src_stride == w) {
        /* Contiguous source: the whole rectangle is one linear stream, because
         * the panel walks the window itself. Sent in buffer-sized chunks rather
         * than row by row.
         *
         * This matters most where it looks least likely to. A 1-pixel-wide
         * column has 168 rows of one pixel each, so the row-by-row path issues
         * 168 two-byte transfers and a raycaster column costs ~170 ms. The same
         * data as one stream is a single transfer. */
        push_pixels_stream(src, w * h);
    } else {
        for (uint32_t row = 0; row < h; row++) {
            /* push_pixels byte-swaps into the transmit buffer, so the source is
             * never modified and need not be aligned to anything but a pixel. */
            push_pixels(src + (uint32_t)row * src_stride, w);
        }
    }

    push_end();
    draw_unlock();
}

void display_clear(uint16_t colour)
{
    display_fill_rect(0, 0, DISP_W, DISP_H, colour);
}

/* ---- text ---------------------------------------------------------------
 * A 5x8 column-encoded font: each glyph is five bytes, each byte one column,
 * bit 0 at the top. A sixth blank column separates characters. Only 32..126 are
 * present; anything else prints as a space.
 *
 * It lives in .rodata, which since UM-NATOS-011 is mapped from flash and costs
 * no DRAM at all.
 */
static const uint8_t FONT5X8[95][5] = {
    {0x00,0x00,0x00,0x00,0x00}, {0x00,0x00,0x5F,0x00,0x00}, {0x00,0x07,0x00,0x07,0x00},
    {0x14,0x7F,0x14,0x7F,0x14}, {0x24,0x2A,0x7F,0x2A,0x12}, {0x23,0x13,0x08,0x64,0x62},
    {0x36,0x49,0x55,0x22,0x50}, {0x00,0x05,0x03,0x00,0x00}, {0x00,0x1C,0x22,0x41,0x00},
    {0x00,0x41,0x22,0x1C,0x00}, {0x14,0x08,0x3E,0x08,0x14}, {0x08,0x08,0x3E,0x08,0x08},
    {0x00,0x50,0x30,0x00,0x00}, {0x08,0x08,0x08,0x08,0x08}, {0x00,0x60,0x60,0x00,0x00},
    {0x20,0x10,0x08,0x04,0x02}, {0x3E,0x51,0x49,0x45,0x3E}, {0x00,0x42,0x7F,0x40,0x00},
    {0x42,0x61,0x51,0x49,0x46}, {0x21,0x41,0x45,0x4B,0x31}, {0x18,0x14,0x12,0x7F,0x10},
    {0x27,0x45,0x45,0x45,0x39}, {0x3C,0x4A,0x49,0x49,0x30}, {0x01,0x71,0x09,0x05,0x03},
    {0x36,0x49,0x49,0x49,0x36}, {0x06,0x49,0x49,0x29,0x1E}, {0x00,0x36,0x36,0x00,0x00},
    {0x00,0x56,0x36,0x00,0x00}, {0x00,0x08,0x14,0x22,0x41}, {0x14,0x14,0x14,0x14,0x14},
    {0x41,0x22,0x14,0x08,0x00}, {0x02,0x01,0x51,0x09,0x06}, {0x32,0x49,0x79,0x41,0x3E},
    {0x7E,0x11,0x11,0x11,0x7E}, {0x7F,0x49,0x49,0x49,0x36}, {0x3E,0x41,0x41,0x41,0x22},
    {0x7F,0x41,0x41,0x22,0x1C}, {0x7F,0x49,0x49,0x49,0x41}, {0x7F,0x09,0x09,0x01,0x01},
    {0x3E,0x41,0x41,0x51,0x32}, {0x7F,0x08,0x08,0x08,0x7F}, {0x00,0x41,0x7F,0x41,0x00},
    {0x20,0x40,0x41,0x3F,0x01}, {0x7F,0x08,0x14,0x22,0x41}, {0x7F,0x40,0x40,0x40,0x40},
    {0x7F,0x02,0x04,0x02,0x7F}, {0x7F,0x04,0x08,0x10,0x7F}, {0x3E,0x41,0x41,0x41,0x3E},
    {0x7F,0x09,0x09,0x09,0x06}, {0x3E,0x41,0x51,0x21,0x5E}, {0x7F,0x09,0x19,0x29,0x46},
    {0x46,0x49,0x49,0x49,0x31}, {0x01,0x01,0x7F,0x01,0x01}, {0x3F,0x40,0x40,0x40,0x3F},
    {0x1F,0x20,0x40,0x20,0x1F}, {0x7F,0x20,0x18,0x20,0x7F}, {0x63,0x14,0x08,0x14,0x63},
    {0x03,0x04,0x78,0x04,0x03}, {0x61,0x51,0x49,0x45,0x43}, {0x00,0x00,0x7F,0x41,0x41},
    {0x02,0x04,0x08,0x10,0x20}, {0x41,0x41,0x7F,0x00,0x00}, {0x04,0x02,0x01,0x02,0x04},
    {0x40,0x40,0x40,0x40,0x40}, {0x00,0x01,0x02,0x04,0x00}, {0x20,0x54,0x54,0x54,0x78},
    {0x7F,0x48,0x44,0x44,0x38}, {0x38,0x44,0x44,0x44,0x20}, {0x38,0x44,0x44,0x48,0x7F},
    {0x38,0x54,0x54,0x54,0x18}, {0x08,0x7E,0x09,0x01,0x02}, {0x08,0x14,0x54,0x54,0x3C},
    {0x7F,0x08,0x04,0x04,0x78}, {0x00,0x44,0x7D,0x40,0x00}, {0x20,0x40,0x44,0x3D,0x00},
    {0x00,0x7F,0x10,0x28,0x44}, {0x00,0x41,0x7F,0x40,0x00}, {0x7C,0x04,0x18,0x04,0x78},
    {0x7C,0x08,0x04,0x04,0x78}, {0x38,0x44,0x44,0x44,0x38}, {0x7C,0x14,0x14,0x14,0x08},
    {0x08,0x14,0x14,0x18,0x7C}, {0x7C,0x08,0x04,0x04,0x08}, {0x48,0x54,0x54,0x54,0x20},
    {0x04,0x3F,0x44,0x40,0x20}, {0x3C,0x40,0x40,0x20,0x7C}, {0x1C,0x20,0x40,0x20,0x1C},
    {0x3C,0x40,0x30,0x40,0x3C}, {0x44,0x28,0x10,0x28,0x44}, {0x0C,0x50,0x50,0x50,0x3C},
    {0x44,0x64,0x54,0x4C,0x44}, {0x00,0x08,0x36,0x41,0x00}, {0x00,0x00,0x7F,0x00,0x00},
    {0x00,0x41,0x36,0x08,0x00}, {0x08,0x08,0x2A,0x1C,0x08},
};

#define GLYPH_W 6u
#define GLYPH_H 8u

void display_text(uint32_t x, uint32_t y, const char *s, uint16_t fg, uint16_t bg,
                  uint32_t scale)
{
    if (scale == 0u) {
        scale = 1u;
    }

    draw_lock();
    for (; *s; s++) {
        uint32_t cw = GLYPH_W * scale;
        if (x >= DISP_W || cw > LINE_MAX) {
            break;
        }

        unsigned char ch = (unsigned char)*s;
        const uint8_t *g = (ch >= 32u && ch < 127u) ? FONT5X8[ch - 32u] : FONT5X8[0];

        /* Rendered a row at a time into the span buffer, so a glyph costs one
         * window and 8*scale spans rather than a per-pixel round trip. */
        set_window(x, y, x + cw - 1u, y + GLYPH_H * scale - 1u);
        for (uint32_t row = 0; row < GLYPH_H; row++) {
            for (uint32_t col = 0; col < GLYPH_W; col++) {
                /* Column 5 is the inter-character gap and is always background. */
                int on = (col < 5u) && ((g[col] >> row) & 1u);
                uint16_t c = on ? fg : bg;
                for (uint32_t sx = 0; sx < scale; sx++) {
                    g_line[col * scale + sx] = c;
                }
            }
            for (uint32_t sy = 0; sy < scale; sy++) {
                push_pixels(g_line, cw);
            }
        }
        push_end();

        x += cw;
    }
    draw_unlock();
}

uint32_t display_bytes_written(void) { return g_bytes; }
uint32_t display_spi_clock_live(void) { return GPIO_REG(SPI2_CLOCK); }

/* Splits a transfer's cost into time the BUS was busy and time the CPU spent
 * programming it. If the bus figure alone accounts for the wall-clock, the
 * panel is the limit and no software change will help; if setup dominates, the
 * transactions are too small and the fix is fewer, larger ones. */
uint32_t display_took_dma(void)  { return g_took_dma; }
uint32_t display_took_fifo(void) { return g_took_fifo; }
uint32_t display_wait_us(void)  { return g_wait_cycles / 80u; }
uint32_t display_setup_us(void) { return g_setup_cycles / 80u; }
void     display_reset_profile(void) { g_wait_cycles = 0; g_setup_cycles = 0; }
int      display_dma_enabled(void)    { return g_dma_ok; }
uint32_t display_fill_cycles(void)   { return g_last_fill_cycles; }
int      display_owner(void)         { return mutex_owner(&g_lock); }

/* Times a full-screen fill at runtime and returns the cost in microseconds.
 *
 * The same work display_init() measures at 43 ms, but run from a task with the
 * scheduler live. init's figure is single-threaded — nothing else exists yet —
 * so the difference between the two is time the display task spent descheduled
 * rather than time the panel spent receiving.
 *
 * That distinction decides what the raycaster's 55 ms blit means: whether the
 * bus is slow, or the measurement is wall-clock across preemptions. */
uint32_t display_fill_bench(void)
{
    uint32_t t0 = task_cpu_cycles();
    display_fill_rect(0, 0, DISP_W, DISP_H, 0x0000u);
    return (task_cpu_cycles() - t0) / 80u;
}

/* The same fill with interrupts masked for its whole duration.
 *
 * display_fill_bench() removed preemption from the MEASUREMENT; this removes it
 * from the RUN. The difference between the two is everything interrupts cost
 * while a task is nominally running: the handler itself, the scheduler it
 * calls, and — the reason this is worth knowing — whatever those do to the
 * instruction cache this kernel executes from flash through.
 *
 * If the masked figure matches init's 43 ms, the whole gap is interrupt-borne
 * and no amount of DMA tuning will touch it. If it does not, the cause is
 * somewhere else entirely and the search moves.
 *
 * Masking for ~50 ms makes the tick late once. timer.c bounds its deadline in
 * both directions and counts it, which is exactly the case that was built for. */
uint32_t display_fill_bench_masked(void)
{
    /* The lock is taken BEFORE masking, and this is not a detail.
     *
     * The first version masked first and called display_fill_rect(), which
     * takes the draw lock. If the display task held it, the shell blocked —
     * and a blocked task with interrupts masked can never be switched back in.
     * The board deadlocked and the hardware watchdog reset it: rst:0x7
     * TG0WDT_SYS_RESET.
     *
     * The panic-mode note forty lines up says exactly this about exactly this
     * lock. Reading it would have been cheaper than the reset.
     *
     * Acquiring first works because the lock is recursive: the acquisition
     * inside display_fill_rect() is already owned by this task and cannot
     * block. Nothing waits while interrupts are off. */
    display_lock();

    uint32_t crit = crit_enter();
    uint32_t t0 = xt_ccount();
    display_fill_rect(0, 0, DISP_W, DISP_H, 0x0000u);
    uint32_t d = xt_ccount() - t0;
    crit_exit(crit);

    display_unlock();
    return d / 80u;
}
