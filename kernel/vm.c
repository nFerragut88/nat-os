/* nat-os — bytecode virtual machine. See vm.h for the ISA and the isolation
 * rationale.
 *
 * The dispatch loop is a switch. A computed-goto table would be faster, but it
 * is also the kind of construct where a missing entry is a silent jump rather
 * than a diagnosed fault, and correctness of the bounds checks matters more
 * here than dispatch overhead. Revisit only with a measurement showing dispatch
 * is actually the bottleneck.
 */

#include "vm.h"
#include "vmarg.h"
#include "device.h"
#include "term.h"
#include "arena.h"
#include "display.h"
#include "ipc.h"
#include "touch.h"
#include "timer.h"
#include "uart.h"

/* Bounds predicate. Deliberately identical in meaning to arena_contains(), but
 * expressed against the cached base/size so the inner loop does not make a
 * function call per access.
 *
 * Comparison happens in the OFFSET domain. Testing `off + len` against `size`
 * directly can wrap and admit an access far outside the arena, which is exactly
 * what a hostile length would aim for. m4_selftest() cross-checks this against
 * arena_contains() so the duplication cannot drift unnoticed. */
/* ---- event delivery -----------------------------------------------------
 *
 * See the note in vm.h. Delivery is a call the kernel injects; the handler's
 * own `ret` unwinds it.
 */

uint32_t vm_events_delivered(const vm_t *vm) { return vm->evt_delivered; }
uint32_t vm_events_dropped(const vm_t *vm)   { return vm->evt_dropped; }

void vm_event_poll(vm_t *vm)
{
    /* Cheap enough to call every tick for every application: with nothing
     * registered this is two loads and a branch. */
    if (!vm->evt_handler[VM_EVT_TICK] && !vm->evt_handler[VM_EVT_KEY]) {
        return;
    }

    if (vm->evt_handler[VM_EVT_TICK]) {
        uint32_t now = timer_ticks();
        if ((int32_t)(now - vm->evt_due[VM_EVT_TICK]) >= 0) {
            /* Re-armed from NOW, not from the old deadline. Catching up on
             * missed intervals would fire a burst the moment a slow handler
             * finished, and this kernel has already been bitten once by a
             * catch-up loop -- the raycaster walked through a wall applying
             * twenty steps of movement in a single frame (UM-NATOS-028). A
             * program that wants elapsed time can read the tick it is given. */
            vm->evt_due[VM_EVT_TICK] = now + vm->evt_param[VM_EVT_TICK];
            if (vm->evt_pending & (1u << VM_EVT_TICK)) {
                vm->evt_dropped++;      /* previous one never ran */
            }
            vm->evt_arg[VM_EVT_TICK] = now;
            vm->evt_pending |= (1u << VM_EVT_TICK);
        }
    }

    if (vm->evt_handler[VM_EVT_KEY] && !(vm->evt_pending & (1u << VM_EVT_KEY))) {
        /* Only taken from the queue when there is somewhere to put it. Popping
         * a key we would then have to drop would consume it on behalf of a
         * program that never saw it, and the queue is shared. */
        uint32_t ch = 0;
        if (term_key_pop(&ch)) {
            vm->evt_arg[VM_EVT_KEY] = ch;
            vm->evt_pending |= (1u << VM_EVT_KEY);
        }
    }
}

int vm_event_dispatch(vm_t *vm)
{
    if (!vm->evt_pending) {
        return 0;
    }

    /* One deep, never re-entrant. An event arriving while a handler runs waits
     * for it to finish; a second of the same kind is counted and dropped. The
     * alternative is a handler interrupting itself, which needs either a
     * second saved register file per nesting level or a rule nobody can
     * enforce. */
    if (vm->in_handler) {
        return 0;
    }

    if (vm->call_sp >= VM_CALL_DEPTH) {
        /* No room to push the return address. Leave it pending rather than
         * faulting the program: it is inside a deep call of its own making and
         * will unwind. */
        return 0;
    }

    uint32_t id;
    for (id = 0; id < VM_EVT_COUNT; id++) {
        if (vm->evt_pending & (1u << id)) {
            break;
        }
    }
    if (id == VM_EVT_COUNT || !vm->evt_handler[id]) {
        vm->evt_pending = 0;            /* handler was unregistered under it */
        return 0;
    }

    for (uint32_t i = 0; i < VM_REGS; i++) {
        vm->saved_reg[i] = vm->reg[i];
    }
    vm->handler_sp = vm->call_sp;
    vm->call[vm->call_sp++] = vm->pc;   /* the handler's `ret` lands here */
    vm->pc = vm->evt_handler[id];
    vm->reg[0] = vm->evt_arg[id];       /* the tick, or the character */
    vm->in_handler = 1;
    vm->evt_pending &= ~(1u << id);
    vm->evt_delivered++;
    return 1;
}

int vm_in_bounds(const vm_t *vm, uint32_t off, uint32_t len)
{
    if (off > vm->size) {
        return 0;
    }
    return len <= vm->size - off;
}

/* Exported so the argument harness can record a fault the same way the
 * interpreter does. vmarg.c is deliberately outside this file -- the device
 * model will use it and does not live here -- but a harness that could only
 * REFUSE without diagnosing would replace a bounds bug with a silent one. */
void vm_raise(vm_t *vm, int code, uint32_t detail)
{
    vm->fault        = code;
    vm->fault_pc     = vm->pc;
    vm->fault_detail = detail;
}

static void fault(vm_t *vm, int code, uint32_t detail)
{
    vm_raise(vm, code, detail);
}

int vm_init(vm_t *vm, int arena_id)
{
    uint32_t base = 0, size = 0;
    if (arena_bounds(arena_id, &base, &size) != 0) {
        return -1;
    }

    for (int i = 0; i < VM_REGS; i++) {
        vm->reg[i] = 0;
    }
    vm->pc = 0;
    vm->call_sp = 0;
    vm->arena = arena_id;
    vm->base = base;
    vm->size = size;
    vm->fault = VM_FAULT_NONE;
    vm->fault_pc = 0;
    vm->fault_detail = 0;
    vm->executed = 0;
    vm->exit_status = 0;
    vm->yield_now = 0;

    /* Whole panel by default. Correct for a VM the kernel hosts itself;
     * app.c narrows it before an application ever runs. */
    vm->vx = 0;
    vm->vy = 0;
    vm->vw = DISP_W;
    vm->vh = DISP_H;
    vm->app_id = -1;
    return 0;
}

void vm_set_app_id(vm_t *vm, int id) { vm->app_id = id; }

void vm_set_viewport(vm_t *vm, uint32_t x, uint32_t y, uint32_t w, uint32_t h)
{
    vm->vx = x;
    vm->vy = y;
    vm->vw = w;
    vm->vh = h;
}

int vm_fault(const vm_t *vm) { return vm->fault; }

const char *vm_fault_name(int f)
{
    switch (f) {
    case VM_FAULT_NONE:       return "none";
    case VM_FAULT_OPCODE:     return "bad opcode";
    case VM_FAULT_REG:        return "bad register";
    case VM_FAULT_BOUNDS:     return "out of bounds";
    case VM_FAULT_ALIGN:      return "misaligned";
    case VM_FAULT_DIV0:       return "divide by zero";
    case VM_FAULT_PC:         return "bad pc";
    case VM_FAULT_CALL_DEPTH: return "call depth";
    case VM_FAULT_RET:        return "return underflow";
    case VM_FAULT_SYSCALL:    return "bad syscall";
    case VM_FAULT_STRING:     return "unterminated string";
    default:                  return "unknown";
    }
}

/* ---- checked accessors -------------------------------------------------- */

static uint32_t load_u32(vm_t *vm, uint32_t off, int *ok)
{
    if (!vm_in_bounds(vm, off, 4)) {
        fault(vm, VM_FAULT_BOUNDS, off);
        *ok = 0;
        return 0;
    }
    if (off & 3u) {
        fault(vm, VM_FAULT_ALIGN, off);
        *ok = 0;
        return 0;
    }
    *ok = 1;
    return *(volatile uint32_t *)(vm->base + off);
}

static uint32_t load_u8(vm_t *vm, uint32_t off, int *ok)
{
    if (!vm_in_bounds(vm, off, 1)) {
        fault(vm, VM_FAULT_BOUNDS, off);
        *ok = 0;
        return 0;
    }
    *ok = 1;
    return *(volatile uint8_t *)(vm->base + off);
}

static int store_u32(vm_t *vm, uint32_t off, uint32_t v)
{
    if (!vm_in_bounds(vm, off, 4)) {
        fault(vm, VM_FAULT_BOUNDS, off);
        return 0;
    }
    if (off & 3u) {
        fault(vm, VM_FAULT_ALIGN, off);
        return 0;
    }
    *(volatile uint32_t *)(vm->base + off) = v;
    return 1;
}

static int store_u8(vm_t *vm, uint32_t off, uint32_t v)
{
    if (!vm_in_bounds(vm, off, 1)) {
        fault(vm, VM_FAULT_BOUNDS, off);
        return 0;
    }
    *(volatile uint8_t *)(vm->base + off) = (uint8_t)v;
    return 1;
}

/* ---- display -------------------------------------------------------------
 *
 * Clipping happens HERE, not in the display driver. The driver clips to the
 * panel, which is the wrong boundary: an application must be confined to its
 * viewport, and a program allowed to paint the whole screen could erase every
 * other application's output and the kernel's status area with it.
 *
 * Arithmetic is in the offset domain for the same reason arena_contains() is
 * (UM-NATOS-010 §5.2). Coordinates arrive as uint32_t, so a program passing a
 * "negative" value hands over something near 0xFFFFFFFF; comparing it against
 * the viewport width rejects it, whereas computing x + w first would wrap and
 * admit it.
 */
/* Containment, measured rather than observed.
 *
 * Whether a hostile program's drawing stays inside its viewport is visible on
 * the panel, but "it looks right" is a judgement and this is the claim the
 * whole design turns on. These counters make it a number: every rectangle that
 * actually reaches the driver is re-checked against the viewport it came from,
 * and g_vp_escapes counts any that would fall outside. It must stay at zero no
 * matter what an application asks for. */
static uint32_t g_vp_escapes;
static uint32_t g_vp_max_y;     /* highest absolute row any app has painted */
static uint32_t g_vp_calls;

/* Touches offered to an application, and touches withheld because they landed
 * outside its viewport. The second number is the containment evidence: it must
 * be non-zero whenever anyone touches elsewhere on the panel, and no
 * application may ever see those coordinates. */
static uint32_t g_touch_given, g_touch_withheld;
static uint32_t g_blits;

/* Drawing primitives dropped because the panel was busy. Counted, not hidden:
 * a best-effort policy that silently discards work is indistinguishable from a
 * broken one, and the ratio of skipped to drawn is the only thing that says
 * whether the policy is reasonable or is starving applications of the screen. */
static uint32_t g_draw_skipped;

uint32_t vm_draw_skipped(void) { return g_draw_skipped; }
static uint32_t g_ipc_bad_buffer;

uint32_t vm_touch_given(void)    { return g_touch_given; }
uint32_t vm_touch_withheld(void) { return g_touch_withheld; }
uint32_t vm_blits(void)          { return g_blits; }
uint32_t vm_ipc_bad_buffer(void) { return g_ipc_bad_buffer; }

uint32_t vm_viewport_escapes(void) { return g_vp_escapes; }
uint32_t vm_viewport_max_y(void)   { return g_vp_max_y; }
uint32_t vm_viewport_calls(void)   { return g_vp_calls; }

static void vp_fill(vm_t *vm, uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                    uint16_t colour)
{
    if (x >= vm->vw || y >= vm->vh) {
        return;                         /* origin outside — nothing to draw */
    }
    if (w > vm->vw - x) { w = vm->vw - x; }
    if (h > vm->vh - y) { h = vm->vh - y; }
    if (w == 0u || h == 0u) {
        return;
    }
    /* Re-derive the absolute rectangle and confirm it lies wholly inside the
     * viewport. This duplicates the clipping above on purpose: the check that
     * matters is on what actually reaches the panel, not on what the clipping
     * intended. */
    uint32_t ax = vm->vx + x, ay = vm->vy + y;
    if (ax < vm->vx || ay < vm->vy ||
        ax + w > vm->vx + vm->vw || ay + h > vm->vy + vm->vh) {
        g_vp_escapes++;
        return;                         /* refuse it as well as count it */
    }
    g_vp_calls++;
    if (ay + h > g_vp_max_y) {
        g_vp_max_y = ay + h;
    }

    /* Best effort. See display_try_lock(): an application that blocks here
     * costs the whole system a scheduling round trip, and the pixel it wanted
     * to draw is worth far less than that. */
    if (!display_try_lock()) {
        g_draw_skipped++;
        return;
    }
    display_fill_rect(ax, ay, w, h, colour);
    display_unlock();
}

#define VM_STR_MAX 48

/* Copies a NUL-terminated string out of the arena into kernel memory, one
 * bounds-checked byte at a time. The copy is not paranoia: display_text() takes
 * a kernel pointer, and handing it an arena address would let a program's own
 * writes change the string mid-render. A string that runs to the end of the
 * arena without a terminator faults rather than reading past it. */
static int copy_string(vm_t *vm, uint32_t off, char *dst, uint32_t max)
{
    return vmarg_string(vm, off, dst, max);
}

static void vp_text(vm_t *vm, uint32_t x, uint32_t y, const char *s,
                    uint16_t fg, uint16_t bg, uint32_t scale)
{
    if (scale == 0u || scale > 4u) {
        scale = 1u;
    }
    if (x >= vm->vw || y >= vm->vh) {
        return;
    }
    /* Reject rather than clip vertically: half a line of text is worse than
     * none, and the glyph renderer has no partial-row mode. */
    if (8u * scale > vm->vh - y) {
        return;
    }

    /* Truncate to what fits horizontally, so a long string cannot run out of
     * the viewport and across a neighbour's. */
    char buf[VM_STR_MAX];
    uint32_t room = (vm->vw - x) / (6u * scale);
    uint32_t i = 0;
    while (s[i] && i < room && i < VM_STR_MAX - 1u) {
        buf[i] = s[i];
        i++;
    }
    buf[i] = 0;
    if (i == 0u) {
        return;
    }
    if (!display_try_lock()) {
        g_draw_skipped++;
        return;
    }
    display_text(vm->vx + x, vm->vy + y, buf, fg, bg, scale);
    display_unlock();
}

/* ---- syscalls ----------------------------------------------------------- */

/* Returns 0 to keep running, 1 to stop (exit or fault). */
static int do_syscall(vm_t *vm, uint32_t num)
{
    switch (num) {
    case VM_SYS_EXIT:
        vm->exit_status = vm->reg[0];
        return 1;

    case VM_SYS_PUTC:
        uart_putc((char)(vm->reg[0] & 0xFFu));
        return 0;

    case VM_SYS_PUTS: {
        /* Walked one byte at a time, each bounds-checked. A string that runs
         * to the end of the arena without a terminator is a fault, not a read
         * past the end — this is the classic way a "harmless" print primitive
         * becomes an information leak. */
        uint32_t off = vm->reg[0];
        for (uint32_t n = 0; n < vm->size; n++) {
            int ok = 0;
            uint32_t ch = load_u8(vm, off + n, &ok);
            if (!ok) {
                return 1;
            }
            if (ch == 0u) {
                return 0;
            }
            uart_putc((char)ch);
        }
        fault(vm, VM_FAULT_STRING, off);
        return 1;
    }

    case VM_SYS_DEVICE: {
        uint32_t op = vm->reg[0];

        /* Who is asking, taken from the vm rather than from the program.
         *
         * The `store` device banks its slots by caller, so this value decides
         * which persistent words a program can reach. It must therefore come
         * from the kernel's own record of which application this is -- a
         * caller id supplied in a register would be a program naming its own
         * bank, which is the same shape of mistake as trusting an offset.
         *
         * A vm with no application id (there is none today, but vm_init leaves
         * it -1 until vm_set_app_id) is refused a bank rather than given the
         * kernel's. */
        uint32_t caller = (vm->app_id >= 0 && vm->app_id < APP_MAX)
                        ? (uint32_t)vm->app_id
                        : DEVICE_CALLER_KERNEL + 1u;   /* deliberately invalid */

        /* Every program-supplied quantity reaching a driver goes through this
         * one switch, and the only one that is a buffer goes through vmarg.
         * Drivers below never see the vm, never see the arena, and receive
         * scalars that have already been checked. */
        switch (op) {
        case DEV_OP_COUNT:
            vm->reg[0] = (uint32_t)device_count();
            return 0;

        case DEV_OP_INFO: {
            uint32_t chans = 0, flags = 0;
            vm->reg[0] = (uint32_t)device_info(vm->reg[1], &chans, &flags);
            vm->reg[1] = chans;
            vm->reg[2] = flags;
            return 0;
        }

        case DEV_OP_NAME: {
            const char *nm = device_name(vm->reg[1]);
            if (!nm) {
                vm->reg[0] = 0;
                return 0;               /* refusal, not a fault */
            }
            uint32_t n = 0;
            while (nm[n]) { n++; }
            n++;                        /* the terminator goes too */

            /* The program says how much room it has; it does not get to be
             * believed about it. `max` is bounded before anything is copied,
             * and vmarg_store checks the destination against the arena. */
            uint32_t max = vm->reg[3];
            if (max > DEVICE_NAME_MAX) {
                max = DEVICE_NAME_MAX;
            }
            if (n > max) {
                n = max;
            }
            if (n == 0u) {
                vm->reg[0] = 0;
                return 0;
            }
            char tmp[DEVICE_NAME_MAX];
            for (uint32_t i = 0; i < n; i++) {
                tmp[i] = nm[i];
            }
            tmp[n - 1u] = 0;            /* always terminated, even truncated */

            if (!vmarg_store(vm, vm->reg[2], tmp, n)) {
                return 1;               /* harness recorded the fault */
            }
            vm->reg[0] = 1;
            return 0;
        }

        case DEV_OP_READ: {
            /* The id is captured BEFORE r1 is overwritten with the result.
             * Reading it back afterwards asks whether the light LEVEL is a slow
             * device, which is nonsense that happens to compile. */
            uint32_t id = vm->reg[1];
            uint32_t v  = 0;
            int ok = device_read(caller, id, vm->reg[2], &v);
            vm->reg[0] = (uint32_t)ok;
            vm->reg[1] = v;
            if (device_is_slow(id)) {
                vm->yield_now = 1;      /* milliseconds, not instructions */
            }
            return 0;
        }

        case DEV_OP_WRITE: {
            uint32_t id = vm->reg[1];
            vm->reg[0] = (uint32_t)device_write(caller, id, vm->reg[2],
                                                vm->reg[3]);
            if (device_is_slow(id)) {
                vm->yield_now = 1;
            }
            return 0;
        }

        case DEV_OP_XFER_OUT:
        case DEV_OP_XFER_IN: {
            /* The bounce buffer. Static, and the reason DEVICE_XFER_MAX is
             * small: it exists whether or not anything transfers.
             *
             * Its purpose is that no arena pointer ever reaches a driver, in
             * either direction. SYS BLIT lends display_blit() a const view of
             * the arena, which is defensible for one known function reviewed
             * beside the check; a device table is the EXTENSIBLE surface, and
             * every future driver author would otherwise have to be trusted
             * with the lifetime of a pointer they were handed. Copying costs
             * 64 bytes and removes the question.
             *
             * Safe as a single static despite four applications: syscalls do
             * not nest, and app_tick() runs one vm at a time. */
            static uint8_t bounce[DEVICE_XFER_MAX];

            uint32_t id   = vm->reg[1];
            uint32_t chan = vm->reg[2];
            uint32_t off  = vm->reg[3];
            uint32_t len  = vm->reg[4];

            /* Length is bounded BEFORE it is used to size anything. A caller
             * asking for more than the buffer holds is refused, not clamped:
             * a silently shortened transfer is a program being lied to about
             * how much it moved. */
            if (len == 0u || len > DEVICE_XFER_MAX) {
                vm->reg[0] = 0;
                return 0;
            }

            if (op == DEV_OP_XFER_OUT) {
                vm_span_t src;
                if (!vmarg_span(vm, off, len, 1u, &src)) {
                    return 1;           /* harness recorded the fault */
                }
                for (uint32_t i = 0; i < len; i++) {
                    bounce[i] = src.ptr[i];
                }
                vm->reg[0] = (uint32_t)device_xfer_out(caller, id, chan,
                                                       bounce, len);
            } else {
                /* Validate the DESTINATION before the transfer, so a bad offset
                 * costs nothing on the bus. A device asked to produce bytes
                 * that then cannot be delivered has already had its side
                 * effect. */
                vm_span_t dst;
                if (!vmarg_span(vm, off, len, 1u, &dst)) {
                    return 1;
                }
                if (!device_xfer_in(caller, id, chan, bounce, len)) {
                    vm->reg[0] = 0;
                    return 0;
                }
                if (!vmarg_store(vm, off, bounce, len)) {
                    return 1;
                }
                vm->reg[0] = 1;
            }

            if (device_is_slow(id)) {
                vm->yield_now = 1;
            }
            return 0;
        }

        default:
            vm->reg[0] = 0;             /* unknown operation: refused, alive */
            return 0;
        }
    }

    case VM_SYS_EVENT: {
        uint32_t id     = vm->reg[0];
        uint32_t offset = vm->reg[1];
        uint32_t param  = vm->reg[2];

        if (id >= VM_EVT_COUNT) {
            vm->reg[0] = 0;             /* refused, not a fault */
            return 0;
        }

        if (offset == 0u) {             /* unregister */
            vm->evt_handler[id] = 0;
            vm->evt_pending &= ~(1u << id);
            vm->reg[0] = 1;
            return 0;
        }

        /* The handler offset is a program-supplied quantity pointing at CODE,
         * so it gets the same treatment as any other: bounds-checked in the
         * offset domain, and instruction-aligned because the interpreter
         * fetches four bytes and would otherwise decode a shifted word.
         *
         * A bad offset here is a FAULT rather than a refusal. Every other
         * refusal in this system means "the device cannot answer that"; this
         * one means the program handed the kernel an address to jump to that
         * is not inside it, which is the same class of error as a bad store. */
        if (!vm_in_bounds(vm, offset, VM_INSN_BYTES)) {
            fault(vm, VM_FAULT_BOUNDS, offset);
            return 1;
        }
        if (offset % VM_INSN_BYTES) {
            fault(vm, VM_FAULT_ALIGN, offset);
            return 1;
        }

        vm->evt_handler[id] = offset;
        vm->evt_param[id]   = param;

        if (id == VM_EVT_TICK) {
            /* An interval of zero would fire every poll and starve the main
             * flow, so it is clamped to one tick rather than refused -- a
             * program asking for "as often as possible" gets exactly that. */
            if (vm->evt_param[id] == 0u) {
                vm->evt_param[id] = 1u;
            }
            vm->evt_due[id] = timer_ticks() + vm->evt_param[id];
        }

        vm->reg[0] = 1;
        return 0;
    }

    case VM_SYS_PUTD:
        uart_put_dec(vm->reg[0]);
        return 0;

    case VM_SYS_TICKS:
        vm->reg[0] = timer_ticks();
        return 0;

    case VM_SYS_FILL:
        vp_fill(vm, vm->reg[0], vm->reg[1], vm->reg[2], vm->reg[3],
                (uint16_t)vm->reg[4]);
        vm->yield_now = 1;              /* milliseconds, not instructions */
        return 0;

    case VM_SYS_TEXT: {
        char buf[VM_STR_MAX];
        if (!copy_string(vm, vm->reg[0], buf, sizeof buf)) {
            return 1;                   /* bounds fault already recorded */
        }
        vp_text(vm, vm->reg[1], vm->reg[2], buf,
                (uint16_t)vm->reg[3], (uint16_t)vm->reg[4], vm->reg[5]);
        vm->yield_now = 1;
        return 0;
    }

    case VM_SYS_TOUCH: {
        touch_state_t t;
        touch_latest(&t);

        /* Absolute panel coordinates translated into this viewport, in the
         * offset domain so a touch above or left of the viewport underflows to
         * a huge value and is rejected rather than wrapping into range. */
        uint32_t dx = t.x - vm->vx;
        uint32_t dy = t.y - vm->vy;
        int inside  = t.down && (t.x >= vm->vx) && (t.y >= vm->vy) &&
                      (dx < vm->vw) && (dy < vm->vh);

        if (t.down && !inside) {
            g_touch_withheld++;
        }
        if (inside) {
            g_touch_given++;
        }

        vm->reg[0] = inside ? 1u : 0u;
        vm->reg[1] = inside ? dx : 0u;
        vm->reg[2] = inside ? dy : 0u;
        return 0;
    }

    case VM_SYS_BLIT: {
        uint32_t off = vm->reg[0];
        uint32_t x   = vm->reg[1], y = vm->reg[2];
        uint32_t w   = vm->reg[3], h = vm->reg[4];

        /* Dimensions are bounded BEFORE they are multiplied. w*h*2 on
         * unchecked 32-bit values overflows readily — 65536x65536 wraps to
         * zero, which would pass a byte-length check and then blit whatever
         * followed. This is the first syscall where the length is supplied by
         * the program rather than implied by the operation. */
        if (w == 0u || h == 0u || w > DISP_W || h > DISP_H) {
            vm->reg[0] = 0;
            return 0;
        }

        /* Both checks are now the harness's, and this call IS the model it was
         * generalised from: w and h are bounded above, so `w * h` cannot wrap,
         * and vmarg_items() re-checks that count against the same ceiling
         * before multiplying by the 2-byte element. Alignment is the `2` --
         * pixels are 16-bit and an odd offset would read a shifted image. */
        vm_span_t src;
        if (!vmarg_items(vm, off, w * h, 2u, DISP_W * DISP_H, 2u, &src)) {
            return 1;                   /* harness recorded the fault */
        }

        /* Clip to the viewport before drawing, exactly as vp_fill does. The
         * source stride stays the ORIGINAL width so a clipped blit still walks
         * the caller's rows correctly. */
        if (x >= vm->vw || y >= vm->vh) {
            vm->reg[0] = 0;
            return 0;
        }
        uint32_t cw = (w > vm->vw - x) ? (vm->vw - x) : w;
        uint32_t ch = (h > vm->vh - y) ? (vm->vh - y) : h;

        /* A skipped blit is not a fault: the program asked for something legal
         * and the panel was busy. It returns success with nothing drawn, the
         * same contract as a blit clipped entirely outside the viewport above. */
        if (!display_try_lock()) {
            g_draw_skipped++;
            vm->reg[0] = 0;
            return 0;
        }
        display_blit(vm->vx + x, vm->vy + y, cw, ch,
                     (const uint16_t *)src.ptr, w);
        display_unlock();

        g_blits++;
        vm->reg[0] = 1;
        vm->yield_now = 1;              /* milliseconds, not instructions */
        return 0;
    }

    case VM_SYS_SEND: {
        uint32_t dst = vm->reg[0];
        uint32_t off = vm->reg[1];
        uint32_t len = vm->reg[2];

        /* The message body must lie inside the SENDER's arena. Checked here
         * rather than in ipc.c, because only the VM knows which arena the
         * offset belongs to — ipc.c is handed a kernel pointer and has no way
         * to tell where it came from. */
        if (len == 0u || len > IPC_MSG_MAX || !vm_in_bounds(vm, off, len)) {
            g_ipc_bad_buffer++;
            vm->reg[0] = 0;
            return 0;
        }

        int ok = ipc_send(vm->app_id, (int)dst,
                          (const uint8_t *)(vm->base + off), len);
        vm->reg[0] = (ok == 0) ? 1u : 0u;
        return 0;
    }

    case VM_SYS_RECV: {
        uint32_t off = vm->reg[0];
        uint32_t max = vm->reg[1];

        if (max == 0u || max > IPC_MSG_MAX || !vm_in_bounds(vm, off, max)) {
            g_ipc_bad_buffer++;
            vm->reg[0] = 0;
            vm->reg[1] = 0;
            return 0;
        }

        int from = -1;
        uint32_t n = ipc_recv(vm->app_id, (uint8_t *)(vm->base + off), max, &from);
        vm->reg[0] = n;
        vm->reg[1] = (n > 0u) ? (uint32_t)from : 0u;
        return 0;
    }

    case VM_SYS_DIMS:
        /* Size only. A program is told how much canvas it has, never where on
         * the panel it sits — position is not its business and knowing it would
         * only tempt a producer into absolute coordinates. */
        vm->reg[0] = (vm->vw << 16) | (vm->vh & 0xFFFFu);
        return 0;

    default:
        fault(vm, VM_FAULT_SYSCALL, num);
        return 1;
    }
}

/* ---- dispatch ----------------------------------------------------------- */

int vm_run(vm_t *vm, uint32_t quantum)
{
    if (vm->fault != VM_FAULT_NONE) {
        return VM_RUN_FAULTED;
    }

    while (quantum--) {
        /* Instruction fetch is bounds-checked like any other access. Code and
         * data share the arena, so nothing here may be assumed valid merely
         * because the pc got there by executing. */
        if (!vm_in_bounds(vm, vm->pc, VM_INSN_BYTES) || (vm->pc & 3u)) {
            fault(vm, VM_FAULT_PC, vm->pc);
            return VM_RUN_FAULTED;
        }

        const volatile uint8_t *ins = (const volatile uint8_t *)(vm->base + vm->pc);
        uint8_t  op  = ins[0];
        uint8_t  a   = ins[1];
        uint8_t  b   = ins[2];
        uint8_t  c   = ins[3];
        uint32_t imm = (uint32_t)b | ((uint32_t)c << 8);
        int32_t  simm = (int32_t)(int16_t)imm;

        uint32_t next = vm->pc + VM_INSN_BYTES;

        /* One register-range check for the whole instruction. Operands that an
         * opcode does not use are still indices in a well-formed program, so
         * checking uniformly costs nothing and closes the gap where an unused
         * field carries garbage. Immediate formats exempt b and c. */
        int imm_form = (op == VM_OP_LDI  || op == VM_OP_LDIH || op == VM_OP_ADDI ||
                        op == VM_OP_JMP  || op == VM_OP_BRZ  || op == VM_OP_BRNZ ||
                        op == VM_OP_CALL || op == VM_OP_SYS);
        int off_form = (op == VM_OP_LDW || op == VM_OP_LDB ||
                        op == VM_OP_STW || op == VM_OP_STB);

        if (op != VM_OP_HALT && op != VM_OP_NOP && op != VM_OP_RET) {
            if (a >= VM_REGS) {
                fault(vm, VM_FAULT_REG, a);
                return VM_RUN_FAULTED;
            }
            if (!imm_form && b >= VM_REGS) {
                fault(vm, VM_FAULT_REG, b);
                return VM_RUN_FAULTED;
            }
            if (!imm_form && !off_form && c >= VM_REGS) {
                fault(vm, VM_FAULT_REG, c);
                return VM_RUN_FAULTED;
            }
        }

        uint32_t *r = vm->reg;
        int ok = 1;

        switch (op) {
        case VM_OP_HALT: vm->executed++; return VM_RUN_HALTED;
        case VM_OP_NOP:  break;

        case VM_OP_MOV:  r[a] = r[b]; break;
        case VM_OP_LDI:  r[a] = imm; break;
        case VM_OP_LDIH: r[a] = (r[a] & 0xFFFFu) | (imm << 16); break;

        case VM_OP_ADD:  r[a] = r[b] + r[c]; break;
        case VM_OP_SUB:  r[a] = r[b] - r[c]; break;
        case VM_OP_MUL:  r[a] = r[b] * r[c]; break;

        case VM_OP_DIV:
            if (r[c] == 0u) { fault(vm, VM_FAULT_DIV0, 0); return VM_RUN_FAULTED; }
            r[a] = r[b] / r[c];
            break;
        case VM_OP_MOD:
            if (r[c] == 0u) { fault(vm, VM_FAULT_DIV0, 0); return VM_RUN_FAULTED; }
            r[a] = r[b] % r[c];
            break;

        case VM_OP_AND:  r[a] = r[b] & r[c]; break;
        case VM_OP_OR:   r[a] = r[b] | r[c]; break;
        case VM_OP_XOR:  r[a] = r[b] ^ r[c]; break;

        /* Shift counts are masked to 5 bits. A shift of 32 or more is undefined
         * in C and would let a program's behaviour depend on the host compiler,
         * which is precisely what a VM exists to prevent. */
        case VM_OP_SHL:  r[a] = r[b] << (r[c] & 31u); break;
        case VM_OP_SHR:  r[a] = r[b] >> (r[c] & 31u); break;
        /* NA-003. SAR used to be (int32_t)r[b] >> n, and right-shifting a
         * NEGATIVE signed value is implementation-defined in C -- not
         * undefined, but chosen by the compiler. GCC picks arithmetic and
         * always has, so this was never wrong in practice.
         *
         * It was wrong in principle, and specifically against the claim two
         * lines above: that a VM exists to stop a program's behaviour depending
         * on the host compiler. SAR was the one opcode that did.
         *
         * Built from unsigned operations, which are fully defined. The n == 0
         * case is separate because `0xFFFFFFFF << 32` would itself be the
         * undefined shift this is avoiding. */
        case VM_OP_SAR: {
            uint32_t n = r[c] & 31u;
            uint32_t v = r[b];
            uint32_t sign = ((v & 0x80000000u) && n) ? (0xFFFFFFFFu << (32u - n))
                                                     : 0u;
            r[a] = (v >> n) | sign;
            break;
        }

        case VM_OP_NOT:  r[a] = ~r[b]; break;
        case VM_OP_NEG:  r[a] = (uint32_t)(-(int32_t)r[b]); break;
        case VM_OP_ADDI: r[a] = (uint32_t)((int32_t)r[a] + simm); break;

        case VM_OP_SEQ:  r[a] = (r[b] == r[c]); break;
        case VM_OP_SNE:  r[a] = (r[b] != r[c]); break;
        case VM_OP_SLT:  r[a] = ((int32_t)r[b] <  (int32_t)r[c]); break;
        case VM_OP_SLTU: r[a] = (r[b] <  r[c]); break;
        case VM_OP_SLE:  r[a] = ((int32_t)r[b] <= (int32_t)r[c]); break;
        case VM_OP_SLEU: r[a] = (r[b] <= r[c]); break;

        case VM_OP_JMP:
            next = (uint32_t)((int32_t)next + simm * VM_INSN_BYTES);
            break;
        case VM_OP_BRZ:
            if (r[a] == 0u) { next = (uint32_t)((int32_t)next + simm * VM_INSN_BYTES); }
            break;
        case VM_OP_BRNZ:
            if (r[a] != 0u) { next = (uint32_t)((int32_t)next + simm * VM_INSN_BYTES); }
            break;

        case VM_OP_CALL:
            if (vm->call_sp >= VM_CALL_DEPTH) {
                fault(vm, VM_FAULT_CALL_DEPTH, vm->call_sp);
                return VM_RUN_FAULTED;
            }
            vm->call[vm->call_sp++] = next;
            next = (uint32_t)((int32_t)next + simm * VM_INSN_BYTES);
            break;
        case VM_OP_RET:
            if (vm->call_sp == 0u) {
                fault(vm, VM_FAULT_RET, 0);
                return VM_RUN_FAULTED;
            }
            next = vm->call[--vm->call_sp];

            /* Leaving an event handler.
             *
             * Detected by the return stack coming back to the depth it had
             * before the kernel injected the call, which needs no marker in
             * the stack and no cooperation from the handler -- a handler that
             * simply returns is indistinguishable from any other function that
             * simply returns, which is the point.
             *
             * The register file is restored here, so the interrupted code
             * cannot tell a handler ran. */
            if (vm->in_handler && vm->call_sp == vm->handler_sp) {
                for (uint32_t i = 0; i < VM_REGS; i++) {
                    vm->reg[i] = vm->saved_reg[i];
                }
                vm->in_handler = 0;
            }
            break;

        case VM_OP_LDW: r[a] = load_u32(vm, r[b] + c, &ok); break;
        case VM_OP_LDB: r[a] = load_u8 (vm, r[b] + c, &ok); break;
        case VM_OP_STW: ok = store_u32(vm, r[b] + c, r[a]); break;
        case VM_OP_STB: ok = store_u8 (vm, r[b] + c, r[a]); break;

        case VM_OP_SYS:
            if (do_syscall(vm, imm)) {
                vm->executed++;
                return vm->fault == VM_FAULT_NONE ? VM_RUN_HALTED : VM_RUN_FAULTED;
            }
            /* An expensive syscall ends the slice regardless of how much of the
             * instruction budget is left, so wall-clock time per vm_run() stays
             * bounded by roughly one display operation instead of by the
             * quantum. Without this the host task disappears for minutes and
             * every other application starves. */
            if (vm->yield_now) {
                vm->yield_now = 0;
                vm->pc = next;
                vm->executed++;
                return VM_RUN_QUANTUM;
            }
            break;

        default:
            fault(vm, VM_FAULT_OPCODE, op);
            return VM_RUN_FAULTED;
        }

        if (!ok) {
            return VM_RUN_FAULTED;
        }

        vm->pc = next;
        vm->executed++;
    }

    return VM_RUN_QUANTUM;
}
