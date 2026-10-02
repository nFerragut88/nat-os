# UM-NATOS-066 — The Engine and the Evidence

**Used Medias LLC — Embedded Systems Division**
Revision 1.0 · 2026-10-02 · Status: **12 fps, 619 of 619 frames, nothing dropped. Every wrong turn was a measurement I already had and had not read.**

---

## 1. Abstract

```
shown 619  dropped 0  = 11.9 fps shown over 51,970 ms   underruns=1
per shown frame: read 39.8 ms  draw 31.7 ms   worst frame 80 ms of 83.3
crc32=0xf584e81c   unchanged at every step of the way
```

UM-NATOS-065 left the board at 10 fps and named the next target: SD reads cost
~0.36 µs a byte of CPU above the 20 MHz wire time, all of it the CPU carrying
bytes through peripheral registers, and DMA would remove it. This report is what
happened — the frame rate went 10 → 12, reads went 1,022 → 1,083 KB/s, and the
route there ran through **five dead ends, a hardware rule nobody documents, and
four occasions where the instrument printed the answer and the engineer read
past it.**

The engineering content is §3 (the rule), §4 (what it cost to find), and §6
(the mistakes, which are the useful part). §7 is what the day did not solve.

---

## 2. What was built

```
spi3_xfer_dma()           full duplex by DMA; 0xFF from a buffer for a read
spi3_read_dma_start()     the same transfer in two halves, so a caller can
spi3_read_dma_collect()   do something while the engine runs
sd_command_hw()           a card command as ONE 8-byte transfer, its answer
                          read in 16-byte batches and scanned in memory
sd_read_blocks()          pipelined: block k's transfer runs while block k-1
                          is copied out of its staging buffer
draw_frame()              a frame read in 512-aligned 2,048-byte chunks
copy_out()                word loads, word stores, destination aligned by hand
spidmatest/time/len       three probes that ask the peripheral, not the author
```

---

## 3. The rule, which is not in any document

**SPI3 is a DMA port for a whole run, or a W-register port for a whole run, and
it cannot be switched.**

One transfer through the W registers permanently costs every later DMA transfer
**16 bytes**, cumulatively, saturating at the peripheral's 64-byte buffer:

```
transfer                              delivered of 512
cold                                       512
after one 16-byte W-register read          496
next, with no W-register read              496     <- it does not recover
next, with no W-register read              496
after a second one                         480
```

Five mechanisms were measured against it and **none** clears it: the DMA channel
reset, the AHB-master FIFO reset, `SPI_SYNC_RESET`, SPI3's DPORT peripheral
reset with a full reconfigure, and re-attaching the pads through the GPIO
matrix. Only a power cycle does.

That is why `sd.c` had to give up the registers entirely — commands included —
and why a tripwire now watches the W-register count and surrenders DMA for the
rest of the run if a diagnostic moves it. `spitest` is one keystroke away from
poisoning the engine, and the fallback is slower and correct rather than fast
and wrong.

**And the engine only retires a descriptor when a whole word has arrived.** A
two-byte transfer leaves the descriptor untouched — owner still set, length 0 —
*after 33 larger transfers in the same burst have succeeded*. The two sub-word
reads the first version made (1–3 bytes to align an offset, then 2 for a
block's CRC) were the whole of what stood between a written driver and a working
one.

---

## 4. What the engine actually costs

```
                     measured   wire at 20 MHz
   16 B               11.0 µs        6.4 µs
   64 B               30.5 µs       25.6 µs
  512 B              206.8 µs      204.8 µs      <- 2 µs of CPU
```

Against 420 µs for the same 512 bytes through the W registers. That is the
promise. The delivery was 7%, and the reason is worth more than the number:
**this implementation busy-waits.** A block is 206 µs of wire time and the task
spends it spinning either way, so DMA removes the per-byte register cost and
nothing else. Bus time is charged to whoever waits for it.

Three things then bought the rest:

| | read/frame | frame | fps |
|---|---|---|---|
| UM-NATOS-065 | 50.5 ms | 86 ms | 10 |
| all-DMA | 47.0 | 82 | 10 |
| + pipelined blocks | 46.2 | 81 | 10 |
| + whole-sector frame reads | **39.8** | **71** | **12** |

**The pipeline** (§2) overlaps the only work available: a block's 44 µs copy
runs during the next block's 206 µs transfer. Two staging buffers, used
alternately, because the engine is writing one while the copy reads the other.

**The whole-sector read** was the big one, and it was not in the driver at all.
`vplay` asked for one blit batch at a time — 12 rows, 2,160 bytes, starting 16
bytes into a sector — so every call came out as

```
partial sector | 3 whole sectors | partial sector
single read    | one pipelined burst | single read
```

and a frame cost **135 block reads for the 113 it contains**. Reading
2,048-byte chunks from the chunk's own 512-aligned base, stepping over the
16-byte header in memory, and emitting rows as bytes accumulate: 113 reads,
every burst pipelined, 39.8 ms a frame.

---

## 5. The copy that was the whole problem

With the transfer working, the data phase split cleanly for the first time:

```
data 405 µs = transfer 290 µs + copy 160 µs
```

160 µs to move 500 bytes is **26 cycles a byte**. This kernel is built `-Os`
and its `memcpy` is a byte loop, so the obvious copy is the expensive one. A
word-wise copy — destination brought to a boundary by hand, aligned source words
shifted into place — took it to **44 µs**, and that single function is the
difference between DMA being slower than what it replaced and faster.

---

## 6. Four measurements I already had

This is the part worth keeping.

**6.1 The probe printed the answer in the same table.** The length sweep said:

```
512 B at +0: delivered 512, written 512
512 B at +2: delivered 512, written 511      <- one byte never arrived
512 B at +3: delivered 512, written 510
```

I read "delivered 512", concluded that destination alignment did not matter,
deleted the alignment check, and got a fast read with the wrong CRC
(`0xf994d357`). **The descriptor's length field is not a count of bytes
written** — and the line that proves it was two rows above the line I acted on.

**6.2 The guard that returned without counting.** A 516-byte transfer exceeded
`SPI3_DMA_MAX` and was refused by the one early return that incremented
nothing. ~12% of blocks silently failed and retried. That was *both* the read
that died at 3.1 MB and the 200 µs of data-phase time I could not account for —
one unexplained number and one unexplained failure, same cause, invisible
because the counter did not exist.

**6.3 The last failure instead of the first.** A one-byte transfer times out
for 500 ms and then falls back, so every failure I examined pointed at `len=1`
— a consequence. The first failure is now recorded and never overwritten, and
it named the real fault (`stage=3 len=2 after 33 good ones`) immediately.

**6.4 A wall clock, again.** Reads measured 884 KB/s where they had measured
1,022, and I spent a cycle on the "regression". Own-CPU time was identical to a
fraction of a percent; the difference was other tasks. This project has a
standing rule about exactly this (UM-NATOS-060) and it has now been broken in
three consecutive reports.

> Four faults, four instruments, one pattern: each one had already measured the
> thing that explained it. The failure was not in the measuring.

### The cost in hardware

Testing this cost **four power cycles and five card reseats**, and almost all of
them were caused by the test before, not by the code under test:

- the alignment pad read through the W registers, poisoning the engine on the
  first block; the tripwire then disabled DMA and the fallback re-read bytes
  already clocked off the card, handing the card a malformed stream
- a probe that configured the peripheral itself and clocked 1.5 KB at a
  chip-select nobody had driven high
- `spi3`, the selftest, tying MISO to a matrix constant and never putting it
  back — two power cycles diagnosed as a wedged card
- 600 back-to-back transfers on a board whose supply UM-NATOS-064 §7 already
  found marginal

And one lesson about the hardware itself: **when this card stops identifying
with varying garbage R1s, it needs reseating, not power.** Four unplug/replug
cycles left it failing; one reseat fixed it immediately, twice. The committed
build failing identically is what finally separated the card from the code.

---

## 7. Metrics

| Quantity | UM-NATOS-065 | Now |
|---|---|---|
| Picture | 180x320, 10 fps | **180x320, 12 fps** |
| Frames | 516 of 516 | **619 of 619**, 0 dropped |
| Read per frame | 50.5 ms | **39.8 ms** |
| Draw per frame | 35.4 ms | **31.7 ms** |
| Frame / budget | 86 of 100 ms | 71.5 of 83.3 ms |
| Worst frame | 88 ms | 80 ms |
| Block reads per frame | 135 | **113** |
| SD throughput | 1,022 KB/s | **1,083 KB/s** |
| Own CPU for 6.4 MB | 4,289 ms | **3,717 ms** |
| Data phase per block | 405 µs | **309 µs** |
| DMA transfers verified | — | 985,160, 0 failures |
| Bytes verified identical | crc32=0xf584e81c | crc32=0xf584e81c |
| Converter prediction | 84% (measured 85%) | 71 ms (measured 71.5) |

---

## 8. What this does not establish

1. **13 fps does not fit.** Its 76.9 ms budget is under the 80 ms worst frame
   already measured. 12 is the honest number.
2. **The next gain is not more DMA.** Read and draw are 40 ms and 32 ms and
   they ADD. Overlapping them — the panel draining one row batch while the card
   fills the next — makes a frame `max(40, 32)` and puts ~20 fps in reach. The
   SD driver has the `start`/`collect` halves; `fat.c` and `display.c` would
   each need the same split. That is a project, not a tweak.
3. **The 16-byte poisoning is unexplained, not merely unfixed.** Five resets
   were measured against it. Something in that peripheral keeps a count that
   nothing in this driver can reach, and the workaround is a policy, not an
   understanding.
4. **One card, one board, one file.** A 250 MB FAT16 SDSC card with 4 KB
   clusters, so a multi-block run is at most 8 sectors.
5. **The busy-wait is still there.** `spi3_read_dma_collect()` spins. Nothing
   yields the CPU during a transfer, so the SD bus still costs the media task
   its full wire time.
6. **1 underrun per playback**, at the end of the file, unchanged since
   UM-NATOS-064 and still not chased.
7. **The volume cap is still empirical** (UM-NATOS-064 §10.3), now surviving
   70% more card traffic than when it was set.

---

## 9. References

- `docs/next_moves/12-video.md` — steps 10–15, with every dead end in order
- UM-NATOS-065 — the measurements that set this report's target
- `kernel/spi3.c` — the engine, the tripwire, and the three probes
- `kernel/sd.c` — the all-DMA command layer and the pipelined burst
- `kernel/vplay.c` — the whole-sector frame reader
- `tools/vidconv.py` — a cost model that now predicts this board to 1 ms

**A from-scratch operating system on an 80 MHz microcontroller plays video at
12 frames a second from its own SD card, full screen and in sync with its
sound, having moved every byte of it through a DMA engine that will not tell
you how many bytes it wrote — and the proof that it works is a CRC that never
changed while everything underneath it did.**
