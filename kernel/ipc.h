/* nat-os — inter-application messaging.
 *
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
 *
 * One mailbox per application, holding one message. A second message to a full
 * mailbox is refused and counted rather than queued: a queue needs a policy for
 * what to drop when it fills, and there is no consumer yet whose requirements
 * would decide that policy. Refusing is the honest placeholder.
 */

#ifndef NATOS_IPC_H
#define NATOS_IPC_H

#include <stdint.h>

#define IPC_MSG_MAX 64u

void ipc_init(void);

/* Copies `len` bytes into `dst`'s mailbox, recording `from` as the sender.
 * Returns 0 on success, or non-zero if the destination is invalid, the mailbox
 * is occupied, or the length exceeds IPC_MSG_MAX. */
int ipc_send(int from, int dst, const uint8_t *data, uint32_t len);

/* Takes the pending message for `id`, if any. Returns its length and writes the
 * sender id through `from`; returns 0 when the mailbox is empty. Copies at most
 * `max` bytes and reports the true length, so a receiver offering too small a
 * buffer loses the tail rather than overrunning. */
uint32_t ipc_recv(int id, uint8_t *out, uint32_t max, int *from);

/* Discards any pending message. Called when an application is retired so a
 * successor in the same slot cannot read its predecessor's mail. */
void ipc_clear(int id);

uint32_t ipc_sent(void);
uint32_t ipc_delivered(void);
uint32_t ipc_refused(void);

#endif /* NATOS_IPC_H */
