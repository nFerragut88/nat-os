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
