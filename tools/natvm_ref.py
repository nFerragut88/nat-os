#!/usr/bin/env python3
"""A reference NatVM, on the host. NOT AUTHORITATIVE -- kernel/vm.c is.

Read this first, because a second implementation of an instruction set is
exactly the shape this project keeps finding bugs in.

WHAT THIS IS FOR: catching compiler regressions. tools/nattest.py compiles each
case in tools/tests/ and runs it here, so a change to natc.py that breaks
operator precedence, or frame balance, or short-circuit evaluation, fails the
build instead of failing on the board an hour later. It is a test oracle for
NATC, not a specification of the VM.

WHAT IT IS NOT FOR: deciding what the machine does. If this file and vm.c
disagree, VM.C IS RIGHT and this file has a bug. Nothing in the kernel may be
changed to make a test here pass. The semantics implemented below were checked
against vm.c line by line -- ldi zero-extends, ldih ORs the high half, addi is
signed on r[a], slt/sle are signed, branches are relative in INSTRUCTIONS from
the one after -- and that check is the only thing making it useful.

The device and event models are deliberately crude: a fixed device table, a
loopback for transfers, and a tick that advances with the instruction count.
They exist so a program that talks to hardware can still be run for its
arithmetic, not to model the board.
"""

import struct

M32 = 0xFFFFFFFF

# device.c's table, in its order. Duplicated here ON PURPOSE and nowhere else:
# a test that depended on the real one would be testing the kernel build.
DEVICES = ["light", "beep", "store", "i2c", "keys", "echo", "sd"]

SIM_TICK_INSNS = 200        # instructions per simulated tick


def s32(v):
    v &= M32
    return v - (1 << 32) if v & 0x80000000 else v


def _divmod(x, y):
    """UNSIGNED, because vm.c holds registers in a uint32_t and writes

        r[a] = r[b] / r[c];

    with no cast. This file had it SIGNED, which is the first real disagreement
    found between the reference and the kernel -- and it was found on the board,
    by a program computing (14 - 16) / 3 and getting 1431655764.

    The rule in the header applied without argument: vm.c is right, this file
    was wrong, and the kernel was not touched."""
    return x // y, x % y


class Halt(Exception):
    def __init__(self, why):
        super().__init__(why)
        self.why = why


def run(arena_bytes, image, limit=2_000_000, keys="", touches=(),
        trace=False):
    """Returns (output, steps, registers, why). `why` is 'halted', 'exit',
    'limit', or a fault name."""
    if len(image) > arena_bytes:
        raise Halt("image does not fit the arena")

    mem = bytearray(arena_bytes)
    mem[:len(image)] = image
    r = [0] * 16
    r[15] = arena_bytes & ~3
    pc = 0
    call = []
    out = []
    steps = 0
    why = "limit"

    evt_h = [0, 0]
    evt_p = [1, 0]
    evt_due = [0, 0]
    pending = None
    keyfeed = list(keys)
    touchfeed = list(touches)
    bounce = bytearray()

    def fault(name, detail=""):
        raise Halt(f"{name}{(' ' + detail) if detail else ''}")

    def check(addr, size, align):
        if addr + size > arena_bytes or addr > arena_bytes:
            fault("BOUNDS", f"at {addr}")
        if align > 1 and addr % align:
            fault("ALIGN", f"at {addr}")

    try:
        while True:
            steps += 1
            if steps > limit:
                break
            tick = steps // SIM_TICK_INSNS

            # An injected handler ends when the return stack comes back to the
            # depth it had before, which is how vm.c detects it. The kernel
            # saves and restores all sixteen registers around the call.
            if pending and len(call) == pending[1] and pc == pending[2]:
                r[:] = pending[0]
                pending = None

            if pending is None:
                fire = None
                if evt_h[0] and tick >= evt_due[0]:
                    evt_due[0] = tick + evt_p[0]
                    fire = (0, tick)
                elif evt_h[1] and keyfeed:
                    fire = (1, ord(keyfeed.pop(0)))
                if fire:
                    pending = (list(r), len(call), pc)
                    call.append(pc)
                    pc = evt_h[fire[0]]
                    r[0] = fire[1]
                    continue

            if pc % 4 or pc + 4 > arena_bytes:
                fault("PC", f"at {pc}")
            op, a, b, c = mem[pc], mem[pc + 1], mem[pc + 2], mem[pc + 3]
            imm = b | (c << 8)
            simm = imm - 0x10000 if imm & 0x8000 else imm
            nxt = pc + 4
            if trace:
                out.append(f"[{pc:04x} {op:02x} {a} {b} {c}]")

            if op == 0x00:
                why = "halted"
                break
            elif op == 0x01:
                pass
            elif op == 0x02:
                r[a] = r[b]
            elif op == 0x03:
                r[a] = imm
            elif op == 0x04:
                r[a] = (r[a] & 0xFFFF) | (imm << 16)
            elif op in (0x13, 0x14):
                if r[c] == 0:
                    fault("DIV0")
                q, m = _divmod(r[b], r[c])
                r[a] = (q if op == 0x13 else m) & M32
            elif 0x10 <= op <= 0x1A:
                x, y = r[b], r[c]
                r[a] = {
                    0x10: lambda: x + y, 0x11: lambda: x - y,
                    0x12: lambda: x * y, 0x15: lambda: x & y,
                    0x16: lambda: x | y, 0x17: lambda: x ^ y,
                    0x18: lambda: x << (y & 31), 0x19: lambda: x >> (y & 31),
                    0x1A: lambda: s32(x) >> (y & 31),
                }[op]() & M32
            elif op == 0x1B:
                r[a] = (~r[b]) & M32
            elif op == 0x1C:
                r[a] = (-s32(r[b])) & M32
            elif op == 0x1D:
                r[a] = (r[a] + simm) & M32
            elif 0x20 <= op <= 0x25:
                x, y = r[b], r[c]
                r[a] = int({
                    0x20: x == y, 0x21: x != y,
                    0x22: s32(x) < s32(y), 0x23: x < y,
                    0x24: s32(x) <= s32(y), 0x25: x <= y,
                }[op])
            elif op == 0x30:
                nxt = pc + 4 + simm * 4
            elif op == 0x31:
                nxt = pc + 4 + simm * 4 if r[a] == 0 else nxt
            elif op == 0x32:
                nxt = pc + 4 + simm * 4 if r[a] != 0 else nxt
            elif op == 0x33:
                if len(call) >= 32:
                    fault("CALL_DEPTH")
                call.append(nxt)
                nxt = pc + 4 + simm * 4
            elif op == 0x34:
                if not call:
                    fault("RET")
                nxt = call.pop()
            elif op in (0x40, 0x41, 0x42, 0x43):
                addr = (r[b] + c) & M32
                wide = op in (0x40, 0x42)
                check(addr, 4 if wide else 1, 4 if wide else 1)
                if op == 0x40:
                    r[a] = struct.unpack_from("<I", mem, addr)[0]
                elif op == 0x41:
                    r[a] = mem[addr]
                elif op == 0x42:
                    struct.pack_into("<I", mem, addr, r[a] & M32)
                else:
                    mem[addr] = r[a] & 0xFF
            elif op == 0x50:
                res = _syscall(imm, r, mem, out, arena_bytes, steps,
                               evt_h, evt_p, evt_due, bounce, touchfeed, fault)
                if res == "exit":
                    why = "exit"
                    break
            else:
                fault("OPCODE", f"0x{op:02x} at {pc}")

            if r[15] > arena_bytes:
                fault("BOUNDS", "stack pointer left the arena")
            pc = nxt
    except Halt as e:
        why = e.why

    return "".join(out), steps, r, why


def _syscall(num, r, mem, out, arena_bytes, steps,
             evt_h, evt_p, evt_due, bounce, touchfeed, fault):
    if num == 0:
        return "exit"
    if num == 1:
        out.append(chr(r[0] & 0xFF))
    elif num == 2:
        p = r[0]
        while p < arena_bytes and mem[p]:
            out.append(chr(mem[p]))
            p += 1
        if p >= arena_bytes:
            fault("STRING", "unterminated")
    elif num == 3:
        out.append(str(r[0] & M32))
    elif num == 4:
        r[0] = steps
    elif num == 7:
        r[0] = (240 << 16) | 320
    elif num == 8:
        # One touch per poll, from the list the harness supplied. A real panel
        # reports the same press for many polls; a test that needed that would
        # repeat the point.
        if touchfeed:
            x, y = touchfeed.pop(0)
            r[0], r[1], r[2] = 1, x, y
        else:
            r[0] = 0
    elif num == 12:
        _device(r, mem, arena_bytes, steps, bounce, fault)
    elif num == 13:
        eid, off, param = r[0], r[1], r[2]
        if eid >= 2:
            r[0] = 0
        else:
            evt_h[eid] = off
            evt_p[eid] = param if param else 1
            evt_due[eid] = (steps // SIM_TICK_INSNS) + evt_p[eid]
            r[0] = 1
    else:
        # fill/text/touch/blit/send/recv: accepted and ignored. A test that
        # cared about the screen would be testing the display driver.
        r[0] = 0
    return None


def _device(r, mem, arena_bytes, steps, bounce, fault):
    op = r[0]
    if op == 0:                                  # COUNT
        r[0] = len(DEVICES)
    elif op == 1:                                # NAME
        if r[1] >= len(DEVICES):
            r[0] = 0
            return
        nm = DEVICES[r[1]].encode()[:max(0, min(r[3], 16) - 1)] + b"\0"
        mem[r[2]:r[2] + len(nm)] = nm
        r[0] = 1
    elif op == 2:                                # READ
        r[0] = 1
        r[1] = 500 + (r[1] * 10)                 # deterministic, per device
    elif op == 3:                                # WRITE
        r[0] = 1
    elif op == 4:                                # INFO
        r[0], r[1], r[2] = 1, 1, 0x11
    elif op == 5:                                # XFER_OUT
        bounce[:] = mem[r[3]:r[3] + r[4]]
        r[0] = 1
    elif op == 6:                                # XFER_IN
        mem[r[3]:r[3] + r[4]] = bounce[:r[4]]
        r[0] = 1
    elif op == 7:                                # FIND
        p = e = r[1]
        while e < arena_bytes and mem[e]:
            e += 1
        want = mem[p:e].decode("latin-1")
        r[0] = 1 if want in DEVICES else 0
        if r[0]:
            r[1] = DEVICES.index(want)
    else:
        fault("SYSCALL", f"device op {op}")
