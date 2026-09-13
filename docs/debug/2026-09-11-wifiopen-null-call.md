# `wifiopen` panics: exccause 20, epc 0x00000000

**2026-09-11.** A reproducible kernel panic whenever the radio is brought up.
Traced to an unchecked allocation inside the vendor blob. Fixed by giving the
heap back 13 KB that two shell diagnostics were holding permanently, and by
setting buffer counts the board can actually pay for.

This record keeps the disproven hypotheses, and two mistakes of my own that a
later reader would otherwise repeat.

---

## The failure

```
>>> wifiopen
*** KERNEL PANIC ***
  exccause : 20  (InstFetchProhibited)
  epc      : 0x00000000   <- faulting instruction
  last osi : entry 38  _task_delete
  osi alloc: calls 18  bytes 10988  largest 3120  FAILS 1  heap free 9784
```

`epc 0` with `InstFetchProhibited` is a **call through a null pointer**.

**Working condition:** none. Every `wifiopen` on the `-WiFi` build panicked.
**Failing condition:** `wifiopen`, immediately, on every build tried.

---

## H1 — I caused it, with the DRAM this session spent

`html.c` added 3,216 bytes of `.bss` and `WEB_BODY_MAX` 768→2048 added 1,280.

**Experiment.** A git worktree at `a1fdd0b` (pre-session HEAD), built and
flashed, `wifiopen`.

**Result.** Identical panic — `exccause 20, epc 0x00000000`, same
`last osi: _task_delete`, same `FAILS 1`.

**ELIMINATED.** Not mine.

### My first mistake, recorded so it is not repeated

I had earlier written that the fault was "recorded as far back as boot #83" and
called it long-standing. That was a misreading: **boot #83 is simply the most
recent occurrence**, and the boot counter had climbed there through this
session's own reflashes. Tracing the captures:

| capture | boot | LAST FAULT reported |
|---|---|---|
| `verify1` | #62 | exccause **29**, epc 0x40080009 (boot #58) |
| `v_run` | #66 | exccause 20, epc 0 (boot #62) |
| `s7` | #85 | exccause 20, epc 0 (boot #83) |

The exccause-20 flavour first appears **at boot #62**, which was this session's
first `-WiFi` flash. That made H1 look strong, and the worktree test is what
actually settled it. A boot number is not a count of failures.

---

## H2 — heap fragmentation

`osi alloc` reported `FAILS 1` with `heap free 9784`. A failure with 9.8 KB free
looks like fragmentation.

**Experiment.** `osi_alloc_note()` already captured `heap_largest_free()` and
**had never printed it anywhere** — the UM-NATOS-060 pattern again, in the one
instrument being relied on. Added `g_osi_fail_size/_free/_largest/_blocks`,
recorded at the first failure, and printed them.

**Result.**

```
osi FAIL  : wanted 1604 B, free 1312, LARGEST BLOCK 1312, blocks 19
```

**ELIMINATED.** Free equals largest block: not fragmented, genuinely exhausted.
The `heap free 9784` on the line above is sampled **at panic time**, after
unwinding, and is not the state at the failure. An instrument reporting a number
from the wrong moment is worse than one reporting nothing.

---

## H3 — fixed shortfall of ~900 bytes

Two builds:

| heap at boot | allocated before failing |
|---|---|
| 11,592 | 10,988 |
| 16,504 | 15,800 |

Difference 4,912 heap → 4,812 more allocated. Extrapolating a fixed demand of
15,800 + 1,604 = 17,404 against HEAD's 16,504 gave "short by 900".

**Experiment.** Free 8,448 bytes by shrinking `snap` in shell.c, giving 20,024.

**Result.** Still panicked, having allocated **20,612** and failed with **16
bytes** free.

**ELIMINATED.** The demand is not fixed. It consumes whatever is there.

---

## H4 — `static_rx_buf_num` is the appetite

`static_rx_buf_num = 10`, and 10 × 1,604 = 16,040.

**Experiment.** 10 → 4.

**Result.** Total moved from 10,988 to 10,916 — **72 bytes** — panic identical.
Recorded at the time as eliminating H4.

### My second mistake

**That elimination was invalid and the hypothesis was right.** At 11,576 bytes
free the heap runs out after ONE buffer, so 4 and 10 fail identically; the
experiment could not have distinguished them. A count only becomes visible once
there is room to reach it. Rule 6 says change one variable at a time; it does
not say the variable you changed was capable of showing anything.

---

## H5 — the blob allocates RX buffers until the heap is dry (CONFIRMED)

**Experiment.** Log every allocation size in order, not just the total.

**Result.**

```
osi sizes : 32 4 60 8 24 3120 1024 136 136 136 596 596 596 596 596 120 1604 1604
            └─────────── fixed overhead = 7,780 ───────────┘ └─ RX buffers ─┘
```

and with 23,688 bytes free, all ten static buffers land and an **eleventh** is
requested:

```
osi alloc : calls 26  bytes 23820  FAILS 1
osi sizes : ... 1604 x10
osi FAIL  : wanted 1604 B, free 432
```

**CONFIRMED.** 7,780 bytes of fixed overhead, then one 1,604-byte buffer per
`static_rx_buf_num`, then **dynamic** buffers: `dynamic_rx_buf_num` was 32, i.e.
another 51,328 bytes. The reference configuration needs 23,820 bytes before a
single dynamic buffer and could never have been met on this board.

**And the blob does not check the result.** `osi table : 118 words, no null
slots`, so the null call is inside the blob: `heap_alloc` returns NULL, the blob
calls through it, `epc 0x00000000`.

---

## The fix

| where | was | now | frees |
|---|---|---|---|
| `shell.c` `snap[NS][NW]` (`txwatch`) | NS 384 | NS 32 | 8,448 B |
| `shell.c` `many[DISP_W*MANY_H]` | MANY_H 12 | MANY_H 4 | 3,840 B |
| `wifi_init_cfg.c` `static_rx_buf_num` | 10 | 6 | 6,416 B |
| `wifi_init_cfg.c` `dynamic_rx_buf_num` | 32 | 8 | bounded |
| `wifi_init_cfg.c` `dynamic_tx_buf_num` | 32 | 8 | bounded |

Two shell diagnostics were holding **13 KB of `.bss` permanently**. `snap`'s own
comment already recorded it being halved twice for DRAM, and `many`'s recorded
being halved once — the memory had been coming out of the radio all along, and
nobody had connected the two.

**Result.**

```
>>> fb
   framebuffer off, 0 B, heap free 23688
>>> wifiopen
   (no panic)
>>> wifijoin
   joining ivory-billed with the compiled-in passphrase
   lwip      netif up, mtu 1500, starting DHCP
   dhcp      state 8 tries 2  addr 0.0.0.0
   associated; data path started
```

No panic, the radio associates, the data path starts, **and the serial link
survives** — the first time `wifiopen` has not ended the session.

DHCP moved from state 6 (SELECTING, 9 retries, nothing arriving) at
`static_rx_buf_num = 4` to **state 8 (CHECKING)** at 6, which means an OFFER was
received. Four static buffers associate but cannot catch an OFFER; six can.

---

## What this does to step 376

376 concluded **supply, not software**, inferring it from the CH340 vanishing
rather than the ESP32 rebooting — and listed what that did not establish:
*"the board went silent, it did not reboot, so this is inference from the shape
of the failure rather than from a reset register."*

A panic produces the same silence: after one, nothing is scheduling, so serial
stops. There was no panic report to look at because nobody had run `wifiopen`
with a capture that survived long enough to print one.

377b's *"changing the connection made the radio come up and stay up"* is a real
observation and may be a second, independent problem. It is not evidence that
there was no software fault.

---

## Outcome, 2026-09-13

Confirmed on hardware once the board was moved to a working USB socket (hub port
7 never enumerated it; ports 1 and 2 always had):

- `wifiopen` does not panic
- DHCP reaches **BOUND**, address 192.168.1.140 -- 392 could only see CHECKING
- a real HTTP fetch of example.com returns **200**, 828 bytes, **0 dropped**,
  and the web view renders it as 142 bytes of text with one link extracted

So the fix holds end to end. The `static_rx_buf_num` question stands: 6 is the
first value that received a DHCP offer, not a measured optimum.

**376's supply fault is separate and still real.** It was not the cause of the
panic and the panic was not the cause of it. Both make the board go silent, and
that is why each hid the other.

## Next

- **Does DHCP bind?** It reached CHECKING. The board stopped answering during
  the follow-up capture, so BOUND has not been observed.
- **Then a real fetch, rendered.** Everything below "bytes arrived" is already
  proved (step 390); this is the last unjoined link.
- **Is 6 the right count?** It is the first value that received an OFFER, not a
  measured optimum. 8 may fit — 7,780 + 8×1,604 = 20,612 against 23,688 — and
  would leave less for dynamic buffers.
- **The 13 KB is not the only slack.** `task.c` holds 26,624 bytes of stacks and
  `lwip_memp.c` 11,472; neither has been examined.
