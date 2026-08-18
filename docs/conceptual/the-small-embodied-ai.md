# The Small Embodied AI — Revised

**Status: conceptual.** Nothing here is committed to. Revision 2 keeps the
original thesis, which is sound, and rewrites the engineering around it to be
honest about difficulty and about what NatOS would and would not contribute.

The original is kept at `The Small Embodied AI.pdf`.

---

## 1. The thesis, which stands

A robot does not need to be human-sized to be useful.

A large humanoid earns its size by interacting with a human-scale physical
environment — carrying, opening, climbing, lifting. A small robot cannot do any
of that and should not try. What it can do is **cognitive and social**: remember
appointments, hold notes and timelines, answer questions about recent events,
communicate with family, and provide a persistent conversational presence that
exists in the room rather than inside a phone.

For someone whose memory is failing, continuity that is simply *present* — that
notices, turns, speaks, and remembers yesterday — is a different thing from an
application you must remember to open. That is the argument, and it is a good
one. It does not depend on the robot being impressive.

Roughly 17 inches. Wide stable legs, narrow torso, low centre of mass.
Prioritise standing, walking, falling safely, and getting up — not stairs.
Indoor floors, rugs, mild slopes.

All of that is kept.

---

## 2. What the original underweights

The incremental path in the original is right. The middle of it is heavier than
it reads.

### 2.1 Small bipeds are harder than large ones

This is counter-intuitive and it matters. Balance time constants scale roughly
with the square root of height: a shorter robot falls **faster** and has less
time to correct. A 17-inch biped needs a *higher* control rate than a 5-foot
one, not a lower one, and has less mass to work with when it does.

Small commercial bipeds get there with high-rate serial-bus servos, good IMUs
and a lot of tuning. None of that is exotic, but "small" is not "easy".

### 2.2 Falling and recovering is harder than walking

Walking on a flat floor is a solved problem with enough servo quality. Falling
without breaking, detecting that you have fallen, working out which way up you
are, and getting back to standing is a much larger behaviour, and it is the one
that determines whether the robot survives a week in a real room.

The original lists it in one clause. It deserves its own phase.

### 2.3 "Notices when someone speaks and turns toward them" is not on this chip

That sentence needs, at minimum:

- **wake-word detection** — feasible on a small DSP or a modest MCU
- **sound-source localisation** — needs a mic array and real DSP
- **speech recognition** — realistically off-device
- **speech synthesis** — realistically off-device or a large lookup

An ESP32-class part does none of the last three. The honest architecture is a
**bigger brain somewhere**: a Pi-class board or a phone or a home server doing
audio and language, with NatOS-class controllers doing the body. That is not a
compromise, it is the correct split — see §4.

### 2.4 Continuous presence is a battery problem

"Exists in the environment all day" and "walks" are in tension. Servos idle at
non-trivial current just holding a pose. A robot that stands all day is a robot
that is docked most of the day and walks in short bursts — which is fine, but it
should be a design decision rather than a surprise.

---

## 3. What NatOS actually contributes

The original says NatOS would be "the physical operating layer" for the whole
robot. That claim is too broad, and being precise about it makes the project
more defensible rather than less.

**At the top of a robot, NatOS competes with ROS2 and loses.** ROS2 has the
ecosystem, the tooling, the message types, the community and twenty years of
accumulated robotics. Rewriting that is not a side quest.

**At the joint, NatOS competes with a bare firmware loop and wins**, for the
same reason it is interesting on the handheld:

| property | why it matters at an actuator |
|---|---|
| application isolation | a misbehaving control law kills one joint, not the leg |
| offset-domain bounds checks | a bad index cannot corrupt the joint that still has to hold you up |
| the device model | encoder, current sense, temperature, brake — all one interface |
| `caller` identity | which controller is allowed to command this joint |
| fault recording | a joint that failed can say why on the next boot |

So the defensible architecture is **one NatOS per actuator or per limb**, with
something larger reasoning above it. The AI decides to walk toward a person;
NatOS holds a joint at a commanded angle and refuses commands that violate its
mechanical limits.

That is a real niche, it is small enough to actually build, and the device model
shipped this week is already the right shape for it: a servo is a `device_t`
with position on one channel, current on another, temperature on a third. One
word in, one word out.

---

## 4. The honest layering

```
speech, language, memory, intent        off-device (Pi-class, phone, server)
        ↓  commands, goals
body coordinator                        Pi-class or larger MCU
        ↓  joint targets, gait phase
NatOS per limb or per joint             ESP32-class
        ↓  PWM, encoder, current, limits
physical actuator
```

The original's diagram put NatOS in the middle of everything. This one gives it
the layer it is actually good at and is honest that the interesting AI is not
running on a microcontroller.

---

## 5. The blocker nobody has mentioned

`every 5ms { balance.update() }` is the load-bearing line of the whole concept,
and **nat-os cannot honour it today.**

From tonight's telemetry:

```
fair maxwait=36
```

Thirty-six ticks — roughly **340 ms** worst case for a ready task to be
scheduled. A balance loop that can be starved for a third of a second is not a
balance loop; it is a fall.

The scheduler is *fair*, which is not the same as *real-time*. Ageing guarantees
that nothing starves forever. It guarantees nothing about when.

**This is a scheduler property, and it will not be fixed by adding motors.**
Before the first joint moves, nat-os needs either:

- a **priority class above ageing** that a control task can occupy, with a
  measured worst-case latency, or
- a **timer-driven callback path** that does not go through the task scheduler
  at all — a control ISR, with the scheduler for everything else.

The second is how most motor control is actually done, and it is probably the
right answer. Either way the number that matters is *measured worst-case jitter
at the control rate*, and nat-os has never measured it because nothing has ever
needed it.

Worth knowing now, because it changes what the scheduler has to become, and the
scheduler is load-bearing for everything else.

---

## 6. Revised sequence, with honest weights

| phase | what | rough weight |
|---|---|---|
| 0 | Handheld device on NatOS, Bluetooth | months — mostly done |
| 1 | **Measure and fix control-loop timing** (§5) | weeks, and prerequisite |
| 2 | One joint: PWM out, encoder in, closed position loop, limit enforcement | weeks |
| 3 | Two-joint limb: kinematics, coordinated motion | weeks |
| 4 | One leg: standing, holding against a push | months |
| 5 | Two legs: walking on flat floor | **months to a year** |
| 6 | **Falling safely and recovering** | its own project |
| 7 | Arms, gestures | weeks, once 2–5 hold |
| 8 | Audio and language, off-device, over a link | separate track, can run in parallel |

Phases 0–3 are genuinely achievable and each produces something that works on
its own. Phase 5 is where hobby biped projects usually stop, and phase 6 is
where most never start.

**That is not an argument against doing it.** It is an argument for treating
phases 2 and 3 as the real milestone — *a joint that holds position reliably
under load, running an isolated control program that cannot corrupt its
neighbour* — because that is both genuinely useful and genuinely novel, and it
is reachable this year.

---

## 7. What the robot is for, restated

The original's closing is right and worth keeping:

> The body provides presence and agency; the AI provides intelligence; memory
> provides continuity; and NatOS provides the interface between all of them and
> the physical world.

With one correction: NatOS provides the interface between the *body* and the
physical world. The intelligence lives somewhere with more memory than this, and
pretending otherwise would put the hardest problem on the smallest chip.

The immediate goal is not the robot. It is one joint that holds its angle, under
an operating system that can be trusted with the next one.
