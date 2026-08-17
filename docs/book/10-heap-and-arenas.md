# Chapter 10 — Memory: A Heap With One Invariant, and Arenas

> Sources: `docs/UM-NATOS-010-m3-verification.md`, `docs/UM-NATOS-011-flash-cache.md`
> Code: `kernel/heap.c`, `kernel/heap.h`, `kernel/arena.c`, `kernel/arena.h`

---

## 10.1 What M3 delivers, and what it actually settles

Milestone 3 delivers the kernel allocator and the arena model that bytecode
applications run inside. Its exit criteria are narrow and measurable: no leak
across 10,000 allocate/free cycles, arena bounds queryable by the interpreter,
and exhaustion that fails rather than corrupts.

All three pass. But:

> The more consequential outcome is not the pass — it is the number the
> allocator finally puts on the DRAM budget, which settles a question M5 could
> otherwise only have guessed at.

That number is §10.7.

## 10.2 The structure: address-ordered, physically linked

The heap is a doubly-linked list of blocks in address order, covering the region
with no gaps. Each block carries a 16-byte header: both physical neighbours, its
payload size, and a magic word that doubles as the used/free flag.

```c
typedef struct block {
    struct block *prev;     /* physically previous block, NULL at heap start */
    struct block *next;     /* physically next block, NULL at heap end       */
    uint32_t      size;     /* payload bytes, always a multiple of HEAP_ALIGN */
    uint32_t      magic;    /* BLK_FREE or BLK_USED                          */
} block_t;
```

| | Segregated free lists | Address-ordered physical list (selected) |
|---|---|---|
| Allocation | O(1) typical | O(n) first fit |
| Coalescing | Needs a search or boundary tags | O(1) both directions |
| State to verify | Several lists plus size classes | One list, one invariant |
| Overhead | Lower | 16 B per block |

The choice is explicitly not about throughput:

> Throughput is not the constraint. This allocator serves arena creation and a
> small number of kernel structures, not millions of short-lived objects. What
> matters is that the structure can be *checked*, and a single address-ordered
> list tiles the heap exactly, which makes the invariant trivially statable:
> **every block's payload must end precisely where the next block's header
> begins.**

That one sentence is what makes `heap_check()` (§10.5) possible.

The file header restates it:

```c
 * Address-ordered with physical links means coalescing is O(1) in both
 * directions and needs no search. The cost is 16 bytes per allocation, which is
 * the right trade here: arenas are large and few, and the alternative — a
 * segregated free list — buys throughput this kernel does not need in exchange
 * for state that is harder to verify.
```

## 10.3 The magic word earns its four bytes

```c
/* Magic values chosen to be implausible as data, and distinct from each other
 * so a block's state is unambiguous even in a raw memory dump. */
#define BLK_FREE 0xF2EEB10Cu
#define BLK_USED 0x05EDB10Cu
```

Two properties, both deliberate. They are implausible as data — so a header is
either valid or *obviously* not — and they are distinct from each other, so the
free/used flag needs no separate field and a raw dump is readable.

The consequence:

> A double free, an interior pointer, or a wild pointer is therefore **counted
> and refused** rather than linked into the list.

And the justification, which is a memory-protection argument rather than a
defensive-programming one:

> On a kernel with no memory protection this is not defensiveness for its own
> sake. A corrupted free list surfaces as a fault in unrelated code much later,
> and M2 already demonstrated how expensive it is to debug a symptom that
> appears far from its cause.

The check happens inside the critical section, and the comment says why that
matters:

```c
    /* A live allocation is the only thing that may be freed. Anything else —
     * double free, interior pointer, wild pointer — is counted and refused.
     * Checked inside the section: two tasks freeing the same pointer must not
     * both see BLK_USED. */
    if (b->magic != BLK_USED) {
        g_bad_free++;
        crit_exit(crit);
        return;
    }
```

Coalescing poisons the absorbed header for the same reason:

```c
    n->magic = 0;               /* poison, so a stale pointer is not mistaken
                                 * for a live header */
```

## 10.4 Allocation

First fit. A block is split only when the remainder can hold a header plus a
useful payload:

```c
/* Splitting only pays if the remainder can hold a header plus a useful
 * payload. Below this the leftover is left attached to the allocation, which
 * wastes a few bytes but avoids manufacturing unusable slivers. */
#define SPLIT_MIN (HDR_BYTES + HEAP_ALIGN)
```

```c
void *heap_alloc(uint32_t bytes)
{
    if (bytes == 0u) {
        return 0;               /* a zero-size allocation has no useful answer */
    }

    uint32_t want = align_up(bytes);

    /* The free list is walked and rewritten here, and a tick landing mid-split
     * would let another task observe — or allocate from — a half-linked block.
     * A critical section rather than a mutex: these are a handful of memory
     * operations, and a mutex would make the allocator depend on the scheduler
     * that may itself want to allocate. */
    uint32_t crit = crit_enter();

    for (block_t *b = g_first; b; b = b->next) {
        if (b->magic != BLK_FREE || b->size < want) {
            continue;
        }

        /* Split only when the tail can stand on its own. */
        if (b->size - want >= SPLIT_MIN) {
            block_t *tail = (block_t *)((char *)b + HDR_BYTES + want);
            tail->size  = b->size - want - HDR_BYTES;
            tail->magic = BLK_FREE;
            tail->prev  = b;
            tail->next  = b->next;
            if (tail->next) {
                tail->next->prev = tail;
            }
            b->next = tail;
            b->size = want;

            /* The new header consumes payload that used to be allocatable. */
            g_total -= HDR_BYTES;
        }

        b->magic = BLK_USED;
        g_used += b->size;
        if (g_used > g_high_water) {
            g_high_water = g_used;
        }
        crit_exit(crit);
        return payload_of(b);
    }

    g_fail++;
    crit_exit(crit);
    return 0;
}
```

The lock choice is argued in one clause and it is the same argument Chapter 11
makes at length: *a mutex would make the allocator depend on the scheduler that
may itself want to allocate.*

### `heap_total()` moves, and that is honest

Note `g_total -= HDR_BYTES` on split and `g_total += HDR_BYTES` on coalesce.
Splitting consumes payload to create a header; coalescing returns it. So the
*usable* total moves as the heap fragments:

> `heap_total()` tracks that honestly rather than reporting a fixed figure.

That is a small decision with a real consequence for §10.6: the no-leak test
checks that `heap_total()` returns to its baseline, which would be trivially true
if the figure were a constant.

## 10.5 `heap_check()`: ten ways to be wrong

The structural check walks the list and returns a non-zero code identifying the
*first* violated invariant:

```c
int heap_check(void)
{
    uint32_t lo = align_up((uint32_t)&_heap_start);
    uint32_t hi = (uint32_t)&_heap_end & ~(HEAP_ALIGN - 1u);

    if (!g_first) {
        return g_total == 0u ? 0 : 1;
    }
    if ((uint32_t)g_first != lo) {
        return 2;
    }

    uint32_t span = 0, used = 0;
    block_t *prev = 0;

    for (block_t *b = g_first; b; prev = b, b = b->next) {
        if (b->magic != BLK_FREE && b->magic != BLK_USED) {
            return 3;                                  /* corrupt header */
        }
        if (b->prev != prev) {
            return 4;                                  /* broken back link */
        }
        if ((b->size % HEAP_ALIGN) != 0u) {
            return 5;
        }
        /* Blocks must tile the region exactly: the next header has to begin
         * where this block's payload ends. A gap or overlap means the split or
         * coalesce arithmetic is wrong. */
        char *end = (char *)b + HDR_BYTES + b->size;
        if (b->next && (char *)b->next != end) {
            return 6;
        }
        if (!b->next && (uint32_t)end != hi) {
            return 7;
        }
        /* Two adjacent free blocks mean a coalesce was missed, which is how a
         * heap fragments itself to death while reporting plenty free. */
        if (b->magic == BLK_FREE && b->next && b->next->magic == BLK_FREE) {
            return 8;
        }

        span += b->size;
        if (b->magic == BLK_USED) {
            used += b->size;
        }
    }

    if (used != g_used) {
        return 9;
    }
    if (span != g_total) {
        return 10;
    }
    return 0;
}
```

Ten distinct codes: corrupt header, wrong first block, broken back link,
misalignment, gap or overlap between blocks, list ending short of the region,
missed coalesce, and two accounting disagreements between the walk and the
running totals.

Two of those deserve highlighting.

**Code 8 — missed coalesce.** "That is how a heap fragments itself to death
while reporting plenty free." It is the one failure a byte-count leak test cannot
see.

**Codes 9 and 10 — accounting versus the walk.** The running counters are
maintained incrementally; the walk recomputes them. Any disagreement means the
incremental bookkeeping and the structure have diverged, which is exactly the
class of bug that produces a heap that looks fine until it does not.

The check is called after *every phase* of the self-test rather than only at the
end, "so a failure names the operation that caused it".

## 10.6 The three criteria, and what each was designed to catch

### Criterion 1 — no leak

10,000 allocate/free cycles across 8 rotating slots with pseudo-random sizes from
16 to 515 bytes, so blocks are split and coalesced continuously.

The test is deliberately stronger than "free bytes return to baseline":

> the **largest free block** and the **block count** must also return, because a
> heap that has fragmented into unusable slivers still reports the correct
> number of free bytes. That is the failure a naive leak test misses.

```
[1] no leak  : PASS  after 10000 cycles  free=166432 largest=166432
               blocks=1 check=0
```

> Returning to one block is the strong form of the result: it says every one of
> the ~10,000 splits was undone by a matching coalesce, not merely that the byte
> totals happen to balance.

### Criterion 2 — arena bounds

Ten cases, chosen to include the ones a naive check gets wrong: exact fit, last
byte, zero-length access, one past the end, one before the base, one byte too
long, **a length chosen to wrap the address space**, an address belonging to a
different arena, and a bogus id.

```
[2] arenas   : PASS  live=2 committed=5120 B  base=0x3ffb27e0 len=4096
```

### Criterion 3 — exhaustion and refusal

```
[3] oom safe : PASS  oversize=NULL fails=1 bad_frees=2 check=0
arenas freed : check=0 free=166432/166432 rejects=1 high_water=5120 B
```

An oversized request returned `NULL` and incremented the failure counter,
leaving the heap byte-identical and structurally clean. The double free and the
stack-address free were both counted and neither perturbed the list.

### The test runs before the tick

```
The self-test runs single-threaded in `kmain` before the tick is armed. Nothing
in it can be disturbed by a context switch, so an M3 failure cannot be blamed on
M2 — and vice versa.
```

That is a small structural decision with a large debugging payoff: it removes an
entire class of "maybe it's the scheduler" from any failure in this test.

## 10.7 Arenas

An arena is a contiguous block of DRAM with a recorded base and length. Four
slots, statically:

```c
#define ARENA_MAX 4
```

```c
typedef struct {
    uint32_t base;      /* 0 when the slot is unused */
    uint32_t len;
} arena_t;
```

### Not resizable, and why

> An arena's base is the value the interpreter holds and bounds-checks against.
> Allowing it to move would mean every bytecode program must survive its memory
> being relocated underneath it — a much harder property to guarantee than
> telling a program its size once, at start.

### Zeroed at creation

```c
        /* Zero before handing over. An arena is application-visible memory and
         * must not leak the previous occupant's contents. */
        uint32_t *w = (uint32_t *)p;
        uint32_t  n = bytes / 4u;
        for (uint32_t i = 0; i < n; i++) {
            w[i] = 0;
        }
        uint8_t *tail = (uint8_t *)p + (n * 4u);
        for (uint32_t i = 0; i < (bytes % 4u); i++) {
            tail[i] = 0;
        }
```

Word-at-a-time with a byte tail, because `-fno-builtin` plus `-nostdlib` means
there is no `memset` unless `kstring.c` supplies one, and this file predates it.

## 10.8 `arena_contains()`, and the offset domain

This is the most important function in the kernel that is four lines long.

```c
int arena_contains(int id, uint32_t addr, uint32_t len)
{
    if (id < 0 || id >= ARENA_MAX || g_arenas[id].base == 0u) {
        return 0;
    }

    uint32_t base = g_arenas[id].base;
    uint32_t size = g_arenas[id].len;

    /* A zero-length access is inside any live arena provided its start is.
     * Treating it as out of bounds would reject legitimate empty copies. */
    if (addr < base) {
        return 0;
    }

    uint32_t offset = addr - base;
    if (offset > size) {
        return 0;
    }

    /* Compare in the offset domain, where the arena length bounds both terms.
     * Testing `addr + len` directly can wrap past the end of the address space
     * and report success for an access that is wildly out of range — the exact
     * case an attacker-supplied length would aim for. */
    if (len > size - offset) {
        return 0;
    }

    return 1;
}
```

The offset-domain comparison is the whole point:

> The wrap case is why `arena_contains()` works in the offset domain. Testing
> `addr + len` directly overflows and reports success for an access far outside
> the arena — precisely what a hostile length would target. Since this function
> is the only thing standing between a bytecode program and the rest of DRAM, it
> is implemented once and exported rather than reimplemented per caller.

That last clause was honoured for exactly one milestone. The VM duplicates it for
performance (`vm_in_bounds`, Chapter 14), and the duplication is *tested* rather
than trusted — 35 cases cross-checked. The same offset-domain reasoning then
propagates to three more places:

| Where | What wraps if done naively |
|---|---|
| `arena_contains()` | `addr + len` past the address space |
| `vm_in_bounds()` | same, on cached base/size |
| `vp_fill()` (Chapter 17) | `x + w` where x is a "negative" coordinate arriving as `0xFFFFFFF0` |
| `SYS TOUCH` (Chapter 17) | `t.x - vm->vx` underflowing to a huge value |

The header declares the property as part of the contract:

```c
/* The bounds check itself, exported so there is exactly one implementation of
 * it rather than one per caller. Returns non-zero when [addr, addr+len) lies
 * entirely inside the arena. Overflow-safe: an addr+len that wraps is refused,
 * which is the case a naive check gets wrong. */
int arena_contains(int id, uint32_t addr, uint32_t len);
```

## 10.9 The DRAM budget, measured

UM-NATOS-007 §5 named arena sizing as M3's principal risk and asked for
measurements rather than guesses.

Of 180,736 B of DRAM: 10,160 B is kernel data, `.rodata` and task stacks;
4,096 B is the boot stack reservation; **166,432 B is allocatable.**

| Consumer | Bytes | Share of heap |
|---|---|---|
| Full 240×320 16-bit framebuffer | 153,600 | 92.3% |
| Remaining for all arenas | 12,832 | 7.7% |

That table is the argument of Chapter 1 §1.4 and it landed here, at M3, on the
strength of a real allocator rather than an estimate.

The figure has moved three times since, and the reasons are recorded:

| Figure | When | Why |
|---|---|---|
| 166,432 B | M3 | Baseline |
| **167,680 B** | Flash cache | `.rodata` moved out of DRAM — grew by *exactly* the 1,248 B of the section that moved |
| 158,048 B | M5 | `TASK_MAX` 4 → 8, adding 8 KB of static stacks, plus the app table and shell buffers |
| lower, unremeasured | now | `TASK_MAX` is 12 and three more drivers exist |

The status summary is honest about the last row:

> **Measured at M3** — 167,680 B allocatable then, 158,000 B after `TASK_MAX`
> rose to 8; it has since risen to 12 and three drivers have been added, so the
> current figure is lower and unremeasured.

A live figure is available at any time from the shell:

```
mem  ->  heap free=155952 largest=155952 blocks=4 high_water=5120 check=0
```

```c
static void cmd_mem(void)
{
    uart_puts("   heap free=");
    uart_put_dec(heap_free_bytes());
    uart_puts(" largest=");
    uart_put_dec(heap_largest_free());
    uart_puts(" blocks=");
    uart_put_dec(heap_blocks());
    uart_puts(" high_water=");
    uart_put_dec(heap_high_water());
    uart_puts(" check=");
    uart_put_dec((unsigned int)heap_check());
    uart_puts("\n");
}
```

`largest` beside `free` is the fragmentation indicator; `check` beside both is
the structural one. Three numbers that together say something no one of them can.

## 10.10 The heap under concurrency

M3 shipped with an explicit gap:

> **No allocator concurrency.** `heap_alloc`/`heap_free` are task-context only.
> There is no lock, because the kernel has no locking primitive yet. An
> allocation from an interrupt handler would corrupt the list, and nothing
> currently prevents one.

Chapter 11 closed the first half — the critical sections shown in §10.4 are that
fix. The second half is still open: nothing prevents an allocation from an
interrupt handler, and a critical section taken *inside* a handler is a no-op
because interrupts are already masked.

The design responds by keeping allocation off the hot paths entirely. `kmain`
creates the VM's arena before the scheduler starts:

```c
    /* The VM's arena is created here, on the boot path, because the heap has no
     * locking and this is the last moment at which exactly one context exists
     * (UM-NATOS-010 §8). The task only ever runs an already-initialised VM. */
    g_vm_arena = arena_create(1024);
```

and the framebuffer is allocated on the boot path too, after the self-tests, with
its own ordering note:

```c
    /* After the self-tests, not before: heap_init() runs inside m3_selftest(),
     * and the leak test there checks free memory returns to its baseline — an
     * 80 KB allocation made earlier would fail for want of a heap and, once the
     * ordering was fixed, would break the baseline instead. */
    if (raycast_set_framebuffer(1) != 0) {
        uart_puts("  raycast fb   : allocation failed, using direct columns\n");
    }
```

Application arenas are the one exception: `app_start()` allocates from task
context. That is safe under the current critical-section protection and is
recorded rather than hidden.

## 10.11 Metrics

| Quantity | Value |
|---|---|
| Image size at M3 | 6,720 B |
| Heap, usable at M3 | 166,432 B |
| Header overhead | 16 B per block |
| Boot stack reserved | 4,096 B |
| Alloc/free cycles tested | 10,000 |
| Blocks after test | 1 (baseline) |
| High-water mark | 5,120 B |
| Allocation failures | 1 (the deliberate one) |
| Refused frees | 2 (both deliberate) |
| `heap_check()` failures | 0 |
| Arena bounds cases | 10 at M3, 35 cross-checked at M4 |

## 10.12 What M3 does not establish

- **No allocator concurrency** beyond critical sections — §10.10.
- **No arena enforcement.** M3 provides `arena_contains()`; nothing *calls* it
  yet. "Until then arenas are bookkeeping, not protection." Closed in Chapter 14.
- **No fragmentation characterisation under realistic load.** The test's size
  distribution is uniform and its lifetimes are uniform. Real workloads are
  neither, and a first-fit allocator's worst case is workload-shaped.
- **No allocation latency measurement.** First fit is O(n) in block count.
- **Arena count is fixed** at `ARENA_MAX = 4`, statically.

---

**Next:** the two locking primitives, and why the cost of contention turned out
to be a count rather than a duration.
