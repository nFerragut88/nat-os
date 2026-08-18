# Chapter 18 — The Display: From 387 ms to 43 ms, and a Stall That Was Never There

> **The stall is closed** (UM-NATOS-030). The title of this chapter used to end
> "and a Stall Still Open", and the change is not cosmetic: §18.9 investigates a
> fault whose *mechanism was real and whose cause was somewhere else entirely*.
> It is kept in full, wrong conclusions included, because what it gets wrong is
> more instructive than what it gets right. Read §18.9's closing note before
> acting on anything in it.

> Sources: `docs/UM-NATOS-015-display.md`
> Code: `kernel/display.c`, `kernel/display.h`, `kernel/gpio.h`

---

## 18.1 The claim

nat-os drives the CYD's ILI9341: 240×320, 16-bit colour, filled regions, text
and bitmaps, from a native task showing live kernel state.

> There is **no framebuffer anywhere in the system**. UM-NATOS-010 §7.2 argued
> that one should not exist — the panel holds the image in its own GRAM, so a
> host copy is a redundant 153,600 B against a 158,000 B heap. This report is
> that argument implemented: every pixel is rendered through a **480-byte** span
> buffer.

That claim held for the driver and was later relaxed for one specific consumer:
the 3D view acquired an 80,640 B framebuffer, which was measured as worthless,
defaulted off, and then — §18.7 — measured again and defaulted on. The general
statement remains true: nothing composites a full screen in DRAM.

## 18.2 Pin map, read rather than remembered

| Signal | GPIO | Note |
|---|---|---|
| SCLK | 14 | |
| MOSI | 13 | |
| MISO | 12 | unused — the driver never reads the panel |
| CS | 15 | |
| DC | 2 | low = command, high = data |
| BL | 21 | active high |
| RST | — | **not wired to a GPIO**; follows the ESP32's own reset |

> Taken from the vendor project's active `TFT_eSPI/User_Setup.h` rather than
> from recollection. On a board where a wrong pin and a wrong init sequence
> produce an identical symptom, the difference between reading and remembering
> is the difference between a first-try success and an afternoon.

Because RST is not under software control, the only reset available after boot is
the panel's `0x01 SWRESET`.

## 18.3 Bit-banged first, deliberately and temporarily

Hardware SPI2 requires DPORT peripheral clock-enable bits and GPIO matrix signal
routing. The staging argument:

> Every mistake in either produces **exactly the same symptom** as a wiring
> error, a wrong pin, or a bad init sequence: a black screen with no diagnostic.
> Bringing up four uncertain subsystems at once and then bisecting them from a
> single bit of output is how M2 consumed nine build cycles.
>
> Bit-banging touches only GPIO registers, which `gpio.h` covers completely. A
> failure could therefore only be the panel, the pins, or the commands — three
> candidates instead of seven.

The bit-banged transport is eleven lines:

```c
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
```

with a comment that records a wrong estimate rather than deleting it:

```c
 * MEASURED: 153,600 bytes in 387 ms, about 397 kB/s, an effective clock near
 * 3.2 MHz. The estimate written here first was ~150 ms — wrong by 2.6x, because
 * a peripheral-register store costs rather more than the few cycles assumed.
 * 2.6 full screens per second is ample for a status display and far too slow
 * for animation; that is the number that decides whether SPI2 is worth bringing
 * up, so it is measured rather than asserted.
```

The estimate being wrong by 2.6× is the point:

> The figure is now measured at init and reported over UART, because throughput
> is the number that decides whether SPI2 is worth the risk, and an assertion
> would have decided it wrongly.

The measurement is taken during `display_init()` itself:

```c
    /* Measured rather than estimated: throughput is the first thing anyone
     * asks of a bit-banged driver, and it decides whether the SPI peripheral is
     * worth the risk of bringing up. */
    uint32_t t0 = xt_ccount();
    display_clear(COLOR_BLACK);
    g_last_fill_cycles = xt_ccount() - t0;
```

## 18.4 Rendering without a framebuffer

```c
#define LINE_MAX DISP_W                 /* one full-width span of pixels */

static uint16_t g_line[LINE_MAX];       /* 480 B — the whole "framebuffer" */
```

`display_fill_rect()` composes one row, sets the panel's address window once, then
pushes that span `h` times:

```c
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
```

> The panel's GRAM is the framebuffer. Setting a window and streaming into it is
> not a workaround for having too little RAM; it is how the device is designed to
> be driven, and the 153,600 B figure was only ever the cost of declining to use
> it.

### The window protocol

```c
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
```

CS stays asserted from the RAMWR until `push_end()` raises it. That shape —
"a window command *and* its entire pixel stream under one CS" — is why the
peripheral's CS automation is not used, and, much later, the mechanism behind
the open fault in §18.9.

### Byte order

```c
/* Byte-swapped copy of the span. RGB565 goes out high byte first, the opposite
 * of how it sits in memory. Sent as one transfer: with a hardware FIFO the
 * per-call overhead dominates if this is done a byte at a time. */
static uint8_t g_txbuf[LINE_MAX * 2u];

static void push_pixels(const uint16_t *px, uint32_t n)
{
    for (uint32_t i = 0; i < n; i++) {
        g_txbuf[i * 2u]      = (uint8_t)(px[i] >> 8);
        g_txbuf[i * 2u + 1u] = (uint8_t)px[i];
    }
    spi_tx(g_txbuf, n * 2u);
}
```

### The font is free

A 5×8 column-encoded font, 95 glyphs, 475 bytes, in `.rodata`:

```c
/* ---- text ---------------------------------------------------------------
 * A 5x8 column-encoded font: each glyph is five bytes, each byte one column,
 * bit 0 at the top. A sixth blank column separates characters. Only 32..126 are
 * present; anything else prints as a space.
 *
 * It lives in .rodata, which since UM-NATOS-011 is mapped from flash and costs
 * no DRAM at all.
 */
static const uint8_t FONT5X8[95][5] = {
```

> This is the first consumer of that work, and the reason it was scheduled before
> M4 rather than after.

### Guarded by a recursive mutex

```c
/* The span buffer and the panel's window state are both shared, so two tasks
 * drawing at once would interleave pixel streams into one window. Recursive,
 * because display_clear() draws through display_fill_rect(). */
static mutex_t g_lock;
```

> Recursive matters here: `display_clear()` draws through
> `display_fill_rect()`, so a non-recursive lock would deadlock on the first
> clear.

Chapter 11 §11.3 records that recursion was chosen for a *different* reason —
"a console lock is exactly the kind of thing that acquires a nested acquisition
by accident" — and this is the case that would have found it immediately.

## 18.5 Hardware SPI2

### The deferral paid off

> §3 argues for bit-banging first because a wrong DPORT clock bit or a wrong
> IOMUX selection produces a black screen. ... That argument is what made this
> change cheap when it came. Panel, pins, command sequence and colour order were
> already known good, so the only thing under test was the transport. **A black
> screen could mean exactly one thing.**

### What it took

> The CYD's display pins are the ESP32's **native HSPI pads**, so IOMUX routes
> SCLK and MOSI straight to the peripheral — no GPIO matrix, and none of the
> 40 MHz ceiling the matrix imposes.
>
> CS and DC stay ordinary GPIOs, because the driver holds CS asserted across a
> window command *and* its entire pixel stream, which is not a shape the
> peripheral's CS automation expresses.

```c
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
```

Two details worth keeping:

**The DPORT write is read-modify-write, never a plain store.**

```c
/* DPORT peripheral clock gating. SPI2 is bit 6. Bit 1 in the same register
 * clocks the flash controller this code executes from, so the write is
 * read-modify-write and never a plain store. */
```

A plain store to that register would stop the flash controller the executing
code depends on.

**Both configuration registers are read back**, and reported at boot:

```
display : init ok, bytes=153634 fullscreen=387 ms clk=0x00001001 dport=0x...
```

`clk=0x00001001` confirms the `sysclk/2` divisor took; `dport` bit 6 confirms the
peripheral clock gate stuck.

The bit-banged path is retained behind `DISPLAY_USE_SPI2 0`, and both backends
route through a single egress point:

```c
/* Single egress point. Both backends implement this and nothing else touches
 * the transport, so switching them cannot leave a stray path behind. */
static void spi_tx(const uint8_t *data, uint32_t n)
```

### The result, and why the estimate was wrong

```
387 ms  ->  78 ms      full-screen fill
```

**5×, against the ~12× the report predicted.**

> At 40 MHz the theoretical floor for 153,600 bytes is about 31 ms, so roughly
> 2.5× of what remains is per-transaction overhead. The FIFO holds 64 bytes, so a
> 480-byte span costs eight transactions — sixteen register writes, a start and a
> poll each — and a full screen is **2,560 of them**.
>
> The original estimate treated the clock rate as the only variable. It is the
> transaction count that dominates, and that is precisely what DMA exists to
> remove. **The gap between the estimate and the measurement is more useful than
> either number alone**, which is why both are recorded.

### The clock ceiling is a person looking at the screen

```c
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
```

That comment is the most quotable thing in the driver, and Chapter 28 is built
on its observation. The clock was later made runtime-selectable for exactly this
reason:

```c
/* Runtime-selectable panel clock.
 *
 * The note above records that clocking this panel too fast puts visible noise
 * on the glass while every counter in the kernel reports success. That is
 * exactly the symptom being chased: identical code rendering cleanly at one
 * moment and torn the next, with corrupt=0, dma=N/0 and no timeouts. The limit
 * is the panel and the flex, and the only instrument that can measure it is a
 * person looking at the screen -- so it needs to be changeable while they do. */
uint32_t display_spi_clock_preset(uint32_t which)
```

## 18.6 DMA

The FIFO path costs 2,560 transactions per full screen. DMA takes a whole
480-byte span in one, so a screen becomes 320 transfers.

```
387 ms   bit-banged GPIO
 78 ms   SPI2, CPU-driven FIFO
 43 ms   SPI2 + DMA          dma=320/0
~31 ms   theoretical floor at 40 MHz
```

> 320 transfers and zero timeouts is exactly one per row, which confirms every
> span went through a descriptor rather than falling back.

### Written so failure is diagnosable rather than fatal

> A DMA engine that never asserts completion would hang the display task forever
> and take the system with it — the failure mode this project has already paid
> for twice, in the `task_yield` freeze and the zero-overhead `LOOP` defect, both
> of which presented as a stopped kernel.

So:

- every wait is bounded by `CCOUNT` and counted, never a bare `while`
- a timeout **disables DMA permanently** and falls through to the FIFO path
- the configuration registers are read back and reported beside the counters

```c
/* Returns 1 if the transfer completed, 0 if it timed out. A timeout disables
 * DMA permanently rather than retrying: a DMA engine that missed one completion
 * has no reason to be trusted with the next, and the FIFO path still works. */
static int spi2_dma_tx(const uint8_t *data, uint32_t n)
{
    GPIO_REG(SPI2_DMA_INT_CLR) = 0xFFFFFFFFu;

    g_desc.flags = (n & 0xFFFu)              /* size   */
                 | ((n & 0xFFFu) << 12)      /* length */
                 | (1u << 30)                /* eof    */
                 | (1u << 31);               /* owned by the DMA engine */
    g_desc.buf   = (uint32_t)data;
    g_desc.next  = 0;

    GPIO_REG(SPI2_DMA_OUT_LINK) = ((uint32_t)&g_desc & 0xFFFFFu) | DMA_OUTLINK_START;

    GPIO_REG(SPI2_MOSI_DLEN) = n * 8u - 1u;
    GPIO_REG(SPI2_CMD)       = SPI_USR_BIT;
    /* ... bounded wait ... */
}
```

### Descriptors as words, and buffers in DRAM

```c
/* Hardware descriptor. Written as explicit words rather than bitfields: the bit
 * order of a C bitfield is implementation-defined, and this layout is the
 * silicon's. */
typedef struct {
    uint32_t flags;     /* size:12 | length:12 | offset:5 | sosf:1 | eof:1 | owner:1 */
    uint32_t buf;
    uint32_t next;
} dma_desc_t;
```

> The descriptor and the transmit buffer are both in `.bss`, which the linker
> places in DRAM — a buffer in IRAM would be silently unreachable by the DMA
> engine.

That is a real hazard on this part and it is closed by construction rather than
by convention: `g_desc` and `g_txbuf` are ordinary statics.

### Small transfers stay on the CPU

```c
    /* Short transfers stay on the FIFO path: below roughly a FIFO's worth, the
     * descriptor setup costs more than it saves. Commands and their few
     * parameter bytes are all in that range. */
    if (g_dma_ok && !g_panic_mode && n > 64u) {
        if (spi2_dma_tx(data, n)) {
            return;
        }
        /* Fell through: DMA timed out and is now disabled. The FIFO path
         * below still works, so the display degrades rather than stopping. */
    }
    spi2_tx(data, n);
```

## 18.7 Two claims that did not survive

### The blitter drew row by row

Driving the display from a raycaster rather than a status screen put a very
different load on this layer:

> **`display_blit` drew row by row.** A 1-pixel-wide column therefore became 168
> separate two-byte transfers. When the source is contiguous the whole rectangle
> is one linear stream, because the panel walks the address window itself.

```c
    if (src_stride == w) {
        /* Contiguous source: the whole rectangle is one linear stream, because
         * the panel walks the window itself. Sent in buffer-sized chunks rather
         * than row by row.
         *
         * This matters most where it looks least likely to. A 1-pixel-wide
         * column has 168 rows of one pixel each, so the row-by-row path issues
         * 168 two-byte transfers and a raycaster column costs ~170 ms. The same
         * data as one stream is a single transfer. */
        uint32_t total = w * h;
        while (total) {
            uint32_t chunk = (total > LINE_MAX) ? LINE_MAX : total;
            push_pixels(src, chunk);
            src   += chunk;
            total -= chunk;
        }
    } else {
        for (uint32_t row = 0; row < h; row++) {
            /* push_pixels byte-swaps into the transmit buffer, so the source is
             * never modified and need not be aligned to anything but a pixel. */
            push_pixels(src + (uint32_t)row * src_stride, w);
        }
    }
```

The strided path remains for genuinely strided sources — which is what a *clipped*
blit produces (Chapter 17 §17.6).

### The lock, per column

```
blit time   25,075,460 us  ->  129,000 us      194x
```

> That is UM-NATOS-014 §5.2 exactly — *a blocking mutex is the wrong instrument
> for a lock contended at high frequency* — **rediscovered by walking into it
> after writing it down.**

### The framebuffer that did not pay, and then did

A framebuffer for the view region was added on the reasoning that 120 address
windows per frame must dominate. It was made **switchable at runtime**, and the
switch is what settled it:

```
fb off:  blit 126,650 us
fb on:   blit 131,411 us
```

> **No measurable difference.** 80,640 B for nothing, so it is implemented, kept,
> and defaulted **off**.

The reasoning behind it was wrong twice over:

> `xt_ccount()` deltas taken across preemptible code measure **wall clock**,
> including every moment the task spent descheduled — so the "450 us per window"
> that motivated the framebuffer was mostly other tasks running. Killing the
> applications moved the frame rate from 8 per 6 s to 19, a 2.4× change from
> freeing CPU alone, which located the real limit.

And then — revision 1.4 — the conclusion was overturned:

> **Superseded.** The switch was rechecked and the conclusion did not survive.
> Both figures above were taken on a clock that intermittently stalled
> (Chapter 7 §7.9) and under draw-lock contention that dominated the frame
> (Chapter 11 §11.9). With both fixed the framebuffer is worth having and is now
> the default. The paragraph above is left standing because the *method* — keep
> the switch, recheck the claim — is what produced the correction.

> §5.7's *conclusion* was wrong; its *method* was right, and is why the error was
> findable — **a switch that can be flipped is a claim that can be rechecked.**

That is the most valuable single sentence in the display chapter, and it
generalises: `spiclk`, `touchcfg`, `fb` and `dfreeze` all exist because of it.

### Where the frame goes now

```
march      2.6 ms      one ray per screen column
compose   13.9 ms      240 x 168 pixels into DRAM
blit      41.9 ms      one 80,640-byte window
                       ~9.1 fps
```

> **The frame is bus-bound**: 72% of it is pushing pixels down SPI at 40 MHz,
> which this board's ceiling. So detail is cheap here and pixels are expensive,
> and that shapes what is worth adding.

Chapter 25 is what was added on the strength of that.

## 18.8 Verification

Visual confirmation plus two designed-in diagnostics.

### The colour strip is an instrument, not decoration

> Eight primaries in a known order make a pixel-format or byte-order fault
> immediately visible; a garbled or monochrome strip would have said which of the
> two was wrong, without inference from a photograph.
>
> **Colour order confirmed.** Red renders leftmost, matching the intended
> `RED, GREEN, BLUE, YELLOW, CYAN, MAGENTA, WHITE, GREY`. That validates the BGR
> bit in `MADCTL 0x48`: had the panel's red and blue channels been transposed,
> the strip would have read blue-first and every colour in the system would have
> been mirrored while still looking entirely plausible in isolation. **Distinct
> primaries alone would not have caught it — only their ORDER does**, which is
> why the strip is drawn in a known sequence rather than as arbitrary swatches.

The init sequence carries the confirmation:

```c
    /* MADCTL 0x48: column order flipped, BGR panel. CONFIRMED on hardware by
     * the colour strip rendering red-leftmost. Had the BGR bit been wrong, red
     * and blue would be transposed system-wide and every screen would still
     * look entirely plausible in isolation — only the strip's known ORDER
     * catches it. */
    static const uint8_t madctl[]  = { 0x48 };
```

### The advancing marker

> The advancing marker block is the same idea applied to liveness: a display
> whose numbers all look plausible but never change is otherwise
> indistinguishable from a working one.

Chapter 17 §17.9 is the session where that instrument was built, documented, and
then not read while it sat frozen.

### The byte count that is exactly right

```
display : init ok, bytes=153634 fullscreen=387 ms
```

> `153,634` is 240×320×2 for the initial clear plus 34 command bytes — the exact
> figure a correct full-screen fill produces, which is what made it usable as a
> check **before anything was known to be visible**.

That is the best kind of instrument: a number derivable in advance, so it can be
checked on a dark screen.

## 18.9 The DMA stall, and three failed fixes

> **Since written — this section's diagnosis is wrong, and usefully so.**
>
> Everything below about the *timeout* is correct: the wall-clock bound really
> was too tight, a preempted display task really did trip it, and the fallback
> really was silent. All of it was **downstream**. The actual defect was one bit
> in a register constant — `DMA_OUTLINK_START` was defined as `(1u << 30)`, which
> is `OUTLINK_RESTART` — so every DMA transfer this driver ever issued told the
> engine to *resume the existing descriptor chain* rather than *begin the
> descriptor just written*. See §18.13 and UM-NATOS-030.
>
> Read what follows as a record of eleven correct eliminations performed
> downstream of an uneliminated cause. The section is left intact because the
> failure mode it demonstrates — a subsystem falling back to a path where the bug
> is genuinely absent, so that investigating the *working* path proves the bug is
> not there — is the most expensive thing in this book.

**Status when written: unresolved and deliberately left alone.** This section is
the evidence, so the next attempt starts from it rather than from a fresh guess.

### What happens

> DMA disables itself within about eight seconds of the raycaster starting. After
> that every transfer takes the 64-byte FIFO path — 2,400 transactions per full
> screen instead of 320 — and the blit costs 55.9 ms instead of a possible
> ~22 ms. **The 3D view has run this way for its entire existence.** Nothing
> reported it; the fallback is silent by design.

### The stall is spurious

```
stall after 63910 good transfers, asking 480 B
cmd        = 0x00000000        <- USR bit already CLEAR
dma_status = 0x00000000
after that : FINISHED late after 0 us
waited     = 30332 us
```

> **The transfer had completed.** `cmd` reads zero, and the follow-up poll found
> it done in 0 µs.

The mechanism is a race between two adjacent lines:

```c
while (GPIO_REG(SPI2_CMD) & SPI_USR_BIT) {      /* A: still running? */
    if ((xt_ccount() - start) > 2000000u) {     /* B: too long?      */
```

> A preemption landing between A and B times out a transfer that has already
> finished. `xt_ccount()` is wall-clock, so descheduled time counts against the
> bound. Rare per transfer — 63,910 succeeded first — and certain over the
> ~2,900 waits a second the raycaster issues.

The shipped source now carries the full analysis, and it is the clearest
statement anywhere of the wall-clock hazard:

```c
    /* Bounded wait, and the bound has to survive PREEMPTION.
     *
     * xt_ccount() is wall clock: it keeps running while this task is not. The
     * old bound was 2,000,000 cycles -- ~25 ms at 80 MHz -- justified as "far
     * beyond the ~100 us a 480-byte transfer needs", which is true of the
     * TRANSFER and false of the WAIT. One scheduling round trip is longer than
     * that, so a display task descheduled mid-transfer times out on a DMA
     * engine that is working perfectly.
     *
     * The consequence is not a dropped frame. A timeout disables DMA
     * PERMANENTLY and falls back to the FIFO path, so a single spurious trip
     * breaks rendering until reboot -- and it presents as the blit getting
     * FASTER (55.8 ms to 31.3 ms), because the fallback does different work.
     * A performance number improving was the symptom.
     *
     * This is the same class of error as the frame timings earlier in this
     * project: wall clock measured where work was meant. Raising the bound
     * rather than switching clocks keeps the guard intact -- a genuinely stuck
     * engine is still caught, it just costs half a second once instead of
     * 25 ms. task_cpu_cycles() would be the principled fix, but it only
     * advances at context switches and so cannot bound a spin inside one. */
```

**"A performance number improving was the symptom"** is worth reading twice.

### The corruption is the recovery, not the timeout

> `spi2_dma_tx()` returns 0 on timeout, and `spi_tx()` then falls through to
> `spi2_tx()` — **re-sending bytes the DMA has already sent.** The panel takes
> the span twice, its window advances one span too far, and every row after it
> shifts.
>
> This is a latent bug in the shipped driver. It fires once per boot and is
> invisible, because the same timeout that causes it also disables DMA so it
> cannot happen again.

### What was tried

| attempt | result |
|---|---|
| Overlap conversion with DMA (two buffers) | no measured gain; broke the view once DMA actually ran |
| Bound by CPU time, reset and retry instead of disabling | blit 21.9 ms — and the view rendered wrong |
| Bound by CPU time, re-read USR, never re-send | view still wrong |

> Three attempts, three breakages. Each had a plausible mechanism and a good
> number **before anyone looked at the screen**, which is the whole lesson: this
> driver's only correctness instrument is a person looking at the panel.

And a separate finding about a change that was kept for the wrong reason:

> The pipelined path deserves its own note. It measured as noise (55.9 → 55.5 ms)
> and was kept anyway as "correct and free". It had also **never actually run** —
> DMA disabled itself before it mattered — so it sat in the tree for a day looking
> like tested code. **An optimisation with no measured gain has no claim on the
> tree, and code that only executes when another bug is absent has been tested by
> nothing.**

### If this is picked up again

- The 55.9 ms blit is correct. It is slow, and it works. That trade has already
  been made three times in the other direction and lost every time.
- Fix the duplicate re-send first. It is a real defect, it is independent of the
  timing question, and it makes any later timeout change safe rather than
  destructive.
- Do not remove the guard to study the failure. That was the first mistake.
- Verify on the glass before reporting a number.

> **How that advice aged.** Four items; three held and one was actively
> misleading.
>
> The duplicate re-send *was* a real defect and fixing it first *was* right —
> `spi_tx()` now abandons the transfer on timeout instead of falling through to
> `spi2_tx()`. "Verify on the glass" held. "Do not remove the guard" held.
>
> The first bullet is the one that misled. "The 55.9 ms blit is correct" was
> true and had the causality backwards: 55.9 ms was the *FIFO fallback*, which
> looked correct precisely because the FIFO path never touches
> `SPI_DMA_OUT_LINK_REG` and therefore never expressed the bug. The advice
> amounted to "keep the configuration in which the defect is invisible", which is
> exactly what kept it invisible for a day.
>
> With DMA actually working the blit is **31.4 ms** and correct — faster *and*
> right, a combination three failed attempts had established was unavailable.

## 18.10 `display_resync()`: a diagnostic for an open question

Chapter 27 §27.10 ends with a hypothesis with a mechanism, and this function is
the instrument built to test it:

```c
/* Re-establish the controller's window and end the transaction, WITHOUT
 * writing a single pixel.
 *
 * The ILI9341 holds a window set by CASET/PASET, and RAMWR starts a stream
 * that continues for as long as CS stays asserted. A stream that ends short,
 * or a CS left low, leaves the controller mid-window -- and the next pixels
 * sent land at whatever offset it had reached rather than at the top left.
 * That is what a garbled image is, and no amount of correct pixel data fixes
 * it, because the data is not what is wrong.
 *
 * This is the test for that: it issues CASET, PASET and RAMWR for the full
 * panel, then raises CS. Nothing is drawn. If a garbled view comes good after
 * calling it, the fault was the controller's idea of where pixels go, not the
 * pixels -- which would also explain why starting an application repairs the
 * view, since every draw it makes issues a fresh window of its own. */
void display_resync(void)
{
    draw_lock();
    set_window(0, 0, DISP_W - 1u, DISP_H - 1u);
    push_end();                 /* CS high: ends the write cleanly */
    draw_unlock();
}
```

A five-line function that is a *falsifiable experiment*: if the view repairs, the
hypothesis is right; if it does not, the remaining suspect is the SPI/DMA stream
itself. That is the correct shape for the ninth theory about a fault, after eight
have been eliminated by measurement.

> **It did not repair, and that was the answer.** The experiment was correctly
> designed and returned a correct negative, which eliminated the controller's
> window state and left "the SPI/DMA stream itself" — where the bug was. The
> instrument worked; it just took two more theories to act on what it said.

## 18.11 The one bit

`SPI_DMA_OUT_LINK_REG` carries the descriptor address in bits 19:0, with
`OUTLINK_STOP` at 28, `OUTLINK_START` at **29**, and `OUTLINK_RESTART` at **30**.
This driver had:

```c
#define DMA_OUTLINK_START  (1u << 30)       /* wrong: that is RESTART */
```

Every DMA transfer nat-os had ever issued asked the engine to resume the existing
descriptor chain from wherever it left off, rather than to begin the descriptor
just written. The transfer completes. `SPI_USR` clears. No timeout fires. The
byte counters advance. The boot self-test reports `dma=320/0`. And the pixels
land progressively displaced.

All three constants are now defined rather than only the one in use, so the next
reader sees the adjacency that caused this:

```c
#define DMA_OUTLINK_STOP    (1u << 28)
#define DMA_OUTLINK_START   (1u << 29)
#define DMA_OUTLINK_RESTART (1u << 30)
```

**This was never only the 3D view.** `spi_tx()` sends anything over 64 bytes by
DMA, so the defect touched every transfer wider than 32 pixels — full-screen
clears, the colour strip, launcher repaints, `display_text()` on longer strings,
the panic screen. Only small fills and command bytes escaped, on the FIFO path
through the W registers. Graphical glitches elsewhere in the UI, previously
attributed to unrelated causes, disappeared at the same time.

The renderer was not the cause. It was the heaviest consumer — 224 DMA transfers
per frame inside a single window, more than everything else in the system
combined — so it expressed the defect most visibly. **It was a display bug the
renderer exercised hardest, not a renderer bug that looked like a display bug.**

### Why six instruments agreed

| instrument | reading | truth |
|---|---|---|
| `SPI_USR` self-clearing | transfer complete | it was |
| `display_dma_transfers()` | 110,507 and climbing | it was |
| `display_dma_timeouts()` | 0 | correct |
| `display_bytes_written()` | matches expectation | it does |
| boot self-test `dma=320/0` | healthy | at boot, with nothing to preempt |
| the panel | **wrong** | — |

`OUTLINK_RESTART` is a legitimate command. The engine performed it and retired
the transfer, so every counter attached to the machine reported the truth. The
only instrument that disagreed was the one not attached to it.

**A counter cannot see a picture.** Where the output is visual, get the output
out of the machine and look at it — which is what `fbdump` finally did (Ch. 28
§28.11).

## 18.12 Metrics

| Quantity | Value |
|---|---|
| Resolution | 240×320, RGB565 |
| Framebuffer | none in the driver; 80,640 B optional for the 3D view |
| Span buffer | 480 B |
| Font | 475 B, in flash |
| Full-screen fill | **43 ms** via SPI2 + DMA (78 ms FIFO, 387 ms bit-banged) |
| SPI clock | 40 MHz (`sysclk/2`); ~3.2 MHz bit-banged |
| Transfers per full screen | 320 with DMA (2,560 on the 64-byte FIFO) |
| Theoretical floor at 40 MHz | ~31 ms |
| Bytes for init + clear | 153,634 |
| Frame cost, march / compose / blit | 2.6 / 13.9 / 41.9 ms |
| Share of frame spent on the bus | 72% |
| SPI clock ceiling on this board | 40 MHz (80 MHz is electrically fine and visibly noisy) |
| Blit improvement from one lock per frame | 194× |
| Image size at this milestone | 18,896 B |
| Full-view blit, FIFO fallback | 55.8 ms |
| **Full-view blit, DMA actually working** | **31.4 ms** |
| Characters changed to fix the "stall" | 1 |

## 18.13 What this does not establish

- **No descriptor chaining.** Each span is a single descriptor started and waited
  on individually. Chaining whole frames would remove most of the gap to the
  31 ms floor.
- **No 80 MHz.** One register bit, electrically fine, visibly noisy.
- **No read phase.** The driver is write-only; MISO is wired but unused.
- **No orientation control.** MADCTL fixed at `0x48`.
- **No clipping of text.** A string running past the right edge stops at the
  panel boundary rather than wrapping.
- **No damage tracking.** Callers decide what to redraw; nothing computes dirty
  regions. This is the interesting gap: the frame cost is dominated by redrawing
  everything every frame, not by the transport.
- **No gamma correction.** Gamma tables left at panel defaults.
- ~~**The DMA stall is open**, §18.9.~~ **Closed** — §18.11. It was never a
  stall.
- **Nothing measures whether the shading or icons are legible.** Judged by one
  person on one panel.
- **MISO reads all zeros.** `panelid` gets `00 00 00 00 00` from both `0xD3` and
  `0x04`. Either the panel's SDO is not populated on this module or the read path
  is misconfigured, and one negative does not separate them. This is why the
  framebuffer had to be dumped over the UART rather than read back off the glass.
- **Descriptor chaining is still not done**, and now matters more: with DMA
  actually working, the blit is 31.4 ms against a ~31 ms theoretical floor at
  40 MHz, so the transport is no longer where the time goes.

---

**Next:** the input side, four attempts, and an axis that was backwards for three
months behind a calibration that could only ever give one answer.
