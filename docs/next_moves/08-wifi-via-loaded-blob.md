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
