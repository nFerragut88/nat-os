# Chapter 5 — The Build Pipeline

> Sources: `docs/UM-NATOS-005-build-pipeline.md`
> Code: `build.ps1`, `tools/vasm.py`, `kernel/linker.ld`, `vendor/README.md`

---

## 5.1 PlatformIO as a toolchain, not a build system

PlatformIO is a **build orchestrator and package manager**. It downloads
toolchains, resolves libraries, invokes the compiler and linker, and calls
`esptool`. Nothing it produces is required on the target.

nat-os uses it as a **source of tools**:

| Tool | Path |
|---|---|
| `xtensa-esp32-elf-gcc` (14.2.0) | `~/.platformio/packages/toolchain-xtensa-esp32/bin/` |
| `xtensa-esp32-elf-objcopy`, `-objdump`, `-size` | same |
| `esptool.py` (4.5.1) | `~/.platformio/packages/tool-esptoolpy/` |
| Python with `pyserial` | `~/.platformio/penv/Scripts/python.exe` |

It is **not** used to build, for one reason:

> PlatformIO builds are organised around a `framework` (`arduino` or `espidf`),
> and each framework links a substantial runtime — FreeRTOS, a heap, a C
> library, and a startup path that ends by calling user code. nat-os replaces
> all of that. The framework is therefore the obstacle rather than the
> foundation, and `-nostdlib -nostartfiles` says precisely that.

A framework-less PlatformIO configuration is possible but works against the
tool's assumptions. The report states the preference plainly:

> For kernel development, every flag should be visible rather than inferred,
> which a 90-line script achieves and a `platformio.ini` does not.

The discovery is defensive — a missing tool is a named failure rather than a
confusing one:

```powershell
function Find-Tool($pattern) {
    $hit = Get-ChildItem "$env:USERPROFILE\.platformio\packages\$pattern" -ErrorAction SilentlyContinue |
           Select-Object -First 1
    if (-not $hit) { throw "could not find $pattern" }
    return $hit.FullName
}
```

## 5.2 The stages

```
tools/*.vasm
      │  python tools/vasm.py
      ▼
kernel/generated/*.h        ← build products, not sources

kernel/*.c, *.S             vendor/windowed/*.c
      │  gcc -mabi=call0 -c        │  gcc -mabi=windowed -c
      ▼                            ▼
build/*.o  ─────────────────────── build/*.o
      │  gcc -T linker.ld  [+ vendor/phy/*.a  -T esp32.rom.ld]
      ▼
build/natos.elf            ← real addresses assigned here
      │  esptool elf2image
      ▼
build/natos.bin            ← bootloader-compatible image
      │  esptool write_flash 0x10000
      ▼
target board
```

Five stages, of which two did not exist when UM-NATOS-005 was written: bytecode
assembly (Chapter 15) and the windowed-ABI compile (Chapter 2 §2.8).

## 5.3 Compiler flags, and why each one

```powershell
$cflags = @(
    "-mabi=call0", "-mtext-section-literals", "-mlongcalls",
    "-ffreestanding", "-fno-builtin", "-fno-stack-protector",
    "-fno-tree-loop-distribute-patterns",
    "-Os", "-Wall", "-Wextra", "-std=c11",
    "-I", "$root\kernel"
)
```

| Flag | Purpose |
|---|---|
| `-mabi=call0` | Select the non-windowed ABI. **The load-bearing flag** — Chapter 2 |
| `-mtext-section-literals` | Place literal pools inside `.text`. Without this they land in a separate section the linker script does not map into IRAM, and `l32r` loads fault |
| `-mlongcalls` | Permit calls beyond the short-displacement range; required once code spans more than a small region |
| `-ffreestanding` | No hosted C environment — no `main` convention, no library assumptions |
| `-fno-builtin` | Prevent GCC replacing loops with calls to `memcpy`/`memset`, which do not exist in this link |
| `-fno-stack-protector` | The stack guard requires runtime support that does not exist |
| `-fno-tree-loop-distribute-patterns` | §5.4 |
| `-Os` | Optimise for size; IRAM is the scarce resource |
| `-Wall -Wextra` | In a kernel, an unused-variable warning is often a real bug |
| `-std=c11` | Explicit language version |

## 5.4 The flag that prevents infinite recursion

`-fno-tree-loop-distribute-patterns` earns its own section because the failure it
prevents is silent, infinite, and would surface arbitrarily far from its cause.

The chain, from UM-NATOS-012 §7:

1. The link failed on an undefined reference to `memcpy` from a function whose
   source never mentions it. GCC synthesises calls to `memcpy`/`memset` from
   ordinary C — byte-copy loops, struct assignments, large local initialisers —
   and does so **even under `-fno-builtin`**, which only governs the treatment
   of those names when written explicitly.
2. Under `-nostdlib` nothing supplies them. So `kernel/kstring.c` was written to
   provide `memcpy`, `memset`, `memmove` and `memcmp`.
3. And then:

> GCC will recognise the copy loop *inside* `memcpy` as a memcpy and rewrite it
> into a call to itself. That bug is silent, infinitely recursive, and would
> surface as a stack overflow in whatever unrelated code first copied a struct.

The build comment records it in one sentence:

```powershell
    # Stops GCC rewriting a hand-written copy loop into a call to memcpy —
    # which, inside memcpy itself, is silent infinite recursion. See kstring.c.
    "-fno-tree-loop-distribute-patterns",
```

## 5.5 Linker flags

```powershell
$ldflags = @(
    "-mabi=call0", "-nostdlib", "-nostartfiles",
    "-Wl,--gc-sections",
    "-Wl,-Map,$build\natos.map",
    "-T", "$root\kernel\linker.ld"
)
& $gcc @ldflags -o $elf @objs @phylibs
```

| Flag | Purpose |
|---|---|
| `-nostdlib` | Link no standard library |
| `-nostartfiles` | Link no C runtime startup — `_start` is ours |
| `-T kernel/linker.ld` | Our memory map (Chapter 4) |
| `-Wl,--gc-sections` | Discard unreferenced sections |
| `-Wl,-Map,build/natos.map` | The authoritative record of what landed where |
| `-mabi=call0` | Required on the link line too |

Note the quoting comment above them:

```powershell
# Quote every -Wl,... argument: PowerShell otherwise reads the comma as an
# array separator and the parser dies before gcc is ever invoked.
```

That is one of the two bring-up defects UM-NATOS-005 §9 records, and neither
involved kernel source:

| Defect | Symptom | Resolution |
|---|---|---|
| Unquoted `-Wl,` arguments | PowerShell parse error before GCC ran; "Missing argument in parameter list" | Quote every `-Wl,...` string |
| Wrong Python interpreter | `ModuleNotFoundError: No module named 'serial'` during **offline** `elf2image` | Use PlatformIO's `penv` interpreter. `esptool` imports `serial` unconditionally, even for operations that touch no serial port |

The second is handled with a fallback:

```powershell
# PlatformIO's own interpreter — it already has pyserial, which esptool needs
# even for offline elf2image (its loader module imports serial unconditionally).
$python = "$env:USERPROFILE\.platformio\penv\Scripts\python.exe"
if (-not (Test-Path $python)) { $python = "python" }
```

## 5.6 Bytecode assembly

Every `tools/*.vasm` file is assembled to a C header in `kernel/generated/`
before compilation:

```powershell
# Bytecode is assembled on the host: the VM on the device is a pure interpreter
# and carries no assembler. Generated headers are build products, not sources.
$gen = Join-Path $root "kernel\generated"
New-Item -ItemType Directory -Force -Path $gen | Out-Null
$vasm = Get-ChildItem "$root\tools\*.vasm" -ErrorAction SilentlyContinue
if ($vasm) {
    Write-Host "== assembling bytecode ==" -ForegroundColor Cyan
    foreach ($src in $vasm) {
        $hdr = Join-Path $gen ($src.BaseName + ".h")
        & $python "$root\tools\vasm.py" $src.FullName -o $hdr --name ("vm_" + $src.BaseName)
        if ($LASTEXITCODE -ne 0) { throw "vasm failed: $($src.Name)" }
    }
}
```

Twelve programs currently assemble this way. Chapter 15 covers the assembler.

## 5.7 The vendor archive link order

The most intricate part of the build, and the one with the longest comment. Two
static archives that call each other, plus a ROM symbol script:

```powershell
$phylibs = @(
    "$root\vendor\phy\libpp_natos.a",
    "$root\vendor\phy\libphy_natos.a",
    "$root\vendor\phy\libpp_natos.a",
    "-T", "$sdk\ld\esp32.rom.ld"
)
```

Three points, each recorded in the script:

**Not `--whole-archive`.**

```powershell
# Linked normally, NOT --whole-archive: the linker pulls in only the objects
# actually referenced, so a build that calls one small function costs a few KB
# rather than libphy's full 56 KB.
```

That distinction dissolved a constraint that had shaped the WiFi work for weeks
(Chapter 4 §4.8).

**`libpp` before `libphy`, and again after.**

```powershell
# libpp_natos.a goes BEFORE libphy: it is the caller, and a static archive
# only satisfies references the linker has already seen to its left.
# Listing it after would leave ic_mac_init and friends unresolved even
# though they are sitting in the archive.
#
# Repeated at the end too, because the two archives call each other and a
# single pass in either order leaves something behind. Cheaper to reason
# about than --start-group, and equivalent for two libraries.
```

**Only `esp32.rom.ld`, and only the patched archives.** The reason is in
Chapter 2 §2.10: Espressif's newlib and libgcc ROM scripts define `memcpy` and
`sprintf` by *bare assignment* rather than `PROVIDE`, which silently redirected
the kernel's own call0 `memcpy` to a windowed ROM routine and panicked the board
on boot.

## 5.8 Packaging and flashing

```powershell
& $python $esptool --chip esp32 elf2image --flash_mode dio --flash_freq 40m --flash_size 4MB -o $bin $elf
```

Flash write targets three regions, and checks the borrowed binaries are present
first — a named failure rather than a confusing one:

```powershell
if ($Flash) {
    foreach ($f in @("bootloader.bin", "partitions.bin")) {
        if (-not (Test-Path (Join-Path $borrowed $f))) { throw "missing $f in $borrowed - see vendor/README.md" }
    }
    Write-Host "== flashing $Port ==" -ForegroundColor Cyan
    & $python $esptool --chip esp32 --port $Port --baud 460800 write_flash -z `
        0x1000  (Join-Path $borrowed "bootloader.bin") `
        0x8000  (Join-Path $borrowed "partitions.bin") `
        0x10000 $bin
    if ($LASTEXITCODE -ne 0) { throw "flash failed" }
}
```

> **Since written — the first of those three regions is built, not copied.**
> `build.ps1 -Flash` invokes `boot/build_boot.ps1` and flashes
> `boot/build/boot.bin` at `0x1000` (Ch. 3 §3.1.1). `-VendorBootloader` puts
> Espressif's back, which is the recovery path and the A/B control, and
> `-WiFi` selects it automatically with a message saying so, because the PHY
> does not currently come up on ours (Ch. 1 §1.2).
>
> The partition table is still copied from `vendor/` to `0x8000`, still checked
> for before the flash starts, and still read by nothing.
>
> The flash step also now counts its own evidence rather than trusting an exit
> code.

```powershell
$verified = ($flashOut | Select-String -SimpleMatch "Hash of data verified").Count
if ($verified -lt 3) {
    throw "flash did not verify: $verified of 3 segments hashed -- the board may still hold the PREVIOUS image, so do not trust any run against it"
}
```

The comment beside it names the incident that produced it: a flash refused with
"Could not open COM5", the board kept the previous image, and the suite results
printed underneath were read as a result. An exit-code check would not have
caught it, and not because it is wrong — because a human, or a `grep`, can see a
failure and read on anyway. **A count that must reach three turns "I should have
noticed" into "the script stopped".**

## 5.9 Usage

```powershell
.\build.ps1                              # build
.\build.ps1 -Flash -Port COM5            # build and flash (our bootloader)
.\build.ps1 -Flash -VendorBootloader     # Espressif's second stage: recovery, and the A/B control
.\build.ps1 -Flash -Monitor -Port COM5   # build, flash, attach monitor
.\build.ps1 -Vendor <path>               # use your own borrowed artefacts
```

The distinction between the first two lines is not cosmetic. Chapter 3 §3.8 and
Chapter 20 §20.5 record what happens when a debugging session assumes the first
one flashed: two hypotheses tested against a board that had never been reflashed,
producing bit-identical output that was read as "none of these are the cause"
when it meant "no experiment has run yet".

The mitigation is procedural, and it is worth restating here since this is the
build chapter:

> the flash step now always shows its `Hash of data verified.` line, and that
> line is checked before any capture is interpreted.

## 5.10 Known gaps

From UM-NATOS-005 §10, all still open:

- **No dependency tracking.** Every build recompiles every file. That was
  "trivial at three files"; it is now 40 translation units and the full build is
  noticeably slower, though still under a minute.
- **No incremental or parallel build.**
- **No test target.** There is no host-side unit test path; everything is
  verified on hardware. This is the largest gap in the list and the one Chapter
  31 argues hardest for closing — several defects in this book (the ring-buffer
  arithmetic in Chapter 26, the offset-domain bounds checks in Chapter 14) are
  pure functions that a host test could exercise exhaustively.
- **Hard-coded paths.** `build.ps1` contains absolute paths to the toolchain
  root. The borrowed-binary path was fixed by vendoring; the toolchain path was
  not.

One gap was closed. "Hard-coded paths to the borrowed binaries" became
`vendor/` plus a `-Vendor` override, and the assembler learned not to bake an
author's home directory into a generated header:

```python
    if args.output.endswith(".h"):
        # Repo-relative, never absolute. An absolute path bakes the author's
        # home directory into a generated header that is then committed, which
        # is noise in a diff and a small privacy leak in a public repository.
        src_rel = os.path.relpath(os.path.abspath(args.source),
                                  os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
```

---

**Next:** what the first working image actually proved, and the four things it
deliberately did not.
