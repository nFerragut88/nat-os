# UM-NATOS-050 — A Real Stack

**Used Medias LLC — Embedded Systems Division**
Revision 1.0 · 2026-08-24 · Status: **lwIP runs on nat-os. DHCP binds, ping is answered, and a browser renders a page served over TCP from port 80.**

---

## 1. Abstract

nat-os serves a web page.

A browser on another machine completed a three-way handshake with this kernel,
sent an HTTP request, and rendered the response. That is every layer at once —
802.11 association, Ethernet, ARP, IPv4, TCP, HTTP — over a Wi-Fi driver this
project reverse-engineered the interface to, on an operating system with no
libc.

Three steps got there:

- **The DHCP bug**, which was a ring buffer sized for a ping and never checked
  against the other protocol in the same file. §3.
- **lwIP**, vendored and ported in `NO_SYS=1` mode, where the port layer
  reduces to `sys_now()` and a netif driver. §4–§6.
- **A TCP listener**, and a browser as the instrument. §7.

This report covers `next_moves/08` steps 231–235. UM-NATOS-049 covers 217–230.

---

## 2. State before and after

| | after 049 (step 230) | now (step 235) |
|---|---|---|
| DHCP | OFFER parsed, **ACK never received** | **bound**, by lwIP's own client |
| addressing | hardcoded static | **DHCP-assigned** |
| ARP / ICMP | ~300 hand-written lines | **lwIP** |
| IP fragment reassembly | none | lwIP |
| **TCP** | none | **listening on :80, serving HTTP** |
| UDP | DHCP client only | lwIP |
| network stack | none | **lwIP 2.1.3, 23 files vendored** |

```
boot   11 PASS 0 FAIL      wintorture 10 real switches, checksum CORRECT
lwip   netif up, mtu 1500, starting DHCP
lwip   DHCP bound -- address 10.224.203.139
http   listening on port 80
       lwip rx 150 drop 0  tx 123 err 0        (110 s of ping, no loss)
       http conns 4 reqs 4 errs 0              (a browser)
```

---

## 3. The DHCP bug: a ring sized for the wrong protocol

`NET_MAX` was 160 bytes, chosen because a ping is 74 bytes from Windows and 98
from Linux. It was never checked against the *other* protocol in the same file.

DHCP options begin at `14 + 20 + 8 + 240 = 282` bytes into the frame. The OFFER
measured 352. **Every reply was truncated before its options.** `dhcp_opt` then
scanned a region that did not exist — `while (i + 1 < len)` with `i = 282` and
`len = 160` never executes — found no message-type option, and the frame was
discarded having arrived perfectly intact.

**How it presented is why it took two passes to find.** The counter read
`dhcp 0/0`: not "the OFFER was rejected", which points here, but "no OFFER was
ever seen", which points at the receive or transmit path. Step 230 shipped a
hardcoded address to work around it.

Truncation is now counted alongside the ring-full drop count. A silently
shortened frame is the shape this project keeps finding: the operation appears
to succeed, the data is wrong, and nothing says so.

---

## 4. Choosing NO_SYS = 1

lwIP has two modes and the choice determines the whole shape of the port.

`NO_SYS=0` provides the BSD sockets API and requires `sys_arch.c`: mutexes,
semaphores, mailboxes and threads, implemented on nat-os's scheduler.

`NO_SYS=1` is bare-metal. No threading abstraction at all, no sockets, no
netconn — and the port layer reduces to **two things**: a millisecond clock and
a netif driver. It still provides IPv4, ARP, ICMP, UDP, TCP and a DHCP client,
which is everything needed to prove the stack works.

The second was chosen. Sockets prove nothing the raw API does not, and the
threading surface is where a port of this kind goes wrong.

---

## 5. The port layer is three things, and two were not predictable

**`sys_now()`** — from `timer_ticks() × 10 ms`. Exact rather than approximate.

**A netif driver** — `linkoutput` flattens a pbuf chain into
`esp_wifi_internal_tx`; the receive ring that already existed for the
hand-written path feeds `ethernet_input`. The driver's callback is *windowed*
and runs on its own task, so it copies and returns; lwIP is entered from
exactly one context, which `NO_SYS=1` requires.

**`sys_check_timeouts()`** from the poll loop. Not housekeeping: without it DHCP
never retries and TCP never recovers a lost segment.

Then two things no amount of reading lwIP would have predicted:

### 5.1 ESP-IDF's lwIP is patched

`opt.h` references `ESP_LWIP_DHCP_FINE_TIMERS_ONDEMAND` and nine siblings as if
they were always defined. A stock `lwipopts.h` does not compile against it. All
are turned **off** — they are optimisations, and off is the behaviour upstream
documents.

IDF's copy was vendored rather than upstream because it is the tree known to
work against this exact Wi-Fi driver.

### 5.2 `_ctype_`

Linking against a `-nostdlib` kernel fails on `_ctype_` — newlib's shared
lookup table — because `isdigit()` is a *macro*, not a function. In lwIP's
source it appears only ever as `isdigit()`, inside `ip4addr_aton`. Nothing about
reading lwIP predicts this symbol; it is a property of the C library it was
last linked against.

The table is generated, not typed, and indexed `_ctype_[c + 1]` so a lookup on
EOF does not read before the array.

---

## 6. The crash, and why the diagnosis took five minutes

The first attempt crashed inside `esp_wifi_init_internal` with `epc 0`.

Not a code fault. lwIP's default `MEM_SIZE` of 16 KB plus a twelve-buffer pbuf
pool put 35 KB into `.bss`, which pushed `_bss_end` up:

```
_heap_start 0x3ffcc808   _heap_end 0x3ffd3000   =  26.6 KB
```

against a Wi-Fi driver whose **measured** peak is 35 KB high-water. lwIP had
taken the memory the driver needs, and an unchecked allocation failure became a
jump to NULL.

The ceiling is not negotiable: `0x3ffd4000` is where the blob's `.data` begins,
and growing into it would *corrupt* the driver rather than starve it. So lwIP
shrank — `MEM_SIZE` 6 KB, pool 6 buffers — and the heap returned to 46 KB.

**That 35 KB figure was recorded at step 203 for no particular reason.** It is
what turned this from a hunt into an arithmetic check. Numbers whose use is not
yet known are worth writing down.

---

## 7. TCP, and a browser as the instrument

TCP was compiled in at step 232 and no byte passed through it until step 235.

A browser is a harsher test than `telnet`: it completes a handshake, sends a
real request, requires a well-formed response *and a clean close*, and renders
nothing if any of that is wrong.

```
http conns 4 reqs 4 errs 0
```

`conns` exceeding `reqs` in earlier samples is the detail worth keeping —
browsers open speculative connections they do not always use, and one request
will have been `/favicon.ico`. Equal counters would have suggested something
simpler than a browser was talking.

**The page carries live numbers** — uptime, lwIP rx/tx, connection and request
counts. Static text would render identically whether or not anything underneath
it worked, and would prove only that some bytes moved.

### 7.1 Three raw-API contracts

- **`tcp_listen` returns a NEW pcb** and frees the one passed to it. Using the
  original afterwards is a use-after-free.
- **The close waits for the ACK.** `tcp_close` straight after `tcp_write` asks
  lwIP to send and shut down in one breath and the response can be lost — which
  presents as a *blank page*, not an error. It closes from `tcp_sent`.
- **`tcp_err` fires after lwIP has already freed the pcb.** That handler only
  counts.

---

## 8. Method

**Make something you do not control answer you.** A phone's Wi-Fi list judged
the beacon. A DHCP server judged the transmit path. `ping` judged ARP and ICMP.
A browser judged TCP. Every one beat a counter incrementing on this board.

**The difference between two failures is data.** `Destination host unreachable`
and `Request timed out` are both failures and mean opposite things about ARP.

**Record numbers before they are needed.** §6.

### 8.1 The instrumentation cost more than the code did

Continuing UM-NATOS-049 §6.1, because the pattern did not stop:

5. **A probe that buffered a human-in-the-loop test.** Three diagnoses were
   delayed and two runs produced nothing at all. It now streams every line as it
   arrives.
6. **An orphaned process holding the serial port.** Killing a background task
   left Python alive on COM5; the next run died on open, and an operator's ping
   went to a board nothing was driving — while this report's author speculated
   about pbuf pools and association drops. A port check now precedes every run.

Both produced confident wrong statements rather than obvious failures, which is
the same property the four in UM-NATOS-049 had. **An instrument that has not
been checked is not an instrument.**

---

## 9. What is on the network

- Associated to an access point, holding the association for minutes.
- A DHCP lease, negotiated by lwIP's client.
- ICMP answered: 123 replies over 110 seconds, `drop 0 err 0`.
- **TCP listening on port 80, serving HTTP to a browser.**

---

## 10. What remains

1. **The WPA supplicant.** Only open networks can be joined. UM-NATOS-049 §3.1
   measures exactly what is missing: the RSN IE for WPA2, SAE for WPA3.
2. **Outbound TCP.** `tcp_connect` is untested — nat-os has never initiated a
   connection, only accepted one.
3. **DNS**, off because it needs `str*` functions this kernel only partly has.
4. **The `w2c_*` save-area overlap**, unchanged since UM-NATOS-045 §8.4.
5. **`kernel/net.c`**, now dead code behind `g_use_lwip = 1`. It earned its
   place by proving the data path and is worth keeping until lwIP has run for
   longer, but it should not stay forever.
6. **Throughput, loss and MTU** are entirely unmeasured. Everything here is
   idle-network behaviour.

---

Full experiment log: `docs/next_moves/08-wifi-via-loaded-blob.md`, steps 231–235.
Companion reports: UM-NATOS-045 (rev 1.0), 046 (rev 1.0), 047 (rev 1.0),
048 (rev 1.4), 049 (rev 1.0).

**nat-os served a web page.**

Written by: Hare
