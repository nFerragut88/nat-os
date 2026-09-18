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
