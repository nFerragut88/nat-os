# The MAC layer: measured, not guessed

State after the OSI table skeleton. Nothing here is flashed; this is all
build-time analysis, and the kernel is untouched.

## The contract

`libpp.a` — Espressif's MAC hardware layer, the part open-mac still needs for
initialisation — reaches its host through **one symbol**: `g_osi_funcs_p`,
pointing at a struct of **116 function pointers**.

`vendor/windowed/wifi_osi.c` now provides all 116, correctly typed, in
Espressif's declaration order. **The bodies are stubs.** The table has the right
shape, which is what makes it link; it has no behaviour yet.

Order is load-bearing: the blob indexes the struct by layout, not by name, so a
member out of position is a call to the wrong function with the wrong
arguments, and nothing would diagnose it. The file is generated from Espressif's
header for exactly that reason.

## What linking libpp.a now reports

Unresolved symbols went **43 -> 35**, and the remainder is understood:

| group | count | what it means |
|---|---|---|
| soft-float helpers | 10 | the trick already used for libphy: rename with `objcopy`, answer via ROM function pointers |
| `memcpy` / `memset` / `memcmp` / `sprintf` | 4 | renamed but not yet written; trivial |
| globals (`g_ic`, `g_wifi_global_lock`, ...) | 5 | the blob expects the host to define them |
| mesh | 5 | no mesh on this build; stubs |
| net80211 callbacks | 10 | **open-mac supplies these** — they are the 802.11 layer it replaces |
| `abort` | 1 | stub |

None of these is an unknown. The largest single unknown in the whole WiFi path
was "what does the MAC blob demand of an OS", and it is answered: 116 entries,
of which 34 are stubs, 50 are thin mappings onto things nat-os already has, and
26 are real subsystems.

## The 26 that are real work

nat-os has tasks, mutexes, a heap, a tick and an interrupt matrix. It does not
have:

- **queues** (7 entries) — blocking send/recv with timeouts
- **semaphores** (5) — counting, with timeout
- **event groups** (5) — wait-for-bits
- **software timers** (6) — one-shot and periodic, armed in ms and us
- a PRNG (`_rand`, `_random`, `_get_random`)

All are buildable on `task_block()` / `task_wake()` / `timer_ticks()`, which
exist. None needs new kernel concepts.

## Honest state

**A table of stubs links. It does not work.** Calling into `libpp` with these
bodies would fail the moment it tried to create a queue and got 0 back. The
skeleton's value is that the shape is now fixed and verified, so the bodies can
be written one at a time against a frame that is known to be correct.

Estimated remaining to a MAC that initialises: the 26 real entries, the 25
mechanical ones above, then open-mac's `init_mac()` — which is itself a single
register write:

```c
#define MAC_CTRL_REG _MMIO_DWORD(0x3ff73cb8)
static void init_mac() { MAC_CTRL_REG = MAC_CTRL_REG & 0xffffe800; }
```

That last line is the whole reason this is worth finishing: the MAC bring-up is
trivial once the environment underneath it exists.
