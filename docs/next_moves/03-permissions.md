# 03 — Per-application device permissions

**Size:** small. **Risk:** low. **Blocked on:** nothing.

The cheapest real capability left, because the hard part was built by accident.

---

## Why it is cheap now

`device_t` grew a `caller` argument during the `store` work — persistent slots
had to be banked per application, and `device_t` could not express that. So
**every device call already carries who is asking**:

```c
int device_read (uint32_t caller, uint32_t id, uint32_t chan, uint32_t *out);
int device_write(uint32_t caller, uint32_t id, uint32_t chan, uint32_t value);
```

The caller comes from `vm->app_id` — the kernel's own record — and **never from
a register**, because a program naming its own identity is the same mistake as
trusting an offset it supplied.

A permission check is therefore one bitmap indexed by caller, consulted in two
functions.

## What to build

```c
/* kernel/device.c */
static uint32_t g_perms[APP_MAX + 1];   /* bit N = may touch device N */

static int permitted(uint32_t caller, uint32_t id)
{
    if (caller > DEVICE_CALLER_KERNEL) return 0;   /* unknown caller: nothing */
    if (caller == DEVICE_CALLER_KERNEL) return 1;  /* kernel and shell: all */
    return (g_perms[caller] >> id) & 1u;
}
```

Called at the top of `device_read`, `device_write`, `device_xfer_out`,
`device_xfer_in`. A refusal is **not a fault** — same contract as a bad channel.
A program asking for something it was not granted gets 0 and keeps running.

Grant them in `app_start()`, from a field in the `PROGRAMS` table in
`kernel/kmain.c` so it is visible next to the arena size.

## Shell

```
perms                    list each application and what it may touch
perms <app> <dev> on|off grant or revoke
```

## The honest limit — say it out loud

**This is not security.** A permission grant is only meaningful if the image it
applies to cannot be swapped for another, and nat-os has no image identity: no
signature, no content hash, nothing. Anyone who can flash the board can put any
bytes in the `PROGRAMS` table.

What it *is*: **containment**. It stops a buggy or careless program from
reaching hardware it was never meant to, and it makes the intended capability
surface explicit and reviewable. That is worth having on its own.

Do not describe it as security in any report until image identity exists.

## Why do it before the language

The NatScript proposal has a `permissions { }` block (§7 of the revision). If
the enforcement point exists first, the manifest is a compile-time projection of
something real. If the syntax comes first, the manifest is decoration and stays
decoration.

## Where the code is

- `kernel/device.c` / `device.h` — `device_read`, `device_write`, the table
- `kernel/vm.c` — `VM_SYS_DEVICE`, where `caller` is derived from `vm->app_id`
- `kernel/kmain.c` — the `PROGRAMS` table
- `docs/conceptual/natscript-and-natvm.md` §7
