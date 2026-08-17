# Appendix G — Timeline from the Commit History

138 commits across three days. The repository was initialised on 2026-08-14 —
the same day the roadmap flagged its absence as an outstanding risk, "because the
sibling project The Word Device was lost and recovered only because an editor's
local history happened to retain it".

Commit subjects are quoted verbatim. They are unusually informative: several
state a *retraction* rather than an addition.

---

## G.1 Day one — 2026-08-14: M0 through M5, in one day

```
Initial commit — cyd-os through Milestone 1
WIP: M2 task switching (not working) + PDF report pipeline
Disable watchdogs; M2 narrowed to boot-context restore
M2 still failing: timer interrupt stops firing with all-fabricated tasks
M2 localized: fabricated-frame entry works, saved-frame resume does not
M2 works: preemptive task switching, 1200+ switches, zero corruption
Docs: UM-CYDOS-009 (M2 verification), two new figures, watchdog corrections
Root cause: PS.EXCM disables the zero-overhead loop; clear it before calling C
```

Eight commits, five of them about one defect. The sequence is the twelve build
cycles of Chapter 8 §8.7 compressed: *not working* → *narrowed* → *still failing*
→ *localized* → *works* → and then, after the milestone had already passed, the
**root cause**.

That ordering is worth noting. M2 was made to work by the `volatile` workaround
before the cause was understood; the cause came afterwards and the workaround was
removed.

```
M3: kernel heap and VM arena model, all exit criteria verified
UM-CYDOS-010 §7: rule out flash-as-framebuffer, and challenge the premise
Map .rodata from flash through the data cache
M4: register-based bytecode VM, assembler, and verified containment
Host the VM in a native task: both preemption mechanisms, composed
M5: multiple applications, isolated, with a shell. Roadmap M0-M5 complete.
Locking: critical sections and a blocking mutex, with a starvation defect found
```

*"challenge the premise"* is the commit that produced Chapter 1 §1.4's argument
that the framebuffer should not exist. The flash-cache work follows immediately,
because it was the prerequisite for M4 (Chapter 4 §4.4).

```
Display: ILI9341 driver, bit-banged SPI, no framebuffer
Display confirmed on hardware; measure the fill and correct a wrong estimate
Confirm display colour order; MADCTL BGR bit verified
WIP: display syscalls with viewport confinement (STALLS - do not flash)
Fix total system freeze: task_yield() must only ever move the tick EARLIER
Merge branch 'wip/display-syscall'
Merge display syscalls onto the freeze fix; restore the graphical demo
```

Three display commits, then a `WIP … do not flash`, then the freeze fix. Chapter
17 §17.9 records that the freeze **predated** the syscall work and had survived
three commits — visible here as the three display commits above it.

## G.2 Day two — 2026-08-15: from kernel to device

```
Correct the previous commit: the shell was never broken
Prove viewport containment numerically, not visually
UM-CYDOS-016: display syscalls, and the freeze they were blamed for
```

The first commit of the day is a **retraction**. The second replaces a judgement
("I think it looks good") with a counter.

```
Touch: XPT2046 driver, and a GPIO bank bug it exposed
Fix build: hoist the touch latch above its user
Touch: PENIRQ detection, and a capture method that was destroying its own data
Touch works: PENIRQ detection confirmed, axes corrected by measurement
Touch calibration complete: both axes verified by measurement
UM-CYDOS-017: touchscreen, and the verification method that kept failing
Touch syscall: applications can read the pointer, confined to their viewport
```

Four attempts, visible as four commits. *"a capture method that was destroying
its own data"* is Chapter 19 §19.4.

```
Hardware SPI2 for the display: 387 ms -> 78 ms full-screen fill
SYS BLIT: applications draw images held in their own arena
IPC: applications exchange messages without sharing memory
SPI2 DMA: 78 ms -> 43 ms full-screen fill
Animate the spectrum strip: crossfade between bars and a scrolling hue sweep
Raycaster: a rainbow dungeon rendered as vertical columns
Framebuffer for the 3D view, switchable - and the measurement says leave it off
Scheduler priorities, with sleep and priority inheritance
```

The framebuffer commit is the one Chapter 18 §18.7 overturns. *"switchable"* is
what made the overturning possible.

```
Close two standing risks: arm the hang detector, pin the panic path to DRAM
Persistence: a record that survives a power cycle
Make the safety net loud: enforce stack guards, keep panics readable, fix UART rx
UM-CYDOS-019: failure handling, and three mechanisms that had never fired
Persist the panic reason, so a fault survives the power cycle that hides it
Panic to the panel, so a standalone board says why it stopped
```

*"fix UART rx"* is buried in the middle of a commit about something else — which
is how Chapter 12 §12.5's finding was made: while building a harness to send
`fault` over the serial line.

```
There was no idle task: nine task_create calls against a TASK_MAX of 8
A touch launcher, and a microSD driver
The touch X axis has been backwards since it was written
Revise 017: the direction test was decided by its own worst sample
```

Four commits, in order: the launcher exposes a missing idle task, the launcher
lands, the launcher exposes the inverted axis, and the report explaining why the
calibration missed it. Chapter 24 §24.1's *"none was found by inspection; all four
were found by building something that depended on them"*.

```
Measure the renderer: it is busy 10% of the time, and the workers were not the problem
Scheduler fairness: age ready tasks so no priority can starve one forever
Instrument the display lock: contention with application drawing costs 3.7x the frame rate
Best-effort application drawing: 3.0 -> 9.9 fps
Shorten the display sleep now that it is the limiter; find the SPI ceiling by eye
Rename lock timing to say what it measures; revise 014 with the contention finding
```

*"Rename lock timing to say what it measures"* — Chapter 11 §11.9's
`blocked` versus `wait`. A commit whose entire content is a naming decision, made
because the wrong name would send the next reader in the wrong direction.

```
Rename the project from cyd-os to nat-os
The tick deadline raced into the future: every yield pushed it one interval
Document the comparator defect in 008
```

The 183 ms stall, found because shortening the display task's sleep removed the
margin that had been concealing it.

```
Icons, and a close button an application cannot reach
Give the screen back to the launcher; the colour artwork becomes something you open
Chrome gets rows no view draws into; sweep the docs the relayout invalidated
Revert the screen relayout; keep the camera fix and add the layout assertions
Stamp the 3D view's close button into its framebuffer instead of over it
Revise 021: the overlay fix, and a correct diagnosis fixed at the wrong scope
```

Six commits containing the whole of Chapter 24 §24.8: the relayout, the revert,
the twenty-times-smaller fix, and the report about scope. Note that the revert
commit **bundles two changes** — "keep the camera fix" — which is exactly what
Chapter 24 §24.9 identifies as having destroyed the information.

```
3D view: one ray per column, and navigation decided per cell instead of per tick
Face shading, and 015 revision 1.4: a framebuffer claim that did not survive
Wall texture: panel seams in world space
Notes app with a multi-tap keypad, replacing the blit icon
Notes: save to flash, an inbox to read them back, and a close button somebody draws
Full screen for the launcher, and the postmortem that made it safe
Merge the full-screen layout and its postmortem
Revise 021 §6.7: the cause was found, and it was not the layout
calib: validate target pairs before fitting, and keep the outcome
docs: record what the calibration fault actually was (017 §7.4, 022 rev 1.1)
```

*"a close button somebody draws"* is Chapter 26 §26.8 — the button that was
present, hit-testable, and drawn by nobody.

*"the cause was found, and it was not the layout"* is the acquittal.

## G.3 Day three — 2026-08-16: peripherals, then the radio

```
intr: route peripheral interrupts, and switch the first consumer back off
adc: SAR ADC1, and the first thing this OS has measured outside itself
i2c: bit-banged two-wire master, and docs for it and the ADC
docs: consistency audit across all 26 reports against the source
```

*"switch the first consumer back off"* is in the commit subject — the interrupt
matrix shipped with its only consumer disabled, and said so.

```
term: the shell on the panel, replacing the counter icon
audio: tones on the DAC, and a speaker that has not been found
docs: record the shell's scrollback, and correct the record on where it landed
audio: LEDC tones on gpio26, and a click on every keypress
```

Two audio commits: the DAC attempt that produced silence, then LEDC. Chapter 23's
three stacked faults sit between them.

```
display: overlap pixel conversion with DMA, and find out the timings lie
task: account CPU time per task, and time work with it instead of wall-clock
display: a single DMA timeout was silently halving throughput for the whole run
display: isolate the DMA timeout to the 3D view, and clear today's changes of it
display: the DMA timeout was measuring preemption, not a stalled engine
Revert today's display changes: the DMA timeouts were real, not preemption
docs: record the DMA stall evidence, and correct the previous commit
```

Seven commits, and the last two contradict the two before them. This is Chapter
18 §18.9 as it happened: a theory, a fix, a revert of the fix, and a report
correcting the revert's commit message. The open status is the honest outcome.

```
window: implement the register-window vectors; windowed-ABI code now runs
window: link and call a real -mabi=windowed object from the call0 kernel
window: run Espressif ROM code -- crc32_le returns the textbook CRC-32
shell: list the three windowed-ABI commands in help
```

The ladder of Chapter 2 §2.7: hand-written windowed assembly, a vendor-compiled
object, then real ROM code with a known answer.

```
vendor/phy: measure what Espressif's radio blob needs, and link it
vendor/phy: first attempt to link the blob into the kernel, and why it failed
vendor/phy: Espressif's radio blob runs inside nat-os
phy: the radio initialises -- register_chipv7_phy returns 0
wifi: the OSI table -- all 116 entries, generated, linking against libpp.a
wifi: the full MAC stack links -- zero unresolved symbols
wifi: implement the OSI table bodies over the call0 kernel
wifi: bring up the MAC and identify the TSF timer
```

*"and why it failed"* is a commit that exists to record a failure. *"all 116
entries … containing nothing whatsoever"* — Chapter 27 §27.2's "it linked
beautifully and would have collapsed the instant anything called it".

```
kernel: fix task_sleep, which was not sleeping
kernel: restore flat-out polling in the display and touch tasks
wifi: read the factory MAC address, verified against its own CRC
wifi: route the MAC interrupt onto a CPU line
wifi: receive chain from open-mac's source; channel tuning still faults
wifi: nat-os receives 802.11 frames
wifi: recycle rx descriptors so reception is continuous
```

*"restore flat-out polling"* is the commit Chapter 27 §27.10 calls a
"restoration" that was not — it substituted `task_yield()` for `task_sleep(1)`
and broke touch responsiveness, which was only diagnosable once `task_sleep` had
been fixed the commit before.

```
wifi: transmit, and beacon a discoverable SSID
wifi: prove transmit does NOT reach the air, and build the test that shows it
wifi: rule out transmit power; the MAC hardware init is what is missing
wifi: link libpp and run the MAC hardware init chain
wifi: drop the rx task back to NORMAL; it was starving touch
```

*"prove transmit does NOT reach the air, and build the test that shows it"* —
the probe-request experiment of Chapter 27 §27.8, committed as a retraction of
the commit two above it.

```
touch: real 10 ms poll at HIGH priority, replacing a yield-per-sample loop
touch: make polling policy runtime-tunable; revert to the last known-good config
display: runtime-selectable panel clock, and touch polling policy
desktop: stop the app chrome painting over the 3D view
touch: make the responsive polling default
docs: UM-NATOS-028, the WiFi/touch/3D-view session
```

*"make polling policy runtime-tunable"* is the commit that turned four
hypotheses into one flash, and the method note in Chapter 27 §27.13 says it
"should have happened three rounds earlier".

```
desktop: narrow the chrome guard to a compile-time geometry assertion
calibration: give it the panel, and add raycast_open()
shell: add fbsum, and record what the 3D startup fault is NOT
shell: add the arena/framebuffer overlap check, and close out the evidence
shell: add dfreeze, and prove the fault is downstream of the framebuffer
docs: render UM-NATOS-028 to PDF, and let the builder use an installed Chrome
docs: UM-NATOS-028 rev 1.1 -- the frozen-renderer result, and rebuild the PDF
```

The last five working commits are all *instruments*, not fixes: `fbsum`, an
overlap check, `dfreeze`. The project ends by building measurement rather than by
guessing, which is the arc of the whole book.

*"record what the 3D startup fault is NOT"* — a commit whose deliverable is an
enumeration of eliminated theories.

---

## G.4 What the commit messages show

**Retractions are commits.** Nine commit subjects announce a correction:

```
Correct the previous commit: the shell was never broken
Revise 017: the direction test was decided by its own worst sample
Revert the screen relayout; keep the camera fix and add the layout assertions
Revise 021: the overlay fix, and a correct diagnosis fixed at the wrong scope
Revise 021 §6.7: the cause was found, and it was not the layout
docs: correct the record on where it landed
Revert today's display changes: the DMA timeouts were real, not preemption
docs: record the DMA stall evidence, and correct the previous commit
wifi: prove transmit does NOT reach the air, and build the test that shows it
```

**Failures are commits.** *"and why it failed"*, *"still failing"*, *"a speaker
that has not been found"*, *"channel tuning still faults"*, *"switch the first
consumer back off"*.

**Documentation is a commit, and often a separate one.** Fourteen commits are
purely `docs:` or `Revise NNN`, including a *"consistency audit across all 26
reports against the source"* — a commit that checked the documentation against
the code rather than the reverse.

**Measurements are the deliverable.** *"measure what the radio blob needs"*,
*"find out the timings lie"*, *"prove the fault is downstream of the
framebuffer"*, *"record what the 3D startup fault is NOT"*.

## G.5 By the numbers

| | |
|---|---|
| Commits | 138 |
| Days | 3 (2026-08-14 to 2026-08-16) |
| Commits that announce a retraction | 9 |
| Commits that are documentation only | 14 |
| Commits about one defect (M2) | 5 |
| Commits about one defect (DMA stall) | 7 |
| Reverts | 3 |
| Milestones reached on day one | M0 through M5 |
