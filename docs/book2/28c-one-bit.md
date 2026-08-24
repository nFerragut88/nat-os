# Chapter 28c — One Bit

> Sources: `docs/UM-NATOS-030-one-bit.md`
> Code: `kernel/display.c`, `kernel/console.c`, `tools/serial/grab_fb.py`

---

## 28c.1 The change

The 3D view fault is closed. It was one bit.

`SPI_DMA_OUT_LINK_REG` on the ESP32 carries the descriptor address in bits 19:0,
`OUTLINK_STOP` at 28, `OUTLINK_START` at **29**, and `OUTLINK_RESTART` at **30**.
This driver defined `DMA_OUTLINK_START` as `(1u << 30)`.

Every DMA transfer nat-os had ever issued to the panel told the engine to
*resume the existing descriptor chain from wherever it left off*, rather than to
*begin the descriptor just written*. The transfer completes. `SPI_USR` clears.
No timeout fires. The byte counters advance. The boot self-test reports
`dma=320/0`. And the pixels land progressively displaced.

```c
/* before */
#define DMA_OUTLINK_START  (1u << 30)

/* after */
#define DMA_OUTLINK_STOP    (1u << 28)
#define DMA_OUTLINK_START   (1u << 29)
#define DMA_OUTLINK_RESTART (1u << 30)
```

Nothing else about the transfer changed. `spi2_dma_tx()` still writes the same
descriptor and still starts the link with the same line:

```c
GPIO_REG(SPI2_DMA_OUT_LINK) = ((uint32_t)&g_desc & 0xFFFFFu) | DMA_OUTLINK_START;
```

All three constants are defined rather than only the one in use, so the next
reader sees the adjacency that caused this and does not have to go back to the
technical reference manual to work out which neighbour they are looking at. The
shipped file now also carries the whole account in a comment above them, which is
the form this project has settled on for a defect worth not repeating.

**The fix is one character. This chapter is about why it took a day**, and what
the two instruments that found it had to do differently from the eleven
experiments that did not.

## 28c.2 Why every instrument agreed

The peripheral reports success because, from its point of view, it succeeded.
`OUTLINK_RESTART` is a legitimate command: the engine performs it and retires the
transfer. Chapter 18 §18.11 has the six-instrument table. The shape of it is what
matters here — six readings attached to the machine, all correct, and one reading
not attached to the machine, disagreeing:

> **The kernel could not see the only thing that was wrong.**

That is Chapter 28's thesis carried to its conclusion. Every counter in the
display driver measures the *transport*, and the transport was doing exactly what
it was told. Nothing in the system measured the *picture*, because the picture is
not a number and the panel cannot be read back — `MISO` is wired on GPIO12 and
reads all zeros (§28c.7).

There was, therefore, no instrument in the kernel that *could* have caught this.
Recognising that a fault of a given kind is outside the reach of every existing
instrument is the step that ends this class of investigation, and it is the step
that took a day.

## 28c.3 The causal chain, in full

The FIFO path never touches `SPI_DMA_OUT_LINK_REG`. That single fact explains
every observation recorded across two sessions, including the ones that had been
filed as separate mysteries.

**The view garbles from boot.** DMA is alive; the blit is displaced.

**It heals ten to sixty seconds later.** `spi2_dma_tx()` bounded its wait on
wall clock at 2,000,000 cycles (~25 ms), which a preempted display task trips on
a perfectly healthy engine. The trip sets `g_dma_ok = 0` permanently, everything
falls back to the FIFO, and the picture becomes correct. **The heal and the
spurious timeout are the same event**, at about twelve seconds.

**`gfxrogue` "repairs" it.** Its clipped fill is 180 px — 360 bytes a row, over
`spi_tx()`'s 64-byte threshold — so it issues DMA transfers, adds opportunities
for the display task to be descheduled mid-wait, and brings the timeout forward.
It was never repairing anything. It was killing the DMA engine faster.

**`hog draw` repairs it.** Same mechanism, same width — and it stopped working
the moment the timeout bound was raised, because the thing it was provoking could
no longer happen. That is the fact which, read in isolation, said the raise had
made things worse; read correctly, it identifies the mechanism outright.

**The `draw` application never repairs it.** Its block is 20 px: 40 bytes a row,
below the threshold, entirely on the FIFO, incapable of provoking a DMA timeout.
This was the observation that fitted no theory for hours. It is the one that
confirms this one.

**"Leave it five minutes and it opens fine."** The timeout arrives on its own.

**Raising the timeout bound made the device worse.** The bound was a genuine
defect and the reasoning for fixing it was sound in isolation. It was also the
only thing keeping the display usable, and removing it left the view broken
indefinitely — including for a single static test pattern, which is what finally
made the fault reproducible without the renderer.

Every one of those was reported as a separate mystery. There was one bug.

The table below is the same chain in the form it is worth remembering, because
each row is an observation that was *used as evidence about the renderer* and is
in fact evidence about a threshold in `spi_tx()`:

| Observation | What it was read as | What it meant |
|---|---|---|
| garbled at open, good later | the renderer needs warming up | DMA alive, then dead |
| `gfxrogue` repairs it | interleaved drawing helps | 180 px > 64 B: it forces a timeout |
| `draw` never repairs it | the program is too small to matter | 20 px < 64 B: it never uses DMA at all |
| five minutes of uptime repairs it | some state settles | the timeout arrives unprovoked |
| raising the bound made it worse | the raise was wrong | the raise removed the accidental workaround |

## 28c.4 What actually found it

Not reasoning. Reasoning produced eleven eliminations, all correct, all
downstream (Ch. 28b §28b.3).

### 28c.4.1 `fbdump` — make the buffer visible

`fbsum` reported a summary; `fbhash` reported a checksum. Neither can say whether
a frame is a corridor or noise, and a frozen camera stably rendering a *wrong*
scene gives a perfectly constant hash. The panel cannot be read back, so there
was no way to compare what was in DRAM against what was on the glass.

`fbdump` sends the framebuffer out over the UART as hex; `tools/serial/grab_fb.py`
reconstructs it as a PNG on the host. The first dump taken *while the panel was
garbled* showed **a pristine corridor** — correct perspective, correct shading,
the close button correctly stamped.

That single image cut the search from "the system" to "between the buffer and the
glass". Everything upstream of `display_blit()` — the raycaster, fixed-point
maths, the map, the camera, the scheduler, the applications, every theory in
§28b.3 that concerned drawing — was eliminated by one picture.

It is worth being precise about why this instrument is different in kind from the
eleven experiments before it. Those asked *does changing X change the symptom?*,
which requires a correct guess about X. This one asked *what is actually in the
buffer?*, which requires no hypothesis at all. **When a search is not converging,
the useful move is usually to stop testing hypotheses and start extracting
state.**

### 28c.4.2 `fbpattern` — remove everything else

`fbpattern` fills the view region with eight flat colour bands and blits it
through the identical `display_blit()`, with the renderer frozen.

**A single static image came out corrupt.**

That removed the raycaster, the scheduler, the applications, the touch task and
all concurrency from suspicion at once, and turned a fault that needed a running
3D view and a window of uptime into one reproducible from a shell command. After
that, `spi2_dma_tx()` is thirty lines and the register map is a short read.

Two instruments, half an hour, after a day of correct experiments. The difference
is not cleverness. It is that both of these were built to *show state* rather than
to *test a theory*, and a defect nobody has correctly guessed is immune to the
second kind and defenceless against the first.

## 28c.5 Instruments that lied — including the new ones

UM-NATOS-028 §8 counted seven. UM-NATOS-029 §7 added five (Ch. 28b §28b.7). This
session added three more, and **two of them were built specifically to end this
class of problem.** Chapter 28 §28.12 carries them as Shape 10; what follows is
the sequence in which they failed, because the order is instructive.

**`fbdump` manufactured the fault it was built to photograph.** The first dump
ran without holding the console. The reporter's periodic status line landed in
the middle of the hex, shifting rows; the host parser silently discarded 61
malformed rows and reassembled the survivors as if they were contiguous. The
resulting image was a convincing rainbow-streaked mess, and it was reported as
proof of a render bug.

It was an artefact. With `console_lock()` held across the dump and each row
carrying its own index, the same buffer is pristine. Three fixes, each closing a
separate way to be wrong:

1. **Hold the lock.** When a diagnostic and a subsystem share a resource, the
   diagnostic will corrupt the evidence.
2. **Index every row**, so position is *read* rather than inferred from arrival
   order.
3. **Refuse incomplete input.** The host reads to an explicit `FBEND` terminator
   and prints `rows received N/112, malformed M, missing [...]`.

**A fixed capture window truncated the dump at row 49, twice.** Silently, and
that is what item 3 above is for. `!! DUMP INCOMPLETE` is worth more than a
plausible image.

**A constant hash was called decisive and was not.** `camfreeze` + `fbhash` was
presented as settling render-versus-transport. It does not: a frozen camera
stably rendering a wrong scene produces a constant hash too. The test was sound;
the claim made for it was not.

**And, carried forward from §28b.7.2:** the timeout bound was raised, `dmastat`
was read as `timeouts=0`, and that was taken as evidence the raise had been
unnecessary. Circular — the raise is why it read zero.

> **A guard can only be shown unnecessary by measuring it in its absence.**

Fifteen instruments caught lying, cumulative, across the project at the close of
this session. Chapter 28 §28.13 is the tally by shape.

## 28c.6 This was never only the 3D view

Confirmed on hardware after the fix: **other graphical glitches across the UI
disappeared at the same time**, and had been attributed to unrelated causes.

That follows directly from the threshold. `spi_tx()` sends anything over 64 bytes
by DMA, so the defect touched *every* transfer wider than 32 pixels —
full-screen clears, the colour strip, launcher repaints, `display_text()` on
longer strings, the panic screen. Only small fills and command bytes escaped,
because they go out through the W registers on the FIFO path.

The 3D view was simply the worst-affected consumer: 224 DMA transfers per frame
inside a single window, more than everything else in the system combined. It was
not a renderer bug that happened to look like a display bug. **It was a display
bug that the renderer exercised hardest** (Ch. 25 §25.14).

There is a general lesson in that, and it is not the obvious one. The renderer
was the best instrument in the system for this defect — it ran the affected path
more often than everything else combined, so it showed the fault first, most
visibly, and most reproducibly. It was treated as the suspect for
exactly the same reason it was the best detector. **Where a fault appears is
evidence about exercise rate, not about location.**

## 28c.7 Also fixed, and still open

**Fixed alongside:**

- **All three DMA resets per transfer.** `OUT_RST`, `AHBM_FIFO_RST` and
  `AHBM_RST`, not just the outbound channel. The driver alternates transports —
  `set_window()` sends command bytes through the W registers, pixels go by DMA —
  and the two share the peripheral's AHB master FIFO. Switching between them
  without clearing it leaves the engine reading from a buffer the CPU path was
  using. (Chapter 18 §18.6 carries where each of those resets belongs, which took
  one further correction to get right.)
- **The timeout bound stays at 40,000,000 cycles (~500 ms).** It was always the
  right change; it is now safe to keep, because the thing it was masking is gone.

**Still open at the close of this report:**

- **MISO reads all zeros.** UM-NATOS-015 records GPIO12 as "wired but unused";
  `panelid` gets `00 00 00 00 00` from both `0xD3` and `0x04`. Either the panel's
  SDO is not populated on this module or the read path is misconfigured, and one
  negative does not separate them. This is why the framebuffer had to be dumped
  over the UART rather than read back off the glass.
- **Phantom touches.** Two independent unattended runs logged spurious presses at
  ~374 s and ~390 s; in the first, they launched a program into slot 2. Real,
  reproducible, and unexplained (Ch. 30 §30.3).
- **Transmit still does not reach the air** (Ch. 27 §27.8).

## 28c.8 Standing rules earned here

All six are stated in full, with their evidence, in Chapter 29 §29.6. In short
form, because this is where they were paid for:

1. **A counter cannot see a picture.** Where the output is visual, get the output
   out of the machine and look at it; do not accept a proxy.
2. **When a diagnostic and a subsystem share a resource, the diagnostic will
   corrupt the evidence.** Hold the lock, or measure something else.
3. **Read positions; never infer them from arrival order.**
4. **A tool must refuse to interpret incomplete input.**
5. **Before fixing a defect, find out what depends on it.** The timeout bound was
   genuinely wrong and had become load-bearing.
6. **One-bit register constants deserve the datasheet, not recall.** `START` and
   `RESTART` are adjacent, and both produce a transfer that completes.

Rule 6 is the one that generalises past this project, and the reason is in its
second clause. A wrong bit that produces *no* transfer is found in minutes. A
wrong bit that produces a *successful-looking* transfer is found in a day, and
only if somebody thinks to look at the output with their eyes.

## 28c.9 Numbers

| | |
|---|---|
| Theories eliminated before the cause was found | 11 |
| Instruments caught lying, cumulative | 15 |
| Characters changed to fix it | 1 |
| Sessions between first symptom and cause | 2 |
| Full-view blit, FIFO fallback | 55.8 ms |
| Full-view blit, DMA working | 31.4 ms |
| Theoretical floor at 40 MHz | ~31 ms |
| DMA transfers per 3D frame | 224 |
| DMA threshold in `spi_tx()` | 64 bytes |

The last three rows are the reason this defect was worth two sessions rather than
one paragraph: with DMA actually working, the blit lands within a millisecond of
the transport's theoretical floor. The performance the driver was designed for
had been in the tree the whole time, unreachable behind a bit.

---

**Next:** the rules those two sessions produced, in the chapter that collects
every rule this project has paid for.
