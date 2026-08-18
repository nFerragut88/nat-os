/* nat-os — the device model.
 *
 * UM-NATOS-007 §2.1 called this the one structural item missing from the
 * roadmap, and book chapter 31 restated it after seventeen more reports:
 *
 *     "Every driver above is reachable only from the kernel. The VM has twelve
 *      syscalls, all hardcoded, and no device model -- so an application cannot
 *      read the light sensor, scan the I2C bus or receive a keypress. Each new
 *      peripheral has meant a kernel edit plus a hand-written syscall, which was
 *      tolerable at two and is the obvious next piece of ARCHITECTURE rather
 *      than more drivers."
 *
 * A thirteenth hand-written syscall would have been as ad-hoc as the twelfth.
 * This is the last one: `sys device` reaches a table, and a new peripheral
 * becomes a table entry rather than a kernel edit and an ISA change.
 *
 * ---- what a device may be --------------------------------------------------
 *
 * Deliberately narrow. Every operation is a u32 in and a u32 out on a numbered
 * channel, because that is what every peripheral this board has actually needs
 * -- a light level, a tone, a key, a byte on a bus -- and because a richer
 * interface would need program-supplied buffers at every entry, which is a much
 * larger surface to get right. Bulk transfer can be added when something needs
 * it, through vmarg like everything else.
 *
 * A driver here NEVER touches the vm and never sees an arena. It receives
 * validated scalars and returns success or refusal. All argument checking
 * happens once, in the syscall, through vmarg -- which is why that harness
 * landed first.
 *
 * ---- refusal is not a fault ------------------------------------------------
 *
 * A bad channel, an unsupported direction or a device that is simply not
 * present returns 0 to the application. It is not a bounds violation and the
 * program is not terminated: asking a device something it cannot answer is
 * legal, and a program that cannot enumerate without dying cannot enumerate.
 * Faults remain for what they have always been -- reaching outside the arena.
 */

#ifndef NATOS_DEVICE_H
#define NATOS_DEVICE_H

#include <stdint.h>
#include "app.h"                /* APP_MAX, for DEVICE_CALLER_KERNEL */

#define DEVICE_MAX 8

/* Ceiling on a name handed back to an application, including the terminator.
 * The program says how much room it has and is not believed about it: the
 * syscall bounds its answer by this before anything is copied. */
#define DEVICE_NAME_MAX 16u

/* Operations, as passed in r0 of `sys device`. */
#define DEV_OP_COUNT 0u     /*                        -> r0 = how many          */
#define DEV_OP_NAME  1u     /* r1=id r2=off r3=max    -> r0 = ok                */
#define DEV_OP_READ  2u     /* r1=id r2=chan          -> r0 = ok, r1 = value    */
#define DEV_OP_WRITE 3u     /* r1=id r2=chan r3=value -> r0 = ok                */
#define DEV_OP_INFO  4u     /* r1=id                  -> r0 = ok, r1 = channels,
                             *                           r2 = flags             */

/* Bulk transfer. r1=id r2=chan r3=arena offset r4=length -> r0 = ok.
 *
 * Added because three of the four things an application still cannot do -- the
 * SD card, the network, and I2C transfers -- all want the same thing, and
 * UM-NATOS-031 §4.2 said this should be driven by a device that needs it rather
 * than guessed at. i2c_write() and i2c_read() are that device.
 *
 * Two operations rather than one with a direction flag: a caller that passes
 * the wrong direction to a combined operation gets a plausible-looking transfer
 * in the wrong direction, and the two have genuinely different consequences for
 * the arena -- OUT only reads it, IN writes into it. */
#define DEV_OP_XFER_OUT 5u  /* arena -> device */
#define DEV_OP_XFER_IN  6u  /* device -> arena */

/* Ceiling on one transfer, and it is deliberately small.
 *
 * Every byte crosses a kernel bounce buffer (see below), so this is DRAM that
 * exists whether or not anything transfers. 64 bytes covers an I2C register
 * write, a sector header, a short packet; anything larger should be several
 * transfers, which the caller has to be able to do anyway once a device is
 * bigger than one buffer. */
#define DEVICE_XFER_MAX 64u

#define DEV_F_READ   (1u << 0)
#define DEV_F_WRITE  (1u << 1)
#define DEV_F_SLOW   (1u << 2)  /* costs milliseconds; ends the caller's slice */

/* Reading CHANGES state -- the value is consumed and the next reader gets a
 * different one.
 *
 * Added because the `dev` listing samples channel 0 of every readable device to
 * show something useful, and for `keys` that pops a keypress: merely listing
 * the table ate a character. A diagnostic that alters what it reports is worse
 * than one that reports nothing, and this kernel has spent a day on instruments
 * that lied. Anything enumerating devices must skip these. */
#define DEV_F_CONSUME (1u << 3)
#define DEV_F_XFER    (1u << 4)  /* supports DEV_OP_XFER_OUT / _IN */

/* Who is asking.
 *
 * An application passes its own id; the kernel and the shell pass this. Added
 * when `store` became the third device and needed to bank its slots per caller
 * -- everything else an application owns here is confined to it, and
 * persistence with one shared pool would be the single place a program could
 * read what another program wrote.
 *
 * Most drivers ignore it, and that is fine. The point is that a driver which
 * needs it can have it without inventing its own way to find out, which is
 * exactly the kind of per-service improvisation the device model exists to
 * stop. */
#define DEVICE_CALLER_KERNEL ((uint32_t)APP_MAX)

typedef struct {
    const char *name;
    uint32_t    channels;
    uint32_t    flags;
    /* Both return 1 on success, 0 on refusal. Neither may fault, block
     * indefinitely, or retain the pointer it is given. */
    int (*read)(uint32_t caller, uint32_t chan, uint32_t *out);
    int (*write)(uint32_t caller, uint32_t chan, uint32_t value);

    /* Bulk transfer. `buf` is a KERNEL buffer of `len` bytes, never an arena
     * pointer -- the syscall copies in and out around these, so a driver cannot
     * hold a pointer into a program's memory even briefly.
     *
     * That is stricter than SYS BLIT, which lends display_blit() a const view of
     * the arena for the duration of the call. The difference is that the display
     * driver is one known function reviewed alongside the check, and this is the
     * EXTENSIBLE surface: every future device author would otherwise have to be
     * trusted to understand the lifetime of a pointer they were handed. Copying
     * costs DEVICE_XFER_MAX bytes and removes the question. */
    int (*xfer_out)(uint32_t caller, uint32_t chan, const uint8_t *buf,
                    uint32_t len);
    int (*xfer_in)(uint32_t caller, uint32_t chan, uint8_t *buf, uint32_t len);
} device_t;

void device_init(void);

/* ---- permissions ---------------------------------------------------------
 *
 * A bitmap per caller: bit N grants device N. Checked in device_read,
 * device_write, device_xfer_out and device_xfer_in, which is every route from a
 * program to hardware.
 *
 * Cheap because the hard part was built by accident. `device_t` grew a `caller`
 * argument so the `store` device could bank persistent slots per application,
 * which means every device call already carries who is asking -- and the caller
 * comes from `vm->app_id`, the kernel's own record, never from a register. A
 * program naming its own identity would be the same mistake as trusting an
 * offset it supplied.
 *
 * ---- what this is, and what it is not ------------------------------------
 *
 * This is CONTAINMENT, not security.
 *
 * A permission grant is only meaningful if the image it applies to cannot be
 * swapped for another, and nat-os has no image identity: no signature, no
 * content hash, nothing. Anyone able to flash the board can put any bytes
 * behind any name in the program table.
 *
 * What it does do is stop a buggy or careless program reaching hardware it was
 * never meant to, and make the intended capability surface explicit and
 * reviewable next to each program's arena size. That is worth having on its
 * own. It must not be described as security in any report until image identity
 * exists.
 *
 * A refusal is NOT a fault, for the same reason a bad channel is not: asking
 * for something you were not granted is legal, and a program that cannot
 * discover its own limits without dying cannot discover them. */
#define DEV_PERM_ALL  0xFFFFFFFFu
#define DEV_PERM_NONE 0u

void     device_grant(uint32_t caller, uint32_t mask);
uint32_t device_perms(uint32_t caller);
uint32_t device_denials(void);

int         device_count(void);
const char *device_name(uint32_t id);
int         device_info(uint32_t id, uint32_t *channels, uint32_t *flags);
int         device_read(uint32_t caller, uint32_t id, uint32_t chan,
                        uint32_t *out);
int         device_write(uint32_t caller, uint32_t id, uint32_t chan,
                         uint32_t value);

/* Bulk transfer through a kernel buffer. `buf` must not be arena memory; the
 * syscall is responsible for copying between it and the program. */
int         device_xfer_out(uint32_t caller, uint32_t id, uint32_t chan,
                            const uint8_t *buf, uint32_t len);
int         device_xfer_in(uint32_t caller, uint32_t id, uint32_t chan,
                           uint8_t *buf, uint32_t len);

/* Does this device's work cost milliseconds? The syscall uses this to decide
 * whether to end the caller's slice, which is the fourth property chapter 31
 * requires any replacement for hand-written syscalls to preserve. */
int         device_is_slow(uint32_t id);

uint32_t    device_reads(void);
uint32_t    device_writes(void);
uint32_t    device_refusals(void);

#endif /* NATOS_DEVICE_H */
