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
    [string]$Vendor            # bootloader/partition source; defaults to vendor/
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
    "-I", "$root\kernel"
)

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
$ldflags = @(
    "-mabi=call0", "-nostdlib", "-nostartfiles",
    "-Wl,--gc-sections",
    "-Wl,-Map,$build\natos.map",
    "-T", "$root\kernel\linker.ld"
)
& $gcc @ldflags -o $elf @objs
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
