/* nat-os -- lwIP architecture layer. next_moves/08 step 232.
 *
 * lwIP expects the port to supply this header: compiler attributes, struct
 * packing, diagnostics and assertions. It is deliberately tiny, because the
 * port is running NO_SYS=1 and therefore needs no threading abstraction at
 * all -- no mutexes, no mailboxes, no sys_arch.c. See kernel/lwipopts.h.
 *
 * Byte order is not declared here on purpose: lwIP's own arch.h defaults
 * BYTE_ORDER to LITTLE_ENDIAN when the port says nothing, which is correct for
 * Xtensa. Declaring it again would be a second copy of a fact, and this
 * project has been bitten by those.
 */

#ifndef LWIP_ARCH_CC_H
#define LWIP_ARCH_CC_H

#include <stdint.h>
#include <stddef.h>

/* nat-os has no unistd.h, no errno.h and no stdio.h. */
#define LWIP_NO_UNISTD_H      1
#define LWIP_ERRNO_STDINCLUDE 0
#define LWIP_NO_INTTYPES_H    1

/* GCC on Xtensa packs with the usual attribute. */
#define PACK_STRUCT_FIELD(x)  x
#define PACK_STRUCT_STRUCT    __attribute__((packed))
#define PACK_STRUCT_BEGIN
#define PACK_STRUCT_END

/* Diagnostics go nowhere: there is no printf in this kernel, and LWIP_DEBUG is
 * off. An assertion is different -- it means lwIP has detected a state it
 * cannot continue from, and swallowing that would turn a stack bug into a
 * mystery. lwip_die() prints and halts. */
void lwip_die(const char *msg);

#define LWIP_PLATFORM_DIAG(x)   do { } while (0)
#define LWIP_PLATFORM_ASSERT(x) do { lwip_die(x); } while (0)

/* lwIP randomises TCP initial sequence numbers and DHCP transaction IDs with
 * this. The hardware RNG from step 196 is the right source; a constant here
 * would make every TCP connection and every DHCP exchange predictable. */
unsigned int lwip_rand_u32(void);
#define LWIP_RAND() (lwip_rand_u32())

#endif /* LWIP_ARCH_CC_H */
