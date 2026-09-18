# UM-NATOS-063 — A Song from the Card

**Used Medias LLC — Embedded Systems Division**
Revision 1.0 · 2026-09-18 · Status: **The board plays MP3s from its SD card, clean, at 80 MHz, from an app on the launcher. The "slow shell" that stood in the way turned out to be the PC.**

---

## 1. Abstract

```
   PLAYING  Night Nurse [PvheubKIJz8].mp3
   at 4:06 of 4:06 (xing frames=10268)  48000 Hz 2 ch  32-320 kbps
   underruns=0 blind=0  ring-full waits=14127  dac rate measured=47999
   decode CPU 116165 ms + SD 15354 ms over 246610 ms wall = 53%
```

This report covers `next_moves/11`, steps **1–7**, all of it written in two
days. On the morning of 2026-09-17 the system could make a square wave. It
could not play a sample, read a file, decode anything, or give a task more of
the CPU than the display allowed. By the evening of the 18th it played a
four-minute, 320 kbps, 48 kHz stereo MP3 from the SD card without a single
underrun. The user heard it and called it "music, clean". It was driven from a
touch app that took ping's place on the launcher.

Every layer was built or measured from scratch, and each one decided the next.
The report is organised by those decisions, not by files. Two of the seven
steps are about instruments that were wrong, and one of those instruments was
written during this work.

---

## 2. What did not exist

Checked in the tree on day one, not assumed:

| need | state |
|---|---|
| sample playback | none. `audio.c` made LEDC tones; UM-NATOS-027 §2 deferred PCM until interrupts worked |
| a filesystem | none. `sd.c` read numbered blocks |
| a decoder | none |
| RAM for one | ~23 KB of heap against a decoder that wants ~23 KB of its own |
| CPU for one | 80 MHz. Nobody knew whether that was enough |
| an FPU | never enabled; FP registers not saved on a switch |

The user made two decisions up front. The first was to **vendor minimp3** rather
than write a Layer III decoder. The second was to **measure before choosing**
between raising the clock and restricting the files. Everything below ran at
80 MHz because the measurement said it could.

---

## 3. Samples without an interrupt (step 1)

UM-NATOS-027's objection was that PCM needs "a clock at 8 kHz against a 100 Hz
tick". The answer used no interrupt at all:

- **I2S0 in built-in-DAC mode generates the sample clock in hardware.** Its DMA
  engine walks a ring of descriptors forever, feeding DAC2 on GPIO26.
- **The producer reads `OUT_EOF_DES_ADDR`**, the last descriptor the DMA
  finished, whenever it polls. The only deadline in software is "refill before
  the ring laps". That is tens of milliseconds, not 45 µs.

The bring-up order and every field value were copied from IDF's own
`dac_dma.c`, and register offsets were read from the vendor headers. DAC1 is
GPIO25, the touch controller's clock, and it is never powered.

Three sounds were played for the user: the CPU writing the DAC directly as a
control, a DMA sine wave, and a DMA sweep. All three came out clean, with zero
underruns across 516 buffers. It also settled something 027 §8 left open: **the
DAC does drive this speaker.** The likely difference from 027's silent attempt
is `XPD_FORCE`, which IDF sets and 027 never did. That is recorded as a guess.

---

## 4. Files, and the first wrong bottleneck (steps 2–3)

`fat.c` reads FAT16 and FAT32, decides the type by **cluster count** (the only
rule fatgen103 recognises), walks directories with long names, follows chains
and seeks. The card was not the FAT32 card that was asked for. It turned out
to be a **256 MB FAT16 card**, which was handled without changes.

### 4.1 A check that needs no copy of the file

A CRC proves the bytes only against a PC-side reference. The frame walk does
not need one. Every Layer III header gives the next header's offset, so a
wrong byte or a misplaced cluster breaks the walk at that frame. **846
consecutive headers** after a **776 KB seek** checked chain-following, seek and
content in one pass.

That 776 KB was an ID3 tag full of cover art. Reading through it at the
bit-banged speed would have cost 45 seconds of silence before the first note,
so the reader seeks past it.

### 4.2 17 KB/s, and what it was actually measuring

The bit-banged SD bus read **17 KB/s** of wall clock. The files needed up to
40 KB/s. The slot is wired to SPI3's native pins, so the card moved to the
peripheral after bit-banged identification. That gave **70 KB/s** at 10 MHz
and only **76** at 20 MHz. Doubling the clock bought 9%, so the clock was not
the limit.

The reading task's own CPU time was the right clock (`task_cpu_cycles()`): of
a 3,706 ms read, **the shell had run for 473 ms**. SPI3 is 4× cheaper per byte
(554 KB/s of CPU time against 134), and token wait is 40 µs per block. The card
was never slow. A new `cpu` command showed why the reader got so little:

```
display 74-76%   vm-host 10-11%   report 8-9%   touch 6-7%   idle 0%
```

**The display task takes three quarters of the CPU on an idle desktop.** It
animates a spectrum strip every frame, by design. That became the real problem,
bigger than 80 MHz.

One hazard came out of this: **40 MHz (div 2) left the card unresponsive for
two re-inits.** Garbled MOSI can decode as any command, including a write, so
`sdspeed` now refuses anything faster than 20 MHz.

---

## 5. The decoder, the memory and the FPU (step 4)

- **minimp3**, CC0, commit `ea99364`, sha256 recorded, with **one local
  change**: an `#ifdef` so its 16 KB `mp3dec_scratch_t` can be static instead
  of a stack local. It is therefore not reentrant, and the vendor README says
  so.
- **SRAM1 claimed.** `next_moves/08` step 396 found 60 KB at `0x3FFF1000` that
  survives a flash write, a PHY bring-up and a WPA2 join. It is now a NOLOAD
  linker region holding decoder state, scratch, buffers, the decoder's 6 KB
  stack, the 16 KB PCM ring and the song list. That is **56.3 KB of 60, and 0
  bytes of heap.**
- **The FPU.** The decoder task sets CPENABLE. FP registers are not saved on a
  switch, which is only correct while one task uses them. So `build.ps1` **fails
  any build in which an object other than `mp3.c` contains an FPU
  instruction.** The check was itself checked: 661 hits in `mp3.c.o`, 0 in
  `kmain.c.o`.
- **`__divsf3`** is supplied locally, because no libgcc is linked. minimp3
  divides once, by an integer ≥ 64. The function uses a reciprocal and three
  Newton steps, is not IEEE, and has its limits stated at the definition.

Measured in the decoder's own cycles, on the user's 48 kHz stereo VBR-320 files:

| | CPU at 80 MHz |
|---|---|
| decode | **46%** of real time (11.2 ms per 24 ms frame; worst 14.2 ms) |
| SD read | 5% |

**80 MHz is enough.** No clock change and no file restriction were needed.

The first benchmark's output peak was **zero**. That was checked, not waved
through: those frames averaged 144 bytes (~48 kbps VBR, which is what digital
silence costs), and the next file came out at full scale.

---

## 6. A priority for hardware deadlines (step 5)

Level with the display at HIGH, the player got **23%** of the CPU and underran
**465 times in 30 s**. A new level fixed it:

- **`TASK_PRIO_AUDIO` (3)**, above the display. `TASK_AGE_MAX` went from 3 to 4,
  exactly as NA-006's static assert requires when a level is added. Without it,
  LOW tasks would starve silently.
- The player **sleeps a tick whenever its ring is full**. That happened 14,127
  times in one song, and it is what makes a top priority safe: the player takes
  what decoding costs (53%) and the display keeps the rest (26–29%).

| player at | its CPU | display | result |
|---|---|---|---|
| HIGH | 23% | 56% | 465 underruns in 30 s |
| AUDIO | 60–62% | 26–29% | **0 underruns, whole song** |

A **FAT mutex** arrived with the second caller. While the player reads the card
for minutes, the shell's `fat`, `sd`, `sdread` and `sdspeed` commands and the
device model's SD channel all go through `fat_lock()`.

---

## 7. The app (step 6)

The user asked for the music app **in ping's place.** Launcher cell 5 is now
"music", a cyan pair of beamed quavers. ping is still a registered program:
`run ping` works, and pong's IPC demo still sends to it. It lost only its cell,
as pong did at step 370.

`player.c` is a native view, like web and wifi, because a VM program cannot
reach the decoder or the card. It has a scrolling list of the card's `.mp3`
files and a now-playing area with elapsed / total and a bar. The controls are
prev, play/pause, stop and next. **Tap to select, tap again to play**: that is
the browser's rule, for the same calibration reason.

The length comes from the **Xing/Info header**, the silent first frame VBR
encoders write. It reads 4:06 exact.

- Switching songs is a request, because the decoder has to drain first. It is
  carried out by `player_service()`, which the display task runs **every
  frame, whatever view is up**, so playback continues with the view closed.
- Pause stops the DMA and keeps the decoder state.

The user tested it by touch and reported "all works".

---

## 8. Two instruments, and the shell that was never slow

### 8.1 `--wait` (step 5)

During playback the shell "took ~130 s to answer." A scheduler change was built
to fix it: demote the display during playback. It was then "measured" with the
same tool, which still showed about 135 s. The actual cause was that
`board.py run` listens for the **full `--wait` after every command**, and the
runs used `--wait 120`. The demotion was removed, because it cut the display to
6% and fixed nothing. `run --prompt` now returns at the prompt.

### 8.2 `read(4096)` (step 7)

After that, commands still took ~10 s at the host, idle or playing. Step 5
blamed the console lock. It had also eliminated "pyserial blocking on
read(4096)", because the port timeout read 0.2 s.

When the user asked for the delay fixed, the board timed itself first
(`shtime`): **every command finished in under 0.5 s.** The echo and the prompt
reached the host together, about 10 s late. `board.py latency` managed to send
three newlines in 25 s. Timing each pyserial call:

```
write 0.00 s   flush 0.00 s   read 11.16 s (4096 bytes)
```

**`s.read(4096)` blocks until it has all 4,096 bytes on this Windows / CH340
setup**, whatever the timeout says. The board's telemetry trickles out at about
400 B/s. Reading only `in_waiting` fixed it: prompts now come back in 0.1 s,
and the probe answers in 1 s instead of 7–11 s. **Nothing in the kernel
changed.**

> Step 5 eliminated the real cause by reading a setting instead of timing the
> call. A configured timeout is not evidence that the call honours it. This is
> the "instruments lie narrowly" pattern, committed while writing it down.

Both corrections are recorded in `next_moves/11` as later steps. The wrong
steps were left as they were written.

---

## 9. Metrics

| Quantity | Value |
|---|---|
| Output | I2S0 → DAC2 (GPIO26), 8-bit, mono, polled DMA, no interrupt |
| Ring | 16 × 512 samples, 170 ms at 48 kHz, in SRAM1 |
| DAC rate measured | 47,999 Hz over a 4:06 song (asked 48,000) |
| Card | 256 MB SDSC, FAT16, 4 KB clusters |
| SD bus | SPI3 at 10 MHz; 554 KB/s of CPU time; token wait ~40 µs/block |
| Decoder | minimp3 (CC0), from flash, hardware FPU |
| Decode cost | 46% of real time, 48 kHz stereo VBR ≤ 320 kbps, at 80 MHz |
| Player total | 53% of one core; worst frame 14.2 ms of 24 |
| Underruns | **0** over a whole 4:06 song |
| SRAM1 | 56.3 of 60 KB; heap cost 0 |
| Shell round trip | 0.0–0.3 s (was reported as ~10 s; §8.2) |
| Commits | 7, one per step |

---

## 10. What this does not establish

1. **Auto-advance on a natural end has not been observed.** Every test stopped
   or switched a song before it ended. The path is short and unexercised.
2. **The display still takes ~75% on an idle desktop** (§4.2). Playback works
   around it; nothing has fixed it.
3. **8 bits, mono, one small speaker.** "Clean" means the song is faithful for
   this hardware, not hi-fi.
4. **Sample rates below 19.6 kHz are refused.** The divider's integer part is 8
   bits. A 16 kHz file would need upsampling, and none has been tried.
5. **One card, one set of files, one board.** FAT32 has not been seen on real
   media; exFAT (cards over 32 GB) is refused.
6. **Open instruments from step 1:** the DAC rate read 1.4–2.2% high at
   22,050 Hz and varied between runs (it read exact at 48 kHz). The direct-DAC
   control's sample count contradicts what the ear heard.
7. **`LAST FAULT: exccause 20, epc 0` from boot #99** (the current boot was
   #127) is unattributed.
8. **SRAM1 is untested under BT, deep sleep, OTA and coredump** (08 step 396).
   Nothing that must survive a reboot lives there.

---

## 11. References

- `docs/next_moves/11-mp3-player.md`: steps 1–7, the record this report
  summarises, including the wrong turns
- UM-NATOS-027: tones, and why PCM waited
- `next_moves/08` step 396: the SRAM1 measurement
- `vendor/minimp3/README.md`: provenance and the one local change
- `kernel/pcm.c`, `fat.c`, `mp3hdr.c`, `mp3.c`, `player.c`; `tools/board.py`
  (`rd()`, `--prompt`, `latency`)

**An operating system written from scratch read a FAT16 card, decoded a
320 kbps MP3 on its own FPU in SRAM it had just measured, and streamed it
through a DMA ring into an 8-bit DAC at 48 kHz for four minutes without a
single gap. It did that from a launcher app that took ping's place.**
