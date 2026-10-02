# 12 — Video

**Size:** large. **Risk:** bandwidth -- the SD bus and the panel, not decoding.
**Started:** 2026-09-19. Follows 11 (the MP3 player), whose measurements it
is designed from.

Play a video from the SD card on the panel, with sound.

---

## The decision: convert on the PC, not on the board

The user asked whether the board should convert MP4 itself or a PC app should
convert it for the board. The PC, for reasons measured in 11:

- **H.264 does not fit.** One 320x240 reference frame is ~115 KB against ~60
  KB of spare SRAM1 and a ~23 KB heap, and MP3 alone costs 46% of the CPU at
  80 MHz (11 step 4). H.264 is far more work per second of media.
- **The board cannot write files.** fat.c is read-only and sd.c has no write
  command; conversion on the board would need a FAT writer first.
- The PC has ffmpeg and does it in seconds.

So the PC does everything expensive, once, and the board copies bytes.

## Budget (estimates from 11's measurements, not yet tested)

| | |
|---|---|
| SD | 554 KB/s of the reading task's CPU at 10 MHz (11 step 3) |
| panel | full-screen 44 ms (display init line) -> ~22 fps ceiling full-screen |
| audio | 8-bit PCM copied into the existing ring: ~0% CPU, not MP3's 53% |

---

## step 1 — tools/vidconv.py, and the .nvd format

`tools/vidconv.py` (ffmpeg + standard library). The format is specified in its
docstring; in short:

- sector 0 header, sector 1 a 256-colour RGB565 palette, then an RGB565 cover
- one chunk per frame at a FIXED stride (multiple of 512): 16-byte chunk
  header, the frame (PAL8 or RGB565), that frame's slice of 8-bit mono audio
- chunk i carries samples [round(i*rate/fps), round((i+1)*rate/fps)), so audio
  cannot drift; seeking is arithmetic
- RGB565 little-endian: display_blit() takes plain RGB565 (red 0xF800) and
  swaps for the panel itself, so the board copies pixels untouched

Defaults: 240 wide (the panel), 10 fps, PAL8 with ONE palette for the whole
video (per-frame palettes shimmer), bayer dither, 22,050 Hz u8 mono, cover 120
wide. `--check` validates a file end to end; `--preview` rebuilds a frame or
the cover the way the board will and writes a PNG; `--selftest` round-trips a
generated clip in both formats.

Established rather than assumed: ffmpeg's raw PAL8 is the indices FOLLOWED by
a 1,024-byte palette per frame (2 frames of 16x8 = 2,304 bytes).

### 1a. The user's example

`E:\Learn Numbers in Mandarin Chinese ... [LfLmuP.mkv` -- MKV not MP4, which
changes nothing: H.264 1920x1080 29.97 fps, AAC stereo 44.1 kHz, four WebVTT
subtitle tracks, and an embedded 1280x720 JPEG cover (plus the .jpg beside it).

```
output   240x134 pal8 @ 10 fps, audio 22050 Hz u8 mono, cover 120x68
         516 frames x 34,816 B chunks = 51.6 s, 17.1 MB, in 24 s on the PC
         the board must read 340 KB/s          (budget: 554)
--check  OK -- every chunk present, in order, 1,137,780 audio samples
```

51.6 s, not the container's 52.8: the video stream is 51.55 s and the audio
51.62 (their DURATION tags); the container's length includes the subtitles.

`--preview` of frame 250 against the source at 25 s: same scene, colours
right (no channel swap), subtitles legible at 240x134. Cover right.

### 1b. Wrong on the first real file

- **ffprobe's JSON was decoded as cp1252** (`text=True` on Windows) and the
  file's tags are Chinese: a crash before anything ran. Decoded as UTF-8
  explicitly now. The self-test's ASCII file name could not have caught it.
- The title kept `[LfLmuP` -- the name on the card is cut off mid-id with no
  closing bracket. The id strip now allows that.
- The file name was cut mid-word (`...Chinese Counti.nvd`); now at a word
  boundary: `Learn Numbers in Mandarin Chinese.nvd`.

### State

```
works  vidconv.py: any ffmpeg-readable video -> .nvd, checked and previewable;
       the example converted, 340 KB/s, colours verified by eye
open   nothing on the board reads .nvd yet
next   the board-side player: read chunks, blit frames, feed audio, and
       MEASURE the frame rate before tuning size / fps / format

---

## step 2 — a list icon in the file, and --set-cover

The user: use the JPG (copied back onto the card after the video was deleted)
as a small icon, to browse videos by thumbnail the way the music app lists
songs. Decided with the user: the browser takes **meter**'s launcher cell.

### 2a. Format

An icon block after the cover: 64x36 RGB565 by default (`--icon-width`),
located by `ICON` at header byte 192 -- previously unused, and 0 means "no
icon", so the addition breaks nothing. A list row reads 9 sectors per video
instead of a 32-sector cover. `--check` now also verifies both pictures sit
between the palette and the first chunk without overlapping.

### 2b. --set-cover PICTURE FILE.nvd

Rebuilds cover and icon from any picture and copies the chunks across byte
for byte. Built because the example's source video is gone: a thumbnail can be
changed without reconverting. One layout helper (`front_layout` /
`front_bytes`) serves both a conversion and --set-cover, so the two cannot
disagree about where anything is.

Self-test grew a case: a sibling picture becomes 120x68 + 64x36; --set-cover
with a different picture makes the icon blue; the frames are compared before
and after -- identical. PASS.

Applied to the card: `Learn Numbers in Mandarin Chinese.nvd` now carries the
user's JPG as cover (120x68 at 1024) and icon (64x36 at 17408); first chunk at
22016; --check OK. The icon, looked at: the purple character, fence, tree and
logo are recognisable at 64x36.

### State

```
works  .nvd files carry a list icon; --set-cover swaps it from any picture
next   the video browser on the board, in meter's launcher cell

---

## step 3 — the video browser on the board, in meter's cell

`kernel/vidlist.c`, a native view like the music app. The card's `.nvd` files,
6 rows of 40 px: the 64x36 icon from the file, the title wrapped over two
lines, the length. Tap to select, tap again for a detail screen: the 120x68
cover, the title, size / fps / colour format -- and, in yellow, that playback
is the next step. It does not pretend to be a player.

- Pictures stream from the file 512 bytes at a time into one buffer and blit
  as they arrive; no icon or cover is ever held whole.
- A file that is not a version-1 `.nvd` is skipped and counted, not shown half
  broken; picture sizes larger than their slot are not drawn.
- Launcher cell 6: "meter" -> "video", a play button in a frame. meter is still
  registered (`run meter`: "started id=0 perms=light"). The glyph there had
  still been pong's paddle under meter's name.
- Shell: `videoopen` (as the icon), `video` (the list as parsed).

### Measured

```
no card     video view: listed=0 (no SD card)          -- says so
card in     videos=1 skipped=0
            'Learn Numbers in Mandarin Chinese | Counting Numbers in '
            240x134 516 frames 51600 ms  icon 64x36@17408  cover 120x68@1024
```

Every value matches `vidconv.py --check` on the PC. **The user, on the
glass: thumbnail and title shown, and two taps show the bigger cover.**

### A number for the next step

SRAM1 is **58.9 of 60 KB** (_sram1_end 0x3FFFF648). The player will need
its frame and audio buffers from somewhere; the MP3 player's buffers are the
obvious candidate, since only one of them can play at a time.

### State

```
works  the video browser: thumbnails, titles, lengths, a cover detail;
       user-verified
next   playback: stream chunks, blit frames, feed the PCM ring, and measure
       the frame rate before tuning the format

---

## step 4 — playback

`kernel/vplay.c`, run on the MP3 player's task (AUDIO priority) as a third job
kind, borrowing that player's idle buffers (`g_in` 4 KB, `g_pcm` 4.6 KB):
SRAM1 had 2.5 KB left, and only one of them can play at a time.

- **The DAC is the clock.** Frame i is drawn when `pcm_samples_played()`
  reaches frame i's first sample; if frame i+1's moment has also passed, frame
  i is dropped and counted. Falling behind costs frames, never sync.
- **Two cursors through one file.** Audio is queued K chunks ahead by one
  `fat_file_t`; frames are read by another. K is bounded by the ring: (K+1)
  frames of audio must fit its 8,192 samples, so K = 2 at 22,050 Hz / 10 fps
  (3 would put every frame's moment in the past as its turn came).
- **The ring starts full of silence**, so the file's first sample plays after
  8,192 samples; the clock is offset by exactly that.
- **`fat_seek` walks forward from where the file is** when the target is not
  behind it. Two cursors a chunk apart through a 17 MB file would otherwise
  have walked the chain from the first cluster -- 4,000+ FAT lookups -- per
  cursor per frame.
- Frames: 9 rows at a time, indices -> RGB565 through the palette, blitted;
  `draw_frame` checks the stop flag per batch, and the view's x calls
  `vidlist_close()`, which stops the video and waits for it to be off the
  panel before the launcher repaints.
- The view: the detail screen has a green "play"; while playing, the view
  draws only OUTSIDE the picture (title above; clock and bar below, once a
  second); any tap stops. Shell: `video <n>` (the button's path), `mp3 video
  <file>`, `mp3` status.

### 4a. Measured -- the whole example

```
frame 516 of 516  240x134 @ 10 fps   audio 2 chunks ahead
shown 516  dropped 0  = 9.9 fps over 51,990 ms (51.6 s of video)
per shown frame: read 58.4 ms  draw 26.1 ms   worst 87 ms   (budget 100)
```

**240x134 PAL8 at 10 fps fits, with ~15% headroom.** Reading is the larger
cost, as the budget said it would be. No tuning of the format needed.

**The user, at the panel: "looks and sounds right"** -- picture, colours,
audio, sync.

### 4b. Wrong on the way, fixed

- **A race I wrote:** `video <n>` built the list on the shell task while the
  display task built the same list with the same static directory iterator.
  The fat mutex guards each call, not a whole walk: "videos=0" for a card with
  a video on it. Only the view's frame() touches the list now; `video <n>`
  leaves a request.
- **2 underruns at the end** of the first full play: the tail waited for the
  last audio without feeding the ring, so it ran dry -- the drain lesson from
  11 step 1, relearned. It now feeds silence; the run after showed 0 (to
  frame 73, see below).
- **Not a bug, and nearly chased as one:** `fat ls` once showed
  `[tEJ6JVBUB.mk]` for `[tEJ6JVBUBmk]`. It is the heartbeat '.' that another
  task prints into the serial stream -- the same interleaving that has broken
  `i.nt_raw` and `0x00.000054` all along. Four clean listings followed.

### 4c. Open: the USB drop is back at video start, and a phantom stop

- **The CH340 dropped off USB at video start, 2 of 2 times** -- after 11 step
  9's soft start made song starts clean (0 of 22). The board played on.
- **In the second run the video STOPPED itself at frame 73** (~7.3 s). Nothing
  sent a stop; in this view only a tap does. The likely cause is a phantom
  touch -- the touch controller misreading during the same electrical event.
  If so, the disturbance reaches the board, not only the PC's link.
- What differs from a song start, which no longer drops: the display blitting
  continuously and SD reads at 340 KB/s, together with the DAC; and this
  video's own audio. The loudness hypothesis was eliminated for songs (11 step
  9: silence dropped, full volume did not) -- but that was the pop; a second,
  load-dependent cause was never tested. Next experiments: the same video
  converted with `--no-audio` (DAC never on), and at `mp3 vol 0`.
- Seen in passing: the boot banner's persisted frame count is 910,336 at boot
  #159, as at #127, though step 8's once-a-minute saves change it in RAM.

### State

```
works  video playback: the example, all 516 frames, 0 dropped, 9.9 fps, in
       sync, user-verified; read 58 + draw 26 ms of a 100 ms frame
open   USB drop at video start (2/2) and a self-stop at frame 73 (4c);
       frame-count persistence (4c); auto-advance in the music app (11 6d)

---

## step 5 — full screen, sideways

The user asked for the video to fill the screen. The panel is 240x320 upright
and the video is 16:9, so something has to give; the choices were put to them
with the frame rate each costs, measured per pixel from step 4 (read 1.81 +
draw 0.81 us/px at 10 MHz). **Rotated, sharp, held sideways** was chosen:
180x320 fills the panel exactly.

### 5a. The bus, first

At the SD bus's 20 MHz (div 4) instead of 10, measured DURING playback:

```
read 42.2 ms/frame (was 58.4)   draw 24.7 (was 26.1)   worst 70 ms (was 87)
```

Reading is ~1.31 us/byte, drawing ~0.77 us/pixel. **div 4 is now the
default** in sd.c; div 2 stays refused (11 step 3d). That is what makes
180x320 possible: 57,600 px costs ~124 ms against 143 ms at 7 fps.

`vidconv.py` now prints that arithmetic for every file it writes -- ms per
frame against the budget -- and warns over 85%. It predicts 70 ms for the
240x134 file whose frames measured 67: close enough to size a format with.

### 5b. Full screen means the WHOLE panel

- `vplay` accepts a picture up to `DISP_H` (was `SPEC_Y`, the strip's top).
- The view plays full screen when the picture is too tall to sit under its
  header: no header, no clock, `y = 0`. A tap still stops it.
- **kmain stops drawing the spectrum strip** while such a video plays
  (`vidlist_fullscreen()`), or it would repaint over the bottom 32 rows of
  every frame.

### 5c. Untested until there is a file to test with

The source .mkv was deleted from the card at the user's request (step 2) and
is not on the PC, so the example cannot be re-converted yet -- the user is
fetching the original. To exercise the path meanwhile, a generated 15 s
rotated clip is on the card: `Test pattern rotated.nvd`, 180x320 pal8 at 7
fps, 416 KB/s, ~124 ms of a 143 ms frame (87%).

### State

```
works  the SD bus at 20 MHz by default; vidconv predicts per-frame cost
open   full screen itself is not yet verified on the glass: a rotated test
       file is on the card, the board build is flashed, and the card is
       currently in the PC

### 5d. Full screen, verified -- and a measurement taken at the wrong speed

The user, on the glass: **the test pattern fills the screen**, with black
strips along the two long edges. Those are the shape, not a defect: a 16:9
picture turned sideways is 180 of the panel's 240 pixels. Filling them needs a
crop (~25% of each shot, ~5 fps) or pixel doubling (blocky, 10 fps); the user
chose to keep the whole picture and the strips.

The first playthrough dropped **13 of 105 frames** at 5.9 fps, read costing
104.6 ms/frame against 79.8 predicted. The bus was at **div 8**: checking
whether a card was present, I had run `sdspeed 8`, which sticks until reboot,
and the board had not rebooted since. At 10 MHz, 1.72 us/byte agrees with the
1.81 measured in 11 step 3 -- the model was right, the conditions were not.

At div 4, the same file:

```
shown 105  dropped 0  = 6.8 fps over 15,370 ms   underruns=1
per shown frame: read 80.1 ms (predicted 79.8)  draw 46.8   worst 129 ms
                 = 127 ms of a 143 ms frame (89%)
```

**Full screen at 180x320 and 7 fps works.** The one underrun is not chased.

### State

```
works  full-screen video, 180x320 at 7 fps, 0 dropped, user-verified;
       vidconv's per-frame prediction matched within 0.4%
open   the example still needs re-converting (the user is fetching the
       original); 1 underrun per play; USB drop at video start (4c)

---

## step 6 — chasing the USB drop and the self-stop, and finding my own tools

Asked by the user: fix the USB drop at video start and the run that stopped
itself (4c).

### 6a. Neither reproduces

With the board-side work of step 5 in place (SD at 20 MHz; the 180x320 file):

```
29 video starts, 14 full playbacks:  0 link drops, 0 self-stops
before, with the 240x134 file at 10 MHz:  2 of 2 starts dropped the link
```

**That is not a fix, it is a failure to reproduce.** Three things changed at
once -- the DAC's soft start (11 step 9), the bus speed, and the format -- and
none of them was tested against the fault in isolation. What exists now is the
instrument that was missing: the view says WHO stopped a video and where the
press was (`[video] stopped by a press at x,y`), so a recurrence names its
cause instead of being inferred.

Added for the experiment that is now not needed: `mp3 vmute 1` plays a video
with the DAC never started (the tick becomes the clock), which would separate
an audio-caused drop from a display/SD-caused one.

### 6b. Defensive fixes worth keeping

- **Unbounded waits on the audio clock.** `vplay` waited for
  `pcm_samples_played()` to reach a frame's moment, and for the drain, with no
  bound. If the DAC ever stops advancing, both wait forever -- the task sleeps
  at 0% CPU, looking idle, while `mp3 stop` blocks in `wait_done()` and the
  shell stops answering. Both are now bounded (5 s) and report
  `VPLAY_E_STALL`; `wait_done()` gives up after 10 s and says so.
- **The UART receive FIFO can wedge.** 128 bytes; overrun it -- a burst of
  commands while the shell is not running -- and the hardware raises
  RXFIFO_OVF and stops accepting while the board keeps printing. Recovery is
  a FIFO reset; `shtime` counts them.

### 6c. The instruments that were lying, both mine

The "wedged shell" that prompted 6b's hunt was **board.py**, twice over:

1. **A session opened on stale text.** The driver keeps what arrived while
   nothing was reading, so a fresh run replayed minutes-old output: a status
   query answered with an earlier run's numbers, and the probe matched a
   prompt printed long ago. `open_port()` now calls `reset_input_buffer()`.
2. **A leftover drain loop.** Fixing (1) I added a drain before each command,
   and left the first attempt in place after it -- and that one extended its
   own deadline on every byte received. Against a board that prints
   continuously it never ends. Every `--prompt` run hung, which read exactly
   like a deaf board; it was chased into the UART registers before the loop
   was found.

The board, asked directly once the tool was fixed: `shtime` reports **0 uart
rx overflows**, 1.88M polls, longest gap 57 ms. It had been answering all
along.

### State

```
works  29 starts / 14 playbacks with no drop and no self-stop; bounded waits
       so a stuck job cannot hang the shell; UART overflow recovery; the view
       names who stopped a video
open   NOT proven fixed -- not reproduced. If it returns, the log now says
       whether a press stopped it, and `mp3 vmute 1` separates audio from
       display/SD
       1 underrun per full playback, consistently, not chased

---

## step 7 — the drop was the speaker, and it is the same fault as the self-stop

Step 6 said "not reproduced, not fixed". It reproduced on the first play of
the USER'S file: every earlier trial had used the generated test pattern,
whose audio is a steady tone. The link dropped AND the video stopped itself at
frame 66 -- together, as at 4c.

Same file, everything else identical:

| | link drops |
|---|---|
| full volume | **2 of 3** |
| `mp3 vol 0` -- DAC on, playing silence | 0 of 3 |
| `mp3 vmute 1` -- DAC never started | 0 of 3 |

**The cause is the current the speaker draws**, not the DAC switching on --
that was the pop, fixed in 11 step 9 (and still fixed: silence at volume 0
does not drop). A song at full volume never did it (0 of 22) because a song
is not also blitting 57,600 pixels and reading 416 KB/s; video adds the rest
and the supply cannot carry all three. The self-stop rides on the same
disturbance: the touch controller reporting a press nobody made.

```
volume 12/16   0 of 4       volume 8/16    0 of 4
```

`vplay` caps a video's volume at **12/16** (`VPLAY_VOL_CAP`), music keeps full
volume, and `mp3 vol` still lowers it. Video audio now honours `mp3 vol` at
all, which it did not before.

```
with the cap, asking for full volume:  0 link drops, 0 self-stops in 5 runs
the user's file, end to end:           361 of 361 frames, 0 dropped,
                                       6.9 fps over 51,940 ms, underruns=1
```

**This is a reduction in demand, not a repair.** The limit is the board's
supply; 12/16 is the lowest value tested to work, not a measured margin.

### State

```
works  video plays whole files with sound, full screen, no link drop and no
       self-stop; video volume capped at 12/16 and obeying `mp3 vol`
open   the cap is empirical; a louder file or a weaker USB port could cross
       the line again -- the view will name a phantom press if it does
       1 underrun per playback, not chased
```

---

## step 8 — the frame budget, halved: 127 ms to 85 ms

The user asked whether the frame rate could be improved. 7 fps was not chosen;
it was what step 5 could afford. One frame cost **127 ms** of a 143 ms budget:

```
read 80.1 ms   draw 46.8 ms
```

Both halves turned out to be paying for the same thing -- bytes moved through
peripheral registers by the CPU, one transaction at a time.

### the instrument first

`spitest` (`spi3_probe_speed()`) times 1, 16 and 64-byte transfers at each
divider with `task_cpu_cycles()`, so preemption cannot be read as the bus
being slow (the first version used `xt_ccount()` and measured the same
transfer at 80 us and 59 us). It showed **~0.4 us per byte of software plus
~2.5 us per transaction, at every clock** -- the driver was charging as much
per byte as the 20 MHz wire was.

### the read side

Three changes, each measured against the same file and checked by CRC:

1. **Word-wise register access.** `spi3_xfer()` packed and unpacked the W
   registers a byte at a time; a new `spi3_read()` fills them with
   0xFFFFFFFF and reads them back as words.
2. **Batched token polling.** The card answers a read command with 0xFF until
   its data token. One byte per transaction cost 343 us a block for bytes that
   are all padding. Now 16 at a time, and the batch that contains the token
   also contains the first data bytes, which are kept rather than re-read.
3. **Multi-block reads (CMD18).** The measurement that made this obvious:

   ```
   per block: cmd 39 us   token wait 347 us   data 405 us
   ```

   That 347 us is the card's access latency, and it is charged **per command,
   not per sector**. `sd_read_blocks()` reads a run of consecutive sectors with
   one CMD18, and `fat.c` offers it every whole-sector run inside a cluster.

```
                    KB/s    cmd   token   data    per block
before              480     46    338*    664     ~694 (CPU)
word-wise + batch   637     46    347     405      791
+ CMD18            1022      7     77     390      474
```

*The "338" was the instrument lying: those phase counters were cumulative
since boot and this board had read blocks at three different dividers, so the
average answered a question nobody asked. `fat cat` now reports the delta
across the one read it performed. The apparent "no change" in the token wait
after batching was that average, not the bus.

**The bytes are unchanged**, which is the only reason any of this counts:
`crc32=0xf584e81c` before and after every step, chain 1563 of 1563 clusters.
And an instrument that can fail: `fat cat` prints how many blocks arrived by
multi-block command, so a future change to `fat.c` that stops offering runs
shows up as `0` rather than as quietly worse throughput.

```
12,496 of 12,504 blocks in 3,124 commands -- 4 a burst, the buffer's size
```

### the draw side

`display_blit()` byte-swaps every pixel into a 480-byte staging buffer and
sends that, so a full-screen frame was 57,600 swaps and 240 DMA transactions
over a ~23 ms wire floor. `display_blit_be()` takes bytes **already in the
panel's order** and hands them to the DMA engine in 4 KB pieces, with no
staging copy. The video player gets that order for free by keeping its palette
byte-swapped: 256 swaps per file instead of 57,600 per frame.

It refuses a rectangle that does not fit rather than clipping one, because
narrowing a contiguous stream misplaces every row after the first -- a subtly
wrong picture is worse than none.

### where the budget stands

```
                 read     draw    frame    worst frame
before          80.1     46.8     127      143 (budget)
after           50.0     35.1      85       88
```

0 frames dropped, 0 underruns, `dmastat` 0 timeouts over 14,494 transfers.
**The file is still 7 fps**, so nothing looks different yet -- the gain is
headroom, and spending it needs the card in the PC and a re-convert.

### State

```
works  the same playback at two thirds the cost per frame; SD reads 1022 KB/s
       (was 480); bytes proven identical by CRC at every step
open   the read half is still ~0.36 us/byte of CPU above the 20 MHz wire
       time, because sd.c moves data through the W registers. SPI3 DMA is the
       next ~2x on that half, and display.c already has the pattern
open   the 7 fps file has not been re-converted, so the headroom is unspent
```

---

## step 9 — the headroom spent: 10 fps, 516 of 516

Step 8's gain was invisible, because the file was still the 7 fps one. The
source MKV was still on the card, so the card went into the PC and
`vidconv.py` re-cut it at 10 fps: 516 frames, 59,904 B chunks, 29.5 MB, and
**585 KB/s the board must now sustain** against 416 before.

The converter's own prediction was re-measured first. It had carried the
step 4-5 constants -- 1.31 us/byte read, 0.77 us/pixel draw -- which after
step 8 describe a machine that no longer exists and would have advised against
a frame rate the board can hold:

```
read  1.31 -> 0.82 us/byte      draw  0.77 -> 0.61 us/pixel
  -> per frame ~84 ms of 100 ms available (84%)
```

Measured, playing it:

```
shown 516  dropped 0  = 9.9 fps shown over 51,930 ms   underruns=0
per shown frame: read 50.2 ms  draw 35.4 ms   worst frame 88 ms
```

51.93 s of playback for 51.6 s of video: the DAC is the clock and it runs
slightly slow, which is the same 0.6% the music player shows. No link drop at
the higher data rate, so the 12/16 volume cap still covers the louder load.

Two things the day's instruments got right, worth recording because so many
of them have been wrong:

- the card had to be re-initialised after being hot-swapped, and the view said
  `listed=0 (SD read failed)` rather than showing an empty list
- `fat cat` reported `12,496 of 12,504 blocks in 3,124 multi-block commands`,
  which is what proves the CMD18 path is the one running

### State

```
works  the example plays at 10 fps, 516 of 516 frames, 0 dropped, 0
       underruns, no link drop; vidconv.py predicts the board it measures
open   the read half is still ~0.36 us/byte of CPU above the wire time; SPI3
       DMA is the next ~2x and would put 13-15 fps in reach
open   1 underrun per full MP3 playback, still not chased
```

---

## step 10 — SPI3 DMA: the engine works, the driver cannot use it

Step 8 left reads costing ~0.36 us a byte of CPU on top of the 0.4 us the
20 MHz wire itself takes, all of it peripheral-bus accesses to the W registers.
DMA removes the CPU from the data path, so this was the next ~2x on the read
half and would have put 13-15 fps in reach. **It does not work here, and the
reason is worth more than the speed would have been.**

### what was built

`spi3_read_dma()` -- inbound SPI3 DMA on channel 2 (the display has channel 1),
with the outbound channel streaming 0xFF from a buffer because a read still has
to send ones. Descriptors in DRAM, every wait bounded, a timeout disabling the
engine for the run, and `sd_read_blocks()`/`sd_read_block()` retrying once when
a transfer fails because the failure disables DMA and the retry then runs on the
proven path.

### the engine works

`spidmatest` (`spi3_probe_dma()`) asks the peripheral and the channel alone,
with CS high so the card ignores the clock entirely:

```
512 B: ok  int=0x1e8  bytes changed=512 of 512  rx_flags=0x40200200
            size 512, length 512, owner cleared -- every byte delivered
```

Four, 16, 64, 100, 256, 500, 508 and 512 bytes, all exact.

### why the driver cannot use it

```
transfer                        delivered of 512
cold                                 512          <- perfect
after one 16-byte W-register read    496
next, with no W-register read        496          <- it did not recover
next, with no W-register read        496
after a second one                   480
next, with no W-register read        480
```

**Each W-register read permanently costs every later DMA transfer 16 bytes**,
cumulative, saturating at the peripheral's 64-byte buffer. Not cleared by the
DMA channel reset, the AHB-master FIFO reset, `SPI_SYNC_RESET`, or taking SPI3
through its DPORT reset and reconfiguring it -- all four measured.

`sd.c` has to mix: commands go out through the W registers and so does the
data-token poll. So DMA needs the whole SD command layer rewritten to be
DMA-only, with commands and token polls batched into buffered transfers rather
than the byte-at-a-time protocol they are today. That is the open route to
13-15 fps; it is not a patch.

The cheap alternative is closed too. Half the W-register traffic is filling the
transmit side with 0xFF, and skipping it looked safe on the grounds that a card
streaming a block is not listening. **It is listening**: during a multi-block
read the card watches MOSI for CMD12, which is the only thing that stops the
stream. Sending the previous chunk's bytes instead put a 0x40-prefixed byte in
front of it and broke the first read of the card.

### five wrong answers, and what each cost

1. **Waited for `IN_SUC_EOF`.** In master mode the peripheral never produces a
   stream EOF for the inbound channel, so a descriptor marked eof=1 always
   retires as `IN_ERR_EOF`. Every transfer "failed" while delivering all 512
   bytes correctly. The fix is to ask the descriptor: owner cleared, and the
   length it reports equal to what was asked.
2. **Fell back to a W-register re-read after a failed DMA.** A failed transfer
   still CLOCKED -- those bytes are gone, so the re-read returns the NEXT ones
   and the block is assembled from two places. That is what `mount failed: no
   FAT boot sector` was, on a card whose block 0 is fine. display.c records
   this same mistake on its transmit side (UM-NATOS-031 §2).
3. **A probe that configured the peripheral itself**, run before anything had
   driven the card's chip-select high. It clocked 1.5 KB at a floating CS and
   the card stopped answering CMD0 until its power was cycled. The probe now
   refuses to clock the bus unless `sd_init()` has just succeeded.
4. **Three builds spent on an uninitialised peripheral.** Nothing in those
   sessions had touched the card, `spi3_init()` is called from `sd_init()`, and
   `SPI_USER` sat at its power-on 0x80000040 -- no receive phase at all. Reading
   the register back is what ended it, and the probe prints it now.
5. **`spi3`, the selftest command, tied MISO to a matrix constant and never
   put it back.** Every read after it failed, `sd_init()` could not recover
   because it only re-routes at the END of a successful identification, and
   this was read as a wedged card twice, with a power cycle each time. It hands
   the bus back now and says whether the card re-identified.

### where things stand

The DMA path stays in the tree, armed and OFF: `spidma 1` enables it,
`spidmatest` probes it. Nothing uses it.

```
fat cat Test pattern rotated.nvd
  6,398,464 bytes, crc32=0xf584e81c, chain 1563 of 1563
  own CPU 4,307 ms -- against 4,289 ms before this step began
video 2, full screen
  shown 516  dropped 0  = 9.9 fps over 51,950 ms  underruns=0
  per frame: read 50.5 ms  draw 35.4 ms  worst frame 88 ms
```

The wall-clock KB/s wandered between 884 and 1022 across these runs and meant
nothing: own CPU is identical to a fraction of a percent. Measured in the wrong
clock, this step would have read as a 13% regression.

### State

```
works  everything step 9 left working, unchanged and re-verified
open   13-15 fps needs an all-DMA SD command layer, for the reason measured
       above. The engine is ready; the protocol layer is not
open   1 underrun per full MP3 playback, still not chased
```

---

## step 11 — the all-DMA SD path: written, measured, and never once completed

Step 10 ended with a rule: a W-register transfer permanently costs every later
DMA transfer 16 bytes, so SPI3 is a DMA port for a whole run or a register port
for a whole run. That makes 13-15 fps conditional on `sd.c` giving up the
registers entirely -- commands included. This step builds that, and it does not
work yet.

### the cost that justified the work

`spidmatime` times back-to-back DMA reads, DMA only, in this task's cycles:

```
                measured   wire at 20 MHz   CPU above the wire
   16 B          11.0 us        6.4 us           4.6 us
   64 B          30.5 us       25.6 us           4.9 us
  512 B         206.8 us      204.8 us           2.0 us
```

A block's data costs 420 us through the W registers and 207 by DMA. Four
transfers a command instead of thirty keeps the command cheap, so a block
projects to ~290 us against 546 -- read 50.5 -> ~27 ms a frame, a ~62 ms frame,
15 fps.

### what was built

`spi3_xfer_dma()` -- full duplex, 0xFF from a buffer when there is nothing to
send, staging for destinations the engine cannot write. `sd.c`'s hardware path
has no W-register transfer left in it: the command goes out as one eight-byte
transfer with its answer read in sixteen-byte batches and scanned in memory,
and the token poll, the block, its CRC and the trailing clocks after deselect
are all DMA. A tripwire watches the W-register count and surrenders DMA for the
rest of the run if a diagnostic moves it.

### the rule that finally emerged

**The engine retires a descriptor only when a whole word has arrived.** A
two-byte transfer leaves the descriptor untouched -- owner still set, length 0
-- *after 33 larger transfers in the same burst had succeeded*:

```
first failure: stage=3 len=2 flags=0xc0000004 after 33 good ones
```

33 good transfers is the command, the token poll and the full 512-byte block,
all correct, all by DMA. The only thing that failed was the two trailing CRC
bytes. A one-byte transfer fails the same way, which is why `sd_xfer()` now
goes to the registers and says so rather than trying.

So the data phase asks for the block's remainder AND its CRC in one transfer,
rounded UP to a word. That overshoots by 0-3 bytes of whatever follows, which
inside a stream can be the next block's token and the bytes behind it, so the
overshoot is scanned and handed to the next token wait. The CRC is always
consumed, which is what keeps the stream in step.

That code is written. It has never completed a read.

### five instruments, four reseats, and the shape of the mistake

Every attempt to test the path needed the card physically reseated first, and
the reason was almost always the test before it:

1. The **alignment pad** read 1-3 bytes through the W registers -- poisoning the
   engine on the first block, after which the tripwire disabled DMA and the
   fallback re-read bytes already clocked off the card. A card handed a
   malformed stream has to be reseated.
2. A **one-byte transfer** then timed out for 500 ms and fell back the same way,
   which is why every failure I examined pointed at `len=1`: a consequence.
   The instrument now records the FIRST failure and never overwrites it.
3. `spi3`, the selftest command, **tied MISO to a matrix constant and never put
   it back**; `sd_init()` only re-routes at the end of a successful
   identification, so a failing one never got there. Two of the four power
   cycles were this. It hands the bus back now.
4. `spidmatime` fired **600 back-to-back 512-byte transfers**; the card wedged
   after every long run. The loop is 40 now. The board's supply was already
   known to be marginal (step 7).
5. And twice the card simply needed **reseating** -- the committed build failed
   identically, which is the only reason I stopped blaming my code.

### what is NOT the explanation

Measured and ruled out, each against the 16-byte deficit: the DMA channel
reset, the AHB-master FIFO reset, `SPI_SYNC_RESET`, SPI3's DPORT peripheral
reset with a full reconfigure, and re-attaching the pads through the GPIO
matrix. The first row of the mixing probe was always perfect, which looked like
evidence that something cleared it; re-arming the engine immediately before the
transfer does not.

### State

```
works  the 10 fps playback of step 9, unchanged, re-verified at crc32=
       0xf584e81c and 516 of 516 frames. DMA is OFF: g_dma_allowed = 0 in
       spi3_dma_init(), which is the whole of what a test session must flip
open   the word-aligned data path has never completed a read. The next
       attempt needs: that one line, a fresh boot, a seated card, and
       `fat cat Test*` -- crc32 agreeing or it is wrong
open   1 underrun per full MP3 playback, still not chased
```

---

## step 12 — the all-DMA path works, and it buys 7%

Step 11's code, two bugs later, reads the card correctly:

```
fat cat Test pattern rotated.nvd
  6,398,464 bytes   crc32=0xf584e81c   chain 1563 of 1563
  own CPU 3,908 ms  (the W-register path: 4,307 ms)
985,160 DMA transfers, 0 timeouts, 0 refusals, 0 failures ever recorded
video 2, full screen, 516 frames: 0 dropped, 0 underruns
  per frame: read 47.0 ms (was 50.5)  draw 35.3 ms  worst frame 85 ms
```

### the two bugs, and what each taught

**A transfer longer than the limit was refused in silence.** A block asks for
its remainder plus the CRC rounded up to a word, which reaches 516 bytes when
the token lands at the end of a poll batch -- over `SPI3_DMA_MAX`, so the one
guard that returned without counting anything rejected ~12% of blocks. Each
then failed, retried, and cost double. That was the read that died at 3.1 MB
AND the 200 us of data-phase time I could not account for. The guard counts
now.

**The copy was the whole cost.** With the transfer working, the data phase
split as `transfer 290 us, copy 160 us` -- and 160 us to move 500 bytes is 26
cycles a byte, because this kernel is built `-Os` and `memcpy` is a byte loop.
A word-wise copy that brings the destination to a word boundary by hand and
shifts aligned source words into place: **160 us -> 44 us**.

### and the measurement I read past

The length-sweep probe answered two questions at once, and I took the wrong
answer from it:

```
512 B at +0: delivered 512, written 512      <- aligned, correct
512 B at +1: delivered 512, written 512
512 B at +2: delivered 512, written 511      <- one byte never arrived
512 B at +3: delivered 512, written 510      <- two
509 B at +0: delivered   0, written 509      <- data fine, length never written
```

I read "delivered 512" and dropped the destination-alignment check, which made
the copy unnecessary and the read fast -- and wrong: `crc32=0xf994d357`. **The
descriptor's length field is not a count of bytes written.** The probe had
already said so in the same table.

The real rule, now in `dma_usable()`: destination word-aligned, length a word
multiple -- the second not because odd lengths fail to arrive, but because they
cannot be CONFIRMED, and an unconfirmable transfer is not something to put a
filesystem on.

### why 7% and not 50%

Because this implementation BUSY-WAITS. A 512-byte block is 206 us of wire time
at 20 MHz, and the media task spends it spinning on a descriptor either way, so
DMA only removes the per-byte register cost. The bus time is still charged to
the task that waits for it.

```
read per frame   50.5 ms -> 47.0 ms       frame 82.2 ms, worst 85 ms
```

11 fps fits (90.9 ms a frame); 12 does not. The remaining time is not CPU
overhead to shave -- it is the bus, and the only way to stop paying for it is to
stop waiting: start a frame's read, draw the PREVIOUS row batch while the engine
works, then collect. Read and draw overlap instead of adding, and a frame
becomes max(47, 35) rather than 47 + 35 -- around 20 fps, with no faster bus and
no faster card.

### State

```
works  all-DMA SD reads, correct over 985k transfers; 10 fps playback with
       0 dropped frames and ~18% more headroom than before
open   11 fps is available for a re-convert; 12 is not
open   the real gain is an ASYNCHRONOUS read -- spi3_read_dma_start() and
       _collect() -- so vplay can draw while the engine reads. That is a
       vplay/pcm change, not a driver one
open   1 underrun per full MP3 playback, still not chased
```

---

## step 13 — the read, in two halves

`spi3_read_dma_start()` and `spi3_read_dma_collect()`: the engine is started,
the caller does something useful, the caller comes back. One transfer in flight,
because there is one descriptor pair, and starting twice without collecting is
refused rather than papered over.

The first thing worth doing in between was already waiting: `sd_read_blocks()`
starts block k's transfer, copies block **k-1** out of its staging buffer, then
collects. Two staging buffers, used alternately -- the engine writes one while
the copy reads the other, which is the point and also why one buffer would be a
corruption bug rather than a slow path.

```
fat cat Test pattern rotated.nvd    crc32=0xf584e81c, chain 1563 of 1563
  data phase per block   408 us -> 309 us
  own CPU for 6.4 MB   4,307 (W registers) -> 3,908 (DMA) -> 3,717 ms
  throughput             940 -> 1,083 KB/s
```

### and almost none of it reaches the video

```
read per frame   50.5 (W) -> 47.0 (DMA) -> 46.2 ms (pipelined)
```

0.8 ms, against the ~11 ms a frame the per-block saving predicts. The reason is
the shape of the reads, not the driver: `vplay` asks for 12 rows at a time, and
12 x 180 = 2,160 bytes starting 16 bytes into a sector. Every such call is

```
  partial sector | 3 whole sectors | partial sector
  single read    | one 3-block burst | single read
```

so only the middle is pipelined, and 135 blocks get read for the 113 a frame
contains -- 20% of the bus wasted on sectors read twice for a few bytes each.

Fixing that is a `vplay` change and needs no new format: read the frame in
**512-aligned 2,048-byte chunks** starting at the chunk's own base (its first 16
bytes are the chunk header, which has to be read anyway), and carry the leftover
bytes across chunk boundaries into row batches. Then every read is four whole
sectors, one burst, fully pipelined, nothing read twice. 113 blocks a frame
instead of 135, with the overlap applying to all of them: ~37 ms a frame, which
puts 13 fps inside the budget.

### State

```
works  all-DMA pipelined SD reads, crc32 verified; 10 fps playback with
       0 dropped frames, read 46.2 ms of an 81.5 ms frame
open   11 fps fits TODAY with a re-convert. 13 needs the aligned chunked
       reads above, which is where the next effort belongs
open   the bigger prize is still overlapping the READ with the DRAW -- the
       driver now has the start/collect halves that would need, but fat.c
       would need them too, and a frame becomes max(read, draw)
open   1 underrun per full MP3 playback, still not chased
```

---

## step 14 — reading a frame in whole sectors

Step 13's pipeline reached almost none of the video because of the shape of the
reads. `draw_frame()` asked for one blit batch at a time -- 12 rows, 2,160
bytes, starting 16 bytes into a sector -- and every such call came out as a
partial sector, three whole ones, and another partial. Only the middle was a
burst, and a frame cost 135 block reads for the 113 it contains.

It now reads **2,048-byte chunks from the chunk's own 512-aligned base**, steps
over the 16-byte chunk header in memory instead of seeking past it, and emits
rows as the bytes accumulate. A row straddling two chunks is drawn when the
second arrives; the leftover carried across is always less than one row, so
compacting it moves at most 179 bytes.

```
                 read    draw   frame   worst
before today     50.5    35.4    85.9    (143 ms budget at 7 fps)
all-DMA          47.0    35.3    82.3
pipelined        46.2    35.3    81.5
whole sectors    39.7    31.7    71.4    80 ms
```

0 dropped frames and 0 underruns throughout, and the picture verified on the
glass -- which matters here more than the numbers, because "0 dropped" says
nothing about whether the rows went to the right places.

The draw got cheaper too, which was not the point and is not fully explained:
fewer, larger blits and less contention with the SD engine are the likely
reasons.

**12 fps fits** -- 83.3 ms a frame against 71.4 average and 80 worst. 13 does
not: its 76.9 ms budget is under the worst frame already measured.

### State

```
works  39.7 ms a frame of reading, 113 block reads a frame instead of 135,
       every burst pipelined; picture confirmed unchanged
open   the file is still 10 fps until it is re-cut at 12
open   past 12 fps needs read and draw to OVERLAP rather than add: the driver
       has the start/collect halves, fat.c does not, and a frame would become
       max(40, 32) instead of 72
open   1 underrun per full MP3 playback, still not chased
```
