# UM-NATOS-067 — Two Buses, One Frame

**Used Medias LLC — Embedded Systems Division**
Revision 1.0 · 2026-10-02 · Status: **15 fps, 773 of 773 frames, nothing dropped. The board stopped paying for the panel.**

---

## 1. Abstract

```
shown 773  dropped 0  = 14.8 fps shown over 51,920 ms   underruns=1
per shown frame: read 40.2 ms  draw 13.9 ms   worst frame 56 ms of 66.7
of the draw: panel wait 4.6 ms   palette 8.1 ms   window 0.8 ms
```

UM-NATOS-066 ended at 12 fps with the remaining arithmetic stated plainly: a
frame cost `read + draw`, 40 ms and 32 ms, and they were being paid one after
the other although they happen on **two different buses** — the card on SPI3,
the panel on SPI2. Making them overlap was named as the next project and
estimated at about 20 fps.

It is 15, the frame costs 54 ms instead of 71, and the work was smaller than
expected for a reason worth stating: **only one side has to be asynchronous.**
The card's read is still a busy-wait, and that wait is exactly what gives the
panel its time.

§3 is the mechanism. §4 is the discovery, which was not in the DMA at all but
in a loop condition. §6 is what two instruments got wrong.

---

## 2. What was built

```
display_blit_be_start()    takes the lock, opens the window, starts the engine
display_blit_be_finish()   waits for it, closes the stream, releases the lock
draw_frame()               reordered around them, two converted buffers
```

The rectangle must fit one DMA descriptor (~4 KB) or it is sent synchronously
and `_finish` has nothing to do, so a caller is correct either way. The display
lock is held between the two calls — a real cost, which is why `_finish`
releases it *before* the caller expands the next batch rather than after.

---

## 3. The order is the mechanism

```
read the bytes this batch needs      <- the PREVIOUS blit is in flight
finish that blit                     <- usually already done
expand the palette into the other buffer
start this batch's blit
```

Two converted buffers, used alternately, because the panel is reading one while
the CPU fills the other. They come from **`g_dec`, minimp3's decoder state.**
SRAM1 has 2.4 KB free of 60, so there was nowhere else to put them; a song and
a video are different jobs of the same task and never both at once, and
`mp3dec_init()` runs at the start of every playback, so whatever a video leaves
in there is overwritten before it could matter.

That borrowing is the kind of thing that is either carefully reasoned or a
latent corruption bug, and the reasoning is in the comment at the call site
rather than in this report, where nobody editing the code would find it.

---

## 4. The discovery was a fill condition

The first version of the loop filled until **one row** of pixels was in hand
and then emitted whatever had accumulated. Batches came out at two rows as
often as nine, with no read in front of them to hide the panel behind, and each
small batch still paid a full window setup:

```
                         panel wait   palette   window   draw
fill to one row            18.6 ms     8.2 ms   1.3 ms   27.9 ms
fill to the whole batch     4.7 ms     8.2 ms   0.8 ms   13.9 ms
```

18 of the panel's 23 ms are hidden by the second version. The first hid 4.

**This was found by an instrument, not by reading the code.** "Draw" is three
different things — waiting for the panel, expanding the palette, opening the
next window — and only the first can be hidden. Guessing which third was which
had already been wrong four times in this project's log (UM-NATOS-066 §6), so
this time the three were counted separately and printed by `mp3`. The answer
was immediate and unambiguous: the panel wait was still 18.6 ms, so the overlap
was not happening, so the thing in front of the blit was missing.

---

## 5. The whole arc, 7 fps to 15

```
                              read    draw   frame   fps   KB/s
morning (UM-NATOS-064)        80.1    46.8    127      7    480
CMD18 + direct blit (065)     50.5    35.4     86     10   1022
all-DMA command layer (066)   47.0    35.3     82     10   1083
pipelined blocks (066)        46.2    35.3     81     10   1083
whole-sector reads (066)      39.8    31.7     71     12   1083
panel not waited for (067)    40.2    13.9     54     15   1083
```

One day, 7 fps to 15, `crc32=0xf584e81c` unchanged at every step, and the
picture confirmed on the glass at each of the three points where only a person
could tell the difference.

---

## 6. Two instruments, both wrong about shape

**6.1 The converter advised against the file it can play.** `vidconv.py`
predicted **70 ms of the 67 available** for the 15 fps cut and printed
"expect dropped frames". The board plays it with none. The model was not
inaccurate, it was the wrong SHAPE: it added the draw to the read, and since
§3 the draw mostly does not add. Corrected to 0.68 µs a byte and 0.24 µs a
pixel it predicts 54 ms against 54.1 measured.

That is the second time this model has had to change shape rather than
magnitude, and both times it warned about a file that plays cleanly. A cost
model is a measurement with a date on it, and the date matters more than the
digits.

**6.2 A counter that outlived its denominator.** The three-way split of §4
accumulated across playbacks while the frame count it was divided by reset with
each one, so the first report after a second playback claimed **34 ms of
palette expansion for work that takes 8.** It is reset per playback now, with
the rest of the status.

This is the same fault as the SD phase counters earlier in the same day
(UM-NATOS-066 §6.2): a counter answering a wider question than the one printed
beside it. Twice in one day, in two different files, by the same hand.

---

## 7. Metrics

| Quantity | UM-NATOS-066 | Now |
|---|---|---|
| Picture | 180x320, 12 fps | **180x320, 15 fps** |
| Frames | 619 of 619 | **773 of 773**, 0 dropped |
| Frame cost / budget | 71.5 of 83.3 ms | **54.1 of 66.7 ms** |
| Worst frame | 80 ms | **56 ms** |
| Read per frame | 39.8 ms | 40.2 ms |
| Draw per frame | 31.7 ms | **13.9 ms** |
| — panel wait | ~23 ms | **4.6 ms** |
| — palette expansion | ~8 ms | 8.1 ms |
| Panel time hidden | 0 | **18 of 23 ms** |
| Data rate demanded | 702 KB/s | **870 KB/s** |
| File | 35.4 MB for 51.6 s | 43.8 MB for 51.5 s |
| Converter prediction | 71 ms (measured 71.5) | 54 ms (measured 54.1) |
| Bytes verified identical | crc32=0xf584e81c | crc32=0xf584e81c |

---

## 8. What this does not establish

1. **16 fps does not fit.** 62.5 ms against a 54.1 ms average leaves nothing
   for a long frame, and the worst measured is 56. 15 is the honest number.
2. **The palette expansion is still unhidden** — 8.1 ms a frame of CPU that
   nothing overlaps. Hiding it needs the CARD's read to be asynchronous too:
   `fat_read_start()` / `_collect()` through cluster walking. The SD driver has
   those halves; `fat.c` does not.
3. **After that the card is the floor.** ~33 ms a frame of wire time and token
   latency, which no host-side change reaches. About 22 fps, and then the only
   routes left are a faster bus (div 2 remains refused as unsafe) or a smaller
   picture.
4. **The display lock is held across each batch's read.** During full-screen
   video nothing else draws, so it has cost nothing measurable — but a future
   caller that draws while a video plays would block for ~1 ms a batch.
5. **Two converted buffers live in the MP3 decoder's state.** Correct for the
   reasons in §3, and wrong the moment anything makes a song and a video
   concurrent.
6. **One card, one file, one board**, and this card needed reseating five times
   today (UM-NATOS-066 §6).
7. **1 underrun per playback**, at the end of the file, unchanged since
   UM-NATOS-064 and still not chased.

---

## 9. References

- `docs/next_moves/12-video.md` — steps 16–17
- UM-NATOS-066 — the all-DMA read path this overlaps with the panel
- `kernel/display.c` — `display_blit_be_start` / `_finish`
- `kernel/vplay.c` — the frame loop, and the fill condition of §4
- `tools/vidconv.py` — a cost model whose shape changed with the kernel

**Two DMA engines, one 80 MHz core, no operating system underneath it but the
one in this repository: the card fills a buffer while the panel empties
another, and a from-scratch kernel plays 15 frames a second of video with its
sound in step — because the frame stopped being read plus draw and became read,
with the draw hidden inside it.**
