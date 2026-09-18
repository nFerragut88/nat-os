# vendor/minimp3

An MP3 (MPEG-1/2/2.5 Layer III) decoder by lieff. **CC0 / public domain**
(see `LICENSE`). Source, not a blob. Vendored for the MP3 player,
`docs/next_moves/11-mp3-player.md`, on the user's decision not to write a
Layer III decoder from scratch.

- Upstream: https://github.com/lieff/minimp3
- Commit: `ea99364f61c14656440e8d77e9c233ccf3124633` (fetched 2026-09-18)
- `minimp3.h` sha256 as fetched: `57e437c5c1f0e8b243885d3929c8973b5e6c778451e0100ab4251d19915cb3ad`

## The one local change

`mp3dec_decode_frame()` declares its `mp3dec_scratch_t` (about 16 KB) as a
local. On a nat-os task stack that overflows immediately. The change wraps that
one declaration:

```c
#ifdef MINIMP3_STATIC_SCRATCH
    static mp3dec_scratch_t scratch MINIMP3_STATIC_SCRATCH;
#else
    mp3dec_scratch_t scratch;
#endif
```

`kernel/mp3.c` defines `MINIMP3_STATIC_SCRATCH` as
`__attribute__((section(".sram1")))`. Consequence: **the decoder is not
reentrant.** One decode at a time, from one task. Without the macro the file is
upstream's.

## How it is built

It is header-only and is not compiled on its own. `kernel/mp3.c` includes it
with `MINIMP3_IMPLEMENTATION`, `MINIMP3_ONLY_MP3` (no Layer I/II) and
`MINIMP3_NO_SIMD`.

It uses the **hardware FPU**. `build.ps1` checks that no other object contains
FPU instructions, because nat-os does not save FPU registers on a task switch.
That is safe only while exactly one task uses them.

It needs `__divsf3` (float division, normally from libgcc, which nat-os does
not link). `kernel/mp3.c` supplies one; its limits are stated there.
