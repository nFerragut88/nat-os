# UM-NATOS-064 — Moving Pictures

**Used Medias LLC — Embedded Systems Division**
Revision 1.0 · 2026-09-19 · Status: **The board plays video with sound, full screen, from its own card. Three of the day's faults were in the tools, not the system.**

---

## 1. Abstract

```
video finished  Learn Numbers in Mandarin Chinese.nvd
frame 361 of 361  180x320 @ 7 fps   audio 2 chunks ahead
shown 361  dropped 0   read 80.1 ms + draw 46.8 ms of a 143 ms frame
```

This report covers `next_moves/12`, steps 1–6, written the day after
UM-NATOS-063. The MP3 player ended with a machine that could decode audio in
real time with half its CPU spare; this is what happened when the remaining
half was pointed at pictures.

The engineering question was decided before a line was written, and by
arithmetic rather than by preference: **the board cannot decode MP4.** What it
can do is copy bytes, and a PC can prepare bytes shaped exactly for it. That
choice is §2. Everything after it is the consequence, including the parts that
went wrong.

The USB link had been dropping whenever a video started, and one run stopped
itself mid-playback. Both are one fault, and it is electrical: §7. Three OTHER
faults that day were in the measuring tools rather than in the board (§8) --
one of them introduced while fixing another, and it cost a trip into the UART
registers.

---

## 2. Why the conversion runs on the PC

The user asked the question directly: should the board convert MP4, or should
a PC app prepare files for it? Three numbers answer it, all from UM-NATOS-063:

| | |
|---|---|
| a single 320x240 H.264 reference frame | ~115 KB |
| SRAM1 free after the MP3 player | 2.5 KB of 60 |
| MP3 alone, decoded in real time | 46% of the CPU at 80 MHz |

H.264 needs whole reference frames in RAM and far more arithmetic per second
of media than MP3. And the board **cannot write files at all** — `fat.c` is
read-only — so converting on the board would mean building a FAT writer first
and then waiting hours per minute of video.

So `tools/vidconv.py` does everything expensive once, on a machine with
ffmpeg, and the board is left with a copy:

| Done on the PC | What reaches the board |
|---|---|
| decode, scale, rotate, choose the frame rate | pixels, already the right size |
| colours → one 256-entry RGB565 palette | one byte per pixel, one palette |
| audio → 8-bit mono PCM | the DAC's own format: ~0% CPU, against MP3's 53% |
| cover art → RGB565, plus a 64x36 list icon | a blit, with no JPEG decoder |

### 2.1 The format, and the one idea in it

`.nvd` is invented here and understood by nothing else (§9). Its parts are
ordinary — RGB565, a palette, unsigned 8-bit PCM — and the only design in it
is the layout:

- a 512-byte header, a 512-byte palette, then the cover and the icon
- **one chunk per frame, every chunk the same size, a multiple of 512**: a
  16-byte header, the frame, then exactly that frame's slice of audio

A fixed stride makes seeking arithmetic rather than a search, keeps every read
on a sector boundary — the case the SD driver does fastest — and lets two
cursors move through one file independently. §4 is what that last property
bought.

Chunk *i* carries samples `[round(i·rate/fps), round((i+1)·rate/fps))`, so
audio cannot drift from the picture however long the file runs.

---

## 3. The browser, and the icon inside every file

The launcher's nine cells were full, so the user chose: **meter's cell became
"video"**, as pong's became meter's and ping's became music. meter remains a
registered program; it lost a cell, not its existence.

`vidlist.c` lists the card's `.nvd` files with the 64x36 icon each one
carries, its title and its length, six rows to a screen. Tap to select, tap
again for a detail screen with the 120x68 cover and a play button.

The icon exists because a list of covers would cost 32 sectors a row; 9 is
cheaper. Both pictures stream from the file 512 bytes at a time and are
blitted as they arrive — **no icon, cover or frame is ever held whole**, which
is the only way a 2.5 KB budget accommodates a 240x320 panel.

`--set-cover PICTURE FILE.nvd` rebuilds the cover and icon of an existing file
and copies its frames across byte for byte. It exists because the user's
source video had already been deleted from the card when they asked for
thumbnails; a picture they pasted back was enough.

---

## 4. Playback: the DAC is the clock

`vplay.c` runs on the MP3 player's task at AUDIO priority and **borrows that
player's buffers** — 4 KB of input, 4.6 KB of PCM — because only one of them
can play at a time and SRAM1 has nothing to spare.

Sync is not maintained, it is structural:

- **Frame *i* is drawn when the DAC has played up to frame *i*'s first
  sample.** `pcm_samples_played()` is the clock, and it cannot drift because
  it *is* the hardware's progress.
- If frame *i+1*'s moment has already passed, frame *i* is **dropped and
  counted**. Falling behind costs frames, never sync.
- Audio must run ahead of the picture, but a frame (32–58 KB) cannot be held
  in RAM waiting for its sound. So **one cursor reads audio K chunks ahead
  while another reads the frame being shown**, both by arithmetic on the
  stride.

K is not a constant. When chunk *i+K*'s audio is queued, the ring must still
hold everything from frame *i* on, so (K+1) frames of audio must fit its 8,192
samples: K = 2 at 22,050 Hz and 10 fps. With K = 3 the play position would
already be past frame *i* when its turn came, and **every frame would be
dropped** — the arithmetic decides it, not a tuning knob.

For two cursors to be affordable, `fat_seek()` had to learn to walk forward
from where the file already is. Restarting from the first cluster, as it did,
meant 4,000+ FAT lookups per cursor per frame deep into a 17 MB file.

---

## 5. Full screen, and what a shape costs

The first version letterboxed 240x134 into the panel's 240x320. Asked to fill
the screen, the options were put to the user with the frame rate each costs,
from the measured per-pixel cost of reading and drawing:

| | pixels | per frame | frame rate |
|---|---|---|---|
| 240x134, letterboxed | 32,160 | 84 ms | 10 fps |
| **180x320, rotated (chosen)** | 57,600 | 127 ms | 7 fps |
| 240x320, cropped upright | 76,800 | 161 ms | ~5 fps |

Rotated fills the panel exactly when the board is held sideways, and keeps the
whole picture. Two 30-pixel strips remain along the long edges — the shape of
16:9 on a 3:4 panel — and the user chose to keep them rather than crop. Those
strips are painted black once as playback starts; the view otherwise draws
nothing, and **kmain stops animating the spectrum strip** so it cannot repaint
over the bottom of every frame.

The SD bus moved to **20 MHz** (div 4) to afford it: measured during playback,
a frame's read cost fell from 58.4 ms to 42.2. div 2 stays refused — that is
the speed that corrupted transfers and left a card unresponsive (UM-NATOS-063
§4.2).

`vidconv.py` now prints the same arithmetic for every file it writes. It
predicted 79.8 ms of reading per frame where the board measured 80.1.

---

## 6. What it does

```
Learn Numbers in Mandarin Chinese, 1920x1080 H.264 + AAC, 52.8 s
  -> 180x320 pal8 at 7 fps, 8-bit mono 22,050 Hz, 21.0 MB, converted in 19 s
  361 of 361 frames shown, 0 dropped, full screen, in sync, 51.94 s wall
```

The user, watching: *"video plays full screen now"*, and earlier, of the
picture and sound together, *"looks and sounds right"*.

---

## 7. The drop, and the video that stopped itself

Since UM-NATOS-063 §9, starting a song no longer dropped the USB link: the
DAC's 0 V -> 1.65 V step was ramped, and the pop with it. Starting a VIDEO
still did, and one run stopped itself at frame 73 with nobody touching the
panel.

For a while it would not reproduce -- 0 in 29 starts -- and this report nearly
shipped saying so. It reproduced on the first play of the user's own file,
where every earlier trial had used a generated test pattern. The difference is
what the speaker is asked to do: a steady tone against real, loud audio.

The same file, three ways, everything else identical:

| | link drops |
|---|---|
| full volume | **2 of 3** |
| volume 0 -- the DAC on, playing silence | 0 of 3 |
| `vmute` -- the DAC never started | 0 of 3 |

**It is the current the speaker draws**, not the DAC being switched on. That
had been the pop, and the pop was already fixed. A song at full volume never
did it (0 of 22 in 063) because a song is not also blitting 57,600 pixels and
reading 416 KB/s from the card; video adds the rest of the load, and the
supply cannot carry all three.

At 12/16: 0 of 4. At 8/16: 0 of 4. So `vplay` caps a video's volume at 12/16
-- music keeps full volume -- and with the cap in place, asking for full
volume: **0 drops and 0 self-stops in 5 runs**, then a whole file end to end.

The self-stop came with it, every time, and is explained by the same
disturbance: the touch controller reporting a press that no finger made. The
view now names who stopped a video and where the press was, so a recurrence
says so rather than being inferred.

> The fix is a reduction in demand, not a repair. The limit is this board's
> supply, and software can only ask for less of it.

---

## 8. Three instruments, all wrong, all mine

This day's most expensive faults were not in the board.

**7.1 A list that emptied itself.** `video <n>` built the song list on the
shell task while the display task built the same list with the same static
directory iterator. The FAT mutex guards each call, not a whole walk, so the
two walks reset each other's count: *"videos=0"* for a card with a video on
it. Only the view's own frame touches the list now.

**7.2 A session that opened on the past.** The serial driver keeps what
arrives while nothing is reading, so every new connection replayed minutes-old
text. A status query answered with an earlier run's numbers; a probe matched a
prompt printed long before. Twice this read as *"the shell has hung"* while
the board was answering normally.

**7.3 A loop that could not end.** Fixing 7.2 meant draining stale text before
each command — and the first attempt, which extended its own deadline on every
byte received, was left in place after the fixed one. Against a board that
prints continuously it never returns. Every timed command hung, which reads
exactly like a deaf board, and it was chased into the UART's receive FIFO
registers before the loop itself was found.

Asked directly, once the tool was honest: **0 receive overflows, 1.88 million
polls, longest gap 57 ms.** The board had been answering all along.

> Three faults, three tools, one pattern: each reported something true and
> narrower than the question it was asked. UM-NATOS-060's rule again, and this
> time the instruments were written the same week.

Two defensive fixes were kept from that hunt, because both faults were real
even though neither was the one being chased: every wait on the audio clock is
now bounded (an unbounded one would hang the shell silently), and an
overflowed UART receive FIFO is reset and counted rather than leaving the
board permanently deaf.

---

## 9. Metrics

| Quantity | Value |
|---|---|
| Picture | 180x320, 256 colours, 7 fps, full panel |
| Audio | 8-bit mono, 22,050 Hz, from the same chunks |
| Per frame | read 80.1 ms + draw 46.8 ms of 143 ms (89%) |
| Frames dropped | **0 of 361**, and 0 of 105 across repeated test runs |
| Underruns | 1 per full playback, consistently — not chased |
| Video volume | capped at 12/16; full volume dropped the USB link 2 times in 3 |
| Data rate | 416 KB/s from the card |
| File | 21.0 MB for 51.6 s; converted in 19 s on the PC |
| SD bus | SPI3 at 20 MHz; read ~1.31 us/byte |
| Board RAM used by video | the MP3 player's buffers, borrowed; SRAM1 unchanged |
| Prediction vs measurement | 79.8 ms predicted, 80.1 measured |

---

## 10. What this does not establish

1. **`.nvd` is invented here.** Nothing else reads it; a PC cannot open one
   without `--preview`. That is the price of a format shaped to a 512-byte
   sector and a palette the panel already speaks.
2. **One video, one card, one board.** FAT32 has still never been seen on real
   media; the card is 256 MB FAT16.
3. **The drop is bounded, not cured.** §7 reduces what the speaker asks of
   the supply; it does not make the supply stronger. A louder passage, a
   weaker USB port, or a bigger picture could cross the same line again, and
   the cap (12/16) is the lowest value TESTED to work, not a measured margin.
4. **1 underrun per playback**, consistently, at a boundary not identified.
5. **No seeking, no pause, no next.** The player plays a file and stops.
6. **7 fps is 89% of the frame budget.** A busier system, or a card that reads
   slower, drops frames — which it will say, having counted them.
7. **The frame-count persistence oddity** noted in UM-NATOS-063 §10 is still
   unexplained.

---

## 11. References

- `docs/next_moves/12-video.md` — steps 1–6, including the wrong turns
- UM-NATOS-063 — the MP3 player, whose measurements sized all of this
- `tools/vidconv.py` — the converter, and the format's specification
- `kernel/vplay.c`, `kernel/vidlist.c` — playback and the browser

**An operating system written from scratch, on a 240x320 panel driven by a
80 MHz microcontroller with no MMU and no C library, played a video from its
own SD card — full screen, in sync with its sound, without dropping a single
frame — from a file a PC had shaped for it byte by byte.**
