#!/usr/bin/env python3
"""vasm — assembler for the cyd-os bytecode VM.

Runs on the host, not the device. A producer has to exist for the ISA to be
usable (UM-CYDOS-007 §6), but nothing about it needs to live in 176 KB of DRAM,
and keeping it here means the on-device side stays a pure interpreter.

    python tools/vasm.py prog.vasm -o kernel/generated/prog.h --name vm_prog

Syntax
------
    ; comment                    (also #)
    label:                       byte offset within the arena
        ldi   r1, 10
        add   r2, r1, r3
        ldw   r1, r2, 4          ; r1 <- u32 at [r2 + 4]
        brz   r4, done           ; label operands are instruction-relative
        sys   puts
        halt
        .string "text\\n"        ; NUL-terminated
        .byte  1, 2, 3
        .word  0xdeadbeef        ; 4-byte aligned automatically
        .align 4
        .space 64                ; n zero bytes

Immediates accept decimal, 0x hex, 'c' character literals, and @label for a
label's byte offset (useful for string arguments to SYS PUTS).
"""

import argparse
import re
import sys

INSN_BYTES = 4

# mnemonic -> (opcode, format). Formats:
#   none  no operands                    ab    a, b
#   abc   a, b, c                        ai    a, imm16
#   i     imm16 (relative label ok)      air   a, relative label
#   abo   a, b, byte offset              sys   syscall name or number
OPS = {
    "halt": (0x00, "none"), "nop":  (0x01, "none"),
    "mov":  (0x02, "ab"),   "ldi":  (0x03, "ai"),  "ldih": (0x04, "ai"),

    "add":  (0x10, "abc"),  "sub":  (0x11, "abc"), "mul":  (0x12, "abc"),
    "div":  (0x13, "abc"),  "mod":  (0x14, "abc"), "and":  (0x15, "abc"),
    "or":   (0x16, "abc"),  "xor":  (0x17, "abc"), "shl":  (0x18, "abc"),
    "shr":  (0x19, "abc"),  "sar":  (0x1A, "abc"), "not":  (0x1B, "ab"),
    "neg":  (0x1C, "ab"),   "addi": (0x1D, "ai"),

    "seq":  (0x20, "abc"),  "sne":  (0x21, "abc"), "slt":  (0x22, "abc"),
    "sltu": (0x23, "abc"),  "sle":  (0x24, "abc"), "sleu": (0x25, "abc"),

    "jmp":  (0x30, "i"),    "brz":  (0x31, "air"), "brnz": (0x32, "air"),
    "call": (0x33, "i"),    "ret":  (0x34, "none"),

    "ldw":  (0x40, "abo"),  "ldb":  (0x41, "abo"),
    "stw":  (0x42, "abo"),  "stb":  (0x43, "abo"),

    "sys":  (0x50, "sys"),
}

SYSCALLS = {"exit": 0, "putc": 1, "puts": 2, "putd": 3, "ticks": 4}

ESCAPES = {"n": 10, "r": 13, "t": 9, "0": 0, "\\": 92, '"': 34, "'": 39}


class AsmError(Exception):
    pass


def parse_string(text):
    """Decode a quoted literal into bytes, honouring backslash escapes."""
    if len(text) < 2 or text[0] != '"' or text[-1] != '"':
        raise AsmError(f"malformed string: {text}")
    out, i, body = bytearray(), 0, text[1:-1]
    while i < len(body):
        ch = body[i]
        if ch == "\\":
            i += 1
            if i >= len(body):
                raise AsmError("string ends in a backslash")
            if body[i] not in ESCAPES:
                raise AsmError(f"unknown escape \\{body[i]}")
            out.append(ESCAPES[body[i]])
        else:
            out.append(ord(ch))
        i += 1
    return bytes(out)


def reg(tok):
    m = re.fullmatch(r"[rR](\d+)", tok)
    if not m:
        raise AsmError(f"expected a register, got '{tok}'")
    n = int(m.group(1))
    if not 0 <= n < 16:
        raise AsmError(f"register out of range: {tok}")
    return n


def split_operands(text):
    """Split on commas that are not inside a quoted string."""
    parts, cur, in_str, esc = [], "", False, False
    for ch in text:
        if in_str:
            cur += ch
            if esc:
                esc = False
            elif ch == "\\":
                esc = True
            elif ch == '"':
                in_str = False
        elif ch == '"':
            in_str = True
            cur += ch
        elif ch == ",":
            parts.append(cur.strip())
            cur = ""
        else:
            cur += ch
    if cur.strip():
        parts.append(cur.strip())
    return parts


def strip_comment(line):
    out, in_str, esc = "", False, False
    for ch in line:
        if in_str:
            out += ch
            if esc:
                esc = False
            elif ch == "\\":
                esc = True
            elif ch == '"':
                in_str = False
            continue
        if ch == '"':
            in_str = True
            out += ch
        elif ch in ";#":
            break
        else:
            out += ch
    return out.strip()


class Assembler:
    def __init__(self):
        self.labels = {}
        self.items = []      # (offset, kind, payload, lineno)
        self.offset = 0

    # ---- pass 1: lay out, recording label offsets ------------------------
    def parse(self, text):
        for lineno, raw in enumerate(text.splitlines(), 1):
            line = strip_comment(raw)
            if not line:
                continue
            try:
                while True:
                    m = re.match(r"([A-Za-z_.][A-Za-z0-9_.]*)\s*:\s*", line)
                    if not m:
                        break
                    name = m.group(1)
                    if name in self.labels:
                        raise AsmError(f"duplicate label '{name}'")
                    self.labels[name] = self.offset
                    line = line[m.end():].strip()
                if not line:
                    continue
                self._layout(line, lineno)
            except AsmError as e:
                raise AsmError(f"line {lineno}: {e}") from None

    def _layout(self, line, lineno):
        parts = line.split(None, 1)
        head = parts[0].lower()
        rest = parts[1] if len(parts) > 1 else ""

        if head == ".string":
            data = parse_string(rest.strip()) + b"\x00"
            self.items.append((self.offset, "raw", data, lineno))
            self.offset += len(data)
        elif head == ".byte":
            vals = split_operands(rest)
            self.items.append((self.offset, "byte", vals, lineno))
            self.offset += len(vals)
        elif head == ".word":
            self._pad_to(4)
            vals = split_operands(rest)
            self.items.append((self.offset, "word", vals, lineno))
            self.offset += 4 * len(vals)
        elif head == ".align":
            self._pad_to(int(rest, 0))
        elif head == ".space":
            n = int(rest, 0)
            self.items.append((self.offset, "raw", bytes(n), lineno))
            self.offset += n
        elif head in OPS:
            # Instructions must be 4-byte aligned; the VM faults otherwise, so
            # pad rather than emit something that cannot execute.
            self._pad_to(INSN_BYTES)
            self.items.append((self.offset, "insn", (head, split_operands(rest)), lineno))
            self.offset += INSN_BYTES
        else:
            raise AsmError(f"unknown mnemonic or directive '{head}'")

    def _pad_to(self, align):
        pad = (-self.offset) % align
        if pad:
            self.items.append((self.offset, "raw", bytes(pad), 0))
            self.offset += pad

    # ---- pass 2: encode, resolving labels --------------------------------
    def encode(self):
        out = bytearray(self.offset)
        for off, kind, payload, lineno in self.items:
            try:
                if kind == "raw":
                    out[off:off + len(payload)] = payload
                elif kind == "byte":
                    for i, v in enumerate(payload):
                        out[off + i] = self.value(v) & 0xFF
                elif kind == "word":
                    for i, v in enumerate(payload):
                        w = self.value(v) & 0xFFFFFFFF
                        out[off + 4 * i: off + 4 * i + 4] = w.to_bytes(4, "little")
                else:
                    out[off:off + INSN_BYTES] = self.insn(payload[0], payload[1], off)
            except AsmError as e:
                where = f"line {lineno}: " if lineno else ""
                raise AsmError(f"{where}{e}") from None
        return bytes(out)

    def value(self, tok):
        tok = tok.strip()
        if tok.startswith("@"):
            name = tok[1:]
            if name not in self.labels:
                raise AsmError(f"unknown label '{name}'")
            return self.labels[name]
        if len(tok) == 3 and tok[0] == tok[2] == "'":
            return ord(tok[1])
        if tok in self.labels:
            return self.labels[tok]
        try:
            return int(tok, 0)
        except ValueError:
            raise AsmError(f"cannot parse value '{tok}'") from None

    def rel(self, tok, here):
        """Branch displacement, counted in instructions from the NEXT one."""
        target = self.value(tok)
        if target % INSN_BYTES:
            raise AsmError(f"branch target '{tok}' is not instruction-aligned")
        delta = (target - (here + INSN_BYTES)) // INSN_BYTES
        if not -32768 <= delta <= 32767:
            raise AsmError(f"branch to '{tok}' out of range ({delta})")
        return delta & 0xFFFF

    def insn(self, mnem, ops, here):
        opcode, fmt = OPS[mnem]
        a = b = c = 0

        def need(n):
            if len(ops) != n:
                raise AsmError(f"{mnem} takes {n} operand(s), got {len(ops)}")

        if fmt == "none":
            need(0)
        elif fmt == "ab":
            need(2); a, b = reg(ops[0]), reg(ops[1])
        elif fmt == "abc":
            need(3); a, b, c = reg(ops[0]), reg(ops[1]), reg(ops[2])
        elif fmt == "ai":
            need(2)
            a = reg(ops[0])
            v = self.value(ops[1])
            if not -32768 <= v <= 65535:
                raise AsmError(f"immediate out of range: {v}")
            b, c = v & 0xFF, (v >> 8) & 0xFF
        elif fmt == "i":
            need(1)
            d = self.rel(ops[0], here)
            b, c = d & 0xFF, (d >> 8) & 0xFF
        elif fmt == "air":
            need(2)
            a = reg(ops[0])
            d = self.rel(ops[1], here)
            b, c = d & 0xFF, (d >> 8) & 0xFF
        elif fmt == "abo":
            need(3)
            a, b = reg(ops[0]), reg(ops[1])
            off = self.value(ops[2])
            if not 0 <= off <= 255:
                raise AsmError(f"offset out of range 0..255: {off}")
            c = off
        elif fmt == "sys":
            need(1)
            key = ops[0].strip().lower()
            v = SYSCALLS[key] if key in SYSCALLS else self.value(ops[0])
            b, c = v & 0xFF, (v >> 8) & 0xFF

        return bytes((opcode, a, b, c))


def emit_header(data, name, labels, source):
    lines = [
        "/* Generated by tools/vasm.py — do not edit.",
        f" * Source: {source}",
        " */",
        "#ifndef CYDOS_GENERATED_%s_H" % name.upper(),
        "#define CYDOS_GENERATED_%s_H" % name.upper(),
        "",
        "#include <stdint.h>",
        "",
        f"static const uint8_t {name}[] = {{",
    ]
    for i in range(0, len(data), 12):
        chunk = ", ".join(f"0x{b:02x}" for b in data[i:i + 12])
        lines.append(f"    {chunk},")
    lines += [
        "};",
        "",
        f"#define {name.upper()}_LEN {len(data)}u",
        "",
    ]
    for label, off in sorted(labels.items(), key=lambda kv: kv[1]):
        lines.append(f"#define {name.upper()}_AT_{label.upper()} {off}u")
    lines += ["", "#endif", ""]
    return "\n".join(lines)


def main():
    ap = argparse.ArgumentParser(description="assemble cyd-os VM bytecode")
    ap.add_argument("source")
    ap.add_argument("-o", "--output", required=True)
    ap.add_argument("--name", default="vm_prog")
    args = ap.parse_args()

    with open(args.source, "r", encoding="utf-8") as fh:
        text = fh.read()

    asm = Assembler()
    try:
        asm.parse(text)
        data = asm.encode()
    except AsmError as e:
        print(f"vasm: {args.source}: {e}", file=sys.stderr)
        return 1

    if args.output.endswith(".h"):
        payload = emit_header(data, args.name, asm.labels, args.source)
        with open(args.output, "w", encoding="utf-8") as fh:
            fh.write(payload)
    else:
        with open(args.output, "wb") as fh:
            fh.write(data)

    print(f"  vasm: {args.source} -> {args.output}  "
          f"({len(data)} bytes, {len(asm.labels)} labels)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
