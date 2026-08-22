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
