# Chapter 16 — Applications: Lifecycle, Three Levels of Scheduling, Messaging

> Sources: `docs/UM-NATOS-013-m5-verification.md`
> Code: `kernel/app.c`, `kernel/app.h`, `kernel/ipc.c`, `kernel/ipc.h`, `kernel/shell.c`, `kernel/kmain.c`

---

## 16.1 Three levels of scheduling, and only the newest knows

The system now preempts at three independent levels:

| Level | Mechanism | Granularity | Introduced |
|---|---|---|---|
| 1 | Timer interrupt preempts native tasks | Any machine instruction | M2 |
| 2 | `vm_run()` quantum returns control | Bytecode instruction boundary | M4 |
| 3 | `app_tick()` round-robins that quantum across applications | One quantum per application | M5 |

The design point:

> **only the newest one knows the others exist.**
>
> Adding level 3 required **no change to the scheduler and no change to the
> interpreter**. `app_tick()` is an ordinary function calling `vm_run()` in a
> loop, hosted by an ordinary native task. Levels 1 and 2 remain unaware that
> applications exist.
>
> That is the payoff for the boundary drawn in M4: because `vm_t` carries all
> execution state and `vm_run()` is resumable, multiplexing applications is
> bookkeeping rather than surgery.

The whole of level 3:

```c
void app_tick(uint32_t quantum)
{
    for (int id = 0; id < APP_MAX; id++) {
        app_t *a = &g_apps[id];
        if (a->state != APP_RUNNING) {
            continue;
        }

        int r = vm_run(&a->vm, quantum);
        if (r == VM_RUN_QUANTUM) {
            continue;               /* still going; next one gets a turn */
        }
        /* ... halt or fault reporting, then retire ... */
    }
}
```

and the task that hosts it:

```c
/* Hosts every application. One native task drives the third scheduling level;
 * the applications inside it are preempted by their quantum, and this task is
 * itself preempted by the timer. */
static void task_apps(void)
{
    for (;;) {
        app_tick(2000);
        if (app_live_count() == 0) {
            task_yield();       /* nothing to run — do not spin at full tilt */
        }
    }
}
```

Eight lines for a whole scheduling level.

## 16.2 The application record

```c
#define APP_MAX 4

typedef enum {
    APP_FREE = 0,
    APP_RUNNING,
    APP_HALTED,      /* ran to completion                       */
    APP_FAULTED,     /* violated a rule and was terminated       */
    APP_KILLED       /* stopped from the shell                   */
} app_state_t;

typedef struct {
    app_state_t state;
    const char *name;
    int         arena;
    uint32_t    base;
    uint32_t    bytes;
    uint32_t    publish_off;
    vm_t        vm;
} app_t;
```

Three terminal states rather than one, so `ps` can distinguish a program that
finished from one that was killed from one that broke a rule. Chapter 24 §24.9
notes the consequence: a faulted program keeps its slot and shows no close
button, because it is not running.

## 16.3 Lifecycle: one release path, not three

`app_start()` allocates an arena, copies the image in, and initialises a VM:

```c
int app_start(const char *name, const uint8_t *img, uint32_t len,
              uint32_t arena_bytes, uint32_t publish_off)
{
    if (len > arena_bytes) {
        return -1;                  /* image would not fit its own arena */
    }

    for (int id = 0; id < APP_MAX; id++) {
        app_t *a = &g_apps[id];
        if (a->state == APP_RUNNING) {
            continue;
        }

        int arena = arena_create(arena_bytes);
        if (arena < 0) {
            return -1;
        }

        uint32_t base = 0;
        if (arena_bounds(arena, &base, 0) != 0) {
            arena_destroy(arena);
            return -1;
        }

        uint8_t *dst = (uint8_t *)base;
        for (uint32_t i = 0; i < len; i++) {
            dst[i] = img[i];
        }

        if (vm_init(&a->vm, arena) != 0) {
            arena_destroy(arena);
            return -1;
        }

        /* One horizontal strip per slot. Assigned by the kernel and never by
         * the application: a program can ask how large its canvas is, but the
         * only coordinates it can express are inside it. Same property as its
         * arena, applied to pixels. */
        vm_set_viewport(&a->vm, 0u, APP_VIEW_Y0 + (uint32_t)id * APP_VIEW_PITCH,
                        APP_VIEW_W, APP_VIEW_H);
        vm_set_app_id(&a->vm, id);

        /* A fresh application must not inherit mail addressed to whoever held
         * this slot before it. */
        ipc_clear(id);

        a->state       = APP_RUNNING;
        /* ... */
        return id;
    }

    return -1;
}
```

Every failure path releases the arena it had just created. That is easy to get
wrong, and the *termination* path is where it was made structural:

```c
/* Releases the arena and records why the application stopped. Kept in one place
 * because "terminating an application releases its arena completely" is an exit
 * criterion, and three separate paths reach it — halt, fault, and kill. Three
 * copies of a release is three chances to leak one. */
static void retire(app_t *a, app_state_t why)
{
    if (a->arena >= 0) {
        arena_destroy(a->arena);
        a->arena = -1;
    }

    /* Wipe the strip on the way out, so a dead application does not leave its
     * last frame on the panel looking like a live one. */
    display_fill_rect(a->vm.vx, a->vm.vy, a->vm.vw, a->vm.vh, COLOR_BLACK);

    /* Undelivered mail dies with the recipient. */
    for (int i = 0; i < APP_MAX; i++) {
        if (&g_apps[i] == a) {
            ipc_clear(i);
        }
    }
    a->base  = 0;
    a->state = why;
}
```

Three concerns in one function, each with its own reason: the arena, the pixels,
and the mail. "A dead application does not leave its last frame on the panel
looking like a live one" is the same class of decision as the advancing marker
block in Chapter 18 — making a stopped thing *look* stopped.

### Diagnose before releasing

```c
        if (r == VM_RUN_FAULTED) {
            /* Diagnose before releasing: the arena is about to go back to the
             * heap, and the offending offset is only meaningful alongside the
             * size it exceeded. */
            console_lock();
            uart_puts("\n  [app ");
            uart_put_dec((unsigned int)id);
            uart_puts(" '");
            uart_puts(a->name);
            uart_puts("' TERMINATED] ");
            uart_puts(vm_fault_name(vm_fault(&a->vm)));
            uart_puts(" at offset ");
            uart_put_dec(a->vm.fault_detail);
            uart_puts(" of ");
            uart_put_dec(a->bytes);
            uart_puts(" B arena, pc=");
            uart_put_dec(a->vm.fault_pc);
            uart_puts(", after ");
            uart_put_dec(a->vm.executed);
            uart_puts(" instructions\n");
            console_unlock();
            retire(a, APP_FAULTED);
        }
```

Producing, for the rogue:

```
  [app 2 'rogue' TERMINATED] out of bounds at offset 256 of 256 B arena,
                             pc=28, after 167 instructions
```

*"offset 256 of 256 B arena"* is the whole result in one phrase — the offending
offset next to the size it exceeded, which is why the diagnostic is printed
before the arena goes back to the heap.

## 16.4 The shell, and launching by name

The shell is a native task polling UART0, and holds no privilege the rest of the
kernel lacks:

> it is a front end to `app_start()` and `app_kill()`.
>
> Programs are registered by the caller rather than referenced directly, so the
> shell has no dependency on which images exist.

```c
static const shell_program_t PROGRAMS[] = {
    { "counter", vm_app_a,     VM_APP_A_LEN,     512u, VM_APP_A_AT_COUNTER  },
    { "squares", vm_app_b,     VM_APP_B_LEN,     512u, VM_APP_B_AT_SQUARE   },
    { "rogue",   vm_app_rogue, VM_APP_ROGUE_LEN, 256u, VM_APP_ROGUE_AT_COUNTER },
    { "draw",    vm_app_draw,  VM_APP_DRAW_LEN,  512u, VM_APP_DRAW_AT_NAME },
    { "gfxrogue", vm_app_gfx_rogue, VM_APP_GFX_ROGUE_LEN, 256u, 0u },
    { "paint",   vm_app_paint, VM_APP_PAINT_LEN, 512u, 0u },
    { "blit",    vm_app_blit,  VM_APP_BLIT_LEN,  512u, 0u },
    { "ping",    vm_app_ping,  VM_APP_PING_LEN,  512u, 0u },
    { "pong",    vm_app_pong,  VM_APP_PONG_LEN,  512u, 0u },
};
```

Note the arena sizes: 512 bytes for most, and **256 for both rogues**. A smaller
arena makes the escape test faster and the boundary figure easier to read.

### Never by index

```c
/* Start a registered program by NAME.
 *
 * The table is indexed by position everywhere else and that is exactly how the
 * paint application failed to launch: PROGRAMS grew from three entries to six,
 * a hard-coded [4] silently became gfxrogue instead of paint, and the symptom
 * was a screen flickering red and white with no obvious connection to the
 * indexing. A name cannot drift when the table is reordered. */
static int start_program(const char *name)
{
    for (int i = 0; i < PROGRAM_COUNT; i++) {
        if (str_same(PROGRAMS[i].name, name)) {
            return app_start(PROGRAMS[i].name, PROGRAMS[i].img, PROGRAMS[i].len,
                             PROGRAMS[i].arena_bytes, PROGRAMS[i].publish_off);
        }
    }
    uart_puts("  [boot] no such program: ");
    uart_puts(name);
    uart_puts("\n");
    return -1;
}
```

The symptom is worth dwelling on because it is a perfect example of a defect
whose *appearance* has no relationship to its *cause*: a strip flickering red and
white, caused by an array index. UM-NATOS-017 §8.4 records that the user's
description — *"touching in the red/white thing was making black dots"* —
identified both the wrong application and a second bug, and was "a faster route
to the cause than the counters, which correctly reported `g/w = 0/0` without
indicating why".

Boot order also matters and is documented:

```c
    /* Order matters: ping addresses application 1, so pong must take that
     * slot. Slots are handed out lowest-free-first. */
    start_program("ping");
    start_program("pong");
```

That is a genuine coupling and it is recorded rather than hidden. It is the one
place in the system where application identity is hard-coded into a program.

### `shell_poll()` never blocks

> UART receive was added for it — polled rather than interrupt-driven, because
> the shell is the only consumer and a console that drops a keystroke under load
> is a better outcome than a second interrupt source competing with the
> scheduler tick.
>
> `shell_poll()` never blocks, so a user holding a key cannot starve the system.

```c
static void task_shell(void)
{
    shell_begin();
    for (;;) {
        shell_poll();
        task_yield();
    }
}
```

## 16.5 The M5 results

```
[1] interleave: PASS  a insns=60000 count=19999  |  b insns=60000 square=224970001

  [app 2 'rogue' TERMINATED] out of bounds at offset 256 of 256 B arena,
                             pc=28, after 167 instructions

[2] isolation : PASS  rogue faulted at offset 256 = arena size;
                      neighbours still running and advancing
[3] release   : PASS  heap 158048/158048 B, live=0, check=0

tasks: report=0 a=1 b=2 vm=3 apps=4 shell=5
```

The self-test runs before the scheduler starts, for the same reason M3's did:

```c
/*
 * Runs single-threaded before the scheduler starts, so the results are
 * deterministic and a failure cannot be blamed on task switching. The live,
 * interactive version of the same thing runs afterwards under the shell.
 *
 * Each block is one exit criterion from UM-NATOS-007 §7.
 */
```

### Criterion 1 — interleaving, checked by arithmetic

Both applications received an identical instruction budget: **60,000 each**.
`app_tick()` hands out the same quantum per round, and equal totals confirm
neither starved nor over-ran.

Their *progress* differs, which is the more informative result:

```
A: 19,999 iterations × 3 instructions + 3 preamble = 60,000
B: 14,999 iterations × 4 instructions + 3 preamble = 59,999   (sampled mid-iteration)
   14,999² = 224,970,001 — the published square, exactly
```

> B does more work per iteration and therefore advances more slowly in its own
> terms while consuming the same CPU. Two applications sharing a core should not
> advance in lockstep, and the differing rates make that observable rather than
> assumed. That B's published value is exactly 14,999² also confirms its
> arithmetic survived every preemption intact.

The pass condition is written to require *all* of that:

```c
    /* Both must have run, and both must have run the SAME amount: app_tick
     * hands out an identical quantum to each. Equal instruction counts with
     * unequal published values is exactly right — B does more work per
     * iteration, so it advances more slowly in its own terms. */
    int c1 = (app_state(a) == APP_RUNNING) && (app_state(b) == APP_RUNNING) &&
             (ia == ib) && (ia > 0u) && (pa > 0u) && (pb > 0u) && (pa != pb);
```

`pa != pb` is the clause that makes this a test rather than a formality: two
applications reporting the *same* progress would mean the quantum was not being
shared, or that one program was not running at all.

### Criterion 2 — the rogue is terminated, alone

> Every store inside the arena succeeded. The first store past the end faulted,
> **at offset 256 of a 256-byte arena** — the boundary is exactly where it should
> be, not approximately. This is a test of *where* the wall stands, not merely
> that one exists.

The neighbours were unaffected: "both still `running`, and both with published
values strictly greater than before the rogue was introduced, so they were still
making progress rather than merely still alive."

And the deeper point, quoted in Chapter 1 and worth having in place:

> A VM address is an offset into its own arena, so there is no value it could
> load that names another application's memory. Reaching a neighbour is not
> refused — it is **unrepresentable**. The bounds check exists for the weaker
> case of a program walking off its own end, which is precisely what was
> observed.

Relaunching `rogue` from the shell reused slot 2 and terminated it again
identically, "confirming the lifecycle is repeatable and not a one-shot".

### Criterion 3 — the arena comes back exactly

> After killing both applications, free heap returned to **158,048 B — exactly**
> its pre-test value, with zero live applications and a clean structural check.
> An exact match rules out a partial release; an approximate one would have
> indicated a leaked header or a missed coalesce.

### The shell, exercised

| Command | Result |
|---|---|
| `help` | command list |
| `progs` | three programs with image and arena sizes |
| `ps` | table with live counters and the faulted rogue's diagnosis |
| `run rogue` | `started id=2`, followed by the termination message |
| `kill 0` | `killed 0, arena released` |
| `mem` | `free=155952 largest=155952 blocks=4 high_water=5120 check=0` |
| `bogus` | rejected, not silently ignored |

```
 id  name      state     arena   insns      published
 0   counter   running   512 B   3812000    1270666
 1   squares   running   512 B   3811635    1795557008
 2   rogue     faulted   256 B   167        0   [out of bounds @256]
```

That table is the security model made legible: three programs, three arena
sizes, three independent instruction counts, and a fault that names both the
offending offset and the boundary it crossed.

## 16.6 Regression

Six native tasks running concurrently — reporter, two M2 workers, the M4 VM
host, the application host, and the shell. Workers' guards intact and
`corrupt=0` throughout; stack headroom 483 words. M2, M3 and M4 self-tests all
still passing.

The heap number moved, and the report explains it rather than hiding it:

> Heap fell from 167,680 B because `TASK_MAX` rose from 4 to 8, adding 8 KB of
> statically allocated task stacks, plus the application table and shell
> buffers. That is a deliberate trade: six concurrent tasks needed the slots.

## 16.7 Messaging

M5 originally shipped with no way for applications to communicate, and called
that "simple but not ultimately useful". Messaging was added afterwards without
weakening anything.

```
ipc s/d/r = 278/277/157957      badbuf = 0
```

> 278 sent and 277 delivered — every accepted message reached its recipient, one
> in flight when sampled. No application ever offered a buffer outside its own
> arena.

### Copied, never shared

`ipc.h`'s header is the design statement:

```c
 * Applications have no shared memory and will not be given any. An arena is the
 * unit of isolation (UM-NATOS-013 §5.2), and mapping one arena into another
 * would dissolve the single property everything above it depends on.
 *
 * So messages are COPIED, twice: out of the sender's arena into a kernel
 * mailbox, and later out of that mailbox into the receiver's arena. The cost is
 * two copies of a small buffer; what it buys is that neither application ever
 * holds a reference to the other's memory, and neither can observe the other's
 * layout.
 *
 * A sender names a DESTINATION APPLICATION, never an address. There is no
 * argument to this interface that could denote memory belonging to somebody
 * else, which is the same shape as the arena, the viewport and the pointer: the
 * unwanted operation is not refused, it is unexpressible.
```

Four resources now share that property: memory, pixels, input, and messaging.
Chapter 17 §17.7 tabulates them.

### Refusal over queueing

```c
 * One mailbox per application, holding one message. A second message to a full
 * mailbox is refused and counted rather than queued: a queue needs a policy for
 * what to drop when it fills, and there is no consumer yet whose requirements
 * would decide that policy. Refusing is the honest placeholder.
```

The 157,957 refusals are the *expected* result of `ping`'s design — it writes in
a tight loop while `pong` drains occasionally:

> Queueing would need a policy for what to drop when the queue fills, and no
> consumer exists yet whose requirements would settle that policy. Refusing is
> the honest placeholder, and the counter makes the back pressure visible rather
> than hiding it in a buffer.

### The implementation

```c
int ipc_send(int from, int dst, const uint8_t *data, uint32_t len)
{
    if (dst < 0 || dst >= APP_MAX || len == 0u || len > IPC_MSG_MAX) {
        g_refused++;
        return -1;
    }

    /* Delivering to a slot that is not running would leave a message for
     * whoever occupies it next, which is a channel between two applications
     * that never agreed to talk. */
    if (app_state(dst) != APP_RUNNING) {
        g_refused++;
        return -1;
    }

    /* The mailbox is shared between the sending and receiving applications,
     * which run in the same task but at different times, and with the reporter
     * reading the counters. Short enough for a critical section. */
    uint32_t crit = crit_enter();

    if (g_box[dst].len != 0u) {
        g_refused++;                /* occupied; sender must retry */
        crit_exit(crit);
        return -1;
    }

    for (uint32_t i = 0; i < len; i++) {
        g_box[dst].data[i] = data[i];
    }
    g_box[dst].len  = len;
    g_box[dst].from = from;
    g_sent++;

    crit_exit(crit);
    return 0;
}
```

Receive copies at most `max` bytes and reports the true length:

```c
/* Takes the pending message for `id`, if any. Returns its length and writes the
 * sender id through `from`; returns 0 when the mailbox is empty. Copies at most
 * `max` bytes and reports the true length, so a receiver offering too small a
 * buffer loses the tail rather than overrunning. */
uint32_t ipc_recv(int id, uint8_t *out, uint32_t max, int *from);
```

### Mail that must not outlive its context

Two cases, both of which would otherwise create a channel nobody asked for:

> - **Sending to a slot that is not running is refused.** Otherwise the message
>   waits for whoever occupies that slot next — two applications connected
>   without either having agreed to it.
> - **Retiring an application clears its mailbox**, so a successor in the same
>   slot cannot read its predecessor's mail.

And a third, on the other side: `app_start()` clears the mailbox too, so a fresh
application cannot inherit mail addressed to its predecessor even if the
predecessor was never retired cleanly.

### Where the buffer is checked

The bounds check for a message body is in `vm.c`, not `ipc.c`, and the reason is
a good general principle:

> **`SEND` and `RECV` check the buffer in `vm.c`, not in `ipc.c`.** Only the VM
> knows which arena an offset belongs to; the messaging layer receives a kernel
> pointer and has no way to tell where it came from. Putting the check where the
> information is, rather than where the operation happens, is the whole reason
> that split exists.

## 16.8 Metrics

| Quantity | Value |
|---|---|
| Native tasks | 6 (of 8 slots) at M5; 9 of 12 now |
| Applications | 4 slots |
| Scheduling levels | 3 |
| Instruction budget per app, criterion 1 | 60,000 each |
| Accounting drift | 0 for A, 1 for B (mid-iteration sample) |
| Rogue fault offset | 256, of a 256 B arena |
| Heap after release | 158,048 B, exactly baseline |
| Image size at M5 | 14,464 B |
| Messages sent / delivered / refused | 278 / 277 / 157,957 |
| Bad buffers offered | 0 |
| Message size limit | 64 B |
| Registered programs | 9 |

## 16.9 What M5 does not establish

- **No memory protection between native tasks.** Only *applications* are
  isolated.
- **No per-application CPU accounting or priority.** Every application gets the
  same quantum, and there is no way to say one matters more.
- **Messaging is one slot deep and unidirectional per send.** No broadcast, no
  queue, and no way to *wait* for a message — a receiver polls.
- **No program loading from storage.** Images are compiled into the kernel.
  There is no filesystem, so "install an application" has no meaning yet.
- **The shell has no history, editing beyond backspace, or completion.**
- **Fault recovery is termination only.** There is no way for an application to
  handle its own fault, and no restart policy.
- ~~Console output interleaves.~~ Closed by Chapter 11.

---

**Next:** the syscalls — twelve at the time, fourteen now — and the four resources an application cannot
observe outside its own allocation.
