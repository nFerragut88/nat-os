# UM-NATOS-061 — The Language Reaches the Network

**Used Medias LLC — Embedded Systems Division**
Revision 1.0 · 2026-09-08 · Status: **A program written in NatScript fetches a web page. The compiler did not change.**

---

## 1. Abstract

```
   netdev    fetch example.com/  started
   netdev    done
  [fetch] asking for example.com/
  [fetch] HTTP 200, 767 bytes
  [fetch] done
```

This report covers `next_moves/08` steps **386–387**. UM-NATOS-060 covers
372–385.

Two things this project built in separate arcs — a language people can write
applications in, and a network stack that associates, completes a WPA2
handshake and speaks HTTP — had never met. A NatScript program could read a
sensor, draw on the panel and feel a finger, and had no way whatever to reach
the network.

It has one now, and **adding it cost one row in a table**. §3 is why that is the
interesting part.

---

## 2. `cached=0`, and the end of a four-step argument

Step 374 moved the WPA crypto to flash and opened a question it could not close:
PBKDF2 only runs on a **cache miss**, every successful join had used a cached
PMK, and the four thousand rounds had never executed from irom.

The chain of attempts is worth recording because the obstacle was not technical:

| | |
|---|---|
| 377 | inferred correctness from a green IP |
| 380c | verified SHA-1, HMAC-SHA1 and AES unwrap directly, by `micbad=0` |
| 383a | argued PBKDF2 from *"the network was forgotten, and forget calls `pmkcache_forget`"* — sound, and not a reading |
| 383b | added `cached=` to the report; the next join read `cached=1`, because the derivation had already re-populated the cache. **The instrument arrived one join too late to witness the thing it was added for** |
| **386** | `4way pmk=1 step=6 cached=0 rx=2 m1=1 m3=1 done=1 micbad=0` |

**`cached=0` means derived, not fetched.** `micbad=0` with `m3=1` means the
derived key was correct — a wrong PMK gives a wrong PTK, a wrong MIC on message
three, and a handshake that stops there.

Every WPA primitive in this system is now *observed* executing from flash:
PBKDF2, SHA-1, HMAC-SHA1, AES key unwrap.

### 2.1 The obstacle was a keyboard

A derivation needs a cache miss, a cache miss needs a forgotten network, and the
join that follows needs a passphrase typed on a multi-tap keyboard. **That is
the one step nothing in this system can automate**, and asking for it produced
three empty capture windows across two days.

`wifijoin` closes it: a shell command that joins with the credentials already in
`wifi_secrets.h` — gitignored, and a network this board has used. The SSID is
echoed because a log without it is useless; the passphrase is not printed. It
deliberately does **not** touch `wifiprefs`: a diagnostic join is not a choice
the user made, and step 347 is what happens when those are confused.

---

## 3. The network is a device

`device.h` has carried one sentence since the table was written:

> *"Anything new is a device.h table entry reached through `sys device`, not a
> thirteenth mnemonic here and a fourteenth case in vm.c."*

That was written as a rule about **syscall discipline**. Step 387 is the first
time it paid a dividend nobody had predicted.

`net` is a row in `DEVICES[]`. The consequence:

**NatScript needed no change at all.** `permissions { net }` makes the name
available — the manifest of step 356 and the by-name resolution of 358 — and
`net.read`, `net.xfer_out` and `net.xfer_in` are the generic device methods the
compiler already emitted for `light`, `store` and `echo`. The board reported
`started id=0 perms=net` on the very first run.

| | |
|---|---|
| `net.xfer_out(0, request, len)` | `"host/path"` — starts a fetch |
| `net.read(0)` | state: 0 idle, 1 resolving, 2 connecting, 3 requesting, 4 done, 5 failed |
| `net.read(1)` / `net.read(2)` | HTTP code, body length |
| `net.xfer_in(block, buf, 64)` | the body, `DEVICE_XFER_MAX` at a time |

Appended to the table, never inserted. Ids are positional there; everything
outside resolves by name **only because nothing has ever moved**.

### 3.1 What a good abstraction is worth

The measure of the device model is not that it made the network *possible* from
NatScript. It is that the network arrived in a programming language without the
language's author being consulted.

Permissions, per-caller grants, bounds-checked transfers across a bounce buffer,
resolution by name, and a refusal path that reports rather than faults — a new
capability inherits all of it by being a row rather than a special case.

### 3.2 The one piece of real design

`webfetch.h` requires the **net task**: the raw lwIP API is not thread safe and
`NO_SYS=1` means there is no lock to make it so. A device callback runs on the
**caller's** task, which for a VM program is the app host.

So `netdev_xfer_out()` records the request and `netdev_service()` picks it up
from the net task's own loop — the same handoff `job.h` performs, for the same
reason.

---

## 4. Three wrong versions, each silent in a different way

| symptom | cause |
|---|---|
| `gave up in state 0`, twice | `netdev_service()` was hooked into `net_poll_for()`'s loop only. That owns the receive ring for sixty seconds and then **hands over** to the net task, which calls a different function. A fetch worked for a minute after boot and never again |
| `state 0`, with the service now in both loops | `wifijoin` associated and never called `wifi_data_path_start()`. Step 350 split those deliberately — a radio is not a connection — so there was no netif, no DHCP, no handover, and the net task never took the ring |
| `gave up in state 3 after 600 polls` | **not a failure.** Each poll is a `DEV_F_SLOW` read that ends the program's slice, so six hundred is a second or two of wall clock. The request had gone out and the reply had not arrived |

The first two printed nothing at all. They were found by adding one line that
says what `netdev_service()` picked up and what `webfetch_start()` returned —
which is the correction UM-NATOS-060 §7 prescribes, applied to code written
after that report.

The third was found by reading the *number* rather than the words "gave up". A
timeout that is really a deadline set too short looks exactly like a failure,
and only the state distinguishes them.

### 4.1 The seventh instrument

A failed fetch reported `5` to the program and nothing else.
`webfetch_status()` has always returned *"a short line naming what happened"*,
and nothing outside the browser view had ever read it.

It is printed on completion now. The successful run ends `netdev done`; a
failure will name which of DNS, connect or the request it was.

That is the seventh in the series UM-NATOS-060 catalogued — and the first one
found in code this project wrote **after** publishing the report about them.
The pattern is not a historical curiosity being cleaned up; it is a mistake this
codebase is still making, in new code, and the correction has to be a habit
rather than a sweep.

---

## 5. What remains

1. **`app_fetch` reads four blocks and stops.** The page is longer than 256
   bytes and the program does not say so — a truncation that is not counted,
   which is the same family as everything in §4.
2. **VM-08 proper.** Image identity needs secure boot and a key in eFuses,
   one-time programmable, deliberately not done.
3. **`APP_MAX` is 4**, pinned by screen geometry rather than memory.
4. **Per-task stack sizing** — 8–10 KB, and a snapshot cannot bound it safely.
5. **The null-`sp` window fault** — identified at 351, never reproduced.

**An operating system, a language, a compiler, a WPA2 supplicant and a TCP
client, all written from scratch — and a program somebody could write in an
afternoon that asks the internet a question and gets an answer.**
