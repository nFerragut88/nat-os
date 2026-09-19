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
