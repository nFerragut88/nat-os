# UM-NATOS-038 — Running the Vendor Stack from Flash

**Used Medias LLC — Embedded Systems Division**
Revision 1.0 · 2026-08-20 · Status: **Partial. Driver init reached; nothing on air.**

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

## 5. Seven defects, each with the measurement that found it

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
- **`esp_wifi_init_internal` does not yet succeed.** It returns
  `ESP_ERR_INVALID_ARG` against a table the blob accepts, which points at the
  config rather than the adapter. The field has not been identified.
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
- **Blocking OS calls cannot work yet.** `phy_stack_call` masks interrupts for
  the duration, so `osi_impl_sem_take` blocking on a contended semaphore would
  block forever with the scheduler frozen. Every call so far has been
  uncontended.

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
