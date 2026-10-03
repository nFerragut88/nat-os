#!/usr/bin/env python3
"""patch_bt.py -- prepare Espressif's Bluetooth controller archives for nat-os.

WHY THIS EXISTS. The BT controller is four archives of windowed-ABI code that
expect an ESP-IDF underneath them. Two separate problems have to be solved
before they can be linked into a call0 kernel that has no C library:

1. THE LIBC COLLISION. The archives call memcpy, printf, qsort, strtol and the
   double-precision soft-float helpers. Those all exist in the ESP32's ROM --
   and the ROM versions are WINDOWED, which is exactly what the blob wants. But
   Espressif's esp32.rom.newlib*.ld and esp32.rom.libgcc.ld define them by BARE
   ASSIGNMENT, so linking those scripts would silently replace the kernel's own
   call0 memcpy with a windowed ROM routine. vendor/phy/README.md records what
   that did: a board that linked, verified, and died on the first call.

   So: rename the blob's references (objcopy --redefine-sym memcpy=btrom_memcpy)
   and emit a linker script that assigns btrom_memcpy to the ROM's address. The
   blob reaches the windowed ROM routine it wanted, the kernel keeps its own
   memcpy, and no script defines a symbol nat-os also defines.

2. WHAT IS LEFT IS THE WORK. Everything the archives need that the ROM does not
   have is an OS service -- queues, tasks, timers, interrupts, the heap, the
   PHY, NVS. This script prints that list, which is the specification for
   vendor/windowed/bt_osi.c, and prints it as a DIFFERENCE rather than a guess.

Usage:
    python vendor/bt/patch_bt.py            # patch and report
    python vendor/bt/patch_bt.py --report   # report only, touch nothing
"""

import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
HERE = os.path.join(ROOT, "vendor", "bt")
SDK = os.path.expandvars(r"%USERPROFILE%\.platformio\packages"
                         r"\framework-arduinoespressif32\tools\sdk\esp32")
TOOLS = os.path.expandvars(r"%USERPROFILE%\.platformio\packages"
                           r"\toolchain-xtensa-esp32\bin")

NM = os.path.join(TOOLS, "xtensa-esp32-elf-nm.exe")
OBJCOPY = os.path.join(TOOLS, "xtensa-esp32-elf-objcopy.exe")
STRIP   = os.path.join(TOOLS, "xtensa-esp32-elf-strip.exe")

# The archives, in the order they must be linked: a static archive only answers
# references the linker has already seen to its left, and these call each other.
ARCHIVES = [
    (os.path.join(SDK, "ld", "libbtdm_app.a"), "libbtdm_app_natos.a"),
    (os.path.join(SDK, "lib", "libbt.a"), "libbt_natos.a"),
    (os.path.join(SDK, "lib", "libcoexist.a"), "libcoexist_natos.a"),
]

# Linked by the kernel already: every entry PROVIDE, so nothing can be
# displaced (vendor/phy/README.md).
ROM_LINKED = ["esp32.rom.ld"]

# NOT linked, because their entries are bare assignments. Their addresses are
# still wanted -- under other names.
ROM_RENAME = ["esp32.rom.newlib-funcs.ld", "esp32.rom.newlib-data.ld",
              "esp32.rom.newlib-nano.ld", "esp32.rom.newlib-time.ld",
              "esp32.rom.newlib-locale.ld", "esp32.rom.libgcc.ld",
              "esp32.rom.redefined.ld", "esp32.rom.eco3.ld"]

PREFIX = "btrom_"

# Symbols nat-os ALSO defines, which the blob must not share.
#
# __divsf3 is the case that forced this: kernel/mp3.c defines a global one, and
# it is the single file in this project allowed to touch the FPU (the build
# refuses any other -- FP registers are not saved across a task switch). The
# blob is windowed, so calling the kernel's call0 copy would break the ABI as
# well. Renamed, and answered in vendor/windowed/bt_osi.c with integer
# arithmetic.
EXTRA_RENAME = {
    "__divsf3": "btsf_divsf3",
    # Three more that already exist in this image, and in the WRONG ABI for the
    # blob to use: lwIP supplies lwip_htonl/htons and vendor/windowed/phy_host.c
    # supplies esp_dport_access_reg_read, all call0. Letting the blob link
    # against them would be the exact fault vendor/phy/README.md describes in
    # its second failure -- a windowed caller reaching call0 code.
    "lwip_htonl": "bt_htonl",
    "lwip_htons": "bt_htons",
    "esp_dport_access_reg_read": "bt_dport_read",
}


def run(args):
    r = subprocess.run(args, capture_output=True, text=True)
    if r.returncode:
        sys.exit("failed: %s\n%s" % (" ".join(args), r.stderr.strip()))
    return r.stdout


def rom_symbols(files):
    """{name: address} for every symbol a ROM script defines, by assignment or
    PROVIDE. Both forms are read: which form it is decides whether the script
    may be LINKED, not whether the address is usable."""
    out = {}
    pat = re.compile(r"^\s*(?:PROVIDE\s*\(\s*)?([A-Za-z_][A-Za-z0-9_]*)\s*=\s*"
                     r"(0x[0-9A-Fa-f]+)")
    for f in files:
        p = os.path.join(SDK, "ld", f)
        if not os.path.exists(p):
            continue
        for line in open(p, encoding="utf-8", errors="replace"):
            m = pat.match(line)
            if m:
                out[m.group(1)] = m.group(2)
    return out


def archive_symbols(path):
    """(undefined, defined) for one archive."""
    und, dfn = set(), set()
    for line in run([NM, path]).splitlines():
        p = line.split()
        if len(p) == 2 and p[0] == "U":
            und.add(p[1])
        elif len(p) >= 3 and p[1] not in ("U",):
            dfn.add(p[2])
    return und, dfn


def main():
    report_only = "--report" in sys.argv
    if not os.path.exists(NM):
        sys.exit("toolchain not found: %s" % NM)

    linked = rom_symbols(ROM_LINKED)
    renameable = rom_symbols(ROM_RENAME)

    need, have = set(), set()
    for src, _ in ARCHIVES:
        if not os.path.exists(src):
            sys.exit("missing archive: %s" % src)
        u, d = archive_symbols(src)
        need |= u
        have |= d

    unresolved = need - have - set(linked)
    to_rename = sorted(s for s in unresolved if s in renameable)
    ours = sorted((unresolved - set(to_rename)) - set(EXTRA_RENAME))
    ours += sorted(EXTRA_RENAME[k] for k in EXTRA_RENAME if k in unresolved)

    print("Bluetooth controller archives")
    print("  symbols needed        %5d" % len(need))
    print("  answered among them   %5d" % len(need & have))
    print("  answered by esp32.rom.ld (already linked)  %d" % len(need & set(linked)))
    print("  renamed to ROM addresses                   %d" % len(to_rename))
    print("  LEFT FOR vendor/windowed/bt_osi.c          %d" % len(ours))
    print()
    print("to write:")
    for s in ours:
        print("    %s" % s)

    if report_only:
        return

    os.makedirs(HERE, exist_ok=True)
    args = []
    for s in to_rename:
        args += ["--redefine-sym", "%s=%s%s" % (s, PREFIX, s)]
    for s, t in EXTRA_RENAME.items():
        args += ["--redefine-sym", "%s=%s" % (s, t)]

    for src, dst in ARCHIVES:
        out = os.path.join(HERE, dst)
        run([OBJCOPY] + args + [src, out])
        before = os.path.getsize(out)

        # --strip-debug, because these archives are COMMITTED.
        #
        # The SDK ships libbt.a with full debug info: 26.9 MB, against 847 KB
        # and 541 KB for the two libphy archives already in vendor/phy. Putting
        # that in git history is permanent and it buys nothing -- nothing in
        # this tree reads the blob's DWARF, and the symbol table the linker
        # needs survives --strip-debug untouched. Verified by linking: the -BT
        # image builds identically from the stripped archives.
        #
        # Done here rather than by hand so re-running this script does not
        # quietly put the 27 MB back.
        run([STRIP, "--strip-debug", out])
        after = os.path.getsize(out)
        print("\nwrote %s (%d KB, %d KB before stripping debug info)"
              % (dst, after // 1024, before // 1024))

    ld = os.path.join(HERE, "bt_rom.ld")
    with open(ld, "w", newline="\n") as f:
        f.write("/* GENERATED by vendor/bt/patch_bt.py -- do not edit.\n"
                " *\n"
                " * The ROM routines the Bluetooth archives call, under names\n"
                " * that cannot collide with the kernel's own call0 versions.\n"
                " * Espressif's newlib and libgcc scripts define these by bare\n"
                " * assignment, which would replace the kernel's memcpy with a\n"
                " * windowed ROM one -- see vendor/phy/README.md.\n"
                " */\n")
        for s in to_rename:
            f.write("%s%s = %s;\n" % (PREFIX, s, renameable[s]))
    print("wrote bt_rom.ld (%d ROM symbols)" % len(to_rename))


if __name__ == "__main__":
    main()
