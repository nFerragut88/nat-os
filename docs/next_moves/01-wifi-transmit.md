# 01 — Make transmit reach the air

> **Status 2026-08-19 — the search is now half the size.** UM-NATOS-034 settled
> the question that had blocked every previous attempt: a second nat-os board,
> promiscuous on the same channel 30 cm away, hears a real access point but
> never hears the transmitter, in both directions, while the transmitter
> reports 302 and 219 completions.
>
> **Nothing demodulable reaches the air.** Framing and content are exonerated;
> the fault is in the RF/PHY path. The leads below survive and are now the
> whole of the search rather than a third of it.
>
> Two things UM-NATOS-034 adds: `chain acks` disagrees with the completion
> count by an order of magnitude and is the only transmit counter not claiming
> success, and `tools/serial/wifi_link.py` now answers "did anything radiate"
> in about a minute.

**Size:** large. **Risk:** real — may cost the working receive path.
**Blocked on:** willingness to accept a link change.

---

## The symptom

nat-os builds an 802.11 probe request, hands it to the MAC, and the hardware
reports **every frame complete** — 178 of 178, `forced=0`. Then nothing answers.
Ten probes, zero responses, from access points whose beacons we are decoding
continuously the entire time.

> A completion bit says the frame left the queue, not that it left the antenna.

Receive works end to end. So the PHY is calibrated, the channel is tuned, the
MAC is clocked. It is specifically the transmit direction that goes nowhere.

## Reproduce it

```
phyinit          # ~10 s, must return 0
macinit
chan 1
macrx
probe            # sends 10 probe requests
txstat           # tx handed=10  completions reaped=10  forced=0
macstat          # beacons arriving; no probe responses
```

`tools/serial/` has no ready-made script for this; the one used was
`wifi_tx.py`, and it is trivial to rewrite from the sequence above.

## Eliminated, by measurement — do not retry these

| lead | result |
|---|---|
| **Transmit power** | `most_tpw` already `0x28` = 10 dBm out of `register_chipv7_phy`. Never zero. Setting it higher changes nothing. |
| **MAC hardware init 0–3** | `ic_mac_init`, `hal_init`, `ic_enable_rx`, `hal_mac_tsf_reset` all run cleanly. No change. |
| **MAC reset** | `DPORT_WIFI_RST_EN_REG` bit 2 pulsed directly. Receive survived; transmit unchanged. Kept behind `macrst`. |
| **Transmit-side init 4–6** | `hal_mac_rate_autoack_init`, `hal_attenna_init`, `hal_mac_disable_low_rate` — all already linked, all ran, no change. `hwinit 4/5/6`. |
| **`wifimac_tx()` transcription** | **Checked line by line against open-mac's `transmit_80211_frame`. It is clean.** Every base address, bit pattern, write order, descriptor field and the final `0xc0000000` match. |
| **Clearing `WIFI_MAC_BITMASK_084` bit 31** | **Killed receive outright.** Descriptors filled → 0, chain acks 157 → 1. Reverted. Not the AP-mode flag it looks like. |

That last one is important: it is the one experiment that *did* change
behaviour, in the wrong direction, and it means the register is not understood.

## What is left

open-mac's `wifi_hw_start_openmac()` does three things nat-os does not, and
**none of the three is in the image**:

```c
esp_wifi_power_domain_on();     // not linked
coex_bt_high_prio();            // not linked, called TWICE
lmacInit / lmacInitAc;          // not linked -- the lower MAC, which arms TX queues
```

Calling any of them changes the link. That is the trap that killed
`register_chipv7_phy` last session — referencing `ram_tx_pwctrl_bg_init` pulled
fresh objects out of `libphy.a` and broke a calibration function that had worked
for days.

## How to do it safely

1. **Commit and push first.** A known-good tree to return to.
2. **Add exactly one reference at a time.** Never two.
3. **`phyinit` returning 0 is the canary.** Run it immediately after flashing.
   If it stops returning 0, the link moved — revert that reference, do not
   investigate further with it in the tree.
4. **Re-verify receive after each**, not just transmit. Receive is the thing
   worth protecting.
5. **Order:** `coex_bt_high_prio` first (smallest, called twice by open-mac so
   plausibly load-bearing), then `esp_wifi_power_domain_on`, then `lmacInit`
   last because it is the largest change.
6. Check what got pulled in:
   ```powershell
   xtensa-esp32-elf-nm build\natos.elf | Select-String " T " | Measure-Object
   ```
   before and after. A big jump means new objects arrived.

## Also unresolved

open-mac calls `periph_module_reset(0x19)`. `0x19` is 25, which in the IDF v5
`periph_module_t` enum looks more like `PERIPH_BT_MODULE` than WiFi. Whether
that is deliberate, a different enum ordering, or open-mac's own transcription
error is unknown and needs the v5.0.1 header to settle.

An earlier claim that `periph_module_reset()` is a no-op for WiFi was asserted
from memory, never verified, and has been **withdrawn** (UM-NATOS-028 §3.1).

## Where the code is

- `kernel/wifimac.c` — `wifimac_tx()`, `wifimac_init()`, `wifimac_hwinit_step()`
- `kernel/wifi_osi_impl.c` — the 39 live OSI table entries
- `vendor/windowed/wifi_osi.c` — the windowed forwarding table
- `docs/UM-NATOS-028` §3, §3.1, §3.2, §3.3 — the full account
