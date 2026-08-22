# 2026-08-22 — X20 probe session: boot panic (inverted heap) and the X20 verdict

Two investigations in one session. The first (boot panic) blocked all serial
work; the second is the X20 result itself.

## Part 1 — boot panic: `.bss` grew into the boot-stack reservation

### Symptoms

* `send_cmd.py COM5 6 "x20" 8` and a `help` control both returned **zero
  bytes** after the command.
* Raw capture from reset (`boot_capture_tmp.py`, kept in Temp, not committed):
  board boots through display init, prints `heap : 0 B usable`, self-tests [1]
  and [3] FAIL, then:

```
*** KERNEL PANIC ***
  exccause : 28  (LoadProhibited)
  epc      : 0x400d6f7f
  excvaddr : 0x00000000
  windowbase: 7   windowstart: 0x00000080   bit(base) SET
  a0/sp out : 0x00000000 / 0x00000000   BOTH ZERO -- context clobbered
...
*** PANIC DURING PANIC ***
```

Board then halted silently — hence the empty `send_cmd.py` captures (settle
window swallowed the banner; nothing after the halt).

### Experiments

| # | Change | Expected | Actual | Conclusion |
|---|--------|----------|--------|------------|
| E1 | Control `help` instead of `x20` via send_cmd.py | reply if harness/X20 at fault | silence | Not X20-specific; console/board path suspect |
| E2 | Raw capture from t=0, no discard | banner ⇒ board alive | banner + panic + halt | Board alive; panics in early boot, before shell |
| E3 | addr2line/objdump of epc 0x400d6f7f | name the faulting instruction | `l32i.n a5, a4, 0` inside kmain's arena zero-scan loop (`loop` ×1024 over `[a4]`) | Scan base `[sp+28]` = `abase` was NULL |
| E4 | nm on natos.elf for `_heap_*` | heap range sane | `_heap_start=0x3FFD37D8 > _heap_end=0x3FFD3000` | Heap range INVERTED |
| E5 | Worktree build of baseline 66bff62 -WiFi | baseline map for comparison | compile fails (wifimac arity predates fix) | No buildable pre-X20 baseline exists; static arithmetic used instead |

### Causal chain (all links verified)

1. Static demand `data+bss = 145,364 B` vs budget `144K − 4K boot stack =
   143,360 B` → overrun ~2 KB. Dominant consumer: `g_i2c_ts`+`g_i2c_val`
   (`APPCPU_CAP_MAX 8192` → 64 KB, 44% of DRAM).
2. Linker places `.bss` to 0x3FFD37D8 — past `_heap_end`, into the boot-stack
   reservation. Region-level check passes (region ends 0x3FFD4000); **no
   assert guards `_heap_start < _heap_end`**, so inversion is silent.
3. Boot survived only because the stack grows down from 0x3FFD4000 and early
   boot used less than the un-clobbered ~2 KB.
4. `heap_init()` sees `end <= start` → returns zero-byte heap.
5. `m3_selftest()`: `arena_create(4096)` fails; `arena_bounds()` leaves
   `abase=0`; the unguarded 1024-word zero-scan (kmain.c:672-677) faults →
   LoadProhibited @ 0 → panic handler faults reading "saved frame @ 0" →
   halt.

Note: commit 697fb4f's trims fixed the earlier REGION overflow ("DRAM
overflow by 12,868") but left zero slack for heap — link success ≠ bootable.

### Fixes

* `kernel/appcpu.h`: `APPCPU_CAP_MAX 8192u → 6144u`. Reference capture is
  5,095 entries (documented in the header), so 2048 would truncate the
  measured workload; 6144 keeps it + ~20% and frees 32 KB.
* `kernel/linker.ld`: `ASSERT(_heap_start < _heap_end, ...)`. Strict
  (zero-byte heap also boots into the NULL-arena fault). Turns this failure
  class into a link error. NB: ld scripts do not support adjacent-string
  concatenation — first attempt failed to parse (linker.ld:296).

### Result

Boot now completes: `heap : 14360 B usable ... blocks 1`; self-tests
[1],[2],[3],[4a-d],isolation,release,mutex all PASS; scheduler hands off;
shell prompt appears. X20 runnable.

Known degradation, non-blocking: `raycast fb : allocation failed, using
direct columns` — raycast's framebuffer no longer fits in the 14 KB heap.
Recorded here so it is not mistaken for a regression introduced elsewhere.

## Part 2 — X20 verdict

Probe: `call8` → callee entered at entry+2 → callee sets its own WindowStart
bit → `retw.n` back. Question: does this silicon permit it?

Command output:

```
[X20] call8 -> entry(+2) -> WS:=own-bit -> retw.n
SURVIVED. outcome=2 pre(ps/ws/wb)=0x00060720/0x00000280/9 wiped=0x00000200
rec(a2/a3/e4/e5)=0x000005a1/0x000005a2/0x4008d364/0x3ffc88c0
```

* `outcome=2` — landing reached; whole sequence ran.
* `pre(ps)=0x00060720` — WOE set, EXCM clear throughout.
* `wiped=0x200` — bit 9: exactly the callee's own allocated bit; caller bits
  untouched.
* `rec(a2/a3)=0x5a1/0x5a2` — magics rotated out and back intact.
* EXCSAVE4/5 unchanged.

**Conclusion:** on ESP32-D0WD-V3 rev 3.1 the ISA holds — a windowed callee may
legally allocate its own window bit and return via `retw.n`. Outcome A
(cause-0 panic inside x20_windowed) eliminated. The X21 vendor-call path
through callx8/windowed ABI is architecturally sound on this chip.

## Next recommended experiment

X21: repeat the sequence with the callee being a real vendor-blob function
called through the same windowed discipline (blob pinned, mutex held), then
the wintorture suite against the new call path.
