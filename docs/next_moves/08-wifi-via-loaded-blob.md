# 08 — WiFi transmit by linking the vendor path, with the blob supplied at runtime

**Size:** large. **Risk:** low technically, high to the project's direction.
**Blocked on:** nothing. [07](07-irom.md) shipped, and the size objection below
is measured to be gone.

> **STEP 1 DONE, 2026-08-20.** The vendor transmit path **links** against a
> nat-os-shaped host with **10 shims**, not the large adapter surface this file
> predicted. See "Measured" at the end. Linking is not transmitting -- read the
> caveat there before treating this as nearly working.

*This is the route that would actually work. Read §"What it costs" before
starting it.*

---

## Why this exists

UM-NATOS-034 §31 corrected a claim the investigation had leaned on for four
sessions:

> both boards call the same blob with identical arguments, so any difference is
> the answer

True of `register_chipv7_phy`. **Never true of transmit.**

| | ESP-IDF | nat-os |
|---|---|---|
| PHY init | `register_chipv7_phy` in `libphy.a` | same function, same archive, arguments verified identical |
| **transmit** | `esp_wifi_80211_tx` in **`libnet80211.a`** → `libpp` → hal → registers | **hand-written register writes**, reconstructed from a disassembly of `hal_mac_txq_enable` |

`libnet80211.a` is not in this tree. `vendor/phy/` holds `libphy_natos.a` and
`libpp_natos.a` and nothing else, and `wifimac_tx()` calls no vendor function at
all.

So nat-os has never run the vendor transmit path. §29 established the
reconstruction does not radiate; this is the option of not reconstructing it.

## Why it does not fit today

```
ieee80211_output.o  (contains esp_wifi_80211_tx)   15,305 bytes .text
                                                   66 undefined symbols
libnet80211.a  .text, whole archive               196,269 bytes
nat-os IRAM free (-WiFi build)                     19,239 bytes
nat-os IRAM window, total                         131,072 bytes
```

The one object holding the entry point takes 80% of free IRAM before resolving a
single one of its 66 dependencies, and the archive's code is larger than the
whole IRAM window. **07 is a hard prerequisite, not an optimisation.**

## The shape, once 07 exists

1. Link `libnet80211.a` (and whatever it drags from `libpp`), placed in IROM.
2. Call `esp_wifi_80211_tx` rather than writing transmit registers by hand.
3. Expect to have to satisfy more of the OS adapter table than the PHY needed —
   net80211 is a protocol stack and will want timers, queues and events that
   `wifi_osi_impl.c` currently stubs or omits. `ositest` reports `0x3F ALL PASS`
   today (UM-NATOS-034 §24.6), which is a floor, not a guarantee.

### Delivering the blob from SD rather than shipping it

The reason to bother: **the repository and the flashed image contain no
Espressif binaries.** Those blobs generally cannot be freely redistributed, so
this is a real practical benefit for a public project, and it keeps
`docs/blob-free.md`'s claim literally true of what is shipped.

SD cannot be executed from — it is not memory-mapped. So SD is the *delivery*
mechanism and flash is the residence:

1. nat-os ships with no vendor binary and no WiFi transmit.
2. On demand, it reads the pre-linked blob image from SD and **writes it into a
   flash partition** — an install step, once, not per boot.
3. Thereafter it is mapped as IROM and executed from flash.

**Pre-link it at build time to a fixed address.** That avoids needing a runtime
linker: no relocation processing, no symbol resolution, no ELF loader. Load
becomes read-file, write-flash, map, call through a small entry table. The
address must be reserved in `linker.ld` so the kernel never lands there.

## What it costs

Stated plainly, because the numbers above make this sound easier than it is.

**Technically:** likely to work. It is what a working stack does.

**To the project:** nat-os's WiFi becomes ESP-IDF's WiFi. A third vendor archive,
larger than the two already here, running the protocol logic. `docs/blob-free.md`
argued that succeeding at WiFi transmit buys function and can never buy
independence — and moving the file to SD changes **where it is stored, not what
it is**. The blob still executes on the CPU with full privileges doing the actual
work.

The honest summary of the benefit: it is a *distribution* property, not an
engineering one. Debian-style non-free firmware separation. That is worth
something and it is not what "blob-free" has meant in this project's documents.

## The alternative, restated because it keeps being right

An SX1262 over SPI3 has a published register map, no PHY blob, no undocumented
calibration, and hardware already ordered. SPI3 is up and self-tested. It
delivers the same capability — a radio nat-os can transmit on — with none of the
above.

UM-NATOS-034 §17 and `docs/blob-free.md` both reached this conclusion before
this route was costed. Costing it has not changed it.

## Related

- UM-NATOS-034 §29 — the reconstruction confirmed not to radiate, on non-Espressif silicon
- UM-NATOS-034 §31 — the transmit-path correction that motivates this
- `docs/blob-free.md` — why function and independence are different purchases
- [07](07-irom.md) — the prerequisite

---

## Measured, 2026-08-20 — step 1 is done and the numbers moved a lot

### The size objection is gone

| | then | now |
|---|---|---|
| where vendor code can live | 19,239 B free IRAM | **3.3 MB IROM window** (07) |
| `libnet80211.a` `.text` | 196,269 B "larger than IRAM" | 234,616 B, irrelevant against IROM |

### The dependency surface is 10 symbols, not 66

The "66 undefined symbols" figure counted one object's references without
asking which resolve internally — the same mistake `vendor/phy/README.md`
avoided for `libphy.a`. Repeating that methodology:

```
libnet80211.a   defined internally 1189   referenced-undef 719
                TRUE EXTERNAL      206
after libpp + libcore + ROM + already-vendored blobs      32
after phy_host.o + librtc.a + libcoexist.a + newlib-nano  10
```

The residual ten:

| symbol | what it is |
|---|---|
| `free`, `puts`, `strtok` | libc; nat-os has equivalents |
| `hexstr2bin` | ten lines |
| `net80211_printf`, `mesh_printf` | route to UART or discard |
| `WIFI_EVENT`, `esp_event_handler_register` / `_unregister`, `esp_mesh_send_event_internal` | the event system — **the only genuinely new surface** |

### The link closes

`vendor/net80211/` holds the probe that proves it: `probe.c` references
`esp_wifi_80211_tx`, `net80211_host.c` supplies the ten, `probe.ld` gives a
memory layout.

```
link exit=0            0 undefined references
400d2b90 T esp_wifi_80211_tx
   text     data      bss
 545480     4113    16664
```

Reproduce with the command in this directory's README. Note `.text` is 545 KB
because the closure drags `libpp`, `libcore`, `libmesh`, `librtc`, `libcoexist`
and `libphy` — **not** net80211's 234 KB alone. nat-os already links two of
those, so the marginal cost is smaller than 545 KB but has not been separated
out. 16,664 B of `.bss` is the DRAM cost, against ~122 KB of free heap.

### The caveat that matters more than any number above

**Linking is not transmitting, and stubs that link are not stubs that work.**

The event stubs accept a registration and never deliver a callback. That is
plausibly enough for a blind `esp_wifi_80211_tx()`, which is a direct call — and
it is definitely **not** enough for scan, association or receive, all of which
are event-driven. The real scope of 08 is that difference, and it is invisible
in a symbol count. A previous run of this same measurement reported "0 undefined
references" when the link had actually failed for an unrelated reason and the
grep matched nothing; the number was only trustworthy after checking the ELF
existed and contained the symbol.

### Next, in order

1. **Reserve a flash partition** and a fixed pre-link address in `linker.ld`.
   Nothing else can be designed until the address is chosen.
2. Pre-link the closure to that address at build time — no runtime relocation.
3. SD-delivery and the flash install step (§"Delivering the blob from SD").
4. Call it, on air, and check the MT7921 sees a frame. Until step 4, none of
   the above is evidence about radiating.

---

## Step 4 — the loader, 2026-08-20. Mapped, loaded, verified. NOT called.

`kernel/blob.c` maps the region and runs the loader half. `blob` on the shell:

```
magic     N802  version 1
image     549240 bytes  of 1048576 reserved
.data     3588 B from 0x40385374 to 0x3ffd4000
.bss      16600 B at 0x3ffd5018
tx entry  0x40302b9c
loader    rc=0 (.data copied, .bss zeroed)
verify    .data 897 words, 0 mismatched;  .bss 4150 words, 0 non-zero
LOAD VERIFIED
NOT CALLED. mapping is not running.
```

Every value matches the pre-link. The MMU mapping, the cache flush, the `.data`
copy out of flash-mapped memory and the `.bss` zero all work, and the whole
range is verified rather than one word — a copy loop that returns is not
evidence it copied anything, and a single matching word would also match if the
loop had run exactly once.

**Mapping and calling are deliberately separate.** `blob map` stops after
validation. If a map is wrong, a call jumps into whatever bytes are there and
the board dies with nothing to read; every number above is obtainable before
anything executes.

### Two things this cost, both worth recording

**The byte copy faulted, and it was the same bug as this morning.** `data_lma`
points into the flash-mapped *instruction* region, which serves 32-bit aligned
accesses and nothing else. A byte loop over it raised `LoadStoreError` at
`0x4008187f`. `boot.c` hit the same class today from the other direction —
storing bytes *into* IRAM — and needed a DRAM bounce buffer. The rule worth
carrying: **if either end of a copy is instruction memory, it is a word copy**,
and the addresses and sizes get checked rather than assumed.

**The DRAM reservation had never been touched by anything.** The heap ends at
`0x3ffd3000` and the boot stack at `0x3ffd4000`, so the 32 KB reserved above it
was declared usable by `linker.ld` and had never once been written in the life
of the project. "The linker script says it is dram" is not evidence that it is
RAM. `dramtest` walks it in 1 KB steps and verifies store/read-back; it passes
across the whole reservation.

### Unexplained: two resets that stopped happening

Between the byte-copy fix and the working loader, two builds reset with
`TG0WDT_SYS_RESET` partway through the `.bss` zero. The current build does not,
across nine consecutive trials with the load fully verified and the scheduler
healthy afterwards.

**That is not the same as understanding it.** The only change between the last
failing build and the first passing one was adding an unrelated shell command,
which is a layout shift — the signature of something that moved rather than
something that was fixed. Both failing runs also followed a boot that had ended
in a kernel panic, so a stale watchdog or store state is a candidate nobody has
ruled in or out.

Recorded here because a fault that disappears when an unrelated thing moves is
this project's own definition of a latent bug, and the next person to see a
watchdog reset around this path should start here rather than from scratch.

### Next

Calling it. That needs `phy_stack_call` or an equivalent bridge — the blob is
windowed and the kernel is call0 — and PHY init must have run first, since
`esp_wifi_80211_tx` reaches the hardware through `libpp` and `libphy`. Nothing
on air until then.

---

## Step 5 — calling it. Three bugs found and fixed; not yet running.

The blob's PHY is now reachable and the call gets *into* vendor code. It does
not complete. Four faults so far, each one a different, correctly identified
cause — recorded because the sequence is the useful part.

### The architecture changed: the blob owns the whole radio stack

The blob links its own `libphy`. The kernel's `-WiFi` build linked a *second*
copy into IRAM. Two copies have two sets of `.bss`, so calibrating one and
transmitting through the other would present as a PHY that reported success and
a radio that stayed silent — a failure shape this project has already lost
sessions to.

So the kernel now initialises **the blob's** copy. `phyinit_run_at(fn)` takes
the address as a parameter and references no Espressif symbol, so `phyinit.c`
compiles into the blob-free build; `phyinit_run()` survives behind
`BOARD_HAS_WIFI`. The kernel image still contains no vendor code.

### Bug 1 — 357,940 bytes of code were missing from the image

`blob.ld` named `.text`; the vendor archives also ship `.iram1`, `.phyiram`
and six `.wifi*iram` classes. `objcopy --only-section` then dropped every one
of them. The image linked, passed its size check, loaded and verified — and the
region read back as **zeros**. First call: `IllegalInstruction` at
`0x4036e728`, inside the hole.

Fixed by naming all of them with **globs** (they arrive as `.wifi0iram.37`, so
bare names match nothing) and by dropping `--only-section` entirely. The real
guard is **`-Wl,--orphan-handling=error`**: the linker now refuses to place a
section nobody named. It immediately caught `.rodata_wlog_warning.59`.

> **Caveat, corrected 2026-08-20.** These are called `*iram` because ESP-IDF
> keeps them in RAM so they stay executable while the flash cache is off —
> ESP-IDF leaves interrupts **enabled** during flash operations, which is why
> its WiFi ISRs are `IRAM_ATTR`.
>
> **That reasoning does not transfer to nat-os.** `flash_erase_sector()` holds
> `crit_enter()` across the entire erase, so nothing at level ≤ `CRIT_LEVEL`
> runs and nothing fetches. Flash placement is safe against the *fault*; the
> cost is 125 ms of **delay**, which is the `next_moves/04` problem, not a
> memory-safety one.
>
> Two things do remain real. `CRIT_LEVEL` is 3, so an interrupt allocated at
> level 4–7 is not masked and would fetch mid-erase — the OSI table's
> `intr_alloc` decides that, so it should be chosen deliberately. And
> `store_save_if_allowed()`'s deferral is bounded: `STORE_DEFER_MAX` forces the
> write through after 32 refusals, and `store_record_fault()`, device writes and
> the boot save bypass the predicate entirely.
>
> For 802.11 the delay alone is disqualifying: ACK timeouts are microseconds and
> beacon intervals ~100 ms, so a 125 ms stall drops the association. Same
> conclusion `04` and `10` reached for LoRa — no scheduler change preempts a
> masked interrupt, so the fix is write policy.

### Bug 2 — `.rodata` was in the instruction window

Vendor code reads bytes out of its own rodata. The instruction window serves
32-bit aligned accesses only. Result: `LoadStoreError`, `epc 0x40362b28`,
`excvaddr 0x4037bd0c` — squarely inside `.rodata`.

This is exactly why ESP-IDF splits flash into IROM and DROM, and `boot.c`
already maps the kernel's own `.flash.rodata` through the data cache. The blob
now gets the same split: rodata at a fixed 64 KB-aligned image offset
(`BLOB_RODATA_OFF`), mapped to `0x3F700000` at MMU entry offset 0, alongside
the code mapping at offset 64.

**`excvaddr` is now in the panic report.** Without it a `LoadStoreError` inside
a blob is a bare `epc` with no way to tell a byte access to rodata from a wild
pointer. It is what turned bug 2 from a guess into a measurement, and it cost
four lines.

### Bug 3 — the loader refused, correctly

`rc=7`: `.rodata` ends on a byte boundary (it is full of byte-granular `wlog`
strings), so `.data`'s load address was unaligned and the word-copy guard threw
the image out. Fixed by aligning the LMA. Worth noting that the guard written
"in case a separately built image stops being aligned" earned its place within
a day.

### Where it stops now

```
epc      0x403001b3   entry a1, 32   inside phy_enter_critical
excvaddr 0x3f6fffe0   32 bytes below the DROM window
```

A window-overflow spill below the stack pointer — so `a1` is not pointing at a
stack. **`rom_call3` runs the callee on the caller's stack**, which here is the
shell task's 2 KB. `phy_stack_call` switches to the dedicated 6 KB
`_phy_stack_top`, but takes only two arguments, and `register_chipv7_phy` needs
three — so `phyinit.c` calls `rom_call3` and the PHY never receives the stack
`phy_stack_prime()` prepared for it.

That inconsistency predates this work and is visible in `phyinit.c` today: it
primes a stack the call it then makes does not use.

**Fixed, and no new assembly was needed.** `phy_stack_call`'s body has always
moved `a3`, `a4` and `a5` into `a10`, `a11` and `a12` — it passes **three**
arguments. Only the prototype in `window.h` said two, which is why
`phyinit.c` could not use it for a three-argument function and reached for
`rom_call3`. Declaring what the code already did is the entire fix.

### The blob's PHY initialises

```
blob loaded. calling its register_chipv7_phy at 0x4035e110
phyinit   rc=0  result=0x00000000
phystack  1296 of 6144 bytes used
IT RETURNED. that is not evidence the radio works.
```

The stack figure is the load-bearing number. A return code of 0 would also be
produced by an early exit; 1296 bytes of nested frames is evidence that a
substantial amount of vendor code actually ran.

**This is under nat-os's own bootloader, from a kernel containing no Espressif
code.** UM-NATOS-036 gated `-WiFi` to Espressif's second stage precisely
because `register_chipv7_phy` panicked with StoreProhibited in
`phy_enter_critical` under ours. That gate covers the copy the kernel *links*;
this is the blob's copy, at different addresses, with `.data` and `.bss`
freshly loaded. Whether the gate can now be lifted is a separate question and
has not been tested — but the failure it describes did not happen here.

### Transmit: attempted, and it fails exactly where predicted

`blobtx` builds a 56-byte beacon (SSID `NATOS-BLB`, channel 6), initialises the
blob's PHY, and hands the frame to `esp_wifi_80211_tx`:

```
phyinit   rc=0
frame     56 bytes, beacon, ssid NATOS-BLB, channel 6
calling esp_wifi_80211_tx at 0x40302bac
*** KERNEL PANIC ***  exccause 28 (LoadProhibited)
epc      0x40302a36    excvaddr 0x00000054
```

Disassembled, it is unambiguous:

```
l32r   a4, ...          ; g_wifi_global_lock
l32i.n a7, a6, 0        ; a7 = obj->table   -> NULL
l32i   a7, a7, 84       ; FAULT: [0 + 0x54]
callx8 a7               ; indirect call through that table
```

A vtable dispatch through a **null function table**. Nothing populated it,
because `esp_wifi_80211_tx` is the *last* step of a driver that was never
brought up. A live PHY is necessary and nowhere near sufficient — which is what
this file predicted at the top, and now has a fault address to prove.

### The remaining sequence, named

The blob exports every entry point needed:

| symbol | address |
|---|---|
| `wifi_osi_funcs_register` | `0x4030ded4` |
| `esp_wifi_init_internal` | `0x4030e14c` |
| `esp_wifi_start` | `0x4030e404` |
| `esp_wifi_80211_tx` | `0x40302bac` |

So the order is **register the OS adapter table → init → start → transmit**, and
`kernel/wifi_osi_impl.c` already exists for the first of those (it reports
`0x3F ALL PASS`, which UM-NATOS-034 §24.6 correctly calls a floor rather than a
guarantee).

That is the real remaining scope of 08, and it is a phase rather than a step:
the OS adapter is where a protocol stack asks for timers, queues, events and
tasks, and `net80211_host.c`'s event stubs currently accept a registration and
never deliver a callback. Association and scan need those to actually work;
a bare `esp_wifi_80211_tx` may not.

**Nothing has been on air.**

---

## Step 6 — the OS adapter table. Built, accepted, and not yet exercised.

`kernel/wifi_osi_table.c` is **generated** from
`esp_private/wifi_os_adapter.h`, not typed. The blob indexes the table by byte
offset, so a misordered entry calls the wrong function through the right slot —
the least debuggable failure available, and not a risk worth taking by hand.

### The layout is per-target, and getting that wrong is silent

The header is `#if`'d. On ESP32 `_phy_common_clock_enable/_disable` **are**
members and two other entries are **not**. A first pass ignored the
conditionals and produced a 123-entry table; the correct ESP32 layout is
**118 entries, 472 bytes**. `CONFIG_IDF_TARGET_ESP32` is now defined in the
header before the struct is included, because the precompiled blob was built
for ESP32 and the two layouts have to agree.

### Every entry is an instrumented stub, deliberately

nat-os has 24 primitives against 118 slots. Rather than guess which a bring-up
needs, each entry records that it was called and returns something safe;
`osiused` reports which entries were reached, in what order, and how often.

This turns "what does the driver need?" from an argument into a measurement,
and it is aimed squarely at this project's oldest failure mode: a stub that
quietly returns success is indistinguishable from a working implementation
until the radio silently does nothing. Entries get real bodies when the
evidence demands them.

### The blob validates the table

```
table     118 entries, version 8, magic 0xdeadbeaf
registering at 0x4030125c
osi_reg   returned 0
accepted - version and magic matched
```

`wifi_osi_funcs_register` checks `_version` and `_magic` and accepted it. That
is the *consumer* confirming the layout, which is better evidence than the
offset arithmetic that produced it.

> **An aside, from `esp_private/wifi_os_adapter.h`:**
>
> ```c
> #define ESP_WIFI_OS_ADAPTER_VERSION  0x00000008
> #define ESP_WIFI_OS_ADAPTER_MAGIC    0xDEADBEAF
> ```
>
> `0xDEADBEAF`. Somebody reached for `0xDEADBEEF`, typed `BEAF`, and shipped
> it — and because it is a magic number checked across an ABI boundary by
> precompiled binaries, it can never be corrected. Every ESP32 WiFi driver ever
> built has agreed to the typo.
>
> It is also, briefly, a trap. A reader who recognises the constant and
> "fixes" the spelling gets a table the blob rejects, and the failure is a bare
> non-zero return from `wifi_osi_funcs_register` with nothing to indicate that
> one letter is the reason. Copy it wrong, on purpose.
>
> A small reminder that vendor blobs are written by people having ordinary
> days, and that this whole exercise is archaeology as much as engineering.

### But transmit is unchanged, and that corrects an earlier claim

With the table registered, `esp_wifi_80211_tx` still faults identically —
`LoadProhibited`, `excvaddr 0x00000054` — and **`osiused` reports zero entries
called.**

### Resolved: the table was never the problem, the global was

The fault is inside `ieee80211_raw_frame_sanity_check` — which
`esp_wifi_80211_tx` calls first, before doing anything else:

```
l32i.n a7, a6, 0      ; a7 = g_wifi_osi_funcs      <- NULL
l32i.n a10, a4, 0     ; a10 = g_wifi_global_lock   (the argument)
l32i   a7, a7, 84     ; a7 = table->_mutex_lock    <- FAULT
callx8 a7             ; _mutex_lock(g_wifi_global_lock)
```

So offset `0x54` **is** `_mutex_lock`, exactly as step 5 said. What is null is
not the table — it is the *global pointer to* the table.

And `esp_wifi.h` says why:

```c
#define WIFI_INIT_CONFIG_DEFAULT() {     .osi_funcs = &g_wifi_osi_funcs,     ...
```

**The table is delivered through `wifi_init_config_t`, not by the registration
call.** ESP-IDF's own path does not rely on `wifi_osi_funcs_register` to
install it; `esp_wifi_init_internal(&cfg)` copies `cfg->osi_funcs` into the
global that the sanity check dereferences.

Both observations were correct and the inference between them was not.
Registering validated the table and returned 0; it simply is not the mechanism
that installs it. The reasoning error is worth keeping: a measurement that
disproves the *mechanism* was read as disproving the *identification*.

### Next, and now precisely

Build a `wifi_init_config_t` with `.osi_funcs = wifi_osi_table()` and call
`esp_wifi_init_internal(&cfg)`. That is what makes the table live, and it is
the first thing that will exercise it — `osiused` should stop being empty and
start naming the entries that need real bodies.

**Nothing has been on air.**

---

## Step 7 — the init config is built; PHY init regresses when it is linked

`kernel/wifi_init_cfg.{c,h}` builds the `wifi_init_config_t` that carries the
OS adapter table into the blob, and `wifiinit` calls
`esp_wifi_init_internal()`. The struct is **copied verbatim** from `esp_wifi.h`
for the same reason `wifi_osi_table.c` is generated — `feature_caps` is a
`uint64_t` and forces padding that is easy to get wrong by hand, and neither
side can detect a layout that is subtly off.

`wpa_crypto_funcs_t` is embedded **by value**: 30 members, 120 bytes. It is
kept as an opaque block with only `size` and `version` written, because nat-os
supplies no crypto and only its size affects the layout of what follows.

Config values are deliberately minimal — `nvs_enable = 0` (nat-os has no NVS at
all), AMPDU rx/tx off, CSI off, `feature_caps = 0`. Buffer counts are left at
ESP-IDF's defaults, because they decide how much the driver allocates through
`_malloc` and starving it produces failures that look like something else.

### The regression, and its exact boundary

**`register_chipv7_phy` hangs** — genuinely, not slowly: 40 s with the watchdog
disarmed and no return. It worked in the immediately preceding commit.

Bisected:

| tree state | `blobphy` |
|---|---|
| HEAD (step 6) | works — `rc=0`, `phystack 1296 of 6144` |
| + `wifi_init_cfg.{c,h}`, nothing referencing them | works |
| + the `wifiinit` command that references them | **hangs** |

The middle row is the informative one: with `--gc-sections`, an unreferenced
`wifi_init_cfg.o` is dropped entirely, so the hang needs the object *linked*,
not merely present.

### Ruled out, by measurement rather than argument

**It is not where `.bss` lands.** Linking the config moves `_phy_stack` from
`0x3ffbc640` to `0x3ffbc720`. Adding an unrelated 224-byte `.bss` array to
`blob.c` moves it to **exactly `0x3ffbc720`** — and PHY init still works. Same
address, same alignment, different outcome.

*(The first run of that probe reported "still works" while the probe had
silently failed to be inserted and `_phy_stack` had not moved at all. Checking
that the address actually changed is what made the result mean anything.)*

**It is not the watchdog.** The 3 s hang detector does fire — `phy_stack_call`
masks interrupts, so no task switches happen and any blob call over three
seconds is reset regardless of whether it is stuck. That is worth knowing on
its own for driver bring-up. But with the watchdog disarmed the call still
never returns.

**It is not stack depth.** The last working measurement was 1296 of 6144 bytes.

### Where it stops

`<prime>`, `<clk>`, `<mac>` and `<call>` all print; `<returned>` never does.
So it is inside `register_chipv7_phy`, reached through `phy_stack_call`, with
no fault raised — a spin, not a crash.

The kernel is unaffected: 11/11 boot self-tests pass and everything except the
blob PHY path behaves normally.

### Next

The unexplained part is small and well-bounded: linking one object changes
whether a vendor function returns. Candidates nobody has excluded are the
`.flash.text` growth in `shell.c` (which is IROM-mapped, so its size interacts
with the MMU work this route added) and the IRAM segment layout, which gained a
segment across the same boundary. Both are cheap to test by padding one without
the other — the same probe technique that ruled out `.bss`.

**Nothing has been on air.**

---

## The prerequisite nobody had identified: preemption inside blob calls

Found while explaining why the watchdog fires. It reframes what "next" is.

### The chain, verified from source

1. nat-os is `-mabi=call0`. Its context switch (`vectors.S`) saves `a0`-`a15`,
   `SAR` and the LOOP registers — and **not `WINDOWBASE`/`WINDOWSTART`**. It
   never spills the register window. (Confirmed: neither appears in the file.)
2. Blob code is **windowed**. While it runs, live frames sit in the physical
   register file, described by `WINDOWSTART`.
3. A context switch mid-call would hand another task a register file still
   marked as holding this call's frames. So `phy_stack_call` masks interrupts
   for the whole call — its own comment says *"Not for atomicity — to stop a
   context switch happening while windowed frames are live."*
4. Masked interrupts mean no tick, so no `task_schedule()`, so no switches.
5. `watchdog_liveness(next != g_current)` is fed **only** by a task switch.

So the hang detector measures "is the scheduler alive", and `phy_stack_call`
deliberately stops it. **Any blob call over 3 s is indistinguishable from a
hung kernel** and gets reset — which is why step 7 had to disarm the watchdog
before "slow" and "stuck" could be told apart.

`_phy_stack` is the sibling fix in the same function: the private 6 KB stack
solves *depth*, the interrupt mask solves *window-state coherence*. Both exist
because a call0 kernel is calling windowed code.

### Why this blocks `esp_wifi_init_internal` specifically

`register_chipv7_phy` never asks the OS for anything. It computes, writes
registers, and returns — so running it with the scheduler frozen is free.

A protocol stack does the opposite. `esp_wifi_init_internal` calls back through
the OS adapter table for mutexes, queues, semaphores, timers and task creation,
and several of those **block**. `_semphr_take(h, block_time)` cannot return
while the scheduler is frozen; `_task_create` produces a task that never runs.

**The current bridge model cannot work for driver init.** That is structural,
not a tuning problem, and it is a prerequisite rather than an optimisation.

### The cheap shape of the fix

Spilling on every switch would make every task in the system pay for WiFi and
would invalidate `next_moves/04`'s fairness measurements. It is not necessary:
a call0 kernel's steady state is **exactly one live frame**, because call0 code
never rotates the window. Multiple frames exist only during a blob excursion.

```
    rsr.windowstart a2       ; exactly one bit set -> today's fast path
                             ; more than one       -> spill + save WINDOWBASE
```

Normal switches stay as fast as they are now; the expensive path runs only for
the few switches that land inside blob code.

This is still the most dangerous code in the kernel to touch — every switch
goes through it, and UM-NATOS-009 records what a subtle error there looks like.
But it is bounded, and it is **testable without a radio**: force a preemption
inside a deliberately long windowed call and check the frames survive.

### Not a verdict on call0

UM-NATOS-003's reasoning still holds for the kernel itself. Calls across the
ABI boundary were always bridged and still work. What was never accounted for
is *preemption during* one, and that only matters when blob code needs to
block — which nothing did until now.

It does sharpen `blob-free.md`'s argument, though. An SX1262 has no windowed
blob: no bridge, no spill, no frozen scheduler, none of this.

---

## The step-7 regression, chased. Bounded, deterministic, mechanism NOT found.

Deliberate write-up of a failed hunt, because the eliminations are worth more
than nothing and re-doing them would cost the next attempt an afternoon.

**Deterministic: 5 of 5 trials.** Not the intermittent kind, and not the same
animal as the step-4 resets.

### What was eliminated, each by measurement

| hypothesis | test | result |
|---|---|---|
| `.bss` position moved `_phy_stack` | 224-byte dummy array puts it at **exactly** `0x3ffbc720`, the hanging address | **works** — not it |
| linking `wifi_init_cfg.o` | one-line command that still forces the link | **works** — not it |
| `.flash.rodata` growth | +224 B of rodata alone (29,116 vs hanging 29,128) | **works** — not it |
| `.flash.text` growth | code padding to **23,400**, *larger* than the hanging 22,908 | **works** — not it |
| a specific statement in the command | bisected in four steps (A/B/C/D) | **every half works** |
| stack depth | last good measurement 1296 of 6144 | not it |
| the watchdog | disarmed; still no return after 40 s | not it |

The bisect is the strange part. HALF D — blob map, load, a second
`phyinit_run_at` call site, the config call, `phy_stack_call` into
`esp_wifi_init_internal`, and the OSI counting loop — is nearly the whole
command and **works** at `.flash.text = 22,892`. The full command hangs at
**22,908**. Sixteen bytes. Yet 23,400 bytes of padding also works.

So it is neither section independently, and not a threshold in either. It is
some combination of exact sizes and layout.

### The flash image is structurally identical

```
WORKING   Segment 5: len 0x0596c load 0x400d0000 file_offs 0x0000fff8 [IROM]
HANGING   Segment 5: len 0x0597c load 0x400d0000 file_offs 0x0000fff8 [IROM]
```

Same file offset, so IROM data lands at flash `0x20000` in both — 64 KB
aligned, MMU entry 77 -> flash page 2, congruent. IRAM is contiguous in both
and ends at the same address. Nothing about the layout is malformed.

### What it smells like

A latent defect in the windowed-call path that particular layouts expose,
rather than anything the new code does. `phy_stack_call`'s own comments record
that its previous StoreProhibited was caused by **stale `WINDOWSTART` bits
surviving from whatever windowed code ran before** — a bug whose visibility
depended on what had executed earlier. This has the same shape: no fault, a
spin inside vendor code, and a trigger that moves when unrelated things move.

That also makes it plausibly the *same* underlying issue as the preemption
prerequisite above, approached from the other side.

### Where it stops

`<prime>`, `<clk>`, `<mac>`, `<call>` print; `<returned>` never does. Inside
`register_chipv7_phy`, reached through `phy_stack_call`, no exception raised.

The kernel is unaffected: 11/11 boot self-tests, everything except the blob PHY
path normal.

### For whoever picks this up

Do not re-run the table above. The untried angles are: dumping `WINDOWSTART`
and `WINDOWBASE` immediately before the `callx8` in both builds and diffing
them; and single-stepping the first few instructions of `register_chipv7_phy`
via the APP CPU capture rig (`appcpu.c`) rather than inferring from the outside.

**Nothing has been on air.**

---

## The preemption prerequisite, measured — and it is narrower than stated

`wintorture` holds **8 live windowed frames** and spins at the bottom for N ms
with **interrupts enabled** (via `rom_call3`, which unlike `phy_stack_call`
does not mask), then verifies a checksum on the way out. The switch count is
the control: without it, a PASS could just mean no preemption happened.

```
spun  60 ms with 8 windowed frames live, interrupts ENABLED
switches during the call: 1  (preemption really happened)
checksum 1632 expected 1632  CORRECT

spun 300 ms ... switches during the call: 3 ... CORRECT
```

**6 of 6 correct.** A context switch across live windowed frames did not
corrupt them.

### Why it survives, and what the real hazard is

nat-os's level-3 handler saves and restores `a0`-`a15` **at the current
`WINDOWBASE`**, and never changes `WINDOWBASE` itself. Every other task is
call0, and call0 code never rotates the window — so it only ever touches the
sixteen physical registers at that same `WINDOWBASE`, which are exactly the
ones the handler saved and restored.

A windowed task's *caller* frames live at other window positions. Call0 tasks
cannot reach them. That is why they survive.

**So the dangerous case is not preemption. It is two tasks inside windowed code
at once.** If task A rotates `WINDOWBASE` and is preempted, and task B then
makes its own windowed call, B's rotation walks forward and can overwrite A's
frames — and B's overflow exceptions would spill registers belonging to A onto
B's stack. An ISR that called into the blob would do the same.

### What this changes

The masking in `phy_stack_call` looks **over-conservative for the single-caller
case**. If the real invariant is *"only one context inside windowed code at a
time"*, it can be enforced with a mutex instead of by disabling preemption —
and then interrupts stay enabled, the scheduler keeps running, other tasks run,
and **blocking OSI calls become possible**. That is precisely what
`esp_wifi_init_internal` needs, and it is far cheaper than making the context
switch window-aware.

It also explains why the watchdog problem exists at all: masking was chosen to
protect an invariant that a mutex protects better.

### Explicitly not proven

- **Two tasks concurrently in windowed code has NOT been tested.** The hazard
  above is reasoned from the architecture, not measured. That test is the next
  thing to build, and it should be built to FAIL first.
- Deep recursion is limited by task stack, not by window handling: `wintest 60`
  panics with `stack guard overwritten, task 5` — the guard working correctly,
  8 frames being the practical depth on a 2 KB stack.
- None of this touches the step-7 hang, which remains unexplained.

**Nothing has been on air.**

### Step-7 hang: a second round of eliminations (symbol-position hypothesis)

Hypothesis tested: *if a symbol belonging to the PHY stack, the OSI adapter, a
blob entry point, a function-pointer table or a stack object moves by exactly
`0x10`, that is the correlation.* Good hypothesis — 16 bytes is the gap between
the working and hanging `.flash.text`, and Xtensa `entry` requires a 16-byte
aligned stack pointer.

**The failing band is narrow**, which is what made it worth testing:

```
.flash.text  22,892  works
             22,908  HANGS
             23,112  works
             23,400  works
```

Both smaller *and* larger builds work. That is not a threshold, it is a band.

**Full symbol diff between the two builds** — 965 symbols compared, 41 moved:

```
deltas: -240 (0xf0): 18 syms   -204 (0xcc): 17 syms
        -208 (0xd0):  5 syms   -44  (0x2c):  1 sym
```

- **No symbol moved by exactly `0x10`.**
- Everything that moved lives in `.flash.text` or `.flash.rodata`. DRAM and
  IRAM symbols are **identical** between the builds, so `_phy_stack`, the task
  stacks, `phy_stack_call`, `phyinit_run_at` and `blob.c` are all at the same
  addresses in both. Those were already eliminated and stay eliminated.
- In the named categories there was exactly one hit: **`g_osi`**, the OSI
  function-pointer table, `0x3f4070f8 -> 0x3f407008` (-240). Same alignment in
  both (≡8 mod 16).

**`g_osi` tested and eliminated.** Forcing it to 64-byte alignment moves it to
`0x3f407080` — a third address, differently aligned — and the build **still
hangs**. Its position is not the trigger.

So the surviving explanation is not a symbol address at all: the only remaining
difference is the *generated code* for `shell.c`'s dispatch function, whose
register allocation changes as branches are added to it. The call site itself
is well formed — `l32i a2, a12, 52` loads `e->phy_init` at the correct offset
and `call0 phyinit_run_at` follows.

**Still not found.** The next measurement would be a full disassembly diff of
the dispatch function between the two builds, looking for a register that is
live across `phy_stack_call` in one and not the other — `phy_stack_call` is
hand-written assembly that saves `a0` and `a12`-`a15` but rewrites
`WINDOWSTART` and does not restore it.

### Step-7 hang: liveness analysis across the call — also identical

Rather than eyeball the assembly, the values live across the call were tracked
in both builds.

**At all three `phy_stack_call` sites**, identical: `a2` live across (the
return value) and `a14` at one site (callee-saved, which `phy_stack_call`
preserves). **No ABI violation, no difference between builds.**

**At all four `phyinit_run_at` sites**, the instruction sequence is identical
once literal-pool offsets and branch targets are normalised away:

```
l32i a2, a12, 52          ; e->phy_init, correct offset
call0 <phyinit_run_at>
or   a12, a2, a2
```

*(Beware `lsi f5, a7, 28` style lines in the raw dump — objdump losing
instruction sync and re-syncing at a different offset. nat-os contains no
floating-point code. They are decode artifacts, not instructions.)*

So: **same instructions, same registers, same liveness, same stack usage —
only addresses differ.** Together with DRAM and IRAM being byte-identical
between the builds, every software-visible difference has now been eliminated.

### The one mechanism left standing

The kernel's flash-mapped code sits at `0x400D0000`; the blob's is at
`0x40300000`. They share one instruction cache. Moving `.flash.text` by ~200
bytes changes which cache sets the kernel's code occupies, and therefore which
of the blob's lines get evicted while `register_chipv7_phy` runs.

Cache pressure normally costs *time*, not correctness — but
`register_chipv7_phy` is **RF calibration**, and calibration loops wait on
hardware convergence with cycle-counted bounds. A loop that is fetched more
slowly can miss its window and never converge, which presents as exactly what
is observed: a spin inside vendor code, no fault, no OS service requested,
duration unbounded (40 s with the watchdog disarmed).

This also fits the *band*: 22,892 and 23,112 work, 22,908 does not. Aliasing is
periodic, so a narrow bad window between good ones is the expected shape,
whereas no monotonic size effect ever appeared.

**Untested.** The cheap probe is to relocate the blob — change `BLOB_IROM_ADDR`
by one 64 KB page and rebuild — which shifts its cache footprint without
touching a line of kernel code. If the hang moves to a different `.flash.text`
band, the mechanism is confirmed.

### Step-7 hang: three corrections, and the diagnosis moves

**1. The cache-aliasing probe was invalid, and its result must be discarded.**

`.blob_pad : { . = . + 0x140; }` advances the location counter but emits **no
bytes**. The LMAs of everything after it shifted by 0x140 — `tx entry` moved
from `0x4031acb8` to `0x4031adf8`, exactly 0x140 — while the image stayed
**606,404 bytes**, unchanged. So the file was 320 bytes shorter than its own
addresses claimed, and what got flashed was a corrupt blob. The "still hangs,
3/3" reading measured nothing but that corruption.

The tell was in the build output the whole time: padding was added and the byte
count did not move. A probe whose size does not change is not a probe.
*(Fourth silently-failed probe in this work. The pattern is always the same —
an edit that does not take, producing a confident result.)*

**2. `blob` hangs too, so the fault is NOT confined to the PHY call.**

With the correct blob restored, in the failing band, plain `blob` — which maps,
loads, verifies and **never touches the PHY** — hangs as well.

Step 7 recorded *"it spins inside `register_chipv7_phy`"*, based on markers
reaching `<call>` and never `<returned>`. That was true of the build it was
measured on and is **not the general case**. The trigger is reachable from
`blob_map()` / `blob_init()` alone.

That is a much better lead, because those two do something the PHY call does
not: **reprogram MMU entries and flush the cache**, from IRAM, with the caller
(`shell.c`) executing from **flash**. Returning to a flash-resident caller
immediately after invalidating the cache is precisely the kind of operation
whose behaviour can depend on where that caller sits — which is the observed
variable.

**3. The controls drifted mid-investigation.**

`vendor_torture` was added to `vendor/windowed/`, and `build.ps1` compiles that
directory **into the kernel**. IRAM `.text` therefore moved from 40,772 to
40,848. The earlier claim that "IRAM symbols are identical between the builds"
held when it was measured and does not hold across that boundary, so results
either side of it are not directly comparable.

### Where this actually leaves it

Known-good state restored and verified: `.flash.text 23,112`, 11/11 boot
self-tests, `blob` -> LOAD VERIFIED, `blobphy` -> `phyinit rc=0`.

The next measurement is now much cheaper than a cache experiment: put markers
**inside `blob_map()`** — before the MMU writes, between them and
`cache_flush()`, and after the return to the flash-resident caller — and find
which of those three the failing band cannot get past. If it is the return
after the flush, the mechanism is instruction refill from flash and has nothing
to do with the PHY at all.

### Step-7 hang: it cannot be instrumented in place

Markers were added inside `blob_map()` — before the MMU writes, between the
IROM and DROM loops, before and after `cache_flush()`, around the magic read —
plus two in `shell.c` either side of the call. `uart_puts` is IRAM-resident, so
they would print even if flash fetch were broken.

**Every marker printed and the load succeeded.**

```
[shell:pre-map] [map:enter] [map:irom-done] [map:mmu-done]
[map:flush-done] [map:read-magic] [map:returning] [shell:post-map]
LOAD VERIFIED
```

The instrumentation added code to `blob.c` (IRAM) and `shell.c` (flash), which
moved the build **out of the failing band**. This is the literal observer
effect: the trigger is a narrow region of layout space, and any probe placed
inside the kernel changes the layout it is trying to observe.

That retrospectively explains the whole hunt. Every probe in this section —
the 224-byte `.bss` array, the code padding, the `g_osi` realignment — "worked",
and each time that was read as *eliminating a hypothesis*. Some of those
eliminations are sound (they reproduced a specific address and still worked),
but any that relied on **"I changed X and the hang went away"** are worthless,
because changing anything makes the hang go away.

### What this means for the next attempt

In-kernel instrumentation is only usable if the build is first padded **back
into the failing band with the instrumentation already present** — a knob
swept until `blob` hangs again, with the markers compiled in the whole time.
That is the only way the markers report on the failing configuration rather
than on a different one.

The alternatives need no layout change at all:

- **The APP CPU rig** (`kernel/appcpu.c`) already runs on core 1 and samples
  registers while core 0 works. It is compiled in regardless, so arming it
  costs no core-0 code.
- **A hardware debugger.** UM-NATOS-001 notes JTAG has never been available on
  this board; this is the first defect that would clearly have justified it.

### Honest status

Not solved. Bounded to a narrow band of kernel layouts, reachable through
`blob_map()` with no PHY involved, deterministic within a build, and
unobservable by any means that alters the build. Known-good state restored and
verified.

### A real defect found while hunting: the MMU was reprogrammed with the cache live

`blob_map()` wrote the flash MMU entries **with the cache enabled** and flushed
afterwards. That is backwards.

- Espressif's `spi_flash_mmap` does `Cache_Read_Disable` -> write entries ->
  flush -> `Cache_Read_Enable`.
- `boot/boot.c` gets the same order **for free**: at boot the cache is still
  off, so it writes entries first and enables afterwards. That is why the
  bootloader path has always been reliable and this one was not.

Changing the translation of an address the cache may be filling from is a
textbook way to produce a fault that appears rarely and depends on what happens
to be resident — which is the shape of the step-7 hang exactly: layout
sensitive, deterministic per build, reachable from `blob_map()` with no PHY
involved, and unaffected by moving the blob's own code.

**Fixed**: cache disabled around the MMU writes, restored before returning.
Safe to run with the cache off because everything reachable from there —
`blob.c`, `uart.c`, `critical.h` — is IRAM-resident, and the cache is back on
before returning to `shell.c`, which is not.

**Whether this IS the step-7 hang is unproven, and probably not directly
provable.** A clean A/B would need the failing build with only this one line
changed — but the fix itself alters the layout the hang depends on, which is
the same observer effect documented above. It is corrected on its own merits:
it was wrong against Espressif's documented sequence regardless of the hang.

Note this was found by checking a hand-written register sequence against the
verified one two files away — the habit this project already records after
"several long debugging sessions traced to a single misremembered bit
position". The bit *definitions* were right; the *order* was not, and a
register read-back would have shown nothing wrong either way.

---

## Step 8 — `esp_wifi_init_internal` runs. The driver names its own first need.

### The OS adapter stubs had to be windowed

First attempt panicked: `IllegalInstruction`, **`epc 0x803014fd`**. Bit 31 set —
that is not a code address, it is a windowed **return-address encoding**
(`call8` stores `(2<<30) | offset` in `a0`). The CPU jumped to it raw.

The generated stubs were ordinary call0 kernel functions, and the blob calls
them through the table with `CALL8`. The window rotated forward on entry, the
callee used the rotated registers as its own, and its `RET` did not rotate
back — so `a0` still held the encoding. `window.S` records the identical fault
from the first time it was hit, at `0x4008a810`.

**Fixed by splitting the file by ABI:**

- `vendor/windowed/wifi_osi_stubs.c` — the 118 stubs and the table itself,
  compiled `-mabi=windowed`, because the blob calls them.
- `kernel/wifi_osi_table.c` — the accessors, still call0, reading the counters
  as **data**. Data has no calling convention; only calls had to move.

### And then it worked

```
phyinit   rc=0
config    224 bytes, magic 0x1f2f3f4f, osi_funcs -> 0x3f4070f8
calling esp_wifi_init_internal at 0x403014dc
init      returned 0x00000101  (an esp_err_t, not OK)
osi       1 of 118 adapter entries were called

1  _recursive_mutex_create  x1
```

**`esp_wifi_init_internal` executed and returned.** No crash, no hang. `0x101`
is `ESP_ERR_NO_MEM`: it asked for a recursive mutex, the stub returned NULL,
and it gave up cleanly — which is a driver behaving correctly against a host
that told it the truth.

This is the first time nat-os has run any part of ESP-IDF's WiFi *driver*, as
opposed to its PHY.

### The instrumented-stub design paid for itself here

118 entries, and the driver has so far needed **one**. Guessing which to
implement would have meant writing dozens on spec; instead the table named its
own requirement, in call order, on the first run. Every entry that never
appears is work nobody has to do.

`_recursive_mutex_create` is also the easiest possible first ask: nat-os
already has recursive mutexes, verified at boot — `[6b] mutex : PASS recursive
depth, ownership, non-owner unlock refused, try_lock both ways`.

### Next

Implement `_recursive_mutex_create` and its companions by bridging from the
windowed stub into nat-os's call0 mutex code — which is exactly what
`w2c_callN` in `window.S` exists for. Then run again and let the table name the
next requirement.

**Nothing has been on air.**

### Step 8b — the in-tree OS adapter table is STALE, and the blob caught it

`vendor/windowed/wifi_osi.c` already held a 118-entry windowed table whose
bodies forward into nat-os. Handing that one over instead looked like the
obvious de-duplication. It fails:

```
init      returned 0x00000102   (ESP_ERR_INVALID_ARG)
real osi forwarded calls: 0
```

**Zero forwarded calls** — rejected before anything ran. Comparing its member
order against the current IDF header:

```
index  54:  in-tree=_phy_common_clock_enable    IDF=_phy_update_country_info
index  55:  in-tree=_phy_common_clock_disable   IDF=_read_mac
... 63 positions differ
```

Both are 118 entries, so nothing about the size gives it away. It was generated
against an older header and every field from index 54 on is read from the wrong
offset.

Both files' own comments warn about exactly this — *"one member out of position
is a call to the wrong function with the wrong arguments, and nothing in the
system would diagnose it."* Something did diagnose it: **the blob**, by
checking `_magic` at the offset it expects and finding it wrong.

So `wifi_osi_stubs.c` is not a duplicate. It is the correct table, generated
from the header actually shipped with these binaries, and the in-tree one must
be regenerated or deleted before it misleads someone.

### Eight entries implemented; the driver walks further each time

Each run names its next requirement, and the loop is fast:

| run | result | what it asked for |
|---|---|---|
| 1 | `NO_MEM` | `_recursive_mutex_create` |
| 2 | `NO_MEM` | `_task_get_current_task`, `_calloc_internal` |
| 3 | `NO_MEM` | `_spin_lock_create` |
| 4 | **`INVALID_ARG`** | — resources satisfied |

Implemented, all forwarding through `w2c_callN` into nat-os's call0 side:

- **mutex group** — `_recursive_mutex_create`, `_mutex_create`, `_mutex_delete`,
  `_mutex_lock`, `_mutex_unlock`, onto `osi_impl_sem_*`
- **memory** — `_malloc`, `_malloc_internal`, `_calloc_internal`,
  `_zalloc_internal`, `_free`, onto `osi_impl_malloc/calloc/free`
- **`_task_get_current_task`** — nat-os ids are small ints and the blob only
  compares the handle for equality, so `id + 1` serves as a non-NULL handle
- **spin locks** — an opaque one-word handle; the blob hands it back to
  `_wifi_int_disable`/`_wifi_int_restore`, which mask globally rather than
  per-lock
- **`_wifi_int_disable`/`_wifi_int_restore`** — inline `rsil`/`wsr.ps`, not a
  bridge, since a bridge here would itself be interruptible (`phy_host.c` does
  the same)

Call order now:

```
_recursive_mutex_create x2   _task_get_current_task x4   _mutex_lock x2
_calloc_internal x1          _mutex_unlock x2            _spin_lock_create x1
_spin_lock_delete x1         _free x1
```

The create/delete pairing shows it allocating, failing validation, and unwinding
cleanly.

### Next

`ESP_ERR_INVALID_ARG` with a table the blob accepts means the **config** is now
the problem, not the adapter. The AMPDU group was already set to IDF defaults
and did not change it; `_version`/`_magic` are correct on both sides. The field
needs finding rather than guessing — the driver validates `wifi_init_config_t`
early, so disassembling the comparisons in the first 100 instructions of
`esp_wifi_init_internal` will name it directly.

**Nothing has been on air.**

### Step 9 — chasing `INVALID_ARG`, and a claim that has to be withdrawn

Static analysis said the config was the problem. Dynamic testing says the
question is worse than that.

**`esp_wifi_init_internal` is 252 bytes** (`0x403014dc`-`0x403015d8`) and
contains exactly one site producing `0x102`:

```
403014e1: bnez.n a2, +0x12      ; config == NULL?
403014e6: movi   a2, 0x102      ;   -> ESP_ERR_INVALID_ARG
403014ee: l32i   a10, a2, 0     ; a10 = config->osi_funcs
403014f1: call8  wifi_osi_funcs_register
```

That path returns **before** registering anything. So it cannot account for the
eight OS adapter calls we observe. Yet:

```
wifiinit null  ->  0x102, 8 of 118 entries called
wifiinit       ->  0x102, 8 of 118 entries called
```

**Identical for a NULL config and a real one.** And the calls are attributable:
boot alone, `blob`, `blobphy` and `osi` each produce **zero** adapter calls;
only `wifiinit` produces eight. So they come from this call, from a function
whose second instruction should have rejected NULL.

### The claim being withdrawn

Step 6 reported `wifi_osi_funcs_register` returning 0 as *"accepted — version
and magic matched"*, and cited it as evidence the generated table had the right
shape. **That wording was invented here, not read from the blob.**

`osi null` — registering a NULL table — also returns **0**. A NULL pointer
cannot satisfy a version or magic check, so this function does not validate
what it is given, or at least not that. The message has been corrected to say
what is actually known: it returned zero, and it returns zero for NULL too.

This weakens, though does not overturn, the step-8b conclusion that the in-tree
table is stale. The 63-position layout divergence is real and measured from the
headers. What is no longer supported is "the blob detected it" — the different
outcome may have had another cause.

### What is actually established

- The eight adapter entries are reached, in a stable order, and their
  implementations work (the driver progressed `NO_MEM` -> `NO_MEM` -> `NO_MEM`
  -> `INVALID_ARG` as each was written).
- PHY init is unaffected and still returns `rc=0`.
- `wifi_osi_funcs_register` returns `ESP_OK` for any argument tried so far.
- The first argument's effect on `esp_wifi_init_internal` is **not observable**,
  which is either an argument-passing fault in `phy_stack_call` or a
  disassembly that is misleading about the function's real shape. objdump has
  already been caught losing sync inside this blob and emitting `lsi f4`
  decodes for code containing no floating point.

### Next

Settle argument passing with a function whose behaviour for a known input is
certain, rather than one whose validation is being inferred. `esp_wifi_80211_tx`
takes a length; calling it with an absurd length should fail differently from a
sane one. If both behave identically, the fault is in the call path and every
result in this section that depended on an argument is void.

**Nothing has been on air.**

### Step 9b — argument passing is CLEARED, by a function that cannot be argued with

`argtest` calls ROM `crc32_le` — fixed address, windowed, **pure**, result
depends on every argument — through both bridges, with two different inputs,
and against values computed on the host:

```
rom_call3       6B=0xd8cf24e2  10B=0xe08b3d62
phy_stack_call  6B=0xd8cf24e2  10B=0xe08b3d62

host zlib.crc32("nat-os")     = 0xd8cf24e2
host zlib.crc32("nat-os-xyz") = 0xe08b3d62
```

Exact match on both, both bridges agree, and the two inputs produce different
answers. **`phy_stack_call` delivers its arguments.** `rom_call3` is the
control — a bridge already known good — so a shared fault would have shown as
both failing.

This is the opposite of the `wifi_osi_funcs_register` test, and deliberately
so. That one asked a vendor function whose validation had to be *inferred*, and
the inference was wrong. This one asks a function whose correct answer is
computable off-device, so a wrong result could not have been explained away.

**What this rules out:** the bridge. Every result in this section that depended
on an argument arriving is therefore still standing, including the eight
adapter implementations and PHY init.

**What remains unexplained:** `esp_wifi_init_internal` behaves identically for
a NULL config and a real one — same `0x102`, same eight adapter calls — while
its second instruction is `bnez.n a2` and its only `0x102` site returns before
registering anything. Those three facts cannot all be true as stated, and the
argument now cannot be the loose one.

The remaining suspect is the disassembly itself. objdump has been caught losing
sync inside this blob twice, emitting `lsi f4` for code containing no floating
point. If the decode is wrong about where instructions begin, it is wrong about
which branch guards what, and the "only one 0x102 site" claim goes with it.

**Next:** stop reading the listing and watch the function instead. `_set_intr`
proved a stub can be given a body that records; the same trick works here —
give one early adapter entry a body that reports its arguments, and the driver
will say how far it got and with what, without any disassembly being trusted.

**Nothing has been on air.**

### Step 9c — the call sequence, and what it settles

`wifiinit` now prints the exact adapter call sequence with repeats and
allocation sizes. `wifi_osi_order()` only ever recorded first touches, which
cannot show a loop, a retry or a rollback.

```
_recursive_mutex_create -> _task_get_current_task -> _mutex_lock
-> _calloc_internal(32) -> _task_get_current_task -> _mutex_unlock
-> _spin_lock_create -> _recursive_mutex_create -> _spin_lock_delete
-> _task_get_current_task -> _mutex_lock -> _free
-> _task_get_current_task -> _mutex_unlock
```

Fourteen calls: take the API lock, allocate 32 bytes, release, create and
delete a spin lock, re-take the lock, free, release. **An init that allocates,
fails, and rolls back cleanly** — which is a driver behaving well against a
host that answers honestly.

**A NULL config produces a byte-identical sequence.** Both runs non-empty,
fourteen calls each, same `0x102`.

*(An earlier attempt at this comparison reported "identical" while both runs
had produced NO sequence at all — comparing two empty strings. The check now
refuses to conclude anything when either side is empty.)*

### What that settles, and what it does not

**Settled — the blob is not stale.** Reflashed and retested: unchanged.

**Settled — the bridge is fine.** §9b, and independently `register_chipv7_phy`
takes two pointers through the same bridge and calibrates successfully. Blob
functions receive their arguments.

**Therefore the listing is wrong.** If instruction 2 were `bnez.n a2` guarding
a `0x102` return, a NULL config would return immediately with zero adapter
calls, and `l32i a10, a2, 0` would fault on address 0. Neither happens. objdump
has been caught losing sync inside this blob twice, decoding `lsi f4` in code
with no floating point — the "only one `0x102` site" claim rests on that same
decode and should be treated as withdrawn.

The coherent reading is that the config is **validated later than the listing
suggested**, after the lock/allocate/rollback above, and is not dereferenced at
entry at all. That fits every observation without requiring anything to be
broken.

### Config bisection has started

`wifi_init_cfg_nvs()` exists so fields can be flipped from the shell rather
than guessed at. First candidate tested and **eliminated**:

| field | tried | result |
|---|---|---|
| `nvs_enable` | 0 -> 1 (IDF default) | `0x102`, 14 calls — unchanged |
| `ampdu_rx/tx`, `rx_ba_win` | -> IDF defaults | `0x102` — unchanged (§8) |

Twenty-one fields, two groups eliminated. The harness makes each further test a
build and a boot, and the 14-call sequence is the control: a config the driver
accepts should change the sequence, not merely the return code.

**Nothing has been on air.**

### Step 10 — the reference board answers it in one measurement

Nineteen config fields remained unbisected and every hypothesis had been wrong.
Instead of continuing, `tools/idf_ref` — real ESP-IDF on COM6 — was made to
print its own `wifi_init_config_t`, offsets and all, and the two were compared.

```
reference: sizeof=216   static_rx_buf_num off=120   feature_caps off=192   magic off=208
nat-os:    sizeof=224   static_rx_buf_num off=124   feature_caps off=200   magic off=216
```

**Not a value — a layout.** Every field from `static_rx_buf_num` onward sat 4
bytes late, and `sizeof` was 224 against 216.

The cause: `wpa_crypto_funcs` is embedded by value and was reserved as **120
bytes**, counted by hand off the header. It is **116** — 29 members, not 30.
One extra word shifted everything after it and added 4 bytes of tail padding.

So the driver read `magic` at offset 208, where nat-os's struct held
`sta_disconnected_pm` — value 0 — and returned `ESP_ERR_INVALID_ARG` without
saying which argument. **Every value in the struct was correct and every one
was in the wrong place.** That is precisely why it was invisible: nothing about
the values could be wrong, so the values kept getting checked.

Two values were also wrong, and the reference named them without being asked:
`feature_caps` is `0xa1` (WPA3_SAE | GMAC | ENTERPRISE), not 0; and
`sta_disconnected_pm` is 1.

### The result

```
config    216 bytes, magic 0x1f2f3f4f
init      returned 0x00000101      <- NO_MEM: self-locating again
osi       11 of 118 adapter entries were called
```

`INVALID_ARG` -> `NO_MEM` is the important part. The driver stopped rejecting
the argument and went back to asking for resources, which is an error that
names its own fix. The sequence grew from 14 calls to 18:

```
... -> _spin_lock_create -> _recursive_mutex_create -> _malloc_internal(60)
-> _task_get_max_priority -> _wifi_create_queue -> _free -> _spin_lock_delete -> ...
```

**`_wifi_create_queue` is the next requirement.**

### The lesson, which the project already had written down

UM-NATOS-034 brought in a second board rather than reasoning about what the
first should be doing, and §12.2 recorded that reaching for the heavy method
and letting its constraints rule out the approach was the error. The same shape
repeated here: a disassembly that could not be trusted, nineteen fields of
guessing, and a board on the desk that could simply be asked.

**Nothing has been on air.**

### Step 11 — queues, semaphores, and the fork arriving on schedule

With the config layout fixed, each run names the next requirement and the loop
runs fast:

| run | rc | asked for |
|---|---|---|
| a | `NO_MEM` | `_wifi_create_queue` |
| b | `NO_MEM` | queue too large — see below |
| c | `NO_MEM` | `_semphr_create` |
| d | `NO_MEM` | **`_task_create_pinned_to_core`** |

**`_wifi_create_queue` needed more than nat-os would give.** ESP-IDF hands back
a `wifi_static_queue_t { handle; storage; }` rather than a bare handle, so the
wrapper is an 8-byte allocation around `osi_impl_queue_create`. That part was
easy; the refusal was not. Recording the request showed the driver asking for
**200 items of 8 bytes = 1600**, against `OSI_QUEUE_BYTES = 512`.

A `NULL` return was ambiguous between "pool exhausted" and "too big" until the
*request* was visible — the same lesson as the adapter stubs, one level down.

**Fixed by allocating storage rather than reserving it.** The queue struct held
`uint8_t buf[512]` inline. Raising that constant would have cost
`OSI_QUEUE_MAX x` the new size in `.bss` regardless of whether the queues
exist — 8 x 2 KB for one queue that needs it. Storage is now heap-allocated to
the size actually requested, and `osi_impl_queue_delete` frees it: marking the
slot unused and walking away would have leaked 1600 bytes per bring-up.

The cap survives as an upper bound (4 KB), because a queue silently shorter
than requested loses messages under load. Refuse, do not truncate.

**Semaphores** were already there under another name — `osi_impl_sem_*` backs
the mutex entries — so `_semphr_create/delete/take/give` are direct forwards.

### The fork has arrived

`_task_create_pinned_to_core` is the next requirement, and it is not another
entry to fill in.

`phy_stack_call` masks interrupts for the whole blob call. A task created
inside that call **cannot run**, and a `_semphr_take` that genuinely blocks
waiting for it **cannot return**. Every take so far has been uncontended, which
is the only reason this has not bitten yet.

This is the preemption prerequisite, reached as a live code path rather than an
argument. The measured answer from earlier still stands: preemption across live
windowed frames is safe (`wintorture`, 6/6, with the switch count as control),
and the real invariant is **one context inside windowed code at a time** — a
mutex, not a masked interrupt. That change is now on the critical path.

**Nothing has been on air.**

### Step 12 — exclusion by mutex instead of by masked interrupts

`blob_call()` enters the blob holding a mutex, with the scheduler still
running. `phy_stack_call`'s masking is now conditional on `g_phy_call_mask`
rather than removed — the smallest change that could work, because duplicating
sixty lines of window-manipulating assembly to get one variant would have been
a worse risk than the bug being fixed.

PS is saved either way, so the restore is correct in both modes: when the mask
is skipped the saved PS equals the current one and writing it back is a no-op.

**What the masking was protecting, and why a mutex is the better answer:**

1. **Window state.** The concern was a context switch landing while windowed
   frames were live. That was **measured and does not hold** — `wintorture`,
   6/6, with the switch counter as control. A call0 task cannot disturb them:
   the handler saves and restores the sixteen registers at the current
   `WINDOWBASE` and never moves it, and call0 code never rotates the window, so
   a windowed task's caller frames sit where no other task can reach. The real
   hazard is **two contexts inside windowed code at once** — exclusion, not
   preemption.

2. **The private stack.** `_phy_stack` is one shared 6 KB buffer. Two contexts
   in `phy_stack_call` would corrupt each other whatever the window did. This
   reason survives the measurement and is on its own sufficient to need a lock.

`register_chipv7_phy` keeps the masked path: it is self-contained calibration
that asks the OS for nothing, so freezing the scheduler around it costs nothing
and changing it would be change for its own sake.

**Live and uncontended:**

```
blob_call: 1 entries, contended 0  (scheduler stayed live)
init       returned 0x00000101      (unchanged -- as expected)
```

The outcome is deliberately unchanged. `_task_create_pinned_to_core` is still a
stub, so nothing blocks yet and nothing *could* have improved. This is
infrastructure landing ahead of the requirement, verified only to the extent
that it does not break what already worked: 11/11 boot self-tests, `blobphy`
`rc=0`, `wintorture` correct.

**Contention is counted, not assumed.** There is one caller today, so
`contended` must stay 0. If it moves, something has started entering the blob
from a second context and the reasoning above needs re-reading.

**Not covered:** an interrupt handler cannot take a mutex. If a WiFi ISR ever
calls into the blob this is insufficient. Nothing does today, and `_set_intr`
clamps and counts, so that day is visible rather than silent.

**Nothing has been on air.**

### Step 13 — the task gets created, runs, and collides. Measured.

`_task_create_pinned_to_core` is implemented: the blob passes a function *and*
a parameter while `task_create()` takes a name and a void entry, so each
request gets a slot and a trampoline. The trampoline finds its slot by task id
and **waits** if it has to — the created task can be scheduled before
`task_create()` has returned the id to store, because the scheduler is live
now, which is the entire point of `blob_call()`.

It worked. The task was created, ran, entered blob code, and blocked on a
semaphore. Then:

```
*** KERNEL PANIC ***
exccause : 0  (IllegalInstruction)
epc      : 0x4008b4fb   ->  inside osi_s_semphr_take
```

`osi_s_semphr_take` is **windowed**. At that moment two contexts were executing
windowed code at once — the new WiFi task via `rom_call3`, and the shell still
inside `phy_stack_call`. The window rotated under a second context and the
frames collided.

**This is the hazard predicted in step 12, arriving as a fault rather than an
argument**, and it is the case `blob_call()`'s mutex cannot cover: the WiFi task
cannot hold that mutex, because by design it holds it forever.

So the earlier conclusion refines once more:

- preemption of windowed code by **call0** tasks is safe (`wintorture`, 6/6)
- **one context** in windowed code is safe with a mutex (step 12)
- **two contexts** in windowed code is not, and a driver task makes that
  unavoidable

### Now gated, not reverted

`wifiinit task` opts in; plain `wifiinit` refuses task creation, reports
`NO_MEM`, and unwinds cleanly — scheduler still running afterwards. The
reproducer stays reachable because it is the test case for the next piece of
work, but the default path no longer takes the board down.

### Two requirements recorded on the way past

- **The blob wants a 6,656-byte task stack.** nat-os gives every task
  `TASK_STACK_WORDS` = 2 KB. Refusals are counted rather than truncated,
  because a stack quietly one third the requested size fails much later and
  somewhere else.
- **Window-aware context switching is now on the critical path**, not deferred:
  spill the window and save `WINDOWBASE`/`WINDOWSTART` when switching away from
  a task with more than one live frame. The lazy check described earlier still
  applies — a call0 kernel's steady state is exactly one frame, so the
  expensive path runs only for the few switches that interrupt blob code.

**Nothing has been on air.**

### Step 14 — first attempt at window-aware switching. Failed, reverted.

**Recorded because the approach looked right and is not.**

The plan was the standard idiom: on switching away from a task with more than
one live frame, force the frames out to their own stacks, then let the incoming
task rotate freely.

- Direct register-file save was **ruled out first**: `rotw` rotates *every*
  register including whichever one holds the frame pointer, so there is no
  register left to address memory with. Parking it in `EXCSAVE_3` and
  re-reading it works for one group and then loses the group it clobbers.
- The ESP32 ROM exports **no** window-spill helper — checked, only
  `xthal_memcpy`, `xthal_get_ccount` and friends.
- So: `win_spill_all`, six nested `CALL12`s (72 registers against a 64-register
  file), letting `_WindowOverflow*` write each oldest frame to the stack
  pointer recorded in it. Invoked from `_handler_level3` after `PS.EXCM` is
  cleared, via `win_spill_if_needed()` which checks `ws & (ws - 1)` so ordinary
  call0 switches pay a read and a branch.

It compiled, and the kernel booted 11/11. Then:

```
wintorture 60  ->  *** KERNEL PANIC *** IllegalInstruction, epc 0x4008b6d8
```

**It broke the case that already worked** — a *single* windowed task, which had
been 6/6 before. Reverted; `wintorture` correct again immediately.

### What that says

The failure is in the mechanism, not the goal. Calling a windowed routine out
of the level-3 handler means rotating the window in the middle of a context
switch, while `a1` is the frame under construction and the handler's own state
lives in registers the spill chain is about to walk over. The spill also
services overflows through handlers that assume ordinary task context.

Untried alternatives, in the order worth trying:

1. **Spill in task context, not handler context.** A task about to enter blob
   code could ensure it is the only one with live frames, or spill on the way
   *out* of `phy_stack_call` rather than at the switch. Nothing then runs
   inside the interrupt handler.
2. **Give each windowed context its own window region** by never letting two
   exist — the "blob server" task from step 12's discussion, whose limits are
   already written down.
3. **Save the register file after all**, using two special registers rather
   than one so the pointer survives rotation.

`wincollide` remains the test, and it still fails, which is the correct state
for a bug that is not fixed.

**Nothing has been on air.**

### Step 15 — second attempt, also reverted. But the diagnosis is now complete.

Step 14 failed because the spill ran inside the interrupt handler. The obvious
next thought was that spilling was not even the missing piece: **`WINDOWBASE`
is never saved.** For call0 tasks that never mattered — nothing rotates, so
every task sees the same base. A task inside vendor code does rotate, and
resuming it at whatever base the intervening task left behind makes its `RETW`
unwind to a window position its frames are not at.

That was implemented — `WINDOWBASE` into the frame's spare word at offset 84,
restored before the registers with the frame address parked in `EXCSAVE_3`
across the rotation, and `WINDOWSTART` narrowed to one live frame.

**It broke `wintorture` too**, and that failure is the informative one:

Narrowing `WINDOWSTART` to a single frame declares every older frame dead. That
is only safe if those frames are **already in memory**. With a single windowed
task nothing has spilled them, so the declaration throws them away and the
`RETW` reloads whatever happens to be on the stack.

### So the complete fix needs all three, in order

1. **Spill** the live frames to their own stacks — they must be in memory before
   anything else is safe.
2. **Save and restore `WINDOWBASE`** per task — otherwise the frames are in
   memory but the task resumes looking for them at the wrong rotation.
3. **Narrow `WINDOWSTART`** to one live frame — only valid *after* step 1, and
   what makes the reload happen through `_WindowUnderflow*`.

Step 14 did 1 (in the wrong place) without 2 or 3. Step 15 did 2 and 3 without
1. Neither could have worked, and each failure eliminated its own hypothesis.

The remaining question is only **where** step 1 can safely run. Not the level-3
handler — step 14 established that. The candidate is task context: spill on
*entry* to windowed code, so a task about to rotate first pushes out whatever
the previously-running task left live. That runs on a normal stack, outside any
handler, and is the same place `phy_stack_call` already narrows `WINDOWSTART`
without spilling first — which is itself a latent version of this bug.

### Status

Both attempts reverted; the tree is clean and verified: boot 11/11,
`wintorture` correct, `blobphy` `rc=0`, `wifiinit` `NO_MEM`. `wincollide` still
fails, which remains correct for an unfixed bug.

**Two failed attempts at the same subsystem in one sitting is a signal.** The
diagnosis above is worth more than a third attempt made tired; the next one
should start from it rather than from scratch.

**Nothing has been on air.**

### Step 16 — part 1 of 3, in the right place this time

`win_spill_all` is back, but invoked from `rom_call3` in **task context**
rather than from the level-3 handler. A task about to rotate the window first
pushes out whatever the previously-running task left live, onto that task's own
stack, where `_WindowOverflow*` already knows to put it.

```
boot        11 PASS 0 FAIL
wintorture  CORRECT      <- steps 14 and 15 both broke this
wincollide  still panics
```

**This is the first version that does not break what already worked.** It is
also not a fix on its own, which the diagnosis predicted: spilling is step 1 of
three, and steps 2 and 3 -- save/restore `WINDOWBASE`, then narrow
`WINDOWSTART` -- are still missing.

### The remaining subtlety, which is why 2 and 3 are not just "add them back"

Narrowing `WINDOWSTART` on resume is only valid if that task's frames were
actually spilled. They are spilled when *another* task enters windowed code —
but if no other task did, the frames are still in registers and untouched, and
narrowing would throw them away. That is exactly how step 15 broke
`wintorture`.

So the restore has to know which happened. A generation counter answers it:
`win_spill_all` bumps it, each frame records the value seen at switch-out, and
on resume the handler compares —

- **unchanged** — nobody rotated over us; restore the saved `WINDOWSTART`
- **changed** — our frames were spilled; narrow to one and let
  `_WindowUnderflow*` reload them

That is a small amount of state and it makes both cases correct rather than
picking one and hoping. Recorded rather than written, because it belongs on top
of a verified base and this one has just been established.

**Nothing has been on air.**

### Step 17 — third failure, and the pattern is the finding

The generation-counter design from step 16 was implemented exactly as
specified: `WINDOWBASE`, `WINDOWSTART` and the generation into the frame's
three spare words; on resume, restore `WINDOWBASE` with the frame address
riding in `EXCSAVE_3`, then compare generations and either restore the saved
mask or narrow to one live frame.

`wintorture` panicked again.

### Three attempts, one common factor

| | what it changed | result |
|---|---|---|
| step 14 | spill **inside the handler** | `wintorture` broke |
| step 15 | `WINDOWBASE` + narrow, **in the handler** | `wintorture` broke |
| step 17 | all three parts, **in the handler** | `wintorture` broke |
| step 16 | spill **in task context** | nothing broke |

Every attempt that writes window state **from inside `_handler_level3`** breaks
the single-task case. The one attempt that stayed out of the handler did not.
That is no longer three unlucky bugs; it is the same wall three times, and the
next attempt should treat "the level-3 handler cannot safely manipulate the
register window" as an established property rather than something to work
around more cleverly.

A plausible reason, recorded as hypothesis not conclusion: the handler runs
with `PS.EXCM` cleared so that C can be called, which means window exceptions
are **enabled** while it is mid-switch. Writing `WINDOWBASE` or `WINDOWSTART`
there can make the very next `l32i` off `a1` take an underflow into a handler
that assumes ordinary task context. That would explain why the failure is
immediate and why placement, not logic, decides it.

### What this leaves

The route that has not failed is doing everything in **task context**:

- spill on entry to windowed code — **done and verified** (step 16, in the tree)
- the remaining two parts would have to happen there too, which means a task
  entering windowed code fixes up its own window state rather than the
  scheduler doing it on everyone's behalf

Whether that is expressible at all is an open question. If it is not, the
honest answer may be that **one windowed context at a time** is the invariant
nat-os can actually hold, which `blob_call()` already enforces — and that a
driver needing its own concurrent task is simply outside what a call0 kernel
can host without a much larger change.

That would be a real answer, and it is worth reaching deliberately rather than
after a fourth attempt.

### Status

Reverted to the step-16 checkpoint and fully verified: boot 11/11,
`wintorture` correct, `blobphy` `rc=0`, `blob` LOAD VERIFIED, `wifiinit`
`NO_MEM`. `wincollide` still fails, correctly.

**Nothing has been on air.**

### Step 18 — two more hypotheses eliminated; the ABI is exonerated

Step 17 concluded with a hypothesis: the handler runs with `PS.EXCM` cleared,
so window exceptions are live while the window state is deliberately
inconsistent. **Tested — that is not sufficient.** Setting `PS.EXCM` across the
restore still panicked.

Second hypothesis, more specific: the `WINDOWSTART` bit for the current
`WINDOWBASE` must be set or the processor state is illegal, so writing
`WINDOWBASE` first is invalid regardless of exception masking — suppressing an
exception does not make a state legal. Widening `WINDOWSTART` to cover both the
old and new base, moving `WINDOWBASE`, then narrowing keeps every intermediate
state legal. **Also tested, also panics.**

### Five attempts. What is now known rather than suspected

- **This is not an ABI problem.** The hardware spills correctly on overflow, to
  the stack pointer recorded in each frame. `-mabi=call0` does not have to be
  revisited, and saying otherwise in step 17 overstated it.
- **Spilling in task context works and is in the tree** (step 16, verified).
- **Restoring window state inside `_handler_level3` has failed five times**, on
  five different formulations, and the two most principled explanations for why
  have both been tested and eliminated.

What is left is ISA-level detail that cannot be settled from outside: whether
`WSR.WINDOWBASE` in this context needs a specific pipeline interlock, whether
`_WindowUnderflow*` can be reached at all with the frame mid-restore, and what
the processor actually does on the instruction after the write. Answering that
needs single-stepping, which means a debug probe — and UM-NATOS-034 §12.2's
warning applies in reverse here: the heavy instrument was wrong for a config
diff, and it is the right one for this.

### The honest position

`blob_call()` already enforces **one windowed context at a time**, and that
works. A driver task needing a second concurrent one is the thing nat-os cannot
currently host. Whether that is a hard limit or five bad implementations is
genuinely open — the evidence says the mechanism exists and my use of it is
wrong, not that it is impossible.

Tree at the step-16 checkpoint, verified: boot 11/11, `wintorture` correct,
`blobphy` `rc=0`, `wifiinit` `NO_MEM`. `wincollide` still fails, correctly.

**Nothing has been on air.**

---

## Step 19 — a shell bug invalidated four experiments. Corrections, and a real advance.

`split()` truncates the command line **in place** at the first space and returns
the remainder as `arg`. Every two-word command added during this work tested
`str_eq(line, "wifiinit null")` — and `line` is `"wifiinit"`. **None of them
ever matched.** Each silently ran the default path.

Six commands were affected: `wifiinit null`, `wifiinit nvs`, `wifiinit task`,
`osi null`, `blob map`, `blobtx force`. All now test `arg`.

### What has to be withdrawn, and what is reinstated

**Step 9's conclusion was wrong.** Re-run properly:

```
wifiinit        rc=0x101 (NO_MEM)        15 of 118 entries
wifiinit null   rc=0x102 (INVALID_ARG)    0 of 118 entries
```

That is **exactly** what the disassembly said — NULL config, `bnez.n a2`,
`movi a2, 0x102`, return before registering anything, hence zero adapter calls.
The listing was right the whole time. "objdump is misleading about this
function" was a conclusion drawn from a command that never ran.

**Step 9b's withdrawal was also wrong.** `wifi_osi_funcs_register(NULL)`
returns **`0x102`** — it does validate. The original step-6 reading, that the
blob accepted the table, was correct and is reinstated. Which also restores
step 8b: the blob really did detect the stale in-tree table.

**Step 8's `nvs_enable` elimination never happened** — the override was never
applied. That field is untested, not eliminated.

**Step 13's task-creation gate never armed.** `wifiinit task` could not enable
it, so the collision has not been re-observed since the gate was added.

### The advance

With the adapter table installed directly into the blob's own `g_osi_funcs_p`
(after `blob_init`, since `.data` is copied over anything written before it):

```
forced g_osi_funcs_p = 0x3f40761c
calling esp_wifi_80211_tx at 0x4031acb8
tx        returned 0x00003004
phystack  1296 of 6144 bytes used
```

**`esp_wifi_80211_tx` executed and returned** rather than dereferencing null.
`0x3004` is `ESP_ERR_WIFI_IF` — *"WiFi interface error"*. It ran its frame
sanity check, took the global lock through our adapter, and rejected the call
for a specific, sensible reason: there is no configured interface, because the
driver was never started.

That is the transmit path running for the first time.

### The lesson

Four experiments produced confident conclusions from commands that never
executed, and two of those conclusions were used to overturn *correct* earlier
findings. The tell was available throughout — `blobtx force` printed no
"forced" line, and I read the absence as the poke being ineffective rather than
the branch never being taken.

**A negative result from a command whose execution was never confirmed is not a
negative result.** The project already had the rule one level up — "a
successful measurement is not evidence if the measurement and the fault share a
dependency" — and this is the same failure with the instrument being the shell
itself.

**Nothing has been on air.**

### Step 20 — the re-tests, and everything converging on one blocker

With arguments actually reaching the commands:

**`wifiinit task`** — the gate that never armed now does, and the collision
reproduces immediately: `IllegalInstruction`. Step 13's finding was real, not
an artifact. Two windowed contexts still corrupt each other.

**`wifiinit nvs`** — `0x101`, unchanged. `nvs_enable` is now *legitimately*
eliminated rather than eliminated by a command that never ran. It was moot in
any case once the config stopped being rejected.

### Where everything now points

Four independent paths all end at the same place:

| path | where it stops |
|---|---|
| `esp_wifi_init_internal` | `_task_create_pinned_to_core` — needs a task |
| `wifiinit task` | task runs, two windowed contexts, `IllegalInstruction` |
| `esp_wifi_80211_tx` | `ESP_ERR_WIFI_IF` — needs a started driver |
| `esp_wifi_start` | needs init to have succeeded |

There is exactly **one** blocker, and it is not a missing adapter entry, a
config field, or a layout error — all of those are now either fixed or
measured. It is that nat-os cannot host two contexts inside windowed code, and
a driver that owns a task requires it.

Five attempts at the fix have failed, all of them writing window state inside
`_handler_level3`. The two best explanations for why have themselves been
tested and eliminated. What remains is ISA behaviour that needs single-stepping
to observe.

### Honest summary of the route

Everything that could be established from outside the chip has been:

- the blob loads, maps into three windows, and verifies
- the PHY initialises under nat-os's own bootloader, from a kernel containing
  no Espressif code
- the adapter table is correct, validated by the blob itself
- driver init runs, allocates, and rolls back cleanly
- **the transmit path executes and returns a specific, sensible error**

What is left needs either a debug probe, or the acceptance that one windowed
context at a time is the invariant this kernel holds — in which case a vendor
driver with its own task is out of reach and `docs/blob-free.md`'s conclusion
stands on engineering grounds rather than principle.

**Nothing has been on air.**

### Step 21 — spill-before-block: implemented, and it does not fix it

The idea was to avoid `_handler_level3` entirely. A blob task holds the blob
lock while it runs; when it is about to **block** inside blob code, the adapter
spills its window — pushing every live frame to the task's own stack, leaving
exactly one, the call0 steady state the existing switch already handles — then
releases the lock, blocks, re-acquires, and returns. Frames reload through
`_WindowUnderflow*` on the way out.

Only the blocking path pays: `_semphr_take` tries the uncontended take first
and returns without spilling or touching the lock in the common case.

**One real bug found on the way.** `blob_lock`/`blob_unlock` are call0 kernel
functions and were called directly from the windowed adapter — the window
rotated, their `RET` did not rotate back, and the CPU jumped to `0x80247feb`:
bit 31 set, a return-address *encoding* used as an address. The same fault
`window.S` records from the first time this project hit it, and the third time
in this work. Fixed by routing them through `w2c_call0f`.

**It still collides.** `wifiinit task` panics in `w2c_call2` — the same place
as before the change.

The reason is the limitation stated when the idea was proposed: it only covers
**voluntary** blocking. A task preempted by the timer mid-blob-code never
reaches `_semphr_take`, has not spilled, and another context rotates over its
frames. The idea was sound and insufficient, and now measurably so rather than
by argument.

### Kept rather than reverted

No regression in the paths verified: boot 11/11, `wintorture` correct, `blob`
LOAD VERIFIED. It is the correct behaviour for voluntary blocking and will be
needed whenever the involuntary case is solved; reverting would discard the
`w2c_call0f` fix as well.

**Verification is incomplete** — the board disconnected from USB partway
through. `blobphy`, `wifiinit` and `blobtx force` were not re-checked after
this change and should be before anything is built on top.

### What this leaves

Involuntary preemption of a task inside windowed code is the whole remaining
problem, and it can only be handled where the preemption happens — the context
switch. Five attempts there have failed and the two best explanations are
eliminated. That needs single-stepping.

**Nothing has been on air.**

### Step 22 — pin the task instead of teaching the scheduler about windows

**IMPLEMENTED, COMPILES, NOT TESTED** — the board was disconnected from USB
when this was written. Treat every claim below as a design, not a result.

The idea: rather than preserve a windowed frame set across a context switch,
make the situation not arise. While a task is executing windowed vendor code,
`task_schedule()` **declines to switch away from it**.

This is not the old masked-interrupt model, and the difference is the whole
point. Interrupts stay **enabled** — the tick fires, ISRs run, time advances.
Only the *switch* is withheld, which is the narrow thing that actually breaks.

It composes with what was already there, and together they cover both halves:

| case | handled by |
|---|---|
| involuntary preemption mid-blob-code | the pin — the scheduler will not switch |
| voluntary blocking inside blob code | spill + unpin + release (step 21) |

The second is what makes the first bounded. A pinned task monopolises the CPU
only while it is *making progress*; the instant it blocks, the adapter spills
its window, clears the pin and drops the lock, and the scheduler is free again.

The watchdog is fed explicitly at the decline. It is normally fed by evidence
of a task switch, and this is the one case where not switching is correct
rather than a symptom — without that, the hang detector would reset a blob call
that is working.

### What it costs, if it works

Scheduling latency for the length of a blob call that never blocks.
`register_chipv7_phy` is the worst known case at well under a second, and
`next_moves/04` already measured a 125 ms flash erase as a larger stall that
the system tolerates. It is a real cost and it is bounded and measurable.

### What has to be checked before believing any of it

- boot self-tests, `wintorture`, `blob`, `blobphy`, `wifiinit`, `blobtx force`
  — none re-run since step 21, let alone since this
- **`wifiinit task`**, which is the actual question
- that `blob_pinned_task()` is cleared on every exit path, including the one
  where `esp_wifi_init_internal` fails and unwinds
- that a pinned task which never blocks and never returns cannot wedge the
  system permanently — the watchdog is fed at the decline, so it would not be
  caught

**Nothing has been on air.**

### Step 23 — the pin, tested. It works, and it cannot be enough.

**Tested now the board is back. No regression:** boot 11/11, `wintorture`
correct, `blob` LOAD VERIFIED, `blobphy` `rc=0`, `wifiinit` `NO_MEM`,
`blobtx force` `ESP_ERR_WIFI_IF`.

**First run wedged**, exactly as predicted: init pinned the caller, created the
blob's task, waited for it, and the pin prevented the task it was waiting for
from ever running. The watchdog, being fed at the decline, could not catch it.

Two fixes followed. `_queue_recv` got the same spill-unpin-release treatment as
`_semphr_take` — a blocking wait must release the pin or it deadlocks against
the thing it is waiting for. And the pin is now **bounded**: after ~2 s of
consecutive declines the watchdog stops being fed, so a blob call that is not
making progress is caught rather than running forever. The pin still holds past
the bound, because switching away would corrupt the window either way.

**Second run does not wedge — it collides again**, `IllegalInstruction` in
`w2c_call2`.

### Why this is a structural answer rather than another bug

The tension is exact and does not depend on implementation quality:

- To stop involuntary preemption corrupting the window, the running context
  must be **pinned**.
- To let the driver's own task run — which init waits for — the pin must be
  **released**.
- The moment it is released, two contexts are inside windowed code, which is
  the original problem.

Spilling narrows it but cannot close it: a context can spill every frame except
the one it is currently executing in, and that last frame is exactly what
another context's rotation lands on.

So the pin removes *involuntary* preemption cleanly, and the driver's design
requires *voluntary* concurrency. **A vendor driver that owns a task cannot be
hosted without per-task `WINDOWBASE`/`WINDOWSTART`** — the thing five attempts
at `_handler_level3` failed to deliver.

That is a real conclusion, reached by testing rather than by giving up, and it
is worth more than another attempt: it says the remaining work is *specifically*
window-aware context switching, not a cleverer way to avoid it.

### Kept

No regression, and both mechanisms are correct within their scope — the pin for
involuntary preemption, the bound for the wedge it would otherwise permit. Both
are prerequisites for any eventual fix rather than alternatives to it.

**Nothing has been on air.**

---

## Step 24 — the blocking set is closed, and the spill was the wedge

Step 23 concluded that a vendor driver owning a task "cannot be hosted without
per-task `WINDOWBASE`/`WINDOWSTART`". **That conclusion was wrong**, and the
thing that overturned it was the question it had skipped: *which* windowed
regions can actually block?

### The blocking set is small and closed

Every blocking entry in the adapter routes through one function, `wait_on()` in
`kernel/wifi_osi_impl.c`. That makes the set enumerable rather than open-ended:

| adapter entry | reaches | spills + releases |
|---|---|---|
| `_semphr_take` | `sem_take` → `wait_on` | yes |
| `_queue_recv` | `queue_recv` → `wait_on` | yes |
| `_mutex_lock` | `sem_take` → `wait_on` | yes |
| `_queue_send` / `_to_back` / `_to_front` | `queue_send` → `wait_on` | yes |
| `_event_group_wait_bits` | `evt_wait` → `wait_on` | yes |
| `_task_delay` | `task_sleep` | not yet |

Each takes the non-blocking path first and only spills and releases the lock
when it is genuinely about to wait — so a wait costs a spill, and a hit costs
nothing.

### The gap the question exposed

`rom_call3` did not take the lock at all. It called `blob_pin()`, and a pin only
*records* which task must not be switched away from — a second caller simply
overwrote the first, so both entered windowed code and collided anyway. The
exclusion that step 23 assumed was in force had never been in force.

Two defects were hiding behind that, and both were measurement failures rather
than design failures:

1. **The change had never been emitted.** The edit targeted text that a previous
   revert had removed, so it silently did nothing, and two rounds of "pinning
   does not help" were measurements of an unmodified binary. Confirming the
   instruction in the disassembly *before* testing is now the rule.
2. **`call0` clobbers `a2..a11`** — which is precisely `rom_call3`'s target and
   its three arguments. Taking the lock destroyed them before use. The frame
   grew to 48 bytes and the arguments are stacked across the call.

### The pin bound was counting the wrong thing

`g_blob_pin_ticks` counted *consecutive ticks on which the current task happened
to be pinned*. Two tasks taking short turns inside the blob accumulate that just
as fast as one wedged task does, so a perfectly healthy workload tripped the
bound after two seconds and died on the watchdog. The bound is now **per pin**:
`blob_pin()` bumps a sequence number and the scheduler resets the budget when it
changes.

### The spill was the wedge

With exclusion genuinely in force, `wincollide` stopped corrupting and started
*hanging* — one `a` on the trace, no matching `A`, the first task entering the
windowed call and never leaving, its pin then starving every other task.

Bisected to one instruction: `call8 win_spill_all` inside `rom_call3`. Removing
it turned a watchdog reset into **156 runs, zero wrong checksums**.

The spill was compensating for the absence of exclusion — it pushed a previous
task's live frames out before rotating over them. Once the lock exists that case
cannot arise: the only way to hold live windowed frames is to hold the lock, and
a task that released it has already unwound them. Keeping the spill was not
merely redundant; called from a freshly spawned task it never returned.

`win_spill_all` itself stays. The blocking adapter entries use it for a
different purpose — reducing a *blocked* task to one live frame — from inside
windowed code where frames genuinely are live.

### Measured

```
boot            11 PASS 0 FAIL
wintorture 60   checksum 1632 expected 1632  CORRECT, 6 switches during the call
wincollide      runs=156  wrong=0
blob            image 606404 bytes, tx entry 0x4031acb8
blobphy         rc=0, 1296 of 6144 bytes of private stack
wifiinit        0x00000101 (ESP_ERR_NO_MEM), 15 of 118 adapter entries called
blobtx force    0x00003004 (ESP_ERR_WIFI_IF)
```

`wintorture`'s switch counter is still the control: 6 real preemptions occurred
with eight windowed frames live, so the CORRECT is evidence rather than an
absence of opportunity.

### Open, and not attributed

`blobtx force` returns `ESP_ERR_WIFI_IF` and the shell answers commands
afterwards, but the board resets on the hang detector **3.5 seconds later**.
There is no baseline for what happened more than three seconds after that
command before this change, so it is recorded as unattributed rather than
assumed pre-existing.

### What this changes

Step 23's "specifically window-aware context switching" is no longer the
remaining work. Exclusion plus a closed blocking set delivers what the driver
needs without per-task `WINDOWBASE`/`WINDOWSTART`, and without disturbing the
`-mabi=call0` decision. The next test is the one step 23 declared impossible:
`blob_task_enable(1)`, with the driver owning a real task.

**Nothing has been on air.**

---

## Step 25 — the blob task's failure is a stack clobber, and an open regression

### What `wifiinit task` actually fails on

Enabling blob task creation still panics, but the panic is now explained rather
than merely located. Phase markers through the blocking stub gave `1-Ssu`:
entered, found the semaphore contended, **spilled successfully**, **released the
lock successfully** — then the blocking `w2c_call2` never came back.

Extending the panic dump with the two registers that decide whether a windowed
instruction is legal, then with the values at the `retw` itself:

```
windowbase: 3   windowstart: 0x0000000a   bit(base) SET
ps at retw: 0x00060d20   EXCM clear
a0 at retw: 0x00000000   callinc n=0   sp: 0x00000000   task: 6
ws before repair: 0x00000000
```

`a0`, `a1` and `WINDOWSTART` are **all zero**, and the faulting task is the
shell — the task that called `blob_call` and was sitting blocked. That is not a
register-window bug at all. A blocked task's saved context was overwritten with
zeros while it waited, and `retw` on a zeroed `a0` (n=0) is illegal, which is
merely the first instruction unlucky enough to notice.

The cause is a resource limit that was being counted instead of enforced:
`blob_task_create` recorded requests exceeding `TASK_STACK_WORDS * 4` (2 KB) in
`g_bt_short` and then created the task anyway. An Espressif WiFi task asks for
several KB, and a task handed a quarter of what it asked for does not fail where
the mistake is — it runs off its slot and writes through its neighbours.

It now **refuses**, returning pdFAIL, which the driver already handles by
unwinding and reporting NO_MEM. A shortfall should read as a resource limit.

### A hypothesis that measurement killed

Before the register dump, the fault looked like a cleared `WINDOWSTART` bit, and
a `WSTART_REPAIR` macro was added to all four `w2c` bridges to set the current
frame's bit before returning. It was emitted, it ran, and the fault did not
move. `ws before repair: 0x00000000` is why: the whole register was zero, not
one bit of it. The macro has been removed rather than left in as insurance
against something it does not fix.

### A defect this introduced

`rom_call4` acquired a `blob_unlock` with **no matching `blob_lock`** — a
single-shot text replacement matched the first identical return sequence in the
file, and `rom_call4` sits above `rom_call3`. Every `rom_call4` return therefore
cleared whatever pin was held and called `mutex_unlock` on a mutex it did not
own. Fixed; both functions were then checked in the disassembly rather than in
the source (`rom_call3`: 1 lock / 1 unlock, `rom_call4`: 0 / 0).

### The `blobphy` regression, bisected to the layout band

`blobphy` hung deterministically, and bisection against HEAD found the cause to
be nothing that runs:

| tree | `blobphy` |
|---|---|
| HEAD | OK rc=0 |
| + `window.S`, `blobcall.c/.h`, `task.c` (the whole lock/spill/pin core) | OK rc=0 |
| + `wifi_osi_stubs.c` (the three added blocking entries) | OK rc=0 |
| + `panic.c`, `shell.c`, `wincollide.c` (print-only) | **hang** |
| + `shell.c` alone | **hang** |

Nine lines of `uart_puts` in `shell.c` — code that does not execute during
`blobphy` — flip it. That is step 7's layout band, and this is the first minimal
reproducer for it.

The mechanism now has a candidate. `kernel/linker.ld` places `.flash.text` in
`irom` with **`shell.c` first**, so `shell.c`'s size shifts everything the flash
MMU maps — and `blob_map()` reprograms that MMU. `blob.c` is deliberately in
IRAM for exactly this reason ("it cannot be fetching its own next instruction
through the thing it is changing"); the shell is not, and it is the caller.
That is a hypothesis with a named mechanism, not yet a proof.

Resolved for now by dropping the diagnostic print, which was the only added
flash-resident code. Every real fix is kept. The band itself remains open, and
it is now cheap to reproduce on purpose.

### Still measured good

```
boot           11 PASS 0 FAIL
wintorture     CORRECT
wincollide     runs=146  wrong=0
blobphy        rc=0
wifiinit       0x00000101 (ESP_ERR_NO_MEM)
blobtx force   0x00003004 (ESP_ERR_WIFI_IF), then resets 3.5 s later -- the
               same unattributed item as step 24, unchanged by any of this
```

The step-24 result stands. **Nothing has been on air.**

---

## Step 26 — variable task stacks, and a wrong diagnosis corrected

### The request, measured

`esp_wifi_init_internal` asks for **6656 bytes** of task stack. nat-os gives
every task a fixed `TASK_STACK_WORDS * 4` = 2048.

The measurement had to be printed from `blobcall.c` rather than from the shell:
`shell.c` is the first object in `.flash.text`, so anything added there shifts
what the flash MMU maps and walks into the step-25 layout band. `blobcall.c` is
not flash-resident, so the same print is free. That is the band being worked
*around* deliberately rather than tripped over.

### Variable stacks

Scaling the pool to fit would cost 12 x 6656 = 78 KB against a heap of 84, so the
size belongs with the one caller that needs it:

- `task_create_with_stack(name, entry, stack, words)` — the general form. The
  caller supplies the memory; it must outlive the task, so in practice a static
  buffer.
- `task_create()` delegates to it with the slot's pool stack.
- `task_t` gained `stack_words`, and `task_stack_headroom()` now bounds its scan
  by *that task's* size rather than by the pool constant — otherwise a task on a
  supplied stack would report headroom from whatever follows it in DRAM.
- `blobcall.c` owns one 7168 B stack for the one task the driver creates, with a
  `_Static_assert` against the measured 6656. A second concurrent blob task
  wanting more than a pool stack is refused and counted, not squeezed.

Cost: heap 84456 -> 77208 B. Boot stays 11 PASS 0 FAIL.

### The diagnosis this corrects

Step 25 attributed the zeroed context to the stack shortfall: a task handed 2 KB
when it asked for 6.5 KB overruns its slot and writes through its neighbours.
That was plausible, and it is wrong.

With a stack that genuinely fits, the fault is unchanged:

```
exccause 0 (IllegalInstruction)   windowbase: 3   windowstart: 0x0000000a  bit(base) SET
a0/sp out : 0x00000000 / 0x00000000   BOTH ZERO -- context clobbered
```

So the stack was a real defect and a real limit — worth fixing on its own terms —
but it was not the cause of the clobber. Something else zeroes a blocked task's
context while the blob task runs.

The primary corruption is `a1`, not `a0`: the bridge reloads `a0` with
`l32i a0, a1, 0`, so a zero stack pointer produces a zero return address for
free. The shape to chase is a task resuming on a **freshly created task's frame**
— `task_create()` zeroes `TASK_FRAME_WORDS` at the top of the new stack, which
is exactly a frame of zeros — i.e. a saved-`sp` aliasing bug, not a register
window bug.

**Nothing has been on air.**

---

## Step 27 — the clobber is the shared PHY stack, re-opened by the lock release

Dumping the task table at the panic ends the guessing:

```
task 5 (shell)  sp 0x3ffbfe00   stack 0x3ffb9e14+2048   guard ok
task 9 (blob)   sp 0x3ffb23d0   stack 0x3ffb0974+7168   guard ok
   ... all twelve slots: guard ok
```

Two things follow immediately. **No stack overflowed** — every guard word is
intact, which retires the step-25 stack-shortfall theory for good. And the blob
task is correctly on its new 7168 B stack, so step 26's fix does what it claims.

The shell's saved `sp` is `0x3ffbfe00`, nowhere near its own 2 KB stack. From the
symbol table:

```
3ffbe800 B _phy_stack
3ffc0000 B _phy_stack_top
```

`0x3ffbfe00` is inside `_phy_stack`, 512 bytes below its top. The shell blocked
while running on the **shared private PHY stack** — and, by the blocking-path
design added in step 24, it *released the blob lock* to do so.

That re-opens precisely the hazard this file's own header says the mutex exists
to close:

> THE PRIVATE STACK. `_phy_stack` is a single shared 6 KB buffer. Two contexts
> entering phy_stack_call would corrupt each other whatever the window did.

The mutex was the only thing keeping a second context off that buffer. Releasing
it on the blocking path lets the blob task in **while the first context is still
parked there**, with live frames and a saved switch frame on it. `_phy_stack` has
no guard word, which is exactly why every task guard reads OK while the resumed
context comes back as zeros.

So the sequence of diagnoses ran: register window (wrong) -> stack shortfall
(wrong, though a real defect) -> **shared PHY stack, unprotected during a
voluntary block**. Each was retired by a measurement rather than by argument, and
the last one is consistent with all of them: zeroed `a0`/`a1`, zeroed
`WINDOWSTART`, and intact guards everywhere.

### What this implies for the design

The step-24 rule "release the lock whenever you are about to block" is sound for
the *window*, and unsound for the *stack*. A context may only release the lock if
nothing it leaves behind is shared — and the PHY stack is shared by construction.

Two directions, neither tested:

1. **Per-context PHY stacks.** Costly (6 KB each) but removes the sharing.
2. **Do not run blockable code on the private stack at all.** `phy_stack_call`
   exists for PHY init, which does not block. The driver's own task already runs
   on its own stack through `rom_call3`; `esp_wifi_init_internal` is the one
   blockable call still routed through `blob_call` -> `phy_stack_call`.

(2) looks much cheaper and matches what the private stack was actually for.

**Nothing has been on air.**

---

## Step 28 — option 2 done; it removed the sharing and did not fix the fault

`blob_call()` now enters the blob through `rom_call4` instead of
`phy_stack_call`, so a blockable driver call runs on the **caller's own task
stack**. `blob_call` has exactly one caller, so this touched no flash-resident
file and stayed clear of the step-25 layout band.

It does what it was meant to do. The shell now blocks at `sp 0x3ffba200`, inside
its own stack (`0x3ffb9e14+2048`), rather than at `0x3ffbfe00` on the shared
`_phy_stack`. The buffer is no longer occupied by a parked context, and the
private stack keeps only the job it was built for.

The fault is unchanged:

```
exccause 0 (IllegalInstruction)  windowbase: 3  windowstart: 0x0000000a  bit SET
a0/sp out : 0x00000000 / 0x00000000   BOTH ZERO
all twelve task stack guards: ok
```

So the shared PHY stack was a third real defect on this path -- and not the
cause either. Three hypotheses have now been retired by measurement:

1. cleared `WINDOWSTART` bit — disproved: the whole register was zero
2. blob task stack shortfall — disproved: fault identical with 7168 B, guards ok
3. shared `_phy_stack` — disproved: fault identical with a private stack, and the
   shell verifiably no longer parks on the buffer

Each was a genuine bug worth fixing on its own terms. None was this one.

### What is left

The corruption is not in memory. Every task guard is intact, the stack is now
private, and the values still come back as zeros — so the damage is in the
**register restore across a changed `WINDOWBASE`**, which is where step 23
pointed before the exclusion work began.

The specific thing to test next: the level-3 handler saves and restores sixteen
registers *at whatever `WINDOWBASE` currently is*, and `WINDOWBASE` is global. A
task switched out at base X and resumed after another context has moved the base
to Y has its registers restored at Y, while its spilled caller frames and the
`retw` encoding in `a0` still describe X. That is precisely the per-task
`WINDOWBASE`/`WINDOWSTART` gap, and it is now the only candidate left standing.

**Nothing has been on air.**

---

## Step 29 — the memory is fine; the damage is in the window state

The faulting task's saved switch frame, read out of memory at the panic:

```
saved frame @ 0x3ffba200: 0x40087abb 0x00000000 0x00060d20 0xfffffff0 0x00000700 0x3ffb0920 ...
task 5 sp 0x3ffba200 stack 0x3ffb9e14+2048 guard ok
```

Word 0 is the saved `a0`: `0x40087abb`, a valid code address. The frame is
**intact**. It was saved correctly, it sits inside the task's own stack, and the
guard above it is untouched.

So the zeros are not in memory. They exist only in the live registers at the
`retw`. Combined with steps 26-28 that closes the question of *where*:

| candidate | verdict | evidence |
|---|---|---|
| cleared `WINDOWSTART` bit | no | whole register was zero, not one bit |
| blob task stack shortfall | no | identical fault at 7168 B, all guards ok |
| shared `_phy_stack` | no | identical fault on a private stack |
| saved frame corrupted in memory | **no** | frame reads back intact |
| register/window state on resume | **only one left** | zeros are live-register-only |

Four memory explanations, each measured and each retired. `a1` is restored from
a good frame and is nevertheless zero by the time the bridge returns, which
means it is being changed by a **window rotation between the restore and the
`retw`** -- the handler puts sixteen registers back at whatever `WINDOWBASE`
happens to be, and `WINDOWBASE` is global and no longer the one this task was
saved at.

That is per-task `WINDOWBASE`/`WINDOWSTART`, precisely as step 23 said, and it
is now the last thing standing rather than the first thing assumed.

### Why it is tractable now, and was not then

Step 23's five attempts had to make context switching window-aware for *any*
task at *any* moment. Two results since have shrunk that problem:

- The **pin** means an involuntary switch never happens while a task holds live
  windowed frames. Timer preemption is out of scope entirely.
- The **spill** on the blocking path means a task that blocks voluntarily has
  exactly **one** live frame.

So the switch does not need to save a window; it needs to save two registers for
the one case that remains, and restore them before the sixteen registers go
back. `TASK_FRAME_WORDS` grows by two, `task_create()` seeds a canonical base,
and the restore sets `WINDOWBASE` before `a1` is reloaded -- which is the part
that has to be written carefully, because changing `WINDOWBASE` changes which
physical register `a1` *is*.

**Nothing has been on air.**

---

## Step 30 — per-task window state: implemented, no regressions, fault moved

`WINDOWBASE` and `WINDOWSTART` are now part of a task's context.

- `TASK_FRAME_WORDS` 21 -> 23, `TASK_FRAME_BYTES` 96 -> 112.
- `_handler_level3` saves both alongside the LOOP registers, and restores them
  **before** any general register, so the sixteen registers land at the base the
  task was saved at rather than at whatever the previous context left behind.
- Writing `WINDOWBASE` changes which physical register `a1` *is*, so the frame
  pointer is parked in `g_switch_sp` — addressed by an `l32r` literal, which
  survives the base change even though no register does — and reloaded after.
- `task_create()` seeds a new task with the creator's base and
  `WINDOWSTART = 1 << base`: exactly one live frame, its own, with no inherited
  claim on the creator's frames.

Only two words are needed rather than a spilled window, because the pin means an
involuntary switch never lands mid-window and the blocking path spills to a
single frame first.

### No regressions

```
boot         11 PASS 0 FAIL
wintorture   CORRECT
wincollide   runs=128  wrong=0
blobphy      rc=0
```

That is the whole scheduler exercised, including the two windowed stress tests,
across a change to every context switch in the system.

### The fault moved

`wifiinit task` no longer dies in `w2c_call2` with a zeroed context. It now dies
inside the handler's own restore:

```
exccause 29 (StoreProhibited)   epc 0x40088cbb   excvaddr 0x000004a2
```

`0x40088cbb` is `l32i.n a8, a1, 28` in the register-restore block, with `a1`
around `0x486` — garbage. So the mechanism is live and doing its job for every
ordinary task, and **some task's saved window state is invalid**: a restored
`WINDOWSTART` claiming frames whose physical registers another context has since
reused would produce exactly this, an overflow spilling through a meaningless
stack pointer.

The invariant the two-word design rests on — *a task is only ever switched away
from with exactly one live frame* — is currently **assumed rather than
enforced**. The next step is to stop assuming it: count the bits in
`WINDOWSTART` on the save path and record any task switched out with more than
one, which turns the assumption into a measurement.

### Noticed in passing

`osi_s_task_delay` is an empty stub. A driver asking to sleep gets an immediate
return, so any wait loop built on it becomes a busy spin. Not the cause of
anything here, but it is wrong and it is counted rather than implemented.

**Nothing has been on air.**

---

## Step 31 — the invariant is false, and the reason is structural

`task_schedule()` now counts the bits in each outgoing task's saved
`WINDOWSTART`. The design says that number is always one. It is not:

```
multiframe: 14 switch-outs with >1 live frame, worst 7 frames,
            last task 6 ws 0x0000e3c0
```

Task 6 is `disp` — an ordinary call0 task that never executes a windowed
instruction — saved with **seven** live frames. `0x0000e3c0` is bits 6, 7, 8, 9,
13, 14 and 15, and none of them are its own.

### Why, and why it is not a bug in the counting

`WINDOWSTART` is a single global register describing the whole 64-register
physical file, and that file holds frames belonging to **several tasks at once**.
When the blob's windowed code leaves frames live and the scheduler switches to
`disp`, those frames are still in the register file and still marked live —
correctly. `disp` has one frame of its own and inherits a claim on six it has
never touched, because there is nowhere else for that claim to live.

This is what steps 30's garbage stack pointer was: restoring a per-task
`WINDOWSTART` writes one task's view of the register file over everyone's, so
either the restoring task's frames are lost or another task's are resurrected
against physical registers that have since been reused. A rotation then spills
through whatever `a1` those stale frames contain.

### What this actually settles

Per-task `WINDOWBASE`/`WINDOWSTART` is **not sufficient on its own**, and the
two-word design cannot be rescued by fixing the seeding or the ordering. The
register file is shared, so either:

1. **Every switch-out spills that task's windows to its own stack**, leaving the
   file genuinely empty of its frames — which is what FreeRTOS/ESP-IDF do, and
   what `win_spill_all` exists for. Then one bit per task is true rather than
   assumed, and per-task `WINDOWSTART` becomes meaningful.
2. Or `WINDOWSTART` stays global and is never restored per task — which is the
   pre-step-30 behaviour, and fails for a task that blocks inside windowed code.

(1) is the real answer and always was. The pin already removes the involuntary
case, so the spill only has to happen on the voluntary path — which is exactly
where the blocking stubs already call `win_spill_all`. The measurement says that
spill is either not reducing to one frame or not covering every path that
reaches a switch.

The counter stays. It is the first thing in this whole sequence that can say
whether a window fix worked without needing the WiFi driver to be the test.

### Kept

Per-task window state is retained: it is necessary but not sufficient, boot is
11 PASS 0 FAIL with it, `wintorture` CORRECT and `wincollide` 128/0. Removing it
would only put back a different half of the same problem.

**Nothing has been on air.**

---

## Step 32 — the spill works; WINDOWSTART is OR-ed; and a correction

### win_spill_all does exactly what it claims

Measured directly, with no blob and no driver in frame — eight live windowed
frames, spill, read `WINDOWSTART` either side:

```
spill: windowstart 0x0000aa8a (7 frames) -> 0x00000008 (1 frames)
spill reduces to ONE frame as designed
```

So the spill is correct, and every failure since step 24 has to be explained
without blaming it. This is also the first instrument here that tests the window
machinery without needing the WiFi driver to be the experiment.

### WINDOWSTART is now OR-ed rather than assigned

A restoring task may claim only the ONE frame it is being restored into.
Everything else in the register file belongs to a task still using it, so the
restore now does `WINDOWSTART |= 1 << saved_WINDOWBASE` and leaves the rest
alone, instead of writing a saved word that resurrects frames whose physical
registers have since been reused.

No regressions: boot 11 PASS 0 FAIL, `wintorture` CORRECT, `wincollide`
runs=131 wrong=0, `blobphy` rc=0.

### Correction to step 31

Step 31 said the one-live-frame invariant is false. **That claim was stronger
than the instrument supports.** The counter reads `WINDOWSTART`, which is global
and describes the whole register file, so it cannot tell whose frames it is
counting. `disp` "saved with seven frames" almost certainly means *some task was
legitimately mid-excursion at that moment* — not that `disp` owned seven frames.

The structural point in step 31 stands: a per-task copy of a global register
cannot be written back wholesale, which is why the OR above is the right shape.
The specific claim that the invariant is violated does not stand, and the
counter needs to mask against the restoring task's own base before it can say
anything about ownership.

### Where it now fails

```
exccause 0 (IllegalInstruction)   epc 0xc008aaae   windowbase 13  windowstart 0x00002802
```

`epc` has **bit 31 set**. That is the signature this project has now hit four
times: a windowed return-address encoding (`a0 = (n<<30) | offset`, here n=3, so
CALL12) jumped to as a raw address. The real target is `0x4008aaae`. It is an
ABI-mixing fault -- call0 code returning through a windowed `a0`, or the reverse
-- not a window-bookkeeping fault, and it is a different bug from the zeroed
context that preceded it.

**Nothing has been on air.**

---

## Step 33 — the OR-restore loses a task's OWN frames, and that closes the argument

`0xc008aaae` decodes to `0x4008aaae` with bits 31:30 forced to `11` — a CALL12
return encoding on an address that is the `retw.n` at the **second nested level
inside `win_spill_all` itself**.

Every call site is identical:

```
call8 4008aaa0 <win_spill_all>      x5, including the probe that works
```

So the entry path is not the difference. The probe spills correctly from eight
clean frames; the adapter stub faults spilling from a frame the scheduler has
restored. The only thing that differs is the **window state on entry**, which
step 32 changed.

### The flaw in OR-ing

`WINDOWSTART |= 1 << saved_base` was introduced so a restoring task cannot
destroy another task's live frames. It succeeds at that and fails at the
converse: it does not restore the task's **own** frames either.

A task that was spilled down to one frame before blocking is fine — one bit is
the whole truth about it. A task that reaches a switch with several live frames
gets exactly one bit back, and its caller frames are then marked dead while
still living only in physical registers that another context is free to reuse.
The next `retw` finds no live caller, takes an underflow, and reloads from a
stack slot that was never written — which is a CALL12-encoded word landing in
PC. Hence the signature.

Assignment resurrects other tasks' frames. OR-ing discards your own. **There is
no correct way to write a per-task copy of a global register back**, and that is
not a bug to be fixed by choosing a better expression — it is the reason
step 31's option (1) was always the answer:

> Every switch-out spills that task's windows to its own stack, leaving the file
> genuinely empty of its frames. Then one bit per task is TRUE rather than
> assumed, and per-task `WINDOWSTART` becomes meaningful.

With one frame per task guaranteed, assignment and OR-ing become the same
operation, and both are correct.

### Why this is now reachable

Spilling inside `_handler_level3` failed five times (steps 14-18) and is still a
bad idea. It no longer has to happen there:

- The **pin** means an involuntary switch never lands while a task holds live
  windowed frames, so the ISR path needs no spill at all.
- Every remaining switch out of windowed code is **voluntary**, and spilling in
  task context is measured to work — `win_spill_all` reduces 7 frames to 1.

So the work is to guarantee that *every* voluntary block from inside windowed
code spills first. The adapter's blocking entries already do. Ordinary kernel
blocking — `mutex_lock`, `task_sleep`, `task_block` — does not, and a task
inside windowed code that blocks through one of those is the remaining hole.

That is a small, well-defined change with an instrument already built to check
it: the multiframe counter, once masked to the restoring task's own base.

**Nothing has been on air.**

---

## Step 34 — spill before parking: 33 multiframe switch-outs down to 2

`task_block()` and `task_sleep()` now leave the register file holding exactly one
frame for the parking task, via a new call0-callable bridge:

- `win_spill_call0` in `window.S` — the same shape as `rom_call3` with no target
  and no lock. call0 code cannot call the windowed `win_spill_all` directly;
  that is the CALL8/call0 mismatch this file has recorded four times, from the
  other direction.
- `spill_before_parking()` in `task.c`, guarded by `ws & (ws - 1)`, so a plain
  call0 task with nothing windowed in flight pays a single register read.

Done in **task context**, not in `_handler_level3` where five attempts failed.
It does not need to be in the handler: the pin means an involuntary switch never
lands mid-window, so every switch out of windowed code is voluntary and passes
through here.

### Measured

```
boot         11 PASS 0 FAIL
wintorture   CORRECT
wincollide   runs=123  wrong=0
blobphy      rc=0
multiframe   33 -> 2 switch-outs with >1 live frame
```

Thirty-three down to two, with no regression anywhere. The mechanism works; what
is left is a hole in its coverage rather than a fault in its design.

### The remaining two

```
exccause 0 (IllegalInstruction)  epc 0xc008aad6  ps 0x00030210
a0/sp out : 0x8008bbca / 0x00000000   (a0 CALL8-encoded, sp still zero)
saved frame @ 0x3ffba200: 0x40087bc9 0x00000000 0x00060f20 0xfffffff0 0xc008aade ...
multiframe: 2 switch-outs with >1 live frame, worst 8 frames, last task 7
```

`0xc008aad6` is `0x4008aad6`, inside `win_spill_all` itself, and word 4 of the
saved frame is `0xc008aade` — a CALL12-encoded return address sitting in saved
register state. Two switch-outs still park with eight live frames, and task 7 is
`touch`, which does not run windowed code: consistent with the counter's known
limitation (it reads the global register and cannot attribute ownership), so the
eight frames are somebody else's, still in flight.

The candidates for the hole, in order:

1. A block that reaches a switch **without** going through `task_block` or
   `task_sleep` — `mutex_lock` calls `task_block` then `task_yield`, but any path
   that yields directly while windowed is uncovered.
2. `spill_before_parking()`'s guard is on the **global** register, so it can skip
   a spill when this task has one frame and another has several — which is
   correct — but it can also spill a task whose frames are not the multiple ones,
   which is wasted rather than wrong.
3. The counter still cannot say *whose* frames it counted. Masking it to the
   restoring task's own base is the prerequisite for reading the last two events
   properly, and is now the cheapest next move.

**Nothing has been on air.**

---

## Step 35 — PS.WOE is clear, which is why a correct spill faults

### Option 1, closed in one line

`spill_before_parking()` now also runs in `task_yield()`, so every voluntary
switch point is covered without having to find the one that was missed.
Multiframe switch-outs **2 -> 1**; boot 11 PASS 0 FAIL, `wintorture` CORRECT,
`wincollide` runs=129 wrong=0, `blobphy` rc=0. As expected it did not move the
panic — the remaining event is task 6 `disp`, which runs no windowed code, so
the counter is reporting somebody else's frames exactly as its known limitation
predicts.

### The actual cause of the IllegalInstruction

```
ps : 0x00030210
```

decodes to INTLEVEL 0, EXCM 1, OWB 2, CALLINC 3, and **WOE = 0**.

With `PS.WOE` clear, every windowed instruction — `entry`, `retw` — raises
IllegalInstruction *by definition*. So the fault inside `win_spill_all` was
never a fault in `win_spill_all`: the standalone probe proves that routine
correct (7 frames -> 1). It was being executed in a processor state in which no
windowed instruction can be legal.

That also retires the reading in step 33. The CALL12-encoded `epc` is not
evidence of a raw jump to an encoded return address; it is `0x4008aae2` with
CALLINC=3 in the high bits, which is what an `epc` looks like when the window
machinery is disabled underneath windowed code.

### One real defect found, and it was not this one

`osi_s_wifi_int_restore` wrote the **whole** word into PS. IDF's counterpart
reaches `XTOS_RESTORE_JUST_INTLEVEL` — the name is the specification — and
restores only the interrupt level. Writing the whole word trusts the driver to
hand back a well-formed PS, and any value with WOE clear disables the window
machinery from that moment on. Now merged: `(ps & ~0xF) | (tmp & 0xF)`.
`phy_exit_critical` had the same shape and got the same fix.

Correct on its own terms, and **not** the source: PS is still `0x00030210`
afterwards.

### Where WOE goes, next

Remaining writers of PS on this path:

1. `phy_stack_call`'s masking (`window.S`), which saves and restores PS around
   the call — and which **also assigns `WINDOWSTART` outright**, the same
   clobber pattern that was wrong in the handler and is still wrong here.
2. The blob's own code. It is entitled to run `wsr.ps`, and nothing currently
   checks what it leaves behind.

The cheap instrument is the one that has worked all session: sample PS at the
entry and exit of each bridge and record the first transition where WOE goes
from 1 to 0. That names the writer instead of inferring it.

**Nothing has been on air.**

---

## Step 36 — the blob never clears WOE; the fault is inside an exception context

The WOE watch instruments `osi_hit()`, which every one of the 118 adapter
entries passes through — the whole blob/kernel boundary in one place:

```
woe watch : never seen clear at an adapter entry   good crossings 15
```

**WOE is set at every crossing.** The blob is not clearing it, and neither is
anything on the kernel side of that boundary. Step 35's shortlist is wrong: it
is neither `phy_stack_call`'s masking nor the blob's own `wsr.ps`.

### What the PS actually says

```
ps 0x00030210   ->  INTLEVEL 0, EXCM 1, UM 0, OWB 2, CALLINC 3, WOE 0
```

`EXCM=1` **with** `UM=0` **and** `WOE=0` is not the state of ordinary task code
that has lost a bit. It is the state a processor is in while running an
**exception handler** — specifically a window overflow/underflow vector, which is
entered with EXCM set and is why those handlers use `s32e`/`l32e` rather than
ordinary windowed instructions.

The faulting instruction is the `retw.n` at `win_spill_all+0x0e`, and the
vectors are installed where they should be:

```
40080000 _WindowOverflow4    40080040 _WindowUnderflow4
40080080 _WindowOverflow8    400800c0 _WindowUnderflow8
40080100 _WindowOverflow12   40080140 _WindowUnderflow12
```

So the sequence is: `win_spill_all`'s `call12` forces a window overflow, the
overflow vector is entered with EXCM set, and control reaches the `retw` **with
EXCM still set** — at which point a windowed instruction is illegal by
definition. The spill is correct, the vectors exist, and the fault is in what
happens between them.

This also joins up with step 30's `StoreProhibited excvaddr 0x000004a2`: a
window vector spilling through a meaningless stack pointer is a fault *inside*
an exception handler, which is the other half of the same story.

### Next

The window vectors themselves, which this project has never examined:

1. Do `_WindowOverflow4/8/12` and `_WindowUnderflow4/8/12` end with `rfwo`/`rfwu`
   — the instructions that clear EXCM and complete the rotation? A handler that
   returns any other way leaves EXCM set exactly as observed.
2. Is the spill destination valid for the frames `win_spill_all` forces out? The
   handler writes through the frame's own `a1`, and step 30 saw it write to
   `0x4a2`.

`vendor/phy/MAC-STATE.md` and `window.S` both assume these vectors are correct
because windowed code has worked; every windowed test so far has stayed shallow
enough not to overflow, so that assumption has never actually been exercised.

**Nothing has been on air.**

---

## Step 37 — the vectors are correct; three latent bugs fixed; fault unmoved

### The window vectors are fine

All six are canonical Xtensa implementations and each ends in `rfwo`/`rfwu`, the
instructions that clear EXCM and complete the rotation. The double-exception
vector is installed too:

```
40080000 _WindowOverflow4    40080040 _WindowUnderflow4
40080080 _WindowOverflow8    400800c0 _WindowUnderflow8
40080100 _WindowOverflow12   40080140 _WindowUnderflow12
400803c0 _vector_double      40080340 _vector_user
```

Step 36's first hypothesis — a handler returning without `rfwo` and leaving EXCM
set — is dead.

### Three latent bugs, found and fixed, none of them this one

All three are real, all match a precedent already documented in this file, and
all are verified not to regress:

1. `osi_s_wifi_int_restore` wrote the whole word into PS where IDF restores only
   the interrupt level. Now merged.
2. `win_spill_call0` — the new call0 bridge — never wrote its **base save area**.
   `_WindowOverflow8/12` recover the caller's sp with `l32e a0, a1, -12` and
   spill through it; a windowed `entry` writes that slot, a call0 frame must do
   it by hand. `phy_stack_call` already does, and says so in a comment that
   names this exact fault.
3. `rom_call3` and `rom_call4` had the same gap, and `rom_call4` is on the
   `blob_call` path since step 28.

Fixed, and the fault did not move: still IllegalInstruction at `win_spill_all`'s
`retw`, still `ps 0x00030210`.

```
boot 11 PASS 0 FAIL   wintorture CORRECT   wincollide runs=121 wrong=0   blobphy rc=0
```

### The assumption still unchecked

Every attempt so far has assumed the spill *causes* the exception state — that a
`call12` overflows, a vector runs, and something goes wrong inside it. That has
never been measured. The alternative fits the evidence just as well and is
simpler:

**PS.EXCM is already set when `win_spill_all` is entered.** Then the spill is
merely the first windowed instruction to notice — exactly as WOE was in step 35,
and exactly the mistake made there.

One sample settles it: read PS at the top of the blocking stub, before
`win_spill_all()` is called. If EXCM is already 1 there, everything since
step 30 has been investigating the messenger, and the real question becomes how
a task comes to be running blob code with EXCM set — for which the level-3
handler's deliberate `PS.EXCM` clear (`vectors.S`, "Clear PS.EXCM before calling
any C") is the obvious first place to look, since it clears the bit for the
handler's own C but says nothing about what the resumed task inherits.

**Nothing has been on air.**

---

## Step 38 — the spill really is entered clean, so the fault is generated inside it

Step 37 proposed that `PS.EXCM` might already be set when `win_spill_all` is
entered, making the spill merely the first windowed instruction to notice. One
sample, taken at the top of the blocking path before the spill runs:

```
pre-spill : ps 0x00060320   EXCM clear before the spill, WOE set
```

**That hypothesis is false.** The blocking path is entered from ordinary task
state — WOE set, EXCM clear, exactly as it should be. So the original direction
was right after all: the exception state is *generated during* the spill, not
inherited by it.

Combined with steps 36-37 this narrows hard. At the moment `win_spill_all` is
called: WOE set, EXCM clear, vectors correct and installed, base save areas now
written by every call0 bridge. During the spill, the processor ends up at
`ps 0x00030210` — EXCM 1, WOE 0 — which is window-vector state. Something in the
overflow sequence is not completing.

### Current fault

```
exccause 2 (InstructionFetchError)  epc 0x4008abbe  ps 0x00030318
windowbase 13  windowstart 0x00002002  bit(base) SET
```

`ps 0x00030318` has `INTLEVEL = 8`, which is not a legal level on this part
(1-7 plus NMI). A PS holding an impossible interrupt level is itself evidence:
either PS is being written with a value that was never a PS, or it is being read
mid-update. That is a stronger and more specific lead than anything the previous
four steps produced, and it is where the next session should start.

### An instrument bug worth recording

The window diagnostics were gated on `exccause == 0`, so when the fault became
`exccause 2` the panic printed nothing and the run looked like a repeat of the
previous one. Two builds were spent before noticing. The guard now covers both.

A diagnostic that is silent for the fault you actually got is worse than no
diagnostic, because the silence reads as "unchanged".

**Nothing has been on air.**

---

## Step 39 — it was a double exception, and the real PC was never being read

### Nothing was illegal

`_vector_double` jumped to `_handler_panic`, the same handler as `_vector_user`,
and that handler reads `EPC1` and `PS`. On Xtensa an exception taken while
`PS.EXCM` is already set writes **`DEPC`**; `EPC1` keeps the FIRST exception's
PC.

A window overflow/underflow *is* an exception: it sets `EPC1` to the instruction
that triggered it and runs with `EXCM=1`, `WOE=0`. A fault inside that handler
therefore arrived reporting:

| reported | actually |
|---|---|
| `epc` = `win_spill_all`'s `retw` | `EPC1` — what *triggered* the spill handler |
| `ps` = EXCM 1, WOE 0, INTLEVEL 8 | the window vector's own execution state |
| — | `DEPC`, the real faulting PC, never read |

So the "cleared WOE" of step 35, the "exception context" of step 36 and the
"impossible INTLEVEL" of step 38 were all one thing: **the window handler's
normal state, misread as task state.** Four steps of explanation for values that
were never anomalous. The lesson is narrow and repeatable — a fault reporter that
cannot distinguish a double exception will confidently describe the wrong
instruction.

`_vector_double` now has its own handler, which reports `DEPC` as the fault and
keeps `EPC1` alongside it, labelled as context rather than as cause.

### What it actually is

```
exccause 2 (InstructionFetchError)
epc      0x3ffd4020   <- DEPC, the faulting instruction
DOUBLE EXCEPTION - a fault inside an exception handler.
epc1     0x4008ac22   <- what triggered the handler (NOT the culprit)
```

`0x3ffd4020` is `BLOB_DRAM_ADDR` (`0x3FFD4000`, kernel/flash.h) **+ 0x20** — the
blob's copied `.data` region. Control was transferred into the blob's data
segment, and ESP32 data RAM is not executable, so the fetch faults.

That is a loader/linker-script question, not a register-window question:
something executable is being placed in `.data` and copied to DRAM, or a pointer
into `.data` is being called. `blob.ld` collects code with globs
(`*(.iram1*) *(.phyiram*) *(.wifi0iram*)`); a section that matches none of them
and holds executable content would land exactly here.

### Next

1. Disassemble the blob image at `.data` offset `0x20` — is it code?
2. List the blob's input sections and check which ones `blob.ld` routes to
   `.data`, looking for anything executable that the globs miss.
3. `--orphan-handling=error` already guarantees nothing is silently dropped; it
   does not guarantee everything is placed in the right window.

**Nothing has been on air.**

---

## Step 40 — not misplaced code: a return address recovered out of the blob's data

`blob.ld` is exonerated. Disassembling the blob image at the faulting address
shows genuine data, and the symbols confirm it:

```
3ffd4018 D g_wifi_crypto_funcs_md5
3ffd401c D g_wifi_osi_funcs_md5        <- 0x3ffd4020 falls in this pointer region
3ffd4024 D libnet80211_reversion_remote
```

Nothing executable is routed into `.data`; the section layout is right:

```
.blob_entry vma 40300000   .text vma 40300044   .rodata vma 3f700000
.data       vma 3ffd4000   .bss  vma 3ffd5018
```

So this is **not** a placement bug. It is a control transfer to the address of a
data variable — a jump through a return address that is not one.

### The mechanism

```
epc1 0x4008ac22  = retw.n at win_spill_all+0x6, the FIRST nested level
DEPC 0x3ffd4020  = where control went
```

`retw` at the first nesting level raises a window **underflow**, whose handler
recovers the frame's return address with `l32e a0, a13, -16` (or the /4 and /8
variants) and returns through it. The value it recovered points into the blob's
`.data`. So the spill area that underflow read from did not contain a saved
frame — it contained blob data, or something that had been overwritten with it.

That is the same family as the base-save-area bugs fixed in step 37, one level
further out: not "a call0 frame forgot to write its save area", but "the memory
the handler read a frame back from was never that frame's save area".

### What to check next

1. **Whose stack is it?** Print `a1`/`a13` from inside the underflow path, or
   record the spill-area address the handler reads. If it points into
   `0x3ffd4000..0x3ffd5018` then some frame's stack pointer is inside the blob's
   `.data` region, and the overflow that preceded it wrote *through* blob data —
   which would also silently corrupt the driver.
2. **Depth.** `win_spill_all` nests six CALL12 frames of 48 bytes plus the spill
   traffic. Confirm the calling task has room: the adapter stubs run on the
   caller's task stack, which for the shell is 2048 bytes and already 1344 deep
   at its tightest.
3. (2) is the more likely of the two and is cheap: `task_stack_headroom()` for
   the blocking task, sampled at the stub before `win_spill_all()`.

The guards all read intact, but a guard only catches an overrun that reaches the
bottom word — a spill that lands *beyond* the stack into a neighbour, or short of
the guard, does not trip it.

**Nothing has been on air.**

---

## Step 41 — the stack has room; the bad return address is genuinely in a frame

Step 40 called stack depth the likely cause. It is not:

```
pre-spill : sp 0x3ffba320   lowest seen 0x3ffba320
task 5      stack 0x3ffb9e3c + 2048   ->  0x3ffb9e3c .. 0x3ffba63c
```

The spill starts 1252 bytes above the bottom of the task's own stack and needs
roughly 288 for its six CALL12 frames plus spill traffic. `lowest seen` never
moved, so no other blocking site goes deeper. The stack is not exhausted, the
spill area is inside the right task's stack, and the guard being intact is for
once actually meaningful.

So the underflow handler read a legitimate save area on the correct stack, and
what it found there was `0x3ffd4020` — a pointer into the blob's `.data` — being
used as a return address.

That leaves one explanation standing: **a frame in the chain genuinely holds
that value in its `a0` slot.** The frames being spilled belong to the blob's own
windowed code, now running on the caller's task stack since step 28 routed
`blob_call` through `rom_call4`. Their return addresses should be blob `.text`
(`0x40300044`+). One of them is not.

### Next, and it is now a two-line instrument

Log the address the window handler reads from and the value it recovers:

- in `_WindowUnderflow4/8/12`, store `a13`/`a9`/`a5` and the loaded `a0` to a
  pair of globals before `rfwu`;
- print both at the panic.

That names the exact save area and the exact frame, rather than inferring either.
The handlers are five instructions each and run with `EXCM` set, so the store
must go to a fixed address via `l32e`-safe means -- no literals, no stack.

Every cheaper explanation has now been tested and eliminated: placement
(step 40), depth (here), base save areas (step 37), PS state (steps 35-38), and
the window bookkeeping itself (steps 30-33).

**Nothing has been on air.**

---

## Step 42 — the underflow probe works, and records the wrong underflow

`_WindowUnderflow4/8/12` now record the return address they recover and the save
area it came from, in `EXCSAVE_4`/`EXCSAVE_5` — the only scratch available to a
handler with no stack, no literal pool and 64 bytes of room.

It reports:

```
underflow : recovered a0 0x400875e0 from save area 0x3ffbb540
```

`0x400875e0` is a valid kernel code address and `0x3ffbb540` is inside task 7's
stack (`touch`, `0x3ffbae14 + 2048`). That is a **healthy underflow belonging to
an unrelated task.**

Underflows happen continuously across the whole system — every `retw` that needs
a frame reloaded — so recording unconditionally captures whichever task
underflowed last before the panic, which is almost never the interesting one.
The probe is correct and the sampling is wrong.

### The fix, and its constraint

Record only a *suspicious* recovery: a return address that is not in the code
region (`< 0x40000000`), which is exactly the `0x3ffd4020`-shaped value the
fault returns through, and make it sticky so the first one survives.

The constraint is real. These handlers may not clobber `a0..a3` (being
restored), have no spare registers, and must stay inside a 64-byte vector slot.
`_WindowUnderflow12` is currently 13 instructions plus the two `wsr`s: about 45
of 64 bytes, so a compare and branch fits, but the register to compare *with*
has to come from somewhere that is not the frame being restored.

The cleanest form is probably to leave the unconditional `wsr` in place and do
the filtering where there is room to do it — sample `EXCSAVE_4` from
`spill_before_parking()` and from the blocking stub, on both sides of
`win_spill_all()`, and keep the first value that is not a code address. Same
information, no constraint on the vector.

### Standing state

```
boot 11 PASS 0 FAIL   wintorture CORRECT   wincollide runs=121 wrong=0   blobphy rc=0
wifiinit task: DOUBLE EXCEPTION, InstructionFetchError, DEPC 0x3ffd4020
```

**Nothing has been on air.**

---

## Step 43 — the bad address is not an underflow recovery, so it is not a window bug

The filtered probe brackets `win_spill_all()` on both sides and keeps the first
underflow recovery that is not a code address:

```
uf filter : no non-code recovery seen at either side of the spill
underflow : recovered a0 0x40087654 from save area 0x3ffbb540   (healthy, task 7)
```

**Nothing bad is ever recovered.** Every underflow the system performs returns a
valid code address. The `0x3ffd4020` that control reaches is therefore *not* a
return address reloaded from a save area, which is what steps 40-42 assumed.

### What that changes

The window machinery is exonerated, and with it roughly ten steps of suspicion:

- the spill is correct (step 32, measured 7 frames -> 1)
- the vectors are correct (step 37)
- base save areas are now written by every call0 bridge (step 37)
- the stack has room and is the right stack (step 41)
- underflow recoveries are all valid code addresses (here)

What remains is the plainest reading of the fault, and the one available since
step 39: **the blob transfers control to an address inside its own `.data`.**
`0x3ffd4020` sits among `g_wifi_crypto_funcs_md5`, `g_wifi_osi_funcs_md5` and
`libnet80211_reversion_*` — pointer-sized slots the driver initialises and
dereferences. A call through one of those, holding a value that is a data
address rather than a code address, produces exactly this.

That is a **blob loading question**, not a scheduler or window question:

1. `blob_init()` word-copies `.data` (LMA `0x403930ac`, 0x1018 bytes) to
   `0x3ffd4000` and zeroes `.bss`. Verify the copy against the image rather than
   trusting it — a shifted or truncated `.data` puts wrong values in exactly
   these slots.
2. The three-window split means `.rodata` lives at `0x3f700000` while `.data` is
   at `0x3ffd4000`. Any pointer the blob expects to resolve into one and which
   lands in the other is this fault.
3. `esp_wifi_init_internal` is the first caller to reach these tables, which is
   why nothing before it faulted.

Ten steps of window investigation produced four real fixes and eliminated the
whole subsystem as the cause. That is worth having, and it is also a reminder
that step 39 named the right region and the next ten steps looked elsewhere.

**Nothing has been on air.**

---

## Step 44 — `.data` is right, and the fault address is an address, not a value

The blob image's `.data`, as linked:

```
3ffd4000  2e02703f 01000000 3e02703f 4602703f
3ffd4010  4e02703f 5602703f 5e02703f 6602703f
3ffd4020  01000000 7702703f 8602703f 01010100
```

Two results, both eliminating step 43's shortlist.

**The window split is consistent.** The `*_md5` and `libnet80211_reversion_*`
slots hold `0x3f70022e`, `0x3f70023e`, `0x3f700277` … — pointers into `.rodata`
at `0x3f700000`, which is precisely where the DROM window maps it. Pointers that
must resolve across the split do resolve. Step 43's hypothesis (2) is dead.

**`0x3ffd4020` holds `0x00000001`.** A boolean, not a pointer. So the faulting
PC is the *address* of a data slot, not a corrupted pointer value read out of
one. Nothing in `.data` contains `0x3ffd4020`; control was computed to it.

That distinction matters, because it rules out the whole family of "a function
pointer holds the wrong value" explanations — including a mis-copied `.data`,
which would have to have written `0x3ffd4020` into some slot, and no slot
contains it.

### What computes 0x3ffd4020

An address, not a loaded value, means it was *formed*: a table base plus an
offset, or a symbol reference. `0x3ffd4020` is `_blob_data_vma + 0x20`, and the
blob reaches this only once `esp_wifi_init_internal` starts walking its own
tables.

The candidates worth testing, in order:

1. **A jump table.** `s_ioctl_table` sits at `0x3ffd4034`, 0x14 past the fault.
   A dispatch computed off a table base in `.data` — rather than off `.rodata`
   or `.text` — lands here. If the blob's linker placed a table our script routed
   to `.data` that the original placed elsewhere, dispatch through it jumps into
   data.
2. **A call through a not-yet-initialised slot** that the driver fills during
   init and which our OSI table never populates, leaving the blob to compute a
   target from a base that is still the table's own address.

(1) is checkable statically: compare which input sections `blob.ld` routes to
`.data` against what the original `libnet80211.a`/`libpp.a` objects declare, and
look for anything the ESP-IDF link would have placed in `.rodata` or IRAM.

**Nothing has been on air.**

---

## Step 45 — the `.data` routing is faithful, so the jump table theory is out

`blob.ld` routes `.data` by the input sections the objects declare:

```
.data : AT(ALIGN(LOADADDR(.rodata) + SIZEOF(.rodata), 4))
{
  _blob_data_vma = .;
  *(.data .data.* .sdata .sdata.*)
  *(.dram1*)
}
```

No pattern here can pull in something IDF would have placed elsewhere — and
`s_ioctl_table`, the table sitting 0x14 past the fault, is `D` in the symbol
table, i.e. the original object declares it as initialised **data**. Our script
puts it exactly where IDF's link would.

So step 44's candidate (1) is eliminated statically, without a board run. The
`.data` window is faithful to the image in placement (step 45), in content
(step 44), and in cross-window pointer resolution (step 44).

### What is left

Candidate (2): the blob computes a call target from a base that is still a table
address because nothing has filled the slot in. `0x3ffd4020` is
`_blob_data_vma + 0x20`, it holds `0x00000001`, and it is only reached once
`esp_wifi_init_internal` walks its own tables — which is also the first moment
the driver expects structures the host was supposed to populate.

The OSI table is the obvious such structure, and there is a live discrepancy in
the existing output worth pulling on before anything else:

```
osi_funcs -> 0x3f40761c   (counting table 0x3f40761c, real table 0x3ffb00bc)
real osi forwarded calls: 0   last impl at 0x00000000
```

**Zero forwarded calls.** 15 adapter entries are reached and counted, but the
counting table never forwards to the real one, and `last impl` is null. If the
blob is reading function pointers out of a table that the host never finished
wiring, a computed dispatch off that table's base is precisely the shape of this
fault — and this line has been printing all along.

Next: work out why the counting table forwards nothing, and what the blob reads
out of it during init.

**Nothing has been on air.**

---

## Step 46 — the "zero forwarded calls" lead was a stale instrument

Step 45 called this the most promising thread left:

```
real osi forwarded calls: 0   last impl at 0x00000000
```

It is not a symptom. `g_osi_last` and `g_osi_hits` are defined in
`vendor/windowed/wifi_osi.c` — the table whose own header begins **"STALE. DO
NOT HAND THIS TABLE TO THE BLOB"**, kept only because its bodies are useful
reference. The live table is the generated `wifi_osi_stubs.c`, whose entries
reach `osi_impl_*` through `w2c_call*` and never touch those counters.

Zero is therefore the correct reading, and it would read zero on a completely
healthy system. The lead was wrong.

### The actual defect here is the instrument

A counter that reads zero for two entirely different reasons — "nothing
forwarded" and "nothing uses this table" — cannot distinguish them, and it sits
in the middle of the output of the one command being used to debug this. It cost
a step, and it is the same shape as the `exccause == 0` gate in step 38 and the
global-`WINDOWSTART` counter in step 31: **an instrument whose silence is
indistinguishable from a result.**

Not fixed in this pass, deliberately. The print lives in `shell.c`, which is the
first object in `.flash.text`, and step 25 measured that adding nine lines there
walks into the layout band. Changing it means re-testing `blobphy` and the whole
blob path for a cosmetic fix. It is recorded here instead, and should be removed
or repointed at `wifi_osi_calls()` the next time `shell.c` is touched for another
reason.

### Standing conclusion

The OSI table is not implicated. The blob accepted it, fifteen entries are
reached and counted through the generated table's own instrumentation
(`osiused`), and the forwarding those entries do is real.

The open question is unchanged from step 44: control reaches
`BLOB_DRAM_ADDR + 0x20`, an address that was computed rather than loaded, during
`esp_wifi_init_internal`. The `.data` window is faithful in placement, content
and cross-window pointer resolution; the window subsystem is measured correct.

**Nothing has been on air.**

---

## Step 47 — the last adapter entry, and a bisect that names the mechanism

### Where the blob is

```
last osi  : entry 15  _semphr_take
```

The blob's final adapter call before the fault is the blocking one. So the
sequence is exactly the designed path: `_semphr_take` finds the semaphore
contended, spills, releases the lock, blocks; the blob task runs; the fault
follows.

### Bisecting the spill out

`wincollide` was fixed by removing one `call8 win_spill_all`. Removing the spill
from the five blocking sites here does not fix this, but it **moves the fault
somewhere far more specific**:

| spill | fault |
|---|---|
| present | `InstructionFetchError`, DEPC `0x3ffd4020` (blob `.data`) |
| removed | `StoreProhibited`, DEPC **`0x40080100`**, excvaddr `0x00000010` |

`0x40080100` is `_WindowOverflow12` itself. The handler recovers the caller's
stack pointer with `l32e a0, a1, -12` and then spills through it — and it is
storing to `0x10`, i.e. it recovered a near-null base.

That is precisely the fault `phy_stack_call`'s own comment describes:

> the caller's prologue does. phy_stack_call is call0 and wrote nothing there,
> so the handler loaded a fresh .bss zero and spilled to 0 - 32.

So the **base save area class is the mechanism after all**, and step 37's three
fixes (`rom_call3`, `rom_call4`, `win_spill_call0`) did not cover every frame the
blob's windowed code rotates over. With the spill present, the spill pushes those
frames out early enough that this overflow never happens — and the failure
presents as the `.data` jump instead. Two faces of one defect.

### What that makes the next step

Find the remaining call0 frame in the chain that windowed code rotates over.
Candidates, all compiled `-mabi=call0` and none of which write a base save area:

1. `blob_call()` — sits directly beneath `rom_call4`
2. the `wifiinit` shell command frame beneath that
3. any `osi_impl_*` reached by `callx0` from a windowed stub (these create no
   window frame, so they are the least likely)

The spill is restored; the bisect was diagnostic, not a fix. The instrument that
would settle it is already half-built: `_WindowOverflow4/8/12` can record the
base they recovered into `EXCSAVE`, exactly as the underflow handlers now do, and
a near-null value names the frame directly.

**Nothing has been on air.**

---

## Step 48 — the overflow spills a frame that was never created

Polling the overflow probe from `task_schedule()` — the one vantage point that
sees every tick regardless of who is running — catches it:

```
of filter : base 0x00000000 recovered from frame sp 0x00000000   AFTER spill
```

Both zero. The handler was spilling a frame whose **own `a1` is zero**. A frame
established by `entry` always has a valid `a1` (`entry` computes it from the
caller's). A frame with `a1 = 0` was never established — yet `WINDOWSTART` had
its bit set, so the hardware believed it was live and tried to push it out.

### What sets a bit for a frame that does not exist

The per-task window restore (step 30, amended step 32):

```
WINDOWSTART |= 1 << saved_WINDOWBASE
```

It asserts "this task has one live frame at its saved base" without any
guarantee that the physical registers at that base belong to this task. For a
task restored onto a base whose registers are stale or zero — a freshly created
task, or one whose base collides with a window position another context has
since rotated through — that bit is a lie, and the next rotation deep enough to
overflow tries to spill it.

That closes the loop with step 33, which established that a per-task copy of a
global register cannot be written back: **assignment resurrects other tasks'
frames, OR-ing invents one of your own.** Both are now measured rather than
argued.

It also explains the two faces from step 47. With the spill present the invented
frame is pushed out early and the damage surfaces later as a computed jump into
`.data`; with the spill removed the overflow hits it directly and stores through
a null base.

### The shape of the fix

`WINDOWSTART` must only claim a frame the restore can *prove* exists. Two ways:

1. Restore the task's registers **and** its base such that the one frame it
   claims is the one just written — i.e. treat `a1` in the restored frame as the
   proof, and refuse to set the bit if the restored `a1` is not a plausible
   stack address for that task. Cheap, and turns a lie into a checked assertion.
2. Guarantee one-frame-per-task by construction, which is step 33's option (1)
   and needs the spill to cover every path that reaches a switch — already true
   for voluntary blocks since step 34.

(1) is a few instructions in `_handler_level3` and is testable immediately with
the counter already in place: `of filter` must stay empty.

**Nothing has been on air.**

---

## Step 49 — assignment tried again, and the OR is load-bearing

Step 48 proposed going back to assigning `WINDOWSTART` on the grounds that
step 34 had removed the condition the OR defended against. Tested:

```
boot 11 PASS 0 FAIL   wintorture FAIL   wincollide FAIL   blobphy FAIL
```

Everything windowed breaks. Reverted; the suite recovers exactly
(`wintorture` CORRECT, `wincollide` runs=134 wrong=0, `blobphy` rc=0).

### Why the argument was wrong

The claim was: "at the moment any task is switched away from, no OTHER task has
live frames to protect." That is false, and the pin is what makes it false.

The pin stops the scheduler switching **away from** a task inside windowed code.
It does nothing about the other direction. Tasks B, C, D are switched away from
and resumed *while* A sits pinned mid-excursion with several live frames in the
register file. Every one of those restores runs with A's frames live, and
assignment wipes them.

So the OR is not a workaround that step 34 outgrew. It is the only thing making
a per-task restore survivable at all while another context holds frames — which
is the normal state, not an edge case.

### What that leaves

The two operations remain exactly as step 33 framed them, and both are now
measured rather than argued:

- **assignment** destroys frames belonging to a task that is still using them
- **OR** never clears, so bits accumulate and eventually name a frame `entry`
  never created — step 48's `a1 = 0` spill

The missing capability is the one the hardware does not provide: knowing **which
task owns a bit**. `WINDOWSTART` is 16 bits of "live", with no owner field, and
nat-os needs owners because more than one task has frames in the file at once.

The honest options, none of them cheap:

1. **Track ownership in software.** A per-task mask of the positions it has
   claimed, OR-ed in on restore and cleared on exit from windowed code. Exact,
   and costs a word per task plus bookkeeping at every window transition.
2. **Never let two tasks hold frames at once.** Spill on switch-*out* for any
   task with more than one live frame, which is step 31's option (1) and needs
   the spill in `_handler_level3` — attempted five times (steps 14-18) and
   failed each time, though never with the pin and per-task base in place.
3. **Give the blob its own window regime**: keep blob excursions strictly
   non-preemptible end to end, so no other task ever runs while frames are live.
   The pin already does this for the excursion; it is the *blocking* release
   that breaks it, which is where this whole sequence started.

(1) is the only one that does not fight the hardware.

**Nothing has been on air.**

---

## Step 50 — seeding an unclaimed base: tried, regressed, reverted

The step-49 diagnostic found `windowstart 0x00002002` — bits 1 and 13 — where
one bit was expected, and traced the extra claim to `task_create()` seeding a new
task's `WINDOWBASE` from **the creator's** base. The blob task is the first task
in this system created from *windowed* code (the driver calls
`_task_create_pinned_to_core` from inside its own excursion), so the creator's
base sits in the middle of the creator's live window.

The fix tried: pick a `WINDOWSTART` bit that is currently clear.

```
boot 11 PASS 0 FAIL   wintorture CORRECT   wincollide FAIL   wifiinit task PANIC
```

Reverted. `wincollide` recovers to runs=134 wrong=0.

### Why a clear bit is not a free base

A task's sixteen registers span **four** window positions — `base`, `base+1`,
`base+2`, `base+3`. A bit being clear says nothing about its three neighbours, so
"unclaimed" in the `WINDOWSTART` sense is not "unoccupied" in the register-file
sense, and restoring sixteen registers at a nominally free base can still write
over frames that are live at the positions above it.

That is the same category error as the multiframe counter in step 31: reading a
16-bit register as if each bit described an independent unit, when the thing it
indexes is four registers wide and tasks straddle four of them.

### What the diagnosis is still worth

The *observation* stands and is the sharpest thing available: a new task created
from windowed code inherits a base inside its creator's live window, and that is
where the phantom frame comes from. What does not follow is that any single free
bit fixes it — a correct base has to be four positions clear of every live frame,
which on a 16-position file with several tasks holding frames is not always
available.

Which puts the weight back on step 49's option (1): track ownership in software,
because the register file cannot be partitioned by inspection.

**Nothing has been on air.**

---

## Step 51 — window ownership tracked in software

`WINDOWSTART` is 16 bits of "a frame lives here" with no owner field, and nat-os
needs owners: a task pinned inside windowed code keeps frames live while every
other task is switched away from and resumed around it. Assignment destroys
those frames (step 49); OR never clears, so bits accumulate until one names a
frame `entry` never created (step 48). Both measured. So the kernel records what
the hardware does not.

- `g_win_mask[TASK_MAX]` — the positions each task owns. A task is the only
  thing that can change `WINDOWSTART` while it runs, so its mask is whatever is
  set at its switch-out that no other task has claimed.
- `g_win_union` — the union, recomputed per switch in C, because the vector has
  no room to walk a table.
- `_handler_level3` now assigns `1 << saved_base | g_win_union` instead of
  OR-ing whatever happened to be in the register. Other tasks' frames survive;
  bits nobody owns do not.

### No regressions

```
boot 11 PASS 0 FAIL   wintorture CORRECT   wincollide runs=142 wrong=0   blobphy rc=0
```

### The fault moved closer to its cause

```
exccause 28 (LoadProhibited)   DEPC 0x40080103   excvaddr 0x0000000d
epc1 0x4008ae57
```

`0x40080103` is `_WindowOverflow12 + 3` — the `l32e a0, a1, -12` that recovers
the caller's stack pointer. It faults on the load itself, with `a1 = 0x19`.

That is progress of a specific kind. Before, a phantom frame was spilled through
a null base and the damage surfaced much later as a computed jump into the blob's
`.data`; now the handler faults immediately, at the instruction that touches the
bad frame, with the bad `a1` visible in `excvaddr`. The failure stopped being
action-at-a-distance.

A phantom frame still exists. Ownership tracking removes bits that nobody
claimed; it does not stop a task from claiming a position whose registers are not
its own, which is what step 50 established `task_create()` does when the creator
is mid-excursion. The two are separate defects and only one is fixed.

**Nothing has been on air.**

---

## Step 52 — new tasks claim nothing, and the phantom is not theirs

A new task has no windowed frames: it starts in call0 code, and the first
windowed call it makes sets its own bit, because that is what `entry` does.
Seeding a bit at creation asserts a frame exists before one does. `task_create()`
now seeds `WINDOWSTART = 0` and records only the base, which the restore needs
somewhere to put sixteen registers and which is harmless because every task's
registers travel through memory on each switch.

Correct on its own terms, and **not** the source of the phantom:

```
boot 11 PASS 0 FAIL   wintorture CORRECT   wincollide runs=142 wrong=0   blobphy rc=0
wifiinit task: LoadProhibited, DEPC 0x40080103, excvaddr 0x0000000d  (unchanged)
```

Byte for byte the same fault. Step 50's diagnosis — that the blob task is created
from inside the driver's excursion and inherits a base in the creator's live
window — was a real defect and is now fixed, but it is not what produces the
frame being spilled.

### What the remaining phantom looks like

`_WindowOverflow12+3` faults loading `[a1 - 12]` with `a1 = 0x19`. Twenty-five.
Not a stack pointer, not null, not an address at all — a small integer, of the
kind a register holds when it contains a count or an index rather than a
pointer.

So a window position has its bit set while its registers hold ordinary data.
With ownership tracking in place the bit must have come from some task's
switch-out `WINDOWSTART` — `g_win_mask[t] = ws_out & ~others` — which means the
hardware itself had that bit set at a switch. Either a frame really was live
there and its registers were later overwritten, or the attribution is wrong.

The next instrument follows directly and is cheap: record, per task, the mask it
claimed and the base it claimed it at, and print the table at the panic. That
turns "some position is lying" into "task N claimed position P at time T", which
is the same move that worked for the double exception and for the overflow base.

**Nothing has been on air.**

---

## Step 53 — the attribution rule is wrong, and the table says so

Printing each task's recorded claim alongside the base it claimed at:

```
task 5 (shell)  win 0x00002000@13   guard ok
task 6 (disp)   win 0x0000c400@1    guard ok
task 0..4,7..9  win 0x00000000@..   guard ok
windowbase 10   windowstart 0x0000e64a
```

`disp` is a call0 task. It never executes a windowed instruction. It has been
credited with **bits 10, 14 and 15**, at a base of 1 — three window positions it
cannot possibly have created.

### Why

The attribution rule is

```c
g_win_mask[g_current] = ws_out & ~others;
```

"whatever is set at my switch-out that nobody else has claimed." That credits
the *current* task with frames belonging to a task that simply has not been
switched out since. The shell enters the blob and goes deep; `disp` is switched
out while those frames are live; the shell has not yet re-claimed them at its own
switch-out, so `~others` lets them through and `disp` takes ownership of the
driver's window.

Then the union keeps them alive after the shell has spilled and released, and an
overflow eventually walks into a position whose registers hold `0x19` — the
`a1 = 25` from step 52, an index, not a pointer.

So ownership tracking (step 51) was the right idea implemented with the wrong
rule. It fixed the accumulation problem and introduced a misattribution one.

### The rule it should be

A task can only own frames it created, and after the step-34 spill it parks with
exactly one: **its own base**. So the claim is `1u << base`, not "everything
unclaimed". A call0 task owns one position; a windowed task that blocked owns one
position; nothing owns three positions at a base four away from them.

That is a one-line change with a ready-made check: `disp` must show
`win 0x00000200@..`-shaped values matching its own base, and `of filter` must
stay empty.

### An instrument failure, repeated

This table did not print on the first attempt: the window diagnostics were gated
on `exccause == 0 || exccause == 2`, and the fault is 28. The identical mistake
was made at step 38, written up there as a lesson, and then made again. The block
is now unconditional.

**Nothing has been on air.**

---

## Step 54 — the one-bit rule fails, and wintorture's control does not measure what it claims

### The rule

`g_win_mask[t] = 1 << base` — a task owns the one frame it is parked on — was
implemented on the step-34 invariant that every task is switched away from with
exactly one live frame. It regresses `wintorture` and `wincollide`; reverted, and
both recover (CORRECT, runs=135 wrong=0).

### The control is not a control

`wintorture` prints:

```
switches during the call: 6  (preemption really happened)
```

and that sentence has been load-bearing since step 14 — it is the reason
"windowed frames survive preemption" was treated as measured rather than assumed.

It counts the wrong thing. In `task_schedule()`:

```c
g_current = next;
g_tasks[next].switches++;
```

The counter increments on **every tick**, including the ones where `next ==
g_current` and nothing switched. The scheduler itself knows the difference — two
lines earlier it passes `next != g_current` to `watchdog_liveness()` — but the
per-task counter does not use it.

And during `wintorture` nothing *can* switch: `rom_call3` takes `blob_lock`,
which pins, so `next` is forced to `g_current` for the whole call. The six are
six ticks of the same task resuming itself.

**So `wintorture` has never demonstrated that windowed frames survive
preemption.** It demonstrated that a pinned task holding eight frames is not
corrupted while nothing else runs — which is true, and much weaker.

That does not make the earlier conclusions wrong. It makes one of them
unsupported, and it is a load-bearing one:

- step 14's "the concern was MEASURED and does not hold" rests on this control
- `blobcall.c`'s header comment states it as established fact
- the pin, the spill and the ownership work were all designed around it

### What to do about it

1. **Fix the counter** so `switches` counts distinct switches, and add a separate
   tick counter if the old number is wanted. One line, and it changes what every
   past `wintorture` run meant.
2. **Re-run `wintorture` with the pin disabled**, which is the experiment the
   test was always supposed to be. If frames survive genuine preemption, the
   original conclusion is restored on real evidence. If they do not, the pin is
   not an optimisation — it is the only thing holding the system together, and
   the one-bit rule failed because the invariant it assumed was never true.

Until (2) runs, the invariant is unknown rather than established, and any rule
built on it — including the one reverted here — is unfounded.

**Nothing has been on air.**

---

## Step 55 — the experiment finally run: windowed frames do NOT survive preemption

### The counter, made honest

`g_tasks[next].switches++` now runs only when `next != g_current`, with a
separate `resumes` for the old meaning. `wintorture` immediately reports the
truth about itself:

```
spun 60 ms with 8 windowed frames live, interrupts ENABLED
switches during the call: 0  -- NONE, so this proves nothing
checksum 1632 expected 1632  CORRECT
```

Zero. The test's own fallback wording — written at step 14 to guard against
exactly this — was correct all along and had never been reached, because the
counter could not produce a zero.

### The experiment

With `BLOB_PIN_DISABLE=1`, so the scheduler really can switch away from a task
holding eight live windowed frames:

```
exccause 29 (StoreProhibited)   DEPC 0x40080115   excvaddr 0x00000190
DOUBLE EXCEPTION   windowbase 6   windowstart 0x0000a248
```

`wintorture` panics. Not a wrong checksum — a fault inside `_WindowOverflow12`.

**Windowed frames do not survive preemption in nat-os.** Step 14's conclusion,
stated in `blobcall.c`'s header as "That concern was MEASURED and does not hold",
is false, and has been since it was written.

### What this re-frames

The pin is not an optimisation or a convenience. It is **the only thing keeping
windowed code alive on this kernel**, and every result that looked like it
survived preemption was a result obtained while the pin silently prevented
preemption from happening.

It also explains step 54 cleanly. The one-bit ownership rule assumed "every task
is switched away from with exactly one live frame". Under the pin that is
vacuously true for windowed tasks — they are never switched away at all — so the
rule was not wrong about the invariant. It was wrong to think the invariant
described a *guarantee* rather than an absence.

And it sharpens the real question. Two contexts inside windowed code work today
(`wincollide` runs=120 wrong=0) because the lock serialises them and the pin
stops the scheduler mid-window. The WiFi driver needs a task that blocks and
resumes inside windowed code, which is exactly the case the pin cannot cover —
and which this step shows the kernel cannot survive without it.

### Restored and verified

```
boot 11 PASS 0 FAIL   wintorture CORRECT   wincollide runs=120 wrong=0
```

`blobcall.c`'s header comment is now known to be wrong and should be rewritten
against this measurement rather than left contradicting it.

**Nothing has been on air.**

---

## Step 56 — precise ownership, and why the precise rules kept failing

Two attempts at an exact attribution rule regressed the suite while the sloppy OR
survived. The reason is not the rule:

**`next == g_current` still runs the whole restore path.** A task pinned inside
windowed code is resumed *as itself* on every tick, and the handler writes
`WINDOWSTART` on each of those resumes. Any rule that narrows the mask therefore
deletes the RUNNING task's deep frames one tick after `entry` created them. The
OR survived only because it never dropped what was live — not because it was
right.

Both pieces are now in:

- **ownership**: a task owns the bit at its own base, and only when the hardware
  says a frame lives there — `((ws_out >> base) & 1) ? (1 << base) : 0`. A call0
  task owns nothing; a task that spilled to one frame before blocking owns that
  one. Frames deeper than the base belong to whoever is running and are unwound
  or spilled before that task reaches a switch.
- **same-task resume**: the union is computed after `next` is selected, and when
  `next == g_current` it is set to the live `WINDOWSTART`, which makes the
  handler's assignment a no-op without a branch in the vector.

```
boot 11 PASS 0 FAIL   wintorture CORRECT   wincollide runs=120 wrong=0   blobphy rc=0
```

The precise rule now coexists with the suite, which neither earlier attempt
managed. `wifiinit task` still panics — `StoreProhibited` in `_WindowOverflow12`,
`excvaddr 0x10` — so a frame the hardware believes in is still being spilled
through a bad base, but the bookkeeping around it is no longer guesswork.

### What step 55 means for the shape of the answer

Windowed frames do not survive preemption. The pin is what prevents preemption,
and the driver needs a task that blocks and resumes inside windowed code — the
one case the pin cannot cover.

So the remaining work is not more bookkeeping. It is making a *blocked* windowed
task genuinely restorable: its frames spilled to its own stack, its window state
reduced to something the restore can reconstruct, and nothing of it left live in
the register file while another context runs. The spill already does the first
part (7 frames to 1, measured). The last live frame is the part that has never
worked, and it is now the only part left.

**Nothing has been on air.**

---

## Step 57 — a non-running task needs no live frame, and the phantom is gone

Every non-running task has all sixteen of its registers in memory: saved by
`_handler_level3` on the way out, restored on the way back in. A `WINDOWSTART`
bit left set for it preserves nothing — the registers at that position belong to
whoever is running now — but it does tell the hardware a frame lives there, and
an overflow reaching it spills through somebody else's registers.

That is the phantom frame of steps 48-56.

So on a **real** task change the incoming task claims its own base and nothing
else; the union contributes zero. This is the assignment that regressed the suite
twice — and both times the damage was done on *same-task* resumes, which step 56's
branch now leaves untouched. With that in place, assignment is correct.

```
boot 11 PASS 0 FAIL   wintorture CORRECT   wincollide runs=118 wrong=0   blobphy rc=0
```

`of filter` no longer reports a null base, and the `StoreProhibited` inside
`_WindowOverflow12` — the fault that has dominated since step 47 — is gone.

### The new failure

```
exccause 0 (IllegalInstruction)
DEPC 0xc008aefe
epc1 0x400891b0   <- what triggered the handler
```

Different in both parts. `epc1` is in kernel code rather than `win_spill_all`,
so whatever raises the window exception is no longer the spill; and per step 39's
lesson the CALL12 bits on `DEPC` are not automatically evidence of a raw jump —
that has to be checked rather than assumed, having been misread once already.

### Where this leaves the sequence

The window bookkeeping is now, for the first time, internally consistent:

- a task owns one bit, at its own base, and only when the hardware confirms a
  frame there (step 56)
- a same-task resume does not touch the window (step 56)
- a non-running task owns nothing, because its registers are in memory (here)

Each of those was measured into existence by a regression, not designed up
front. What remains is a fault that no longer has anything to do with the
frames' bookkeeping.

**Nothing has been on air.**

---

## Step 58 — the bad sp is the PHY stack, again

Validating every saved stack pointer against its owning task's stack, at the
point the scheduler hands it over rather than one restore later:

```
bad sp : task 5 sp 0x3ffc0000 outside 0x3ffb9e88..0x3ffba688
```

`0x3ffc0000` is `_phy_stack_top`. The shell is being switched out while executing
on the **shared private PHY stack**, with `sp` at its very top — the moment
`phy_stack_call` has just switched onto it or is about to switch off.

That is the same shared buffer as step 27, which was supposedly retired at
step 28 by routing `blob_call` through `rom_call4` so blockable driver code runs
on the caller's own task stack. It did do that — and something else still enters
`phy_stack_call` during `wifiinit task`. `phyinit_run_at` completes before the
init call, so the remaining user is inside the blob's own execution.

### Why this breaks everything downstream

A task parked on `_phy_stack` is outside its own stack, so:

- its stack guard cannot protect it — the guard word is in the task stack, and
  the frames are not
- the window bookkeeping keyed to "this task's base" has no relationship to where
  its frames actually live
- a second context entering `phy_stack_call` overwrites them, which is exactly
  the hazard `blobcall.c`'s header has documented from the beginning

The handler then faults restoring through it (`l32i.n a8, a1, 28` at
`0x400891b0`, step 57), one switch after the damage.

### Next

Find the remaining caller. `phy_stack_call` is reached from `phyinit_run_at` and
from `win_call_vendor`; the OSI entries that route PHY work through it are the
likely path, and `osiused` plus the `last osi` print can name it. Then either
give that path the caller's stack as step 28 did for `blob_call`, or make
`phy_stack_call` non-blocking by construction so a switch cannot land on it.

The measurement to keep: `bad sp` must read "none -- every saved sp was inside
its own stack". That is a property worth asserting permanently, not just while
this bug is open.

**Nothing has been on air.**

---

## Step 59 — the PHY stack race, closed

`phy_stack_call` switched `a1` onto the shared `_phy_stack` and *then* masked
interrupts. For the half-dozen instructions between, the stack pointer pointed at
a shared buffer while the timer could still fire — and a tick landing there saves
the task with a stack pointer outside its own stack.

That is step 58's measurement exactly: `task 5 sp 0x3ffc0000 outside
0x3ffb9e88..0x3ffba688`, and `0x3ffc0000` is `_phy_stack_top`.

The order is now: build the frame, save PS relative to `a8`, take the mask, and
switch **last**. PS is stored through `a8` rather than `a1` because `a1` is not
the new stack yet, and the mask test uses `a10` so `a8` survives to the switch.

```
boot 11 PASS 0 FAIL   wintorture CORRECT   wincollide runs=138 wrong=0   blobphy rc=0
```

### What this was, and was not

It was a genuine latent race, present since PHY init first worked. It stayed
invisible because it needs a path that keeps running long enough afterwards for
the bad saved `sp` to be restored through — and `wifiinit task` is the first one.

It was **not** the remaining `wifiinit task` failure. That is unchanged in
identity — `InstructionFetchError`, `DEPC 0x3ffd4020` — though `epc1` moved to
`0x4008926c`.

It also settles step 58's framing: there is no second caller leaking onto the
shared buffer. Step 28's fix was sound, and what remained was an ordering bug
inside `phy_stack_call` itself.

### Note on the diagnostics

The panic dump in this run printed only its first few lines before the capture
window closed — the block is unconditional since step 53, but it is now long
enough that a 30 s read can miss the tail. Worth trimming once the scaffolding
comes out, and worth remembering before reading a short dump as a short answer.

**Nothing has been on air.**

---

## Step 60 — the race fix did not close it, and the value says what did

Step 59 closed a real race and then claimed the `bad sp` was fixed, on the
strength of "no regressions". That claim was not checked, because the capture
window truncated the dump. Checked properly:

```
bad sp : task 5 sp 0x3ffc0000 outside 0x3ffb9e88..0x3ffba688
```

Unchanged. The reordering was correct and necessary; it was not this.

### The value was the clue all along

`phy_stack_call` builds its frame at `a8 = _phy_stack_top - 64` and switches `a1`
to *that*. A task caught mid-switch would show `0x3ffbffc0`. The recorded value is
`0x3ffc0000` — `_phy_stack_top` exactly, which `a1` never holds.

It is the **sentinel**:

```asm
movi a9, _phy_stack_top
addi a10, a8, -12
s32i a9,  a10, 0        /* make the call0 base frame look windowed */
```

written so `_WindowOverflow8/12` finds a plausible caller sp. `_WindowUnderflow8/12`
recovers `a1` from that same slot with `l32e a1, a13, -12`, so a frame that
underflows through the base frame gets `a1 = _phy_stack_top` by construction —
and if a switch lands while that value is live, the task is saved pointing at the
top of a shared buffer it does not own.

So the sentinel that makes a call0 frame survive an *overflow* is the same value
that corrupts a task's `sp` on the matching *underflow*. It was introduced to fix
a StoreProhibited and has been carrying this since.

### What to do with that

The sentinel needs to be a stack pointer the underflow can legitimately restore,
not a marker. Two shapes:

1. Write the **caller's real sp** into the base save area instead of
   `_phy_stack_top` — it is already saved at `[a8 + 0]`, so it costs a load.
   Then an underflow through the base frame restores something true.
2. Keep the sentinel but ensure no switch can observe it, which is what the mask
   is for — and step 59 shows the mask now covers the switch, so whatever path
   exposes it is outside the masked region.

(1) is the honest fix: the value should be correct rather than merely
unobservable.

### Method note

Two claims in two steps were made without their own measurement — step 59's
"race closed" and step 28's "PHY stack sharing eliminated". Both were true of the
path under test and generalised past it. The `bad sp` check exists precisely to
catch that class, and it did.

**Nothing has been on air.**

---

## Step 61 — the sentinel is gone, the symptom is not

The base save area now holds the caller's real `sp` instead of `_phy_stack_top`.
`a1` still carries it at that point, since step 59 moved the switch after the
mask, so the slot costs nothing and an underflow through the base frame now
restores something true rather than something merely plausible.

Correct, no regressions — and not the symptom:

```
boot 11 PASS 0 FAIL   wintorture CORRECT   wincollide runs=138 wrong=0   blobphy rc=0
bad sp : task 5 sp 0x3ffc0000 outside 0x3ffb9e88..0x3ffba688     (unchanged)
```

### What that leaves

`_phy_stack_top` now appears in exactly three places:

- `phy_stack_call`'s frame base, which is `top - 64` and never the top itself
- `phyinit.c`, twice, as a **bound** for priming and for `phy_stack_size()` —
  never as a stack pointer

So on the current reading, nothing assigns `a1 = _phy_stack_top`, and the check
still reports it. One of those two statements is wrong, and the cheap way to find
out which is to stop reasoning about who *could* write it and record who *did*:
latch the value in `task_schedule()` the first time a saved `sp` equals
`_phy_stack_top` exactly, along with the task and its saved `epc`. The `epc` names
the instruction that was executing, which is the thing every inference so far has
been standing in for.

That is the same move that ended the double-exception confusion (step 39) and the
overflow-base hunt (step 48): stop deducing the writer, record it.

### Standing

Three fixes in this stretch — the switch/mask ordering (59), the truthful base
save area (61), and before them the phantom frame (57) — are all correct, all
verified free of regressions, and none of them is the `bad sp`. That is worth
stating plainly rather than letting a run of green suites imply progress on the
symptom.

**Nothing has been on air.**

---

## Step 62 — nobody saved it: the task table is being written from outside

Recording the writer instead of deducing it. The save path in `task_schedule()`
now latches the task and its `EPC3` the first time `current_sp` equals
`_phy_stack_top` exactly:

```
bad sp : task 5 sp 0x3ffc0000 outside 0x3ffb9e90..0x3ffba690
phytop : never saved at _phy_stack_top
```

Both true in the same run. The scheduler never saved that value, and the
scheduler is the only code that assigns `g_tasks[].sp` after creation — so
`g_tasks[5].sp` is being written by **something that is not the scheduler**.

This is a wild store into kernel data, not a register-window problem.

### Why the last several steps could not have worked

Steps 58-61 all treated `0x3ffc0000` as a stack pointer that some code path had
legitimately produced, and looked for the path: the switch/mask race, the base
save area sentinel, a second `phy_stack_call` caller. Three fixes came out of
that, all correct, none of them the symptom — because the premise was wrong. The
value never flowed through a stack pointer at all.

`_phy_stack_top` is simply what happens to sit at the address being clobbered, or
what happens to be written there. The name misled every inference built on it,
including mine, for four steps.

### What to do

Find the writer, the same way:

1. **Bracket it in time.** `g_tasks[5].sp` is checked once per switch already.
   Record the tick and the running task the first time it goes bad, and compare
   against `last osi` — that says which adapter entry the blob was in when the
   table was clobbered.
2. **Bracket it in space.** `g_tasks[]` sits in DRAM near the stacks and near
   `_phy_stack` (`0x3ffbe800..0x3ffc0000`). A guard word either side of
   `g_tasks[]`, checked per switch, converts "something writes here" into
   "something overran that neighbour".
3. The blob's `.bss` is zeroed by `blob_init()` at `0x3ffd5018..0x3ffd90f8` and
   its `.data` copied to `0x3ffd4000` — both well clear of `g_tasks[]`, but the
   loader's range checks are worth re-reading against the linker symbols rather
   than trusted.

(2) is the cheapest and names the direction of the overrun immediately.

**Nothing has been on air.**

---

## Step 63 — not an overrun, and the value tracks the symbol

The task table is now fenced: one struct holding a guard word, `g_tasks[]`, and a
second guard word, checked once per switch.

```
ttab fence: intact both sides
bad sp    : task 5 sp 0x3ffc0020 outside 0x3ffba0b8..0x3ffba8b8
phytop    : never saved at _phy_stack_top
last osi  : entry 29  _queue_recv
epc       : 0x7ffffff0   (no DOUBLE line -- a first-level fault this time)
```

Three things follow.

**It is not a linear overrun.** Both fences are untouched, so nothing is walking
into `g_tasks[]` from the object below or above it.

**The value follows the symbol.** Adding eight bytes of `.bss` for the fence
moved the reported `sp` from `0x3ffc0000` to `0x3ffc0020` — exactly the shift, so
it really is `_phy_stack_top`'s address and not a coincidence of the number.

**And the scheduler still never wrote it.** `phytop` remains silent, so the value
does not arrive through `g_tasks[g_current].sp = current_sp`.

A targeted write of a known symbol's address into one field of one task, with the
fences either side untouched, is not corruption by accident. It is something
computing a pointer into the table.

### Also changed

`last osi` is now **entry 29 `_queue_recv`**, not 15 `_semphr_take`, and the
fault is a first-level `IllegalInstruction` at `0x7ffffff0` rather than a double
exception. The blob is getting further than it was — far enough to reach a
different adapter entry — which is consistent with steps 57-61 having removed
real obstacles even though none of them was this.

### Next

The write is targeted, so catch it by address rather than by symptom. `g_tasks[]`
is at a known DRAM address and `sp` is at a known offset within each entry:

1. Take the address of `g_tasks[5].sp` and watch that one word — sample it in
   `task_schedule()` and in the blocking stub, before and after each blob call,
   and latch the first transition together with `last osi`. That brackets the
   write to a single adapter entry.
2. The ESP32 has no data watchpoint available without a debug probe, which is
   why this is sampling rather than a trap. The probe remains ordered and not in
   hand.

**Nothing has been on air.**

---

## Step 64 — it happens before the driver runs at all

Latching `last osi` and the tick at the moment the table first goes bad:

```
bad sp : task 5 sp 0x3ffc0020 outside 0x3ffba0bc..0x3ffba8bc
         at tick 1433, last osi none yet
last osi (at panic): entry 29  _queue_recv
```

**`last osi none yet`.** No adapter entry had been reached when `g_tasks[5].sp`
first went bad, though by the time of the panic the blob has called fifteen of
them and got as far as `_queue_recv`.

So the write does not come from the WiFi driver. It happens in the window between
the command starting and the blob's first call into the OS adapter — which is
`blob_map()`, `blob_init()` and `phyinit_run_at()`, and nothing else.

That retires the framing of steps 58-63 completely. Every one of them looked for
the writer inside the driver's execution or the window machinery serving it. The
value was already wrong before the driver started.

### What is in that window

- `blob_map()` reprograms the flash MMU and invalidates the cache
- `blob_init()` word-copies `.data` to `0x3ffd4000` and zeroes `.bss` at
  `0x3ffd5018..0x3ffd90f8` — both far above `g_tasks[]`, and both range-checked,
  though the checks are worth reading against the linker symbols rather than
  trusted
- `phyinit_run_at()` primes `_phy_stack` (`0x3ffbe800..0x3ffc0020`) and calls
  `register_chipv7_phy` on it

The reported value is the *address* of `_phy_stack_top`, and `phy_stack_prime()`
is the one thing in that window that iterates to exactly that address:

```c
for (uint32_t *p = _phy_stack; p < _phy_stack_top; p++) { ... }
```

A loop bound and a written value being the same symbol is worth more than a
coincidence, and it is checkable directly: the prime writes a fill pattern, so if
it is the writer, `g_tasks[5].sp` would hold the pattern, not the address. It
holds the address — so something is storing the *pointer*, not dereferencing it.

### Next

Bisect the window rather than reason about it. Run `blob` alone, then
`blob` + `blobphy`, checking `bad sp` after each. Three runs name which of the
three stages does it, with no new instrumentation at all — every probe needed is
already in the build.

**Nothing has been on air.**

---

## Step 65 — bisected: PHY init is the stage

The `bad sp` check now announces at the latch rather than only in a panic dump,
so any command is self-reporting. (`nestfault` was no help — it goes through
`kernel_panic_msg`, a different entry point that does not print the window
diagnostics.)

```
nothing      ok
blob         ok        <- blob_map + blob_init are clean
blobphy      BAD       <- [!] task 5 sp 0x3ffc0020 left its stack
wifiinit     BAD
```

`blob_map()` and `blob_init()` are innocent. The corruption happens inside
`phyinit_run_at()` — `phy_stack_call` → `register_chipv7_phy`.

That is a large narrowing. The search space went from "somewhere in WiFi init,
across a driver, an adapter table and a task" to a single call that has been
working since rev 1.1 and reports `rc=0`.

### What it means that PHY init is where it happens

`phy_stack_call` masks interrupts for the whole call, and since step 59 the mask
is taken before the switch — so **no context switch can occur during the PHY
call at all**. The scheduler cannot be saving anything, which matches `phytop`
staying silent.

So `g_tasks[5].sp` is written by a store that is not the scheduler, during a
window in which the scheduler is not running. That is the blob's own PHY code
writing outside its stack, and the value it leaves is the address of
`_phy_stack_top`.

The obvious reading — the PHY writes a pointer to its own stack top somewhere it
should not — is exactly the kind of inference that has been wrong four times in
this investigation, so it is written here as a candidate and not a conclusion.

### Next

`blobphy` is now a one-command reproducer that runs in seconds and needs no
driver, no adapter table and no blob task. That is a far better instrument than
`wifiinit task`, and the first thing to do with it is bracket the write inside
the call:

1. sample `g_tasks[5].sp` immediately before and after `phy_stack_call` in
   `phyinit_run_at()` — if it is bad after, the write is inside the PHY call
2. if so, `phy_stack_used()` already reports how deep the PHY went; compare
   against the 6 KB buffer and the distance from `_phy_stack` to `g_tasks[]`

**Nothing has been on air.**

---

## Step 66 — the write is inside the PHY call, and the scheduler is running when it shouldn't be

Bracketing `phy_stack_call` in `phyinit_run_at()`:

```
[!] task 5 sp 0x3ffc0020 left its stack
[phy] saved sp of task 5 changed across the call: 0x3ffba810 -> 0x3ffc0020,
      phy used 1296 of 6144 B
```

Two facts, and the second is the important one.

**It is not a buffer overrun.** The PHY used 1296 of 6144 bytes. Nothing walked
off the end of `_phy_stack`.

**The scheduler ran during the call.** `g_tasks[].sp` is written by nothing else
after task creation, and the `[!]` line — emitted from `task_schedule()` — is
printed *before* the post-call bracket. So a tick was serviced while `a1` was on
the private PHY stack, and the task was duly saved pointing at it.

That should be impossible. `g_phy_call_mask` is `1` (`.word 1` in `.data`, not a
`.bss` zero), so `phy_stack_call` takes `rsil a9, 3`, and the tick is level 3.

### The candidate

`phy_exit_critical()` in `vendor/windowed/phy_host.c`:

```c
if (g_crit_depth > 0 && --g_crit_depth == 0) {
    ps = (ps & ~0xFu) | (g_crit_saved & 0xFu);   /* step 35 made this INTLEVEL-only */
}
```

with `g_crit_saved` captured only on the *outermost* enter:

```c
if (g_crit_depth++ == 0) { g_crit_saved = ps; }
```

The blob calls these around its own critical sections. If `g_crit_depth` is ever
stale — an unbalanced pair, or a depth left over from an earlier call — then the
level restored is one captured **outside** `phy_stack_call`'s masked region, and
`INTLEVEL` drops to 0 in the middle of the PHY call. Interrupts return, the tick
fires, and the scheduler saves a task that is running on a stack it does not own.

This is a candidate with a mechanism, not a conclusion — the last five of those
were wrong, and the way to settle it is the same as every time: record
`g_crit_depth` and the `INTLEVEL` actually restored, rather than reason about
when they could be wrong.

### Why this matters beyond the symptom

If the blob can drop the kernel's interrupt level from inside a masked region,
then *no* masked region in this system is safe while blob code runs — which is a
much larger statement than one corrupted `sp`, and it would apply to
`phy_stack_call`, `crit_enter()` and the blob lock equally.

**Nothing has been on air.**

---

## Step 67 — no: the blob never touches the interrupt level

Step 66's candidate was that the blob lowers `PS.INTLEVEL` through
`phy_exit_critical()` with a stale `g_crit_depth`, re-enabling interrupts inside
`phy_stack_call`'s masked region. Instrumented and measured:

```
[phy] crit enter/exit 0/0   never lowered the level
```

**Zero.** `register_chipv7_phy` never calls the critical-section API at all, so it
never restores an interrupt level, so it cannot have re-enabled interrupts that
way. The hypothesis is dead, and with it step 66's larger worry that no masked
region in the system is safe while blob code runs. That worry was unfounded.

### What survives

```
[!] task 5 sp 0x3ffc0020 left its stack
[phy] saved sp of task 5 changed across the call: 0x3ffba810 -> 0x3ffc0020
```

`g_tasks[5].sp` still changes across the PHY call, and `phytop` still never fires,
so the value is still not arriving through `g_tasks[g_current].sp = current_sp`.

And the arithmetic still refuses the simple reading: `phy_stack_call` builds at
`a8 = _phy_stack_top - 64` = `0x3ffbffe0`, so `a1` inside the call is
`0x3ffbffe0`, never `0x3ffc0020`. The recorded value is the **top itself**, which
no stack pointer in that function ever holds. This is the same arithmetic that
retired the mid-switch race at step 60, and it retires the "a tick caught it on
the PHY stack" reading here for exactly the same reason.

### What that leaves

Something writes the *address* `_phy_stack_top` into one word of the task table
during the PHY call, while the scheduler is masked out, without overrunning the
table's fences and without going through the save path.

The one thing not yet checked is the simplest: whether `g_tasks[5].sp` is where
this code thinks it is. Every conclusion above assumes the address of that field,
and the `[!]` print reports a value read back through the same expression that
would be wrong if the assumption were. Print `&g_tasks[5].sp` alongside
`_phy_stack` and `_phy_stack_top` and confirm they are distinct regions — a check
that costs one line and has never been done.

**Nothing has been on air.**

---

## Step 68 — the value was never `_phy_stack_top`, and four steps rest on that

Printing the addresses instead of assuming them:

```
[phy] &tasks[5].sp 0x3ffb0158   _phy_stack 0x3ffbe8d0..0x3ffc00d0
```

Two results.

**No aliasing.** The task table is at `0x3ffb0158`, far below the PHY stack. The
assumption that `g_tasks[5].sp` is where the code thinks it is was correct.

**The constant was wrong.** `_phy_stack_top` is `0x3ffc00d0`. The reported bad
value is `0x3ffc0020` — **inside** the PHY stack, 176 bytes below the top, not
the top itself.

`0x3ffc0000` came from an `nm` dump taken many steps ago and was never re-read
after the symbol moved. Every "the arithmetic refuses this" argument since has
been built on it:

- step 60 retired the mid-switch race because "a1 would be top-64, never the top
  exactly" — but the value was never the top
- step 61 replaced the base-save-area sentinel to stop it being restored as an sp
- step 62 concluded the scheduler "never saved it", from a `phytop` check that
  compares against `_phy_stack_top` and so could never match a value that is not
  it
- step 67 repeated the same arithmetic to retire the tick-on-the-PHY-stack
  reading

**So the original diagnosis was right all along.** `0x3ffc0020` is a perfectly
ordinary stack pointer *within* `_phy_stack`, which is exactly what a task caught
by a tick during `phy_stack_call` would be saved with. Steps 58 and 59 had it,
and a mis-transcribed constant argued it away.

### What is actually still open

Why a tick is serviced at all during a call that takes `rsil a9, 3`, with
`g_phy_call_mask` confirmed `1` and the mask taken before the switch since
step 59. That is the one question, and it is now a narrow one.

### The lesson, stated plainly

A constant read once and carried forward is an assumption, not a measurement.
This one survived four steps of reasoning *because* the reasoning was careful —
each argument was internally valid and every one of them was wrong at the root.
The `phytop` probe built on it could not have fired, so its silence was read as
evidence when it was a tautology.

Same family as steps 45, 54 and 63: an instrument that cannot report the thing
it is being trusted to rule out.

**Nothing has been on air.**

---

## Step 69 — yes, the blob re-enables interrupts. Directly.

Capturing `EPS3` — the PS the handler saved from the interrupted context — at the
instant the tick lands:

```
[!] task 5 sp 0x3ffc0020 left its stack
    eps3 0x00060320  intlevel 0
```

**INTLEVEL 0.** The mask was not in force. `phy_stack_call` executes
`rsil a9, 3`, `g_phy_call_mask` is confirmed `1`, and the interrupted context was
nevertheless running at level 0.

Step 67 measured that `phy_enter_critical`/`phy_exit_critical` are called 0/0
times, and that remains true. The blob does not lower the level through the
adapter. **It lowers it directly** — its own PHY code writes PS, as ESP-IDF's PHY
does via `XTOS_SET_INTLEVEL`, and nothing in this kernel is consulted.

So the answer to "did the blob turn interrupts back on" is **yes**, and step 67's
"no" was correct only about the route it tested. Measuring one path and reporting
it as the whole answer is the same error as step 28 and step 59.

### What this actually means

`phy_stack_call`'s mask is not a guarantee. It masks on entry, and the callee is
free to unmask — which vendor PHY code does as a matter of course, because under
IDF it runs on a FreeRTOS task with a normal stack and nothing depends on the
level staying raised.

Under nat-os something does: the private PHY stack. The whole design of
`phy_stack_call` — switch to `_phy_stack`, run, switch back — is safe only while
no context switch can occur, and the thing preventing that switch is a mask the
callee discards.

That is why a task ends up saved with `sp` inside `_phy_stack`: entirely
predictable, once the mask is known to be advisory rather than binding.

### What follows

The private stack and the mask cannot both be kept. Either:

1. **Drop the private stack.** Run PHY init on the caller's own task stack, as
   `blob_call` already does for `esp_wifi_init_internal` since step 28. Measured:
   the PHY used 1296 bytes, which a 2 KB task stack cannot absorb — so this needs
   the caller to be a task with a bigger stack, which `task_create_with_stack()`
   now provides.
2. **Keep the private stack and make the switch survivable** — teach the
   scheduler that a task can legitimately be on `_phy_stack`, which means the
   stack-guard and bad-sp checks need to know about it, and two contexts must
   still be excluded by the blob lock rather than by the mask.

(1) is smaller, matches what was already done for the WiFi init path, and removes
a shared buffer rather than adding bookkeeping around it.

**Nothing has been on air.**

---

## Step 70 — the `bad sp` was never a defect

Pinning the PHY call did not silence the check:

```
[!] task 5 sp 0x3ffc0020 left its stack   eps3 intlevel 0
[phy] saved sp of task 5 changed across the call: 0x3ffba820 -> 0x3ffc0020
phyinit rc=0     wintorture CORRECT     wincollide runs=119 wrong=0
```

And the reason is the point. `blob_pin()` stops the scheduler switching to a
**different** task. It does not stop the current task's context being *saved*
every tick — with `sp` legitimately on `_phy_stack`, because that is genuinely
where it is executing — and then restored to the same task on the way out.

That save and restore are symmetric. Nothing is lost. **A task saved with `sp`
on the private stack is not a bug**; it is what running on a private stack looks
like from the scheduler's side.

So the check I added at step 58 — "a saved sp outside the owning task's stack" —
encodes an assumption that was never true of this kernel: that a task only ever
executes on its own stack. `phy_stack_call` exists precisely to violate that.

### What the thread was worth anyway

Steps 58-69 chased this as corruption and it was not corruption. But the chase
produced two things that stand independently:

- **The mask is advisory** (step 69, rev 1.5 §13). The blob writes PS directly
  and drops `INTLEVEL` to 0 inside a region the kernel masked. That is real,
  measured, and applies to anything relying on a mask across a blob call.
- **A stale constant survived four steps of careful reasoning** (step 68), and
  the probe built on it could not fire, so its silence read as evidence.

Neither would have been found without the false trail, which is not a defence of
the false trail.

### What to do with the check

Teach it the exception rather than delete it: a saved `sp` inside `_phy_stack`
while that task holds the blob lock is expected. Anything else outside the
owning task's stack is still worth catching — that property is what would have
caught a real corruption, and it has never actually been violated.

### Where that leaves `wifiinit task`

Unchanged, and now without a spurious explanation attached to it. The failure is
still `IllegalInstruction` reaching `_queue_recv`, with the window bookkeeping
consistent, the phantom frame gone, and the PHY path pinned and excluded.

**Nothing has been on air.**
