# Chapter 26 — The Note Pad and the Shell on the Panel

> Sources: `docs/UM-NATOS-022-notes.md`, `docs/UM-NATOS-026-onscreen-shell.md`
> Code: `kernel/notes.c`, `kernel/notes.h`, `kernel/messages.c`, `kernel/term.c`, `kernel/term.h`, `kernel/shell.c`, `kernel/uart.c`

---

# Part A — The Note Pad

## 26.1 The first thing that takes text and keeps it

> The first thing in this project that takes **text** from a person and keeps it.
> Everything before it read a tap or a serial line, and a serial line needs a
> computer attached — which is the thing the launcher exists to avoid needing.
>
> A message is written on a multi-tap keypad, saved to flash, and read back after
> a power cycle. **That last part is the whole claim.**

## 26.2 Why it is native code, and why that is not a win

```c
 * Every other application is VM bytecode confined to a 26-row strip
 * (UM-NATOS-016 §2). A keyboard does not fit in 26 rows, and the VM has no
 * syscall that returns a keypress. This owns the launcher's region instead, the
 * same way the 3D view does, and is native code for the same reason the
 * launcher is: it is part of the interface rather than something running inside
 * it.
 *
 * That is a real limitation and not a design win. It means text entry is a
 * kernel feature rather than a service applications can ask for. Giving the VM
 * a keyboard syscall is the version that would let any program read text, and
 * it is not built.
```

The report is blunter:

> **text entry is a kernel feature rather than a service applications can
> request.** Any program wanting text input cannot have it.
>
> That is recorded here and in `notes.h` because it is the kind of limitation
> that becomes invisible once the feature works: the note pad takes text, so text
> entry appears solved, and **it is solved for exactly one program.**

Recording a limitation *at the point where a reader would assume it was
addressed* is a habit worth naming. Chapter 30 collects every instance.

## 26.3 The keypad is a workaround, and saying so kept it on the record

### The layout that failed

> The first version was QWERTY: three letter rows, keys **24 × 26**.
>
> It was close to unusable. Tapping `e` produced `w` — one key to the left,
> consistently, for every key. Reported as *"quite a task to write anything
> legible"*, which is an accurate description of a keyboard whose letters are
> reliably wrong.

### What was actually broken, and the correction to that diagnosis

The original diagnosis was that the touch mapping read "systematically about
24 px low on X". That was wrong, and the report corrects itself in place:

> **What that paragraph got wrong.** The cause is right; the characterisation is
> not. "Reads systematically about 24 px low on X" describes a constant offset,
> and the fault was a **magnification** — the narrow range is a divisor, so the
> error is proportional to distance from centre and changes sign across the
> screen (−29 px at the left edge, +12 px at the right).
>
> The number 24 was real. It was measured in one place, on keys near the middle,
> and then written down as though it were a property of the panel rather than of
> where it happened to be measured. **A single sample of a position-dependent
> quantity looks exactly like a constant.**
>
> It also explains something this document treated as unremarkable: **no
> adjustment of the constants ever helped, because there is no offset that
> corrects a scale error. That should have been the clue.**

"No adjustment ever helped" is a diagnostic signature in its own right: a fix
that moves the fault around rather than removing it is evidence about the *shape*
of the fault.

### Why 80 px keys work anyway

> Twelve keys, 3 × 4, each **80 × 26** — a third of the panel wide. An 80 px key
> absorbs a 24 px error. A 24 px key is destroyed by it.
>
> The launcher's icon cells are also 80 px and were never affected, which is why
> this fault survived unnoticed until something needed fine positioning.

### The paragraph that kept a fault on the record

This is the most valuable thing in the report:

> The keypad is tolerant of the defect, not a correction of it. Those are
> different, and calling the first one a fix is how a known fault becomes
> folklore. Anything finer than 80 px will hit it again.

> That paragraph is the reason this section survived. **Because the keypad was
> never called a fix, the fault stayed on the record as unresolved, and
> calibrating it properly stayed on the list instead of quietly becoming the way
> things are. A workaround that is honestly labelled is a debt; one that is
> called a solution is a defect with good manners.**

The fault *was* subsequently corrected (Chapter 19 §19.10), and the keypad
stayed — now for a different reason, which the report also insists on recording:

> **Since correction**, 80 px keys are no longer load-bearing. They are kept
> because multi-tap is the point of this app, not because a smaller key cannot be
> hit — and that difference is now testable rather than assumed.
>
> It is now chosen — the same keypad, held for a different reason, **which is
> worth writing down because a decision whose original justification has expired
> is one nobody re-examines.**

## 26.4 Multi-tap details that matter in use

Four, each closing a specific usability failure:

> - **The live key is drawn back-lit.** Without it, multi-tap is guesswork about
>   whether a press registered — and the press that "did not register" is usually
>   one that did, replacing the letter you wanted.
> - **An 800 ms timeout commits.** This is how two letters from one key are typed.
> - **Cycling wraps** rather than sticking at the end of a sequence.
> - **One press, one action**, latched on the first sample. The last sample
>   before release is the one a resistive panel gets wrong, and typing the
>   *wrong* letter is worse than missing a press.

The first of those is what Chapter 23 exists to reinforce: the audio click was
built specifically because *"the press which did not register is usually one that
did"*.

`notes.h` restates the latching rule at the interface:

```c
/* Feeds a touch sample. Acts on the FIRST sample of a press, for the reason in
 * UM-NATOS-021 §4.2: the last sample before release is the one a resistive
 * panel gets wrong, and a keyboard that types the wrong letter is worse than
 * one that misses a press. */
void notes_touch(uint32_t x, uint32_t y, int down);
```

## 26.5 Storage

### A separate sector, deliberately

> Messages live in the flash sector **after** the kernel's boot record, not in
> it.
>
> A flash write is erase-then-write over a whole 4 KB sector. Sharing one would
> mean rewriting the boot counter on every save, and losing every message if a
> save were interrupted while the counter was the thing being rewritten.
>
> **Separate sectors can only damage themselves.**

### Eight messages of 160 characters

> 160 is the SMS limit. It suits what this is imitating, and it bounds the store:
> eight of them plus a header is under 1.4 KB, comfortably inside one sector.
>
> **When full, the oldest is dropped.** A note pad that refuses to save is worse
> than one that forgets what you wrote first.

Validation follows the boot record's rule — magic, version, count and checksum,
and on any failure the store resets to empty, "so a first run and a corrupt sector
behave identically rather than producing a half-believed store".

The buffer size is chosen against the screen rather than against memory:

```c
/* Longest note. Deliberately small: this lives in .bss, and the panel can show
 * about seven lines of forty characters at once, so a buffer much larger than
 * the screen would be text the user cannot see or reach. */
#define NOTES_MAX 256u
```

## 26.6 Reading, and states you can get out of

> The **header bar is the navigation control**. Tapping it swaps `WRITE` and
> `INBOX`, the way a phone with three buttons did it, and it costs no key.
>
> In the inbox, the left half of the text area pages back and the right half
> forward, with a position indicator so paging has a place rather than being an
> endless cycle.
>
> **Letter keys do nothing while reading.** A stray tap must not silently begin
> composing over a message being read. `del`, `space` and `save` still act, so
> **there is always a way out of a state.**

## 26.7 Verification, and the reflash that matters

```
write a message on the panel
save                        -> header shows "saved", box clears
power cycle
reflash
boot                        -> messages : loaded 1 saved
open notes, tap header      -> message reads back in the inbox
```

> The reflash matters: it proves the message is in flash rather than in RAM that
> happened to survive a warm reset.

A warm reset does not necessarily clear DRAM, so a power cycle alone is a weaker
test than it looks. Reflashing rewrites the image and cannot leave application
state behind.

And a small correctness property:

> The box clears **only on success**. A save that failed must not look like one
> that worked by leaving an empty box behind.

## 26.8 The close button nobody drew

> Worth its own section because it is a fix's blast radius landing on a view that
> did not exist when the fix was written.

Chapter 24 §24.7 stopped the 3D view's close button flickering by stamping it
into the raycaster's framebuffer, and made `desktop_chrome()` skip drawing it
whenever the framebuffer is on. Correct for the only full-region view that then
existed.

> The note pad has no framebuffer to stamp into. It fell through both paths:
> **present, hit-testable, and drawn by nobody.** The button worked — tapping the
> corner returned to the launcher — and was invisible.

Ownership is now explicit for all three cases:

| view | who draws the close button |
|---|---|
| 3D, framebuffer on | the raycaster stamps it into the buffer |
| 3D, framebuffer off | `desktop_chrome()`, and it flickers |
| note pad | the app, in its own header |

> The note pad draws it at exactly the coordinates `desktop_chrome_touch()`
> already tests, so the drawing and the hit test sit in one file and cannot
> drift. That was an open gap in UM-NATOS-021 §9 and is now **half closed** —
> half, because nothing *enforces* the agreement, it is merely adjacent.

Half-closing a gap and saying which half is the honest form.

---

# Part B — The Shell on the Panel

## 26.9 One command set, not two

> The shell has always been a front end that happened to read a serial port. This
> gives it a second front end and changes nothing behind it.
>
> The consequence is larger than the feature: **the device no longer needs a
> computer attached to be inspected.** `mem`, `ps`, `stacks`, `adc`, `i2c`,
> `intr`, `calshow` and `kill` were reachable only from a host over USB.

### The shortcut not taken

> The easy version is an on-screen menu of the popular commands — a grid of
> buttons labelled `mem`, `ps`, `adc`, each calling the function directly. It
> needs no keyboard, no line buffer and no output capture, and it would have
> taken an hour.
>
> It would also have created **a second command set**. Every command added to the
> real shell would be absent from the panel until someone remembered; every
> command whose output changed would have a stale twin. **The two would diverge
> in the direction of whichever was used more, and nothing would report the
> drift.**

So instead:

> a line typed on the glass goes through `shell_run_line()` into the same
> `execute()` a serial line reaches — same parsing, same splitting, same output,
> same unknown-command message. There is no on-panel command table, because
> there is no on-panel command set. **`help` on the glass lists what `help` lists,
> because it *is* `help`.**

This is the same "delete the second mechanism" principle as Chapter 8 §8.2 (one
way for a task to exist), Chapter 8 §8.3 (one switching mechanism), and
Chapter 16 §16.3 (one `retire()` path).

### The one concession, and why it is also a safety check

```c
/* Runs one command line from somewhere other than the UART.
 *
 * execute() splits its argument in place, so a caller's string cannot be passed
 * straight through — it would be modified, and a string literal would fault.
 * The copy is also the length check: a line longer than the shell's own buffer
 * is refused rather than truncated into a different command.
 *
 * This is the whole interface the on-screen shell needs. It deliberately does
 * not bypass execute(): a command typed on the panel takes exactly the same
 * path, with the same parsing and the same output, as one typed over serial. */
void shell_run_line(const char *line)
{
    static char buf[LINE_MAX];
    uint32_t i = 0;

    while (line[i] && i < LINE_MAX - 1u) {
        buf[i] = line[i];
        i++;
    }
    if (line[i]) {
        uart_puts("   line too long\n");
        return;
    }
    buf[i] = 0;
    execute(buf);
}
```

> a truncated command is a different command, and `kill 12` truncated to `kill 1`
> kills something.

## 26.10 Output capture by teeing

> The shell writes to the UART, because that is where a shell writes. Rather than
> teach it a second destination, `uart_putc()` grew an optional tee and the app
> installs itself as one.

```c
/* Optional second destination for everything printed.
 *
 * Exists so the on-screen shell can show a command's output on the panel. The
 * shell writes to the UART because that is where a shell writes; teeing here
 * means it does not need to know it is being watched, and no output path grows
 * a second case.
 *
 * Installed only around a single command's execution, never left on: the
 * reporter task prints several lines a second, and a tee left installed would
 * fill the screen buffer with telemetry the user did not ask for. */
static uart_tee_fn g_tee;

void uart_set_tee(uart_tee_fn fn) { g_tee = fn; }

void uart_putc(char c)
{
    if (g_tee) {
        g_tee(c);
    }
    /* ... */
}
```

> **The tee is installed for exactly the duration of one command and removed
> immediately.** Left on, it would capture the reporter task's telemetry —
> several lines a second — and the pane would fill with output nobody asked for
> within about two seconds. `execute()` holds the console lock for its whole run,
> so while the tee is set the only writer is the command itself.

The console lock (Chapter 11 §11.8) turns out to do double duty: it was added for
interleaving, and it is what makes the tee's exclusivity hold.

Three constraints on the capture function, all consequences of where it runs:

> The capture function runs inside `uart_putc()`. It must not print, must not
> lock, and must not be slow, since it is called once per character of output.

## 26.11 Layout, and three assertions

```c
#define HDR_H      22u
#define KEY_ROWS   4u
#define KEY_COLS   3u
#define KEY_H      26u
#define KEY_W      (DISP_W / KEY_COLS)              /* 80 */
#define KB_Y       (DESK_H - KEY_ROWS * KEY_H)      /* 224 - 104 = 120 */

#define INPUT_H    11u
#define INPUT_Y    (KB_Y - INPUT_H)                 /* 109 */
#define OUT_Y      HDR_H
#define OUT_H      (INPUT_Y - OUT_Y)                /* 87 */

#define CHAR_W     6u
#define LINE_H     9u
#define TERM_COLS  39u                              /* 240/6, less a margin */
#define TERM_ROWS  (OUT_H / LINE_H)                 /* 9 */

_Static_assert(KB_Y + KEY_ROWS * KEY_H == DESK_H,
               "keyboard must end exactly at the region boundary");
_Static_assert(OUT_Y + OUT_H == INPUT_Y, "output pane must meet the input line");
_Static_assert(TERM_ROWS >= 6u, "an output pane under six lines is not worth having");
```

| band | rows | |
|---|---|---|
| header | 22 | title and the close button |
| output | 87 | 9 lines at 9 px |
| input | 11 | prompt, line, block cursor |
| keyboard | 104 | 12 keys at 80 × 26 |

> The keyboard is the fixed cost — four rows of 26 px is 104 rows, nearly half
> the region — and everything else fits in what is left.

The third assertion is unusual and worth noting: it encodes a *quality*
requirement, not a geometric one. "An output pane under six lines is not worth
having" refuses a build that would compile and produce a cramped, useless
terminal.

> That is the same discipline as UM-NATOS-021 §7, arrived at the same way — by
> having previously broken a layout and not noticed.

### Not the note pad's look, deliberately

```c
/* A terminal, deliberately not the note pad's LCD. Two full-region native apps
 * that look alike are two apps a user has to read the header to tell apart. */
#define TRM_BG   0x0000u        /* black                    */
#define TRM_FG   0x07E0u        /* phosphor green           */
#define TRM_DIM  0x03E0u        /* half-bright, key faces   */
#define TRM_KEY  0x2124u        /* key body                 */
```

## 26.12 The output pane is a ring of lines, not a byte stream

> The output pane is a **ring of fixed-width lines**, not a byte stream. A stream
> would need re-wrapping on every draw, and the wrap is already decided at
> capture time by where the newlines fall.

### Scrollback

> Nine visible lines was the wrong size for the two commands most worth running.
> `help` produces about thirty lines and `adc` about fifteen, so the pane showed
> the end of a list whose beginning was the part being asked for.
>
> The ring now holds **48 lines** — a little over three screens of `help` — while
> still showing nine. The bound is deliberate rather than generous: **this is a
> fixed allocation in a kernel with no paging, so scrollback that grew with
> output would be an unbounded allocation driven by whatever the user typed.**
> 48 lines costs under 2 KB of `.bss`.

### Scrolling by tap, in half-screens

> **Tapping the output pane scrolls it**: upper half back, lower half forward,
> half a screen per tap. A line per tap would be nine taps to move one screen,
> which on a panel this slow is not a control but a chore; half-screens also
> leave two lines of overlap so the reader need not remember what the last line
> was.

> A **scrollbar appears on the right only when there is something to scroll**. It
> is the affordance as much as the indicator — nothing else on screen says the
> pane can be scrolled, and **a gesture with no visible cue is a feature only its
> author knows about.**

### Addressed backwards from the write cursor

> Lines are addressed by **distance back from the write cursor**, never forward
> from an origin. A ring has no origin once it has wrapped, and counting forward
> from one means recomputing where it moved to on every draw.

And the arithmetic was tested rather than reasoned:

> The arithmetic was checked by simulating 70 lines through the 48-line ring and
> comparing the window against a plain list at all 40 scroll positions.

That is the closest thing in this project to a unit test — an exhaustive check of
a pure function against a reference implementation. Chapter 31 argues there should
be more of them.

### It does not follow while you read

> Submitting a command snaps to the newest; scrolling stays put while reading.
> Output only ever arrives because a command was submitted, so a pane that jumped
> mid-read would be worse than one that does not follow.

## 26.13 The keypad is a second copy, and that is recorded

```c
 * This is a SECOND copy of the note pad's cycling logic. That is duplication
 * and is recorded as such rather than pretended away: factoring it out means
 * changing a working app to serve a new one, which is a worse trade tonight
 * than two copies with a comment. If a third consumer appears, factor it then.
 *
 * The bottom row differs from the note pad's. There is no `save`; the third key
 * is `run`, because a shell's terminating key submits.
```

The report's version adds the reason the trade is asymmetric:

> Factoring it into a shared widget means modifying a working application to
> serve a new one, and **the note pad is the app holding the user's saved
> messages.** Two copies with a comment was judged the better trade against
> changing working code late at night; **if a third consumer appears, that is
> when it should be extracted**, because at three the duplication stops being a
> shortcut and becomes a pattern.

A stated threshold for when to pay a debt is more useful than an intention to pay
it.

And the key size, held for a new reason:

> The keys are 80 × 26, matching the note pad. There, that size was forced. That
> error has since been corrected, so here the size is **a choice rather than a
> workaround**, and the reason is different: shell commands are short lowercase
> words, and multi-tap types those adequately.

## 26.14 Verification

```
double-tap the shell icon
type a command on the keypad, press run
-> the command echoes after a "> " prompt
-> its output appears in the pane
-> the same output appears on the serial port, unchanged
close returns to the launcher
```

> The serial half of that matters: it confirms the tee **copies** output rather
> than diverting it, so attaching a host does not change what the panel shows.

A one-line check that distinguishes a tee from a redirect — the two would look
identical from the panel alone.

## 26.15 Metrics

### Note pad

| Quantity | Value |
|---|---|
| Keys | 12, 80 × 26 |
| Key size that failed | 24 × 26 |
| Touch X error absorbed | ~24 px near centre, up to 29 px at an edge |
| Calibration status | corrected, Chapter 19 §19.10 |
| Multi-tap commit timeout | 800 ms |
| Messages stored | 8 × 160 characters |
| Store size | ~1.4 KB in one 4 KB sector |
| Compose buffer | 256 B in `.bss` |
| Flash sectors used | 1, separate from the boot record |

### On-screen shell

| Quantity | Value |
|---|---|
| Output pane | 9 lines × 39 columns visible |
| Scrollback ring | 48 lines, under 2 KB of `.bss` |
| Input line | 40 characters, refused beyond |
| Keys | 12, 80 × 26 |
| Multi-tap commit | 80 ticks (~800 ms) |
| **Commands reachable without a host** | **all 25** |
| **Command sets** | **1** |
| Copies of the keypad in the tree | 2 |

## 26.16 What these do not establish

**Note pad.** The calibration is corrected but still linear, run once on one
unit; nothing measures panel non-linearity between target positions, so a UI
element much finer than the keys "has not been shown to work — only stopped being
impossible". The full store has never been exercised, so the oldest-dropped path
has never run outside reasoning. Corrupt-sector rejection is reasoned, not
tested. A save interrupted by power loss is untested. No editing beyond `del`. No
timestamps, "because the kernel has no clock that survives a power cycle — only a
tick counter that restarts at zero". **Text entry is not available to
applications.**

**On-screen shell.** 48 lines is still a bound and what leaves is gone. Scrolling
costs presses — nine taps to reach the top of a full ring. No history, no editing
beyond `del`, no recall. Arguments are typeable but tedious. Uppercase is
impossible and nothing enforces that no command needs it. The tee is single-slot
and not reentrant. And one that is honest about the difference between "never
happens" and "cannot happen":

> **Output during a command from ANOTHER task is captured too**, if that task
> prints without taking the console lock. Nothing in the tree does, so this has
> never happened — **which is not the same as being prevented.**

---

**Next:** the largest single piece of work in the project, and the one that
required the kernel's calling convention to be bridged rather than chosen.
