# 09 — LoRa Phase 0: one link

**Size:** medium. **Risk:** low — a documented peripheral with a published
register map. **Blocked on:** the SX1262 hardware arriving.

*This is what the project is for.* `docs/conceptual/the-ark-and-fiendnet.md` §7
calls it Phase 0 and reckons it "answerable in an evening":

> Two boards, SX1262 on SPI3, send a packet, receive it, print it. No mesh, no
> bundles, no encryption. The only question being answered is whether this
> kernel can drive this radio.

---

## Why this and not more WiFi

UM-NATOS-034 §29 closed WiFi transmit as a negative, confirmed on non-Espressif
silicon, after every comparable layer was measured and found matching. The
remaining route ([08](08-wifi-via-loaded-blob.md)) buys ESP-IDF's WiFi stack
running on your board.

The SX1262 has **a published register map, a documented command set, and no
blob.** It is the only path where "working" and "ours" are the same outcome.

## What already exists

| | |
|---|---|
| `kernel/spi3.h` | `spi3_init`, `spi3_route`, `spi3_xfer`, plus two self-tests |
| SPI3 self-tests | `spi3_selftest_const`, `spi3_selftest_loopback` — both pass |
| `kernel/board_lora32.h` | a board profile with `BOARD_HAS_LORA 1` and a pin map |
| the device model | UM-NATOS-031 — a peripheral is a table entry, not a kernel edit |
| persistence | `store.c`, needed by [10](10-lora-bundle.md), already works |
| IRAM headroom | 94 KB free after UM-NATOS-037; a driver will fit |

**`board_lora32.h`'s pin map is marked UNVERIFIED** and its `BOARD_NAME` says so
out loud. Verify it against the board that actually arrives before trusting a
single line of it. A wrong pin map presents as a radio that answers nothing,
which is indistinguishable from a radio that is broken.

## The order to do it in

1. **`GetStatus` (0xC0) before anything else.** One SPI transaction, no
   configuration required, and the reply encodes chip mode and command status.
   It is the cheapest possible proof that the part is powered, selected, and
   talking. Do not configure a single register until this returns something
   plausible.
2. **`ReadRegister` on a known-default register**, as the second, independent
   check — because this project has a standing rule that *a successful register
   write is not evidence*, and the corollary is that a plausible-looking status
   byte from a floating bus is not either. Two different reads agreeing is.
3. **Standby, then RF frequency, modulation params, packet params.** All
   documented; follow the datasheet's order, which exists for a reason.
4. **Transmit one packet.** `SetTx`, then poll `IRQ_TX_DONE`.
5. **Receive it on the second board.** `SetRx`, `IRQ_RX_DONE`, read the buffer.

## Verification, learned the hard way

Everything UM-NATOS-034 cost should be spent once here rather than again:

- **A completion flag is not evidence of radiation.** `IRQ_TX_DONE` means the
  chip finished its state machine. WiFi's `completions reaped=295` meant that
  too, and nothing left the antenna. **The second board receiving is the only
  proof.**
- **Both directions, each with its own control.** A→B and B→A, and each receiver
  must be shown to hear *something* before its silence counts. `link_test.py`
  has the shape already — a receiver that has not proved it can hear is not
  producing negatives, it is producing nothing.
- **Key on the payload, never on a counter.** Put a known byte pattern in the
  packet and match it. `wifi_sweep.py` was retired for keying on a frame-count
  delta.

## The one real hazard

`BOARD_SPARE_PIN` exists because `spi3 loop` once defaulted to GPIO27 — the I²C
clock — and left it driven, which made `i2c` report a broken bus and looked for
an afternoon like a regression in the refactor. `board_pin_claimed()` is there to
stop exactly that. **Use it when the SX1262 pins are assigned**, and do not
assume a pin is free because nothing obvious is on it.

## Where the code will go

- `kernel/lora.c` / `lora.h` — the driver, modelled on `spi3.c`'s shape
- `kernel/device.c` — register it as a device so applications reach it through
  the permission-gated table rather than directly (UM-NATOS-032)
- `kernel/board_lora32.h` — the verified pin map
- `tools/serial/` — a two-board link test, following `link_test.py`

## Related

- `docs/conceptual/the-ark-and-fiendnet.md` §7 — the build order this serves
- UM-NATOS-034 §29 — why WiFi is not this
- UM-NATOS-031 — the device model
- [10](10-lora-bundle.md) — what this unblocks
