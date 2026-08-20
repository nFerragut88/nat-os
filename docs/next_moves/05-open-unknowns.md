# 05 — Two open unknowns

> **STATUS 2026-08-19.** 5.2's mitigation shipped and the behaviour has not
> reproduced; 5.1 now has an answer. Both sections are updated below and this
> item is effectively closed — what remains for 5.1 is a meter, not code.

**Size:** small each. **Risk:** none. **Blocked on:** nothing.

Neither breaks anything. Both are recorded because an unexplained behaviour that
nobody wrote down becomes folklore, and folklore gets designed around.

---

## 5.1 MISO reads all zeros — ANSWERED

`panelpull` had existed unrun. Run at last, and it required one change before
its answer could be trusted.

**The instrument had to prove its own variable varied.** The command reads the
same register three ways — no pull, pull-up, pull-down — and concludes from the
comparison. If the pull bits had never reached the pad, all three passes would
have been identical *by construction* and the verdict would have been drawn from
an experiment that never varied anything. `display_panel_pad()` now reads the
IO_MUX register back and `panelpull` prints it per pass:

```
0xD3 pull=none pad=0x00001a00 -> 00 00 00 00 00 00 00 00
0xD3 pull=up   pad=0x00001b00 -> 00 00 00 00 00 00 00 00
0xD3 pull=down pad=0x00001a80 -> 00 00 00 00 00 00 00 00
```

`0x100` is `FUN_PU`, `0x80` is `FUN_PD`. The three configurations are visibly
different and **all three read zero.**

### What that rules out

**"SDO is not populated on this module" — the leading candidate — is wrong.** An
unconnected pin with the ESP32's internal pull-up (~45 kΩ) reads `0xFF`. This
reads `0x00` with the pull-up applied, so something is winning against 45 kΩ.

### The explanation, and why it is not a bug

**GPIO12 is MTDI, a strapping pin.** Held high at reset it selects a 1.8 V flash
supply and the board does not boot. Boards commonly fit an external pull-down —
typically 10 kΩ — precisely to guarantee that never happens.

A 10 kΩ external pull-down beats a 45 kΩ internal pull-up. And this board boots
reliably every time, which means MTDI *is* held low at reset rather than
floating — consistent with a fitted resistor rather than luck.

> **The resistor that makes the board boot is the resistor that makes MISO
> unreadable.** It is a hardware trade-off, not a defect, and removing it to
> read the panel would risk the boot it exists to protect.

### Status

**Closed for software purposes.** Panel read-back is unavailable on this board
and no amount of driver work will change that. `fbdump` remains the way to
answer "is what is on the glass what we sent", and it answers it from DRAM.

**Not proven, and the remaining step is a meter, not code**: put an ohmmeter
between GPIO12 and ground with the board unpowered and look for ~10 kΩ. Worth
five minutes if anyone is curious; worth nothing to the software.

A second possibility, ranked below it: the ILI9341 driving SDO low continuously
rather than releasing it. Same practical consequence.

---

## 5.2 Phantom touches at around six minutes — MITIGATED, CAUSE UNKNOWN

**The mitigation shipped.** `kernel/touch.c` has `TOUCH_DOWN_SAMPLES 2` — a
press is reported only after two consecutive qualifying samples — plus a
`g_blips` counter for how often a single sample failed to qualify, so the
suppressed events are counted rather than silently dropped.

**It did not reproduce.** UM-NATOS-033: two 13-minute runs, 91,000 samples, and
thermal drift eliminated as a candidate.

**What that means.** The alarming part is fixed: a phantom tap can no longer
launch a program, because a single spurious sample never becomes a press. The
cause was never found, and with the behaviour not reproducing there is nothing
left to measure.

**Left open deliberately.** If it ever returns, `g_blips` is the counter to
watch — a rising blip count with no presses is the signature of the original
behaviour being suppressed rather than absent.

## Where the code is

- `kernel/display.c` — `display_panel_read_pull()`, `display_panel_pad()`
- `kernel/touch.c` — `TOUCH_DOWN_SAMPLES`, `g_blips`, the pressure threshold
- `kernel/shell.c` — `panelid`, `panelpull`
- UM-NATOS-030 §7, UM-NATOS-031 §8, UM-NATOS-033
