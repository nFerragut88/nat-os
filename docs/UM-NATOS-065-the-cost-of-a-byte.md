# UM-NATOS-065 — The Cost of a Byte

**Used Medias LLC — Embedded Systems Division**
Revision 1.0 · 2026-09-27 · Status: **10 fps, 516 of 516 frames, nothing dropped. The picture did not get cheaper; the bytes did.**

---

## 1. Abstract

```
shown 516  dropped 0  = 9.9 fps shown over 51,930 ms   underruns=0
per shown frame: read 50.2 ms  draw 35.4 ms   worst frame 88 ms
```

UM-NATOS-064 left the board playing video full screen at **7 fps**. That rate
was never chosen. It was what one frame cost: 80.1 ms to read and 46.8 ms to
draw, 127 ms of the 143 ms a 7 fps frame allows. The user asked whether it
could be improved.

It could, by 33%, and **not one line of the improvement is about video.** Both
halves of a frame were paying for the same thing: bytes carried between memory
and a peripheral by the CPU, one small transaction at a time. The card can
stream a run of sectors for a single command; the panel can take pixels in the
order they already sit in memory. Neither was being asked to.

This report covers `next_moves/12` steps 8–9. §2 is the instrument that found
it, because the first version of that instrument was wrong and said so loudly.
§3 is the read half, §4 the draw half, §5 the re-cut file that finally makes
the gain visible, and §6 the one measurement that mattered more than any
speed: that the bytes did not change.

---

## 2. The instrument, and what it accused

`spitest` (`spi3_probe_speed()`) times a 1, 16 and 64-byte transfer at each
clock divider. Its first version used `xt_ccount()` — wall clock — and measured
the same transfer at 80 µs and then 59 µs, which is not a bus changing speed
but a task being preempted mid-measurement. Re-armed with
`task_cpu_cycles()`, which counts only this task's cycles, it accused the
driver rather than the bus:

```
div 4 (20 MHz)   1 B: 2.9 us    16 B: 14.5 us    64 B: 53.8 us
div 8 (10 MHz)   1 B: 2.9 us    16 B: 14.6 us    64 B: 54.0 us
```

**The clock barely appears.** Halving it changed a 64-byte transfer by 0.2 µs.
What the numbers describe is ~0.4 µs per byte of software plus ~2.5 µs per
transaction — and at 20 MHz the *wire* costs 0.4 µs a byte. The driver was
charging as much to prepare a byte as the bus charged to send it.

> A measurement that is the same at both clocks is not measuring the clock.
> This is the project's standing rule (UM-NATOS-060) arriving from the other
> direction: an instrument can be right about the wrong quantity.

---

## 3. The read half: three changes, 480 → 1022 KB/s

**3.1 Word-wise registers.** SPI3's data registers are 32 bits wide and the
driver packed and unpacked them a byte at a time. `spi3_read()` fills them with
`0xFFFFFFFF` — a read transaction sends nothing that matters — and reads the
answers back as words.

**3.2 Batched token polling.** After a read command the card sends `0xFF` until
its data token. One byte per transaction cost **343 µs a block** for bytes that
are all padding. The poll now takes 16 at a time, and the batch that contains
the token also contains the first data bytes behind it, which are kept rather
than re-read.

**3.3 Multi-block reads.** This is the one that mattered, and it came straight
out of the phase timings:

```
per block: cmd 39 us   token wait 347 us   data 405 us
```

That 347 µs is the card's own access latency, and it is charged **per command,
not per sector.** `sd_read_blocks()` reads a run of consecutive sectors with a
single CMD18 and stops the stream with CMD12; `fat.c` hands it every
whole-sector run inside a cluster. The latency is paid once for the run.

```
                        KB/s     cmd   token   data   per block (CPU)
before                   480      46    338*    664       694
word-wise + batched      637      46    347     405       791
+ multi-block (CMD18)   1022       7      77    390       474
```

**3.4 The asterisk: an instrument answering a narrower question.** After 3.1
and 3.2 the token wait read 338 µs — unchanged — which said the batching had
achieved nothing. It had not: those phase counters were **cumulative since
boot**, and this board had by then read blocks at three different clock
dividers, so the figure was an average over a population nobody had asked
about. `fat cat` now reports the delta across the one read it performs. The
"no change" was the instrument.

**3.5 An instrument that can fail.** A future change to `fat.c` could stop
offering runs of sectors, and the only symptom would be throughput quietly
returning to where it started. So the report names the path it took:

```
of those, 12,496 came in 3,124 multi-block commands
```

Four blocks a burst — exactly the read buffer's size, which is the arithmetic
agreeing rather than a number to trust on its own.

---

## 4. The draw half: bytes the panel can already use

`display_blit()` byte-swaps every pixel into a 480-byte staging buffer and
sends that buffer. RGB565 goes out high byte first and sits in memory low byte
first, so the swap is real work — but for a full-screen frame it is **57,600
swaps and 240 DMA transactions** over a ~23 ms wire floor at this clock.

`display_blit_be()` takes bytes already in the panel's order and hands them to
the DMA engine in the largest pieces one descriptor holds, with no staging copy
and no per-row transaction. The video player gets that order for nothing by
keeping its palette byte-swapped: the lookup that expands a frame has to write
each pixel somewhere regardless, so the cost becomes **256 swaps per file
instead of 57,600 per frame**.

It refuses a rectangle that does not fit the panel rather than clipping one.
`display_blit()` clips, which is right when it walks rows; narrowing a
*contiguous stream* would put every row after the first at the wrong offset,
and a subtly wrong picture is worse than no picture.

```
draw per frame   46.8 ms -> 35.4 ms      dmastat: 14,494 transfers, 0 timeouts
```

The byte order is the one change in this report a measurement cannot settle —
wrong, it would show as wrong colours. Confirmed by the user on the glass, with
the browser's thumbnails as the control: they still use the old path.

---

## 5. Spending it: the file re-cut at 10 fps

A cheaper frame changes nothing a person can see while the file is still 7 fps.
The source MKV was still on the card, so the card went to the PC and
`vidconv.py` re-cut it: **516 frames, 59,904-byte chunks, 29.5 MB, 585 KB/s**
the board must now sustain, against 416 before.

The converter's own advice was corrected first. It predicted per-frame cost
from the step 4–5 constants — 1.31 µs/byte read, 0.77 µs/pixel draw — which
describe a machine that no longer exists, and would have advised **against** a
frame rate this board holds comfortably:

```
read 1.31 -> 0.82 us/byte     draw 0.77 -> 0.61 us/pixel
predicted: ~84 ms of 100 ms available (84%)      measured: 85 ms
```

Two things about the card, recorded because both were nearly mistaken for
faults:

- The card is **250 MB with 30.3 MB free**, and the new file is 29.5 MB. The
  7 fps file had to be deleted before the new one would fit; it is regenerable
  from the MKV, which stays on the card. The copy was verified on the card by
  MD5, not by the copy command's silence.
- A hot-swapped card needs the SD stack re-initialised, and the browser said
  so: `listed=0 (SD read failed)`, not an empty list. An instrument being
  specific about its own failure, for once.

---

## 6. What did not change

Everything in §3 rewrites the path that carries a file's bytes. The only
result that makes any of it admissible:

```
before:  6,398,464 bytes  crc32=0xf584e81c  clusters 1563 of 1563
after:   6,398,464 bytes  crc32=0xf584e81c  clusters 1563 of 1563
```

Checked after each of the three read changes, not once at the end. A faster
reader that returns different bytes is not a faster reader.

---

## 7. Metrics

| Quantity | Before (UM-NATOS-064) | Now |
|---|---|---|
| Picture | 180x320, 7 fps | **180x320, 10 fps** |
| Read per frame | 80.1 ms | **50.2 ms** |
| Draw per frame | 46.8 ms | **35.4 ms** |
| Frame cost / budget | 127 of 143 ms (89%) | 85 of 100 ms (85%) |
| Worst frame | — | 88 ms |
| Frames dropped | 0 of 361 | **0 of 516** |
| Underruns | 1 per playback | 0 |
| SD throughput | 480 KB/s | **1022 KB/s** |
| Per block | cmd 46 / token 338 / data 664 µs | cmd 7 / token 77 / data 390 µs |
| SD bus | SPI3 at 20 MHz | unchanged — no clock was raised |
| Data rate demanded | 416 KB/s | 585 KB/s |
| File | 21.0 MB for 51.6 s | 29.5 MB for 51.6 s |
| Panel transactions per frame | 240 | 29 |
| Pixel swaps per frame | 57,600 | 0 (256 per file) |
| Bytes verified identical | — | crc32=0xf584e81c, every step |
| USB link drops | 0 with the 12/16 cap | 0, at 40% more data |

---

## 8. What this does not establish

1. **The read half is still ~0.36 µs/byte of CPU above the wire time**, because
   `sd.c` moves block data through the peripheral's registers. SPI3 DMA is the
   next ~2x on that half and would put 13–15 fps in reach; `display.c` already
   has the pattern. It has not been attempted.
2. **10 fps is 85% of the frame budget**, measured worst frame 88 ms. It is
   headroom, not comfort; a busier system drops frames — and says so.
3. **No clock was raised.** Divider 2 (40 MHz) remains refused: it failed
   identification once and left the card unresponsive across two re-inits, and
   garbled bits on MOSI can decode as any command, a write included.
4. **One card, one file, one board.** A 250 MB FAT16 card whose cluster is
   4096 bytes, so a multi-block run is at most 8 sectors. FAT32 on real media
   is still unseen, and a larger cluster would make runs longer — untested.
5. **The CMD18 path shares one bus with nothing.** It holds CS for a run of
   sectors; no other task touches SPI3, which is true today and is not
   enforced by anything but the FAT mutex above it.
6. **The volume cap is still empirical** (UM-NATOS-064 §10.3). It survived 40%
   more card traffic, which is evidence, not a margin.
7. **The byte order of the fast blit is confirmed by eye**, not by a test. A
   panel cannot be read back on this board.
8. **1 underrun per full MP3 playback** is still unexplained, and the frame-count
   persistence oddity from UM-NATOS-063 §10 still is too.

---

## 9. References

- `docs/next_moves/12-video.md` — steps 8–9, with the numbers as they arrived
- UM-NATOS-064 — the video player this speeds up
- `kernel/sd.c` — `sd_read_blocks()`, CMD18 and CMD12
- `kernel/spi3.c` — `spi3_read()`, `spi3_probe_speed()` (`spitest`)
- `kernel/display.c` — `display_blit_be()`
- `tools/vidconv.py` — the cost model that now describes this board

**Nothing in this report made the picture smaller, the clock faster or the
work simpler. It asked the card for a run of sectors instead of one at a time,
and handed the panel bytes it could already use — and a from-scratch operating
system on an 80 MHz microcontroller went from 7 frames a second to 10, with
every byte on the card proven unchanged.**
