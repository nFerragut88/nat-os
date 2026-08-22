# UM-NATOS-040 — The Heap That Wasn't There and the Bit That Held

**Used Medias LLC — Embedded Systems Division**
Revision 1.0 · 2026-08-22 · Status: **Boot-blocking memory-map defect root-caused, fixed and verified on hardware. X20 answered: the ISA permits the windowed self-bit discipline; the vendor-call path is cleared for X21.**

---

## 1. Abstract

The X20 probe — `call8` into a windowed callee that sets its *own* WINDOWSTART
bit, then returns via `retw.n` — went to flash at the end of the previous
session and produced nothing: every serial command returned zero bytes. This
session began by taking that silence seriously, and it was right to. The board
was not running the probe; it was dying in early boot, before the shell
existed, inside its own heap self-test.

The cause was not exotic. The kernel's heap is defined by subtraction —
`_heap_start` from the end of `.bss`, `_heap_end` from the top of DRAM minus
the boot stack — and nothing anywhere asserted that the subtraction stays
positive. Static demand had exceeded the heap-compatible budget by about
2 KB, the linker had placed `.bss` straight into the boot-stack reservation
without complaint, and `heap_init()` had responded exactly as designed to an
inverted range: by declaring a zero-byte heap. The boot self-test then
dereferenced the NULL arena base that follows from that, panicked, panicked
again inside its own panic handler, and halted.

Both defects are fixed: the capture buffer that dominated the map is sized
against its measured workload instead of round-headroom enthusiasm, and the
linker now refuses any image whose `.bss` reaches the reservation at all.
With the board actually alive, X20 ran and returned the answer the series was
built to get: **the hardware honours the discipline**. Every observable —
outcome counter, wiped mask, register magics, exception saves — matched the
ISA's contract exactly.

---

## 2. The silence

`send_cmd.py COM5 6 "x20" 8` printed its sentinel and never another byte. A
benign control (`help`) through the same harness was equally silent, which
eliminated the probe as a variable before anything was touched.

Raw capture from reset, discarding nothing, told the whole story in one run:

```
heap         : 0 B usable, largest 0 B, blocks 0
[1] no leak  : FAIL  after 10000 cycles  free=0 largest=0 blocks=0 ...
[3] oom safe : FAIL  oversize=NULL overflow=NULL fails=10004 bad_frees=1 ...

*** KERNEL PANIC ***
  exccause : 28  (LoadProhibited)
  epc      : 0x400d6f7f
  excvaddr : 0x00000000
  windowbase: 7   windowstart: 0x00000080   bit(base) SET
  a0/sp out : 0x00000000 / 0x00000000   BOTH ZERO -- context clobbered
*** PANIC DURING PANIC ***
```

…then thirteen seconds of nothing. Two harness behaviours had hidden this
completely: the settle loop consumes and discards everything before the
command is sent, and a board halted in panic-during-panic emits nothing
afterward. Empty capture had been read as "no reply"; it meant "no board".

### Attribution experiments

| # | Experiment | Result | Eliminated / established |
|---|---|---|---|
| E1 | `help` control | silence | Not X20-specific |
| E2 | Raw t=0 capture | banner → panic → halt | Board and UART fine; death precedes the shell |
| E3 | addr2line/objdump of EPC | `l32i.n a5, a4, 0` in kmain's arena zero-scan (`loop` ×1024 over `[a4]`) | Faulting load's base register held NULL |
| E4 | `nm` for `_heap_*` | `_heap_start = 0x3FFD37D8 > _heap_end = 0x3FFD3000` | Heap range inverted |
| E5 | Worktree build of pre-X20 snapshot 66bff62 | `-WiFi` build fails (wifimac arity predates the header fix) | No buildable pre-X20 baseline ever existed; comparison must be static |

E5 matters beyond this bug: between the windowed-ABI header change and this
session's call-site fixes, no `-WiFi` image could be built at all. The last
bootable WiFi image predates both.

---

## 3. The inverted range

The full causal chain, every link verified against artifacts:

1. **Budget.** The dram region spans 144 KB ending at 0x3FFD4000; the last
   4 KB are the boot-stack reservation, so `.data + .bss` must stay below
   0x3FFD3000 — 143,360 B. Measured demand: **145,364 B**. Overrun: ~2 KB.
2. **Placement.** The linker laid `.bss` out to 0x3FFD37D8 — 2,008 bytes into
   the reservation. Boot survived only because the stack grows *down* from
   0x3FFD4000 and the early path used less than the un-clobbered remainder.
   The heap did not survive: `_heap_start > _heap_end`.
3. **Silence.** No diagnostic exists for this. The region-level checks see
   only that `.bss` fits the region (it does, 2 KB under 0x3FFD4000); the
   derived symbols are plain arithmetic. The linker script's own comment says
   the heap "absorbs changes in .bss automatically" — true until absorption
   goes negative, at which point the failure moves to run time, several
   self-tests deep, wearing the costume of a NULL-pointer bug.
4. **Death.** `heap_init()` meets `end <= start` and returns a zero-byte heap,
   by design. `m3_selftest()` calls `arena_create(4096)`, which fails;
   `arena_bounds()` leaves `abase` at its initializer; the 1024-word
   zero-check scan (kmain.c:672) runs unguarded and faults at address 0. The
   panic handler then reads a "saved frame @ 0x00000000", faults again, and
   halts preserving the first record.

### Where the bytes went

`nm --size-sort` ranked the occupants. The dominant consumer was not task
stacks or blob structures but a diagnostic instrument: `g_i2c_ts` +
`g_i2c_val`, two 8192-entry capture arrays for the core-1 regi2c sampler —
**64 KB, 44% of the entire map**, sized by a round number rather than by the
workload it serves.

This session also corrected the previous session's account of its own fix:
the 14,976 B of scratch trims solved the *region* overflow ("DRAM overflow by
12,868") but left zero slack for heap or margin. Link success is placement
success, not bootability. The distinction is now enforced rather than hoped
for.

---

## 4. The solution

Two changes, one restoring the budget, one making recurrence impossible to
miss:

* **`kernel/appcpu.h`: `APPCPU_CAP_MAX 8192 → 6144`.** The header documents
  the measured requirement: the reference board captured 5,095 regi2c events
  during PHY init. 6144 keeps that sequence plus ~20% and returns 32 KB to
  the map. Cutting to a smaller power of two would have traded a documented
  capability for convenience; the comment block records both bounds so the
  next sizing decision starts from evidence.
* **`kernel/linker.ld`: strict heap assert.**

  ```ld
  ASSERT(_heap_start < _heap_end, ".bss grew into the heap/boot-stack ...")
  ```

  Strict, deliberately: equality yields a zero-byte heap, and this session
  demonstrated where that leads. One toolchain note preserved per house
  rules — GNU ld linker scripts do **not** perform C-style adjacent-string
  concatenation; the multi-line message form fails to parse.

### Verification

Rebuilt, reflashed, raw-captured:

```
heap         : 14360 B usable, largest 14360 B, blocks 1
[1] no leak  : PASS      [3] oom safe : PASS
[2] arenas   : PASS  live=2 committed=5120 B  base=0x3ffcf7e8 len=4096
[4a-d], isolation, release, mutex : PASS
handing off to the scheduler — kmain does not return
nat-os shell — 'help' for commands
```

Known, accepted degradation, recorded so it is not mistaken for a regression:
the raycast framebuffer no longer fits the smaller heap and falls back to
direct-column rendering (`raycast fb : allocation failed`). Restoring it is a
future budget decision, not an accident.

---

## 5. X20 — the verdict

With the board alive, the probe committed in 697fb4f finally ran. Its
question: may a windowed callee legally set its own WindowStart bit and exit
via `retw.n` — the discipline every future vendor-blob call will lean on?

```
[X20] call8 -> entry(+2) -> WS:=own-bit -> retw.n
SURVIVED. outcome=2 pre(ps/ws/wb)=0x00060720/0x00000280/9 wiped=0x00000200
          rec(a2/a3/e4/e5)=0x000005a1/0x000005a2/0x4008d364/0x3ffc88c0
```

Field by field:

| observation | value | reading |
|---|---|---|
| outcome | 2 | landing reached; the complete sequence executed |
| pre-ps | 0x00060720 | WOE set, EXCM clear throughout — windowed mode live, exceptions off the table |
| pre-ws/wb | 0x280 / 9 | caller's frames allocated as expected |
| wiped | 0x200 | exactly one bit — the callee's own — consumed by `retw.n`; neighbours untouched |
| rec a2/a3 | 0x5a1 / 0x5a2 | both magics rotated out and back intact |
| rec e4/e5 | unchanged | no stray exception machinery engaged |

Outcome A — a cause-0 fault raised by the silicon against a legal sequence —
is eliminated on this part (ESP32-D0WD-V3 rev 3.1). The ISA holds. The
windowed vendor-call design proceeds to X21 on architectural merit, with the
remaining risks being nat-os's own (pinning, mutex discipline, spill paths),
which is precisely where the next probe belongs.

---

## 6. Transferable conclusions

1. **Empty output is a measurement, not an absence of one.** "No reply"
   concealed "halted before the shell". Any harness that discards bytes
   before a command must be paired with a way to ask what happened before
   the discard window.
2. **Derived symbols need their own asserts.** A range defined by subtraction
   (`heap = region − bss − reserve`) fails silently when a term grows; the
   region-level checks cannot see it. One line of linker script converts a
   boot-time NULL dereference into a compile-gate error.
3. **Link success ≠ bootable.** Region fit and heap viability are different
   predicates; only the second one keeps a board alive. The gap between them
   was exactly the size of this failure.
4. **Size instruments by their measurements.** The 64 KB capture buffer was
   round-number headroom; the workload needs 5,095 entries. Round numbers
   are how 44% of an address space disappears without anyone deciding it
   should.
5. **A green suite measures opportunity, not correctness** — reaffirmed by
   E5: the pre-X20 snapshot could not even be built, so several sessions of
   "known-good baseline" reasoning rested on a tree that never booted in its
   committed form. Baselines must be buildable artifacts, not intentions.

Full experiment log: `docs/debug/2026-08-22-x20-and-inverted-heap.md`.

Written by: Tortoise
