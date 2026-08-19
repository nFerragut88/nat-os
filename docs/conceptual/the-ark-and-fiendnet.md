# The Ark and FiendNet — Revised

**Status: conceptual.** Nothing here is committed to. This keeps the original
thesis, which is the strongest of the three conceptual documents, and rewrites
the engineering around it to be honest about difficulty, about what NatOS would
and would not contribute, and about which parts are load-bearing.

The originals are kept at `DTN_Ark.txt` and `FiendNet.txt`.

---

## 1. The thesis, which stands

One sentence in the original does all the work:

> Internet says "now or never". NatOS says "eventually, always".

That is the idea, and it is correct.

The systems people need most in a disaster are the ones that fail first, because
the infrastructure is itself the casualty. Cell towers lose backhaul or mains
power. Congestion collapses what survives. Everything in the stack — TCP, DNS,
TLS session resumption, every app built on them — assumes an end-to-end path
exists *right now*, and returns an error when it does not.

**A message that arrives in three days is worth infinitely more than one that
never sends.** Delay-tolerant networking is the design that takes that
seriously: store the message, carry it, forward it when a link appears, and hold
custody until someone else accepts it. It is not speculative — it is RFC 9171,
and NASA uses it for interplanetary links where round-trip times are measured in
hours and outages in days.

Applied to a flooded county rather than to Mars, it says: the network does not
need to exist at the moment you press send. It needs to exist *eventually*, in
pieces, over time.

That thesis is kept entirely. Everything below is about pointing it somewhere it
can actually save someone.

---

## 2. What this is actually for

The original aims at the planet: Fairplay to Tokyo, 50,000 nodes, a replacement
internet. That is the least defensible part of it and also **the least
life-saving part**, and those two facts are related.

Nobody dies for want of reaching Tokyo.

People die in the **first seventy-two hours, within about ten kilometres of where
they already are.** The messages that matter are:

- *I am alive, I am at the school.*
- *We need insulin at 14 Elm Street.*
- *The bridge on Route 9 is gone, do not send trucks.*
- *Third floor, two people, we can hear tapping.*

Those are short, local, urgent, and unroutable when the towers are down. They are
also exactly what a few dozen LoRa nodes on churches, water towers and fire
stations can carry.

So the target is not a replacement internet. It is a **local resilience layer**:

| | replacement internet | resilience layer |
|---|---|---|
| Scale to be useful | ~50,000 nodes | ~30 nodes, one county |
| Time to first value | years | one season |
| Who deploys it | a movement | a volunteer fire department |
| Failure if adoption stalls | total | none — 30 nodes still work |
| Competes with | Starlink, ISPs, carriers | nothing; the alternative is silence |

The second column is achievable this year and saves someone. The first column is
a much larger bet that pays nothing until it is nearly complete.

**A resilience layer that works in one county is not a smaller version of the
global mesh. It is the only version that has a customer.**

---

## 3. The alignment the original misses

This is the most important technical point in this document and it appears in
neither original.

NatOS **cannot do WiFi or Bluetooth.** The `call0` ABI decision permanently
excludes Espressif's radio blobs; UM-NATOS-027 and UM-NATOS-028 spent days
establishing that receive works and transmit does not, and the remaining leads
all require abandoning the ABI the whole kernel is built on. That has been
treated as this project's ceiling.

**LoRa is not in that category.**

An SX1262 is a plain SPI peripheral with an interrupt line and a documented
register set. No vendor blob. No windowed-ABI bridge. No PHY binary. No
`libpp.a`. It is, in kernel terms, closer to the SD card than to the WiFi MAC.

And the board has room for it:

| resource | state | relevance |
|---|---|---|
| SPI2 (HSPI) | display | taken |
| **SPI3 (VSPI)** | **entirely unclaimed** | free for a radio |
| SD, touch | bit-banged GPIO | proven fallback if SPI3 is wanted elsewhere |
| Interrupt matrix | working, UM-NATOS-023 | DIO1 needs exactly this |
| Persistence | checksummed flash record, survives power cycles | bundle custody |
| microSD | read working | bundle storage |

**LoRa is the one radio this kernel can own end to end.** Every capability NatOS
was locked out of on WiFi, it gets for free here — and it gets the whole stack,
with no binary in it that nobody can read.

That is not a small thing for infrastructure whose entire pitch is that you can
trust it when everything else has failed.

---

## 4. Why this kernel's weaknesses do not bind here

The other two conceptual documents both broke on the same measured number:

```
fair maxwait=36        ~340 ms worst case for a ready task
```

The robot needs `every 5ms { balance.update() }` and cannot have it. The
scheduler is *fair*, not *real-time*.

**A DTN node does not care.** Its entire design premise is tolerance of delay
measured in hours and days. A 340 ms scheduling jitter is six orders of magnitude
inside its tolerance. This is the first application considered for NatOS where
the kernel's known limitation is simply irrelevant.

It goes further than that. Run down the list of things this kernel is bad at:

| known weakness | does it bind? |
|---|---|
| No WiFi or Bluetooth | **No** — LoRa needs neither |
| Fair, not real-time (~340 ms) | **No** — DTN tolerates days |
| Slow display, no damage tracking | **No** — a node barely draws |
| ~240 KB RAM | **No** — bundles live on flash and SD, not in RAM |
| No filesystem | **Mostly no** — a bundle store is append-only with an index |
| One core (APP_CPU never started) | **No** — the workload is not compute |

And the things it is unusually good at all bind directly:

| strength | why it matters here |
|---|---|
| Arena isolation, bounds-checked every access | a relay carries other people's payloads |
| Per-application device permissions (UM-NATOS-032) | a node can run a neighbour's app without giving it the radio |
| Bounded VM execution | a misbehaving app costs its quantum, not the node |
| Checksummed persistence across power loss | custody must survive a dead battery |
| Every wait bounded and counted | a node that hangs in a flood is a node that killed someone |

**This is the application this kernel was accidentally designed for.** Not the
robot, not the language. A device that must sit on a roof for years, run
untrusted code, survive power loss, and never hang.

---

## 5. What the originals get wrong

### 5.1 Meshtastic is not "80% there" — it is the network you should join

The original frames Meshtastic as a partial solution to be surpassed. That
undersells it and misreads the problem.

Meshtastic solved the radio layer, the mesh routing, the encryption and the
flashing experience, and it has a deployed installed base. **A life-saving
network's value scales with density, not with elegance.** A better protocol with
one node is worth less than a worse protocol with two hundred, because the metric
is whether a message gets out of the valley.

The right posture is interoperation, not replacement: speak the existing
protocol, join the existing mesh, and add what is missing on top. Starting a
second incompatible network halves the density of both.

What is genuinely missing above it — long-TTL bundle custody for things larger
and slower than chat — is a real gap and worth building. That is an *addition*,
which is a much smaller and much more defensible claim than a rival.

### 5.2 The political framing costs lives

"Bypass greed", "no permission", "route around the FCC" — this is the part I
would cut hardest, and not on ideological grounds.

The organisations that deploy emergency communications are county emergency
management, search and rescue teams, volunteer fire departments, rural clinics
and the Red Cross. They have the towers, the funding, the mandate and the people
standing in the flood. **None of them can touch a thing that brands itself as
ungovernable**, and without them the nodes never get on the roofs.

It is also unnecessary. LoRa in the ISM bands is already legal to operate. There
is nothing to route around. Framing it as defiance invites the one intervention
that could actually stop it, in exchange for nothing.

Same technology. Same independence from any single provider. Different sentence
describing it, and the different sentence is the one that gets an antenna on a
water tower.

### 5.3 Strike the medical devices

> Fridges, tractors, insulin pumps run NatOS.

Cut this line. It inverts the entire document.

Medical device software carries IEC 62304, hazard analysis, formal verification
obligations and redundancy requirements, and acquiring that regime is measured in
years and specialists, not in obsession. It is not a matter of the kernel being
good enough — it is a matter of the evidence required being a different kind of
thing than this project produces.

The honest argument against it is in the project's own record. Today the driver
was found to have been displacing **every DMA transfer it ever sent**, invisible
behind six counters that all agreed it was working (UM-NATOS-033). That is not a
criticism of the kernel; it is the *normal* condition of systems software, and it
is precisely why pumps carry the certification burden they do.

Note the asymmetry that makes the rest of this document safe:

- **Comms infrastructure fails late.** The message arrives in three days.
- **A pump fails fatal.**

Everything else here is a system whose worst case is a delay. Keep it that way.

### 5.4 Mesh money, mesh DNS, reputation, mesh GPS

All four are scope creep away from a working thing, and each is a research
project. "Fiend credit" is a distributed ledger with a friendly name. Mesh GPS by
node triangulation needs time synchronisation this hardware does not have.

They are not wrong ideas. They are the ideas you write down and do not build
until the boring version has been in a flood.

### 5.5 The bandwidth is not a detail

The original waves at "300bps of pure freedom" as though it were a slogan. It is
a constraint that decides what the system can be.

At LoRa's slower settings a 10 MB PDF is not "slow", it is **structurally
impossible** across a shared medium serving a town — one such transfer would
occupy the channel for weeks, and the channel is the thing everyone's *I am
alive* has to fit through. Regional regulations add dwell-time and duty-cycle
limits on top.

This changes the design rather than defeating it, and the change is in the right
direction:

- Text and coordinates, not documents. A life-critical message is under 200 bytes.
- Priority is not optional. *I am alive* must pre-empt everything.
- **The bulk case belongs on sneakernet**, which the original already saw. A USB
  stick in a truck is enormously higher bandwidth than any radio here, and the
  400-page book travelling by cargo ship is a *correct* answer — it just is not a
  radio answer.

---

## 6. What NatOS actually contributes

Not a soul. Two concrete things.

**Auditability.** Emergency infrastructure is short of systems whose behaviour
can be *proved* rather than hoped for. This kernel's habits — every wait bounded
and counted, a counter that must be zero beside one that must be non-zero, a
written record of every retracted claim — are unusual and they are worth more
here than anywhere else the project has considered pointing them.

**Isolation with permissions.** A relay node that can run a neighbour's
application, in an arena it cannot escape, with a device grant it cannot widen,
is a genuinely useful object and not a common one at this price. It is what makes
a shared node safe to be shared.

Neither is glamorous. Both are the actual product.

---

## 7. A build order that is real

Grounded in what is already in the tree rather than in what would need inventing.

**Phase 0 — one link.** Two boards, SX1262 on SPI3, send a packet, receive it,
print it. No mesh, no bundles, no encryption. The only question being answered is
whether this kernel can drive this radio, and it is answerable in an evening.

**Phase 1 — one bundle that survives.** A signed message with a destination and a
90-day TTL, written to the existing persistence record, still present after the
power is pulled, forwarded to a second node when it appears. This is the whole
thesis in miniature and almost all of it already exists — persistence works, SD
works, the device model is the right place for the radio to live.

**Phase 2 — join, do not replace.** Speak the existing protocol well enough to be
a node on a mesh that already has neighbours. Density is the metric.

**Phase 3 — the thing nobody else does.** Long-TTL custody for payloads too big
and too slow for chat, with priority so the small urgent messages always win, and
opportunistic sync over any link that appears — including a USB stick.

**Phase 4 — one county.** Thirty nodes, real sites, real owners, real solar. A
tabletop exercise with the local emergency manager. This is the phase that
matters and the one every project like this skips.

---

## 8. What would actually make this fail

Software is not the risk. These are, in order:

1. **Nobody keeps the nodes charged.** The projects that die, die here. A node on
   a roof is a physical object with an owner, a battery and a duty of care, and
   the hard problem is social, not technical.
2. **Density never arrives.** A mesh below critical density is a set of
   disconnected radios. This is the argument for interoperating rather than
   starting fresh.
3. **It is never tested against a real disaster before one happens.** Untested
   emergency equipment is decoration. Exercises with the people who would use it
   are not optional.
4. **Scope kills it.** Every hour on mesh money is an hour not spent on the thing
   that carries *third floor, two people*.
5. **The framing keeps it out of the hands that would deploy it.** See §5.2.

---

## 9. What to keep from the originals

The sentence. *Eventually, always.*

And the marble analogy in `FiendNet.txt`, which the author apologises for and
should not — the correction it makes to itself, that the marbles stay still and
the electrons move, is exactly right and is the clearest short description of
store-and-forward routing in any of these documents.

---

## 10. The honest summary

The original asks: what if we replaced the internet?

The realistic version asks: **what if, when the towers go down, a county could
still get short messages to each other for a week?**

The second one is smaller, achievable, and the one where someone is alive who
would not otherwise have been. It also happens to be the application this
kernel's peculiar shape suits better than any other yet considered — because the
things NatOS is bad at do not matter to it, and the things it is unusually good
at are exactly what it needs.

That is not a consolation prize for the big vision. It is the big vision with a
first customer.
