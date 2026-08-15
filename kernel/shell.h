/* nat-os — minimal console shell.
 *
 * Runs as an ordinary native task and polls UART0. It holds no privileges the
 * rest of the kernel lacks; it is a front end to app_start() and app_kill().
 *
 * Programs are registered by the caller rather than referenced directly, so the
 * shell has no dependency on which images exist or how they are generated.
 */

#ifndef NATOS_SHELL_H
#define NATOS_SHELL_H

#include <stdint.h>

typedef struct {
    const char    *name;
    const uint8_t *img;
    uint32_t       len;
    uint32_t       arena_bytes;
    uint32_t       publish_off;
} shell_program_t;

void shell_register(const shell_program_t *table, int count);

/* Starts a registered program by name. Returns the application id, or -1 if the
 * name is unknown or no slot is free.
 *
 * Exists so the desktop launches through the SAME table the shell does. An
 * earlier launcher indexed the table directly and silently started a different
 * program than the one it named, because the table had been reordered — the
 * defect that made a `rogue` icon run `gfxrogue`. A name lookup cannot drift
 * out of step with the table it reads. */
int shell_launch(const char *name);

/* Prints the banner and prompt. Call once before the first shell_poll(). */
void shell_begin(void);

/* Consumes whatever input is waiting and returns. Never blocks, so the hosting
 * task stays preemptible and a user holding a key cannot starve the system. */
void shell_poll(void);

#endif /* NATOS_SHELL_H */
