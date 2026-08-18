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

#define DEV_F_READ   (1u << 0)
#define DEV_F_WRITE  (1u << 1)
#define DEV_F_SLOW   (1u << 2)  /* costs milliseconds; ends the caller's slice */

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
} device_t;

void device_init(void);

int         device_count(void);
const char *device_name(uint32_t id);
int         device_info(uint32_t id, uint32_t *channels, uint32_t *flags);
int         device_read(uint32_t caller, uint32_t id, uint32_t chan,
                        uint32_t *out);
int         device_write(uint32_t caller, uint32_t id, uint32_t chan,
                         uint32_t value);

/* Does this device's work cost milliseconds? The syscall uses this to decide
 * whether to end the caller's slice, which is the fourth property chapter 31
 * requires any replacement for hand-written syscalls to preserve. */
int         device_is_slow(uint32_t id);

uint32_t    device_reads(void);
uint32_t    device_writes(void);
uint32_t    device_refusals(void);

#endif /* NATOS_DEVICE_H */
