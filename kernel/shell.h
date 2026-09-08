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

    /* [step 356] The MANIFEST: names the image itself declares, resolved
     * against the device table at launch.
     *
     * This was a hand-written bitmap, with a comment arguing that declaring it
     * here rather than in the image kept both limits on a program visible in
     * one place. That was a fair trade when programs were written by hand in
     * assembly and the table was the only place to put anything -- but it means
     * THE KERNEL DECIDES WHAT THE PROGRAM WANTS, which is the wrong way round
     * and is exactly why a NatScript `permissions { }` block had nowhere to go.
     *
     * The image declares; the kernel disposes. A name the device table does not
     * know refuses the launch rather than dropping a permission quietly.
     *
     * Still containment rather than security, and device.h says why: a grant is
     * only meaningful if the image it applies to cannot be substituted, and
     * nothing here is signed. */
    /* [step 371] What the assembler computed over the image AND the manifest.
     * Recomputed at launch; a mismatch refuses. See app.h for why this is a
     * drift check and not a signature. */
    uint32_t           image_id;

    const char *const *perm_names;
    uint32_t           perm_count;
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

/* Runs one command line from somewhere other than the serial port.
 *
 * The on-screen shell uses this. It takes the same path as a typed line — same
 * parsing, same output — so an on-panel command cannot diverge from the serial
 * one it looks like. Lines longer than the shell's buffer are refused rather
 * than truncated, because a truncated command is a DIFFERENT command. */
void shell_run_line(const char *line);

#endif /* NATOS_SHELL_H */
