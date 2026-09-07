#!/usr/bin/env python3
"""The NatScript compiler's test suite. Run by build.ps1; a failure fails it.

This is the first automated test suite in this project, and it exists because
natc.py had none: every language change could silently break something that had
worked, and the only detector was a person reading generated assembly.

A case is a .nat file in tools/tests/ with its expectation in a header comment:

    // expect: the exact text the program should print
    // expect: a second line
    // arena: 2048                (optional, default 4096)
    // keys: hi                   (optional, fed to `when key`)
    // touch: 30,40 100,200        (optional, one point per touch poll)
    // steps: 40000               (optional instruction limit)

or, for a program that must NOT compile:

    // error: some words the compiler's message must contain

A NEGATIVE CASE IS A TEST TOO, and this suite has as many of them as positive
ones. Most of what a compiler owes its user is refusing things clearly.

The oracle is tools/natvm_ref.py, which is NOT the kernel -- see its header.
This catches natc regressions. It cannot catch a disagreement between the
reference and vm.c, and nothing here should ever be taken as licence to change
the kernel.
"""

import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import natc                                             # noqa: E402
import natvm_ref                                        # noqa: E402
import vasm                                             # noqa: E402


def parse_header(text):
    spec = {"expect": [], "error": None, "arena": 4096, "keys": "",
            "steps": 200000, "touch": []}
    for line in text.splitlines():
        line = line.strip()
        if not line.startswith("//"):
            continue
        body = line[2:].strip()
        for key in ("expect", "error", "arena", "keys", "steps", "touch"):
            if body.startswith(key + ":"):
                value = body[len(key) + 1:]
                if key == "expect":
                    spec["expect"].append(value[1:] if value.startswith(" ")
                                          else value)
                elif key in ("arena", "steps"):
                    spec[key] = int(value.strip())
                elif key == "touch":
                    for pair in value.split():
                        x, y = pair.split(",")
                        spec["touch"].append((int(x), int(y)))
                else:
                    spec[key] = value.strip()
    return spec


def run_case(path):
    """Returns (ok, message)."""
    with open(path, "r", encoding="utf-8") as fh:
        text = fh.read()
    spec = parse_header(text)
    name = os.path.basename(path)

    try:
        asm = natc.compile_source(text, name)
    except natc.NatError as e:
        if spec["error"]:
            if spec["error"].lower() in e.msg.lower():
                return True, ""
            return False, (f"refused, but not for the stated reason\n"
                           f"      wanted: {spec['error']}\n"
                           f"      got:    {e.msg}")
        return False, f"did not compile: line {e.line}: {e.msg}"

    if spec["error"]:
        return False, f"compiled, but should have been refused ({spec['error']})"

    a = vasm.Assembler()
    try:
        a.parse(asm)
        image = a.encode()
    except vasm.AsmError as e:
        return False, f"natc emitted assembly vasm rejects: {e}"

    out, steps, regs, why = natvm_ref.run(spec["arena"], image,
                                          limit=spec["steps"],
                                          keys=spec["keys"],
                                          touches=spec["touch"])
    want = "\n".join(spec["expect"])
    got = out.rstrip("\n")
    if got != want:
        return False, ("output differs\n"
                       + "      wanted: " + repr(want) + "\n"
                       + "      got:    " + repr(got)
                       + f"\n      ({why} after {steps} instructions)")

    # The frame convention balancing is checked on EVERY case, not in one test
    # of its own: a leak of four bytes per call is invisible in output and
    # fatal in a loop.
    if why in ("halted", "exit") and regs[15] != (spec["arena"] & ~3):
        return False, (f"the stack pointer came back wrong: r15={regs[15]}, "
                       f"expected {spec['arena'] & ~3}")
    if why not in ("halted", "exit", "limit"):
        return False, f"stopped with {why}"
    return True, ""


def main():
    tests = sorted(f for f in os.listdir(os.path.join(HERE, "tests"))
                   if f.endswith(".nat"))
    if not tests:
        print("nattest: no cases found", file=sys.stderr)
        return 1

    failed = []
    for t in tests:
        ok, msg = run_case(os.path.join(HERE, "tests", t))
        if not ok:
            failed.append((t, msg))
            print(f"  FAIL  {t}: {msg}")

    print(f"  nattest: {len(tests) - len(failed)}/{len(tests)} cases passed")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
