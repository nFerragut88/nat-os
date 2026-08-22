# UM-NATOS-042 — The Shape of the Remaining Fault

**Used Medias LLC — Embedded Systems Division**
Revision 1.1 · 2026-08-22 · Status: **Diagnostic. The init-time crash is fixed; one fault remains. Section 7's correlation has since been tested and CONFIRMED — the window handler is reading a call0 local as a stack pointer.**

---

## 1. Abstract

UM-NATOS-041 rev 2.0 recorded that `esp_wifi_init_internal` executes without
faulting for the first time since the vendor stack began running from flash. This
report takes stock: what the system actually is at the level the fault lives at,
what the fault is, what has been positively eliminated, and what is left.

It exists because the investigation has reached the point where the remaining
question is narrow but the *surrounding* architecture is now large enough that
new work — or a second person — cannot safely start from the step log alone.

One new finding is recorded in §7 and marked clearly as untested.

---

## 2. The architecture the fault lives in

### 2.1 Address space

```
iram (rwx)   0x40080000  128 KB     kernel code, vectors, window handlers
dram (rw)    0x3FFB0000  144 KB     task stacks, heap, .bss, PHY stack
irom (rx)    0x400D0000  2.2 MB     flash-mapped kernel code (.flash.text)
drom (r)     0x3F400020  4 MB       flash-mapped kernel rodata
```

Vendor blob, mapped as three separate windows because one will not do:

```
BLOB_IROM_ADDR   0x40300000   1 MB    code, executed from flash via the MMU
BLOB_DROM_ADDR   0x3F700000   256 KB  rodata (instruction memory cannot serve it)
BLOB_DRAM_ADDR   0x3FFD4000   32 KB   .data copied + .bss zeroed at load
BLOB_FLASH_ADDR  0x220000     1 MB    where the image lives on flash
```

The split is not cosmetic. Instruction memory serves 32-bit aligned fetches only,
so `.rodata` in the IROM window faults on any byte access — measured, and the
reason `BLOB_DROM_ADDR` exists at all.

Current image budget: `.text` 55 KB IRAM, `.flash.text` 24 KB, `.bss` 62 KB DRAM,
heap 79,736 B usable.

### 2.2 The ABI boundary — the central design fact

The kernel is compiled **`-mabi=call0`**: no register windows, ever. The vendor
blob is **windowed**, as all ESP-IDF code is. Every crossing between them goes
through a hand-written bridge in `kernel/window.S`:

```
call0 -> windowed      rom_call3   rom_call4   phy_stack_call   win_call_vendor
windowed -> call0      w2c_call0f  w2c_call1   w2c_call2        w2c_call3
spill helper           win_spill_all   win_spill_call0
```

The boundary is enforced **by file**, not by function: anything windowed lives in
`vendor/windowed/`, everything in `kernel/` is call0. Crossing without a bridge
produces an `epc` with bit 31 set — a windowed return encoding jumped to as an
address. This project has hit that four separate times, which is why the rule is
structural rather than a convention.

### 2.3 Task model

```
TASK_MAX          12          TASK_STACK_WORDS  512  (2 KB per pool task)
TASK_FRAME_WORDS  23          TASK_FRAME_BYTES  112
```

Ten tasks exist at the fault: eight system tasks, the shell, and the blob task on
a dedicated 7,168-byte stack (`task_create_with_stack`, added because
`esp_wifi_init_internal` asks for 6,656 bytes and the pool provides 2,048).

The switch frame carries the sixteen general registers, `SAR`, `EPC3`, `EPS3`,
the three LOOP registers, and — since this work — `WINDOWBASE` and `WINDOWSTART`.

### 2.4 Exclusion, and its three layers

Windowed vendor code is protected by three mechanisms, each with a boundary that
matters:

1. **The blob mutex** — one context inside vendor code at a time. Cannot be taken
   from an interrupt handler, which is why ISR-context callbacks (UM-NATOS-041
   §5.3) need a different mechanism entirely.
2. **The pin** — `task_schedule()` refuses to switch away from the task inside
   windowed code. Bounded per pin, so a wedged call still reaches the watchdog.
3. **The spill** — a task about to block reduces itself to one live window frame,
   so the restore has something simple to reconstruct.

Windowed frames **do not** survive preemption on this kernel (UM-NATOS-038 §12.3,
measured by disabling the pin and watching `wintorture` panic). The pin is not an
optimisation; it is what makes windowed code viable at all.

---

## 3. Current state, measured

```
boot          11 PASS  0 FAIL
wintorture    CORRECT
wincollide    runs=118  wrong=0
blobphy       rc=0, phystack 1296/6144
blobtx force  0x00003004  (ESP_ERR_WIFI_IF)
wifiinit      PANIC after ~68 s of initialisation
heap          79736 B usable
```

Everything except `wifiinit` is healthy, and `wifiinit` now runs roughly sixty-
eight seconds deep into driver initialisation before failing, against dying
immediately before UM-NATOS-041's fix.

---

## 4. The fault

```
exccause 28 (LoadProhibited)   DEPC 0x400800d5   excvaddr 0x00000170
DOUBLE EXCEPTION               last osi: entry 29  _queue_recv

uf frame @0x3ffb2820
  -24 0x8008da53   -20 0x3ffb4278
  a0-16 0x3ffd8f78   a1-12 0x3ffb27e0   a2-8 0x8008da60   a3-4 0x4008c684
  +0 0x00000003      +4 0xffffffff
```

`0x400800d5` is `_WindowUnderflow8 + 0x15`, the instruction `l32e a4, a7, -32`.
The handler had already executed:

```
l32e a0, a9, -16     -> 0x3ffd8f78   = &adc_ana_conf_org, a blob .bss global
l32e a1, a9, -12     -> 0x3ffb27e0
l32e a7, a1, -12     -> 0x00000190   = 400
l32e a4, a7, -32     -> FAULTS at 0x170
```

The call path is established from the vendor image: the blob's `ppTask` — its
main packet-processing task — calls OSI table entry 29 with
`block_time_tick = -1`, passing its own stack pointer as the destination buffer.

---

## 5. What has been positively eliminated

Each of these was measured, not argued, and each is a *negative* result that cost
a build and a run:

| candidate | verdict | how |
|---|---|---|
| the restore drops frames | **no** | "no task was ever granted less than it held", every switch |
| the chain runs off the stack top | **no** | terminator intact for every task; the bad frame is mid-chain |
| the bridges' save areas | **complete** | `[sp-16]` and `[sp-12]` both written since step 92 |
| `a12` clobbered by a callee | **no** | "a12 survived every call0 callee" |
| our spill writes bad save areas | **no** | 11 frames walked, 10 valid encodings, the 11th a terminus |
| the blob uses call widths we mishandle | **no** | `ppTask` is CALL8-only; no CALL12 anywhere in it |
| a switch frame overlaps the frame | **no** | overlap test never fires |
| the save area is shifted | **no** | `a1` lands where the frame size demands |
| `bit(base) CLEAR` is an anomaly | **no** | it is the normal state inside an underflow handler |
| the blob re-enables interrupts via our API | **no** | `phy_enter/exit_critical` called 0/0 times |
| stack-pointer corruption anywhere | **no** | "bad sp: none — every saved sp was inside its own stack" |

The scheduler, the bridges, the spill, the restore, the terminator and the window
ownership machinery are each individually cleared.

---

## 6. What the fault therefore is

One word — the `a0` slot of one frame's base save area — holds a pointer to a
blob global where a windowed return encoding belongs. Its three neighbours are
correct, `a1` is exactly the value the frame arithmetic demands, the address is
where it should be, and the memory around it is intact.

That is the entire remaining defect.

---

## 7. A correlation, now CONFIRMED

`excvaddr` is `0x00000170` in **every** run of this fault, across builds in which
every address moves. The faulting instruction is `l32e a4, a7, -32`, so:

```
a7 = 0x170 + 32 = 0x190 = 400
```

And in `kernel/wifi_osi_impl.c`:

```c
#define OSI_FOREVER_CAP 400u        /* ~4 s at the current tick */
...
if (ticks == OSI_MAX_DELAY && spent >= OSI_FOREVER_CAP) { ... }
```

**`a7` equals `OSI_FOREVER_CAP` exactly.** `a7` is loaded from `[a1-12]`, and
`spent` is the loop counter in `osi_impl_queue_recv` that counts to precisely
that value. The `_WindowUnderflow8` fault also first appeared in step 86 — the
step that introduced the cap.

The reading this suggests: the memory the handler is walking as a frame chain
contains `osi_impl_queue_recv`'s **local variables**, not saved registers, and
`a7` is that function's spilled loop counter.

**This is a correlation, not a finding.** Three specific ways it could be
coincidence or misleading:

- 400 is a round number; a different variable could hold it.
- The cap changed timing as well as memory, so "appeared with the cap" is not
  evidence of a memory relationship.
- The address `[a1-12]` is only known to be `spent`'s home by inference.

The test is cheap and decisive: change `OSI_FOREVER_CAP` to a distinctive value —
`0x2A2A` — rebuild, and read `excvaddr`. If it becomes `0x2A0A`, the identity is
proven. If it stays `0x170`, the correlation is a coincidence and this section
should be struck.

**The run exists.** `OSI_FOREVER_CAP` 400 -> 460 moved `excvaddr` from `0x170`
to `0x1ac` — exactly `0x1CC - 32`. The identity is proven: `a7` is `spent`, and
the window handler is reading a call0 function's local variable as a stack
pointer. The chain has an `a1` link pointing into a frame that was never windowed.

See `next_moves/08` step 103. §5's eliminations are unaffected — they concern
frames that *are* windowed; this is a link pointing at one that never was.

---

## 8. Method — what this investigation has cost, and why

Fourteen distinct accounts of this fault have been proposed and retired, each
fitting the evidence available when it was written. The failures cluster into
kinds worth naming, because they recur:

- **Instruments that could not report what they were trusted to rule out** — ten
  instances, catalogued in UM-NATOS-041 §7. The most expensive produced a
  *positive* reading (an uninitialised `EXCSAVE` satisfying its own filter) which
  is far more persuasive than a silence and no more true.
- **Constants carried across rebuilds.** `_phy_stack_top` cost four steps;
  `ppTask sp + 48` cost two more, and the contradiction was visible in the same
  run's own output.
- **Values read without asking which context produced them.** `bit(base) CLEAR`
  is routine inside an underflow handler and was treated as an anomaly for three
  steps.
- **Windows too short.** A 50-second capture reported "the fault is gone" for a
  fault that arrives at 68 seconds.

The pattern that has worked, every time it was used: **name the thing by address
and read a range, not a word.** Reading four words of the save area settled in
one run what a single word had left open for three steps. The pattern that has
failed, every time: reasoning about which function *must* be responsible.

---

## 9. Recommendations

1. **Run the §7 test first.** It is one constant, one build, one run, and it
   either hands over the mechanism or removes the last standing lead.
2. **Do not add instrumentation to `shell.c`.** It is the first object in
   `.flash.text`; adding to it shifts everything the flash MMU maps and walks
   into the step-7 layout band, which is reproducible and still unexplained.
3. **The instrumentation debt is now substantial** — probes across `window.S`,
   `task.c`, `panic.c`, `wifi_osi_stubs.c` and `wifi_osi_impl.c`, several built
   on premises since disproved. It needs a deliberate pass, file by file with a
   build between each; an attempt to do it as an end-of-session tidy-up cascaded
   into three build failures and was reverted.
4. **`OSI_FOREVER_CAP` is scaffolding.** Everything downstream of it describes
   what the driver does when its queue times out, not what it does normally. It
   comes out when interrupts are wired, and `g_osi_capped` will say whether it
   still fires.
5. **The remaining WiFi work is larger than this fault.** Interrupts were never
   wired (`_set_intr` clamps and counts), timer entries are stubs, `_task_delay`
   returns immediately, event callbacks never fire, and there is no data path
   above the MAC. This fault is a blocker, not the last one.

---

Full experiment log: `docs/next_moves/08-wifi-via-loaded-blob.md`, steps 78–102.
Companion reports: UM-NATOS-038 (rev 1.5), 039, 040, 041 (rev 2.0).

**Nothing has been on air.**

Written by: Hare
