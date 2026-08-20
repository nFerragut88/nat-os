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

## Timing: do 04's measurement first

[04](04-scheduler-timing.md) is listed as "nothing needs it yet". This is what
will need it.

A relay node has a duty-cycled radio and receive windows it has to be awake for.
`fair maxwait=36` is ~340 ms worst *observed* wait, and 04 names `store_save()`
as the suspect — a flash erase with interrupts masked, tens of milliseconds
where nothing runs at all.

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
