# UM-NATOS-049 — Associated, and Answering

**Used Medias LLC — Embedded Systems Division**
Revision 1.0 · 2026-08-24 · Status: **nat-os joins a Wi-Fi network as a station and replies to a ping from another machine.**

---

## 1. Abstract

nat-os is on a network.

It associates with an access point, is handed frames by the driver's data
path, builds Ethernet, IPv4, UDP, ARP and ICMP packets by hand, and **answers a
ping from a separate machine**. The verdict came from a laptop this project
does not control, running software it did not write — the same standard the
beacon test used at step 209, and the only standard that means anything here.

Three things account for the distance covered:

- **`wpa_sta_connect`**, one line of a supplicant function read out of the
  ESP-IDF source, which moved the association from silent to completing. §3.
- **The data path** — `esp_wifi_internal_reg_rxcb` and `esp_wifi_internal_tx` —
  and a DHCP DISCOVER used as a *provocation*, because a silent network and a
  broken receiver look identical. §4.
- **ARP and ICMP echo**, about three hundred lines in `kernel/net.c`. §5.

This report covers `next_moves/08` steps 217–230. UM-NATOS-048 covers 197–216.

---

## 2. State before and after

| | after 048 (step 216) | now (step 230) |
|---|---|---|
| association | never attempted | **CONNECTED to an open network** |
| WPA2 / WPA3 networks | — | fail with a named reason code |
| data frames received | none | **yes, via `reg_rxcb`** |
| packets transmitted | raw 802.11 beacons | **Ethernet / IPv4 / UDP / ICMP** |
| DHCP | — | DISCOVER sent, OFFER parsed, **ACK not received** |
| ARP | — | **answered** |
| ICMP echo | — | **answered — a ping replies** |
| blob entry table | version 12 | version 15 |

```
boot        11 PASS 0 FAIL
wintorture  10 real switches, checksum CORRECT
wifiinit start
            init / wpa_cb / STA / start / sweep / beacons /
            set_config / connect / reg_rxcb / DHCP / poll
            -- all ESP_OK
ping 10.224.203.200 from another machine  ->  REPLY
            net +20s frames 43 arp 1 icmp 20 drop 0 [IP]
```

---

## 3. Association

Step 218 measured where the association stopped: `cnx_connect_next_ap` loads
`wpa_cb + 8` and calls it. Offset 8 is `wpa_sta_connect`, and nat-os had a
recording stub there, so nothing drove the next step — thirty seconds of
`STA_START` and no other event at all.

ESP-IDF's version, **read from the source on this machine rather than guessed**:

```c
int wpa_sta_connect(uint8_t *bssid) {
    ret = wpa_config_profile(bssid);
    if (ret == 0) { ret = wpa_config_bss(bssid); if (ret) return ret; }
    else if (authmode == NONE_AUTH) esp_set_assoc_ie(bssid, NULL, 0, 0);
    return esp_wifi_sta_connect_internal(bssid);
}
```

The first two calls are the WPA half — RSN IE parsing, PMK derivation, the
four-way handshake — and that is a subsystem nat-os does not have. The **last
line** is what moves the driver, and for an open network it is very nearly the
whole function. That line, and nothing else, is implemented.

### 3.1 Three networks, three answers

| network | security | result |
|---|---|---|
| a WPA2-PSK router | WPA2 | `203 ASSOC_FAIL` — association needs the RSN IE |
| a WPA3-SAE phone hotspot | WPA3 | `202 AUTH_FAIL` — SAE authentication needs `wpa3_build_sae_msg` |
| an open hotspot | none | **`CONNECTED`** |

The differences are exactly the supplicant's contribution, which is a far
sharper statement than "association fails". Step 219 **predicted** the open-network
result before it could be tested, and recorded it as a prediction; step 221
confirmed it. Predicting first is what turned a lucky outcome into a measurement.

### 3.2 The stub return value was wrong in both directions

Step 205 returned 1 from every `wpa_cb` entry, because the entries it knew about
returned `bool`. Against the WPA3 hotspot:

```
exccause 28 LoadProhibited   epc 0x4000c2af (a ROM copy routine)
excvaddr 0x00000001
```

`struct wpa_funcs` also contains **pointer-returning** entries, and a
WPA3-capable access point reaches one. The driver took the `1` as a source
pointer and handed it to `memcpy`.

Returning 0 everywhere instead hung before `esp_wifi_set_mode` and
watchdog-reset the board, because the bool entries read false as refusal.

Neither blanket answer is right, and `esp_wifi_driver.h` is on this machine and
says which of the 25 entries returns what:

| entries | return | correct value |
|---|---|---|
| 0, 1, 8, 9, 10, 12 | `bool` | **1** |
| 6 `wpa_sta_in_4way_handshake` | `bool` | **0** — we are never in one, and true would make the driver wait for a handshake that cannot complete |
| 7, 11, 14, 18, 23 | pointer | **0** |
| the rest | `int` / `void` | 0 |

Reading the header cost one grep. Guessing cost a crash and a watchdog reset.

### 3.3 The SSID was not what it looked like

iOS names devices with a **typographic** apostrophe, U+2019, not ASCII `0x27`.
The scan said so rather than anyone assuming it:

```
ssid [Nathan???s iPhone]  ch@39=6  rssi -44dBm
```

Three `?` is exactly the three non-printable bytes of U+2019 in UTF-8. An ASCII
apostrophe would have produced an SSID matching nothing on air and
`NO_AP_FOUND` for a network at −44 dBm — **a wrong answer that looks like a
radio problem**. Confirmed afterwards by the driver echoing `ssid_len 17`
(6 + 3 + 8) in the disconnect payload.

---

## 4. The data path, and silence as a non-measurement

Entry table version 15 added `esp_wifi_internal_reg_rxcb`,
`esp_wifi_internal_tx` and `esp_wifi_internal_free_rx_buffer`. The callback is
**windowed** — the driver calls it with `callx8` from its own task — and it does
the least possible: copy the frame into a ring, free the driver's buffer, return.
Everything else crosses into call0 through `w2c_call2` and runs from a polling
loop.

The first measurement was `frames 0`, and it meant nothing at all.

**A zero is consistent with two opposite worlds**: a hotspot with no other
clients and therefore no broadcast traffic, or a receive path that is not wired.
Waiting longer cannot separate them and neither can looking harder at zero.

A **DHCP DISCOVER** can, because a server is obliged to answer:

```
dhcp DISCOVER 291 B  tx rc 0x00000000  sent
rx   frames 1 bytes 352
     len 352  dst ffffffffffff  src <AP>  type 0x0800 IPv4
     DHCP reply, offered 10.224.203.139  from 10.224.203.104
```

One exchange proved the transmit path, the receive path and the subnet.
Provocation beat observation, exactly as the beacon did at step 209.

### 4.1 Two wrong turns first

The first `frames 0` was taken **while the thirteen-channel sweep was running** —
the receiver was on a different channel entirely. A receiver that is not on the
access point's channel is not evidence of a quiet network.

Then promiscuous mode was suspected, since step 197 enabled it and nothing had
disabled it, and promiscuous routes frames to a callback nat-os never
registered. Turning it off changed nothing. **Disproved, not left hanging** —
though the entry stays off, because an associated station has no use for it.

---

## 5. ARP, ICMP, and the ping

`kernel/net.c` is about three hundred lines: a receive ring, ARP replies, ICMP
echo replies, and the client half of DHCP. It is **not a network stack and is
not trying to be**. lwIP is the right answer for a real stack — it needs a port
layer and is forty thousand lines dropped into a kernel with no libc, and it
deserves its own session. This exists to answer one question with outside
evidence.

### 5.1 The two failures that located the bug

The *difference* between two ping errors was the whole diagnostic:

| the pinging machine said | what it proved |
|---|---|
| `Destination host unreachable` | ARP unanswered — correctly, since `net_handle` ignores ARP for an address it does not own, and DHCP had given it none |
| `Request timed out` | **ARP answered.** The other machine had our MAC and sent the ICMP. Only the reply was missing. |

Those two failures could not be told apart while DHCP and ARP failed together.
**A static address separated them** — and that separation is what made the
second failure visible on its own.

### 5.2 One index

```c
ip[12+i] = in[14+16+i];   /* src = incoming dst = us   -- correct */
ip[16+i] = g_ip[i];       /* dst = us again            -- wrong   */
```

Every echo reply was **addressed to nat-os itself**. Built correctly,
checksummed correctly, transmitted correctly, delivered to the wrong machine —
which from the other end is indistinguishable from not replying at all. The
sender's address is the incoming IP **source** at `+12`.

Corrected, the ping replies. The board's own counters agree with the other
machine's screen, independently:

```
net +10s frames 22 arp 1 icmp 10 dhcp 0/0 drop 0 [IP]
net +20s frames 43 arp 1 icmp 20 dhcp 0/0 drop 0 [IP]
```

**`arp 1`** -- asked once and cached thereafter, which is what ARP is for.
**`icmp 10` then `icmp 20`** -- exactly one echo request per second, which is
exactly the rate `ping` sends at. Two instruments that share no code agreed on
the same number, which is worth more than either alone. `drop 0`: the receive
ring never overflowed.

### 5.3 What this does not establish

- **DHCP does not complete.** The OFFER arrives and parses; no ACK follows the
  REQUEST. The address in use is hardcoded. This is now cleanly isolated from
  ARP and ICMP, which is precisely what the static address bought.
- No TCP, no UDP beyond the DHCP client, no routing, no DNS, no sockets.
- No WPA2 or WPA3 association — the open hotspot is the only network nat-os can
  currently join.
- Reception is idle-network reception. Throughput, loss and MTU behaviour are
  entirely unmeasured.

---

## 6. Method

**Provocation beats observation.** Twice now — the beacon a phone could see, and
a DHCP DISCOVER a server had to answer. Both replaced an unfalsifiable silence
with a definite reply.

**Predict before testing.** Step 219 wrote down what an open network *should* do
and why. When step 221 matched it, that confirmed a model rather than producing
a happy accident.

**The difference between two failures is data.** "Unreachable" and "timed out"
are both failures and mean opposite things about ARP. Reading only the second
would have sent the search to the wrong half of the code.

**Read the header that is already on the machine.** §3.2 cost a crash and a
watchdog reset before anyone opened `esp_wifi_driver.h`.

### 6.1 Four instrumentation failures, all mine

Worth recording because together they cost more time than any defect in the
code did.

1. **A poll that counted its own sleeps.** `spent += 2` per iteration assumed
   `task_sleep(2)` costs exactly two ticks. It does not, so a "120 s" window ran
   long enough to overrun a 210 s capture and print nothing. It now reads the
   hardware timer. Same error as the beacon pacing at step 209.
2. **A capture window never verified.** A chain of `sed` substitutions produced
   a drain of ~90 s while the reasoning assumed 190. Three runs were wasted
   before the value was checked. Same class as the `TX_BEACONS` slip.
3. **A probe that buffered its output**, so a test in which a human types `ping`
   mid-run could not be watched at all. It now prints progress every ten
   seconds — because a silent minute and a hung minute are identical otherwise.
4. **A grep misread as evidence.** "The sweep finds ABCDE" came from a pattern
   that had actually matched *this project's own console output*. The scan had
   not run at all. It was stated to the operator as fact and was wrong.

The pattern is one thing: **an instrument that has not been checked is not an
instrument.** Three of the four produced confident wrong statements rather than
obvious failures.

---

## 7. What is on air

- **Associated** to an open access point, holding the association across scans.
- **Transmitting** ARP replies, ICMP echo replies, and hand-built DHCP packets.
- **Receiving** data frames through the driver's own path and parsing them.
- **Answering** a ping from a machine on the same network.

Not associated to any protected network. No stack above ICMP and the DHCP
client.

---

## 8. What remains

1. **The DHCP REQUEST.** The OFFER parses; the ACK never comes. The address is
   hardcoded until this works, and it is the smallest remaining defect.
2. **lwIP**, for an actual stack — TCP, UDP, sockets, DNS. The port layer
   (`sys_arch`, a netif driver, threading) is the work, and `net.c` already
   proves the netif half is reachable.
3. **The WPA supplicant**, to join a protected network. §3.1 measures exactly
   which parts are missing: the RSN IE for WPA2, SAE for WPA3.
4. **The `w2c_*` save-area overlap**, unchanged since UM-NATOS-045 §8.4 —
   dormant, and needing `MOVSP` plus an Alloca handler.
5. **The `reason 2` drops.** Roughly one association in three is deauthenticated
   by the access point. The hypothesis — that a client which never speaks gets
   dropped — is now testable, since nat-os does speak.

---

Full experiment log: `docs/next_moves/08-wifi-via-loaded-blob.md`, steps 217–230.
Companion reports: UM-NATOS-042 (rev 1.1), 045 (rev 1.0), 046 (rev 1.0),
047 (rev 1.0), 048 (rev 1.4).

**nat-os has been pinged, and replied.**

Written by: Hare
