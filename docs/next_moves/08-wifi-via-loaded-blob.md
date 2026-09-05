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

---

## Step 71 — the false signal is gone, and the real one is unobscured

The `sp` check now knows that `_phy_stack` is a legitimate place for a task to be
executing, and asserts the rest:

```
blobphy      rc=0     no spurious [!]
wintorture   CORRECT
wincollide   runs=119 wrong=0
bad sp       none -- every saved sp was inside its own stack
```

**"none" is the useful result.** It says there is, and was, no stack-pointer
corruption anywhere in this system — the property the check was written to assert
now holds and is stated rather than assumed. Thirteen steps were spent on a
signal that meant "a task ran on the private stack", which it was designed to do.

### The real failure, unobscured

```
exccause 28 (LoadProhibited)   DEPC 0x40080103   epc1 0x4008b34b
last osi : entry 15  _semphr_take
```

`0x40080103` is `_WindowOverflow12 + 3`: the `l32e a0, a1, -12` that recovers a
caller's stack pointer before spilling through it. A frame is being overflowed
whose `a1` is not a stack pointer.

That is the same fault as step 51, and everything since has been either a real
but unrelated fix or a false trail. What is different now is the ground it stands
on:

- window bookkeeping is internally consistent (56, 57)
- the phantom frame from bad ownership is gone (57)
- no task's saved `sp` is ever wrong (71)
- the PHY path is pinned and excluded, not merely masked (69, 70)
- the mask is known to be advisory across a blob call (69)

So the frame with the bad `a1` is not produced by any of those, and the next
question is narrow: **which frame, and who built it.** The overflow probe already
records the recovered base and the frame `sp` into `EXCSAVE 6/7`; it needs the
same treatment the `sp` check just got — an exception for what is legitimate, so
that what it reports is what is wrong.

**Nothing has been on air.**

---

## Step 72 — `wifiinit 0x101` was a stale number, not a regression

Plain `wifiinit` panics: `LoadProhibited` at `_WindowOverflow12 + 3`, the same
fault as `wifiinit task`. Two single-variable tests cleared the obvious suspects:

| test | result |
|---|---|
| step 70's pin around `phy_stack_call` removed | still panics |
| step 61's base-save-area change reverted | still panics |

So it was bisected instead:

| commit | `wifiinit` |
|---|---|
| `09cf215` (HEAD) | PANIC |
| `be82d1c` | PANIC |
| `4423f36` | PANIC |
| `b817349` | PANIC |
| `c16d257` | **PANIC** |

`c16d257` is the commit whose own message reads
`wifiinit 0x101 ESP_ERR_NO_MEM`. It was already panicking when that was written.

**There is no regression.** `0x101` was measured once, well before that commit,
and then carried forward through every "standing state" table since — in commit
messages, in `next_moves/08`, and in UM-NATOS-038 rev 1.4 and 1.5 — without being
re-measured. The suites run in between mostly exercised `wifiinit task`, and the
one number nobody checked stayed in the report looking like evidence.

This is the same failure as step 68's `_phy_stack_top`, and it is the fourth of
its kind in this investigation:

- a constant read once and carried forward (step 68)
- a counter that could not produce the value it was trusted to rule out (54)
- a probe comparing against a stale symbol, so its silence was a tautology (62)
- **a result carried in a summary table long after it stopped being true (here)**

The rule that covers all four: **a number in a report is a claim, and a claim
that is not re-measured decays into an assumption without changing appearance.**

### Consequence

`wifiinit` and `wifiinit task` fail identically, which means the fault has nothing
to do with blob task creation. The reproducer is smaller than believed: no task,
no `_semphr_take` blocking path required. `esp_wifi_init_internal` alone
overflows a window through a frame whose `a1` is not a stack pointer.

Standing state, re-measured rather than recalled:

```
boot 11 PASS 0 FAIL   wintorture CORRECT (switches=0)   wincollide runs=125 wrong=0
blobphy rc=0, phystack 1296/6144        blobtx force 0x00003004
wifiinit       PANIC  LoadProhibited, DEPC 0x40080103
wifiinit task  PANIC  LoadProhibited, DEPC 0x40080103
bad sp none
```

**Nothing has been on air.**

---

## Step 73 — the overflow probe was reporting its own uninitialised state

`EXCSAVE 6/7` are written by `_WindowOverflow8/12` on every overflow, and until
the first one they hold whatever survived reset. `of_sample`'s filter was
`base < 0x3ff00000`, which a zero satisfies perfectly — so the probe latched
before any overflow had occurred, and `base 0x00000000 recovered from frame sp
0x00000000` has meant *nothing happened yet* since step 48.

Seeded to `0xFFFFFFFF` (from `task_schedule()`, not `kmain.c` — kmain is
flash-resident and step 25 measured that adding to it walks into the layout
band) and filtered against the seed:

```
of filter : no near-null base recovered
```

Twenty-five steps of "AFTER spill, base 0x0" were the probe firing on itself.
Fifth instance of the class, and the most expensive: unlike the others this one
produced a *positive* reading rather than a silence, which is far more
persuasive and no more true.

### What the fault actually is, now that nothing is lying

```
exccause 29 (StoreProhibited)   DEPC 0x40080100   excvaddr 0x00000009
windowbase 7   windowstart 0x0000e4c8   last osi 15 _semphr_take
```

`0x40080100` is the **first instruction** of `_WindowOverflow12`:

```asm
s32e a0, a13, -16
```

`excvaddr 0x9` means `a13 = 0x19`. **The frame being spilled has a stack pointer
of 25.** Not a corrupted pointer — 25 is a plausible loop counter, index or
length. A register holding ordinary data is being used as a frame pointer,
because `WINDOWSTART` says a frame lives at that position.

`windowstart 0x0000e4c8` = bits 3, 6, 7, 10, 13, 14, 15 — seven live frames, with
`windowbase 7`. The blob is genuinely deep in windowed calls when this happens.

### Next

The probe records at the `l32e`, which is the *second* instruction — after the
faulting store. It never runs. Move the `wsr.excsave7 a13` to the very top of
`_WindowOverflow12`, before the first `s32e`, and the frame that owns the bad
pointer is named directly rather than inferred.

**Nothing has been on air.**

---

## Step 74 — the overflow recovers a fill pattern

The probe now records the frame pointer *before* the first store — the store is
what faults, so recording after it never ran — and the panic handler reads
`EXCSAVE 6/7` directly, because a bogus frame takes the system down inside the
handler and no downstream sample point is ever reached.

```
overflow  : frame sp 0x00000019   recovered base 0xeeeeeeee
exccause 29 (StoreProhibited)  DEPC 0x40080103  excvaddr 0x00000009
windowbase 7  windowstart 0x0000e4c8   last osi 15 _semphr_take
```

Two values, and both are decisive.

**`0xeeeeeeee` is a fill pattern.** The overflow handler recovered a caller stack
pointer out of memory that had been *filled and never written* — a base save area
that no `entry` prologue ever populated. That is the defect class `phy_stack_call`
documents and step 37 fixed in three call0 bridges; this is a fourth site, and it
is being reached from the blob's own windowed frames.

**`0x19` is not an address at all.** Twenty-five: a loop counter, an index, a
length. `WINDOWSTART` (`0x0000e4c8`, seven live frames at base 7) claims a frame
exists at a position whose registers hold ordinary data.

The two go together. A frame whose save area was never written yields a garbage
caller pointer on the way out; the window walks into a position that was never a
frame; the next overflow spills through whatever that register happens to hold.

### Why this is the end of the search rather than another step in it

Every previous candidate was eliminated by measurement — the window bookkeeping,
the phantom frame, the saved `sp`, the PHY stack sharing, the mask, the blob's
interrupt level, the `.data` placement, the loader. What is left is a specific,
named, mechanical fault with a known cure: **find the frame whose base save area
is unwritten, and write it**, exactly as `rom_call3`, `rom_call4` and
`win_spill_call0` now do.

The candidate sites are the call0 functions the blob's windowed code can rotate
over that are *not* those three: `blob_call()` itself, and the shell command frame
beneath it — both compiled `-mabi=call0`, both never writing `[sp-12]`.

**Nothing has been on air.**

---

## Step 75 — the frame is named: a call0 frame on the shell's own stack

Recording the frame pointer two deep, so the last good value survives the store
that kills the handler:

```
overflow  : prev good frame sp 0x3ffba520   this frame sp 0x3ffba520
            recovered base 0xeeeeeeee
task 5      sp 0x3ffba4a0  stack 0x3ffba0c4+2048  win 0x00002000@13  guard ok
```

**The frame pointer is valid.** `0x3ffba520` sits inside task 5's stack
(`0x3ffba0c4 .. 0x3ffba8c4`). It is a real call0 frame, not a garbage register —
step 74's `0x19` was a stale `EXCSAVE` read from before the two-deep record
existed, and reading it as "a counter used as a frame pointer" was wrong.

**And `[0x3ffba520 - 12]` is `0xeeeeeeee`.** That is `STACK_FILL`
(`kernel/task.c:30`), which `task_create()` writes across every stack. So the
base save area of that frame was **never written** — the memory still holds the
fill pattern from task creation.

That is the whole fault, and it is the class `phy_stack_call`'s comment has
described since rev 1.1: a call0 function whose frame windowed code rotates over,
which never populated `[sp-12]`, so `_WindowOverflow8/12` recovers fill and
stores through it.

### Which function

The frame is on the **shell task's** stack, at a shallower address than the
task's saved `sp` (`0x3ffba4a0`), so it is a caller of whatever was executing.
The call0 frames on that path during `wifiinit` are the shell command frame,
`blob_call()`, and `rom_call4` — and `rom_call4` has written its save area since
step 37.

So the site is `blob_call()` or the shell frame above it. Both are C compiled
`-mabi=call0`, which will never emit that store, so the fix needs an asm shim or
for the bridge to establish the boundary on their behalf.

Naming it by address rather than by argument is the point of this step: five
instruments in this investigation reported something other than what they were
trusted for, and patching the likelier-looking of two candidates would have had
even odds of producing another correct-but-irrelevant fix.

**Nothing has been on air.**

---

## Step 76 — writing the caller's save area from the bridge: no change

`rom_call3` and `rom_call4` were made to write `[caller_sp - 12]` with the
caller's own `sp` — a real address on the right stack, so a spill unwinding that
far would write inside the caller's frame instead of through a fill pattern.

No regressions (boot 11 PASS, `wintorture` CORRECT, `wincollide` runs=123
wrong=0, `blobphy` rc=0) and **no change to the fault**:

```
overflow : prev good frame sp 0x3ffba520   recovered base 0xeeeeeeee
```

Identical frame, identical fill. So the frame at `0x3ffba520` is not the one
either bridge writes for — `[caller_sp - 12]` for `rom_call4` lands elsewhere in
the chain. Reverted rather than left in: it is unproven code that fixes nothing,
and this investigation already carries enough of that.

### What the negative result narrows

The failing frame is at a fixed, reproducible address on the shell's stack, and
neither bridge's caller is it. That excludes the two candidates step 75 named by
elimination and leaves the frame identified only by address.

The next move is to name it directly rather than by reasoning about the call
chain, which has now been wrong twice:

1. `0x3ffba520` is stable across runs. The saved `a0` of that frame — at
   `[0x3ffba520 + 0]` for a call0 frame, or recoverable from the window — is a
   return address, and a return address maps to a function in the disassembly.
2. Simpler still: the panic already prints `epc1 0x4008b3a7`, the instruction
   that triggered the window exception. Disassembling *that* names the function
   whose call caused the overflow, and its caller is the frame in question.

Neither costs a build.

**Nothing has been on air.**

### Note on numbering — two series, deliberately not renumbered

From here the log carries **two independent step sequences** that collided at the
merge, and they are told apart by heading level:

* `### Step NN (branch dev)` — Tortoise's, written on `dev` before the merge.
  Covers 77.5 and 78, the phantom-sweep work that became UM-NATOS-039/040.
* `## Step NN` — Hare's, written on `main` after the merge. Restarts at 78 and
  runs to 86.

So `### Step 78` and `## Step 78` are different entries about different things.
Neither series is renumbered: commit messages, UM-NATOS-041 and the debug notes
all cite these numbers, and rewriting them would break every cross-reference to
fix a cosmetic clash. Anything after the merge that cites a bare step number
without a branch means the `##` series.

### Step 77.5 (branch `dev`) — H1 implemented as a controlled experiment, and refuted by its own instrumentation

**What changed** (kernel/vectors.S + panic.c + intr.c, all marked
`[H1 experiment]`): `_handler_level3` now reads the interrupted EPS3 before
dispatching. If PS.EXCM was set — the tick landed while the interrupted context
was inside a window handler — the handler services the sources (the tick must
still re-arm CCOMPARE1 or the scheduler freezes) but skips `task_schedule` for
that one tick, deferring the switch by 10 ms. Every hit increments
`g_tick_excm_hits`; the first hit records EPC3/EPS3. The counters print in the
panic dump (`tick-excm :`) and in `'intr'`.

Build note: the first build carried the defer decision across
`call0 intr_dispatch` in a5, which the call clobbers; no tick ever switched and
the hang detector reset the board every 3 s (TG0WDT_SYS_RESET boot loop). Fixed
by branching before the dispatch — two call sites, no cross-call state.

**Expected if H1 were right**: either wifiinit survives with hits > 0 (H1
confirmed and fixed), or it panics with hits > 0 recorded (preemption happens
but is not sufficient).

**Actual**: wifiinit panicked exactly as before — same StoreProhibited at
DEPC 0x40080109 (_WindowOverflow12+9), excvaddr 9, ws 0xe4c8, last osi entry 15
_semphr_take, saved frame on task 6's sp, M6 sp-change reproduced inside
blobphy too (0x3ffba820 -> 0x3ffc0040, rc=0, no crash) — and the counter read:

    tick-excm : never deferred

Zero EXCM-set preemptions occurred across the whole run, including the failing
excursion. The existing independent instrument agrees: `multiframe: 0
switch-outs with >1 live frame`.

**Conclusion**: H1 is ELIMINATED. A deterministic failure cannot be caused by
an event that provably does not happen. Neither preemption-flavoured mechanism
(tick mid-window-handler, switch-out with >1 live frame) occurs before the
fault. Attention shifts to what happens WITHOUT any preemption: H2 (something
writes over parked contexts / save areas in DRAM — note blobphy reproduces the
M6 sp-change harmlessly, so the change itself is not the poison) and H3
(bridge save-area writes; though upstream commit 154e5be already tested writing
the caller's save area from the bridge and it changed nothing).

The guard stays on `dev`: it costs nothing while its condition never fires,
and it doubles as standing X1 instrumentation for any future preemption theory.

Regressions on this build: wincollide runs=156 wrong=0; wintorture checksum
CORRECT (switches-during-call 0 as always); blobphy rc=0.

### Step 78 (branch `dev`) — the phantom sweep, caught on three instruments

H1 was eliminated by its own counter (step 77.5), so the window-state
transitions left on the failing path were the voluntary ones. Three
instruments went in, all marked `[X4/X5/X6 experiment]`:

1. `blk_sample` around every blocking excursion (pre-spill / post-spill /
   post-wake). Result: win_spill_all correctly reduces 6 frames to 1 on the
   failing excursion too; the excursion then blocks and NEVER returns (wake
   slot still holds its DEADBEEF sentinel at death).
2. `sbp-last`: spill_before_parking records task/wb/ws whenever it sees more
   than one live frame. Result: the fatal sweep runs under whichever task
   parks next after the pollution -- run A it was the display task reading
   six bits, none of them its own; run B it was task 5 itself reading two.
   One run's recovery walked into STACK_FILL: memory never written as a save
   area. The victim varies; the StoreProhibited-in-_WindowOverflow12
   signature does not.
3. switch-out/switch-in recorders in vectors.S. Result: the restore grants a
   CLEAN single bit every time (`wb 1 ws 0x00000002`), the paired save
   recorded one bit, no further switches happened before death, and yet the
   current call0-only task then read seven live-frame bits.

Mechanism (measured end to end): excursion parks mid-block -> stale
WINDOWSTART bits linger in hardware with no owner -> some later task's
spill_before_parking sees ">1 frames" and sweeps -> rotation lands on phantom
frames whose physical sp slots hold garbage (~25) -> OF12 faults ->
double exception.

Open question narrowed to one: WHO writes the phantom bits between switches,
while only call0 code is running? Next: tick-handler ring sampler or a
marked diagnostic clamp of spill_before_parking to the own-task mask
(confirmation-by-prevention).

Regressions after each build: wincollide runs=156 wrong=0, wintorture CORRECT,
blobphy rc=0. Nothing on air changed.

Step 79 (session 3): SOLVED. The phantom-bit writer was the context-switch
restore itself. vectors.S wrote wsr.windowstart AFTER wsr.windowbase+rsync,
so the a3 operand resolved to the wrong physical register whenever outgoing
and incoming WINDOWBASE differed (base-13 excursion -> any base-1 task);
stale register content truncated to 16 bits became ownerless WINDOWSTART
bits. Same-base grants worked only because old and new views name the same
physical slot. Fix: write windowstart BEFORE windowbase. Proof: restore
readback mismatched exactly at n398 (wrote 0x0002, committed 0xe9a8 = low
half of stale stack pointer 0x3ffbe9a8); after fix every restore commits its
computed mask, StoreProhibited gone, regressions green (wintorture,
wincollide, blobphy all corrupt=0 fault=none).

NEW, distinct, deterministic failure now reachable (wifiinit dies at tick
463, IllegalInstruction in ROM with CLEAN window state): the wifi driver's
own task 9 is the first genuinely multi-frame windowed task ever parked, and
spill_before_parking/win_spill_call0 assume call0-shell single-frame shape.
Next: hypothesis loop for that park path (H-A) vs wake-path/osi-glue (H-B),
before touching anything.

Regressions after each build: unchanged discipline, see session 3 record.

Step 80 (session 3 close-out): recorded as UM-NATOS-039
(docs/UM-NATOS-039-the-phantom-window-bits.md). Bug 1 (StoreProhibited double
exception during esp_wifi_init_internal) is closed: root cause was the
context-switch restore writing wsr.windowstart through a rotated register
view; fixed by writing windowstart before windowbase; verified by restore
readback on every grant and by all three regressions. Instrumentation
(ring sampler, restore readback, junk-source capture, restore history) stays
in the image.

NEXT STEPS are failure 2, not bug 1:

1. Reproduce once more on the current image to confirm the signature is
   stable at tick ~463 / epc 0x4008b8af / IllegalInstruction, task-9 park
   seven ticks earlier (two identical runs already logged).
2. Discriminate H-A vs H-B with one experiment, not a fix: make
   spill_before_parking record-and-skip (no win_spill_call0) for tasks whose
   base is NOT the shell base -- clearly marked diagnostic clamp, compared
   against unmodified behaviour per investigation rules. If wifiinit then
   survives past tick 463 or dies differently LATER in the queue path, the
   sweep-on-real-windowed-task corruption (H-A) is implicated; if the death
   is unchanged tick-for-tick, look at the wake path/osi glue (H-B).
3. If H-A wins, design the real mechanism for suspending genuinely multi-frame
   windowed tasks: either preserve the full WS word per task (and widen the
   restore contract accordingly), or give the driver task its own shell
   wrapper so it parks single-frame like everything else. Decide AFTER the
   experiment, from evidence.
4. Regressions after every build as always; note that they cannot see this
   class of defect while every regression task shares one window base --
   consider adding a regression task parked at a second base so cross-base
   switches stop being invisible to the suites.

Step 81 (session 4): X8 clamp (skip forced sweep) turned the fault into a
healthy livelock -> sweep is load-bearing, clamp reverted. X9 outcome probe
proved the sweep SUCCEEDS on task 9 (ws 0xa -> single-bit 0x8 @wb3,
"single-bit ok"); session-3's "seven-bit residue" was the stub's own
mid-chain sample point, not sweep output. H-A ELIMINATED. UM-NATOS-039
section 6 corrected.

Failure 2 now reads: t9 parks cleanly on _queue_recv and never wakes; no osi
activity afterwards; t5 faults IllegalInstruction seconds later at a
timing-variable PC in a tight ROM cluster (0x4008b8af@t463 vs
0x4008b977@t367 across builds differing only by instrumentation; 0x4008b8e4
in saved a7). Fault recorder corroborates across reboots.

NEXT (one variable each):
1. Extend panic dump to a8-a15; collect several runs; map the illegal-PC
   cluster and inspect t5's return chain for a corrupted link (H-D).
2. Use the existing level-3 ring to establish whether ANY interrupt arrives
   between t9's park and death; if none, find which interrupt source was
   supposed to post t9's queue and check whether nat-os routes it at all
   (H-C). The woe-watch "good crossings 17" and intenable 0x00808000 are the
   current baseline facts.
3. Only after H-C/H-D discriminate: design the real mechanism (interrupt
   route or wait-path repair). No speculative fixes.
4. Regression discipline unchanged; heartbeat dots now appear in all logs
   (X8b, marked diagnostic) -- ignore or filter them when diffing.

---

## Step 78 — the retw is illegal because `a0` is not a return address

Picked up from Tortoise's X20 handoff. X20's verdict — the ISA permits a
windowed callee to allocate its own bit and return via `retw.n` — makes the live
fault a contradiction worth resolving before X21 is built, because the live fault
*is* a `retw.n` with `bit(base) SET`.

Symbolized: `0x4008be47` is the **exit of `w2c_call2`**, the windowed->call0
bridge. Full dump:

```
exccause 0 (IllegalInstruction)   epc 0x4008be47   ps 0x00060f30
win-exit-ps: 0x00060f20   bit18(WOE) SET   bit4(EXCM) CLEAR
win-exit-ws: 0x00002000   wb 13   caller-bit CLEAR
a0/sp out  : 0x0000000d / 0x00000000
last osi   : entry 29 _queue_recv
```

`retw` raises IllegalInstruction under exactly three conditions: `PS.WOE` clear,
`PS.EXCM` set, or **`a0[31:30] == 0`** (callinc zero). The first two are measured
absent. `a0 = 0x0000000d` has callinc 0.

**So the instruction is illegal because `a0` holds 13, not a return address.**
Not a window-state fault, not an ISA question — X20's conclusion stands and is
simply not the issue here.

### An instrumentation bug found on the way, and it mattered

The dump's `sp out : 0x00000000` looked like a1 = 0, which is impossible: the
`l32i.n a0, a1, 0` two instructions earlier used a1 as a base and did not fault.
The layout explains it:

```
3ffb0ba4  g_win_sp
3ffb0ba8  g_win_a0     <- w2c_call2 stores a0 at +0 and a1 at +4 = 0x3ffb0bac
```

`g_win_sp` is *below* `g_win_a0`, so the a1 store lands on neither. The panic
reads `g_win_sp`, which this path never writes — a permanently-zero global
printed as a measurement. Seventh instrument of this class in this
investigation, and the first found by arithmetic on the symbol table rather than
by a contradiction on hardware.

With it discounted, `a0 = 0x0d` is a genuine capture and self-consistent.

### What this makes the next question

`w2c_call2` saves the windowed `a0` at entry, does `callx0 a8` (which destroys
a0 — it is the call0 return-address register), then restores it with
`l32i.n a0, a1, 0` before the `retw`. So either that stack slot held 13, or a0
was clobbered after the reload.

13 is also `windowbase`, which the immediately preceding instrumentation loads
into `a10`. That coincidence is worth exactly one check and no inference: it has
been a stack pointer's low half once already (X7).

Concretely: record `[a1+0]` at entry and again at the reload. Equal and wrong
means the callee wrote through into the frame; different means the reload is
sourcing from a moved a1.

**Nothing has been on air.**

---

## Step 79 — step 78's diagnosis is falsified, by its own instrument

Step 78 concluded the `retw.n` is illegal because `a0` held 13 (callinc 0). Two
measurements later, that is wrong on both legs.

### Leg 1: the value was never paired to an invocation

The first attempt recorded a0/a1 at the entry save and again at the reload, into
singleton globals, and compared them:

```
a0 trace : saved 0x8008d129 @ 0x3ffb91c0   reloaded 0x8008d227 @ 0x3ffb2760   ADDRESS MOVED
```

`0x3ffb91c0` is on task 5's stack and `0x3ffb2760` on task 9's. They are
different invocations — another task calls through `w2c_call2` between our entry
and our reload, and each pass overwrites the globals. "ADDRESS MOVED" measured
nothing. Both values are in fact *valid* windowed encodings (bit 31 set,
callinc 2).

That is the eighth instrument of this class in this investigation, and I wrote it
in the same step that documented the seventh. The lesson has to be structural
rather than remembered: **a singleton global written on a hot path cannot
attribute anything, and pairing two of them is worse than reading one.**

### Leg 2: `a0` is never illegal where it is read

Replaced with a self-contained sticky latch — at the reload, `a1` *is* this
frame's pointer and `[a1+0]` is the slot just read, so a0 and a1 together need no
pairing. Latch only `a0[31:30] == 0`, only the first, so the record must belong
to the fatal crossing:

```
a0 trace : no illegal a0 latched
```

It never fires. `a0` is a valid windowed return encoding on every reload, and
nothing between the reload and the `retw` writes it. So `a0 = 0x0000000d` in the
step-78 dump was a stale singleton, exactly like leg 1.

### What that leaves, and what not to assert

The `retw.n` at `w2c_call2`'s exit faults with `IllegalInstruction`, with
`WOE` set and `EXCM` clear per the exit capture — though that capture is a
singleton too, and now carries the same caveat.

Step 78 also asserted the three ISA conditions for `retw` raising
IllegalInstruction from memory. Two of them (WOE clear, EXCM set) are
well-founded; the third — callinc 0 — was stated with more confidence than a
recalled ISA rule deserves, and it is the one the measurement just removed. It
should be checked against the ISA reference rather than re-argued.

### Next

A ring, not a singleton: latch `a0`, `a1` and `WINDOWBASE` for the last N
crossings of `w2c_call2`, tagged with `g_win_seq`, so the fatal pass is
identifiable rather than assumed. The same shape Tortoise used for the restore
history in X7, which is the one instrument in this codebase that has not lied.

**Nothing has been on air.**

---

## Step 80 — the ring names it: six window bits dropped across a switch

Eight entries of `{seq, a0, a1, WINDOWSTART}` recorded at `w2c_call2`'s `retw`
itself, so the newest entry *is* the faulting crossing:

```
#5  a0 0x8008cfa8  n=2  a1 0x3ffb9240  ws 0x00002aaa
#6  a0 0x8008d08d  n=2  a1 0x3ffb9240  ws 0x00002aaa
#7  a0 0x8008d03c  n=2  a1 0x3ffb9220  ws 0x00002aaa
#8  a0 0x8008d14a  n=2  a1 0x3ffb9240  ws 0x00002aaa
#9  a0 0x0000000d  n=0  a1 0x3ffb9240  ws 0x00002000
```

### Step 78 was right; step 79 was the wrong measurement

`a0 = 0x0000000d`, callinc **n=0**, on the fatal crossing. That is step 78's
original claim, and step 79 retracted it on the strength of a sticky latch that
never fired.

The latch was placed at the **reload** (`l32i a0, a1, 0`); the ring records at
the **retw**. `a0` is legal at the reload and illegal a few instructions later —
which is only possible if a context switch lands between them. The latch was
correct about its own instant and wrong about the one that matters, and step 79
generalised from it. Ninth instrument, same family, and the first where the flaw
was *where* it sampled rather than *what* it read.

Both retractions now stand corrected: the callinc-0 condition is real and
measured on this silicon, not recalled.

### What actually happens

`ws` drops **0x00002aaa -> 0x00002000** between the last good crossing and the
fatal one. `0x2aaa` is bits 1,3,5,7,9,11,13 — seven live frames. `0x2000` is bit
13 alone. Six frames' bits are gone, `a1` is unchanged, so it is the same frame
on the same stack.

That is the restore path narrowing a resumed task to its own base bit and
dropping its caller frames — steps 56-57's ownership rule. Their registers are
then reused by whatever ran in between, so when the task resumes and executes
`retw`, `a0` is no longer a return address.

It also matches step 55 exactly: windowed frames do not survive preemption. The
pin exists to stop that, and `last osi : entry 29 _queue_recv` is the blocking
path — which by design spills and calls `blob_unlock()` before waiting, releasing
the pin with frames still live.

### Next

The spill is supposed to reduce the task to one live frame before it parks, which
would make the one-bit restore correct. The ring says it had seven bits live at
crossings #5-#8. So either the spill did not run on this path, or it ran and the
frames were re-established before the switch. `sbp-last`/`sbp-post` already
report the spill's own view; correlating them with `g_xseq` is the next
measurement, and the ring is already tagged for it.

**Nothing has been on air.**

---

## Step 81 — two tasks, two failures, and the union confirms the mechanism

Read Tortoise's `blk_sample(0/1/2)` brackets — pre-spill, post-spill, post-wait —
which already exist in the blocking stubs and answer the question step 80 left
open:

```
blk-window: pre ws 0xa00a wb 3 | spill ws ...a2b wb 3 | wake ws 0xdeadbeef | union 0x00000000
sbp-last  : task 9 wb 3 ws 0x0000000a
sbp-post  : wb 3 ws 0x00000008  single-bit ok
retw ring : a1 0x3ffb9240  -> inside task 5's stack (0x3ffb8cf0 + 2048)
```

### The two are different tasks

`blk-window` and `sbp-*` carry `wb 3`, which `sbp-last` names as task 9, the blob
task. The faulting `retw` is task 5, the shell. So `last osi : entry 29
_queue_recv` is what task **9** last called — it is a global counter, and step 80
and UM-NATOS-041 rev 1.0 both read it as belonging to the dying task. Corrected
in 041 rev 1.1.

### Three results

1. **`wake ws 0xdeadbeef` is the sentinel — the post-wait sample never ran.**
   Task 9 entered the blocking `_queue_recv` and never came back. That is a
   second, distinct failure: a callback that blocks forever. It is not the fault
   the panic reports, and it deserves its own investigation rather than being
   folded into this one.

2. **`sbp-post : single-bit ok`.** The park machinery does reduce a task to one
   live frame — for task 9. So the spill is not simply missing, which was step
   80's leading hypothesis for why seven bits were live.

3. **`union 0x00000000` — this confirms step 80's mechanism from the other
   side.** With an empty union, a resuming task is granted exactly
   `1 << its own base`. Task 5 held seven frames, its recorded mask was one bit
   by the step-56 rule, and the union carried none of the rest. The frames were
   disclaimed, not unwound, and that is now measured at both ends rather than
   inferred at one.

### Not read as fact

`spill ws 0x0000000a2b` prints ten hex digits where the printer emits eight. Until
that print is fixed, no conclusion is drawn about whether `win_spill_all()`
reduced task 9's frame count inside the stub — which is exactly the quantity
step 80 wanted. Reading a malformed field as data is how the last nine
instruments went wrong.

### Next

Two separable threads, and they should not be merged again:

- **A.** Task 5's disclaimed frames — the restore must grant what the task held.
  That means recording the count at spill time rather than inferring one bit at
  restore, and `sbp-post` already proves the spill knows the number.
- **B.** Task 9's `_queue_recv` never returning. Whoever was meant to post to
  that queue either never ran or never posted; `sbp-skip` reads 0, so the park
  itself was not skipped.

**Nothing has been on air.**

---

## Step 82 — a0 is written once, and the slot it comes from is corrupt

Two measurements, and together they close the question of where `a0 = 13`
originates.

### The context switch is not the culprit

Checking the switch frame's saved `a0` at both ends, sticky, filtered to values
that are neither a code address nor a windowed encoding (`[1, 0x3fffffff]`):

```
a0 at save: always a valid address
a0 at rest: always a valid address
```

Never anomalous, in either direction. The save/restore path does not corrupt
`a0`, which removes step 80's presumed causal chain — "frames disclaimed,
registers reused" — as the explanation for *this* value. The `ws` drop is real
and still unexplained, but it is not how `a0` becomes 13.

### `a0` is written exactly once

Every reference to `a0` in the emitted `w2c_call2`:

```
4008bfca  s32i.n a0, a1, 0     store a0 to the frame
4008bfe8  l32i.n a0, a1, 0     <- the ONLY instruction that writes a0
4008bfed  s32i.n a0, a9, 0     read
4008c01a  s32i.n a0, a10, 4    read (the ring)
```

So `a0` at the `retw` *is* the value loaded from `[a1+0]`. The ring says that
value is `0x0000000d`. Therefore **the windowed frame's saved-a0 slot on the
task's own stack held 13 at the reload.**

That is a memory corruption of one word at a known address — not a register
effect, and not the same location as the switch frame's `a0`, which is why both
measurements can be true at once.

### Why step 79 could not have seen it

The step-79 sticky latch is in the source at `window.S:605`. `w2c_call2` begins at
line 636. Line 605 is inside **`w2c_call0f`** — the edit patched the first
matching reload pattern in the file, which belongs to a different bridge.

It never fired because it was watching the wrong function. Step 79's retraction
of step 78 was therefore wrong twice over: wrong sampling point *and* wrong
bridge. The ring, placed deliberately inside `w2c_call2`, is the instrument that
holds.

Tenth of the class, and the first whose flaw was *which function* it was
installed in. A pattern-matched edit into a file with four near-identical
functions will silently pick one, and `.replace(..., 1)` picks the first.

### Next

The question is now narrow and mechanical: **who writes 13 into
`[frame_sp + 0]` while the task is parked?** 13 is a small integer — a count or
an index, not a pointer — so it is a value being *stored*, and something is using
that address as a destination.

The spill is the obvious candidate: `win_spill_all()` and `_WindowOverflow*` both
write to computed save areas on this stack. A miscomputed base would land here.
`sbp-post` already reports the spill's own view of the frame count, and the ring
carries `a1` — pairing them by address rather than by task is the measurement.

**Nothing has been on air.**

---

## Step 83 — the slot is provably overwritten, and the neighbours look like a switch frame

A watch on the exact word: stamp `[a1+0]` at `w2c_call2`'s entry save, record the
frame address, and at the reload compare — guarded on `a1` being the same frame,
so it cannot pair two invocations the way steps 78-79 did.

```
slot watch: frame 0x3ffb9270  stamped 0x8008d359  came back 0x0000000d
neighbours +4 0x00000700  +8 0x3ffb0c78  +12 0x00000000
```

**Direct proof.** The same frame's `[sp+0]` held a valid windowed return encoding
when stamped and 13 when read back. One word of a live frame is overwritten while
the task is inside the blocking call. This is no longer an inference from `a0` —
it is the memory itself, before and after, at a known address.

### The signature

`13` is `WINDOWBASE`, and `_handler_level3` writes `WINDOWBASE` at offset 84 of
its 112-byte switch frame. But the neighbours point somewhere simpler. A switch
frame captured in an earlier dump reads

```
... 0xfffffff0  0x00000700  0x3ffb0930  0x00000000 ...   at offsets 12/16/20/24
```

and the watched slot's neighbours are `0x00000700`, `0x3ffb0c78`, `0x00000000` at
`+4/+8/+12`. The same three-word run, shifted by 16 — consistent with the watched
word sitting at **offset 12 of a context-switch frame** that overlaps this
windowed frame's storage.

Stated as a hypothesis, not a finding: the values differ between runs, so the
match is structural rather than exact, and this investigation has now been wrong
ten times by reading a resemblance as an identification.

### The test that settles it

`task_schedule()` already sees `current_sp`, which is the switch frame's base.
Latch whenever that base falls within 128 bytes of `g_slotwatch[0]` — the frame
being watched — and record both addresses. If a switch frame is pushed across a
live windowed frame, this catches it by address with no interpretation required.
If it never fires, the resemblance is a coincidence and the writer is something
else.

That is one comparison in code that already runs on every switch, and it does not
depend on any reading of the neighbour pattern above.

### Why the guard matters here

The `bne a10, a1, 2f` in the watch is what makes this evidence rather than
another paired-singleton error. Steps 78-79 compared an entry sample and a reload
sample that belonged to different invocations on different task stacks, and
concluded "ADDRESS MOVED" from it. This compares only when the frame pointer is
identical, so stamp and readback are provably the same frame.

**Nothing has been on air.**

---

## Step 84 — no switch frame ever lands on the watched frame

Step 83 noticed that the corrupted word's neighbours resembled a context-switch
frame, and flagged it as a resemblance rather than an identification. The test
that settles it needs no interpretation: `task_schedule()` already holds
`current_sp`, which *is* the switch frame's base, and the handler writes 112
bytes up from it. Latch whenever that range covers the frame `w2c_call2` is
watching.

```
overlap : no switch frame ever landed on the watched frame
```

Never fires. **Step 83's hypothesis is dead** — the byte-pattern match was
coincidence, exactly as it was recorded to be.

That is the whole content of the step, and it is worth its own entry because it
is the measurement that stopped a plausible wrong answer from being adopted.
Recorded late: it was originally folded into step 85's prose, which buried a
clean negative result inside the write-up of a positive one.

Its real value was directional. With switch frames excluded and the writer still
unknown, the only remaining move was to stop hunting the writer and change one
variable instead — which is what step 85 did, and what found the bug.

---

## Step 85 — FIXED: the bridges were saving `a0` in the callee's own scratch

### The discriminating experiment

Step 84's overlap test came back **"no switch frame ever landed on the watched
frame"**, killing step 83's hypothesis. What settled it instead was moving the
save one word and watching what the readback followed:

| store at | watched slot returns | `[sp+0]` | `[sp+4]` | `[sp+8]` | `[sp+12]` |
|---|---|---|---|---|---|
| `[sp+0]` | `0x0000000d` | `0x0000000d` | `0x00000700` | `0x3ffb0c78` | `0` |
| `[sp+4]` | `0x00000700` | `0x0000000d` | `0x00000700` | `0x3ffb0c78` | `0` |

The memory is **identical in both runs**. Moving our store just moved it into
another word the writer also owns, and the readback followed the writer rather
than the stamp. So this was never a targeted store: a block of at least four
words is being written, with a consistent payload.

And the payload names itself. `0x00000700` is 1792, which is
`BLOB_TASK_STACK_WORDS`; `0x3ffb0c78` is inside the blob task's stack. That is
the call0 callee's own data.

### The bug

`[sp+0]` upward is the **call0 caller-provided argument and spill area**. The
callee is entitled to write there. All four `w2c_*` bridges were saving the
windowed `a0` into it across `callx0`, and the callee overwrote it exactly as the
ABI permits.

The bridges had no right to that memory. `a12..a15` are callee-saved under call0,
so the callee must preserve them — which makes `a12` both correct and free:

```asm
    mov     a12, a0         /* was: s32i a0, a1, 0 */
    callx0  a8
    mov     a0, a12         /* was: l32i a0, a1, 0 */
```

Applied to `w2c_call0f`, `w2c_call1`, `w2c_call2` and `w2c_call3`.

### Result

```
boot 11 PASS 0 FAIL   wintorture CORRECT   wincollide runs=121 wrong=0
blobphy rc=0          blobtx force 0x00003004
wifiinit  -- no fault, no reset, system alive
```

**The panic is gone.** `esp_wifi_init_internal` now executes without faulting for
the first time since the vendor stack began running from flash. It does not yet
return — that is thread B, the blocking `_queue_recv` that never comes back,
identified separately in step 81 and unchanged by this.

### What this was, in the end

Ten instruments lied along the way, and the fault was mistaken for a register
window problem, a scheduler problem, a stack-pointer problem and an ISA problem
in turn. It was none of those. It was three lines in a bridge I wrote, storing
one word in memory that belonged to somebody else — and it only ever surfaced
once a callee big enough to use that scratch ran on the far side of it.

The measurement that cracked it was not cleverer than the ones before. It was
just the first that changed one variable and watched whether the *evidence*
followed the change rather than the story.

**Nothing has been on air.**

---

## Step 86 — the hang is a wait for hardware that was never wired

### Why it hangs

`osi_impl_queue_recv` honours its timeout correctly. The driver simply passes
`OSI_MAX_DELAY`, which is correct FreeRTOS usage: the WiFi task sleeps until an
ISR or a timer posts work. nat-os has neither — `_set_intr` clamps and counts,
the timer entries are stubs — so nothing can ever post.

Both sides then block permanently: the blob task inside `_queue_recv`, and the
shell inside `esp_wifi_init_internal` waiting on it. Measured — the shell stops
answering the UART entirely.

An infinite wait for an event that cannot occur is a hang, and a hang reports
nothing at all.

### The scaffold

`OSI_FOREVER_CAP` (400 ticks, ~4 s) cuts a "forever" wait short and returns a
timeout the driver already knows how to handle, with `g_osi_capped` counting how
often it fires. This is bring-up scaffolding, not design: when interrupts are
wired it comes out, and the counter is what will say whether it still triggers.

It works. `blk-window` now reads `wake ws 0x00002800 wb 13` where it read
`0xdeadbeef` before — the post-wait sample runs, so the blocking call returns.

**Read with care.** Past this point the driver is doing *what it does when its
queue times out*, not what it does normally. Anything downstream is informative
about our OS and much weaker evidence about the driver.

### The next fault

```
exccause 28 (LoadProhibited)   DEPC 0x400800d5   excvaddr 0x00000170
DOUBLE EXCEPTION   epc1 0x4008c1da
windowbase 1   windowstart 0x00000008   bit(base) CLEAR
underflow : recovered a0 0x3ffd8f78 from save area 0x3ffb2810
```

`0x400800d5` is inside `_WindowUnderflow8` (vector at `0x400800c0`), faulting on
a load through a near-null base.

Two things stand out and neither should be run with yet:

- `bit(base) CLEAR` — the current frame's own `WINDOWSTART` bit is *not* set,
  which is an inconsistent window state rather than a merely surprising one.
- the underflow recovered `0x3ffd8f78` as a return address, which is inside the
  blob's `.bss` (`0x3ffd5018..0x3ffd90f8`) — data, not code.

The retw ring is clean this run (`n=2` on every entry including #9), the slot
watch reports `[sp+0] never diverged`, and `a0` is valid at save and restore. So
the step-85 fix is holding and this is a different failure, not a return of the
old one.

### State

```
boot 11 PASS 0 FAIL   wintorture CORRECT   wincollide runs=121 wrong=0
blobphy rc=0          blobtx force 0x00003004
wifiinit  -- reaches deeper, faults in _WindowUnderflow8
```

**Nothing has been on air.**

---

## Step 87 — the new fault decoded: another uninitialised base save area

The faulting instruction, exactly:

```
400800c0 <_WindowUnderflow8>:
400800c0  l32e a0, a9, -16      <- a0  = [a9-16]  = 0x3ffd8f78   (blob .bss!)
400800c9  l32e a1, a9, -12      <- a1  = [a9-12]
400800cf  l32e a7, a1, -12      <- a7  = [a1-12]  = 0x190        (garbage)
400800d5  l32e a4, a7, -32      <- FAULTS, excvaddr 0x170 = 0x190 - 32
```

With the probe values `excsave5 = a9 = 0x3ffb2810` and `excsave4 = a0 =
0x3ffd8f78`, the chain reads cleanly backwards:

- `a9 = 0x3ffb2810` — near the top of **task 9's** stack (`0x3ffb0c1c + 7168`).
- `[a9-16] = 0x3ffd8f78` — inside the blob's `.bss` (`0x3ffd5018..0x3ffd90f8`).
  A data address sitting where a return address belongs.
- `[a1-12] = 0x190` — not an address at all.

So the base save area for that frame was **never written**, and the underflow
walked a chain of uninitialised words. Same defect class as step 37
(`rom_call3`/`rom_call4`/`win_spill_call0`) and step 85 (the `w2c_*` bridges),
now at the blob task's own call0-to-windowed boundary rather than the shell's.

### What is already covered

`rom_call3` writes its own base save area explicitly:

```asm
    addi    a8, a1, 48          /* the caller's sp */
    addi    a9, a1, -12
    s32i    a8, a9, 0
```

and the blob task enters through it — `blob_task_entry()` -> `blob_lock()` ->
`rom_call3(fn, arg, 0, 0)`. So the boundary `rom_call3` owns is not the gap.

### What to check next, in order

1. **Whose frame is `0x3ffb2810`?** Task 9's stack runs `0x3ffb0c1c..0x3ffb282c`,
   so this is within 28 bytes of the top — the outermost frame of the task, not a
   deep one. `task_create_with_stack()` builds the initial frame there and no
   windowed prologue has ever run above it.
2. **Does the CALL8 out of `rom_call3` leave the callee a usable chain?** Under
   the windowed ABI a CALL8 callee's `a1` comes from `ENTRY` reading the caller's
   `a1`, but the caller's `a9` becomes the callee's `a1` register beforehand —
   and `rom_call3` uses `a9` as scratch for the base-save-area write immediately
   before the call. That ordering deserves reading against the ISA rather than
   assumption; it has not been checked.
3. **The step-85 fix is not implicated.** The retw ring is clean (`n=2` on every
   entry), the slot watch reports `[sp+0] never diverged`, and `a0` is valid at
   save and restore across the run that produced this fault.

Point 2 is the one worth doing first, and it is a static check — no board time,
and it either finds a real ordering bug or removes the largest remaining
candidate.

**Nothing has been on air.**

---

## Step 88 — the underflow walks off the top of the task's stack

### The static check clears `rom_call3`

Step 87 flagged the CALL8 ordering as the largest remaining candidate:
`rom_call3` uses `a9` as scratch for the base-save-area write immediately before
`call8`, and under CALL8 the caller's `a9` becomes the callee's `a1`.

It is harmless. CALL8 makes `a8` the callee's `a0` — which the instruction itself
writes with the return address — and `a9` the callee's `a1`, which `ENTRY`
overwrites on the callee's very first instruction, computing it from the caller's
`a1`. Junk in either register never survives to be read. Candidate removed, with
no board time.

### Where the fault actually points

```
task 9 stack   0x3ffb0c1c .. 0x3ffb281c
top & ~15      0x3ffb2810
initial sp     0x3ffb27a0     (task_create: top-112)
underflow a9   0x3ffb2810     <- exactly top & ~15
reads          [0x3ffb2800 .. 0x3ffb280c]
```

`a9` is **the aligned top of the stack**, not a frame within it. The underflow is
using the top of the task's stack as a frame pointer and reading the last 16
bytes of the initial context frame as though they were a windowed base save area.

Those bytes are frame offsets 96..111. `TASK_FRAME_WORDS` is 23 — 92 bytes — so
`task_create_with_stack()` zeroes 0..91 and leaves 92..111 as padding it never
writes. The underflow is reading padding and following it as a chain.

### What that means

The blob task's windowed chain has **no terminator**. Nothing marks its outermost
frame as the end, so an underflow that unwinds one frame too far walks off the
top of the stack into memory that was never a save area — which is exactly the
`0x3ffd8f78` (blob `.bss`) and `0x190` the handler recovered.

This is the same family as steps 37, 85 and 87, and it is the last boundary in
the chain: not a bridge that forgot to write its save area, but the *end* of the
chain having no save area to write.

### Two candidate fixes

1. **Terminate the chain.** Have `task_create_with_stack()` — or the blob task's
   entry — write a valid base save area at the top of the stack, so an underflow
   that reaches it finds a sane caller pointer instead of padding. Cheap, and it
   makes the failure survivable rather than preventing it.
2. **Stop the unwind.** The chain should never unwind past the entry frame in the
   first place. `bit(base) CLEAR` in the dump says the window state believed the
   current frame was not live, which is what forces the underflow — so this is
   the same window-ownership question, arriving from a new direction.

(1) is a guard; (2) is the cause. Worth doing (1) first anyway, because a task
whose chain terminates cannot corrupt memory outside its own stack while (2) is
being investigated.

**Nothing has been on air.**

---

## Step 89 — the terminator is correct, and something overwrites it

Implemented step 88's candidate (1): `task_create_with_stack()` now writes a
valid base save area into the 16 bytes at `[top-16..top-1]` for **every** task —
`a0` a well-formed CALLINC-1 return encoding aimed at a trap, `a1` the top
itself so `[a1-12]` stays inside the task's own stack and cannot fault, `a2`/`a3`
zero. Those are frame offsets 96..111, past the 92 bytes the initial context
frame actually uses, so nothing collides.

No regressions:

```
boot 11 PASS 0 FAIL   wintorture CORRECT   wincollide runs=123 wrong=0
blobphy rc=0          blobtx force 0x00003004
```

### It does not hold

The fault is byte-identical — `excvaddr 0x170`, same instruction — and the probe
says why:

```
underflow : recovered a0 0x3ffd8f78 from save area 0x3ffb2810
```

`0x3ffb2810` is exactly where the terminator was written, and `[0x3ffb2810-16]`
should now read `0x4008....` — the trap encoding. It reads `0x3ffd8f78`, an
address inside the blob's `.bss`.

**So the terminator is being overwritten between task creation and the
underflow**, and by something that deals in blob `.bss` pointers.

That is a new fact and a better one than the guard would have been. The write
lands 96 bytes *above* the blob task's initial stack pointer (`0x3ffb27a0`),
which no correct code should touch: a task's own frames grow downward from there.

### What it does not mean

It does not mean the guard is wrong, and the guard stays. The invariant — a
task's windowed chain terminates inside its own stack — is correct independently,
costs four stores at creation, and will hold for every task that is not being
scribbled on. It converts this failure from "reads unknown memory" into "reads
memory somebody else wrote", which is a strictly better starting point.

### Next

Watch the terminator the way step 83 watched the frame word: it is at a fixed
address per task, known at creation, and it has exactly one legitimate writer
(none, after creation). Sampling it per switch and latching the first change —
with the running task recorded — names the writer by task rather than by
inference, which is the move that has worked every time it has been used here and
the one that failed every time it was skipped.

**Nothing has been on air.**

---

## Step 90 — the terminator holds; the fault moves much later

### The watch named the writer, and it was the task itself

```
terminator: task 6's was clobbered while task 6 ran:  0x400884d4 -> 0x00000000
```

Task 6 overwrote **its own** terminator, which exposed a flaw in step 89's guard
rather than an external writer. `_WindowUnderflow8` reads a caller's frame from
`[a9-16..a9-4]`, so terminating the chain requires the 16 bytes *below* the top
of the stack to be valid — but the handler pops its 112-byte frame and leaves
`a1 = top`, and the task's entry then allocates downward straight through that
same region. The terminator was sitting in memory the task's own first frame
uses.

Fixed by reserving it: `top -= 16` before the initial frame is placed, so a
task's usable stack ends below the terminator and can never reach it.

```
terminator: intact for every task
boot 11 PASS 0 FAIL   wintorture CORRECT   wincollide runs=121 wrong=0   blobphy rc=0
```

### A premature claim, corrected in the same step

A 50-second run showed no panic and was reported as "the fault is gone". It was
not: a 90-second run panics at **68.5 s**. The window was too short, and the
absence of a fault inside it was read as the absence of a fault.

That is the same error as `wifiinit 0x101` in step 72 and the truncated dump in
step 59 — a measurement whose bounds were not stated being treated as a result.
Recorded here rather than quietly fixed, because it is the third instance.

### What did change

The fault is unchanged in identity — `LoadProhibited`, `_WindowUnderflow8+0x15`,
`excvaddr 0x170` — but it now takes **68.5 seconds** to arrive, where it was
near-immediate before. The driver is getting much further through init, grinding
through repeated `OSI_FOREVER_CAP` timeouts on the way.

### The open contradiction

```
terminator: intact for every task
underflow : recovered a0 0x3ffd8f78 from save area 0x3ffb2810
```

If `0x3ffb2810` were still task 9's terminator, "intact" and a recovered
`0x3ffd8f78` cannot both be true. The likely resolution is dull: `g_blob_stack`
is a static array whose address moves with every build, so `0x3ffb2810` may no
longer belong to that task — or to any task.

**Not assumed.** The next run should print the task table alongside the underflow
address, which the panic already does, and the two should be compared rather than
matched from memory of an older layout. That specific mistake — carrying an
address across a rebuild — cost four steps at 68.

**Nothing has been on air.**

---

## Step 91 — it does not walk off the top: the bad frame is mid-chain

Step 90's contradiction is resolved by comparing against **this build's** task
table rather than an address remembered from an older one:

```
task 9  sp 0x3ffb2710   stack 0x3ffb0cdc + 7168  ->  0x3ffb0cdc .. 0x3ffb28dc
underflow save area     0x3ffb2810
terminator (top & ~15, less the 16 reserved)  0x3ffb28c0
```

`0x3ffb2810` **is** inside task 9's stack — so both readings were true and not in
conflict. It is simply not the terminator: it sits 256 bytes *above* task 9's
current stack pointer and 176 bytes *below* the terminator.

### This corrects step 88

Step 88 concluded the underflow "walks off the top of the task's stack" because
`a9` matched `top & ~15` in that build. It does not. The chain dies at a frame
**in the middle** of task 9's own call chain, well inside the stack, and never
reaches the end at all.

The match at step 88 was a coincidence of that build's layout — the same class of
error as step 68's stale `_phy_stack_top`, and caught this time only because the
address was re-derived from the live task table instead of recalled.

### What the terminator work was, then

Steps 89 and 90 fixed a real defect — a task's chain had no terminator, and the
one added was being overwritten by the task's own first frame until 16 bytes were
reserved for it. That is correct and it stays. But it was never going to fix
*this* fault, because this fault never reaches the terminator.

Its measurable contribution: the panic moved from near-immediate to 68.5 s, which
is the driver getting much further through init. Whether that is the reservation
or the `OSI_FOREVER_CAP` timeouts accumulating is not established.

### Where the fault actually is

A frame at `0x3ffb2810` in task 9's chain has a base save area holding
`0x3ffd8f78` — an address inside the blob's `.bss` — where a return address
belongs. Task 9's chain from the top down is `blob_task_entry` (call0) ->
`blob_lock` -> `rom_call3` (call0, writes its own save area) -> the blob's
windowed code.

So the next question is narrow: **which frame is at `0x3ffb2810`, and who was
supposed to write `[0x3ffb2800]`?** The answer is one disassembly and one
comparison against `rom_call3`'s known frame size, not another hypothesis.

**Nothing has been on air.**

---

## Step 92 — the bridges' save areas were half-written; the bad frame is the blob's

### A real gap, closed

`_WindowUnderflow8` reads **both** halves of a base save area:

```
l32e a0, a9, -16      <- the return address
l32e a1, a9, -12      <- the caller's sp
```

Step 37 taught the call0 bridges to write `[sp-12]`, because
`_WindowOverflow8/12` recover the caller's sp from there. Nothing ever wrote
`[sp-16]`. Confirmed in the emitted `rom_call3`:

```
4008c1eb  addi a8, a1, 48
4008c1ee  addi a9, a1, -12
4008c1f1  s32i.n a8, a9, 0      <- [sp-12] only
```

So any underflow unwinding into a bridge frame recovered an uninitialised word as
its return address. `rom_call3` and `rom_call4` now write `[sp-16]` too, pointing
at `win_chain_trap` — whose own address doubles as a valid CALLINC-1 return
encoding, since kernel code lives at `0x4008xxxx` and bits 31:30 are already 01.

No regressions: boot 11 PASS 0 FAIL, `wintorture` CORRECT, `wincollide`
runs=121 wrong=0, `blobphy` rc=0.

### It does not fix this fault, and that is the useful part

```
underflow : recovered a0 0x3ffd8f78 from save area 0x3ffb2810
```

Unchanged. If `0x3ffb2810` were a bridge frame, `[0x3ffb2800]` would now read
`0x4008....` — the trap. It still reads the blob `.bss` address.

The arithmetic agrees: `blob_task_entry`'s frame is 16 bytes and `rom_call3`'s is
48, so the chain reaches `0x3ffb2880` at the deepest bridge frame. `0x3ffb2810`
is a further **112 bytes down** — inside the blob's own windowed code.

**So the frame with the corrupt save area belongs to the blob, not to us.**

### Which puts it back on the ownership question

A windowed frame's save area is written by the overflow handler when the frame is
spilled. If the frame was never spilled, the underflow should never read it —
`WINDOWSTART` would say it is live in registers. But the dump reports
`bit(base) CLEAR`: the window state claims the frame is *not* live, so the
hardware underflows and reads memory nobody wrote.

That is step 88's candidate (2), and steps 89-92 have now excluded everything
around it: the chain terminates, the bridges' save areas are complete, and the
frame is the blob's own. What remains is why the window state disowns a frame
whose registers are still in use — the same question steps 53-57 circled, now
arriving with the surrounding possibilities eliminated rather than assumed.

**Nothing has been on air.**

---

## Step 93 — the "save area" was never a save area

One reading of the existing evidence, with no new instrumentation:

```
underflow : recovered a0 0x3ffd8f78 from save area 0x3ffb2810
```

`0x3ffd8f78` is **not** `STACK_FILL` (`0xEEEEEEEE`), and not a code address. So
that word is not uninitialised — something wrote it, deliberately, with a pointer
into the blob's own `.bss`.

Combined with step 92 (the frame is 112 bytes below the deepest bridge frame,
inside the blob's own code), the simplest account that fits every measurement is:

**`[0x3ffb2800]` was never a windowed save area at all.** It is ordinary stack
data the blob wrote — a local, a spilled pointer — and the window machinery is
reading it as a frame because `WINDOWBASE`/`WINDOWSTART` claim a frame lives at
that position when it does not.

That inverts the last several steps. The question is not "who corrupted the save
area", it is "why does the window state point at a stack location that was never
a frame". Steps 89-92 were answering the first question, and each found a real
defect while leaving this one untouched — which is consistent with them having
been the wrong question.

### How this connects back

Step 80 measured `WINDOWSTART` dropping seven live frames to one across a switch,
and step 91 confirmed the frames are the blob's own. If a frame is disowned, its
window position is free for another context to occupy; when the original task
resumes and unwinds, the position it believes holds its caller now holds
somebody else's data — or nothing that was ever a frame.

`bit(base) CLEAR` in every dump of this fault says exactly that: the hardware is
being told a frame is not live at the base it is executing on.

### Status of this account

It is a hypothesis that fits all current evidence and contradicts none of it. It
is **not** measured, and this investigation has retired ten plausible accounts
that fit the evidence at the time. The discriminating test is the one step 88
named as candidate (2) and steps 89-92 have now cleared the ground around: record
what a task actually holds at switch-out, and compare it against what the restore
grants — for the blob task specifically, which is the only multi-frame windowed
program in the system.

**Nothing has been on air.**

---

## Step 94 — the restore never drops a frame, and `bit(base) CLEAR` was never a clue

### The discriminating test, run

Every switch now compares what a task held on the way out against what the
restore is about to grant it — `1 << saved_base | g_win_union` — and latches the
first shortfall with the task named:

```
frames : no task was ever granted less than it held
```

**Never.** Not once, across an entire `wifiinit` run to the fault at 68 seconds.

Step 93's account is refuted, and with it step 80's mechanism: frames are not
being disowned by the restore. That is the eleventh account in this
investigation that fitted all the evidence available at the time and was wrong.

### A false clue, removed

Every dump of this fault reports `bit(base) CLEAR`, and steps 88, 92 and 93 each
treated that as evidence of an inconsistent window state.

It is not. `retw` rotates `WINDOWBASE` back to the caller's position and takes an
underflow *precisely because* that position's `WINDOWSTART` bit is clear — that
is the mechanism by which a spilled frame gets reloaded from memory. The panic
handler reads the window state from inside the underflow handler, so
`bit(base) CLEAR` is the **normal, expected** condition there.

Reading it as an anomaly put a false plank under three steps of reasoning. Worth
recording as its own class: *a value that is abnormal in one context and routine
in another, read without asking which context produced it.*

### What that leaves

If no frames are lost and the bit is legitimately clear, then the frame at
`0x3ffb2810` **was** spilled, and its save area **was** written — by the overflow
handler, with that frame's real `a0`. And the value there is `0x3ffd8f78`, a
pointer into the blob's `.bss`.

So the question narrows again, and this time away from the scheduler entirely:
**why would a windowed frame's `a0` be a data pointer at spill time?** Either the
blob keeps a non-return value in `a0` across a call — legal in call0 code, not in
windowed code — or the frame being spilled was not windowed and something marked
it live anyway.

That is answerable by disassembling the blob around the return address the chain
*should* have had, which is static work on the image and needs no board time.

**Nothing has been on air.**

---

## Step 95 — the value is a named PHY global; the mixed-ABI theory is not supported

### The address has a name

```
3ffd8f68 B phy_rxbb_dc
3ffd8f78 B adc_ana_conf_org      <- the value the underflow recovered as a0
3ffd8f7c B set_most_tpw
```

`0x3ffd8f78` is exactly `adc_ana_conf_org`, a PHY variable in the blob's `.bss`.
So the word the underflow reads as a return address is the **address of a PHY
global** — not a corrupted pointer, not fill, but a meaningful value something
deliberately stored.

### The theory it suggested, and why it does not hold up

If the blob mixed call0 functions among its windowed ones, a call0 frame's `a0`
could legitimately hold data, and windowed code rotating over it would spill that
data as a return address — the exact hazard this project has documented for its
own call0 frames since rev 1.1.

Instruction census over the blob's 180,577 disassembled lines:

```
entry  2744    retw  3852        <- overwhelmingly windowed
ret       4    callx0   3        <- a handful
call8  7096    callx8 4889    call12 112    call4 102
```

Four `ret` and three `callx0` is **not** support for that theory:

- It is within the noise of **data misdecoded as code**. `objdump` disassembles
  `.text` linearly, and a 600 KB vendor blob with interleaved constants will
  produce spurious instructions at that rate.
- The enclosing symbols are `ieee80211_ht_node_init`, `is_esp_mesh_assoc`,
  `nan_set_config_local`, `esp_mesh_recv_xon`, `set_cca` — mesh and NAN paths
  that `esp_wifi_init_internal` does not walk.

So the blob is windowed throughout on every path that matters, and the mixed-ABI
account is recorded as **unsupported**, not as a lead.

### A near-miss worth recording

The first census reported `retw 0` and `ret.n 0`, which would have been a
startling finding — a windowed binary with no windowed returns. It was a broken
regex: the pattern required trailing whitespace, and `retw.n` ends its line.
Caught before it was written down, but only just, and it is the same failure as
the ten instruments before it.

### Where this leaves the fault

`a0` for that frame held `&adc_ana_conf_org` at spill time, the frame is the
blob's own, the restore never dropped it, and `bit(base) CLEAR` is routine. The
remaining question is unchanged and now quite specific: **what put a data pointer
in `a0` of a windowed frame** — which is either the blob doing something the
windowed ABI forbids, or that memory never being the frame's save area in the
first place.

Distinguishing those needs the frame's *other* saved words read alongside `a0`:
a real spilled frame has a plausible `a1` at `[sp-12]`, and the step-83 watch
already showed how to read a range rather than a single word.

**Nothing has been on air.**

---

## Step 96 — it IS a save area, on our own blocking path, and only `a0` is wrong

The vector slot is 64 bytes and would not hold the extra loads — the build
failed to link, which is a useful reminder that these handlers have no room.
They do not need any: `excsave5` already carries the frame address, so the panic
handler reads the whole save area itself.

```
uf frame @0x3ffb2810   a0 0x3ffd8f78   a1 0x3ffb27d0   a2 0x8008d7f0   a3 0x4008c490
```

Resolved:

| slot | value | what it is |
|---|---|---|
| `a0` | `0x3ffd8f78` | `&adc_ana_conf_org`, a blob `.bss` global — **wrong** |
| `a1` | `0x3ffb27d0` | a valid stack pointer inside task 9 — plausible |
| `a2` | `0x8008d7f0` | a windowed return encoding into **`osi_s_queue_recv`** |
| `a3` | `0x4008c490` | **`w2c_call3`** |

### This kills step 93

Three of the four words are exactly what a genuine spilled frame holds, and two
of them name our own code on the blocking path — `osi_s_queue_recv` calls
`w2c_call3` to reach `osi_impl_queue_recv`. That memory **is** a save area, it
belongs to a frame we created, and step 93's account — "never a save area at
all" — is refuted.

Twelfth account retired. It fitted the evidence available at step 93 because only
one word had been read; reading four settled it immediately. The lesson is not
subtle: *a single word cannot distinguish a corrupt frame from something that was
never a frame, and step 83 had already demonstrated reading a range.*

### What is left

One register, in a frame on a path we own, holding a pointer to a PHY global
where a return address belongs. Everything around it is intact: the stack pointer
is right, the neighbouring saved registers are right, the frame is where it
should be, the restore never dropped it, and the chain terminates.

So the question is now as small as it has been: **what writes `&adc_ana_conf_org`
into `a0` of a windowed frame on the `_queue_recv` blocking path?**

`w2c_call3` is the bridge in that frame, and since step 85 it keeps the windowed
`a0` in `a12` across `callx0` rather than on the stack — a change made to
`w2c_call0f`, `w2c_call1`, `w2c_call2` and `w2c_call3` together. Whether `a12`
survives *this* callee is the obvious next question, and it is one instruction to
check: a call0 callee must preserve `a12..a15`, and the blob's own functions are
what runs on the far side.

**Nothing has been on air.**

---

## Step 97 — `a12` is preserved; step 85's assumption is sound

Step 85 moved the windowed `a0` into `a12` on the grounds that the call0 ABI
makes `a12..a15` callee-saved. That was an assumption about every function
reached through the bridges, and it had never been tested. `w2c_call3` now
verifies it directly — a restored `a0` whose top two bits are zero is not a
windowed return encoding, so `a12` did not survive — and latches the offender.

```
a12 check : a12 survived every call0 callee
```

Never fires, across a full run to the fault. **The assumption holds** and step 85
is sound.

Worth noting what the callee actually is on this path: `w2c_call3` reaches
`osi_impl_queue_recv`, which is *our* code compiled `-mabi=call0`, not the blob's.
The blob is on the other side — it calls *into* `osi_s_queue_recv`, which is
windowed. So the question "does the blob preserve `a12`" does not arise on this
particular path; what was tested is the callee that is actually there.

### The fault is unchanged, and the save area still does not decode

```
uf frame @0x3ffb2820   a0 0x3ffd8f78   a1 0x3ffb27e0   a2 0x8008d850   a3 0x4008c4dc
```

Neither candidate frame explains all four words:

- If this were **`w2c_call3`'s** frame, after its argument shuffle `a2` would be
  the queue handle and `a3` the item pointer. They are a windowed return encoding
  and a `w2c_call3` code address instead.
- If it were **`osi_s_queue_recv`'s** frame, `a2`/`a3` would be its own arguments
  (queue, item) and `a0` a return encoding into blob code. `a0` is a blob `.bss`
  global.

So `a1` is consistently plausible, `a2`/`a3` are consistently code-shaped, and
`a0` is consistently a data pointer — across three separate runs at three
different addresses as the build shifts. That regularity is itself information:
the pattern is stable, so it is produced by something systematic, not by a race.

A shifted or misaligned view of the save area would produce exactly this shape,
and it has not been ruled out. Neither has the possibility that the frame belongs
to a function neither of the two considered.

### Standing

```
boot 11 PASS 0 FAIL   wintorture CORRECT   wincollide runs=121 wrong=0
blobphy rc=0          wifiinit  -- runs ~68 s into init, faults in _WindowUnderflow8
```

**Nothing has been on air.**

---

## Step 98 — our spill writes valid save areas; the defect is blob-path-specific

`WINDOWSTART` going 7 -> 1 (step 32) proved frames left the register file. It
never proved *what was written for them*. `_WindowUnderflow8` restores `a0` from
`[sp-16]`, so after a correct spill that word must be a windowed return encoding
for every frame on the chain.

The blob-free probe now walks the chain from the spilled frame upward through the
saved `a1` links and checks each `a0`:

```
spill: windowstart 0x0000aa8a (7 frames) -> 0x00000008 (1 frames)
spill: walked 11 frames, 1 with a non-encoding a0   first 0x00000000 at 0x3ffbb640
```

**Ten of eleven frames carry a valid encoding.** The single exception is
`0x00000000` at the end of the walk — the chain's terminus, where a zero is what
an unwritten slot looks like and the walk stops anyway.

So `win_spill_all` writes correct save areas, eleven frames deep, with no blob
involved. Our window machinery is not the defect.

### What that eliminates

Combined with step 97, two assumptions underpinning the recent work are now
measured rather than assumed:

- `a12` survives every call0 callee on the bridge path (step 97), so step 85's
  fix is sound.
- The spill writes valid save areas for our own windowed code (here), so the
  overflow handlers and `win_spill_all` are correct.

The corrupt `a0` on the `_queue_recv` path is therefore **specific to the blob's
own chain**, not a general failure of the spill, the bridges, the restore or the
scheduler — each of which has now been individually cleared by measurement.

### What remains, and what it would take

A frame on the blob's chain has `a0` holding `&adc_ana_conf_org` where a return
encoding belongs, with `a1` plausible and `a2`/`a3` code-shaped, reproducibly and
at three different addresses across builds.

Everything on our side of that boundary has been eliminated. What has *not* been
examined is the blob's own frames: how deep its chain runs, whether it uses
CALL12 (112 sites in the image), and whether any of its functions manipulate `a0`
in ways a windowed frame permits but our spill assumptions do not anticipate.

That is a different kind of work from the last twenty steps — reading vendor
disassembly along a specific call path rather than instrumenting our own — and it
should start from the return address the chain *should* have had, which
`osi_s_queue_recv`'s caller supplies.

**Nothing has been on air.**

---

## Step 99 — the caller is `ppTask`, and the call site confirms two earlier readings

Step 98 asked for the return address the blob supplies. Captured at the stub's
entry:

```
qr caller : 0x4036bb18  (raw a0 0x8036bb18)
```

`0x8036bb18` is a CALL8 encoding; the target resolves to `0x4036bb18`, inside
**`ppTask`** — the WiFi driver's main packet-processing task. Its call site:

```asm
4036bb0a  l32i.n a5, a2, 0        ; the OSI table pointer
4036bb0c  movi.n a12, -1          ; block_time_tick = -1
4036bb0e  mov.n  a11, a1          ; item = ppTask's OWN stack pointer
4036bb10  l32i   a5, a5, 116      ; table[116/4 = 29] = _queue_recv
4036bb13  l32i.n a10, a3, 0       ; the queue handle
4036bb15  callx8 a5
4036bb18  beqi   a10, 1, ...      ; check the result
```

Three things are confirmed from the vendor image rather than inferred:

1. **Offset 116 is entry 29.** The generated table's layout is right where the
   driver reaches for `_queue_recv`, which is independent confirmation of the
   work in UM-NATOS-038 §5.
2. **`a12 = -1` is `portMAX_DELAY`.** Step 86 concluded the driver waits forever
   by design; here is the instruction that does it.
3. **`item` is `ppTask`'s own stack pointer.** The driver hands us a pointer to
   a local as the destination buffer.

### The theory that suggested, and why it does not fit

If our queue's `item_size` were wrong, `copy_n(item, ..., item_size)` would
scribble on `ppTask`'s frame. But `item_size` is whatever the driver passed to
`queue_create` — 200 items of 8 bytes, the figure recorded in
`osi_impl_queue_create`'s own comment — so the copy writes 8 bytes at `[a1]`.

The corrupt save area is at `0x3ffb2810` and the frame's `a1` is `0x3ffb27e0`: an
8-byte write at the latter cannot reach the former. Recorded and dropped rather
than pursued.

### What this changes

The investigation has crossed a boundary. Everything through step 98 instrumented
our own code and cleared it — bridges, spill, restore, scheduler, `a12`, save
areas. This is the first step reading the vendor image along the failing path,
and it produced a named function, a confirmed table offset, and hard confirmation
of the `portMAX_DELAY` behaviour in one pass.

`ppTask` is the right place to continue: its frame is the one whose save area is
corrupt, its stack pointer is the one being handed to us, and its chain is the
one the underflow is walking.

**Nothing has been on air.**

---

## Step 100 — the save area is `ppTask`'s own, and only `a0` is wrong

`ppTask`'s prologue and call profile, from the vendor image:

```
4036baec <ppTask>:
4036baec  entry a1, 48            <- a 48-byte frame
...
entry 1   retw 1
call8 13  callx8 21
call4 0   call12 0   callx4 0   callx12 0
```

A single windowed function, 177 instructions, **CALL8 exclusively** — no CALL12
anywhere, which retires the "the blob uses call widths our spill does not
anticipate" strand before it was ever pursued.

### The arithmetic closes

```
ppTask sp   0x3ffb27e0
+ frame           48
=           0x3ffb2810     <- exactly the corrupt save area address
```

So the save area at `0x3ffb2810` describes **`ppTask`'s own** `a0..a3`, and its
`a1` slot holds `0x3ffb27e0` — `ppTask`'s stack pointer, correct. The layout is
right, the frame is the right frame, and the address is where it should be.

**Only `a0` is wrong**, and that is now established by arithmetic rather than by
resemblance.

### What `a0` should contain

`ppTask` is the function `blob_task_create` was handed, so the chain into it is
`blob_task_entry` -> `blob_lock` -> `rom_call3` -> `call8` -> `ppTask`. Its `a0`
should therefore be a windowed return encoding into `rom_call3` — a `0x8008....`
value, since our kernel lives at `0x4008xxxx` and CALL8 sets bits 31:30 to `10`.

The save area holds:

```
[a9-16]  0x3ffd8f78   a blob .bss global          <- a0, wrong
[a9-12]  0x3ffb27e0   ppTask's sp                 <- a1, correct
[a9-8]   0x8008d950   a 0x8008.... encoding       <- a2
[a9-4]   0x4008c5b4   a kernel code address       <- a3
```

A value of exactly the shape `a0` should have is sitting two slots along. That is
either a coincidence of what `ppTask` happened to hold in `a2`, or the save area
is shifted — and the two are distinguishable by reading `[a9-20]` and `[a9]` as
well, extending the window rather than arguing about it.

Recorded as an observation, not a conclusion. Twelve accounts have died here, and
several died on exactly this kind of pattern-match.

**Nothing has been on air.**

---

## Step 101 — not a shift: `a1` lands exactly where arithmetic says it should

Widened the save-area read two words either side, to distinguish "the layout is
shifted" from "a value of `a0`'s shape happens to sit nearby":

```
uf frame @0x3ffb2820
  -24 0x8008d987   -20 0x3ffb4278
  a0-16 0x3ffd8f78   a1-12 0x3ffb27e0   a2-8 0x8008d994   a3-4 0x4008c5f8
  +0 0x00000003      +4 0xffffffff
```

There is real structure — `(-24, -20)` is an `(encoding, pointer)` pair of
exactly the shape a saved `(a0, a1)` has, which is what a shifted layout would
produce.

**But the layout is not shifted.** `a1-12` holds `0x3ffb27e0`, and step 100
established by arithmetic that `ppTask`'s sp *is* `0x3ffb27e0` — `ppTask sp + its
48-byte frame = the save area address`. If the window were shifted, `a1` would
not land exactly where the frame size says it must.

So the `(-24, -20)` pair belongs to the next frame out, and the save area under
examination is at the right address with the right layout. One word inside it is
wrong and its three neighbours are right.

### What that rules out, and what it costs to finish

The shift hypothesis is dead, which was the last structural explanation
available. What remains is that **something writes one specific word**, and every
candidate on our side has been individually cleared: the spill writes valid save
areas eleven frames deep (98), `a12` survives every callee (97), the restore
never drops a frame (94), the bridges' save areas are complete (92), the chain
terminates (90), and `bit(base) CLEAR` is routine (94).

The decisive test is the one that has worked every time it was used in this
investigation and failed every time it was skipped: **watch that word.** Its
address is computable at the moment `win_spill_all` returns in the blocking stub
— the spill is what writes the save area — so recording the value there and
comparing it at the underflow separates "the spill wrote it wrong" from "the
spill wrote it right and something later changed it".

Those two have entirely different culprits, and no amount of further reading
distinguishes them.

**Nothing has been on air.**

---

## Step 102 — the watch was placed on the wrong word

Sampled a save-area word immediately after `win_spill_all()` in the blocking
stub, to separate "the spill wrote it wrong" from "the spill wrote it right and
something changed it".

```
sa watch  : @0x3ffb2880  after spill 0x4008c530  now 0x4008c530   UNCHANGED
uf frame  : @0x3ffb2820  a0-16 0x3ffd8f78  a1-12 0x3ffb27e0 ...
```

**Different addresses.** The watch is on `0x3ffb2880`; the failing save area is at
`0x3ffb2820`, 96 bytes lower. So the test did not run on the word in question.

### The arithmetic error

The stub computed the caller's stack pointer as `[my_sp - 12]`. That is wrong:
the base save area at `[sp-16..sp-4]` holds the `a0..a3` of the frame whose sp
*is* `sp` — so `[my_sp - 12]` is this frame's own `a1`, which is `my_sp` itself.
Reading it as the caller's sp is circular, and the address it produced happened
to land on a different, healthy frame.

The relationship the panic dump reports is also not what step 100 derived:
`a1-12 = 0x3ffb27e0` and the frame address is `0x3ffb2820`, a difference of 64,
not the 48 that `ppTask`'s `entry a1, 48` implies. Step 100 read that difference
as 48 in an earlier build and treated `ppTask sp + 48 = save area` as
established. **It is not**, and the discrepancy was visible in this run's own
numbers.

### What the run did establish

The word actually sampled — `0x3ffb2880`, on a neighbouring frame — held a valid
kernel code address after the spill and **still held it at the fault**. That is
consistent with step 98: the spill writes correct save areas and nothing
disturbs them. It is one more frame's worth of evidence for a conclusion already
reached, and nothing about the failing frame.

### What to fix before retrying

The stub cannot compute `ppTask`'s save-area address from its own frame without
the relationship being right, and this step shows the relationship is not
understood well enough to hard-code. Two ways out, both cheap:

1. Record `a0` at the stub's entry — it is `ppTask`'s return encoding — and
   derive nothing; instead have the *panic* handler, which already knows the
   failing address from `excsave5`, print what the stub sampled at that same
   address by sampling a small window rather than a single word.
2. Or sample the whole 64-byte region above the stub's own frame after the spill,
   and compare it word-for-word at the fault. Heavier, but it needs no
   arithmetic at all — and arithmetic is what failed here.

(2) is the safer of the two given the record.

**Nothing has been on air.**

---

## Step 103 — CONFIRMED: the handler is reading a call0 local as a stack pointer

UM-NATOS-042 §7 recorded that `excvaddr` is `0x00000170` in every run of this
fault, across builds in which every other address moves, and that
`a7 = 0x170 + 32 = 400` is exactly `OSI_FOREVER_CAP`. It was written up as a
correlation with three ways it could mislead, and the one-constant experiment
that would settle it.

Run. `OSI_FOREVER_CAP` 400 -> 460:

```
prediction:  excvaddr becomes 0x1AC   (0x1CC - 32)
measured  :  excvaddr : 0x000001ac    a7 = 0x1cc = 460
```

Exact. **`a7` is `spent`** — the loop counter in `osi_impl_queue_recv` — and the
fault address is a function of a constant in our own C source.

### What that establishes

The chain the underflow walks is:

```
a9 = 0x3ffb2820                     the frame being unwound
a0 = [a9-16] = 0x3ffd8f78           a blob .bss global
a1 = [a9-12] = 0x3ffb27e0           taken as the caller's stack pointer
a7 = [a1-12] = 460                  <- spent, a local of osi_impl_queue_recv
a4 = [a7-32]                        <- faults
```

So `[a9-12]` does not point at a windowed frame. It points **into
`osi_impl_queue_recv`'s call0 frame**, and `[a1-12]` — where the handler expects
the next caller's stack pointer — is that function's loop counter.

`osi_impl_queue_recv` is our own code, compiled `-mabi=call0`, reached through
`w2c_call3`'s `callx0`. A call0 frame has no windowed base save area: nothing
below it was ever written by an `entry` or by an overflow handler, because call0
frames do not participate in the windowed chain at all.

**The windowed chain has an `a1` link that points into a call0 frame.** That is
the defect, stated positively for the first time, and it is the hazard this
project has documented since rev 1.1 — appearing at a boundary nobody had
examined.

### Why every previous elimination still stands

Nothing in §5 of UM-NATOS-042 is contradicted. The restore does not drop frames,
the spill writes valid save areas, the bridges' save areas are complete, `a12`
survives. All of that is about frames that *are* windowed. This is a chain link
pointing at a frame that never was.

### Next

Two things, in order:

1. **Which frame's `a1` slot holds `0x3ffb27e0`, and who wrote it.** The frame at
   `0x3ffb2820` is one link out; its `a1` slot is what points into the call0
   frame. If an overflow handler wrote it, the value came from a register that
   held a call0 frame pointer at spill time.
2. `w2c_call3` is the bridge between the windowed stub and the call0
   implementation, and it is the only place in the chain where a windowed frame
   and a call0 frame are adjacent. Its own frame is where the boundary sits.

**Nothing has been on air.**

---

## Step 104 — why the chain always ends up in call0, and what actually fixes it

The observation that prompted this: *the chain will point back into call0 no
matter what the blob does, because that boundary is inherent.* That is correct,
and the disassembly gives the mechanism exactly.

```asm
w2c_call3:            entry a1, 32        windowed frame
                      callx0 a8           does NOT rotate the window

osi_impl_queue_recv:  addi a1, a1, -64    moves the WINDOWED frame's a1
                      s32i.n a3, a1, 0    locals at [a1+0]; no 16-byte reserve
```

Three facts compose into the defect:

1. **`callx0` does not rotate.** The call0 callee shares the windowed frame's
   register window, `a1` included.
2. **call0 code moves `a1` freely** and reserves nothing below it, because
   `-mabi=call0` has no windows to reserve for.
3. **A window exception during the callee captures whatever `a1` currently is**
   — a call0 stack pointer — and writes it into the windowed frame's save area
   as that frame's `a1`.

Later, an underflow walks the chain and reads `[saved_a1 - 12]` expecting the
next caller's stack pointer. It gets a call0 local. Step 103 proved that local is
`spent` by changing a constant and watching the fault address follow.

### Why the obvious fixes do not work

**Spill before `callx0`.** The stub already spills, but `w2c_call3`'s frame is
created *after* it, so it is live. Moving the spill inside the bridge does not
help either: `win_spill_all` returns, the frame is reloaded and live again, and a
later overflow spills it with the moved `a1`. The spill only helps if the frame
stays dead, and it cannot — the bridge has to return through it.

**Reserve 16 bytes in the bridge.** The window machinery always uses
`[current_a1 - 16]`, and `a1` is whatever the callee has made it. Reserving at
the bridge's own depth changes nothing.

**Mask window exceptions during the callee.** The callee blocks. Masking across a
block is a deadlock, and UM-NATOS-038 §13 already established that a mask taken
before vendor code is advisory anyway.

### What does fix it

The root cause is narrower than "call0 and windowed are mixed": it is
**a call0 function that BLOCKS while holding a windowed frame's `a1`.** A call0
callee that returns promptly is harmless — `a1` is restored before any spill can
capture it, which is why every non-blocking adapter entry has worked for months.

Two designs follow, and they differ in cost rather than correctness:

1. **Move the wait into windowed code.** Make the `osi_impl_*` functions
   non-blocking — poll and return — and put the wait loop in the windowed stub,
   which already spills before it sleeps. No call0 frame is then live across a
   block. This is the smaller change and it matches the structure the blocking
   stubs already have.
2. **Compile the `osi_impl_*` layer `-mabi=windowed`.** Then `call8` rotates, the
   callee gets its own frame, and the chain is ABI-consistent throughout. But
   those functions call `mutex_lock`, `task_sleep` and `heap_alloc`, which are
   call0 — so the boundary moves down rather than disappearing, and the blocking
   ones reintroduce the same problem one level deeper.

(1) is the recommendation. It removes the condition rather than relocating it,
and it is a restructure of code we own with no new ABI surface.

### What this explains retroactively

Every non-blocking adapter entry has worked since the table was accepted. Only
the five blocking entries route through `wait_on`, and only those hold a call0
frame across a park. The fault has always been on `_semphr_take` or
`_queue_recv` — never on the other 113.

**Nothing has been on air.**

---

## Step 105 — the wait moved into windowed code, and it was not enough

Implemented step 104's recommendation: `osi_s_queue_recv` polls non-blocking
while **pinned**, then waits **unpinned** inside `osi_windowed_idle()` — four
nested windowed frames with a short spin, so a context switch landing in the wait
finds `a1` belonging to a windowed frame with a proper save area beneath it. It
also keeps the window rotating while the driver waits, which is closer to what
the blob expects of a task blocked on a queue.

No regressions (boot 11 PASS 0 FAIL, `wintorture` CORRECT, `wincollide`
runs=118 wrong=0, `blobphy` rc=0), and **it did not fix the fault**:

```
epc 0x400800cf   excvaddr 0x000000f3
a0-16 0x3ffd8f78  a1-12 0x000000ff  a2-8 0x8008da21  a3-4 0x4008c59e
+0 0xeeeeeeee     +4 0xeeeeeeee
```

Different shape — `a1` is now `0xff`, and the words above are `STACK_FILL` — but
the same class: a save area whose `a1` slot is a small integer rather than a
stack pointer.

### Why it was not enough

The loop still makes three call0 excursions per round, and two of them can be
switched away from:

- `blob_unlock()` — runs pinned until it clears the pin, then returns *unpinned*
  through call0 code with `a1` moved. A tick landing in those few instructions
  reproduces the original condition exactly.
- `blob_lock()` — takes the blob mutex, and `mutex_lock` calls `task_block()` and
  `task_yield()`. **That is a call0 function that blocks**, which is precisely the
  condition step 104 identified. Moving the queue wait out of call0 did nothing
  about the lock wait, which is also in call0.

So the restructure removed one instance of the pattern and left two, one of them
a blocking call0 function reached on every round.

### What that teaches

Step 104's diagnosis is unchanged and still believed: *a call0 function that
blocks while holding a windowed frame's `a1`*. What is now clear is that the
condition is not confined to the adapter's own waits — **the kernel's own
synchronisation primitives are call0 and block**, so any windowed code that takes
a mutex has the same problem.

That is a larger statement than step 104 made, and it means fix (1) as scoped
there is insufficient by construction. A complete version needs either:

- the pin held across every call0 excursion, with the unpin issued *from windowed
  code* as a direct store rather than through a call0 helper; and the lock
  acquired without a call0 block — a try-lock loop with the retry in windowed
  code, not `mutex_lock`; or
- fix (2) from step 104: the `osi_impl_*` layer compiled `-mabi=windowed`, which
  pushes the boundary onto the kernel primitives and requires the same treatment
  of them.

Reverted. The change is more correct in principle than what it replaced, but it
is unproven, it converts a sleep into a spin, and keeping an unproven restructure
on the failing path would make the next measurement harder to read.

**Nothing has been on air.**

---

## Step 106 — the complete form, and it turns the fault into a clean reproducer

Implemented the full version of step 104's fix (1), with the invariant stated
exactly:

```
every call0 excursion runs PINNED            -> no switch can land in one
every wait runs UNPINNED in WINDOWED frames  -> switches land where a1 belongs
                                                to a windowed frame
pin and unpin are DIRECT STORES from windowed code -> no frame, no return
                                                      sequence, nothing to catch
the lock is acquired by TRY, never mutex_lock      -> no call0 block
```

New in the kernel: `g_pinned` is no longer static (windowed code writes it
directly), plus `blob_trylock()` and `blob_unlock_only()` — non-blocking halves,
because `blob_lock()` reaches `mutex_lock()`, which blocks, and step 105 showed
that is the same defect one level along.

No regressions: boot 11 PASS 0 FAIL, `wintorture` CORRECT, `wincollide`
runs=118 wrong=0, `blobphy` rc=0.

### The failure mode changed completely

```
exccause 29 (StoreProhibited)   epc 0x4008da92   excvaddr 0x00001000
```

**No double exception. Not in a window vector.** A first-level fault in our own
code, and the instruction is exact:

```asm
4008da87  s32i.n a2, a6, 0          ; UNPIN   -- a6 = &g_pinned, correct here
4008da89  call8  osi_windowed_idle  ; the unpinned wait
4008da92  s32i.n a7, a6, 0          ; REPIN   -- a6 is now 0x1000
```

`a6` holds `&g_pinned` across the call, is caller-saved in this frame, and **did
not survive**. That is the corruption itself, observed directly, in a single
register, in code we wrote — rather than inferred from a window handler walking a
broken chain eight steps later.

### Why this is worth keeping

For a hundred steps the fault has been visible only as a window handler
dereferencing something it should not. It is now visible as a plain invariant
violation with a one-line statement: *a register in a windowed frame did not
survive a windowed call made while the task was unpinned.*

That is a far better reproducer. It is first-level rather than a double
exception, it names the exact register and the exact instruction pair either side
of the call, and it does not depend on the underflow handler running at all.

**The underlying defect is unchanged** — a frame's spill and reload across an
unpinned window is losing register state — but it is now observable at the
moment it happens instead of at the moment something trips over it.

### What this does not do

It does not fix `wifiinit`, and the failure is now at 1.0 s rather than 68 s
because the spin-based wait cycles far faster than the old sleep. Neither number
is a measure of progress through init.

The obvious mitigation — re-derive `&g_pinned` after the wait instead of holding
it in a register — would **mask** this, not fix it, and would throw away the
clearest reproducer this investigation has produced. Left alone deliberately.

**Nothing has been on air.**

---

## Step 107 — the restore DOES drop frames, and step 94 measured a case that never happens

A blob-free test: hold magics in locals across a windowed call, **unpinned** so a
tick can land, and check what survives. No adapter, no queue, no vendor code.

```
frames : task 9 held 0x0000002a granted 0x00000020 LOST 0x0000000a
```

The step-94 check — *"no task was ever granted less than it held"* — has never
fired in a hundred steps. It fires on the first unpinned round. The task held
bits 1, 3 and 5; the restore granted bit 5 alone; bits 1 and 3 were dropped.

### Why step 94 was wrong

`wintorture` and `wincollide` both reach windowed code through `rom_call3`, which
takes `blob_lock` and therefore **pins**. The scheduler refuses to switch away, so
a task never gets switched out holding multiple live windowed frames — and the
frame-loss check has nothing to catch.

Step 94 ran that check across a full `wifiinit` and reported it never fired.
That was true and meaningless: **the unpinned case had no coverage at all.**
Every windowed test in this project has run pinned since the pin was introduced.

An instrument that cannot reach the condition it is testing reports the same
silence as one that finds nothing wrong. Eleventh of the class, and the first
where the gap was *coverage* rather than construction.

### What this restores

**Step 80 was right.** It measured `WINDOWSTART` dropping seven frames to one
across a switch and named the restore rule from steps 56-57 as the cause. Step 94
retracted that on the strength of a check that could not fire. The retraction is
withdrawn; the original mechanism stands, now with a blob-free reproducer.

The rule at fault:

```c
g_win_mask[t] = ((ws_out >> base) & 1) ? (1u << base) : 0u;
```

One bit, whatever the task actually held. Correct for a task parked after the
spill — which is every task the pin permits to be switched out — and wrong for a
task preempted mid-window, which only happens unpinned.

### Why this matters beyond the bug

The blocking adapter path *must* unpin: a callback that holds the pin while
waiting deadlocks against the task it waits for. So the WiFi driver is the first
thing in this system that routinely creates the one condition the window
bookkeeping was never tested against.

That is the whole fault, stated in one sentence, and there is now a test for it
that runs in two seconds without the blob.

### Next

Grant what the task held rather than one bit — step 88's candidate (2). Steps 53
and 54 both failed at this, but neither had a reproducer; every attempt was
validated against suites that ran pinned and therefore could not tell a fix from
a no-op. That is no longer true.

**Nothing has been on air.**

---

## Step 108 — the rule cannot be fixed by granting bits back

Three attempts against the step-107 reproducer, each measured rather than argued:

| change | frame loss | `wincollide` |
|---|---|---|
| mask = every live bit not owned elsewhere | still lost | ok |
| ...and stop zeroing the union on a real switch | **fixed** | **PANIC** |
| ...plus a spilled/preempted distinction per task | **fixed** | **PANIC** |

The first did nothing because step 57 discards the union on a real task change,
so the grant is only ever `1 << base` and the per-task masks never reach the
hardware at all. Keeping the union fixes the frame loss and immediately
reinstates step 57's phantom: `wincollide` panics in `_WindowOverflow8`.

### Why no version of this rule can work

Granting the bits back restores a *claim*, not the frames. After the task was
preempted mid-window, other tasks ran and rotated the window through those
positions, and **the physical registers were reused**. Marking them live again
tells the hardware to spill registers that now belong to somebody else — which
is exactly the phantom, and exactly what `wincollide` catches.

So the two reproducers are not in tension because the rule is subtly wrong. They
are in tension because the information does not exist: once the registers are
reused, the frames are unrecoverable, and no bookkeeping can bring them back.

### What that leaves

Only two positions are self-consistent:

1. **Never preempt a task mid-window** — the pin. Correct today, and the reason
   every windowed test passes. Its cost is that the blocking adapter path must
   release it, which is precisely where the WiFi driver lives.
2. **Spill on preemption** — `_handler_level3` pushes the outgoing task's window
   to its own stack when it holds more than one live frame, so there is nothing
   to lose. Then the one-bit restore is not merely safe but *correct*, because
   one frame is genuinely all that remains.

(2) is the answer, and it is the one attempted five times in steps 14-18 and
abandoned. What is different now: `win_spill_call0` exists and is measured to
work from task context, `spill_before_parking` already does exactly this on the
voluntary path, and there is a reproducer that can tell a working spill from a
no-op — which none of those five attempts had.

Reverted to the state at step 107: the reproducer reports the loss, `wincollide`
runs clean, and nothing pretends the rule is fixed.

**Nothing has been on air.**

---

## Step 109 — Tortoise's discriminator: `a6` was a symptom (outcome 3)

Tortoise proposed testing **H-windowed-reg-loss** by re-deriving `&g_pinned`
after the wait instead of holding it across, with three outcomes that
discriminate between mechanisms. Step 106 had declined this as "masking"; that
was the wrong reading — masking a symptom to see what is behind it is a
measurement, and the three-way split is what makes it one.

Baseline re-verified first rather than assumed: `StoreProhibited`,
`excvaddr 0x00001000`, at 1.0 s. Then the change — address re-derived after the
wait, value routed through a volatile local so nothing lives in a register across
`call8 osi_windowed_idle`.

```
exccause 28 (LoadProhibited)   epc 0x400800cf   (_WindowUnderflow8 + 0xf)
DOUBLE EXCEPTION               excvaddr 0x000000f3
uf frame @0x3ffb2810   a0-16 0x3ffd8f78   a1-12 0x000000ff
```

**Outcome 3.** The `0x1000` store fault is gone and the fault reverts to the
window-handler double exception, with `a1` a small integer and `[a1-12]` read as
a stack pointer — the same shape as `0x170`/`0x1ac`, differing only in which
call0 local now occupies the slot (`0xff` rather than `spent`, because the code
around it changed).

Per the experiment's own discriminator, that means:

> `a6` was a symptom, and the cause is `w2c_call3`'s `entry`/`callx0` moving the
> windowed `a1` — requiring fix (2): compile `wifi_osi_impl.c` windowed, or have
> `w2c_call3` allocate its own window.

### What this settles

Step 106's register-loss finding was real but downstream. The `a6` clobber and
the underflow fault are the same defect seen at two depths, and step 104's
diagnosis — a call0 callee moving a windowed frame's `a1` — is the one that
survives.

It also closes fix (1) as a direction. Steps 105, 106 and 108 each removed one
instance of the pattern and each found another behind it, because the pattern is
not in the adapter's waits but in the **bridge**: `w2c_call3` does `entry a1, 32`
and then `callx0`, and every call0 callee it reaches moves that frame's `a1`.
Nothing arranged around the bridge fixes what the bridge itself does.

### The change is kept

Re-deriving rather than trusting a register across an unpinned call is defensively
correct on its own terms, and with it in place the fault that shows is the real
one rather than a symptom eight instructions downstream. That makes the next
measurement easier to read, not harder.

### Next: fix (2)

Two forms, from step 104:

1. **`w2c_call3` allocates its own window for the callee** — so the callee's `a1`
   adjustments happen inside a frame the bridge owns, not the one the chain
   depends on.
2. **Compile `kernel/wifi_osi_impl.c` `-mabi=windowed`** — then `call8` rotates,
   the callee gets a real frame, and no call0 code touches the windowed `a1`.
   The boundary moves down onto `mutex_lock`, `task_sleep` and `heap_alloc`,
   which need the same treatment — but those are ours and mostly non-blocking.

**Nothing has been on air.**

---

## Step 110 — fix (2) implemented on the queue path; still faults

`w2c_call3` cannot "allocate its own window" for a call0 callee — rotation
requires the callee to have an `entry`. So fix (2) had to be the windowed-compile
form, and the check that made it tractable: `crit_enter`/`crit_exit` are
`static inline`, `copy_n` and `wake_all` are file-statics, and with `ticks = 0`
the poll never reaches `wait_on`. A windowed poll therefore needs **zero call0
calls**.

`vendor/windowed/wifi_osi_queue.c` — compiled `-mabi=windowed`, reached by a real
CALL8 so the window rotates and the callee gets its own frame:

- dequeue and copy under a hand-rolled `rsil`/`wsr.ps` rather than the inline
  `crit_enter`, because "it would very likely inline cleanly" is not a property
  this investigation has been well served by;
- `_Static_assert(sizeof(osi_queue_t) == 36)` so a layout drift from
  `kernel/wifi_osi_impl.c` is a build error rather than a silent one;
- **no** `task_wake()` — that is call0, and calling it from here would reinstate
  the boundary the file exists to remove. The poll reports a wake is owed and the
  caller does it through a bridge, pinned.

Whole-file compilation was rejected deliberately: it would turn every kernel call
in `wifi_osi_impl.c` (`task_sleep`, `heap_alloc`, `task_wake`) into a CALL8 to a
call0 function, which is the bit-31 fault hit four times in this project.

### Result

```
boot 11 PASS 0 FAIL   wintorture CORRECT   blobphy rc=0
wifiinit  PANIC at 1.1 s -- StoreProhibited, excvaddr 0x1c0
          a0-16 0x3ffd8f78  a1-12 0x3ffbdaa8  a2-8 0x0000002c
```

The bridge is gone from the poll path and the fault persists, in the same family:
`a0` still `&adc_ana_conf_org`, `a2` now a small integer.

### wincollide's panic is mine, not a regression

`wincollide` panics — **inside the step-107 unpin test**, which is a deliberate
fault injector that creates the frame-loss condition on purpose. It reaches and
passes the spill test first (`7 frames -> 1, as designed`), then enters the unpin
test and faults there.

That is the reproducer working. It is also a mistake: putting a fault injector
inside the regression command destroyed `wincollide` as a pass/fail signal, and
for one step I read its panic as a possible regression from fix (2). The unpin
test needs its own command before the suite means anything again.

### Where that leaves fix (2)

Implemented and correct on the poll path, and not sufficient. The remaining call0
excursions on the blocking path are `blob_unlock_only`, `blob_trylock` and the
wake — all reached through `w2c_call0f`/`w2c_call1`, which have the identical
`entry`/`callx0` shape as `w2c_call3` did.

Fix (2) applied to *one* callee removes one bridge. The bridges themselves are
the pattern, and there are four of them.

**Nothing has been on air.**

---

## Step 111 — all four bridges removed from the blocking path

`vendor/windowed/blob_lock_w.c`, compiled windowed: `blob_trylock_w()` and
`blob_unlock_w()` operate on `g_blob_mutex` directly — a compare and a store
under a masked interrupt, nothing else — so the stub takes and releases the lock
without a `callx0` anywhere.

Two things it deliberately does not do, both for the same reason:

- **No waking.** `task_unblock()` is call0 and reaches the scheduler.
  `blob_unlock_w()` returns the waiter mask and the caller does the wake through
  a bridge, pinned, where no switch can land inside.
- **No ownership hand-off.** `mutex_unlock()` transfers the lock to the
  longest-waiting task so a spinner cannot steal it, and that needs
  `task_unblock()`. Here the lock is released and a waiter takes it on its next
  try. The cost is fairness, not correctness, and the blocking path retries in
  windowed frames anyway.

`_Static_assert(sizeof(mutex_w_t) == 28)` guards the layout, because a silent
wrong-offset write is the failure mode that cost this project a session on
`wifi_init_config_t`.

### Result

```
boot 11 PASS 0 FAIL   wintorture CORRECT   blobphy rc=0
wifiinit  PANIC at 1.1 s
  a0-16 0xeeeeeeee   a1-12 0xeeeeeeee   a2-8 0x00000000   a3-4 0x4008b8c6
  +0 0x3ffd8f78      +4 0x3ffbdaa8
```

The blocking path now contains **no call0 bridge at all**, and the fault
persists — but the signature has changed in a way that matters. The save area is
`STACK_FILL` in both the `a0` and `a1` slots: not call0 locals, not a stale
pointer, but **memory nothing ever wrote**.

Every previous variant of this fault read *something* out of that slot — `spent`,
`0xff`, a blob global. Reading fill means the frame the handler is unwinding to
was never spilled there at all, which is a different failure from every earlier
one and is not explained by anything in steps 103-110.

### What is established

Fix (2) is now applied to the whole blocking path — the queue poll, the lock, the
unlock — and the bridges it was meant to eliminate are gone. **That was not
sufficient**, and the diagnosis in step 104 therefore does not account for the
whole fault, however well it accounted for the `spent` identity in step 103.

Two readings remain open, and they are distinguishable:

1. The blob's own frames have the same problem, and removing our bridges only
   removed our contribution. `ppTask` is windowed throughout (step 100), but what
   it calls may not be.
2. The frame is genuinely never spilled — a gap in coverage rather than a
   corrupted value — which would point back at when `win_spill_all` runs relative
   to frames created after it.

Reading (2) is testable with the instruments already present: the spill probe
walks the chain and reports each frame's `a0`. Pointing it at the blocking path
rather than the synthetic one would say whether the frame in question is on the
chain the spill actually covers.

**Nothing has been on air.**

---

## Step 112 — the faulting frame is not on the chain the spill covers

Step 111 left two readings open. This settles them: the spill probe from step 98,
pointed at the blocking path instead of the synthetic chain, taken immediately
after `win_spill_all()`.

```
qspill  : from 0x3ffb2850  walked 4 frames, 1 bad  first a0 0x00000000 at 0x3ffb28e0
uf frame: @0x3ffb2830
```

`win_spill_all()` walks **upward** from the caller's stack pointer into its
callers — four frames, all of them above `0x3ffb2850`. The frame the underflow
later dies on is at `0x3ffb2830`, thirty-two bytes **below** that.

**It was never in the spill's range.** Reading (2) is correct, and this is why
step 111 found `STACK_FILL` in the save area: nothing wrote it because nothing
ever spilled that frame.

### Why it is deeper

The spill happens, and *then* the wait is entered. `osi_windowed_idle()` is
called after `win_spill_all()` returns, so its frames are created below the
point the spill reached. The task is unpinned across exactly that call — by
design, so other tasks can run — and those fresh frames are what a preemption
finds.

So the sequence is:

```
win_spill_all()          spills everything ABOVE this point   <- covered
unpin
osi_windowed_idle()      creates frames BELOW this point      <- NOT covered
   ... preempted here, frames live in registers, unspilled
```

Every version of the blocking path since step 24 has had this shape — spill,
then release, then wait — and the waiting has always happened in frames the
spill could not have covered, because they did not exist when it ran.

### What this means for the fixes so far

The four bridge removals (steps 110-111) were aimed at the wrong thing. They were
correct on their own terms — a call0 callee moving a windowed `a1` is a real
defect and step 103 proved it precisely — but the fault that remains is not that
one. It is a frame created after the spill, preempted before any spill can cover
it.

Which also explains why each fix changed the fault's shape without removing it:
they altered which frames exist below the spill point, and therefore what the
handler read, but not the fact that frames exist there at all.

### The shape of the actual fix

Nothing may be created below the spill point while unpinned. Either:

1. **Spill again inside the wait**, at the deepest point, before unpinning — so
   the frames that will be preempted are already in memory; or
2. **Do not create frames during the wait** — an unpinned wait that allocates no
   windowed frames at all, which means a leaf, not a nested call chain; or
3. **Spill on preemption** — step 108's conclusion, which covers this case and
   every other one, and is the only version that does not depend on getting the
   ordering right by hand.

(3) remains the answer. (2) is the cheap test of this step's finding: make
`osi_windowed_idle` a leaf and see whether the fault moves.

## Step 113 — the leaf test. The fault is gone.

Step 112's option (2), run as written: remove every windowed frame created
during the unpinned wait.

The change is smaller than "make `osi_windowed_idle` a leaf". A leaf still
executes `entry` and still allocates one frame below the spill point. So the
wait was moved into the stub's *own* frame -- an inline `ccount` spin, no call
of any kind:

```c
*pinp = -1;                     /* UNPIN */
{
    uint32_t t0, now;
    __asm__ volatile ("rsr.ccount %0" : "=r"(t0));
    for (;;) {
        __asm__ volatile ("rsr.ccount %0" : "=r"(now));
        if ((now - t0) > 120000u) { break; }
    }
}
```

That leaves exactly one live frame across the preemption -- the frame
`win_spill_all()` deliberately leaves live, and the one the restore is known to
handle correctly.

**Result: `wifiinit` no longer faults.** It was panicking at ~1.1 s in every run
since step 86. It now runs indefinitely -- 200 s observed -- with no exception,
no double exception, and the rest of the OS scheduling normally throughout.

Step 112's reading is confirmed. The mechanism was windowed frames created
*below* the spill point while unpinned: `win_spill_all()` walks upward into
callers, so anything entered after it returns is outside the range it covered,
and a preemption there leaves the restore walking save areas nobody wrote.

### What the run says now

```
[qr] budget spent, still waiting  calls=5 timeouts=4 lastosi=29 osin=21
```

Twenty-one OSI calls, then `_queue_recv` (entry 29) called repeatedly, each one
timing out at 400 rounds and reporting empty. Nothing ever posts to that queue,
because `_set_intr` clamps and counts rather than wiring an interrupt and the
timer entries are stubs. This is UM-NATOS-042 §9.5's wall, arrived at rather
than argued about.

Suite: boot 11 PASS 0 FAIL, `wintorture` CORRECT, `blobphy` rc=0, `blobtx force`
0x3004. `wincollide` still panics inside its own step-107 fault injector, which
remains contamination to be removed.

### Two instrument mistakes, both recorded rather than quietly fixed

**The first budget attempt caused a watchdog reset.** Bounding the *total*
blocking rounds and returning empty immediately once spent turned the blob's
retry into a tight loop that never yielded, and TG0WDT reset the chip at ~10 s.
The reset was mine, not the blob's; the un-budgeted run before it had spun for
150 s with the OS fully alive. Replaced with a one-shot bridged report that
leaves the blob's pacing alone.

**The 150-second run was misread first.** It was recorded as "no fault, no
return" because the capture was grepped for panic lines only. Reading the whole
log in order showed `rst:0x7 (TG0WDT_SYS_RESET)` partway through, with
everything after it a fresh boot. Same failure mode as the 50-second capture in
UM-NATOS-042 §8: a filter that could not report the thing it was trusted to
rule out.

### What this is and is not

It is a **confirmed diagnosis**, not a finished fix. The spin is a busy-wait: it
burns 600 ms of CPU per `_queue_recv` and only works because the task is
unpinned, so other tasks still run. It cannot survive a real wait -- one that
must sleep until an interrupt arrives.

The proper fix is unchanged and is step 108's: **spill on preemption**. That
covers this case and every other one, and does not require hand-ordering the
spill against frame creation. What step 113 buys is certainty about what that
fix has to accomplish, and a system that runs long enough to work on the next
problem.

## Step 114 — the forever-cap meant three different things

Noticed from the outside: "600 ms sounds like a hardcoded value." It was not
hardcoded, and chasing where it came from turned up two further errors behind
the first.

**One: the unit was dropped.** `OSI_FOREVER_CAP` is `400u` in
kernel/wifi_osi_impl.c, commented "~4 s at the current tick", and correct there
-- `spent` counts ticks, which is why it is compared against `ticks` two lines
earlier. Step 111 rebuilt the blocking path in windowed code and carried the
digits without the unit: `rounds` counts 120000-cycle spins, so `rounds >= 400u`
meant 600 ms where the constant it was copied from meant 4 s. The comment I put
on that line said "the forever-cap, unchanged in meaning". It was 6.7x shorter.

It was also a *second* copy: after step 111 the blocking path no longer reads
`OSI_FOREVER_CAP`, so changing that constant no longer changes the blocking
path -- the exact knob step 103 turned to prove `excvaddr` was `spent`
(UM-NATOS-042 section 7). That experiment would now read as a null result for a
reason unrelated to its hypothesis.

**Two: the windowed directory had no include path.** `$wflags` in build.ps1
carried no `-I` at all, so a windowed file could only ever restate a kernel
constant, never share one. That is the mechanism by which the cap came to mean
two things. Fixed, with the constraint written down: macros only may cross, as a
call0 static inline pulled into windowed code is an ABI crossing with no bridge.

The first build after adding `#include "osi_wait.h"` therefore failed -- and was
reported clean, because the error filter matched `": error"` and GCC writes
`fatal error:` with no preceding colon. The board kept running the stale image
and reported `calls=5 timeouts=4`, identical to the previous build. What caught
it was arithmetic: with a 6.7x longer timeout the budget must be reached on call
3, not call 5. Disassembly confirmed `movi a3, 0x7d0` -- the old 2000 -- still in
the image. **Eleventh instrument in this investigation to report a result it was
not able to observe.**

**Three: a round is not a spin.** With the arithmetic fixed the nominal wait was
4 s, and the budget fired at t=152.5 s -- 19 ms per round against a 1.5 ms spin.
A round also runs `win_spill_all()`, and the task is unpinned across the wait, so
a scheduling round trip lands in the middle of every one. The nominal 4 s was
really ~51 s. Same class of error as the first, reached from the other side: a
count cannot express a duration unless the thing counted has a fixed cost.

### The fix

`kernel/osi_wait.h` -- macros only, shared by both ABIs. One number with a
meaning, `OSI_FOREVER_CAP_MS = 4000`; each side derives its own bound:

```
OSI_FOREVER_CAP         = 400 ticks          (impl side, unchanged)
OSI_FOREVER_CAP_CYCLES  = 320,000,000        (windowed side, elapsed cycles)
```

The windowed path now bounds itself by `(now - wait_t0) >= OSI_FOREVER_CAP_CYCLES`
read from `ccount`, which needs no assumption about what a round costs. `rounds`
survives as a diagnostic that is never a bound. Static asserts cover the u32
overflow, a period exceeding the whole wait, and the ccount wrap period; kmain.c
asserts its tick period still equals the one the header derives from.

Verified with the real preprocessor, not by inspection: `OSI_FOREVER_CAP == 400`
and, before it was retired, `OSI_FOREVER_ROUNDS == 2666`.

### Measured

```
[qr] budget spent, still waiting  calls=4 timeouts=3 lastosi=29 rounds/wait=204 osin=20
report at t=12.7 s          three 4 s waits -- predicted ~12
rounds/wait=204             4000 ms / 204 = 19.6 ms per round
```

Two independent numbers agree, and the second is the one that exposed the third
error. Suite: boot 11 PASS 0 FAIL, `wintorture` CORRECT, `blobphy` rc=0,
`blobtx force` 0x3004, `wifiinit` no fault and no reset over 120 s.

### Left alone deliberately

`osi_impl_queue_recv`'s own `spent >= OSI_FOREVER_CAP` still assumes one tick per
iteration, because `wait_on()` blocks until woken. Nothing on the blocking path
reaches it now, so the assumption is untested rather than wrong. It should get
the same treatment when interrupts are wired and that path is live again.

## Step 115 — spill on preemption: attempted, reverted, placement diagnosed

The sweep went into `task_schedule()`: if the outgoing task's saved WINDOWSTART
has more than one bit, call `win_spill_call0()` and rewrite the saved word to
`1 << base`. Then every task leaves with one frame, no task's frames occupy a
slot another task needs, and the restore can grant a single bit instead of
guessing at `g_win_union`. The same build removed the union from the grant.

The preconditions the site needs are genuinely there, and worth recording since
they were the reason to try C rather than assembly: `_handler_level3` already
clears `PS.EXCM` at entry, so window exceptions are taken normally rather than
becoming double faults; the H1 defer branch means the scheduler is never entered
mid-window-handler; and the frame is built on the interrupted task's own stack.

**It broke `wintorture`** -- LoadProhibited, `epc 0x40080155`, inside the window
vectors. Two changes, one symptom, so the union went back on its own:
`wintorture` still panicked. **The sweep is the cause; the grant simplification
is untested rather than wrong.**

### Why the placement cannot work

`task_schedule()` runs after the handler has done `addi a1, a1, -112` and built
the switch frame. At spill time `a1` is the frame, not the task's stack pointer,
and the frame occupies the 112 bytes directly below the task's sp.

Spilling with the task's real sp instead is not the fix either: `win_spill_call0`
writes its base save area at `sp-32-12`, which lands inside that frame.

So the sweep needs a point where the task's sp is intact AND nothing has been
written below it -- the handler prologue, before the frame is built. That in turn
needs the spill's clobbers (a2..a11) handled, since the registers it destroys are
the ones not yet saved. An assembly change to the interrupt prologue, and it
wants its own step with `wintorture` as the test and the switch count as the
control.

### One measurement worth keeping

With the sweep in, `wifiinit` reported `sweeps=0`. The blob task is pinned
whenever it holds more than one frame, and unpinned only in the step-113 leaf
spin where it holds exactly one -- so on the path this was built for, the sweep
had nothing to do. `wintorture` is a different workload and did trigger it.

That is not an argument against the fix. It says step 113's spin is currently
carrying the invariant that spill-on-preemption is meant to carry properly, and
that `wintorture` -- not `wifiinit` -- is the test that will judge it.

Suite after the revert: boot 11 PASS 0 FAIL, `wintorture` CORRECT, `blobphy`
rc=0, `blobtx force` 0x3004, `wifiinit` no fault and no reset.

## Step 116 — the sweep cannot fire while the pin exists

Step 115's placement diagnosis was acted on: the sweep moved into
`_handler_level3` on the `.Lsched` path, before `task_schedule`, borrowing a
dedicated 1 KB `.bss` stack rather than using the switch frame.

The reasoning for the borrowed stack still looks right, and is worth keeping
because it is the part that generalises: **each frame overflows through the `a1`
held in its own window**, so every parent writes to the task's stack no matter
where `a1` points. Only the CURRENT window's save area follows `a1`, and that is
the handler's own context, which is discarded. The one thing that must survive is
the innermost windowed frame's base save area at `[task_sp-16 .. task_sp-4]` --
frame offsets 96..108, inside the padding, since the frame stores nothing above
88. Verified against TASK_FRAME_WORDS 23 padded to TASK_FRAME_BYTES 112.

**`wintorture` panicked again, identically: LoadProhibited, `epc 0x40080155`.**
And this time the diagnostics say why that is not a verdict on the sweep:

```
switch-out: n 6151 wb 3 ws 0x00000008
multiframe: 0 switch-outs with >1 live frame
```

**Zero.** Every switch-out already carries exactly one live frame, so
`ws & (ws - 1)` was never true and the sweep body never executed. It broke
`wintorture` without running -- which points at the 1 KB added to `.bss` and the
code shift ahead of it, the step-7/25 layout band, not at the logic.

### What this actually establishes

The pin is doing the sweep's job. A task never reaches a switch-out holding more
than one windowed frame, because `task_schedule()` refuses to switch away from
one that does. So **spill-on-preemption cannot be tested while the pin exists**:
it is dead code by construction, and `wintorture` passing or failing says
nothing about it either way.

That reframes the order of work. The sweep is not a change to make and then
verify; it is the thing that has to be in place before the pin can come out, and
the only test that can judge it is `wintorture` **with the pin disabled** --
which is precisely the configuration UM-NATOS-038 section 12.3 measured as
panicking, and the reason the pin was introduced.

So the next attempt is three things in one step, not one:

1. land the sweep without disturbing the layout -- put the borrowed stack where
   an existing reserved stack already lives rather than appending to `.bss`, and
   confirm `wintorture` is unchanged while the sweep is still dead code;
2. disable the pin;
3. run `wintorture` with the switch count as the control and `multiframe` as the
   proof the sweep is now reached at all.

Step 115 said `wintorture` rather than `wifiinit` would judge this. That was
half right: `wintorture` with the pin ON judges nothing.

Reverted. Suite: boot 11 PASS 0 FAIL, `wintorture` CORRECT.

## Step 117 — step 116 was wrong: the sweep runs, and it erased its own evidence

The sweep moved to the PHY stack's unused low 1 KB, adding **nothing** to `.bss`.
`wintorture` panicked identically -- `LoadProhibited`, `epc 0x40080155`. So
step 116's layout account is **disproved**: zero bytes were added and the failure
did not move.

That left the other half of step 116 to check, and it does not survive either.

### The correction

Step 116 concluded the sweep body never executed, from
`multiframe: 0 switch-outs with >1 live frame`. But `multiframe` is computed in
`task.c` from the saved WINDOWSTART at frame offset 88 -- **the word the sweep
rewrites to a single bit before `task_schedule` ever reads it.** The sweep was
erasing the only evidence it had run, and the zero was read as its absence.

Suppressing just that write, for one run:

```
multiframe: 1 switch-outs with >1 live frame, worst 7 frames, last task 5 ws 0x0000aa8a
```

**The sweep runs.** It fires on a real preemption of task 5 holding seven live
windowed frames. Step 116's headline finding -- "the sweep cannot fire while the
pin exists" -- is withdrawn.

This is the twelfth instrument in this investigation to report something it could
not observe, and the first built two steps running: the measurement was
downstream of the change it was measuring. The catalogue in UM-NATOS-041 section
7 and UM-NATOS-042 section 8 gains a new entry, and it is the worst kind, because
it produced a confident negative that redirected a whole step.

### What is actually true now

`epc 0x40080155` is `_WindowUnderflow12 + 0x15` -- confirmed by symbol
(`_WindowUnderflow12` at `0x40080140`), not by arithmetic on a remembered vector
layout. The `wifiinit` fault that steps 86-113 chased was `_WindowUnderflow8 +
0x15`. Same offset into a sibling handler.

Two facts worth carrying:

1. **The pin is not airtight.** With the pin ON, one switch-out in ~6100 still
   carried seven live frames. The pin makes the case rare, not impossible, which
   means the register-file partitioning has always had a hole in it and
   `g_win_union` has been covering that hole rather than the pin preventing it.
2. **The sweep, when it fires, breaks what HEAD survives.** On HEAD that same
   preemption happens and `wintorture` passes, because the restore grants the
   bits back out of `g_win_union` and the frames are still sitting in the
   register file. The sweep moves them to memory and something about the
   restore then reads the wrong thing.

So the sweep is not dead code and never was; it is a live change with a real
defect, on a path that is genuinely exercised. That is a better position than
step 116 described, and a worse one than step 115 assumed.

### Next

The single event is now identifiable -- task 5, `ws 0x0000aa8a`, seven frames --
so the next step should dump the save areas the sweep writes for exactly that
event and compare them against what `_WindowUnderflow12` then reads, rather than
inferring from a pass/fail. That is the pattern UM-NATOS-042 section 8 records as
having worked every time it was used: name the thing by address and read a range.

Reverted. Suite: boot 11 PASS 0 FAIL, `wintorture` CORRECT, `blobphy` rc=0,
`wifiinit` no fault and no reset. `blobtx force` not re-verified this run -- the
serial link dropped twice mid-suite and the reading was not retaken.

## Step 118 — the sweep works; the spilled frames are not where the restore looks

Steps 115-117 judged the sweep by whether `wintorture` passed. It does not pass,
and pass/fail cannot name a bad save area. So the sweep now audits its own
output -- walk the chain from the task's stack pointer upward, check each
frame's `a0` is a windowed return encoding -- and the result prints in the panic
dump. Same walk step 112 used on the blocking path.

```
pspill   : sweeps=1 pre_ws=0x0000aa8a post_ws=0x00000008
           from 0x3ffb9220 wb=3 walked 1 bad 0
exccause 28 (LoadProhibited)  epc 0x40080155  excvaddr 0x00e40d90
```

### The sweep is not the broken part

`pre_ws 0x0000aa8a` is bits 1, 3, 7, 9, 11, 13, 15 -- seven live frames, matching
step 117's `worst 7 frames`. `post_ws 0x00000008` is bit 3 alone, and `wb` is 3.

**The sweep reduced seven frames to exactly one, and that one the task's own
base.** That is precisely what it was written to do. Three steps of suspicion
pointed at the sweep's logic and the logic is correct.

### Where it actually fails

`walked 1`. The audit walked ONE frame from `0x3ffb9220` and stopped, because the
parent link at `[sp-12]` did not ascend. Seven frames were spilled and the chain
from the task's stack pointer reaches none of them.

That is enough to explain the fault without further inference. On restore the
task gets one window back and `a1 = task_sp`; the first `retw` underflows and
reads `[task_sp-12]` for its parent, finds something that is not a stack pointer,
and loads through it -- `excvaddr 0x00e40d90`. On HEAD the same preemption is
survived because the parents are still sitting in the register file and
`g_win_union` grants their bits back, so no underflow ever happens.

### The assumption to test next

The borrowed-stack design rests on one claim, stated in the step-117 comment:
*each frame overflows through the `a1` held in its own window, so every parent
still writes to the task's stack wherever `a1` points.* If that were true the
chain would be intact. It is not intact, so the claim is the thing to test.

The concrete next experiment: dump the borrowed stack (`_phy_stack` + 0..1024)
after a sweep and see whether the seven frames landed THERE. If they did, the
premise is refuted -- the spill follows the sweeping context's `a1`, not each
frame's own -- and the borrowed stack is exactly the wrong idea, because it sends
every parent somewhere the task can never find. The sweep would then have to run
on the task's own stack, below the switch frame rather than beside it.

Note also that the audit's starting point deserves its own check: the innermost
windowed frame is the one the interrupt handler is running in, and the handler
moved `a1` before anything was spilled. Whether `[task_sp-12]` was ever a valid
base save area, or was only ever the frame padding, is a second thing the dump
should settle rather than assume.

Reverted. Suite: boot 11 PASS 0 FAIL, `wintorture` CORRECT, `blobtx force`
0x3004, `wifiinit` no fault and no reset. `blobphy` not re-verified -- the serial
link dropped on that command and the reading was not retaken.

The audit instrumentation stays in `task.c` and `panic.c`; with no sweep present
it prints "no sweep audited" and costs nothing. Step 117 disproved the layout
concern that would have argued for removing it.

## Step 119 — the borrowed stack is refuted, by my own premise's test

Step 118 named the assumption the whole borrowed-stack design rests on and said
to test it. Tested:

```
pspill : sweeps=1 pre_ws=0x0000aa8a post_ws=0x00000008
         from 0x3ffb9230 wb=3 walked 1 bad 0
         [task_sp-16]=0xeeeeeeee [task_sp-12]=0xeeeeeeee  borrowed: enc=3 sp_like=5
```

`0xeeeeeeee` is the stack fill pattern. **Both words of the innermost frame's
base save area are untouched poison -- nothing was written to the task's stack
at all** -- while the borrowed stack came back holding three windowed return
encodings and five words that look like task stack pointers.

The premise from step 117 was:

> each frame overflows through the `a1` held in ITS OWN window, so every parent
> still writes to the task's stack wherever `a1` points

**That is false.** The spill followed the sweeping context's `a1`. The borrowed
stack is not a neutral scratch area, it is where all seven frames went -- which
makes it the worst possible choice, because it sends every parent to an address
the task can never look at. The design was wrong in exactly the way step 118
guessed it might be, and it took a direct measurement rather than more reasoning
to establish it.

### The second thing the dump settles

`[task_sp-16]` being poison is not only about the sweep. It says the innermost
windowed frame's base save area was **never written by anything**, sweep or no
sweep. If `task_sp` were a genuine windowed frame boundary, `entry` would have
written it when the frame was created. So either `current_sp + TASK_FRAME_BYTES`
is not the interrupted `a1` after all, or the interrupted context was inside a
call0 stretch where no windowed frame starts at that address.

That matters beyond this fix: step 118's audit walk, and its `walked 1`, both
start from that address. Until it is established, the walk is measuring from a
place that may not be a frame boundary.

### Where this leaves spill-on-preemption

Not refuted -- the sweep still does what it claims, reducing seven live frames to
one at the task's own base, and step 118 confirmed that separately. What is
refuted is running it on a stack the task does not own.

The next attempt must spill with `a1` on the task's own stack, below the switch
frame rather than beside it, so the save areas land where the restore will look.
Step 115 rejected that on the grounds that `win_spill_call0` writes at `sp-32-12`
and would land inside the frame -- but that objection assumed spilling at the
task's sp. Spilling below the frame (`current_sp` minus a margin) puts the
scratch clear of it and still on the right stack, and the frames' own addresses
are what the save areas encode.

One more thing to settle first, cheaply: whether `pre_ws`'s seven bits are all
one task's frames. WINDOWSTART is global hardware state, and the whole
partitioning design means several tasks' frames coexist in the register file at
once. If those seven bits span tasks, then a sweep spills OTHER tasks' frames
through their own stack pointers, and "one task, one sweep" is the wrong model
for what this operation even does.

Reverted. Suite: boot 11 PASS 0 FAIL, `wintorture` CORRECT, `blobphy` rc=0,
`wifiinit` no fault and no reset.

## Step 120 — wintorture has been proving nothing, and says so in its own output

Looking for a reporting surface on a passing run, rather than a panicking one,
turned up this as the whole of `wintorture`'s output:

```
spun 60 ms with 8 windowed frames live, interrupts ENABLED
switches during the call: 0  -- NONE, so this proves nothing
checksum 1632 expected 1632  CORRECT
```

**Zero switches during the call.** The command's own control says the run is
meaningless, in a line written when the test was built, and every check in steps
115-119 grepped for `CORRECT` and discarded it.

### What this invalidates

`wintorture` was the judge in four consecutive steps. It cannot be:

- **"wintorture CORRECT" on HEAD does not establish that windowed frames survive
  preemption.** No preemption occurs during the windowed section, so the
  checksum only proves the frames survive *not* being switched away from.
- **"wintorture PANIC" with the sweep in was a real regression, but not about
  wintorture's frames.** No switch happens inside its windowed call, so the
  sweep's single firing -- task 5, seven frames -- came from elsewhere in the
  system. The sweep broke something real; the evidence never said what.
- Step 116's plan ("wintorture with the switch count as the control") was right
  in form and I then failed to read the control it named.

The reason is the pin: `rom_call3` takes the blob lock, which pins, so the tick
declines to switch for the whole call. `wintorture` and the pin were introduced
to answer different questions and have been silently cancelling each other out.

### The instrument catalogue gains its worst entry

UM-NATOS-041 section 7 and UM-NATOS-042 section 8 record ten and then twelve
instruments that reported what they could not observe. This one is different in
kind and worse: **the instrument reported its own invalidity correctly, in
plain language, and the filter reading it threw that line away.** Nothing was
wrong with the measurement. Four steps of conclusions rested on a `grep` for the
word CORRECT.

The rule this earns: when a test prints a control, the control is part of the
result. A pass/fail extracted without it is not a reading.

### What to do next

`BLOB_PIN_DISABLE` already exists in `kernel/blobcall.c` and turns the pin off.
That is the missing test bench, and the order is now:

1. `BLOB_PIN_DISABLE 1`, no sweep. Confirm `switches during the call` is
   non-zero and that `wintorture` fails -- reproducing UM-NATOS-038 section
   12.3's measurement, which is the baseline every later claim needs.
2. Add the sweep, on the task's own stack below the switch frame (step 119).
3. Judge it on the checksum WITH the switch count beside it, both quoted.

Nothing was built or changed in this step. Suite unchanged: boot 11 PASS 0 FAIL,
`wintorture` CORRECT-but-meaningless, `blobphy` rc=0, `wifiinit` no fault and no
reset.

## Step 121 — the baseline, at last: what the pin has been hiding

`BLOB_PIN_DISABLE 1`, no sweep. This is the run every claim since step 115
needed and none of them had.

```
*** KERNEL PANIC ***
  exccause : 0  (IllegalInstruction)   epc 0x6eeeeeee   ps 0x00070130
  windowbase: 14   windowstart: 0x00004000
  frames    : task 5 held 0x0000aa8a granted 0x00000008 LOST 0x0000aa82
  a0/sp out : 0x00000000 / 0x00000000   BOTH ZERO -- context clobbered
```

`wintorture` dies immediately, reproducing UM-NATOS-038 section 12.3. And unlike
every run in steps 115-120, the failure states its own mechanism:

**Task 5 held seven windowed frames, was granted one, and lost six.** `0xaa8a`
in, `0x8` out, `0xaa82` gone. `epc 0x6eeeeeee` is stack poison executed as an
instruction address -- a `retw` returning through a save area nobody wrote,
because the frames it wanted were in registers the next task reused.

That is spill-on-preemption's entire reason for existing, finally on screen.

### Two questions answered for free

**`granted 0x00000008` -- the union is empty when it matters.** The restore
computes `(1 << base) | g_win_union` and the grant came out as the base bit
alone, so `g_win_union` was zero at the moment six frames needed covering. The
partitioning design does not merely have a hole (step 117); with the pin off it
contributes nothing at all.

**Step 119's open question is settled: the seven bits are one task's.**
`0x0000aa8a` here is attributed by name -- `task 5 held` -- and is bit-for-bit
the `pre_ws` the sweep saw in steps 118 and 119. So "one task, one sweep" is the
right model, and step 119's worry that a sweep might be spilling several tasks'
frames through several stacks does not apply.

### What the bench is now

With the pin off, this is a reproducible, immediate, self-describing failure with
a named quantity to drive to zero: **LOST**. A correct sweep makes
`held == granted` and `LOST 0x00000000`, and `wintorture`'s checksum becomes
meaningful because switches genuinely occur during the windowed call.

Every previous step judged the sweep by a checksum that could not move. This one
can.

### Next

Step 119's conclusion, now testable: spill with `a1` on the task's OWN stack,
below the switch frame rather than beside it, so the save areas land where the
restore looks. Judge on `frames: ... LOST`, with the switch count quoted -- not
on the word CORRECT.

Reverted to `BLOB_PIN_DISABLE 0`. Suite green with the pin on: boot 11 PASS 0
FAIL, `blobphy` rc=0, `wifiinit` no fault and no reset, `wintorture` CORRECT and
still meaningless.

## Step 122 — the sweep on the task's own stack: LOST goes to zero

`a1` set just below the switch frame -- still the task's own stack, clear of the
112 bytes the prologue wrote. Run on step 121's bench, pin off:

```
frames    : no task was ever granted less than it held
pspill    : sweeps=1 pre_ws=0x0000aa8a post_ws=0x00000008
            [task_sp-16]=0xeeeeeeee [task_sp-12]=0xeeeeeeee
            borrowed: enc=0 sp_like=0
```

**`LOST` is zero.** The baseline's `task 5 held 0x0000aa8a granted 0x00000008
LOST 0x0000aa82` is gone: every task is now granted everything it held. The
defect step 121 named is fixed, and it was fixed by putting the spill on the
stack the restore actually reads.

### Step 119's conclusion is withdrawn

Step 119 read `borrowed: enc=3 sp_like=5` as "all seven frames went to the PHY
stack" and declared the premise refuted. That was wrong twice over:

- `post_ws` keeps the innermost window's bit, so only the six OLDER frames ever
  overflow. Parents live at HIGHER addresses, so `[task_sp-16]` was always going
  to be untouched poison -- it is not evidence of anything.
- `0xeeeeeeee >> 30` is non-zero, so the audit counted stack poison as a valid
  return encoding (`bad 0`) and then chased it out of range. That is why
  `walked 1`, and why the walk never said what it appeared to say.

This run settles it: with the spill on the task's stack, `borrowed: enc=0
sp_like=0`. The PHY stack is untouched. So the three encodings step 119 counted
were `win_spill_all`'s OWN nested frames, not the task's -- and the premise it
claimed to refute was never tested.

### What is still broken

`wintorture` still panics on the bench, `epc 0x6eeeeeee` -- poison executed as an
address. Frames are no longer lost, so this is a second and downstream problem:
something returns through a save area that was never written, even though the
accounting says nothing went missing.

`[task_sp-16]` being poison points at where to look. If `task_sp` were a real
windowed frame boundary, `entry` would have written it. It did not, so the
interrupted context was in a call0 stretch whose `a1` had moved below the last
windowed frame -- and the restore hands the task back an `a1` of `task_sp`
regardless. That is the next thing to measure, by finding the innermost windowed
frame's true `a1` rather than assuming it is `current_sp + TASK_FRAME_BYTES`.

### Not kept

With the pin back ON, the sweep panics `wintorture` (LoadProhibited) and also
`wifiinit`, which is clean on HEAD. So it is a regression in the shipping
configuration and does not stay in, despite `LOST` going to zero. Reverted.

Suite after revert: boot 11 PASS 0 FAIL, `wintorture` CORRECT (and still
meaningless with the pin on), `blobphy` rc=0, `wifiinit` no fault and no reset.

## Step 123 — the innermost frame's a1, measured: the assumption was right

Captured by `_handler_level3`'s first instruction, `wsr.excsave2 a1`, before the
prologue moves it. `wsr` writes a special register and clobbers no AR, so it is
free and needs no register saved first.

```
ih a1 : raw=0x3ffb9250 calc=0x3ffb9250  AGREE  ws=0x0000aa8a wb=3 bit(base)=1
```

**`current_sp + TASK_FRAME_BYTES` IS the interrupted a1.** The assumption steps
118, 119 and 122 rested on is sound, and `bit(base)=1` says a windowed frame
genuinely lives at the base. So `task_sp` is a real windowed frame boundary and
every walk that started there was starting in the right place.

### The lead it kills

Step 122 proposed this measurement because `[task_sp-16]` reads poison, and
argued that if `task_sp` were a real frame boundary `entry` would have written
it. **That argument was wrong.** `entry` allocates a frame and rotates the
window; it writes no memory. A frame's base save area is filled LAZILY, by the
overflow handler, when the frame is actually spilled. `post_ws` keeps bit 3, so
the innermost frame never overflowed, so its save area was never written.

Poison at `[task_sp-16]` is therefore the correct and expected state, and it has
now sent two steps down blind alleys -- 119 read it as "the frames went to the
borrowed stack", 122 read it as "task_sp is not a frame boundary". Neither
followed from it. The word means only "this frame has not been spilled".

### And one more instrument fixed mid-run

The first version of this measurement reported `DIFFER`, with
`raw=0x3ffb8dc0 calc=0x3ffb9240`. `g_ih_a1_raw` is rewritten by every interrupt
while `calc` is latched once, so it compared one event's `calc` against a later
tick's `raw`. What gave it away was not the mismatch but the value: `raw` was
below task 5's stack base, so it belonged to another task and could not be the
answer to any question being asked. Latching both in the same event gives AGREE.

Thirteenth entry in the catalogue, caught in one run this time, by checking
whether the number was even in the right address range before believing what it
implied.

### Where this leaves the sweep

Step 122 stands: on the task's own stack the sweep drives `LOST` to zero, and it
still panics `wintorture` on the bench with `epc 0x6eeeeeee` and regresses
`wifiinit` with the pin on. What is now excluded is that the walk or the spill
was aimed at the wrong address. The remaining fault is downstream of both.

The next thing to read is which frame's `retw` faults and what its save area
holds -- the frames are no longer lost, so the defect is in what was written to
them, not in whether they were written at all.

Instrumentation kept: one `wsr` in the interrupt prologue and a latch in
`task_schedule`, no behaviour change. Suite: boot 11 PASS 0 FAIL, `wintorture`
CORRECT, `blobphy` rc=0, `blobtx force` 0x3004, `wifiinit` no fault and no reset.

## Step 124 — the sweep is spilling a frame that does not exist

Sweep back on the bench, pin off, and the whole panic dump read rather than a
chosen field. Two lines answer it:

```
overflow  : prev good frame sp 0xeeeeeeee   this frame sp 0x3ffb9180  recovered base 0x3ffb9290
underflow : recovered a0 0x00000000 from save area 0xeeeeeeee
```

The underflow followed an `a1` link of **`0xeeeeeeee`** -- stack poison used as
an address -- and recovered `a0 = 0`. And during the sweep itself, an overflow
found the previous frame's `sp` already poison.

So the defect is not placement, not addressing, and not what the sweep wrote.
**One of the seven windows in `pre_ws 0x0000aa8a` is not a live frame at all.**
Its `a1` is stack fill. The sweep spills it because WINDOWSTART says a frame
lives there, the overflow handler writes through the poison it holds, and the
matching underflow later reads back a garbage chain -- `epc 0x6eeeeeee` is that
`a0` jumped to.

This is the same "ownerless phantom frame set" the X7 comment in vectors.S names
and that steps 53, 54 and 57 kept measuring from the other side. It was survivable
while the frames stayed in registers and `g_win_union` handed the bits back --
nothing ever walked them. A sweep walks them, which is why spill-on-preemption
surfaces a defect that predates it.

### What that means for the fix

`win_spill_all` cannot be pointed at WINDOWSTART and trusted, because
WINDOWSTART is not a list of live frames -- it is a list of windows with their
bit set, and at least one of those has never held a real frame. Either:

1. the phantom bit must be found and stopped at its source, which is the older
   and larger problem; or
2. the sweep must refuse to spill a window whose `a1` is not inside the owning
   task's stack -- a check the kernel already knows how to make, since
   `task_schedule` runs exactly that test on saved stack pointers ("bad sp:
   none -- every saved sp was inside its own stack").

(2) is testable immediately and does not require understanding (1). If the sweep
skips phantom windows and `wintorture` then passes on the bench with `LOST` still
zero, the phantom is isolated rather than merely avoided, and (1) becomes a
separate, bounded question.

### One instrument note

`ih a1 : raw=0 calc=0 AGREE ws=0 wb=0 bit(base)=0` -- step 123's latch never
fired this run. Its condition is `ws & (ws - 1)` read from the frame, and the
sweep rewrites that word to a single bit before `task_schedule` sees it. Exactly
the self-erasure of step 116, in a probe written two steps after learning it.
With the sweep present the latch must read `g_pspill_pre_ws` instead. Step 123's
AGREE stands -- it was measured with no sweep in.

Reverted. Suite: boot 11 PASS 0 FAIL, `wintorture` CORRECT, `blobphy` rc=0,
`wifiinit` no fault and no reset.

## Step 125 — the a1 range check: both placements blocked, and the way through

Step 124's fix (2) was "the sweep must refuse to spill a window whose `a1` is not
inside the owning task's stack". Costed before writing it, and it does not fit in
either of the two obvious places. **No code changed this step.**

### In the sweep: needs window rotation

The check needs each live window's `a1`, and those are in the physical register
file. Reaching window `b` means `wsr.windowbase b` -- which remaps `a0..a15`, so
**any scratch register used while rotated belongs to the frame being inspected**
and corrupts the very thing the check exists to protect.

Doing it safely means, per live window: rotate in, borrow one AR with
`xsr.excsave4` (which saves the frame's value rather than destroying it), stash
`a1` to a special register, rotate out, read it, then rotate back in to restore
the borrowed AR. Loop state cannot live in ARs at all -- it goes in memory or
special registers. That is roughly fifty lines of subtle assembly in the
interrupt prologue, and a bug in it would be **indistinguishable from the phantom
frame it is testing for**. Not something to start at the end of a session.

### In the overflow handler: no room

The natural alternative -- refuse the store when the frame pointer is not in
DRAM -- belongs in `_WindowOverflow8/12`, which is where the bad write actually
happens. There is no space. `_WindowOverflow8` currently holds three
`xsr`/`wsr` probe writes (steps 48, 74, 75), eight `s32e`, an `l32e` and `rfwo`:
about 45 bytes of a 64-byte vector slot. A range check is four more
instructions. An earlier attempt to add loads to `_WindowUnderflow8` already
failed to link for this exact reason.

### The way through, and it is already on the list

**Remove the probes from `_WindowOverflow8` to make room for the guard.**

Those three writes are instrumentation, and UM-NATOS-042 section 9.3 already
flags this class as debt -- "probes across window.S, task.c, panic.c,
wifi_osi_stubs.c and wifi_osi_impl.c, several built on premises since
disproved". The step-48 probe filters for "a near-null base", a hypothesis long
retired; steps 74 and 75 chased a bogus frame pointer, which step 124 has now
identified by other means. Retiring them buys about five instructions, which is
more than the guard needs.

That turns an unbounded assembly problem into a bounded one: delete known-dead
probes from one 64-byte slot, then add a four-instruction range check where the
faulting store already is. It also pays down debt the reports have been asking
for since step 102.

### Order for the next session

1. Delete the retired probes from `_WindowOverflow8`, build, confirm the suite is
   unchanged -- a pure removal, with nothing else moving.
2. Add the range check: if the frame pointer is outside DRAM, skip the stores.
   Count the skips somewhere already allocated.
3. Sweep back on the bench, pin off. `LOST` should stay zero and `wintorture`
   should now survive, because the phantom's stores never happen.
4. If it survives, the phantom is contained but not explained. Its source
   remains open and is the older question steps 53, 54 and 57 were circling.

Suite unchanged: boot 11 PASS 0 FAIL, `wintorture` CORRECT, `blobphy` rc=0,
`wifiinit` no fault and no reset.

## Step 126 — the guard is in, and it moves the fault rather than removing it

Both halves of step 125's plan executed.

**The probes are gone.** `_WindowOverflow8` and `_WindowOverflow12` each carried
five special-register writes from steps 48, 74 and 75, filtering for hypotheses
long retired. Removed, and `panic.c`'s "overflow" line now says so rather than
printing `excsave5` -- which the UNDERFLOW handlers still write, so leaving it
would have reported one handler's data under another's heading.

**The guard is in**, on OF4, OF8 and OF12:

```
40080080 <_WindowOverflow8>:
40080080:  bbsi  a9, 31, 400800a1     <- frame pointer not DRAM: skip the stores
40080083:  s32e  a0, a9, -16
...
40080092:  bbsi  a0, 31, 400800a1     <- recovered caller sp, same test
...
400800a1:  rfwo
```

One instruction per guard, because the skip target is the `rfwo` that was already
there. `bbsi ..., 31` is exact for this machine: DRAM is `0x3FFxxxxx` with bit 31
clear, and every fill value this kernel uses has it set. 36 bytes of a 64-byte
slot. Suite with the guard alone: **unchanged**.

### On the bench, it does not fix it

```
result   : PANIC   exccause 0 (IllegalInstruction)   epc 0x6eeeeeee
frames   : no task was ever granted less than it held
pspill   : sweeps=1 pre_ws=0x0000aa8a post_ws=0x00000008
```

And `0x6eeeeeee` now explains itself. `retw` computes its target as
`(PC & 0xC0000000) | (a0 & 0x3FFFFFFF)`. With `a0 = 0xEEEEEEEE` that is
`0x40000000 | 0x2EEEEEEE` = **`0x6EEEEEEE`** exactly. The faulting return is one
whose `a0` came back as stack fill.

So the guard stops the phantom being **written** through, which was real -- but
`rfwo` still clears its WINDOWSTART bit, so a later underflow tries to
**restore** a frame that was never saved, reads an unwritten save area, and
returns through the poison in it.

**Containment at the handler moved the failure from the write to the read.** The
same guard cannot be applied to the underflow: an overflow that skips has nothing
to save, but an underflow that skips has nothing to return to. There is no value
it could substitute.

### What that settles

A phantom window can be neither spilled nor restored. Any scheme that leaves a
bit set in WINDOWSTART for a window holding no real frame will fail the moment
something walks it -- and the sweep is exactly such a thing. **The phantom has to
be eliminated at its source; it cannot be contained downstream.** Step 125's
option (2) is therefore closed, and option (1) is the only one left.

That is the older question steps 53, 54 and 57 were circling and the X7 comment
in vectors.S names outright: which code path sets a WINDOWSTART bit for a window
that never held a frame. It is now the single blocker for spill-on-preemption,
and through that for unpinned windowed execution.

### Kept

The guard stays. It is defensively correct on its own terms -- refusing to store
through a value that cannot be a pointer is better than faulting on it -- it costs
one instruction in three handlers, and the suite is unchanged with it in. The
probes it replaced were debt UM-NATOS-042 section 9.3 had already flagged.

Suite: boot 11 PASS 0 FAIL, `wintorture` CORRECT, `blobphy` rc=0, `blobtx force`
0x3004, `wifiinit` no fault and no reset.

## Step 128 — Tier B step 1: ROTW solves the clobber problem, the identity does not hold yet

UM-NATOS-043 rev 1.2 §8.1 said the save and restore are only correct as a pair,
because the scratch registers a save needs belong to the window being saved and
`wsr.windowbase` makes the clobber unrepairable.

**`ROTW` dissolves that.** It adds an *immediate* to `WINDOWBASE`, so no general
register is involved in rotating at all. Each window can be left exactly as
found before moving on, and four `rotw 4` steps advance by 16 and wrap to the
start, so `WINDOWBASE` needs no saving either. Confirmed in the disassembly:
eight `rotw` present, four per pass.

The sequence asks `WINDOWSTART` nothing, which is the whole point of Tier B.

**The identity pair does not hold.** `wintorture` fails and `wifiinit` panics —
the latter having been clean since step 113. Reverted; suite green again.

### What is known, and what is only suspected

Known: `rotw` assembles and is present, and the block structure produces the
right offsets — a0 at `k*64`, a1..a15 at `k*64 + i*4`, four windows over 256
bytes.

Suspected, and **not verified**: the sequence opens with

```
    movi     a2, g_regsave
    wsr.excsave3 a2
```

which destroys the current window's `a2` **before** the first block saves it. For
the current window that should be harmless — the handler already wrote the task's
`a2` to the switch frame, and `.Lresume` reloads a0..a15 from there — so it
should be benign, and if it is, the fault is elsewhere. It is the first thing to
eliminate, not the answer.

Two other candidates, in order of how cheaply they can be excluded:

1. **`excsave3` may not be free.** The sequence assumes it, and the assumption
   was never checked against the underflow/overflow probes, `panic.c`, or
   `win_probe_seed`. Step 123 took `excsave2` on the same basis and that one was
   verified; this one was not.
2. **Rotating to a window whose `WINDOWSTART` bit is clear.** No window
   instruction executes while rotated, so it should be legal, but "should be" is
   a phrase this log has been punished for. The architecture requires the bit at
   `WINDOWBASE` to be set for a valid current frame, and four of the stops will
   not have it.

The discipline that applies: name it by address and read a range, rather than
reasoning about which line must be responsible. The next run should dump
`g_regsave` after one identity pass and compare it against the switch frame,
which turns "the pair is wrong" into "this register, at this offset, differs".

### Not a setback to the design

`ROTW` removing the clobber problem is a real advance — it is what makes a
register-file save implementable at all in an interrupt prologue, and §8.1's
objection is now retired rather than worked around. What failed is a first
draft of the sequence, on its first run, with the tree green afterwards.

Suite: boot 11 PASS 0 FAIL, `wintorture` CORRECT, `wifiinit` no fault and no
reset.

## Step 129 — the save loop is correct; it was running after two `call0`s

Two things made this readable where step 128 was not.

**`ROTW` makes the save separable.** Each block restores its window's `a0` and
`a1` before rotating, so a save-only pass is a genuine no-op. That splits the two
loops, which §8.1 said could not be split — and the split localises the failure
immediately: **save-only panics too, so the defect is in the save.**

**Then the diff named it, register by register:**

```
regsave : a0 got 0x4008acbc want 0x400838bd   a1 0x3ffb9d80 sp 0x3ffb9d80
```

`a1` **matches exactly** — `g_regsave[1]` equals `current_sp`. The rotation, the
offsets, the four-window mapping and the store sequence are all correct. `a0` is
wrong, and both values are IRAM code addresses.

### The cause

`a0` is the call0 return-address register, and the save pass sits at `.Lsched` —
**after `call0 intr_dispatch`**. It captured `intr_dispatch`'s return address
instead of the task's.

The same rule condemns more than `a0`: under call0, `a2..a11` are caller-saved,
so `intr_dispatch` may clobber all of them too. `a1` and `a12..a15` survive,
which is exactly the pattern the diff shows — `a1` correct, `a0` the first
mismatch found.

So the loop was never wrong. **It was reading the register file at a point where
the register file no longer held the task's values**, roughly seventy
instructions and two `call0`s after the moment it needed.

That is also why step 128's identity pair failed: it faithfully saved
`intr_dispatch`'s leftovers and faithfully restored them over the task's
registers.

### The fix, and why the placement is now forced

The save must run **before any `call0`** — in the prologue, after the frame
stores and the window-state save, while `a0..a15` still hold what the tick
interrupted. The restore must run at `.Lresume`, after the last `call0`, for the
mirror-image reason.

That is a stronger constraint than "somewhere in the handler", and it is the
first placement in this whole line of work that is *derived* rather than tried:
the sweep placements of steps 115–119 were guesses between plausible sites, and
this one is fixed by the ABI.

### A note on the revert

Reverting `vectors.S` alone broke the link — `task.c` still referenced
`g_regsave` — and the stale image then reported `wintorture PANIC`, which for a
moment looked like the revert had failed. It had not; the build had. The
instrumentation spans three files and has to come out together.

Suite after the full revert: boot 11 PASS 0 FAIL, `wintorture` CORRECT,
`blobphy` rc=0, `wifiinit` no fault and no reset.

## Step 130 — the forced placement does not boot

Step 129's diagnosis was acted on. The save moved into the prologue, after the
window-state save and **before any `call0`**; the restore to `.Lresume`, after
the last one. Two defects of the previous attempt were removed on the way:

- the pointer is now staged with `movi a0, g_regsave` **inside** each block,
  after that window's `a0` is already in EXCSAVE2, so **no register is clobbered
  before it is recorded**. Step 129's `movi a2, g_regsave` preamble destroyed the
  task's `a2`;
- EXCSAVE3 is no longer used, retiring an assumption that was never checked.

**The board does not boot.** 0 PASS, against 11 PASS before. Not a fault in a
test — the kernel does not reach the end of its own startup. Reverted; suite
green.

### What this does and does not tell us

It is a worse symptom than step 128's and therefore a *cheaper* one to chase: a
failure at boot is reproducible in eighteen seconds with no command typed, and
whatever it is, it happens on one of the first few ticks rather than deep inside
a torture run.

What is **not** in doubt, from step 129's diff: the save loop's rotation,
offsets, window mapping and store sequence are correct, and `a1` came back
bit-exact. This step changed *where* the loop runs, not what it does.

So the fault is in the new placement, and the two candidates are the two things
placement changed:

1. **The prologue runs on every interrupt from the very first tick**, including
   before the scheduler exists and before any task has a valid frame. The old
   site at `.Lsched` was reached only when a switch was actually being
   considered; the new one is not gated at all. `task_schedule` is not even
   called on the H1 defer path, but the save now runs there too.
2. **The restore at `.Lresume` writes the outgoing task's windows 1–3 into the
   register file after the scheduler has chosen a different task**, because
   there is still only one shared buffer. Across a real switch that is not an
   identity — it is task A's registers landing in task B's file. Step 1 was
   designed as an identity on the assumption that save and restore bracket the
   same task, and with a single buffer that assumption fails the moment the
   scheduler actually switches.

(2) is the more likely of the two and is *structural*: the identity-pair test
cannot be run with one buffer at these two sites, because the sites straddle the
switch. It needs either per-task slots — step 2, brought forward — or both halves
placed on the same side of `task_schedule`.

That is a design error in the plan rather than in the code, and it is mine:
UM-NATOS-043 §8 sequenced "identity pair" before "per-task slots" without
noticing that the forced placement of the pair puts a context switch between its
two halves.

### Next

Bring per-task slots forward: index `g_regsave` by the outgoing task on save and
by the incoming task on restore, which is what makes the pair an identity again
and is required by the design regardless. `g_current` is `static` in `task.c` and
will need exposing — the accessor that already exists, `task_current()`, is a
`call0` function and cannot be called from the prologue, so this wants a plain
non-static variable rather than a call.

Suite: boot 11 PASS 0 FAIL, `wintorture` CORRECT, `blobphy` rc=0, `wifiinit` no
fault and no reset.

## Step 131 — Tier B's save pass is in, per-task, and green

First piece of Tier B that works in place.

```
boot 11 PASS 0 FAIL   wintorture CORRECT   blobphy rc=0
blobtx force 0x3004   wifiinit no fault, no reset
heap 76,496 B usable  (was 79,568 -- exactly the 3,072 costed)
```

The save runs on **every** switch, in the prologue **before any `call0`**, into
the outgoing task's own 256-byte slot. It is a no-op when correct, and the suite
says it is.

### What made it work, in order

**Step 129** — a register-by-register diff, rather than pass/fail: `a1` matched
bit-exact, `a0` did not, and `a0` is the call0 return-address register. The loop
was right; it was reading the file two `call0`s too late.

**Step 130** — the forced placement alone did not boot, because a single shared
buffer cannot bracket a context switch. The restore at `.Lresume` ran after the
scheduler had chosen a different task, so it wrote task A's windows into task
B's file. That was an error in my plan, not the code: UM-NATOS-043 §8 ordered
"identity pair" before "per-task slots" without noticing the pair straddles the
switch.

**This step** — per-task slots brought forward, and the save proven alone.

Three properties earned along the way, each of which had to be discovered:

- **`ROTW` rotates by an immediate**, so no general register moves the window.
  That retires §8.1's objection *and* makes a save-only pass a genuine no-op,
  which is what allowed the two loops to be tested separately at all.
- **Nothing is clobbered before it is recorded.** Each block stashes its `a0` in
  EXCSAVE2 and only then loads a pointer into it. Step 129's `movi a2, ...`
  preamble destroyed the task's `a2`.
- **The slot is re-derived per block** rather than carried across `ROTW`, because
  a register holding it would be one of the registers being saved.

`g_current` is no longer `static`. The prologue cannot call `task_current()` --
it is `call0` -- so a variable is the only option, not a preference.

### What is not done

**The restore.** Nothing reads `g_regsave` yet, so this buys no behaviour; it
proves the hard half is sound and costs the 3,072 bytes up front. The restore
goes at `.Lresume`, after the last `call0`, indexed by `g_current` -- which by
then is the INCOMING task, because `task_schedule` has run. That asymmetry is the
whole reason per-task slots were needed, and it is what makes the pair an
identity again.

After that: take `WINDOWBASE`/`WINDOWSTART` from the saved file rather than the
frame, delete `g_win_mask`/`g_win_union`/the grant, then `BLOB_PIN_DISABLE 1` and
`wintorture` **read with its control line** (step 120) against step 121's
baseline of an immediate `IllegalInstruction`.

## Step 132 — the restore cannot be enabled while the union grant exists

Restore added at `.Lresume`, after the last `call0`, indexed by `g_current` --
by then the incoming task. Structurally the mirror of step 131's save.

**Boot drops to 10 PASS and `wintorture` fails.** Reverted to the save-only
state, which is green.

### Why, and it is the same lesson as step 130

The restore writes windows 1–3 from the **incoming** task's slot. For a task that
has never been switched out, that slot is zeros — so the restore erases whatever
those windows held.

Under the current design that is fatal, because the restore also still computes
`(1 << base) | g_win_union` and hands tasks back frames that are **expected to
have survived in the register file**. Two contradictory models of who owns the
register file are now both live: per-task slots say "your windows come from
memory", the union says "your windows are still in the file where you left them".
The first erases what the second promises.

So step 2 and step 3 of UM-NATOS-043 §8 — "switch the restore over" and "delete
the bookkeeping" — **cannot be separated.** Enabling the per-task restore
*requires*, in the same build:

- taking `WINDOWBASE`/`WINDOWSTART` from the saved register file rather than from
  the frame, and
- deleting the `| g_win_union` grant,

because the union's promise is exactly what the restore invalidates.

### The pattern worth naming

This is the third time the plan has assumed two pieces were separable when the
mechanism couples them:

- §8.1 — save and restore, separable only once `ROTW` removed the clobber;
- step 130 — identity pair before per-task slots, when the pair straddles a
  switch;
- here — restore before deleting the union, when the union is what the restore
  contradicts.

Each was a sequencing error in a plan I wrote, found by building it. The code has
been sound at every step since 129; **the ordering has been the recurring
defect**, and the reason is consistent: I keep decomposing by *what the change
touches* rather than by *what the mechanism guarantees*.

The remaining Tier B work is therefore **one build, not three**: restore +
window-state-from-slot + union deletion, together, judged on `wintorture` with
the pin off and its control line read (step 120) against step 121's baseline.
That is a bigger single step than this log prefers, and it is what the mechanism
requires.

Suite: boot 11 PASS 0 FAIL, `wintorture` CORRECT, `blobphy` rc=0, `wifiinit` no
fault and no reset. The save pass from step 131 remains in and green.

## Step 133 — all three together: boot recovers, the windowed paths do not

Restore, window-state-from-slot and union deletion in one build, as step 132
concluded they must be. One ordering point was got right and is worth keeping:
the restore sits **after** `wsr.windowbase`, because a slot's four blocks are
indexed relative to the owning task's `WINDOWBASE`, so the rotation must start
from the *incoming* task's base rather than whatever the handler was running at.

```
boot 11 PASS 0 FAIL        (step 132 broke this -- 10 PASS)
wintorture   FAIL/PANIC
blobphy      rc=0
wifiinit     PANIC
```

**Boot recovers fully.** Deleting the union alongside the restore fixed what
step 132's restore-alone broke, which confirms step 132's diagnosis: the two
were contradicting each other, and removing the contradiction restored startup.

**The windowed paths still fail.** `wintorture` panics and `wifiinit` panics —
the latter clean since step 113. Reverted; suite green with step 131's save pass
still in.

### Where this leaves Tier B

Three of four pieces are now known-good in place:

| piece | state |
|---|---|
| save, per-task, before any `call0` | **in and green** (step 131) |
| union deletion | **works** — boot recovers with it |
| restore placement after `wsr.windowbase` | **correct in principle**, and boot proves it is not catastrophic |
| restore contents | **wrong** — the windowed paths fail |

The remaining defect is narrow and, for once, unambiguous in kind: the save has
been proven bit-exact for `a1` (step 129) and harmless in place (step 131), boot
survives the restore, so what is wrong is **what the restore writes into windows
1–3, or when a slot has never been populated**.

The specific untested assumption: a task scheduled for the first time has a slot
of zeros, and the restore writes those zeros over three windows. With the union
gone nothing hands the frames back, so any live windowed frame belonging to
another task is destroyed at that moment. The save populates a slot only for
tasks that have been switched *out*, so every task's first switch *in* restores
garbage.

That points at a concrete next change rather than a hypothesis: **mark a slot
valid on first save, and skip the restore for a slot that has never been
written.** It is a one-word flag per task and it makes the first schedule of each
task a no-op instead of an erasure.

### Honest position

Tier B is not working, after five builds. What it has produced is a save pass
that is in, green, and proven; a costed and confirmed 3,072-byte footprint; and
a defect narrowed from "the register file is shared" to "the restore writes an
unpopulated slot". The three sequencing errors (§8.1, step 130, step 132) were
mine and are recorded as such.

Suite: boot 11 PASS 0 FAIL, `wintorture` CORRECT, `blobphy` rc=0, `wifiinit` no
fault and no reset.

## Step 134 — the valid flag changes nothing; the unpopulated-slot account is wrong

One word per task, set at the end of the save, checked before the restore. Step
133 named it as the indicated fix.

```
boot 11 PASS 0 FAIL        wintorture FAIL/PANIC
blobphy rc=0               wifiinit   PANIC
```

**Byte-for-byte the same as step 133.** Reverted; suite green with the save pass
still in.

### What is now excluded

Step 133's account was: a task scheduled for the first time has a slot of zeros,
the restore writes those zeros over windows 1–3, and with the union gone nothing
hands the frames back. Skipping the restore for an unwritten slot makes that
impossible — and the result did not move at all.

**So restoring an unpopulated slot is not the defect.** That was a plausible,
specific, testable account and it is now retired. It cost one build, which is
what a hypothesis of that shape should cost.

### What the shape of the result says

Boot passes 11 of 11 with the full restore active. That is not a small thing: the
restore runs on every switch during startup, across ten tasks, and startup is
clean. Whatever is wrong is **specific to the windowed paths** — `wintorture` and
`wifiinit` — and not to the mechanism of copying 64 registers per switch.

The distinguishing feature of those two paths is the only one left standing: they
are the paths where **more than one window is live at a time**. Every other task
runs call0 with a single frame, and for those the restore demonstrably works.

Which suggests the next thing to read is not another hypothesis but the same
instrument that settled step 129 — a diff, this time on the **restore** side:
save a task's slot, let it be scheduled, and compare the register file it comes
back with against the slot it should have come back with. That turns "the
windowed paths fail" into "this register, this window, this offset", exactly as
step 129 turned "the pair is wrong" into "a0, and here is why".

### Position after six builds

Tier B is not working. What is established:

- the save is in, green, per-task, and proven bit-exact (steps 129, 131);
- the union deletion is required and correct (steps 132, 133);
- the restore's placement after `wsr.windowbase` is right (boot proves it);
- the restore's *contents* are wrong, and it is **not** the unpopulated-slot case;
- 3,072 bytes, exactly as costed.

Four accounts of the restore failure have now been proposed and three retired.
The remaining work is a measurement, not another attempt.

Suite: boot 11 PASS 0 FAIL, `wintorture` CORRECT, `blobphy` rc=0, `wifiinit` no
fault and no reset.

## Step 135 — the restore-side diff is invalid; the reading must not be used

Restore from the slot, then a verify pass re-reading all 64 registers into a
second buffer, then a comparison in `task_schedule`. It reported:

```
restore : win 0 a0 got 0x00000000 want 0x400838bd
```

**Discard that.** The instrument is wrong, and the flaw is structural.

The comparison runs in `task_schedule`. The **save pass runs in the prologue,
before `task_schedule` is reached.** So by the time the diff executes, the task's
slot has already been overwritten with its registers *at the new switch-out*.
`want 0x400838bd` is not the value that was restored; it is the value saved
microseconds earlier, on a different switch.

The two sides of the comparison are from different events, which is precisely the
defect step 123 caught in its own first attempt (`g_ih_a1_raw` rewritten every
interrupt while `calc` was latched once). **Fourteenth entry in the catalogue,
and a repeat of a lesson learned twelve steps ago.**

### What a valid version requires

The verify buffer and the slot must be compared **before anything can rewrite
either**. Two ways, both cheap:

1. **Compare in the prologue**, in assembly, before the save pass runs —
   awkward, since the comparison wants C.
2. **Snapshot the slot at restore time** into a third buffer, alongside
   `g_regverify`, so the diff compares two things captured in the same event.
   That is what step 123's fix did, and it is the one to use.

The instrument cost one build and produced nothing, which is the correct price
for catching it here rather than acting on it.

### Unchanged

`wintorture` panicked with `exccause 28`, `epc 0x40080155` — `_WindowUnderflow12
+ 0x15`, the same signature as steps 117–126. The restore is still wrong; nothing
in this step says how.

Reverted. Suite: boot 11 PASS 0 FAIL, `wintorture` CORRECT, `blobphy` rc=0,
`wifiinit` no fault and no reset. Step 131's save pass remains in and green.

## Step 136 — the restore is provably correct: all 64 landed

Instrument fixed the way step 123's was: both sides captured in the same event.
The prologue now **holds off re-saving** while `g_diff_pending` is set, so the
slot survives untouched until the comparison runs. One task carries one stale
slot for one switch, which costs nothing while no slot is load-bearing.

```
restore : all 64 landed
```

**Every one of the 64 registers matches the slot exactly.** First positive
verification in the whole Tier B effort, and it settles the largest open
question: the save writes the right values (step 129, `a1` bit-exact), and the
restore puts them back — correctly, in the right physical registers, for all four
windows.

So of the four pieces, three are now *proven* rather than merely not-crashing:

| piece | state |
|---|---|
| save, per-task, before any `call0` | proven (129, 131) |
| restore, after `wsr.windowbase` | **proven — all 64 landed** |
| union deletion | required and correct (132, 133) |
| **what else the switch needs** | **the remaining defect** |

### The fault changed, and that is the finding

`wintorture` still panics, but no longer as `_WindowUnderflow12 + 0x15`
(`exccause 28`). It is now:

```
exccause 0 (IllegalInstruction)   epc 0x6eeeeeee
```

That is **step 121's pin-off baseline signature** — a `retw` whose `a0` came back
as stack fill, `(PC & 0xC0000000) | (0xEEEEEEEE & 0x3FFFFFFF)`. With the pin still
ON.

Which says what remains. The register *file* is now private per task, and
demonstrably so. `WINDOWSTART` is not: it still comes from the frame, and the
phantom bit step 124 found is still in it. Tier B confines the registers; it does
nothing about a bit claiming a frame that never existed. Restoring 64 correct
registers alongside a `WINDOWSTART` that lies still produces an underflow into an
unwritten save area.

UM-NATOS-043 §6 said this plainly — "It does not eliminate the phantom window" —
and predicted containment rather than repair. What it did not anticipate is that
with `g_win_union` deleted, the phantom stops being survivable: the union was
handing frames back and papering over it. So Tier B and the phantom have to be
solved together, not in sequence.

### Position after eight builds

Not working, and now for a *named* reason rather than an unknown one. The
mechanism is verified end to end; what defeats it is the same defect steps 53,
54, 57, 124 and 126 kept arriving at from different directions — a `WINDOWSTART`
bit that no frame backs, whose source has never been found.

That is the next thing, and it is no longer avoidable by design choice.

Reverted. Suite: boot 11 PASS 0 FAIL, `wintorture` CORRECT, `blobphy` rc=0,
`wifiinit` no fault and no reset. Step 131's save pass remains in and green.

## Step 137 — the phantom's source, named in the code's own comment

Three places write `WINDOWSTART` outside the scheduler, all in `window.S`, and
all do the same thing: **wipe it to the current window's bit alone.**
`x20_windowed` (line 967), and `phy_stack_call` before and after its `callx8`
(1172, 1192).

`phy_stack_call` says why, and in saying why it describes the phantom exactly:

> `WINDOWSTART` is deliberately **NOT** restored afterwards. The bits describe
> frames whose registers this excursion has already overwritten, so putting them
> back would re-mark frames whose contents are gone. A call0 kernel's correct
> steady state is one live frame, and that is what this leaves behind.

That comment is correct about its own behaviour and correct about the danger. The
defect is that **the kernel then does the very thing the comment refuses to do.**

`g_win_mask[]` still records those frames as owned by their tasks. The excursion
does not clear them — it has no idea they exist. So at the next switch the
restore computes:

```
grant = (1 << base) | g_win_union
```

and `g_win_union` re-marks exactly the frames whose registers `phy_stack_call`
overwrote. **A bit set for a window whose contents are gone is a phantom**, and
this is a mechanism that produces one, by construction, every time a windowed
excursion runs while another task holds frames.

### Why this closes the arc

- Step 124 measured a `WINDOWSTART` bit whose `a1` is `0xeeeeeeee` — a frame
  that is claimed and empty. This produces precisely that.
- Steps 53, 54, 57, 94 and 108 all failed to fix the ownership rule. They could
  not: the rule's *inputs* are wrong, because `g_win_mask` is never told when an
  excursion destroys what it is tracking.
- Step 126 showed a phantom can be neither spilled nor restored. Nothing
  downstream could have helped.
- Step 136 showed Tier B's register save/restore is provably correct and *still*
  faults — because Tier B preserves registers, and this defect is about a bit
  claiming registers that were legitimately overwritten by someone else.

### What follows

The excursions and the bookkeeping have to agree. Three shapes, in order of
size:

1. **Tell the bookkeeping.** `phy_stack_call`/`x20_windowed` clear
   `g_win_mask[]` for every task when they wipe, since that is precisely what
   the wipe means. Small, and it makes `g_win_union` truthful rather than
   optimistic.
2. **Do not wipe.** Spill the frames the excursion is about to overwrite instead
   of disowning them. Larger, and it is spill-on-preemption again in a different
   place.
3. **Tier B makes the question moot** — with the register file private per task,
   an excursion cannot overwrite another task's frames at all, so there is
   nothing to disown. This is the argument UM-NATOS-043 §3A.1 makes, arriving
   from a third direction.

(1) is the cheap test of this account and can be done in one build: if clearing
`g_win_mask` at the wipe removes the phantom, the mechanism is confirmed.

**No code changed in this step.** Suite: boot 11 PASS 0 FAIL, `wintorture`
CORRECT, `blobphy` rc=0, `wifiinit` no fault and no reset.

## Step 138 — option (1) is in and harmless, and the bench cannot judge it

The three `WINDOWSTART` wipes in `window.S` now set `g_win_disowned`, and
`task_schedule` clears `g_win_mask[]` and `g_win_union` when it sees the flag.
Three stores in assembly and five lines of C, making the bookkeeping believe what
the wipe already did.

**Pin on:** boot 11 PASS 0 FAIL, `wintorture` CORRECT, `blobphy` rc=0,
`blobtx force` 0x3004, `wifiinit` no fault. No regression.

**Pin off:** identical to step 121's baseline.

```
frames : task 5 held 0x0000aa8a granted 0x00000008 LOST 0x0000aa82
exccause 28   epc 0x40080155
```

### Why that is not a verdict on option (1)

`LOST 0x0000aa82` is **frame loss** — six windows held and not granted back —
which is the shared-register-file problem. Option (1) does not address it and was
never going to: it targets the phantom, a bit set for a window whose *contents*
are gone, which is a different defect.

With the pin off, frame loss dominates and fails first. So the bench cannot
isolate the phantom while the register file is still shared. **Step 137's account
remains untested**, not refuted.

That is the answer to "is Tier B still worth it", and it is a firmer yes than
before: **Tier B is now a prerequisite for testing the phantom fix at all**, not
merely a competing approach to it. The order is forced —

1. Tier B, so tasks stop losing frames and `LOST` reaches zero;
2. *then* the phantom becomes the first failure rather than the second, and
   option (1) becomes testable.

### What is kept, and on what basis

Option (1) stays in. It is green across the full suite, it costs three stores,
and it makes `g_win_union` honest about something `phy_stack_call`'s own comment
says must not be put back. **It is retained as a correctness alignment, not as a
verified fix** — nothing has shown it changes any outcome, and the log should not
later mistake its presence for evidence.

`g_disown_hits` counts the wipes, so the next pin-off run can say whether the
path is even exercised during `wintorture` — which is worth knowing before
believing any of this.

Suite: boot 11 PASS 0 FAIL, `wintorture` CORRECT, `blobphy` rc=0, `wifiinit` no
fault and no reset.

## Step 139/140 — Tier B assembled; a real defect found, and it is not finished

Full assembly: save (step 131) + valid flag + restore (verified in 136) + union
deleted. Pin off, and `LOST` did **not** move: still `0x0000aa82`.

### The defect that exposed

`LOST` is computed against the **grant**, and the grant was synthesised:

```asm
ssl  a2
sll  a3, a3          /* 1 << saved WINDOWBASE */
```

**The task's real `WINDOWSTART` sits in the frame at offset 88 and was never read
back.** Not by Tier B, and not by any version of this handler since it was
written. The restore has always discarded it and rebuilt a grant from the base
bit plus `g_win_union`.

With a shared register file that was survivable, because the union handed the
other frames back. With Tier B it is fatal in a specific way: **all 64 registers
are restored correctly — step 136 verified every one — and then the hardware is
told only one window is live.** The other six frames are present and unclaimed,
so the first `retw` past them underflows into a save area nobody wrote and
returns through poison. That is `epc 0x6eeeeeee`.

**`LOST 0x0000aa82` was measuring exactly this, and I read it as frame loss for
twenty steps.** The frames were never lost. The grant was refusing to admit them.

### The correction, and where it got to

Taking the grant from `frame[88]` instead moved the fault from
`_WindowUnderflow12` to `_WindowOverflow8 + 0x09`, `StoreProhibited` — a
different failure, not a fix. Admitting seven frames means the overflow path now
runs on them, and one of those frames is step 124's phantom, whose `a1` is
`0xeeeeeeee`. Step 126's guard skips the store; something after it does not.

Also noted: the `frames :` line still printed `granted 0x00000008` after the
change, because `task.c` recomputes the grant independently rather than reading
what the handler actually wrote. **That diagnostic is now wrong and will mislead
the next reader** — it must be made to report the real value or be deleted.

### Position

Reverted. Suite green: boot 11 PASS 0 FAIL, `wintorture` CORRECT, `blobphy`
rc=0, `blobtx force` 0x3004, `wifiinit` no fault and no reset. Kept in: step
131's save pass, step 126's overflow guard, step 138's disown.

Tier B is **not finished**, and this is the honest state of it:

| piece | state |
|---|---|
| save, per-task | in, green, verified bit-exact |
| restore | verified — all 64 land |
| union deletion | correct |
| grant from `frame[88]` | **found, necessary, and not enough** |
| the phantom | still there, now on the overflow path |

What changed today is that the last unexamined assumption in the restore has
been found and named. What has not changed is the phantom, which is now the only
thing standing between this and a working unpinned switch — exactly where step
137 said it would be, reached one step sooner than expected.

## Step 141 — the `frames` diagnostic now checks itself, and the check is proven

`task.c` predicts the grant by recomputing what `_handler_level3` assigns. That
duplication is not optional — the vector has no room to walk a task table — but
it drifts silently, and step 140 caught it drifting: the handler was changed to
take the grant from `frame[88]` and this kept reporting `granted 0x00000008`, a
number nothing had written.

**The prediction is now checked against reality.** The handler already records
the grant it actually wrote, in `g_rin_ws`. One switch later, `task_schedule`
compares that against what it predicted for the same task, and a mismatch is
printed above the `frames` line by name.

Lagged by one switch deliberately: `g_rin_ws` is written *after* `task_schedule`
returns, so comparing in the same call would test this event's prediction against
the previous event's grant. That is the mistake step 135 made and step 123 made
before it, and it is now avoided by construction rather than by care.

### Both directions verified

An alarm that has never fired is the failure mode this log has catalogued
fourteen times, so it was tested in both states.

**Quiet when the models agree** — forced dump on a healthy system prints nothing.

**Fires when they do not.** Step 140's `frame[88]` change was reintroduced
deliberately for one build:

```
GRANT DRIFT: task.c predicted 0x00000002 but vectors.S wrote 0x00000000 for task 6
             the frames line below is computed from the wrong model
frames    : no task was ever granted less than it held
```

The second line is the point. `no task was ever granted less than it held` is
exactly the reassurance that survived twenty steps of being wrong, and it is now
labelled unreliable at the moment it becomes so, rather than being discovered to
have been fiction later.

Divergence reverted; the checker is quiet again.

### What this does not fix

Nothing about the phantom, Tier B, or the grant itself. It makes one diagnostic
incapable of lying quietly, which is worth a step of its own given how much of
this investigation has been spent on instruments that reported what they could
not observe.

Suite: boot 11 PASS 0 FAIL, `wintorture` CORRECT, `blobphy` rc=0, `blobtx force`
0x3004, `wifiinit` no fault and no reset.

## Step 142 — a latent bug found: tasks were created with WINDOWSTART = 0

Took the grant from `frame[88]` as step 140 concluded it must be, pin off, and
the drift checker fired on its first real outing:

```
GRANT DRIFT: task.c predicted 0x00000002 but vectors.S wrote 0x00000000 for task 6
excvaddr 0x0000aa8a   <- the WINDOWSTART value, used as an address
```

`frame[88]` was **zero** for task 6, because `task_create_with_stack()` seeded
`frame[TASK_FRAME_IDX_WSTART] = 0u`.

**A `WINDOWSTART` with no bit set is architecturally meaningless.** The bit at
`WINDOWBASE` is what marks the current frame; a context with no current frame
cannot execute. Every task in this kernel has been created that way since tasks
existed.

It never mattered because **the restore never read the word**. It synthesised
`1 << saved base | g_win_union` and overwrote the seed before it could be used.
The first change to read a task's real `WINDOWSTART` back exposed it immediately.

Fixed: `frame[TASK_FRAME_IDX_WSTART] = 1u << (wb & 15u)` — one live window at the
task's own base, which is what a freshly created task actually has. **Kept**, and
green with the grant reverted, because the seed is now correct whether or not
anything reads it.

### The drift checker paid for itself in one step

Written in step 141 and verified in both directions there. This is its first
unplanned use, and it turned "the fault moved to the overflow path" into "the
handler wrote 0x00000000 for task 6, and here is the task" without a single
extra build. Every previous finding of that shape cost two or three.

### What is still wrong

With the seed fixed and the grant still from `frame[88]`, the fault persists:

```
exccause 28   epc 0x4008e7da   excvaddr 0x0000aa8a
```

`0x0000aa8a` is task 5's held-window mask being dereferenced as a pointer, in
kernel code rather than in a window vector. Something takes a `WINDOWSTART`
value and uses it as an address. That is a narrow, specific thing to look for and
it is the next step: `0x4008e7da` names the instruction, and the symbol it falls
in will name the function.

Note the `frames :` line still printed `granted 0x00000008` while the drift
checker stayed quiet, which needs reconciling — either the checker's one-switch
lag is looking at a different task than the line reports, or the two disagree
about which grant belongs to which task. **Do not trust either number until that
is settled.**

Reverted the grant change; the seed fix stays. Suite: boot 11 PASS 0 FAIL,
`wintorture` CORRECT, `blobphy` rc=0, `blobtx force` 0x3004, `wifiinit` no fault
and no reset, drift checker quiet.

## Step 143 — `0x4008e7da` is inside `vendor_torture`, not kernel code

Resolved in a **rebuild of the configuration that produced it** — grant from
`frame[88]`, seed fixed — rather than against the current ELF. Addresses move
between builds, and reading a stale one against a new image is the trap that cost
four steps on `_phy_stack_top`.

```
containing symbol : 4008e7a0 vendor_torture
next symbol       : 4008e7ec vendor_spilltest
```

**The faulting instruction is in the windowed test function itself.** Step 142
recorded this as "something in kernel code takes a `WINDOWSTART` value and uses
it as an address". That was wrong. Nothing in the kernel dereferences it — a
register belonging to `vendor_torture`'s own frame came back holding `0x0000aa8a`,
and `vendor_torture` then used it the way it was entitled to use its own register.

So the question is not "who dereferences a mask" but **"which underflow handed a
frame the mask instead of its saved register"**.

### An arithmetic coincidence worth testing

The switch frame is 112 bytes at `[task_sp-112, task_sp)`, and `WINDOWSTART` is
stored at offset 88. That puts it at:

```
task_sp - 112 + 88 = task_sp - 24
```

A windowed frame whose `a1` is `task_sp - 8` has its base save area at
`[task_sp-24 .. task_sp-12]` — **exactly on top of the switch frame's
`WINDOWSTART`, `WINDOWBASE` and LOOP fields.** An underflow through such a frame
would recover `WINDOWSTART` as a register, which is precisely the observed value
in precisely the observed place.

Step 126 checked that the *innermost* frame's save area at `[task_sp-16 ..
task_sp-4]` lands in the frame's padding above offset 88, and it does. **What was
never checked is any frame below that.** The padding is 92..111 — twenty bytes —
so exactly one save area fits safely and the next one down collides.

This is a hypothesis with an address in it, not an account of a symptom. It is
testable directly: record `a1` for the frame the underflow is servicing and see
whether it is `task_sp - 8`, or generally whether any live frame's save area
falls below `task_sp - 16`.

If it holds, the fix is structural and cheap in either direction — move the
window-state fields to the top of the frame, or reserve the save-area span below
`task_sp` the way `task_create` already reserves 16 bytes at the stack top for
the chain terminator (steps 90 and the comment at `task.c:425`). The kernel has
solved this exact class of overlap once already.

### Correction

Step 142 said the dereference was in kernel code. It is not. Corrected here
rather than left to be inherited.

Suite unchanged: boot 11 PASS 0 FAIL, `wintorture` CORRECT, `blobphy` rc=0,
`blobtx force` 0x3004, `wifiinit` no fault and no reset.

## Step 144 — the switch frame is written through the CALL12 save area

Step 143's hypothesis was wrong in its detail and right in its arithmetic.

**Wrong:** it proposed a live frame at `a1 = task_sp - 8`. Stacks grow down, the
innermost frame *is* `task_sp`, and nothing lives below it. No such frame can
exist.

**Right:** something does read `[task_sp-24]`. It is one level up, and it is in
`_WindowOverflow12`, which this project wrote:

```asm
s32e a4, a0, -48    s32e a8,  a0, -32
s32e a5, a0, -44    s32e a9,  a0, -28
s32e a6, a0, -40    s32e a10, a0, -24     <-- offset 88 of the switch frame
s32e a7, a0, -36    s32e a11, a0, -20
```

A **CALL12 frame's extended save area spans `[caller_sp-48 .. caller_sp-20]`**,
where `a0` is the caller's stack pointer recovered by `l32e a0, a1, -12`.

With `caller_sp = task_sp`, that range is `[task_sp-48 .. task_sp-20]`. And
`_handler_level3` opens with `addi a1, a1, -112`, placing the switch frame at
`[task_sp-112, task_sp)` — **straight through it.**

### The value, the register, and the offset all agree

```
frame offset 88 = task_sp - 112 + 88 = task_sp - 24
[caller_sp - 24] is a10, per the store list above
frame offset 88 holds WINDOWSTART = 0x0000aa8a
observed: excvaddr 0x0000aa8a, inside vendor_torture
```

`a10` is handed the saved `WINDOWSTART` and `vendor_torture` uses it as the
pointer it believes it to be. **This is not a coincidence to test; it is
arithmetic.** And it explains the signature this investigation has returned to
since step 117: every one of these faults has been `_WindowUnderflow12`, because
only CALL12 frames reach 48 bytes below their caller.

### Scope — this is older and wider than Tier B

The collision needs only three things: a task in windowed code, a CALL12 frame,
and a tick. None of them involve Tier B, the sweep, the union, or the phantom.
**It has been present since `_handler_level3` was written**, and the pin has been
hiding it by preventing exactly that tick.

It also reframes several earlier steps. Step 126 verified the *innermost* frame's
base save area at `[task_sp-16 .. task_sp-4]` lands in the frame's padding, and
it does — but that check answered a smaller question than it appeared to. The
padding is 92..111. The CALL12 extended area starts at offset 64 and runs to 92,
overlapping `EPC3`, `EPS3`, all three LOOP registers, `WINDOWBASE` and
`WINDOWSTART`.

### The fix

The handler must not write within 48 bytes of `task_sp`. `task_create` already
reserves 16 bytes at the stack top for the chain terminator, for the same class
of reason (step 90, and the comment at `task.c:425`) — this is the same fix at
the other end.

Cheapest form: open the prologue with `addi a1, a1, -160` instead of `-112`,
leaving the 48-byte save-area span untouched below `task_sp` and the frame below
that. Every site that computes `current_sp + TASK_FRAME_BYTES` to recover
`task_sp` must move with it — step 123's `AGREE` check exists precisely to catch
that and will fire if one is missed.

**Not applied in this step.** The constant appears in the prologue, the epilogue
and `task.c`, and changing a stack-frame size across three files is not something
to do without a build and a full suite behind it.

Suite unchanged: boot 11 PASS 0 FAIL, `wintorture` CORRECT, `blobphy` rc=0,
`blobtx force` 0x3004, `wifiinit` no fault and no reset.

## Step 145 — the reserve is applied and correct; step 144's account is not

`TASK_FRAME_RESERVE 48` added in `task.h`, and the six sites that assumed the
frame size moved together: the prologue (`-160`), the epilogue (`+160`),
`task_create`'s frame placement, the overlap check, and both derivations of
`task_sp`.

**Pin on: fully green.** boot 11 PASS 0 FAIL, `wintorture` CORRECT, `blobphy`
rc=0, `blobtx force` 0x3004, `wifiinit` no fault, drift checker quiet. Tightest
stack 1,492 of 2,048 B free — the 48 bytes cost nothing that matters.

**Pin off: the signature changed.** `_WindowUnderflow12 + 0x15` — the fault this
investigation has returned to since step 117 — is **gone**, replaced by step
121's baseline `epc 0x6eeeeeee`. Something real was removed.

### But the account did not survive its own test

Adding the grant from `frame[88]` on top of the reserve gives:

```
exccause 28   epc 0x4008e7ee   excvaddr 0x0000aa8a
```

`0x0000aa8a` again — the `WINDOWSTART` value used as an address. **With the
reserve in, frame offset 88 sits at `task_sp-72`, outside the CALL12 range
entirely.** The mechanism step 144 described cannot be delivering that value any
more, and the value still arrives.

So step 144 was right that the switch frame overlapped the CALL12 extended save
area — that is arithmetic and it is not in doubt — and wrong that this was the
path by which `0xaa8a` reached a register. **Two true statements, one wrong
inference between them.**

### What is kept, and on what basis

The reserve stays. Its justification is independent of this fault: a CALL12
frame's `a4..a11` live at `[caller_sp-48 .. caller_sp-20]`, the interrupted sp is
a `caller_sp`, and writing 112 bytes of switch frame through that span corrupts
saved registers whenever a tick lands on a task holding a CALL12 frame. That is
true whether or not it explains `0xaa8a`, it is a real defect present since the
handler was written, and removing the `_WindowUnderflow12` signature is
consistent with having fixed something.

**Recorded as correct by arithmetic, not proven by outcome.** The distinction
matters: this log has twice let a plausible fix inherit credit for an unrelated
improvement.

### What is still unexplained

`0x0000aa8a` reaching a register in `vendor_torture`/`vendor_spilltest`, by a
route that is not the CALL12 save area. `WINDOWSTART` is loaded into `a3` at the
restore and consumed by `wsr.windowstart`, and `a3` is then reloaded from the
frame before RFI — so the obvious path is closed too.

The next question is narrow and has not been asked: **which register holds
`0xaa8a` at the fault**, rather than which address was dereferenced. The panic
dump prints the saved frame; comparing every word of it against `0xaa8a` names
the register, and the register names the path.

Suite: boot 11 PASS 0 FAIL, `wintorture` CORRECT, `blobphy` rc=0, `blobtx force`
0x3004, `wifiinit` no fault and no reset.

## Step 146 — `0xaa8a` is the handler's own scratch, left behind by the rotation

Asked the narrow question at last -- **which register**, not which address -- and
the dump answered it.

```
switch-in : n 6141 wb 3 ws 0x0000aa8a rbck 0x0000aa8a  commit ok
excvaddr  : 0x0000aa8a
saved frame @ 0x3ffb91d0: 0x8008e7e7 0x00000000 0x5683411b 0x0000000c ...
```

Two facts settle it:

1. **The grant is written correctly.** `ws 0x0000aa8a rbck 0x0000aa8a commit ok`
   -- the restore now admits all seven frames and the readback agrees. That half
   works.
2. **`0xaa8a` appears nowhere in the saved frame.** It is not coming from memory,
   so no save area is delivering it.

It is the handler's own scratch. The sequence is:

```asm
l32i     a3, a1, 88        /* a3 <- the task's WINDOWSTART */
wsr.windowstart a3
wsr.windowbase  a2         /* <-- the register view rotates HERE */
...
l32i     a3, a1, 8         /* writes the NEW a3 */
```

After `wsr.windowbase`, the name `a3` resolves to a different physical register.
The epilogue reloads that one. **The old `a3` -- physical register
`old_base*4 + 3` -- keeps `0xaa8a`, and that register belongs to one of the
windows the restore has just declared live.** `vendor_torture` then reads its own
register and gets a window mask.

### This hazard is already documented, for the other half

`vectors.S` carries a long X7 comment on precisely this instruction pair:

> The grant mask in a3 was computed under the OUTGOING window view. WINDOWSTART
> must therefore be written BEFORE `wsr.windowbase` rotates the view: afterwards
> the name a3 resolves to a different physical register.

That fix was correct and is still correct — it concerns *where the value is
written from*. What it does not cover is **what the value leaves behind.** The
same rotation that invalidates the operand also strands it.

It was invisible while the grant was `1 << saved_base`: the residue was a single
low bit, which is a plausible-looking small integer and, in a register the task
was about to overwrite, harmless most of the time. Reading the real
`WINDOWSTART` back made the residue large, distinctive, and fatal — which is why
this surfaced only now, and why it looked like the `frame[88]` change had broken
something.

### The fix

The handler must not leave scratch in a physical register the incoming task
owns. Cheapest forms, in order:

1. **Zero `a3` after `wsr.windowstart` and before `wsr.windowbase`** — one
   instruction, and the residue becomes 0 rather than a mask. Zero is still
   wrong-if-read, but it is not a plausible pointer, and the task's own reload
   overwrites the register it lands in.
2. **Use a register the incoming task does not own.** Requires knowing its
   window set, which is exactly what the grant is; circular but computable.
3. **Do the rotation last** — set `WINDOWBASE` after every register the epilogue
   will reload has been reloaded. Structural, and the ordering constraint from
   X7 makes it delicate.

(1) is one instruction and testable immediately. It is not a *correct* fix, only
a defanging, and the log should say so: the right answer is (3), and (1) buys the
time to do it properly.

Reverted to green. Suite: boot 11 PASS 0 FAIL, `wintorture` CORRECT, `blobphy`
rc=0, `blobtx force` 0x3004, `wifiinit` no fault and no reset. The 48-byte
CALL12 reserve from step 145 remains in.

## Step 147 — the scratch residue is fixed; `0xaa8a` is gone

### First, a correction to my own proposal

Step 146 offered "(3) rotate last -- set `WINDOWBASE` after every register the
epilogue reloads". **That is not implementable, and I should have seen it when I
wrote it.** The reloads are `l32i a0..a15` through `a1`; they must land in the
physical registers visible at the *incoming* base, so the rotation is required to
precede them. Loading first would write the outgoing view -- worse than the bug.

The implementable form is the same idea from the other end: **restore the
pre-rotation scratch from the frame, immediately before rotating.**

```asm
wsr.windowstart a3
rsync
l32i     a3, a1, 8        /* outgoing a3 */
l32i     a4, a1, 12
l32i     a5, a1, 16
l32i     a6, a1, 20
wsr.windowbase  a2
rsync
```

The ordering is forced in this direction too: it cannot be done *after*
`wsr.windowbase`, because by then `a1` names a different register and the frame
is no longer addressable through it.

`a2` is unavoidable — it is the rotation's own operand, so it still carries the
saved `WINDOWBASE` afterwards. That is a value in 0..15: not a plausible pointer,
not a mask, the least harmful residue available. **A known remainder, recorded as
one rather than left to be discovered.**

### Measured

Reserve + scratch restore + grant from `frame[88]`, pin off:

```
before : excvaddr 0x0000aa8a   <- the task's own WINDOWSTART
after  : excvaddr 0x000b8e4f   <- something else entirely
```

**`0xaa8a` no longer reaches any register.** The residue is gone, confirmed by
the symptom changing rather than by inspection.

### Three defects fixed, each confirmed by a changed symptom

| step | defect | evidence it went |
|---|---|---|
| 142 | tasks created with `WINDOWSTART = 0` | drift checker stopped firing |
| 145 | switch frame written through the CALL12 save area | `_WindowUnderflow12` signature gone |
| 147 | handler scratch stranded by the rotation | `0xaa8a` gone from `excvaddr` |

All three were present since `_handler_level3` was written. All three were hidden
by the pin, which prevents the tick that exposes them. None of them is Tier B,
the sweep, the union or the phantom — they are ordinary defects in the switch
path that a windowed workload was never run against.

### Still open

`excvaddr 0x000b8e4f`, `epc 0x4008e7f2`, pin off. A fourth value, not obviously a
mask or a stack address. The method that has worked three times running applies
unchanged: find which register holds it, and the register names the path.

Kept in and green: the 48-byte CALL12 reserve, the `WINDOWSTART` seed, the
scratch restore. The `frame[88]` grant is **not** kept — it is necessary and its
consequences are still being worked through.

Suite: boot 11 PASS 0 FAIL, `wintorture` CORRECT, `blobphy` rc=0, `blobtx force`
0x3004, `wifiinit` no fault and no reset.

## Step 148 — the register is `a3`, and it holds a stack pointer with 12 bits cut off

```
excvaddr : 0x000b8e58
saved frame @ 0x3ffb91d0: 0x8008e7eb 0x00000000 0x000b8e58 0x0000000c ...
                            [0]=a0     [1]=a2     [2]=a3
0x3ff00000 | 0x000b8e58 = 0x3ffb8e58   -- inside task 5's stack
```

Frame word 2 is offset 8, which is **`a3`**. The value is a valid task-stack
address with its **top 12 bits stripped**: `0x3ffb8e58` becomes `0x000b8e58`.

### Two things this rules out

**It is not the handler's stranded scratch.** That was step 147 and it is fixed —
`0xaa8a` is gone. This value is in the *saved frame*, written by the prologue at
switch-out, so **`a3` was already corrupt when the tick landed.** The handler
recorded a bad register faithfully rather than creating one.

**It is not a random pointer.** `0x3ffb8e58` is a real address in the right
stack, and the corruption is exactly `& 0x000FFFFF`. A wild store or a torn read
does not produce that; a shift or a mask does.

### The signature names a small family of instructions

Losing precisely the top twelve bits of a 32-bit value is `x << 12 >> 12`, or an
`extui`-style field extract of width 20. On Xtensa that means `SAR` was 12 or 20
when a shift ran over `a3`.

`ssl a2 / sll a3, a3` in the restore sets `SAR` from the saved `WINDOWBASE` and
shifts `a3` by it. With the grant as `1 << base` that operates on a constant 1
and is harmless. **The question is whether any other path lets `a3` reach a shift
with `SAR` set from something else** — `window.S` has three
`rsr.windowbase / movi / ssl / sll` sequences of its own in the wipes, and they
use `a9`/`a10`/`a11` rather than `a3`, so they are not it on inspection alone.

That is a lead, and it is recorded as a lead. What is measured is the register,
the value, the mask width, and that the corruption precedes the switch.

### Method note

Three faults running have now been resolved by asking *which register* rather
than *which address*, each in a single run: `0xaa8a` (step 146, handler scratch),
and now `a3` here. The dump already contained everything needed both times — the
change was in the question, not the instrument.

### A near-miss in the reporting

The revert build's flash **failed** -- `Could not open COM5, the port doesn't
exist`, the port still held by the preceding serial session -- and the suite that
ran afterwards was the **bench image**, not the reverted source. It duly reported
`wintorture FAIL` and `wifiinit PANIC`, which is correct for what was actually on
the board and wrong for what the commit claimed.

Caught because those two results contradicted a tree that had just been reverted.
`GetPortNames()` showed COM5 present the whole time; the earlier `Win32_SerialPort`
query saying "absent" was the unreliable one. Reflashed, verified.

**This is the same class as step 128's "build failed, stale image reported
clean" and step 135's cross-event comparison** -- the third time this session a
toolchain step failed and the run after it was read as a result.

**Correction to the first account of this.** It was recorded as "the build
script's exit status is checked; the flash step's is not". That is false.
`build.ps1` has always carried `if ($LASTEXITCODE -ne 0) { throw "flash failed" }`
on the flash, and `A fatal error occurred: Could not open COM5` was printed in
that very run. The script did its job. **The suite results underneath the failure
were read anyway.** The gap was in the reading, not the tooling, and calling it a
tooling gap would have left the actual habit unexamined.

The script now also *counts* `Hash of data verified` and refuses to continue on
fewer than three. That is worth having for a reason the exit code cannot cover --
a flash that succeeds having written only some segments -- and because it turns
"I should have noticed" into "the build stopped". It is not the fix for what went
wrong here.

Reverted to green, **verified after a successful reflash**. Kept: the CALL12
reserve, the `WINDOWSTART` seed, the scratch restore. Suite: boot 11 PASS 0 FAIL,
`wintorture` CORRECT, `blobphy` rc=0, `blobtx force` 0x3004, `wifiinit` no fault
and no reset.

## Step 149 — `a3` is a pointer by design, and it is reloaded from `[sp-4]`

Stopped inferring and read `vendor_torture`:

```asm
4008e7bc <vendor_torture>:
4008e7bc:  entry   a1, 64
4008e7c4:  mov.n   a3, a1        <- a3 IS a stack pointer, by design
...
4008e7f4:  call8   vendor_torture
4008e7fe:  l32i.n  a9, a3, 0     <- and it is the unwind cursor
4008e800:  addi.n  a3, a3, 4
```

`a3` is not a corrupted scalar. It is a copy of the frame base, kept across the
recursive `call8` and used to walk the six locals on the way out. The fault is a
**pointer that came back wrong**, which is a different thing from a value that
was mangled in place.

### Two corrections

**The 20-bit-mask lead is weak.** Step 148 reasoned that `0x3ffb8e58` losing its
top twelve bits implied a shift with `SAR` set to 12 or 20. Every shift and
extract in `window.S` and `vectors.S` was then listed: **none of them touches
`a3`.** The only `a3` shift is the grant's `ssl a2 / sll a3, a3`, which operates
on a `movi a3, 1`. So no code in this kernel masks `a3`, and "a shift or a mask
did this" should not be carried forward as though it were established.

**The location was wrong.** Step 148 identified `a3` as frame word 2 of the
*switch* frame — correct for where the prologue stores it at switch-out, and not
where the faulting value comes from. A windowed frame's `a0..a3` are saved at
`[sp-16 .. sp-4]`, so **`a3` lives at `[sp-4]`**, the top word of the base save
area, and an underflow reloads it from there.

That matters for the reserve. With the old layout the switch frame reached
`task_sp-112`, so `[task_sp-4]` was frame offset 108 — inside the padding. With
step 145's reserve the frame starts at `task_sp-160` and `[task_sp-4]` is inside
the 48-byte gap, untouched. **So for the innermost frame the save area is now
clean either way, and the bad `a3` must be arriving from a deeper frame's
`[sp-4]` — an address above `task_sp`, in the task's own stack, which the switch
frame never reaches.**

### What that leaves

The corruption is not the switch frame overwriting a save area. Something writes
`[sp-4]` of a live parent frame, or an underflow reads the wrong `sp`. Both are
checkable by address rather than by argument: the frames are at known offsets
from `task_sp`, and `[sp-4]` of each can be read and compared against the `a3`
each frame should hold — `vendor_torture` sets `a3 = a1`, so the correct value at
`[sp-4]` of any frame is that frame's own `sp`.

**That is an unusually strong invariant to test against**: every frame's saved
`a3` must equal its own `sp`. A single walk says which frame breaks it, and
whether the stored value is a truncation of the right answer or something
unrelated.

Suite unchanged: boot 11 PASS 0 FAIL, `wintorture` CORRECT, `blobphy` rc=0,
`blobtx force` 0x3004, `wifiinit` no fault and no reset.

## Step 150 — the invariant is right; memory is the wrong place to test it

Step 149's invariant refines to something needing no external data. A base save
area at `[sp-16 .. sp-4]` holds the **parent's** `a0..a3` — `a1` at `[sp-12]`,
`a3` at `[sp-4]` — and `vendor_torture` opens with `mov.n a3, a1`. So for any
frame whose parent is a torture frame:

```
[sp-4] must equal [sp-12]
```

The chain compared against itself. Implemented as a walk in the panic dump.

```
a3==a1 : 1 frames, all agree
```

**One frame, then the walk stops** — `[sp-12]` does not ascend, so there is
nowhere to go. Exactly where step 118's walk stopped, for exactly the same
reason, which I did not connect before building this.

### Why memory cannot answer it

With the pin off, the task's frames are **live in the register file**, not in
memory. Base save areas are written *lazily*, by the overflow handler, when a
frame is actually spilled — step 123 established that and step 149 forgot it. The
innermost frame has never overflowed, so `[task_sp-12]` is unwritten and the
chain has no second link to follow.

The invariant is sound. The place to evaluate it is not memory.

### The instrument for it is already in the tree

Step 131's save pass writes all 64 physical registers into `g_regsave` on every
switch, per task, and has been in and green since. In that slot, window *k*'s
registers are at `slot[k*16 + n]`, so:

```
a1 of window k = slot[k*16 + 1]
a3 of window k = slot[k*16 + 3]
```

The check is four comparisons against data that is already being captured, with
no walk, no chain, and no dependence on anything having been spilled. It reads
what the hardware actually held rather than what memory happens to record.

That is the next step, and it is small: compare those pairs for the faulting
task and report the first window where they differ, with both values. If a3 is a
truncation of a1 the mask question is settled; if it is unrelated, the mask lead
dies properly rather than by elimination.

### Note

Two instruments in a row have now been built to walk a chain that is not
populated in the configuration being measured. The distinguishing question —
*is this data in memory or in registers right now?* — is cheap to ask and has
been skipped twice.

Reverted to green, flash verified. Suite: boot 11 PASS 0 FAIL, `wintorture`
CORRECT, `blobphy` rc=0, `wifiinit` no fault and no reset.

## Step 151 — the register file is clean at switch-out; the damage is on the way in

Read `a1` and `a3` out of `g_regsave` — what the hardware actually held — instead
of walking a memory chain that does not exist in this configuration.

```
regs a1/a3: w0 0x3ffb91d0/0x3ffb634c*  w1 0x3ffb93f0/0x3ffb93f0=
            w2 0x3ffb9370/0x3ffb9370=  w3 0x3ffb92f0/0x3ffb92f0=
excvaddr  : 0x000b90c5
```

**Windows 1, 2 and 3 satisfy `a3 == a1` exactly**, at `0x3ffb93f0`, `0x3ffb9370`
and `0x3ffb92f0` — spaced `0x80` apart, which is `entry a1, 64` plus its save
area. Three clean `vendor_torture` frames.

Window 0's pair differs, and should: w0 is the **handler's own window**, not a
torture frame, so `a3` there is whatever the handler last used.

### Two things this settles

**The save path is not the defect.** At switch-out the register file holds
correct, full, aligned frame pointers. Whatever damages `a3` happens **after**
the save — on the restore, or while another task runs between the two. Every
step from 143 to 150 was looking at the outbound half.

**The mask theory is dead, and not by elimination.** `0x000b90c5` as a DRAM
address is `0x3ffb90c5` — **unaligned**. A frame pointer is 16-byte aligned; the
three real ones above end in `0`. A truncated frame pointer would still be
aligned. So this value was never a frame pointer at all, and the "top twelve bits
were shifted off" account from step 148 is wrong outright rather than merely
unsupported.

What `0x000b90c5` *is* remains open. It is close to the frames — `0x3ffb90c5`
sits just below `w0`'s `0x3ffb91d0` — which suggests something reading at a
byte offset into that region rather than a mangled pointer.

### Where to look now

The restore path, with the register file known good going in. That is a much
smaller surface than the last eight steps have been searching, and the same
instrument answers it: `g_regsave` is written at switch-out and the fault happens
after switch-in, so **capturing the file again immediately after the restore and
diffing the two says exactly which register changed and when.** Step 136 already
built that comparison and proved it works — `all 64 landed` — it simply has not
been pointed at a failing run.

Reverted to green, flash verified. Suite: boot 11 PASS 0 FAIL, `wintorture`
CORRECT, `blobphy` rc=0, `wifiinit` no fault and no reset.

## Step 152 — the switch-in capture does not boot; a register-lifetime slip

The measurement step 151 called for: capture the register file again at the end
of `.Lresume`, after the frame reload, and diff it against the slot saved on the
way out. Any register that differs was changed while the task was away — the
shared-register-file question asked directly.

**The board does not boot.** 0 PASS against 11. Reverted; suite green, flash
verified.

### The mistake, which is small and specific

The inserted block ends by restoring `a1`:

```asm
    l32i     a1, a4, 0                  /* a4 still holds g_switch_sp */
```

That comment is false at that point in the handler. `a4` holds `g_switch_sp`
earlier, but by the end of `.Lresume` the frame reload has already run and `a4`
carries the **task's** `a4`. The load reads through a task register and returns
garbage as the stack pointer, immediately before `rfi`.

I copied the idiom from where it is true and did not check it was still true
where I put it. That is the same class as step 129 (the save pass reading
registers two `call0`s after the values it wanted) and step 135 (comparing two
values captured in different events): **a correct fragment placed where its
preconditions no longer hold.** Three times now, and the common factor is
reusing a working sequence without re-verifying what it depends on at the new
site.

### The correct placement

The capture must run **before** the frame reload, not after — the reload is the
last thing that touches the register file, so the file is complete the moment
before it starts, and `a1` is still the frame pointer there. That also makes the
comparison sharper: the reload rewrites window 0 from the frame, so capturing
after it would mask any damage to window 0 with the frame's own copy.

Concretely, the block belongs immediately after the LOOP-register restore and
before `l32i a0, a1, 0`, with no `a4` dependency at all.

**Not attempted here.** The correct site is a five-line move from the wrong one,
but placing assembly in the interrupt epilogue at the end of a session is how
step 130 and step 152 both went, and the tree is green.

Suite: boot 11 PASS 0 FAIL, `wintorture` CORRECT, `blobphy` rc=0, `wifiinit` no
fault and no reset.

## Step 153 — the capture works and is not free; window 0 is the wrong place to look

Placed correctly this time: after the LOOP restore, before the frame reload,
where `a1` is still the frame pointer and nothing depends on `a4`. **It boots** —
11 PASS, against step 152's 0 — so the placement diagnosis was right.

It also produced a reading:

```
while away: w0 a0 was 0x400838bd now 0x4008af2e
```

### That reading is uninformative, and predictably so

`w0` is the **handler's own window**, and `a0` is the call0 return-address
register. Between the save in the prologue and the capture in the epilogue the
handler makes two `call0`s — `intr_dispatch` and `task_schedule` — so `a0`
differing is not damage, it is the handler working.

Step 151 already established this exact point about window 0: its `a1`/`a3` pair
differs *because* w0 is the handler's window, not a torture frame. I wrote that
down and then built a diff that starts at window 0 anyway. **The comparison
should begin at window 1**, or exclude `a0`/`a1` of w0 explicitly.

### And the block is not free

`wintorture` panics with the pin **on**, where it passed before. The capture was
described as non-destructive because each block restores its window's `a0` and
`a1` before rotating — true of the stores, but the four `rotw` steps still move
`WINDOWBASE` through windows whose `WINDOWSTART` bits are clear, *after* the
grant has been written and while the epilogue is mid-restore. That is a different
context from the prologue, where the same block has been running safely since
step 131.

So "non-destructive" was carried over from the save site without rechecking it
at the new one — **the same slip as step 152, one step after naming it.** The
difference is that this time the tree caught it rather than the boot.

Reverted; suite green, flash verified.

### What is worth keeping

The placement is correct and demonstrated: before the frame reload, no `a4`
dependency, boots cleanly. The instrument needs two changes before it can answer
anything: **start the comparison at window 1**, and find a capture that does not
rotate the window during the epilogue — reading the four windows via the already
proven prologue path and deferring the *comparison* is one option, since
`g_regsave` is written there safely on every switch already.

Suite: boot 11 PASS 0 FAIL, `wintorture` CORRECT, `blobphy` rc=0, `wifiinit` no
fault and no reset.

## Step 154 — the handler looks clean, and the instrument is not trustworthy enough to say so

Capture moved to the top of `.Lresume`, before any window-state write, on the
reasoning that the base and window state there match the prologue — where this
identical block has run safely since step 131. Comparison started at **window 1**,
since step 153 confirmed window 0 is the handler's own and differs for good
reasons.

```
handler : w1-w3 unchanged -- handler is clean
```

Between the prologue's save and the top of `.Lresume` — the span containing
`intr_dispatch` and `task_schedule` — windows 1 to 3 are **untouched**. Taken at
face value that clears the handler and moves the search to the window-state
write, the frame reload, or after `RFI`.

### Why it is not taken at face value

**`wintorture` still panics with the pin ON.** The block regresses the suite at
this site too, so my reasoning about matching conditions was wrong twice: it is
not safe in the epilogue (step 153) and it is not safe here either.

That makes the reading a measurement of **a system the instrument perturbed**.
"w1-w3 unchanged" may be true of the healthy kernel, or true only of the kernel
with four extra `ROTW` steps per switch. Nothing in the run distinguishes those.

This log has a name for reporting the first and ignoring the second, and it has
cost enough steps that the finding is recorded as **provisional** rather than
banked: *the handler appears clean, on an instrument that changes behaviour.*

### What is actually established

The `ROTW`-based capture is unsafe anywhere in the restore path — both ends
tried, both regress. It is safe only in the prologue. That is a property of the
block, now measured at three sites rather than assumed from one, and it bounds
what any future version of this instrument can do.

Confirming "the handler is clean" needs a capture that does not rotate. The
options are narrow: read the four windows in the prologue where rotation is
already proven safe and compare there, or find a way to sample the incoming
task's windows without moving `WINDOWBASE` at all — which, given `ROTW` is the
only way to reach them, probably means not sampling them and testing the
hypothesis some other way.

Reverted; suite green, flash verified. Suite: boot 11 PASS 0 FAIL, `wintorture`
CORRECT, `blobphy` rc=0, `wifiinit` no fault and no reset.

## Step 155 — the all-ones rotation is not the missing piece either

Tier B assembled with everything learned: save (step 131, in and green), valid
flag, restore, union deleted, grant taken from `frame[88]`, and one new idea from
step 154 —

**`WINDOWSTART` set to all-ones across the rotation.** Steps 153 and 154 showed
`ROTW` in the restore path regresses the suite, and the reason offered was that
rotating onto a window whose `WINDOWSTART` bit is clear leaves the machine
undefined for what follows. If that were the mechanism, marking every window live
for the duration of the rotation and installing the real mask afterwards would
fix it. The grant travelled in `EXCSAVE3` so it would survive the rotation — the
X7 hazard handled from the other side.

**Boot drops to 10 PASS and `wintorture` fails.** Reverted; suite green, flash
verified.

### What that costs and what it buys

The all-ones account of step 153/154 is **not supported**. Rotating onto
clear-bit windows may still be a problem, but it is not *the* problem, because
removing it changes nothing. That hypothesis should not be carried forward as
though it explained anything.

What stands unchanged is the measurement underneath it: **`ROTW` anywhere in the
restore path regresses the suite, and no account of why has survived contact.**
Four sites tried now (epilogue, top of `.Lresume`, and this), one variable
controlled, still broken.

### Honest position on Tier B

Eleven builds across steps 128–155. What is proven:

- the save is correct, per-task, and green — `a1` bit-exact (129), no-op in place (131);
- the restore mechanism is correct — `all 64 landed` (136);
- the union must go with it (132, 133);
- the grant must come from `frame[88]` (140);
- three unrelated switch-path defects were found and fixed along the way (142, 145, 147), each confirmed by a changed symptom.

What is not solved: assembling those into a working restore. Every arrangement
either fails to boot or fails `wintorture` with the pin **on** — that is, it
breaks the configuration that currently works, before ever being judged on the
configuration it was built for.

**That pattern is the finding.** The restore path tolerates the register file
being *read* in the prologue and does not tolerate it being *touched* on the way
back in, and eleven builds have not produced an account of why. Continuing to
vary the arrangement is not working; the next step should be to understand what
the restore path actually requires, from the ISA rather than from experiment —
specifically what `ROTW` guarantees about `WINDOWSTART`, and what the epilogue's
`RFI` depends on that a rotation can disturb.

Suite: boot 11 PASS 0 FAIL, `wintorture` CORRECT, `blobphy` rc=0, `blobtx force`
0x3004, `wifiinit` no fault and no reset.

## Step 156 — step 14 already ruled this out, and said why

Sent to look up what `ROTW` guarantees about `WINDOWSTART`. Found something more
useful first: **this log answered the question 140 steps ago.**

From step 14, "first attempt at window-aware switching. Failed, reverted":

> Direct register-file save was **ruled out first**: `rotw` rotates *every*
> register including whichever one holds the frame pointer, so there is no
> register left to address memory with. Parking it in `EXCSAVE_3` and re-reading
> it works for one group and then loses the group it clobbers.

and, on the attempt that was actually built:

> **It broke the case that already worked** — a *single* windowed task, which had
> been 6/6 before. Reverted; `wintorture` correct again immediately.

> Calling a windowed routine out of the level-3 handler means rotating the window
> in the middle of a context switch, while `a1` is the frame under construction
> and the handler's own state lives in registers the spill chain is about to walk
> over.

**That is the exact signature of steps 153, 154 and 155**: every arrangement
regressed `wintorture` with the pin ON — breaking the case that already worked,
before ever being judged on the case it was built for. I described that as "the
finding" in step 155. It was step 14's finding.

### What is genuinely new, and what is not

**New:** step 14's *first* objection is overcome. It says there is no register
left to address memory with, and that parking a pointer in `EXCSAVE` "works for
one group and then loses the group it clobbers". Deriving the pointer **inside
each block** with `movi a0, g_regsave`, after that window's `a0` is already in
`EXCSAVE2`, avoids carrying anything across the rotation at all. That is why the
save pass works and is green (step 131), and it is a real advance on step 14.

**Not new:** everything about rotating during the restore. Step 14 measured it,
recorded it, and named the mechanism. Steps 128–155 re-measured it at four sites
and produced no better account.

### On the ISA question itself

Still worth answering properly, and I am not going to answer it from memory. What
I recall — `ROTW` adds its immediate to `WINDOWBASE` and does **not** modify
`WINDOWSTART`, and the architecture requires `WINDOWSTART[WINDOWBASE]` to be set
for windowed instructions to behave defined — is consistent with step 155's
result, since setting all-ones changed nothing and my sequence executes no
windowed instruction anyway. But "consistent with" is not "verified", and this
log has been burned by recalled facts often enough that it should be read from
the Xtensa ISA reference before anything is built on it.

**The empirical finding is stronger than my recollection either way**, and it is
already recorded twice: rotating the window inside the switch breaks the working
case. That is a measurement at five sites across two separate attempts, 140 steps
apart.

### What this means for Tier B

Tier B's save half is sound and in. Its restore half requires rotating during the
restore, which is the thing this project has now failed at twice with different
people looking at it. **The design needs to change, not the arrangement.**

The option that does not rotate during the restore: leave the incoming task's
extra windows where they are and let the existing underflow handlers pull frames
back from the stack lazily, as the hardware intends — which is what the kernel
already does, and which makes Tier B's restore unnecessary rather than difficult.
That reduces Tier B to "save on the way out so nothing is lost", and the question
becomes whether a saved-but-never-restored file is any use. It is not obviously
so, and that is the honest place to reconsider.

Before the next attempt, read step 14 and steps 128–155 together. They are the
same investigation.

Suite unchanged: boot 11 PASS 0 FAIL, `wintorture` CORRECT, `blobphy` rc=0,
`blobtx force` 0x3004, `wifiinit` no fault and no reset.

## Step 157 — a real sleep brings back the original fault

Replaced step 113's ccount spin with a genuine park, ordered as the evidence
said it should be:

```
caller bridges in PINNED        -- the bridge's `entry` frame is created where
                                   no tick can land on it
win_spill_call0()               -- one live window, still pinned
g_pinned = -1                   -- unpin only now
task_sleep(1)                   -- park
g_pinned = me                   -- repin before the bridged return
```

The claim was that nothing windowed is created between the spill and the park —
step 112's rule — since everything after `win_spill_call0()` is call0.

**`wifiinit` panics: `exccause 28`, `epc 0x400800d5`.** That is
`_WindowUnderflow8 + 0x15`, the instruction `l32e a4, a7, -32` — **the original
`wifiinit` fault, verbatim, from steps 86 through 113.** The rest of the suite is
untouched: `wintorture` CORRECT, `blobphy` rc=0, `blobtx force` 0x3004. Reverted.

### What that tells us

The fault is specific to the blocking path and returns the moment the spin is
replaced. Step 113 did not *fix* that fault — it **avoided** it, by removing the
only construct that triggers it. That was clear at the time and is worth
restating now that the avoidance has been tested: the spin is not a workaround
for a solved problem, it is the problem still being routed around.

My ordering argument is therefore incomplete somewhere. The obvious gap: the
spill leaves the **bridge's own frame** live, so on return the bridge's `retw`
underflows the *caller's* frame back from a save area written before the park.
Everything about that is meant to work, and the panic is in exactly that
handler — `_WindowUnderflow8`, reading the extended save area at `[a7-32]`.

So the next question is narrow and has an address in it: **at the park, what is
in `osi_s_queue_recv`'s save area, and is it the same when the task wakes?** If
it changes across the sleep, something writes it while the task is parked. If it
was wrong before the park, the spill did not write what it should have.

That is checkable with the same read-a-range discipline that resolved steps 146
and 151, and it is a better-posed question than any asked in steps 128-156 —
because the failing path is now four instructions long and bounded by a spill on
one side and a `retw` on the other.

### Status

`osi_impl_park()` is reverted, not kept. The busy-spin stands, and with it the
600 ms per `_queue_recv` and the inability to wait on an interrupt.

Suite: boot 11 PASS 0 FAIL, `wintorture` CORRECT, `blobphy` rc=0, `blobtx force`
0x3004, `wifiinit` no fault and no reset.

## Step 158 — the save-area probe cannot report its own state, and moved the fault

Park reinstated, with `[sp-48 .. sp+12]` sampled before the bridged call and
again after it returns — spanning the extended save area `_WindowUnderflow8`
reads at `[a7-32]` and the base save area below it.

```
save area : park never reached
exccause  : 9 (LoadStoreAlignment)   epc 0x4008e6af   excvaddr 0x4008d002
underflow : recovered a0 0x3ffd8f78 from save area 0x3ffb2830
```

**Two failures, both mine, and neither is a finding about the kernel.**

### The probe cannot distinguish its own states

`g_sa_diff` stays at `-2` until the **post**-capture runs. So "park never
reached" prints in two completely different situations: the park was never
reached, or **the park was reached and never returned.** Those are the two
outcomes the probe exists to separate, and it reports the same string for both.

A separate flag for the pre-capture was needed and is one line. This is the
fifteenth entry in the catalogue and it is the plainest one yet — not a subtle
context mismatch, just a state variable doing two jobs.

### And it moved the fault

Step 157 gave `_WindowUnderflow8 + 0x15`. This gives `LoadStoreAlignment` at
`0x4008e6af` with `excvaddr 0x4008d002` — unaligned, in IROM. Different fault,
different place. The `underflow` line reports `a0` recovered as `0x3ffd8f78`, a
blob `.bss` global, from save area `0x3ffb2830` — which is UM-NATOS-042 §4's
original signature, not step 157's.

So the sixteen-word read either side of the park changed what the path does.
**Fourth instrument in this stretch to perturb what it measured** — after steps
152, 153 and 154.

### What that pattern is worth

Six consecutive steps have now produced instrument failures rather than kernel
findings. Steps 146 and 151 worked because they read data the kernel was
*already* recording; every failure since has come from adding a new capture into
the path under test.

That is a usable rule, and it is the one to apply next: **do not add reads to the
blocking path.** If the question is what happens to that save area across the
park, the answer has to come from something already being written — the switch
frame, `g_regsave`, the existing `uf`/`of` probes — or from changing *when* the
park happens rather than watching it.

### Status

Everything reverted. The busy-spin stands. Suite: boot 11 PASS 0 FAIL,
`wintorture` CORRECT, `blobphy` rc=0, `blobtx force` 0x3004, `wifiinit` no fault
and no reset.

## Step 159 — the park fault is deterministic, and `a7` comes back holding a PS

Step 158's rule applied: change *when* the park happens rather than watching it.
Three builds, sleep durations 1, 4 and 16 ticks, no probes added to the path.

```
ticks=1   PANIC  exccause 28  epc 0x400800d5  excvaddr 0x00060500
ticks=4   PANIC  exccause 28  epc 0x400800d5  excvaddr 0x00060500
ticks=16  PANIC  exccause 28  epc 0x400800d5  excvaddr 0x00060500
```

**Identical — same cause, same instruction, same address, sixteen-fold change in
how long the task is away.**

### What that rules out

Not a race. Not a window where another task's timing matters. Not a wake arriving
early or late. The park produces the same wrong value every time regardless of
duration, which makes it **structural**: the sequence itself is wrong, and it
would be wrong if the task slept for a microsecond or a second.

That retires a whole class of explanation that has been implicitly live since
step 106 — anything of the form "a tick lands at an unlucky moment". It does not.

### And the value names itself

`epc 0x400800d5` is `_WindowUnderflow8 + 0x15`, the instruction `l32e a4, a7, -32`.
So `a7 = 0x00060500 + 32 = 0x00060520`.

**That is a PS value.** Every processor-state word printed in this log has the
shape `0x0006xxxx` — `0x00060320` and `0x00060330` appear in the dumps of steps
142, 146 and 148. `a7` is the caller's stack pointer, recovered by the underflow
from `[a9-12]` of the frame it is restoring.

So the underflow read a **saved PS where a stack pointer belongs**, and then
loaded through it.

This cost nothing to learn: no probe, no added read, no perturbation. It came out
of the pass/fail signal and the existing dump — which is exactly what step 158
said the next move had to look like.

### Where a PS could come from

The switch frame stores `EPS3` at offset 68 and `EPC3` at 64. `task_sleep()`
parks through the ordinary block path. Something in that sequence puts a saved
processor-state word at the address the underflow later reads as a frame link —
and since the fault is deterministic, it is a fixed offset relationship, not a
collision that sometimes happens.

That is the next thing to work out, and it is arithmetic rather than
experiment: which write puts `EPS3` at the address `[a9-12]` resolves to for the
frame being restored. The switch frame's layout and the park's stack geometry are
both known and fixed.

Reverted; suite green, flash verified: boot 11 PASS 0 FAIL, `wintorture` CORRECT,
`wifiinit` no fault and no reset.

## Step 160 — the park's stack geometry, written down; the derivation is not finished

Step 159 established the fault is deterministic and that `a7` comes back holding
a PS value, so there is a **fixed** offset relationship putting a saved `EPS3`
where a frame link belongs. That is arithmetic. Here is what the arithmetic has
to work with, read from the code rather than recalled:

```
stub frame sp                    = S            (osi_s_queue_recv's a1)
w2c_call2:  entry a1, 32         -> bridge frame sp = S - 32
win_spill_call0: addi a1, a1,-32 -> a1 = S - 64  (call0; shares the bridge window)
switch frame base                = sleep_sp - 160        (TASK_FRAME_TOTAL)
  EPC3 at +64, EPS3 at +68, WINDOWBASE at +84, WINDOWSTART at +88
underflow reads the frame link at [a9-12], and a4..a7 from [a7-48 .. a7-20]
```

The condition the fault expresses is `a9 - 12 == (sleep_sp - 160) + 68`, i.e.
`a9 == sleep_sp - 80`.

**I have not closed it**, because `sleep_sp` depends on `task_sleep`'s own call0
frame chain below `osi_impl_park`, and I have not measured that. Guessing it
would produce a number that looks like a derivation and is not one — which is
precisely how steps 143, 144 and 148 went.

### One observation that is solid

`a9 == sleep_sp - 80` puts the frame being restored **below** the sleeping
stack pointer — inside the region the switch frame occupies. A live windowed
frame cannot be below the current sp; stacks grow down and the innermost frame
*is* the sp. So whatever `WINDOWSTART` bit drives that underflow is claiming a
window whose `a1` points **into the switch frame itself**.

That is the same shape as step 124's phantom — a bit with no real frame behind
it — reached from a completely different direction, and it is consistent with
the fault being deterministic: the switch frame is at a fixed offset, so a
phantom link into it lands on the same word every time.

### On the datasheet

The file is `DS_ESP32`, the hardware datasheet — pinout, electrical
characteristics, RF, package. It carries no instruction-set material. The
document that answers `ROTW`/`WINDOWSTART` is the **Xtensa ISA Reference
Manual**, Windowed Register Option; the ESP32 **Technical Reference Manual** has
some window material as a second source. Also worth noting for future sessions:
PDFs cannot be read on this machine — `pdftoppm` is not installed — so a text
extract is needed either way.

### Next

Measure `sleep_sp` rather than derive it — it is one value, available from the
switch frame the park itself creates, and the kernel already records saved stack
pointers per task. That closes the arithmetic with a number instead of an
assumption.

Suite unchanged: boot 11 PASS 0 FAIL, `wintorture` CORRECT, `blobphy` rc=0,
`wifiinit` no fault and no reset.

## Step 161 — the ISA answers it: `PS.EXCM`, and step 155 was backwards

`pdftotext` **is** installed even though `pdftoppm` is not, so the ISA summary
was readable after all. Two passages settle a question that has cost roughly
thirty steps across two sessions.

### 1. `ROTW` does not touch `WINDOWSTART`

```
WindowBase <- WindowBase + imm4 if Windowed Register Option
```

That is the whole operation. It is privileged, "intended for use in exception
handlers and context switch code". My recollection in step 156 was right, and
irrelevant — the danger is not what `ROTW` writes.

### 2. The overflow check fires on ANY register reference

From §6.1.3, Window Overflow Check:

```
procedure WindowCheck (wr, ws, wt)
   n <- if (wr != 2'b00 or ws != 2'b00 or wt != 2'b00)
           and WindowStart[WindowBase+1] then 2'b01
        else if (wr1 or ws1 or wt1) and WindowStart[WindowBase+2] then 2'b10
        else if (wr = 2'b11 ...) and WindowStart[WindowBase+3] then 2'b11
        else 2'b00
   if CWOE = 1 and n != 2'b00 then  PS.OWB <- WindowBase ... PS.EXCM <- 1 ...
```

with `ref()` being "1 if the register is used by the instruction". **Not
`ENTRY`. Any instruction that references a register.**

Every attempt from step 128 onward rested on the claim that "only
`wsr`/`rsr`/`movi`/`s32i`/`l32i`/`rotw` execute, so no window exception can
fire". **That claim is false.** An `s32i a12, a0, N` after a `ROTW` triggers the
check whenever `WindowStart[WindowBase+1]` is set.

### 3. And the gate is `PS.EXCM`

```
CWOE <- if PS.EXCM then 0 else PS.WOE
```

`_handler_level3` **clears `PS.EXCM` at entry** — deliberately, with its own
comment, so that a fault inside the handler reaches the panic handler rather
than the double-exception vector. That clear sits *after* the prologue and
*before* `.Lresume`.

So:

| site | `PS.EXCM` | `CWOE` | overflow check | result |
|---|---|---|---|---|
| prologue save pass | 1 | 0 | **off** | works, green since step 131 |
| every restore-path attempt | 0 | 1 | **on** | breaks, five times |

**That is the answer.** Not placement, not ordering, not the phantom. The save
pass has always run inside the EXCM window where the check is disabled, and
every restore attempt has run outside it.

### Step 155 was exactly backwards

Step 155 set `WINDOWSTART` to **all-ones** across the rotation, reasoning that
rotating onto a clear bit was the hazard. All-ones guarantees
`WindowStart[WindowBase+1]` is set at every stop, so with `CWOE = 1` **every
register reference after every `ROTW` faults.** Boot dropped from 11 to 10, and
I recorded that the hypothesis was "not supported" without seeing that I had
maximised the very condition that breaks it.

### The fix, and it is small

Set `PS.EXCM` for the duration of the rotation in the restore path, or clear
`PS.WOE` — either drives `CWOE` to 0 and disables the check, exactly as the
prologue already enjoys by accident of ordering. The ISA also notes a hazard
worth honouring: after `WSR WINDOWSTART`, an `RSYNC` is required before *any* use
or definition of an AR while `CWOE = 1`; the existing code already does that.

That makes Tier B's restore a two-instruction change away from being testable
for the first time, rather than a design that has failed five times.

### Method note

The answer was in a document nobody had opened, and it took one `pdftotext`.
Thirty steps of experiment produced a correct empirical rule — *rotating in the
restore path breaks the working case* — and no explanation. The explanation was
four pages of a reference manual.

Suite unchanged: boot 11 PASS 0 FAIL, `wintorture` CORRECT, `blobphy` rc=0,
`wifiinit` no fault and no reset.

## Step 162 — TIER B WORKS. Windowed frames survive preemption.

The two-instruction fix step 161 derived, applied: `PS.EXCM` set for the duration
of the rotation in the restore path, driving `CWOE` to 0 and disabling the
overflow check — the condition the prologue's save pass has enjoyed since step
131 purely by accident of ordering. With that, the full Tier B restore went in:
per-task slots, valid flag, grant from `frame[88]`, `g_win_union` deleted.

**Pin ON — green, first time in eleven builds:**

```
boot 11 PASS 0 FAIL   wintorture CORRECT   blobphy rc=0   wifiinit no fault
```

**Pin OFF — the test this was built for, read with its control (step 120):**

```
  60 ms : CORRECT   switches during the call: 1   (preemption really happened)
 300 ms : CORRECT   switches during the call: 3
1000 ms : CORRECT   switches during the call: 10
wincollide : ran      blobphy : rc=0
wifiinit   : no fault, no reset          -- UNPINNED
```

**Ten genuine preemptions with eight windowed frames live, checksum correct every
time.** Against step 121's baseline on the same bench:

```
*** KERNEL PANIC ***  IllegalInstruction  epc 0x6eeeeeee
frames : task 5 held 0x0000aa8a granted 0x00000008 LOST 0x0000aa82
```

Windowed frames now survive a genuine context switch on this kernel. That has
not been true at any point in this project.

### What it took, and what it did not

The mechanism was verified piecemeal long ago — the save bit-exact (129), the
restore landing all 64 registers (136), the union needing to go (132), the grant
needing to come from `frame[88]` (140). **Every one of those was correct.** What
blocked assembly for eleven builds was a single condition nobody had checked: the
window overflow check fires on any register reference, and `_handler_level3`
clears `PS.EXCM` between the prologue and the restore, so the same code was
running with the check off in one place and on in the other.

Not placement. Not ordering. Not the phantom. Not the sweep. One bit in `PS`.

### What this unlocks

The pin exists because windowed frames did not survive preemption
(UM-NATOS-038 §12.3). They do now. That makes `BLOB_PIN_DISABLE` a one-line
change backed by evidence rather than a hope — and with it, the busy-spin from
step 113 can become a real blocking wait, which is what a driver waiting on an
interrupt requires.

**The pin is left ON in this commit.** Turning it off is a behavioural change to
the shipping configuration and deserves its own step with the full suite behind
it, not a rider on the change that made it possible.

### Cost, against UM-NATOS-043's estimate

```
heap 76,400 B usable   (79,680 at session start; 3,072 Tier B + 112 metering + 96 other)
```

The 3,072 bytes are exactly as costed in §5.1. The `~2.1 µs` per switch estimate
in §5.2 remains unmeasured and should be confirmed with `xt_ccount()`.

Suite at the committed default (pin ON): boot 11 PASS 0 FAIL, `wintorture`
CORRECT, `blobphy` rc=0, `blobtx force` 0x3004, `wifiinit` no fault and no reset.

## Step 163 — the pin is off

`BLOB_PIN_DISABLE` now defaults to 1. Full suite, unpinned:

```
boot 11 PASS 0 FAIL
wintorture 1000 ms : switches during the call: 10  (preemption really happened)
                     checksum 1632 expected 1632  CORRECT
wincollide  ran        wintest ok        blobphy rc=0
blobtx force 0x3004    wifiinit no fault, no reset
```

The pin existed for exactly one reason — windowed frames did not survive
preemption, measured at UM-NATOS-038 §12.3 by disabling it and watching
`wintorture` panic. Tier B removed that reason, and this is the same measurement
run again with the opposite result.

### What it cost while it was there

It forbade preemption inside vendor code, which forced every blocking wait in the
radio path to be step 113's busy-spin: 600 ms of CPU per `_queue_recv`, and no
way to write a wait that sleeps until an interrupt arrives. That constraint has
shaped this file since step 106.

### Kept as a switch, not deleted

`BLOB_PIN_DISABLE` stays. It is the control for every windowed measurement in
this log, and step 121's baseline — `LOST 0x0000aa82`, the immediate
`IllegalInstruction` — is only reproducible with it back at 0. Deleting it would
make the strongest before/after comparison in the investigation unreproducible.

### What is now possible that was not

Step 113's spin can become a real block. Step 157 tried exactly that and got the
original `_WindowUnderflow8` fault back — but that attempt ran **with the pin on
and without Tier B's restore**, which is to say under the conditions that made it
impossible. It deserves retrying now that neither holds.

Beyond that, UM-NATOS-042 §9.5's list is unchanged and is what stands between
here and a working radio: interrupts were never wired, `_task_delay` returns
immediately, the timer entries are stubs, event callbacks never fire, and there
is no data path above the MAC.

## Step 164 — the park fault is independent of Tier B and of the pin

Step 163 said step 157's park deserved retrying, because it had run "with the
pin ON and without Tier B's restore, i.e. under the conditions that made it
impossible". Retried with both conditions removed:

```
exccause 28   epc 0x400800d5   excvaddr 0x00060500
```

**Identical to steps 157 and 159.** Same handler, same instruction, same
PS-shaped value. `wintorture` CORRECT and `blobphy` rc=0 alongside it, so nothing
else regressed.

### What that settles

The park's fault is **not** a register-preservation problem. It survives the
mechanism that makes windowed frames survive preemption, and it survives removing
the pin. Those were the two things this file has spent since step 106 arranging,
and neither touches it.

My step 163 framing was wrong: I described the pin and the missing restore as
"the conditions that made it impossible". They were conditions that made it
*untestable in isolation*. They were not the cause.

So the park has its own defect, and it is now cleanly separated from everything
Tier B addressed. Three facts about it, all measured:

- **deterministic** — identical across sleep durations 1, 4 and 16 (step 159);
- **`a7` comes back holding a PS value**, `0x00060520`, where a caller's stack
  pointer belongs (step 159);
- **the implied frame link points below the sleeping stack pointer**, into the
  switch frame's own region — `a9 == sleep_sp - 80` from step 160's geometry,
  and a live windowed frame cannot be below the current sp.

That last one is the shape of a phantom `WINDOWSTART` bit, reached independently
of step 124's, and it explains the determinism: the switch frame sits at a fixed
offset, so a bad link into it lands on the same word every time.

### Where that leaves the radio

Tier B and the unpinned scheduler are real and they hold — `wintorture` survives
ten genuine preemptions with eight frames live. What they do **not** do is make
the blob's blocking wait work, and step 113's busy-spin remains the only version
of that wait which runs.

The next question is the one step 160 left open and did not close: **measure
`sleep_sp`** rather than derive it, and confirm whether `a9` really resolves into
the switch frame. That closes the arithmetic with a number, and it needs no new
probe on the blocking path — the park creates the switch frame that holds the
value, and the kernel already records saved stack pointers per task.

Reverted; the spin stands. Suite: boot 11 PASS 0 FAIL, `wintorture` CORRECT,
`blobphy` rc=0, `wifiinit` no fault and no reset, pin off.

## Step 165 — measured: the machine is in an invalid window state, and `a3` holds a PS

Step 160's arithmetic closed with numbers instead of assumptions. The park's own
switch frame is the blob task's saved sp, which the panic dump already prints —
no probe added to the blocking path.

```
task 9 sp 0x3ffb26e0  stack 0x3ffb0d18+7168  win 0x00000008@3
saved frame @ 0x3ffb26e0: 0x40089a92 0x00000000 0x00060520 0xfffffff0 ...
                            [0]=a0     [1]=a2     [2]=a3
saved ctl   @ ...         0x40088b49 0x00060520 ...        <- EPC3, EPS3
excvaddr 0x00060500  ->  a7 = 0x00060520
windowbase: 1   windowstart: 0x00000008   bit(base) CLEAR
```

### Two findings, and the second is the stronger

**`a3` holds a PS.** Frame word 2 is offset 8 — `a3` — and it is `0x00060520`,
**the same value as `EPS3`** two lines below, and the same value `a7` came back
holding. Three appearances of one processor-state word where a stack pointer and
a general register belong.

**`bit(base) CLEAR`.** `WINDOWBASE` is 1, `WINDOWSTART` is `0x00000008` — bit 3
only. The bit at the current base is **not set**, and the panic dump has a label
for exactly this because it is architecturally invalid: §6.1.2 says a
`WINDOWSTART` bit "is set if those four registers are AR[0] to AR[3] for some
call", and the current window is by definition such a call.

Task 9 was *saved* as `win 0x00000008@3` — base 3, bit 3, consistent. At the
fault the base is 1 with bit 3 still the only one set. **Something moved
`WINDOWBASE` without moving `WINDOWSTART` with it.**

`ROTW` is precisely the instruction that does that (step 161: it adds to
`WindowBase` and touches nothing else), and Tier B's restore contains four of
them. They are meant to wrap — 4 × `rotw 4` = +16 = identity on a 4-bit
`WINDOWBASE` — so a net displacement of 2 means the sequence did not complete as
written, or something else rotated.

### What that makes the next step

Not another hypothesis: **read `WINDOWBASE` before and after the restore's
rotation sequence.** Two `rsr.windowbase` into globals, in the prologue-safe
style, and the dump says whether the four `ROTW`s are net-zero. If they are, the
displacement comes from elsewhere — the underflow handler's own `rfwu`, or the
H1 defer path — and that is a different search. If they are not, Tier B's restore
has an exit path that skips rotations, and the guards are the place to look.

This is worth stating plainly: **Tier B works for `wintorture` and may still be
leaving the window state inconsistent on the park path.** Those are compatible —
`wintorture` never parks — and the invalid state is measured, not inferred.

Reverted; the spin stands. Suite: boot 11 PASS 0 FAIL, `wintorture` CORRECT,
`blobphy` rc=0, `wifiinit` no fault and no reset, pin off.

## Step 166 — the rotation is identity, and step 165's "invalid state" was not one

Captured `WINDOWBASE` either side of Tier B's rotation, inside the `PS.EXCM`
window so the capture itself cannot trip the overflow check:

```
rotw net : pre 3 post 3  identity
```

**The four `rotw 4` are net-zero, as designed.** Tier B's restore displaces
nothing. That eliminates the mechanism step 165 proposed.

### And the premise was wrong too

Step 165 read `windowbase: 1  windowstart: 0x00000008  bit(base) CLEAR` as "an
architecturally invalid state", reasoning from ISA §6.1.2 that the bit at the
current base must be set.

That is true of ordinary task context and **false of a window handler**, which is
where this dump is taken — the fault is inside `_WindowUnderflow8`, and an
underflow handler runs with the window rotated and the bit not yet set. The base
of 1 is the handler's, not the task's.

UM-NATOS-042 §5 already lists this, in the table of things positively eliminated:

> `bit(base) CLEAR` is an anomaly | **no** | it is the normal state inside an
> underflow handler

**Third time this session I have re-raised something the log had already
settled** — step 156 was step 14's finding, and this is §5's. The pattern is
specific enough to name: when a dump line looks anomalous, the elimination table
in UM-NATOS-042 §5 should be read *before* building on it, not after. It exists
for exactly this and costs one minute.

### What survives from step 165

One thing, and it is still unexplained: **`a3` holds `0x00060520`, which is also
`EPS3`, and also the value `a7` came back with.** Three appearances of one
processor-state word — in a general register, in the saved control block, and in
the pointer the underflow loaded through. That is not a normal state and no
elimination covers it.

### Where the park investigation actually stands

Measured and holding: the fault is deterministic across sleep durations (159);
`a7` returns a PS (159); Tier B's rotation is identity (this step); the fault is
independent of Tier B and of the pin (164).

Eliminated: rotation displacement (this step); `bit(base)` anomaly (§5, re-
confirmed); a race or timing window (159).

Open: how a PS reaches `a3`. That is now the single thread, and it is narrow —
`EPS3` is written at exactly one place in the prologue (`rsr.eps3 a2; s32i a2,
a1, 68`) and read at exactly one place in the epilogue. Whatever puts it in `a3`
is between those.

Reverted; the spin stands. Suite: boot 11 PASS 0 FAIL, `wintorture` CORRECT,
`blobphy` rc=0, `wifiinit` no fault and no reset, pin off.

## Step 167 — the dump decoded, and `a3` holding a PS is probably innocent

Verified what the panic dump actually prints, before building anything on it:

```
saved frame : words 0..7   -> a0, a2, a3, a4, a5, a6, a7, a8
saved hi    : words 8..15  -> a9..a15, SAR
saved ctl   : words 15..20 -> SAR, EPC3, EPS3, LBEG, LEND, LCOUNT
```

Both step 165 readings were correct: `a3 = 0x00060520` and `EPS3 = 0x00060520`.

### But "three appearances of one PS" overstates it

`a3` holding a processor-state word is not by itself anomalous. Two candidates
were checked rather than assumed:

- `crit_enter()` returns `ps & 0xFu` — four bits only, so **not** this;
- the windowed helpers do `__asm__ volatile ("rsil %0, 3" : "=r"(ps))`, and
  `RSIL` returns the **full previous PS**. `osi_qpoll_w`, `blob_trylock_w` and
  `blob_unlock_w` all use it, and all run immediately before the park.

So a live register holding `0x0006xxxx` on that path is **expected**, and `a3`
being one of them is ordinary register allocation. Step 165 presented it as a
third corroborating sighting; it is better read as the normal state of code that
has just masked interrupts.

That leaves the anomaly narrower and sharper: **`a7`**. `a7` is the caller's
stack pointer recovered by `_WindowUnderflow8` from `[a9-12]`, and it came back
as a PS. A register holding a PS is unremarkable; a **frame link** holding one is
the defect.

### Corrected position

| claim | status |
|---|---|
| `a3` holds a PS | true, and probably legitimate — `RSIL` returns one |
| `EPS3` holds the same value | true, and unsurprising if the task was interrupted at that PS |
| `a7` holds a PS where a frame link belongs | **the actual defect, unexplained** |
| rotation displacement | eliminated (166) |
| `bit(base)` anomaly | eliminated (§5, re-confirmed 166) |
| race / timing | eliminated (159) |

The question is therefore not "how does a PS get into a register" — that has an
ordinary answer — but **"how does a save area come to hold one where a stack
pointer should be"**. Given `a3` legitimately carries a PS on this path, the most
economical account is that a save area is being written from the wrong register,
or read at the wrong offset, such that `a3`'s value lands where `a1`'s belongs.
Both are checkable against the handler's store list rather than by experiment.

## Step 169 — nothing writes the PS; the save area was never written at all

Dumped the memory around the save area the underflow read. Safe here: the panic
path, not the blocking path, so the read cannot perturb what it measures.

```
uf window : a9 0x3ffb27d0
0x000008ef 0x3ffb27f0 0x3ffb0000 0x4008caf2  [0x000008ef 0x3ffb2790 0x8008e6f0 0x4008d10e] ...
                                              a0         a1         a2         a3
```

### First, a correction to the trace

`_WindowUnderflow8` goes one level further than the last four steps assumed:

```asm
l32e a0, a9, -16      -> 0x000008ef
l32e a1, a9, -12      -> 0x3ffb2790          <- the PARENT's sp
l32e a2, a9,  -8
l32e a7, a1, -12      -> [0x3ffb2784]        <- a7 comes from HERE
l32e a4, a7, -32      -> faults
```

`a7` is not `[a9-12]`. It is `[a1-12]`, one link up. So the PS lives at
**`0x3ffb2784`**, inside the base save area of the frame at `0x3ffb2790`
(`[0x3ffb2780 .. 0x3ffb278c]`) — not in the save area at `a9` at all.

Steps 165 through 168 all reasoned about the wrong word. The arithmetic in step
160 and the "a9 relative to base" figure in step 168 were computed against a
link the fault never loaded through.

### And the finding that follows

**Nothing wrote a PS there.** Xtensa fills base save areas **lazily** — the
overflow handler writes them when a frame is actually spilled, and `ENTRY` writes
nothing (established at step 123, forgotten at 149, and load-bearing here).

So `[0x3ffb2784]` holds whatever last occupied that stack address. A switch frame
with `EPS3` at `0x3ffb2784` has its base at `0x3ffb2740` and spans
`[0x3ffb2740, 0x3ffb27e0)` — a preemption taken when the task's sp was
`0x3ffb27e0`, which is squarely on the park's own descent. The bytes are a **dead
switch frame's `EPS3` slot**, sitting in memory a later windowed frame's save
area now nominally occupies.

The PS was never the defect. It is debris, and its shape only ever said *which*
debris.

### What the defect actually is

**An underflow fired for a frame whose save area was never written.**

`retw` underflows only when the parent's `WINDOWSTART` bit is clear, and a bit is
cleared by `rfwo` — after the overflow handler has written the save area. A bit
that is clear *without* the save area having been written is the same object step
126 named: a claim with nothing behind it.

That reframes the search from "who writes a PS" — nobody — to **"which bit was
cleared without a spill"**, and it puts this back on the same defect the sweep
work circled from step 124 onward, now reached from the park path with a specific
address in hand.

The park's spill is `win_spill_call0()` inside `osi_impl_park()`. Step 112's rule
applies to it directly: anything created **below** the spill point is outside the
range the spill covered. `task_sleep()` runs after that spill and builds call0
frames, but the bridge's own windowed frame is above it — so which frames the
spill actually reached is the next thing to read, and `win_spill_all`'s walk is
already instrumented from step 112.

Reverted; the spin stands. Suite: boot 11 PASS 0 FAIL, `wintorture` CORRECT,
`blobphy` rc=0, `wifiinit` no fault and no reset, pin off.

## Step 170 — the overlap is measured and exact; the obvious fix regresses

### The measurement

Probed what the park's spill covered — call0 code, special-register reads and
global stores only, none of the shapes that perturbed steps 152–158 — and checked
the fault signature was unmoved before believing any of it:

```
park spill : ws 0x0000000a -> 0x00000008   wb 3   sp 0x3ffb27c0
signature  : epc 0x400800d5  excvaddr 0x00060500   UNCHANGED
```

The spill cleared bit 1 (the stub's frame) and kept bit 3 (the bridge's), which
is correct. So the bridge's `retw` underflows the stub's frame, and reads its
save area at `[bridge_sp-16 .. bridge_sp)` = **`[0x3ffb27c0, 0x3ffb27d0)`**.

`osi_impl_park`'s call0 frame has `a1 = 0x3ffb27c0`. In call0, `a1` is the lowest
address of the frame, so that frame is `[0x3ffb27c0, 0x3ffb27d0)`.

**Exactly coincident.** The call0 callee's frame *is* the caller's save area.

### And it is an asymmetry between the bridges

```
win_spill_call0 :  addi a1, a1, -32   before its work
rom_call3       :  addi a1, a1, -48
w2c_call0f/1/2/3:  entry a1, 32  ...  callx0 a8      <- no reservation
```

`entry a1, 32` puts the frame at `[a1, a1+32)` and the caller's save area
*below* it, in what is otherwise free stack. Two bridges reserve that space and
four do not. `win_spill_call0`'s own comment describes this class of bug and
protects against it; the `w2c_*` bridges were written from `rom_call3`'s shape
and inherited the gap — the same inheritance step 37 records for `phy_stack_call`.

That also explains the shape of the whole investigation: **the step-113 spin makes
no call0 call after the spill**, so nothing allocates there and nothing is
overwritten. Every attempt at a real blocking wait does make one.

### The obvious fix regresses

Adding `addi a1, a1, -32` / `+32` around all four `callx0` sites **breaks
`wifiinit`, which was clean**:

```
exccause 28   epc 0x400800cf   DOUBLE EXCEPTION   excvaddr 0x000000f3
epc1 0x4008d176   windowbase 11   windowstart 0x00002000
```

A different fault, in `_WindowUnderflow8 + 0x0f`, on the path the spin normally
survives. Reverted; suite green.

So the analysis is measured and the remedy is not simply "reserve the space".
Something else depends on the bridge's `a1` during the `callx0` — the candidates
are the `[a1+0]` slot diagnostics the bridges write (steps 79, 82), a callee that
reads through the caller's sp, or stack depth on a nested crossing. **Not
diagnosed, and recorded as not diagnosed.**

### Status

The overlap is the first mechanism in this investigation that explains *both*
why the spin works and why every blocking wait fails, and it is arithmetic rather
than inference — two addresses that coincide exactly. That is worth more than the
failed remedy costs.

Suite: boot 11 PASS 0 FAIL, `wintorture` CORRECT, `blobphy` rc=0, `wifiinit` no
fault and no reset, pin off.

## Step 171/172 — the regression is diagnosed; the park fault is not

Step 170 left one question — why did reserving the caller's save area regress a
working case — and one claim, that the overlap explains the park fault. The first
is answered. The second is now doubtful.

### Why the reservation regressed: the ISA says so, under MOVSP

Applied to `w2c_call2` alone (the bridge the park uses), the park's fault
**moved**, from `_WindowUnderflow8 + 0x15` to `+0x0f`:

```
+0x0f : l32e a7, a1, -12      excvaddr 0x000000f3  ->  a1 = 0x000000ff
```

`a1` is loaded from `[a9-12]` and came back as small garbage rather than the
`0x3ffb2790` it held before. Different debris, same unwritten save area.

The reason the approach cannot work is stated outright in the ISA, under `MOVSP`:

> The Xtensa Windowed Register ABI specifies that **some of the caller's
> registers may be stored just below the callee's stack pointer. When the stack
> frame is extended, these values may need to be moved.** Under the Windowed
> Register Option this is handled by raising an **Alloca exception** so that the
> registers can be moved with interrupts and exceptions disabled.

**`a1` is not a free register — it is the address the save area is defined
against.** Moving it with a plain `addi` relocates the definition and leaves the
data behind, so the spill wrote `[moved_a1-16]` and the `retw` read
`[orig_a1-16]`, 32 bytes apart. `MOVSP` exists precisely to make that move
atomic, and this kernel has no Alloca handler.

So the reservation is not a fix that needs tuning. **It is the wrong instrument**,
and step 170's "not diagnosed" is now diagnosed.

### And the overlap is not the park fault

With the reservation removed and `win_spill_call0()` taken out of the park
entirely — no spill at all, on the theory that Tier B has made it unnecessary —
the fault returns exactly as before:

```
exccause 28   epc 0x400800d5   excvaddr 0x00060500
```

Identical with the spill and without it. **So the spill is not implicated, and
the save-area overlap step 170 measured is not what produces this fault.** The
overlap is real — the arithmetic is exact — but it is a latent defect the park
happens to sit near, not the cause of the park failing.

That corrects step 170's closing claim that the overlap explains both why the
spin works and why every blocking wait fails. On this evidence it explains
neither.

### Where the park fault now stands

Eliminated, each by measurement rather than argument: rotation displacement
(166), `bit(base)` as an anomaly (§5), timing and races (159), Tier B and the pin
(164), the spill (172), and the save-area overlap (172).

Still true: deterministic; `a7` recovered as a PS; the save area it reads was
never written (169); the PS in it is debris from a dead switch frame.

What has not been asked: **why an underflow fires at all.** `retw` underflows
only when the caller's `WINDOWSTART` bit is clear. With no spill in the park, the
stub's frame should never have been spilled, its bit should still be set, and no
underflow should occur on the bridge's return. One does. **Which bit is clear,
and who cleared it, is the question** — and it is the same one step 126 reached
and named, now from a fourth direction.

Reverted; suite green with the pin off.

## Step 173 — the missing-ENTRY hypothesis: right about the mechanism, wrong about the fix

Prompted from outside: is the park failing because some window has no `ENTRY`,
so its `WINDOWSTART` bit was never set the normal way and its save area was never
written?

That is a good shape for the evidence. `ENTRY` is what sets a bit
(`WindowStart[WindowBase] <- 1`, ISA 8.3.106) and an overflow is what writes the
save area below it. A frame that got its bit some other way has one and not the
other — which is exactly step 169's finding, and exactly step 126's phantom.

**And a task's initial frame is such a frame.** Step 142 gave it a bit
(`frame[WSTART] = 1 << wb`); no `ENTRY` ever ran for it. The ISA has a rule for
precisely this, in the thread-startup section:

> the base save area at `sp - n` … must be initialized as if it had been written
> by a window overflow … The return address register (a0) for the first procedure
> on the stack must be explicitly set to zero.

### What was found

`task_create` does write a chain terminator, but at `[term_top-16, term_top)`,
and the task's initial `sp` is `top = term_top - 16`. So the terminator sits at
`[sp, sp+16)` — the task's own locals region — while an underflow below that
frame would read `[sp-16, sp)`.

Moving it to `[sp-16, sp)` builds clean, regresses nothing, and **does not fix
the park**: `epc 0x400800d5`, `excvaddr 0x00060500`, unchanged.

### Why it was reverted rather than kept

The justification does not survive checking. The outermost windowed function's
`entry a1, N` sets its sp to `top - N`, so the *initial context's* registers
spill to `[top-N-16]` — and **N is not known at task creation.** Neither the old
address nor the new one is "the ABI's base save area"; the location depends on
the first callee's frame size.

nat-os's terminator is a different construct: a fixed high sentinel carrying a
return encoding to `task_chain_end`, paired with the chain-ascends check in the
walks. That is a pragmatic design, not the ISA's startup rule, and moving it on
a misreading of the ISA would have changed task-creation semantics for no
measured benefit.

**Unverified change, shaky justification, no effect — reverted.** The temptation
to keep it because it "looks more correct" is the same one that has produced
three findings in this log that were later withdrawn.

### What the hypothesis is still worth

The mechanism it proposes remains the best available account: **a bit set without
an `ENTRY`, or cleared without an overflow, produces exactly the observed
fault.** What has not been found is which bit, and the initial frame is now
eliminated as the candidate — the park's fault is unchanged whether that area is
initialized or not.

The remaining places a bit can be set or cleared outside `ENTRY`/`RETW` are
enumerable and short: `task_create` (eliminated here), the restore's
`wsr.windowstart` in `_handler_level3`, the three wipes in `window.S`, and
`rfwo`/`rfwu`. Four of the five are already instrumented.

Reverted; suite green with the pin off.

## Step 174/176 — THE BLOCKING WAIT WORKS. The spill was the defect.

### How it was found

Tagged every site that writes `WINDOWSTART` outside `ENTRY`/`RETW` — the restore,
`x20_windowed`'s wipe, and `phy_stack_call`'s two — with a counter and the value
each last wrote. At the park fault:

```
ws write : restore  n=6339  last=0x00000008     <- ONE bit
ws write : x20wipe  n=0
ws write : phypre   n=1  last=0x00000002
ws write : phypost  n=1  last=0x00000002
```

The restore was the last writer and wrote a single bit, so the task had been
**saved holding one frame** — something had spilled the stub's and the bridge's
frames during the park. With the explicit spill removed (step 172), nothing
should have.

`task_sleep()` calls `spill_before_parking()` as its first act.

### Two invalid tests, both mine

Step 172 removed `win_spill_call0()` from `osi_impl_park` and concluded "the
spill is not implicated". **`task_sleep` spilled anyway.**

Step 175 then called `task_sleep_armed()` directly to skip that. **It reaches
`task_yield()`, which also spills.** The fault changed to a stack guard overrun,
which looked like progress and was the blob simply running longer before hitting
the same thing.

All three parking primitives spill: `task_block`, `task_sleep`, `task_yield`.
Disabling `spill_before_parking()` at its single definition is the first test
that actually removes it.

### The result

```
wifiinit : NO FAULT, NO RESET
[qr] budget spent, still waiting  calls=4 timeouts=3 rounds/wait=24
```

`rounds/wait` was **191** with step 113's busy-spin. It is 24 now, because the
wait is a real sleep. **The blob's blocking wait no longer burns the CPU**, and
the radio can wait on something rather than counting cycles.

Full suite, unpinned: boot 11 PASS 0 FAIL; `wintorture 1000 ms` with **10 genuine
preemptions** and checksum CORRECT; `wincollide` ran; `wintest` ok; `blobphy`
rc=0; `blobtx force` 0x3004. Heap 76,368 B. The 12 KB blob stack tried at step
175 was **not** needed — the overrun belonged to the broken intermediate and 7 KB
stands.

### Why the spill was harmful

Step 170's overlap was the mechanism all along, and step 172's refutation of it
was reached from an invalid test.

Every parking primitive calls the spill, so on the blob's path it runs **inside a
call0 callee entered from a windowed bridge**. That callee's frame sits at
`[bridge_sp-16, bridge_sp)` — exactly where the ABI keeps the bridge's caller's
base save area — and the spill writes the stub's `a0..a3` into it. They overwrite
each other, and the bridge's `retw` underflows into the wreckage.

Measured at step 170: park `a1 = 0x3ffb27c0` against a save area of
`[0x3ffb27c0, 0x3ffb27d0)`. Exactly coincident.

And it explains step 113's spin: it makes no call0 call after the spill, so
nothing is ever allocated over the save area.

### Why removing it is correct, not a workaround

`spill_before_parking()` exists because windowed frames did not survive
preemption (UM-NATOS-038 §12.3). **Tier B made them survive** — step 163 measured
ten genuine preemptions with eight frames live. Reducing a task to one frame
before it parks is no longer buying anything, and on this path it was costing
correctness.

Kept: the park (`osi_impl_park` via `w2c_call2`), the disabled spill, and the
`WINDOWSTART` writer tagging, which earned its place by producing this in one
run.

**Nothing has been on air.**
---

## step 177 — the interrupts, wired

`_set_intr`, `_set_isr`, `_ints_on` and `_ints_off` have counted their calls and
done nothing since the table was written. They now reach the kernel.

- `kernel/wifi_osi_impl.c`: 32 per-line call0 trampolines (`blob_isr_0..31`) and
  a `blob_isr_t g_blob_isr[32]` table. A trampoline looks up the blob's handler
  for its line and calls it with **`rom_call4`**, not `rom_call3` — `rom_call3`
  takes the blob mutex, and a mutex cannot be taken from an ISR
  (UM-NATOS-042 §2.4).
- `vendor/windowed/wifi_osi_stubs.c`: the four stubs bridge out through
  `w2c_call1`/`w2c_call3`.
- The kernel side already existed: `intr_route(source, line, fn)`,
  `intr_dispatch()`, `xt_enable_interrupt()`, and the reserved
  `INTR_SRC_WIFI_MAC 0` / `INTR_LINE_WIFI_MAC 27`.

Green: boot 11 PASS 0 FAIL, `wintorture` 10 real switches checksum CORRECT,
`blobphy` rc=0.

And inert: **`[intr] routed=0`**. The blob never calls `_set_intr`, because it
never gets that far. So the trace was printed instead — the OSI call numbers, in
order, from the live `[qr]` block, since `wifiinit` does not return and `osiused`
cannot be reached:

```
[osi] 19 41 21 87 41 22 8 19 85 42 93 13 42 36 15 16 29 29 29 29
```

```
 1. 19  recursive_mutex_create      9. 85  malloc_internal
 2. 41  task_get_current_task      10. 42  task_get_max_priority
 3. 21  mutex_lock                 11. 93  wifi_create_queue
 4. 87  calloc_internal            12. 13  semphr_create
 5. 41  task_get_current_task      13. 42  task_get_max_priority
 6. 22  mutex_unlock               14. 36  task_create_pinned_to_core
 7.  8  spin_lock_create           15. 15  semphr_take
 8. 19  recursive_mutex_create     16. 16  semphr_give
                                   17. 29  queue_recv, forever
```

Sixteen setup calls, then the driver creates its worker task and waits on a queue
for that worker to signal. It waits for ever. **That is the whole stall**, and it
is one call further on than the report says.

An earlier mapping of these numbers, taken from the order of the fields in the
OSI struct, was misaligned and was discarded; the numbers above are `osi_hit(N)`
arguments read out of the stub bodies.

---

## step 178 — who was refused, and two tools that lied

### 178a. The blob task is created

`wifiinit task` and plain `wifiinit` produced byte-identical traces, which looked
like the `task` flag not taking. It is not visible from the trace either way:
`osi_hit(36)` fires at the top of `osi_s_task_create_pinned_to_core`, **before**
`blob_task_create()` decides anything, so a refused create and a successful one
are indistinguishable there.

`blobcall.c` already maintained the three counters that separate them. Reporting
them costs nothing new — no read is added to the blocking path, which is step
167's rule:

```
[bt] created=1 refused=0 want_stack=6656
```

So the task exists, and it took the big stack (6,656 B requested, 7,168 B
available in `g_blob_stack`; a nat-os task stack is 2,048 B and would have been
refused). Task 9 appears in the panic dump at `0x3ffb0d18+7168`.

The stall is therefore **not** a refused task creation. The worker exists and the
driver still waits for it.

### 178b. Two tools that lied, and neither was the board

Most of this step went on instrumentation that was not the kernel's.

**`build.ps1` without `-Flash` does not flash.** It builds, packages, prints
`image: 123,040 bytes`, and exits 0. Read as a flash, it means every subsequent
measurement exercises the previous image. Same shape as step 148 — the exit code
was right, the reading was wrong.

**`send_cmd.py` wedges on `wifiinit`.** Its capture loop is
`while time.time() < deadline: ser.read(4096)`. Asking for 4,096 bytes from a
port that has none never returned on this host, so a 35-second capture ran past
90 seconds and printed nothing — for `wifiinit`, and only for `wifiinit`, because
that is the one command that leaves long silent gaps. `mem` and `help` kept the
stream busy enough to hide it.

The symptom — zero bytes, not one byte of garbage, and a port that stops
accepting writes — reads exactly like a board that has died on the command. It
had not. Reading `ser.in_waiting` first and only asking for what is there
produced 1,051 bytes of perfectly ordinary output on the first attempt.

Two builds were spent on an A/B against this: the `[bt]` line was reverted and
reflashed on the suspicion it had shifted the flash layout into the step-7 band.
It had not — the step-177 image behaved identically, which is what said the fault
was in the reading rather than the image. A third went to `-WiFi`, which is the
wrong flag for this work: it links the blob rather than loading it, and leaves
8,048 B of heap against the ~76 KB every report in this series quotes.

*Do not read silence as a result.* A capture that returns nothing is a claim
about the tool until the tool has been shown to be able to return something.

### 178c. Releasing the blob mutex across the park — tried, wrong, informative

`blob_task_entry()` takes `blob_lock()` before it may run any windowed code.
`esp_wifi_init_internal` runs under `blob_call()`, which holds that mutex. If the
mutex were held across the wait, the worker could never start and the waiter
could never be signalled — a clean deadlock, and it fits every symptom.

Tried: `osi_impl_park()` releasing with `blob_unlock_only()` before
`task_sleep_armed()` and re-taking with a new `blob_lock_only()` after, arming
the wake first so a signal landing in the gap is not lost.

It changed the outcome sharply. Task 9 reached `win 0x00000008@3` — the blob's
worker ran windowed code with three frames live, which it had never done — and
the run ended in a new fault rather than a silent wait:

```
exccause 20  InstFetchProhibited   epc 0x00000000
a0 0x8008ea84 -> osi_s_queue_recv       sp 0x00000000
GRANT DRIFT: task.c predicted 0x00002000, vectors.S wrote 0x00002800 for task 5
```

**And the premise is wrong.** `osi_s_queue_recv` already releases the lock on
every iteration of its wait loop — `blob_unlock_w(me)` at the top, `w2c_call2` to
the park, `blob_trylock_w(me)` on the way back (steps 111, 113). It is released
by design, in windowed code, by TRY, precisely so no call0 excursion can block.
So `blob_unlock_only()` released a mutex the caller had already released, and
`blob_lock_only()` left it held where the loop's own bookkeeping expects it free.

Reverted. What survives is a fact worth more than the patch: the difference
between that version and this one is that the waiter **blocked on the mutex**
instead of retrying a trylock, and under blocking acquisition the worker task ran
for the first time. The next question is whether the trylock retry can win the
race at all — not whether the lock is released, which it is.

`epc 0x00000000` with `a0` inside `osi_s_queue_recv` is not yet explained and
should not be quoted as "the next missing stub" until it is; it is equally
consistent with a windowed return through a frame whose `a1` had become 0.

### What is committed

The interrupt wiring (177) and the `[bt]` counters (178a). Both are inert with
respect to the stall and both are green:

```
boot 11 PASS 0 FAIL
wintorture 1000 ms : switches during the call: 10  (preemption really happened)
                     checksum 1632 expected 1632  CORRECT
blobphy rc=0
wifiinit : no fault, no reset
[qr] calls=4 timeouts=3 lastosi=29 rounds/wait=23 osin=20
[intr] routed=0 nofn=0 fired:
[bt]  created=1 refused=0 want_stack=6656
```

Not committed: the park change of 178c.

**Nothing has been on air.**
---

## step 179 — checked against Espressif's own headers, and the stall named

Two sources arrived: a PDF (`ESP32_WiFi.pdf`, the ESP-WROOM-32 module datasheet)
and, found while looking for it, **ESP-IDF's actual headers and
`esp_adapter.c`**, already on this machine under the PlatformIO trees.

The datasheet is hardware only — pins, strapping, crystal, RF power, reflow,
schematics. Grepped for interrupt/window/mutex/queue/ISR/DPORT/cache/RTOS; one
line confirms the blob targets FreeRTOS + LwIP and nothing else applies. **No
bearing on any open bug.** Recorded so nobody reads it again for this purpose.

The IDF sources are a different matter, and four things that had been assumed for
177 steps are now checked rather than assumed.

### 179a. What the headers confirm

| checked against | result |
|---|---|
| `wifi_os_adapter.h` v0x00000008 | **the OSI table is exact** |
| `soc.h` `ETS_WIFI_MAC_INTR_SOURCE` | **`INTR_SRC_WIFI_MAC 0` correct** |
| `wifi.h` `wifi_static_queue_t` | **`_wifi_create_queue` shape correct** |
| `esp_adapter.c` `set_intr_wrapper` | **signature matches, 4 args** |

The table check is worth stating precisely, because table drift has been a
standing suspicion. Diffing nat-os's 118 entries against IDF's function-pointer
list gives exactly two differences: `_version` at the front and `_magic` at the
back. 118 = 116 + 2. Order identical, no gaps, no reordering. `_version` and
`_magic` both match (`0x8`, `0xDEADBEAF`). **Table misalignment is eliminated as
a cause of anything.**

`_wifi_create_queue` was the best remaining candidate before this: IDF requires a
`wifi_static_queue_t { handle, storage }` and the blob dereferences `->handle`,
so returning a bare handle would hand it garbage and produce an eternal wait.
nat-os already allocates the 8 bytes and fills them correctly. Eliminated.

**Correcting step 177's write-up.** It recorded `INTR_LINE_WIFI_MAC 27` against
IDF's `ETS_WMAC_INUM = 0` as a divergence that might leave us listening on the
wrong line. `esp_adapter.c` settles it:

```c
static void set_intr_wrapper(int32_t cpu_no, uint32_t intr_source,
                             uint32_t intr_num, int32_t intr_prio)
{ esp_rom_route_intr_matrix(cpu_no, intr_source, intr_num); }
```

**The blob supplies `intr_num`.** nat-os's `osi_s_set_intr` passes it straight
through to `osi_impl_set_intr`; the `27` constant is a reservation for the
routing path, not a line forced on the blob. The concern was real in shape and
wrong in fact.

Also noted, not acted on: `osi_hit(N)` numbers are author-assigned IDs, **not**
struct indices — `osi_hit(85)` sits in `_malloc_internal`, which is IDF field 88.
The trace readings are right; the numbers must not be used to reason about
layout. And `osi_s_queue_create` is a bare `return 0` stub. The blob has not
called it, but a NULL queue would be a silent eternal wait if it ever does.

### 179b. Who is actually waiting — the trace Tortoise asked for

Three instruments, all recording values already in memory at the point of
recording, so none adds a read to the blocking path (step 167):

- the calling task for **every** OSI call, from `g_pinned`, one store in
  `osi_trace()`;
- every queue handle handed to the blob, and the caller and handle of every
  `_queue_recv`;
- `reached`/`running`/`returned` around the worker's `blob_lock()`.

```
[who] 5 5 5 5 5 5 5 5 5 5 5 5 5 5 5  9  9  9  9  9
[osi] 19 41 21 87 41 22 8 19 85 42 93 13 42 36 15 16 29 29 29 29
[bt]  created=1 refused=0 want_stack=6656
[wrk] reached=1 running=1 returned=0   mutex owner=9 acq=76 cont=2 err=0
[q]   made: via96=0x05100000   recv: t9@0x05100000 x4
[sem] blocking_take done=1 rc=1 relocked=0
```

That is the whole answer, and almost none of it is what step 178 supposed.

**There is no deadlock.** `reached=1 running=1` — the worker took the blob lock
and is running the blob's worker function. Step 178c's account is dead.

**The worker is not the stalled party.** All four `_queue_recv` calls are
**task 9**, on `0x05100000` — the one queue `_wifi_create_queue` ever made. In
IDF that is the WiFi task's event queue, and its messages come from
`_queue_send_from_isr` in the MAC ISR. An empty queue there is the **normal idle
state**, not a fault. Nothing is supposed to send it a message yet.

**The stalled party is the init task.** Task 5's last OSI call is 15,
`_semphr_take`; task 9's first is 16, `_semphr_give`. That is the standard
startup handshake — init creates the worker and waits for it to signal that it
is up — and it **worked**: `[sem] done=1 rc=1`. The semaphore was taken
successfully.

`relocked=0` says where task 5 then went. The tail of `osi_s_semphr_take` is:

```c
(void)w2c_call0f((uint32_t)&blob_unlock);
int32_t r = osi_impl_sem_take(semphr, block_time_tick);   /* returned 1 */
(void)w2c_call0f((uint32_t)&blob_lock);                   /* never returned */
```

**Task 5 is blocked re-acquiring the blob mutex**, which task 9 holds —
`owner=9`, and `err=0` proves task 9 never once lost it.

### 179c. The defect: a convoy on the blob mutex

`osi_s_queue_recv`'s wait loop is, every round:

```
blob_unlock_w(me)  ->  blob_wake_waiters(owed)  ->  park 1 tick  ->  blob_trylock_w(me)
```

The release is real and the hand-off is real: `blob_unlock_w` returns the waiter
bitmask and `blob_wake_waiters` calls `task_unblock` on each. But the re-acquire
is a **trylock that succeeds unconditionally whenever the owner is FREE**. It
does not defer to the waiter it just woke, and the park is **one tick** — the
shortest sleep the kernel offers.

So task 5 is woken, becomes runnable, and must be scheduled and reach
`mutex_lock` inside a one-tick window before task 9 takes the lock straight back.
It never wins. `acq=76` against `cont=2` is the convoy in two numbers: task 9
acquired seventy-six times while task 5 contended twice and stopped being
counted, because `mutex_lock` blocks rather than re-contending.

**The worker starves the init task**, which is the exact inverse of the failure
Tortoise warned about, and it has the same root cause.

### 179d. Priority: nat-os does not honour the blob's request

Checked, because Tortoise asked and because it is load-bearing for 179c.

- `osi_s_task_get_max_priority()` returns **25**, IDF's `configMAX_PRIORITIES`,
  deliberately — the blob does arithmetic on it. Correct.
- IDF's wrapper passes the result straight through:
  `xTaskCreatePinnedToCore(task_func, name, stack_depth, param, prio, ...)`.
  The blob's WiFi task is meant to run near the top of that scale.
- **nat-os's `blob_task_create()` discards `r->prio` entirely.** The request
  struct carries it (`struct blob_task_req { fn, arg, prio, handle,
  stack_bytes; }`), the slot struct has no field for it, and neither
  `task_create()` nor `task_create_with_stack()` takes one. Every task is created
  at `TASK_PRIO_NORMAL` (`task.c:492`).

The stub for `_task_get_max_priority` says where the work was meant to go:

> *The mapping down to `TASK_PRIO_LOW/NORMAL/HIGH` belongs in task creation,
> where the number is actually used.*

It was never written. nat-os has strict priorities with ageing
(`TASK_PRIO_LEVELS 3`) and a setter at `task.c:1665`, so honouring the request is
a mapping and one call — not a scheduler change.

Both parties therefore sit at `TASK_PRIO_NORMAL`, which is why the one-tick
window in 179c is decided by round-robin luck rather than by policy.

### 179e. What should happen before `_set_intr` is reached

Directly, since it has been an open question since step 177.

`set_intr_wrapper` is **not part of `esp_wifi_init_internal`**. In IDF the MAC
interrupt is attached on the start path, not the init path. So
**`[intr] routed=0` at this stage is expected and is not a bug** — the wiring
committed in 177 is correct and simply has not been reached.

The chain is: `esp_wifi_init_internal` must **return** → which needs task 5 to
re-acquire the blob mutex → which needs 179c fixed. Only then does
`esp_wifi_start()` become callable, and only that path calls `_set_intr`.

### What this leaves

Two candidate fixes for 179c, neither applied, because they are design choices
and not obviously equivalent:

1. **Fair hand-off** — `blob_trylock_w` refuses when a woken waiter is
   outstanding, so the release actually transfers ownership. Fixes the convoy
   whatever the priorities are.
2. **Honour the requested priority** (179d) — correct on its own merits and
   independently needed, but on its own it would make the worker *higher*
   priority than init and could deepen the convoy rather than break it.

They interact, and (1) looks like the one that must come first.

Per Tortoise's instruction the interrupt implementation was not touched.

**Nothing has been on air.**
---

## step 182 — the unwind, and what was actually causing it

Step 180 left `wifiinit` faulting at `_task_delete`, and the obvious move was to
implement `_task_delete`. The trace said not to:

```
t5:39 t5:14 t5:92 t5:13 t5:10 t5:11 t5:40 t5:25 t5:15
t9:10 t9:11 t9:30 t9:16 t9:94 t9:38
```

`_task_delay`, `_semphr_delete`, `_wifi_zalloc`, ... then the worker deleting its
own queue (`94`) and itself (`38`). **That is a shutdown sequence.** The crash at
`_task_delete` was the last step of an orderly teardown, not the first step of a
bug, and implementing `_task_delete` would only have moved the crash.

So the question was which earlier call failed. It took no instrumentation:

```c
static void * osi_s_wifi_zalloc(size_t size) { osi_hit(92u); return 0; }
```

**All four WiFi-heap allocators returned NULL unconditionally.** `_wifi_malloc`,
`_wifi_realloc`, `_wifi_calloc`, `_wifi_zalloc`. Task 5 asked for memory, was
told there was none, and unwound the driver.

### 182a. A survey rather than another crash

Rather than find these one fault at a time, every adapter entry was scanned for a
body that is empty or `return 0`. Discounting the eight one-line entries that are
genuinely implemented, the ones **on the live path** were:

| id | entry | returned | fixed |
|---|---|---|---|
| 89 | `_wifi_malloc` | NULL | yes |
| 91 | `_wifi_calloc` | NULL | yes |
| 92 | `_wifi_zalloc` | NULL | yes |
| 40 | `_task_ms_to_tick` | 0 | yes |
| 46 | `_get_free_heap_size` | 0 | yes |
| 30 | `_queue_msg_waiting` | 0 | yes |
| 17 | `_wifi_thread_semphr_get` | NULL | yes |
| 90 | `_wifi_realloc` | NULL | **no** — nat-os has no `heap_realloc` |
| 38 | `_task_delete` | — | no |
| 39 | `_task_delay` | — | no |

Two of these are worth naming on their own.

**`_task_ms_to_tick` answered 0.** The blob derives the `queue_send` and
`semphr_take` timeouts on the init path from exactly this call, so every one of
them collapsed to "do not block". nat-os's tick is `OSI_TICK_CYCLES` at
`OSI_CYCLES_PER_MS` — 10 ms — and the answer now rounds up, so a non-zero request
can never become a zero wait.

**`_get_free_heap_size` answered 0**, while `osi_impl_free_heap()` has existed all
along. Telling a driver it has no memory is the same defect as refusing the
allocation, one step earlier.

The allocators needed nothing new: IDF separates `_wifi_*` from `_malloc_internal`
only by which heap they draw from, `MALLOC_CAP_INTERNAL` either way on this part,
and nat-os has one heap. They route to what `_malloc_internal` was already using
successfully.

### 182b. `_wifi_thread_semphr_get`, and the second unwind

With the allocators in, three new calls appeared and the trace stopped in a new
place:

```
t5:92 t5:41 t5:17 t5:44 ...
```

`_task_get_current_task`, `_wifi_thread_semphr_get` — **NULL** — and then `_free`
one call later. The same shape as before, one gap further along.

IDF's `wifi_thread_semphr_get_wrapper` is a lazily created per-thread semaphore:
read FreeRTOS thread-local slot 0, and if it is empty create
`xSemaphoreCreateCounting(1, 0)` and store it there. nat-os has no TLS, but the
slot is only ever indexed by "the current task", so an array indexed by task id
is the same object.

### 182c. Result: the unwind is gone

```
before:  ... t5:92 t5:41 t5:17 t5:44 t5:13 ...  | t9:10 t9:11 t9:30 t9:16 t9:94 t9:38
after:   ... t5:92 t5:41 t5:17 t5:41 t5:21 t5:10 t5:11 t5:40 t5:25 t5:41 t5:22 t5:15 | t9:10 t9:11
```

No `_free`, no `_semphr_delete`, no `_wifi_delete_queue`, no `_task_delete`.
What replaces them is a **WiFi API call**: `_mutex_lock`, interrupts masked and
restored, `_task_ms_to_tick`, `_queue_send`, `_mutex_unlock`, `_semphr_take`.
Take the API lock, post the request, release it, wait for the reply. That is the
mechanism working, not unwinding.

`esp_wifi_init_internal` still does not return, and `esp_wifi_start()` is still
not reached.

### 182d. The new fault, and one thing it is not

```
exccause 20  InstFetchProhibited   epc 0x00000000
a0 0x8008ee40 -> osi_s_queue_recv     windowbase 15
frames  : task 9 held 0x0000000a granted 0x00000008 LOST 0x00000002
qspill  : walked 4 frames, 1 bad   first a0 0x00000000 at 0x3ffb2910
```

The worker, in `osi_s_queue_recv`, with a windowed save area whose return address
is zero. Reproducible byte-for-byte across runs.

This is no longer a missing stub — it is a windowing fault, and the obvious
suspicion is that step 180's full-depth release woke the save-area overlap
UM-NATOS-045 §8.4 listed as dormant: a call0 callee entered from a `w2c_*` bridge
still allocates over its caller's base save area, and full release lets another
context into windowed code while this task has live frames.

**That suspicion is not supported.** Step 180's own dump already carried
`qspill ... 1 bad first a0 0x00000000 at 0x3ffb2900` — the same zero-`a0` frame,
before any of this step's changes. It is pre-existing and was simply not being
reached. Worth stating plainly, because "the last change woke it" is the
comfortable answer and it happens to be wrong.

### What is committed

Seven adapter entries, all pure and non-blocking, none of them touching the
interrupt implementation.

```
boot 11 PASS 0 FAIL
wintorture 1000 ms : switches during the call: 10  (preemption really happened)
                     checksum 1632 expected 1632  CORRECT
blobphy rc=0
wifiinit : reaches the API round-trip, faults in the worker
```

Not done, and deliberately: `_task_delete` and `_task_delay` both need blocking
semantics and the same release/re-acquire dance `_semphr_take` performs, and
neither is the current blocker. `_wifi_realloc` needs a `heap_realloc` that does
not exist.

**Nothing has been on air.**
---

## step 183 — four hypotheses killed, and two instruments that were lying

The fault left by step 182: the worker, `exccause 20 InstFetchProhibited`,
`epc 0x00000000`. Reproducible byte-for-byte. No code changed this step except
the panic dump; everything below is elimination.

### 183a. Not concurrency, and not the save-area overlap

Step 180's full-depth release lets two contexts sit in windowed code at once for
the first time, so the obvious suspect was the `w2c_*` save-area overlap that
UM-NATOS-045 §8.4 lists as dormant.

`BLOB_PIN_DISABLE` was kept as a switch for exactly this question (UM-NATOS-044
§9). Set to `0`, only one context can be inside the blob:

```
pin off : exccause 20  epc 0x00000000  last osi 11  trace ...t9:10 t9:11
pin on  : exccause 20  epc 0x00000000  last osi 11  trace ...t9:10 t9:11
```

Identical. **The fault does not need two contexts in windowed code**, so the
overlap and every other concurrency story are out.

This also **corrects step 182**, which dismissed the same hypothesis on the
grounds that the zero-`a0` frame pre-dated the change. That argument was about
the wrong frame — see §183c — and did not settle anything. The pin A/B does.

### 183b. The worker did not return

The chain `epc 0` ← `retw` ← a save area holding `a0 = 0` has an economical
explanation: the blob's worker function returned, unwinding past `rom_call3`'s
`entry` into `blob_task_entry`'s **call0** frame, where a "save area" is only
call0 locals and the zero is whatever happened to be there.

`reached`/`running`/`returned` have existed since step 179 but print only from
the `[qr]` block, which a panic pre-empts. Moved into the panic dump:

```
worker    : reached 1  running 1  returned 0
```

**`returned 0`.** The worker is still inside `rom_call3`. The hypothesis is dead,
and it cost one line of output rather than a rewrite of `_task_delete`.

### 183c. Two instruments that were lying, and one conclusion built on one

Two lines in the dump were being read as fault-time facts. Neither is.

**`a0/sp out`** is written by `w2c_call*` on its way out (`window.S:772`), not by
the fault. It is a **singleton** overwritten by every bridge call, so the value
that survives to the panic belongs to whichever bridge ran last — exactly the
flaw UM-NATOS-044 §7 records about step 79's first attempt, reintroduced in a
different probe. Its verdict string is worse than useless: it prints
`context survived` whenever `a0 | sp` is non-zero, so `sp = 0` alone reads as
healthy.

A chain of reasoning here — *`a1` became zero, therefore the underflow read from
`0xfffffff0`, therefore …* — was built on that number. It is not the faulting
`a1` and the chain does not stand.

**`saved frame @`** dumps `task_saved_sp(task_current())`, the current task's
**last switch-out**. When the current task is the one that faulted, that frame is
stale by construction, and the `0xfffffff0` read as `a1 - 16` is just an old
saved register.

**The zero-`a0` frame is probably not a defect either.** `qspill` walks the a1
chain and flags `a0 == 0` at `0x3ffb2910`. Task 9's stack is
`0x3ffb0d3c + 7168`, so that address is 0x20 below the top: the outermost frame,
whose caller is `blob_task_entry` — call0 code, which has no windowed save area
and no `a0` to find. A task's base frame having no caller is the normal state.
The walk has no stopping rule at the task base, so it reports the normal state as
`1 bad`.

Two more, noticed while reading and not yet fixed:

- `sbp-post : wb 4294967295 ws 0xffffffff  SWEEP LEFT MULTI-BIT` — `0xffffffff`
  is the never-sampled sentinel, and the verdict is printed off it. It has been
  announcing a defect in a probe that never ran.
- `blk-window : ... spill ws 0x000002802b ...` — twenty-one bits of a sixteen-bit
  register, from a missing separator running two fields together.

### What is actually known

Reliable, because the panic handler reads them live:

```
exccause 20  InstFetchProhibited   epc 0x00000000
ps 0x00060130     EXCM set, WOE set
windowbase 15     windowstart 0x00008000    one frame
worker: reached 1 running 1 returned 0
last osi: entry 11  _wifi_int_restore
```

The blob jumped to address 0 while its worker was still executing inside it, and
`_wifi_int_restore` — verified this step to merge only `PS.INTLEVEL`, exactly
what `XTOS_RESTORE_JUST_INTLEVEL` specifies — was the last adapter entry before
it. `osi_impl_queue_send` and `osi_qpoll_w` were both read and both copy
`item_size` bytes correctly, so a mis-delivered message is not the cause.

That leaves **the blob calling a function pointer that is zero**, with the OSI
table itself verified complete in step 179. The remaining candidates are pointers
the blob expects something else to have filled in — most plausibly a field inside
a structure it allocated with `_wifi_zalloc`, which now returns real zeroed
memory where before it returned NULL and the driver gave up before ever reading
it.

### Next

Capture the **true** fault-time `a0`. The exception path must stash it somewhere
the panic handler can reach; `excsave1` reads `0x00000000` in the dump, so it is
not there yet. That single value names the caller, and the caller names the
pointer. Nothing else should be built on `a0/sp out` or `saved frame @` until
they are fixed or deleted.

**Nothing has been on air.**
---

## step 184 — the real registers, and what the blob jumped through

Step 183 ended with a rule: build nothing further on `a0/sp out` or
`saved frame @`, and get the true fault-time `a0`. Both done.

### 184a. Recording the registers that actually faulted

`_handler_panic`'s first instruction was `movi a1, _panic_stack_top`, which
destroys the faulting `a1`, and `a0` was never recorded at all. Every "register"
in the dump came from a probe sampled somewhere else.

`EXCSAVE1` has read `0x00000000` throughout the investigation, so it is free to
hold the faulting `a1` for the two instructions it takes to free a register:

```asm
wsr.excsave1 a1                 /* stash faulting a1        */
movi    a1, g_fault_regs
s32i    a0, a1, 0               /* faulting a0              */
rsr.excsave1 a0                 /* faulting a1 back into a0 */
s32i    a0, a1, 4
... a2..a15, windowbase, windowstart, valid flag ...
```

Only `a0` and `a1` are clobbered, and both are saved first.

```
fault regs: a0 0x8008d765  a1 0x3ffb28b0  wb 15  ws 0x00008000
fault a2- : 0x3ffd4e08 0x3ffd4e9c 0x00000000 0x3ffd7ac8 0x3ffb0cd8 0x00000000
            0x8036bb99 0x3ffb2890 0x00000006 0x00060120 0x1312cfff 0x00000008
            0x05100000 0x3ffc05c8
```

**`a1 = 0x3ffb28b0`** — a valid address inside task 9's stack. The "a1 became
zero" reading that step 183 retracted is now not merely unsupported but false.

### 184b. Which frame, and therefore which instruction

`a0 = 0x8008d765` disassembles exactly:

```asm
4008d762:  callx8  a2          <- a2 = g_bt[slot].fn
4008d765:  mov.n   a2, a10     <- the recorded a0
4008d767:  s32i.n  a2, a1, 20
           call0   blob_unlock
```

That is `rom_call3`, which is **call0** and enters the blob through `callx8`. So
the frame that faulted is the blob's **outermost windowed function**, and its
`a0` is simply the link back to `rom_call3` — not the caller of address zero.

`ws = 0x00008000`, one bit, at `wb 15`: a single windowed frame, and **no
rotation happened at the fault**. That rules the instruction in:

| candidate | ruled out because |
|---|---|
| `callx8` | rotates `WindowBase`; `wb` would not still be 15 |
| `callx0` | overwrites `a0`, which still holds the `rom_call3` link |
| `retw` | `a0` is a valid address, not zero |

What is left is an **indirect jump through a register holding zero**. `a4` and
`a7` are both `0x00000000`.

### 184c. What it was dispatching on

A `jx` through null inside a driver is a dispatch table, and the worker had just
taken its first message off the queue. `osi_qpoll_w` now copies the first
message it ever delivers:

```
first msg : 8 B : 06 00 00 00  48 18 fc 3f
```

Eight bytes: `{ id = 6, arg = 0x3ffc1848 }`. A plain API request, and the
argument is a plausible DRAM address. **The message is sane** — and `a10` at the
fault is `0x00000006`, the same id, still live in a register.

So the sequence is complete and none of it is nat-os mis-delivering anything:
task 5 posts request 6, the worker receives it correctly, dispatches on 6, and
the table entry it jumps through is zero.

`osi_impl_queue_send` and `osi_qpoll_w` were already read in step 183 and copy
`item_size` bytes correctly; this measurement confirms it end to end rather than
by inspection.

### 184d. Why the entry is zero — not established

The blob is a **loaded** image, so a table of function pointers that never got
populated is the shape to look for. The loader was read:

- `.text` is MMU-mapped into IROM;
- `.rodata` is MMU-mapped into DROM, and range-checked (`-8` if it falls
  outside the window);
- `.data` is copied `data_lma -> data_vma`, word-aligned and word-at-a-time;
- `.bss` is zeroed.

No gap is visible there, which makes the more likely explanation **a table the
blob populates at run time, by an initialisation step nat-os never performs**.
IDF's bring-up is not `esp_wifi_init_internal` alone. That is a hypothesis, and
it is recorded as one.

### What is committed

The fault-register recorder in `_handler_panic`, its dump, and the first-message
capture. No functional change to the driver path.

```
boot 11 PASS 0 FAIL
wintorture 1000 ms : switches during the call: 10  (preemption really happened)
                     checksum 1632 expected 1632  CORRECT
blobphy rc=0
```

### Next

Find what populates the dispatch table for API id 6. Two ways in, and the first
is cheaper: disassemble the blob around the outermost function reached from
`g_bt[0].fn` and locate the `jx` and the table it indexes; or work forward from
IDF's init sequence and find the registration step that has no counterpart here.

Still to clean up, from step 183 and unchanged: `a0/sp out` and `saved frame @`
are misleading and now redundant — `fault regs` supersedes both. `sbp-post`
prints its verdict off a never-sampled sentinel, and `blk-window` runs two fields
together into a twenty-one-bit `WINDOWSTART`.

**Nothing has been on air.**
---

## step 185 — two words of zero in the adapter table

The fault is fixed, and it was two missing lines.

### 185a. Reading the blob

`build/blob.elf` carries symbols, which had not been used. The worker function
is **`ppTask`** at `0x4036baec`, and its prologue reads straight off our table:

```asm
4036baef:  l32r  a2, (3ffd4e08 <g_osi_funcs_p>)
4036bafb:  l32i  a3, a3, 64        ; offset 64  = field 16 = _semphr_give
4036bafe:  callx8 a3
4036bb10:  l32i  a5, a5, 116       ; offset 116 = field 29 = _queue_recv
4036bb15:  callx8 a5
```

`_semphr_give` then `_queue_recv` — exactly `t9:16 t9:29`, the first two calls
the worker ever made. Every fault register matched a named blob object:
`a2 = &g_osi_funcs_p`, `a3 = &xphyQueue`, `a14 = 0x05100000`, the queue handle.

The signal dispatch:

```asm
4036bb6e:  l32r   a5, (3f7011a0)   ; jump table, in DROM
4036bb71:  addx4  a5, a10, a5      ; a10 = the signal id
4036bb74:  l32i.n a5, a5, 0
4036bb76:  jx     a5
```

`a10 = 0x00000006` at the fault, and the first message delivered was
`{ id = 6, arg = 0x3ffc1848 }`. Table entry 6 is `0x4036bbab`, and that
instruction loads `0x3ffd7ac8` — **which is the recorded `a5`**. So the jump
table was intact and the `jx` had already succeeded. The fault was further on.

### 185b. The defect

Entry 6 lands in a shared tail:

```asm
4036bb8c:  l32i.n a8, a5, 0
4036bb8e:  beqz   a8, ...          ; the blob DOES check this one
4036bb91:  l32i.n a8, a2, 0        ; g_osi_funcs_p
4036bb93:  l32i   a8, a8, 216      ; offset 216 = field 54
4036bb96:  callx8 a8               ; and does NOT check this one
```

Offset 216 is **`_phy_common_clock_enable`**. In `wifi_osi_stubs.c` the struct
declares it, and the initializer goes straight from `._phy_enable` to
`._phy_update_country_info`:

```c
._phy_disable = osi_s_phy_disable,
._phy_enable  = osi_s_phy_enable,
/* _phy_common_clock_enable  -- absent */
/* _phy_common_clock_disable -- absent */
._phy_update_country_info = osi_s_phy_update_country_info,
```

**A designated initializer zero-fills omitted members.** Two words of the table
handed to the driver were NULL, and the driver calls them without checking.

`wifi_osi.c`'s table has both and is clean; only the stubs table had the hole.

### 185c. A correction to step 184

Step 184 ruled out `callx8` as the faulting instruction because "it rotates
`WindowBase`, and `wb` would not still be 15", and concluded the instruction had
to be an indirect *jump*.

That is wrong about the ISA. **`CALL8` does not rotate at the call site.** It
writes the return address into `a8` of the *current* window and branches; the
rotation happens in the callee's `ENTRY`. Address 0 never executed an `ENTRY`,
so no rotation ever occurred — which is exactly why the window looked like a
single unrotated frame, and why `a8 = 0x8036bb99` was sitting there: that is
precisely the return address `callx8` had just written.

The evidence step 184 gathered was right and completely sufficient. The
elimination argument applied to it was not.

### 185d. And a correction to my own scan

A scan for holes reported five. Three were false: `_slowclk_cal_get` is
`#if !CONFIG_IDF_TARGET_ESP32`, and `_regdma_link_set_write_wait_content` and
`_sleep_retention_find_link_by_id` are ESP32-C6 only. The scan matched struct
members textually and did not honour the preprocessor. Two were real, and the
compiler said so when the other three were wired.

Step 183's survey could not have found either: it looked for stub *functions*
with empty or `return 0` bodies, and here there is no stub to find. A hole in a
designated initializer is invisible to every check this project had.

### 185e. The guard

`wifi_init_cfg()` is the last code to touch the table before the blob does. It
now walks the words between `_version` and `_magic` and refuses to be quiet:

```
osi table : 118 words, no null slots
```

A zero among those words is always a defect, and this would have printed it on
the first `wifiinit` of the investigation.

### 185f. Result

```
before:  ... t9:10 t9:11                                        -> epc 0
after:   ... t9:10 t9:11 t9:117 t9:57 t9:55 t9:55 t9:79 t9:92 t9:85
```

`t9:117` is the new `_phy_common_clock_enable`. ppTask now gets through it into
the callback at `s_wifi_queue+0x14`, which runs `_timer_disarm`, `_read_mac`
twice, `_get_random`, `_wifi_zalloc` and `_malloc_internal`.

The new fault is different in kind:

```
exccause 28  LoadProhibited   epc 0x4000be94   (ESP32 ROM)
wb 15  ws 0x0000aaa8          seven live frames
osi alloc : calls 7  bytes 4272  largest 3120  FAILS 0  heap free 67384
```

**Not memory exhaustion** — nothing failed and 67 KB remain. A ROM routine read
a bad address. `0x4000be94` and the return `0x40056c48` are not in
`esp32.rom.ld`; the nearest exported symbols are `+0x1e0` and `+0xc28` away, so
they are unexported ROM and those names mean nothing. They are not quoted here
as identifications.

The strongest lead is in the trace immediately before it:

```c
static int osi_s_read_mac(uint8_t* mac, unsigned int type)
{
    osi_hit(55u);
    return 0;                       /* ESP_OK, and mac is never written */
}
```

It reports success and leaves the caller's buffer untouched. The blob called it
**twice** and then allocated and called into ROM. A MAC address the driver
believes it has and has not got is exactly the shape of a bad load.

### Next

1. Implement `_read_mac`. It needs a real MAC from eFuse; nat-os reads eFuse
   already for other purposes.
2. Then re-run and see whether the ROM load survives.
3. The instrumentation debt is now large enough to be its own task: `a0/sp out`
   and `saved frame @` are superseded by `fault regs` and actively mislead;
   `sbp-post` prints a verdict off a never-sampled sentinel; `blk-window` runs
   two fields together.

**Nothing has been on air.**
---

## step 186 — a MAC that was never read, and an operating system ROM expected

### 186a. `_read_mac`

```c
static int osi_s_read_mac(uint8_t* mac, unsigned int type)
{
    osi_hit(55u);
    return 0;                       /* ESP_OK, and mac never written */
}
```

Reporting success for work never done, and the blob asked twice. Implemented
from eFuse, with the layout taken from ESP-IDF's `esp_efuse_table.c`
(`MAC_FACTORY`) rather than recalled — BLK0 bit offsets, most significant byte
first, so `mac[0]` is bits 72..79 and `mac[5]` is bits 32..39, which lands as
`RDATA2`'s low half followed by `RDATA1` big-endian. Type derivation is
`esp_read_mac()`'s: STA is the base, SoftAP `+1`, BT `+2`, ETH `+3`.

Verified against ground truth rather than by inspection — **esptool prints the
MAC while flashing**:

```
esptool  MAC: 5c:01:3b:50:3f:64
nat-os   base mac : 5c:01:3b:50:3f:64
```

**And it did not move the fault at all.** Byte-identical trace, same
`excvaddr`. A real defect, and not this one. Worth stating plainly: the step-185
report named it as "the strongest lead", and it was wrong.

### 186b. The fault: an operating system ROM expected and did not find

`excvaddr` settles a `LoadProhibited` immediately, and had been in the dump all
along:

```
exccause 28  LoadProhibited   epc 0x4000be94   excvaddr 0x00000000
```

`esp32.rom.ld` alone does not resolve `0x4000be94`, but the ROM symbols are
spread over ten linker scripts. Across all of them:

```
0x4000be94 -> __getreent + 0x8   [esp32.rom.syscalls.ld]
              next symbol: malloc at 0x4000bea0
```

The ESP32 mask ROM contains a newlib — `malloc`, `printf`, the string and float
routines — but not the operating system those routines need. Every ROM entry
that touches libc state reaches through a table of callbacks the runtime
installs at `syscall_table_ptr_pro` (`0x3FFAE024`). Measured, before assuming:

```
rom stubs : pro=0x00000000 app=0x00000000
```

**nat-os never wrote either.** The blob called a ROM routine, the ROM routine
called `__getreent`, and `__getreent` dereferenced a null table pointer. Not the
blob's doing and not the adapter's.

`kernel/rom_stubs.c` + `vendor/windowed/rom_stubs_w.c` install a 36-entry table:
the allocators route to nat-os's heap, `__getreent` returns a writable zeroed
block so ROM has somewhere for errno, the lock family is deliberately empty
(nat-os already serialises every path that reaches ROM libc, and making them
real would mean a blocking call0 excursion from inside ROM code — step 104), and
everything else refuses. Field order is copied from IDF's `libc_stubs.h`.

`_realloc_r` returns NULL rather than faking it: nat-os's heap has no realloc and
the old size is not recoverable from the pointer, so there is no way to copy the
right number of bytes. A visible failure beats invisible corruption.

### 186c. The ABI boundary, crossed wrongly in both directions

Two mistakes in one step, both mine, both instructive.

**call0 table, windowed caller.** The first attempt put the stubs in `kernel/`,
which builds `-mabi=call0`. ROM calls them with `CALL8`, which writes the return
address into `a8` and leaves the rotation to the callee's `ENTRY`. A call0
function has no `ENTRY`, so nothing rotates and it returns through an `a0` the
caller never set:

```
exccause 2  InstructionFetchError   epc 0x3ffb2770   (inside task 9's stack)
```

**Windowed helper, call0 caller.** Moving the table to `vendor/windowed/` fixed
that and introduced the mirror image: `rom_stub_words()` was a windowed function,
and `kernel/rom_stubs.c` — call0 — called it directly.

```
exccause 0  IllegalInstruction   epc 0x4008de55   (rom_stub_words)
```

It is now a `const uint32_t`. A word of data needs no ABI at all.

The same boundary, crossed both ways, in the file written to fix a crossing. The
project's rule — *the boundary is by file* — is not a style preference, and the
two symptoms are worth keeping as its signature: **call0 callee entered from
windowed returns into a stack address; windowed callee entered from call0 traps
IllegalInstruction on its `entry`.**

### 186d. Result

```
before:  ... t9:79 t9:92 t9:85                                    -> __getreent+8
after:   ... t9:85 t9:44 t9:92 t9:92 t9:92 t9:21 t9:22 t9:89 t9:21
```

`_free`, three `_wifi_zalloc`, `_mutex_lock`/`_unlock`, `_wifi_malloc`. And the
allocation figures move for the first time in this investigation:

| | step 185 | now |
|---|---|---|
| allocator calls | 7 | **26** |
| bytes | 4,272 | **23,820** |
| heap free | 67,384 | 47,880 |
| fails | 0 | 0 |

The driver is genuinely building its state rather than dying on the way in.

The new fault is another jump to zero, and the technique from step 185 named the
site in one step:

```
fault regs: a0 0x00000000  a1 0x3ffb9560  wb 1  ws 0x00000002
a8 = 0x8008d964  ->  4008d961: callx8 a2      (rom_call4)
```

`a1` is in **task 5's** stack, so this is the init context, not the worker, and
`rom_call4` was called with `fn == 0`. The step-177 ISR trampolines also use
`rom_call4`, and they were checked first: `blob_isr_run()` guards with
`if (!fn) { g_blob_isr_nofn++; return; }`, so this is not that. The other caller
is `blob_call()`.

`esp_wifi_init_internal` still does not return.

### Next

1. Find what calls `blob_call()` with a null function on task 5. The caller is
   nat-os's, not the blob's, so this is a short search.
2. `_get_random` is the same defect as `_read_mac` was — `return 0` with the
   caller's buffer untouched — and is on the live path. It should be fixed on its
   own merits, and *not* described as a lead until something implicates it.
3. The instrumentation debt is overdue and now has a new entry: `a0/sp out` and
   `saved frame @` are superseded by `fault regs`, `sbp-post` prints a verdict
   off a never-sampled sentinel, `blk-window` runs two fields together, and
   `qspill` reports a task's base frame as `1 bad`.

```
boot 11 PASS 0 FAIL
wintorture 1000 ms : switches during the call: 10  (preemption really happened)
                     checksum 1632 expected 1632  CORRECT
blobphy rc=0
```

**Nothing has been on air.**
---

## step 187 — identifying the null pointer

The question was narrow: what is the pointer, and why is it null. It is not what
step 186 supposed, and it is not a function pointer at all.

### 187a. What it is not

Step 186 read `a8 = 0x8008d964` in the fault registers, resolved it to the
instruction after `callx8 a2` in `rom_call4`, and concluded the bridge had been
called with a null target. Three eliminations, in order:

**`blob_call` has exactly one caller** — `shell.c:820`, passing `e->wifi_init`,
which the shell prints as `0x403014dc`. `rom_call4`'s other callers pass
constants, and `blob_isr_run()` already guards with
`if (!fn) { g_blob_isr_nofn++; return; }`.

**`rom_call4` now refuses a null target** and records the caller rather than
jumping to zero. It reports `n 0`. **The bridge was never called with null.**
`a8` was a stale leftover in a register the fault did not touch — the same trap
step 183 recorded for `a0/sp out`, in a different register.

**Neither table has a hole.** The adapter guard reports `118 words, no null
slots`; the ROM stub guard reports `36 entries, installed`; and the stub entry
points were disassembled and are all proper windowed functions —
`stub_getreent` returns `g_rom_reent`, not zero.

### 187b. What it is

`rom_call4` primes the base save area's `a0` slot with `win_chain_trap`, so that
a chain unwinding past it traps by name. Recording where it primed, and
re-reading it at the fault:

```
chain base: at 0x3ffb9530  primed 0x4008dae8  now 0x4008dae8
            sp 0x3ffb9540  saved a0 @sp 0x00000000
fault regs: a0 0x00000000  a1 0x3ffb9560  wb 1  ws 0x00000002
```

`rom_call4` ran with `sp = 0x3ffb9540`. It begins `addi a1, a1, -32`, so its
caller's sp was `0x3ffb9560` — **exactly the faulting `a1`**. `a1` had already
been restored, which places the fault in the epilogue:

```asm
    l32i.n  a0, a1, 0       /* reload the call0 return address */
    ...
    addi    a1, a1, 32
    ret                     /* -> a0 */
```

And `[sp+0]`, the word `rom_call4` wrote at entry with `s32i a0, a1, 0`, now
reads **zero**.

**The pointer is `rom_call4`'s own saved call0 return address.** Not a function
pointer, not an adapter slot, not a blob callback. The bridge returned to
address zero because the return address it had stored on its own stack was gone.

### 187c. Why it is null

It was not null when written. Two things establish that the store happened and
the frame was set up correctly:

- `s32i a0, a1, 0` stores whatever `blob_call` passed in `a0`, and a `call0`
  return address is never zero;
- the word 16 bytes lower, at `0x3ffb9530`, was primed in the same prologue and
  **still holds `win_chain_trap`**.

So the prologue ran, and exactly one word of `rom_call4`'s frame was
subsequently zeroed while the blob was executing. This is an **overwrite, not a
miss** — which is why no guard caught it: every guard added so far checks that a
value was *installed*, and this one was.

That also explains why `win_chain_trap` never fired. It protects the save area
**below** `rom_call4`'s sp. The word that was destroyed is at `sp` itself, above
everything the priming covers.

### What is committed

`rom_call4` refuses a null target and names the caller; the panic dump reports
the chain-base priming and re-reads both the primed slot and the saved return
address. All three are read-only reporting of values the kernel already had.

```
boot 11 PASS 0 FAIL
wintorture 1000 ms : switches during the call: 10  (preemption really happened)
                     checksum 1632 expected 1632  CORRECT
blobphy rc=0
```

### Next

Find what writes zero to `0x3ffb9540`. It is one word, at a known address, in
task 5's stack, written while the blob runs — a watchpoint-shaped question rather
than a search. Candidates in order of cheapness:

1. A `w2c_*` bridge's dormant save-area overlap (UM-NATOS-045 §8.4) reaching a
   word above the bridge's sp rather than below it.
2. A blob write through a pointer nat-os handed it. `_read_mac` now writes six
   bytes and `osi_qpoll_w` writes `item_size`; both write to caller-supplied
   addresses and neither is bounds-checked.
3. A spill from a window vector using a stale sp.

The `frames` and `overlap` probes already in the dump watch a *different* frame
and reported nothing; a probe aimed at this word specifically is the next step.

**Nothing has been on air.**
---

## step 188 — what zeroes the saved return address

Step 187 established that the null "pointer" is `rom_call4`'s own saved call0
return address, overwritten while the blob ran. This step names the writer and
measures the write. Two attempted fixes failed; both are recorded.

### 188a. Bracketing

`rom_call4` already records the address of its saved-a0 slot. Checking that one
word at ordered points along the path latches the first site that sees it zero.
The first bracket, at every adapter call:

```
rc0 zero : by trace idx 15  entry 16  task 9
```

Index 15 is `t9:16`, the worker's **first** call — thirty-three calls before the
crash. Index 14 is `t5:15`, `_semphr_take`. So the word dies inside that call's
blocking path, and five checkpoints along it give:

```
rc0 zero : by trace idx 15  entry 0  task 5  site 2
```

**Site 2 is immediately after `win_spill_all()`.** Site 1, before it, does not
fire — so the value was intact going in.

### 188b. The measurement

`win_spill_all()` is six nested `entry a1, 48` + `call12`: it deliberately
starves the register file so the hardware spills every live window. Sixteen
words of task 5's stack, either side, anchored at `rom_call4`'s a0 slot − 16:

```
spill pre : ws 0x00002aaa  wb 13   [a0 slot] 0x40081e1c
   0000000 3ffb6290 3ffbe4d8 4008ce62 | 40081e1c 3ffbeb3c 403014dc 00000000 | 00000000 403014dc ...
spill post: ws 0x00002800  wb 13   [a0 slot] 0x00000000
   0000000 3ffb6290 3ffbe4d8 4008ce62 | 00000000 00000000 00000000 cccccccd | 00000000 403014dc ...
```

`WINDOWSTART` goes from seven live frames to two — the spill did exactly its
job. And it wrote **exactly sixteen bytes**, at `[0x3ffb9540, 0x3ffb9550)`.
Nothing on either side changed.

Sixteen bytes is one base save area, `a0..a3`, written for a frame whose sp is
`0x3ffb9550`. `rom_call4`'s frame is `[0x3ffb9530, 0x3ffb9560)`, so that frame's
recorded stack pointer is **inside `rom_call4`'s own frame**, and it cannot
belong to the chain `rom_call4` started: everything `callx8` reaches lives below
`rom_call4`'s sp.

It is a **window that was already live when `rom_call4` was entered** — a stale
`WINDOWSTART` bit carrying a stack pointer from earlier windowed activity on
task 5. `rom_call4` then allocated its call0 frame across that address, and the
first spill deep inside the blob wrote the old frame home, on top of it.

The destroyed word was `0x40081e1c` — **`blob_call`**, the return address
`rom_call4` was going to use.

### 188c. Two fixes that did not work

**Reserving the base save area.** A call0 function that starts a windowed chain
should keep its own data out of `[sp, sp+16)`. `rom_call4` was widened to 48
bytes with its saves moved to `+16..+32`.

No effect, and the arithmetic says why: the caller's sp is fixed at
`0x3ffb9560`, so a 48-byte frame puts sp at `0x3ffb9530` and the a0 slot at
`sp+16` — `0x3ffb9540`, **exactly where it already was**. The danger zone is
anchored to the *caller's* sp, not to `rom_call4`'s, so growing the frame moves
the frame and the slot together. Reverted.

**Spilling before descending.** If the stale frames are flushed before
`rom_call4` allocates, nothing is left to spill over it later. `blob_call` was
given a `win_spill_call0()` immediately before the bridge.

Worse, not better: the panic moved to before the first adapter call, with
`rom_call4` recording `sp 0x3ffd3f50` — an address in the blob's DRAM, not task
5's stack. Spilling from that point leaves `a1` somewhere it must not be. The
idea may still be right; that call site is not. Reverted.

### 188d. A probe that lied, again

The window snapshots were first recorded unconditionally and printed
`[a0 slot] 0x00000000` on **both** sides of the spill — which reads as "already
dead before we got here" and would have sent the next step somewhere useless.

`osi_s_semphr_take` runs more than once. The snapshot was a singleton
overwriting itself, so it described the *last* call while the latch beside it
described the *first*. Latching the snapshot to the first pass produced the
table in §188b. This is the third time in this investigation that a
self-overwriting global has produced a confident wrong reading — steps 79, 183,
and here.

### What is committed

The bracket, the checkpoints, the before/after stack dump, `rom_call4` recording
its a0-slot address, and `rom_call4` refusing a null target. All reporting; no
change to the driver path. Both attempted fixes are reverted, and the tree
reproduces the step-187 fault exactly.

```
boot 11 PASS 0 FAIL
wintorture 1000 ms : switches during the call: 10  (preemption really happened)
                     checksum 1632 expected 1632  CORRECT
blobphy rc=0
```

### Next

The defect is now fully specified: **a windowed frame that is live across a
call0 function which then allocates over its recorded stack pointer.** Options,
none tried:

1. Have `rom_call4` clear the stale windows properly — `win_spill_call0()` is
   the right tool at the wrong site; inside `rom_call4`, after its own frame is
   allocated, may be correct.
2. Keep `rom_call4`'s saved return address out of memory entirely for the
   duration of the call — there is no call0 register that survives `CALL8`, so
   this needs a per-task slot rather than the stack.
3. Ask why a frame from earlier task-5 windowed activity is still live at all.
   `RETW` clears its own `WINDOWSTART` bit, so a bit surviving its function's
   return is itself worth explaining, and may be the actual defect.

(3) is the one that would explain rather than avoid, and it is cheap: log
`WINDOWSTART` at `rom_call4` entry and compare against the frames the chain
should own.

**Nothing has been on air.**
---

## step 189 — `esp_wifi_init_internal` returns ESP_OK

```
   calling esp_wifi_init_internal at 0x403014dc
   init      returned 0x00000000  (ESP_OK)
   osi       32 of 118 adapter entries were called
   intr clamped to CRIT_LEVEL: 0  (must stay zero)
```

The shell returns to its prompt and the system keeps running. That call has
never returned before.

### 189a. Reading the other party's code

The fix came from a question asked from outside the work: *does §4.7 of the book
apply here?*

§4.7 is about a different overlap — a bootloader IRAM segment that looked like it
would clobber the kernel and did not — and its conclusion was that the conflict
was imaginary. But the rule it produced is exact:

> an apparent conflict in an address map is a prompt to read the other party's
> link script, not to move our own

Step 188's write was measured, not inferred, so the conflict was real. The
**interpretation** was not measured. Sixteen bytes at `[0x3ffb9540, 0x3ffb9550)`
was read as a base save area — `[sp-16, sp)` for a frame with sp `0x3ffb9550` —
because that is what the ISA's convention says a base save area looks like. From
there the account required a stale window, and neither of the two fixes built on
it worked.

nat-os's own `_WindowOverflow8` is the source of truth, and it writes **two**
regions:

```asm
    s32e    a0, a9, -16      /* a0..a3 -> [a9-16, a9)     the base save area */
    l32e    a0, a1, -12      /* a0 <- the CALLER's sp                        */
    s32e    a4, a0, -32      /* a4..a7 -> [a0-32, a0-16)  the extended area  */
```

It was the second. With `a0` = `0x3ffb9560`, `[a0-32, a0-16)` is
`[0x3ffb9540, 0x3ffb9550)` — the measured range exactly. **There is no stale
frame.** Both readings fit the addresses; only one fits the handler.

### 189b. The defect

`a0` is loaded from `[a1-12]`, and that slot is primed by `rom_call4` itself:

```asm
    addi    a1, a1, -32
    addi    a8, a1, 32       /* = the caller's sp */
    addi    a9, a1, -12
    s32i    a8, a9, 0
    ...
    s32i    a0,  a1, 0       /* its own return address, at caller_sp - 32 */
```

So `rom_call4` hands the overflow handler a pointer to the caller's stack, and
the handler writes the caller's `a4..a7` at `caller_sp-32`, which with a 32-byte
frame is `rom_call4`'s own `[sp+0]` — the saved return address. The bridge armed
the write that destroyed it.

This is **step 145's defect in a second place**. That step found the task switch
frame written through the CALL12 extended save area and answered it with
`TASK_FRAME_RESERVE 48`; the same 48 bytes, for the same reason, were missing
here. CALL8 reaches `[caller_sp-32, caller_sp-16)`; CALL12 reaches
`[caller_sp-48, caller_sp-20]`.

### 189c. The fix

Reserve 48 bytes below the caller and put the bridge's own saves **below** that:
frame 80, saves at `sp+0..sp+28`, which is `[caller_sp-80, caller_sp-52)`.

Step 188's first attempt failed for a reason worth keeping: it widened the frame
to 48 and moved the saves *up* by 16. The caller's sp is fixed, so a bigger
frame moves `sp` down and the offset up by the same amount and the slot's
address does not change at all. **The danger zone is anchored to the caller's
stack pointer, not to ours** — so the saves have to go down, not up.

Three bridges had it:

| | before | after | evidence |
|---|---|---|---|
| `rom_call4` | 32 | 80 | measured failing |
| `rom_call3` | 48 | 80 | same defect by construction; the worker's bridge |
| `win_spill_call0` | 32 | 80 | same, and it exists to *cause* the spill |

`win_spill_call0` also explains step 188's second failed fix. Calling it from
`blob_call` to flush stale windows moved the panic earlier, with `rom_call4`
reporting a stack pointer inside the blob's DRAM. It was eating its own return
address on the way out. The idea was sound; the tool was broken in the same way
as the thing it was meant to help.

Cost: 32 bytes of stack per bridged call. Tightest stack after: shell
828/2048 B free.

### 189d. Result

```
boot 11 PASS 0 FAIL
wintorture 1000 ms : switches during the call: 10  (preemption really happened)
                     checksum 1632 expected 1632  CORRECT
blobphy rc=0
wifiinit : init returned 0x00000000 (ESP_OK), shell returns, system keeps running
```

Thirty-two adapter entries are called, in this order:

```
_recursive_mutex_create, _task_get_current_task, _mutex_lock, _calloc_internal(32),
_task_get_current_task, _mutex_unlock, _spin_lock_create, _recursive_mutex_create,
_malloc_internal(60), _task_get_max_priority, _wifi_create_queue, _semphr_create,
_task_get_max_priority, _task_create_pinned_to_core(6656), _semphr_take,
_semphr_give, _queue_recv, _task_delay, _semphr_delete, _wifi_zalloc,
_task_get_current_task, _wifi_thread_semphr_get, _task_get_current_task,
_mutex_lock, _wifi_int_disable, _wifi_int_restore, _task_ms_to_tick, _queue_send,
_task_get_current_task, _mutex_unlock, _semphr_take, _wifi_int_disable,
_wifi_int_restore, _phy_common_clock_enable, _timer_disarm, _read_mac, _read_mac,
_get_random, _wifi_zalloc, _malloc_internal(1024), _free, _wifi_zalloc x3,
_mutex_lock, _mutex_unlock, _wifi_malloc, _mutex_lock
```

Cosmetic: the trace prints `_phy_common_clock_enable` as `_magic`, because step
185 gave it trace id 117 and `wifi_osi_name()` only knows 1..116. The id is
outside the field numbering on purpose; the name table needs the two entries
added.

### Next

`esp_wifi_start()` is now **reachable and has never been called**. `e->wifi_start`
sits in the blob entry table with nothing invoking it, and there is no shell
command for it. That is the next step, and it is the first one where the radio
could do something.

Also outstanding, unchanged: `_get_random` still returns success without filling
the caller's buffer — the same defect `_read_mac` had — and the instrumentation
debt from steps 183 and 184.

**Nothing has been on air.**
---

## step 190 — `esp_wifi_start()` returns ESP_OK

```
   calling esp_wifi_start at 0x40301794
   start     returned 0x00000000  (ESP_OK)
   init      returned 0x00000000  (ESP_OK)
   osi       46 of 118 adapter entries were called
   [intr]    routed=1 nofn=0 fired: none
```

The driver initialises and starts. Forty-six adapter entries, up from
thirty-two, and `_set_intr`, `_set_isr` and `_ints_on` are reached for the first
time — the interrupt wiring committed in step 177 has been inert for thirteen
steps and is now exercised.

### 190a. A call site without growing `shell.c`

`esp_wifi_start` needed somewhere to be called from, and `shell.c` is the one
file this project does not add to: it is first in `.flash.text`, so anything
appended shifts everything the flash MMU maps and walks into the step-7 layout
band — measured, nine lines of `uart_puts` there hung `blob_map`
(UM-NATOS-042 §9.2).

The rule is about growth, so the bring-up moved to `wifi_init_cfg.c` and
`shell.c` got **smaller**: a three-line `blob_call` became
`uint32_t r = wifi_bringup(e, want_null);`, plus one line for the new argument.
Two insertions, three deletions.

`wifiinit start` runs both; plain `wifiinit` runs init only, so every
measurement taken up to step 189 stays reproducible unchanged — verified, byte
for byte.

### 190b. What start reached

Newly called, beyond the thirty-two init already used:

```
_set_intr  _set_isr  _ints_on  _phy_enable  _wifi_clock_enable  _timer_arm
_event_post  _coex_init  _coex_enable  _coex_wifi_request  _coex_wifi_release
_coex_schm_register_cb  _coex_register_start_cb
```

`intr clamped to CRIT_LEVEL: 0` — the clamp that keeps WiFi ISRs below the level
`flash_erase_sector()` masks has not had to fire.

### 190c. What is on air: nothing, and the claim is now worth stating carefully

Every report in this series has ended "nothing has been on air", and until now
that was trivially true because the driver never started. It is still true, and
here is the basis rather than the assertion:

- **One line is routed** (`routed=1`) and **no interrupt has been taken**
  (`fired: none`). The MAC is armed and silent.
- **No mode was set.** `esp_wifi_set_mode()` is never called, so there is no
  station or AP interface.
- **Nothing was commanded to transmit** — no scan, no connect, no beacon.

What *has* changed: `_phy_enable` and `_wifi_clock_enable` were called, so the
PHY is powered where before it was not. That is a real change of hardware state
and it should not be glossed. Powered is not transmitting, and no path to a
transmit exists yet, but the sign-off no longer means "the radio hardware was
never touched".

### 190d. It does not survive `blobphy` first

Standalone, `wifiinit start` is clean and reproducible. Run after `blobphy` in
the same boot, it panics **inside** the blob:

```
exccause 28  LoadProhibited   epc 0x4035c61d   excvaddr 0x0000006c
last osi : entry 57  _timer_disarm
```

`0x4035c61d` is `set_chanfreq_nomac` in the blob. A null structure dereferenced
at offset `0x6c`, immediately after `_timer_disarm`.

All five timer entries — `_timer_arm`, `_timer_disarm`, `_timer_setfn`,
`_timer_done`, `_timer_arm_us` — are empty stubs. `esp_wifi_start` arms a timer
and then relies on it, and the state `blobphy` leaves behind is enough to make
the difference show. This is UM-NATOS-042 §9.5's "the timer entries are stubs",
reached at last.

Recorded as a **conditional** result, not a clean one: init and start both
return ESP_OK, and they do so from a cold boot. The suite's combined ordering
does not yet pass.

```
boot 11 PASS 0 FAIL
wintorture 1000 ms : switches during the call: 10  (preemption really happened)
                     checksum 1632 expected 1632  CORRECT
blobphy rc=0
wifiinit start, from cold : init ESP_OK, start ESP_OK
wifiinit start, after blobphy : panic in set_chanfreq_nomac
```

### Next

1. **Implement the timer entries.** They are the named cause of the only
   remaining failure, and nat-os has `timer.h` already.
2. **`_get_random`** still returns success without filling the caller's buffer —
   the defect `_read_mac` had. It is called during start.
3. Then a mode and a scan, which is the first thing that would put anything on
   air and should not be attempted casually.
4. The naming table needs `_phy_common_clock_enable`/`_disable` added; they print
   as `_magic` because step 185 gave them ids outside 1..116.

**Nothing has been on air.**
---

## step 191 — the timers were already written, and the interrupt line was wrong

```
   init      returned 0x00000000  (ESP_OK)
   start     returned 0x00000000  (ESP_OK)
   [intr]    src=0 line=27 prio=1 routed=1 nofn=0 fired: none  timers=15 refused=0
```

### 191a. A correction to step 190

Step 190 named the timer stubs as "the named cause of the only remaining
failure". They were not. Implementing them changed the panic not at all —
identical `exccause`, `epc` and `excvaddr`.

`last osi : entry 57 _timer_disarm` is the last adapter entry *before* the
fault, which is correlation. It was written up as cause. The habit this
investigation keeps having to relearn: the last thing recorded before a fault is
not the thing that caused it, and step 183 caught the same error with `a8`.

### 191b. And the timers were already implemented

`kernel/wifi_osi_impl.c` has had a full timer implementation the whole time —
`osi_impl_timer_alloc/setfn/arm/arm_us/disarm/done`, a service task polling at
tick granularity, and `osi_impl_timers_used()` for reporting. A duplicate ETS
emulation was written before checking, and thrown away.

Two real defects in what already existed:

**`timer_of()` never matched anything the driver passed.** It compared the
pointer against `&g_timer[i]` — i.e. handles from `osi_impl_timer_alloc()`. But
the ETS contract is that the **caller owns the structure**: the blob allocates
its own `ETSTimer` and passes its address. Every timer call was a silent no-op.
Fixed with `timer_bind()`, which binds a blob-owned pointer to a free slot on
first use and keeps the old handle lookup working.

**The service called windowed code directly.** `osi_impl_timer_service()` did
`t->fn(t->arg)`. The handler is windowed vendor code and this file is call0 —
step 186's ABI trap, where the callee never executes `ENTRY` and returns through
an `a0` the caller never set. It now goes through `blob_call()`, which also
takes the blob mutex the handler needs.

Nothing started the service task, either; `arm_ticks()` now does.

Measured: `timers=12 refused=4` — the driver arms sixteen timers and the pool
held twelve. Refusing a timer the driver believes it armed is the same class of
silent failure this investigation keeps finding, so `OSI_TIMER_MAX` went 12 → 24
and `g_timer_short` is reported. Now `timers=15 refused=0`.

### 191c. The interrupt line, which step 179 flagged and got half right

With the timers actually working, `esp_wifi_start` got further and armed its
interrupt — and the board panicked asynchronously, in the display task:

```
exccause 4  Level1Interrupt   epc 0x40083933  (spi_tx)
```

Recording what the blob asked for:

```
[intr] src=0 line=0 prio=1
```

`src=0` is `ETS_WIFI_MAC_INTR_SOURCE` and `line=0` is `ETS_WMAC_INUM` — priority
**1**. nat-os installs exactly one interrupt handler, at level 3. Routing the
source to line 0 faithfully and then unmasking it arms a line nothing can
service, and the first time it fires the CPU takes a Level1Interrupt with no
handler.

Step 179 raised this and reached the wrong shape. It worried that the blob might
arm line 0 *behind our backs* through ROM helpers while we listened on 27. What
actually happens is simpler and was visible in `esp_adapter.c` all along: **the
blob passes the line number to `_set_intr`**, we route it faithfully, and
faithful is wrong.

The interrupt matrix does not care which line a source lands on, so the line is
remapped onto `INTR_LINE_WIFI_MAC` — 27, priority 3, extern level, served by the
existing `_handler_level3` and reserved for exactly this in UM-NATOS-042 without
ever being used.

The remap has to be applied in **three** places or it is worse than nothing:
`_set_intr`, and `_ints_on`/`_ints_off`, which are handed a *mask* of the blob's
line numbers. Unmasking bit 0 while the handler sits on 27 would arm an
unserviced line and disarm nothing.

### 191d. Where it stands

```
boot 11 PASS 0 FAIL
wintorture 1000 ms : switches during the call: 10  (preemption really happened)
                     checksum 1632 expected 1632  CORRECT
blobphy rc=0
wifiinit start, from cold : init ESP_OK, start ESP_OK, no fault
```

`fired: none` — the MAC is now routed to a line that *can* be serviced, and has
not fired. Nothing has been received and nothing transmitted. No mode is set.

**Still failing: `wifiinit start` after `blobphy` in the same boot.** Unchanged
by any of this work, and now understood well enough to state:

```
exccause 28  LoadProhibited  epc 0x4035c61d (set_chanfreq_nomac)  excvaddr 0x6c
```

Both commands run `phyinit`, so the combined ordering calls
`register_chipv7_phy` **twice**, which ESP-IDF never does. The control that
separates it from "start is fragile": running `wifiinit` twice — double *init*,
single extra phyinit — returns `0x101` (`ESP_ERR_NO_MEM`) and refuses cleanly,
with no fault. So double-init is handled and double-phyinit is not.

That is a sequencing property of the test harness rather than a defect in the
bring-up, and it should be fixed by making `phyinit` idempotent or by not
repeating it — not by changing anything downstream.

### Next

1. Make `phyinit` run once per boot.
2. `_get_random` still reports success without filling the caller's buffer.
3. A mode and a scan — the first thing that would put anything on air, and not
   to be attempted casually.

**Nothing has been on air.**
---

## step 192 — the two guards disagreed

The last failing case is fixed, and step 191's explanation of it was wrong.

### 192a. Not double phyinit

Step 191 concluded that `blobphy` followed by `wifiinit start` failed because
both run `phyinit`, calling `register_chipv7_phy` twice. That is not what
happens, and the evidence was already in the output:

```
phyinit   rc=0        <- blobphy
phyinit   rc=1        <- wifiinit, refused
```

`phyinit_run_at()` has guarded itself since long before this work:

```c
if (g_phy_attempted) { return -1; }
```

with a comment recording that a second call faults inside the blob and that
ESP-IDF calls it exactly once. The guard worked. The second phyinit never ran.

The reasoning was "both commands call phyinit, so it runs twice" — geometry from
a call graph, without reading the function. The same mistake §4.7 of the book
names, three steps after it was quoted.

### 192b. What actually happens

`blob_init()` **zeroes the blob's `.bss`**, and it was not guarded. So:

1. `blobphy` — `blob_init()` zeroes `.bss`; `phyinit_run_at()` fills a good deal
   of it with calibration data and the pointers `register_chipv7_phy` leaves
   behind.
2. `wifiinit start` — `blob_init()` runs **again** and wipes all of it. phyinit
   is guarded, so nothing rebuilds it. `esp_wifi_start` then dereferences what
   was erased.

```
exccause 28  LoadProhibited  epc 0x4035c61d (set_chanfreq_nomac)  excvaddr 0x6c
```

The two guards disagreed, and **the destructive function was the one without
one**. `blob_init()` now returns 0 immediately when `g_ready`.

Consequence worth stating: reloading a newly flashed blob image now needs a
board reset. That is the correct trade — the alternative is a function that
silently erases live state whenever it is called twice.

### 192c. The suite, in one boot

```
boot        11 PASS 0 FAIL
wintorture  switches during the call: 10  (preemption really happened)
            checksum 1632 expected 1632  CORRECT
blobphy     phyinit rc=0
wifiinit start
            phyinit rc=1        (guarded, correctly)
            init      returned 0x00000000  (ESP_OK)
            start     returned 0x00000000  (ESP_OK)
            [intr] src=0 line=27 prio=1 routed=1 nofn=0 fired: none
                   timers=15 refused=0
```

Every command passes in a single boot for the first time in this investigation.

`fired: none`. The MAC is routed to a line that can be serviced and has not
fired. No mode is set, nothing has been received, and nothing transmitted.

### Next

1. `_get_random` still reports success without filling the caller's buffer --
   the defect `_read_mac` had, unfixed since step 186 named it.
2. A mode and a scan. That is the first action that would put anything on air.
3. The instrumentation debt, which has grown again this session: the step-188
   brackets and stack dumps have served their purpose, and `a0/sp out` and
   `saved frame @` are superseded by `fault regs` and actively mislead.

**Nothing has been on air.**
---

## step 193 — the random number generator

`_rand`, `_random` and `_get_random` all answered 0, and `_get_random` did it
while leaving the caller's buffer untouched. Same shape as `_read_mac` before
step 186: success reported for work never done, which is worse than failing
because the caller cannot find out. It is called during `esp_wifi_start`.

**And `osi_impl_random()` already existed**, as an xorshift32, with a comment
that set the condition for replacing it:

> The ESP32 has a hardware RNG, but it is only properly random while the radio
> is running — which is the thing being brought up. This is deterministic on
> purpose rather than by accident, and **should be replaced once the PHY is
> live**.

Step 190 called `_phy_enable`. The condition is met, so the function now reads
`WDEV_RND_REG` — Espressif's own constant from
`soc/esp32/include/soc/wdev_reg.h`, not a derived one. `g_rng` is kept and still
stirred, so the xorshift is one line away if the hardware read ever proves
unavailable.

`osi_impl_get_random()` is new: it fills the caller's buffer a word at a time,
as `esp_fill_random` does, and returns `-1` rather than success on a null
pointer.

**Entropy, stated rather than assumed.** ESP-IDF documents this register as a
true random number generator only while the RF subsystem is running, and a much
weaker one otherwise. The driver's calls arrive after `esp_wifi_start`, which is
the good side of that — but nothing here enforces it, and these entries must not
be treated as a cryptographic source on that basis alone.

Verified that the register decodes and is not stuck, across boots:

```
rng=0x592ce85b,0x30971b55
rng=0xab345ca1,0xfa7c58e9
rng=0x485b8340,0x7dd1ecfd
```

That is a decode test, not a randomness test, and is not offered as one.

### The third duplicate

This is the third thing written this session that already existed — the ETS
timers in step 191, `osi_impl_random` here, and a duplicate ETS emulation
thrown away in between. Each time the existing code was found only after the
compiler refused a redefinition.

The rule that would have caught all three is the same one §4.7 of the book gives
for address maps, applied to our own tree: **grep before writing**. It is cheaper
than the build that catches it.

```
boot 11 PASS 0 FAIL   wintorture 10 real switches, checksum CORRECT
blobphy rc=0          wifiinit start : init ESP_OK, start ESP_OK
```

**Nothing has been on air.**
---

## step 194 — the instrumentation debt, and one probe that could not be removed

UM-NATOS-042 §9.3 has asked for this since step 102, and warned how it goes
wrong: *"a deliberate pass, file by file with a build between each; an attempt to
do it as an end-of-session tidy-up cascaded into three build failures and was
reverted."* Six builds, one file at a time.

### 194a. Removed

| probe | why |
|---|---|
| `rc0 zero`, `spill pre/post`, `chain base` | step 187/188 machinery; the defect they found is fixed |
| `a0/sp out` | written by `w2c_call*` on the way OUT, not at the fault, from a singleton every bridge call overwrote. Its verdict read "context survived" whenever either half was non-zero, so `sp == 0` alone looked healthy — step 186 built a retracted account on exactly that |
| `saved frame @` / `saved hi` / `saved ctl` | dumped the current task's LAST SWITCH-OUT, stale by construction whenever that task is the one that faulted |
| the `rcz_*` bracket machinery in `wifi_osi_stubs.c` | its call sites, helpers and globals |
| `rom_call4`'s prime-site recorder | nothing reads it; verified no behavioural effect |

Both of the first two are superseded by `fault regs`, which records a0..a15,
`WINDOWBASE` and `WINDOWSTART` as they were **at** the fault.

### 194b. Fixed rather than removed

`blk-window` printed `spill ws 0x000002802b` — twenty-one bits of a sixteen-bit
register. It was a hex word and a bit count run together with no separator.

`sbp-post` printed `wb 4294967295 ws 0xffffffff SWEEP LEFT MULTI-BIT`. That is
the never-sampled sentinel, and it has been announcing a defect in a probe that
never ran since `spill_before_parking()` was disabled at step 176. It now says
`never sampled`. Kept rather than deleted: it works again the moment the spill is
re-enabled.

### 194c. The first attempt cascaded, exactly as warned

The panic-dump removals were first done by walking braces outward from each
print. That cut across block boundaries and left dangling fragments — three
compile errors in one build, the same shape §9.3 records. Reverted with
`git checkout` and redone by reading the exact line ranges first. The warning was
right and the shortcut was not worth taking.

### 194d. One probe that could not be removed

`w2c_call0f` writes three instructions of dead instrumentation:

```asm
    movi    a9, g_win_a0
    s32i    a0, a9, 0
    s32i    a1, a9, 4
```

Nothing reads `g_win_a0`/`g_win_sp` any more — the panic line that did was
removed in §194a. They are dead by inspection: stores to a global with no reader.

**Removing them makes `esp_wifi_init_internal` return `0x101`
(`ESP_ERR_NO_MEM`), reproducibly, from a cold boot.**

Bisected to exactly those three instructions. The `rom_call4` prime recorder next
door removes with no effect; removing only this block breaks init; restoring only
this block restores `ESP_OK`.

Three stores to an unread global cannot change program semantics. So the cause is
**positional** — code layout, or the timing of a bridge on the hot path — and it
is the same shape as the step-7 layout band UM-NATOS-042 §9.2 records for
`shell.c`, where nine lines of `uart_puts` hung `blob_map`. That band is still
unexplained.

They stay, with a comment saying all of this. Deleting dead code while the
sensitivity is unexplained costs a working radio to save nine bytes, and the
right time to remove them is after the sensitivity is understood, not before.

### 194e. State

```
boot 11 PASS 0 FAIL
wintorture  10 real switches, checksum 1632 CORRECT
blobphy     phyinit rc=0
wifiinit start
            phyinit rc=1
            init  returned 0x00000000  (ESP_OK)
            start returned 0x00000000  (ESP_OK)
            [intr] src=0 line=27 prio=1 routed=1 nofn=0 fired: none
                   timers=15 refused=0  rng=0xd3016921,0xd3892460
```

Still outstanding: the trace prints `_phy_common_clock_enable` as `_magic`,
because step 185 gave it a trace id outside the 1..116 the name table knows.

**Nothing has been on air.**
---

## step 195 — the layout sensitivity stops being a curiosity

Two small things landed. The third stopped, and the reason it stopped is the
result worth keeping.

### 195a. The trace prints the right names again

`osi_hit` ids 117 and 118 are outside the struct's 0..117 indexing — step 185
appended them for the two `phy_common_clock` stubs because nat-os had already
given 54 and 55 to `_phy_update_country_info` and `_read_mac`. The name table
ends at `_magic` (117), so the dump printed `_phy_common_clock_enable` as
`_magic`. `wifi_osi_name()` now special-cases the two ids rather than resizing
`OSI_N`, which also feeds the null-slot guard's word count.

### 195b. The blob's entry table now carries set_mode and get_mode

`esp_wifi_set_mode`, `esp_wifi_get_mode`, `esp_wifi_scan_start`,
`esp_wifi_connect`, `esp_wifi_set_config` and `esp_wifi_set_channel` are all
exported by the blob. Only the two mode entries were added: setting a mode does
not transmit, and scan and connect do, so they stay out until that is a decision
rather than a side effect.

The fields are appended and **the version stays 4**. Appending is backward
compatible — a kernel whose struct is shorter simply never reads them — and the
version is what makes a stale image a clean rejection rather than a wild call.
It will be bumped when a kernel actually declares them.

### 195c. What stopped: the kernel could not grow to use them

`wifi_bringup()` was extended to call `esp_wifi_set_mode(WIFI_MODE_STA)` between
init and start, reached by `wifiinit sta`. `shell.c` did not grow — the existing
`wifi_start_enable(str_eq(arg,"start"))` became `wifi_start_enable(arg)` and the
parsing moved to the kernel. About twenty-five lines were added to
`wifi_init_cfg.c`.

The board then **watchdog-reset inside `phyinit`**:

```
[phy] &tasks[5]sp 0x3ffb01a0  _phy_stack ...
rst:0x7 (TG0WDT_SYS_RESET)
```

`blobphy` alone reproduces it. `phyinit` runs long before any mode logic, so the
new code is not being executed when it hangs.

**Bisected, because two things had changed at once.** The first attempt reverted
both sides and proved nothing; the second held the kernel fixed and moved only
the blob:

| blob | kernel | phyinit |
|---|---|---|
| v4, no new fields | unchanged | `rc=0` |
| **v4, both new fields** | **unchanged** | **`rc=0`** |
| v5, both new fields | v5, +25 lines | **watchdog reset** |
| v4, both new fields | unchanged | `rc=0` (restored) |

The blob is innocent. Both images are the same size, 606,404 bytes, so nothing
extra was pulled from the archives and `.text` did not move. **The kernel-side
growth is what broke it**, and what it broke is a routine that the added code
never touches.

### 195d. Three of these now, and this one blocks work

| | change | effect |
|---|---|---|
| UM-NATOS-042 §9.2 | nine lines of `uart_puts` added to `shell.c` | hung `blob_map` |
| step 194 | three dead stores **removed** from `w2c_call0f` | init returns `ESP_ERR_NO_MEM` |
| step 195 | twenty-five lines added to `wifi_init_cfg.c` | `phyinit` watchdog-hangs |

Growth and shrinkage, three different files, two of them IRAM-resident rather
than the flash-mapped `shell.c` the original note blamed. The common factor is
that the kernel image moved, and something that is not supposed to care about
that cared.

Up to now this was a documented oddity with a workaround: do not add to
`shell.c`. It is no longer that. It has cost a step-194 cleanup, and it has now
refused an ordinary twenty-five-line feature that is correct by inspection. Any
further work on the driver is one careless edit away from the same wall.

Reverted, and the state is the step-194 baseline with §195a and §195b kept:

```
boot 11 PASS 0 FAIL
wintorture  10 real switches, checksum 1632 CORRECT
blobphy     phyinit rc=0
wifiinit start
            init  returned 0x00000000  (ESP_OK)
            start returned 0x00000000  (ESP_OK)
            [intr] src=0 line=27 prio=1 routed=1 nofn=0 fired: none
                   timers=15 refused=0
```

### Next

**Understand the layout sensitivity.** Not work around it a fourth time. It is
now the thing standing between a driver that initialises and starts, and a driver
that does anything. Everything else on the list — a mode, a scan, the `w2c_*`
save-area overlap — is behind it.

The three known instances give a starting point that earlier ones did not: two
are in IRAM, one in flash-mapped text; one is a removal, two are additions; and
one of them (step 194) is bisected to three specific instructions, which is as
small a reproducer as this is likely to get.

**Nothing has been on air.**
---

## step 196 — a station interface, and the wall has a width

```
   init      returned 0x00000000  (ESP_OK)
   set_mode  STA returned 0x00000000
   start     returned 0x00000000  (ESP_OK)
```

`esp_wifi_set_mode(WIFI_MODE_STA)` succeeds. The driver builds a station
interface where before it was `WIFI_MODE_NULL`.

### 196a. Retried unmodified, then made smaller

Step 195's change was retried exactly as written first. It reproduced the
`phyinit` watchdog reset precisely — **the sensitivity is deterministic, not
flaky**. Worth knowing: a marginal timing race would have passed sometimes.

Then it was shrunk. Comments are free; only instructions move the image. Step
195 added roughly a hundred bytes — a new global, `wifi_start_enable()` rewritten
to parse the shell argument, and the mode call. Dropping the argument parsing
entirely leaves about thirty, and thirty bytes fits where a hundred did not.

`wifiinit start` now means init + `set_mode(STA)` + start. A driver with no
interface is not a state worth keeping a command for, so nothing is lost.

**This measures the wall; it does not remove it.** The sensitivity is to size and
position, and the margin in this file is somewhere between 30 and 100 bytes.
That is the first quantitative thing known about it, and it is worth more than
the workaround.

### 196b. A fourth place for the remap

Asking why no interrupt had fired turned up a real bug.

`osi_impl_set_isr()` files the driver's handler under the line the **driver**
asked for — 0 — while the trampoline that actually runs is the one for the line
we routed it to, 27. The handler was stored where nothing looks; a MAC interrupt
would have found `g_blob_isr[27].fn == 0` and counted itself as `nofn`.

Step 191 recorded that the remap belongs in three places: `_set_intr`,
`_ints_on`, `_ints_off`. **It is four.** A translation applied to some of the
paths that use a number and not all of them is worse than no translation, and
this is the second time in two steps that the count was wrong.

### 196c. It changed nothing, and that is the useful part

```
[intr] src=0 line=27 prio=1 routed=1 nofn=0 fired: none
```

Polled over twenty-five seconds in STA mode. `nofn=0` is the informative half:
not one interrupt arrives even to be counted as unhandled. So the MAC is **idle**,
not mis-routed — which is what a station that has been started and never told to
scan or associate should be.

### State

```
boot 11 PASS 0 FAIL
wintorture  10 real switches, checksum 1632 CORRECT
blobphy     phyinit rc=0
wifiinit start
            init      returned 0x00000000  (ESP_OK)
            set_mode  STA returned 0x00000000
            start     returned 0x00000000  (ESP_OK)
            [intr] routed=1 nofn=0 fired: none  timers=14 refused=0
```

The driver initialises, has a station interface, and is started. It receives
nothing and sends nothing.

The next action is a **scan**, and that is the first one that would put energy on
air. It is not taken here.

**Nothing has been on air.**
---

## step 197 — the receiver is live

```
[intr] routed=1 nofn=0 fired: L27=1
[intr] routed=1 nofn=0 fired: L27=9
```

The WiFi MAC raises interrupts and nat-os services them. The interrupt matrix,
the line remap, `_handler_level3`, the trampoline and the driver's own ISR all
work end to end. Nothing had ever fired before.

### 197a. Three things that returned ESP_OK and changed nothing

`set_mode(STA)`, then `esp_wifi_set_channel(1)`, then
`esp_wifi_set_promiscuous(true)` were added one at a time. Every one returned
`ESP_OK`. Not one produced an interrupt.

`nofn` stayed **0** throughout, and that is the half that mattered: not one
interrupt arrived even to be counted as *unhandled*, so the fault was not in the
routing. The hardware agreed:

```
intenable = 0x08808000    bits 15, 23, 27 -- our line IS unmasked
interrupt = 0x00010060    bits 5, 6, 16   -- bit 27 never pending
```

Correct plumbing, silent MAC.

### 197b. The entries that turn the radio on were empty

```
_phy_enable          osi_hit(53u);
_wifi_clock_enable   osi_hit(62u);
_wifi_clock_disable  osi_hit(63u);
_wifi_reset_mac      osi_hit(61u);
```

All four counted the call and did nothing. The driver calls them to ungate the
WiFi clock and pulse the MAC out of reset, and neither happened — the same
**success reported for work never done** as `_read_mac` (step 186) and
`_get_random` (step 193), except that here the work is powering the radio.

Three of them now write the registers, with Espressif's own constants from
`soc/esp32/dport_reg.h`:

| register | address | bits |
|---|---|---|
| `DPORT_WIFI_CLK_EN_REG` | `0x3FF000CC` | `0x406` |
| `DPORT_CORE_RST_EN_REG` | `0x3FF000D0` | `BIT(2)` |

`_phy_enable` is deliberately left empty — `phyinit_run_at()` already does the
one-time PHY bring-up, and it is the next candidate if more is needed.

### 197c. What is on air

**Nothing has been transmitted.** No scan, no probe request, no association, no
frame of any kind has left this board.

What is new is that the **receiver** is running. Promiscuous mode on channel 1,
taking interrupts from frames that were already in the air. Receiving is not
emitting, and doing it in this order was the point: the entire RX path is now
proven without a single transmission.

The entry table gained `set_channel`, `scan_start` and `set_promiscuous`
(version 7). Only the two receive-side entries are called. **`scan_start` is
present and not invoked** — an active scan transmits, and that stays a decision.

### 197d. What the counts say

Single digits over seconds, where a busy 2.4 GHz channel would give hundreds of
beacons. So the ISR fires but the driver is probably not consuming or re-arming
fully.

That is a question about the RX path rather than about whether there is one, and
it is the next thing to measure.

```
boot 11 PASS 0 FAIL
wintorture  10 real switches, checksum 1632 CORRECT
blobphy     phyinit rc=0
wifiinit start
            init      ESP_OK     set_mode STA  ESP_OK
            start     ESP_OK     promisc       ESP_OK
            channel 1 ESP_OK     L27 firing
```

**Nothing has been transmitted.**
---

## step 198 — modem sleep, tested; and a correction to step 197

### 198a. The correction

Step 197 said "the receiver is live" and "taking interrupts from frames already
in the air". That was over-read from one sample. Repeated measurement:

```
poll 1   fired: (none)
poll 2   fired: L27=1
poll 3   fired: L27=1
```

`L27` reaches one, occasionally a few, and stops. One earlier run read 1 then 9,
which looked like a climbing count and is what the claim was built on. **It does
not reproduce.** The MAC raises an interrupt or two around start-up and goes
quiet.

What is true, and is not small: the MAC raises interrupts where it never did, so
the whole path — matrix, line remap, `_handler_level3`, trampoline, the driver's
own ISR — is proven end to end. What is not true is **sustained reception**.

One sample was enough to see a signal and not enough to describe it, and the
report was written before the second measurement. That is the same error as
step 190's, three steps later.

### 198b. The hypothesis, and it was a good one

Modem sleep was proposed as something that would stop the radio starting without
reporting an error. It is real, and documented in `esp_wifi.h` on
`esp_wifi_set_ps`:

> **`@attention Default power save type is WIFI_PS_MIN_MODEM.`**

with `WIFI_PS_MIN_MODEM` described as *"station wakes up to receive beacon every
DTIM period"*. An unassociated station has no DTIM to sync to, and nothing
reports an error either way.

**Tested.** `esp_wifi_set_ps(WIFI_PS_NONE)` added to the entry table and called
after start. It returns `ESP_OK` and the interrupt count does not change.

So power save is not what is holding this back. The hypothesis was sound,
documented, and worth the two builds — a mechanism that fails silently is exactly
the class this investigation keeps finding, and ruling it out is worth as much as
confirming it would have been.

### 198c. What chasing it found

ESP-IDF's `esp_phy_enable()`:

```c
if (!s_is_phy_calibrated) { esp_phy_load_cal_and_init(); ... }
else                      { phy_wakeup_init(); }
```

Calibrate on the first call; **wake the PHY on every one after**. And
`_phy_disable()` is `phy_close_rf()`, which sleeps it.

Both of ours were empty. Anything that slept the radio would have left it asleep
permanently and silently — precisely the failure described. `_phy_enable` now
calls `phy_wakeup_init()`.

It did not change the count either, so the PHY was evidently not asleep. The
entry is correct now regardless, and the failure it would have caused was real.

### 198d. Two implementation notes

**`blob_map()` is not a getter.** It reprograms the flash MMU with the cache off.
Calling it from inside a blob call — while executing out of the mapping it is
rewriting — is an `IllegalInstruction`, measured. The address is cached at
bring-up instead, while nothing is running out of the blob.

**`phy_wakeup_init` is vendor code called from a vendor stub**, so it is a plain
windowed-to-windowed call: no bridge, no blob mutex, no pin. Only the address
crosses the ABI boundary.

### State

Entry table at version 9: `set_channel`, `scan_start`, `set_promiscuous`,
`set_ps`, `phy_wakeup`. **`scan_start` is present and still not invoked.**

```
boot 11 PASS 0 FAIL
wintorture  10 real switches, checksum 1632 CORRECT
blobphy     phyinit rc=0
wifiinit start
            init / set_mode STA / start / ps NONE / promisc / channel 1
            all ESP_OK,  fired: L27=1
```

**Nothing has been transmitted.**
---

## step 199 — scan_start invoked, and the transmit claim is retired

```
channel 1 returned 0x00000000
scan      returned 0x00000000  after ticks 13
```

`esp_wifi_scan_start()` runs and returns `ESP_OK`. Interrupt activity rises with
it: `L27` climbs 0 → 6 over a twenty-second poll, where it previously sat flat
at 1.

### 199a. The claim that can no longer be made

Every report so far ended "nothing has been on air", later "nothing has been
transmitted". **That ends here** — and not because a transmission was observed,
but because it can no longer be ruled out.

The scan is configured **passive**. The layout of `wifi_scan_config_t` comes from
the Arduino-ESP32 `esp_wifi_types.h`, the closest source of truth available, but
**not provably the same IDF vintage as this blob**. `esp_wifi_scan_start` does
not read the struct itself — it passes the pointer down — so confirming the
offset of `scan_type` would mean chasing several more functions, and that was not
done. If the offset differs, the field reads 0, which is
`WIFI_SCAN_TYPE_ACTIVE`, and the scan transmits probe requests.

The risk was **bounded rather than eliminated**: `channel` is pinned to 1, so the
exposure if the layout is wrong is one channel rather than a sweep of fourteen.

A plan to tell the two apart by duration — 1500 ms of passive dwell against
roughly 120 ms of active — **did not work**. `block = 0` makes the call return as
soon as the scan is *initiated*, so the thirteen ticks measured are call
overhead, not scan time. The test was designed before the parameter was chosen,
and the parameter invalidated it.

So: a scan was started, its mode is unverified, and the honest statement is that
**a transmission may have occurred**. Saying otherwise would assert something not
measured, which is the failure this log has spent two hundred steps trying not to
commit.

### 199b. `block = 1` hangs, and why

A blocking scan waits for `WIFI_EVENT_SCAN_DONE`. `_event_post` is still a stub
returning 0, so nothing is ever posted and the wait cannot end — measured: the
call never returned and the shell task stayed inside it.

That is UM-NATOS-042 §9.5's *"event callbacks never fire"*, reached at last, and
it is the next real piece of work. Nothing above the MAC can complete a
request-and-wait without it.

### 199c. Ordering

The scan block was first placed before `set_channel`, and `set_channel` then
returned `0xffffffff`: the channel cannot be changed while a scan is running.
Moved after it, both return `ESP_OK`.

### State

```
boot 11 PASS 0 FAIL
wintorture  10 real switches, checksum 1632 CORRECT
blobphy     phyinit rc=0
wifiinit start
            init / set_mode STA / start / ps NONE / promisc /
            channel 1 / scan   -- all ESP_OK
            L27 climbing, 0 -> 6 over twenty seconds
```

### Next

1. **`_event_post`.** It gates every blocking driver call, and a scan that cannot
   report completion cannot yield results.
2. **Verify the scan config layout**, which would let passive be claimed rather
   than intended.
3. Sustained reception is still not established: 6 interrupts in twenty seconds
   is activity, not a receiver.

**A scan has been started. Whether it transmitted is unverified.**
---

## step 200 — the event groups were never connected

The timer trap, a third time. `osi_impl_evt_create`, `_delete`, `_set`, `_clear`
and `_wait` have existed in full the whole time, and every stub was hardcoded:

```c
_event_group_create      return 0;      /* a NULL event group          */
_event_group_set_bits    return 0;      /* nothing ever signalled      */
_event_group_clear_bits  return 0;
_event_group_wait_bits   { uint32_t r = 0u; if (r) { return r; } }
```

That last is worth reading twice. The "try without blocking" fast path assigned
zero to a local and tested it. It never called anything.

### 200a. And a real argument bug underneath

`osi_impl_evt_wait` takes **five** arguments — `(h, bits, clear_on_exit,
wait_for_all, ticks)` — and the widest windowed-to-call0 bridge carries three.
The stub called it with three, so `block_time_tick` landed where
`clear_on_exit` belongs, and `wait_for_all` and `ticks` were whatever the
registers happened to hold.

Rather than add a `w2c_call5` to `window.S` — the file step 194 showed is
sensitive to its own size, where removing three *dead stores* broke init — the
three flag arguments are handed over first through `osi_impl_evt_wait_args()`
and the wait then needs only two. No packing, so nothing is lost: `ticks` may
legitimately be `0xFFFFFFFF`.

### 200b. It did not unblock the blocking scan

`esp_wifi_scan_start(cfg, 1)` still never returns, and still leaves the shell
task inside it, identically to before.

The theory was that the driver waited on an event group that could never be
signalled. **It did not.** `_event_post` — still a stub returning 0 — remains the
suspect. That is recorded in the comment at the call site so the next person does
not retry it.

Kept anyway, and not as consolation. A NULL event group handed to a driver that
will call `set_bits` and `wait_bits` on it is a live defect whatever the scan
does, and the five-into-three call was corrupting two arguments on every use.

### State

```
boot 11 PASS 0 FAIL
wintorture  10 real switches, checksum 1632 CORRECT
blobphy     phyinit rc=0
wifiinit start
            init / set_mode STA / start / ps NONE / promisc /
            channel 1 / scan   -- all ESP_OK
```

### Next

`_event_post`. It is now the only remaining candidate for the blocking scan, it
gates every request-and-wait above the MAC, and it is the last of the four
"success reported for work never done" entries on the live path.

**A scan has been started. Whether it transmitted is unverified.**
---

## step 201 — the blocking scan completes, and it is provably passive

```
scan      returned 0x00000000  after ticks 171
```

`esp_wifi_scan_start(cfg, block=1)` completes. It had hung on every previous
attempt.

### 201a. It was not waiting on an event

Two guesses had already been wrong — the event groups in step 200, `_event_post`
before that. Reading the blob instead of guessing a third time, the blocking
branch is a **poll loop**:

```asm
movi   a10, 100
l32i   a3, a3, 160     ; field 40 = _task_ms_to_tick
l32i   a7, a3, 156     ; field 39 = _task_delay
callx8 a3 ; callx8 a7  ; until the scan id changes
```

It paces itself with **`_task_delay`**, which was empty — the fourth entry this
investigation has found where the implementation existed in full and the stub
counted the call and returned. `osi_impl_delay()` has been there all along. With
it empty the loop never yielded, so the driver task it was waiting for could not
make progress.

Wired with the same shape as `_semphr_take`, since it blocks from windowed code:
spill to one frame, drop the blob lock, sleep, take it back.

### 201b. And the scan is provably passive

Step 199 could not claim this. The `wifi_scan_config_t` layout came from a header
not provably matched to this blob, and if `scan_type` sat at a different offset
the field would read 0 — `WIFI_SCAN_TYPE_ACTIVE` — and transmit. The duration
test planned there did not work, because `block=0` returns before the scan runs.

With `block=1` it does work. Changing **only** the passive dwell:

| dwell | measured |
|---|---|
| 1500 ms | 171 ticks = 1710 ms |
| 500 ms | 80 ticks = 800 ms |

A 1000 ms change in the config moved the duration by 910 ms — one for one. **The
blob is reading the struct at the offsets assumed**, so `scan_type` at +12 is
read correctly too, and it holds 1.

So the scan is passive on evidence rather than intent. It listens and does not
transmit — and since every earlier scan used the same config and the same code
path, those were passive as well.

**"Nothing has been transmitted" is reinstated**, and it is now a measurement
rather than an assumption. Step 199 retired it for the right reason; this
restores it for a better one.

Dwell left at 500 ms: still four times longer than an active scan would take, so
the distinction stays observable, and bring-up is not blocked for two seconds.

### State

```
boot 11 PASS 0 FAIL
wintorture  10 real switches, checksum 1632 CORRECT
blobphy     phyinit rc=0
wifiinit start
            init / set_mode STA / start / ps NONE / promisc /
            channel 1 / blocking passive scan   -- all ESP_OK
```

Sustained reception is still not established — `L27` sits at 2 — and that remains
the open question.

**Nothing has been transmitted, and this time it is measured.**
---

## step 202 — measure reception instead of inferring it

```
scan      returned 0x00000000  after ticks 80
scan      ap_num rc 0x00000000  found 0
```

`esp_wifi_scan_get_ap_num()` added to the entry table (version 10) and reported
after the scan. **A 500 ms passive dwell on channel 1 finds zero access
points.**

That is the measurement this has needed. Interrupt counts were an *inference*
about reception — "`L27` climbed to 6" reads like progress and says nothing about
whether a frame was decoded. `ap_num` is the driver's own answer to the only
question that matters, and the answer is no.

### 202a. Two ISR entries, fixed, and not the cause

`_queue_send_from_isr` returned 0 and posted nothing. The MAC ISR hands each
received frame to the driver task through it, so every frame would have been
dropped and reported as a failed post. `_is_from_isr` answered `false`
unconditionally, which tells the driver it may use blocking variants from an
interrupt; it now reports whether the trampoline is on the stack.

Both are real defects and both are kept. But `osiused` shows **the driver never
calls either**, so neither is on the path that is failing.

Fixing things because they are broken is right. Claiming they were the cause
would not have been, and the temptation was there — the first was a very good
story for "two interrupts then silence".

### 202b. A second defect: the all-channel scan panics

Setting the config's `channel` to 0 — scan all channels — panics:

```
exccause 28  LoadProhibited,  during config, before init returns
```

Reproducible. **It is not the layout sensitivity**, and that was checked rather
than assumed: the log string was padded back to the exact length of the working
build and it still panicked, then reverting only the channel field made it work
again. So the all-channel scan path reaches something the single-channel path
does not.

Recorded, not chased.

### State

The driver initialises, has a station interface, starts, sets a channel, and runs
a blocking passive scan to completion — that hears nothing. 53 of 118 adapter
entries are exercised.

```
boot 11 PASS 0 FAIL
wintorture  10 real switches, checksum 1632 CORRECT
blobphy     phyinit rc=0
wifiinit start
            init / set_mode STA / start / ps NONE / promisc /
            channel 1 / passive scan   -- all ESP_OK, found 0
```

### Next

The question is now narrow and well-posed: **the radio decodes nothing.** Not
"interrupts are low" — nothing. Candidates, none tested:

1. RX buffers. The driver allocates them at init from a config nat-os trimmed
   hard; `_wifi_malloc` and friends only started working at step 182, so the
   counts have never been checked against what the driver actually got.
2. `_wifi_apb80m_request`/`_release` and `_wifi_rtc_enable_iso`/`_disable_iso`,
   all still empty, all clock- and power-domain related.
3. The PHY calibration data. `phyinit_run_at()` runs `register_chipv7_phy` with a
   calibration buffer nat-os builds; whether the result is a working RF front end
   has never been checked beyond `rc=0`.

**Nothing has been transmitted, and it is measured.**

---

# steps 203–259 — RECOVERED, not contemporaneous

**Read this before trusting the entries below.**

This log stopped being written at step 202 on 2026-08-24. The next commit,
`4cf3490`, is titled *"step 203-204 of next_moves/08"* — still citing this file
by name, still numbering against it — but did not write to it, and neither did
any of the thirty-two commits after it. The file kept being the **numbering
authority** while ceasing to be the **record**.

Nothing announced the change. What displaced it is visible in the same day's
commits: `e66a0db`, UM-NATOS-048. The UM reports had taken over the narrative,
each one covering a range, and once a polished report covered a range, writing
those steps here as well was duplicated work. Reasonable in the moment; nobody
wrote down that it had happened, and the gap then ran for fifty-seven steps.

**Steps 203–259 below were reconstructed on 2026-08-26 from the commit messages
and the UM reports.** They are not what was written at the time, because nothing
was written at the time. Every measurement quoted is one that appears in a
commit message, which is where the contemporaneous record actually lives — so
the numbers are as good as they ever were, but the *ordering of thought* within
a step is recovered rather than recorded. Where a step's reasoning matters, the
commit hash is given and it is the primary source.

The entries are also **shorter than steps 1–202**. That is deliberate and not an
apology: the full account of 203–235 is in UM-NATOS-048, 049 and 050, and of
236–259 in UM-NATOS-051. Repeating 200 KB of prose that already exists in four
reports would make this file longer without making it more useful. What is kept
here is what a working log is *for* — the measurement, the conclusion, and above
all **what was eliminated, so it is not retried.**

| steps | report | commits |
|---|---|---|
| 203–216 | UM-NATOS-048 | `4cf3490` … `b9caecf` |
| 217–230 | UM-NATOS-049 | `7eef249` … `4fd9262` |
| 231–235 | UM-NATOS-050 | `2dd142c` … `076c40e` |
| 236–259 | UM-NATOS-051 | `d599ebf` … `3adb09f` |

---

## steps 203–204 — sweep all thirteen channels, and four candidates

`4cf3490`

```
ch 1 found 0   ch 2 found 0   ...   ch 10 found 0
```

Step 202 found zero access points on channel 1 and named three candidates. The
sweep widens the question: the receiver is deaf **everywhere**, not on one
channel, so "the AP is on channel 6" is dead.

Four candidates tested, three eliminated by measurement rather than argument:

- **AMPDU.** RX/TX aggregation was enabled in `g_cfg` while the comment three
  lines above said it should not be. Turned off. Still found 0. **Kept anyway**
  — the code now matches its own documented intent and the driver carries less
  state.
- **Buffers.** The driver allocates RX and management buffers at init and
  `_wifi_malloc` only started working at step 182, so nothing had ever checked
  the result. It gets everything it asks for. Eliminated.
- The remaining two are in the commit.

**Eliminated: aggregation, buffer starvation.** Do not retry either.

---

## step 205 — THE RADIO HEARS

`b9c1b32`

```
ch 6  rc 0x00000000 found 1
ch 11 rc 0x00000000 found 1
```

An access point. Off the air. Decoded by this driver, on this board, after two
hundred and four steps of finding nothing.

**What was missing was a whole initialisation step**, not a tuning problem.
Reading the blob rather than guessing at it:

```asm
l32r  a5, <g_ic>          ; 0x3ffd526c
l32i  a6, a5, 0x1b4       ; g_ic->wpa_cb
```

`g_ic->wpa_cb` had been NULL since the driver first initialised. A NULL table
faults in `cannel_scan_connect_state`, which checks the *function*; an all-zero
table faults in `wifi_station_start`, which checks the *table*. Neither "leave
it NULL" nor "hand it zeros" is right, so every slot points at one stub that
records where it was called **from** — the driver names the entries it needs
instead of a static scan guessing them.

That technique — record the blob's own return address and let it tell you —
is used again at steps 211 and 247.

---

## step 206 — an SSID, off the air, by name

`a105734`

```
ch 6   found 1   ap[0] bssid 44:25:38:xx:xx:xx  ssid [T....]
ch 11  found 1   ap[0] bssid 7e:26:f6:xx:xx:xx  ssid [i.........]
```

An SSID is a string transmitted by somebody else's hardware. It can only have
arrived through the antenna, the PHY, the MAC, the interrupt, the queue and the
worker — so one of them coming out proves every one of those works.

**Redacted on purpose:** this repository is public and a BSSID is a geolocation,
resolvable against public wardriving databases. What survives is the OUI, and
the OUI is the point — `44:25:38` is Technicolor and the SSID it came with is
the Technicolor default form, so the vendor prefix and the name corroborate each
other.

Only record **zero** is read. The stride between records is not stated by any
header this project can check, and reading a second at a guessed offset would be
the exact error step 199 refused to make with `scan_type`. See step 212.

---

## steps 207–208 — measure the reception instead of worrying about it

`504e3ff`

```
summary  ch6=1/3 max1  ch11=3/3 max1
ch 11  ssid [i.........]  rssi -61 dBm
ch  6  ssid [T....]       rssi -91 dBm
```

Step 206 called the reception "marginal" on the evidence that one channel found
an AP at 150 ms and none at 600 ms. That was the right worry and the wrong
conclusion, and **the difference is one number.**

−61 dBm is a strong, ordinary signal, heard on 3 passes of 3, every run, same
BSSID. −91 dBm is the noise floor, where *any* receiver is intermittent. The
receiver is not marginal; one of the two access points is far away.

A worry that a measurement can settle should be settled, not carried.

---

## step 209 — NAT-OS TRANSMITS

`d9babc1`

```
tx  beacon 70 B, ch 1, ssid [nat-os-transmitting] x500
tx  accepted 500  refused 0        (62 s, 12.5 ticks/beacon)
```

**And it was seen.** A phone, several feet away, listed the SSID in its network
picker — a device this project does not control, running software it did not
write, displaying a string this code chose. It could only have arrived through
the antenna.

Every report from step 1 to step 208 ended "nothing has been transmitted", and
every one was true and measured. That sentence retires here, on external
evidence rather than on a return code. **`rc` is not evidence** — the phrase
enters the vocabulary at this step and is quoted for the rest of the log.

---

## step 210 — the adapter table audited; `_env_is_chip` lied

`ffc1424`

UM-NATOS-048 §10 says six adapter entries have had a plausible body and the
wrong semantics, and that the untested ones should be **assumed** to be the same
rather than hoped not to be. This acts on that assumption: all 116 stubs scanned
for bodies that reach no implementation. Sixty-four do not.

Most are harmless — coexistence is dead code without Bluetooth, NVS is off by
configuration, log entries and DPORT stalls do nothing on one core. Two were
not, including `_env_is_chip` returning false and a clock reading zero.

---

## step 211 — `_event_post`, and a retraction

`3f2f435`

**The leak that wasn't.** UM-NATOS-048 rev 1.0 named a leaked
`g_wifi_global_lock` as the highest-priority open defect in the WiFi
implementation. **There is no such leak**, and the report is corrected to rev
1.1 with a retraction.

`_mutex_lock` and `_mutex_unlock` were instrumented to record the blob's own
return address for every acquisition and pop it on release — the technique from
step 205. The acquisition stack came back **empty**. Exactly balanced.

A report that names a defect which does not exist sends the next person into the
wrong file. The retraction is the work, not an embarrassment.

---

## step 212 — the record stride is 84, read out of the blob

`11f4b05`

Step 206 read only record zero and said why. Step 211 measured ch11 reporting
**two** access points for the first time, which promoted the stride from a
curiosity to an under-report — and made it measurable: ask for both, zero the
buffer, and look at where the second lands.

**84 bytes**, measured rather than assumed.

---

## step 213 — the unwired entries audited: no live defect

`9e921bd`

The assumption from §10 tested instead of carried. All 116 stubs scanned; 64
reach no implementation; `osiused` then says which of those the driver actually
calls. The answer changes the priority list, and **two entries that looked like
stubs are correct as written.**

---

## steps 214–216 — the layout sensitivity was a wild write

`a5aafd6`, `136bb6e`, `b9caecf`

```
[blobtask] req en=1 stack=6656 prio=23 fn=0x4036bb64
init returned 0x00000000    start returned 0x00000000
```

— with the step-194 probe **removed**. Three dead stores in `window.S` had been
kept since step 194 because deleting them made `esp_wifi_init_internal` return
`ESP_ERR_NO_MEM`, reproducibly, from a cold boot. They are deleted here and
everything works.

**The wild write is found, and it was the probe itself:**

```
3ffb0d70 B g_win_sp
3ffb0d74 B g_win_a0     <- AFTER g_win_sp, not before
```

`window.S` stored through `g_win_a0` at offsets 0 and 4 on the assumption that
`g_win_sp` — a separate global, declared on the next line in C — followed it.
The linker put them the other way round. Assembly-backed storage is now declared
honestly.

**Step 216 lifts a constraint that has stood since step 102.** UM-NATOS-042 §9.2
said *"do not add uart_puts lines to shell.c"* because of an apparent layout
band. Tested: +9 lines and +120 lines, both `rc=0`, init and start `ESP_OK`, 20
beacons, ch6 and ch11 both found. **It does not reproduce.** The constraint was
the wild write all along, and UM-NATOS-042 recommendation 2 is marked
superseded.

A rule kept for a hundred steps on evidence that no longer holds is a tax on
every future change. Retest old constraints when their cause is found.

---

## step 217 — the association path works: reason 201

`7eef249` · entry table v13

```
assoc  set_config rc 0x00000000
assoc  connect    rc 0x00000000   ssid [nat-os-no-such-network]
[evt]  posted 2   id=2  id=5(len22 reason201)
```

id 2 is `STA_START`, id 5 is `STA_DISCONNECTED`, reason 201 is `NO_AP_FOUND`.
The driver accepted a station config, began an association, looked for a network
that cannot exist, and said so. **A deliberately impossible SSID is the control**
— the correct answer is a specific failure, and getting it proves the path.

---

## step 218 — real credentials, and the association stalls

`fc1f4d3`

```
assoc  connect rc 0x00000000   ssid [<operator's network>] pass 13 chars
[evt]  posted 1    id=2
wpa hits 5
```

Thirty seconds. `STA_START` and nothing else — no CONNECTED, no DISCONNECTED, no
reason code. **The driver does not fail; it STOPS.** `cnx_connect_next_ap` loads
`wpa_cb + 8` and calls it, and offset 8 is `wpa_sta_connect`, which was a
recording stub returning 1 and doing nothing.

---

## step 219 — `wpa_sta_connect`, and ASSOC_FAIL

`78f2b23` · entry table v14

```
[evt] id=2  id=5(len12 reason203 ASSOC_FAIL)
```

**Read from the source, not guessed.** ESP-IDF's `wpa_sta_connect` is
`wpa_config_profile` → `wpa_config_bss` → `esp_wifi_sta_connect_internal`. The
first two are the WPA half, which nat-os does not have. The last line is what
moves the driver, and for an open network it is very nearly the whole function.
That line, and nothing else, is implemented.

**A prediction is recorded here before it can be tested:** an open network should
associate; a protected one should not, but should now fail with a reason code
instead of silence. Confirmed at step 221.

Also fixed: the SSID length self-check was hardcoded to 22 — the length of step
217's impossible SSID — so a real network of a different length read as
"layout?" when the layout was fine. **A self-check that only passes for one input
is not a self-check.**

---

## step 220 — `wpa_cb` return values are per entry

`1d47808`

```
ivory-billed      WPA2-PSK   reason 203 ASSOC_FAIL
a phone hotspot   WPA3-SAE   reason 202 AUTH_FAIL
```

Two networks, two different failure points, one cause. Both are "no supplicant",
one step apart — and the *difference between them* is a far sharper statement
than "association fails".

**Both blanket answers were measured wrong.** Returning 1 everywhere crashed
against the WPA3 hotspot — `LoadProhibited`, `excvaddr 0x00000001`, inside a ROM
copy routine — because `struct wpa_funcs` contains pointer-returning entries and
the driver took the 1 as a pointer. Returning 0 everywhere hung before
`set_mode` and watchdog-reset the board, because the bool entries read false as
refusal.

`esp_wifi_driver.h` is on the build machine and says which of the 25 entries
returns what. **Reading the header cost one grep; guessing cost a crash and a
watchdog reset.**

---

## step 221 — ASSOCIATED

`9a3588d`

```
assoc  connect rc 0x00000000   ssid [ABCDE] pass none
[evt]  id=2  id=4(len5 ch1 auth0 CONNECTED)
```

`WIFI_EVENT_STA_CONNECTED`. nat-os is a station on somebody else's access point
— scanned for it, authenticated, associated, and stayed associated through
thirty-nine subsequent scans.

**Step 219's prediction is confirmed.** Predicting first is what turned a lucky
outcome into a measurement.

---

## steps 222–225 — the data path, and a DHCP OFFER decoded

`d4c9ae6` · entry table v15

```
dhcp  DISCOVER 291 B  tx rc 0x00000000  sent
rx    frames 1 bytes 352
  len 352  dst ffffffffffff  src <AP>  type 0x0800 IPv4
  DHCP reply, offered 10.224.203.139  from 10.224.203.104
```

nat-os built an Ethernet II / IPv4 / UDP / DHCP packet by hand and put it on the
air. The RX callback is **windowed** — the driver calls it with `callx8` — and
does the least possible: copy into a ring, free the driver's buffer, return.

**It must free the buffer.** Not calling
`esp_wifi_internal_free_rx_buffer` leaks until the pool empties and reception
stops — a failure that would look like the radio going deaf rather than like a
missing free.

**Silence is not a measurement.** The first reading was `frames 0`, and it meant
nothing: consistent both with a quiet hotspot and with an unwired receive path.
A DHCP DISCOVER separates them because a server is *obliged* to answer.
Provocation beat observation, exactly as the beacon did at step 209.

Two wrong turns first: `frames 0` was measured while the thirteen-channel sweep
was running — a receiver on the wrong channel is not evidence of a quiet network
— and promiscuous mode was suspected and **disproved, not left hanging.**

---

## steps 226–230 — NAT-OS ANSWERS A PING

`4fd9262`

```
ping 10.224.203.200  ->  reply
```

ARP, ICMP echo and the client half of DHCP in `kernel/net.c`, about three
hundred lines, in a kernel with no network stack and no libc. **Confirmed from
another machine on the same network** — the verdict comes from a device this
project does not control, the same standard the beacon met at step 209. A
counter incremented on this board would prove nothing.

DHCP got an OFFER but never an ACK; step 230 shipped a hardcoded address to work
around it. See step 231.

---

## step 231 — DHCP completes: the ring truncated the OFFER

`2dd142c`

```
net  DHCP ACK -- address 10.224.203.139
net  +60s frames 102 arp 3 icmp 61 dhcp 1/1 drop 0/0 [IP]
```

**The bug was the ring size, and it was mine.** `NET_MAX` was 160 bytes, chosen
because a ping is 74 bytes from Windows and 98 from Linux — and never checked
against the *other* protocol in the same file. DHCP options begin at
`14 + 20 + 8 + 240 = 282` bytes in; the OFFER measured 352. Every reply was
truncated before its options, and the option scan then walked a region that did
not exist.

**How it presented is why it took two passes.** The counter read `dhcp 0/0`: not
"the OFFER was rejected", which points here, but "no OFFER was ever seen", which
points at the receive or transmit path.

Truncation is now counted alongside the ring-full drop count. A silently
shortened frame is the shape this project keeps finding.

---

## steps 232–234 — lwIP runs on nat-os

`d120912`

```
lwip  DHCP bound -- address 10.224.203.139
lwip  +40s  rx 75  drop 0  tx 52  err 0
```

A real TCP/IP stack on a kernel with no libc, over a driver this project
reverse-engineered the interface to. `NO_SYS=1`, so the port layer reduces to
`sys_now()` and a netif driver. DHCP negotiated by lwIP's own client, pings
answered by lwIP's own ICMP, confirmed from another machine.

---

## step 235 — TCP: nat-os serves a web page

`076c40e`

```
http  listening on port 80
      http conns 4 reqs 4 errs 0
```

A browser rendered a page served by this kernel: 802.11 association, Ethernet,
ARP, IPv4, TCP, HTTP in one transaction — **and TCP had been compiled in since
step 232 without a single byte ever passing through it.** Compiled is not
exercised.

---

## steps 236–237 — the RSN IE, and a reading that was wrong for fifteen steps

`d599ebf` · entry table v16

```
step 219   reason 203 ASSOC_FAIL   association rejected, no RSN IE
now        reason 39  TIMEOUT
```

Twenty-two constant bytes. For WPA2-PSK with CCMP the RSN information element is
a **constant** — a declaration of what the station intends to negotiate, not
proof that it can. No crypto is involved.

**Step 219's hypothesis is confirmed:** the WPA2 failure was the missing RSN
element.

**And the reading of `203 → 39` recorded here is WRONG.** It was written as "the
RSN IE gets WPA2 past association" and then hardcoded into the reason table as
the string `" TIMEOUT(associated, no handshake)"`, where it stopped looking like
a hypothesis. Nine steps took it as their premise. See **step 246**, which
measures it, and **step 252**, which finds the mechanism: 203 was the access
point *answering*; 39 is the access point no longer able to read the question.

**A flash-versus-RAM fault worth keeping.** The element was first declared
`static const uint8_t[22]` — `.rodata`, flash-mapped at `0x3F4xxxxx` — and
handing that pointer to the driver produced `exccause 3 LoadStoreError`,
`excvaddr 0x3f40b334`. **A buffer handed to the blob must live in RAM.**

---

## steps 238–239 — vendor the crypto, prove it against published vectors

`6378f8c`

```
wpa  crypto self-test (IEEE 802.11i vectors)
  PASS pbkdf2 #0    PASS pbkdf2 #1    PASS pbkdf2 #2
  3 passed, 0 failed
```

Two routes sized before either was started: port ESP-IDF's `rsn_supp` (5,660
lines, five protocols where nat-os needs one) or hand-write everything (~1,900
lines, including hand-rolled SHA-1 and AES — where subtle, silent,
security-relevant bugs live). **Neither.** Vendor the crypto, hand-write the
state machine: seven files, 234 KB.

**The deciding argument was testability.** Crypto has published vectors, so it
can be checked on the bench with no network and no ambiguity. A handshake that
fails because PBKDF2 is off by one iteration is indistinguishable from a dozen
other faults and would be debugged over the air, twenty seconds at a time.

That decision pays at step 258.

---

## step 240 — the crypto moves to the windowed ABI

`32749e0`

`esp_wifi_set_sta_key_internal` takes **nine arguments** and the call0-to-windowed
bridges carry four, so the handshake must be windowed to install a key at all.
Compiling the crypto windowed removes every bridge from the path.

**Two ABI violations, both mine, both the same rule.** `wpatest.c` became
windowed and kept calling call0 `uart_puts` — `LoadProhibited`, `excvaddr 0`.
Then the windowed crypto called `os_memcpy`/`os_memset`, macros onto call0
`kstring.c` — `IllegalInstruction` with `epc == a0`, a windowed return address
leaking into PC.

UM-NATOS-048 §9.2 wrote down the tell: **a layout fault MOVES when sizes change;
an ABI fault does not.** It was written down and it was used.

Also measured here and **ignored three steps later**: PBKDF2 costs most of a
minute for three derivations, and "should not be recomputed per association".
See step 243.

---

## steps 241–242 — the handshake, written and NOT YET REACHED

`ecd5d68` · entry table v17

```
4way pmk=1 st=0 m1=0 m3=0 done=0 micbad=0 txerr=0 poll=0
```

The handshake exists — msg1 to msg2, MIC verification, GTK unwrap, key
installation, msg4 — it compiles, it links, and **it has never run.** `m1=0` says
`wpa_sta_rx_eapol` was never called.

**Committed anyway and labelled as unexecuted**, because the next person needs
the state to be honest rather than encouraging. Every line below the PMK is
written from the specification and none of it has met an access point.

A third ABI fault on the way: GCC turned the *inlined* `os_memcpy` back into a
call to `memcpy`, because an aggregate initializer (`u8 seq[6] = {0,…}`) is a
language-level copy that `-fno-builtin` does not touch.

`wpa_sta_in_4way_impl` is **bounded** — answering "still in a handshake" forever
made the driver spin at HIGH priority and watchdog-reset the board. A bounded
answer turns a hang into a diagnosable timeout.

---

## step 243 — derive the PMK off the driver's connect path

`80a4b23`

```
wpa   PMK ready after 15 s
evt   reason39
```

**PBKDF2 was running inside the driver's connect callback.** `wpa_sta_connect` is
called by the driver from `cnx_connect_next_ap`, mid-association, and
`wpa_hs_arm()` was deriving the PMK there. Measured: **15 seconds.** The
association is long dead before message one would arrive — which is exactly the
silence step 241 observed.

Step 240 measured that cost and wrote down that it should not be per-association.
It was then put in the per-association path anyway, three steps later.
**Measuring a thing does not help if the measurement is not used**, and the note
that would have prevented this was written by the same hand that ignored it.

---

## steps 244–245 — two hypotheses for the missing EAPOL, both disproved

`0d62924` · entry table v18

```
prof  authmode 5  is_rsn 1
lwip  eapol@netif 0
4way  m1=0
```

**"The driver thinks this is an open network."** The station config is zeroed
apart from SSID and password, making `threshold.authmode` `WIFI_AUTH_OPEN`.
Wrong: `esp_wifi_sta_get_prof_authmode_internal` answers 5 and
`prof_is_rsn` answers 1. The driver knows exactly what it is connecting to.

**"lwIP is eating the EAPOL."** Counted at the netif before anything else touches
the frame: zero. No EAPOL reaches lwIP either.

Both were written down before testing, which is what makes eliminating them
worth a commit. Both diagnostics kept.

The step concludes that distinguishing what remains "needs something outside
this board — a packet capture on another machine". **Step 250 shows that is
false:** the radio was already listening.

---

## step 246 — reason 39 never meant "associated"

`375b0ed` · UM-NATOS-051 §6

```
wpa conn=0  disc=1  cbreason=39
evt posted 15   :2   :1 x13   :5(reason39)      -- NO :4
```

Slots 3 and 4 of `struct wpa_funcs` — `wpa_sta_connected_cb` and
`wpa_sta_disconnected_cb` — named rather than pooled into the shared stub. The
driver **never** calls the connected callback, and no `STA_CONNECTED` event is
ever posted.

**The station does not associate.** So the missing EAPOL of steps 241–245 needs
no explanation beyond that: the access point never had an associated station to
send message one to. Steps 237–245 were debugging the wrong half.

39 is `WIFI_REASON_TIMEOUT`. The code for "associated, then EAPOL failed" is
**204 `HANDSHAKE_TIMEOUT`**, or 15, and neither had ever appeared here.

**Eliminated at zero cost:** the theory that the driver gates EAPOL on
`wpa_sta_in_4way_handshake` (slot 6). `rsn_supp/wpa.c:682` sets
`WPA_FIRST_HALF_4WAY_HANDSHAKE` *inside* the message-one handler, so real IDF
answers false before message one too. Reading beat guessing.

---

## step 247 — name every supplicant slot

`00aae4b`

```
wpa seq    0 15 21 21 21 21 15 21 21 22 15 21
wpa named  0=sta_init  15=parse_wpa_ie  21=sta_rx_mgmt  22=config_done
```

One trampoline per slot, 0–31, each recording its own index. Twelve anonymous
return addresses become slot numbers.

**Slot 15, `wpa_parse_wpa_ie`, is called three times on the failing connect**
while returning 0 — success — and writing nothing, so the driver reads back
proto 0, pairwise 0, key_mgmt 0 about a WPA2 network. That is the fifth instance
of the class this log keeps finding, after `_event_post`, `_task_delay`, the
event groups and `_queue_send_from_isr`: **an entry that reports success for
work never done.**

Slot 21 fires seven times — the driver *is* receiving management frames. Whatever
fails, it is not the RF path.

---

## step 248 — parse the RSN element: correct, and NOT the cause

`5e5740c`

```
wpa ie   calls=3 ok=3 bad=0 body=20B
         group=0x8  pair=0x8  akm=0x2  caps=0x0
wpa conn=0  disc=1  cbreason=39          <- unchanged
```

The parser reads the access point exactly right — group CCMP, pairwise CCMP, AKM
PSK. **The association fails identically.** The zeroed struct was not what
stopped it. **Eliminated.**

Kept anyway, on the reasoning of steps 200 and 202: a live defect is worth
fixing whether or not it is today's.

**The field conventions are not uniform**, and only the wrapper says so: `proto`
and `key_mgmt` are supplicant bitmasks; `pairwise_cipher` and `group_cipher` are
public enum values mapped through `cipher_type_map_supp_to_public`. Guessing one
convention for all five would have put TKIP (3) where CCMP (4) belongs — a wrong
answer that looks like a right one.

---

## step 249 — slot 21 is scan traffic, not the association

`7c93d84`

```
wpa mgmt calls=5 connect=1
    type 5 PROBE_RESP   from ..2f57c6
    type 8 BEACON       from ..2f57c6
```

Every one is a beacon or probe response from the scan sweep, forwarded so the
supplicant can maintain a BSS table. **None touches the association.** The
reading that "the AP is talking to us, the failure is later" is retired.

**What it does not establish is the opposite.** IDF routes *authentication*
frames to this slot only for SAE; for open-system auth the driver handles them
itself. Absence here is not evidence of absence on air — the tempting sentence
would have been a conclusion the instrument cannot support.

**A branch closed before it cost a step.** Step 248 planned for the possibility
that nothing was arriving, putting the failure on the way *out*, and noted that
this project had never confirmed a transmission. That is **wrong**, and
UM-NATOS-049 says so: step 230 answered a ping from a separate machine. The
operator's memory caught it; the docs confirmed it.

---

## step 250 — a sniffer, and the access point DOES answer

`8a15e5b` · entry table v19

```
sniff  seen 577  beacons/probes 569  layout-miss 0  KEPT 8
       AUTH  to ..503f64  from ..2f57c6  ch11 rssi-55
```

Promiscuous mode had been **on since step 197 with no callback registered**, so
every management frame decoded outside the data path was thrown away. Step 245
said separating "the AP never answers" from "the driver discards it" needed a
capture on another machine. **It did not.** The radio was already listening and
nobody had asked it what it heard.

An authentication frame from the access point, addressed to this board, means
our request was transmitted, received and answered.

**Deliberately not claimed:** whether that frame is an *acceptance*. A rejection
is also a frame from the AP addressed to us. See step 251.

`rx_ctrl` is seven 32-bit words on ESP32, counted from the bitfields, and two
self-checks guard the +28 offset. Across 577 frames, **layout-miss is zero** — the
offset is measured, not believed.

---

## step 251 — authentication SUCCEEDS

`2325eff`

```
AUTH  alg0(OPEN)  seq2  STATUS 0 SUCCESS
```

Open-system, transaction sequence 2 — the authenticator's reply — status 0.
**The 802.11 exchange completes its first half.** Nothing in the radio, the PHY,
the MAC, the transmit path or the channel is at fault, and none of that is
inference any more.

And then nothing: no association response, ever, in 602 frames. The failure is
located between a completed authentication and an association response that
never comes — which is the first time in fifteen steps that the reason code and
the measurement agree about the same event.

**Still not separated:** the sniffer reports *received* frames, so "no
association response" is consistent with both "never sent" and "sent and
ignored".

---

## step 252 — the association request IS sent

`1f605ec`

| arm | element | the AP's answer | reason |
|---|---|---|---|
| `startnoie` | none | `ASSOC_RESP STATUS 10` | 203 |
| `start` | AKM PSK | **silence** | 39 |

With the element suppressed the access point **receives our association request
and answers it.** The frame goes out.

**So the failure is our RSN element, and it is malformed rather than merely
unacceptable.** A well-formed request the AP dislikes earns a status code; one it
cannot parse earns silence.

**This retires step 237.** `203 → 39` was recorded as progress. It is the
opposite: 203 was the access point answering, 39 is the access point no longer
able to read the question. The IE moved this backwards, and it took fifteen
steps and a sniffer to see it.

---

## step 253 — the appie buffer: panic, "eliminated" — AND THIS ENTRY IS WRONG

`518eb8d` · **corrected at step 256** · UM-NATOS-051 §8

```
element at +2, length 44   *** KERNEL PANIC ***
                           exccause 20 InstFetchProhibited
                           epc 0x00006898   a0 0x8036cc9c
```

ESP-IDF's `set_assoc_ie()` hands `esp_wifi_set_appie_internal` a pointer **two
bytes ahead of the element** with a length that is a capacity. Reproducing that
shape panicked, and the change was recorded as eliminated.

**It was not eliminated. It was working.** The association completed, the access
point sent message one, and nat-os's own handshake crashed on it. The crash beat
the evidence to the console.

`AGENTS.md` rule 12 says: *"when an experiment produces a surprising result,
investigate the implication rather than immediately reverting it."* This step
reverted. The rule was written down, was on screen, and was not followed.

**Do not re-derive this from the original entry.** Read step 256.

---

## step 254 — the element's PRESENCE, not its content

`34c63f6`

```
startnoie     no element         ASSOC_RESP status 10
start         AKM PSK    (02)    silence
start8021x    AKM 802.1X (01)    silence
```

The third arm differs from the second by **a single byte**, asking a WPA2-PSK
router for 802.1X — which it cannot provide and must refuse. A refusal is a
frame: status 43, 44, any of them. It refuses nothing.

**Content eliminated.** The access point does not care what is in the element,
only that there is one. That leaves the insertion.

---

## step 255 — both appie candidates eliminated — AND THIS ENTRY IS ALSO WRONG

`86b0294` · **corrected at step 256**

```
startassocie  type ASSOC_REQ(1)   silence
startflag0    flag 1 -> 0         *** PANIC ***  a0 0x8036cc9c
```

Recorded as: neither candidate is the fix.

**The flag WAS the fix**, and the panic was it working — same error as step 253.
This entry contains the arithmetic that eventually caught both: `a0` is
`0x8036cc9c` in **both** panics, and the blob's symbol table resolves it —

```
4036cb30 T sta_rx_eapol      <-  0x4036cc9c is sta_rx_eapol + 0x16c
```

An access point sends EAPOL to a station that has **associated**, and to no other
kind. That was written here as a hypothesis, correctly hedged against a
register-window signature in the dump, and settled one step later.

---

## step 256 — NAT-OS ASSOCIATES WITH WPA2

`74549b0`

```
AUTH        STATUS 0 SUCCESS
ASSOC_RESP  STATUS 0 SUCCESS   aid 12
DEAUTH      reason 15
4way  pmk=1  m1=3  ki=0xeeee
```

The handshake reduced to a counter, and the flag0 arm re-run. **Association
identifier 12. Message one three times.** Deauthentication with reason 15,
`4WAY_HANDSHAKE_TIMEOUT`, because passive mode answers nothing — all correct
behaviour for a station that associated and went quiet.

**The fix:** `esp_wifi_set_appie_internal`'s trailing argument must be **0**, not
the 1 ESP-IDF's own RSN call site passes. Why IDF passes 1 and works is not
understood and is not guessed at.

**And the crash of steps 253 and 255 is ours, in one line:**

```c
g_snonce[i] = (u8)(lwip_rand_u32() >> ((i & 3u) * 8u));
```

`lwip_rand_u32` is in `kernel/kstring.c`, which is **call0**; `wpa_hs.c` is
**windowed**. The first thing the message-one path does was an ABI violation.

---

## step 257 — the handshake runs

`82b2e86`

```
4way  pmk=1 st=1 m1=1 m3=1 done=0 micbad=0 unwrap=1 ki=0x13ca
```

Every counter is a proof. `m3=1` means the access point sent message three,
which it does **only** after message two arrives with a MIC it accepts;
`micbad=0` means message three's own MIC verified. So the PMK, the PTK, the PRF
and the address and nonce orderings are all right, and the KCK is right twice
over. Everything step 240 vendored works against real hardware.

`lwip_rand_u32` replaced by a direct `WDEV_RND_REG` read — **the cheapest answer
to an ABI crossing is not to cross.** Checked that it was the only one.

`unwrap=1`: the group key would not come out.

---

## step 258 — THE FOUR-WAY HANDSHAKE COMPLETES

`3aa83cb`

```
wpa    4 passed, 0 failed   (3x pbkdf2 + aes_unwrap RFC 3394)
4way   pmk=1 step=6 m1=1 m3=1 done=1 micbad=0 why=0
wpa    conn=1 disc=0 cbreason=none
```

**`conn=1`** — the callback step 246 measured at zero for thirteen steps.

**The primitive was proved before the protocol was blamed.** `aes_unwrap` was the
obvious suspect and the one function step 240 never checked against a published
answer. Checked against **RFC 3394 §4.1**: it passes. The crypto was never wrong,
and without that vector the next days would have gone into `aes-unwrap.c`.

Three bugs, each exposed by the last:

1. **The KDE walk stopped at the first element.** A WPA2 message three's key data
   is the AP's **RSN IE (`0x30`)** followed by the GTK KDE, so the walk broke on
   element one and never reached the group key `aes_unwrap` had already
   decrypted correctly.
2. **The group key was installed at address NULL** — `LoadProhibited`,
   `epc 0x4000c28c`, `excvaddr 0`. IDF passes `sm->bssid`.
3. **`esp_wifi_wpa_ptk_init_done_internal` is an access-point function.** Its only
   caller in the whole IDF supplicant is `src/ap/wpa_auth.c:2003`. It never
   returned and took the blob mutex with it — which also explains earlier runs
   dying at `prof authmode` with no message.

Bug 3 was found by instrumentation, not insight: `m3=1` with `done=0` is a
five-way guess; one store per stage made it `step=4`, which names the call.

---

## step 259 — the watchdog is NOT starvation, and DHCP binds over WPA2

`3adb09f`

```
lwip   DHCP bound -- address 192.168.1.140
wdt    f/s=600/0   cfg=0xe01f8000
<TG0WDT_SYS_RESET>
```

**nat-os holds a DHCP lease on a WPA2 network.** The keys work. Step 258's "no
DHCP" was the wrong instrument — `offer/ack 0/0` is `kernel/net.c`'s hand-rolled
client, and lwIP is a different stack that had bound an address.

**The watchdog reset is not starvation.** The hang detector now prints which task
it is about to give up on — it has two seconds of headroom and nothing else is
running to be starved by the printing. In the run that reset it printed
**nothing**, and feeds were advancing on schedule. The watchdog fired **while
being fed correctly**, which retires the step-242 class the boot banner's own
description implies.

**Also eliminated:** the shell stack (236 of 2048 free appears in runs that never
handshake and never reset) and lock contention (the rising `contended=` is
`g_shared_lock`, the wintorture test mutex, nothing to do with the blob).

### State

```
boot   11 PASS 0 FAIL      wintorture checksum 1632 CORRECT
wpa    4 crypto vectors passed, 0 failed
       ASSOCIATED aid 12, handshake done=1, conn=1
lwip   DHCP bound -- 192.168.1.140
wdt    2 resets in 4 completed runs -- mechanism NOT established
```

### Next

1. **The watchdog.** A watchdog that fires while fed is one whose configuration
   or clock is not what this kernel thinks. TIMG0 is Espressif hardware and the
   blob is Espressif code. The config readback is sampled per status line and
   needs **latching on change** — the interesting moment is inside the last
   three seconds.
2. **The web page over the encrypted link.** Step 235 proved TCP on an open
   network; it has never been tried on this one.
3. **`docs/debug/`** still has nothing on this investigation, which AGENTS.md
   asks for.
4. Group-key rekeying, roaming, PMKSA caching, WPA3/SAE — `202 AUTH_FAIL` is
   still where step 220 left it — and the all-channel scan that has panicked
   since step 202.

**nat-os is a station on a WPA2-PSK network, with an address, and the mechanism
of one intermittent reset is not yet known.**

---

## step 260 — the web page does NOT load, and step 259's instrumentation was the reason nothing ran

`(this commit)`

```
wpa    4 passed, 0 failed        PMK ready after 15 s
       ASSOC_RESP STATUS 0 SUCCESS aid 12      4way done=1
lwip   DHCP bound -- address 192.168.1.140
lwip   rx 695 drop 0  tx 10 err 0
http   conns 0 reqs 0 errs 0
```

The browser test **fails**. A machine on the same subnet cannot reach the board:
the page times out, `ping 192.168.1.140` gets nothing, and `arp -a` reports **no
ARP entry** — this station never answers a request for its own address.

### 260a. First, a retraction. Step 259's cfg readback broke `wifiinit`.

Step 259 added a TIMG0 `WDTCONFIG0` readback to the status line, to catch the
watchdog being reconfigured by the blob. It reported that across a clean run the
value never moved.

**Both halves of that are wrong, and the second caused the first.**

    cap27 .. cap30    PMK ready after 15 s, full runs
    cap31             FIRST run carrying the cfg readback
                      -> stalls at [blobtask] wifi prio 23/25 -> HIGH
    every run since   stalls identically

The cfg values step 259 quoted came from the boot-phase status lines of a run
that had **already stalled**. The claim is withdrawn; it was never a measurement
of a running system.

Confirmed by A/B on the one call site, nothing else changed:

    cfg readback present   stall at [blobtask], no crypto, no association
    cfg readback removed   4 passed, PMK 15 s, aid 12, done=1, DHCP bound

WHY a plain read of `0x3FF5F048` wedges the crypto phase is **not known** and is
not guessed at here. It is now a one-line switch, which is a far better position
than a mystery. The accessor stays in `watchdog.c`; only the call site is gone.

This is the same shape as step 211's retracted lock leak, and it is worse in one
way: step 211's phantom defect merely sent a reader to the wrong file, while
this one silently disabled the subsystem it was installed to measure. **An
instrument that stops the machine reports nothing about the machine.**

### 260b. The poll window, and a wrong "too late"

The first browser attempt was made after the 120 s window closed, and the board
recorded **no ARP for its own address** — so nothing had tried, and "the page did
not load" meant nothing at all. Step 227 sized that window so a human could type
`ping`; the browser test races the operator in a way `ping` does not, because
the address cannot be handed over until DHCP has bound and DHCP binds *inside*
the window. Now 600 s.

That removed timing as a variable and the test still fails, which is what makes
the failure worth recording.

### 260c. What works, measured

    rx 695 drop 0        sustained encrypted reception, no losses
    tx 10 err 0          the DHCP exchange, and NOTHING AFTER IT

Reception over CCMP is not marginal: 695 frames across four minutes with zero
drops. The pairwise key decrypts real traffic continuously, which is a stronger
statement than step 259's DHCP lease — four datagrams could hide a lot.

`tx` is the whole finding. It reaches 10 during DHCP — so encrypted transmit
**does** work — and then never moves again. No ARP reply, no ICMP echo, no SYN
+ ACK. **On WPA2 this station receives and does not answer.**

Step 230 answered a ping on an OPEN network, so the ARP and ICMP code is not the
suspect.

### 260d. And the poll stops servicing lwIP

    net_poll_for(60000u)      600 s requested
    last lwip report          +241s
    tick counter after        35526, still climbing

The loop was asked for 60000 ticks and stopped reporting at roughly 24100, while
the rest of the system kept running — status lines, tasks, telemetry, all
healthy. The shell task appears to be stuck inside the lwIP servicing path
(`netif_wifi_input` or `netif_wifi_tick`), which explains why `rx` freezes and
why nothing is answered after that point.

**This invalidates the timing of both negative tests.** The operator's browse and
the ping from 192.168.1.102 both landed after +241s, when nothing was draining
the ring. So the honest statement is narrow:

> Between DHCP binding and +241s the board transmitted nothing beyond the DHCP
> exchange. After +241s it was not servicing the stack at all, and no test
> conducted in that period is evidence about the network path.

Whether an ARP arriving *during* the servicing window would have been answered
is **not yet known**, and that is the next thing to find out.

### State

```
boot   11 PASS 0 FAIL
wpa    4 crypto vectors passed, 0 failed
       ASSOCIATED aid 12, handshake done=1, conn=1
lwip   DHCP bound -- 192.168.1.140,  rx 695 drop 0,  tx 10
http   listening on port 80, conns 0 -- NOT REACHED
```

### Next

1. **Why the poll stops at ~241 s.** Everything downstream is unmeasurable
   until the stack is being serviced for the whole window. Instrument the loop
   itself — which call it is inside — rather than inferring from silence.
2. **Then re-run the browser test inside a known-good servicing window**, and
   only then is "the page does not load" a fact about the network.
3. **The cfg readback.** A register read that wedges the crypto phase is a real
   defect in something, and the A/B is one line.
4. The intermittent watchdog of step 259 is **untouched** and its diagnosis
   still stands: not starvation, not stack, not lock contention.

**nat-os holds a WPA2 association and a DHCP lease, receives 695 encrypted
frames without loss, and answers nothing.**

---

## step 261 — NAT-OS SERVES A WEB PAGE OVER WPA2

`(this commit)`

```
$ curl http://192.168.1.140/
HTTP 200   350 bytes

    nat-os
    This page was served by a from-scratch operating system over a
    Wi-Fi driver it reverse-engineered the interface to.
      uptime: 252 s
      lwIP rx: 465 (dropped 0)
      lwIP tx: 23 (errors 0)
      connections: 2
      requests: 2
```

Fetched from 192.168.1.102 — a machine this project does not control, running
software it did not write. The page reports its own counters, so the instrument
and the subject are the same object and the reader can check one against the
other.

**Every layer at once, and this time over CCMP**: 802.11 association, the WPA2
four-way handshake, an installed pairwise key, Ethernet, ARP, IPv4, TCP, HTTP.
Step 235 did this on an open network. This is the same stack with the
encryption underneath it.

### 261a. Step 260's central claim was WRONG

Step 260 concluded, in bold:

> **On WPA2 this station receives and does not answer.**

It answers. The ARP table on the other machine resolves the address to this
board's own MAC:

```
192.168.1.140    5c-01-3b-50-3f-64    dynamic
```

What step 260 measured was real -- `tx` frozen at 10, no ARP entry, no ping --
but every one of those observations was taken **after the poll stopped
servicing lwIP at +241s**, which step 260 itself recorded two sections later
without connecting the two. The section that noted the limit did not correct
the headline above it.

The correct statement is the narrow one 260 nearly reached: *nothing that
happens outside the servicing window is evidence about the network path.* Run
the test inside the window and the answer inverts completely.

### 261b. And a harness discipline, paid for four times

Four conclusions in this stretch were drawn from **byte counts** rather than
from the milestone the run was supposed to reach:

```
vfy.txt      12657 bytes -- called "stalled", CONTAINED "PMK ready after 15 s"
cap_ctrl      8561 bytes -- called "the constant print stalls too"
ctl, ctl2     8541 bytes -- called "the committed state is broken"
verify        8541 bytes -- called "the committed state is broken"
```

Every one of the last four has `rom stubs` count **zero**: the command was never
delivered, so the capture says nothing whatever about the build. On that basis
step 260's A/B was briefly declared invalid and the fault "intermittent". It is
not. Judged by the milestone instead of the file size:

| build | runs reaching `PMK ready` |
|---|---|
| before the cfg readback | **4 / 4** |
| with the cfg readback | **0 / 4** |
| cfg readback removed | **4 / 4** |

Step 260's A/B stands exactly as committed.

**A capture without the command's first line is not evidence, and file size is
not a milestone.** The harness now retries until `rom stubs` appears and says so
in the capture; a run that never delivers is labelled rather than silently
counted.

### 261c. What is still open, unchanged

The poll still stops servicing lwIP at roughly +241s of a 600 s window while
the rest of the system runs on healthy. That is now a **performance and
liveness** bug rather than a correctness one -- everything works inside the
window -- but it is why the first three browser attempts failed and it will
bite again.

The single attempt to instrument it (a stage counter published to the status
line) produced one stalled run. One data point, on a fault that may be
intermittent, is not a finding; it is recorded here so it is not mistaken for
one.

### State

```
boot   11 PASS 0 FAIL
wpa    4 crypto vectors passed, 0 failed
       ASSOCIATED aid 12, handshake done=1, conn=1
lwip   DHCP bound -- 192.168.1.140,  rx 465 drop 0,  tx 23 err 0
http   HTTP 200, 2 connections, 2 requests -- SERVED OVER WPA2
```

### Next

1. **Why the poll stops servicing lwIP at ~241 s.** Instrument the loop so it
   names the call it is inside, and get a RATE before believing any single run.
2. **The intermittent watchdog** of step 259, still not mechanised.
3. **The cfg readback.** A status-line register read that reliably prevents the
   crypto from running is a real defect in something, and the A/B is one line.
4. Group-key rekeying, roaming, PMKSA caching, WPA3/SAE, and the all-channel
   scan that has panicked since step 202.

**nat-os is a station on a WPA2-PSK network, holds a DHCP lease, and serves
HTTP over an encrypted link to a browser it has never met.**

---

## step 262 — the drain loop was starving lwIP's timers

`(this commit)`

```
before   lwip report lines in an 85 KB capture:  2      (+10s, +20s, silence)
after    lwip report lines:                     10      (+10s .. +100s)
```

### 262a. The mechanism, and it inverts step 260 again

`net_poll_for()` drained the receive ring like this:

```c
while (g_tail != g_head) {
    netif_wifi_input(...);
    g_tail = (g_tail + 1u) % NET_SLOTS;
}
```

**Unbounded.** On a busy network the ring is never empty — frames arrive as
fast as they are taken — so the loop never exits. `netif_wifi_tick()` sits
directly below it and never runs, and neither does the report.

Step 260 read the silence as "the poll stops servicing lwIP at +241s" and step
261 corrected that to "the reporting stops, the servicing continues". Both were
half right, and the half that matters is this: **input kept running and the
TIMERS stopped.**

That distinction is the whole bug. lwIP's input path is what drives TCP for a
request that arrives — which is why the page ever loaded at all. Its timer path
is ARP expiry, DHCP renewal and, above all, **TCP retransmission**. A stack that
receives but never retransmits answers instantly when nothing is lost and hangs
forever when anything is, which is exactly how the page behaved across five
attempts: mostly a timeout, occasionally instant.

Bounded to sixteen frames per pass — well above the burst the ring holds between
iterations, far below the point where the timers starve.

### 262b. Verified, and what is NOT

**Verified.** The reports resume and keep coming: ten of them, +10s through
+100s, where the same capture length previously held two. `netif_wifi_tick()`
is running, so the timers are firing.

**NOT verified: end-to-end HTTP on this build.** Two runs were spent and
neither closed the loop:

- one hit the **step-259 intermittent watchdog** (`TG0WDT_SYS_RESET`) during
  the scan, before the poll was reached;
- the other associated and completed the handshake (`done=1`, rx 471, drop 0)
  but **never obtained a DHCP lease**, so there was no address to fetch from and
  five HTTP attempts failed for want of an IP rather than for want of TCP.

So the fix is established as *correct in mechanism and confirmed in its direct
effect*, and **unproven end to end**. Saying otherwise would be the error of
step 260, which is what this step exists to undo.

### 262c. External verification of step 261, recorded

The operator loaded the page in their own browser and confirmed it. Step 261
rested on a `curl` from this machine; it now also rests on a person and a
browser this project does not control — the standard the beacon met at step 209
and the ping at step 230.

### State

```
boot   11 PASS 0 FAIL
wpa    4 crypto vectors passed, 0 failed
       ASSOCIATED, handshake done=1
lwip   timers RUNNING (10 reports vs 2)
       this run: no DHCP lease, so no HTTP
http   step 261's HTTP 200 stands, on the previous build
```

### Next

1. **Re-run until a lease and a fetch land in the same run**, and get a RATE.
   Two runs is not a measurement; step 259 needed a rate for the watchdog and
   this needs one too.
2. **The DHCP failure in 262b** — new, one occurrence, not yet a pattern.
3. **The intermittent watchdog** of step 259, still not mechanised, and now
   demonstrably costing runs.
4. **Persistence.** The stack lives inside one shell command's loop; when
   `wifiinit start` returns, nothing services lwIP and the board goes silent.
   Servicing it from a task is what turns "I caught the page during a run" into
   "the board is a web server".
5. The cfg readback, rekeying, roaming, PMKSA, WPA3/SAE, the all-channel panic.

**The timers run again. Whether the page is now reliable is not yet measured.**

---

## step 263 — a RATE at last, and the watchdog is the blocker

`(this commit)`

Eight runs of the step-262 build, each taken to a lease and then fetched from
192.168.1.102:

| | count |
|---|---|
| command delivered | 8 / 8 |
| DHCP lease | **6 / 8** |
| **TG0WDT_SYS_RESET** | **3 / 8** |
| HTTP 200 | run 6 — lease, no reset, ARP resolved first |

**Every HTTP failure coincided with either a watchdog reset or a fetch made
without pinging first.** The one run that had a lease, no reset, and a resolved
ARP entry served the page.

### 263a. This reprioritises everything

The step-259 watchdog has been carried for four steps as a curiosity — an
intermittent reset with no mechanism, noted and deferred. It is not a
curiosity. **It is the thing standing between nat-os and a working server**, and
at 3 in 8 it will defeat any attempt at persistence long before the design of
the polling loop does.

The rate is consistent with step 259's own 2 in 4, measured on a different
build, which is worth something: two independent samples, ~40%, across code
changes that did not touch it.

### 263b. What a rate buys

Step 259 could not name the mechanism because a single observation cannot
distinguish a cause from a coincidence — and steps 253, 255 and 260 each drew a
confident conclusion from one run and each had to be retracted. Eight runs cost
forty minutes and settle more than the previous four steps of argument did.

**Do not diagnose an intermittent fault from one run.** Written down here
because it has now been learned four times in this file.

### 263c. The ARP nudge, unexplained

Both successful fetches in this project so far — step 261 and run 6 — were made
*after* pinging the board. A fetch without it has failed every time, including
run 2, which had a lease and no reset. Windows should ARP on its own before the
SYN.

Not chased, and not explained. Recorded so it is not mistaken for noise: it may
be a slow or dropped first ARP reply, which would be a real defect on the
station side.

### State

```
boot   11 PASS 0 FAIL      wpa 4 crypto vectors passed
assoc + handshake          reliable
DHCP lease                 6/8
TG0WDT reset               3/8   <- the blocker
HTTP 200                   when neither of the above interferes
```

### Next

1. **The watchdog, properly.** It fires while being fed (step 259), it is not
   starvation, not stack, not lock contention, and it now has a measured rate on
   two builds. Next instrument must survive the reset — the store already
   persists `LAST FAULT` across boots and a watchdog leaves nothing.
2. The first-ARP question of 263c.
3. Persistence: service lwIP from a task rather than a shell command's loop.

**The stack works. The board reboots underneath it three times in eight.**

---

## step 264 — the watchdog names its task, and it is the DISPLAY

`(this commit)`

```
forced (shell 'hang'):   LAST TICK : task 4 at tick 1738  (6140 ticks that boot)

real, run 11:            LAST TICK : task 6 at tick 21524 (105207 ticks)
real, run 12:            LAST TICK : task 6 at tick 19410 (93956 ticks)
real, run 13:            LAST TICK : task 6 at tick 15706 (74576 ticks)

tasks : report=0 a=1 b=2 vm=3 apps=4 shell=5 disp=6 touch=7
```

**Three of three real watchdog resets were running the DISPLAY task.**

### 264a. A breadcrumb that survives the reset

A watchdog reset is the one failure in this kernel that leaves nothing — no
exception, no register dump, no `LAST FAULT`, just a reboot. Four steps of
argument produced no mechanism because every observation died with the board.

RTC slow memory does not die with it: `TG0WDT_SYS_RESET` resets the digital
core and the RTC domain keeps its contents. Three words written per tick —
sequence, current task, tick — are read back and printed on the next boot,
guarded by a magic so a power-cycled RTC reads as absent rather than as
garbage.

**Validated before it was trusted.** The shell's `hang` command wedges the
system deliberately, so the recorder could be proved against a watchdog reset
on demand rather than hoped at at 3-in-8. It survived, and named a task.

### 264b. Why this is a surprise, and why it is not

Every hypothesis so far pointed at the WiFi side — the blob, the poll loop, the
crypto, the TIMG0 register. The display task was never a suspect. It is the
only task that has never been touched by any of this work.

And yet UM-NATOS-029 spent a whole report on the display driver monopolising
the panel: `dlock hold ms=7965` against ~7,860 ms of uptime, `drawskip`
climbing into the thousands, `cont=0` because everything else yields rather
than waits. In the run that reset above, the same counters read
`dlock blk/hold ms=2434/15371`, `takes=414269`, `cont=34`, `drawskip=1721`.

The novel's Scheduler refused Touch a context switch because *"the Display
Driver is holding the panel lock, and I do not take a lock off a man who holds
it."* Step 264 is that sentence with a reset attached.

### 264c. What is established, and what is not

**Established:** the board is executing the display task at the last tick
before the watchdog fires, three times out of three, on a fault measured at
3-in-8 across two builds.

**Not established:** that the display task is the *cause*. The breadcrumb
records which task the tick landed in, and a task that runs often is more
likely to be caught. `disp` is a HIGH-priority task that draws continuously, so
a naive prior would already favour it. Three of three is suggestive; it is not
proof, and step 263's lesson is that this file has repeatedly mistaken one for
the other.

What would settle it: record the *previous* task as well, and whether the panel
lock is held, so a monopoly is distinguishable from a coincidence of timing.

### State

```
boot   11 PASS 0 FAIL      wpa 4 crypto vectors passed
assoc + handshake          reliable
watchdog                   3/8 (step 263), 3/4 here, always task 6 = disp
breadcrumb                 survives the reset, validated against 'hang'
```

### Next

1. **Widen the breadcrumb**: previous task, panel-lock holder, blob pin. That
   turns "the tick landed in disp" into "disp was holding X for N ticks".
2. Whether disabling the display task removes the reset — a blunt A/B, but a
   decisive one, and cheap.
3. The first-ARP question of step 263c.
4. Persistence, once the board stops rebooting underneath it.

**The watchdog has a name for the first time in five steps, and it is not the
one anybody was looking for.**

---

## step 265 — the display holds the panel lock, and monopolises

`(this commit)`

```
run 22:  LAST TICK : task 6 at tick 24040  lock6  hist 6:234566
run 23:  LAST TICK : task 6 at tick 25680  lock6  hist 66666666
```

Two more real watchdog resets, with the breadcrumb widened to carry the eight
most recent task ids and the panel-lock holder.

**`lock6` in both: the display task is HOLDING THE PANEL MUTEX when the
watchdog fires.** And run 23's history is eight consecutive ticks of task 6 —
every scheduler invocation the board managed before it died was the display
task.

That is what step 264 said it could not distinguish. A coincidence of timing
gives a mixed history; `66666666` is a monopoly.

Run 22 is the mixture — 6, 10, 2, 3, 4, 5, 6, 6 — so the monopoly is not
present every time, but the LOCK is: both resets have disp holding it.

### 265a. The mechanism this implies, stated as a hypothesis

The breadcrumb is written from `watchdog_liveness`, which the scheduler calls.
If the scheduler stops being entered, the breadcrumb stops updating and its
last value is whatever was current when the ticks stopped. `hist 66666666`
therefore reads: *the last eight scheduler entries were all disp, and then
there were no more.*

So: **the display task takes the panel lock, enters something long, and the
scheduler stops running for longer than the three-second timeout.** Not
starvation in the sense step 259 tested for — `watchdog_liveness` never
reported a starved window, because it was not being called at all.

That also explains why five steps of WiFi hypotheses found nothing. The WiFi
work did not cause this; it made it *visible*, by giving the board something
to do for four minutes at a stretch.

### 265b. Not established

That the display is the cause rather than the victim. A task holding a mutex
when the world stops is equally consistent with it *waiting* on something the
blob holds — and the blob runs at nat-os priority HIGH with its own critical
sections, which is exactly the class of thing that stops a scheduler.

Two resets is also two. Step 263's lesson stands.

### 265c. A rendering bug in this step's own instrument

`hist` packs four bits per task and prints `'0' + nibble`, so task 10 renders
as `:`. Tasks 8, 9 and 10 exist — they are the blob's. Harmless here because
the reading is unambiguous, and noted so the next reader does not think the
board printed a colon.

### State

```
watchdog        3/8 (step 263); 3/3 then 2/2 naming task 6
breadcrumb      task + tick + 8-deep history + panel-lock holder
at the reset    disp is current, disp holds the lock, sometimes 8/8 ticks
```

### Next

1. **The blunt A/B: run with the display task disabled.** If the resets stop,
   disp is the cause; if they continue, disp was the victim and the search
   moves to what it was waiting on. One switch, and it is decisive in a way no
   amount of further instrumentation is.
2. If disp is the cause: what does it do under the lock that takes seconds?
   UM-NATOS-029 measured `dlock hold ms` at essentially all of uptime.
3. Fix the `hist` nibble rendering to hex.

**The watchdog fires while the display task holds the panel lock. Whether it is
the cause or the victim is one experiment away.**

---

## step 266 — the display is NOT the cause: the A/B says so

`(this commit)`

```
baseline (step 263)          watchdog 3 / 8
display frozen (dfreeze)     watchdog 1 / 4

the one frozen-arm reset:
    LAST TICK : task 6 at tick 7265  lock4294967294  hist 12346796
```

`dfreeze` sets `g_display_frozen` and nothing repaints. Four runs with it on.

**The watchdog still fires.** And the reset that happened is qualitatively
different from every un-frozen one:

| | un-frozen (steps 264-265) | frozen (this step) |
|---|---|---|
| last task | 6 | 6 |
| panel lock | **held by 6** | **4294967294 = (unsigned)-2, nobody** |
| history | `66666666` and `6:234566` | `12346796` — mixed |

So the lock-holding and the eight-in-a-row were **symptoms of drawing**, not the
mechanism. Stop the drawing and the board still resets, with the lock free and
seven different tasks in the last eight scheduler entries.

**Step 265's hypothesis is disproved.** "The display task takes the panel lock,
enters something long, and the scheduler stops" cannot be the mechanism, because
the scheduler stops when it is holding nothing.

### 266a. What the rate does and does not say

1 in 4 against 3 in 8 is **not a distinguishable reduction** at these numbers.
It is consistent with no effect, and it is consistent with a modest one. Four
runs cannot separate those, and this file has been burned enough times by
treating a small sample as an answer that it is worth writing the non-result
down rather than rounding it into a story.

What IS established is qualitative and does not depend on the rate: a reset
occurred with the display frozen, the lock free, and a mixed history. That is
enough to eliminate disp as *necessary*.

### 266b. Where that leaves it

The display was the most-caught task because it is HIGH priority and runs
constantly — exactly the naive prior step 264 warned about, now confirmed as
the explanation for its prominence rather than as a mechanism.

What survives across every reset, frozen or not:

- the tick stops entirely (the breadcrumb freezes; `watchdog_liveness` never
  reports a starved window, because it is not being called);
- it happens only once WiFi is running for minutes at a stretch;
- `wdt f/s` shows feeds advancing normally right up to the end.

A scheduler that stops being entered while the CPU is alive is an interrupts-off
condition. The blob takes critical sections, runs at nat-os priority HIGH, and
is the one component whose internals this project cannot read.

### State

```
watchdog     3/8 baseline, 1/4 with the display frozen -- not distinguishable
eliminated   starvation (259), shell stack (259), lock contention (259),
             the display task (266)
breadcrumb   task + tick + 8-deep history + panel-lock holder, survives reset
```

### Next

1. **Record the interrupt level and PS in the breadcrumb.** If the tick stops
   because interrupts are masked, the last breadcrumb before the stop is
   written with the level that did it. That is three more words and it tests
   the only hypothesis left standing.
2. Fix the `hist` nibble rendering to hex.
3. Persistence, which still waits on this.

**Not the display. The scheduler stops while nothing holds anything, and the
only component that can do that is the one whose source this project does not
have.**

---

## step 267 — the blob's critical sections were never implemented

`(this commit)`

```c
static uint32_t osi_wifi_int_disable(void *wifi_int_mux)
{
    (void)wifi_int_mux;
    return 0;                    /* stub */
}
```

`wifi_osi_impl.c:1037` has carried this marker since the ISR path was written:

> *"The blob has its own answer — it wraps its critical regions in
> `_wifi_int_disable`/`_wifi_int_restore`, which mask interrupts globally — and
> those are already implemented here. Whether that is sufficient is not yet
> measured, and this comment is the marker for it."*

**They were not implemented.** Both were empty stubs. The blob has been
entering every critical region it has, being told interrupts are masked, and
running with them fully enabled — so an ISR firing while the blob task is
inside vendor code puts **two contexts in windowed vendor code at once**, which
is the exact hazard the marker was left for.

That is the **sixth** entry found reporting success for work never done, after
`_event_post`, `_task_delay`, the event groups, `_queue_send_from_isr` and
`wpa_parse_wpa_ie`. The marker was right to exist and wrong about the facts.

Implemented with `rsil 3` and a level-only restore, for the reason
`phy_exit_critical` gives: the rest of PS is the kernel's execution mode, and
restoring a whole saved word can put WOE back as it was at capture time rather
than as it must be now.

### 267a. It is NOT shown to be the watchdog

```
baseline (step 263)          3 / 8
critical sections real       1 / 6
```

**Not distinguishable at these numbers.** 1 in 6 against 3 in 8 is consistent
with a real improvement and with none, and six runs cannot separate them — the
same non-result step 266 recorded for the display, written down rather than
rounded up.

And the one reset argues against the hypothesis directly:

```
LAST TICK : task 3 at tick 9175  lock6  crit0  hist 66666613
```

**`crit0`** — the blob was not inside a critical region when the board died.
If unprotected critical sections were the mechanism, this is where a non-zero
depth would have appeared, and it did not.

So: a real defect, correctly fixed, on the sixth instance of a pattern this
project keeps finding — and not the answer to the question that found it.

### 267b. What the reset now looks like

`lock6` again, and `hist 66666613` — six of the last eight scheduler entries
are the display, ending in vm. Step 266 established disp is not *necessary*
(it resets with the display frozen too), so its recurrence here is the same
prior: a HIGH-priority task that runs constantly is the one most often caught.

This reset also came early — tick 9175, before any lease — where the others
came after minutes of traffic. That is a difference worth noting and not yet
worth a theory.

### State

```
watchdog        3/8 baseline, 1/4 display frozen, 1/6 critical sections real
                -- none of these is distinguishable from the others
eliminated      starvation, shell stack, lock contention, the display task,
                and now the blob's unprotected critical regions
breadcrumb      task + tick + history + panel lock + blob critical depth
```

### Next

1. **The interrupt level of the interrupted code.** The breadcrumb records the
   tick's own context; what is wanted is `EPS3` — the PS of whatever the tick
   interrupted — so a task running at a raised level shows up before the
   freeze. The panic dump already reads EPS3, so the machinery exists.
2. Persistence, still waiting on this.
3. The first-ARP question, the cfg readback, and the `hist` hex rendering.

**Six instruments, five eliminations, and the board still reboots. What is left
is the interrupt level, and after that, the blob.**

---

## step 268 — nothing was masking interrupts

`(this commit)`

```
run 1:  task 7 at tick 8016  lock6  lvl 00000000  crit0  hist 67966167
run 2:  task 6 at tick 8089  lock6  lvl 00000000  crit0  hist 67967666
```

The breadcrumb now carries `EPS3` — the PS of whatever each tick interrupted —
and packs the interrupt LEVEL of the last eight into a nibble history. The
saved EPS3 was already being read in `task.c:937` for the EXCM and WOE watches,
so publishing it cost one store.

**`lvl 00000000` in both.** Every one of the last eight interrupted contexts
was running at interrupt level zero. Nothing masked interrupts before the tick
stopped.

### 268a. The last standing hypothesis is dead

UM-NATOS-052 §9 concluded: *"a scheduler that stops being entered while the CPU
is alive is an interrupts-off condition. The blob takes critical sections, runs
at nat-os priority HIGH, and is the one component whose internals this project
cannot read."*

It is not an interrupts-off condition. The measurement that would have shown it
shows the opposite, twice, and step 267 already had `crit0` saying the blob was
not inside a critical region either.

Eliminated so far, each by measurement: starvation, the shell stack, lock
contention, the display task, the blob's unprotected critical regions, and now
interrupt masking.

### 268b. What that leaves, and it is a better question

If nothing masked interrupts and the code was at level 0, then the tick was
free to fire. So either it fired and the scheduler did not reach
`watchdog_liveness`, or **the timer stopped**, or **the watchdog was
reconfigured**.

The third is newly interesting, because this project already has a strange
result about that register: step 259's readback of `TIMG0_WDTCONFIG0` from the
status line reliably prevents the crypto from running — 0 of 4 runs against 4
of 4 without it (UM-NATOS-052 §3). A register whose mere reading changes
behaviour is a register somebody else is using.

TIMG0 is a timer group. nat-os uses it for the hang detector. ESP-IDF's WiFi
stack uses timers of its own, and nat-os implements the blob's `_timer_*`
adapter entries. **Contention over TIMG0 would explain both the unexplained
readback and a watchdog that fires while being fed on schedule** — if the
timeout is shortened underneath us, a feed every second is simply too slow.

### 268c. How to test it without the readback

Not from the status line, which is where the step-259 anomaly lives. From the
breadcrumb, written in the tick handler: record `WDTCONFIG0` and `WDTCONFIG2`
every tick, and read them back after the reset. If either differs from what
`watchdog_arm()` wrote, somebody else owns the watchdog.

### State

```
watchdog     3/8 baseline, 1/4 display frozen, 1/6 critical sections real
             -- none distinguishable from the others
eliminated   starvation, shell stack, lock contention, the display task,
             blob critical regions, interrupt masking
breadcrumb   task, tick, task history, panel lock, blob crit depth,
             interrupted PS, interrupt-level history
```

### Next

1. **TIMG0's own registers in the breadcrumb**, per 268c.
2. Persistence, still waiting.
3. The first-ARP question, the `hist` hex rendering.

**Six eliminations. The tick stops while interrupts are on, nothing is held,
and the feeds are healthy — which points at the watchdog's own configuration.**

---

## step 269 — the watchdog's own registers are untouched

`(this commit)`

```
run 1:  task 6 at tick 20499  lvl 00000000  cfg 0xe01f8000/6000  crit0
run 2:  task 6 at tick 10701  lvl 00000000  cfg 0xe01f8000/6000  crit0
```

`WDTCONFIG0` and `WDTCONFIG2` sampled in the feed path -- where the protect key
is already lifted and the block already being written, so no new access pattern
is introduced -- and carried through the reset.

**Both read exactly the armed values**: `0xe01f8000`, and 6000 = 3000 ms x 2.
Nobody reconfigured the watchdog. Step 268's leading candidate is eliminated.

### 269a. And an incidental result about step 259

`crypto=1` in both runs. Reading `TIMG0_WDTCONFIG0` **from the feed path does
not break the crypto**, where reading it from the status line reliably did --
0 of 4 against 4 of 4 (UM-NATOS-052 §3).

So the step-259 anomaly is not "reading this register is harmful". It is
something about that call site: its position, its size, or the layout shift it
caused. That is a much smaller and less alarming mystery than the one recorded
in UM-NATOS-052, and it is narrowed here rather than solved.

### 269b. Which leaves the tick itself

The tick does not come from TIMG0. `timer.c` uses **CCOMPARE1**, the core's own
cycle comparator, interrupt 15 at level 3 -- chosen because it needs no clock
gating, no pin muxing and no interrupt-matrix routing.

It is **one-shot**: it must be re-armed inside the handler. And `timer.c`
already carries the scar from this exact failure:

> *"CCOMPARE1 ended up 14,630,119 cycles — 18 tick periods, 183 ms — ahead of
> ccount, and the tick simply stopped for that long... Nothing else showed a
> symptom, which is the uncomfortable part."*

A guard was added: if the new deadline is not within one interval of now, it is
reset to `ccount + interval` and `g_late` counts it. **So the mechanism is
known, was fixed once, and has a counter — which nothing has looked at during
any of these seven steps.**

If the comparator is armed far ahead of the counter, the tick stops for the
difference, interrupts stay enabled the whole time, nothing is held, the
watchdog config is untouched, and it fires three seconds later. That is every
observation in steps 264-269 at once.

### Next

Record `CCOMPARE1`, `CCOUNT` and `g_late` in the breadcrumb. If the comparator
is far ahead of the counter at the last tick before a reset, this is finished.

---

## step 270 — the comparator instrument, armed and not yet fired

`(this commit)`

The breadcrumb now carries `ahead` — `CCOMPARE1 - CCOUNT` at the last tick,
which is how far into the future the one-shot comparator was armed — and
`late`, `timer.c`'s cumulative count of re-arms that had already elapsed.

Both are read in the liveness path, where the scheduler is already running.

**Six runs, no reset.** So `ahead` and `late` have no reading yet, and step
269's hypothesis is neither confirmed nor denied.

### 270a. The rate, which is now the confusing part

| build | resets |
|---|---|
| step 263 baseline | 3 / 8 |
| display frozen (266) | 1 / 4 |
| critical sections real (267) | 1 / 6 |
| level history (268) | **2 / 2** |
| watchdog registers (269) | **2 / 2** |
| comparator instrument (270) | **0 / 6** |

Read as a sequence that looks like a signal, and it is not one. 2/2 twice in a
row and then 0/6 is exactly what a ~35% coin does often enough to mislead
anyone reading it a build at a time — which is what this file has been doing.

P(0 in 6) at 37.5% is about 6%: mildly surprising, not evidence. And each of
the 2/2 builds is unremarkable on its own. **No build has been shown to change
the rate**, including the two that fixed real defects.

The honest summary of six steps of instrumentation is: the fault is intermittent
at roughly a third, it has not moved, and every hypothesis about *why* has been
eliminated rather than confirmed.

### 270b. Why the breadcrumb only survives some resets

Worth writing down because it explains an absence. The test harness resets the
board by pulsing RTS, which drives **CHIP_PU** — a chip-enable reset that clears
the RTC domain along with everything else. Only `TG0WDT_SYS_RESET` leaves RTC
memory intact.

So a `LAST TICK` line appears **only after a watchdog reset**, never after a
harness reset. That is the desired behaviour and it is not a bug, but it means
a run of clean runs produces no breadcrumb output at all, which reads like a
broken instrument until you know why.

### State

```
watchdog     ~1 in 3 across 28 runs and six builds; unmoved
eliminated   starvation, shell stack, lock contention, the display task,
             blob critical regions, interrupt masking, watchdog
             reconfiguration
armed        ahead (CCOMPARE1 - CCOUNT) and late, waiting for a reset
```

### Next

1. **Catch a reset with `ahead` and `late` populated.** It is one run away
   whenever the fault chooses to appear.
2. If `ahead` is large, step 269's mechanism is confirmed and the fix is in
   `timer.c`'s re-arm guard. If it is small, the tick was armed correctly and
   the interrupt did not arrive — a different and worse problem.
3. Persistence, the first-ARP question, the `hist` hex rendering.

**Seven eliminations, no mechanism, and an instrument waiting for the fault to
appear again.**

---

## step 271 — the stack is a service, not a subroutine

`(this commit)`

```
net   handover -- the stack stays up now
                                     <- 'wifiinit start' has RETURNED
fetch 1: HTTP 200
fetch 2: HTTP 200
fetch 3: HTTP 200
+30s after handover: HTTP 200
+60s after handover: HTTP 200
+90s after handover: FAIL   (the board had rebooted)
```

**The page survives the command that started it.** Until now the whole network
lived inside `net_poll_for()` — one shell command's loop — so when `wifiinit
start` returned, nothing drained the ring or ran lwIP's timers and the board
went silent. Every browser test was a race against that window, which is what
made three of them fail for reasons that had nothing to do with the network.

### 271a. The shape

`net_service_once()` is the loop body, factored out. A **net task** calls it
forever; `net_poll_for()` calls it as before. They never both own the ring:

```c
volatile int g_net_polling;    /* set while net_poll_for owns it   */
volatile int g_net_handover;   /* set when the loop hands it over  */

void net_service_task_step(void)
{
    if (g_net_handover && !g_net_polling) { net_service_once(); }
}
```

The ring is single-consumer by design and stays that way.

The poll is now **60 s, was 600** — it is only the bring-up window, long enough
to reach a lease and report it. The long window existed because the poll *was*
the network.

`TASK_MAX` goes 12 to 13. kmain creates nine and the blob takes two more, which
left exactly one free slot — and taking it would have cost the blob its second.
One slot is 2 KB of DRAM against ~47 KB spare.

### 271b. And the task ids have shifted

```
before:  report=0 a=1 b=2 vm=3 apps=4 shell=5 disp=6 touch=7
now:     report=0 a=1 b=2 vm=3 apps=4 shell=5 net=6 disp=7 touch=8
```

**Every breadcrumb reading in steps 264-270 said "task 6" meaning the display.
From this build on, task 6 is the net task.** Anyone re-reading those steps
against a current board will otherwise reach the opposite conclusion from the
same number.

### 271c. What killed it at +90s

The board had rebooted: the tick counter restarted and climbed again, with no
boot banner inside the sampling window. That is the watchdog, and step 263
predicted this exact outcome — *"at 3 in 8 it will defeat any attempt at
persistence long before the design of the polling loop does."*

So persistence is implemented, works, and is **capped by the reset**. The page
now stays up for as long as the board does, and the board lasts about a minute
and a half.

### State

```
network      serviced by a task; survives the command that started it
page         HTTP 200 repeatedly after handover, out to +60s
watchdog     unchanged, ~1 in 3, and now the only thing bounding uptime
eliminated   starvation, shell stack, lock contention, the display task,
             blob critical regions, interrupt masking, watchdog config
armed        ahead (CCOMPARE1 - CCOUNT) and late, still unread
```

### Next

1. **The reset, still.** It is now unambiguously the last thing between this
   board and a working server, and the instrument for it is already in place
   waiting for one to happen.
2. The first-ARP question; the `hist` hex rendering.

**nat-os is a web server that stays up until it reboots.**

---

## step 272 — the mechanism: the comparator is left in the past

`(this commit)`

```
tickguard 0
LAST TICK : task 7 at tick 12623  lock7  lvl 00000000
            ahead 4208397466  late 2  cfg 0xe01f8000/6000  crit0
```

`ahead` is `CCOMPARE1 - CCOUNT` stored unsigned. Read as **signed 32-bit**:

```
4208397466 - 4294967296 = -86,569,830 cycles
                        = -1.082 SECONDS at 80 MHz
```

**The comparator was armed 1.08 seconds in the past.**

A one-shot comparator whose match has already gone by does not fire again until
`CCOUNT` wraps the entire 32-bit range — **53.7 seconds** at 80 MHz. The
watchdog fires at three. That is every observation of steps 264-271 in one
number: the tick stops, interrupts stay enabled, nothing is held, the watchdog
config is untouched, and the board reboots.

The earlier catch read `ahead 789738` — 9.87 ms, one interval, correct — so
this is not the normal state. It is a specific event.

### 272a. Why `timer_isr`'s guard did not catch it

`timer.c` already guards against exactly this:

```c
int32_t ahead = (int32_t)(g_next - xt_ccount());
if (ahead <= 0 || ahead > (int32_t)g_interval) {
    g_next = xt_ccount() + g_interval;
    g_late++;
}
```

`late 2` — it fired twice all run and neither was this. **The guard lives inside
the handler**, and the handler is what is not running. A check that can only run
when the fault is absent cannot catch the fault. This project has a name for
that shape: UM-NATOS-051 §10.1, the instrument sharing a dependency with the
thing it measures.

### 272b. How a match gets missed

On Xtensa, `CCOMPARE` asserts its interrupt on the cycle `CCOUNT` **equals** it.
If interrupts are masked for that cycle, the match passes and is gone — the
interrupt is not latched for later.

So any region that masks interrupts at level 3 or above across the moment the
counter passes the comparator loses the tick, and loses it for 53 seconds. Two
such regions exist and both are on the WiFi path:

- `phy_enter_critical()` — `rsil 3`, and it predates all of this
- `osi_wifi_int_disable()` — `rsil 3`, added at step 267

Which explains the shape of the whole investigation. The tick is lost while
interrupts are masked; by the time anything can observe, they are unmasked
again and level reads 0 — **which is exactly what `lvl 00000000` has been
saying at every reset**, and why step 268 eliminated interrupt masking. The
measurement was right and the inference from it was wrong: it shows the level
*after* the damage, not during.

### 272c. And a guard that stays

`tickguard 0`: the blob never asks to disable or re-route `INTR_LINE_TIMER1`.
That candidate is eliminated. The guard is kept anyway — `blob_line_map()`
passes every line but the MAC through unchanged, so a mask with bit 15 set
would reach `xt_disable_interrupt(15)` and stop the scheduler, and nothing else
prevents it.

### State

```
mechanism    FOUND: the one-shot comparator is left in the past, so the
             tick stops for up to 53 s and the 3 s watchdog fires
eliminated   starvation, shell stack, lock contention, the display task,
             blob critical depth, watchdog reconfiguration, blob tick
             disable -- all correctly, and none of them was it
```

### Next

**The fix has to run where the tick cannot.** Re-arming belongs at the point
interrupts are *lowered*, not inside the handler: on leaving a critical region,
if `CCOMPARE1` is in the past, set it to `CCOUNT + interval`. Both
`phy_exit_critical()` and `osi_wifi_int_restore()` lower the level and both
already read PS to do it.

**Eight steps, seven eliminations, and the answer was a signed comparison.**

---

## step 273 — NAT-OS IS A WORKING SERVER

`(this commit)`

```
net   handover -- the stack stays up now
+0s +60s +120s +180s +240s :  HTTP 200, every one

    uptime: 589 s
    lwIP rx: 1329 (dropped 0)
    lwIP tx: 37 (errors 0)
    connections: 6
    requests: 6

resets in the whole capture: 0
```

**Nearly ten minutes, no reboot, six requests served.** Before this the board
rebooted roughly every ninety seconds.

### 273a. The fix is three lines, and the bug was in a guard

Step 272 found the mechanism: `CCOMPARE1` left in the past — measured at
−86,569,830 and −64,463,462 cycles — so the one-shot never matches again until
`CCOUNT` wraps, 53.7 s away, and the 3 s watchdog wins.

`task_yield()` writes the comparator on **every voluntary switch**, and it had
a guard:

```c
uint32_t soon = xt_ccount() + 64u;
if ((int32_t)(soon - xt_get_ccompare1()) < 0) { xt_set_ccompare1(soon); }
```

*Only ever earlier, never later* — correct, and paid for with a full debugging
session when writing it unconditionally froze the kernel.

**But it cannot tell EARLIER from STALE.** With the comparator 64 million
cycles behind, `soon - ccompare1` is large and *positive*, the guard reads that
as "already earlier", and every yield sails straight past the one write that
would restart the clock. The system had a rescue path running constantly and
declining to use it.

So the stale case is handled before the guard sees it:

```c
(void)timer_rescue();          /* <- added */
uint32_t soon = xt_ccount() + 64u;
if ((int32_t)(soon - xt_get_ccompare1()) < 0) { xt_set_ccompare1(soon); }
```

`timer_rescue()` re-arms only if the deadline has already passed. It is also
called from `osi_wifi_int_restore()`, the blob's critical-section exit, which is
one of the windows that eats the match.

### 273b. Why it is a STATIC INLINE

`osi_wifi_int_restore()` is **windowed**. A windowed `call8` into a call0
function is the violation this project has paid for five times, so the rescue
lives in `timer.h` as a static inline and its state is exported for it.
Verified in the disassembly: that function contains **no call of any kind**.

Deliberately **not** wired into `phy_exit_critical()`, which also lowers the
level: `phy_host.c` is compiled into the pre-linked blob as well as the kernel,
and an inline there would reference kernel globals the blob cannot resolve.

### 273c. Why every earlier hypothesis missed it

Seven were eliminated correctly and none was it, because the fault leaves no
trace in the place everyone looked. The tick dies *while interrupts are masked*;
by the time anything observes, they are unmasked again and the level reads 0.
`lvl 00000000` at every reset was a true measurement of the wrong instant.

It took carrying `CCOMPARE1 - CCOUNT` through the reset — and reading it as
**signed** — for the answer to be a single number.

### State

```
nat-os     associates with WPA2-PSK, completes the four-way handshake,
           holds a DHCP lease, and serves HTTP over CCMP
uptime     589 s and counting, 0 resets
network    serviced by a task; survives the command that started it
```

### Next

1. **Confirm the rate is actually zero**, not merely lower. One 10-minute run
   is one run; the fault was ~1 in 3 and this file has been wrong about small
   samples five times.
2. The first-ARP question of step 263c; the `hist` hex rendering.
3. Group-key rekeying, roaming, PMKSA caching, WPA3/SAE, the all-channel scan.

**A from-scratch operating system, on a reverse-engineered radio, over an
encrypted link, serving a page and staying up to do it.**

---

## step 274 — the fix confirmed, and the rescue proved to fire

`(this commit)`

```
ten trials, resets per trial:  0 0 0 0 0 0 0 0 0 0
every trial: handover reached, DHCP lease held

trial 9:  tickguard 0  tickrescue 1
trial 10: tickguard 0  tickrescue 0
```

**Ten runs, zero resets.** The fault ran at roughly one in three across 28 runs
and six builds (steps 263-270), so ten clean runs is about 1.7% likely by
chance. That is an answer rather than a hope, which is what step 273 said it
still needed.

### 274a. And the fix is REACHED, which matters more than the streak

`tickrescue 1` on trial 9. **The rescue fires.**

Without that number ten clean runs would prove nothing: a fix that is never
reached is indistinguishable from a fault that did not appear, and this file
has mistaken one for the other before. The counter separates them —
`timer_rescue()` found the comparator already past its deadline, re-armed it,
and the board carried on where it would previously have rebooted.

Trial 10 reads `tickrescue 0` and also did not reset: the fault simply did not
occur that run. Both readings are consistent with a fault at roughly a third
and a rescue that catches it.

**A caveat on the count.** It is printed during `wifiinit`, before the poll and
the handover, so it only covers bring-up. Rescues during the long serving
period are not in this number, and the true firing rate is therefore at least
what is shown and probably higher.

### 274b. `tickguard 0`, still

The blob has never once asked to disable or re-route `INTR_LINE_TIMER1` across
every run since step 272. That candidate stays eliminated, and the guard stays
in place — `blob_line_map()` passes every line but the MAC through unchanged, so
nothing else prevents it.

### State

```
nat-os    associates with WPA2-PSK, completes the four-way handshake,
          holds a DHCP lease, serves HTTP over CCMP, and stays up
watchdog   0 in 10 against a ~1 in 3 baseline; rescue confirmed firing
```

### Next

1. The first-ARP question of step 263c.
2. The step-259 anomaly, now narrowed to the call site rather than the register.
3. The `hist` nibble rendering, which prints task 10 as `:`.
4. Group-key rekeying, roaming, PMKSA caching, WPA3/SAE, the all-channel scan.

**The reset is fixed, the fix is proved to run, and the server stays up.**

---

## step 275 — two of the small ones closed

`(this commit)`

### 275a. The first-ARP question is closed, and it was a symptom

Step 263c recorded that both successful fetches in this project had been made
*after* pinging the board, and that a fetch without it had failed every time —
including a run with a lease and no reset. It was flagged as possibly a slow or
dropped first ARP reply, which would have been a real station-side defect.

**It no longer reproduces.**

```
fetch, no ping first:   HTTP 200 in 448 ms
```

Repeatedly, promptly, with no ICMP anywhere near it. So there is no ARP defect
to chase: the board answers.

What it was is now obvious in hindsight. Every observation behind step 263c was
taken **before step 262 bounded the drain loop** — when `netif_wifi_tick()` was
never running, so lwIP had no timers: no ARP expiry, no retransmission. A first
SYN whose ARP resolution needed a retry had nothing to retry it, and a ping
happened to prime the cache by a path that did not depend on lwIP's timers at
all.

**A symptom of the timer starvation, wearing the costume of a protocol bug.**
Recorded as such rather than deleted, because the observation was real and the
inference from it was the reasonable one at the time.

### 275b. The history renders as hex

`hist` packed four bits per task and printed `'0' + nibble`, so task 10 came out
as `:`. Tasks 8, 9 and 10 exist — they are the blob's — and steps 265 and 272
both printed histories containing one:

```
step 265:  hist 6:234566
step 272:  hist :578:378
```

Now a table lookup, so 10 reads `a`. A forced watchdog prints `hist 78781234` —
all digits, which does not exercise the fixed path, and the proof is the table
rather than the run: `hx4[10]` is `'a'` and no `:` can be emitted.

### 275c. Still open

The step-259 anomaly: reading `TIMG0_WDTCONFIG0` from the **status line**
reliably stops the crypto (0 of 4 against 4 of 4), while reading the same
register from the **feed path** does not (step 269a). Not the register — the
call site, its position, or the layout shift it caused. Narrowed twice and not
yet explained.

### State

```
nat-os     serves HTTP over WPA2 continuously, 0 resets in 10 runs
closed     the first-ARP question (a symptom of 262), the hist rendering
open       the step-259 call-site anomaly
```

**Two down. The one that is left is the oldest and the strangest.**

---

## step 276 — the step-259 anomaly was the comparator too

`(this commit)`

```
step 259, before the fix:   cfg= readback in the status line   crypto 0 / 4
step 276, after the fix:    the SAME readback, restored        crypto 4 / 4
                                                               leases 4 / 4
                                                               resets 0 / 4
```

The oldest unexplained result in this file is explained, and it was not about
the register.

### 276a. What it looked like, and what it was

Step 259 added a `TIMG0_WDTCONFIG0` readback to the periodic status line. Every
run after it stalled at `[blobtask] wifi prio 23/25 -> HIGH` and never reached
the crypto — 0 of 4, against 4 of 4 without it. Step 269a then found that the
*same register read from the feed path* was harmless, which narrowed it to the
call site and left it there, twice narrowed and never explained.

UM-NATOS-052 §3 drew the strongest available conclusion at the time: *"an
instrument that stops the machine reports nothing about the machine."* That was
right about the effect and wrong about the cause.

**It was the comparator.** A longer status line is more time in the report task
per pass, which is more opportunity for a masked window to fall across the cycle
`CCOUNT` equals `CCOMPARE1`. The match is lost, the tick dies for 53 seconds,
and the board silently stops — during the crypto, because the crypto is the
longest thing it does, sixty seconds of PBKDF2 with the report task running
alongside it.

Restore the exact readback on top of step 273's rescue and it is harmless: four
runs, four crypto completions, four leases, no resets.

### 276b. Three symptoms, one bug

That makes three separate things in this log that were the same fault wearing
different costumes:

| step | looked like | was |
|---|---|---|
| 259 | a register that breaks the crypto when read | a lost tick during a long task |
| 263c | a slow or dropped first ARP reply | lwIP's timers not running (262) |
| 264-272 | starvation, the display, masking, watchdog config | a comparator left in the past |

Each was investigated as its own defect and each produced correct, useful
eliminations. **None of them was wrong; all of them were downstream.**

### 276c. Stated as inference, not proof

This is an A/B across a fix rather than a direct observation of the mechanism.
The readback was harmful before step 273 and is harmless after it, on 4 runs
each way, and the proposed chain — longer line, longer task, wider masked
window, lost match — is consistent with everything measured in steps 272-274.
It is not the same standard as catching `ahead` negative in the act.

The readback is **kept**. It is a real instrument on a real register, it is now
harmless, and removing it would leave the file with a warning about a call site
that no longer misbehaves.

### State

```
nat-os     serves HTTP over WPA2 continuously
closed     the reset (273/274), the first-ARP question (275),
           the hist rendering (275), the step-259 anomaly (276)
open       group-key rekeying, roaming, PMKSA caching, WPA3/SAE,
           and the all-channel scan that has panicked since step 202
```

**Every small thing on the list is closed, and three of them were the same bug.**

---

## step 277 — the keyboard reaches the rainbow bar

`(this commit)`

Not a wifi step. Recorded here because this log is the project's history and
skipping the interface work would make the wifi app that follows arrive without
the ground it stands on.

### 277a. The shell keyboard grows into the gap

The keyboard was `KEY_H 26` and stopped short of the spectrum strip along the
bottom, leaving a band of dead screen. Rows are 42 px now, and the geometry is
stated rather than trusted:

```c
#define KB_Y  (SPEC_Y - KEY_ROWS * KEY_H)
_Static_assert(KB_Y + KEY_ROWS * KEY_H == SPEC_Y, "");
```

`SPEC_Y` moved from `kmain.c` to `display.h` so both the keyboard and the strip
compute from the same number instead of agreeing by coincidence.

### 277b. And the bottom row stopped responding

Reported immediately: the bottom buttons did nothing. The **hit test still read
`y >= DESK_H`**, the old boundary — so the two grown rows drew in a region the
touch handler thought belonged to the desktop. Drawing and hitting had been the
same expression by accident and were now two, and only one had been changed.

Fixed in both `term.c` and `notes.c`, and the note pad's keyboard was brought to
the same size in the same change: a keyboard that is one size in one app and
another size in the next is a bug that only looks like a preference.

### 277c. A near miss worth writing down

The first attempt at suppressing desktop chrome inside these views returned
early from `desktop_chrome_touch()` — which would also have killed the top-right
exit button, **trapping the user in the shell with no way back**. Caught before
flashing, by reading what else that function owned. The guard now sits below the
exit test, and the reason is in the comment so it is not undone later.

---

## step 278 — squares becomes a wifi app

`(this commit)`

The launcher's `squares` was a VM program. The wifi view cannot be one: reaching
the radio means calling the vendor blob, and a bytecode application has no path
to it **by design**. So this is a native view, routed like the shell and the
note pad.

`kernel/wifiapp.c`, `kernel/wifiapp.h`, `MODE_WIFI`, `DESK_ACTION_WIFI`, and a
`wifi_scan_channel()` in `wifi_osi_impl.c` that returns scan results **as data**
— one channel per call, so the sweep can be painted as a list filling in rather
than a five-second freeze.

Two supporting pieces:

- `wifi_join_ssid()` re-derives the PMK for a new SSID. `g_hs_pmk_ready` exists
  so the 15-second PBKDF2 runs once; a different network needs a different key,
  so `g_hs_pmk_ready_reset()` was added to say so explicitly.
- The view sits in **irom**. `linker.ld` gained `wifiapp.c.o`, and `notes.c.o`
  and `term.c.o` went with it — iram had 128 KB and the ISR trampolines in
  `wifi_osi_impl.c` need to stay in it.

### 278a. And it did not work

Reported as "I don't see it doing anything", then as "it just says join failed".
Both were true and neither was the launcher. See step 279.

---

## step 279 — a status line that claimed a join that never happened

`(this commit)`

### 279a. What the user saw

`join failed`, on a freshly booted board, from an app that had never attempted
a join.

### 279b. What it was

Two defects, and the first one hid the second.

**`ST_FAILED` carried three meanings.** `scan_step()` set it when
`!blob_ready()` — the radio was never brought up — and `draw_status()` rendered
every one of them as `"join failed"`. So a radio that does not exist reported
itself as a join that was refused, which points the reader at the passphrase,
the access point, the handshake: everywhere except the actual cause.

This is the **sixth** time this project has found a status claiming an outcome
for work that never ran, after `_event_post`, `_task_delay`, the event groups,
`_queue_send_from_isr` and the blob's critical sections (UM-NATOS-053 §4). The
difference is the reader. The other five lied to a log; this one lied to a
person, who then reported the lie back as the symptom.

Split into `ST_NORADIO`, `ST_STARTING` and `ST_NOSTART`, each saying what it is.

**And the app could not start the radio at all.** It could scan and it could
join — both of which need a radio that *something else* had already brought up.
So it worked on a board where the shell had been used first and was inert on a
board that had just booted. **An app that only works after you have used a
different app is not an app**, and that is the whole of why the first version
appeared to do nothing.

`start_radio()` does what `wifiinit start` does: `blob_map` → `blob_init` →
`phyinit_run_at` → the five settled flags → `wifi_bringup`. The flag values are
**copied from the shell, not re-derived** — they are the answers to steps
252–257 and this view must not become a second opinion about them.

One button, whose label follows the radio: `start` when it is down, `scan` when
it is up. Two buttons would mean one of them is always the wrong thing to press.

### 279c. A hazard flagged and withdrawn

I recorded, while writing 278, that `scan_step()` calling `blob_map()` from a
flash-resident view was the step-198 fault — reprogramming the flash MMU with
the cache off, from code executing out of the mapping being rewritten.

**It is not.** `blob.c` says so in as many words:

> *"Safe to run with the cache off because everything reachable from here —
> this file, uart.c, critical.h — is IRAM-resident. The cache is back on before
> returning to shell.c, which is not."*

`blob_map()` rewrites the **blob's** window at 0x40300000, not the kernel's, and
`shell.c` has called it from irom since step 190. Step 198 was read as a general
rule when it was a statement about one particular mapping. Recorded rather than
quietly dropped, because a hazard raised and then withdrawn is exactly the kind
of thing that gets re-raised in six months by someone reading the first half.

### State

```
wifi app   starts the radio, sweeps 13 channels, joins on a double tap
status     five distinct states; none of them claims work that did not run
open       measure it on a cold-booted board -- NOT YET CONFIRMED
```

---

## step 280 — the app does the whole job, and one network is one row

`(this commit)`

### 280a. Measured, on a cold-booted board

The board was reset by the capture's own port open, booted to the desktop, and
then given exactly two taps: the wifi icon, and `start`. Nothing was typed and
the shell was never opened.

```
4way pmk=1 step=6 m1=1 m3=1 done=1 micbad=0 why=0
wpa conn=1 disc=0 cbreason=none
wpa mgmt calls=3 connect=1
lwip rx 223 drop 0  tx 10 err 0
```

`step=6` is `auth_done`. Message one received, message three received, pairwise
key set, group key set, `ptk_init_done`, `auth_done` — with `micbad=0` and
`why=0`, so no MIC was rejected and no key unwrap failed.

**nat-os brought up a vendor radio it reverse-engineered the interface to, and
negotiated a WPA2-PSK link, from an icon tap.** Step 279's `NOT YET CONFIRMED`
is now confirmed.

### 280b. One row per network, not one per sighting

Reported as the SSID appearing three times.

`scan_step()` appended whatever each channel returned. The 2.4 GHz channels
overlap by about 20 MHz, so a beacon transmitted on channel 6 is receivable
while the radio is parked on 5 and on 7, and the blob reports it on each. Three
rows, one access point, and **all three readings correct** — the defect was in
treating a sighting as a network.

Merged on SSID, strongest sighting winning and carrying its channel. On SSID and
not BSSID deliberately: a mesh with three radios behind one name is one row,
which is also what someone choosing a network to join wants.

### 280c. An instrument that reset the run it was measuring

The capture script opened the serial port, and on a dropped link it reopened it.
**Opening the port resets the board.** So a link glitch during a 90-second
bring-up reopened the port and rebooted the board halfway through — and the log
showed a clean boot banner, which reads as a spontaneous reset rather than as
one the instrument caused.

Caught by the tick counter: `t=307963` before the drop, `t=4089` after.

Deasserting DTR and RTS before open is **not** sufficient, and this was measured
rather than assumed:

```
noreset_test.py:  open #1 last tick 2193
                  open #2 last tick 37      -> RESET across reopen
```

So the port is opened **once** and never reopened; on a drop the capture writes
`RUN IS VOID` and stops. A run that is void must read as void, not as a board
that mysteriously rebooted.

This also retracts something said several times in this session: *"the board is
NOT reset, so tap now"*. Every capture opened this session reset the board. It
cost nothing when nothing was in flight and it cost a whole run today.

UM-NATOS-051 §10.1 and UM-NATOS-053 §6.1 are both about instruments that share
a dependency with what they measure. This is the third, and the first where the
instrument did not merely fail to see the fault but **caused** it.

### 280d. Still open

- **No DHCP lease.** `dhcp offer/ack 0/0` with `tx` frozen at 10 while `rx`
  climbed 124 -> 223. The link is up, authenticated and receiving; addressing
  has not completed. Below the app, in the lwIP layer that worked from the shell
  path in step 273 — so something differs between the two routes into it, and
  that difference is the next thing to find.
- **The USB link drops.** Three times today, once with no radio activity at all,
  each as `GetOverlappedResult: Access is denied` followed by re-enumeration.
  Host-side: cable, port or supply. Not a board fault and not a firmware one.

### State

```
wifi app   icon tap -> radio up -> scan -> associate -> WPA2 handshake complete
list       one row per network
open       DHCP does not complete by this route; the USB link is unreliable
```

---

## step 281 — three ways to make a view unusable, all of them mine

`(this commit)`

### 281a. A working exit button, painted over

Reported as "I can't reopen the wifi app because it doesn't have an X button".

`desktop_chrome_touch()` accepts the top-right 22x22 corner as *leave this
view*, and checks it before anything else — so the exit worked the entire time.
`draw_all()` painted a full-width header across y 0..22, over the top of it.

Step 277c records nearly trapping the user in the shell by removing that
handler. **This is the same trap reached from the other side**: the handler was
left alone and its pixels were taken instead. "The button still works" is not a
defence when nobody can see it. There is a red `x` drawn there now.

### 281b. Ninety seconds with nothing listening

Reported as "it says starting radio, I'm not seeing any network, and the x
button isn't working".

`start_radio()` was called straight out of `wifiapp_touch()`, which runs on the
**touch task** — so for the whole ninety-second bring-up the task that reads the
glass was inside `wifi_bringup()`. The status bar said `starting radio -- 90 s`
while every press went unread, including the way out.

**A status line that asks the user to wait, displayed by a system that has
stopped accepting input, is worse than no status line** — it invites exactly the
presses it cannot answer.

Moved to the net task, which has nothing to service until the radio exists and
whose frame at that point is shallower than the touch task's was. The touch task
stays answerable and the display task keeps painting. Leaving the view
mid-bring-up is allowed and the bring-up continues: the radio is the system's,
not the view's.

### 281c. And that created a second caller into the blob

Reported as a loud continuous beep and a white screen on re-entering the view.

`wifiapp_frame()` runs on the **display** task and calls `scan_step()`, which
calls into the blob. After 281b the bring-up runs on the **net** task and does
the same. `blobcall.c` had left the marker:

> *"Today there is exactly one caller, so this should stay zero; if it does not,
> something has started entering the blob from a second context and the
> assumptions above are worth re-reading."*

**I made the second caller.** The symptom fits: a display task parked inside the
blob stops painting and stops silencing the click, which is a white screen and a
tone that never ends.

Guarded at the caller I added — `scan_step()` is skipped while a bring-up is in
flight — rather than by changing the exclusion everything else depends on. The
sweep pauses for the duration, and the bring-up ends by starting a fresh sweep,
so nothing is lost.

**NOT CONFIRMED BY MEASUREMENT.** The mechanism fits the symptom and fits a
marker left in the code for exactly this, and that is not the same as having
read it happen. Four capture runs were lost to the serial link (§281d) and none
of them caught the panic. Written down as the leading candidate, not as the
answer.

### 281d. The link, and what it changes

Four runs void today, each `GetOverlappedResult: Access is denied` followed by
re-enumeration, once with the radio completely idle. Host-side — cable, port or
supply — not a board or firmware fault.

The consequence for method: **a board whose log cannot be relied on has to
report from the glass.** The scan now reads `scanning ch 7 -- 2 found`, so a
sweep that runs and finds nothing is distinguishable from one that never
started, without a serial link to watch it on.

### State

```
wifi app   exit visible; bring-up off the touch task; one blob caller at a time
scan       reports channel and count on the display
open       the white-screen crash is EXPLAINED BUT NOT MEASURED
           no DHCP lease by this route
           the USB link drops
```

---

## step 282 — confirmed from the glass

`(this commit)`

```
ivory-billed
TC7NR
```

Two networks, one row each, after an icon tap and a `start`. No white screen, no
stuck tone, and the exit button visible and answering throughout.

That confirms three things by observation:

- **280b**, the merge. The sweep sights the same access point on adjacent
  channels and now reports it once. Two APs on the air, two rows — and the
  second network is the check that matters, because a merge that collapsed
  *everything* into one row would look identical to a correct one if only a
  single AP were present.
- **281a and 281b**, the exit and the deferred bring-up.
- **281c** only to the extent that the crash did not recur. The guard is in and
  the symptom is gone, which is consistent with the explanation and is **not the
  same as having measured the mechanism**. Step 281c stays marked as a candidate.
  Nothing was read; a symptom stopped appearing.

The whole path — radio up, thirteen channels swept, networks listed, WPA2
four-way handshake completed (step 280a) — driven by two taps on a board that
had just booted, with no shell and no serial link involved.

### What is left

1. **No DHCP lease by this route.** `dhcp offer/ack 0/0`, `tx` frozen at 10
   while `rx` climbed. The link is authenticated and receiving; addressing does
   not complete. It works from the shell path (step 273), so the two routes into
   lwIP differ somewhere and that difference is the next thing to find.
2. **The white screen is explained, not measured.** §281c.
3. **The USB link drops.** Four void runs in one session, once with the radio
   idle. Host-side.

---

## step 283 — the address goes on the screen, and a retraction

`(this commit)`

### 283a. Retraction: the DHCP evidence was worthless

Step 280d and 282 both cited this as evidence that DHCP had failed:

```
net  frames 0  dropped 0  dhcp offer/ack 0/0  arp 0->0  icmp 0->0  [no IP]
```

**Every one of those counters is zero by construction.** They belong to the
hand-written network path in `net_handle()`, which runs only when `g_use_lwip`
is 0. It is 1. lwIP owns the stack and never touches them.

This file warned about it, at step 234, in a comment sitting a few lines above
the code that prints them:

> *"In lwIP mode the counters below belong to the hand-written path and stay at
> zero, which reads as 'nothing is happening' when in fact everything is."*

Read a stale instrument, reached the conclusion it was documented to invite, and
reported it twice. The conclusion may still be right — see 283b — but it was not
supported by anything quoted for it.

### 283b. What the evidence actually is

The absence of the line lwIP prints when it binds:

```
lwip      DHCP bound -- address ...
```

`netif_wifi_report()` announces the first binding and `net_poll_for()` calls it
every pass. The `lwip +30s`, `+40s` and `+50s` lines were captured, so the stream
was being read across that window and an announcement in it would have appeared.

Weaker than a positive measurement, and the right instrument to have quoted.

### 283c. The address belongs on the screen anyway

`netif_wifi_ip()` is a pure accessor — `netif_wifi_report()` also announces, and
a view polling every frame must not decide when that happens.

`ST_JOINED` now reads `ivory-billed 192.168.1.140` in green, or
`ivory-billed -- no IP` in yellow. **"Joined" and "on the network" are not the
same claim, and this view had been making the stronger one** — an association
with no lease looked identical to success, which is precisely the state the
board has been in.

It repaints when the address arrives, because DHCP binds seconds after the join,
after the paint that reported it.

And it is readable without a serial link, which four void runs have shown is the
only kind of instrument worth building here.

### State

```
open   whether DHCP binds at all -- now answerable from the glass
       the white screen: explained, not measured (281c)
       the USB link drops
```

---

## step 284 — the same defect, three lines away, unfixed

`(this commit)`

Reported as selecting a network and the system crashing outright.

### 284a. What it was

`join()` was still called **directly from `wifiapp_touch()`**. So it ran on the
touch task, blocked it for the ~15 s of PBKDF2, and entered the blob while
`scan_step()` on the display task could be doing the same.

That is exactly the defect step 281b and 281c described and fixed — **for
`start_radio()` only**, while the identical second caller sat three lines below
it in the same function. Worse, `join()` also called `draw_status()`, so once it
moved off the touch task it would have had two tasks writing the display SPI.

Fixing one instance of a fault and not looking for the second is how the same
bug gets found twice. It cost the user a crash to find what reading the function
I had just edited would have shown.

### 284b. And the guard was the wrong shape

Step 281 added a condition to the sweep:

```c
if (g_scan_ch && !g_want_start && g_state != ST_STARTING) { scan_step(); }
```

Step 284 needed two more terms on it. **A guard that grows a term per caller is
the wrong construction** — it makes correctness depend on every future caller
remembering to appear in a list that lives somewhere else.

The sweep is now stopped at the point of asking:

```c
g_scan_ch   = 0u;      /* the sweep stops HERE, before the request */
g_want_join = (int)i;
```

so no window exists in which a sweep and a blob-entering request coexist, and
the condition is back to `if (g_scan_ch)`. Structure rather than vigilance.

### 284c. The passphrase, said plainly

Also reported: *"I couldn't even enter a password or anything."*

That is by design and it is a real limitation. `wifiapp.h` states the scope:
tapping a network joins it with the passphrase compiled into
`kernel/wifi_secrets.h`. It is right for the network this board lives on and
wrong for every other one — **`TC7NR` cannot be joined and will fail the
four-way handshake**, which the view will report as `join failed`, correctly and
uselessly.

Typing a passphrase needs the multi-tap keyboard the shell and note pad already
have. It is a known missing piece, not an oversight, and it is the obvious next
piece of work on this view.

### State

```
join       off the touch task; no second blob caller by construction
open       whether a sweep works at all once associated -- the radio is parked
           on the AP's channel and the other twelve may return nothing
           whether DHCP binds (283)
           the white screen (281c), the USB link
```

---

## step 285 — type the passphrase, and keep it

`(this commit)`

Asked for directly: enter the password for a network, and have the device
remember it. Step 284c had recorded the missing piece; this is it.

### 285a. The keyboard, factored on the terms its own comment set

`term.c`, above its copy of the multi-tap logic:

> *"This is a SECOND copy of the note pad's cycling logic... factoring it out
> means changing a working app to serve a new one, which is a worse trade
> tonight than two copies with a comment. **If a third consumer appears, factor
> it then.**"*

A third consumer appeared. `kernel/keyboard.c` is that factoring, written to
match both existing copies exactly — same tables, same phone-keypad order, same
800 ms settle — so migrating them to it is a deletion rather than a change in
how either app feels.

**term.c and notes.c are NOT migrated yet, and that is a debt, not a decision.**
Moving two working apps onto a module that has never run would risk three apps
on one untested change. They follow once this one has been used in anger. Until
then there are three copies, which is worse than two, and it is written here so
it is owed rather than forgotten.

### 285b. Where a credential lives, and why not in store.c

`store.c` rejects a record whose version does not match, so adding fields to
`store_t` **discards every existing record — including the touch calibration.**
Making someone recalibrate the screen to gain a saved password is a bad trade
and an avoidable one.

`flash.h` reserves 0x200000 for the record and 0x201000 for the message sector,
and the blob starts at 0x220000. The 120 KB between is unused. `wificred.c`
takes one sector of it at 0x202000, with its own magic, version and checksum, so
the two records migrate and fail independently.

Eight slots. A ninth drops the oldest rather than refusing: someone typing a
password wants it to work now, and an error they cannot act on without a way to
browse and delete slots is not a better answer.

**It is not secure, and the header says so rather than implying otherwise.** The
passphrase sits in plaintext flash, readable by anyone who can read the chip.
That is the same guarantee as the compiled-in `WIFI_STA_PASS` it replaces, which
sits in plaintext in the binary — nothing is lost and nothing is gained.
Encrypting it needs a key, and there is nowhere on this part to keep one that an
attacker with the flash cannot also read. eFuse blocks and flash encryption are
the real answer and are separate work.

### 285c. What it does now

- Selecting a network with a saved passphrase **joins straight away**.
- Selecting one without asks for it, on a keyboard reaching the rainbow bar.
- The passphrase is **saved before the join is attempted**. One that turns out
  to be wrong is still the one the user meant to type, and losing it to a failed
  join means retyping it on multi-tap to find out why.
- `wifi_join_ssid_pass()` points `g_hs_pass` at storage that outlives the
  caller. A typed passphrase wins; with none typed the compiled-in one still
  applies, so nothing that worked before stops working.
- Shown in the clear, not masked. Multi-tap without seeing the result is close
  to impossible — the cycling depends on watching the letter change — and
  masking would buy theoretical secrecy on a device that stores the string in
  plaintext anyway.

### 285d. And the list got taller

Taking the application band for the keyboard means the view owns everything
above the rainbow bar, so the list uses it: **eleven rows instead of eight**, on
a screen whose whole purpose is choosing from a list.

### State

```
wifi app   scan, pick, type a passphrase, join, and it is remembered
owed       migrate term.c and notes.c onto keyboard.c (285a)
open       whether a sweep works while associated; DHCP (283); 281c; the USB link
```

---

## step 286 — the empty list explains itself

`(this commit)`

### 286a. Three reports, one cause, and it was the interface

"Not seeing any networks", three times, across three different builds. Each time
I looked for a scan bug. Each time I asked for the status bar text to tell the
cases apart.

**The radio was off.** Every flash reboots the board, a fresh boot starts with
the radio down, and the view was opened before `start` was tapped. The app was
correct on all three occasions.

The list said `no networks -- tap scan` in every empty case — including the one
where there was no radio to scan with — and the reason lived in the status bar,
a separate strip. **Reading one without the other gave the wrong answer, and I
asked the user to do the cross-referencing twice rather than fixing the thing
that required it.** An interface that needs two places read to understand one
fact is the defect; the missing networks never existed.

The empty list now carries its own reason: `radio off -- tap start`,
`scanning...`, `radio did not start`, or `none found -- tap scan`. The last of
those is the only one that means a scan problem, and it has never yet been seen.

### 286b. A real one, found while looking

`scan_step()` ended the sweep with `g_state = ST_IDLE`, unconditionally. The
bring-up joins, sets `ST_JOINED`, and then starts a sweep — so **the sweep
erased a successful join from the display a few seconds after it appeared.**
The scan finishing says nothing about whether the board is associated, and it
was overwriting the one field that does.

Now only `ST_SCANNING` returns to `ST_IDLE`.

### 286c. A target you can hit

The scan/start button was 48x16 at the very bottom of the screen — where a
resistive panel's calibration is worst and a fingertip is widest. 56x28 now. The
hit test always accepted the whole strip; the button simply did not look like
the area it occupied, which is its own kind of lie.

### State

```
open   none found -- tap scan  has never been observed; there may be no scan bug
       DHCP (283), the white screen (281c), the USB link, term/notes on keyboard.c
```

---

## step 287 — flash-resident code driving the flash bus

`(this commit)`

Reported as: tapped a network, the screen turned white.

### 287a. What it was

Tapping a network calls `wificred_get()` to look for a saved passphrase, which
calls `flash_read()`. That is the **first flash operation this firmware performs
at runtime**, and it happens at exactly the moment of the tap.

`wificred.c` was in **irom**. `flash.c` says what that costs:

> *"SPI1 shares the flash bus with the cache's SPI0, and these registers are how
> the cache issues its own reads. Overwriting them and walking away leaves the
> cache unable to read flash AT ALL — which presented as every string literal in
> the kernel returning 0xFF immediately after the first flash operation."*

Flash-resident code was put in charge of the flash bus. **`store.c`, the only
other module in this kernel that touches flash, has always been in iram** — and
step 285 broke that precedent while adding `wificred.c` to the irom list next to
`wifiapp.c` and `keyboard.c` to save space, without asking why `store.c` was not
already there.

Fixed by placement, verified by address rather than by reading the linker script:

```
flash_read     0x40084ca4   iram
wificred_get   0x4008d37c   iram   (was 0x400d....)
keyboard_touch 0x400d6cd0   irom   -- draws and counts ticks; never the bus
```

### 287b. The hazard I withdrew, and this one

Step 279c withdrew a step-198 hazard about `blob_map()` from flash, correctly:
`blob_map()` is IRAM-resident and restores the cache before returning, and
`shell.c` has called it from irom since step 190.

**The withdrawal was right and the general lesson was not drawn.** The rule is
not "flash-resident callers are fine"; it is "the code that *touches the bus*
must not live on it". `blob_map()` satisfies that and `wificred.c` did not. A
correct retraction of a wrong specific claim left the actual principle
unstated — and four steps later I placed a new file on the wrong side of it.

### 287c. And 281c is still not measured

This is the second white screen with a confident mechanism attached. The first
(281c, two tasks in the blob) was never measured either, and its symptom stopped
after a guard went in. **These may not have been the same fault**, and the
earlier explanation should not gain credibility from this one being found: they
share a symptom because a white screen is what this board does when the display
task cannot fetch its own code or its own strings.

### State

```
open   DHCP (283); 281c unmeasured; the USB link; term/notes onto keyboard.c
```

---

## step 288 — inconsistency is a rate, so count it

`(this commit)`

Reported as: the network list is *inconsistent* — sometimes there, sometimes not.

### 288a. One task in the blob, by construction

`scan_step()` ran on the **display** task. Two things wrong with that, and the
second is the one that matters:

- It blocks for `SWEEP_DWELL` (400 ms) **per channel**, so a thirteen-channel
  sweep froze the display for over five seconds.
- It was **the last place the display task entered the blob** — the hazard step
  281c guarded around instead of removing, and then step 284 had to widen that
  guard, and then narrow it again by stopping the sweep at the point of request.

Three steps of tending a condition that existed only because two tasks could be
in the blob at once. The sweep now runs where the bring-up and the join already
do, so **exactly one task enters the blob**, which is what `blobcall.c` assumed
from the beginning:

> *"Today there is exactly one caller, so this should stay zero."*

**A guard that keeps two callers apart is worth less than not having two.**

### 288b. Refused and quiet are different answers

`wifi_scan_channel()` returned 0 both when the driver refused the scan and when
the channel was genuinely empty. Those are opposite findings and they printed
identically.

`g_scan_refused` counts the refusals, and the empty list now shows what the last
sweep did:

```
none found -- tap scan
13 ch, 11 refused
```

Thirteen channels with eleven refused is a driver that would not look — most
likely because it is associated and parked on the AP's channel. Thirteen with
none refused is an empty band. **The reported symptom is a rate, and a rate
cannot be diagnosed from a message that has no numbers in it.**

This is the third instrument added to the display rather than the log (281d,
286a), for the same reason: a serial link that voids four runs out of five is
not an instrument, and the glass is.

### State

```
open   whether refusals explain the inconsistency -- NOW MEASURABLE, not yet measured
       DHCP (283); 281c unmeasured; the USB link; term/notes onto keyboard.c
```

---

## step 289 — a guard that became a lock, and feedback that says where

`(this commit)`

### 289a. "When I tap a network nothing happens"

Every press in this view began:

```c
if (g_state == ST_STARTING || g_state == ST_DERIVING) { return; }
```

That was added in step 284 so a second press could not queue a second bring-up.
It has no expiry. **If a join or a bring-up ever fails to complete, the view
ignores every press from then on — permanently — while still clicking to say
the press had landed.**

A guard against a second request became a guard against ever using the view
again, and the audio feedback made it worse by confirming presses the code had
already decided to discard.

It expires now, into `ST_FAILED`, at 120 s: well past the 15 s a key derivation
takes and the ~90 s of a bring-up, so anything still pending then is not going
to finish. The view says so instead of going quiet.

**This is the same shape as step 279's status line**: a state the code could
enter and not report. There it was `join failed` for a join never attempted;
here it was silence for presses never processed. Both told the user something
untrue about what the system was doing.

### 289b. Feedback that says WHERE

Asked for: no beep, a white flash on the selected network instead.

The click said *a press landed*. On a list, **where** is the whole message — and
the click was identical whether the press hit the row above, the row below, or a
guard that threw it away. The row now flashes white for a few frames.

The keyboard's click goes too, and it loses nothing: it already draws the live
key blue for exactly as long as another tap can still change that character. That
is strictly more information than a beep — which key, and how long left — and it
was there all along underneath the noise.

`term.c` and `notes.c` keep their click; that decision is theirs, and if they
migrate onto `keyboard.c` (285a) this becomes a flag rather than something
chosen on their behalf.

### State

```
open   whether refusals explain the inconsistency (288b); DHCP (283);
       281c unmeasured; the USB link; term/notes onto keyboard.c
```

---

## step 290 — DHCP binds, and the claim it did not is withdrawn

`(this commit)`

### 290a. Measured

```
4way pmk=1 step=6 m1=1 m3=1 done=1 micbad=0 why=0
lwip      DHCP bound -- address 192.168.1.140
```

**The wifi view does the whole job from two taps**: radio up, thirteen channels
swept, associated, WPA2-PSK four-way handshake complete, DHCP lease bound. The
board is on the network at 192.168.1.140, which is the address step 273 reached
from the shell.

### 290b. Withdrawing the claim, not just the evidence

Steps 280d and 282 said DHCP did not complete by this route. Step 283a already
retracted the **evidence** — the `dhcp offer/ack 0/0` counters belong to the
hand-written path and are zero by construction — while noting the conclusion
might still be right on the strength of a missing `DHCP bound` line.

It was not right. The line is here. **A conclusion that survives the retraction
of its evidence is still just a guess**, and it should have been demoted to one
in 283 rather than kept on the open list for three more steps.

The honest summary of that whole thread: an instrument was misread, the
misreading produced a bug report, the bug report shaped three steps of work, and
the bug did not exist.

### 290c. The net task is now the tightest stack in the system

```
tightest stack=net 664/2048 B free
```

It was `touch 580/2048` when the bring-up ran there (280a) and `app-host
1396/2048` before any of this. The net task now carries the bring-up, the join
and the sweep — every blocking, blob-entering job in the view — and 664 bytes is
the margin that is left.

That is not a fault today and it is the thinnest margin in the kernel. Recorded
because the next thing added to `wifiapp_service()` spends from it, and because
`TASK_STACK_WORDS` is one size for every task (task.h:197 already notes that
this stopped being workable).

### 290d. The panic did not reproduce

A 420 s capture across a full bring-up: no panic, no reset, no fault, and no
`wificred saved` — so the passphrase submit was not reached in that window. The
reported panic remains real and unmeasured; the capture stands ready.

### State

```
works  icon tap -> radio -> scan -> associate -> WPA2 -> DHCP -> 192.168.1.140
open   the save panic (unreproduced); net stack margin 664 B; 281c unmeasured;
       the USB link; term/notes onto keyboard.c
```

---

## step 291 — the panic measured: a lost register window, and my trigger for it

`(this commit)`

Reported as `IllegalInstruction` on tapping a network. Measured this time
instead of explained.

### 291a. What the fault actually is

```
exccause : 0  (IllegalInstruction)
epc      : 0x4009d35e
fault regs: a0 0x00000030   wb 3  ws 0x0000000a
GRANT DRIFT: task.c predicted 0x00000008 but vectors.S wrote 0x0000000a for task 10
frames    : task 10 held 0x0000000a granted 0x00000008 LOST 0x00000002
overlap   : task 10 pushed a switch frame at 0x3ffb29e0 across 0x3ffb2a10
multiframe: 21026 switch-outs with >1 live frame, worst 7 frames
```

`epc` is in **iram**, not flash — so the two theories I was carrying into this
(the flash cache, and a second context in the blob) are both wrong, and were
wrong for the earlier white screens too as far as anything here shows.

Task 10 holds a 7168-byte stack: `BLOB_TASK_STACK_WORDS`, so it is the driver's
own task. It was switched out holding **two** live register windows and restored
with **one** — `LOST 0x00000002` — and resumed on `a0 = 0x30`, a return address
that is not one, and executed it.

This is the window-ownership problem of steps 49-50 and 123-141, in the area
`task.c` describes as needing owners "because more than one task holds frames in
the register file at once". `multiframe` says it has survived 21,026 such
switch-outs. **The fault is pre-existing and is not the wifi view's.**

### 291b. What IS mine: the trigger

`wificred_get()` faulted the credential record in **lazily**, so the first tap on
a network performed a flash read — and `flash_read()` masks interrupts for the
whole SPI transaction. Dropped next to a live blob task, that perturbation
turned a 1-in-21,000 window race into something reproducible on demand.

Primed at boot now, from `kmain`, before the radio or any windowed vendor task
exists. **This does not fix the fault and is not claimed to**; it removes a
trigger this module introduced, at a moment when there is nothing windowed to
disturb.

### 291c. A dead line, and a comment that was the opposite of true

`start_radio()` carried:

```c
blob_task_enable(0);      /* blob task creation still panics (step 190) */
wifi_start_enable(1);
```

`wifi_start_enable()` sets `blob_task_enable(on)` itself — step 214: *"Starting
the driver REQUIRES its task, so enabling one enables the other."* So the first
line was **overwritten one line later**, and the comment beside it described a
state of the world that ended at step 214.

A no-op asking for something that would break the radio, annotated with the
opposite of the current design. It got there by copying the shell's settled
block wholesale (285/279) without re-reading what each line still meant. The
capture is what caught it: `[blobtask] req en=0x00000001` said plainly that the
flag was on when I believed I had turned it off.

### State

```
open   the window-ownership fault (49-141) -- pre-existing, now easily provoked
       wificred_put() still erases flash with the radio live: same trigger class,
       and a far longer masked window. UNTESTED.
       281c's white screen: my explanation for it is now doubtful too
       the USB link; term/notes onto keyboard.c; net stack margin 664 B
```

---

## step 292 — the crash was a call0 into windowed code, and it was mine

`(this commit)`

### 292a. Named, not guessed

The fault survived a reboot in the persistence record and pointed twice at the
same twelve bytes:

```
live dump   epc 0x4009d35e
next boot   LAST FAULT: exception, exccause 0, epc 0x4009d36e (boot #815)

4009d334 T wpa_sta_connect_impl
4009d364 T g_hs_pmk_ready_reset      <- 0x4009d36e is inside this
4009d370 T wpa_hs_derive_pmk
```

`g_hs_pmk_ready_reset()` is twelve bytes long and was added by **step 278**. It
lives in `vendor/windowed/wifi_glue.c`, compiled **windowed**.
`wifi_join_ssid()`, which called it, is in `kernel/wifi_osi_impl.c` and is
**call0**.

A call0 `call0` into a windowed function reaches a `retw` that pops a register
window nobody pushed. It returned onto `a0 = 0x30` and executed it. That is the
`IllegalInstruction`, and it is the same fault in both reports.

### 292b. The correct form was one line away

```c
(void)g_hs_pmk_ready_reset();                              /* direct  */
(void)blob_call((uint32_t)&wpa_hs_derive_pmk, 0,0,0,0);    /* bridged */
```

The next statement in the same block crosses the same boundary correctly, and
the two sat one line apart for fourteen steps. This is the **sixth** time this
project has paid for that crossing and the first where the right answer was
visible without moving the eye.

### 292c. Fixed by not making the call

`g_hs_pmk_ready` is one word. The function existed to zero it. **A memory write
has no calling convention**, so the flag is `extern` now and the call0 side
assigns it directly. Verified in the binary: the symbol is gone, and
`g_hs_pmk_ready` is `B` at 0x3ffc8a60 — data, not text.

The cheapest fix for an ABI bug is to not make the call.

### 292d. What this retires, and what it does not

Both user reports were this: "kernel panic once I entered the password" (the
save requests a join, which calls `wifi_join_ssid`) and "crashing when I tap on
the network" (the passphrase had by then been saved, so the tap joins directly
and skips the keyboard). One bug, two symptoms, and the second only appeared
because the first had **successfully saved a credential** — the feature working
is what exposed it.

**It does not retire step 291's window-ownership finding.** `LOST 0x00000002`
and `GRANT DRIFT` were real readings of a real accounting problem, and
`multiframe` still counts 21,026 switch-outs with more than one live frame. What
changes is that they were the *consequence* here, not the cause: a `retw` with
no matching `entry` is exactly how a window goes missing. Steps 49-141 remain
open; this particular crash is no longer evidence for them.

Step 291b's boot-time prime stays. It removed a real trigger and it was not the
cause of anything.

### 292e. And the touch calibration is gone

```
touch cal    : defaults (run 'cal' to measure)
```

Which explains `rx=400-4095` and a saturated raw X. Unrelated to any of the
above, and worth a `cal` run.

### State

```
open   steps 49-141 window ownership (no longer evidenced by this crash)
       wificred_put() erases flash with the radio live -- still untested
       the USB link; term/notes onto keyboard.c; net stack margin 664 B
```

---

## step 293 — the view scanned the link out from under itself

`(this commit)`

Reported as: the screen says green `ivory-billed 192.168.1.140`, and the URL
does not load.

### 293a. Measured from the other end

```
this machine        192.168.1.102      (same subnet)
ping 192.168.1.140  no reply
arp -a 192.168.1.140  No ARP Entries Found
HTTP                timed out
```

**No ARP entry** is the useful one: the board was not answering at the link
layer at all, so this was never an HTTP question. Meanwhile the board itself was
healthy — shell responsive, `t=296080` and climbing, no reset.

### 293b. What it was

`start_radio()` ended with:

```c
/* Either way, fill the list, so what appears next is a set of networks to
 * choose from rather than an empty view with a live radio. */
g_scan_ch = 1u;
```

Unconditionally — **including immediately after a successful join.**

A station has one radio. A passive sweep retunes it away from the access point
for `SWEEP_DWELL` (400 ms) per channel, thirteen channels, five seconds across
the band. The view was doing that seconds after associating and binding a DHCP
address, to a user who had not asked for it.

The address on screen was true when it was printed and dead by the time anyone
used it. **Every ingredient of this was written by me in step 279, and the
comment justifying it reads like a courtesy.**

Scanning while connected is a real trade and it stays on the scan button, where
someone chooses it. Doing it unasked, right after connecting, is not a trade.

### 293c. And the green line was overclaiming

`netif_wifi_ip()` returns lwIP's address, and lwIP keeps its address when the
radio goes away. So the view showed a working address in green — the strongest
claim it can make — for a link that no longer existed.

It now consults the driver's own disconnect callback and reads `-- link lost`.

**Against a baseline, not against zero.** `g_wpa_disc_cb` counts every
disconnect since boot, so testing it for non-zero would report a live link as
lost because of a drop that happened before this join and was recovered from.
The count is stamped when `ST_JOINED` is entered and compared against that. The
question is not "has this board ever disconnected" but "has it disconnected
since it connected this time" — the same distinction step 283 got wrong with the
DHCP counters, caught this time before it shipped.

### State

```
open   whether the link now survives the bring-up -- the point of this step
       wificred_put() with the radio live, still untested
       steps 49-141; the USB link; term/notes onto keyboard.c
```

---

## step 294 — the page, from an icon

`(this commit)`

Confirmed from a second machine on the same network, not from the board's own
account of itself:

```
HTTP 200   350 bytes in 625 ms
Reply from 192.168.1.140: time=86ms TTL=255    0% loss
arp -a     192.168.1.140   5c-01-3b-50-3f-64   dynamic
```

The ARP entry matters as much as the 200: it is the board answering at the link
layer with its own MAC, which is the thing that was absent in 293a.

**Every layer at once, launched by two taps on a touchscreen:**

- the radio brought up from an icon — no shell, no serial, no typed command
- a vendor blob running behind an interface this project measured out of a
  disassembly rather than took from a header
- WPA2-PSK negotiated by this project's own supplicant: PBKDF2-SHA1 at 4096
  rounds, PTK derivation, HMAC-SHA1 MIC, AES key unwrap, `step=6 micbad=0`
- DHCP bound
- TCP and HTTP over the encrypted link, to a browser that knows none of it

### 294a. What actually changed

Nothing in this step. 293 stopped the view sweeping immediately after a join,
and the sweep was the whole difference between a board that answered and a board
that showed a green address and nothing else.

Worth stating plainly: **the stack was working the entire time.** Steps 280-292
found and fixed six real defects, and none of them was why the page did not
load. The page did not load because the view retuned the radio away from the
access point five seconds after connecting, for no reason a user had asked for.

### State

```
works  icon tap -> radio -> associate -> WPA2 -> DHCP -> HTTP 200, from a browser
open   wificred_put() with the radio live, still untested -- the TC7NR path
       steps 49-141 window ownership; the USB link; term/notes onto keyboard.c
       the touch panel reads saturated with calibration at defaults ('cal')
```

---

## step 295 — the feature worked and looked like nothing happening

`(this commit)`

Reported as: tapping `ivory-billed` gives no password prompt, maybe it needs
reflashing.

### 295a. It was already saved

The passphrase typed during the step-292 crash **had been written to flash
before the crash**. `wificred_put()` runs and returns; the fault was in the join
that followed it. So the credential has been on the board ever since, through
every reflash — the kernel image is at 0x10000 and the credential sector is at
0x202000, and nothing in the build touches it.

So the tap found a saved passphrase and joined without asking. **That is the
whole feature: ask once, remember forever.** It has been working since before
the crash that hid it was found.

### 295b. Which nothing on screen said

The list showed a network the board had the key for exactly as it showed one it
did not. There was no way to tell "I know this one" from "I did not respond to
your tap", and the second reading is the natural one when the expected keyboard
does not appear.

A green dot on rows with a saved passphrase. It reads the primed RAM cache and
never touches flash, so it is safe on a draw path — the distinction step 291 was
about.

**This is the fourth time in this arc a working system read as a broken one
because the interface did not report its own state**: `join failed` for a join
never attempted (279), an empty list that would not say why (286), a green
address for a link that had gone (293), and now silence for a credential it
already had. Each was a real defect in the same place — not in what the code
did, but in what it said it was doing.

### State

```
works  ask once, remember forever -- confirmed by its own silence, now visible
open   an explicit way to FORGET a credential; there is none, and re-testing the
       ask path currently needs the sector erased by other means
       steps 49-141; the USB link; term/notes onto keyboard.c; touch 'cal'
```

---

## step 296 — forget

`(this commit)`

The store could be taught a passphrase and never untaught. That made a wrong
password uncorrectable and put the "ask me" path out of reach for every network
already known — which is how step 295 was reported in the first place: *"I can't
double tap it to enter password."*

`wificred_forget()` removes the entry, **closes the gap** rather than leaving a
hole (`wificred_get` walks `count` entries in order, so a hole would shadow
everything behind it), and **wipes the vacated slot** — the passphrase would
otherwise stay legible in flash after someone asked for it to be gone, which is
not what "forget" means to the person who tapped it.

The button appears only when a selected network has something to forget. A
control that is always present but usually inert teaches people to ignore it;
one that appears exactly when it applies explains itself by appearing.

### State

```
works  scan, pick, type a passphrase, join, remembered, and now forgettable
open   steps 49-141; the USB link; term/notes onto keyboard.c; touch 'cal'
```

---

## step 297 — the whole path, exercised

`(this commit)`

```
HTTP 200   353 bytes in 480 ms
arp -a     192.168.1.140   5c-01-3b-50-3f-64   dynamic
```

353 bytes against the 350 of step 294: the page is generated, not cached, so
this is a live fetch and not a stale answer.

With `forget` in place (296) the last untested piece finally ran:

- the credential was removed from flash and the dot went with it
- the double tap reached the **keyboard** instead of joining silently
- `wificred_put()` **erased and rewrote a flash sector with the radio live** —
  flagged as structurally risky and untested since step 285, and now run
- the join used the **typed** passphrase, through the `g_hs_pmk_ready` write
  that replaced the call which crashed in 292
- the board reassociated, rebound DHCP, and served

**That closes the request that started this arc**: enter the password for a
network on the device, and have it kept.

### 297a. And it needed `forget` to be testable at all

The save path could not be exercised a second time while the store had no way
to remove an entry — every attempt joined silently instead. A feature that
cannot be re-run cannot be tested, and 296 was written as a usability gap
without noticing it was also the reason the risky path had stayed unmeasured
for eleven steps.

### State

```
works  scan, pick, type, join, remembered across reboots, forgettable
       and the page is served over the link that credential opened
open   steps 49-141; the USB link; term/notes onto keyboard.c; touch 'cal'
```

---

## step 298 — a web view, and what it honestly cannot do

`(this commit)`

Asked for: replace `draw` with a browser that displays Google.

### 298a. What was said before building it

**google.com is HTTPS-only, and TLS does not fit on this board.** A TLS 1.2
handshake needs X.509 parsing, RSA or ECDSA verification, ECDHE, AES-GCM,
SHA-256 and a root store; mbedTLS wants roughly 40-50 KB of heap for one
handshake, and this board reports **38,648 bytes of heap in total** at boot. Nor
is there an HTML parser, a layout engine or CSS, and there would be no room for
them either.

So what was built is a browser in the fetch-and-show sense: DNS, TCP, an HTTP
GET, and the response on screen. Pointed at google.com it shows **Google's 301
redirect to HTTPS** — a real answer from Google's servers, reached over a link
this board negotiated itself, displayed as what it is rather than dressed up as
a page. The header says so, so the icon does not overpromise.

### 298b. Its own DNS resolver

`lwipopts.h` has `LWIP_DNS 0`, annotated *"needs str* this kernel does not
have"*. Rather than pull in lwIP's resolver and the string library under it,
`webfetch.c` asks one A-record question over raw UDP: a twelve-byte header, the
name as length-prefixed labels, and four bytes of answer. Name compression is
handled by skipping 0xC0 pointers.

It asks **the gateway**, not a public resolver. A home router forwards DNS, and
using it keeps this board off any name server the user did not already choose by
joining their network — which is the right default for something that is not
going to ask.

### 298c. Placement, by the rule that cost a crash

```
wificred.c   iram    drives flash_read/erase/write
webfetch.c   irom    only calls lwIP
browser.c    irom    draws
```

iram overflowed by 700 bytes on the first link, and the fix was **not** to move
whatever was largest. Step 292's rule decides it: *code that touches the flash
bus must not live on it.* `webfetch.c` does not, so it goes to flash; the
linker script says why, next to the entry.

### 298d. Where the fetch runs

On the **net task**, through `browser_service()`. Two reasons, both already
paid for: the raw lwIP API is single-context under `NO_SYS=1`, and the task that
reads the glass must stay answerable (281b). The view sets a flag; the net task
performs the fetch and times it out.

### State

```
new    web view: URL bar, keyboard entry, DNS, HTTP GET, response on screen
open   whether google.com resolves and answers -- NOT YET RUN
       steps 49-141; the USB link; term/notes onto keyboard.c; touch 'cal'
```

---

## step 299 — a view that reports a network failure should say what it believes

`(this commit)`

Reported as `no network` from the web view — while the board was answering pings
at 192.168.1.140 from another machine.

The message is `webfetch_start()` reporting that `netif_wifi_ip()` returned
zero, which happens when the radio has not been brought up. Every flash reboots
the board and a fresh boot starts with the radio off, so a `go` tapped before
running the wifi view produces exactly this — and the FAILED state then persists
until the next attempt, long after the radio has come up.

**The only useful follow-up question is what the board thinks its address is,
and the view could not answer it.** The bar now shows the board's own address
whenever no fetch is in flight: `192.168.1.140` in grey, or `no address -- run
wifi` in red.

That is the fifth instance of the pattern in UM-NATOS-054 §8 — the first one
found in a view less than an hour old, which suggests the lesson had not
actually been learned when the report about it was written.

### State

```
open   the web fetch has still not run; DNS and the TCP client are unexercised
       steps 49-141; the USB link; term/notes onto keyboard.c; touch 'cal'
```

---

## step 300 — a lesson written down and not applied

`(this commit)`

Reported as nothing happening in the web view. The wiring was correct — icon,
action value (5, no collision), mode, both routing chains and the open handler
all check out by reading.

**The controls were 26x14 pixels.** The touch calibration reads `defaults` and
the raw X saturates at 4095, so the mapping is poor and a target that small is
not a target.

Step 286c enlarged the wifi view's button from 16 px to 28 px for exactly this
reason. This view was then built with 14-pixel controls, **in the same session,
after that fix**. Writing a lesson into the log is not the same as applying it
to the next thing built, and nothing in the process caught it — the geometry
compiled, the asserts passed, and the defect only exists against a panel.

`go` and `ed` are 46x26 now and the URL bar is 30 px tall.

### 300a. The USB link, sixth failure

The flash could not open the port: both COM5 and COM6 reading `Unknown`, stale
entries with no live device. Recovered by a physical replug, and the user
reports the port itself is bad. Recorded because it has now cost five capture
runs and one flash, and because it is the reason every instrument in this arc
had to move to the glass (UM-NATOS-054 §7.1).

### State

```
open   the web fetch STILL has not run; DNS and the TCP client are unexercised
       touch calibration reads defaults -- 'cal' would help every view
       steps 49-141; the USB link; term/notes onto keyboard.c
```

---

## step 301 — the redraw storm

`(this commit)`

Reported as: the web view works, but the screen flashes constantly.

Two mistakes stacked, each harmless alone:

**`draw_all()` cleared the whole view before every repaint.** Every `draw_*`
below it already fills its own region, so the clear was never needed for
correctness — but it meant any repaint went black-then-content, which is a
visible flash rather than an update. It happens once now, on entering the view
or switching between the page and the editor, which are the only times the
layout actually changes.

**And the view marked itself dirty on every frame while a fetch was in
flight.** The comment said *"body still growing"*, which is true for perhaps a
few frames out of the dozens that pass — the rest repainted an identical screen.
Combined with the full clear, that is the entire display blanked and rewritten
about eight times a second.

The response length is what actually moves, so that is what is watched now: the
view repaints when the state changes or when a byte has arrived, and otherwise
holds still.

**A dirty flag set unconditionally is not a dirty flag.** It is the same shape
as step 289's guard that never expired: a mechanism whose whole purpose is to
distinguish two cases, wired so that it always reports one of them.

### State

```
works  web view fetches and displays without flicker
open   the fetch itself -- DNS and the TCP client remain unexercised
       touch 'cal'; steps 49-141; the USB link; term/notes onto keyboard.c
```

---

## step 302 — the wifi view: a flash per channel, and a step with no decision in it

`(this commit)`

Reported as: the view is glitchy, and connecting means leaving it, coming back
and tapping scan.

### 302a. Thirteen full repaints per sweep

`draw_all()` blanked the whole view before repainting, and `scan_step()` marks
the view dirty **once per channel**. So a sweep was thirteen
black-then-content cycles — a flash each time, not an update.

Identical to the browser's flicker (301), in code written earlier, and not
noticed until the same mistake was made twice. The full clear now happens on
entering the view; the list region clears itself so rows and the empty-state
text can replace each other, and nothing else needs blanking at all.

### 302b. A step with no decision in it

`start_radio()` clears the list, and step 293 correctly stopped it sweeping
afterwards — so entering the view showed an empty list until the user tapped
`scan`. **That tap carried no decision.** There is nothing else to do with an
empty list, and the interface was asking anyway.

The view now sweeps on entry when there is a radio, nothing is connected, and
the list is empty. **Not when already joined**: scanning retunes the radio off
the access point (293) and would drop the connection the user came here to
keep. The scan button stays for choosing a different network on purpose, and
the empty list while connected now reads `connected -- scan for others` rather
than inviting a tap that would cost the link.

This is 293 finishing properly. That step removed a sweep that was destroying
the connection and left the user with nothing in its place; the missing half
was doing it at the one moment it is both safe and wanted.

### State

```
works  enter the view -> it scans -> tap a network -> join; no flicker
open   the web fetch; touch 'cal'; steps 49-141; the USB link; term/notes
```

---

## step 303 — a dirty flag raced across two tasks

`(this commit)`

Reported as: it still only works after leaving the view and reopening it.

That symptom names the fault precisely. **Re-entering forces a repaint** — so
the state underneath was correct all along and the SCREEN was stale. Something
was losing updates.

`g_dirty` is written by the **net task** (`scan_step`, `join`, `start_radio`)
and read by the **display task**:

```c
if (!g_dirty) { return; }
g_dirty = 0;                /* a set landing HERE is lost */
draw_all();
```

A plain `int`, not volatile, with a test-then-clear straddling two tasks. A
sweep result arriving in that window was never drawn, and nothing set the flag
again, so the view stayed stale until it was re-entered.

Now a **sequence**: producers bump it, the display task records what it last
painted, and a bump during a paint is simply seen on the next frame. A counter
cannot lose an update.

### 303a. Why the earlier fixes did not find it

Steps 302 and 301 were both real (thirteen full repaints per sweep; a dirty flag
set unconditionally) and both were in the drawing. This one is not in the
drawing — it is in the handoff between the task that changes the state and the
task that shows it, which no amount of looking at `draw_all()` would reveal.

The clue was in the report and not in the code: *only* when I exit and reopen.
An action that fixes a symptom without touching the subsystem being blamed is
pointing somewhere else, and it took three tries to read that.

**The wifi view was the first thing in this project to move work onto another
task** (281b, 284, 288). Every one of those steps moved a producer across a task
boundary and none of them revisited how the result got back.

### State

```
works  the view repaints when the net task changes something, first time
open   the web fetch; touch 'cal'; steps 49-141; the USB link; term/notes
```

---

## step 304 — sixty of the ninety seconds were waiting for nothing

`(this commit)`

Asked for: cut the ninety-second wait after tapping `start`.

### 304a. The poll that outlived its reason

`wifi_rx_start()` ended with `net_poll_for(6000u)` — **sixty seconds**, two
thirds of the whole wait. Step 271's own comment beside it had already said what
that call had become:

> *"60 s, was 600. The long window existed because the poll WAS the network:
> when it ended the stack went silent. The net task now services lwIP for as
> long as the board is up, so this is only the bring-up window."*

The window was shortened when the task arrived and never questioned again. What
it still has to do is **reach the handover** at the end of `net_poll_for`, which
gives the ring to the task — one pass, not six thousand ticks. DHCP completes
afterwards, serviced by the task, and the view watches for the address rather
than the bring-up blocking until it arrives.

150 ticks now. **Fifty-eight and a half seconds, removed by reading a comment
that was already correct.**

### 304b. And a sweep the view is about to repeat

`wifi_bringup()` runs its own thirteen-channel sweep, about five seconds. Step
227 added it so a failed connect could not be confused with a missing network —
*"evidence first, then the action it informs"* — and that reasoning still holds
for the shell, which has no other view of the air.

It does not hold for the wifi view, which sweeps on entry (302) and shows the
result. Those five seconds bought evidence the user was about to gather
themselves. Skippable now, and **the full sweep is the default**:
`wifi_start_enable()` clears the flag, so only a caller that asks afterwards
gets the quick path and the shell cannot inherit it from a previous run of the
app.

### 304c. What is left, and what it is

Roughly 25 s, and the largest single piece is **PBKDF2 at 4096 iterations —
about fifteen seconds** — which is real cryptographic work and cannot be
removed. It could be *avoided*: the PMK is a pure function of the SSID and the
passphrase, so caching it beside the credential would make every join after the
first one near-instant. That means bumping `CRED_VERSION`, which discards saved
credentials, so it is recorded as the next move rather than taken today.

The screen now says `starting radio -- 30 s`, because a status line with a
number in it has to have the right number.

### State

```
works  start -> about 25 s -> associated, versus about 90 s before
open   cache the PMK beside the credential (304c); the web fetch; touch 'cal'
       steps 49-141; the USB link; term/notes onto keyboard.c
```
