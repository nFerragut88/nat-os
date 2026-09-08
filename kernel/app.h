/* nat-os — application table and lifecycle.
 *
 * An application is a bytecode program, an arena it is confined to, and a VM
 * executing it. This layer owns their lifecycle: starting one allocates an
 * arena and loads an image into it; stopping one releases the arena completely,
 * whether it stopped because it finished, because it faulted, or because it was
 * killed.
 *
 * Scheduling here is a THIRD level, above the two that already exist:
 *
 *   1. The timer interrupt preempts native tasks (M2).
 *   2. vm_run()'s quantum returns control at a bytecode boundary (M4).
 *   3. app_tick() round-robins that quantum across every live application.
 *
 * Only the third is aware of applications. The other two are unchanged and do
 * not know this layer exists, which is why adding it required no modification
 * to the scheduler or the interpreter.
 *
 * A faulting application is terminated. It cannot damage another because it
 * cannot address another: a VM offset is arena-relative and bounds-checked, so
 * an address outside its own arena is not merely refused, it is inexpressible.
 */

#ifndef NATOS_APP_H
#define NATOS_APP_H

#include <stdint.h>
#include "display.h"

#define APP_MAX 4

/* ---- where an application may draw, and where it may not ----------------
 *
 * Exported because the close button depends on the exact boundary. The kernel
 * reserves a column at the right of every strip and draws the X there, so the
 * viewport handed to the application STOPS short of it.
 *
 * That is isolation, not layout. If the close button lived inside the viewport
 * an application could paint over it, draw a decoy elsewhere, or simply fill
 * its strip and hide the way out. Putting it outside means the one control the
 * user needs in order to escape a misbehaving program is the one control that
 * program cannot touch — the same argument as the viewport itself
 * (UM-NATOS-016 §2), applied to a pixel the user owns rather than one the
 * application does. */
#define APP_VIEW_Y0     224u
#define APP_VIEW_PITCH   16u
#define APP_VIEW_H       14u
/* The kernel's column: the program's NAME and its close button.
 *
 * The name is there because without it the area is unreadable. Four empty
 * strips with two X floating in them is what a user actually saw, and the
 * honest reading of that is "what are those" — the programs starting at boot
 * exchange messages rather than drawing, so nothing identified them. A close
 * button for something you cannot name is worse than no close button.
 *
 * 7 characters at 6 px, then the X. Everything left of it belongs to the
 * application. */
#define APP_NAME_W       44u
#define APP_CLOSE_W      16u
#define APP_CHROME_W    (APP_NAME_W + APP_CLOSE_W)
#define APP_VIEW_W      (DISP_W - APP_CHROME_W)

typedef enum {
    APP_FREE = 0,
    APP_RUNNING,
    APP_HALTED,      /* ran to completion                       */
    APP_FAULTED,     /* violated a rule and was terminated       */
    APP_KILLED       /* stopped from the shell                   */
} app_state_t;

/* Loads `img` into a fresh arena of `arena_bytes` and starts it. `publish_off`
 * is the byte offset within the arena at which the program publishes a progress
 * word, so the kernel can observe it without the program cooperating.
 *
 * Returns an application id, or -1 if no slot or no memory. */
int app_start(const char *name, const uint8_t *img, uint32_t len,
              uint32_t arena_bytes, uint32_t publish_off);

/* Stops an application and releases its arena. Safe on an already-stopped id. */
void app_kill(int id);

/* Gives every RUNNING application one quantum of bytecode. Applications that
 * halt or fault during it are reported and terminated here. */
void app_tick(uint32_t quantum);

/* [step 277] Suspend or restore every application viewport. The shell's
 * keyboard covers the band they draw in; this stops the draw, not the
 * program. */
void        app_views_suspend(int on);

/* [step 369] Give one application the main region instead of its strip, or
 * take it back with id = -1.
 *
 * The canvas is DISP_W x (DESK_H - APP_FULL_Y0): the top rows stay the
 * kernel's, so the control that closes the program is outside the program's
 * reach. That is the same argument the strip design makes about its close
 * button, applied to a region nineteen times the size.
 *
 * Only one application can hold it. Every other running program keeps its
 * strip, which is why `ps` and the band still work while one is focused. */
#define APP_FULL_Y0  22u

/* [step 371] FNV-1a over an image AND its manifest, recomputed at load and
 * compared with what the assembler wrote down.
 *
 * WHAT THIS IS NOT. It is not a signature and it is not the image identity
 * device.h asks for. The id lives in the same kernel image as the bytes it
 * describes, so anybody able to reflash the board rewrites both -- which is
 * exactly the sentence device.h has carried since permissions were written.
 *
 * WHAT IT IS. A binding between a manifest and the bytes it was granted for. A
 * stale generated header, a table entry pointing at another program's array, a
 * manifest edited without rebuilding -- those are mechanical failures with no
 * attacker in them, and they are the ones that actually happen. They are also
 * the ones that would let a program run under somebody else's permissions.
 *
 * It becomes load-bearing the moment a program comes from anywhere other than
 * the kernel image: flash, a card, a network. Nothing does yet. */
uint32_t    app_image_id(const uint8_t *img, uint32_t len,
                         const char *const *perms, uint32_t nperms);

void        app_view_focus(int id);
int         app_view_focused(void);

int         app_state(int id);
const char *app_state_name(int id);
const char *app_name(int id);
uint32_t    app_instructions(int id);
uint32_t    app_published(int id);
uint32_t    app_arena_bytes(int id);
uint32_t    app_arena_base(int id);
int         app_fault(int id);
uint32_t    app_fault_detail(int id);
int         app_live_count(void);

#endif /* NATOS_APP_H */
