# 11 — An MP3 player

**Size:** large. **Risk:** medium; the unknown is CPU time, not correctness.
**Started:** 2026-09-17. MP4/video is explicitly out of scope, for later.

Play an MP3 from the SD card through the SPEAK connector, from an app on the
desktop with a file list, play/pause/next and a progress bar.

---

## What did not exist when this started

Checked in the tree, not assumed:

| need | state on 2026-09-17 |
|---|---|
| sample playback | **none.** `audio.c` is LEDC tones only; UM-NATOS-027 s2 deferred PCM until interrupts worked |
| a filesystem | **none.** `sd.c` reads raw 512-byte blocks; nothing parses FAT |
| a decoder | none |
| RAM for a decoder | heap ~23 KB; minimp3 wants ~23 KB of its own. SRAM1's 60 KB at `0x3fff1000` is measured free (08 step 396) but unclaimed |
| CPU for a decoder | **the chip runs at 80 MHz** (`clock.c`). A 44.1 kHz stereo MP3 may not fit; unmeasured |
| FPU | minimp3 is floating point; whether this kernel has CPENABLE set, or saves FP state across switches, is unchecked |

## Decisions taken with the user

- **Decoder: vendor minimp3** (public domain, source, into `vendor/` the way
  lwIP is). Writing a Layer III decoder is a project of its own.
- **CPU: measure first.** Run stages A-D at 80 MHz, time real frames, then
  choose between raising the clock and limiting the file format, on numbers.

## Plan

- **A.** PCM out: I2S0 -> DAC2 by DMA, polled. `pcm` command. **DONE, step 1.**
- **B.** FAT32 read-only + SD throughput measurement (128 kbps needs 16 KB/s,
  and the SD bus is bit-banged).
- **C.** Claim SRAM1; enable the FPU for one task.
- **D.** minimp3 on the board: `mp3 decode FILE` reports ms/frame against the
  real-time budget. This is where the 80 MHz decision is made.
- **E.** Streaming playback: `mp3 play FILE`.
- **F.** The desktop app.

---

## step 1 — the DAC plays samples, by DMA, with no interrupt

### 1a. What was built

`kernel/pcm.c`: I2S0 in built-in-DAC mode, feeding DAC2 (GPIO26) from a ring
of 8 descriptors x 512 samples (8 KB, 186 ms at 22,050 Hz), allocated from
the heap only while playing.

**No interrupt.** The producer reads `OUT_EOF_DES_ADDR` -- the last descriptor
the DMA finished -- whenever it polls, and retires buffers from that. The only
timing requirement on software is refilling before the ring laps, which is
tens of milliseconds. UM-NATOS-027 s2's objection was that PCM needs a clock
at 8 kHz against a 100 Hz tick; I2S generates the clock and the DMA walks the
ring, so the tick only has to be fast enough to refill.

Register offsets from the vendor's `i2s_reg.h`/`i2s_struct.h`/`sens_reg.h`/
`rtc_io_reg.h`. The bring-up ORDER and every field value copied from IDF's own
`components/driver/dac/esp32/dac_dma.c`: 16-bit mono FIFO mode, `tx_chan_mod
1`, `bck_div 16`, LCD mode, right-first, `dac_dig_force` + `dac_clk_inv`.
DAC1 (GPIO25, the touch clock) is never powered.

Three instruments, each built before it was needed: `rate_actual` (samples the
DMA consumed per second of CCOUNT), `underruns`, and `blind` (polls far enough
apart that the ring could have lapped unseen -- the case where `underruns`
itself is a lower bound).

### 1b. What the board says

```
pcm tone 3000    rate asked=22050 measured=22534  over 3249 ms
                 buffers played=143 underruns=0 blind=0 bogus_eof=0
                 clkm=0x001859e2   (226 + 25/33 -- exactly 160 MHz / (32 x 22050))
                 pad_dac2=0x84060400 (MUX_SEL, XPD_DAC, XPD_FORCE, code 0x80)
                 dac_ctrl1=0x02400000 (DIG_FORCE, CLK_INV)
pcm sweep        measured=22349 over 5269 ms, 230 buffers, underruns=0
```

### 1c. What the ear says

The user, at the speaker:

| test | heard |
|---|---|
| `pcm direct 3000` -- CPU writes PDAC2_DAC, no I2S | clean tone |
| `pcm tone 3000` -- I2S + DMA | clean steady tone |
| `pcm sweep` -- 500 -> 6000 Hz through DMA | smooth rising glide |

**Sample playback works.** The DAC, not only LEDC, drives this speaker. UM-027
s8 recorded "the DAC is untouched now... whether the cosine generator could
have been made to work was never established". The DAC itself is now
established. The likely difference from 027's attempt is `XPD_FORCE` (bit 10),
which IDF's `dac_ll_power_on()` sets and 027's driver never did, but that was
**not tested** and is recorded as a guess.

### 1d. Open: two instruments that disagree with something

1. **The measured rate is 1.4-2.2% high and varies between runs** (22,349 /
   22,484 / 22,534) against a divider that is exactly right. A wrong clock
   would be wrong by the same amount every time. The spread points at the
   measurement: elapsed time comes from CCOUNT, and **if CCOUNT pauses while
   the idle task is in WAITI**, busier runs would read closer to true.
   Hypothesis only. The distinguishing experiment is the same interval timed
   against `timer_ticks()` and against CCOUNT side by side. 2% is a third of a
   semitone and does not block anything.
2. **`pcm direct` reported 8,065 samples written in 3 s** (of ~66,000) and 37
   preemptions, which predicts a badly broken tone. The ear reported a clean
   one. Either the count or the reading of it is wrong. Not chased: the direct
   path was a control, the control passed, and nothing will use it.

### 1e. Eliminated

- **"PCM needs working interrupts."** It does not. A polled DMA ring with
  186 ms of depth ran 3 tests, 516 buffers, 0 underruns, with the producer
  being the shell task yielding between 64-sample refills.
- **"The DAC does not drive this speaker"** -- the reading UM-027 s8 left
  open. It does.

### State

```
works  pcm tone / pcm sweep: I2S0 -> DAC2 by polled DMA, 8-bit, 22,050 Hz,
       audible and clean; 0 underruns across 516 buffers
open   rate reads 1.4-2.2% high and varies -- suspect CCOUNT in WAITI, untested
       pcm direct's sample count contradicts the ear
       rates below 19.6 kHz are unavailable (8-bit divider); a 16 kHz MP3
       will need upsampling
       a store_save() masks interrupts for 125 ms -- inside the 186 ms ring,
       but only just; the player should defer saves while playing
next   B: FAT32, and how fast the bit-banged SD bus actually reads
```

---

## step 2 — the card as files, and a bus too slow to play them

### 2a. What was built

`kernel/fat.c`, read-only: MBR or superfloppy, **FAT16 and FAT32 decided by
cluster count** (fatgen103's only rule, not the "FAT16" label), directories,
cluster chains, seek, and long filenames checked against each short entry's
checksum. `fat` / `fat ls [dir]` / `fat cat <file>` / `fat head <KB> <file>`;
a name ending `*` matches by prefix, because these names are long, bracketed
and full of spaces.

`kernel/mp3hdr.c`: Layer III frame headers and ID3v2 tag sizes, no decoding.
Built as an instrument first (below), and the player will need it anyway.

### 2b. The card is not what was asked for, and that is fine

Asked for FAT32. It is a **256 MB SDSC card, FAT16** (MBR type 0x06, 490,000
sectors, 4 KB clusters, 512 root entries, formatted `MSDOS5.0`). Handled.

```
fat ls     12 entries: System Volume Information/ and 11 .mp3,
           7.9 MB to 54.7 MB, long names intact (non-ASCII dashes as '?')
```

### 2c. The check that needs no copy of the file

A CRC32 proves the bytes only against a reference computed on a PC. The frame
walk does not need one: every Layer III header gives the next header's offset,
so a wrong byte or a cluster read from the wrong place breaks the walk AT that
frame.

```
fat head 256 musical*
   ID3 tag of 776776 bytes skipped by seeking
   262144 bytes in 15271 ms = 17 KB/s   blocks=513
   MPEG-1 Layer III  48000 Hz  stereo  32-320 kbps (VBR)
   frames walked=846 = 20 s of audio -- every header where the previous one said
fat head 128 night*
   ID3 tag of 453504 bytes skipped by seeking
   frames walked=182 = 4 s   (32 KB/s of audio data in this stretch)
```

846 consecutive headers after a 776 KB seek, across 64 clusters: chain
following, `fat_seek`, and the bytes themselves, all at once.

### 2d. Two things the files taught

1. **The tags are enormous.** 776 KB and 453 KB of ID3 -- cover art. Read
   rather than seeked, that is 45 s of silence before the first note at this
   bus speed. The first `fat head` run read 256 KB of JPEG and truthfully
   reported "no frame found"; the reader now seeks.
2. **These are 48 kHz stereo VBR up to 320 kbps.** Measured stretches need
   13-32 KB/s; a 320 kbps passage needs 40.

### 2e. The number that decides step 3

**17 KB/s**, wall clock, in the shell task, bit-banged. Every bit costs CPU and
the shell has only a share of it -- which is also what the decoder will need.
It does not carry these files.

The fix is structural: the slot is wired to **SPI3's native IO_MUX pins** (18,
19, 23, 5 -- spi3.h says so), and SPI3 is otherwise unused.

### State

```
works  fat / fat ls / fat cat / fat head: FAT16 read-only with long names,
       seek, and an MPEG frame walk that validated 846 frames after a 776 KB seek
open   17 KB/s is below what these files need (up to 40 KB/s)
       fat.c and sd.c are unlocked, and device.c is a second SD caller; the
       player makes a third. Needs a mutex before the player exists.
       shell stack low-water 704 B free after fat head -- watch it
next   step 3: SD on the SPI3 peripheral

---

## step 3 — SD on SPI3, and the real bottleneck was never the bus

### 3a. What was built

`sd.c`: identification stays bit-banged at ~250 kHz (the card requires
<= 400 kHz until initialised, and that path is proven). Afterwards SCK/MOSI/
MISO are routed to **SPI3** through the matrix and every byte goes through the
peripheral; block data in 64-byte bursts (the W registers' capacity).
`sdspeed <div>` selects 80 MHz / div, 0 = bit-banged. Default div 8 (10 MHz),
now also at boot.

`spi3_set_div()` added to spi3.c. `cpu`: per-task CPU share over one second.

### 3b. Measurements

Same 256 KB after the same 776 KB seek, frame walk identical every time
(846 frames, every header in place):

| bus | wall | this task actually ran | token wait / block |
|---|---|---|---|
| bit-banged | 16 KB/s | **134 KB/s of its own CPU time** | 31 bytes |
| SPI3 10 MHz | 70 KB/s | **554 KB/s** | 49 bytes |
| SPI3 20 MHz | 76 KB/s | -- | -- |

- **The card is fast**: ~49 bytes of token wait per block, ~40 us. Multi-block
  reads (CMD18) would buy little. Not done.
- **The clock is not the limit**: doubling it moved wall-clock 70 -> 76 KB/s.
- **SPI3 is 4x cheaper per byte of CPU.** A 320 kbps file costs the reader
  ~7% of one core.
- **The limit is CPU share.** Of a 3,706 ms read, the shell ran 473 ms.

### 3c. Where the CPU goes

```
cpu      display 74-76%   vm-host 10-11%   report 8-9%   touch 6-7%
         shell 0%   idle 0%        -- with the desktop idle, nothing moving
```

**The machine is saturated while doing nothing visible.** The display task takes
three quarters of the CPU with an unchanging desktop. This is the most
important number for the player, more than 80 MHz: a decoder will need that CPU
back (redraw only on change) or priority over it. Not chased yet; it is the
display's behaviour, and step D must first learn the decoder's cost in its OWN
cycles (`task_cpu_cycles()`), independent of what share it gets.

`cpu` reports a 100-tick sleep as 1,145 ms of CCOUNT. The sleep returns when
the shell next gets a slice after its deadline, so this is not a clock
measurement. Recorded because 1d's rate skew makes CCOUNT-vs-tick figures
worth noticing.

### 3d. Eliminated, and a hazard found

- **"The card is slow"** -- 40 us of access latency per block.
- **"The bus clock is the limit"** -- 2x clock, 1.09x wall.
- **div 2 (40 MHz) is dangerous, not just unreliable.** Identification failed
  and the card stayed unresponsive through the next two inits, recovering only
  after a clean bit-banged init. Garbled MOSI can decode as ANY command,
  including a write. `sd_set_speed` now clamps to div >= 4 and `sdspeed`
  refuses below 4. The routing theory (pins left on SPI3) was checked and
  **eliminated**: gpio_out_init restores both IO_MUX and FUNC_OUT_SEL, and ten
  consecutive inits in both directions later all passed.

### State

```
works  SD over SPI3 at 10 MHz: 554 KB/s of CPU time, frame walk identical
       cpu: per-task share
open   display takes ~75% of the CPU on an idle desktop; idle task gets 0%.
       The player's real obstacle.
       fat/sd unlocked; device.c and boot are also SD callers (step 2e)
next   C: claim SRAM1, enable the FPU; D: minimp3, timed in its own cycles

---

## step 4 — minimp3 decodes on the board, at 46% of real time, at 80 MHz

### 4a. What was built

- **`vendor/minimp3/`**: lieff's minimp3, CC0, commit `ea99364`, sha256
  recorded. **One local change**: an `#ifdef` letting the 16 KB
  `mp3dec_scratch_t` be a static instead of a stack local (README.md there).
  The decoder is therefore not reentrant.
- **SRAM1 claimed.** `linker.ld` region `sram1` = 0x3FFF1000..0x40000000,
  exactly the block 08 step 396 measured as surviving a flash write, PHY
  bring-up and a WPA2 join. NOLOAD section `.sram1`. It holds the decoder
  state, scratch, an 8 KB input buffer, the PCM frame and the decoder task's
  6 KB stack: **41.9 KB of 60, and 0 bytes of heap.**
- **The FPU.** CPENABLE was never set and FP registers are not saved on a
  switch. The decoder task sets CPENABLE; `build.ps1` now **fails any build in
  which an object other than mp3.c contains an FPU instruction**, so "only one
  task uses the FPU" is checked rather than remembered. The check was itself
  checked: 661 hits in mp3.c.o, 0 in kmain.c.o.
- **`__divsf3`** supplied in mp3.c (no libgcc): reciprocal by bit trick + 3
  Newton steps. minimp3 divides once, in L3_pow_43, by an integer >= 64. Not
  IEEE; limits stated at the definition.
- **`mp3 bench <frames> <file>`**: decodes on its own task ('mp3'), timing
  decode and SD reads in THAT TASK'S OWN cycles.

### 4b. Measured

```
mp3 bench 100 musical*   100 frames 48 kHz 2 ch 32-256 kbps
   decode 1002 ms CPU for 2400 ms audio = 41%   avg 10.0 ms/frame, worst 11.4
   output peak=0 mean=0     <- 14,432 bytes for 100 frames: a silent lead-in
mp3 bench 300 night*     300 frames 48 kHz 2 ch 32-320 kbps
   decode 3373 ms CPU for 7200 ms audio = 46%   avg 11.2 ms/frame, worst 14.2
   SD read  425 ms CPU = 5%
   output peak=32768 mean |s|=4745 -- music, at full scale
   cpenable=0x1 read back inside the task; no fault
```

**The "measure first" decision is answered: 80 MHz is enough.** 48 kHz stereo
at up to 320 kbps costs ~46% decode + ~5% SD = ~51% of one core, with the
worst frame (14.2 ms) well inside its 24 ms. No clock change and no file
limit needed -- decoded from flash (irom) with cache misses included.

The zero-peak result on the first file was checked, not waved through: that
stretch averaged 144 bytes/frame (~48 kbps, floor 32), which is what digital
silence costs in VBR, and the second file's full-scale output shows the path
from decoder to sample buffer works.

### 4c. What this makes the real problem

~51% needed, and step 3c measured the display task taking ~75% on an idle
desktop with idle at 0%. Round-robin will not give the decoder half the CPU.
That is step 5's problem.

### 4d. Not attributed

The boot banner showed `LAST FAULT: exception, exccause 20, epc 0x00000000
(boot #99)` -- InstFetchProhibited at address 0, a call through a null
pointer. The current boot is **#127**. The record persists until another fault
overwrites it, so this happened 28 boots ago and cannot be placed within this
work or outside it from the record alone. Recorded, not chased.

### State

```
works  minimp3 on its own task, SRAM1, FPU; 46% of real time for 48 kHz
       stereo 320 kbps at 80 MHz; SD 5%
open   the decoder needs ~51% of the CPU; the display takes ~75%
       LAST FAULT exccause 20 epc 0 from boot #99, unattributed
next   E: streaming playback -- decode, downmix, pcm_write at 48 kHz --
       and the scheduling that makes it keep up

---

## step 5 — a whole song, clean, at 80 MHz

### 5a. What was built

- **`mp3 play <file>`** runs in the background on the 'mp3' task; `mp3` shows
  position, underruns, the DAC's measured rate and the decoder's CPU against
  wall time; `mp3 stop` stops. Stereo is averaged to mono, played at the
  file's own rate; rates outside 19.6-48 kHz are refused, not played at the
  wrong speed. A ring of silence follows the last frame so the DMA does not
  loop stale audio.
- **The PCM ring moved to SRAM1 and doubled**: 16 x 512 samples, 170 ms at
  48 kHz (8 buffers were only 85 ms there -- less than one 125 ms
  interrupts-masked store_save). SRAM1: 54.3 of 60 KB.
- **A FAT mutex.** The decoder reads the card for minutes while a person can
  type `fat ls`; fat.c/sd.c share one sector buffer and are not reentrant.
  Every public fat_* takes a recursive mutex; `fat_lock()` is exported and
  taken by the shell's sd/sdread/sdspeed and by device.c's SD channel.
- **`TASK_PRIO_AUDIO` (3)**, above the display. `TASK_AGE_MAX` 3 -> 4, as
  NA-006's static assert requires for a new level.

### 5b. The priority, measured both ways

| player at | decoder CPU | display | result over the run |
|---|---|---|---|
| HIGH (level with display) | 23% | 56% | **465 underruns**, 11 s of audio in 30.6 s |
| AUDIO (above display) | 60-62% | 26-29% | **0 underruns**, whole song |

The player sleeps a tick whenever the ring is full (14,127 times in one song),
which is what makes a top priority safe: it takes what decoding costs.

### 5c. The result

```
Night Nurse, start to finish
   at 4:06  48000 Hz 2 ch  32-320 kbps
   underruns=0 blind=0  ring-full waits=14127  dac rate measured=47999
   decode CPU 116165 ms + SD 15354 ms over 246610 ms wall = 53%
   worst frame 14232 us     output peak=32768 mean |s|=6144
```

**The user, at the speaker: "Music, clean."** First song ever played by this
kernel.

Note `dac rate measured=47999` here against step 1's 1.4-2.2% high at 22,050
Hz. Same instrument, longer runs, different rate; 1d's question stays open
but the error is not present at 48 kHz over four minutes.

### 5d. Two wrong turns, recorded so they are not retaken

1. **"The shell takes ~130 s to answer during playback" -- false; it was the
   tool.** `board.py run` listens for the full `--wait` after EVERY command,
   and the runs used `--wait 120`. 134/142/140 s was 120 s of listening plus
   the probe. It also explains "`cpu` took two minutes" and "`mp3 stop` landed
   at 2:11" (sent after two 60 s waits). Believing it, a fix was built --
   demote the display to NORMAL during playback -- and "measured" against the
   same broken instrument (still ~135 s: no change, of course). **The
   demotion is removed**: it cut the display to 6% during playback and bought
   nothing that could be shown. `board.py run --prompt` now returns at the
   prompt and prints the elapsed time; the note in cmd_run records why.
2. **"The shell is slow because the display ages above it" -- eliminated** as
   the explanation for the above. The ageing arithmetic (display HIGH+4 vs
   shell NORMAL+4) is real, but the demotion that removes it changed nothing.

### 5e. Open: the ~10 s commands

With `--prompt`, measured honestly: `mp3 play` returns in **1.5 s** (2.9 s in
another run); `cpu`, `mp3 stop`, `fat ls` and idle `mp3` take **8-10.7 s**,
idle or playing. `cpu` sleeps only 1 s. So the delay depends on the command,
not on playback -- and it predates the player (the probe has said "shell
answered after 7-11 s" all along). Eliminated: pyserial blocking on
read(4096) (port timeout is 0.2 s). Suspect next: the console lock -- the
report task prints ~900-byte telemetry lines at NORMAL while holding it, and
the slow commands are the ones that print more. Not chased; the desktop app
will be driven by touch (HIGH), not the shell.

### State

```
works  mp3 play / mp3 / mp3 stop: 48 kHz stereo VBR-320 from SD, decoded by
       minimp3 at 80 MHz, 0 underruns over a whole 4:06 song, 53% CPU;
       heard clean by the user
open   shell commands that print take ~10 s to return (5e) -- predates this
       display animates at ~75% CPU when idle (step 3c) -- unchanged, and now
       gets 26-29% during playback
       LAST FAULT exccause 20 from boot #99, unattributed (4d)
next   F: the player as a desktop app -- file list, play/pause/next, progress

---

## step 6 — the music app, in ping's place on the desktop

### 6a. What was built

- **`kernel/player.c`**, a native view like web and wifi (a VM program cannot
  reach the decoder or the card, by design). Header with the red x; the
  card's `.mp3` files, 9 rows with ^/v scroll; now playing (name, elapsed /
  total, progress bar); prev, play/pause, stop, next. Tap a song to select
  it, tap again to play -- the browser's two-tap rule, for the same
  calibration reason. The list (32 x 64 B) lives in SRAM1: 56.3 of 60 KB.
- **Desktop:** icon slot 5 was "ping"; it is now **"music"** (cyan, two
  beamed quavers), `DESK_ACTION_MUSIC`, `MODE_MUSIC`, claiming the band as the
  web view does. At the user's request. **ping is still a registered program**
  (`run ping`; pong's IPC demo sends to it) -- it lost its cell as pong did at
  step 370, nothing more.
- **`mp3.h` grew an API** the view uses and nothing in it blocks: `mp3_play`,
  `mp3_request_stop`, `mp3_set_pause`, `mp3_busy`, `mp3_status`. Switching
  songs is a request -- stop, then start when the decoder is free -- carried
  out by `player_service()`, which the display task runs EVERY frame
  whatever view is up, so a song that ends moves on with the view closed.
- **Pause** stops the DMA and keeps the decoder state and file position; the
  paused time is taken out of the CPU-vs-wall figure.
- **Length from the Xing/Info header** (`mp3hdr_xing_frames`), the silent
  first frame VBR encoders write. Without one, an estimate from bytes
  consumed, shown with a `~`.
- Shell: `musicopen` (as the icon), `music` (the view's own state),
  `music <n>` (play n through the view's transport), `mp3 pause|resume`.

### 6b. Measured

```
music          listed=1 songs=11, all names intact
music 7        Night Nurse: 4:06 exact (xing frames=10268)
music 2        while playing: stop, queue, start -> Billy Boyo 5:35 exact
mp3 pause      PAUSED, position held; resume continued from it
mp3 stop       "stopped" (not "finished")
```

**The user, on the glass: "All works"** -- the icon opens the view, songs
play, the buttons and scrolling do what they say, x returns to the desktop.

### 6c. Wrong on the first try, recorded

- **4:04 for a 4:06 song.** `(frames/100) * spf / (hz/100)` was written to
  "stay inside 32 bits" and lost two seconds to the truncation; `frames * spf`
  fits for anything under ~27 hours. The guard cost more than the overflow.
- Removing the shell's duplicate `err_text` left a stray brace: caught by the
  compiler, not the board.

### 6d. Not yet verified

- **Auto-advance on a natural end.** Every test stopped the song or switched
  it; none let one reach its end inside the view. The path is short
  (`MP3_ST_FINISHED` with an unseen seq -> `start(cur + 1)`) but unexercised.
- Pause/stop from the buttons were exercised by the user by touch; the
  shell exercised the same API calls. The CPU/underrun figures of step 5 were
  not re-measured with the view open; the view repaints only on change
  (once a second while playing, for the clock).

### State

```
works  the music app: launcher icon in ping's place, list from the card,
       tap-tap to play, prev/play-pause/stop/next, scroll, exact length
       from Xing, keeps playing with the view closed; user-verified by touch
open   auto-advance on a natural end not yet observed (6d)
       shell commands that print take ~10 s (5e); display ~75% when idle (3c)
       LAST FAULT exccause 20 from boot #99, unattributed (4d)
```

---

## step 7 — the "10 second shell delay" was the PC reading 4 KB at a time

Asked by the user: fix the ~10 s every shell command took to answer (5e).

### 7a. The board timed itself first

`shtime` (shell.c): CCOUNT stamps across the previous command -- first
character to Enter, waiting for the console lock, running, and the gap to the
prompt -- plus the longest gap between two `shell_poll()` calls.

```
musicopen   lock wait 219 ms, ran 0 ms        host saw: 10.2 s
fat ls      lock wait 0 ms,   ran 230 ms      host saw: 10.0 s
cpu         lock wait 149 ms, ran 1201 ms     host saw: 10.1 s
shell polled the UART 84-267 times between commands, longest gap 517 ms
```

**The board answers in well under a second.** The 10 s is outside it.

### 7b. Then the pipe

`board.py run --prompt` now also records when the ECHO arrives (the shell
echoes as it reads). Echo and prompt arrived together at ~10 s -- so either
input or output was late, not the command. `board.py latency`, a newline every
0.5 s, then managed to send **3 newlines in 25 s**: the host loop itself was
blocking. Timing each pyserial call:

```
write 0.00 s   flush 0.00 s   read 11.16 s (4096 bytes)
write 0.00 s   flush 0.00 s   read 10.27 s (4096 bytes)
```

**`s.read(4096)` blocks until it has all 4,096 bytes**, whatever the port's
0.2 s timeout says, on this Windows / CH340 / pyserial 3.5 setup. The board's
telemetry trickles out at ~400 B/s, so every read took ~10 s -- and every
command, probe and prompt waited behind one.

### 7c. The fix, in board.py only

`rd(s)` reads `in_waiting` bytes (at least one, so a quiet line still sleeps
for the timeout). All four read sites use it.

```
latency 15        sent 30 newlines, got 30 prompts, each ~0.1 s after sending
probe             "shell answered after 1s"  (was 7-11 s all session)
musicopen 0.0 s   mp3 0.1 s   fat ls 0.3 s   cpu 1.3 s (it sleeps 1 s)
```

**Nothing in NatOS changed to fix it.** The shell was never slow.

### 7d. Corrections to earlier steps, in place of rewriting them

- **5e is wrong about where the delay lived.** It says the delay "depends on
  the command, not on playback" and names the console lock as the suspect.
  Both wrong: `mp3 play`'s 1.4 s was simply a read that happened to fill
  sooner. And 5e's elimination -- "pyserial blocking on read(4096) (port
  timeout is 0.2 s)" -- **eliminated the actual cause by reading a setting
  instead of timing the call.** That is the pattern this project's own notes
  warn about, committed while writing it down.
- **The probe's 7-11 s** (every run since step 1, and earlier in 08) was the
  same read, not the board booting or the shell starving.
- 5d's finding stands: `--wait` IS a fixed listen, and the 130 s figures were
  that. It was two tool faults stacked, not one.

### State

```
works  board.py answers at the speed of the board: prompts in ~0.1 s
       shtime: the shell times its own commands
open   display ~75% when idle (3c); auto-advance unobserved (6d);
       LAST FAULT from boot #99 (4d)
```
