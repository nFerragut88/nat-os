# 10 — LoRa Phase 1: one bundle that survives

**Size:** large. **Risk:** low technically; the risk is scope.
**Blocked on:** [09](09-lora-one-link.md).

`docs/conceptual/the-ark-and-fiendnet.md` §7, Phase 1:

> A signed message with a destination and a 90-day TTL, written to the existing
> persistence record, still present after the power is pulled, forwarded to a
> second node when it appears. This is the whole thesis in miniature and almost
> all of it already exists.

---

## Why it is smaller than it sounds

The concept doc is right that most of this is built. What Phase 1 needs:

| need | state |
|---|---|
| a radio | [09](09-lora-one-link.md) |
| storage that survives power loss | `store.c`, working since UM-NATOS-018 |
| bulk storage | SD, working |
| a place for the radio to live | the device model, UM-NATOS-031 |
| isolation for a neighbour's code | arenas + permissions, UM-NATOS-032 |

**What does not exist:** the bundle itself — a header, a TTL, a destination, an
identity — and the forwarding decision.

## The honest scope warning

This is the item most likely to sprawl, because the concept doc's later phases
are genuinely exciting and none of them belong here. Phase 1 is **one bundle,
one hop, surviving one power cut.** Not a mesh, not routing, not encryption
beyond a signature, not Meshtastic compatibility — that is Phase 2 and its own
entry when the time comes.

The measure of success is embarrassing in its smallness: pull the power, plug it
back in, and the message is still there and still gets delivered.

## The identity problem, which is already recorded

UM-NATOS-032 §3 states it plainly: nat-os has **no image identity** — no
signature, no content hash — so what exists is *containment, not security*, and
the `store` device's per-slot banks have the same gap.

A bundle with a *signed* payload needs a notion of who signed it. That is the
same missing primitive, and Phase 1 is the first thing in the project that
actually requires it rather than merely noting its absence.

**Decide deliberately whether Phase 1 includes identity or defers it.** Deferring
is defensible — an unsigned bundle that survives a power cut still proves the
thesis in miniature — but it should be a decision written down, not an omission
discovered later.

## Timing: 04's measurement is DONE, and the number is 125 ms

[04](04-scheduler-timing.md) was listed as "nothing needs it yet". This is
what needed it, and the measurement has been taken. **Read the section at the
end of this file before designing anything that has to be awake on time.**

A relay node has a duty-cycled radio and receive windows it has to be awake for.
`fair maxwait=36` is ~360 ms worst *observed* scheduler wait, and 04 named
`store_save()` as the suspect — a flash erase with interrupts masked. It
guessed "tens of milliseconds". **It is 125 ms**, measured, five times.

**A node that writes a bundle to flash while a receive window opens will miss
it.** That is exactly the interaction 04 exists to measure, and the measurement
is cheap. Do it before designing the radio's duty cycle, not after diagnosing a
node that drops packets when it is busy.

## Verification

The thesis is "it survives", so the test is a power cut, not a reboot:

1. Node A accepts a bundle with a destination and a TTL.
2. **Pull the power.** Not `reset` — the supply.
3. Reapply. The bundle is still there, with its TTL correctly reduced by the
   elapsed time or correctly marked as unknown-elapsed, whichever the design
   chose deliberately.
4. Node B appears. The bundle is forwarded, once, and marked delivered.
5. Node B is removed and returns. The bundle is **not** forwarded again.

Step 5 is the one that catches a design that works and does not scale.

## Related

- `docs/conceptual/the-ark-and-fiendnet.md` §7 — the phases, and §2 for what it is for
- UM-NATOS-018 — the persistence this rests on
- UM-NATOS-032 §3 — the identity gap this is the first user of
- [04](04-scheduler-timing.md) — measure it before designing the duty cycle

---

## The flash stall, and why it is worse than it looks

**Measured 2026-08-19, `next_moves/04`.** The number this file needs:

```
store_save()  ->  125 ms, interrupts masked, nothing else runs
                  every ~8-26 s from the render loop (STORE_EVERY_FRAMES 256),
                  plus once at boot and on any device write
```

**A LoRa receive window is shorter than 125 ms.** A node that erases flash while
a window opens does not receive *late* — it does not receive. And writing a
bundle down is a relay node's *normal* behaviour, not an exceptional one.

So the naive design — "store the bundle when it arrives" — drops the next
packet, reliably, and presents as a node that loses traffic in proportion to how
much traffic it is given. That is a horrible thing to debug in the field and a
cheap thing to design around now.

### Do not expect the scheduler to fix it

`next_moves/04` measured this and reached a conclusion that matters here: a
timer-driven callback bypassing the scheduler **does not help**, because the
erase masks interrupts and blocks a tick-ISR callback exactly as it blocks a
task. No scheduler design preempts a masked interrupt.

### And the obvious fix is blocked by UM-NATOS-037

Narrowing the critical section around the erase looks correct — the 125 ms is a
status-register poll, not a hardware requirement. It is not currently safe:

> While erasing, an SPI NOR chip cannot serve a read. Since UM-NATOS-037,
> `shell.c` and `kmain.c` execute **from flash**. Letting the scheduler run
> during an erase may enter a task whose instructions cannot be fetched.

The critical section is doing two jobs and only one is obvious. The full
argument and the three ways out are at `flash_erase_sector()` in
`kernel/flash.c`, written where someone attempting the fix will be standing.

### Decide the write policy before the duty cycle

Not after debugging a node that drops packets when busy. None of these need
kernel work:

- **Write between windows.** The radio driver knows when it is idle; the bundle
  layer can ask. Cheapest, and the metadata path probably wants it.
- **Batch.** One erase per N bundles rather than per bundle. `store.c` already
  defers with `g_dirty` for exactly this reason — extend that idea rather than
  invent a new one.
- **Write payloads to SD instead**, where a block write is milliseconds and
  needs no erase at all. The bulk path exists, and Phase 1's payloads are not
  small.

The third is probably right for payloads and the first for metadata — but that
is a decision to make deliberately, and the point of recording it here is that
it should be made before the radio's timing is designed rather than discovered
afterwards.
