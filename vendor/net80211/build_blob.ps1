# Pre-links the vendor 802.11 closure into net80211.bin — next_moves/08 step 3.
#
# Produces the file that goes on the SD card. NOT part of build.ps1 and
# deliberately separate: this reads Espressif archives from the local ESP-IDF
# install, and nat-os's own build must never depend on them being present.
#
# No Espressif binary is written into the repository. The output goes to
# build/ (gitignored) unless -Out says otherwise.
#
#   powershell -File vendor/net80211/build_blob.ps1

param(
    [switch]$Flash,
    [string]$Port = "COM5",
    [string]$Out = "",
    [string]$Idf = "$env:USERPROFILE\.platformio\packages\framework-espidf",
    [string]$Toolchain = "$env:USERPROFILE\.platformio\packages\toolchain-xtensa-esp32\bin"
)

$ErrorActionPreference = "Stop"
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$root = Split-Path -Parent (Split-Path -Parent $here)
if ($Out -eq "") { $Out = Join-Path $root "build" }
if (-not (Test-Path $Out)) { New-Item -ItemType Directory -Force $Out | Out-Null }

$gcc     = Join-Path $Toolchain "xtensa-esp32-elf-gcc.exe"
$nm      = Join-Path $Toolchain "xtensa-esp32-elf-nm.exe"
$objcopy = Join-Path $Toolchain "xtensa-esp32-elf-objcopy.exe"

$lib   = Join-Path $Idf "components\esp_wifi\lib\esp32"
$romld = Join-Path $Idf "components\esp_rom\esp32\ld"
$rtc   = Join-Path $Idf "components\esp_phy\lib\esp32\librtc.a"
$coex  = Join-Path $Idf "components\esp_coex\lib\esp32\libcoexist.a"

foreach ($p in @($gcc, $lib, $romld, $rtc, $coex)) {
    if (-not (Test-Path $p)) { throw "missing: $p  (is ESP-IDF installed?)" }
}

# phy_host.o is nat-os's own, and supplies the ten-shim set's phy_* half.
$phyHost = Join-Path $root "build\phy_host.o"
if (-not (Test-Path $phyHost)) {
    throw "build/phy_host.o not found -- run build.ps1 -WiFi first"
}

Write-Host "compiling shims"
& $gcc -c -mlongcalls -O2 -o (Join-Path $Out "blob_entry.o")    (Join-Path $here "blob_entry.c")
if ($LASTEXITCODE -ne 0) { throw "blob_entry.c failed" }
& $gcc -c -mlongcalls -O2 -o (Join-Path $Out "net80211_host.o") (Join-Path $here "net80211_host.c")
if ($LASTEXITCODE -ne 0) { throw "net80211_host.c failed" }

# Comma-containing linker args must be passed as array elements, never as one
# string -- PowerShell reads a bare comma as an argument separator.
$elf = Join-Path $Out "blob.elf"
Write-Host "pre-linking to 0x40300000"
& $gcc -nostdlib -nostartfiles -o $elf `
    (Join-Path $Out "blob_entry.o") (Join-Path $Out "net80211_host.o") $phyHost `
    "-Wl,-e,0" "-Wl,--orphan-handling=error" "-T" (Join-Path $here "blob.ld") `
    "-T" "$romld\esp32.rom.ld"              "-T" "$romld\esp32.rom.libgcc.ld" `
    "-T" "$romld\esp32.rom.newlib-funcs.ld" "-T" "$romld\esp32.rom.newlib-data.ld" `
    "-T" "$romld\esp32.rom.newlib-nano.ld" `
    "-Wl,--start-group" `
      "$lib\libnet80211.a" "$lib\libpp.a" "$lib\libcore.a" "$lib\libmesh.a" `
      $rtc $coex (Join-Path $root "vendor\phy\libphy_natos.a") "-lgcc" `
    "-Wl,--end-group"
if ($LASTEXITCODE -ne 0) { throw "pre-link failed" }

$bin = Join-Path $Out "net80211.bin"
# NO --only-section list. That list is what silently dropped 357,940 bytes of
# .iram1/.phyiram/.wifi*iram code and produced an image full of zeros that
# linked, verified, and died on the first call. objcopy -O binary emits every
# loadable section; .bss is NOLOAD and is excluded automatically.
& $objcopy -O binary $elf $bin
if ($LASTEXITCODE -ne 0) { throw "objcopy failed" }

# The image carries its own size. If the file and the header disagree, the
# objcopy section list is wrong and the loader would copy .data from garbage --
# so check it here rather than discovering it on the board.
$fs = (Get-Item $bin).Length
$hdr = [System.IO.File]::ReadAllBytes($bin)[0..55]
$magic  = [BitConverter]::ToUInt32($hdr, 0)
$hdrSz  = [BitConverter]::ToUInt32($hdr, 8)
# Offset 48, not 36: the rodata_lma/vma/size fields sit before it. This read
# 36 and cheerfully reported the rodata base as the tx entry point.
$roLma  = [BitConverter]::ToUInt32($hdr, 36)
$txFn   = [BitConverter]::ToUInt32($hdr, 48)

Write-Host ""
Write-Host ("  net80211.bin  {0} bytes ({1:P1} of the 1 MB reservation)" -f $fs, ($fs / 1048576))
Write-Host ("  magic         0x{0:x8}  {1}" -f $magic, $(if ($magic -eq 0x3230384E) { "N802 OK" } else { "WRONG" }))
Write-Host ("  header size   {0}" -f $hdrSz)
Write-Host ("  rodata lma    0x{0:x8}  (mapped to DROM 0x3f700000)" -f $roLma)
Write-Host ("  tx entry      0x{0:x8}" -f $txFn)
if ($txFn -lt 0x40300000 -or $txFn -ge 0x40400000) {
    throw "tx entry 0x$('{0:x8}' -f $txFn) is not in the code window -- header layout drifted"
}

if ($magic -ne 0x3230384E) { throw "bad magic -- .blob_entry is not first in the image" }
if ($hdrSz -ne $fs)        { throw "image is $fs bytes but its header says $hdrSz" }
if ($fs -gt 0x100000)      { throw "image exceeds BLOB_FLASH_SIZE" }

Write-Host ""
Write-Host "  OK. Install with -Flash (esptool -> 0x220000), then run 'blob' on the board."
Write-Host "  There is no filesystem, so SD delivery would need a FAT reader or raw LBAs."

# Development install path. esptool writes the image to BLOB_FLASH_ADDR
# exactly as it writes the kernel -- no SD, no serial protocol, no filesystem.
# Delivery from SD or over serial is a convenience for a board with no computer
# attached and is a separate decision; it is not a prerequisite for finding out
# whether the blob runs.
if ($Flash) {
    # The exact path build.ps1 uses. A -Recurse search finds three copies of
    # esptool.py and two of them cannot import their own package.
    $esptool = Get-ChildItem (Join-Path $env:USERPROFILE ".platformio/packages/tool-esptoolpy/esptool.py") `
               -ErrorAction SilentlyContinue | Select-Object -First 1
    if (-not $esptool) { throw "esptool.py not found in tool-esptoolpy" }
    $esptool = $esptool.FullName
    $py = Join-Path $env:USERPROFILE ".platformio\penv\Scripts\python.exe"
    if (-not (Test-Path $py)) { $py = "python" }

    Write-Host ""
    Write-Host "== writing $bin to 0x220000 on $Port ==" -ForegroundColor Cyan
    & $py $esptool --chip esp32 --port $Port --baud 460800 write_flash -z 0x220000 $bin
    if ($LASTEXITCODE -ne 0) { throw "flash failed" }
    Write-Host "  installed. run `"blob`" on the board." -ForegroundColor Green
}
