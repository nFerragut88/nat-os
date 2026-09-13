#!/usr/bin/env python3
"""arenacheck -- does each program's arena hold its image AND its stack?

WHY THIS EXISTS. kmain.c's application table carries an arena size per program,
hand-written. vasm knows the image size. Since step 389 vasm also knows the
deepest the stack goes. Nothing carried those three numbers to the same place,
and kmain.c has said so since step 364:

    "Arena sizes in this table are hand-written and unchecked against the image
     the compiler produced. natc knows both numbers; nothing carries them
     across."

Step 388 found out what that costs. Five new string literals took app_fetch's
image from 2,180 bytes to 3,039 against an arena of 3,072. r15 starts at the
TOP of the arena and grows DOWN into the image (vm.c:180), so the program
shipped with 33 bytes of stack against a need of 12 -- and it worked, by 21
bytes, which is a coincidence rather than a margin.

NOTHING WOULD HAVE CAUGHT IT. app_start() checks `len > arena_bytes`
(app.c:154) and nothing else. A stack running past the image writes INSIDE the
arena, so the VM's software bounds check passes and the corruption lands in the
string literals at the top of the image. The symptom is a garbled message, not
a fault -- and in 388's case the literal it would have eaten first was
"  [fetch] done -- the whole body", which the failing test case never printed.

So this is a build step, not a warning: an arena that cannot hold image + stack
fails the build, and one that can only just hold them fails too. TIGHT is a
POLICY, not a measurement -- see MIN_MARGIN.
"""

import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
GEN = os.path.join(ROOT, "kernel", "generated")
KMAIN = os.path.join(ROOT, "kernel", "kmain.c")

# A judgement, not a measurement: below this a program cannot gain one more
# string literal without eating its own image, and the failure is silent. 388's
# margin was 21 bytes. Raise the arena rather than lowering this.
MIN_MARGIN = 128

ENTRY = re.compile(
    r'\{\s*"(?P<name>[^"]+)"\s*,\s*(?P<sym>\w+)\s*,\s*'
    r'(?P<len>\w+_LEN)\s*,\s*(?P<arena>\d+)u')


def defines(pattern):
    found = {}
    for fn in sorted(os.listdir(GEN)):
        if not fn.endswith(".h"):
            continue
        text = open(os.path.join(GEN, fn), encoding="utf-8").read()
        for m in re.finditer(pattern, text):
            found[m.group(1)] = int(m.group(2))
    return found


def main():
    if not os.path.isdir(GEN):
        print("arenacheck: no generated headers -- run vasm first",
              file=sys.stderr)
        return 1

    lens = defines(r"#define\s+(\w+_LEN)\s+(\d+)u")
    stacks = defines(r"#define\s+(\w+)_STACK\s+(\d+)u")

    rows, bad = [], []
    for m in ENTRY.finditer(open(KMAIN, encoding="utf-8").read()):
        name, lenname, arena = m.group("name"), m.group("len"), int(m.group("arena"))
        if lenname not in lens:
            bad.append((name, f"no {lenname} in any generated header"))
            continue
        image = lens[lenname]
        base = lenname[:-4]                      # VM_APP_X_LEN -> VM_APP_X
        if base not in stacks:
            bad.append((name, f"no {base}_STACK -- rebuild with vasm"))
            continue
        stack = stacks[base]
        margin = arena - image - stack
        rows.append((margin, name, image, stack, arena))

    rows.sort()
    print("  arenacheck: image + stack against the arena in kmain.c")
    print("    {:<10} {:>7} {:>6} {:>7} {:>8}  {}".format(
        "program", "image", "stack", "arena", "margin", ""))
    for margin, name, image, stack, arena in rows:
        if margin < 0:
            verdict = "OVERRUNS THE IMAGE"
        elif margin < MIN_MARGIN:
            verdict = "TIGHT (< %d)" % MIN_MARGIN
        else:
            verdict = ""
        print("    {:<10} {:>7} {:>6} {:>7} {:>8}  {}".format(
            name, image, stack, arena, margin, verdict))
        if margin < MIN_MARGIN:
            bad.append((name, "arena %d holds image %d + stack %d with %d to "
                              "spare; raise it to at least %d"
                        % (arena, image, stack, margin,
                           image + stack + MIN_MARGIN)))

    if not rows and not bad:
        print("    (no application table entries found -- has kmain.c moved?)")
        return 1
    if bad:
        print("\n  arenacheck FAILED:")
        for name, why in bad:
            print("    %-10s %s" % (name, why))
        return 1
    print("    %d programs, all clear" % len(rows))
    return 0


if __name__ == "__main__":
    sys.exit(main())
