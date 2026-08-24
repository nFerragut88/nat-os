# Appendix F — Every Measured Number

Every quantitative claim in this book, in one place, with its grade
(**M**easured / **D**erived / **T**ranscribed) and its chapter.

---

## F.1 The machine

| Quantity | Value | Grade | Ch |
|---|---|---|---|
| CPU clock | **80 MHz** — not the 240 MHz maximum. Derived from tick counts when this book was written; **set and reported** by `kernel/clock.c` since UM-NATOS-036 | D → M | 7, 1 |
| CPU clock without `clock_init()`, on our bootloader | **40 MHz** — the bare crystal, and no instrument in the system could see it | M | 1 |
| Second-stage bootloader, ours / Espressif's | **2,736 B** / 17,536 B | M | 3 |
| Tick interval | 800,000 cycles ≈ 10 ms | M | 7, 8 |
| Interrupt handler prologue overhead | **30 cycles**, constant | M | 7 |
| Internal SRAM | 520 KB | T | 1 |
| DRAM claimed by the linker | 180,736 B | M | 4 |
| PSRAM | **none** — ESP32-D0WD-V3 rev 3.1, coding scheme none | M | 1 |
| Flash | 4 MB, chip ID `0x684016` | M | 20 |

## F.2 Image sizes, by milestone

| Milestone | `.text` | `.data` | `.bss` | Image |
|---|---|---|---|---|
| M0 | 1,124 B | 4 B | 4 B | 1,216 B |
| M1 | 3,340 B | 4 B | 544 B | 3,424 B |
| M2 | — | — | — | 5,312 B |
| M3 | — | — | — | 6,720 B |
| Flash cache | — | — | — | 6,736 B |
| M4 | — | — | — | 10,560 B |
| M5 | — | — | — | 14,464 B |
| Locking | — | — | — | 16,256 B |
| Display | — | — | — | 18,896 B |
| Display syscalls | — | — | — | 19,856 B |
| Touch | — | — | — | 21,888 B |
| **Current** | **119,151 B** of 131,072 IRAM | — | — | **37,248 B** |

## F.3 Memory

| Quantity | Value | Grade | Ch |
|---|---|---|---|
| Heap at M3 | 166,432 B | M | 10 |
| Heap after flash-mapped `.rodata` | **167,680 B** — grew by exactly the 1,248 B moved | M | 4 |
| Heap at M5 (`TASK_MAX` 4→8) | 158,048 B | M | 16 |
| Heap now | lower, **unremeasured** | — | 30 |
| `.rodata` relocated to flash | 1,248 B (0.7% of heap) | M | 4 |
| Heap header overhead | 16 B per block | — | 10 |
| Boot stack reservation | 4,096 B (2% of DRAM) | — | 4 |
| Full 240×320 framebuffer would cost | 153,600 B = **92.3%** of the heap | D | 1 |
| Span buffer instead | 480 B | — | 18 |
| 3D view framebuffer | 80,640 B | — | 25 |
| Font | 475 B, in flash, **0 DRAM** | M | 18 |
| Task stack | 2 KB × 12 slots | — | 8 |
| Stack headroom, worst | 1,604 B free of 2,048 (`app-host`) | M | 12 |
| Stack headroom, best | 1,844 B free of 2,048 (`report`) | M | 12 |
| Minimum margin across all tasks | **78%** | M | 12 |
| Context frame | 96 B / 21 words | — | 8 |
| Mutex | 24 B | — | 11 |
| Persistent record | 52 B, version 3 | — | 20 |
| Message store | ~1.4 KB in one 4 KB sector | — | 26 |
| Terminal scrollback ring | 48 lines, under 2 KB `.bss` | — | 26 |
| PHY calibration buffer | 1,904 B `.bss` | — | 27 |
| PHY private stack | 6 KB, **272 B used** | M | 27 |

## F.4 Scheduling

| Quantity | Value | Grade | Ch |
|---|---|---|---|
| M2 ticks observed | 3,418 | M | 8 |
| M2 switches | 1,140 / 1,139 / 1,139 | M | 8 |
| M2 corruption events | **0** | M | 8 |
| Register checks under interruption (M1) | 1,556,680, **0** corrupt | M | 7 |
| Priority levels | 3, plus ageing credit up to 3 | — | 9 |
| Ageing threshold | 30 ticks per level | — | 9 |
| Worst-case wait for a ready task | ~600 ms, bounded | D | 9 |
| Longest wait observed under stress | 35 ticks (350 ms) | M | 9 |
| Ageing rescues at shipped settings, at M2 | **0** — inert, as intended | M | 9 |
| Ageing rescues on the finished device, one dump | **4,513** — not inert at all | M | 9, 28b |
| Reporter lines in 20 s, starved / with ageing | 0 → 8 | M | 9 |
| `task_sleep(50)` before / after | 107 cycles / **639 ms** (64 ticks) | M | 9 |
| `timer_ticks()` rate before the fix | **217/s** where 100 is correct | M | 7 |
| Comparator overrun before the fix | 14,630,119 cycles = 18 tick periods = **183 ms** | M | 7 |
| Milestones the comparator defect survived | 3 | — | 7 |
| Self-tests that noticed it | **1 of 11** | — | 7 |

## F.5 Locking

| Quantity | Value | Grade | Ch |
|---|---|---|---|
| Acquisitions measured | 3,133 | M | 11 |
| Contentions | 200 | M | 11 |
| Non-owner unlocks / lost updates | 0 / 0 | M | 11 |
| Throughput, pathological (lock every iteration) | ~2,060 | M | 11 |
| Throughput, 1-in-32 | ~50,143 | M | 11 |
| Starvation defect: worker A / worker B | **238,542 / 0** | M | 11 |
| Draw lock held, applications running | 979 ms = **12%** of wall clock | M | 11 |
| Aggregate blocked | **13,051 ms** in 8.08 s of wall clock | M | 11 |
| Blocked per contention vs hold | **63 ms** against a **24 ms** hold | M | 11 |
| Effect of narrowing the hold 25% | 13,051 → 13,085 ms — **none** | M | 11 |
| Blocked, display / application host | 3,880 ms (65%) / 5,894 ms (98%) | M | 11 |
| Frames/s, blocking → best-effort | **3.0 → 9.9** | M | 11 |
| Frames/s with all applications killed | 11.1 | M | 11 |
| Primitives skipped under best-effort | 45 of 1,325 = **3.4%** | M | 11 |
| Raycaster: lock per column → per frame | 25,075,460 µs → 129,000 µs = **194×** | M | 18 |
| Draw lock held by the renderer, finished device | `hold ms=7965` against ~7,860 ms uptime | M | 11, 28b |
| Blocking acquisitions, display / apps / touch / shell | **3,630 / 0 / 0 / 0** | M | 11, 28b |
| Contentions reported on the finished device | **0** — because nothing is permitted to block | M | 11, 28b |

## F.6 The heap

| Quantity | Value | Grade | Ch |
|---|---|---|---|
| Alloc/free cycles tested | 10,000 | M | 10 |
| Blocks after test | **1** — every split undone by a coalesce | M | 10 |
| High-water mark | 5,120 B | M | 10 |
| Allocation failures | 1 (the deliberate one) | M | 10 |
| Refused frees | 2 (both deliberate) | M | 10 |
| `heap_check()` failures | **0** | M | 10 |
| Arena bounds cases, M3 / M4 cross-check | 10 / 35 | — | 10, 14 |

## F.7 The virtual machine

| Quantity | Value | Grade | Ch |
|---|---|---|---|
| Opcodes | 35 (roadmap ceiling ~40) | — | 14 |
| Registers / call depth | 16 / 32 | — | 14 |
| Syscalls | 12, on **one** opcode | — | 17 |
| `sum(1..10)` | **70** instructions | M | 14 |
| Demo program | 120 B, 6 labels | M | 14 |
| Quantum resumptions during the demo | 4 | M | 14 |
| Fault classes exercised | **6 of 10** | M | 14 |
| Bytecode instructions under the scheduler | **3,291,313** | M | 14 |
| Preemptions survived | 450 | M | 14 |
| Accounting: `1,097,103 × 3 + 3` vs observed | 3,291,312 vs 3,291,313 — **drift 1** | D | 14 |
| Dispatch cost | ~**109 cycles**/instruction (ceiling) | D | 14 |
| M5 instruction budget per app | 60,000 each | M | 16 |
| App A: `19,999 × 3 + 3` | = 60,000 exactly | D | 16 |
| App B: `14,999²` | = **224,970,001**, the published value | D | 16 |
| Rogue fault offset | **256, of a 256 B arena** | M | 16 |
| Heap after release | 158,048 B — **exactly** baseline | M | 16 |
| Runaway program | exactly 500 instructions under a 500 quantum | M | 14 |

## F.8 Confinement

| Quantity | Value | Grade | Ch |
|---|---|---|---|
| Fills audited | 136 | M | 17 |
| **Viewport escapes** | **0** | M | 17 |
| Lowest row painted | **250** = slot 2's exact bottom edge, of 320 | M | 17 |
| Touches delivered / withheld | **81 / 109,211** | M | 17 |
| Confinement failures | **0** | M | 17 |
| Messages sent / delivered / refused | 278 / 277 / 157,957 | M | 16 |
| Bad buffers offered | **0** | M | 16 |
| String buffer | 48 B, per-byte bounds-checked | — | 17 |

## F.9 The display

| Quantity | Value | Grade | Ch |
|---|---|---|---|
| Full-screen fill, bit-banged | **387 ms** (~397 kB/s, ~3.2 MHz effective) | M | 18 |
| — original estimate | ~150 ms — **wrong by 2.6×** | — | 18 |
| Full-screen fill, SPI2 FIFO | **78 ms** | M | 18 |
| — predicted improvement | ~12×; actual **5×** | — | 18 |
| Full-screen fill, SPI2 + DMA | **43 ms**, `dma=320/0` | M | 18 |
| Theoretical floor at 40 MHz | ~31 ms | D | 18 |
| Transfers per screen: DMA / FIFO | 320 / 2,560 | D | 18 |
| Bytes for init + clear | **153,634** = 240×320×2 + 34 command bytes | M | 18 |
| SPI clock | 40 MHz (`sysclk/2`) | M | 18 |
| At 80 MHz | works electrically, **visibly noisy** | M | 18 |
| Blit on the FIFO fallback | 55.9 ms | M | 18 |
| **Blit with DMA actually working** | **31.4 ms** | M | 18 |
| Good transfers before the spurious stall | 63,910 | M | 18 |
| Waits per second the raycaster issues | ~2,900 | D | 18 |
| Theories eliminated before the one-bit cause was found | 11 | — | 18, 28b |
| Characters changed to fix it | **1** | — | 18, 28c |
| DMA transfers completed before anyone noticed DMA was off | 110,507 | M | 28b |
| DMA timeouts needed to disable it for the run | **1** | — | 28b |
| Compose / blit, on the FIFO fallback | 21.7 / **55.8 ms** | M | 28b |
| Share of the frame spent getting it to the glass, FIFO | ~70% | D | 28b |
| Wait bound, before / after | 2,000,000 → **40,000,000** cycles (~25 → ~500 ms) | — | 18, 28b |
| DMA threshold in `spi_tx()` | 64 bytes — 32 pixels | — | 28c |
| `gfxrogue` fill width / `draw` fill width | 180 px (360 B a row) / 20 px (40 B a row) | — | 28c |
| DMA transfers per 3D frame | 224, inside one window | — | 28c |
| Framebuffer checksum, camera frozen | `fbhash=0x5b985ce0`, constant for 76 s | M | 28b |
| Pixels covered by `fbhash` | 53,760 | — | 28b |
| Rows in an `fbdump`, and rows silently dropped on its first run | 112 / **61** | M | 28c |
| Panic screen bytes drawn | ~176,000 (`fault` 175,955, `smash` 176,704) | M | 12 |

## F.10 The renderer

> **Every frame number in this section was measured on the FIFO fallback**, i.e.
> after the spurious timeout had permanently disabled DMA — which was the state
> the 3D view ran in for its entire existence up to UM-NATOS-030. They are honest
> measurements of the wrong configuration. The blit component in particular is
> 41.9 ms here against 31.4 ms with DMA working, and the 72% bus share is
> correspondingly overstated. **Not remeasured**, and left rather than adjusted
> by arithmetic, because a derived number presented alongside measured ones is
> how this project got into trouble twice already.

| Quantity | Value | Grade | Ch |
|---|---|---|---|
| Rays per frame | 240, one per column | — | 25 |
| Frame: march / compose / blit | **2.6 / 13.9 / 41.9 ms** | M | 25 |
| Frame rate | ~9.1 fps | M | 25 |
| Share of frame on the bus | **72%** | D | 25 |
| `RAY_COLW` 2→1: predicted / actual | 0.2 ms / ~9 ms — **wrong by 40×** | — | 25 |
| Face shading + wall seams cost | unmeasurable — 9.1 fps before and after | M | 25 |
| Distinct cells visited in 20 s, before / after | **4 / 12** | M | 25 |
| Framebuffer, first measurement | fb off 126,650 µs / fb on 131,411 µs — no difference | M | 18 |
| Framebuffer, after two fixes | worth having; **default on** | M | 18 |
| Renderer at HIGH, alone / busy NORMAL band | 2 → 8.5 fps / ~3 fps | M | 9 |
| Frame rate after the comparator fix | 12.4 → **16.0** fps | M | 7 |
| Overlay cost per frame | 324 pixels | — | 24 |

## F.11 Touch

| Quantity | Value | Grade | Ch |
|---|---|---|---|
| Poll rate | 100 Hz, HIGH priority, real 10 ms sleep | — | 19 |
| Conversions per read | 11, two discarded | — | 19 |
| Peak pressure | 2,065 (threshold 300) | M | 19 |
| `z1` under touch / idle | 1,123 / 0 | M | 19 |
| `z2` under touch / idle | 2,992 / ~4,090 | M | 19 |
| Pressure: real contact / approach-release | ~2,000 / ~10 — a factor of 100 | M | 19 |
| Raw span, horizontal / vertical drag | 3,050 / 2,772 counts | M | 19 |
| `raw_x` left / right edge | ~3,300 / ~480 — **decreases** | M | 19 |
| Corner readings TL/TR/BL/BR (raw_x) | 3360 / 591 / 3258 / 376 | M | 19 |
| Calibration error, corner-derived | **23% on X, 11% on Y** | M | 19 |
| Mapping error at x = 30 / 120 / 210 | −29 / −8 / **+12** px — changes sign | M | 19 |
| Days the X axis was inverted | **~3** | — | 19 |
| Rail samples per tap before the gate | ~2 of 3 | M | 19 |
| Samples per reporter interval, before / after | ~15 / **~150** | M | 9 |
| Attempts before the driver worked | 4 | — | 19 |
| Self-inflicted interrupt edges before masking | **3,917** | M | 22 |
| Finger-driven wakes ever observed | **0** | M | 22 |

## F.12 Storage

| Quantity | Value | Grade | Ch |
|---|---|---|---|
| Flash user clock | ~8 MHz (80 MHz / 1 / 10) | — | 20 |
| Chip ID read / expected before the fix | `0x34200B` / `0x684016` — exactly `>> 1` | M | 20 |
| Divider × edge combinations tested in one boot | **16** | M | 20 |
| Registers saved/restored per transaction | 6 | — | 20 |
| Boots survived in test | 16 | M | 20 |
| Frames across resets | 256 → 512 → 512 | M | 20 |
| Save cadence | every 256 frames (~60 s at 4.4 fps; now less) | M | 20 |
| Renderer rate when the cadence was set | **4.4 fps** — 81 frames in 18.3 s | M | 20 |
| SD identification clock | ~250 kHz (spec ≤400 kHz) | — | 21 |
| SD error codes | 7, one per stage | — | 21 |
| Card under test | ~250 MB, SDSC, FAT16 | M | 21 |
| Partition | type 0x06, LBA 240, 490,000 sectors | M | 21 |
| IO_MUX entries cross-checked / that could have caught the fault | **4 / 0** | — | 21 |

## F.13 Sensors and sound

| Quantity | Value | Grade | Ch |
|---|---|---|---|
| ADC channels / resolution / attenuation | 8 / 12-bit / 11 dB | — | 22 |
| Light sensor swing (hand over board) | **265 counts** | M | 22 |
| Control-group maximum | **47 counts** | M | 22 |
| Sensor usable range | 273–538 of 0–4095 | M | 22 |
| Wrong bits in the ADC driver | **1** | — | 22 |
| I²C pull-ups | internal, ~45 kΩ (vs 2.2–10 kΩ wanted) | T | 22 |
| Devices found on a bare bus | 0 | M | 22 |
| **Bytes ever transferred over I²C** | **0** | — | 22 |
| Speaker: 440 Hz / 3 kHz | inaudible / clear | M | 23 |
| Usable from | ~1 kHz (estimate, not measurement) | — | 23 |
| Click | 3 kHz, 2 ticks (~20 ms) | — | 23 |
| CPU while sounding | **none** | — | 23 |
| Pins swept before the fault was found | **15** | — | 23 |
| Faults stacked | **3** | — | 23 |

## F.14 WiFi

| Quantity | Value | Grade | Ch |
|---|---|---|---|
| OSI table entries declared / implemented | 116 / **39** | — | 27 |
| MAC moving words: cold / after phy / after mac | **0 / 6 / 13–15** | M | 27 |
| TSF rate, two measurements | 1000 kHz / 1001 kHz | M | 27 |
| — over | 622 ms and 508 ms of CPU time | M | 27 |
| MAC address | `5c:01:3b:50:3f:64`, CRC8 `0x08` | M | 27 |
| — reversed order gives | `0x8f` | M | 27 |
| PHY stack used | **272 B of 6,144** | M | 27 |
| Frames received / descriptors recycled | 372 / 374 across **4 buffers** | M | 27 |
| Networks decoded | 2 | M | 27 |
| Beacon rate before / after the HIGH change | 3 Hz → 9.4 Hz (theoretical) | M | 27 |
| Frames transmitted / completed / **answered** | 178 / 178 / **0** | M | 27 |
| Probe requests / responses | 20 / **0** | M | 27 |
| Transmit power (`most_tpw`) | `0x28` = 40 quarter-dBm = **10 dBm** | M | 27 |
| IRAM cost of the MAC init chain | **2,459 B** — not the 48 KB believed | M | 27 |
| `update_rx_chain` worst-case wait | **1** iteration | M | 27 |

## F.15 Process

| Quantity | Value | Ch |
|---|---|---|
| Build cycles spent on the M2 defect | **12** | 8 |
| Wrong hypotheses recorded (M2) | 3 | 8 |
| Instructions in the M2 fix | **5** | 8 |
| Commits the display freeze survived | 3 | 17 |
| Hypotheses tested against stale firmware | 2 | 20 |
| Tools written to avoid a capture failure that reproduced it | **2 of 2** | 19 |
| Instruments caught lying, one session | **7** | 27 |
| Instruments caught lying, cumulative after the one-bit fix | **15** | 28c |
| Display theories eliminated by measurement | 8 | 27 |
| Sessions between the first garbled frame and the cause | 2 | 28b, 28c |
| Instruments built during those two sessions | 10 | C, 28b |
| Of those, instruments that found the defect | **2** — `fbdump`, `fbpattern` | 28c |
| Conclusions published and retracted | **21** | E |
| Scripted edits that failed silently | 4 | 12 |
| Reverted attempt | ~200 lines, four files | 24 |
| Layout assertions | 6, spanning four files | 24 |
| Screen rows allocated | **320 of 320** | 24 |
| Commits | 138 | G |
| Reports synthesised | 30 | E |
| Kernel source | 671,884 B across 79 files | A |
| **Image** | **37,248 B** | — |

## F.16 The four numbers worth remembering

**37,248 bytes.** The whole operating system.

**0 escapes across 136 fills**, and **0 confinement failures across 109,292
touch queries**. The isolation claim, as numbers.

**63 ms blocked against a 24 ms hold.** Why contention cost is a count and not a
duration.

**3,291,313 instructions, 450 preemptions, drift of 1.** The context switch,
proved by arithmetic rather than by absence of complaint.
