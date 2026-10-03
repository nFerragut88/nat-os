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
