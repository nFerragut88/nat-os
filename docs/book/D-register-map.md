# Appendix D — Address and Register Map

Every hardware address this kernel touches, with the file that owns it and the
chapter that explains it.

**Evidence grading** (Chapter 0b): **M** = measured on hardware, **T** =
transcribed from a vendor header or manual, **R** = reverse-engineered or
identified behaviourally.

---

## D.1 Memory regions

| Region | Range | Grade | Chapter |
|---|---|---|---|
| SRAM0 (IRAM view) | `0x40070000`–`0x400A0000` | T | 4 |
| SRAM1 (DRAM view) | `0x3FFE0000`–`0x40000000` | T | 4 |
| SRAM1 (IRAM view) | `0x400A0000`–`0x400C0000` | T | 4 |
| SRAM2 (DRAM view) | `0x3FFAE000`–`0x3FFE0000` | T | 4 |
| Flash instruction (XIP) | `0x400D0000`+ | T | 4 |
| Flash read-only data | `0x3F400000`+ | T | 4 |

### What the linker script claims

| Region | Origin | Length | Grade |
|---|---|---|---|
| `iram` | `0x40080000` | `0x20000` (128 KB) | **M** — confirmed by the 24 KB span experiment |
| `dram` | `0x3FFB0000` | `0x2C200` (~176 KB) | **M** — stack top read back as `0x3FFDC200` |
| `drom` | `0x3F400020` | `0x400000 − 0x20` | **M** — every printed string is `.rodata` |

The `0x20` offset makes the virtual address congruent with the flash offset
across a 64 KB MMU page: 24-byte image header plus 8-byte segment header.

### Symbols the linker defines

| Symbol | Meaning |
|---|---|
| `_vecbase` | Start of `.vectors`; 1024-byte aligned |
| `_bss_start`, `_bss_end` | Zeroed by `start.S` |
| `_rodata_start`, `_rodata_end` | Flash-mapped read-only data |
| `_stack_top` | `ORIGIN(dram) + LENGTH(dram)` = `0x3FFDC200` |
| `_boot_stack_size` | 4 KB, permanently reserved |
| `_heap_start`, `_heap_end` | Everything between `.bss` and the boot stack |

## D.2 Vector offsets from `VECBASE`

| Offset | Slot | Populated |
|---|---|---|
| `0x000` | Window overflow 4 | yes |
| `0x040` | Window underflow 4 | yes |
| `0x080` | Window overflow 8 | yes |
| `0x0C0` | Window underflow 8 | yes |
| `0x100` | Window overflow 12 | yes |
| `0x140` | Window underflow 12 | yes |
| `0x180` | Level 2 | — |
| `0x1C0` | **Level 3** | yes → `_handler_level3` |
| `0x200` | Level 4 | — |
| `0x240` | Level 5 | — |
| `0x280` | Debug | — |
| `0x2C0` | NMI | — |
| `0x300` | **Kernel exception** | yes → `_handler_panic` |
| `0x340` | **User exception** | yes → `_handler_panic` |
| `0x3C0` | **Double exception** | yes → `_handler_panic` |

Each slot is 64 bytes and holds a single `j`. Verified in the linked ELF with
`nm` *before* flashing (Chapter 4 §4.3).

## D.3 Flash layout

| Offset | Contents | Size |
|---|---|---|
| `0x1000` | Second-stage bootloader (borrowed) | 17,536 B |
| `0x8000` | Partition table (borrowed) | 3,072 B |
| `0x10000` | nat-os image | 37,248 B |
| `0x200000` | Persistent record sector | 4,096 B |
| `0x201000` | Message store sector | 4,096 B |

## D.4 Xtensa special registers

Accessed through `xtensa.h`, which bakes in the required synchronisation.

| Register | Access | Sync | Use |
|---|---|---|---|
| `CCOUNT` | read | — | Free-running cycle counter |
| `CCOMPARE1` | read/write | `ESYNC` | The tick; writing also acknowledges |
| `INTENABLE` | read/write | `ESYNC` | Per-line enable mask |
| `INTERRUPT` | read | — | Lines asserting now, regardless of enable |
| `PS` | read/write | `RSYNC` | `INTLEVEL` in bits 3:0, `EXCM` bit 4 |
| `VECBASE` | read/write | `ISYNC` | Vector table base |
| `EPC1` / `EPC3` | read | — | Interrupted PC (exception / level 3) |
| `EPS3` | read | — | Interrupted PS |
| `EXCCAUSE` | read | — | Exception cause code |
| `SAR` | read/write | — | Shift amount; saved in the frame |
| `LBEG`/`LEND`/`LCOUNT` | read/write | — | Zero-overhead loop; saved in the frame |
| `WINDOWBASE`/`WINDOWSTART` | read/write | `RSYNC` | Windowed excursions only |

**Interrupt sources and lines:**

| Constant | Value | Meaning |
|---|---|---|
| `XT_TIMER1_INTERRUPT` | 15 | CCOMPARE1, internal, level 3 |
| `INTR_LINE_GPIO` | 23 | Level 3, **level-triggered** |
| `INTR_LINE_WIFI_MAC` | 27 | Level 3, level-triggered |
| `INTR_SRC_GPIO_PRO` | 22 | The GPIO peripheral source |
| `INTR_SRC_WIFI_MAC` | 0 | First entry in the silicon's table |

## D.5 DPORT

| Address | Register | Use | Chapter |
|---|---|---|---|
| `0x3FF00104` + 4×src | PRO CPU interrupt map | One word per source; holds a line number | 22 |
| `0x3FF000C0` | `PERIP_CLK_EN` | SPI2 bit 6, LEDC bit 11, SPI DMA bit 22 | 18, 23 |
| `0x3FF000C4` | `PERIP_RST_EN` | Same bit assignments | 18, 23 |
| `0x3FF000CC` | `WIFI_CLK_EN` | WiFi/BT common clock, mask `0x3C9` | 27 |
| `0x3FF005A8` | `SPI_DMA_CHAN_SEL` | Bits 2:3 select SPI2's DMA channel | 18 |

> Bit 1 in the same register clocks the flash controller this code executes
> from, so the write is read-modify-write and never a plain store.

## D.6 GPIO

| Address | Register | Bank |
|---|---|---|
| `0x3FF44008` / `0x3FF4400C` | `OUT_W1TS` / `OUT_W1TC` | 0–31 |
| `0x3FF44014` / `0x3FF44018` | `OUT1_W1TS` / `OUT1_W1TC` | 32–39 |
| `0x3FF44024` / `0x3FF44028` | `ENABLE_W1TS` / `_W1TC` | 0–31 |
| `0x3FF44030` / `0x3FF44034` | `ENABLE1_W1TS` / `_W1TC` | 32–39 |
| `0x3FF4403C` / `0x3FF44040` | `IN` / `IN1` | |
| `0x3FF44044` / `0x3FF4404C` | `STATUS` / `STATUS_W1TC` | 0–31 |
| `0x3FF44050` / `0x3FF44058` | `STATUS1` / `STATUS1_W1TC` | 32–39 |
| `0x3FF44054` | `STATUS1_W1TS` | Edge injection — Chapter 22 §22.5 |
| `0x3FF44088` + 4×n | `GPIO_PIN<n>` | `INT_TYPE` 9:7, `INT_ENA` 17:13 |
| `0x3FF44530` + 4×n | `FUNC<n>_OUT_SEL_CFG` | 256 = `SIG_GPIO_OUT_IDX` |

**`GPIO_PIN_INT_ENA_PRO` is bit 15** — **measured**, not read. Bit 13 (the field's
low bit, matching the vendor header) delivers to the APP CPU, which this kernel
never starts.

### IO_MUX — not in pin order

| Pin | Address | Pin | Address |
|---|---|---|---|
| GPIO2 | `0x3FF49040` | GPIO25 | `0x3FF49024` |
| GPIO12 | `0x3FF49034` | GPIO26 | `0x3FF49028` |
| GPIO13 | `0x3FF49038` | GPIO27 | `0x3FF4902C` |
| GPIO14 | `0x3FF49030` | GPIO32 | `0x3FF4901C` |
| GPIO15 | `0x3FF4903C` | GPIO33 | `0x3FF49020` |
| GPIO21 | `0x3FF4907C` | GPIO36 | `0x3FF49004` (SENSOR_VP) |
| GPIO22 | `0x3FF49080` | GPIO39 | `0x3FF49010` (SENSOR_VN) |

**The trap:** `0x84` is `U0RXD` and `0x88` is `U0TXD`, between GPIO22 (`0x80`)
and GPIO23 (`0x8C`). Counting up from GPIO21 puts GPIO23 on the console's receive
pad (Chapter 21 §21.4).

**Fields:** `MCU_SEL` 14:12 (2 = GPIO), `FUN_DRV` 11:10, `FUN_IE` bit 9,
`FUN_PU` bit 8.

## D.7 Peripherals

### UART0

| Address | Register |
|---|---|
| `0x3FF40000` | `FIFO` — write to transmit |
| `0x3FF4001C` | `STATUS` — TX count 23:16, RX count 7:0 |
| **`0x60000000`** | **AHB alias — read to receive** |

The APB address for receive runs exactly one byte behind. Behavioural finding;
no erratum cited (Chapter 12 §12.5).

### SPI1 — flash

`0x3FF42000` base. `CMD` +0x00, `ADDR` +0x04, `CTRL` +0x08, `CTRL2` +0x14,
`CLOCK` +0x18, `USER` +0x1C, `USER1` +0x20, `USER2` +0x24, `MOSI_DLEN` +0x28,
`MISO_DLEN` +0x2C, `W(n)` +0x80.

Six registers saved and restored per transaction, **including on the failure
path**. Clock set explicitly to ~8 MHz; the inherited cache-read divider samples
MISO one clock early.

### SPI2 — display

`0x3FF64000` base, same layout. Plus DMA: `DMA_CONF` +0x100, `DMA_OUT_LINK`
+0x104, `DMA_INT_CLR` +0x11C.

`SPI2_CLKDIV = 0x00001001` — sysclk/2, 40 MHz. The ceiling on this board,
established by trying 80 MHz and looking at the panel.

### Watchdogs

| Address | Register |
|---|---|
| `0x3FF4808C` / `0x3FF480A4` | RTC `WDTCONFIG0` / `WDTWPROTECT` |
| `0x3FF5F048` / `0x3FF5F064` | TIMG0 `WDTCONFIG0` / `WDTWPROTECT` |
| `0x3FF5F04C` / `0x3FF5F050` / `0x3FF5F060` | TIMG0 prescaler / stage 0 / feed |
| `0x3FF60048` / `0x3FF60064` | TIMG1 `WDTCONFIG0` / `WDTWPROTECT` |

Write-protect key `0x50D83AA1`, shared by all three. Re-locked after every
change.

### SAR ADC1

`0x3FF48800` (SENS) base. `READ_CTRL` +0x0000, `MEAS_WAIT2` +0x000C,
`START_FORCE` +0x002C, `ATTEN1` +0x0034, `MEAS_START1` +0x0054.

**`SAR1_EN_PAD_FORCE` is bit 31** — written as bit 27 from memory, which is
*inside* the twelve-bit `SAR1_EN_PAD` field at shift 19 (Chapter 22 §22.10).

RTC IO base `0x3FF48400`; `PAD_DAC1` +0x84, `PAD_DAC2` +0x88.

### LEDC

`0x3FF59000` base. `HSCH0_CONF0` +0x0000, `HPOINT` +0x0004, `DUTY` +0x0008,
`CONF1` +0x000C, `HSTIMER0_CONF` +0x0140, `CONF` +0x0190.

GPIO matrix signal 71 = `LEDC_HS_SIG_OUT0`. **Clock-gated out of reset** — DPORT
bit 11.

### eFuse

`0x3FF5A004` = `BLK0_RDATA1` (MAC[31:0]), `0x3FF5A008` = `BLK0_RDATA2`
(MAC[47:32] low half, CRC8 bits 23:16). **Documented**, unlike the rest of the
WiFi path.

### WiFi MAC

`0x3FF73000` base. `CTRL` `0x3FF73CB8`. **TSF `0x3FF73C00`** — identified
behaviourally as the only word advancing at exactly 1000 kHz across repeated
samples, confirmed against CCOUNT to 0.1% over half a second.

Reverse-engineered from open-mac except where noted.

## D.8 Board pin assignments

| Function | Pins |
|---|---|
| Display | SCLK 14, MOSI 13, MISO 12, CS 15, DC 2, BL 21 |
| Touch | CLK 25, MOSI 32, MISO 39, CS 33, IRQ 36 |
| microSD | CS 5, SCK 18, MISO 19, MOSI 23 |
| Light sensor | 34 (ADC1 channel 6) |
| Speaker | 26 |
| I²C | SDA 22, SCL 27 |
| RGB LED | 4, 16, 17 |
| Console | U0TXD 1, U0RXD 3 |

GPIO 34–39 are **input only**, which is why touch MISO and IRQ sit on 39 and 36 —
"they can never be driven by mistake".

Every other pin is spoken for. SDA 22 and SCL 27 are the only two brought out to
a header and unclaimed.

## D.9 Magic constants

| Constant | Value | Purpose |
|---|---|---|
| `STACK_GUARD` | `0x57ACC0DE` | Task stack base word |
| `STACK_FILL` | `0xEEEEEEEE` | Untouched-stack pattern |
| `BLK_FREE` | `0xF2EEB10C` | Free heap block |
| `BLK_USED` | `0x05EDB10C` | Live heap block |
| `MUTEX_FREE` | `-2` | Unheld — **not −1**, which is the boot context |
| `STORE_MAGIC` | `0x59444F53` | `"SODY"` |
| `WDT_WKEY` | `0x50D83AA1` | Watchdog write-protect key |
| `H_TAG` | `0x05100000` | WiFi OSI handle tag |
| M0 `.data` canary | `0xC0DEFACE` | Proves `.data` was copied |
| `smash` value | `0xDEADBEEF` | Deliberate guard corruption |

Every one is chosen to be implausible as data, so its corruption announces itself
rather than requiring interpretation.
