/* cyd-os — application table and lifecycle. See app.h for the layering. */

#include "app.h"
#include "arena.h"
#include "console.h"
#include "display.h"
#include "uart.h"
#include "vm.h"

typedef struct {
    app_state_t state;
    const char *name;
    int         arena;
    uint32_t    base;
    uint32_t    bytes;
    uint32_t    publish_off;
    vm_t        vm;
} app_t;

static app_t g_apps[APP_MAX];

/* Where applications may draw. Above this the kernel keeps its status area;
 * below it the colour strip. Neither is reachable from an application. */
#define APP_VIEW_Y0    168u
#define APP_VIEW_PITCH 28u
#define APP_VIEW_H     26u

/* Releases the arena and records why the application stopped. Kept in one place
 * because "terminating an application releases its arena completely" is an exit
 * criterion, and three separate paths reach it — halt, fault, and kill. Three
 * copies of a release is three chances to leak one. */
static void retire(app_t *a, app_state_t why)
{
    if (a->arena >= 0) {
        arena_destroy(a->arena);
        a->arena = -1;
    }

    /* Wipe the strip on the way out, so a dead application does not leave its
     * last frame on the panel looking like a live one. */
    display_fill_rect(a->vm.vx, a->vm.vy, a->vm.vw, a->vm.vh, COLOR_BLACK);
    a->base  = 0;
    a->state = why;
}

int app_start(const char *name, const uint8_t *img, uint32_t len,
              uint32_t arena_bytes, uint32_t publish_off)
{
    if (len > arena_bytes) {
        return -1;                  /* image would not fit its own arena */
    }

    for (int id = 0; id < APP_MAX; id++) {
        app_t *a = &g_apps[id];
        if (a->state == APP_RUNNING) {
            continue;
        }

        int arena = arena_create(arena_bytes);
        if (arena < 0) {
            return -1;
        }

        uint32_t base = 0;
        if (arena_bounds(arena, &base, 0) != 0) {
            arena_destroy(arena);
            return -1;
        }

        uint8_t *dst = (uint8_t *)base;
        for (uint32_t i = 0; i < len; i++) {
            dst[i] = img[i];
        }

        if (vm_init(&a->vm, arena) != 0) {
            arena_destroy(arena);
            return -1;
        }

        /* One horizontal strip per slot. Assigned by the kernel and never by
         * the application: a program can ask how large its canvas is, but the
         * only coordinates it can express are inside it. Same property as its
         * arena, applied to pixels. */
        vm_set_viewport(&a->vm, 0u, APP_VIEW_Y0 + (uint32_t)id * APP_VIEW_PITCH,
                        DISP_W, APP_VIEW_H);

        a->state       = APP_RUNNING;
        a->name        = name;
        a->arena       = arena;
        a->base        = base;
        a->bytes       = arena_bytes;
        a->publish_off = publish_off;
        return id;
    }

    return -1;
}

void app_kill(int id)
{
    if (id < 0 || id >= APP_MAX || g_apps[id].state != APP_RUNNING) {
        return;
    }
    retire(&g_apps[id], APP_KILLED);
}

void app_tick(uint32_t quantum)
{
    for (int id = 0; id < APP_MAX; id++) {
        app_t *a = &g_apps[id];
        if (a->state != APP_RUNNING) {
            continue;
        }

        int r = vm_run(&a->vm, quantum);
        if (r == VM_RUN_QUANTUM) {
            continue;               /* still going; next one gets a turn */
        }

        if (r == VM_RUN_FAULTED) {
            /* Diagnose before releasing: the arena is about to go back to the
             * heap, and the offending offset is only meaningful alongside the
             * size it exceeded. */
            console_lock();
            uart_puts("\n  [app ");
            uart_put_dec((unsigned int)id);
            uart_puts(" '");
            uart_puts(a->name);
            uart_puts("' TERMINATED] ");
            uart_puts(vm_fault_name(vm_fault(&a->vm)));
            uart_puts(" at offset ");
            uart_put_dec(a->vm.fault_detail);
            uart_puts(" of ");
            uart_put_dec(a->bytes);
            uart_puts(" B arena, pc=");
            uart_put_dec(a->vm.fault_pc);
            uart_puts(", after ");
            uart_put_dec(a->vm.executed);
            uart_puts(" instructions\n");
            console_unlock();
            retire(a, APP_FAULTED);
        } else {
            console_lock();
            uart_puts("\n  [app ");
            uart_put_dec((unsigned int)id);
            uart_puts(" '");
            uart_puts(a->name);
            uart_puts("' finished] status=");
            uart_put_dec(a->vm.exit_status);
            uart_puts(" after ");
            uart_put_dec(a->vm.executed);
            uart_puts(" instructions\n");
            console_unlock();
            retire(a, APP_HALTED);
        }
    }
}

int app_state(int id)
{
    return (id >= 0 && id < APP_MAX) ? (int)g_apps[id].state : (int)APP_FREE;
}

const char *app_state_name(int id)
{
    switch (app_state(id)) {
    case APP_RUNNING: return "running";
    case APP_HALTED:  return "halted";
    case APP_FAULTED: return "faulted";
    case APP_KILLED:  return "killed";
    default:          return "-";
    }
}

const char *app_name(int id)
{
    if (id < 0 || id >= APP_MAX || g_apps[id].name == 0) {
        return "-";
    }
    return g_apps[id].name;
}

uint32_t app_instructions(int id)
{
    return (id >= 0 && id < APP_MAX) ? g_apps[id].vm.executed : 0u;
}

/* Reads the progress word out of a live arena. The kernel can see into an
 * arena; a program cannot see out of one. That asymmetry is the whole design. */
uint32_t app_published(int id)
{
    if (id < 0 || id >= APP_MAX || g_apps[id].state != APP_RUNNING ||
        g_apps[id].base == 0u) {
        return 0u;
    }
    return *(volatile uint32_t *)(g_apps[id].base + g_apps[id].publish_off);
}

uint32_t app_arena_bytes(int id)
{
    return (id >= 0 && id < APP_MAX) ? g_apps[id].bytes : 0u;
}

int app_fault(int id)
{
    return (id >= 0 && id < APP_MAX) ? vm_fault(&g_apps[id].vm) : 0;
}

uint32_t app_fault_detail(int id)
{
    return (id >= 0 && id < APP_MAX) ? g_apps[id].vm.fault_detail : 0u;
}

int app_live_count(void)
{
    int n = 0;
    for (int id = 0; id < APP_MAX; id++) {
        if (g_apps[id].state == APP_RUNNING) {
            n++;
        }
    }
    return n;
}
