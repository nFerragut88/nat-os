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
