# vendor/net80211 — what Espressif's 802.11 stack asks of its host

**Measured, not estimated**, the same way `../phy/README.md` measured `libphy.a`.
Nothing here is speculative, and nothing here has transmitted anything.

This directory contains **no Espressif binaries**. It contains the probe that
proves the vendor transmit path links against a nat-os-shaped host, and the ten
shims that close it. The archives are read from the local ESP-IDF install.

## The finding

`libnet80211.a` exports 1,189 symbols and references 719 its own objects do not
define — but most resolve inside the archive or against pools that already
exist:

```
TRUE EXTERNAL                                              206
after libpp + libcore + ROM + already-vendored blobs        32
after phy_host.o + librtc.a + libcoexist.a + newlib-nano    10
```

**Ten symbols**, in `net80211_host.c`:

| symbol | implementation |
|---|---|
| `free`, `puts`, `strtok` | libc; nat-os has equivalents |
| `hexstr2bin` | ten lines, written |
| `net80211_printf`, `mesh_printf` | UART or discard |
| `WIFI_EVENT`, `esp_event_handler_register` / `_unregister`, `esp_mesh_send_event_internal` | the event system — the only genuinely new surface |

`coex_bt_high_prio` is in **`librtc.a`**, not `libcoexist.a`. That was asserted
the wrong way round once without checking (UM-NATOS-034 §25.5).

## Verified link

```sh
IDF=$HOME/.platformio/packages/framework-espidf
LIB=$IDF/components/esp_wifi/lib/esp32
ROMLD=$IDF/components/esp_rom/esp32/ld
RTC=$IDF/components/esp_phy/lib/esp32/librtc.a
COEX=$IDF/components/esp_coex/lib/esp32/libcoexist.a

xtensa-esp32-elf-gcc -c -mlongcalls -O2 -o probe.o        probe.c
xtensa-esp32-elf-gcc -c -mlongcalls -O2 -o net80211_host.o net80211_host.c

xtensa-esp32-elf-gcc -nostdlib -nostartfiles -o probe.elf \
  probe.o net80211_host.o ../../build/phy_host.o \
  -Wl,-e,probe_entry -T probe.ld \
  -T $ROMLD/esp32.rom.ld            -T $ROMLD/esp32.rom.libgcc.ld \
  -T $ROMLD/esp32.rom.newlib-funcs.ld -T $ROMLD/esp32.rom.newlib-data.ld \
  -T $ROMLD/esp32.rom.newlib-nano.ld \
  -Wl,--start-group \
     $LIB/libnet80211.a $LIB/libpp.a $LIB/libcore.a $LIB/libmesh.a \
     $RTC $COEX ../phy/libphy_natos.a -lgcc \
  -Wl,--end-group
```

```
link exit=0          0 undefined references
400d2b90 T esp_wifi_80211_tx

   text     data      bss
 545480     4113    16664
```

`.text` is 545 KB because the closure drags `libpp`, `libcore`, `libmesh`,
`librtc`, `libcoexist` and `libphy` — not net80211's 234 KB alone. nat-os
already links two of those, so the marginal cost is smaller and has not been
separated out. The 16,664 B of `.bss` is the DRAM cost, against ~122 KB of free
heap.

## What this does NOT show

**Linking is not transmitting.** `net80211_host.c` says so at the top and it is
worth repeating here, because a closed link is the most persuasive-looking
non-result this project has produced so far.

The event stubs accept a registration and never deliver a callback. That is
plausibly enough for a blind `esp_wifi_80211_tx()`, which is a direct call. It
is definitely not enough for scan, association or receive, which are all
event-driven. That difference is the real remaining scope of `next_moves/08`
and none of it is visible in a symbol count.

An earlier run of this same measurement printed **"0 undefined references"**
when the link had in fact failed for an unrelated reason and the grep simply
matched nothing. The figure only became trustworthy after checking that the ELF
existed and that `esp_wifi_80211_tx` was in it at a real address. A count of
absent errors is not a result.

## Licensing

The archives are Espressif's and are **not** redistributed here. They are read
from the local ESP-IDF install at the paths above. Keeping them out of the tree
is the point of `next_moves/08`'s SD-delivery plan — see `docs/blob-free.md`
for why that is a distribution property and not an engineering one.
