# UM-NATOS-062 — The Page on the Glass

**Used Medias LLC — Embedded Systems Division**
Revision 1.0 · 2026-09-13 · Status: **The board fetched a page off the internet and drew it as words. A panic that had blocked every attempt turned out to be the vendor blob eating the heap.**

---

## 1. Abstract

```
   web      mode=page  at=example.com/  sel=none  text=142B  links=1
     0 |Example Domain|
     1 |This domain is for use in|
     2 |documentation examples without needing|
     3 |permission. Avoid use in operations.|
     4 |[Learn more]|
     link 0 -> https://iana.org/domains/example
```

This report covers `next_moves/08` steps **388–393**. UM-NATOS-061 covers
386–387.

Step 387 fetched a page. It could not *read* one: the web view showed the raw
response — status line, headers, doctype, and whatever markup fitted in eight
lines. This report is about closing that, and about the three defects found on
the way, each of which was invisible to the instrument that should have caught
it.

The last of them had been stopping every radio bring-up on this board for
sixteen steps, and had been attributed to the power supply.

---

## 2. The 767 that was never a page

UM-NATOS-061 §5 filed one open item as the smallest: *"`app_fetch` reads four
blocks and stops."* True, and the shallower half.

`webfetch.c`'s receive loop stops at `WEB_BODY_MAX` and drops the rest of every
pbuf, while `tcp_recved()` acknowledges those bytes to the server regardless. So
`webfetch_len()` returned 767 and `webfetch_status()` said `"done"`.

**The 767 bytes that step 387 published was the ceiling, not the page.** There
was no reading anywhere in the system that distinguished a complete 767-byte
page from the first 767 bytes of a longer one.

That is the sixth instrument in this log to know something it did not say, and
the second found in code written *after* UM-NATOS-060 catalogued the pattern.
The truncation is deliberate and documented at the definition. **The silence
about it was not.**

`g_dropped` counts `q->len - i` per pbuf, `"done"` becomes `"done (truncated)"`,
and the `net` device gained a fourth read channel to carry the number to
programs.

### 2.1 A third truncation, in the printing

```
buf chunk[64]
if net.xfer_in(block, chunk, 64) { puts(chunk) }
```

`VM_SYS_PUTS` walks until a NUL or the end of the arena. A block that comes back
**full** puts 64 bytes into a 64-byte buffer and leaves no NUL, so `puts` ran off
the end and printed whatever lay next. Bounds-checked, so not a fault — but the
program was printing bytes the fetch never delivered.

A host harness driving the *real* compiled program showed exactly what: 256 bytes
transferred, **268 printed**, and the twelve extra were `'NOPQRSTUVnet'` — stale
arena bytes followed by the literal **`net`**, the device-name string the program
resolves itself by. `buf chunk[68]` fixes it for one byte.

---

## 3. Twenty-one bytes of stack

388's reporting added five string literals. `app_fetch`'s image went from **2,180
bytes to 3,039** — against an arena of **3,072**.

`vm.c:180` starts r15 at the top of the arena and grows it *down into the image*.
The change shipped with **33 bytes of stack** against a deepest excursion of 12.
It fitted, by 21 bytes, and every test passed *because* it fitted.

**Nothing would have caught it.** `app_start()` checks `len > arena_bytes` and
nothing else. A stack running past the image writes *inside* the arena, so the
VM's bounds check passes and the damage lands in the string literals — a garbled
message, not a fault.

The first measurement missed it too, and how is worth keeping: the program gave
byte-identical output at an arena of 3,039 — zero headroom — which read as *"the
stack is never used"*. It is used. The literal it corrupts at that size is
`"  [fetch] done -- the whole body"`, and the truncated test case never prints
it. **A test whose blind spot was exactly the string the failure destroys.**

### 3.1 The third number

kmain.c has said it since step 364: *"Arena sizes in this table are hand-written
and unchecked against the image the compiler produced. natc knows both numbers;
nothing carries them across."*

It is three numbers. The arena is hand-written; the image size comes from vasm;
**the deepest the stack goes was computed by nobody** — and it is the one that
decides whether the other two are safe.

`Assembler.stack_depth()` now computes it and `emit_header` writes
`VM_APP_FETCH_STACK 12u`. `tools/arenacheck.py` reads all three and fails the
build, wired in after codegen and before the compiler. Proved by putting the bug
back: at 3,072 it reports `TIGHT`, at 3,044 `OVERRUNS THE IMAGE`, and both stop
`build.ps1` before `== compiling ==`.

---

## 4. A reader, not a rendering engine

`kernel/html.c` turns the response into text and links in one pass: headers
skipped at the blank line, tags removed, block tags to breaks, `<script>` and
`<style>` contents discarded *and skipped in the source* — their bodies contain
`<` and would be read as markup — entities decoded, and `<a href>` recorded with
the byte range of its text. Anything that will not fit is **counted**.

The view word-wraps it, draws each row in runs of one colour so a link looks like
one, and remembers where every visible row began so a tap maps back to an offset.
**Two taps to follow**, because this panel's calibration puts the reported point
some way from the finger and a single tap that navigated would send the reader
somewhere they did not choose.

### 4.1 Seeing it, with no radio and no readable panel

`fbdump` dumps the raycast framebuffer, not the panel. MISO is held low by
GPIO12's strapping resistor, so the glass cannot be read back. And the radio was
taking the USB link with it. This view could not be photographed, fetched into,
or touched.

Three diagnostics, all on the paths the real thing uses: `htmltest` runs the real
`html_render()` over 31 checks on the board; `webdemo` injects a canned response
through the same state a completed fetch sets; `webtap x y` calls
`browser_touch()`, the function the touch task calls — press *and* release,
because sending only the press leaves `g_was_down` set and swallows the next tap.

They also found a bug the old view had hidden. `ROWS` is 24 and a rendered page
is often six; one tap in the lower half scrolled by twelve and left a **blank
view** with no way back. The scroll was never clamped. It had not mattered while
the raw dump was twenty-odd rows of headers.

---

## 5. The blob eats the heap

Checking that 391's DRAM had not starved the radio produced a panic report —
`exccause 20`, `epc 0x00000000`, a call through a null pointer — and the
investigation is recorded in full at `docs/debug/2026-09-11-wifiopen-null-call.md`.

The mechanism:

```
osi sizes : 32 4 60 8 24 3120 1024 136 136 136 596 596 596 596 596 120 1604 1604
            └─────────── fixed overhead = 7,780 ───────────┘ └─ RX buffers ─┘
```

7,780 bytes of fixed overhead, then a **1,604-byte RX buffer per
`static_rx_buf_num`**, then dynamic buffers — and `dynamic_rx_buf_num` was 32,
another 51,328 bytes. Espressif's reference configuration needs 23,820 bytes
before a single dynamic buffer, against a heap of 11,592.

`heap_alloc` returns NULL. **The blob does not check it** — the OSI table has no
null slots, so the null call is inside the blob — and calls through it.

### 5.1 Where the memory was

Two **shell diagnostics** were holding 13 KB of `.bss` permanently:
`snap[384][6]` at 9,216 bytes, for `txwatch` — a *transmit* diagnostic, on a
board whose transmit was closed as a negative at step 01 — and `many[240×12]` at
5,760. `snap`'s own comment records it being halved **twice** for DRAM pressure
and `many`'s once. The memory had been coming out of the radio the whole time and
the two facts had never been put beside each other.

Heap at boot: **11,592 → 23,688**.

### 5.2 Two mistakes, kept

**"Recorded as far back as boot #83"** was a misreading: boot #83 was the latest
occurrence, and the counter had climbed there through the session's own
reflashes. What settled that the fault was not mine was building `a1fdd0b` in a
throwaway worktree — identical panic. A boot number is not a count of failures.

**`static_rx_buf_num` was wrongly eliminated.** Setting 10 → 4 moved 72 bytes, so
it was recorded as eliminated. The experiment could not have shown anything: at
11,576 bytes free the heap runs out after *one* buffer, so 4 and 10 fail
identically. Rule 6 says change one variable at a time; it does not say the
variable you changed was capable of distinguishing anything.

### 5.3 What this does to step 376

376 concluded **supply, not software**, and said honestly what it had not
established: *"the board went silent, it did not reboot, so this is inference
from the shape of the failure rather than from a reset register."*

A panic produces the same silence. There was no panic report because no capture
had survived `wifiopen` long enough to print one.

**And the supply fault is real and separate.** After the fix, one run still lost
the link at `wifiopen` with no panic, and the CH340 left the USB bus entirely.
The board was in hub port 7; it has only ever enumerated on ports 1 and 2.
Two independent faults producing the same silence, which is why each hid the
other for sixteen steps.

---

## 6. The reading

```
>>> wifijoin
   dhcp      state 10 tries 0  addr 192.168.1.140
>>> webtap 165 37          (the go button)
   web      mode=page  text=142B  links=1
     link 0 -> https://iana.org/domains/example
>>> webtap 10 5            (header toggles raw)
     0 |HTTP/1.1 200 OK |
     1 |Date: Sun, 13 Sep 2026 15:54:56 GMT |
     4 |Server: cloudflare |
```

**The text is not the sample.** Every rendering before this came from `webdemo`'s
hand-written replica, which used example.com's older copy — "illustrative
examples", "More information...". The live page reads "documentation examples
without needing permission" and its link says "Learn more". Nothing in this tree
contains those words.

### 6.1 The number 388 could only predict

388 said plainly that `g_dropped` had never been read on hardware — *"the number
it will report for example.com is a prediction, not a measurement."*

| | |
|---|---|
| body length | **828** bytes |
| dropped | **0** |

The whole response was kept. It also settles whether 391's raise was
load-bearing: at the old cap of 768 this response would have lost **61 bytes off
its end** — the closing tags and the terminator of the only link on the page.

391 sized the buffer against a 1,461-byte replica of the *old* example.com; the
real page is 828. 2,048 is more headroom than this page needs, which is the right
direction to be wrong in.

---

## 7. What remains

1. **The link on that page is `https://`.** Tapping it twice refuses, out loud.
   There is no TLS and there will not be: a handshake wants more heap than this
   board has in total. The browser reads the page and declines the link, and that
   is an honest description of the machine.
2. **`static_rx_buf_num = 6`** is the first value that received a DHCP offer, not
   a measured optimum. Four associates but cannot catch an offer.
3. **The socket fault is understood, not fixed.** Hub port 7 still fails; the
   board is simply not in it.
4. **VM-08 proper** needs secure boot and a key in eFuses, deliberately not done.
5. **`APP_MAX` is 4**; per-task stacks (352c); the null-`sp` fault (351).

**An operating system with no ESP-IDF, no FreeRTOS and no C library associated
over WPA2, took an address from DHCP, fetched a page over HTTP, stripped the
markup, and drew it on the glass as words with the link picked out in cyan.**
