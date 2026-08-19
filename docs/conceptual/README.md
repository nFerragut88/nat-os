# Conceptual documents

Ideas, not commitments. Nothing here is a plan and nothing here constrains the
kernel; the numbered UM-NATOS reports remain the record of what is actually
true.

Each idea exists twice: the original as written, and a revision that keeps the
thesis and rewrites the engineering around it to be honest about difficulty.
Both are kept, because the revision is only meaningful next to what it revised.

| idea | original | revision |
|---|---|---|
| A high-level language for NatOS | [PDF](NatScript%20Language%20and%20NatVM%20Architecture%20Proposal.pdf) | [natscript-and-natvm.md](natscript-and-natvm.md) |
| A small robot NatOS could drive | [PDF](The%20Small%20Embodied%20AI.pdf) | [the-small-embodied-ai.md](the-small-embodied-ai.md) |
| Comms that survive the infrastructure | [DTN_Ark.txt](DTN_Ark.txt), [FiendNet.txt](FiendNet.txt) | [the-ark-and-fiendnet.md](the-ark-and-fiendnet.md) |

## What the revisions changed

**NatScript.** The language is the least defensible part of the proposal —
MicroPython, CircuitPython, Espruino and Arduino all exist and win on ecosystem.
What none of them has is **application isolation on a microcontroller**, which
nat-os does have and which the original undersold. `when` and `every` turn out
to be the same missing feature, and it is a VM feature: an application today is
one linear thing with one program counter, and nothing can call into it. The
prior art is IEC 61131-3 and ladder logic rather than Java. Bytecode portability
is worth less than it sounds.

**The robot.** The thesis holds — a small robot for cognitive and social
assistance is a real idea. The engineering underneath it was optimistic: small
bipeds are *harder* than large ones because they fall faster, recovery is a
bigger problem than walking, and speech does not fit on this class of chip. At
the top of a robot NatOS competes with ROS2 and loses; **at the joint it
competes with a bare firmware loop and wins**, which is a smaller and much more
defensible claim.

**The Ark.** The strongest of the three, and the only one whose thesis needed no
weakening at all: a message that arrives in three days beats one that never
sends. What changed is the target. The originals aim at replacing the internet
globally, which is both the least defensible part and **the least life-saving
part** — nobody dies for want of reaching Tokyo. People die in the first
seventy-two hours within ten kilometres of home, which is a thirty-node problem,
not a fifty-thousand-node one. Also cut: the medical-device claim, which inverts
a system whose worst case is a delay into one whose worst case is fatal.

## The finding that makes the Ark different

The other two revisions both broke on the same measured number (below). The Ark
does not, and the reason is worth stating on its own.

Every known weakness of this kernel is irrelevant to a delay-tolerant radio node
— no WiFi (LoRa needs none), fair-not-real-time (DTN tolerates days), small RAM
(bundles live on flash), no filesystem (the store is append-only). Every unusual
strength binds directly: arena isolation and per-application device permissions
for carrying other people's code, checksummed persistence for custody across
power loss, bounded waits for a device that must never hang on a roof.

And one alignment neither original noticed: **LoRa is the only radio this kernel
can own end to end.** An SX1262 is a plain SPI peripheral — no vendor blob, no
windowed-ABI bridge, no PHY binary — and SPI3 is entirely unclaimed. Everything
NatOS is permanently locked out of on WiFi, it gets for free here.

## The one finding that touches today's kernel

Both revisions converge on the same measured number:

```
fair maxwait=36        ~340 ms worst case for a ready task
```

`every 5ms { balance.update() }` cannot be honoured by a scheduler with that
bound. The scheduler is *fair*, which is not *real-time* — ageing guarantees
nothing starves forever, and guarantees nothing about when.

That is a real constraint on a real kernel, discovered by reading a speculative
document about a robot that does not exist. It is the reason to keep writing
them.
