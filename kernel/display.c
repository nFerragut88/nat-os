/* cyd-os — ILI9341 driver. See display.h for the pin map and the reasoning. */

#include "display.h"
#include "gpio.h"
#include "mutex.h"
#include "xtensa.h"

#define PIN_MOSI 13u
#define PIN_SCLK 14u
#define PIN_CS   15u
#define PIN_DC    2u
#define PIN_BL   21u

/* Derived in UM-CYDOS-008 §5.2 from the measured tick rate. Only used for the
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

static void write_cmd(uint8_t c)
{
    gpio_clear(PIN_DC);                 /* DC low = command */
    gpio_clear(PIN_CS);
    spi_write(c);
    gpio_set(PIN_CS);
}

/* Command followed by its parameters, holding CS across the whole exchange —
 * the panel latches a command and its data as one transaction. */
static void write_cmd_data(uint8_t c, const uint8_t *data, uint32_t n)
{
    gpio_clear(PIN_DC);
    gpio_clear(PIN_CS);
    spi_write(c);
    if (n) {
        gpio_set(PIN_DC);
        for (uint32_t i = 0; i < n; i++) {
            spi_write(data[i]);
        }
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
    spi_write(0x2C);                    /* RAMWR — memory write   */
    gpio_set(PIN_DC);                   /* everything after is data; CS stays low */
}

static void push_pixels(const uint16_t *px, uint32_t n)
{
    for (uint32_t i = 0; i < n; i++) {
        spi_write((uint8_t)(px[i] >> 8));
        spi_write((uint8_t)px[i]);
    }
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
    return 0;
}

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
    mutex_lock(&g_lock);
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
    mutex_unlock(&g_lock);
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
 * It lives in .rodata, which since UM-CYDOS-011 is mapped from flash and costs
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

    mutex_lock(&g_lock);
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
    mutex_unlock(&g_lock);
}

uint32_t display_bytes_written(void) { return g_bytes; }
uint32_t display_fill_cycles(void)   { return g_last_fill_cycles; }
