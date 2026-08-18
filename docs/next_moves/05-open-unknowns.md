# 05 — Two unexplained behaviours

**Size:** small each. **Risk:** none. **Blocked on:** nothing.

Neither breaks anything. Both are recorded because an unexplained behaviour that
nobody wrote down becomes folklore, and folklore gets designed around.

---

## 5.1 MISO reads all zeros

`docs/UM-NATOS-015` §3 records the display pinout as:

```
| MISO | 12 | unused -- the driver never reads the panel |
```

"Wired but unused" was documented and, on the evidence, never tested.

`display_panel_read()` and the `panelid` command now exist. `0xD3` (Read ID4)
returns `00 93 41` on every ILI9341 ever made. nat-os gets:

```
0xD3 -> 00 00 00 00 00
0x04 -> 00 00 00 00 00
```

**Why it matters:** if the panel could be read back, *"is what is on the glass
what we sent?"* becomes a checksum comparison instead of a question only a human
can answer. That question cost most of a day during the DMA bug hunt, and
`fbdump` only got halfway to solving it — it shows what is in DRAM, not what is
on the glass.

**Candidates, in order:**

1. **SDO not populated on this module.** Common on the ESP32-2432S028R. Check
   the board with a meter or a magnifier — GPIO12 to the panel flex.
2. **Read timing.** `display_panel_read()` drops to 2 MHz, which should be
   ample, but the dummy-clock count for `0xD3` may be wrong. Try reading more
   bytes and looking for `93 41` at any offset.
3. **Pad configuration.** GPIO12 is set to HSPIQ with `FUN_IE`. Verify the
   IO_MUX register reads back as written and that nothing else claims GPIO12.

**Caution:** GPIO12 is MTDI, a strapping pin. Held high at reset it selects a
1.8 V flash supply and the board will not boot. Nothing currently drives it —
the pad is a peripheral input — and that must stay true.

---

## 5.2 Phantom touches at around six minutes

Two independent unattended runs logged spurious touch presses with nobody in the
room:

| run | first press | touch errors | consequence |
|---|---|---|---|
| 1 | ~390 s | 25 | **4 taps, 2 opens — launched a program into slot 2** |
| 2 | ~374 s | 46 | none (routing was suppressed by `touchoff`) |

Run 1 is the alarming one: a phantom tap **started an application**. That is a
device doing something nobody asked for, and on a machine meant to run
unattended it is a real defect rather than a curiosity.

**What is known:** the presses carry real pressure readings (`z1max`, `zmax`
move), so the XPT2046 is reporting them, not the kernel inventing them. The
timing is suspiciously consistent — around six minutes in both runs.

**Candidates:**

1. **Thermal drift.** Six minutes is about right for a small board to reach
   equilibrium. If the pressure threshold sits near a drifting baseline, it
   would start tripping at a repeatable time. Test: log `z` continuously from
   boot and look for a trend crossing the threshold.
2. **Charge accumulation on the panel.**
3. **A genuine electrical event** — supply sag from the WiFi PHY, the SD card,
   or a flash erase coinciding with a sample.

**Cheapest test:** `touchoff` leaves sampling live while suppressing routing, so
a long run with periodic `z` logging costs nothing and would settle candidate 1
immediately.

**Mitigation regardless of cause:** require two consecutive samples above
threshold before reporting a press. A real finger is present for many samples;
a noise spike is not. That is a small change in `kernel/touch.c` and would make
the system trustworthy unattended even if the cause is never found.

## Where the code is

- `kernel/display.c` — `display_panel_read()`, `SPI2_CLKDIV_READ`
- `kernel/touch.c` — sampling, `z1`/`z2`, the pressure threshold
- `kernel/kmain.c` — `task_touch()`, `g_touch_events_off`
- `docs/UM-NATOS-030` §7 and `UM-NATOS-031` §8
