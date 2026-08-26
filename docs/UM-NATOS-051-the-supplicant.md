# UM-NATOS-051 — The Supplicant

**Used Medias LLC — Embedded Systems Division**
Revision 1.0 · 2026-08-26 · Status: **nat-os associates with a WPA2-PSK network, completes the four-way handshake, and holds a DHCP lease.**

---

## 1. Abstract

nat-os is on a protected network.

It advertises an RSN information element, authenticates, associates, derives a
pairwise key from a passphrase, answers message two of a four-way handshake with
a MIC an access point accepts, verifies message three's MIC, unwraps the group
key, installs both keys into the radio, and is handed an IP address by a DHCP
server it has never seen.

Four things account for the distance, and only one of them is cryptography:

- **Vendored crypto, hand-written state machine**, decided by testability rather
  than taste — the crypto has published vectors and the state machine is one
  protocol rather than five. §4.
- **A packet sniffer**, which ended fifteen steps of inference by reading what
  was actually on the channel. §7.
- **Two working fixes recorded as failures**, because each one succeeded and
  then crashed downstream, and the crash reached the console first. §8.
- **Three bugs in the handshake**, none of them in the crypto, all found by
  instrumentation rather than insight. §9.

This report covers `next_moves/08` steps 236–259. UM-NATOS-050 covers 231–235.

---

## 2. State before and after

| | after 050 (step 235) | now (step 259) |
|---|---|---|
| open networks | associated, DHCP, TCP | unchanged |
| **WPA2-PSK networks** | `203 ASSOC_FAIL` | **associated, aid 12** |
| RSN information element | none | 22 constant bytes, advertised |
| crypto | none | SHA-1, HMAC, PBKDF2, PRF, AES, AES-unwrap |
| crypto self-test | — | **4 published vectors, 0 failed** |
| four-way handshake | none | **completes — m1, m2, m3, m4** |
| pairwise key | — | **installed** |
| group key | — | **unwrapped and installed** |
| `wpa_sta_connected_cb` | never called | **called** |
| DHCP over WPA2 | — | **bound, 192.168.1.140** |
| supplicant entries implemented | 1 of 25 | 7 of 25, all 32 slots traced |
| blob entry table | version 16 | version 19 |
| 802.11 visibility | reason codes only | **every frame on the channel** |

```
boot   11 PASS 0 FAIL      wintorture checksum 1632 CORRECT
wpa    crypto self-test (IEEE 802.11i vectors + RFC 3394)
       PASS pbkdf2 #0   PASS pbkdf2 #1   PASS pbkdf2 #2
       PASS aes_unwrap (RFC 3394 4.1)
       4 passed, 0 failed
sniff  AUTH        to ..503f64  from ..2f57c6   alg0(OPEN) seq2 STATUS 0 SUCCESS
       ASSOC_RESP  to ..503f64  from ..2f57c6   STATUS 0 SUCCESS aid 12
4way   pmk=1 step=6 m1=1 m3=1 done=1 micbad=0 why=0
wpa    conn=1 disc=0 cbreason=none
lwip   DHCP bound -- address 192.168.1.140
```

---

## 3. Twenty-two constant bytes

For WPA2-PSK with CCMP the RSN information element is a **constant**. It is a
declaration of what the station intends to negotiate, not proof that it can:

```
30 14                element 48 (RSN), length 20
01 00                version 1
00 0F AC 04          group cipher            CCMP
01 00  00 0F AC 04   one pairwise cipher     CCMP
01 00  00 0F AC 02   one AKM                 PSK
00 00                capabilities
```

Installing it moved the reason code from `203 ASSOC_FAIL` to `39 TIMEOUT`, and
**step 237 read that as progress.** It was recorded as "the RSN IE gets WPA2
past association".

It is the opposite, and §6 is how that was found out. 203 was the access point
*answering*; 39 is the access point no longer able to read the question. The
element went in correct and the way it was *installed* corrupted the association
request. Fifteen steps were spent on the far side of that reading.

### 3.1 A buffer handed to the blob must live in RAM

The first attempt declared the element `static const uint8_t[22]`, which puts it
in `.rodata` — flash, mapped through the data cache at `0x3F4xxxxx`. Handing
that pointer to the driver produced

```
exccause 3 LoadStoreError   epc 0x40310458   excvaddr 0x3f40b334
```

a fault inside the driver reading our own element. Flash-mapped DROM does not
tolerate the access widths and alignments RAM does. Dropping `const` puts it in
`.data`; the `aligned(4)` attribute settles the width. Nothing in the C says
otherwise and nothing in the build warns.

---

## 4. Vendor the crypto, hand-write the state machine

Two routes were sized before either was started:

| route | cost |
|---|---|
| port ESP-IDF's `rsn_supp` | 5,660 lines of state machine — `wpa.c` alone is 3,017 — plus `utils/eloop`, `wpabuf` and a set of IDF headers, handling WPA, WPA2, WPA3, enterprise, FT roaming and PMKSA caching: **five protocols where nat-os needs one** |
| hand-write everything | ~1,900 lines, including hand-rolled SHA-1 and AES — **where subtle, silent, security-relevant bugs live** |

Neither. Seven files, 234 KB of vendored crypto against 2.1 MB for lwIP, and a
state machine written for one protocol.

**The deciding argument was testability.** Crypto has published test vectors, so
it can be checked on the bench with no network and no ambiguity — the answer is
the published constant or it is not. A handshake that fails because PBKDF2 is off
by one iteration is indistinguishable from a dozen other faults and would be
debugged over the air, twenty seconds at a time.

That decision paid at step 258, when the group key would not unwrap and
`aes_unwrap` was the obvious suspect. §9.1.

### 4.1 The whole path speaks one ABI

`esp_wifi_set_sta_key_internal` takes **nine arguments** and the
call0-to-windowed bridges carry four, so the handshake must be windowed to
install a key at all. Windowed code reaching call0 crypto would then need bridges
carrying three, against `pbkdf2_sha1`'s six and `sha1_prf`'s seven. Compiling the
crypto windowed removes every bridge from the path: handshake, crypto and blob
all speak one ABI, and only addresses cross.

The crypto is ordinary C with no ABI opinion, so this cost a compiler flag.

### 4.2 Four ABI violations, all the same rule

Moving code across the boundary broke it four times, each in this project's own
documented failure mode:

1. `wpatest.c` became windowed and kept calling `uart_puts`, which is call0 —
   `LoadProhibited`, `excvaddr 0`.
2. The windowed crypto called `os_memcpy`/`os_memset`, macros forwarding to
   call0 `kstring.c` — `IllegalInstruction` with `epc == a0`, a windowed return
   address leaking into PC, because `call8` leaves it in `a8` while a call0
   callee returns through `a0`.
3. GCC turned the *inlined* replacement back into a call to `memcpy`, because an
   aggregate initializer (`u8 seq[6] = {0,…}`) is a language-level copy that
   `-fno-builtin` does not touch.
4. `lwip_rand_u32()` on the message-one path — §9, and it hid two working fixes
   for two steps.

The general hazard, now paid for four times: **aggregate initializers and struct
assignments emit calls to call0 string functions that no source line mentions.**

---

## 5. Fifteen seconds inside a callback

`wpa_sta_connect` is called **by the driver**, from `cnx_connect_next_ap`, in the
middle of establishing an association. PBKDF2 at 4096 iterations was being run
there. Measured on this part: **15 seconds.**

Fifteen seconds inside that callback means the association is long dead before
the access point would send message one — which is exactly the silence that was
observed: no CONNECTED, no DISCONNECTED, no EAPOL, no event of any kind.

Step 240 measured PBKDF2's cost and wrote, in its own commit message, that it
"should not be recomputed per association". It was then put in the
per-association path anyway, three steps later.

**Measuring a thing does not help if the measurement is not used**, and the note
that would have prevented this was written by the same hand that ignored it. The
derivation now runs once at bring-up and reports its own duration, so the cost
stays visible rather than becoming folklore.

---

## 6. Reason 39 never meant "associated"

Steps 237–245 all rest on one reading: that reason 39 means *associated, and then
no handshake*. That reading was never measured. It was inferred at step 237 from
203 becoming 39, and then **written into the reason table as a string** —
`" TIMEOUT(associated, no handshake)"` — where it stopped looking like a
hypothesis and started looking like a fact. Nine steps took it as their premise.

Two things should have been the tell. `39` is `WIFI_REASON_TIMEOUT`; the code
ESP-IDF's driver reports for a station that associates and then cannot complete
EAPOL is **204 `HANDSHAKE_TIMEOUT`**, or 15. Neither had ever appeared in this
project.

### 6.1 Measured, from two independent paths

`struct wpa_funcs` slot 3 is `wpa_sta_connected_cb` and slot 4 is
`wpa_sta_disconnected_cb`. Both had been pooled into one shared recording stub,
so a call to either was indistinguishable from a call to any other slot — twelve
return addresses and no names. Named:

```
wpa conn=0  disc=1  cbreason=39
evt posted 15   :2   :1 x13   :5(reason39)      -- NO :4
```

The driver never calls `wpa_sta_connected_cb`, and no `STA_CONNECTED` event is
ever posted. **The station does not associate.** The missing EAPOL of steps
241–245 then needs no explanation at all: the access point never had an
associated station to send message one to. The handshake in `wpa_hs.c` was not
broken, and was never reached, because nothing could reach it.

The reason also arrives at slot 4 as an *argument*, independent of the guessed
`+39` offset into the event payload, so the two readings cross-check.

### 6.2 One hypothesis killed at zero cost

The leading theory was that the driver gates EAPOL delivery on
`wpa_sta_in_4way_handshake` (slot 6), which answers false until message one
arrives — a chicken-and-egg. The ESP-IDF supplicant source is on the build
machine, and `rsn_supp/wpa.c:682` sets `WPA_FIRST_HALF_4WAY_HANDSHAKE` *inside*
the message-one handler. Real IDF answers false before message one too.

Not the gate. Eliminated by reading, before a build was spent. The same read
confirmed the slot map against IDF 5.1.4 — including the pointer-returning
entries that step 220 had derived empirically, from crashes.

### 6.3 Naming all thirty-two

One trampoline per slot, each recording its own index:

```
wpa seq    0 15 21 21 21 21 15 21 21 22 15 21
wpa named  0=sta_init  15=parse_wpa_ie  21=sta_rx_mgmt  22=config_done
```

`wpa_parse_wpa_ie` was called **three times on the failing connect** while
returning 0 — success — and writing nothing, so the driver read back proto 0,
pairwise 0, key_mgmt 0: *this access point offers no security*, about a WPA2
network. That is the fifth instance of the class this investigation keeps
finding, after `_event_post`, `_task_delay`, the event groups and
`_queue_send_from_isr`: **an entry that reports success for work never done**,
invisible because nothing returns an error.

It was implemented, it reads the access point exactly right — group CCMP,
pairwise CCMP, AKM PSK, from a 20-byte body — and **it was not the cause.** The
association failed identically. Kept anyway, on the reasoning steps 200 and 202
used: a live defect is worth fixing whether or not it is today's.

### 6.4 The field conventions are not uniform

`wifi_wpa_ie_t` mixes two conventions, and the wrapper it replaces is the only
place that says so:

| field | convention |
|---|---|
| `proto`, `key_mgmt` | **supplicant** bitmasks (`WPA_PROTO_RSN` = BIT(1)) |
| `pairwise_cipher`, `group_cipher` | **public** enum, mapped through `cipher_type_map_supp_to_public` |
| `capabilities` | raw, straight from the element |

Guessing one convention for all five would have put TKIP (3) where CCMP (4)
belongs — a wrong answer that looks like a right one, and it would have poisoned
every step after it.

---

## 7. The sniffer, and the end of inference

Promiscuous mode had been **on since step 197 with no callback registered**, so
every management frame the radio decoded outside the data path was decoded and
thrown away. Step 245 concluded that separating "the access point never answers"
from "the driver discards it" needed a packet capture on another machine.

It did not. The radio was already listening and nobody had asked it what it
heard. Entry table version 19 adds `esp_wifi_set_promiscuous_rx_cb` and
`esp_wifi_set_promiscuous_filter`; the callback is windowed, copies nine bytes,
and returns. It does **not** free the buffer — unlike the data path's `rxcb`, the
promiscuous callback does not own it.

```
sniff  seen 577  beacons/probes 569  layout-miss 0  KEPT 8
       AUTH  to ..503f64  from ..2f57c6  ch11 rssi-55
              alg0(OPEN)  seq2  STATUS 0 SUCCESS
```

**Authentication succeeds.** Open-system, transaction sequence 2 — the
authenticator's reply — status 0. nat-os transmits an authentication request, the
access point receives it, accepts it, and answers. Nothing in the radio, the PHY,
the MAC, the transmit path or the channel is at fault, and none of that is
inference any more.

And then nothing. No association response, ever, in 577 frames.

### 7.1 The layout was measured, not believed

`wifi_promiscuous_pkt_t` is `rx_ctrl` followed by the raw 802.11 frame, and on
ESP32 `rx_ctrl` is **seven 32-bit words** — counted from the bitfields in
`esp_wifi_types.h`, not guessed. Two self-checks guard the +28 offset: `sig_len`
must be a plausible frame length, and the frame control's protocol version bits
must be zero. Across 577 frames, `layout-miss` is **zero**, so the offset is
measured rather than believed.

Beacons and probe responses were 569 of 577. Step 249's five "management frames"
— forwarded to the supplicant for BSS-table maintenance — were all of that kind,
and keeping them would have buried the one frame that mattered.

### 7.2 The A/B that located the fault

The sniffer reports *received* frames, so it cannot see what this board
transmits. "No association response" was still consistent with both *the request
was never sent* and *it was sent and ignored*. One variable separates them:

| arm | element | the access point's answer | reason |
|---|---|---|---|
| `startnoie` | none | `ASSOC_RESP STATUS 10` | 203 |
| `start` | AKM PSK (`02`) | **silence** | 39 |
| `start8021x` | AKM 802.1X (`01`) | **silence** | 39 |

With the element suppressed the access point **receives our association request
and answers it.** The frame goes out. So the failure is our RSN element — and it
is **malformed rather than merely unacceptable**, which is the distinction that
matters: a well-formed request the access point dislikes earns a status code
(status 10, *cannot support all requested capabilities*); one it cannot parse
earns silence.

The third arm differs from the second by **a single byte**, asking a WPA2-PSK
router for 802.1X, which it cannot provide and must refuse. It refuses nothing.
So content is eliminated too: the access point does not care what is in the
element, only that there is one.

All three are runtime arms of one build, not patches, and they stay in the tree.

---

## 8. Two working fixes, recorded as failures

Steps 253 and 255 each changed the appie call, each produced a kernel panic, and
each recorded the change as **eliminated**.

The panic was not the change failing. It was the change **working** — the
association completing, the access point sending message one, and nat-os's own
four-way handshake crashing on the first frame it had ever received. The crash
beat the evidence to the console, so all that survived was the fault.

`AGENTS.md` rule 12 says, in those words: *"when an experiment produces a
surprising result, investigate the implication rather than immediately reverting
it."* Step 253 reverted. The rule was written down, was on screen, and was not
followed. It cost two steps and put two wrong entries in the record.

**What caught it was not judgement but arithmetic.** `a0` was `0x8036cc9c` in
both panics — two different exceptions, the same frame, the same caller — and the
blob's own symbol table resolves it:

```
4036cb30 T sta_rx_eapol          <-  0x4036cc9c is sta_rx_eapol + 0x16c
```

An access point sends EAPOL to a station that has **associated**, and to no other
kind.

### 8.1 The arm that settled it

Rather than argue, the handshake was reduced to a counter — no PTK derivation, no
MIC, no transmit, no key install — and the association arm re-run:

```
ASSOC_RESP  STATUS 0 SUCCESS  aid 12
DEAUTH      reason 15
4way  pmk=1  m1=3  ki=0xeeee
```

Association identifier 12. Message one **three times**. Deauthentication with
reason 15, `4WAY_HANDSHAKE_TIMEOUT`, because passive mode counts the frame and
answers nothing. Every one of those is correct behaviour for a station that
associated and then went quiet.

The fix is that `esp_wifi_set_appie_internal`'s trailing argument must be **0**,
not the 1 that ESP-IDF's own RSN call site passes. Why IDF passes 1 and works is
not understood, and is not guessed at here.

---

## 9. Three bugs, none of them the crypto

With the association fixed, the handshake ran for the first time and got most of
the way on its first attempt:

```
4way  pmk=1 st=1 m1=1 m3=1 done=0 micbad=0 unwrap=1 ki=0x13ca
```

Every counter is a proof. `m3=1` means the access point sent message three, which
it does **only** after message two arrives with a MIC it accepts. `micbad=0`
means message three's own MIC verified against our KCK. `ki=0x13ca` is
`PAIRWISE|INSTALL|ACK|MIC|SECURE|ENCRYPT`, descriptor version 2 — exactly a
WPA2-CCMP message three.

So the PMK is right, which validates PBKDF2 over this passphrase and SSID; the
PTK is right, which validates the PRF and the address and nonce orderings — an
access point does not answer a message two whose MIC it cannot reproduce — and
the KCK is right twice over.

`unwrap=1`. The group key would not come out.

### 9.1 The primitive was proved before the protocol was blamed

`aes_unwrap` was the obvious suspect: the one function on this path that step 240
never checked against a published answer. So it was checked, against **RFC 3394
§4.1**, and it passes. The crypto was never wrong.

Had that vector not been written first, the next stretch of work would have gone
into `aes-unwrap.c`. It is now the fourth vector in the boot self-test.

### 9.2 The key data of message three begins with the access point's RSN IE

```c
if (t != 0xDDu || l < 4u || i + 2u + l > n) { break; }
```

A WPA2 message three's key data is the access point's **RSN information element,
id `0x30`** — followed by the GTK KDE. The walk broke on element one, every time,
and never reached the group key that `aes_unwrap` had already decrypted
correctly. A non-GTK element is not an error; it is an element.

### 9.3 The group key was installed at address NULL

With the KDE found, the driver took a NULL pointer into a ROM copy routine:
`LoadProhibited`, `epc 0x4000c28c`, `excvaddr 0x00000000`. ESP-IDF's
`wpa_supplicant_install_gtk` passes `sm->bssid` — the access point — with the
broadcast address commented out beside it, so this was a choice between two known
values rather than a guess between none.

### 9.4 An access-point function on the station path

`esp_wifi_wpa_ptk_init_done_internal` never returned, **and it took the blob
mutex with it** — which is why the shell's next blob call hung, and why several
earlier runs died at `prof authmode` with no explanation at all.

Its only caller in the entire ESP-IDF supplicant is `src/ap/wpa_auth.c:2003`, the
*authenticator* side. A station completing a four-way handshake calls
`wpa_neg_complete()`, which is `esp_wifi_auth_done_internal` and nothing else.
The entry stays in the table for the day nat-os runs an access point; the station
path leaves it alone.

**What found it was instrumentation, not insight.** `m3=1` with `done=0` is a
five-way guess. One store per stage through the message-three tail turned it into
`step=4`, which names the call.

```
4way  pmk=1 step=6 m1=1 m3=1 done=1 micbad=0 why=0
wpa   conn=1 disc=0 cbreason=none
lwip  DHCP bound -- address 192.168.1.140
```

---

## 10. Method

### 10.1 The instrument was wrong twice, and both times it was checked

Step 258 reported "no DHCP" from a counter reading `offer/ack 0/0`. That counter
belongs to `kernel/net.c`, the hand-rolled stack; lwIP is a different stack with
its own client, and **it had bound an address.** The encrypted data path was
working and the wrong instrument was being read.

Step 258 also flagged rising `contended=` in the status line as blob-lock
pressure. It is `g_shared_lock` — the `wintorture` test mutex, nothing to do with
the blob. Reading the print site is what caught it.

A measurement is only as good as knowing which thing it measures. This report
contains two cases where that was got wrong, and one — §6.4 — where reading the
source before writing the fix is the only reason it was got right.

### 10.2 Silence is a measurement; a panic is not

Step 253's revert was wrong for the reason §8 gives, but the principle it invoked
was sound and is worth keeping separate from the error: given a choice between a
state that produces a diagnosable failure and one that produces a crash, hold the
diagnosable one. The mistake was not preferring silence. It was failing to ask
what the crash meant.

### 10.3 The cost of instrumentation, again

`iram` filled twice during this work. Step 259's starvation report was paid for by
deleting step 249's per-frame subtype list — which the sniffer had already
superseded, printing a worse version of a better measurement. Step 258's stage
markers were paid for by dropping `ki=`, `st=`, `poll=`, `txerr=` and `unwrap=`,
all of which had been reporting the same value for steps.

**A counter that has read the same value for ten steps is not instrumentation, it
is furniture.**

---

## 11. What is on the network

```
ssid       ivory-billed         WPA2-PSK / CCMP
bssid      7e:26:f6:2f:57:c6    channel 11, -54 dBm
station    5c:01:3b:50:3f:64    aid 12
address    192.168.1.140        DHCP, lease held
keys       pairwise CCMP installed, group CCMP installed
```

---

## 12. What remains

**An intermittent watchdog reset**, two of the four runs that completed the
handshake. (Step 259's commit message says "two in six"; two of the six runs
were cut short by the test harness before the handshake and should not have been
counted.)
Step 259 diagnosed what it is *not*, which is the useful half:

- **Not starvation.** The hang detector now prints which task it was about to
  give up on — it has two seconds of headroom, and by definition nothing else is
  running to be starved by the printing. In the run that reset it printed
  **nothing**, and `wdt f/s=600/0` shows feeds advancing on schedule. The
  watchdog fired **while being fed correctly**, which retires the entire step-242
  class that the boot banner's own description implies.
- **Not the shell stack.** 236 of 2048 bytes free after `wifiinit` looks
  alarming; the same 236 appears in runs that never reach the handshake and never
  reset.
- **Not lock contention.** §10.1 — wrong lock.

A watchdog that fires while fed is one whose configuration or clock is not what
this kernel thinks. TIMG0 is Espressif hardware and the blob is Espressif code,
and nothing prevents it reconfiguring a timer group nat-os also owns. The config
register is now read back every status line against the value `watchdog_arm()`
wrote — `0xe01f8000` — and across the runs sampled it never moved. **That is not yet a
result:** the interesting sample is the one immediately before a reset, and a
status line can be seconds away from it. Catching it needs the readback latched on
change rather than sampled.

**Not yet attempted:** serving UM-NATOS-050's web page over the encrypted link,
group-key rekeying, roaming, PMKSA caching, WPA3/SAE — `202 AUTH_FAIL` against an
SAE hotspot is still where step 219 left it — and the all-channel scan that has
panicked since step 202.
