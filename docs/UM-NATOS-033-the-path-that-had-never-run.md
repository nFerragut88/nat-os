# UM-NATOS-033 — The Path That Had Never Run

**Used Medias LLC — Embedded Systems Division**
Revision 1.0 · 2026-08-18 · Status: **Fixed and confirmed on hardware**

---

## 1. Abstract

UM-NATOS-030 fixed one bit and the 3D view came right. Within hours a new fault
was reported: the view's close button, and a narrow column with it, appeared on
the **left** of the panel when the code puts them on the right.

The observation that reframed it came from the person holding the board:

> this issue with the wrap-around pixels being displayed happened after you did
> the fix for 3D view which I believe was just changing 1 single bit

That is exactly right, and it is the whole report. Before UM-NATOS-030 the DMA
engine spuriously timed out within about twelve seconds of every boot and
disabled itself permanently, so **every transfer this project ever made took the
FIFO path**. Fixing `OUTLINK_START` did not introduce a bug. It started executing
a code path that had contained bugs since the day it was written and had never
run long enough for anyone to see them.

Four defects were found in that path. Three were real and are fixed; one was the
cause. A fourth instrument was caught lying on the way past.

---

## 2. What was wrong

### 2.1 The cause: the shifter started before the DMA had data

`spi2_dma_tx()` started the outbound descriptor link and then immediately started
the SPI transaction:

```c
GPIO_REG(SPI2_DMA_OUT_LINK) = ((uint32_t)&g_desc & 0xFFFFFu) | DMA_OUTLINK_START;
GPIO_REG(SPI2_MOSI_DLEN)    = n * 8u - 1u;
GPIO_REG(SPI2_CMD)          = SPI_USR_BIT;          /* <- races the DMA */
```

The DMA channel needs a few cycles to fetch the descriptor and push the first
words into the peripheral's FIFO. Setting `SPI_USR` before that lands makes the
shifter send **whatever the FIFO already held**, with the real pixels arriving
behind it.

The consequence is a constant few bytes in front of every transfer. Each one
advances the panel's write cursor further than the pixel data accounts for, so
the image walks to the right — by a fixed amount per transfer, accumulating for
as long as the window stays open.

The fix waits for the outbound channel to actually be running:

```c
for (uint32_t spins = 0;
     !(GPIO_REG(SPI2_DMA_STATUS) & DMA_STATUS_TX_EN) && spins < 1000u;
     spins++) {
}
```

A fixed delay of sixteen iterations also worked and was rejected: this project
already has a rule about describing a loop by its bound rather than by the
bound's value on the day, and a magic 16 is that mistake wearing different
clothes. The spin is still bounded, and falling through leaves exactly the
previous behaviour rather than a hang.

### 2.2 `DMA_OUT_RST` was the inbound channel

```c
#define DMA_OUT_RST (1u << 2)       /* wrong: SPI_IN_RST */
```

`SPI_IN_RST` is BIT(2); `SPI_OUT_RST` is BIT(3). The driver carries a long
comment explaining why the outbound channel must be reset before every transfer.
It was resetting the receive channel of a write-only display, and had been since
the file was written.

**And correcting it broke the engine.** With the right bit, the first transfer
completed and the second never signalled EOF: resetting the outbound channel
between back-to-back transfers tears down the state that produces that signal.
The reset belongs at init, where it now happens once. Per transfer, only the
shared AHB master FIFO is cleared — which is what the comment's actual argument
(the driver alternates transports within one window) requires.

Two wrongs had been cancelling out. The bit was wrong, so the reset never
happened, so nobody discovered that doing it there is harmful.

### 2.3 `DMA_OUTDSCR_BURST` was the inbound descriptor burst

`SPI_OUTDSCR_BURST_EN` is BIT(10); the driver had BIT(11), which is
`SPI_INDSCR_BURST_EN`. Outbound descriptor burst had never been enabled.

### 2.4 `dmastat` reported the wrong thing

It inferred the transport from the timeout count, so after `dmaoff` forced the
FIFO path by hand it printed *"DMA still active"*. It nearly invalidated the
transport A/B test that proved the cause. It now reports the flag.

---

## 3. Why none of this was visible before

`spi_tx()` sends anything over 64 bytes by DMA and everything else through the W
registers. That threshold is why the fault hid so well even once DMA was running:

| drawn thing | bytes per transfer | path | fault visible? |
|---|---|---|---|
| text, icons, small fills | < 64 | FIFO | no — never affected |
| solid backgrounds | > 64 | DMA | no — a slant inside one colour is invisible |
| the 3D view | 480 × 224 | DMA | **yes** |

Everything with structure was small enough to stay on the FIFO. Everything on
DMA was a flat colour, where a progressive displacement cannot be seen. The 3D
view is the only thing in the system that is both large and detailed, and even
there the corridor is smooth enough that the shear reads as nothing in
particular — until it reaches the one sharp landmark, the close button, which
ends up on the far side of the panel.

---

## 4. How it was found

Not by reading. The driver was read repeatedly and the race was not visible in
it; three of the four defects above were found by comparing constants against
`soc/spi_reg.h`, and the cause was found by narrowing with tests.

### 4.1 The question had to be moved out of "left" and "right"

The first three exchanges went nowhere because a fault described as "the column
is on the left" was being checked against code that computes it at the right, and
both descriptions were correct about different things. What broke the deadlock
was painting a **map of the panel in the kernel's own coordinates** — full-height
colour columns at each extreme of the x axis — and asking which colour the
misplaced column was sitting in. That answer does not depend on either party
agreeing what "left" means.

### 4.2 Making the drawing paths identify themselves

Each candidate path was temporarily given a loud unique colour: chrome magenta,
close buttons yellow, launcher backdrop blue, the framebuffer overlay cyan. One
question then established that the narrow strip was the launcher's backdrop
showing through, and the X on it was the 3D view's overlay — which the
framebuffer dump had already shown sitting at buffer x=220, the right edge.

That is the moment the fault became specific: **the buffer is correct and the
transport places it wrong.**

### 4.3 Vertical bars, because the failure writes itself across them

A tall band of vertical bars distinguishes a constant offset from an accumulating
one by eye, with no instrument: a constant offset moves the whole band, an
accumulating one makes the bars slant. They slanted.

### 4.4 Markers, because a slant is not a number

Three short markers — `fill_rect`, a single blit row, and the 24th row of a
24-row blit — against a 16-pixel tick ruler. That gave two facts at once: a
single transfer is **already** displaced, and 24 transfers add about 16 px more.
Constant-per-transfer plus accumulation, which is the signature of extra bytes at
the head of each transfer rather than a bad window or a bad descriptor.

### 4.5 The control that proved the transport

`dmaoff` puts every transfer on the FIFO path and changes nothing else. With it
on, all three markers line up. That is the A/B that made "the DMA path is the
cause" a measurement instead of an inference — and it was nearly wrecked by
`dmastat` (§2.4) claiming DMA was still running.

---

## 5. Verification

| Claim | Evidence |
|---|---|
| Buffer content is correct | `fbdump` → PNG, close button at buffer x=220 |
| Transport displaces it | markers offset with DMA, aligned with `dmaoff` |
| Offset is per-transfer | one blit row already displaced |
| Offset accumulates | 24 rows add ~16 px; bars slant |
| Fix works | all three markers align, DMA active |
| Fix works at scale | 3D view: X back at top right, column gone, 224 rows |
| No regression | clean boot, `corrupt=0`, 296,054 transfers, 0 timeouts |
| Speed retained | blit **30.9 ms** (FIFO fallback is 55.9 ms) |

---

## 6. Standing rules earned here

1. **A fix that makes a dormant path live inherits every bug in it.** The one-bit
   fix in UM-NATOS-030 was correct and complete. It also turned on ~30 lines that
   had never executed under load, and those lines had four defects. Expect this
   whenever a fallback stops being taken.
2. **When a question and its answer are both clear and the result is still
   impossible, the two sides are describing different things.** Stop asking and
   go define the terms — in this case by painting the coordinate system.
3. **Make the code identify itself.** Giving each drawing path a unique colour
   answered "who drew this pixel" in one question, after four had failed.
4. **Choose a test object the failure writes itself across.** Vertical bars
   distinguish constant offset from accumulation at a glance; solid rectangles
   hide both.
5. **A guard that fires on your own mistake is working.** Waiting on the wrong
   completion bit disabled DMA on the first transfer instead of producing subtly
   wrong pixels. That is the bounded-wait discipline paying for itself.

---

## 7. Still open

- **The phantom touches did not reproduce.** Two 13-minute runs, 91,000 samples,
  idle and under display load, zero events and zero PENIRQ assertions. Thermal
  drift is eliminated: the baseline is flat (mean peak z 53.6 → 52.4 over 13
  minutes). See next_moves/05 §5.2.
- **MISO still reads all zeros**, which is why the framebuffer had to be dumped
  over the UART rather than read back off the glass. `panelpull` is written and
  untested.
~~**`panic.c` fills 320 pixels wide on a 240-wide panel.**~~ **Fixed** — §8.


---

## 8. A latent one, found by reading and closed by firing it

`panic_screen()` drew its title bar with:

```c
display_fill_rect(0, 0, 320, 20, COLOR_WHITE);   /* 320 is the panel's HEIGHT */
```

The panel is 240 wide and 320 tall, so this asked for a rectangle a third wider
than the screen. It now says `DISP_W`.

**It had never produced a wrong pixel.** `display_fill_rect()` clips `w` against
`DISP_W` before drawing, so the driver silently corrected it every time. That is
precisely why it survived: the only code that draws this screen runs after the
system has already failed, so it is seen rarely, and when it is seen it looks
right.

Named rather than replaced with `240`, because the two dimensions being
confusable *is* the defect. A sweep of the rest of the kernel for the same
mistake found no other instance.

Verified by causing a real panic (`fault`) rather than by inspection — the panel
path is one of Chapter 12's "mechanisms that had never fired", and this is the
second time it has been fired deliberately. The screen renders and every line is
legible. The fix is invisible by construction, which is the correct outcome for
removing a constant the driver was already compensating for.
