# Blob-free by default

**Direction note, 2026-08-19.** Not a report — a decision about what this
kernel is, with the measurements that made it obvious.

---

## Where the blobs were

Two files, 1.4 MB, both Espressif's:

```
vendor/phy/libphy_natos.a   847 KB   analog RF calibration
vendor/phy/libpp_natos.a    541 KB   MAC hardware layer
```

They were reached from exactly three source files — `phyinit.c`, `wifimac.c`,
`wifi_osi_impl.c` — and nothing else in the kernel touched them. Everything
else was already this project's own code:

> scheduler, heap, arenas, VM, vmarg, IPC, device model, permissions,
> persistence, display, touch, SD, SPI3, ADC, I²C, audio, flash, panic,
> watchdog, renderer, launcher, note pad, terminal
>
> — 33 of 37 source files, no vendor binary

So nat-os was already essentially blob-free. The blobs existed for one
subsystem, and **that subsystem is the one that does not work.**

---

## Why WiFi can never be clean here

`libphy.a` performs analog RF calibration: VCO and PLL tuning, filter
calibration, I/Q imbalance correction, temperature compensation. It cannot be
reimplemented from public information — not "is difficult", but *Espressif has
never published the RF characterisation it encodes*.

Even **esp32-open-mac**, a dedicated reverse-engineering project, keeps the PHY
blob and replaces only the MAC above it. Nobody has replaced `libphy` on this
chip.

Which leads to the fact that actually decided this:

> **Succeeding at WiFi transmit would not make the project clean.** After
> however many months of tracing, the image still links 1.4 MB of somebody
> else's binary. The work buys function; it can never buy independence.

If the goal is a kernel that owes nothing to anyone, WiFi transmit is the wrong
project — not because it is hard, but because it *cannot deliver the thing*.
UM-NATOS-034 §17 argues it is achievable. This note argues it is beside the
point.

---

## What changed

WiFi is now behind `BOARD_HAS_WIFI`, **off by default**.

```powershell
.\build.ps1              # blob-free
.\build.ps1 -WiFi        # links libphy + libpp, for research
```

The default build does not compile `phyinit.c`, `wifimac.c` or
`wifi_osi_impl.c`, and does not put the vendor archives on the link line at all.
There is no stub, no dead code, no path to the blob that happens to be unused —
**if it is not compiled it cannot link, and `nm` can prove it.**

### Measured

| | default | `-WiFi` |
|---|---|---|
| image | **79,712 B** | 145,584 B |
| text symbols | **474** | 918 |
| vendor archives linked | **none** | libphy + libpp |
| `register_chipv7_phy` | absent | present |
| `hal_mac_init`, `lmacInit`, `ic_mac_init` | absent | present |

**45% of the firmware was somebody else's code.** It is gone from the default
image and every symbol that remains is from this project.

### Verified on hardware

The blob-free image boots and runs: applications scheduled, `corrupt=0`,
`heap check=0`, SD card `sd_init OK type=SDSC`, I²C `bus looks sane`, ADC
sweeping, light sensor and speaker working, display and DMA healthy, SPI3
self-tests passing.

Nothing was lost except WiFi, which did not transmit anyway.

---

## The research is not deleted

`UM-NATOS-027`, `028` and `034` are among the better records in this project,
and the drivers still compile under `-WiFi`. Receive still works there. What
changed is the default, and what the default *says*.

---

## The remaining dependency, and the finish line

> **Closed the same day. See UM-NATOS-035.**

One thing stood between this and a kernel that is entirely its own code:

```
vendor/bootloader.bin    17 KB   Espressif's second-stage bootloader
```

Unlike the PHY it is **fully documented** — UM-NATOS-002 already describes the
image format this project's own build produces — so writing a replacement was a
bounded, achievable project. It was also the only one left.

### It is written

`boot/`, **2,736 bytes**. It reads the image header at `0x10000`, maps the DROM
segment through the flash MMU, copies the DRAM and IRAM segments, enables the
cache and jumps. `build.ps1 -Flash` builds and flashes it by default;
`-VendorBootloader` restores Espressif's for recovery or comparison.

It was short for one reason: `kernel/flash.c` drives SPI1 through its registers
with no ROM calls and no dependence on the cache, so the usual
read-flash-before-flash-works problem was already solved by a file written for
an unrelated purpose. It is compiled into the bootloader unchanged.

Verified on hardware: full boot, every self-test PASS, SD, display, I²C, VM,
`romcall` returning the published CRC-32 check value, and every string in the
log arriving through the MMU entry the bootloader wrote.

### What is left, stated plainly

| | |
|---|---|
| ROM first-stage loader | in silicon; not replaceable by anyone |
| `vendor/partitions.bin` | 3 KB at `0x8000`, **read by nothing in this chain** — kept for external tooling |
| `vendor/bootloader.bin` | out of the boot path; kept as the recovery image |

**The executable chain from reset to shell prompt is now this project's own code,
except for the part that is physically in the chip.**
