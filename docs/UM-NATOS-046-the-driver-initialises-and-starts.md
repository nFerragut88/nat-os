# UM-NATOS-046 — The Driver Initialises, and Starts

**Used Medias LLC — Embedded Systems Division**
Revision 1.0 · 2026-08-23 · Status: **Milestone. `esp_wifi_init_internal()` and `esp_wifi_start()` both return ESP_OK. The whole suite passes in one boot.**

---

## 1. Abstract

`esp_wifi_init_internal()` returns `ESP_OK`. `esp_wifi_start()` returns `ESP_OK`.
Forty-six of the 118 OS adapter entries are exercised, the driver's own timers
are bound and serviced, its MAC interrupt is routed to a line this kernel can
service, and every command in the test suite passes in a single boot for the
first time in this investigation.

Nothing has been on air. §8 states the basis for that rather than asserting it,
because for the first time it is not trivially true.

This report covers `next_moves/08` steps 177–192. UM-NATOS-045 covers 163–176
and should be read first.

Sixteen steps, eleven defects, and four wrong diagnoses that are recorded at the
same weight as the right ones, because three of them were wrong in the same way.

---

## 2. State before and after

| | after 045 (step 176) | now (step 192) |
|---|---|---|
| `esp_wifi_init_internal` | never returned | **ESP_OK** |
| `esp_wifi_start` | never called | **ESP_OK** |
| adapter entries exercised | 20 | **46** |
| driver timers | every call a silent no-op | **15 bound, 0 refused** |
| MAC interrupt | never requested | **routed, line 27, serviceable** |
| driver heap use | 104 B / 4 allocations | **23,820 B / 26 allocations** |
| suite in one boot | `wifiinit` hangs | **all green** |

```
boot        11 PASS 0 FAIL
wintorture  switches during the call: 10  (preemption really happened)
            checksum 1632 expected 1632  CORRECT
blobphy     phyinit rc=0
wifiinit start
            phyinit rc=1        (guarded, correctly)
            init      returned 0x00000000  (ESP_OK)
            start     returned 0x00000000  (ESP_OK)
            [intr] src=0 line=27 prio=1 routed=1 nofn=0 fired: none
                   timers=15 refused=0
heap free 47,880 B
```

---

## 3. Where the driver was actually stuck

UM-NATOS-045 left the blob waiting in `_queue_recv` and offered no account of
why. Three instruments settled it, all of them recording values the kernel
already held at the point of recording:

- the **calling task** for every adapter call, one store in `osi_trace()`;
- every queue handle handed to the blob, and the caller of every `_queue_recv`;
- `reached`/`running`/`returned` around the worker's `blob_lock()`.

```
[who] 5 5 5 5 5 5 5 5 5 5 5 5 5 5 5  9  9  9  9  9
[osi] 19 41 21 87 41 22 8 19 85 42 93 13 42 36 15 16 29 29 29 29
[wrk] reached=1 running=1 returned=0   mutex owner=9 depth=2 waiters=0x20
[sem] blocking_take done=1 rc=1 relocked=0
```

Almost none of it was what had been supposed.

**There was no deadlock.** The worker took the blob lock and ran.

**The worker was not the stalled party.** All four `_queue_recv` calls are
task 9 on the one queue `_wifi_create_queue` made — which in ESP-IDF is the WiFi
task's event queue, fed by the MAC ISR. An empty queue there is the normal idle
state.

**The stalled party was the init task.** Task 5's last call is `_semphr_take`;
task 9's first is `_semphr_give`. That is the standard startup handshake and it
**succeeded** — `rc=1`. `relocked=0` says where task 5 then went: blocked
re-acquiring the blob mutex, which task 9 held at **depth 2** and, with `err=0`,
never once lost.

Two defects in one lock:

**`MUTEX_FREE_W` was `-1`; `MUTEX_FREE` is `-2`.** The two halves of the same
mutex disagreed about what "free" means. `blob_unlock_w()` released by writing
`-1`; `mutex_lock()` waits for `-2` and never saw it. `mutex.h` states why `-1`
is wrong — it is the pre-scheduler boot context's task id. The `_Static_assert`
on `sizeof(mutex_w_t)` caught layout drift and could not catch a value that had
drifted.

**The blocking wait released one level of recursion, not all of them.** The
worker holds the mutex twice — `blob_task_entry()`'s `blob_lock()` plus
`rom_call3()`'s — so `osi_s_queue_recv`'s single release took it 2 → 1 and it was
never freed. A blocking wait must drop the mutex completely; that is the whole
contract. `blob_unlock_all_w()`/`blob_relock_all_w()` release every level and
restore the depth on the way back, which is safe now and was not before Tier B.

---

## 4. Six adapter entries that reported success for work never done

With the lock fixed the driver ran on and then **unwound**: `_semphr_delete`,
`_wifi_delete_queue`, `_task_delete`. The crash at `_task_delete` was the last
step of an orderly teardown, and implementing it would have moved the fault
rather than fixed it.

The cause needed no instrumentation:

```c
static void * osi_s_wifi_zalloc(size_t size) { osi_hit(92u); return 0; }
```

All four WiFi-heap allocators returned NULL unconditionally. The driver asked
for memory, was told there was none, and tore itself down.

Rather than find the rest one fault at a time, every adapter entry was scanned
for an empty or `return 0` body. Seven on the live path:

| entry | was | now |
|---|---|---|
| `_wifi_malloc` / `_wifi_calloc` / `_wifi_zalloc` | NULL | nat-os heap |
| `_task_ms_to_tick` | 0 | ms → ticks, rounded up |
| `_get_free_heap_size` | 0 | `osi_impl_free_heap()` |
| `_queue_msg_waiting` | 0 | `osi_impl_queue_waiting()` |
| `_wifi_thread_semphr_get` | NULL | per-task counting(1,0) semaphore |

Two are worth naming on their own.

**`_task_ms_to_tick` answered 0.** The driver derives the `queue_send` and
`semphr_take` timeouts on the init path from exactly this call, so every one of
them collapsed to "do not block".

**`_get_free_heap_size` answered 0** while `osi_impl_free_heap()` had existed all
along. Telling a driver it has no memory is the same defect as refusing the
allocation, one step earlier.

`_read_mac` was the same shape — `return 0` with the caller's buffer untouched —
and was implemented from eFuse, with the layout taken from ESP-IDF's
`esp_efuse_table.c` rather than recalled. Verified against ground truth rather
than by inspection, since esptool prints the MAC while flashing:

```
esptool  MAC: 5c:01:3b:50:3f:64
nat-os   base mac : 5c:01:3b:50:3f:64
```

**And it did not move the fault at all.** A real defect, and not that one.
UM-NATOS-045's successor draft had called it "the strongest lead".

---

## 5. Two words of zero, and an operating system ROM expected

### 5.1 A hole in a designated initializer

The driver jumped to address 0. `build/blob.elf` carries symbols, which nobody
had used; the worker is `ppTask`, and its prologue reads straight off our table —
offset 64 is `_semphr_give`, offset 116 is `_queue_recv`, exactly the first two
calls the trace recorded. The dispatch is:

```asm
4036bb93:  l32i   a8, a8, 216      /* offset 216 = field 54 */
4036bb96:  callx8 a8               /* not checked for null */
```

Field 54 is `_phy_common_clock_enable`. The struct in `wifi_osi_stubs.c` declares
it; the initializer goes straight from `._phy_enable` to
`._phy_update_country_info`. **A designated initializer zero-fills what it
omits**, so two words of a table handed to a driver that calls them unchecked
were NULL.

No check this project had could see it: a scan for unimplemented stubs looks for
stub *functions*, and here there is no stub. `wifi_init_cfg()` — the last code to
touch the table before the blob does — now walks the words between `_version` and
`_magic` and reports `osi table : 118 words, no null slots`.

### 5.2 The syscall table nobody wrote

Next fault: `LoadProhibited`, `excvaddr 0x00000000`. `esp32.rom.ld` alone does
not resolve the PC, but the ROM symbols are spread over ten linker scripts;
across all of them it is `__getreent + 0x8`, with `malloc` as the next symbol.

The ESP32 mask ROM contains a newlib but not the operating system those routines
need. Every ROM entry that touches libc state reaches through a table the runtime
installs at `syscall_table_ptr_pro`. Measured, before assuming:

```
rom stubs : pro=0x00000000 app=0x00000000
```

nat-os had never written either. A 36-entry table now installs at blob load, with
the same null-slot guard as §5.1's: allocators to the nat-os heap, `__getreent`
returning a writable zeroed block, the lock family deliberately empty, everything
else refusing. `_realloc_r` returns NULL rather than faking it — nat-os's heap has
no realloc and the old size is not recoverable, so a visible failure beats
invisible corruption.

---

## 6. The defect that took five steps: a bridge that armed its own destruction

### 6.1 What it was

`wifiinit` faulted with `epc 0`, `a0 = 0`, in `rom_call4`'s epilogue. Recording
where the bridge primed its frame and re-reading it at the fault:

```
chain base: at 0x3ffb9530  primed 0x4008dae8  now 0x4008dae8
            a0slot 0x3ffb9540  saved a0 0x00000000
```

The null "pointer" was not a function pointer at all. It was **`rom_call4`'s own
saved call0 return address** — `blob_call`'s — overwritten while the driver ran.
It was not null when written: the word 16 bytes lower, primed in the same
prologue, still held `win_chain_trap`. An **overwrite, not a miss**, which is
why no guard caught it: every guard added so far checks that a value was
*installed*, and this one was.

### 6.2 The writer

Checkpoints along the blocking path latched the first site that saw it zero:
immediately after `win_spill_all()`. Sixteen words of the stack, either side:

```
spill pre : ws 0x00002aaa (7 frames)  [a0 slot] 0x40081e1c
spill post: ws 0x00000800 (2 frames)  [a0 slot] 0x00000000
```

Exactly sixteen bytes at `[0x3ffb9540, 0x3ffb9550)`, nothing on either side
changed.

### 6.3 Two fixes that failed, and the reading that produced them

Sixteen bytes was read as a **base save area** — `[sp-16, sp)` for a frame with
sp `0x3ffb9550` — because that is what the ISA convention says one looks like.
That required a stale window, and both fixes built on it failed:

- **Reserving `[sp, sp+16)`.** `rom_call4` widened to 48 bytes with its saves
  moved up 16. No effect: the caller's sp is fixed, so a bigger frame moves `sp`
  down and the offset up by the same amount and the slot's address does not
  change. The danger zone is anchored to the **caller's** sp, not ours.
- **Spilling before descending.** `win_spill_call0()` in `blob_call`. Worse — the
  panic moved earlier, with `rom_call4` reporting a stack pointer inside the
  blob's DRAM.

### 6.4 The source of truth

The question that broke it came from outside the work: *does §4.7 of the book
apply here?* §4.7 is about a different overlap — a bootloader IRAM segment that
looked like it would clobber the kernel and did not — but the rule it produced
is exact:

> an apparent conflict in an address map is a prompt to read the other party's
> link script, not to move our own

The write was measured. The **interpretation** was not. nat-os's own
`_WindowOverflow8` writes **two** regions:

```asm
    s32e    a0, a9, -16      /* a0..a3 -> [a9-16, a9)     base save area */
    l32e    a0, a1, -12      /* a0 <- the CALLER's sp                    */
    s32e    a4, a0, -32      /* a4..a7 -> [a0-32, a0-16)  extended area  */
```

It was the second. With `a0 = 0x3ffb9560`, `[a0-32, a0-16)` is the measured range
exactly. **There was no stale frame.** Both readings fit the addresses; only one
fits the handler.

And `a0` is loaded from `[a1-12]` — the slot **`rom_call4` primes itself** with
the caller's sp. The bridge handed the overflow handler a pointer and the handler
wrote through it into the bridge's own 32-byte frame. It armed the write that
destroyed it.

This is **step 145's defect in a second place**. That step found the task switch
frame written through the CALL12 extended save area and answered it with
`TASK_FRAME_RESERVE 48`; the same 48 bytes were missing here. CALL8 reaches
`[caller_sp-32, caller_sp-16)`; CALL12 reaches `[caller_sp-48, caller_sp-20]`.

Three bridges had it, and the fix is the same in each — reserve 48 below the
caller, put the bridge's own saves below that:

| | before | after | evidence |
|---|---|---|---|
| `rom_call4` | 32 | 80 | measured failing |
| `rom_call3` | 48 | 80 | same defect by construction; the worker's bridge |
| `win_spill_call0` | 32 | 80 | same, and it exists to *cause* the spill |

The third explains §6.3's second failed fix. `win_spill_call0()` was eating its
own return address on the way out. The idea was sound; the tool was broken in the
same way as the thing it was meant to help.

Cost: 32 bytes of stack per bridged call.

**`esp_wifi_init_internal()` returned `ESP_OK`.**

---

## 7. Start, and the two things it needed

`esp_wifi_start()` needed a call site, and `shell.c` is the one file this project
does not add to (UM-NATOS-042 §9.2). The rule is about growth, so the bring-up
moved to `wifi_init_cfg.c` and `shell.c` got **smaller** — a three-line
`blob_call` became one line. Plain `wifiinit` still runs init only, so every
measurement taken up to step 189 stays reproducible, verified byte for byte.

It returned `ESP_OK` on the first attempt from a cold boot, and then needed two
things before it would do so reliably.

### 7.1 The driver's timers were never bound

All five ETS entries were stubs — and `wifi_osi_impl.c` had had a full timer
implementation the whole time: alloc, setfn, arm, arm_us, disarm, done, a service
task, and `timers_used()` for reporting. A duplicate emulation was written before
checking, and thrown away.

Two real defects in what already existed:

**`timer_of()` never matched anything the driver passed.** It compared the
pointer against `&g_timer[i]` — handles from `osi_impl_timer_alloc()`. The ETS
contract is that the **caller owns the structure**: the blob allocates its own
`ETSTimer` and passes its address. Every timer call was a silent no-op.

**The service called windowed code directly** — `t->fn(t->arg)`, from a call0
file. The callee never executes `ENTRY` and returns through an `a0` the caller
never set. It goes through `blob_call()` now, which also takes the mutex the
handler needs.

Nothing started the service task either. And the pool was too small:
`timers=12 refused=4` on the first working run, so `OSI_TIMER_MAX` went 12 → 24.
Refusing a timer the driver believes it armed is the same class of silent failure
this report keeps describing.

### 7.2 The interrupt line was one this kernel cannot serve

With the timers working, `esp_wifi_start` got further, armed its interrupt, and
the board panicked asynchronously in the display task:

```
exccause 4  Level1Interrupt   epc 0x40083933  (spi_tx)
```

Recording what the driver asked for:

```
[intr] src=0 line=0 prio=1
```

`ETS_WIFI_MAC_INTR_SOURCE` on `ETS_WMAC_INUM` — **priority 1**. nat-os installs
exactly one interrupt handler, at level 3. Routing faithfully and then unmasking
arms a line nothing can service.

Step 179 raised this risk and reached the wrong shape: it worried the blob might
arm line 0 *behind our backs* through ROM helpers. What actually happens was
visible in `esp_adapter.c` all along — **the driver passes the line number to
`_set_intr`**, and faithful is wrong.

The interrupt matrix does not care which line a source lands on, so the line is
remapped onto `INTR_LINE_WIFI_MAC` — 27, priority 3, extern level, served by the
existing `_handler_level3` and reserved for exactly this in UM-NATOS-042 without
ever being used. The remap is applied in **three** places or it is worse than
nothing: `_set_intr`, and `_ints_on`/`_ints_off`, which are handed a *mask* of the
driver's line numbers.

---

## 8. What is on air

Nothing. Every report in this series has ended on that sentence, and until now it
was trivially true because the driver never started. The basis, rather than the
assertion:

- **One line is routed and no interrupt has been taken** — `routed=1`,
  `fired: none`. The MAC is armed and silent.
- **No mode is set.** `esp_wifi_set_mode()` is never called, so there is no
  station or AP interface.
- **Nothing has been commanded to transmit** — no scan, no connect, no beacon.

What *has* changed: `_phy_enable` and `_wifi_clock_enable` were called, so the
PHY is powered where before it was not. Powered is not transmitting, and no path
to a transmit exists yet, but the sentence no longer means "the radio hardware
was never touched", and it should not be allowed to carry weight it has stopped
having.

---

## 9. Method: four wrong diagnoses, three of them the same mistake

Sixteen steps produced eleven defects and four confident wrong answers. Naming
them is cheaper than repeating them, and this stretch repeated one.

**The last thing recorded before a fault is not its cause.** Step 186 read
`a8 = 0x8008d964`, resolved it to the instruction after `callx8 a2` in
`rom_call4`, and concluded the bridge had been called with a null target. It had
not — `rom_call4` was then made to refuse a null target and record the caller,
and it reported `n 0`. The register was a stale leftover the fault never touched.
Step 190 did the same thing with `last osi : entry 57 _timer_disarm` and named
the timer stubs as the cause of a fault they had nothing to do with.

**Geometry instead of the source of truth, three times.** §6.3 read a sixteen-byte
write as a base save area because that is what the convention says one looks
like, and two fixes were built on it. Step 191 concluded that `blobphy` followed
by `wifiinit start` failed because both call `phyinit` — from the call graph,
without reading the function, which has guarded itself for a hundred steps and
prints `phyinit rc=1` when it refuses. §4.7 of the book names this exact error,
and it was made twice more after being quoted.

**Self-overwriting globals, for the third time in this investigation.** The
window snapshots either side of the spill were recorded unconditionally and
printed `[a0 slot] 0` on **both** sides — which reads as "already dead before we
got here". `osi_s_semphr_take` runs more than once, so the singleton described
the last call while the latch beside it described the first. Steps 79 and 183
recorded the same trap in `a0/sp out`; it was reintroduced in a fresh probe two
steps after being written up.

**Instruments that had to be checked before they were believed.** `a0/sp out` is
written by `w2c_call*` on its way out, not by the fault. `saved frame @` dumps
the current task's last switch-out, which is stale by construction when that task
is the one that faulted. `sbp-post` prints its verdict off a never-sampled
sentinel. A chain of reasoning was built on the first of these and had to be
retracted. `_handler_panic` now records the true faulting registers before it
takes the panic stack, which it never did:

```
fault regs: a0 0x8008d765  a1 0x3ffb28b0  wb 15  ws 0x00008000
```

The one that worked every time was reading the other party's code — IDF's
headers, `esp_adapter.c`, `libc_stubs.h`, `esp_efuse_table.c`, the ROM linker
scripts, `blob.elf`, and nat-os's own window vectors. Every one of those was
cheaper than the reasoning it replaced.

---

## 10. What remains

1. **`_get_random`** still reports success without filling the caller's buffer —
   the defect `_read_mac` had. Named in step 186 and still unfixed.
2. **A mode and a scan.** The first action that would put anything on air, and
   not to be attempted casually.
3. **The instrumentation debt**, which has grown again: the step-188 brackets and
   stack dumps have served their purpose, `a0/sp out` and `saved frame @` are
   superseded by `fault regs` and actively mislead, and UM-NATOS-042 §9.3 has
   warned about this since step 102.
4. **The `w2c_*` bridges** still allocate over their caller's base save area.
   §6 fixed the three call0 bridges; the windowed ones were not touched and
   remain as UM-NATOS-045 §8.4 describes them.

---

## 11. One consequence, stated plainly

`blob_init()` zeroes the blob's `.bss`, and it was not guarded.
`phyinit_run_at()` then fills a good deal of that `.bss` with calibration data
and the pointers `register_chipv7_phy` leaves behind — and guards itself, so it
can never rebuild them.

Running `blobphy` and then `wifiinit start` in one boot therefore ran
`blob_init()` twice and `phyinit` once: the second `blob_init()` erased PHY state
that nothing would restore, and `esp_wifi_start` dereferenced what had been
wiped. The two guards disagreed, and the destructive function was the one
without one.

`blob_init()` now returns immediately when the image is already loaded, and that
is what makes the suite pass in a single boot.

It is not free, and the cost belongs in the record rather than in a footnote:
**reloading a newly flashed blob image now requires a board reset.** Flashing a
new blob and re-running `wifiinit` will silently use the image already in
memory. That is the correct trade against a function that erases live state
whenever it is called twice — a reset is visible, cheap, and something the
operator chooses; the erasure was none of those. But it is a behaviour change,
not a free fix, and anyone who flashes a blob and sees the old one's behaviour
should look here first.

---

Full experiment log: `docs/next_moves/08-wifi-via-loaded-blob.md`, steps 177–192.
Companion reports: UM-NATOS-038 (rev 1.5), 041 (rev 2.0), 042 (rev 1.1),
043 (rev 1.3), 044 (rev 1.0), 045 (rev 1.0).

Written by: Hare
