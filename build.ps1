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

    # Flash Espressif's second stage at 0x1000 instead of ours. The recovery
    # path, and the A/B control if our bootloader is ever suspected.
    [switch]$VendorBootloader,

    # Which board this image is for. Selects a pin map and a set of fitted
    # peripherals from kernel/board_<name>.h.
    #
    #   cyd      ESP32-2432S028R, the display board everything was measured on
    #   lora32   an ESP32 + SX1262 relay node -- PIN MAP NOT YET VERIFIED
    [ValidateSet("cyd", "lora32")]
    [string]$Board = "cyd",

    # Build the WiFi subsystem, and with it the only vendor binaries this
    # project links. Off by default: everything else in the kernel is code from
    # this project, and libphy cannot be reimplemented from public information,
    # so WiFi is the one part that can never be clean. See docs/blob-free.md.
    [switch]$WiFi
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
    # lwIP: its own headers, plus vendor/lwip/include/arch/cc.h. lwipopts.h
    # lives in kernel/ and is found by the -I above.
    "-I", "$root\vendor\lwip\include",
    "-I", "$root\vendor\wpa\include",
    "-DBOARD_$($Board.ToUpper())"
)
if ($WiFi) { $cflags += "-DBOARD_WIFI_OVERRIDE=1" }

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
# The three files that reach the vendor blobs. Excluded entirely unless -WiFi,
# so a default build has no path to libphy at all -- not a stubbed one, not a
# dead-code one. If it is not compiled it cannot link, and `nm` can prove it.
# phyinit.c is NOT here any more: since the blob carries its own libphy, its
# bring-up sequence is needed by the blob-free build too. It references no
# Espressif symbol unless BOARD_HAS_WIFI is set.
# wifi_osi_impl.c left the list for the same reason phyinit.c did: the
# windowed OS adapter table in vendor/windowed/wifi_osi.c forwards into it,
# and that table is now handed to the loaded blob from a build that links no
# Espressif code. It includes only kernel headers.
$blobFiles = @("wifimac.c")

foreach ($src in (Get-ChildItem "$root\kernel" -Include *.c,*.S -Recurse)) {
    if ((-not $WiFi) -and ($blobFiles -contains $src.Name)) { continue }
    # Full name, not BaseName. A kernel with both appcpu.c and appcpu.S would
    # otherwise compile both to appcpu.o, the second silently overwriting the
    # first, and the only symptom is an undefined-reference at link time for
    # symbols whose source file is plainly sitting in the tree. Cost one build
    # to diagnose.
    $obj = Join-Path $build ($src.Name + ".o")
    Write-Host ("  {0}" -f $src.Name)
    & $gcc @cflags -c $src.FullName -o $obj
    if ($LASTEXITCODE -ne 0) { throw "compile failed: $($src.Name)" }
    $objs += $obj
}

# lwIP, vendored in vendor/lwip. Compiled call0 exactly like the kernel -- it is
# ordinary C with no ABI opinion, and it runs on nat-os tasks, not the blob's.
#
# NOT under kernel/, deliberately: that directory is globbed recursively, and
# twenty-three files of somebody else's code sitting inside it would blur the
# line between what this project wrote and what it merely uses.
#
# -Wno-* because lwIP is warning-clean under ITS build, not under -Wall -Wextra
# with this vintage of GCC. Silencing them for vendored code keeps nat-os's own
# warnings visible, which is the only reason warnings are useful here.
# WPA crypto, vendored in vendor/wpa. Compiled WINDOWED, not call0.
#
# [step 240] It was call0 and had to move. The four-way handshake calls
# esp_wifi_set_sta_key_internal, which takes NINE arguments, and the
# call0-to-windowed bridges carry four. So the handshake must be windowed to
# reach the blob at all -- and windowed code reaching call0 crypto would need
# w2c bridges that carry three, against pbkdf2_sha1's six and sha1_prf's seven.
#
# Compiling the crypto windowed removes every bridge from the path: handshake,
# crypto and blob all speak one ABI and call each other directly. The crypto is
# ordinary C with no ABI opinion, so this costs nothing.
#
# These are NOT hand-written, deliberately: hand-rolled SHA-1 and AES is where
# subtle, silent, security-relevant bugs live. The four-way handshake state
# machine IS hand-written, because porting ESP-IDF's rsn_supp would mean 5,660
# lines handling five protocols where nat-os needs one, plus utils/eloop and
# wpabuf. Both halves of that split were measured before choosing.
#
# Same -Wno-* reasoning as lwIP: vendored code's warnings would drown nat-os's.
$wpasrc = Get-ChildItem "$root\vendor\wpa\src\*.c" -ErrorAction SilentlyContinue
if ($wpasrc) {
    Write-Host "== compiling WPA crypto ==" -ForegroundColor Cyan
    $wpflags = @("-mabi=windowed", "-mlongcalls", "-ffreestanding", "-fno-builtin",
                 "-fno-stack-protector", "-Os", "-std=c11",
                 # See the note on $wflags: without this the inline os_memcpy
                 # becomes a call to call0 memcpy from windowed code.
                 "-fno-tree-loop-distribute-patterns",
                 "-I", "$root\kernel", "-I", "$root\vendor\wpa\include",
                 "-Wno-unused-parameter", "-Wno-sign-compare",
                 "-Wno-type-limits", "-Wno-implicit-fallthrough",
                 "-Wno-unused-but-set-variable", "-Wno-char-subscripts")
    foreach ($src in $wpasrc) {
        $obj = Join-Path $build ("wpa_" + $src.Name + ".o")
        & $gcc @wpflags -c $src.FullName -o $obj
        if ($LASTEXITCODE -ne 0) { throw "wpa crypto compile failed: $($src.Name)" }
        $objs += $obj
    }
    Write-Host ("  {0} WPA crypto objects" -f $wpasrc.Count)
}

$lwipsrc = Get-ChildItem "$root\vendor\lwip\src\*.c" -ErrorAction SilentlyContinue
if ($lwipsrc) {
    Write-Host "== compiling lwIP ==" -ForegroundColor Cyan
    $lwflags = $cflags + @("-Wno-unused-parameter", "-Wno-sign-compare",
                           "-Wno-address", "-Wno-type-limits",
                           "-Wno-implicit-fallthrough", "-Wno-array-bounds")
    foreach ($src in $lwipsrc) {
        $obj = Join-Path $build ("lwip_" + $src.Name + ".o")
        & $gcc @lwflags -c $src.FullName -o $obj
        if ($LASTEXITCODE -ne 0) { throw "lwip compile failed: $($src.Name)" }
        $objs += $obj
    }
    Write-Host ("  {0} lwIP objects" -f $lwipsrc.Count)
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
    # -I kernel: this directory had no include path at all, so a windowed file
    # could only ever restate a kernel constant rather than share it -- which is
    # how the OSI forever-cap came to mean 4 s on one side of the boundary and
    # 600 ms on the other. Only MACROS may be taken across: a call0 static inline
    # pulled in here would be an ABI crossing with no bridge.
    $wflags = @("-mabi=windowed", "-mlongcalls", "-ffreestanding", "-fno-builtin",
                "-fno-stack-protector", "-Os", "-Wall", "-Wextra", "-std=c11",
                # [step 241] -fno-tree-loop-distribute-patterns, and it is
                # load-bearing here for a SECOND reason beyond kstring.c's.
                #
                # GCC rewrites a hand-written byte-copy loop into a call to
                # memcpy. In kstring.c that is infinite recursion. In WINDOWED
                # code it is an ABI violation: memcpy is call0, and a windowed
                # caller reaches it with call8, which leaves the return address
                # in a8 where a call0 callee returns through a0.
                #
                # Measured: the inline os_memcpy in vendor/wpa/include/includes.h
                # -- written inline precisely to avoid that crossing -- was
                # turned back into a call to it, and faulted inside memcpy
                # storing to 0x40000000.
                "-fno-tree-loop-distribute-patterns",
                "-I", "$root\kernel",
                # [step 240] the WPA crypto headers: the handshake and the
                # self-test are windowed and call it directly.
                "-I", "$root\vendor\wpa\include")
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
} elseif (-not $WiFi) {
    Write-Host "  no vendor archives: this image is blob-free" -ForegroundColor Green
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
    # The bootloader at 0x1000 is ours now (boot/, UM-NATOS-035). It is built
    # here rather than assumed present, so `build.ps1 -Flash` can never write a
    # stale second stage next to a fresh kernel -- the two agree about the image
    # format, and a mismatch between them is not a class of bug worth inventing.
    #
    # -VendorBootloader falls back to Espressif's copy. Keep that path working:
    # it is how a broken bootloader gets recovered, and the day it is needed is
    # not the day to be debugging it.
    # -WiFi forces Espressif's second stage, for now.
    #
    # UM-NATOS-036: with our bootloader, register_chipv7_phy() panics with
    # StoreProhibited inside phy_enter_critical. The clock difference that used
    # to HANG it is fixed (kernel/clock.c), and that fix is real and applies to
    # every build -- but at least one more thing Espressif's loader leaves
    # behind is still missing, and it has not been found. Eight register
    # differences were applied and did not explain it; the windowed ABI was
    # tested and is intact.
    #
    # The blob-free default build is unaffected and fully verified on our
    # loader. This gate exists so that WiFi research is not silently conducted
    # on a broken foundation -- debugging the PHY on top of an unexplained
    # bootloader gap is exactly how this project has previously lost sessions.
    if ($WiFi -and -not $VendorBootloader) {
        Write-Host "  stage 2: vendor (forced by -WiFi; see UM-NATOS-036)" -ForegroundColor Yellow
        $VendorBootloader = $true
    }

    if ($VendorBootloader) {
        if (-not (Test-Path (Join-Path $borrowed "bootloader.bin"))) {
            throw "missing bootloader.bin in $borrowed - see vendor/README.md"
        }
        $stage2 = Join-Path $borrowed "bootloader.bin"
        Write-Host "  stage 2: vendor (Espressif)" -ForegroundColor Yellow
    } else {
        & (Join-Path $PSScriptRoot "boot\build_boot.ps1")
        if ($LASTEXITCODE -ne 0) { throw "bootloader build failed" }
        $stage2 = Join-Path $PSScriptRoot "boot\build\boot.bin"
        Write-Host "  stage 2: ours" -ForegroundColor Green
    }

    # The partition table stays borrowed and stays at 0x8000. Nothing in this
    # chain reads it any more -- boot.c hardcodes 0x10000 -- but esptool and
    # every external tool expect one to be there, and 3 KB is cheaper than the
    # surprise.
    if (-not (Test-Path (Join-Path $borrowed "partitions.bin"))) {
        throw "missing partitions.bin in $borrowed - see vendor/README.md"
    }

    Write-Host "== flashing $Port ==" -ForegroundColor Cyan
    # Output is captured so the flash can be VERIFIED rather than assumed, then
    # echoed so nothing is hidden. esptool prints "Hash of data verified." once
    # per segment and there are three.
    #
    # The exit-code check below is not enough on its own, and not because it is
    # wrong -- because a human (or a grep) reading the console can see a failure
    # and still read the suite results printed underneath it. That happened:
    # next_moves/08 step 148, where a flash refused with "Could not open COM5",
    # the board kept the previous image, and the run afterwards was reported as
    # a result. A count that must reach three turns "I should have noticed" into
    # "the script stopped".
    #
    # No 2>&1: redirecting a native command's stderr in PS 5.1 wraps each line in
    # an ErrorRecord and falsifies $?, which would break the very check this is.
    $flashOut = & $python $esptool --chip esp32 --port $Port --baud 460800 write_flash -z `
        0x1000  $stage2 `
        0x8000  (Join-Path $borrowed "partitions.bin") `
        0x10000 $bin
    $flashOut | ForEach-Object { Write-Host $_ }
    if ($LASTEXITCODE -ne 0) { throw "flash failed" }

    $verified = ($flashOut | Select-String -SimpleMatch "Hash of data verified").Count
    if ($verified -lt 3) {
        throw "flash did not verify: $verified of 3 segments hashed -- the board may still hold the PREVIOUS image, so do not trust any run against it"
    }
    Write-Host "== flash verified: $verified segments ==" -ForegroundColor Green
}

if ($Monitor) {
    Write-Host "== monitor $Port (Ctrl+C to exit) ==" -ForegroundColor Cyan
    & $python -m serial.tools.miniterm $Port 115200
}
