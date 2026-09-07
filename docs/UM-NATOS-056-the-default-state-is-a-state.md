# UM-NATOS-056 — The Default State Is a State

**Used Medias LLC — Embedded Systems Division**
Revision 1.0 · 2026-09-06 · Status: **nat-os remembers the network you chose and joins it on its own. Every bug in getting there was in the state a new board starts in.**

---

## 1. Abstract

The goal, in the user's words:

> *"the user selects a network and enters a password once, and then it auto
> connects in the future (but the user can disable / enable wifi in the wifi app
> in order to save RAM)"*

Which now works:

```
open wifi     -> connected, network listed
forget        -> leaves it, rescans
reboot        -> joins nothing, scans, asks
pick + type   -> joined, remembered
reboot        -> connected, no password asked
```

This report covers `next_moves/08` steps 341–348. UM-NATOS-055 covers 297–334.

**Six defects were found between the feature working and the feature being
usable, and every one of them was in a state a brand-new board occupies.** The
happy path — a network already chosen, a passphrase already saved — worked from
the first flash. §5 is what that pattern cost and why it kept recurring.

---

## 2. What was built

**`wifiprefs.c`** — the preferred network and an enabled flag, in their own
flash sector at 0x204000. Its own sector for the reason `store.c` taught twice
(054 §3, 055): extending a versioned record discards every existing one, and the
credentials one sector away are passphrases somebody typed on a multi-tap
keyboard.

**Auto-connect.** `wifi_try_connect()` has associated with `WIFI_STA_SSID` since
step 218 — correct for the board this was developed on, wrong for anybody
else's. It now prefers the network the user chose. Choosing **is** preferring:
a successful join records it, so there is no second confirmation to forget to
give.

**An `ON`/`OFF` switch**, persisted.

---

## 3. What "off" actually saves

Stated plainly, because the honest answer is narrower than the feature sounds.
The blob entry table has `esp_wifi_init`, `_start`, `_connect` and
`_disconnect`, and **no `_stop` and no `_deinit`**. The radio can be told to
leave a network; it cannot be told to give its memory back.

| | |
|---|---|
| **off at boot** | the driver is never initialised, so it never allocates. Heap returns to ~38 KB and the blob's 32 KB DRAM window is never populated. This is the whole saving and it is real. |
| **off while running** | the station disconnects and nothing auto-joins, but the driver's allocations return at the next boot. The screen says `wifi off (memory returns on reboot)`. |

A feature that quietly under-delivers is the failure mode this project has spent
two reports on. The header says this, the screen says this, and this report says
this.

---

## 4. Six defects, and what each one was

| # | reported as | what it was |
|---|---|---|
| 342 | *"no option to forget"* | joined ⇒ no scan ⇒ empty list ⇒ nothing selectable |
| 343 | *"still no networks listed"* | the row came from the preference or `g_joined`, **both empty on an unconfigured board** |
| 344 | *"I can still access the internet"* | `forget` removed the key and left the association running |
| 346 | *"still connected somehow"* | `wifi_leave()` had a silent `return` if the blob entry was missing — and cleared the connected flag anyway |
| 347 | *"forget doesn't survive a reboot"* | the compiled-in credentials rejoined the forgotten network from the binary |
| 348 | *"no forget button when selected"* | the button required a **saved passphrase**, which an auto-connecting board does not have |

### 4.1 The deadlock in 348

The last one is the sharpest. A board auto-connecting from `wifi_secrets.h` has
no stored passphrase, so no `forget` button appeared; without the button
`chosen` could never be set; without `chosen` the binary's credentials kept
rejoining. **The most ordinary board there is could not be reconfigured at all**,
and the path was closed by a condition that reads perfectly reasonably:
`wificred_has(ssid)`.

What the user forgets is the **connection**. The passphrase is a detail of how
it was made, and on that board there never was one.

### 4.2 The one that was worst

`wifi_leave()`:

```c
if (!e || !blob_ready() || !e->wifi_disconnect) { return; }   /* silent */
...
g_wpa_conn_cb = 0;                                            /* regardless */
```

If that entry is unpopulated, leaving a network is a no-op **indistinguishable
from leaving a network** — and the connected flag was cleared either way, so the
view reported a disconnect that may never have happened. Caught the only way it
could be: the user browsed the internet on a network the board said it had left.

That is the **seventh** time this project has found a status reporting an
outcome for work that never ran. This one had the proof one line above it.

---

## 5. The finding: the default state is a state

Every defect above lives in a state a **new** board occupies, and none was
reachable once the feature had been used:

- no preference stored yet (343, 347)
- no passphrase stored yet (348)
- connected, therefore not scanning (342)
- connected without ever having been told to (343, 348)

The happy path — choose, type, join, reboot, reconnect — worked from the first
flash. Everything that broke was **before the user had done anything**, which is
where every user starts and where a feature is judged.

The same shape appeared twice in UM-NATOS-054: the credential store was
write-only until `forget` existed (which is also why the risky save path went
eleven steps unmeasured), and the empty list could not say why it was empty. It
recurs because **testing a feature means using it**, and using it moves the
system out of exactly the state that needs testing.

**A default is not the absence of configuration. It is a configuration, and it
is the only one every user is guaranteed to see.**

### 5.1 And a smaller one, worth its own line

The `ON`/`OFF` switch went where the old `scan`/`start` button had been. The
first thing that happened was a tap out of habit, wifi off, persisted to flash,
and a report of no networks. New controls at familiar coordinates inherit the
old control's muscle memory.

---

## 6. Method: verify before theorising

When the list stayed empty the user proposed the obvious explanation — *"maybe
the board has an older version"* — and it was worth one command:

```
esptool verify_flash 0x10000 build/natos.bin
-- verify OK (digest matched)
```

Byte-for-byte identical. That closed a whole branch of investigation in seconds
and sent the next question back to the logic, where the fault was.

Three of the six defects were named by the on-screen log (UM-NATOS-055 §7)
without a capture, a reset or a theory. The serial port is used here for one
thing now: verifying that the flash contains what was built.

---

## 7. What remains

1. **The step-319 panic** — `StoreProhibited`, `excvaddr 0`, scanning while
   joined. Avoided, not explained.
2. **The bring-up still associates before the user has chosen**, using an
   impossible SSID when there is nothing to join (347). The correct structure is
   313b: bring-up brings up a radio; joining is the join's job. That is a
   restructure of `wifi_bringup()` and `join()`, and skipping the association
   without it crashes the board — measured.
3. **`wifi_leave()` may be a no-op** if the blob's `_disconnect` entry is
   unpopulated. It now reports which; the reading has not been taken.
4. Both memories remain tight; `term.c` and `notes.c` still owe their migration
   onto `keyboard.c`.

**A board you take out of the box, hand a password once, and never think about
again — on an operating system, a radio driver interface, a WPA2 supplicant, a
DNS resolver and a TCP client that were all written from scratch.**
