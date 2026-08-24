# Chapter 21 — microSD, and a Pad Table That Is Not in Pin Order

> Sources: `docs/UM-NATOS-020-sdcard.md`
> Code: `kernel/sd.c`, `kernel/sd.h`, `kernel/gpio.h`

---

## 21.1 The first storage a person can carry away

> Flash holds what the kernel was built with; this holds what the user brought.

The driver was straightforward. One constant was not, and it is the reason this
chapter exists:

> the ESP32's IO_MUX pad table is **not in pin order**, and the two UART0 pads
> sit between GPIO22 and GPIO23. Counting up from a known entry put GPIO23 at
> the address of `U0RXD`, so initialising the SD bus reconfigured the console's
> receive pin as a GPIO output. Transmit kept working, so the board went on
> printing telemetry and simply stopped answering.

## 21.2 Why SPI mode

The card supports a 4-bit parallel protocol several times faster. This driver
uses 1-bit SPI mode, for three reasons stated in `sd.h`:

```c
 *   - it is the mode every card must support, so a card that fails here is
 *     faulty rather than unsupported
 *   - the CYD wires the slot to ordinary GPIOs, not to the ESP32's dedicated
 *     SDMMC pins, so the fast path is not available on this board anyway
 *   - this project has now brought up three SPI devices, and reusing a shape
 *     that is understood beats learning a protocol whose failures are new
```

### Bit-banged first, again

```c
 * As with the display (UM-NATOS-015 §3), the first version bit-bangs. Card
 * initialisation is a one-time cost measured in milliseconds and is required to
 * run at 400 kHz or slower, which no hardware peripheral makes easier. Sector
 * reads can move to SPI3 once something is proven to be reading the right
 * bytes; moving first would mean debugging a protocol and a peripheral at the
 * same time, which is how the display cost three commits to one defect.
```

Third application of the same staging argument (Chapter 18 §18.3, Chapter 22
§22.7), and by this point it is a stated project convention rather than a case-by-
case judgement.

### Pins, and a caveat attached

```c
 * Taken from the ESP32-2432S028R board design, which puts the slot on the VSPI
 * default pads. These are ASSUMED until a card answers CMD0 — a wrong pin map
 * and an absent card are the same silence, so sd_init() reports which stage
 * failed rather than a single boolean.
 */

#define SD_PIN_CS    5u
#define SD_PIN_SCK  18u
#define SD_PIN_MISO 19u
#define SD_PIN_MOSI 23u
```

"These are ASSUMED until a card answers CMD0" is exactly the kind of labelling
Chapter 0b describes: a transcribed value marked as transcribed, with the
condition that would upgrade it to measured.

## 21.3 One error code per stage

```c
/* Distinct codes per failure stage. "No card" and "wrong wiring" and "card
 * present but refuses the voltage range" are three different problems, and a
 * driver that returns -1 for all of them makes the user guess. */
typedef enum {
    SD_OK           =  0,
    SD_ERR_IDLE     = -1,   /* no answer to CMD0 — absent card, or bad wiring */
    SD_ERR_IFCOND   = -2,   /* CMD8 rejected — pre-2.0 card, or a bad bus     */
    SD_ERR_READY    = -3,   /* ACMD41 never completed initialisation          */
    SD_ERR_OCR      = -4,   /* CMD58 failed, so addressing mode is unknown    */
    SD_ERR_BLOCKLEN = -5,   /* CMD16 refused on a byte-addressed card         */
    SD_ERR_READ     = -6,   /* CMD17 refused                                  */
    SD_ERR_TOKEN    = -7,   /* card never sent a data token                   */
} sd_err_t;
```

Seven codes for a driver whose whole job is "read a block". Each one names a
distinct thing to check next.

### Every wait is bounded, and the reason is not defensive habit

> **Every wait is bounded**, and that is not defensive habit — the slot is
> normally *empty*. A probe that hangs on an absent card would be worse than no
> driver at all, because the normal case would be the fatal one.

Verified:

```
sd_init FAILED  type=none  last R1=0x000000ff
stage: CMD0 - no card, or MISO/CS/SCK wrong
reporter lines after a failed probe: 4      <- kernel still running
```

That last line is the assertion. A failed probe that leaves the kernel running is
the requirement; a driver that reports the failure and then wedges would have
passed a naive reading of the first two lines.

> `R1 = 0xFF` is unambiguous: bit 7 of a real R1 is always zero, so "no answer"
> cannot be confused with any legal response.

Another instance of *choose a sentinel that cannot be a legal value* — the same
reasoning as the heap's magic words and `MUTEX_FREE == -2`.

`kmain` probes at boot and treats failure as non-fatal, with the reasoning
recorded:

```c
    /* Probe the card at boot, so storage is ready before anything asks for it
     * and so an absent card is reported once rather than discovered later by
     * whatever needed it. A failure here is not fatal: the slot is normally
     * empty, and every wait inside sd_init() is bounded for exactly that
     * reason. */
    uart_puts("  sd           : ");
    {
        int sd_rc = sd_init();
        if (sd_rc == SD_OK) {
            uart_puts(sd_type() == SD_TYPE_SDHC ? "SDHC ready" : "SDSC ready");
        } else if (sd_rc == SD_ERR_IDLE) {
            uart_puts("no card");
        } else {
            uart_puts("present but init failed at stage ");
            uart_put_dec((unsigned int)(-sd_rc));
        }
        uart_puts("\n");
    }
```

## 21.4 The pad table, and a check that could not fail

### The table

`gpio_out_init()` takes a pin number *and* its IO_MUX register address, because
the table is not in pin order:

```c
/* IO_MUX pad registers are not in pin order — the table is the silicon's, not
 * ours, so it is written out rather than computed. Only the pins used here. */
#define IO_MUX_GPIO2          0x3FF49040u
#define IO_MUX_GPIO12         0x3FF49034u
#define IO_MUX_GPIO13         0x3FF49038u
#define IO_MUX_GPIO14         0x3FF49030u
#define IO_MUX_GPIO15         0x3FF4903Cu
#define IO_MUX_GPIO21         0x3FF4907Cu
```

The relevant stretch of the real table:

```
0x7C GPIO21    0x80 GPIO22    0x84 U0RXD(GPIO3)    0x88 U0TXD(GPIO1)    0x8C GPIO23
```

> Counting up from GPIO21 while forgetting the two UART pads puts GPIO23 at
> `0x84`, which is the console's **receive** pad. Configuring it as a GPIO output
> kills serial input.

### The symptom named the fault

> Transmit is a different pad (`U0TXD` at `0x88`, untouched), so the board kept
> printing its telemetry at full rate and simply stopped responding to anything
> typed. A shell that echoes nothing looks like a hung shell; the reporter output
> proved the kernel was fine, which narrowed it to the input path immediately.
>
> **That asymmetry was luck.** Had the mistake landed on `0x88` instead, the
> board would have gone silent and looked like a boot failure.

### The verification that agreed with the wrong answer

*This is the most transferable part of the chapter.*

The pin map was checked before flashing, against the four IO_MUX entries already
in `gpio.h`:

| entry | offset |
|---|---|
| GPIO2 | 0x40 |
| GPIO12 | 0x34 |
| GPIO14 | 0x30 |
| GPIO21 | 0x7C |

> All four confirmed the indexing. **All four are below the UART pads**, so every
> one of them agreed with the wrong answer. The check was real, it was performed,
> and **it could not have failed.**

> **A cross-check only tests what its samples can distinguish.** Four entries
> that all sit on the same side of the anomaly confirm the rule and say nothing
> about the exception. The samples must **straddle** the thing being verified.

The report explicitly links it to its sibling:

> This is the same shape as UM-NATOS-017 §7.1, where a calibration inferred axis
> direction from a sample that could only ever give one answer.

Both are cases where a test was *run*, *passed*, and was *incapable of failing*.
Chapter 28 groups them with a third — the audio self-test that ran on the one pin
structurally unable to exhibit the fault.

The positive form of the rule is visible in the VM's bounds cross-check
(Chapter 14 §14.5), where the 35 cases are chosen to include *the exact end, one
past the end, and lengths chosen to wrap the address space* — samples that
straddle the boundary rather than agreeing with it.

## 21.5 Verification: what block 0 does and does not prove

```
sdread 0 -> FA 33 C0 8E D0 BC 00 7C 8B F4 50 07 50 1F FB FC
            BF 00 06 B9 00 01 F2 A5 EA 1D 06 00 00 BE BE 07
            signature=0x55 0xAA
            partition 0: type=0x06 start=240 sectors=490000
```

> `FA 33 C0 8E D0 BC 00 7C` is the textbook MBR bootstrap — `cli`, `xor ax,ax`,
> `mov ss,ax`, `mov sp,0x7C00` — and `BE BE 07` points at the partition table.
> Real data, correctly framed.

And then the caveat, which is the reason the section exists:

> It proves nothing about the **addressing mode**, and that is worth stating
> because it looks like a complete result. Block 0 is byte 0 on a byte-addressed
> card and block 0 on a block-addressed one. The read succeeds either way and
> discriminates nothing.

### The read that does discriminate

```
sdread 240 -> signature=0x55 0xAA
              fs type: FAT16
```

> Those five characters require the pin map, the clock, the addressing mode and
> the block framing to all be correct **simultaneously**. A wrong addressing mode
> would land 512 times too far into the card and return something that is not a
> filesystem header.

A single read at a non-zero LBA is worth more than any number of reads at zero,
because it is the first one whose *result depends on* the property under test.
That is the same principle as the straddling samples in §21.4, applied to a
different question.

And a note on luck:

> The card is ~250 MB and genuinely `SDSC`, which was luck: it exercised the
> byte-addressing path that a modern card would have skipped entirely.

The corresponding gap is recorded: SDHC is untested, the `CCS` bit is read and
the branch exists, and the path a modern card would take has never executed.

## 21.6 Metrics

| Quantity | Value |
|---|---|
| Mode | SPI, 1-bit, bit-banged |
| Identification clock | ~250 kHz (spec requires ≤400 kHz) |
| Error codes | 7, one per stage |
| Unbounded waits | 0 |
| Card under test | ~250 MB, SDSC, FAT16 |
| Partition found | type 0x06, start LBA 240, 490,000 sectors |
| IO_MUX entries cross-checked | 4 |
| **IO_MUX entries that could have caught the fault** | **0** |

That last row is the chapter in one line.

## 21.7 What this does not establish

- **No write path.** `sd_read_block()` only.
- **No filesystem.** The MBR is decoded far enough to find a partition and confirm
  a FAT signature. There is no directory walk, no file lookup, no FAT chain
  traversal. This is the largest missing piece in the storage story and the
  reason "install an application" still has no meaning (Chapter 16 §16.9).
- **No card-removal detection.** Pull the card mid-read and the result is a token
  timeout rather than a clean "card gone" error. Nothing polls for insertion.
- **SDHC is untested.**
- **Still slow.** Bit-banged at every stage including bulk reads. Moving to SPI3
  is deliberate future work, "but nothing here measures what the current
  throughput actually is".
- **The pin map is confirmed only for this board.**

---

**Next:** three peripherals brought up in one session, and four distinct ways to
deliver an interrupt to something that could not act on it.
