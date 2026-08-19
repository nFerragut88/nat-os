# nat-os build — compile, link, and package a bootable image.
#
# The kernel is entirely our own code; only the 2nd-stage bootloader and the
# partition table are borrowed (from the CYD PlatformIO project) so we don't
# spend the first week on silicon bring-up. Swap those out later if you decide
# to own L1 too.
#
#   .\build.ps1              # build only
#   .\build.ps1 -Flash       # build, then flash over COM5
#   .\build.ps1 -Flash -Port COM6 -Monitor

param(
    [switch]$Flash,
    [switch]$Monitor,
    [string]$Port = "COM5",
    [string]$Vendor,           # bootloader/partition source; defaults to vendor/

    # Which board this image is for. Selects a pin map and a set of fitted
    # peripherals from kernel/board_<name>.h.
    #
    #   cyd      ESP32-2432S028R, the display board everything was measured on
    #   lora32   an ESP32 + SX1262 relay node -- PIN MAP NOT YET VERIFIED
    [ValidateSet("cyd", "lora32")]
    [string]$Board = "cyd"
)

$ErrorActionPreference = "Stop"
$root = $PSScriptRoot
$build = Join-Path $root "build"
# The second-stage bootloader and partition table. Both live in vendor/ so a
# fresh clone builds with nothing else installed; see vendor/README.md for what
# they are and how to rebuild them yourself. Override with -Vendor <path> to use
# artefacts from your own PlatformIO build instead.
$borrowed = if ($Vendor) { $Vendor } else { Join-Path $root "vendor" }

function Find-Tool($pattern) {
    $hit = Get-ChildItem "$env:USERPROFILE\.platformio\packages\$pattern" -ErrorAction SilentlyContinue |
           Select-Object -First 1
    if (-not $hit) { throw "could not find $pattern" }
    return $hit.FullName
}

$gcc     = Find-Tool "toolchain-xtensa-esp32\bin\xtensa-esp32-elf-gcc.exe"
$objcopy = Find-Tool "toolchain-xtensa-esp32\bin\xtensa-esp32-elf-objcopy.exe"
$size    = Find-Tool "toolchain-xtensa-esp32\bin\xtensa-esp32-elf-size.exe"
$esptool = Find-Tool "tool-esptoolpy\esptool.py"
# PlatformIO's own interpreter — it already has pyserial, which esptool needs
# even for offline elf2image (its loader module imports serial unconditionally).
$python = "$env:USERPROFILE\.platformio\penv\Scripts\python.exe"
if (-not (Test-Path $python)) { $python = "python" }

New-Item -ItemType Directory -Force -Path $build | Out-Null

# -mabi=call0        : no register windows — the whole point (see start.S)
# -mtext-section-literals : keep literal pools in .text so they land in IRAM
# -ffreestanding     : no libc assumptions, we have no runtime
# -fno-builtin       : don't let gcc emit calls to memcpy/memset we don't have
$cflags = @(
    "-mabi=call0", "-mtext-section-literals", "-mlongcalls",
    "-ffreestanding", "-fno-builtin", "-fno-stack-protector",
    # Stops GCC rewriting a hand-written copy loop into a call to memcpy —
    # which, inside memcpy itself, is silent infinite recursion. See kstring.c.
    "-fno-tree-loop-distribute-patterns",
    "-Os", "-Wall", "-Wextra", "-std=c11",
    "-I", "$root\kernel",
    "-DBOARD_$($Board.ToUpper())"
)

Write-Host "== board: $Board ==" -ForegroundColor Cyan

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

Write-Host "== compiling ==" -ForegroundColor Cyan
$objs = @()
foreach ($src in (Get-ChildItem "$root\kernel" -Include *.c,*.S -Recurse)) {
    $obj = Join-Path $build ($src.BaseName + ".o")
    Write-Host ("  {0}" -f $src.Name)
    & $gcc @cflags -c $src.FullName -o $obj
    if ($LASTEXITCODE -ne 0) { throw "compile failed: $($src.Name)" }
    $objs += $obj
}

# Objects built for the WINDOWED ABI, linked alongside the call0 kernel.
#
# Everything in kernel/ is -mabi=call0 and always will be. This directory holds
# code that is not: the ABI every precompiled Espressif library uses. Mixing the
# two in one image is a link-time question, not a compile-time one, and the
# register-window handlers in kernel/window.S are what make the resulting calls
# actually work at run time.
$winsrc = Get-ChildItem "$root\vendor\windowed\*.c" -ErrorAction SilentlyContinue
if ($winsrc) {
    Write-Host "== compiling windowed ABI ==" -ForegroundColor Cyan
    $wflags = @("-mabi=windowed", "-mlongcalls", "-ffreestanding", "-fno-builtin",
                "-fno-stack-protector", "-Os", "-Wall", "-Wextra", "-std=c11")
    foreach ($src in $winsrc) {
        $obj = Join-Path $build ($src.BaseName + ".o")
        Write-Host ("  {0}  [windowed]" -f $src.Name)
        & $gcc @wflags -c $src.FullName -o $obj
        if ($LASTEXITCODE -ne 0) { throw "compile failed: $($src.Name)" }
        $objs += $obj
    }
}

Write-Host "== linking ==" -ForegroundColor Cyan
$elf = Join-Path $build "natos.elf"
# Quote every -Wl,... argument: PowerShell otherwise reads the comma as an
# array separator and the parser dies before gcc is ever invoked.
# Espressif's radio blob and the ROM symbol tables it resolves against.
#
# Linked normally, NOT --whole-archive: the linker pulls in only the objects
# actually referenced, so a build that calls one small function costs a few KB
# rather than libphy's full 56 KB. vendor/phy/README.md has the measurement.
#
# The ROM scripts are pure address assignments -- memcpy, sprintf, soft-float
# helpers and hundreds of others already burned into the chip. libgcc supplies
# __divsf3, the one helper the ROM lacks.
$sdk = "$env:USERPROFILE\.platformio\packages\framework-arduinoespressif32\tools\sdk\esp32"
$phylibs = @()
if (Test-Path "$root\vendor\phy\libphy_natos.a") {
    # ONLY esp32.rom.ld, and only the patched archive.
    #
    # The first attempt linked Espressif's newlib and libgcc ROM scripts too,
    # which define memcpy and sprintf by BARE ASSIGNMENT rather than PROVIDE.
    # That silently redirected the kernel's own call0 memcpy to a windowed ROM
    # routine and panicked the board on boot.
    #
    # libphy_natos.a is libphy.a with its memcpy, sprintf and soft-float
    # references renamed by objcopy, answered instead in vendor/windowed/. So no
    # script needs to supply them, and esp32.rom.ld -- whose every entry is
    # PROVIDE, verified -- is the only one linked. Nothing the kernel defines
    # can be displaced, because nothing strong is defined at all.
    # libpp_natos.a goes BEFORE libphy: it is the caller, and a static archive
    # only satisfies references the linker has already seen to its left.
    # Listing it after would leave ic_mac_init and friends unresolved even
    # though they are sitting in the archive.
    #
    # Repeated at the end too, because the two archives call each other and a
    # single pass in either order leaves something behind. Cheaper to reason
    # about than --start-group, and equivalent for two libraries.
    $phylibs = @(
        "$root\vendor\phy\libpp_natos.a",
        "$root\vendor\phy\libphy_natos.a",
        "$root\vendor\phy\libpp_natos.a",
        "-T", "$sdk\ld\esp32.rom.ld"
    )
    Write-Host "  linking libpp_natos.a + libphy_natos.a + esp32.rom.ld" -ForegroundColor DarkGray
}

$ldflags = @(
    "-mabi=call0", "-nostdlib", "-nostartfiles",
    "-Wl,--gc-sections",
    "-Wl,-Map,$build\natos.map",
    "-T", "$root\kernel\linker.ld"
)
& $gcc @ldflags -o $elf @objs @phylibs
if ($LASTEXITCODE -ne 0) { throw "link failed" }

& $size $elf

Write-Host "== packaging image ==" -ForegroundColor Cyan
$bin = Join-Path $build "natos.bin"
& $python $esptool --chip esp32 elf2image --flash_mode dio --flash_freq 40m --flash_size 4MB -o $bin $elf
if ($LASTEXITCODE -ne 0) { throw "elf2image failed" }
Write-Host ("  image: {0:N0} bytes" -f (Get-Item $bin).Length)

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

if ($Monitor) {
    Write-Host "== monitor $Port (Ctrl+C to exit) ==" -ForegroundColor Cyan
    & $python -m serial.tools.miniterm $Port 115200
}
