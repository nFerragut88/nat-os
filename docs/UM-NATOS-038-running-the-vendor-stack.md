# UM-NATOS-038 — Running the Vendor Stack from Flash

**Used Medias LLC — Embedded Systems Division**
Revision 1.2 · 2026-08-20 · Status: **Blocked on one kernel limitation. Transmit path runs; nothing on air.**

---

## 1. Abstract

`next_moves/08` proposed linking Espressif's 802.11 stack, delivering it at
runtime, and calling `esp_wifi_80211_tx` rather than reconstructing transmit by
hand. It called the route "large" and blocked on two claims: that the archive
does not fit, and that one object needs 66 undefined symbols.

Both were wrong once measured. The size objection died with UM-NATOS-037's IROM
window — 234 KB of archive against 3.3 MB of address space. The dependency
figure counted references without asking which resolve internally; the true
external surface is **ten symbols**.

What now exists: a **606 KB pre-linked vendor image**, held in a reserved flash
partition, mapped into three separate windows at runtime, its `.data` copied and
`.bss` zeroed by a loader that verifies its own work. Its PHY initialises. ESP-IDF's
WiFi **driver** initialisation runs, allocates through nat-os's heap and
mutexes, and returns an ordinary `esp_err_t`.

**The kernel image still contains no Espressif code.** The blob is installed
separately and the repository holds none of it.

Nothing has been transmitted.

---

## 2. What was built

| piece | where |
|---|---|
| flash + address reservations | `kernel/flash.h`, `kernel/linker.ld` |
| pre-link and image build | `vendor/net80211/blob.ld`, `build_blob.ps1` |
| self-describing entry table | `vendor/net80211/blob_entry.c` |
| map / load / verify | `kernel/blob.c` |
| PHY bring-up at a runtime address | `kernel/phyinit.c`, `phyinit_run_at()` |
| OS adapter table (118 entries) | `vendor/windowed/wifi_osi_stubs.c` |
| driver config | `kernel/wifi_init_cfg.c` |

The image is linked **once, at build time, to fixed addresses**. That is the
decision the rest follows from: with no relocation to process, the kernel needs
no runtime linker, no symbol table and no ELF loader. Installing reduces to
read, write, map, call.

---

## 3. Three windows, because one is not enough

The reservation is not a single region. It is three, and each is required for a
different reason.

```
code    flash 0x220000        -> vaddr 0x40300000   MMU offset 64  (IRAM0_CACHE)
rodata  flash 0x2A0000        -> vaddr 0x3F700000   MMU offset  0  (DROM0)
data                             DRAM  0x3FFD4000   copied, not mapped
```

**Code** goes to the instruction window. Obvious, and the only part the original
plan anticipated.

**Read-only data must go somewhere else.** The instruction window serves 32-bit
aligned accesses only, and vendor code reads bytes out of its own `.rodata`
constantly. Leaving it in the code window produced `LoadStoreError` at
`excvaddr 0x4037bd0c` — inside `.rodata` — on the first real call. This is
precisely why ESP-IDF splits flash into IROM and DROM, and `boot/boot.c` had
already been doing it for the kernel's own `.rodata` since UM-NATOS-037.

**Writable data cannot be mapped at all.** The MMU maps flash; it does not
populate RAM. `.data` initialisers ship inside the image at a load address in
the code window and are **copied** to DRAM at install time, and `.bss` is
zeroed. The blob's own entry table carries the addresses, because the blob is
the thing that knows them.

The DRAM reservation is the only one that costs anything real: 32 KB against a
measured 20,720 B need, taken directly from the heap, which is whatever lies
between `_bss_end` and the stack.

```
free heap  122,064 -> 88,784 B     (-33,280, exactly the reservation)
```

The flash and IROM carve-outs are free — the kernel uses 21 KB of a 2.3 MB
window.

---

## 4. The image describes itself

`blob_entry` sits at offset 0 and is found at `BLOB_IROM_ADDR` without parsing
anything. It carries magic, version, and **the addresses the loader needs**:
where to copy `.data` from and to, what range to zero, and the callable entry
points.

`build_blob.ps1` refuses to emit an image whose length disagrees with the size
its own header declares. That check exists because an `objcopy` section list
that is subtly wrong produces a file that links, passes a size check, loads, and
**verifies** — while the loader copies `.data` out of zeros. A blob that almost
works is worse than one that does not.

The loader verifies the whole range rather than a sample:

```
verify    .data 1030 words, 0 mismatched;  .bss 4152 words, 0 non-zero
LOAD VERIFIED
```

A copy loop that returns is not evidence it copied anything, and one matching
word would also match a loop that ran exactly once.

**Mapping and calling are deliberately separate.** `blob map` stops after
validation. If a mapping is wrong, a call jumps into whatever bytes are there
and the board dies with nothing to read; every number above is obtainable
before anything executes.

---

## 5. Eight defects, each with the measurement that found it

**5.1 A byte copy from instruction memory.** `LoadStoreError` at `0x4008187f`.
`data_lma` points into the flash-mapped *instruction* region, which serves
32-bit aligned accesses only. `boot.c` had paid for the same class the same day
from the other direction — storing bytes *into* IRAM — and needed a DRAM bounce
buffer. **If either end of a copy is instruction memory, it is a word copy.**

**5.2 357,940 bytes of code missing from the image.** `blob.ld` named `.text`;
the archives also ship `.iram1`, `.phyiram` and six `.wifi*iram` classes, and
`objcopy --only-section` dropped every one. The image linked, passed its size
check, loaded and verified — and the region read back as **zeros**:
`IllegalInstruction` at `0x4036e728`, inside the hole. Fixed with glob patterns
(they arrive as `.wifi0iram.37`, so bare names match nothing), but the actual
guard is **`-Wl,--orphan-handling=error`**, which makes the linker refuse to
place a section nobody named. It caught `.rodata_wlog_warning.59` immediately.

**5.3 `.rodata` in the instruction window.** §3. Found only because `excvaddr`
was added to the panic report — four lines, and it turned a guess into a
measurement. Without it a `LoadStoreError` inside a blob is a bare `epc` that
cannot distinguish a byte access to rodata from a wild pointer.

**5.4 An unaligned `.data` load address.** `.rodata` ends on a byte boundary —
it is full of byte-granular `wlog` strings — so `.data`'s LMA was unaligned and
the loader's word-copy guard rejected the image with `rc=7`. The guard had been
written the previous day "in case a separately built image stops being aligned"
and earned its place within a day.

**5.5 call0 stubs called with `CALL8`.** The generated OS adapter entries were
ordinary kernel functions; the blob calls them windowed. `IllegalInstruction` at
**`epc 0x803014fd`** — bit 31 set, which is not a code address but a windowed
*return-address encoding*, `(2<<30)|offset`, jumped to raw. The window rotated
forward on entry, the callee used the rotated registers as its own, and its
`RET` did not rotate back. `window.S` records the identical fault from the first
time this project hit it. Fixed by splitting the file along the ABI boundary:
stubs and table windowed in `vendor/windowed/`, accessors call0 in `kernel/`,
reading the counters as **data** — data has no calling convention.

**5.6 The in-tree OS adapter table was stale.** `vendor/windowed/wifi_osi.c`
already held a 118-entry table. Handing it over failed with
`ESP_ERR_INVALID_ARG` and **zero forwarded calls**. Its member order diverges
from the current header from index 54 onward, 63 positions differing — it still
has `_phy_common_clock_enable/_disable` where the header now has
`_phy_update_country_info/_read_mac`. Both are 118 entries, so size gives
nothing away. Both files' comments warn that "one member out of position is a
call to the wrong function with the wrong arguments, and nothing in the system
would diagnose it." **The blob diagnosed it**, by checking `_magic` at the
offset it expects.

**5.7 The MMU reprogrammed with the cache live.** `blob_map()` wrote the entries
with the cache enabled and flushed afterwards. Espressif's `spi_flash_mmap` does
disable → write → flush → enable, and `boot.c` gets that order for free because
at boot the cache is still off. Corrected. Whether this was also the unexplained
hang in §7.2 is unproven and probably not directly provable.

---

**5.8 The init config was four bytes too long.** `esp_wifi_init_internal`
returned `ESP_ERR_INVALID_ARG` — an error that, unlike `NO_MEM`, names nothing.
Nineteen config fields remained unbisected and every hypothesis was wrong.

The answer came from `tools/idf_ref`, a board running real ESP-IDF, made to
print its own `wifi_init_config_t` with offsets:

```
reference: sizeof=216   static_rx_buf_num off=120   feature_caps 192   magic 208
nat-os:    sizeof=224   static_rx_buf_num off=124   feature_caps 200   magic 216
```

`wpa_crypto_funcs` is embedded **by value** and had 120 bytes reserved for it,
counted by hand from the header. It is **116** — 29 members, not 30. One extra
word pushed every following field 4 bytes late and added 4 bytes of tail
padding, so the driver read `magic` at offset 208 where nat-os held
`sta_disconnected_pm` = 0.

**Every value in the struct was correct and every one was in the wrong place.**
That is why it survived so long: nothing about the values could be wrong, so
the values kept being checked. The reference also supplied two genuinely wrong
values without being asked — `feature_caps` is `0xa1`, not 0, and
`sta_disconnected_pm` is 1.

Fixed, and the error immediately became self-locating again:
`INVALID_ARG` -> `NO_MEM`, 8 adapter entries -> 11, sequence 14 calls -> 18.

**The method matters more than the bug.** A second board answering a question
directly beat both static analysis and field-by-field bisection, and
UM-NATOS-034 had already recorded the same lesson once. Any question of the
form "what value does the vendor stack expect here" can be asked of COM6
instead of guessed.

## 6. The adapter names its own requirements

118 entries; nat-os has 24 primitives. Rather than guess which a bring-up needs,
every entry was generated as an **instrumented stub** that records being called
and returns something safe. `osiused` then reports which were reached, in order,
with counts.

The driver named its requirements one run at a time:

| run | result | asked for |
|---|---|---|
| 1 | `NO_MEM` | `_recursive_mutex_create` |
| 2 | `NO_MEM` | `_task_get_current_task`, `_calloc_internal` |
| 3 | `NO_MEM` | `_spin_lock_create` |
| 4 | `INVALID_ARG` | — resources satisfied |

`NO_MEM` is a **self-locating** error: it arrives with the trace of the entry
that had just returned NULL. Eight entries were implemented on that basis, and
every entry that never appears is work nobody has to do.

The table order is generated from Espressif's header rather than typed, and the
header is `#if`'d per target: on ESP32 `_phy_common_clock_enable/_disable` **are**
members and two others are not. A first pass ignored the conditionals and
produced 123; the correct layout is **118 entries, 472 bytes**. §5.6 is what that
mistake looks like when it ships.

---

## 7. Verification, and what this does not establish

### 7.1 Verified on hardware

```
boot            11/11 self-tests PASS
blob            LOAD VERIFIED (1030 data words, 4152 bss words)
blobphy         phyinit rc=0, phystack 1296 of 6144 bytes used
wifiinit        esp_wifi_init_internal returns an esp_err_t; 8 adapter
                entries called, create/delete correctly paired
```

The PHY runs **under nat-os's own bootloader from a kernel containing no
Espressif code** — something the `-WiFi` build has never managed. UM-NATOS-036
gated that build to Espressif's second stage because `register_chipv7_phy`
panicked in `phy_enter_critical` under ours. That gate covers the copy the
kernel *links*; this is the blob's copy, and the failure it describes did not
occur. **Whether the gate can now be lifted has not been tested.**

The 1296-byte stack figure is the load-bearing number, not `rc=0` — an early
exit would also return zero.

### 7.2 What this does not establish

- **Nothing has been on air.** No frame has been transmitted, and
  `esp_wifi_start` has never been called.
- **`esp_wifi_init_internal` does not yet succeed**, but it no longer fails on
  its argument. See §5.8 — the `INVALID_ARG` was a struct-layout bug and is
  fixed. It now returns `ESP_ERR_NO_MEM` and names the resource it wants.
- **One hang remains unexplained.** A narrow band of kernel layouts caused
  `blob_map()` to hang deterministically. Everything software-visible was
  eliminated — `.bss` position, section sizes, symbol addresses, instruction
  sequence and liveness across the call were all identical. It could not be
  instrumented: any probe changed the layout it was observing. §5.7 may or may
  not have been its cause.
- **The `*iram` sections are in flash — measured, and closed by clamping.**
  53.7 KB of `.wifi0iram`, `.iram1`, `.phyiram` and friends live in the code
  window rather than RAM. Espressif keeps them in RAM because it leaves
  interrupts **enabled** during flash writes; nat-os made the opposite trade and
  masks to `CRIT_LEVEL` across the whole erase, so nothing at or below that
  level runs and nothing fetches.

  The hole was an interrupt allocated **above** `CRIT_LEVEL`. `_set_intr` now
  clamps to it and counts every time it has to, so a driver asking for a
  higher-priority interrupt is visible rather than silently unsafe. The cost is
  up to 125 ms of ISR delay during an erase — the `next_moves/04` problem,
  already answered by write policy.

  A fourth window was measured as the alternative: 53.7 KB against 86.4 KB of
  free IRAM, so it **does** fit. Not spent, because masking already covers it.
  If a WiFi ISR ever genuinely needs to run above `CRIT_LEVEL`, the clamp
  counter is what will say so, and the window is the answer.

  The counter is **exercised, not merely present** — `osiclamp` calls
  `_set_intr` through `rom_call4` (it is windowed) asking for priority 7, then
  for 2:

  ```
  clamped count: 0 -> 1 -> 1
  PASS - clamps above CRIT_LEVEL, leaves the rest alone
  ```

  The second half is the control. Without it, a clamp that fired on *every*
  call would also show a rising count and read as working. And the counter's
  reading of 0 during `wifiinit` means only that `_set_intr` has not been
  reached yet — init fails before interrupt setup — not that the clamp has been
  shown to be unnecessary.
- **Blocking OS calls** were fixed after this was written: `blob_call()` enters
  the blob holding a mutex instead of masking interrupts, so the scheduler
  keeps running. See §10.

- **One blocker remains, and it is a kernel limitation rather than missing
  work.** See §10.

---

## 8. Rules earned

**An unstated precondition is a defect even when every caller satisfies it.**
The word-copy alignment guard, written speculatively, rejected a real image the
next day.

**A count of absent errors is not a result.** A link that reported "0 undefined
references" had in fact failed for an unrelated reason and the grep matched
nothing. The figure became trustworthy only after checking the ELF existed and
contained the symbol.

**A probe whose size does not change is not a probe.** A padding section that
advanced the location counter without emitting bytes shifted every load address
while leaving the image byte count identical, producing a corrupt blob and a
confident, meaningless measurement.

**Prefer the error that locates itself.** `NO_MEM` from an honest stub names the
next piece of work. A stub that returns success instead is indistinguishable
from a working implementation until the radio silently does nothing.

**Check a hand-written register sequence against the verified one, not against
memory.** §5.7's bit *definitions* were right and the *order* was wrong; a
register read-back would have shown nothing either way.

**Reaching for the heaviest instrument can rule out the whole approach.**
JTAG was proposed for §7.2's hang and its absence treated as the wall — the same
error UM-NATOS-034 §12.2 had already recorded. The cheap check found a real
defect instead.

---

## 9. Related

- [UM-NATOS-037](UM-NATOS-037-code-in-flash.md) — the IROM window this rests on
- [UM-NATOS-036](UM-NATOS-036-the-half-speed-board.md) — the `-WiFi` bootloader gate
- [UM-NATOS-034](UM-NATOS-034-the-second-receiver.md) §31 — the transmit-path correction that motivated this route
- `docs/next_moves/08-wifi-via-loaded-blob.md` — the running log, including the failed hunts
- `docs/blob-free.md` — why function and independence are different purchases

---

## 10. Where it stopped, and why (rev 1.2)

Everything after §7 was established later and changes the picture.

### 10.1 What was finished

- **The config was a layout bug**, not a value — `wpa_crypto_funcs` reserved
  120 bytes where it is 116, so every field after it sat 4 bytes late and
  `magic` was read from `sta_disconnected_pm`. Found by asking a board running
  real ESP-IDF to print its own offsets. §5.8.
- **Fifteen adapter entries implemented** — mutexes, memory, spin locks,
  interrupt masking, queues, semaphores, task creation — each one named by the
  driver on the previous run.
- **`blob_call()`** enters the blob under a mutex with the scheduler live,
  replacing the masked-interrupt model, so blob code that blocks can progress.
- **The transmit path runs.** With the adapter table installed,
  `esp_wifi_80211_tx` executes its frame sanity check, takes the global lock
  through nat-os's adapter, and returns `ESP_ERR_WIFI_IF` — a specific,
  correct refusal, because no interface is configured.

### 10.2 The single blocker

A vendor driver owns a task, and that task runs windowed code concurrently with
API calls. **nat-os cannot host two contexts inside windowed code at once.**
`kernel/wincollide.c` reproduces the corruption in two seconds with two
ordinary tasks and no blob involved.

Four independent paths end there: init needs a task; the task collides;
transmit needs a started driver; start needs init.

**Five attempts at the fix failed**, all writing window state inside
`_handler_level3`. Spilling in *task* context works and is in the tree; the
remaining parts — saving `WINDOWBASE`, narrowing `WINDOWSTART` — break the
single-task case wherever they are placed in the handler. The two best
explanations (window exceptions live because `PS.EXCM` is cleared; illegal
intermediate window state) were both tested and eliminated.

This is **not** an ABI problem. The hardware spills correctly to each frame's
recorded stack pointer. What is left is ISA behaviour that needs single-stepping
to observe, which means a debug probe.

### 10.3 A correction about method

Four experiments in this work produced confident conclusions from shell
commands that **never executed** — `split()` truncates the line at the first
space, so every two-word command tested a string that could not match. Two of
those conclusions were used to overturn *correct* earlier findings, which had
to be reinstated.

The rule this earns: **a negative result from a command whose execution was
never confirmed is not a negative result.** The project already had the same
rule one level up, about instruments sharing a dependency with the fault. Here
the instrument was the shell.

---

## 11. Concurrency, and a fault that took twelve steps to name (rev 1.3)

Rev 1.2 closed with the vendor stack running single-threaded and the conclusion
that hosting a driver which owns a task needed per-task `WINDOWBASE`/
`WINDOWSTART`. That conclusion was half right, and the half that was wrong was
found by asking a narrower question: *which* windowed regions can actually block?

### 11.1 The blocking set is closed

Every blocking entry in the adapter funnels through one function, `wait_on()`.
Six entries, enumerable rather than open-ended, and all but `_task_delay` now
take the non-blocking path first and only spill-and-release when genuinely about
to wait. A wait costs a spill; a hit costs nothing.

### 11.2 Exclusion, not preemption

`rom_call3` recorded a pin but took no lock, so a second caller simply overwrote
the first and both entered windowed code. With a real lock:

```
wintorture 60   checksum 1632 = 1632 CORRECT, 6 real preemptions
wincollide      runs=156  wrong=0
```

`wincollide` had reproduced corruption in ~2 s since step 13. The spill inside
`rom_call3` turned out to be the *wedge* — it compensated for the absence of
exclusion, and once the lock existed it could not help, only hang. Removing one
`call8` turned a watchdog reset into 156 clean runs.

### 11.3 Resource limits, enforced rather than counted

`esp_wifi_init_internal` asks for **6656 bytes** of task stack; nat-os gave every
task 2048 and *counted* the shortfall while creating the task anyway. Scaling the
pool would cost 78 KB against an 84 KB heap, so tasks may now bring their own
stack (`task_create_with_stack`), and blob tasks get one 7168-byte stack sized
against the measured request. Heap 84456 -> 77208 B.

### 11.4 Four diagnoses, each retired by measurement

The remaining failure produced a blocked task resuming with `a0`, `a1` and
`WINDOWSTART` all zero. In order, each of these was proposed, tested, and killed:

| # | diagnosis | how it died |
|---|---|---|
| 1 | cleared `WINDOWSTART` bit | the whole register was zero, not one bit |
| 2 | blob task stack shortfall | identical fault with 7168 B, all guards intact |
| 3 | shared `_phy_stack` | identical fault on a private stack |
| 4 | saved frame corrupted in memory | the frame read back intact |

Each was a real defect and is fixed. None was the cause.

### 11.5 The fault reporter was describing the wrong instruction

`_vector_double` jumped to the same handler as `_vector_user`, which reads
`EPC1` and `PS`. A double exception writes **`DEPC`**; `EPC1` keeps the *first*
exception's PC. A window overflow/underflow *is* an exception, so a fault inside
one arrived reporting the `retw` that triggered it and the window vector's own
`EXCM=1, WOE=0` state — which was then explained, at length, as corrupted task
state.

Steps 35-38 of `next_moves/08` are an investigation into values that were never
anomalous. The rule earned:

> **A fault reporter that cannot distinguish a double exception will confidently
> describe the wrong instruction.** `DEPC` is not optional.

With `_vector_double` given its own handler:

```
exccause 2 (InstructionFetchError)
epc   0x3ffd4020   <- DEPC, the real faulting instruction
epc1  0x4008ac82   <- what triggered the handler, not the culprit
```

`0x3ffd4020` is `BLOB_DRAM_ADDR + 0x20`: control transferred into the blob's own
`.data`.

### 11.6 The window subsystem, exonerated

Having spent twelve steps there, it is worth recording what is now *measured* to
be correct rather than assumed:

- `win_spill_all` reduces 7 live frames to 1 (probed directly, no blob involved)
- all six window vectors are canonical and end in `rfwo`/`rfwu`
- every call0 bridge now writes its base save area (`rom_call3`, `rom_call4`,
  `win_spill_call0`) — three latent bugs of the class `phy_stack_call` already
  documented
- the spill runs on the right stack with 1252 bytes to spare
- every underflow in the system recovers a valid code address

### 11.7 Where it stands

The `.data` window is faithful in placement, content, and cross-window pointer
resolution. `0x3ffd4020` holds `0x00000001` — so the faulting PC is an address
that was *computed*, not a pointer that was loaded, which rules out the entire
"a function pointer holds the wrong value" family.

What remains is a line that has been printing since rev 1.2 and was read past
every time:

```
real osi forwarded calls: 0   last impl at 0x00000000
```

Fifteen adapter entries are reached and counted, and the counting table forwards
**none** of them. The blob accepting the table's version and magic proved the
header was right; it never proved the entries resolve. A computed dispatch off a
table base the host never finished wiring is exactly the shape of this fault.

**Nothing has been on air.**

---

## 12. A control that was not one (rev 1.4)

Rev 1.3 recorded the concurrency work and a fault that took twelve steps to
name. Continuing it overturned something older and more important than the fault.

### 12.1 The instrument class

Three instruments in this work returned a value that was indistinguishable from
"nothing to report":

| instrument | silent because | cost |
|---|---|---|
| `real osi forwarded calls: 0` | counts a table deliberately not in use | one step chasing a healthy zero |
| window diagnostics | gated on `exccause == 0`, then `0 or 2` | hid a changed fault twice |
| `multiframe` counter | reads a global register, cannot attribute owners | a wrong conclusion in step 31 |

The rule earned: **an instrument whose silence is indistinguishable from a result
is worse than no instrument**, because the silence reads as confirmation.

### 12.2 The control that was not one

`wintorture` has printed this since step 14:

```
switches during the call: 6  (preemption really happened)
```

and it is why "windowed frames survive preemption" was treated as established.
The counter incremented on **every tick**, including those where the scheduler
resumed the same task — and `rom_call3` takes the blob lock, which pins, so
during `wintorture` the scheduler cannot switch away at all.

With the counter corrected to count distinct switches:

```
switches during the call: 0  -- NONE, so this proves nothing
```

The test's own fallback wording, added at step 14 to guard against exactly this,
had never been reachable.

### 12.3 The experiment, finally run

With the pin disabled so preemption can genuinely occur:

```
exccause 29 (StoreProhibited)   DEPC 0x40080115   excvaddr 0x00000190
DOUBLE EXCEPTION   windowbase 6   windowstart 0x0000a248
```

`wintorture` panics inside `_WindowOverflow12`.

**Windowed frames do not survive preemption on this kernel.** The pin is not an
optimisation. It is the only thing keeping windowed code alive, and every result
that looked like surviving preemption was obtained while the pin silently
prevented preemption from occurring. `blobcall.c`'s header has been rewritten
against this measurement.

### 12.4 Window ownership

`WINDOWSTART` is 16 bits of "a frame lives here" with no owner field, and more
than one task holds frames in the register file at once. Assignment on restore
destroys frames a task is still using; OR never clears, so bits accumulate until
one names a frame `entry` never created. Both measured.

The kernel now records what the hardware does not — a per-task mask, claimed only
when the hardware confirms a frame at that task's own base, with a same-task
resume left untouched because a pinned task is resumed as itself on every tick
and any narrowing rule would delete its deep frames.

### 12.5 Where it stands

```
boot 11 PASS 0 FAIL   wintorture CORRECT   wincollide runs=120 wrong=0
blobphy rc=0   wifiinit 0x101   blobtx force 0x3004
```

`wifiinit task` panics in `_WindowOverflow12`. The driver needs a task that
blocks and resumes inside windowed code — precisely the case the pin cannot
cover, and which §12.3 shows the kernel cannot survive without it. The spill
reduces a blocking task from seven frames to one, measured. **The last live frame
is the part that has never worked, and it is now the only part left.**

**Nothing has been on air.**
