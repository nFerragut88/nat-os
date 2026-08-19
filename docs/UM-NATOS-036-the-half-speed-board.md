# UM-NATOS-036 — The Half-Speed Board

**Used Medias LLC — Embedded Systems Division**
Revision 1.1 · 2026-08-19 · Status: **Fixed, and now verified against an independent clock (§11); one consequence still open (§7)**

---

## 1. Abstract

UM-NATOS-035 replaced Espressif's second-stage bootloader and reported it
verified: full boot, every self-test passing, SD, display, I²C, VM, a
known-answer CRC from the ROM. All of that was true.

It was also incomplete in a way that hid a serious defect. **The board was
running at 40 MHz instead of 80.**

Espressif's bootloader switches the SoC to the 320 MHz PLL and divides it to
80 MHz before jumping to the application. nat-os had inherited that for its
entire life without any part of the kernel knowing it depended on it. The
replacement loader copies segments and jumps — it does not touch the clock — so
the kernel came up on the bare 40 MHz crystal.

Nothing said so. Every duration this kernel reports is derived from `CCOUNT`,
which counts CPU cycles: halve the clock and you halve the cycles per second, so
a duration measured in cycles and printed as milliseconds shows **the same
number for twice the wall-clock time.** The display's `fullscreen=44 ms` was
identical before and after the bootloader change, and it was 44 ms of cycles
against 88 ms of reality.

That is this project's standing rule arriving from a new direction:

> **A successful measurement is not evidence, if the measurement and the fault
> share a dependency.**

The fix is `kernel/clock.c`, and it is in the kernel rather than the bootloader
on purpose. §5 says why.

One consequence remains open and is stated in §7 rather than buried: with our
bootloader the PHY still fails, now differently, and the cause has not been
found. `-WiFi` builds are gated to Espressif's loader until it is.

---

## 2. How it surfaced

Not through timing. Through the loudest possible symptom, in the subsystem this
session was actually trying to work on.

The task in hand was UM-NATOS-034 §18's cheap next test: write the
`0x3FF73DB8` sequence around `wifimac_tx()`. That needs a `-WiFi` build. It
never got that far:

```
[phyinit] ungating the radio clock, calling register_chipv7_phy...
          *** nothing. ever. ***
```

`register_chipv7_phy()` hung and never returned. The board was otherwise alive
and had passed every self-test moments earlier.

**The A/B control settled it in one flash.** Same kernel image, Espressif's
bootloader at `0x1000`:

```
[phyinit] ungating the radio clock, calling register_chipv7_phy... | returned 0
[macinit] MAC IS RUNNING - counters advance that did not before
```

That control existed because UM-NATOS-035 §9 kept the vendor path working and
A/B tested it, on the argument that *the day it is needed is not the day to be
debugging it*. It was needed four hours later.

---

## 3. The cause

Reading the RTC retention registers under each bootloader, at the same point in
the same kernel:

| | Espressif | ours |
|---|---|---|
| `RTC_XTAL_FREQ_REG` (`0x3FF480B0`) | `0x00280028` — 40 MHz | **`0x00000000` — 0 MHz** |
| `RTC_APB_FREQ_REG` (`0x3FF480B4`) | 80 MHz | 40 MHz |
| `RTC_CNTL_CLK_CONF` `SOC_CLK_SEL` | **1 = PLL** | **0 = XTAL** |
| `DPORT_CPU_PER_CONF` | 80 MHz divider | 80 MHz divider |

Two separate faults, one cause.

**The hang** is the first row. The crystal frequency is not measured by the PHY;
it is *looked up* in an RTC retention register that the bootloader is expected to
fill in. Ours left it at zero. A radio calibration handed a crystal frequency of
zero does not return.

**The half speed** is the third row, and it is the one that would have gone on
mattering silently. `SOC_CLK_SEL = 0` is the bare crystal.

Note that `DPORT_CPU_PER_CONF` matched. The divider was right; there was nothing
to divide.

---

## 4. Why the instruments agreed the board was fine

Worth spelling out, because the list of things that reported success is long and
each of them is a good instrument.

- **Every self-test passed** — heap, arenas, VM, faults, isolation, mutex, IPC.
  All of them are *logical*: they check what happened, not when. Halving the
  clock cannot fail any of them.
- **`fullscreen=44 ms` was unchanged** — computed from `CCOUNT`, so it measures
  cycles and labels them milliseconds.
- **`tick every 800000 cycles`** — a cycle count, correct in cycles.
- **The serial console was readable** — the ROM had programmed UART0's divisor
  for a 40 MHz APB, and the APB really was 40 MHz. Consistent, and wrong.
- **The display, SD card and I²C all worked** — SPI and bit-banged buses ran at
  half rate, which is slower and still perfectly legal.

There was no instrument in the system that measured wall-clock time against an
independent reference, so there was nothing that *could* have caught this. That
is the real finding, and §8 records what to do about it.

---

## 5. The fix, and why it is in the kernel

`kernel/clock.c` brings the SoC to 80 MHz off the PLL and records the crystal
and bus frequencies where the ROM and the PHY look for them. It could have gone
back into the bootloader, which is where Espressif puts it and where the
regression came from. Three reasons it is not:

1. **The analog PLL is programmed over an undocumented internal I²C bus.** The
   only way in is `rom_i2c_writeReg()`, a **windowed** ROM function, and calling
   windowed code needs the window-overflow handlers — which live in the kernel,
   installed at `VECBASE`, and would have to be duplicated into a 2.7 KB loader
   whose entire job is three memory copies. (ESP-IDF calls the same ROM routine
   on this chip; there is no register-level path, because the analog I²C master
   is not in the TRM. It is silicon, in the category UM-NATOS-035 §10 already
   argued is not a dependency worth counting.)

2. **Choosing a CPU frequency is an operating system's decision.** A loader's
   job is to put the kernel in memory.

3. **It makes nat-os independent of what loaded it** — which is strictly better
   than the situation *before* this bug, and is precisely the property whose
   absence caused it. The kernel had a hard requirement that nothing in the
   kernel stated, checked, or owned.

`clock_init()` returns immediately if it finds the SoC already on the PLL, so one
image boots correctly from either loader. It **refuses** a crystal frequency it
does not have a divider table for rather than approximating one: applying the
26 MHz row to a 40 MHz part would lock the PLL to the wrong frequency and make
every timing in the system wrong by a ratio — the exact failure this file exists
to fix.

Two ordering details that would each have been a bad afternoon:

- The digital regulator is raised **before** the core is asked to go faster.
  The other order is a brownout on a board with no way to report one.
- `CPUPERIOD_SEL` is set **before** `SOC_CLK_SEL`. The other order runs the core
  at 320 MHz for however long the next instruction takes.

And one that was not obvious until it was: APB doubles, and UART0's divisor does
not hear about it. `uart_rescale()` scales the existing divisor by the frequency
ratio rather than recomputing from a baud-rate constant — the baud in use is the
one the ROM negotiated with esptool, which is not necessarily the one a constant
here would have claimed. It is carried in sixteenths so the fractional field is
not dropped; dropping it is a 6% baud error, which shows as *mostly* correct
text.

`rom_call4()` was added to `kernel/window.S` — `rom_call3` existed, and
`rom_i2c_writeReg(block, host_id, reg_add, data)` takes four.

Every constant in `clock.c` was read out of the SDK headers named beside it
during this session. None were recalled.

---

## 6. Verified

**Default (blob-free) build, our bootloader:**

```
cpu clock    : 80 MHz (PLL, switched by the kernel)
```

One ROM banner in 15 s — no reset loop. `sd SDSC ready`, `display init ok
bytes=153634`, `heap 121344 B`, and every self-test PASS: no-leak, oom-safe,
arenas, VM program/faults/runaway/predicate, interleave, isolation, release,
mutex.

**Espressif's bootloader, same kernel:** `cpu clock : 80 MHz (PLL, already set
by the bootloader)` — the early-return path, and the PHY still comes up. One
image, either loader.

**The windowed ABI, after `clock_init()` has made ROM calls:** `wintest 12`
returns 12 CORRECT (depth 12 forces window overflow and underflow exceptions,
so this is a checksum over every handler invocation, not merely an absence of
crash); `vendorcall 10` runs a `-mabi=windowed` object built by the vendor
compiler; `romcall` returns `0xcbf43926`.

---

## 7. What is still broken, stated plainly

**With our bootloader, `-WiFi` builds still fail.** The hang became a panic:

```
*** KERNEL PANIC ***
  exccause : 29  (StoreProhibited)
  epc      : 0x40090bdb   <- phy_enter_critical + 3
  phystack : 0 of 6144 bytes used
```

The PHY now gets far enough to call back into us and dies in a three-instruction
shim. Under Espressif's loader, the identical kernel returns 0 and the MAC comes
up.

**Eliminated this session:**

- **The clock.** Fixed and verified; this panic is downstream of it, not it.
- **All eight register differences.** A full `regdump` under each loader at the
  same point differs in exactly eight registers once `0x3FF74000+` — buffer RAM,
  random on both sides — is excluded. Four RTC and four DPORT, including the
  cache-mask registers our bootloader writes. Applying Espressif's values for
  all of them changes nothing. (One, `0x3FF48090`, silently refuses writes —
  worth remembering, since this project has now found two such registers.)
- **Window-state corruption from `clock_init`'s ROM calls.** The obvious
  suspicion, given the fault is at a windowed function's prologue. `wintest 12`,
  `vendorcall` and `romcall` all pass afterwards.
- **PHY stack depth.** `0 of 6144 bytes used`.

So the remaining difference is something a register dump does not reach:
memory contents, a region outside the dumped ranges, or an ordering.

**`build.ps1 -WiFi` now forces Espressif's second stage**, with a message saying
so. Not to hide the problem — to stop WiFi research being conducted on an
unexplained foundation, which is exactly how this project has previously lost
sessions. The blob-free default build is unaffected and fully verified on our
loader.

---

## 8. Correction to UM-NATOS-035

That report's §8 is headed "Verified" and lists a full boot, every self-test,
five subsystems, a known-answer ROM test and a DROM argument. Every claim in it
is true. It was still wrong as a whole, in two specific ways:

1. **It only ever tested the default build.** The `-WiFi` build was never
   flashed on the new bootloader. Half the configurations shipped untested.
2. **It had no check that could detect a clock change**, and did not notice
   that it had none. The verification consisted of instruments that were all
   downstream of the same assumption.

The sentence *"The executable chain from reset to shell prompt is now this
project's own code"* stands. The sentence *"Verified on hardware"* should have
read **"the default build is verified; the WiFi build is untested."**

---

## 9. Rules earned

**A successful measurement is not evidence, if the measurement and the fault
share a dependency.** Every timing instrument in this kernel is derived from
`CCOUNT`. A clock fault is invisible to all of them simultaneously, and their
agreement is not corroboration — it is one instrument reported nine times.

**Verify the configurations you ship, not the one you built.** `-WiFi` was a
supported build with a documented flag, and it was never flashed. A verification
section that lists twenty passing checks in one configuration is weaker than one
that lists three in each.

**An inherited dependency that nothing states is a defect already.** The kernel
needed an 80 MHz PLL and no file said so, nothing checked it, and nothing owned
it. It worked for the whole life of the project by luck of who was loading it.
The fix is not only to set the clock — it is that `clock_init()` now *asserts*
what the kernel requires and reports what it found.

**Keep the recovery path, and test it.** UM-NATOS-035 kept Espressif's
bootloader on the argument that the day it is needed is not the day to be
debugging it. Without that working A/B control, this session would have been
spent debugging the PHY instead of the loader.

---

## 10. What this cost, and what it did not

The session's actual task — writing the `0x3FF73DB8` sequence into
`wifimac_tx()` and re-running the radiated test — **was not done.** It is
blocked on §7, not abandoned, and UM-NATOS-034 §18.5 still holds.

Against that: the board has been running at half speed since UM-NATOS-035
landed, every timing figure published in that report is a cycle count wearing a
millisecond's clothes, and none of it was going to be noticed by any test this
project owns. Finding that was worth the session.

---

*Filed 2026-08-19.*

---

## 11. Verifying the fix against something that is not CCOUNT

**Added revision 1.1.**

§6 reported the fix verified: `cpu clock : 80 MHz (PLL, switched by the kernel)`,
all self-tests passing, on both bootloaders.

That verification had the same shape as the defect. It read `SOC_CLK_SEL` and
printed a number derived from it. **A register is a statement of intent; it is
not a measurement of a frequency.** §9's rule — *a successful measurement is not
evidence, if the measurement and the fault share a dependency* — applies to the
fix as much as to the bug, and §6 did not notice.

`tools/serial/measure_mhz.py` uses a reference the board cannot influence: the
host's clock. The scheduler tick is programmed at `TICK_INTERVAL_CYCLES =
800000` CPU cycles and the reporter prints a monotonic `t=` count, so

```
ticks_per_second x 800000 = CPU Hz
```

80 MHz is 100 ticks/s; 40 MHz is 50. Nothing in that chain reads a clock
register.

### 11.1 Measured

| configuration | measured |
|---|---|
| default build, **our** bootloader | **78.1 MHz** |
| default build, Espressif's bootloader (early-return path) | **78.1 MHz** |
| `-WiFi` build, Espressif's bootloader (as gated) | **78.1 MHz** |
| **negative control** — our bootloader, `clock_init()` switch disabled | **39.8 MHz** |

The negative control is the part that makes the rest mean anything. An
instrument that has never produced a wrong answer has not been shown to be
capable of one, and this project has now been bitten three times by detectors
that could only say what was expected. Disabling the switch and rebuilding
produced 39.8 MHz — a clean 1.96x separation, and independent confirmation that
the original diagnosis was right rather than merely consistent.

All three shipped configurations agree exactly, which is the second thing worth
having: the frequency no longer depends on which loader ran or which path
`clock_init()` took.

### 11.2 The residual, stated rather than rounded away

78.1 against an expected 80.0 is 2.4% low, and 78.1/39.8 is 1.96 rather than
exactly 2. Both are small, both are consistent across configurations, and
neither is explained. The most plausible cause is tick accounting — a handler
that reprograms CCOMPARE from the current CCOUNT rather than by adding the
interval accumulates its own latency once per tick — but that has not been
checked, and it is recorded here as an open minor discrepancy rather than
rounded to "80 MHz confirmed".

What the measurement establishes is not the third significant figure. It is that
the board runs at roughly 80 and not at roughly 40, measured by something with no
stake in the answer.
