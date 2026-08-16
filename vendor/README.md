# vendor/ — the two borrowed binaries

nat-os writes every instruction from the image entry point onward. These two
files are the exception, and they are here so the repository builds without
anything else installed.

| file | size | sha256 (first 32) |
|---|---|---|
| `bootloader.bin` | 17536 B | `3d234a7471f67b013686dabd4dee7c1f` |
| `partitions.bin` | 3072 B | `6a88d59601a83a16a19a08114b59d338` |

## What they are

- **`bootloader.bin`** — the ESP32 second-stage bootloader. Flashed at `0x1000`.
  It initialises the SPI flash controller, programs the flash MMU so read-only
  data maps into the address space, enables the cache, and jumps to the image
  entry point. UM-NATOS-002 documents the handoff; UM-NATOS-011 documents what
  the kernel depends on it having done.
- **`partitions.bin`** — the partition table. Flashed at `0x8000`. The
  bootloader reads it to find the application image at `0x10000`.

## Where they came from

Built by PlatformIO for a stock `esp32dev` project — they are unmodified
ESP-IDF build artefacts, licensed **Apache 2.0** by Espressif. They are
redistributed here under that licence; nat-os's own MIT licence does not apply
to them.

## Why they are borrowed rather than written

UM-NATOS-001 §3 makes the argument in full. In short: replacing them means
writing flash-controller and MMU bring-up before anything else can run, which
is weeks of work whose only reward is arriving back where this project already
starts. The interface to them is the image header alone, so replacing them
later changes nothing above.

## Reproducing them yourself

If you would rather not trust a binary in a repository — a reasonable
position — build any stock ESP32 PlatformIO project and take the two files from
`.pio/build/<env>/`:

```ini
[env:esp32dev]
platform = espressif32
board = esp32dev
framework = arduino
```

Then point `build.ps1` at them with `-Vendor <path>`.
