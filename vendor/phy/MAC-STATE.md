# The MAC is running

Measured on hardware, not inferred. Follows MAC-NEXT.md.

## What was done

open-mac's entire MAC initialisation:

```c
#define MAC_CTRL_REG _MMIO_DWORD(0x3ff73cb8)
static void init_mac() { MAC_CTRL_REG = MAC_CTRL_REG & 0xffffe800; }
```

plus one thing open-mac gets from ESP-IDF and nat-os had to do itself: ungating
`DPORT_WIFI_CLK_WIFI_EN` (0x406) in `DPORT_WIFI_CLK_EN_REG`, on top of the
WiFi/BT common bits (0x3C9) that `phyinit` already sets.

## Why the evidence is a register scan and not a readback

A readback proving the mask took effect would prove almost nothing. This kernel
has now been caught three separate times by a peripheral whose registers read
back exactly as written while the hardware did nothing — LEDC gated behind
`DPORT_PERIP_CLK_EN` bit 11 (UM-NATOS-027 §3.3) being the clearest. **A
successful store is not evidence.**

So the test is: read the 4 KB MAC window, wait, read it again, and count words
that changed with nothing driving them. A gated block is static. A running MAC
has free-running counters in it, and no write that went nowhere can fake one.

| stage | moving words |
|---|---|
| cold boot | **0** |
| after `phyinit` | **6** |
| after `macinit` | **13–15** |

## The TSF timer, identified by behaviour

Rates across the window (`maclive`), normalised against measured CPU cycles:

```
0x3ff73c00  1000 kHz   <- stable across every run
0x3ff73c14  1054 -> 1102 kHz
0x3ff73c18  1055 -> 1107 kHz
0x3ff73d80   527 ->  460 kHz
0x3ff73d84/88/8c   2 kHz  (three consecutive words, identical rate)
0x3ff73db4    16 kHz
0x3ff73dd0  1063 -> 1114 kHz
0x3ff73dd4  1054 -> 1107 kHz
```

`0x3ff73c00` is the only word that reports **exactly 1000 kHz on every run**;
the others drift between samples. Confirmed over a long interval against the
CPU's own cycle counter — two clocks with no connection to each other:

```
tsf advanced 622458 over 622 ms of cpu time -> 1000 kHz
tsf advanced 508686 over 508 ms of cpu time -> 1001 kHz
```

Agreement to 0.1% over half a second is not something a misread address
produces. **0x3ff73c00 is the 802.11 TSF timer**, and nat-os now has a 1 MHz
microsecond timebase — which it did not have before, its tick being 10 ms.

This identification rests on behaviour alone. Every other address in this
region remains a reverse-engineered guess.

## What did NOT happen

`MAC_CTRL_REG` read `0x00000000` both before and after, so `0 & 0xffffe800` was
a **no-op**. The 6 -> 13 jump in moving words came from the clock enable, not
from open-mac's write.

That is not a failure — `init_mac()` is a clearing operation and at cold boot
there is nothing to clear — but it means this register write is **currently
unverified**. It will matter once the blob has set those bits.

## Unrelated bug found on the way — now fixed

`task_sleep()` was returning without sleeping. Found because the first TSF
check used it and reported ~1 us for a requested 500 ms, with the CPU cycle
counter and the MAC's TSF independently agreeing.

TWO causes, and the first diagnosis in this file was wrong about the second:

**1. `timer_ticks()` counted ISR entries, not time.** `task_yield()` ends a
slice by pulling CCOMPARE1 to `ccount + 64`, so `timer_isr` ran on every yield
as well as every deadline — and `g_ticks++` sat at the bottom of it
unconditionally. Measured at **217 ticks per real second** where 100 is
correct, and far higher when a task sat in an idle-yield loop. Every deadline
expressed in ticks came due early, in proportion to how much yielding the
system happened to be doing. Fixed by advancing the clock only when the
deadline has actually passed: **217 -> 105**.

**2. `task_yield()` arms a switch; it does not perform one.** It writes the
comparator and returns, so the caller keeps running for ~60 cycles into what it
believes is a sleep. `task_sleep(50)` — half a second — returned to its caller
in **107 cycles**, the cost of the function body and nothing else. The sleep
did happen; it started after the caller had already read the clock. Fixed by
re-arming until the deadline genuinely arrives, with a `woken` flag so
`task_wake()` can still cut a sleep short — `touch_irq_wait()` depends on that
and a naive loop would have turned every early wake into a full-length one.

Verified: `task_sleep(50)` now takes 639 ms / 64 ticks, and 64 ticks x 10 ms =
640 ms — the tick count and the wall clock agree, which they did not before.
The overshoot past 50 is scheduling latency for a NORMAL-priority task waiting
behind the HIGH-priority display task, which is correct behaviour for a sleep.

### It made the 3D view 44% faster

| | before | after |
|---|---|---|
| ticks per real second | 217 | 105 |
| raycaster blit | 55.85 ms | **31.4 ms** |

Every spurious tick cost a full context save, a scheduler run and a restore.
Removing them was pure gain — the timing fix was expected to risk the frame
rate and improved it instead.

The uncomfortable part is the same one `timer.c` already noted about an earlier
bug in this function: **nothing showed a symptom.** Sleeps ran short, timeouts
ran loose, and every frame-rate figure this project has recorded was taken
against a clock that ran fast by a load-dependent factor.

---

# Receive: built, armed, not yet receiving

## Now built from open-mac's actual source

Earlier steps used addresses recalled rather than read. The register set has now
been checked against esp32-open-mac directly, and the recalled ones were right:
`WIFI_DMA_INT_STATUS` 0x3ff73c48, `WIFI_DMA_INT_CLR` 0x3ff73c4c, `MAC_CTRL_REG`
0x3ff73cb8, and the interrupt handler shape (read status, return if zero, write
it back to clear) is identical.

What could NOT be recalled, and is now taken verbatim, is the descriptor:

```c
typedef struct __attribute__((packed)) dma_list_item {
	uint16_t size : 12;
	uint16_t length : 12;
	uint8_t  _unknown : 6;
	uint8_t  has_data : 1;
	uint8_t  owner : 1;
	void    *packet;
	struct dma_list_item *next;
} dma_list_item;
```

A bitfield order guessed wrong here would hand the DMA engine a length where it
expects a flag and corrupt memory silently — the one class of error this kernel
has no defence against. It was worth fetching rather than reconstructing.

Four buffers of 1600 bytes, against open-mac's ten: 6.4 KB rather than 16 KB, on
a board with no PSRAM and about 21 KB of heap. Fewer buffers drops frames under
burst load; it does not stop reception.

## What the hardware confirmed

- `macrx` returns 0 — `update_rx_chain()` set bit 0 of 0x3ff73084 and the
  hardware **cleared it**, which is an acknowledgement, not a readback.
- `WIFI_NEXT_RX_DSCR` reads back 0x3ffd7028 — one of our own descriptors. The
  DMA engine ingested the base pointer and walked into the chain.

That is as far as it goes. Zero interrupts, zero filled descriptors.

## Where it stops: chip_v7_set_chan_nomac

Nothing had tuned the radio, so nothing could arrive. open-mac's sequence is

```c
deinit_mac(); chip_v7_set_chan_nomac(ch, 0);
disable_wifi_agc(); init_mac(); enable_wifi_agc();
```

`chip_v7_set_chan_nomac` panics: **exccause 29, StoreProhibited**, inside
`ram_chip_i2c_readReg`.

The faulting instruction is not the RF access. Disassembled, it is a `call8` —
so what failed is the WINDOW OVERFLOW SPILL the call triggered, writing register
state to a stack with no room left. That reframes it from an RF problem into a
stack problem.

nat-os gives every task 2 KB. That is generous for the call0 kernel and far too
little for windowed blob code, where every nested `call8` can spill four
registers. ESP-IDF gives its WiFi tasks 4 KB and up.

**Attempted fix:** `phy_stack_call()` in window.S, which switches to a private
6 KB stack for the duration of a PHY call — the same trick `_panic_stack`
already uses, and it means only tasks that enter the blob pay for it.

**Result: the fault moved forward but did not go away.** 0x40094efe ->
0x40094f2e, past the first `call8` and onto a later one. So depth was part of it
and is not the whole story.

## Next lead

The remaining fault is at a `call8` whose argument register is loaded with
0x3ff4e0c4 — a peripheral address, not one of ours. Worth checking before
anything else:

1. Whether 6 KB is simply still not enough (raise it and see if the fault walks
   further again — if it does, it is purely depth).
2. Whether `phy_enter_critical`/`phy_exit_critical` in vendor/windowed/phy_host.c
   behave correctly when re-entered from this path. They are depth-counted, but
   that counting was verified for phy_init, not for a retune.
3. Whether the PHY needs state that `register_chipv7_phy` alone does not set —
   ESP-IDF's `esp_phy_enable()` does more than call it.

Nothing on the normal boot path is affected: `chan` is opt-in, and phyinit,
macinit, macirq, macrx and the 3D view all work.

---

# It receives

```
802.11 frame: BEACON  len=348
bssid 44:25:38:19:0d:1a   ssid "TC7NR"
```

A real beacon from a real access point, decoded on a kernel built `-mabi=call0`
running Espressif's PHY blob through hand-written window handlers.

## What the fault actually was

Three theories, two of them wrong, and the measurements that killed them were
worth more than the fix:

**1. Stack depth.** `chip_v7_set_chan_nomac` died with StoreProhibited at a
`call8`, so the PHY was given a private 6 KB stack. The fault moved forward but
did not go away — which looked like progress and was not. Priming the stack with
a pattern and reporting the high-water mark from the panic handler settled it:
**272 bytes of 6144 used.** Depth had never been the problem. Without that
number the obvious next step was a bigger stack, and it would have failed too.

**2. Stale WINDOWSTART bits.** nat-os's context switch saves a0..a15 and neither
saves nor spills the register window, so frames from an earlier task can still be
marked live. Declaring exactly one live frame before entering the blob changed
nothing.

**3. The base frame was not a windowed frame.** This was it.

`_WindowOverflow8` — the handler in this very repository, line 69 — spills
`a4..a7` relative to the caller's stack pointer, which it fetches with:

```asm
    l32e    a0, a1, -12             /* a0 <- call[j-1]'s sp */
```

Every windowed frame must hold its caller's stack pointer at `sp - 12`. `ENTRY`
does not write it; the caller's prologue does. `phy_stack_call` is call0 code and
wrote nothing there, so the handler read a fresh `.bss` zero and spilled to
`0 - 32`.

That also explains the misleading fault address. The spill faults *inside* the
overflow handler, which runs with `PS.EXCM` set, so the second fault vectors to
the double-exception handler while `EPC1` still holds the instruction that caused
the original overflow — the `call8`. The reported PC was never where the store
was.

The fix is one store: put a valid stack pointer at `sp - 12` before `callx8`.

## The receive path

Frames do not begin at the start of the DMA buffer — the hardware prepends a
**40-byte receive control header** (RSSI, rate, channel, timestamp). Measured,
not documented: found by locating where the beacon actually started, which is
unmistakable because address1 of a beacon is `ff:ff:ff:ff:ff:ff` and cannot land
at the right offset by accident.

Evidence the frames are genuine rather than uninitialised memory:

- `has_data` set by hardware on 3 of 4 descriptors, all initialised to 0
- the bytes CHANGE between captures — different RSSI and timestamps each run
- the 802.11 header decodes: type 0 subtype 8, broadcast address1, addr2 == addr3
- the SSID tag parses to printable ASCII naming a real network

## Continuous reception

Descriptors are now recycled, per open-mac's `rs_recycle_dma_item`: `length` is
restored to `size`, `has_data` cleared, and the item appended to the TAIL so
buffers rotate instead of one being reused while the rest sit idle. `owner` is
deliberately left alone -- open-mac does not touch it and this is not a place to
improvise.

A `wifirx` task polls and drains, bounded to ten frames per pass. Bounded
because on a cooperative scheduler a busy channel can otherwise keep the loop
fed indefinitely and starve everything else.

Soak result:

```
frames=181  recycled=182  networks=2      ...then, minutes later...
frames=372  recycled=374  networks=2
   44:25:38:19:0d:1a  x199  "TC7NR"
   38:88:71:2d:c1:cd   x53  "Verizon_S6QHX4"
```

With four buffers, 372 frames can only come from reuse. Reception also
continued through a 3D rendering session, which is the interesting part: the
radio and the display share nothing but the scheduler, and neither starved.

`recycled` runs slightly ahead of `frames` because runt descriptors are handed
back without being counted as frames. That is intended, and the gap is the
count of them.

## Not done

- **Interrupts never fire.** `irq fires=0` while DMA fills descriptors happily
  -- which is why reception is polled. Routing installs and the line is not
  disabled, so either source 0 is the wrong index or the MAC's own interrupt
  mask has not been enabled. Polling works; an interrupt would be cheaper.
- **Only the latest frame is decoded.** There is no queue: a frame arriving
  before the previous one is read replaces it. Fine for a scanner, not for a
  protocol.
- **Shell stack is thin on the WiFi path.** 1856 B free at rest, 528 B while
  the WiFi commands run. `execute()` is one long if/else chain so GCC sums every
  branch's locals into one frame; the larger buffers are now static, which
  recovered most of it. The guard is intact and `corrupt=0`, but this wants
  splitting into functions before more commands are added.
- **Only one TX slot** is used, of the several the hardware has.
- **No association, no data frames.** Beacons only.


---

# It transmits

```
tx handed to hardware=178  completions reaped=178   forced=0
```

Registers and ordering from open-mac's `transmit_80211_frame`, unrearranged:
`TX_CONFIG |= 0xa`, descriptor address into `PLCP0`, the PLCP/duration words,
two more `TX_CONFIG` bits, and only at the very end the `0xc0000000` in `PLCP0`
that actually puts the frame on the air.

Note the base/offset pairs index BACKWARDS -- `MAC_TX_PLCP0_OS` is -2 on a
`uint32_t` array, so slot n sits 8 bytes BELOW the base. Slot 0 is the base.

The completion count is the point. A transmit call returning 0 proves only that
some register stores did not fault, which is the weak evidence this project has
been caught by three times. The hardware setting a completion bit is the MAC
saying it put the frame out.

## Two bugs found by watching the counters

**Beacons paced against loop iterations, not the clock.** First version counted
passes of the rx task and beaconed once every four seconds instead of ten times
a second. The loop rate is not fixed. Repaced against `timer_ticks()`.

**The rx task was starved.** Even repaced it managed 3 Hz, because `fair
maxwait=36` -- a NORMAL-priority task can wait 36 ticks, 360 ms, behind the
HIGH-priority renderer, so a 10 ms sleep became a third of a second. Raised to
HIGH, matching the display: **9.4 Hz**, and the raycaster blit did not move
(55.84 ms).

The useful measurement was the one that ruled the obvious suspect OUT.
`update_rx_chain()` spins on a hardware acknowledge and looked like the cost;
instrumenting it showed a worst-case wait of **one** iteration. It was never the
radio, it was the scheduler.

**Reusing a descriptor still in flight.** Every frame shares one descriptor and
one buffer, and the counters showed 421 sent against 273 reaped -- frames handed
over while the previous was possibly still being read by DMA. Guarded with a
pending flag that self-clears after 50 ms, so a completion that never arrives
cannot stop transmission for good. Now 178/178 with zero forced clears.

## What is beaconed

A 51-byte beacon: broadcast address1, the factory MAC as address2 and BSSID,
100 TU interval, ESS capability, SSID, basic rates 1 and 2 Mbps, and a DS
parameter naming the tuned channel. Sequence control is filled in per frame --
a receiver that sees a repeated sequence number treats the frame as a
retransmission and drops it, which would look exactly like transmit not working.

Beacons rather than arbitrary frames because a completion bit proves the MAC
accepted the frame, and only a second radio proves it reached the air.
