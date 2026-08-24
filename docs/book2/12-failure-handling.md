# Chapter 12 — Failure Handling: Three Mechanisms That Had Never Fired

> Sources: `docs/UM-NATOS-019-failure-handling.md`
> Code: `kernel/panic.c`, `kernel/watchdog.c`, `kernel/task.c`, `kernel/uart.c`, `kernel/store.c`, `kernel/display.c`, `kernel/shell.c`

---

## 12.1 The premise

The kernel had three mechanisms for surviving its own failure: stack guards, a
panic handler, and a hang detector. Two of the three had never been observed
doing anything, and the third had been silently broken by the second.

> The unifying failure is not technical. Each of these was verified by observing
> that it *existed* — a guard word was written, a banner was printed, a watchdog
> register was armed — rather than by observing it *work*.

## 12.2 Recovery and evidence are in conflict

The hang detector feeds on the scheduler switching between distinct tasks. That
is the right signal for a hang. It is also exactly what a **deliberately halted**
kernel looks like, and `panic.c` ended with:

```c
for (;;) {
    /* Deliberately spin rather than reset: a reset loop would scroll the
     * evidence off the terminal. */
}
```

> Two correct decisions, taken days apart, that combine into a wrong one. The
> comment states the panic handler's entire purpose, and arming the watchdog
> defeated it without touching that file.

### Measured, not assumed

The obvious move is to disarm the watchdog in the panic path. The less obvious
one is to first confirm there is anything to fix — the interaction depends on
whether the timer interrupt still fires with `PS.EXCM` set, which is a question
about silicon rather than about this code.

With the watchdog left armed and `fault` invoked:

```
*** KERNEL PANIC ***
  exccause : 0  (IllegalInstruction)
  epc      : 0x40083fd0   <- faulting instruction

  halted. reset the board to continue.

rst:0x7 (TG0WDT_SYS_RESET),boot:0x13 (SPI_FAST_FLASH_BOOT)
  [task 0 entered]
```

The report is produced and then destroyed about three seconds later. With
`watchdog_disarm()` in the panic path, output stops at `halted.` and stays there
through a fourteen-second capture.

And the reason the measurement mattered more than the fix:

> Had the tick continued to fire during a panic, the fix would have been wrong
> *and* would have hidden a worse problem — a scheduler switching away from a
> faulted context and resuming normal operation on it. The result rules that
> out: no reporter output appears after the panic banner in either
> configuration, so scheduling genuinely stops.

### The resulting rule

> The distinction is whether the kernel can say **why** it stopped:
>
> - it cannot explain a hang, so recovery is the only useful response
> - it can explain a fault or a broken guard, and a reset would destroy the
>   explanation

`watchdog_disarm()` exists solely to express that, and the header says it is not
a general-purpose control.

## 12.3 The watchdog

Three watchdogs matter at boot: one in the RTC controller and one in each timer
group. The bootloader arms the RTC watchdog and expects the application to take
ownership.

```c
 * nat-os did neither for three milestones. The consequence was RTCWDT_RTC_RESET
 * roughly every second once M2 kept the CPU busy, which presented as a "stuck
 * scheduler" and cost three build cycles chasing bugs that did not exist. The
 * reset reason said so on the first line of every boot.
```

That last sentence is the whole indictment. The diagnostic was printed, on every
boot, on the first line, and was not read.

### Disabling

Each configuration register is write-protected behind a key:

```c
/* Write-protect key, shared by all three watchdog blocks. */
#define WDT_WKEY               0x50D83AA1u

static void disable_one(unsigned int wprotect, unsigned int config0)
{
    REG(wprotect) = WDT_WKEY;   /* unlock */
    REG(config0)  = 0;          /* clear enable and every stage action */
    REG(wprotect) = 0;          /* re-lock */
}

void watchdog_disable_all(void)
{
    disable_one(RTC_CNTL_WDTWPROTECT, RTC_CNTL_WDTCONFIG0);
    disable_one(TIMG0_WDTWPROTECT,    TIMG0_WDTCONFIG0);
    disable_one(TIMG1_WDTWPROTECT,    TIMG1_WDTCONFIG0);
}
```

Re-locking afterwards "prevents a stray store from silently re-enabling one".

`kmain` does this first, before anything can take long enough to trip it, and
reads the result back:

```c
    /* Before anything can take long enough to trip it. The bootloader arms the
     * RTC watchdog and expects the application to take ownership. */
    watchdog_disable_all();
    uart_puts("  rtc wdt      : ");
    uart_puts(watchdog_rtc_config() == 0u ? "disarmed\n" : "STILL ARMED\n");
```

A read-back rather than an assumption. Chapters 22 and 23 show three cases where
a read-back was *not* sufficient, but it is still strictly better than nothing.

> **Since written.** The replacement second-stage bootloader (Ch. 3 §3.1.1)
> disables the RTC watchdog itself — `rtc_wdt_disable()` is the first thing
> `boot_main()` does, with the same `0x50D83AA1` key and the same re-lock. So on
> the default build the kernel now disarms a watchdog that is already disarmed,
> and reads back `disarmed` for a different reason than it used to.
>
> That is the correct arrangement and it is worth saying why, because "the
> bootloader already did it" is the argument that would remove this code. The
> kernel does not know which loader started it: one image boots from ours or
> from Espressif's, and only one of those two arms the thing. A guard that is
> redundant under one configuration and load-bearing under the other is not
> redundant.

### Re-arming as a hang detector

TIMG0 is used, and the choice is justified in units:

```c
/* TIMG0 watchdog, used as the hang detector. Chosen over the RTC watchdog
 * because its timeout is expressed in APB-clock ticks through a prescaler,
 * which is a number this kernel already knows, rather than in RTC slow-clock
 * cycles whose frequency is only approximately known. */
```

```c
void watchdog_arm(unsigned int ms)
{
    REG(TIMG0_WDTWPROTECT) = WDT_WKEY;

    /* APB is 80 MHz; a prescaler of 40,000 gives a 2 kHz tick, so the timeout
     * is milliseconds times two. Chosen so the timeout fits comfortably in the
     * 32-bit stage register at any duration worth using. */
    REG(TIMG0_WDTCONFIG1) = 40000u << 16;
    REG(TIMG0_WDTCONFIG2) = ms * 2u;

    REG(TIMG0_WDTCONFIG0) = WDT_EN | WDT_STG0_RESET_SYSTEM |
                            WDT_SYS_RESET_LEN | WDT_CPU_RESET_LEN;

    REG(TIMG0_WDTFEED)     = 1u;
    REG(TIMG0_WDTWPROTECT) = 0;
}
```

### The liveness signal, and its limitation

```c
/* Called every tick from the scheduler. `switched` is non-zero if this tick
 * resumed a DIFFERENT task than the one it interrupted.
 *
 * The window is deliberately much shorter than the watchdog timeout: a healthy
 * system switches many times a second, so requiring one switch per window is a
 * low bar that only a genuine monopoly fails. */
#define LIVENESS_WINDOW_TICKS 100u      /* ~1 s */

void watchdog_liveness(int switched)
{
    static unsigned int ticks;
    static unsigned int seen;

    if (switched) {
        seen++;
    }

    if (++ticks >= LIVENESS_WINDOW_TICKS) {
        ticks = 0;
        if (seen) {
            seen = 0;
            watchdog_feed();
        } else {
            /* No distinct switch for a whole window. Deliberately does NOT
             * feed: the watchdog is the only thing that can recover this, and
             * feeding on the way past would defeat the entire mechanism. */
            g_starved++;
        }
    }
}
```

Fed by the scheduler at the decision point:

```c
    /* Liveness for the hang detector: a tick that resumes the SAME task is not
     * evidence the system is healthy — it is exactly what a monopoly looks
     * like. Only a switch between distinct tasks counts. */
    watchdog_liveness(next != g_current);
```

The limitation is exactly what Chapter 9 §9.6 ran into:

> **The hang detector asks whether ANY distinct switch happened, not whether
> every ready task got a turn.**

A task starved to complete silence produced no crash, no watchdog reset, and no
symptom other than the absence of its own output. Ageing was the answer, not a
better hang detector — the detector is correct for the question it asks.

Arming is deliberately last in `kmain`:

```c
    /* Armed last, immediately before the scheduler takes over. Arming earlier
     * would have the single-threaded boot path - which never switches tasks and
     * so never feeds - reset the board partway through its own self-tests. */
    watchdog_arm(3000u);
    uart_puts("  hang detector: armed, 3000 ms, fed on distinct task switches\n");
```

## 12.4 Stack guards: reported, not enforced

`task.c` filled every stack with a pattern and planted a guard word at its base
from the beginning. `task_stack_headroom()` was written to walk that pattern.

> It was never called — both `high_water` figures anywhere in the tree were the
> *heap's*.

Three separate weaknesses, each individually reasonable:

1. **Coverage.** `kmain` checked guards for three of eight tasks. The display
   task, which carries the raycaster's call chain, was not among them.
2. **Timing.** The check ran in the reporter, roughly every two seconds. "A guard
   word is broken *after* the damage; a check that runs seconds later reports a
   corruption whose cause is long gone."
3. **Response.** A break printed `BROKEN` beside the telemetry and execution
   continued — "scheduling tasks whose stacks had already written into a
   neighbour's, and printing numbers that were by then meaningless."

### The fix: check every slot, every switch, and panic

```c
    /* A broken guard is fatal, not cosmetic.
     *
     * It used to print "BROKEN" beside the telemetry and carry on, which means
     * continuing to schedule tasks whose stacks have already written into a
     * neighbour's. Every number printed after that point is suspect, including
     * the ones that would be used to diagnose it. Stopping at the switch that
     * noticed keeps the damage bounded and the report honest. */
    int broken = task_stack_broken();
    if (broken >= 0) {
        kernel_panic_msg("stack guard overwritten", (unsigned int)broken);
    }
```

```c
int task_stack_broken(void)
{
    for (int id = 0; id < TASK_MAX; id++) {
        if (g_tasks[id].stack_base && g_tasks[id].stack_base[0] != STACK_GUARD) {
            return id;
        }
    }
    return -1;
}
```

> A word load per slot per tick is not a cost worth weighing against continuing
> to run on corrupted memory.

The prose-versus-code lesson attached to it is small and good:

> Written as "all eight tasks" when `TASK_MAX` was 8. It is now 12, and the
> check is written against `TASK_MAX` rather than a literal, so it followed. The
> prose did not — which is the argument for describing a loop by its bound
> rather than by the bound's value on the day.

### The margins, which were never known

```
   id  name        free B  of 2048
   0   report      1844          4   app-host    1604   <- tightest
   1   worker-a    1796          5   shell       1828
   2   worker-b    1796          6   display     1668
   3   vm-host     1732          7   touch       1716
```

Every task retains at least 78% of its stack; the worst uses 444 B of 2048. The
2 KB figure in `task.h` was a guess and turns out to be a comfortable one.

And the finding that justifies measuring all of them:

> The tightest task is `app-host`, **not** `display` — which is where the
> deepest call chain was assumed to be when deciding which three tasks were
> worth checking. That assumption picked the wrong tasks to watch, and is the
> argument for measuring all of them rather than the interesting-looking ones.

The reporting helper exists for the same reason:

```c
/* The task closest to overflowing. Reported so the margin is a number somebody
 * has seen, rather than an assumption that 2 KB was enough. */
int task_stack_tightest(void)
```

## 12.5 The shell had never worked

*This is the most consequential finding in the report, and it was found by
accident while building a test harness for something else.*

While building a harness to send `fault` over the serial line, `mem\r` echoed
`mem` and did nothing. `mem\r\n` worked.

> That difference is diagnostic. If `\r` triggered execution, then `\r\n` would
> trigger twice — the second on an empty line, which returns immediately. It
> working *only* with both bytes means the terminator was not being consumed at
> all until something followed it.

Confirmed directly:

```
send "mem\r", wait 2.5 s   -> reply BEFORE the extra byte: no
send " ",     wait 2.5 s   -> reply AFTER  the extra byte: YES
```

The receive path ran exactly one byte behind. `uart_getc_nb()` read the RX FIFO
through the APB address at `0x3FF40000`; it now reads the AHB alias at
`0x60000000`.

```c
/* The RX FIFO is read through its AHB alias, not through the APB address used
 * for transmit.
 *
 * Measured, not inherited from documentation: with the APB address, the receive
 * path ran exactly one byte behind. Writing "mem\r" echoed `mem` and did
 * nothing; a single further byte then delivered the `\r` and the command ran.
 * Sending "mem\r\n" appeared to work, which is why this survived — every test
 * of the shell had been run from a sender that emitted both.
 *
 * The consequence was worse than a test artefact. A person at a terminal
 * presses Enter and sees nothing happen, because the Enter itself is the byte
 * left waiting. The shell has never been usable interactively, and nothing
 * caught it: it was verified by reading its banner rather than by using it. */
#define UART_FIFO_AHB_REG      0x60000000u
```

```c
int uart_getc_nb(void)
{
    if (!uart_rx_ready()) {
        return -1;
    }
    return (int)(REG(UART_FIFO_AHB_REG) & 0xFFu);
}
```

### Why it survived

Two reasons, and both are the same mistake:

> - every automated test drove it from a sender emitting **both** CR and LF,
>   which is the one input shape that masks the defect
> - the shell was verified by observing that its **banner printed**, not by
>   observing that a command **ran**

> A banner proves the task was created and the transmit path works. It says
> nothing whatsoever about receive. The same substitution — a startup artefact
> standing in for the behaviour it introduces — is what let the guard checks and
> the panic handler go unexercised for as long as they did.

This is one of the standing rules in Chapter 29, and it is arguably the most
broadly applicable thing this project has produced.

One caveat is recorded in Chapter 30: the AHB alias is validated
*behaviourally*, not from documentation. The measurement establishes that the APB
address lags and the alias does not; it does not establish *why*, and no erratum
is cited.

## 12.6 Triggering failure on purpose

Three shell commands exist solely to make these paths reachable.

| Command | Induces | Expected |
|---|---|---|
| `hang` | masks interrupts, spins | watchdog resets the board |
| `fault` | executes `ill` | panic, halt, evidence retained |
| `smash` | clobbers the running task's guard word | panic at the next switch, naming the task |

```c
void task_smash_guard(void)
{
    if (g_current >= 0 && g_tasks[g_current].stack_base) {
        g_tasks[g_current].stack_base[0] = 0xDEADBEEFu;
    }
}
```

Verified on hardware:

```
smash  -> *** KERNEL PANIC ***
          reason   : stack guard overwritten
          detail   : 5                      (task 5 is the shell)

fault  -> *** KERNEL PANIC ***
          exccause : 0  (IllegalInstruction)
          halted.                           still halted after 14 s

hang   -> rst:0x7 (TG0WDT_SYS_RESET)        recovered, rebooted

mem\r  -> heap free=71792 ...               no trailing byte needed
```

> `smash` correctly identified task 5, which is the shell — the task that invoked
> it. That is a stronger result than a bare panic, because it shows the scheduler
> attributing the break to the right task rather than to whichever it happened to
> be examining.

And the policy decision:

> These are deliberately not compiled out. A destructive command in a
> development shell is a smaller risk than a recovery path nobody can reach to
> test.

The project README puts it more bluntly:

> The last three exist on purpose. A recovery path that has never been observed
> to fire is confidence without evidence.

## 12.7 Recording the reason for the next boot

Everything above still assumes a serial cable. A panic prints and halts, which
helps only somebody attached at the moment it happens — and this board is
normally attached to nothing at all.

The persistent record (Chapter 20) carries it:

```c
uint32_t fault_kind;    /* exception, guard break, or none    */
uint32_t fault_detail;  /* exccause, or the task id           */
uint32_t fault_epc;     /* faulting instruction               */
uint32_t fault_boot;    /* which boot it happened on          */
```

### Recorded before it is reported

Both panic entry points write the record **before** printing anything:

```c
void kernel_panic_msg(const char *why, unsigned int detail)
{
#if FLASH_ENABLE
    /* Written BEFORE anything is printed. A handler that reports first and
     * records second loses the record if it dies while reporting, and that is
     * not far-fetched — the UART is the more complicated of the two paths and
     * the system is already in an unknown state. */
    g_record_rc = store_record_fault(STORE_FAULT_GUARD, detail, 0);
#endif
```

> The reverse ordering would also be self-concealing — a panic that died during
> its own report would leave no record AND no output, which is
> indistinguishable from a fault that never happened.

The store side is written to assume as little as possible:

```c
/* Runs inside a panic, so it assumes as little as possible about the state of
 * the system. It does not read the existing record first: g_rec already holds
 * what was loaded at boot, and re-reading flash here would add a failure mode
 * to a path that exists precisely because something has already gone wrong. */
int store_record_fault(uint32_t kind, uint32_t detail, uint32_t epc)
```

### Not cleared by being read

> A fault stays on the record until a different one replaces it. Clearing it
> after one report would mean that power-cycling past the message destroys it,
> and the user who most needs this feature is exactly the one who reboots twice
> before thinking to attach a cable.
>
> `fault_boot` is what keeps that honest. Without it, a fault from thirty boots
> ago reads identically to one from the last run.

The boot-path reporter:

```c
    if (store_fault_kind() != STORE_FAULT_NONE) {
        uart_puts("  LAST FAULT   : ");
        if (store_fault_kind() == STORE_FAULT_GUARD) {
            uart_puts("stack guard overwritten, task ");
            uart_put_dec(store_fault_detail());
        } else {
            uart_puts("exception, exccause ");
            uart_put_dec(store_fault_detail());
            uart_puts(", epc ");
            uart_put_hex(store_fault_epc());
        }
        uart_puts("  (boot #");
        uart_put_dec(store_fault_boot());
        uart_puts(")\n");
    }
```

### Verified, with three distinct failure modes covered

```
smash        -> recorded : yes
next boot    -> LAST FAULT : stack guard overwritten, task 5  (boot #6)

fault        -> epc 0x40084071
next boot    -> LAST FAULT : exception, exccause 0, epc 0x40084071  (boot #8)
boot again   -> same line, unchanged
```

> the guard break survived a reset; the exception **replaced** it rather than
> being ignored or appended; and the second reboot proves reading does not
> consume the record. The reported `epc` matches what the panic itself printed,
> which is what rules out the record being written from stale or default state.

A pleasing side effect:

> Bumping the record from version 1 to 2 incidentally exercised the version
> check that UM-NATOS-018 §2 describes but had never triggered: the old record
> was rejected and reset cleanly rather than being read with the wrong field
> layout.

## 12.8 Putting it on the panel

§12.7 makes a fault legible on the *next* boot. It does nothing for the person
holding a frozen device now, and "the kernel panicked" and "the renderer stopped"
look identical from across the room.

The panic handler now draws to the display. That is a larger claim than it
sounds, because the display driver is built for a healthy system.

### Three assumptions suspended at once

`display_enter_panic_mode()` turns off, permanently:

| Assumption | Why it cannot hold |
|---|---|
| the draw lock | Blocking on a mutex means `task_block()` and a yield, and the scheduler has stopped. Waiting on a lock whose owner will never run again is a hang — and a hang here costs the report. |
| DMA | The descriptors may describe a transfer already in flight, or a buffer belonging to whatever just died. |
| the unbounded FIFO wait | `spi2_tx()` spins until the controller retires a transfer. Correct while the system is healthy; an infinite loop once it is not. |

The bound only exists in panic mode, so the healthy path costs one comparison:

```c
        /* Bounded only in panic mode. A healthy controller retires a 64-byte
         * transfer in microseconds, so the bound is never reached in normal
         * operation and costs a comparison; in a panic an unretired transfer
         * must not be allowed to consume the one chance to report a fault. */
        uint32_t spins = 0;
        while (GPIO_REG(SPI2_CMD) & SPI_USR_BIT) {
            if (g_panic_mode && ++spins > 4000000u) {
                break;
            }
        }
```

The switch is one-way:

> There is no `display_leave_panic_mode()`, because a driver that can be talked
> back out of panic mode invites somebody to try, and the kernel is on its way
> to a halt in any case.

And `display_ready()` guards the whole thing: a panic before `display_init()` has
completed draws nothing rather than writing to an unconfigured controller.

### Ordered by decreasing reliability

```c
static void halt_forever(void)
{
    watchdog_disarm();

#if FLASH_ENABLE
    /* The record was already written by the caller. Confirm it on the terminal
     * so the two reports can be compared: if the next boot disagrees with what
     * was printed here, the persistence path is what is wrong, not the fault. */
    uart_puts(g_record_rc == 0 ? "  recorded : yes, the next boot will report this\n"
                               : "  recorded : NO — the fault will be lost\n");
#endif

    uart_puts("\n  halted. reset the board to continue.\n");

    uint32_t before = display_bytes_written();
    panic_screen();
    uart_puts("  panel    : ");
    uart_put_dec(display_bytes_written() - before);
    uart_puts(" bytes drawn\n");

    for (;;) {
    }
}
```

1. **write the flash record** — fewest moving parts, and the only one that
   survives the power cycle
2. **print to the UART** — one peripheral, no memory beyond a string literal
   already resident in DRAM
3. **draw the panel** — needs the SPI controller, the flash-mapped font, and a
   peripheral whose state nobody has verified

> Each step can only cost the steps after it. If drawing wedges despite the
> bounds, the record and the serial report have both already happened. The
> reverse order would put the most fragile step first and risk everything on it.

### Measured, because a screen cannot be queried

```c
    /* Report how many bytes the panel actually took.
     *
     * Nobody can query a halted board, and "the screen looks wrong" and "the
     * driver never ran" are indistinguishable by eye. A byte count crossing the
     * wire separates them: roughly 150 KB means a full repaint happened and the
     * question is what was drawn; a number near zero means the panic never
     * reached the panel at all. */
```

```
fault -> panel : 175955 bytes drawn
smash -> panel : 176704 bytes drawn
```

A full repaint is 320 × 240 × 2 = 153,600 bytes; the remainder is the title bar
and the text.

> This converts an unfalsifiable visual impression into a claim that fails
> loudly — the same move as the boot counter in Chapter 20 §20.3.

### The screen itself

```c
static void panic_screen(void)
{
    char buf[12];

    if (!display_ready()) {
        return;
    }
    display_enter_panic_mode();

    display_clear(COLOR_BLUE);
    display_fill_rect(0, 0, 320, 20, COLOR_WHITE);
    display_text(6, 6, "KERNEL PANIC", COLOR_BLUE, COLOR_WHITE, 1);

    display_text(6, 36, g_panic_what, COLOR_WHITE, COLOR_BLUE, 1);

    /* Numbers as hex without labels: at this size a label costs more width
     * than it buys, and the two values are positional in the same order the
     * UART report prints them. */
    display_text(6, 56, "cause", COLOR_YELLOW, COLOR_BLUE, 1);
    hex8(buf, g_panic_a);
    display_text(70, 56, buf, COLOR_WHITE, COLOR_BLUE, 1);
    if (g_panic_has_b) {
        display_text(6, 72, "pc", COLOR_YELLOW, COLOR_BLUE, 1);
        hex8(buf, g_panic_b);
        display_text(70, 72, buf, COLOR_WHITE, COLOR_BLUE, 1);
    }

    display_text(6, 104, "halted - reset the board", COLOR_WHITE, COLOR_BLUE, 1);
    display_text(6, 120, "reason is saved; next boot", COLOR_GREY, COLOR_BLUE, 1);
    display_text(6, 136, "will report it over serial", COLOR_GREY, COLOR_BLUE, 1);
}
```

Even the hex formatter is local, and the reason is a dependency argument:

```c
/* Eight hex digits into a caller-supplied buffer. Local to this file because
 * the panic path must not depend on anything it does not have to: no printf,
 * no heap, no shared scratch buffer that something else might be using. */
static void hex8(char *out, unsigned int v)
```

### The exception decode

```c
/* Xtensa EXCCAUSE values worth naming. The rest print as a bare number rather
 * than carrying a table that would be mostly dead weight. */
static const char *cause_name(unsigned int cause)
{
    switch (cause) {
    case 0:  return "IllegalInstruction";
    case 1:  return "Syscall";
    case 2:  return "InstructionFetchError";
    case 3:  return "LoadStoreError";
    case 4:  return "Level1Interrupt";
    case 5:  return "Alloca";
    case 6:  return "IntegerDivideByZero";
    case 8:  return "Privileged";
    case 9:  return "LoadStoreAlignment";
    case 20: return "InstFetchProhibited";
    case 28: return "LoadProhibited";
    case 29: return "StoreProhibited";
    default: return "unknown";
    }
}
```

`StoreProhibited` (29) is the one Chapter 27 spends a session on.

The handler also reports the PHY stack high-water mark, and is candid about when
that number means anything:

```c
    /* How deep the PHY got before dying. Meaningless unless a PHY call was in
     * flight, but when one was, this is the difference between a stack that
     * ran out and a fault that merely happened to land in a spill. */
    uart_puts("  phystack : ");
    uart_put_dec(phy_stack_used());
```

## 12.9 A hang in the code that prevents hangs

The last defect in this chapter is a two-line comedy with a serious lesson.

Redirecting the driver's internal `mutex_lock(&g_lock)` calls to the new
panic-aware `draw_lock()` was done with a regular expression, which also rewrote
the body of `draw_lock` itself:

```c
static void draw_lock(void)
{
    if (!g_panic_mode) {
        draw_lock();          /* <- was mutex_lock(&g_lock) */
    }
}
```

> Infinite recursion, inside the function whose entire purpose is to avoid a
> hang. The board wedged in `display_init()` before printing a single self-test.

Two findings:

> It was found by bisecting — reverting `display.c` alone with `git checkout` and
> rebuilding — rather than by reading the diff, which is the faster route once a
> symptom is "nothing happens at all".

> It is also the **fourth scripted edit in this project to fail silently**, and
> the reason those edits now carry assertions that stop on a missed match rather
> than writing an unchanged file.

## 12.10 Metrics

| Quantity | Value |
|---|---|
| Tasks with guards checked, before → after | 3 → 8 (now 12, by `TASK_MAX`) |
| Guard check frequency, before → after | ~2 s (reporter) → every context switch |
| Guard check cost | 8 word loads per tick |
| Stack headroom, worst case | 1,604 B free of 2,048 (`app-host`) |
| Stack headroom, best case | 1,844 B free of 2,048 (`report`) |
| Minimum margin across all tasks | 78% |
| UART rx lag, before → after | 1 byte → 0 |
| Sessions the shell was unusable interactively | every one since M5 |
| Failure paths now triggerable on demand | 3 |
| Fault fields added to the persistent record | 4 |
| Record version | 1 → 2 (now 3) |
| Panic paths that record before reporting | 2 of 2 |
| Driver assumptions suspended in panic mode | 3 |
| Bytes drawn by a panic screen | ~176,000 |
| Reporting steps, ordered by reliability | 3 |

## 12.11 What this does not establish

- **No pre-emptive overflow detection.** A guard word is found *after* it has been
  overwritten. A guard page would be a barrier; this hardware has no MMU to
  provide one.
- **No wild-pointer protection.** Guards catch growth past a stack's own base;
  nothing catches a stray write into the middle of a neighbour.
- **The panic screen has no failure path of its own.** If the panel is wedged,
  the byte count says so — but only to somebody with a serial cable, which is the
  audience the screen exists to replace.
- **Panic mode is untested against a genuinely broken controller.** Every test so
  far ran on a display that was working perfectly at the moment of the fault, so
  the bounds have never actually been reached.
- **A fault during the recording itself is not survivable.** The checksum would
  reject the torn record on the next boot, correctly, and the reason would be
  lost.
- **Only two fault kinds are recorded.** A hang recovered by the watchdog writes
  nothing at all, "so the most common recoverable failure is the one the record
  cannot describe".
- **No stack sizing per task.** All twelve slots get 2 KB regardless of measured
  use.
- **The AHB alias is validated behaviourally, not from documentation.**

> **Since written.** The panel path carried a latent geometry fault the whole
> time: `panic_screen()` drew its title bar 320 pixels wide on a 240-wide panel —
> the two dimensions written the wrong way round. It never produced a wrong
> pixel, because `display_fill_rect()` clips width against `DISP_W` and silently
> corrected it on every call.
>
> That is this chapter's own thesis arriving from a new direction. A mechanism
> that fires rarely is not just untested, it is *unexamined*: the panic screen is
> only ever seen after the system has already failed, so the rare look it does
> get is not a careful one, and a driver that quietly fixes your arithmetic
> removes the last chance of noticing. Found by reading, fixed to `DISP_W`, and
> then verified by causing a real panic rather than by inspection —
> UM-NATOS-033 §8.

---

**Part II ends here.** The kernel is complete: it boots, it interrupts, it
switches, it schedules fairly, it allocates and checks its own heap, it has two
locking primitives with measured contention behaviour, and it can survive,
explain and report its own failures three separate ways.

**Part III** is what all of that exists to host.
