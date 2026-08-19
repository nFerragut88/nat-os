# Build the nat-os second-stage bootloader.
#
# Separate from build.ps1 because this is a different program with a different
# link map that happens to share two source files with the kernel. It compiles
# them again rather than linking against the kernel, which is not in memory yet
# -- putting it there is this program's whole job.
param(
    [switch]$Flash,
    [string]$Port = "COM5"
)

$ErrorActionPreference = "Stop"
$root  = Split-Path $PSScriptRoot -Parent
$build = Join-Path $PSScriptRoot "build"
New-Item -ItemType Directory -Force $build | Out-Null

$tc  = "$env:USERPROFILE\.platformio\packages\toolchain-xtensa-esp32\bin"
$gcc = "$tc\xtensa-esp32-elf-gcc.exe"
$py  = "python"
$esptool = "$env:USERPROFILE\.platformio\packages\tool-esptoolpy\esptool.py"

# Same flags as the kernel. -mabi=call0 above all: this jumps into nat-os, and
# two halves of a boot that disagree about the calling convention would fail in
# the register window rather than anywhere informative.
$cflags = @(
    "-mabi=call0", "-mtext-section-literals", "-mlongcalls",
    "-ffreestanding", "-fno-builtin", "-fno-stack-protector",
    "-fno-tree-loop-distribute-patterns",
    "-Os", "-Wall", "-Wextra", "-std=c11",
    "-I", "$PSScriptRoot", "-I", "$root\kernel"
)

# boot.c is ours; flash.c and uart.c are the kernel's, compiled again here.
# flash.c is the reason this project is short rather than long: it drives SPI1
# through its own registers and works with the cache off, which is exactly the
# chicken-and-egg a bootloader normally has to solve from scratch.
$srcs = @(
    "$PSScriptRoot\boot_start.S",
    "$PSScriptRoot\boot.c",
    "$root\kernel\flash.c",
    "$root\kernel\uart.c",
    "$root\kernel\kstring.c"
)

Write-Host "== compiling bootloader ==" -ForegroundColor Cyan
$objs = @()
foreach ($s in $srcs) {
    # GetFileNameWithoutExtension, not Split-Path -LeafBase: this box runs
    # Windows PowerShell 5.1, where that parameter does not exist.
    $o = Join-Path $build ([System.IO.Path]::GetFileNameWithoutExtension($s) + ".o")
    Write-Host ("  {0}" -f (Split-Path $s -Leaf))
    & $gcc @cflags -c $s -o $o
    if ($LASTEXITCODE -ne 0) { throw "compile failed: $s" }
    $objs += $o
}

Write-Host "== linking ==" -ForegroundColor Cyan
$elf = Join-Path $build "boot.elf"
# Arguments in an array, not inline. PowerShell's parser reads the comma in
# `-Wl,--gc-sections` as its own argument separator and fails before gcc ever
# sees it.
$ldflags = @("-mabi=call0", "-nostdlib", "-Wl,--gc-sections",
             "-T", "$PSScriptRoot\boot.ld", "-o", $elf)
& $gcc @ldflags @objs
if ($LASTEXITCODE -ne 0) { throw "link failed" }

& "$tc\xtensa-esp32-elf-size.exe" $elf

$bin = Join-Path $build "boot.bin"
& $py $esptool --chip esp32 elf2image --flash_mode dio --flash_freq 40m --flash_size 4MB -o $bin $elf
if ($LASTEXITCODE -ne 0) { throw "elf2image failed" }
Write-Host ("  image: {0:N0} bytes" -f (Get-Item $bin).Length) -ForegroundColor Green

if ($Flash) {
    Write-Host "== flashing bootloader to 0x1000 on $Port ==" -ForegroundColor Yellow
    Write-Host "   recovery: write_flash 0x1000 vendor\bootloader.bin" -ForegroundColor DarkGray
    & $py $esptool --chip esp32 --port $Port --baud 460800 write_flash -z 0x1000 $bin
    if ($LASTEXITCODE -ne 0) { throw "flash failed" }
}
