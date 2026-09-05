# UM-NATOS-054 — The Radio in an Icon

**Used Medias LLC — Embedded Systems Division**
Revision 1.0 · 2026-09-04 · Status: **nat-os joins a WPA2 network and serves a page from a touchscreen icon. The passphrase is typed on the device and kept across reboots.**

---

## 1. Abstract

```
two taps on a launcher icon
  -> vendor radio initialised
  -> 13 channels swept
  -> WPA2-PSK associated, four-way handshake complete
  -> DHCP bound, 192.168.1.140
  -> HTTP 200, 350 bytes, 625 ms, in a browser on another machine
```

UM-NATOS-053 left nat-os serving HTTP over WPA2 — from a shell command, typed
over a serial cable, by someone who knew the command. This report is the work
that turned that into an **application**: an icon, a list of networks, a
passphrase typed on the glass, and a credential that survives a power cut.

It covers `next_moves/08` steps 277–296.

The engineering is in §4–§7. The finding that outlasts it is §8: **four separate
times, a working system was reported as broken because the interface would not
say what it was doing.** Not one of those was a fault in what the code did. All
four were faults in what it claimed.

---

## 2. State before and after

| | after 053 (step 273) | now (step 296) |
|---|---|---|
| joining a network | `wifiinit start` over serial | **two taps on an icon** |
| which network | compiled into the binary | **any, with the passphrase typed on the device** |
| the passphrase | `#define WIFI_STA_PASS` | **typed, saved to flash, forgettable** |
| the keyboard | two copies, term and notes | **a module, plus those two copies (§7.3)** |
| scan results | printed to a serial console | **a list you can touch** |
| the page | reachable if you caught the window | reachable because the board stays on the network |
| ABI violations paid for | 5 | **6** (§5) |

---

## 3. What was built

**`kernel/wifiapp.c`** — the view. A state for every condition the radio can be
in, each one saying what it is. A sweep spread one channel per pass so the list
fills rather than freezing. Networks merged on SSID, strongest sighting winning:
the 2.4 GHz channels overlap by about 20 MHz, so one access point is sighted on
three of them and the first version listed three rows.

**`kernel/keyboard.c`** — the multi-tap keyboard, factored out on the terms
`term.c`'s own comment set: *"If a third consumer appears, factor it then."*

**`kernel/wificred.c`** — passphrases in flash, in their own sector with their
own magic, version and checksum. **Not** in `store.c`: that record is rejected
outright on a version mismatch, so extending it would have discarded the touch
calibration to gain a saved password. `flash.h` reserves 0x200000 for the record
and 0x201000 for the message sector, and the blob starts at 0x220000; the 120 KB
between was unused. One sector of it now holds eight credentials.

It is **not secure**, and the header says so rather than implying otherwise:
plaintext in flash, the same guarantee as the compiled-in passphrase it
replaces. Encrypting it needs a key, and there is nowhere on this part to keep
one that an attacker with the flash cannot also read.

---

## 4. Six defects, and what each one was

| # | reported as | what it actually was |
|---|---|---|
| 1 | `join failed` on a board that never tried | one enum carrying three meanings (§8) |
| 2 | "I don't see it doing anything" | the app could not start the radio at all |
| 3 | "the x button isn't working" | it worked; the header painted over it |
| 4 | 90 s of dead input | the bring-up ran on the task that reads the glass |
| 5 | white screen on tap | `wificred.c` in irom, driving the flash bus it executed from |
| 6 | `IllegalInstruction` on tap | **§5** |

Defect 2 is the one worth restating. The view could scan and it could join, and
both of those presuppose a radio that *something else* had already brought up.
It worked on a board where the shell had been used first and was inert on a
board that had just booted. **An app that only works after you have used a
different app is not an app.**

Defect 5 has a rule attached that was learned twice. A `blob_map()` hazard was
raised and then correctly withdrawn — `blob_map()` is IRAM-resident and restores
the cache before returning. But the withdrawal was stated as "flash-resident
callers are fine", when the actual rule is **"code that touches the bus must not
live on it"**. Four steps later a new file went on the wrong side of it. A
correct retraction that leaves the principle unstated is a trap set for later.

---

## 5. The sixth ABI crossing

The crash that took three rounds, and the only one measured rather than argued:

```
LAST FAULT : exception, exccause 0, epc 0x4009d36e  (boot #815)

4009d334 T wpa_sta_connect_impl
4009d364 T g_hs_pmk_ready_reset      <- 0x4009d36e is inside this
4009d370 T wpa_hs_derive_pmk
```

`g_hs_pmk_ready_reset()` is twelve bytes. It lives in `wifi_glue.c`, compiled
**windowed**. `wifi_join_ssid()`, which called it, is **call0**. A call0 call
into windowed code reaches a `retw` that pops a register window nobody pushed:
it returned onto `a0 = 0x30` and executed it.

The next statement in the same block crosses the same boundary correctly:

```c
(void)g_hs_pmk_ready_reset();                              /* direct  */
(void)blob_call((uint32_t)&wpa_hs_derive_pmk, 0,0,0,0);    /* bridged */
```

**Sixth time this project has paid for that crossing, and the first where the
correct form was one line below the wrong one.**

Fixed by not making the call. The function existed to zero one word, and a
memory write has no calling convention: the flag is `extern` now and the call0
side assigns it. Verified in the binary — symbol gone, `g_hs_pmk_ready` is `B`
at 0x3ffc8a60.

### 5.1 Found by the instrument, not by reasoning

Two earlier white screens in this arc were explained confidently and never
measured, and both explanations are now doubtful. This one was named because the
panic handler writes the fault to the persistence record and the **next boot
prints it** — so it survived a reboot, a replug and a reflash, and `nm` turned
an address into a function name.

Both theories carried into that dump — the flash cache, and two contexts in the
blob — were wrong. `epc` was in IRAM. **A confident mechanism attached to a
symptom is worth nothing beside one address.**

---

## 6. The page came back by deleting a line

The board reported `ivory-billed 192.168.1.140` in green and answered nothing.
Measured from a second machine on the same subnet:

```
ping 192.168.1.140     no reply
arp -a 192.168.1.140   No ARP Entries Found
HTTP                   timed out
```

**No ARP entry** is the diagnostic: not an HTTP problem, not on the air at all.
Meanwhile the board was healthy — shell responsive, tick climbing, no reset.

`start_radio()` ended by starting a thirteen-channel sweep unconditionally,
*including immediately after a successful join*. A station has one radio. A
passive sweep retunes it off the access point for 400 ms per channel — five
seconds across the band — and it was doing that seconds after associating and
binding an address, to a user who had not asked for it. The comment justifying
the line reads like a courtesy: *"so what appears next is a set of networks to
choose from."*

Deleting it brought the page back:

```
HTTP 200   350 bytes in 625 ms
Reply from 192.168.1.140: time=86ms TTL=255    0% loss
arp -a     192.168.1.140   5c-01-3b-50-3f-64   dynamic
```

**The stack had been working the whole time.** Six real defects were found and
fixed in the steps leading to this point, and none of them was why the page did
not load.

---

## 7. Method

### 7.1 The instrument moved to the glass

The serial link voided five capture runs in one session — `GetOverlappedResult:
Access is denied` followed by re-enumeration, once with the radio completely
idle. A link that fails four times in five is not an instrument.

So the board reports from the screen. The scan says `scanning ch 7 -- 2 found`.
An empty list says *why* it is empty. A finished sweep says `13 ch, 11 refused`,
because a channel the driver refused and a channel that was quiet are opposite
findings that used to print identically. A saved network carries a dot. Every
one of those was added because a serial capture could not be relied upon to
exist.

### 7.2 An instrument that destroyed the run it measured

The capture script reopened the port after a link glitch. **Opening the port
resets the board.** So a glitch during a ninety-second bring-up rebooted it
half-way and logged a clean boot banner — which reads as a spontaneous reset.

Caught by the tick counter: `t=307963` before the drop, `t=4089` after.
Deasserting DTR and RTS before open is *not* sufficient, and that was measured
rather than assumed:

```
open #1 last tick 2193
open #2 last tick 37      -> RESET across reopen
```

The port is opened once now, and a drop is declared `RUN IS VOID` rather than
quietly rebooting the thing under test. This is the third instrument in this
project to share a dependency with what it measures (051 §10.1, 053 §6.1) and
the first to *cause* the fault rather than merely miss it.

### 7.3 A debt, recorded as one

`keyboard.c` exists; `term.c` and `notes.c` still carry their own copies. That is
**three** copies where there were two — worse, deliberately, and written down as
owed rather than left to be discovered. Moving two working apps onto a module
that had never run would have put three apps at risk on one untested change.

---

## 8. The finding: four working systems, reported as broken

| what the user saw | what was true | what the interface said |
|---|---|---|
| `join failed` | no radio had ever been started | a join was attempted and refused |
| no networks, three times | the radio was off after a flash | `no networks -- tap scan` |
| green `192.168.1.140` | the link was gone | an address, in green |
| no password prompt | the passphrase was already saved | nothing at all |

Not one of these was a fault in what the code did. **All four were faults in
what it claimed.** And each cost real time: the second produced three rounds of
hunting a scan bug that never existed, and the first pointed at the passphrase,
the access point and the handshake — everywhere except the radio that was
switched off.

The same shape appears on the reporting side. `dhcp offer/ack 0/0` was quoted
twice as evidence DHCP had failed. Those counters belong to the hand-written
network path and are **zero by construction** when lwIP owns the stack, which
`net.c` warns about a few lines above the code that prints them. The evidence
was retracted and the conclusion was kept for three more steps — and the
conclusion was wrong too; DHCP had bound. **A conclusion that survives the
retraction of its evidence is still a guess.**

The five status enums, the self-explaining empty list, the refused-channel
count, the link-lost check taken against a baseline rather than against zero,
and the green dot are all one correction: **a system that cannot say what it is
doing will be debugged as though it were doing something else.**

---

## 9. What is on the network

```
ssid       ivory-billed         WPA2-PSK / CCMP
station    5c:01:3b:50:3f:64
address    192.168.1.140        DHCP, bound
service    HTTP on port 80      200, 350 bytes, 625 ms
reached    from 192.168.1.102, ping 0% loss, ARP resolved
launched   by two taps on a touchscreen
```

---

## 10. What remains

1. **Register-window ownership** (steps 49–141). `multiframe` counts 21,026
   switch-outs with more than one live frame. Step 291's `LOST 0x00000002` and
   `GRANT DRIFT` were real readings; §5 showed they were the *consequence* of a
   bad `retw` on that occasion, which is not the same as showing the accounting
   is sound.
2. **The net task carries every blocking job in the view** — bring-up, join and
   sweep — and is the tightest stack in the kernel at 664 of 2048 bytes free.
   `TASK_STACK_WORDS` is one size for every task.
3. **`term.c` and `notes.c` onto `keyboard.c`** (§7.3).
4. **The USB link**, which is host-side and cost five runs.
5. **Touch calibration** reads defaults and the panel saturates at 4095.
6. Group-key rekeying, roaming, PMKSA caching, WPA3/SAE, and the all-channel
   scan that has panicked since step 202.

**A from-scratch operating system, on a radio whose interface it reverse-
engineered, over a link it encrypted itself, joined to a network chosen from a
list and unlocked with a passphrase typed on the glass — serving a page to a
browser that knows none of it.**
