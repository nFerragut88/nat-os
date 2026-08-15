/* cyd-os — bytecode virtual machine. See vm.h for the ISA and the isolation
 * rationale.
 *
 * The dispatch loop is a switch. A computed-goto table would be faster, but it
 * is also the kind of construct where a missing entry is a silent jump rather
 * than a diagnosed fault, and correctness of the bounds checks matters more
 * here than dispatch overhead. Revisit only with a measurement showing dispatch
 * is actually the bottleneck.
 */

#include "vm.h"
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
int vm_in_bounds(const vm_t *vm, uint32_t off, uint32_t len)
{
    if (off > vm->size) {
        return 0;
    }
    return len <= vm->size - off;
}

static void fault(vm_t *vm, int code, uint32_t detail)
{
    vm->fault        = code;
    vm->fault_pc     = vm->pc;
    vm->fault_detail = detail;
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
 * (UM-CYDOS-010 §5.2). Coordinates arrive as uint32_t, so a program passing a
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

    display_fill_rect(ax, ay, w, h, colour);
}

#define VM_STR_MAX 48

/* Copies a NUL-terminated string out of the arena into kernel memory, one
 * bounds-checked byte at a time. The copy is not paranoia: display_text() takes
 * a kernel pointer, and handing it an arena address would let a program's own
 * writes change the string mid-render. A string that runs to the end of the
 * arena without a terminator faults rather than reading past it. */
static int copy_string(vm_t *vm, uint32_t off, char *dst, uint32_t max)
{
    for (uint32_t n = 0; n < max - 1u; n++) {
        int ok = 0;
        uint32_t ch = load_u8(vm, off + n, &ok);
        if (!ok) {
            return 0;                   /* load_u8 recorded the fault */
        }
        dst[n] = (char)ch;
        if (ch == 0u) {
            return 1;
        }
    }
    dst[max - 1u] = 0;
    return 1;                           /* truncated, not a fault */
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
    display_text(vm->vx + x, vm->vy + y, buf, fg, bg, scale);
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

        /* Pixels are 16-bit; an odd offset would mean unaligned loads. Faults
         * rather than silently reading a shifted image. */
        if (off & 1u) {
            fault(vm, VM_FAULT_ALIGN, off);
            return 1;
        }

        uint32_t bytes = w * h * 2u;            /* <= 240*320*2, cannot wrap */
        if (!vm_in_bounds(vm, off, bytes)) {
            fault(vm, VM_FAULT_BOUNDS, off);
            return 1;
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

        display_blit(vm->vx + x, vm->vy + y, cw, ch,
                     (const uint16_t *)(vm->base + off), w);

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
        case VM_OP_SAR:  r[a] = (uint32_t)((int32_t)r[b] >> (r[c] & 31u)); break;

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
