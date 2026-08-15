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
    return 0;
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
