# 13 — Bluetooth

Stage 1 is one sentence: get Espressif's BT controller to answer `HCI_Reset`
with a Command Complete event. Nothing else proves the blob, the OSI shims, the
ABI bridge, the PHY and the interrupt routing are all working at once; and once
it does answer, advertising and scanning are configuration rather than
archaeology.

Stage 2 is advertising — a phone sees the board. Stage 3 is scanning, and a
native view that replaces the rogue cell on the desktop while `run gfxrogue`
keeps working from the shell.

---

## Step 1 — the controller runs, and asks for a level-1 interrupt

Not finished. `HCI_Reset` has not been answered yet. What follows is where it
actually stops, which is six layers further in than where it started.

### what was built

`vendor/bt/patch_bt.py` takes the four archives (`libbtdm_app`, `libbt`,
`libcoexist`, `libphy`), renames the symbols that collide with the kernel's
own, emits a `bt_rom.ld` mapping the ROM entries, and prints what is left as
the specification for the host:

```
symbols needed       3608
answered internally  3218
from esp32.rom.ld     244
renamed to the ROM     32
LEFT -- this file     120
```

Those 120 are `vendor/windowed/bt_osi.c` (windowed, the ABI the blob calls
with) forwarding into `kernel/bt_host.c` (call0, where the kernel is).
`kernel/bt.c` is the driver: PHY, configuration, enable, VHCI, one HCI command.
`build.ps1 -BT` links it; `-BTTrace` adds a shim trace.

Memory came from moving the PHY's text to flash, shrinking lwIP's buffers in a
`-BT` build and gating the WiFi blob's 7 KB task stack: heap 9,288 → 30,344 B,
IRAM 122,840 of 131,072.

### six things that were wrong

Each of these presented as something other than what it was, which is the only
reason they are worth writing down.

**1. `.btdm_bss` was never zeroed.** It is a `NOLOAD` section with its own
marker pair, placed before `.data` so the controller's data and bss are each
contiguous and the markers bracket all of it. That placement puts it outside
`_bss_start.._bss_end`, and `start.S` zeroes exactly that one range — so 2,904
bytes of the controller's uninitialised state were whatever DRAM held at reset.
It presented as

```
exccause 28 (LoadProhibited)  epc 0x401128e5  excvaddr 0x12250025
```

which is `semphr_delete_wrapper +0x9` deleting a semaphore whose handle had
never been assigned. Not a Bluetooth bug at all: a section that looked zeroed
because every other bss is. The boot segment list says it plainly once you look
— `0x3ffb0b58..0x3ffb16b0` appears in no segment.

**2. `periph_module_enable()` was `{ (void)module; }`.** An uncounted stub, the
one kind this project is not allowed to have. The BT clock stayed gated and the
BT resets stayed asserted, so every register the controller polled read back a
constant. `wifi_osi_impl.c` already carries a long comment about the identical
mistake on the WiFi side (`_wifi_clock_enable` was empty, and the radio reported
success and did nothing). The module number is **26**, logged rather than
guessed, because this tree has no `periph_module_t` header to check against.

**3. `intr_route()` bounded `line` but not `source`.** `DPORT_PRO_MAP(src)` is
`base + 4*src` with no limit, so a bad source does not fail — it writes the line
number wherever that arithmetic lands. The controller asked for
`ETS_INTERNAL_SW1_INTR_SOURCE`, which is IDF's **-5**: internal sources are
negative and do not come through the matrix at all. The write went to

```
0x3FF00104 + 4 * 0xFFFFFFFB = 0x3FF000F0
```

twenty bytes below the array, in DPORT's clock and reset block, four words from
`PERIP_CLK_EN` and `CORE_RST_EN`. This is a kernel hole, not a BT one, and it is
now counted and refused (`intr_bad_sources()`).

**4. `BT_LINES` contained line 15 — the scheduler tick.** `intr_route()`
installs by overwriting `g_handler[line]`, so the controller's second allocation
would have replaced the kernel's clock with a BT handler. Nothing would have
reported it; the board would simply have stopped keeping time, in a build that
had just been given Bluetooth to blame. Lines 11 and 29 were in that list too
and belong to the internal profiling and SW1 interrupts. What is actually free
at level 3, once the kernel's 15, 23 and 27 are set aside, is line 22 — one
line, stated honestly instead of padded out to four with lines that belong to
someone else.

**5. The bring-up cannot run on `phy_stack_call`.** This was the thirty-second
freeze: no ticks, no task switches, no output, until the hang detector reset the
board. The shim trace ended on

```
. xTaskCreatePinnedToCore      the controller creates its own task
. esp_coex_version_get
. xQueueGenericSend            it posts work to that task
. xQueueSemaphoreTake          and waits for the answer
```

which is not a deadlock in the semaphore. It is `esp_bt_controller_init`
**blocking**, which is ordinary, and which `phy_stack_call` cannot survive: the
wait reaches `wait_on()` → `task_sleep()`, and the scheduler then switches away
from a task whose stack pointer is on the shared 6 KB `_phy_stack`.
`phyinit.c` **pins** the scheduler across its own `phy_stack_call` for exactly
that reason and says so — "nothing is ever saved with an sp on `_phy_stack`".
The PHY only gets away with that mechanism because `register_chipv7_phy` never
blocks.

So the bring-up moved to a task that owns its stack (`btboot`, 6 KB), entering
the blob through `rom_call4`, where blocking is legal. The hang detector's
window went back to 3,000 ms: the 30-second widening it needed before was a
symptom of running the blob where it could not block, not a requirement of
Bluetooth.

**6. The controller's own task entered windowed code from call0.**
`bt_task_tramp` called `g_bt_entry(g_bt_arg)` directly. That function is
windowed blob code and the trampoline is call0, so the first window overflow in
the callee spilled relative to a caller-sp fetched from `[a1-12]` — a slot only
a windowed prologue writes — and the second fault vectored to the
double-exception handler. No panic, no output, nothing. `rom_call4` now builds
the base frame on the task's own stack.

### where it stops

With all six fixed, the trace gets this far:

```
. xQueueSemaphoreTake
. xQueueReceive          the controller's task is running and taking work
. esp_read_mac           it reads the MAC address
. xt_ints_on
   [bt] refusing to enable lines 0x20  -- not level 3
. xt_set_interrupt_handler
   [bt] handler wanted on CPU line 8   (NOT level 3)
. xt_set_interrupt_handler
   [bt] handler wanted on CPU line 7   (NOT level 3)
*** KERNEL PANIC *** exccause 20 (InstFetchProhibited) epc 0x00000000
```

CPU lines 5, 7 and 8 are **level 1**. nat-os dispatches level 3 only, from
`_handler_level3`, and a level-1 line enabled with nothing behind it is not a
lost interrupt — it is `exccause 4` on the first arrival, which is how this was
found. Refusing them is strictly better than granting them, and is what the
kernel now does, out loud.

But the controller is not asking idly. IDF's FreeRTOS port dispatches level 1;
that is its ordinary interrupt path. So this is a real missing kernel feature
rather than a shim to adjust, and the `epc 0x00000000` immediately after is the
controller calling through a pointer it expected those handlers to have left.

### State

```
works   image links and boots: 498 KB, IRAM 122,840/131,072, heap 30,344 B
works   PHY calibrates through the ABI bridge, 1,296 of 6,144 B of stack
works   esp_bt_controller_init runs, allocates, creates its task, and that
        task runs: queue receive, esp_read_mac, interrupt requests
works   the ABI bridge both ways, on three different stacks
open    the controller needs LEVEL-1 interrupt dispatch (lines 5, 7, 8).
        This kernel has none. That is the next step and it is a kernel
        feature: a level-1 vector, its context save, and how it interacts
        with the scheduler and with crit_enter's level raising
open    exccause 20 at epc 0 right after the refused handlers -- expected to
        be a consequence of the above, to be re-checked once level 1 exists
open    HCI_Reset has never been sent; stage 1's actual test has not run
note    `bt` is shell-only and brings the radio up on first use. Nothing
        Bluetooth runs at boot: a radio that wedges would take the shell
        with it
```

---

## Step 2 — level-1 interrupts, and the DRAM the ROM already owns

Level-1 dispatch works and is tested. It was not the last blocker, and the one
behind it is a memory-map constraint that had not been suspected.

### level-1 interrupts

"Add a level-1 vector" was the wrong description of the work. The Xtensa
architecture does not give level 1 a vector of its own — it arrives at the
**general (user) exception vector** with `EXCCAUSE = 4`, which is precisely why
every level-1 arrival in this kernel has presented as

```
*** KERNEL PANIC ***  exccause : 4  (Level1Interrupt)
```

The vector was always being taken. What was missing was the two instructions
that tell a level-1 interrupt apart from a fault, and a dispatcher behind them.
`_vector_user` now goes to `_handler_user`, which checks `EXCCAUSE`, dispatches
if it is 4, and otherwise hands `a0` back untouched and falls through to
`_handler_panic` exactly as before.

Three things it does differently from `_handler_level3`:

- **No context switch.** The level-3 handler is also the scheduler's switch
  point and carries the whole register-window investigation with it. This one
  returns to the context it interrupted, so it saves only what C may clobber:
  `a0`, `a2..a11`, `SAR`, and the LOOP registers. `a12..a15` are callee-saved.
- **`EPC1` travels in the frame.** Level 1 gets no `EPCn`/`EPSn` pair of its
  own: the interrupted PC is in `EPC1` and the interrupted PS is `PS` with
  `EXCM` set. A window exception inside the dispatcher is itself an exception
  and would overwrite `EPC1`, so it cannot be trusted to survive the call.
- **`INTLEVEL 3`, not 1.** Handlers are non-reentrant and the scheduler is
  locked out: no tick, no switch, no second level-1 arrival on top of the
  first. IDF leaves them preemptible; this does not, and the cost is that a
  handler which blocks stops the clock. That is the right trade for a first
  version — if something later needs to block in one, the answer is to hand
  work to a task, not to lower this.

The 48-byte reserve below the interrupted `sp` is the same hazard step 145
found on the level-3 path: those bytes are a `CALL12` frame's extended save
area.

Proven rather than asserted, at boot:

```
[6c] level-1  : PASS  software line 7 asserted, handler ran, line cleared
```

Line 7 is the level-1 software interrupt, so the whole path is reachable from C
with no peripheral and no radio. The flag is cleared first and only the handler
can set it, so the test can fail. A software line stays asserted until
`INTCLEAR` clears it — the same obligation a level-triggered peripheral carries
— and the handler clears it.

With that in place the controller's requests are granted instead of refused:

```
[bt] handler wanted on CPU line 8 (level 1, serviceable)
[bt] handler wanted on CPU line 7 (level 1, serviceable)
```

### the real blocker: the BT ROM owns DRAM nat-os is using

The controller still dies, and in the same place as before, so the NULL call was
never a consequence of the refused handlers:

```
*** KERNEL PANIC *** exccause 20 (InstFetchProhibited)  epc 0x00000000
fault regs: a0 0x8010fb35
```

`a0` is a windowed return address, so the caller is `0x40000000 | 0x0010fb35` =
**0x4010fb35**, which is `r_rwip_init +0xc5`. The `r_` prefix is the giveaway:
that is the ESP32's **ROM-resident** Riviera-Waves IP, and ROM code reaches the
blob through function-pointer tables at **fixed DRAM addresses**. From
`esp32.rom.ld`:

```
r_ip_funcs_p      = 0x3ffae70c
r_modules_funcs_p = 0x3ffafd68
r_plf_funcs_p     = 0x3ffb8360
rwip_rf           = 0x3ffbdb28
```

`0x3ffb8360` is in the fault's own register dump — the ROM was reading its
platform-function table. And nat-os's DRAM starts at `0x3FFB0000`, so:

```
r_ip_funcs_p       0x3ffae70c  below nat-os DRAM -- ROM-reserved, untouched
r_modules_funcs_p  0x3ffafd68  below nat-os DRAM -- ROM-reserved, untouched
r_plf_funcs_p      0x3ffb8360  --> g_store  (+0x3a4)    the settings struct
rwip_rf            0x3ffbdb28  --> g_stacks (+0x784)    a task stack
```

The ROM's BT data region is where this kernel keeps its persistent settings and
its task stacks. `r_rwip_init` reads a function pointer out of what is actually
`g_store`, gets zero, and calls it.

This is what IDF's `CONFIG_BT_RESERVE_DRAM` is for: `0xdb5c` bytes from
`0x3FFB0000`, ending at `0x3FFBDB5C` — immediately past `rwip_rf`. The number
checks out against the symbol addresses exactly, which is the useful part: the
reservation is not a safety margin, it is the ROM's data.

Nothing earlier could have caught this. The region is not declared, not
referenced by any symbol the link resolves, and writing to it is perfectly legal
right up until ROM code reads it back.

### the budget this creates

```
nat-os DRAM            0x3FFB0000..0x3FFD3000   143,360 B
BT ROM reservation     0x3FFB0000..0x3FFBDB5C    56,156 B
left for the kernel                              87,204 B
currently static (.data + .bss + btdm)          113,508 B
heap on top of that                              30,360 B
```

So a `-BT` image does not fit, and is short by about 56 KB — which is roughly
what the stacks cost: twelve task stacks at 2 KB is 24 KB, `_phy_stack` is 6 KB,
and the two stacks this work added (`btboot` 6 KB, `btctrl` 5 KB) are 11 KB.

The way out is already in the linker script and nearly unused:

```
sram1 (rw) : ORIGIN = 0x3FFF1000, LENGTH = 0xF000    /* 60 KB */
```

60 KB against the 56 KB that has to be vacated. Stacks and diagnostic buffers
need no DMA and do not care which SRAM they live in, so moving `g_stacks`,
`g_regsave`, `_phy_stack` and the two BT stacks into `.sram1` is the obvious
shape of it — and `g_store` has to move regardless, since it is sitting on
`r_plf_funcs_p`.

### State

```
works   level-1 interrupt dispatch, [6c] PASS at boot, line 7 round trip
works   the controller's level-1 handler requests are granted (lines 5, 7, 8)
works   everything from step 1 still holds; default image unaffected and all
        boot self-tests pass
open    0x3FFB0000..0x3FFBDB5C must be reserved for the BT ROM's data, and
        nat-os must vacate it. g_store and g_stacks are the two confirmed
        occupants; there may be more above 0x3ffbdb28
open    about 56 KB of static DRAM has to move, probably into the 60 KB of
        sram1 at 0x3FFF1000 that is currently almost empty
open    whether sram1 at 0x3FFF1000 is wholly usable on this silicon has not
        been checked -- it is NOLOAD and nothing has leaned on it yet
open    HCI_Reset still never sent
note    INTLEVEL 3 in the level-1 handler is a deliberate simplification.
        Revisit only with a reason, and a test
```
