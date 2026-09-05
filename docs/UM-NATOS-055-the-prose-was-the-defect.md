# UM-NATOS-055 — The Prose Was the Defect

**Used Medias LLC — Embedded Systems Division**
Revision 1.0 · 2026-09-05 · Status: **nat-os fetches a page from the internet. Three of the faults that stood in the way were comments, not code.**

---

## 1. Abstract

```
tap the wifi icon   -> radio up, networks listed, tap to join
tap the web icon    -> go
                       dns query to gw, try 1
                       dns reply, bytes 62
                       resolved -- connecting
                       request sent
                       recv cb, bytes 1400
                    -> HTTP/1.0 301 Moved Permanently, on a 240x320 screen
```

A DNS query built byte by byte, an answer parsed out of a real UDP packet, a TCP
connection to Google, an HTTP request, and a response rendered — over a link
whose WPA2 handshake this project wrote, on a radio whose interface it
reverse-engineered from a disassembly.

This report covers `next_moves/08` steps 297–334. UM-NATOS-054 covers 277–296.

The engineering is §3–§5. **The finding is §6: three separate faults in this
stretch were sentences that outlived their subject.** In every case the code was
correct and had been for a long time; the prose describing it had not been
revisited when the thing it described changed, and it was believed — by the
author of this report, repeatedly, while hunting the symptom it caused.

---

## 2. State before and after

| | after 054 (step 296) | now (step 334) |
|---|---|---|
| the web | nothing | **DNS, TCP, HTTP, a page on screen** |
| receive ring | 512-byte slots | **1600, three of them (§4)** |
| wifi bring-up | ~90 s | **~10 s** (§5.1) |
| PBKDF2 per join | every time, ~15 s | **once ever, cached** |
| blocking work | three hand-rolled queues | **one job model** |
| the wifi view | start button, scan button, six status phrases | **open it; it lists networks** |
| register-window "fault" | the top open item | **a stale comment (§6.1)** |

---

## 3. A browser, and what it honestly is

`browser.c` + `webfetch.c`: a URL bar, a keyboard, and a fetch. **Not** a
rendering engine — there is no HTML parser, no layout, no CSS, and no TLS.

TLS is not a matter of effort. A TLS 1.2 handshake needs X.509 parsing, RSA or
ECDSA verification, ECDHE, AES-GCM and a root store; mbedTLS wants 40–50 KB of
heap for one handshake and this board's **entire heap is 32 KB**. Pointed at an
HTTPS-only host it shows that host's redirect, which is a real answer from a
real server and is displayed as what it is.

It carries **its own DNS resolver**. `lwipopts.h` has `LWIP_DNS 0`, annotated
*"needs str\* this kernel does not have"*; one A-record question over raw UDP is
a header, a name in length-prefixed labels, and four bytes of answer — shorter
than the string shim would have been. It asks **the gateway**, not a public
resolver, which keeps the board off any name server the user did not already
choose by joining their network.

---

## 4. The 512-byte ceiling

The fetch reached `request sent` and stopped. Twelve seconds later,
`tcp error 4294967283` — **−13, `ERR_ABRT`**, this code's own `tcp_abort()`
after its own timeout. So the connection established, the request went out, and
nothing came back.

The request was cleared without touching the board: those exact 75 bytes, sent
from a desktop, get a 301 from Google immediately.

```c
#define NET_MAX   512u
if (len > NET_MAX) { g_net_truncated++; }
uint32_t n = len < NET_MAX ? len : NET_MAX;      /* TRUNCATED */
```

A truncated TCP segment fails its checksum and lwIP discards it. The effect is
not a short read — **it is total silence on exactly the traffic that matters.**

### 4.1 Why two hundred steps never hit it

Everything this ring had ever carried was small: a DHCP offer, an ARP reply, an
ICMP echo, a DNS answer — and the HTTP **requests** arriving at nat-os's own web
server, which are a couple of hundred bytes. Every packet in this project's
history has been inbound-small or outbound.

**A reply from a web server is the first packet ever to exceed 512 bytes.** The
ceiling was correct for every workload that had existed until the moment the
board became a client instead of a server.

`g_net_truncated` was incrementing the whole time. Its comment reads *"a
silently shortened frame is how the DHCP bug hid"* — the instrument for this was
in place, and nothing read it.

### 4.2 And the fix broke the radio

Six 1600-byte slots is 9.6 KB of static DRAM, and static DRAM comes out of the
heap the WiFi driver allocates from:

```
before  heap : 38648 B usable
after   heap : 27320 B usable      11 KB gone; the wifi view found no networks
now     heap : 32120 B usable      three slots, not six
```

Three full-size slots cost 1.8 KB more than six small ones, against eleven. A
burst deeper than three drops and `g_net_dropped` counts it, which is a visible
cost rather than a silent one. **Holding one whole frame matters more than
holding six halves of one.**

The boot banner prints the heap on every boot. It went unread until the radio
stopped working.

---

## 5. The wifi view, and five reports of one bug

"It only works when I close it and reopen it," reported five times. Four fixes
were made, **all four were real defects, and none was the cause**: thirteen full
repaints per sweep (302), a dirty flag raced across two tasks (303), a held
press — which *introduced* the symptom it was meant to fix (305/310) — and the
view not knowing a bring-up was running (307).

The cause was **two implementations of one decision**. `wifiapp_open()` decided
the view's state and whether to sweep; `start_radio()` decided the same things
again in its own code. Reopening re-ran the first with the radio finally up; the
automatic path ran the second, and they had drifted. Steps 307 and 309 were both
edits to the copy, made without noticing there was one.

The user supplied the diagnosis twice in plain words — *"the first attempt should
never have been attempted"*, then *"use the functionality of closing and
reopening, but on the first time"* — and both were read as symptom reports.

### 5.1 Waits disguised as work

Three separate times, removing time exposed something that had been holding
together by accident:

- a **60-second poll** (304) that had also been standing in for the DHCP bind
- **15 seconds of PBKDF2** (315, cached) that had been standing in for an
  unwaited association — `join()` tested `wifi_joined()` on the line after
  starting the connect, and passed only because the derivation sat between them
- a **crypto self-test** (326) whose minute had been absorbing the difference

**An accidental delay is not a wait**, and code that depends on one is correct by
coincidence. The fastest way to find out is to make the system faster.

---

## 6. The finding: prose that outlived its subject

### 6.1 "Zero is the design; anything else is the bug"

`multiframe: 49065` was quoted in two panic dumps and taken, by this author, as
49,065 violations of the invariant the scheduler rests on. It was raised to the
user as the most serious open item in the project.

Tier B (step 131, proven 162) replaced the two-word window save with a **full
register file per task** — `vectors.S:943`. Windowed frames survive preemption,
so the pin that forced one-frame switch-outs was **deliberately disabled** at
step 163. A non-zero count is the current design operating normally.

`LOST` and `GRANT DRIFT` compare against `(1 << base) | union`, the grant a
two-word save could offer. Tier B does not restore that way. **All three
counters are pre-Tier-B instruments reporting the current design as a fault**,
and only one of them is labelled as what it measures.

Cost: an afternoon, and a recommendation to the user that turned out to be about
nothing.

### 6.2 "Runs once, costs a few milliseconds"

The crypto self-test ran on **every** bring-up and verifies PBKDF2 against
published vectors — 4096 rounds per vector, ~15 s each. The sentence was true
when the self-test covered SHA-1 and AES; the PBKDF2 vectors came later. It was
read past **twice in one session while hunting the wait it caused.**

### 6.3 The 512-byte assumption, never written down

`NET_MAX 512` was right for every workload that existed when it was chosen. The
assumption — *this board receives small packets* — was never stated, so there
was nothing to revisit when the board became a client.

**A silent assumption is a comment that was never written.** It fails the same
way and gives you less to find.

### 6.4 What the three have in common

In every case **the code was correct**. In every case the description was
consulted, believed, and acted on. And in every case the person misled had the
means to check in seconds — `git log`, a grep, a boot banner already on screen.

The lesson is not "write better comments". It is that **a comment is a claim
about the system, and claims decay**. This project's habit of explaining *why* in
prose is the reason it is debuggable at all; §6.1's chain of four files read like
a proof. The same habit is what made three stale sentences load-bearing.

---

## 7. Method: the instrument moved to the glass

The serial link voided **six** capture runs in this stretch, needs a machine
attached, and **resets the board when opened** — which drops the very connection
under test.

So the diagnostics went on the screen. An eleven-line log in the space the
content will occupy:

```
view opened            go tapped
no radio -- starting   starting fetch
mapping the blob       dns query to gw, try 1
loading the driver     dns reply, bytes 62
starting the wifi drv  resolved -- connecting
  esp_wifi_init        request sent
  crypto self-test     recv cb, bytes 1400
```

**Every fault in the second half of this report was found in one report each**,
by the user reading the last line. `crypto self-test` (326), `driver started`
then silence (327), `request sent` then `tcp error` (332). None needed a capture,
a reset, or a theory.

Against a working system `looking for networks...` is the right thing to say.
Against a broken one it is the worst possible message: it covers blob mapping,
driver load, PHY init, a bring-up and a thirteen-channel sweep with one sentence
that changes for none of them.

**The user is the only instrument that is always attached.**

---

## 8. What is on the network

```
ssid       ivory-billed          WPA2-PSK / CCMP, passphrase typed on the device
station    5c:01:3b:50:3f:64
address    192.168.1.140         DHCP
serving    HTTP on port 80       200, confirmed from another machine
fetching   google.com            301, over DNS + TCP written for this board
heap       32120 B
```

---

## 9. What remains

1. **The step-319 panic** — `StoreProhibited`, `excvaddr 0`, on scanning while
   joined. Its `GRANT DRIFT` and `LOST` lines were §6.1 noise, so it now has **no
   explanation** rather than a wrong one. Leaving the network before scanning
   avoids it; avoidance is not a fix.
2. **Both memories are tight.** iram could not accept 20 bytes of kernel
   instrumentation (328); DRAM is 6.5 KB below where it started.
3. **The debug scaffolding** — the `WA` traces and `SWEEP` timing can go. The
   on-screen log should stay; it earned its place four times.
4. `term.c` and `notes.c` onto `keyboard.c` (054 §7.3), still owed.
5. The data-path restructure (313b), touch calibration, and the older list:
   group-key rekeying, roaming, PMKSA caching, WPA3/SAE.

**A from-scratch operating system that joins a network chosen from a list,
unlocked with a password typed on its own screen, and fetches a page from the
internet over a stack it wrote itself.**
