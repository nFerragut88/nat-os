# UM-NATOS-048 — The Radio Hears, and Speaks

**Used Medias LLC — Embedded Systems Division**
Revision 1.3 · 2026-08-24 · Status: **nat-os receives real 802.11 beacons and transmits frames that an independent device displays. Both directions of the radio work.**

---

## 1. Abstract

nat-os is on the air, in both directions.

It **receives**: a passive scan across all thirteen channels decodes real beacon
frames and reports each access point by SSID, BSSID, primary channel and signal
strength. It **transmits**: hand-built beacon frames leave the antenna, and a
phone several feet away lists the SSID in its network picker.

Every report from UM-NATOS-034 to UM-NATOS-047 ended with a sentence saying
nothing had been on air, and every one of those sentences was true and was
measured. This report retires it.

Two defects account for almost all of it, and they are the same shape as each
other and as four found before them: **an OS adapter entry that returns a
plausible value and has the wrong semantics.**

- `g_ic->wpa_cb` was NULL for the entire life of the project, because the call
  that sets it lives in the open-source `esp_wifi_init()` *wrapper* and nat-os
  calls `esp_wifi_init_internal()` directly. With it NULL the radio decoded
  nothing. §5.
- `_recursive_mutex_create` returned a plain binary semaphore, so
  `esp_wifi_80211_tx` deadlocked taking a lock the calling task already held.
  §8.

The layout sensitivity that UM-NATOS-047 called "the thing blocking work" did
**not** block this stretch, and §9.2 records an occasion where it was blamed for
three consecutive builds and was innocent.

This report covers `next_moves/08` steps 197–209. UM-NATOS-047 covers 193–196.

> **Revision 1.1** retracts a claim made in 1.0: there is no leaked
> `g_wifi_global_lock`. §12.0 gives the measurement that disproved it and why
> the original reading was wrong. It also records `_event_post`, done at step
> 211, and two adapter entries corrected at step 210 — `_env_is_chip`, which
> had been telling the driver it was running on an FPGA, and
> `_esp_timer_get_time`, which returned zero for every timestamp.

---

## 2. State before and after

| | after 047 (step 196) | now (step 209) |
|---|---|---|
| `esp_wifi_scan_start` | never called | **ESP_OK, blocking, provably passive** |
| access points found | — | **2, by SSID and BSSID** |
| RSSI | — | **read, with the struct layout self-checked** |
| MAC interrupts taken | none | **hundreds, scaling with listening time** |
| `esp_wifi_80211_tx` | never called | **ESP_OK ×500, refused 0** |
| seen by another device | no | **yes — SSID in a phone's Wi-Fi list** |
| `g_ic->wpa_cb` | NULL | registered |
| `_recursive_mutex_create` | binary semaphore | **recursive, with owner and depth** |
| blob entry table | version 9 | version 12 |

```
boot        11 PASS 0 FAIL
wintorture  10 real switches, checksum CORRECT
blobphy     phyinit rc=0
wifiinit start
            init / wpa_cb / set_mode STA / start / ps NONE / promisc
            passive sweep ch1-13 x3 passes
            500 beacons on ch 1
            -- all ESP_OK
```

---

## 3. The receiver was deaf, and four things were eliminated cheaply

Step 202 established the measurement that had been missing. Interrupt counts had
been used as a proxy for reception, and they are not one: "L27 climbed to 6"
reads like progress and says nothing about whether a frame was decoded.
`esp_wifi_scan_get_ap_num()` is the driver's own answer.

The answer was zero. On a 500 ms passive dwell, on a live channel.

Four candidates were then eliminated, three of them by measurement:

**AMPDU.** `g_cfg.ampdu_rx_enable` and `ampdu_tx_enable` were `1`, while the
comment three lines above them said they should not be — *"block acknowledgement
is an optimisation and a large amount of state. Not needed to put one frame in
the air."* Turned off. Still zero. **Kept anyway**: the code now matches its own
documented intent and the driver carries less state.

**Buffers.** The driver allocates RX and management buffers during init, and
`_wifi_malloc` only began working at step 182 — nothing had checked the outcome
since. It gets everything it asks for:

```
alloc 37/26316B fails 0
```

**Channel.** This is the instructive one. Every scan this project had ever run
was pinned to channel 1. Step 199 pinned it deliberately, to bound the exposure
to a single probe request on a single channel if `scan_type` had been read at
the wrong offset and the scan transmitted. Step 201 proved the layout right and
the scan passive, **which retired that reason — and nobody moved the channel
back.** If the access point had been on 6 or 11, `found 0` would have been the
correct answer all along and the radio would never have been the problem.

It was not. Ten channels, 150 ms passive dwell each, zero on every one. The
radio was deaf across the band, not mistuned to one part of it — which also
eliminates the whole class of "the RF is fine, we were looking in the wrong
place" explanations.

**PHY init data** needed no test. The 128-byte table had already been diffed
byte for byte against ESP-IDF when the six TX-power entries were corrected, and
every other byte matches.

### 3.1 What the driver's own call log did and did not say

```
42 _set_intr x1    43 _set_isr x1    44 _ints_on x1
55 _queue_send_from_isr x4    56 _queue_msg_waiting x4    49 _event_post x2
```

Four MAC interrupts, four ISR posts, four received by the worker. **The
interrupt path works end to end** — matrix, line remap, level-3 handler,
trampoline, the driver's own ISR, the ISR-safe queue post, the worker's receive.
Every piece. It was simply never asked to run.

The list is **capped**. Indices 79, 85, 87, 92, 93 and 117 appear in the call
trace and not in the list, so absence from it is not evidence of absence. This
is recorded because the temptation to read "`_timer_arm` is not in the list,
therefore no timer was armed" was immediate and would have been wrong.

---

## 4. A second scan panicked, and the first reading of it was wrong

Step 202 recorded a `LoadProhibited` when `channel` was set to 0 (all channels)
and filed it as a defect in the all-channel path. Step 204 found the same
exception on a *repeated single-channel* scan and doubted that reading. Reading
the blob settled it:

```asm
l32r  a5, <g_ic>          ; 0x3ffd526c
l32i  a6, a5, 0x1b4       ; g_ic->[0x1b4]
l32i  a6, a6, 84          ; FAULT -- excvaddr 0x54, so [0x1b4] was NULL
```

Not an all-channel path at all. A null pointer that both paths reach.

---

## 5. The missing initialisation step

`g_ic + 0x1b4` is written in exactly **one** place in the blob's 180,000
instructions:

```asm
40322554 <esp_wifi_register_wpa_cb_internal>:
  entry a1, 32
  call8 esp_wifi_unregister_wpa_cb_internal
  l32r  a8, <g_ic>
  s32i  a2, a8, 0x1b4
```

and that function **has no caller anywhere in the blob**. It is an export.

In ESP-IDF it is called by `esp_supplicant_init()`, which is called by the
`esp_wifi_init()` wrapper — open-source IDF code, not part of this blob. nat-os
calls `esp_wifi_init_internal()` directly, and always has. So the WPA callback
table had been NULL since the driver first initialised at step 189, and nobody
had reason to look for a step that does not appear in any header this project
links against.

### 5.1 Stubs, not zeros

The obvious repair — hand it a zeroed table — is wrong, and the blob says so
plainly. The driver is **inconsistent about which half it null-checks**:

```asm
cannel_scan_connect_state          wifi_station_start
  l32i  a6, a5, 0x1b4                l32i   a9, a3, 0x1b4
  l32i  a6, a6, 84                   beqz.n a9, skip     <- TABLE checked
  beqz.n a6, skip   <- FN checked    l32i   a10, a9, 0
                                     callx8 a10          <- FN not checked
```

A zeroed table fixed the `LoadProhibited` at NULL+0x54 and immediately produced
an `InstFetchProhibited` at 0 from `wifi_station_start`.

A static scan of the 41 read sites was attempted and **could not** say which
entries are called unguarded: following the destination register forward runs
into the register being reused, and it reported offsets like 18 and 22, which
cannot be function-pointer slots at all. That output was discarded rather than
acted on.

Instead, every slot points at one stub that records **where it was called
from**, so the driver names the entries it needs:

```
wpa hits 16  0x4031f744 0x4031d2f5 0x4031d604 0x4031ecfb ...
```

Four distinct call sites, the first inside `wifi_station_start`.

### 5.2 The result

```
before  found 0 on every channel, L27 flat at 1-4
150 ms  ch6 and ch11 found 1, twice, L27 19 and 22
600 ms  ch11 found 1, L27 56 and 58
```

The interrupt count **scales with how long the receiver listens** — four, then
twenty, then fifty-six. That is sustained reception, and it is precisely the
claim step 198 had to retract for asserting without evidence.

The repeated-scan `LoadProhibited` of §4 is also gone. Thirty-nine consecutive
scans, no fault.

---

## 6. An SSID, by name

`ap_num` returning 1 is the driver counting its own work. An SSID is a string
transmitted by somebody else's hardware, which can only have arrived through the
antenna, the PHY, the MAC, the interrupt line, the ISR, the queue and the worker
task. `esp_wifi_scan_get_ap_records` was added to the entry table (version 12):

```
ch 6   ap[0] rc 0x0  bssid 44:25:38:xx:xx:xx  ssid [T....]
ch 11  ap[0] rc 0x0  bssid 7e:26:f6:xx:xx:xx  ssid [i.........]
```

**Redacted deliberately.** This repository is public and a BSSID is a
geolocation, resolvable against public wardriving databases. What survives is
the OUI, and the OUI is the corroboration: `44:25:38` is Technicolor and the
SSID it arrived with is the Technicolor default form. The vendor prefix and the
name agree, so this is a decoded beacon and not thirty-three bytes of noise that
happened to be printable.

### 6.1 One record, on purpose

Only record **zero** is read. `wifi_ap_record_t` has begun with `bssid[6]` then
`ssid[33]` in every ESP-IDF version, so those two offsets are safe. The
**stride** between records is not: the blob decides it inside
`wifi_get_ap_list_process`, and no header this project can check states it.
Reading one record needs no stride. Reading two would need one that is not in
evidence.

---

## 7. Measuring the reception instead of worrying about it

Step 206 ended by calling the reception "marginal", on the evidence that one
channel found an access point at 150 ms and none at 600 ms. That was the right
thing to be suspicious of and the wrong conclusion, and one number settles it.

The sweep was made to repeat — three passes, pass-major rather than
channel-major, so that consecutive scans of one channel do not share whatever
transient state a single scan leaves behind.

| channel | RSSI | detected |
|---|---|---|
| 11 | **−60 / −61 dBm** | 3/3, every run |
| 6 | **−91 dBm** | 1/3 to 2/3, varying by run |

−61 dBm is an ordinary strong signal and it is heard every time, same BSSID
every time. −91 dBm is the noise floor, where **any** receiver is intermittent.
That is not a defective receiver; it is a correct receiver reporting a weak
transmitter honestly. And "only one access point per channel" was never a
ceiling — it is what is in range.

### 7.1 The struct layout is checked, not assumed

Reading RSSI means trusting `wifi_ap_record_t`'s offsets past `ssid[33]` —
`primary` at +39, `second` at +40, `rssi` at +44 — which come from a header not
provably matched to this blob. That is the same doubt that made step 199 refuse
to trust `scan_type` and §6.1 refuse to read a second record.

Here it costs nothing to settle. **+39 is the primary channel, and the sweep
already knows which channel it asked for:**

```
ch@39=11 on the channel-11 scan     ch@39=6 on the channel-6 scan
```

If it had disagreed the line would print `MISMATCH` and no dBm figure. It
agrees, so +44 really is the RSSI.

---

## 8. Transmit

`esp_wifi_80211_tx` never returned. Not an error, not a fault — `call 0 enter`
and then nothing, forever. Reading the blob:

```asm
l32r a13, <g_osi_funcs_p>
l32r a9,  <g_wifi_global_lock>
l32i a7,  a13, 0
l32i a10, a9, 0            ; the argument
l32i a7,  a7, 84           ; wifi_osi_funcs_t + 84 = _mutex_lock
callx8 a7
```

Offset 84 is `_mutex_lock` and 88 is `_mutex_unlock`, confirmed by the failure
path taking 88 and returning `ESP_ERR_NO_MEM`. So the hang was in **nat-os's own
code**, on the driver's global lock.

The semaphore pool named the holder:

```
[sem] before tx  #0=1/1  #1=1/1  #2=0/1 heldByTask5
```

Task 5 is the shell task. **The caller.** It was waiting for a lock it already
held.

`g_wifi_global_lock` is created with `_recursive_mutex_create`, and
`osi_s_recursive_mutex_create` returned `osi_impl_sem_create(1,1)` — a plain
binary semaphore. A recursive mutex re-entered by its owner must succeed; a
binary semaphore re-entered by its owner deadlocks.

The stub carried a comment: *"nat-os's mutex is recursive already — [6b]
verifies depth."* That is **true of `blob_lock`**, a completely different
object, and it is why the entry was never re-examined.

`osi_sem_t` now carries `recursive`, `owner` and `depth`. The lock frees at
depth zero only: releasing on the inner unlock would hand it to another task
while this one is still inside the region it believes it owns, which is worse
than the deadlock it replaces. The stub reaches the new constructor through
`w2c_call1` with a dummy argument rather than a new `w2c_call0`, because
`window.S` is the file where removing three dead stores broke
`esp_wifi_init_internal` at step 194 and it is not growing for a convenience.

### 8.1 Verified by something that is not this board

A return code proves nothing here. `scan_start` returned `ESP_OK` for six steps
while the radio decoded nothing.

So the transmit sends **beacons**, carrying an SSID that any nearby phone will
list in its network picker: an independent receiver, owned by someone else,
displaying a string this code chose.

```
tx  beacon 70 B, ch 1, ssid [nat-os-transmitting] x500
tx  accepted 500  refused 0        (62 s, 12.5 ticks/beacon)
```

**It was seen.** The name appeared in a phone's Wi-Fi list.

The SSID is deliberately unmistakable and deliberately nobody else's.
Impersonating a real network would be trivial here and is not something this
project will do. Channel 1 was chosen because 6 and 11 are the two channels the
sweep found real access points on and there is no reason to sit on top of them.

The frame is a textbook beacon — `fc/dur/da/sa/bssid/seq`, timestamp, beacon
interval, capability, then SSID, supported-rates and DS-parameter tags — built
by hand rather than by asking the driver for AP mode, which would pull in
`esp_wifi_set_config` and a second interface for no gain in what is being
established.

### 8.2 What this does not establish

- **No association.** No probe response, no authentication, no four-way
  handshake, no data frame. The beacon is transmitted; nothing answers it and
  nothing is expected to.
- **No receive-side verification of our own transmission.** The phone confirms
  the frame is decodable; nat-os cannot hear itself.
- **Transmit power and rate are whatever the driver chose.** Neither was set,
  measured, or checked against the PHY init data's TX-power table.
- **`g_wifi_global_lock` is *not* leaked.** Revision 1.0 said it was. See
  §12.0 — the claim was wrong and is retracted.

---

## 9. Method

**Read the blob instead of guessing.** Every substantive result in this report
came from disassembly: the single writer of `g_ic+0x1b4`, the guarded/unguarded
asymmetry in §5.1, `_mutex_lock` at offset 84. Step 201 established this habit
by finding a poll loop after two wrong guesses; this stretch did not guess.

**A single sample is not a result.** §7 exists because "found 1 at 150 ms, found
0 at 600 ms" was treated as a finding for one step before being measured
properly. Step 198 had already had to retract exactly this.

**Falsify the assumption when it is free.** §7.1 turns an assumed struct layout
into a checked one for the cost of printing a byte that is already known.

**Absence from a truncated list is not absence.** §3.1.

### 9.1 Eliminating a hypothesis is a result

The channel sweep (§3) disproved a theory rather than confirming one, and it was
the most valuable single measurement of the stretch: it killed an entire class
of explanation. Three of the four cheap candidates produced no fix, and two of
the changes were kept anyway because they were correct on their own terms.

### 9.2 The layout wall was blamed for a bug it did not cause

Adding the WPA table panicked *inside* `esp_wifi_init_internal` at a garbage PC.
The layout sensitivity was blamed. The caller was trimmed from fifteen lines to
four; then the code was moved out of its new translation unit into an existing
one.

**The fault did not move by a single byte across all three attempts** — the same
`epc 0x503b015c` every time.

A layout effect moves. That is the whole point of it. The second identical epc
should have ended the theory, and it took a third. The actual cause was an ABI
violation: `wifi_bringup()` is in `kernel/` and is call0, and it was calling a
function in `vendor/windowed/` directly. **This project enforces that boundary by
directory and the boundary was crossed anyway.**

The diagnostic worth keeping: *a layout fault moves when sizes change; an ABI
fault does not.*

### 9.3 Two errors of pacing and constants

`TX_BEACONS` never changed. The edit that set it to 900 hit an assertion and
aborted **before writing the file**; the rebuild carried the old value, and the
operator was asked to watch a phone for ninety seconds while the board sent four
beacons over four tenths of a second. Not checking that a constant landed before
flashing is the same failure as building without `-Flash`, which
`docs/next_moves/README.md` already warns about.

And the pacing was asserted rather than measured. `task_sleep(10)` is ten
**ticks**, not ten milliseconds, and each `blob_call` costs about twelve ticks
more — so "~100 ms between beacons" was really 220 ms, which is why 900 of them
did not fit in a 170-second window. The loop now prints its own rate; with the
sleep at one tick it measures 12.5 ticks per beacon, near enough the 102 ms a
real access point uses.

---

## 10. The pattern, stated plainly

Six entries in this investigation have had a plausible body and the wrong
semantics:

| entry | what it did | what it should have done |
|---|---|---|
| ETS timers | counted the call, returned | arm a timer against the caller's own struct |
| event groups | `return 0` on every operation | create, set, clear and wait |
| `_task_delay` | empty | sleep, so a poll loop can yield |
| `_queue_send_from_isr` | returned 0, posted nothing | post the received frame |
| `_recursive_mutex_create` | binary semaphore | recursive mutex |
| (WPA table) | never registered at all | — |

**None of these is visible to a compiler, a linker, or a scan for unimplemented
stubs.** There is no stub to find; there is a function that returns a
believable value. Every one was caught by watching the driver do something and
noticing it did not do the next thing.

This is the single most useful generalisation the project has produced about
integrating a binary driver, and it should be assumed to apply to the remaining
untested entries rather than hoped not to.

---

## 11. What is on air

Something, at last, and it was seen by another device.

- **Received:** real 802.11 beacon frames, decoded, with SSID, BSSID, primary
  channel and RSSI. Two access points, reproducibly, across three passes.
- **Transmitted:** 500 beacon frames on channel 1 over 62 seconds, accepted by
  the driver without a single refusal, and displayed by an independent receiver.

Not associated. No data path above the MAC. No IP, no ARP, no DHCP — nat-os has
no network stack at all, and none of this report changes that.

---

## 12.0 Retraction — the lock that was never leaked

**Revision 1.0 of this report named a leaked `g_wifi_global_lock` as the
highest-priority open defect. There is no such leak.**

Step 211 instrumented `_mutex_lock` and `_mutex_unlock` to record the blob's own
return address for every acquisition and pop it on release. The acquisition
stack came back **empty** — the two are exactly balanced — while the pool still
showed a semaphore at zero.

Printing whether each semaphore is *recursive* settled it:

```
#0=1/1R   #1=1/1R   #2=0/1s
```

Both recursive mutexes are **free**. `#2` is a plain signalling semaphore —
`osi_impl_thread_sem_get` creates them with an initial count of **0** — so zero
is its correct resting state and nobody holds it.

Two things produced the error, and both were mine. Reading "count 0" as "held"
conflates a mutex with a semaphore. And the diagnostic printed `heldByTask5` for
*any* semaphore at zero, which turned that conflation into an authoritative-
looking line of output that was then believed. **A diagnostic that overstates
its certainty costs more than no diagnostic**, and this one propagated into a
published report. It now prints `held` only for a recursive mutex with a nonzero
depth, and reports that depth.

What remains true from §8: `_recursive_mutex_create` really did return a binary
semaphore, `esp_wifi_80211_tx` really did deadlock re-entering it, and fixing it
is what made transmit work. The defect and the fix are unaffected. Only the
speculation about *why* the semaphore was at zero was wrong.

---

## 12. What remains

1. ~~**The leaked `g_wifi_global_lock`.**~~ **RETRACTED — see §12.0.**
2. ~~**`_event_post`**~~ **— done at step 211.** It records every event the
   driver raises and reports them. Measured: one `id=2` (`STA_START`) after
   `esp_wifi_start`, then exactly one `id=1` (`SCAN_DONE`) per scan — 39 scans,
   39 events. `event_base` arrives NULL because `WIFI_EVENT` is defined by the
   open-source `esp_event` component and is not in the blob; the ID carries the
   information.
3. ~~**The `wifi_ap_record_t` stride**~~ **— measured at step 212: 84 bytes.**
   Not from a header and not from a lucky scan, but from the blob's own address
   arithmetic in `wifi_get_ap_list_process` (`addx2`/`subx8`/`addx4` → `base +
   i * 84`), which is true whether or not anything is on air. Confirmed three
   ways: that arithmetic; the channel byte at +39 of *record one* agreeing with
   the channel scanned; and record one decoding to a coherent vendor OUI whose
   SSID is that vendor's default naming form. All records are now read.
4. ~~**The remaining unwired adapter entries**~~ **— audited at step 213, no
   live defect.** See §13.
5. ~~**The layout sensitivity**~~ **— explained and eliminated at step 214. It
   was never positional.** See §14. What replaces it on this list is the wild
   write that caused it, which is still unfound.
6. **The `w2c_*` bridges** still allocate over their caller's base save area.
   Unchanged since UM-NATOS-045 §8.4.
7. **Association**, now the next milestone. Item 2 was one of its two
   prerequisites and is done; the other is that the WPA callbacks are still
   recording stubs, which is a body of work rather than a defect.

---

## 13. The unwired entries, audited

§10 said the remaining untested entries should be **assumed** to share the
defect pattern rather than hoped not to. Step 213 tested that assumption instead
of leaving it as a worry. All 116 stubs were scanned for bodies that reach no
implementation — 64 do — and then `osiused` was read to find which of those the
driver actually calls.

**The five that looked most dangerous are never called at all:**

| entry | calls | why it does not matter |
|---|---|---|
| `_queue_create` | 0 | driver uses `_wifi_create_queue`, which is wired |
| `_task_create` | 0 | only `_task_create_pinned_to_core`, ×1, wired |
| `_queue_send_to_back` / `_to_front` | 0 | `_queue_send` ×420, wired |
| `_realloc_internal` / `_wifi_realloc` | 0 | — |
| `_task_delete` | 0 | — |

Latent, not live. They should still be implemented before anything new is
enabled, because "not called" is a property of the current configuration and not
a guarantee.

**Two ARE called and are empty, and both are empty in ESP-IDF too:**

- `_wifi_apb80m_request` ×3 / `_release` ×2. IDF's wrapper body is entirely
  inside `#ifdef CONFIG_PM_ENABLE`. nat-os has no power management, so empty is
  not a stub — it is the correct implementation.
- `_phy_update_country_info` ×1. IDF's `esp_phy_update_country_info` body is
  entirely inside `#if CONFIG_ESP_PHY_MULTIPLE_INIT_DATA_BIN`. nat-os has one
  hardcoded 128-byte PHY init table, so returning `ESP_OK` is correct.

Both were checked against the ESP-IDF source on this machine rather than from
memory, which is the habit UM-NATOS's own notes require for register and header
facts and which applies equally to "what does the reference implementation
actually do here".

**And two that were fixed at step 210 turn out to have been lying constantly:**
`_env_is_chip` ×99 per run (returning "this is an FPGA") and
`_esp_timer_get_time` ×340 per run (returning zero). Neither produced a
measurable symptom, which is exactly what makes this class of defect worth
hunting deliberately rather than waiting for.

---

## 14. The layout sensitivity was a wild write

**UM-NATOS-047 §5.3 called this "the thing blocking work" and measured its width
at 30–100 bytes. UM-NATOS-042 §9.2 recorded the same shape for `shell.c` as the
step-7 layout band. Both explained it as code position — the flash MMU,
alignment, the timing of a bridge on the hot path. It is none of those.**

The reproducer was three dead stores in `window.S`, kept since step 194 because
deleting them made `esp_wifi_init_internal` return `ESP_ERR_NO_MEM` from a cold
boot, reproducibly, bisected to exactly those instructions. They are deleted
now and everything works.

### 14.1 The chain

`0x101` is a resource refusal, not a fault — which meant the failure could be
followed rather than guessed at. The OSI call sequence diverges at one point:

```
working  ... _task_create_pinned_to_core(6656) -> _semphr_take -> _queue_recv ...
broken   ... _task_create_pinned_to_core(6656) -> _wifi_delete_queue -> _free
```

Task creation failed. `blob_task_create` has **four** paths that return `pdFAIL`,
and three of them printed nothing whatsoever — so the driver reported `NO_MEM`
and nothing in between said which one fired. Making all four report turned six
steps of mystery into one line:

```
[blobtask] req en=0 stack=6656 prio=23 fn=0x4036bb64
[blobtask] refused: creation disabled
```

**The request is perfect.** `stack=6656`, `prio=23`, a valid function pointer.
Not a corrupted argument, and not the `w2c_*` save-area overlap of UM-NATOS-045
§8.4 — which was the hypothesis going in, and was wrong. `g_bt_enabled` was 0.

### 14.2 The shell disables it on purpose

```c
blob_task_enable(str_eq(arg, "task"));
wifi_start_enable(str_eq(arg, "start"));
```

`wifiinit start` calls `blob_task_enable(0)`. It always has. **The driver's own
task could never legitimately be created by the command this project has run
several hundred times.**

### 14.3 So why did it ever work

The working build reports:

```
[blobtask] req en=1073452080
```

`0x3FFB9430` — a **DRAM stack pointer**, in an `int` that holds 0 or 1.
Something writes a stack pointer over that global, and a stack pointer is not
zero, so `if (!g_bt_enabled)` passes and the task is created.

That is the entire sensitivity. Move nine bytes of code, the wild write lands
elsewhere, `g_bt_enabled` stays 0, and the driver **correctly** refuses. **The
build that failed was the honest one.** Every build that has "worked" since step
194 depended on memory corruption to enable a feature the command line had
switched off.

The fix is what the code always meant — starting the driver enables its task:

```c
void wifi_start_enable(int on) { g_want_start = on; blob_task_enable(on); }
```

### 14.4 What this does not fix

**The wild write is still there and still unfound.** This removes the
*dependence* on it, not the bug. It will corrupt something else eventually — but
nothing benefits from it now, so the next symptom will be honest rather than
load-bearing.

It also does not explain §9.2's `shell.c` instance, which hung `blob_map` rather
than refusing a task. Same class most likely — the same wild write landing on a
different global — but that is a hypothesis and is recorded as one.

### 14.5 Method

**A silent failure path is not a small defect.** Three unreported `return 0`s in
one function cost this project six steps, two reports' worth of wrong
explanation, and a piece of dead code that could not be deleted. The fix was
four `uart_puts` calls.

**"Positional" was a category, not an explanation.** Once named, it stopped
anyone looking further — including me, three times in one session (§9.2). A
label that ends inquiry is worse than an open question.

**An error code is more tractable than a crash.** `ESP_ERR_NO_MEM` says a
resource was refused, which narrows the search to code that refuses resources.
The instinct to treat it as "the wall again" wasted more time than reading it
did.

---

Full experiment log: `docs/next_moves/08-wifi-via-loaded-blob.md`, steps 197–213.
Companion reports: UM-NATOS-042 (rev 1.1), 043 (rev 1.3), 044 (rev 1.0),
045 (rev 1.0), 046 (rev 1.0), 047 (rev 1.0).

**nat-os has received a frame and transmitted one. Neither sentence could be
written before this report.**

Written by: Hare
