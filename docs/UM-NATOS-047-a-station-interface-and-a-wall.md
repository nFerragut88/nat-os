# UM-NATOS-047 — A Station Interface, and a Wall

**Used Medias LLC — Embedded Systems Division**
Revision 1.0 · 2026-08-23 · Status: **`esp_wifi_set_mode(STA)` returns ESP_OK. The dominant obstacle is now a code-layout sensitivity with three instances and a measured width.**

---

## 1. Abstract

The driver has a station interface. `esp_wifi_init_internal()`,
`esp_wifi_set_mode(WIFI_MODE_STA)` and `esp_wifi_start()` all return `ESP_OK`
from a cold boot, in one command.

Three smaller things also landed: the hardware random number generator, the
instrumentation debt UM-NATOS-042 §9.3 has asked for since step 102, and the
adapter name table.

The larger result is not a feature. **A sensitivity to code size and position,
previously a documented oddity with a workaround, is now the thing blocking
work.** It has three instances, two of them from this stretch, one triggered by
*deleting* dead code. It is deterministic, and its width in one file has been
measured: somewhere between 30 and 100 bytes.

Nothing has been on air, and §7 gives the basis.

This report covers `next_moves/08` steps 193–196. UM-NATOS-046 covers 177–192.

---

## 2. State before and after

| | after 046 (step 192) | now (step 196) |
|---|---|---|
| `esp_wifi_set_mode` | never called | **ESP_OK, WIFI_MODE_STA** |
| `_rand`/`_random`/`_get_random` | all returned 0, buffer untouched | **hardware RNG, buffer filled** |
| adapter name table | printed `_magic` for a real entry | correct |
| dead/misleading probes | six | **removed or made honest** |
| MAC interrupt | routed | routed, **handler filed on the right line** |
| interrupts taken | none | **none** |

```
boot        11 PASS 0 FAIL
wintorture  switches during the call: 10  (preemption really happened)
            checksum 1632 expected 1632  CORRECT
blobphy     phyinit rc=0
wifiinit start
            phyinit rc=1        (guarded, correctly)
            init      returned 0x00000000  (ESP_OK)
            set_mode  STA returned 0x00000000
            start     returned 0x00000000  (ESP_OK)
            [intr] src=0 line=27 prio=1 routed=1 nofn=0 fired: none
                   timers=14 refused=0
```

---

## 3. The random number generator

`_rand`, `_random` and `_get_random` all answered 0, and `_get_random` did it
while leaving the caller's buffer untouched — the shape `_read_mac` had before
step 186, and the one this investigation keeps finding: **success reported for
work never done**, which is worse than failing because the caller cannot find
out. It is called during `esp_wifi_start`.

`osi_impl_random()` already existed as an xorshift32, and its own comment set the
condition for replacing it:

> The ESP32 has a hardware RNG, but it is only properly random while the radio is
> running — which is the thing being brought up. This is deterministic on purpose
> rather than by accident, and **should be replaced once the PHY is live**.

Step 190 called `_phy_enable`. The condition was met, so it now reads
`WDEV_RND_REG`, Espressif's own constant. `g_rng` is kept and still stirred, so
the xorshift is one line away if the hardware read ever proves unavailable.
`osi_impl_get_random()` is new and fills the buffer a word at a time, returning
`-1` rather than success on a null pointer.

Verified across boots, which is a **decode** test and not a randomness test:

```
rng=0x592ce85b,0x30971b55
rng=0xab345ca1,0xfa7c58e9
rng=0x485b8340,0x7dd1ecfd
```

**Entropy, stated rather than assumed.** ESP-IDF documents this register as a
true random number generator only while the RF subsystem is running. The driver's
calls arrive after `esp_wifi_start`, which is the good side of that, but nothing
enforces it and these entries must not be treated as a cryptographic source on
that basis alone.

### The third duplicate

This was the third thing written in one session that already existed — the ETS
timers in step 191, `osi_impl_random` here, and a duplicate ETS emulation thrown
away between them. Each was found only when the compiler refused a redefinition.

The rule that would have caught all three is §4.7's, applied to our own tree:
**grep before writing.** It is cheaper than the build that catches it.

---

## 4. The instrumentation debt

Six builds, one file at a time, as §9.3 asked.

### 4.1 Removed

| probe | why |
|---|---|
| `rc0 zero`, `spill pre/post`, `chain base` | step 187/188 machinery; the defect they found is fixed |
| `a0/sp out` | written by `w2c_call*` on the way **out**, not at the fault, from a singleton every bridge call overwrote. Its verdict read `context survived` whenever either half was non-zero, so `sp == 0` alone looked healthy |
| `saved frame @` / `saved hi` / `saved ctl` | dumped the current task's **last switch-out**, stale by construction whenever that task is the one that faulted |
| the `rcz_*` bracket machinery | call sites, helpers, globals |
| `rom_call4`'s prime-site recorder | nothing reads it; verified no behavioural effect |

The first two are superseded by `fault regs`, which records a0..a15,
`WINDOWBASE` and `WINDOWSTART` as they were **at** the fault. Step 186 built a
chain of reasoning on `a0/sp out` and had to retract it.

### 4.2 Fixed rather than removed

`blk-window` printed `spill ws 0x000002802b` — twenty-one bits of a sixteen-bit
register, a hex word and a bit count run together with no separator.

`sbp-post` printed its verdict off the never-sampled `0xffffffff` sentinel, so it
had been announcing `SWEEP LEFT MULTI-BIT` for a probe that has not run since
step 176 disabled the spill. It now says `never sampled`, and is kept so it works
again if the spill is re-enabled.

### 4.3 The first attempt cascaded, exactly as warned

The removals were first done by walking braces outward from each print. That cut
across block boundaries and produced three compile errors in one build — the
same shape §9.3 records. Reverted with `git checkout` and redone by reading the
exact line ranges. The warning was right and the shortcut was not worth taking.

---

## 5. The wall

### 5.1 A probe that could not be removed

`w2c_call0f` writes three instructions of dead instrumentation:

```asm
    movi    a9, g_win_a0
    s32i    a0, a9, 0
    s32i    a1, a9, 4
```

Nothing reads `g_win_a0`/`g_win_sp` any more; the panic line that did was removed
in §4.1. They are dead by inspection — stores to a global with no reader.

**Removing them makes `esp_wifi_init_internal` return `0x101`
(`ESP_ERR_NO_MEM`), reproducibly, from a cold boot.** Bisected to exactly those
three instructions: the `rom_call4` recorder next door removes with no effect,
removing only this block breaks init, restoring only this block restores
`ESP_OK`.

They stay, labelled. Deleting dead code while the sensitivity is unexplained
costs a working radio to save nine bytes.

### 5.2 And then it refused a feature

`wifi_bringup()` was extended to call `esp_wifi_set_mode(WIFI_MODE_STA)` between
init and start — about twenty-five lines in `wifi_init_cfg.c`, with `shell.c`
deliberately not growing. The board **watchdog-reset inside `phyinit`**, a
routine that runs long before any mode logic and never executes the new code.

Two things had changed at once — the blob's entry table had gained two pointers —
so the first bisect reverted both and proved nothing. Holding the kernel fixed
and moving only the blob:

| blob | kernel | phyinit |
|---|---|---|
| v4, no new fields | unchanged | `rc=0` |
| **v4, both new fields** | **unchanged** | **`rc=0`** |
| v5, both new fields | v5, +25 lines | **watchdog reset** |
| v4, both new fields | unchanged | `rc=0` (restored) |

The blob is innocent. Both images are 606,404 bytes, so nothing extra was pulled
from the archives and `.text` did not move. The **kernel-side growth** broke a
routine the added code never touches.

### 5.3 Three instances, and a width

| | change | effect |
|---|---|---|
| UM-NATOS-042 §9.2 | nine lines of `uart_puts` **added** to `shell.c` | hung `blob_map` |
| step 194 | three dead stores **removed** from `w2c_call0f` | init returns `ESP_ERR_NO_MEM` |
| step 195 | twenty-five lines **added** to `wifi_init_cfg.c` | `phyinit` watchdog-hangs |

Growth and shrinkage, three files, two of them IRAM-resident rather than the
flash-mapped `shell.c` the original note blamed. The common factor is that the
kernel image moved and something that is not supposed to care about that cared.

Two facts were established that earlier instances did not have.

**It is deterministic.** Step 195's change was retried byte-for-byte and
reproduced the watchdog reset exactly. A marginal timing race would have passed
sometimes; this does not.

**It has a width.** Comments are free — only instructions move the image. Step
195's hundred bytes became about thirty when the argument parsing was dropped,
and thirty fits where a hundred did not. So the margin in `wifi_init_cfg.c` is
somewhere between 30 and 100 bytes.

That is a workaround and it is named as one. What it buys is the first
quantitative statement anyone has about this, which is worth more than the
feature it unblocked.

---

## 6. The station interface

```
init      returned 0x00000000  (ESP_OK)
set_mode  STA returned 0x00000000
start     returned 0x00000000  (ESP_OK)
```

`wifiinit start` now means init + `set_mode(STA)` + start. A driver with no
interface is not a state worth keeping a command for, so dropping the argument
parsing lost nothing.

The blob exports `esp_wifi_set_mode`, `esp_wifi_get_mode`, `esp_wifi_scan_start`,
`esp_wifi_connect`, `esp_wifi_set_config` and `esp_wifi_set_channel`. **Only the
two mode entries were added to the entry table.** Setting a mode does not
transmit; scan and connect do, and they stay out until that is a decision rather
than a side effect. The fields are appended and the version stays 4 — appending
is backward compatible, and the version is what makes a stale image a clean
rejection rather than a wild call.

### A fourth place for the remap

Asking why no interrupt had fired found a real bug. `osi_impl_set_isr()` files
the driver's handler under the line the **driver** asked for — 0 — while the
trampoline that actually runs is the one for the line we routed it to, 27. The
handler was stored where nothing looks; a MAC interrupt would have found
`g_blob_isr[27].fn == 0` and counted itself as `nofn`.

Step 191 recorded that the remap belongs in three places: `_set_intr`,
`_ints_on`, `_ints_off`. **It is four.** A translation applied to some of the
paths that use a number and not all of them is worse than no translation.

---

## 7. What is on air

Nothing.

```
[intr] src=0 line=27 prio=1 routed=1 nofn=0 fired: none
```

Polled over twenty-five seconds with the station interface up. **`nofn=0` is the
informative half**: not one interrupt arrives even to be counted as *unhandled*.
So the MAC is idle rather than mis-routed, which is what a station that has been
started and never told to scan or associate should be.

- No frame has been transmitted. No scan, no probe request, no association.
- No frame has been received. The interrupt that would signal one has never
  fired.
- There is still no data path above the MAC.

The PHY is powered, as UM-NATOS-046 §8 recorded. That remains the only respect in
which the sentence carries less weight than it did before step 190.

---

## 8. Method

**The last thing recorded before a fault is not its cause.** Step 190 named the
timer stubs on the strength of `last osi : entry 57 _timer_disarm`, and
implementing them changed the fault not at all. This report's §5.2 was nearly the
same error: `phyinit` hung, so `phyinit` looked implicated, and it was not.

**Change one thing.** §5.2's first bisect moved the kernel and the blob together
and established nothing. Two builds later, holding one side fixed answered it.

**Retry before rewriting.** Step 195's change was retried unmodified before being
made smaller. Had it passed, the sensitivity would have been flaky and every
conclusion about it wrong. It did not pass, and the retry is what makes
"deterministic" a measurement rather than an assumption.

**Grep before writing.** Three duplicates in one session, each caught only by the
compiler.

---

## 9. What remains

1. **Understand the layout sensitivity.** It is no longer an oddity to route
   around. It has cost a cleanup, refused a feature, and now stands between a
   driver that starts and a driver that does anything. Step 194's instance is
   bisected to three specific instructions, which is as small a reproducer as
   this is likely to get.
2. **A scan.** The first action that would put energy on air, and the first that
   should be a deliberate decision rather than a next step.
3. **The `w2c_*` bridges** still allocate over their caller's base save area.
   UM-NATOS-046 §6 fixed the three call0 bridges; the windowed ones are untouched
   and remain as UM-NATOS-045 §8.4 describes them.

---

Full experiment log: `docs/next_moves/08-wifi-via-loaded-blob.md`, steps 193–196.
Companion reports: UM-NATOS-042 (rev 1.1), 043 (rev 1.3), 044 (rev 1.0),
045 (rev 1.0), 046 (rev 1.0).

**Nothing has been on air.**

Written by: Hare
