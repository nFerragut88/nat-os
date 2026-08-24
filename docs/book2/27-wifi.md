# Chapter 27 — WiFi: Mixed ABIs, a Vendor Blob, and Frames Off the Air

> Sources: `docs/UM-NATOS-028-wifi-touch-and-the-chrome-column.md`
> Code: `kernel/window.S`, `kernel/window.h`, `kernel/wifi_osi_impl.c`, `kernel/wifimac.c`, `kernel/phyinit.c`, `kernel/efuse.c`, `vendor/windowed/`, `vendor/phy/`

---

## 27.1 What happened

The report opens with an unusually honest summary of a working day:

> This report covers one long day that went: *let us finish the WiFi MAC* →
> *hold on, why is `task_sleep` not sleeping* → *the touchscreen has gone
> strange* → *and now the 3D view is torn* → *ah, it was a rectangle of black
> paint the whole time*.

The outcome:

> nat-os now **receives 802.11 frames**, decodes beacons, names the networks
> around it, and keeps doing so continuously while rendering a 3D view. It also
> **transmits** — in the sense that the MAC accepts frames and reports them
> complete — but nothing on Earth has yet heard one, which is a distinction §27.4
> will insist on at some length.

This is the largest single piece of work in the project and the one that required
the calling convention of Chapter 2 to be *bridged* rather than merely chosen.

## 27.2 The OSI table gets bodies

`wifi_osi_funcs_t` is Espressif's 116-entry function table: the entire contract
between the MAC blob and whatever operating system hosts it.

> Previously nat-os provided all 116 entries with the correct types, in the
> correct order, containing nothing whatsoever. **It linked beautifully and would
> have collapsed the instant anything called it.**

Thirty-nine entries now do real work: semaphores, mutexes, queues, event groups,
software timers, the malloc family, delays, tick conversion and a PRNG.

### The shape matters more than the count

> The table **must** be windowed ABI, because libpp calls it. The kernel **is**
> call0 and always will be. So every windowed entry does no work at all — it
> forwards through `w2c_callN()` in `window.S` into `wifi_osi_impl.c`, where the
> heap and scheduler actually live:

```
libpp (windowed) → wifi_osi.c entry (windowed, does nothing)
                 → w2c_callN (window.S)
                 → wifi_osi_impl.c (call0: heap, scheduler, tick)
```

```c
 * call0, deliberately. The OSI table itself must be windowed because libpp
 * calls it, but the table's only job is to forward: the work happens here,
 * where the kernel's heap, scheduler and tick can be used directly. Each
 * windowed entry crosses back through w2c_callN() in window.S.
 *
 * That split is the design. Writing these bodies in the windowed file would
 * mean either duplicating the kernel's primitives or calling into them across
 * a boundary that does not permit it — the fault that put IllegalInstruction
 * at 0x4008a810 the first time it was tried.
```

### Three design notes

**Fixed pools, not the heap:**

```c
 * ---- static pools, not the heap ------------------------------------------
 *
 * Semaphores, queues, event groups and timers come from fixed arrays. The blob
 * creates its objects during init and keeps them, so a pool costs a known
 * amount of DRAM and cannot fragment, cannot fail late, and cannot leak.
```

```c
#define OSI_SEM_MAX     12u
#define OSI_QUEUE_MAX    8u
#define OSI_EVT_MAX      4u
#define OSI_TIMER_MAX   12u
#define OSI_QUEUE_BYTES 512u        /* item storage per queue */
```

**Handles are tagged indices, checked on every use:**

```c
 * A handle is a tagged index rather than a pointer, so a stale or foreign
 * handle is detected instead of becoming a wild write. The blob is not code
 * this kernel can inspect, and it is the only caller.
 */

/* Tagged handles. The tag is checked on every use. */
#define H_TAG        0x05100000u
#define H_MASK       0xFFF00000u
#define H_MAKE(i)    ((void *)(uintptr_t)(H_TAG | (i)))
#define H_INDEX(h)   (((uintptr_t)(h)) & 0xFFFu)
#define H_OK(h, n)   ((((uintptr_t)(h)) & H_MASK) == H_TAG && H_INDEX(h) < (n))
```

The same argument as the heap's magic words (Chapter 10 §10.3), applied to an
untrusted caller rather than to a buggy one.

**Blocking uses `task_sleep`, not `task_block`:**

> even for an infinite wait. A lost wake-up on `task_block` never runs again.
> This costs a tick of latency on a missed wake; the caller re-checks anyway.
> **Deliberate pessimism.**

That is Chapter 9 §9.3's design applied at the point it was built for: a wait
that degrades to polling rather than to deafness.

Waiters are a bitmask of task ids, which `TASK_MAX = 12` makes trivially cheap:

```c
/* Waiters are a bitmask of task ids. TASK_MAX is 12, so one word covers every
 * task and "wake everyone waiting" is a loop over set bits. */
```

### Verified by driving the table the way libpp will

> `ositest` drives the table through its function pointers exactly as libpp will,
> so each check crosses windowed→call0 and back. All six pass, pools return to
> zero, nothing leaks.

Calling *through the function pointers* rather than calling the implementations
directly is what makes this a test of the bridge rather than of the bodies.

## 27.3 The MAC wakes up, and a fourth read-back failure answered differently

open-mac's entire MAC initialisation is one masked register write:

```c
MAC_CTRL_REG = MAC_CTRL_REG & 0xffffe800;
```

> The work was proving it did anything. This kernel has been caught **three
> separate times** by peripherals whose registers read back perfectly while the
> hardware sat completely dead — LEDC behind `DPORT_PERIP_CLK_EN` bit 11 being
> the clearest. **A readback is not evidence.**

So instead of reading registers, look for *motion*:

```c
/* Reads the MAC register window twice and reports how many words differ.
 *
 * This is the evidence that the peripheral is RUNNING rather than merely
 * readable. A clock-gated block returns a stable value -- usually zero -- from
 * every address; a live MAC has free-running counters in it, and those show up
 * as words that change between two passes with nothing driving them. */
uint32_t wifimac_liveness(uint32_t *first);
```

| stage | moving words |
|---|---|
| cold boot | **0** |
| after `phyinit` | **6** |
| after `macinit` | **13–15** |

Three measurements, monotonically increasing, on a quantity that *cannot* be
faked by a write that went nowhere. This is the best answer in the book to the
read-back problem, and it is qualitatively different from the previous three: it
does not ask the hardware a question at all.

## 27.4 Finding the TSF timer by behaviour

> Espressif publishes nothing about this register map, and every address in
> circulation is reverse-engineered. Rather than assert that some offset is the
> TSF timer, the scan reports the *rate* of every mover:

```
0x3ff73c00  1000 kHz   <- stable across every run
0x3ff73c14  1054 -> 1102 kHz
0x3ff73c18  1055 -> 1107 kHz
0x3ff73dd0  1063 -> 1114 kHz
```

> `0x3ff73c00` is the only word reporting **exactly** 1000 kHz every time;
> everything else drifts.

Confirmed against an independent clock:

```
tsf advanced 622458 over 622 ms of cpu time -> 1000 kHz
tsf advanced 508686 over 508 ms of cpu time -> 1001 kHz
```

> Agreement to 0.1% over half a second is not something a misread address
> produces. **nat-os now has a 1 MHz timebase**, which it did not have before —
> the kernel tick is 10 ms.

The header records the identification method rather than just the address:

```c
/* The 802.11 TSF timer, identified by behaviour: 0x3ff73c00 is the one word in
 * the MAC window that advances at exactly 1 MHz across repeated samples. */
#define WIFIMAC_TSF_REG   0x3FF73C00u

/* Counts TSF ticks against the kernel's own cycle counter over `ms`
 * milliseconds. Returns the TSF delta; `cycles` receives the cycles elapsed.
 * A genuine 1 MHz counter yields delta ~= ms * 1000 -- a long-interval match
 * no drifting or noisy register can produce by accident. */
uint32_t wifimac_tsf_check(uint32_t ms, uint32_t *cycles);
```

**Two clocks with no connection to each other agreeing to 0.1%** is the strongest
form of evidence available without documentation.

## 27.5 The MAC address, and a better oracle

A six-byte address looks equally plausible in any byte order, so the decode needed
checking against something.

> The first attempt compared the top three bytes against a list of Espressif OUIs
> and reported **failure** for `5c:01:3b:50:3f:64`.
>
> The decode was right. The **list** was wrong. That test was measuring a
> recollection of IEEE registrations, not the hardware.

`efuse.c` records the whole thing:

```c
 * The first attempt checked the top three bytes against a list of Espressif
 * OUIs, on the reasoning that an OUI is a published, assigned value. It
 * reported failure on this board: 5c:01:3b:50:3f:64, no match. The decode was
 * correct and the LIST was wrong -- an OUI assigned after the list was written,
 * or to the module maker rather than to Espressif. The test was measuring my
 * recollection of IEEE registrations, not the hardware.
 *
 * eFuse stores a CRC8 of the address in the same block. Checking against that
 * tests the decode against data on the chip itself: if the bytes are read in
 * the wrong order the CRC cannot match, and no external knowledge is involved.
 * Confirmed here -- 5c:01:3b:50:3f:64 gives 0x08, which is the stored byte,
 * while the reversed order gives 0x8f.
 *
 * The lesson is worth keeping: the better oracle was already on the chip.
```

Note also what the file says about the *rest* of the WiFi work by contrast:

```c
 * Unlike almost everything else in the WiFi path, this is DOCUMENTED. The eFuse
 * controller is in the technical reference manual, the block 0 layout is
 * public, and the decode below is the same one ESP-IDF performs. Nothing here
 * is reverse-engineered.
```

Marking which parts of a subsystem rest on documentation and which on
reverse-engineering is exactly the evidence grading of Chapter 0b, applied at file
scope.

## 27.6 The PHY, and a StoreProhibited that was an ABI requirement

`phyinit.c` is candid about the change in risk:

```c
/* nat-os — bringing Espressif's PHY up. See vendor/phy/README.md.
 *
 * This is the first code in the project that touches the radio. Everything
 * before it — the window handlers, the ROM calls, phy_version_print — was
 * provably inert: it could not affect hardware even if it was wrong. This can.
```

What it needs:

```c
 *   - the WiFi/BT common clock, ungated in DPORT. Without it the PHY's
 *     registers are dead and it hangs waiting on a peripheral that is not
 *     running — the same shape as the LEDC clock gate in UM-NATOS-027 §3.3,
 *     which is the third time that pattern has appeared in this project.
 *   - 128 bytes of initialisation data. This is Espressif's own default table,
 *     copied from phy_init_data.h; the 107 listed values are theirs and the
 *     remaining 21 are the zeros C fills in.
 *   - a calibration buffer it writes into. Normally this is persisted to flash
 *     and reused; here it is .bss, so every run pays for a full calibration.
 *
 * PHY_RF_CAL_FULL is chosen deliberately over PARTIAL or NONE: the other two
 * expect calibration data from a previous run, and there has never been one.
 */
```

### Three theories, and the two wrong ones were the useful part

The blocker was `chip_v7_set_chan_nomac` panicking with `StoreProhibited`.

**1. Stack depth.** The PHY got a private 6 KB stack.

> The fault moved *forward* without going away — which looked like progress and
> was not. Priming the stack with a pattern and reporting its high-water mark
> from the panic handler settled it: **272 bytes of 6144 used.** Depth was never
> the problem, and **without that number the obvious next move was a bigger
> stack, which would also have failed.**

That instrument is in `window.h` with its purpose stated:

```c
/* Fills the PHY stack with a pattern, and reports how much of it has since
 * been touched. The point is to settle a question rather than argue it: when a
 * PHY call dies inside a window-overflow spill, "the stack was too small" and
 * "something else is wrong" look identical from the fault address alone. If
 * the call consumes the whole stack the cause is depth; if it dies having used
 * a fraction of it, depth was never the problem. */
void     phy_stack_prime(void);
uint32_t phy_stack_used(void);
```

The *priming* is essential and its absence produced one of the seven lying
instruments in §27.8: an unprimed `.bss` stack is all zeros, and the fill-pattern
scan reads that as fully consumed — reporting `6144 of 6144 bytes used` in a real
panic, which "looked exactly like a stack exhaustion that had not happened".

**2. Stale `WINDOWSTART` bits.** Declaring exactly one live frame: no change.

**3. The base frame was not a windowed frame.** This was it.

### The one store that fixed it

`_WindowOverflow8` — the handler in this very repository — spills `a4..a7`
relative to the caller's stack pointer, fetched with:

```asm
    l32e    a0, a1, -12             /* a0 <- call[j-1]'s sp */
```

> Every windowed frame must carry its caller's stack pointer at `sp - 12`.
> `ENTRY` does not write it; the *caller's prologue* does. `phy_stack_call` is
> call0 code and wrote nothing there, so the handler read a fresh `.bss` zero and
> spilled to `0 - 32`.

And the reason the fault address was useless:

> That also explains why the reported PC was never where the store was: the spill
> faults *inside* the overflow handler, which runs with `PS.EXCM` set, so the
> second fault vectors to the double-exception handler while `EPC1` still holds
> the instruction that caused the *original* overflow — the `call8`. **Hours were
> spent disassembling the wrong instruction.**

`PS.EXCM` again — Chapter 8's defect from a completely different angle.

The fix in `window.S`, with the whole analysis attached:

```asm
    /* THE BASE FRAME MUST LOOK LIKE A WINDOWED ONE.
     *
     * This is what the StoreProhibited actually was, and it is an ABI
     * requirement that call0 code has no reason to know about.
     *
     * _WindowOverflow8 spills a4..a7 relative to the CALLER'S stack pointer,
     * which it fetches with `l32e a0, a1, -12`. Every windowed frame is
     * required to hold its caller's sp in that slot; ENTRY does not write it,
     * the caller's prologue does. phy_stack_call is call0 and wrote nothing
     * there, so the handler loaded a fresh .bss zero and spilled to 0 - 32.
     * ...
     * Two earlier theories were wrong and are recorded because the measurements
     * that killed them were the useful part: it was not stack DEPTH -- the
     * fault came 272 bytes into 6144 -- and it was not stale WINDOWSTART bits,
     * since declaring a single live frame did not change it either. */
    movi    a9,  _phy_stack_top
    addi    a10, a8, -12            /* s32i takes no negative offset */
    s32i    a9,  a10, 0
```

> The fix is one store. Everything after it worked first try.

### Two more things `phy_stack_call` must do

**Mask interrupts, and not for atomicity:**

```asm
    /* Mask interrupts for the duration.
     *
     * Not for atomicity — to stop a context switch happening while windowed
     * frames are live. nat-os's level-3 handler saves a0..a15 and nothing else:
     * it does not save WINDOWBASE or WINDOWSTART and does not spill the window.
     * A switch in the middle of blob code therefore leaves another task running
     * with this one's frames still marked live in the register file. */
    rsil    a9, 3
```

That is the cost of Chapter 8's minimal context frame, paid here rather than by
enlarging every task switch in the system. A good trade: windowed excursions are
rare and bounded.

**Declare exactly one live frame, and deliberately not restore `WINDOWSTART`:**

```asm
     * WINDOWSTART is deliberately NOT restored afterwards. The bits describe
     * frames whose registers this excursion has already overwritten, so
     * putting them back would re-mark frames whose contents are gone. A call0
     * kernel's correct steady state is one live frame, and that is what this
     * leaves behind. */
```

## 27.7 It receives

```
802.11 frame: BEACON  len=348
bssid 44:25:38:19:0d:1a   ssid "TC7NR"
```

> A real beacon from a real access point, on a kernel built `-mabi=call0` running
> Espressif's PHY blob through hand-written window handlers.

And then continuous reception, via descriptor recycling:

```
frames=372  recycled=374  networks=2
   44:25:38:19:0d:1a  x199  "TC7NR"
   38:88:71:2d:c1:cd   x53  "Verizon_S6QHX4"
```

> With four buffers, 372 frames can only come from reuse.

A four-buffer pool and 372 frames is a *derived* proof of recycling — the same
style of arithmetic check as Chapter 14 §14.8's instruction accounting.

> It also kept receiving *through* a 3D rendering session — the radio and the
> display share nothing but the scheduler, and neither starved.

## 27.8 Transmit, and the difference between "sent" and "sent"

The MAC accepts frames and reports every one complete:

```
tx handed to hardware=178  completions reaped=178   forced=0
```

> This report initially described a rising completion count as "the MAC saying
> the frame actually went out". That was an overclaim, and the user checked their
> phone for the beacon SSID, and it was not there.

### The test that settles it

> A **probe request**. A beacon is a statement and nothing has to answer it, so
> silence proves nothing. A broadcast probe request with a wildcard SSID obliges
> every access point in range to send a probe *response* addressed to this
> station — and one arriving is **unforgeable**, because a radio cannot hear
> itself and nobody sends to this MAC without having heard from it first.

```
sent 20 probe requests
completions reaped=20
frames addressed to us=0
```

> Twenty frames the hardware called complete, zero answers from two access points
> loud enough that we receive them continuously. **A completion bit says the
> frame left the queue, not that it left the antenna.**

This is the best-designed experiment in the book. It converts an unfalsifiable
claim ("did the frame transmit?") into a falsifiable one ("did anything answer?"),
using a response that cannot be produced by any local fault.

### Two causes eliminated by measurement

> - **Transmit power.** `most_tpw` already reads `0x28` — 40 quarter-dBm,
>   **10 dBm** — straight out of `register_chipv7_phy`. Never zero.
>   `phy_set_most_tpw(78)` returns success and changes nothing.
> - **The MAC hardware init chain.** `ic_mac_init`, `hal_init`, `ic_enable_rx`
>   and `hal_mac_tsf_reset` all run without a crash. Still zero answers.

### Two pieces of good news from a failed hypothesis

**The IRAM panic was unfounded** — Chapter 4 §4.8's 2,459 bytes against a
believed 48 KB. A constraint that had shaped decisions for weeks did not exist.

**The blob runs.**

> This was the first time nat-os executed libpp at all, and the OSI table, the
> window handlers and the mixed-ABI bridges all held up under the code they were
> built for. **That was never certain.**

### And an expensive lesson about linking

> **naming a symbol changes the link.** Referencing `ram_tx_pwctrl_bg_init`,
> which was not previously linked, pulled fresh objects out of `libphy.a`, after
> which `register_chipv7_phy` died with IllegalInstruction inside
> `set_rx_gain_testchip_70` — a calibration function nobody had touched and which
> had worked for days.
>
> Rule adopted: **reference only what open-mac references**, and check with `nm`
> that a symbol is already in the image before calling it.

### The strongest untried lead

> `periph_module_reset(0x19)`. nat-os has only ever *ungated* the WiFi
> peripheral, never *reset* it, and a MAC left in whatever state the ROM
> bootloader put it in would plausibly receive while refusing to transmit. **That
> asymmetry fits the symptom exactly.**

## 27.9 The `task_sleep` detour, which was not a detour

Covered in Chapter 9 §9.4. Summarised here because of where it was found:

> Mid-WiFi, the TSF check reported ~1 µs for a requested 500 ms sleep. **Two
> independent clocks agreed on that**, so it was not a measurement artefact.

Two bugs, stacked: `timer_ticks()` counted ISR entries rather than time (217/s
where 100 is correct), and `task_yield()` arms a switch rather than performing
one (`task_sleep(50)` returning in 107 cycles).

Why it mattered *here* specifically:

> every OSI queue and semaphore timeout is built on `task_sleep`, so anything the
> blob did with a timeout was quietly unreliable.

And the general form:

> **nothing showed a symptom.** Sleeps ran short, timeouts ran loose, and every
> frame-rate figure this project has ever recorded was measured against a clock
> running fast by a load-dependent factor.

It was found because the WiFi work introduced the first *independent* clock the
kernel had ever had. A second clock is the only thing that can catch a first one
lying.

## 27.10 The touchscreen gets worse, twice

### A background task outranking the user

> The WiFi receive task was raised to `HIGH` to get beacons from 3 Hz to 9.4 Hz.
> The raycaster blit was checked, found unchanged, and the change declared a
> clean win.
>
> The touch task is `NORMAL`. With **two** flat-out `HIGH` tasks instead of one
> it stopped being scheduled except when ageing rescued it — roughly every
> 300 ms. The panel went from responsive to intermittent, **which no counter in
> this kernel reports** and which the user noticed within minutes.

And the merits, reassessed:

> It was also a bad trade on the merits: beacon timing was prioritised over input
> latency, and **the beacons were not reaching the air anyway**, so the 9.4 Hz was
> entirely theoretical.

The useful measurement was the one that ruled out the obvious suspect:

> `update_rx_chain()` spins on a hardware acknowledge and looked exactly like the
> cost; instrumenting it showed a worst-case wait of **one** iteration. It was
> never the radio, it was the scheduler.

### A "restoration" that was not

Reverting the priority was not enough. The cause was an *earlier* change
described in a commit message as a "restoration" — the `task_yield()` for
`task_sleep(1)` substitution of Chapter 9 §9.4.

> Fixing `task_sleep` had, with perfect irony, broken the thing that was
> accidentally relying on it being broken.

| | before | after |
|---|---|---|
| touch samples per reporter interval | ~15 | **~150** |

## 27.11 The 3D view, and four confident wrong answers

Then the 3D view started tearing, and the close button vanished.

**Theory one: DMA had fallen back to the FIFO path.** The reasoning was sound
(Chapter 18 §18.9 is that reasoning, and it is genuinely a real defect). The
counter said `dma=320/0`. **Zero timeouts. Ever.** The bound was raised anyway on
principle, then restored when it fixed nothing.

**Theory two: touch preempting the display mid-transfer.** And here the method
changed:

> Instead of one flash per guess, both knobs were made runtime-tunable
> (`touchcfg <prio> <sleep>`), and all four combinations tested in a single
> build:
>
> ```
> NORMAL + yield   31399 31332 31394
> HIGH   + yield   31352 31398 31353
> NORMAL + sleep1  31399 31346 31385
> HIGH   + sleep1  31424 31387 31438
> ```
>
> Identical. Touch scheduling does not move it. **This is the single best thing
> that happened during the display investigation, and it should have happened
> three rounds earlier.**

**Theory three: applications drawing over the view.** `ps` showed `ping` and
`pong` running and drawing, with `drawskip` climbing. Killed all three
applications. `drawskip` froze. No change on screen.

**Theory four: the panel clock.** The driver has documented since Chapter 18 that
clocking this panel too fast puts visible noise on the glass *while every counter
reports success* — "a suspiciously exact description of the symptom". A runtime
clock selector was added and the panel dropped from 40 MHz to 10 MHz. Different.
Still wrong.

### The misleading evidence, and the user who was right

> At this point the investigation had a genuinely misleading piece of evidence:
> the same commit rendered correctly earlier in the day and torn later, with no
> code change in between. That reads as hardware. A reasonable-sounding case was
> made for a marginal display flex and a sagging USB rail, and the user was asked
> to reseat connectors.
>
> The user replied, in effect, *no, it is a bug in the code, because the X button
> is also missing*.
>
> **They were right.** What differed between the two runs was not the hardware.
> It was which application slots happened to be occupied.

Third instance in this book of a human observation outranking the instruments.

### The test that should have been first

> A **static test pattern** through the raycaster's exact blit call: same buffer,
> same width, same stride, same contiguous path. Six vertical colour bars, plus a
> white block in the top-right corner where the close button goes.
>
> This splits one question into two, and the two have completely different
> answers:
>
> - If the pattern is sheared or torn → the **transport** is broken.
> - If the pattern is clean but something is missing → the content is fine and
>   something **overwrites it afterwards**.
>
> Result: **clean bars, and a hole exactly where the white block had been
> written.**

### The actual bug

`desktop_chrome()` repaints a right-hand column every frame, one strip per
application slot — a name and a red X for running slots, and **solid black** for
empty ones:

```c
display_fill_rect(CHROME_X, y, APP_CHROME_W, APP_VIEW_H, COLOR_BLACK);
```

> That column is documented as reserved *outside every application viewport*,
> which is entirely true — for applications, whose viewports are narrower.
>
> **The 3D view is not an application.** `RAY_VIEW_W` is 240. `DISP_W` is 240.
> The view owns the whole width, including that column, and the chrome runs
> immediately after `raycast_frame()`, painting over the right edge of a view
> that had just drawn it.

One cause, every symptom:

> - the tearing along the right edge
> - the missing close button
> - the vanished white test block, in precisely that corner
> - and why killing the applications made it **worse** — an empty slot fills the
>   column with solid black, while a running one at least draws text
>
> That last point is also why "the same binary behaved differently" was so
> convincing and so wrong. **The binary was identical. The application slots were
> not.**

Compare Chapter 8 §8.7, where one cause explained six observations. The
difference between a correct diagnosis and a plausible one is whether it accounts
for the observations that looked *arbitrary*.

Chapter 24 §24.11 covers the fix and its subsequent narrowing to a compile-time
assertion.

## 27.12 The seven instruments that lied

> This is the part worth keeping. Every one of these cost real time.

1. **The blit timer.** `task_cpu_cycles()` only advances at context switches, so
   the figure tracks switches, not work. A 55.85 → 31.3 ms shift was read as a
   serious regression, and reported as a 44% *improvement* an hour earlier. "It
   was an artifact both times."
2. **`timer_ticks()`.** Counted ISR entries rather than elapsed time. Silent for
   days.
3. **The transmit completion bit.** Reports that a frame left the *queue*. "A
   phone was the instrument that caught it."
4. **The OUI list.** Declared a correct MAC decode invalid. "The chip's own CRC8
   was sitting right there."
5. **The PHY stack high-water mark.** Reported `6144 of 6144 bytes used` — not a
   large number but a *missing measurement*, because an unprimed `.bss` stack is
   all zeros.
6. **`raycast_framebuffer()`.** Returns a boolean. Named like an accessor.
   Panicked a diagnostic that trusted the name.
7. **`fb=on` in the reporter.** Reports the framebuffer *mode*, not whether a
   buffer exists.

The pattern:

> **the counters were all clean while the picture was visibly wrong.**
> `corrupt=0`, `dma=320/0`, no timeouts, no skew, no lock contention worth
> mentioning. Every automated check this kernel has said everything was fine, and
> every one of them was telling the truth about the narrow thing it measured.
>
> **The person looking at the screen was the only instrument that could see the
> actual fault**, and twice in this session that person was right while the
> counters and the reasoning were wrong.

## 27.13 Three method notes worth repeating deliberately

**Make the variable runtime-tunable before forming a theory about it.**

> `touchcfg` turned four hypotheses into one flash. `spiclk` did the same for the
> panel clock. Both were written *after* several reflash-per-guess rounds that
> they would have eliminated.

The same technique as the flash divider sweep in Chapter 20 §20.6 — sixteen
measurements, one flash cycle — and as the interrupt-enable bit sweep in
Chapter 22 §22.5.

**When something looks corrupted, draw a static test pattern first.**

> It separates "the content is wrong" from "something overwrites it afterwards"
> in a single step, and those two have disjoint suspect lists. This was the fifth
> thing tried and should have been the first.

**Prefer an oracle that lives on the chip.**

> The eFuse CRC beat a hand-maintained OUI list. Two independent clocks agreeing
> beat one clock asserting. A hardware acknowledge bit clearing beat a register
> reading back what was written.

And a process note:

> when a user says *it worked a minute ago on this exact build*, that is data,
> but it is not automatically evidence of hardware. It is equally evidence that
> **something in the run-time state differs** — in this case, which application
> slots were full. **Enumerate that state before reaching for the screwdriver.**

## 27.14 The open fault, and the measurement that cracked it open

**Symptom.** Opening the 3D view sometimes shows a wrong picture for ten to
thirty seconds, then it comes good on its own.

**Everything ruled out**, each by direct measurement rather than argument:

| theory | how it died |
|---|---|
| DMA fell back to the FIFO path | `dma=320/0` — the guard has never fired |
| Touch scheduling | identical blit across all four priority/poll combinations |
| Applications overdrawing the view | killed them all; `drawskip` froze; no change |
| Panel signal integrity | clock 40 → 10 MHz changes appearance, does not fix |
| The chrome column | `APP_VIEW_Y0` 224 == `RAY_VIEW_H` 224; they never overlap |
| Movement catch-up burst | fixed by `raycast_open()`; symptom unchanged |
| Touch calibration | was genuinely broken and is fixed; symptom unchanged |
| An arena overlapping the framebuffer | `fb 0x3ffbcd70..0x3ffd7170`, arenas from `0x3ffd7590` — clear |

**The strangest observation:** opening `gfxrogue` *repairs* the view.

> If a second program starting fixes the first, the view is missing an
> initialisation step that an application start happens to perform, rather than
> being actively corrupted by anything.

### A contaminated diagnostic

> The blittest that produced [one] theory was also contaminated: it called
> `desktop_set_active(1)` to stop the renderer overwriting the pattern, which put
> the launcher into repaint mode and erased the test block itself. **A diagnostic
> that changes the state it is measuring is worse than no diagnostic**, and this
> one was believed for several rounds.

### The measurement that finally said something

> The obvious experiment — compare the framebuffer before and after a launch —
> does not work as stated. The raycaster rewrites **every pixel** of the buffer
> every frame as the camera moves, so any two samples differ for entirely
> innocent reasons. **A comparison whose result is "different" no matter what is
> not a measurement.**

So the renderer is frozen first (`dfreeze`, Chapter 25 §25.11):

```
BEFORE launch    equal-to-first=25088   rows 67e0 67e0 67e0 67e0
CONTROL          equal-to-first=25088   rows 67e0 67e0 67e0 67e0
AFTER launch     equal-to-first=25088   rows 67e0 67e0 67e0 67e0
```

> The **control** row is the load-bearing one. Taken with nothing done in
> between, it proves the freeze holds the buffer still and the sampler is
> stable — without it, "no change" would be indistinguishable from a broken
> measurement. **That is the same lesson as the contaminated blittest above,
> applied in advance for once.**

> **Launching a program does not alter one sampled byte of the framebuffer.**
>
> This is the first positive result in the entire investigation, and it
> eliminates more than the previous eight negatives combined: the renderer, the
> framebuffer contents, the heap, and all five of `app_start()`'s operations.
> **Whatever the repair is, it happens downstream of the image.**

### A mechanism that fits

> The ILI9341 holds a **window**, set by `set_window()`, and pixel data streams
> into it with CS asserted. If a stream ever ends short, or CS is left asserted,
> the controller sits mid-window and the *next* pixels land at the wrong offset.
> That is what a garbled image is. Any other drawer issuing a fresh
> `set_window()` and `push_end()` **resynchronises the controller** — and
> applications draw constantly, while the raycaster alone only ever writes one
> full-screen window per frame.
>
> That accounts for every observation, including the one that looked like magic:
> code which never touches the image can repair the image, because it is not
> repairing the image — **it is repairing the panel's idea of where the image
> goes.**

And it retires an earlier claim:

> "Transport is clean, proven with colour bars" tested a single blit **from a
> fresh state**. It did not test a blit issued after a previous one may have left
> the controller mid-window, which is a different question and the one that
> matters.

**Status: untested.** *"It is a hypothesis with a mechanism, which is more than
the previous eight had."* `display_resync()` (Chapter 18 §18.10) is the
instrument built to settle it.

## 27.15 Where things ended up

| | |
|---|---|
| **3D view** | fixed, close button present, `corrupt=0` |
| **Touch** | 100 Hz at HIGH priority, ~10× the previous sample rate, default |
| **WiFi receive** | working — continuous scanning, beacon decode, descriptor recycling |
| **WiFi transmit** | MAC accepts and completes frames; nothing hears them |
| **`task_sleep`** | actually sleeps, for the first time |
| **`timer_ticks()`** | actually counts time, for the first time |
| **IRAM budget** | not a real constraint; the 48 KB figure was a measurement error |

## 27.16 Metrics

| Quantity | Value |
|---|---|
| OSI table entries | 116 declared, 39 implemented |
| OSI pools | 12 semaphores, 8 queues, 4 event groups, 12 timers |
| Window vectors implemented | 6 |
| MAC moving words: cold / after phy / after mac | 0 / 6 / 13–15 |
| TSF rate, two independent measurements | 1000 kHz / 1001 kHz |
| MAC address | `5c:01:3b:50:3f:64`, CRC8 `0x08` confirmed |
| PHY stack | 6 KB private, **272 B used** |
| PHY init data | 128 B, Espressif's default table |
| PHY calibration buffer | 1,904 B in `.bss`, full calibration every boot |
| Frames received | 372 across 4 buffers, 374 recycled |
| Networks decoded | 2 |
| Frames transmitted / completed / answered | 178 / 178 / **0** |
| Probe requests sent / responses received | 20 / **0** |
| IRAM cost of the MAC init chain | 2,459 B (not 48 KB) |
| Text / IRAM | 119,151 of 131,072 |
| Instruments caught lying | 7 |
| Display theories eliminated by measurement | 8 |

## 27.17 What this does not establish

- **Transmit does not reach the air.** The single largest open item in the
  project, with a named strongest lead (`periph_module_reset`).
- **No association, no encryption, no IP.** This is a raw 802.11 receiver, not a
  network stack. There is no DHCP, no ARP, no TCP, nothing above the MAC frame.
- **77 of 116 OSI entries are still stubs.** They are the ones the MAC has not
  asked for yet, which is a statement about the code path exercised so far rather
  than about the table.
- **The MAC register map is reverse-engineered** except where §27.5 notes
  otherwise. Every address other than eFuse and DPORT came from open-mac or from
  behavioural identification.
- **No power management.** The PHY runs a full calibration every boot because
  there is nowhere to persist the result — the flash record exists (Chapter 20)
  and has not been extended to hold 1,904 bytes.
- **The 3D startup glitch is open**, §27.14.
- **The DMA stall is open**, Chapter 18 §18.9.

---

**Part V ends here.** The device is complete as a device: it boots, schedules,
isolates, draws, listens to a finger, remembers, makes a sound, and hears the
networks around it.

**Part VI** is what the project learned about how to know any of that is true.
