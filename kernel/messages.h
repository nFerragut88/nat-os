/* nat-os — saved messages, kept in flash.
 *
 * Notes that outlive the power. The boot record in UM-NATOS-018 proved the
 * mechanism; this is the first thing stored there that a person wrote.
 *
 * ---- why a second sector -------------------------------------------------
 *
 * The kernel's own record lives at FLASH_DATA_ADDR. Messages live in the next
 * sector along, separately, because a flash write is erase-then-write over a
 * whole 4 KB sector: sharing one sector would mean rewriting the boot counter
 * every time a message is saved, and losing every message if a save were
 * interrupted while the counter was the thing being rewritten.
 *
 * Separate sectors mean the two can only damage themselves.
 *
 * ---- 160 characters ------------------------------------------------------
 *
 * The classic SMS limit, chosen because it is the right length for the thing
 * this is imitating and because it bounds the store: eight of them plus a
 * header is under 1.4 KB, comfortably inside one sector.
 */

#ifndef NATOS_MESSAGES_H
#define NATOS_MESSAGES_H

#include <stdint.h>

#define MSG_MAX  8u         /* messages kept; the oldest is dropped */
#define MSG_LEN  160u       /* characters each, plus a terminator   */

/* Reads and validates the store. Returns 0 if a valid one was found; on failure
 * the store is reset to empty, so a first run and a corrupt sector behave
 * identically — the same rule the boot record uses. */
int msg_load(void);

/* Appends a message and writes the sector. Returns 0 on success. When the store
 * is full the OLDEST is dropped, because a note pad that refuses to save is
 * worse than one that forgets the thing you wrote first. */
int msg_save(const char *text);

uint32_t    msg_count(void);
const char *msg_get(uint32_t i);     /* 0 if out of range; newest is last */

/* Erases every message. Returns 0 on success. */
int msg_clear(void);

#endif /* NATOS_MESSAGES_H */
