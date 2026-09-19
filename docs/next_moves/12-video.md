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
