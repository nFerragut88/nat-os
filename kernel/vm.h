/* nat-os — bytecode virtual machine.
 *
 * A register machine with 16 general registers and fixed 4-byte instructions.
 * Register-based was chosen over stack-based: dispatch is the inner loop of
 * everything this kernel will run, and a register machine executes a given
 * program in far fewer instructions than a stack machine, which pays for the
 * slightly larger encoding many times over.
 *
 * THE VM IS THE ISOLATION MECHANISM. The ESP32 has no MMU paging, so there is
 * no hardware that can confine an application (UM-NATOS-001 §4.2). Every memory
 * access a program makes is bounds-checked against its arena in software, on
 * every instruction, and that check is not optional and must never be compiled
 * out for speed. If it is removed, this kernel has no isolation of any kind.
 *
 * Two consequences of that, both deliberate:
 *
 *   - Return addresses live in KERNEL memory, not in the arena. A program
 *     cannot reach its own call stack, so it cannot corrupt a return address
 *     however wrong or hostile it is. This is the one structure where being
 *     unable to express something is worth more than the flexibility of a
 *     conventional in-memory stack.
 *
 *   - A faulting program stops the program, never the kernel. Every fault is a
 *     recorded code and a halt, and vm_run() returns normally afterwards.
 *
 * Execution is bounded by an instruction quantum rather than run to completion,
 * so a task hosting a VM can be preempted at an instruction boundary — the
 * safe-boundary preemption model of UM-NATOS-001 §4.2. A program that never
 * terminates costs its quantum and no more.
 */

#ifndef NATOS_VM_H
#define NATOS_VM_H

#include <stdint.h>

#define VM_REGS       16
#define VM_CALL_DEPTH 32       /* kernel-side return stack */
#define VM_INSN_BYTES 4

/* ---- instruction encoding -------------------------------------------------
 *
 *   byte 0   opcode
 *   byte 1   a          destination register, or the only operand
 *   byte 2   b          source register, or immediate low byte
 *   byte 3   c          source register / offset, or immediate high byte
 *
 * R-format reads a, b, c as register indices. I-format reads a as a register
 * and (b, c) as a little-endian 16-bit immediate. A register index of 16 or
 * more is a fault rather than a masked-down index: silently truncating turns a
 * producer bug into wrong answers instead of a diagnosed stop.
 */

enum {
    /* control */
    VM_OP_HALT = 0x00,
    VM_OP_NOP  = 0x01,
    VM_OP_MOV  = 0x02,   /* a <- b                                   */
    VM_OP_LDI  = 0x03,   /* a <- imm16 (zero-extended)               */
    VM_OP_LDIH = 0x04,   /* a <- (a & 0xFFFF) | (imm16 << 16)        */

    /* arithmetic and logic */
    VM_OP_ADD  = 0x10,   /* a <- b + c                               */
    VM_OP_SUB  = 0x11,
    VM_OP_MUL  = 0x12,
    VM_OP_DIV  = 0x13,   /* c == 0 faults                            */
    VM_OP_MOD  = 0x14,   /* c == 0 faults                            */
    VM_OP_AND  = 0x15,
    VM_OP_OR   = 0x16,
    VM_OP_XOR  = 0x17,
    VM_OP_SHL  = 0x18,   /* shift count taken modulo 32              */
    VM_OP_SHR  = 0x19,   /* logical                                  */
    VM_OP_SAR  = 0x1A,   /* arithmetic                               */
    VM_OP_NOT  = 0x1B,   /* a <- ~b                                  */
    VM_OP_NEG  = 0x1C,   /* a <- -b                                  */
    VM_OP_ADDI = 0x1D,   /* a <- a + sign_extend(imm16)              */

    /* comparison — produce 0 or 1 so branches stay simple */
    VM_OP_SEQ  = 0x20,
    VM_OP_SNE  = 0x21,
    VM_OP_SLT  = 0x22,   /* signed                                   */
    VM_OP_SLTU = 0x23,
    VM_OP_SLE  = 0x24,
    VM_OP_SLEU = 0x25,

    /* control flow — immediates count INSTRUCTIONS, relative to the next one */
    VM_OP_JMP  = 0x30,
    VM_OP_BRZ  = 0x31,
    VM_OP_BRNZ = 0x32,
    VM_OP_CALL = 0x33,
    VM_OP_RET  = 0x34,

    /* memory — every access bounds-checked; c is an unsigned byte offset */
    VM_OP_LDW  = 0x40,   /* a <- u32 at [b + c]; must be 4-byte aligned */
    VM_OP_LDB  = 0x41,   /* a <- u8  at [b + c]                        */
    VM_OP_STW  = 0x42,   /* u32 at [b + c] <- a                        */
    VM_OP_STB  = 0x43,   /* u8  at [b + c] <- a                        */

    VM_OP_SYS  = 0x50    /* imm16 selects the service; see below       */
};

/* Syscalls. Arguments in r0..r5, results in r0..r2 -- `text` reads as far as r5
 * and the device transfers read r4, so the r0..r3 this comment used to claim was
 * two registers short of what the interpreter actually reads. */
enum {
    VM_SYS_EXIT  = 0,    /* r0 = status                                */
    VM_SYS_PUTC  = 1,    /* r0 = character                             */
    VM_SYS_PUTS  = 2,    /* r0 = offset of a NUL-terminated string     */
    VM_SYS_PUTD  = 3,    /* r0 = value, printed as unsigned decimal    */
    VM_SYS_TICKS = 4,    /* r0 <- timer_ticks()                        */

    /* Display. Coordinates are VIEWPORT-relative and clipped to it, so an
     * application cannot name a pixel outside its own region — the same
     * property arenas give it for memory. There is no syscall to move or
     * resize a viewport; that is the kernel's to assign. */
    VM_SYS_FILL  = 5,    /* r0=x r1=y r2=w r3=h r4=colour              */
    VM_SYS_TEXT  = 6,    /* r0=str offset r1=x r2=y r3=fg r4=bg r5=scale */
    VM_SYS_DIMS  = 7,    /* r0 <- (width << 16) | height               */

    /* Pointer input, viewport-relative like everything else an application
     * sees. Returns r0 = touched, r1 = x, r2 = y. A touch outside this
     * application's viewport reports as no touch at all: an application must
     * not be able to observe input directed at its neighbours, any more than it
     * can read their memory or draw on their pixels. */
    VM_SYS_TOUCH = 8,    /* r0 <- touched, r1 <- x, r2 <- y            */

    /* Blit an image the application holds in its own arena.
     *   r0 = arena offset of RGB565 pixels, row-major, no padding
     *   r1 = x, r2 = y   (viewport-relative)
     *   r3 = w, r4 = h
     * The first syscall to take a pointer AND a length from the program, so it
     * is the first where the size itself is attacker-controlled. */
    VM_SYS_BLIT  = 9,    /* r0=offset r1=x r2=y r3=w r4=h              */

    /* Messaging. A sender names a destination APPLICATION, never an address.
     *   SEND: r0 = destination id, r1 = arena offset, r2 = length
     *         r0 <- 1 delivered, 0 refused
     *   RECV: r0 = arena offset, r1 = buffer size
     *         r0 <- bytes received (0 if none), r1 <- sender id
     * Messages are copied through a kernel mailbox, so no application ever
     * holds a reference to another's memory. */
    VM_SYS_SEND  = 10,
    VM_SYS_RECV  = 11,

    /* The last hand-written syscall. Everything after this is a device.h table
     * entry rather than an addition to this enum and to vasm.py's SYSCALLS.
     *
     *   r0 = operation (DEV_OP_* in device.h)
     *   0 COUNT                        -> r0 = how many devices
     *   1 NAME  r1=id r2=off r3=max    -> r0 = ok, name written into the arena
     *   2 READ  r1=id r2=chan          -> r0 = ok, r1 = value
     *   3 WRITE r1=id r2=chan r3=value -> r0 = ok
     *   4 INFO  r1=id                  -> r0 = ok, r1 = channels, r2 = flags
     *   5 XFER_OUT r1=id r2=chan r3=off r4=len -> r0 = ok  (arena -> device)
     *   6 XFER_IN  r1=id r2=chan r3=off r4=len -> r0 = ok  (device -> arena)
     *
     * Refusal returns r0 = 0 and is NOT a fault: asking a device something it
     * cannot answer is legal, and a program that cannot enumerate without dying
     * cannot enumerate. Faults stay what they have always been -- reaching
     * outside the arena.
     *
     * A device this application was not GRANTED refuses the same way, and is
     * indistinguishable from one that cannot answer. That is deliberate: the
     * permission bitmap is not something a program can map by probing. */
    VM_SYS_DEVICE = 12,

    /* Register a handler the KERNEL may call.
     *
     *   r0 = event id (VM_EVT_*)
     *   r1 = code offset of the handler, or 0 to unregister
     *   r2 = parameter -- for VM_EVT_TICK, the interval in ticks
     *   -> r0 = 1 accepted, 0 refused
     *
     * This is a VM service, not a peripheral. UM-NATOS-031 says `sys device`
     * is the last hand-written syscall, and that claim was about reaching
     * HARDWARE: a new sensor is a table entry, not an opcode. Registering a
     * handler is the same kind of thing as `exit` or `send` -- it is about the
     * execution model itself, which no device table can express. Said plainly
     * here so the earlier claim is not quietly broken.
     *
     * Until now an application was one linear thing with one program counter,
     * and everything the kernel offered ran the other way: the program asks,
     * the kernel answers. This is the first mechanism that lets the kernel call
     * INTO a program, which is what `when` and `every` both need. */
    VM_SYS_EVENT = 13
};

/* Fault codes. VM_FAULT_NONE is the only non-terminal value. */
enum {
    VM_FAULT_NONE = 0,
    VM_FAULT_OPCODE,        /* unknown opcode                          */
    VM_FAULT_REG,           /* register index >= VM_REGS               */
    VM_FAULT_BOUNDS,        /* access outside the arena                */
    VM_FAULT_ALIGN,         /* misaligned word access                  */
    VM_FAULT_DIV0,
    VM_FAULT_PC,            /* pc outside the arena, or misaligned     */
    VM_FAULT_CALL_DEPTH,    /* call stack exhausted                    */
    VM_FAULT_RET,           /* return with an empty call stack         */
    VM_FAULT_SYSCALL,       /* unknown syscall number                  */
    VM_FAULT_STRING         /* PUTS string unterminated inside arena   */
};

/* Why vm_run() returned. */
enum {
    VM_RUN_HALTED = 0,      /* HALT or SYS_EXIT — program finished     */
    VM_RUN_FAULTED,         /* see vm_fault()                          */
    VM_RUN_QUANTUM          /* budget spent; call vm_run() again       */
};

/* ---- events -------------------------------------------------------------
 *
 * Two to begin with, chosen because they are the two halves of what a physical
 * program actually wants: something happened, and time passed.
 *
 *   VM_EVT_TICK   fires every `param` ticks. This is `every 10ms`.
 *                 The handler receives the current tick count in r0.
 *   VM_EVT_KEY    fires when the panel keypad settles a character. This is
 *                 `when key.pressed`. The handler receives it in r0.
 *
 * The key queue is SHARED and reading it is destructive, so an application
 * registered for VM_EVT_KEY competes with the shell's `keys` command and with
 * any other application doing the same. That is the same arrangement the
 * touchscreen already has and it is a real limitation, not an oversight --
 * per-application queues need a concept of focus, which this system does not
 * have. */
#define VM_EVT_TICK   0u
#define VM_EVT_KEY    1u
#define VM_EVT_COUNT  2u

typedef struct {
    uint32_t reg[VM_REGS];
    uint32_t pc;                        /* byte offset into the arena   */

    /* Return addresses, deliberately outside anything the program can
     * address. See the header comment. */
    uint32_t call[VM_CALL_DEPTH];
    uint32_t call_sp;

    /* Viewport, in panel coordinates. Assigned by the kernel; the program can
     * read its size but never its position, and cannot draw outside it. */
    uint32_t vx, vy, vw, vh;

    int      arena;                     /* owning arena id              */

    /* Which application this VM is. Messaging addresses applications, not
     * arenas or tasks, so the identity has to be here. -1 for a VM the kernel
     * hosts directly, which therefore cannot be sent to. */
    int      app_id;
    uint32_t base;                      /* cached kernel address        */
    uint32_t size;                      /* cached arena length          */

    int      fault;
    uint32_t fault_pc;                  /* pc of the faulting insn      */
    uint32_t fault_detail;              /* offending offset, opcode, …  */

    /* Set by a syscall whose cost is measured in milliseconds rather than
     * instructions. The quantum bounds INSTRUCTIONS; a display fill is one
     * instruction and ~31 ms of bit-banged SPI, so without this a 2,000
     * instruction budget can be minutes of drawing inside a single vm_run()
     * and the preemption guarantee is worthless. */
    int      yield_now;

    /* ---- event handlers -------------------------------------------------
     *
     * Delivery is a CALL injected by the kernel: the current pc is pushed onto
     * the same return stack `call` uses, and pc is set to the handler. The
     * handler's own `ret` therefore resumes the interrupted code with no new
     * return mechanism and no second program counter.
     *
     * Registers are saved on entry and restored when the handler returns,
     * which is detected by the return stack coming back to the depth it had
     * before injection. That means a handler may use every register freely and
     * the interrupted code cannot tell it ran -- the alternative was a calling
     * convention, and a convention nobody can enforce is a convention every
     * program gets wrong differently. */
    uint32_t evt_handler[VM_EVT_COUNT]; /* code offset; 0 = not registered */
    uint32_t evt_param[VM_EVT_COUNT];   /* TICK: interval in ticks         */
    uint32_t evt_due[VM_EVT_COUNT];     /* TICK: next tick it is due       */
    uint32_t evt_arg[VM_EVT_COUNT];     /* argument for a pending event    */
    uint32_t evt_pending;               /* bit per event id                */

    int      in_handler;
    uint32_t handler_sp;                /* call_sp BEFORE the injection    */
    uint32_t saved_reg[VM_REGS];

    uint32_t evt_delivered;
    uint32_t evt_dropped;               /* arrived while one was running   */

    uint32_t executed;                  /* instructions retired         */
    uint32_t exit_status;
} vm_t;

/* Binds a VM to an arena and resets it. Returns 0, or non-zero if the arena id
 * is unknown. Does not load a program — copy one in before running. */
int vm_init(vm_t *vm, int arena_id);

/* Confines this VM's drawing to a rectangle of the panel. vm_init() defaults to
 * the whole panel, which is right for a VM the kernel hosts directly and wrong
 * for an application — app.c narrows it. */
void vm_set_viewport(vm_t *vm, uint32_t x, uint32_t y, uint32_t w, uint32_t h);

/* Binds this VM to an application id, so messages can be addressed to it. */
void vm_set_app_id(vm_t *vm, int id);

/* Runs at most `quantum` instructions. Returns one of VM_RUN_*. Safe to call
 * again after VM_RUN_QUANTUM; calling after HALTED or FAULTED returns the same
 * result without executing anything. */
int vm_run(vm_t *vm, uint32_t quantum);

int      vm_fault(const vm_t *vm);
const char *vm_fault_name(int fault);

/* Exposed so the bounds predicate can be cross-checked against
 * arena_contains() rather than assumed to agree with it. */
int vm_in_bounds(const vm_t *vm, uint32_t off, uint32_t len);

/* Records a fault exactly as the interpreter does. Exported for vmarg.c, which
 * lives outside this file because the device model will share it. */
void vm_raise(vm_t *vm, int code, uint32_t detail);

/* ---- event delivery, called by the application host --------------------
 *
 * vm_event_poll() decides whether anything is due for this vm and marks it
 * pending; vm_event_dispatch() injects the call if one is. They are separate so
 * the host can poll cheaply every tick and dispatch only when it is about to
 * run the program.
 *
 * Both are no-ops for a vm with no handlers registered, which is every
 * application written before this existed. */
void vm_event_poll(vm_t *vm);
int  vm_event_dispatch(vm_t *vm);       /* 1 if a handler was entered */

uint32_t vm_events_delivered(const vm_t *vm);
uint32_t vm_events_dropped(const vm_t *vm);

/* Viewport containment, as numbers rather than as something to look at.
 * escapes must be 0 for any program, however hostile; max_y is the lowest row
 * any application has actually painted and must never reach the panel bottom. */
uint32_t vm_viewport_escapes(void);
uint32_t vm_viewport_max_y(void);
uint32_t vm_viewport_calls(void);

/* Touches handed to applications, and touches withheld for landing outside the
 * asking application's viewport. */
uint32_t vm_touch_given(void);
uint32_t vm_touch_withheld(void);

/* Bitmaps accepted from applications. */
uint32_t vm_blits(void);

/* Drawing primitives dropped because the panel was busy. See vm.c. */
uint32_t vm_draw_skipped(void);

/* The VM's own view of messaging: attempts that never reached ipc.c because
 * the buffer was outside the caller's arena. */
uint32_t vm_ipc_bad_buffer(void);

#endif /* NATOS_VM_H */
