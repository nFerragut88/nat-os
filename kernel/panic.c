/* nat-os — fault reporting.
 *
 * Entered from the exception vectors when something the kernel did not plan
 * for happens. Until a JTAG probe is available this is the only way a fault
 * says anything at all; without it, a bad pointer or illegal instruction
 * produces a silent reset and no evidence.
 *
 * Runs on a dedicated stack (see vectors.S) because the faulting stack may be
 * the reason we are here. Uses only uart_*, which touch nothing but hardware
 * registers and are safe in this state.
 *
 * Never returns.
 */

#include "panic.h"
#include "board.h"
#include "uart.h"
#include "window.h"
#include "watchdog.h"
#include "store.h"
#include "display.h"
#include "flash.h"
#include "critical.h"
#include "xtensa.h"
#include "task.h"
#include "wifi_osi_table.h"

static int g_record_rc = -99;

/* [0] set by _handler_double, [1] the first fault's EPC1. See vectors.S. */
volatile uint32_t g_panic_double[2];

/* NA-007. How many times the panic path has been entered.
 *
 * The handler does the two least reliable things in the kernel, in order: it
 * writes flash, then it drives a display peripheral this file's own comment
 * describes as "a peripheral whose state nobody has verified" -- all while the
 * system is by definition in an unknown state. A fault in either is not a
 * remote possibility, it is the expected failure.
 *
 * Without a guard, that second fault re-enters through the vector, which resets
 * a1 to _panic_stack_top and calls straight back in. It does not overflow -- it
 * does something quieter and worse. store_record_fault() runs again and
 * OVERWRITES the record of the original fault with the one the panic handler
 * caused. On a board with no serial cable attached, that record is the only
 * evidence that survives to the next boot, so the effect is to replace the
 * cause with its own consequence and report it confidently. */
static int g_panic_depth;

/* Set by the shell's `nestfault`. See halt_forever(). */
volatile int g_panic_nest_test;

/* Second entry. Deliberately does nothing that could fault a third time: no
 * flash, no display, no scheduler -- one string and a spin. */
static void panic_nested(void)
{
    uart_puts("\n\n*** PANIC DURING PANIC ***\n");
    uart_puts("  a fault occurred inside the panic handler.\n");
    uart_puts("  the FIRST fault's record is preserved and was not overwritten;\n");
    uart_puts("  the next boot will report that one. this one is not saved.\n");
    watchdog_disarm();
    for (;;) {
    }
}

/* Shared prologue for both entry points.
 *
 * NA-008. Masks interrupts, which the handler never did. Both callers today are
 * safe by accident rather than by construction: kernel_panic() arrives through a
 * vector that has already raised PS.INTLEVEL, and task.c's stack-guard call sits
 * inside task_schedule(), which runs from the tick ISR. kernel_panic_msg() is a
 * general-purpose entry point in a public header whose contract says nothing
 * about interrupt context.
 *
 * Called from an ordinary task with interrupts on, the old code would print
 * "halted", disarm the watchdog, and then spin -- with the tick still firing, so
 * the scheduler would switch away and every other task would carry on running,
 * unrecoverably, behind a screen claiming the kernel had stopped. Masking here
 * makes "does not return" true for every caller rather than for the two that
 * happen to exist. */
static void panic_prologue(void)
{
    xt_set_intlevel(CRIT_LEVEL);
    if (++g_panic_depth > 1) {
        panic_nested();             /* never returns */
    }
}

/* What to put on the panel, filled in by whichever entry point ran. Statics
 * rather than parameters because halt_forever() is shared and the two entry
 * points describe a fault differently. */
static const char *g_panic_what = "unknown";
static unsigned int g_panic_a, g_panic_b;
static int g_panic_has_b;

/* Puts the fault on the panel.
 *
 * The device is standalone. Everything else in this file assumes a serial cable
 * that is usually not attached, and UM-NATOS-018's record only answers the
 * question on the NEXT boot. Between the fault and that reboot the user is
 * looking at a frozen screen with no indication anything is wrong — which is
 * indistinguishable from the renderer having simply stopped.
 *
 * Drawn AFTER the UART report, deliberately. The panel is the more elaborate of
 * the two paths: it needs the SPI controller, the flash-mapped font, and a
 * peripheral whose state nobody has verified. If drawing wedges despite the
 * bounds in the driver, the serial report and the flash record have both
 * already happened, and only the least reliable of the three is lost.
 *
 * See below the helper. */

/* Eight hex digits into a caller-supplied buffer. Local to this file because
 * the panic path must not depend on anything it does not have to: no printf,
 * no heap, no shared scratch buffer that something else might be using. */
static void hex8(char *out, unsigned int v)
{
    static const char DIGITS[] = "0123456789abcdef";
    out[0] = '0';
    out[1] = 'x';
    for (int i = 0; i < 8; i++) {
        out[2 + i] = DIGITS[(v >> ((7 - i) * 4)) & 0xFu];
    }
    out[10] = 0;
}

static void panic_screen(void)
{
    char buf[12];

    if (!display_ready()) {
        return;
    }
    display_enter_panic_mode();

    display_clear(COLOR_BLUE);
    /* DISP_W, not a literal. This read `320` -- the panel's HEIGHT -- so the
     * title bar asked for a rectangle a third wider than the screen.
     *
     * It never showed, because display_fill_rect() clips w against DISP_W and
     * quietly drew the right thing. That is exactly why it survived: the only
     * code in this kernel that draws it runs after the system has already
     * failed, so nobody sees it often, and when they do it looks correct.
     *
     * Naming the constant is the fix rather than writing 240, because the two
     * dimensions being confusable is the whole defect. */
    display_fill_rect(0, 0, DISP_W, 20, COLOR_WHITE);
    display_text(6, 6, "KERNEL PANIC", COLOR_BLUE, COLOR_WHITE, 1);

    display_text(6, 36, g_panic_what, COLOR_WHITE, COLOR_BLUE, 1);

    /* Numbers as hex without labels: at this size a label costs more width
     * than it buys, and the two values are positional in the same order the
     * UART report prints them. */
    display_text(6, 56, "cause", COLOR_YELLOW, COLOR_BLUE, 1);
    hex8(buf, g_panic_a);
    display_text(70, 56, buf, COLOR_WHITE, COLOR_BLUE, 1);
    if (g_panic_has_b) {
        display_text(6, 72, "pc", COLOR_YELLOW, COLOR_BLUE, 1);
        hex8(buf, g_panic_b);
        display_text(70, 72, buf, COLOR_WHITE, COLOR_BLUE, 1);
    }

    display_text(6, 104, "halted - reset the board", COLOR_WHITE, COLOR_BLUE, 1);
    display_text(6, 120, "reason is saved; next boot", COLOR_GREY, COLOR_BLUE, 1);
    display_text(6, 136, "will report it over serial", COLOR_GREY, COLOR_BLUE, 1);
}

/* Shared ending for both entry points. Spins rather than resetting so the
 * evidence stays put.
 *
 * The watchdog is disarmed first, and that is a deliberate reversal. It is armed
 * to recover a system that has stopped making progress, and a halted panic looks
 * exactly like one — so without this, the board resets a few seconds in and
 * scrolls away the report this handler exists to produce. A hang the kernel
 * cannot explain should be recovered automatically; a fault it CAN explain
 * should be left for someone to read.
 *
 * Order is by decreasing reliability: flash record (already written by the
 * caller), then UART, then the panel. Each step can only cost the steps after
 * it. */
static void halt_forever(void)
{
    watchdog_disarm();

#if FLASH_ENABLE
    /* The record was already written by the caller. Confirm it on the terminal
     * so the two reports can be compared: if the next boot disagrees with what
     * was printed here, the persistence path is what is wrong, not the fault. */
    uart_puts(g_record_rc == 0 ? "  recorded : yes, the next boot will report this\n"
                               : "  recorded : NO — the fault will be lost\n");
#endif

    uart_puts("\n  halted. reset the board to continue.\n");

    /* NA-007 exerciser. A guard with no way to trigger it is untested code, and
     * this project has a long record of those reporting success. Faults on
     * purpose HERE -- after the record is safely written -- so the guard has to
     * catch it, and the original record has to survive to the next boot, which
     * is the property actually being claimed. */
    if (g_panic_nest_test) {
        g_panic_nest_test = 0;
        uart_puts("  nest test: faulting on purpose inside the panic handler\n");
        *(volatile uint32_t *)0x00000000 = 1u;
        uart_puts("  nest test: THE STORE DID NOT FAULT - test is inconclusive\n");
    }

    /* Report how many bytes the panel actually took.
     *
     * Nobody can query a halted board, and "the screen looks wrong" and "the
     * driver never ran" are indistinguishable by eye. A byte count crossing the
     * wire separates them: roughly 150 KB means a full repaint happened and the
     * question is what was drawn; a number near zero means the panic never
     * reached the panel at all. */
    uint32_t before = display_bytes_written();
    panic_screen();
    uart_puts("  panel    : ");
    uart_put_dec(display_bytes_written() - before);
    uart_puts(" bytes drawn\n");

    for (;;) {
    }
}

void kernel_panic_msg(const char *why, unsigned int detail)
{
    panic_prologue();

#if FLASH_ENABLE
    /* Written BEFORE anything is printed. A handler that reports first and
     * records second loses the record if it dies while reporting, and that is
     * not far-fetched — the UART is the more complicated of the two paths and
     * the system is already in an unknown state. */
    g_record_rc = store_record_fault(STORE_FAULT_GUARD, detail, 0);
#endif

    g_panic_what  = why;
    g_panic_a     = detail;
    g_panic_has_b = 0;

    uart_puts("\n\n*** KERNEL PANIC ***\n\n");
    uart_puts("  reason   : ");
    uart_puts(why);
    uart_puts("\n  detail   : ");
    uart_put_dec(detail);
    uart_puts("\n");
    halt_forever();
}

/* Xtensa EXCCAUSE values worth naming. The rest print as a bare number rather
 * than carrying a table that would be mostly dead weight. */
static const char *cause_name(unsigned int cause)
{
    switch (cause) {
    case 0:  return "IllegalInstruction";
    case 1:  return "Syscall";
    case 2:  return "InstructionFetchError";
    case 3:  return "LoadStoreError";
    case 4:  return "Level1Interrupt";
    case 5:  return "Alloca";
    case 6:  return "IntegerDivideByZero";
    case 8:  return "Privileged";
    case 9:  return "LoadStoreAlignment";
    case 20: return "InstFetchProhibited";
    case 28: return "LoadProhibited";
    case 29: return "StoreProhibited";
    default: return "unknown";
    }
}

void kernel_panic(unsigned int exccause, unsigned int epc, unsigned int ps)
{
    panic_prologue();

#if FLASH_ENABLE
    /* Recorded first, for the reason given in kernel_panic_msg(). */
    g_record_rc = store_record_fault(STORE_FAULT_EXCEPTION, exccause, epc);
#endif

    g_panic_what  = cause_name(exccause);
    g_panic_a     = exccause;
    g_panic_b     = epc;
    g_panic_has_b = 1;

    uart_puts("\n\n*** KERNEL PANIC ***\n");

    uart_puts("  exccause : ");
    uart_put_dec(exccause);
    uart_puts("  (");
    uart_puts(cause_name(exccause));
    uart_puts(")\n");

    uart_puts("  epc      : ");
    uart_put_hex(epc);
    uart_puts(g_panic_double[0] ? "   <- DEPC, the faulting instruction\n"
                                : "   <- faulting instruction\n");
    if (g_panic_double[0]) {
        uart_puts("  DOUBLE EXCEPTION - a fault inside an exception handler.\n");
        uart_puts("  epc1     : ");
        uart_put_hex(g_panic_double[1]);
        uart_puts("   <- what triggered the handler (NOT the culprit)\n");
    }

    uart_puts("  ps       : ");
    uart_put_hex(ps);
    uart_puts("\n");

    /* [X15] What does the CPU actually see at the bridge helpers RIGHT NOW?
     * Three benign instructions (mov.n, s32i.n, retw.n) cannot raise
     * IllegalInstruction under any PS state, yet the fault PCs land on them.
     * Either runtime bytes differ from the linked image, or they do not.
     * Dump both helpers and compare offline against objdump of this exact
     * build. 12 words covers w2c_call1 (0x24 bytes) fully and w2c_call2
     * past its retw.n. */
    {
        extern uint32_t w2c_call1(uint32_t fn, uint32_t a);
        extern uint32_t w2c_call2(uint32_t fn, uint32_t a, uint32_t b);
        const volatile uint32_t *p;
        int i;
        p = (const volatile uint32_t *)(uintptr_t)&w2c_call1;
        uart_puts("  exec-bytes1: ");
        for (i = 0; i < 12; i++) { uart_put_hex(p[i]); uart_puts(" "); }
        uart_puts("\n");
        p = (const volatile uint32_t *)(uintptr_t)&w2c_call2;
        uart_puts("  exec-bytes2: ");
        for (i = 0; i < 12; i++) { uart_put_hex(p[i]); uart_puts(" "); }
        uart_puts("\n");
    }

    /* EXCVADDR: the address the faulting instruction tried to reach.
     *
     * For exccause 3, 9, 28 and 29 the interesting question is never "where
     * was the code" -- epc answers that -- but "what did it touch". Without
     * this, a LoadStoreError inside a vendor blob is a bare epc with no way to
     * tell a byte access to rodata apart from a wild pointer. Still valid at
     * this point because nothing between the vector and here writes it. */
    if (exccause == 3u || exccause == 9u || exccause == 28u || exccause == 29u) {
        uint32_t va;
        __asm__ volatile ("rsr.excvaddr %0" : "=r"(va));
        uart_puts("  excvaddr : ");
        uart_put_hex(va);
        uart_puts("   <- the address it tried to reach\n");
    }

    /* IllegalInstruction on a windowed kernel is nearly always the register
     * window, and an epc alone cannot tell WHICH way. WINDOWBASE and
     * WINDOWSTART are the two registers that decide whether an `entry` or a
     * `retw` is legal, and the call0 handler between the fault and here does
     * not touch either -- so they still read as they did at the fault. */
    /* UNCONDITIONAL. Gating this on a cause list has now hidden a changed fault
     * twice -- step 38 (exccause 2 arrived, the block was gated on 0) and again
     * at step 53 (exccause 28 arrived, the block was gated on 0 and 2). Both
     * times the panic printed less than it knew and the run read as unchanged.
     * The lesson was written down after the first one and not applied. */
    {
        uint32_t wb, ws;
        __asm__ volatile ("rsr.windowbase  %0" : "=r"(wb));
        __asm__ volatile ("rsr.windowstart %0" : "=r"(ws));
        uart_puts("  windowbase: ");
        uart_put_dec(wb);
        uart_puts("   windowstart: ");
        uart_put_hex(ws);
        uart_puts("   bit(base) ");
        uart_puts(((ws >> (wb & 31u)) & 1u) ? "SET\n" : "CLEAR\n");
        extern volatile uint32_t g_win_a0, g_win_sp;
        uart_puts("  a0/sp out : ");
        uart_put_hex(g_win_a0);
        uart_puts(" / ");
        uart_put_hex(g_win_sp);
        uart_puts(((g_win_a0 | g_win_sp) == 0u) ? "   BOTH ZERO -- context clobbered\n"
                                                : "   non-zero -- context survived\n");
        /* [X16] PS read inside w2c_call2 a few instructions before the
         * trapping retw.n. bit18 = WOE, bit4 = EXCM, bit5 = UM. */
        {
            extern volatile uint32_t g_win_ps[3];
            uint32_t wps = g_win_ps[0], wws = g_win_ps[1], wwb = g_win_ps[2];
            uart_puts("  win-exit-ps: ");
            uart_put_hex(wps);
            uart_puts("   bit18(WOE) ");
            uart_puts(((wps >> 18) & 1u) ? "SET" : "CLEAR");
            uart_puts("  bit4(EXCM) ");
            uart_puts(((wps >> 4) & 1u) ? "SET\n" : "CLEAR\n");
            /* [X17] window state in the same instant. caller-bit checks the
             * frame one below base: if CLEAR, this retw.n needs an underflow
             * refill, and whatever answers that refill is our suspect. */
            uart_puts("  win-exit-ws: ");
            uart_put_hex(wws);
            uart_puts("  wb ");
            uart_put_dec(wwb);
            uart_puts("  caller-bit ");
            uart_puts(((wws >> ((wwb + 15u) & 15u)) & 1u) ? "SET\n" : "CLEAR\n");
            /* [X18] entry-side state of the most recent crossing + counter. */
            extern volatile uint32_t g_win_in[3];
            extern volatile uint32_t g_win_seq;
            uart_puts("  win-seq    : ");
            uart_put_dec(g_win_seq);
            uart_puts("  in-ps ");
            uart_put_hex(g_win_in[0]);
            uart_puts("  in-ws ");
            uart_put_hex(g_win_in[1]);
            uart_puts("  in-wb ");
            uart_put_dec(g_win_in[2]);
            uart_puts("\n");
            /* [X19] state right before callx0 into the vendor callee. */
            extern volatile uint32_t g_win_mid[3];
            uart_puts("  win-mid    : ps ");
            uart_put_hex(g_win_mid[0]);
            uart_puts("  ws ");
            uart_put_hex(g_win_mid[1]);
            uart_puts("  wb ");
            uart_put_dec(g_win_mid[2]);
            uart_puts("\n");
        }
        /* [X17] exception-machinery registers at panic time: vecbase must be
         * 0x40080000; excsave1 nonzero means another handler ran first; depc
         * nonzero means a previous exception was taken with EXCM set. */
        {
            uint32_t vb, es1, dp;
            __asm__ volatile ("rsr.vecbase %0"  : "=r"(vb));
            __asm__ volatile ("rsr.excsave1 %0" : "=r"(es1));
            __asm__ volatile ("rsr.depc %0"     : "=r"(dp));
            uart_puts("  vecbase: ");
            uart_put_hex(vb);
            uart_puts("  excsave1: ");
            uart_put_hex(es1);
            uart_puts("  depc: ");
            uart_put_hex(dp);
            uart_puts("\n");
        }
        /* The faulting task's saved switch frame, as it sits in memory.
         *
         * This splits the last two possibilities apart. Good values here mean
         * the frame was saved correctly and the damage happens on the way back
         * out (the register restore / window state). Zeros here mean the frame
         * itself was destroyed, and no amount of window bookkeeping would help. */
        {
            uint32_t fsp = task_saved_sp(task_current());
            uart_puts("  saved frame @ ");
            uart_put_hex(fsp);
            uart_puts(":");
            for (int w = 0; w < 8; w++) {
                uart_puts(" ");
                uart_put_hex(((volatile uint32_t *)fsp)[w]);
            }
            /* [X10 experiment] upper half of the interrupted context: a8-a15.
             * With a clean window grant and a clean sweep, a corrupted return
             * chain has to show up HERE -- garbage link registers in the
             * deeper frames of the faulting task's saved view. */
            uart_puts("\n  saved hi  @ ");
            for (int w = 8; w < 16; w++) {
                uart_puts(" ");
                uart_put_hex(((volatile uint32_t *)fsp)[w]);
            }
            /* [X13] the victim's own control state: SAR/EPC3/EPS3/LBEG/LEND/
             * LCOUNT. EPS3 is the ps this task is resumed WITH -- the header
             * 'ps' line is panic-path state and says nothing about the faulting
             * context. EPC3 is where execution resumes; comparing it with the
             * fatal epc shows whether death happened at the frozen resume
             * point or after control wandered there. */
            uart_puts("\n  saved ctl @");
            for (int w = 15; w < 21; w++) {
                uart_puts(" ");
                uart_put_hex(((volatile uint32_t *)fsp)[w]);
            }
            uart_puts("\n");
        }

        {
            extern volatile uint32_t g_woe_lost_ps, g_woe_lost_at;
            extern volatile uint32_t g_woe_prev_hit, g_woe_seen_ok;
            extern volatile uint32_t g_stub_ps_pre_spill;
                {
        extern uint32_t g_a0trace[4];
        {
            extern volatile int g_a0bad_out_task, g_a0bad_in_task;
            extern volatile uint32_t g_a0bad_out_val, g_a0bad_in_val;
            uart_puts("  a0 at save: ");
            if (g_a0bad_out_task < 0) { uart_puts("always a valid address"); }
            else { uart_puts("task "); uart_put_dec((unsigned)g_a0bad_out_task);
                   uart_puts(" saved a0 "); uart_put_hex(g_a0bad_out_val); }
            uart_puts("\n  a0 at rest: ");
            if (g_a0bad_in_task < 0) { uart_puts("always a valid address"); }
            else { uart_puts("task "); uart_put_dec((unsigned)g_a0bad_in_task);
                   uart_puts(" restored a0 "); uart_put_hex(g_a0bad_in_val); }
            uart_puts("\n");
        }

        {
            extern volatile int g_lost_task;
            extern volatile uint32_t g_lost_had, g_lost_grant, g_lost_bits;
            {
                extern volatile int      g_grant_drift_task;
                extern volatile uint32_t g_grant_drift_pred, g_grant_drift_real;
                if (g_grant_drift_task >= 0) {
                    uart_puts("  GRANT DRIFT: task.c predicted ");
                    uart_put_hex(g_grant_drift_pred);
                    uart_puts(" but vectors.S wrote ");
                    uart_put_hex(g_grant_drift_real);
                    uart_puts(" for task ");
                    uart_put_dec((unsigned int)g_grant_drift_task);
                    uart_puts((const char[]){10,0});
                    uart_puts("               the frames line below is computed from the wrong model");
                    uart_puts((const char[]){10,0});
                }
            }
            {
                extern volatile uint32_t g_wsw[8];
                static const char *nm[4] = { "restore", "x20wipe", "phypre ", "phypost" };
                for (int k = 0; k < 4; k++) {
                    uart_puts("  ws write  : ");
                    uart_puts(nm[k]);
                    uart_puts("  n="); uart_put_dec(g_wsw[k * 2]);
                    uart_puts("  last="); uart_put_hex(g_wsw[k * 2 + 1]);
                    uart_puts((const char[]){10,0});
                }
            }
            uart_puts("  frames    : ");
            if (g_lost_task < 0) { uart_puts("no task was ever granted less than it held"); }
            else {
                uart_puts("task "); uart_put_dec((unsigned)g_lost_task);
                uart_puts(" held "); uart_put_hex(g_lost_had);
                uart_puts(" granted "); uart_put_hex(g_lost_grant);
                uart_puts(" LOST "); uart_put_hex(g_lost_bits);
            }
            uart_puts("\n");
        }

        {
            extern volatile int g_term_hit, g_term_by;
            extern volatile uint32_t g_term_was, g_term_now;
            uart_puts("  terminator: ");
            if (g_term_hit < 0) { uart_puts("intact for every task"); }
            else {
                uart_puts("task "); uart_put_dec((unsigned)g_term_hit);
                uart_puts("'s was clobbered while task ");
                uart_put_dec((unsigned)g_term_by);
                uart_puts(" ran:  "); uart_put_hex(g_term_was);
                uart_puts(" -> "); uart_put_hex(g_term_now);
            }
            uart_puts("\n");
        }

        {
            extern volatile uint32_t g_ovlp_seen, g_ovlp_frame, g_ovlp_slot;
            extern volatile int g_ovlp_task;
            uart_puts("  overlap   : ");
            if (!g_ovlp_seen) { uart_puts("no switch frame ever landed on the watched frame"); }
            else {
                uart_puts("task "); uart_put_dec((unsigned)g_ovlp_task);
                uart_puts(" pushed a switch frame at "); uart_put_hex(g_ovlp_frame);
                uart_puts(" across "); uart_put_hex(g_ovlp_slot);
            }
            uart_puts("\n");
        }

        {
            extern volatile uint32_t g_slotwatch[9];
            uart_puts("  slot watch: ");
            if (!g_slotwatch[2]) { uart_puts("[sp+0] never diverged"); }
            else {
                uart_puts("frame "); uart_put_hex(g_slotwatch[5]);
                uart_puts("  stamped "); uart_put_hex(g_slotwatch[3]);
                uart_puts("  came back "); uart_put_hex(g_slotwatch[4]);
                uart_puts("\n              neighbours +4 ");
                uart_put_hex(g_slotwatch[6]);
                uart_puts("  +8 "); uart_put_hex(g_slotwatch[7]);
                uart_puts("  +12 "); uart_put_hex(g_slotwatch[8]);
            }
            uart_puts("\n");
        }

        {
            extern uint32_t g_xseq, g_xring[8][4];
            uart_puts("  retw ring : (newest last)\n");
            for (int k = 7; k >= 0; k--) {
                uint32_t idx = (g_xseq - (uint32_t)k) & 7u;
                if (!g_xring[idx][0]) { continue; }
                uart_puts("     #");
                uart_put_dec(g_xring[idx][0]);
                uart_puts("  a0 ");
                uart_put_hex(g_xring[idx][1]);
                uart_puts("  n=");
                uart_put_dec((g_xring[idx][1] >> 30) & 3u);
                uart_puts("  a1 ");
                uart_put_hex(g_xring[idx][2]);
                uart_puts("  ws ");
                uart_put_hex(g_xring[idx][3]);
                uart_puts("\n");
            }
        }

        uart_puts("  a0 trace  : ");
        if (!g_a0trace[0]) { uart_puts("no illegal a0 latched\n"); }
        else {
            uart_put_hex(g_a0trace[1]);
            uart_puts(" read from frame ");
            uart_put_hex(g_a0trace[2]);
            uart_puts(" at wb ");
            uart_put_dec(g_a0trace[3]);
            uart_puts("\n");
        }
    }

uart_puts("  pre-spill : ps ");
            uart_put_hex(g_stub_ps_pre_spill);
            if (g_stub_ps_pre_spill != 0xFFFFFFFFu) {
                uart_puts((g_stub_ps_pre_spill & 0x10u)
                          ? "   EXCM ALREADY SET before the spill"
                          : "   EXCM clear before the spill");
                uart_puts((g_stub_ps_pre_spill & (1u << 18))
                          ? ", WOE set\n" : ", WOE CLEAR\n");
            } else {
                uart_puts("   blocking path never reached\n");
            }

            extern volatile uint32_t g_stub_sp_pre_spill, g_stub_sp_min;
            uart_puts("  pre-spill : sp ");
            uart_put_hex(g_stub_sp_pre_spill);
            uart_puts("   lowest seen ");
            uart_put_hex(g_stub_sp_min);
            uart_puts("\n");

            {
                uint32_t ua0, ubase;
                __asm__ volatile ("rsr.excsave4 %0" : "=r"(ua0));
                __asm__ volatile ("rsr.excsave5 %0" : "=r"(ubase));
                {
                    /* The overflow probe, read HERE rather than sampled
                     * later: a frame with a bogus pointer takes the
                     * system down inside the handler, so no sample point
                     * downstream ever runs. See step 74. */
                    uint32_t ob, of;
                    __asm__ volatile ("rsr.excsave6 %0" : "=r"(ob));
                    __asm__ volatile ("rsr.excsave7 %0" : "=r"(of));
                    uint32_t pf;
                    __asm__ volatile ("rsr.excsave5 %0" : "=r"(pf));
                    /* [step 126] The overflow probe was removed to make room for
                     * the frame-pointer guard in the same 64-byte slot. excsave5
                     * is still written by the UNDERFLOW handlers, so printing
                     * these under an "overflow" heading would report one
                     * handler's data as another's -- the exact mistake this log
                     * keeps cataloguing. Named for what they now are. */
                    (void)pf; (void)of; (void)ob;
                    uart_puts("  overflow  : probe removed (step 126 guard took the slot)");
                    uart_puts("\n");
                }
                {
                    extern volatile uint32_t g_a12bad[3];
                    uart_puts("  a12 check : ");
                    if (!g_a12bad[0]) { uart_puts("a12 survived every call0 callee"); }
                    else {
                        uart_puts("CLOBBERED -- came back "); uart_put_hex(g_a12bad[1]);
                        uart_puts(" after callee "); uart_put_hex(g_a12bad[2]);
                    }
                    uart_puts("\n");
                }
                {
                    extern volatile uint32_t g_sa_addr, g_sa_after_spill, g_sa_have;
                    uart_puts("  sa watch  : ");
                    if (!g_sa_have) { uart_puts("never sampled"); }
                    else {
                        uint32_t now = ((const uint32_t *)g_sa_addr)[0];
                        uart_puts("@"); uart_put_hex(g_sa_addr);
                        uart_puts("  after spill "); uart_put_hex(g_sa_after_spill);
                        uart_puts("  now "); uart_put_hex(now);
                        uart_puts((g_sa_after_spill == now)
                                  ? "   UNCHANGED -- the spill wrote this"
                                  : "   CHANGED since the spill");
                    }
                    uart_puts("\n");
                }
                {
                    extern volatile uint32_t g_qspill_have, g_qspill_walked, g_qspill_bad;
                    extern volatile uint32_t g_qspill_bad_a0, g_qspill_bad_at, g_qspill_top;
                    {
                        extern volatile uint32_t g_pspill_count, g_pspill_pre_ws;
                        extern volatile uint32_t g_pspill_post_ws, g_pspill_have;
                        extern volatile uint32_t g_pspill_walked, g_pspill_badframes;
                        extern volatile uint32_t g_pspill_bad_a0, g_pspill_bad_at;
                        extern volatile uint32_t g_pspill_sp, g_pspill_wb;
                        {
                            extern volatile uint32_t g_ih_a1_raw, g_ih_a1_calc, g_ih_a1_latched;
                            extern volatile uint32_t g_ih_ws, g_ih_wb, g_ih_bitset;
                            uart_puts("  ih a1     : raw="); uart_put_hex(g_ih_a1_latched);
                            uart_puts(" calc="); uart_put_hex(g_ih_a1_calc);
                            uart_puts(g_ih_a1_latched == g_ih_a1_calc ? "  AGREE" : "  DIFFER");
                            uart_puts("  ws="); uart_put_hex(g_ih_ws);
                            uart_puts(" wb="); uart_put_dec(g_ih_wb);
                            uart_puts(" bit(base)="); uart_put_dec(g_ih_bitset);
                            uart_puts("\n");
                        }
                        uart_puts("  pspill    : sweeps=");
                        uart_put_dec(g_pspill_count);
                        uart_puts(" pre_ws="); uart_put_hex(g_pspill_pre_ws);
                        uart_puts(" post_ws="); uart_put_hex(g_pspill_post_ws);
                        if (g_pspill_have) {
                            uart_puts("\n              from ");
                            uart_put_hex(g_pspill_sp);
                            uart_puts(" wb="); uart_put_dec(g_pspill_wb);
                            uart_puts(" walked "); uart_put_dec(g_pspill_walked);
                            uart_puts(" bad "); uart_put_dec(g_pspill_badframes);
                            if (g_pspill_bad_at) {
                                uart_puts("  first a0 "); uart_put_hex(g_pspill_bad_a0);
                                uart_puts(" at "); uart_put_hex(g_pspill_bad_at);
                            }
                            {
                                extern volatile uint32_t g_pspill_bs_enc, g_pspill_bs_sp;
                                extern volatile uint32_t g_pspill_link, g_pspill_a0slot;
                                uart_puts("\n              [task_sp-16]=");
                                uart_put_hex(g_pspill_a0slot);
                                uart_puts(" [task_sp-12]="); uart_put_hex(g_pspill_link);
                                uart_puts("  borrowed: enc="); uart_put_dec(g_pspill_bs_enc);
                                uart_puts(" sp_like="); uart_put_dec(g_pspill_bs_sp);
                            }
                        } else {
                            uart_puts("  (no sweep audited)");
                        }
                        uart_puts("\n");
                    }
                    uart_puts("  qspill    : ");
                    if (!g_qspill_have) { uart_puts("blocking path never spilled"); }
                    else {
                        uart_puts("from "); uart_put_hex(g_qspill_top);
                        uart_puts(" walked "); uart_put_dec(g_qspill_walked);
                        uart_puts(" frames, "); uart_put_dec(g_qspill_bad);
                        uart_puts(" bad");
                        if (g_qspill_bad_at) {
                            uart_puts("  first a0 "); uart_put_hex(g_qspill_bad_a0);
                            uart_puts(" at "); uart_put_hex(g_qspill_bad_at);
                        }
                    }
                    uart_puts("\n");
                }
                {
                    extern volatile uint32_t g_qr_caller, g_qr_caller_raw;
                    uart_puts("  qr caller : ");
                    if (!g_qr_caller) { uart_puts("never entered"); }
                    else {
                        uart_put_hex(g_qr_caller);
                        uart_puts("  (raw a0 "); uart_put_hex(g_qr_caller_raw);
                        uart_puts(")");
                    }
                    uart_puts("\n");
                }
                {
                    /* [step 96] The WHOLE save area, read HERE.
                     * The vector slot is 64 bytes and could not hold the
                     * extra loads -- it failed to link. It does not need
                     * to: excsave5 already holds the frame address, so
                     * the panic handler can read all four words itself.
                     *
                     * a0 alone could not distinguish "the blob stored
                     * data in a0" from "this memory was never a save
                     * area". A real spilled frame has a plausible stack
                     * pointer at [sp-12]; four words of ordinary data
                     * means it never was one. */
                    uint32_t fa;
                    __asm__ volatile ("rsr.excsave5 %0" : "=r"(fa));
                    if (fa >= 0x3ff00000u && fa < 0x40000000u) {
                        /* [step 101] Two words either side of the save area.
                         *
                         * Step 100 found a value of a0's expected shape two
                         * slots along, which is either a coincidence or a
                         * shifted save area. Widening the read distinguishes
                         * them: if the layout is shifted, the correct a0/a1 pair
                         * appears at a consistent offset; if it is a
                         * coincidence, the neighbours are unremarkable. */
                        const uint32_t *w = (const uint32_t *)(fa - 24u);
                        uart_puts("  uf frame  : @");
                        uart_put_hex(fa);
                        uart_puts("  -24 "); uart_put_hex(w[0]);
                        uart_puts("  -20 "); uart_put_hex(w[1]);
                        uart_puts("\n            a0-16 ");
                        uart_put_hex(w[2]);
                        uart_puts("  a1-12 "); uart_put_hex(w[3]);
                        uart_puts("  a2-8 "); uart_put_hex(w[4]);
                        uart_puts("  a3-4 "); uart_put_hex(w[5]);
                        uart_puts("  +0 "); uart_put_hex(w[6]);
                        uart_puts("  +4 "); uart_put_hex(w[7]);
                        uart_puts("\n");
                    }
                }
                uart_puts("  underflow : recovered a0 ");
                uart_put_hex(ua0);
                uart_puts(" from save area ");
                uart_put_hex(ubase);
                uart_puts("\n");
            }

            {
                extern volatile uint32_t g_uf_bad_a0, g_uf_bad_base, g_uf_bad_when;
                uart_puts("  uf filter : ");
                if (!g_uf_bad_when) {
                    uart_puts("no non-code recovery seen at either side of the spill\n");
                } else {
                    uart_puts("a0 ");
                    uart_put_hex(g_uf_bad_a0);
                    uart_puts(" from ");
                    uart_put_hex(g_uf_bad_base);
                    uart_puts(g_uf_bad_when == 1u ? "  BEFORE the spill\n"
                                                  : "  AFTER the spill\n");
                }
            }

            {
                extern volatile uint32_t g_woe_prev_hit;
                /* The last adapter entry the blob reached. Says WHERE in
                 * esp_wifi_init_internal it was when control left. */
                uart_puts("  last osi  : entry ");
                if (g_woe_prev_hit == 0xFFFFFFFFu) { uart_puts("none"); }
                else {
                    uart_put_dec(g_woe_prev_hit);
                    uart_puts("  ");
                    uart_puts(wifi_osi_name(g_woe_prev_hit));
                }
                uart_puts("\n");
            }

            {
                extern volatile uint32_t g_of_bad_base, g_of_bad_frame, g_of_bad_when;
                uart_puts("  of filter : ");
                if (!g_of_bad_when) { uart_puts("no near-null base recovered"); }
                else {
                    uart_puts("base ");
                    uart_put_hex(g_of_bad_base);
                    uart_puts(" recovered from frame sp ");
                    uart_put_hex(g_of_bad_frame);
                    uart_puts(g_of_bad_when == 1u ? "  BEFORE spill" : "  AFTER spill");
                }
                uart_puts("\n");
            }

            {
                /* [X4 experiment] Window state around the LAST voluntary
                 * block: pre-spill / post-spill / post-wake, overwritten per
                 * excursion. Post-spill must be exactly one live frame; more
                 * means the sweep finished over frames that are not this
                 * task's, whose register slots hold stale stack pointers. */
                extern volatile uint32_t g_blk_ws[3];
                extern volatile uint32_t g_blk_wb[3];
                extern volatile uint32_t g_blk_union;
                uint32_t bits = 0;
                for (uint32_t v = g_blk_ws[1]; v; v >>= 1) { bits += v & 1u; }
                uart_puts("  blk-window: pre ws ");
                uart_put_hex(g_blk_ws[0]);
                uart_puts(" wb ");
                uart_put_dec(g_blk_wb[0]);
                uart_puts(" | spill ws ");
                uart_put_hex(g_blk_ws[1]);
                uart_put_dec(bits);
                uart_puts("b wb ");
                uart_put_dec(g_blk_wb[1]);
                uart_puts(" | wake ws ");
                uart_put_hex(g_blk_ws[2]);
                uart_puts(" wb ");
                uart_put_dec(g_blk_wb[2]);
                uart_puts(" | union ");
                uart_put_hex(g_blk_union);
                uart_puts("\n");
            }

            {
                /* [X5 experiment] Last park point that saw more than one live
                 * frame. If the task named here is not the owner of every bit
                 * in ws, its sweep rotated over somebody else's parked frame
                 * -- the phantom that faults with a stale sp. */
                extern volatile uint32_t g_sbp_ws, g_sbp_wb;
                extern volatile int      g_sbp_task;
                uart_puts("  sbp-last  : ");
                if (g_sbp_task < 0) { uart_puts("no multi-frame park seen"); }
                else {
                    uart_puts("task ");
                    uart_put_dec((unsigned int)g_sbp_task);
                    uart_puts(" wb ");
                    uart_put_dec(g_sbp_wb);
                    uart_puts(" ws ");
                    uart_put_hex(g_sbp_ws);
                }
                uart_puts("\n");
                extern volatile uint32_t g_sbp_skipped;
                uart_puts("  sbp-skip  : ");
                uart_put_dec(g_sbp_skipped);
                uart_puts(" parks swept-skipped (X8 clamp)\n");
                extern volatile uint32_t g_sbp_post_ws, g_sbp_post_wb;
                uart_puts("  sbp-post  : wb ");
                uart_put_dec(g_sbp_post_wb);
                uart_puts(" ws ");
                uart_put_hex(g_sbp_post_ws);
                if (g_sbp_post_ws && !(g_sbp_post_ws & (g_sbp_post_ws - 1u))) {
                    uart_puts("  single-bit ok");
                } else {
                    uart_puts("  SWEEP LEFT MULTI-BIT");
                }
                uart_puts("\n");
            }

            {
                /* [X6 experiment] The last switch-in: what the restore path
                 * actually wrote into WINDOWBASE/WINDOWSTART. If the granted
                 * word already holds foreign bits, the leak is upstream of
                 * the write (saved frame or union); if it is clean, the bits
                 * materialised while a call0-only task was current. */
                extern volatile uint32_t g_rin_seq, g_rin_wb, g_rin_ws;
                extern volatile uint32_t g_rin_verify;
                uart_puts("  switch-in : n ");
                uart_put_dec(g_rin_seq);
                uart_puts(" wb ");
                uart_put_dec(g_rin_wb);
                uart_puts(" ws ");
                uart_put_hex(g_rin_ws);
                uart_puts(" rbck ");
                uart_put_hex(g_rin_verify);
                uart_puts(g_rin_verify == g_rin_ws ? "  commit ok\n" : "  MISMATCH\n");
                /* [X6 experiment] The paired switch-OUT record: the raw
                 * hardware window state of the task that was interrupted,
                 * before bookkeeping narrows it to the per-task mask. */
                extern volatile uint32_t g_rout_seq, g_rout_wb, g_rout_ws;
                uart_puts("  switch-out: n ");
                uart_put_dec(g_rout_seq);
                uart_puts(" wb ");
                uart_put_dec(g_rout_wb);
                uart_puts(" ws ");
                uart_put_hex(g_rout_ws);
                uart_puts("\n");
            }

            {
                /* [X7 experiment] Restore history: for each of the last 8
                 * restores, the value the remapped a3 held at the
                 * wsr.windowstart (junksrc) versus what the readback saw.
                 * Healthy switches where junksrc == intended mask would mean
                 * correctness has been accidental all along. */
                extern volatile uint32_t g_rjunk;
                extern volatile uint32_t g_rin_seq;
                uart_puts("  rst-hist  :");
                for (uint32_t k = 8u; k >= 1u; k--) {
                    if (g_rin_seq < k) { continue; }
                    uint32_t s   = g_rin_seq - k + 1u;
                    uint32_t idx = s & 15u;
                    const volatile uint32_t *e = &g_rjunk + idx * 3u;
                    uart_puts("  ");
                    uart_put_dec(s);
                    uart_puts(":");
                    uart_put_hex(e[1]);
                    uart_puts("/");
                    uart_put_hex(e[2]);
                }
                uart_puts("\n");
            }

            {
                /* [X7 experiment] The last eight ring samples: {seq, task,
                 * wb/ws}. The first entry whose ws carries bits beyond the
                 * single-bit grant timestamps the pollution and names the
                 * task that was current when it happened. */
                extern volatile uint32_t g_ring;
                extern volatile uint32_t g_ring_task;
                extern volatile uint32_t g_rout_seq;
                uart_puts("  win-ring   :");
                for (uint32_t k = 24u; k >= 1u; k--) {
                    if (g_rout_seq < k) { continue; }
                    uint32_t s   = g_rout_seq - k + 1u;
                    uint32_t idx = s & 63u;
                    const volatile uint32_t *e = &g_ring + idx * 3u;
                    uart_puts("  ");
                    uart_put_dec(s);
                    uart_puts("t");
                    uart_put_dec((&g_ring_task)[idx]);
                    uart_puts(":");
                    uart_put_dec(e[1]);
                    uart_puts("/");
                    uart_put_hex(e[2]);
                }
                uart_puts("\n");
            }

            {
                extern volatile int g_badsp_task;
                extern volatile uint32_t g_badsp_val, g_badsp_lo, g_badsp_hi;
                uart_puts("  bad sp    : ");
                if (g_badsp_task < 0) { uart_puts("none -- every saved sp was inside its own stack"); }
                else {
                    uart_puts("task ");
                    uart_put_dec((unsigned int)g_badsp_task);
                    uart_puts(" sp ");
                    uart_put_hex(g_badsp_val);
                    uart_puts(" outside ");
                    uart_put_hex(g_badsp_lo);
                    uart_puts("..");
                    uart_put_hex(g_badsp_hi);
                    {
                        extern volatile uint32_t g_badsp_osi, g_badsp_tick;
                        uart_puts("  at tick ");
                        uart_put_dec(g_badsp_tick);
                        uart_puts(", last osi ");
                        if (g_badsp_osi == 0xFFFFFFFFu) { uart_puts("none yet"); }
                        else {
                            uart_put_dec(g_badsp_osi);
                            uart_puts(" ");
                            uart_puts(wifi_osi_name(g_badsp_osi));
                        }
                    }
                }
                uart_puts("\n");
            }

            {
                extern volatile int g_phytop_task;
                extern volatile uint32_t g_phytop_epc, g_phytop_a0;
                uart_puts("  phytop    : ");
                if (g_phytop_task < 0) { uart_puts("never saved at _phy_stack_top"); }
                else {
                    uart_puts("task ");
                    uart_put_dec((unsigned int)g_phytop_task);
                    uart_puts(" was executing epc ");
                    uart_put_hex(g_phytop_epc);
                    uart_puts(" a0 ");
                    uart_put_hex(g_phytop_a0);
                }
                uart_puts("\n");
            }

            {
                extern volatile int g_ttab_side;
                extern volatile uint32_t g_ttab_lo_seen, g_ttab_hi_seen;
                uart_puts("  ttab fence: ");
                if (g_ttab_side < 0) { uart_puts("intact both sides"); }
                else if (g_ttab_side == 0) {
                    uart_puts("BELOW the table clobbered, value ");
                    uart_put_hex(g_ttab_lo_seen);
                } else {
                    uart_puts("ABOVE the table clobbered, value ");
                    uart_put_hex(g_ttab_hi_seen);
                }
                uart_puts("\n");
            }

            uart_puts("  woe watch : ");
            if (g_woe_lost_at == 0xFFFFFFFFu) {
                uart_puts("never seen clear at an adapter entry");
            } else {
                uart_puts("first clear at osi entry ");
                uart_put_dec(g_woe_lost_at);
                uart_puts(", last good entry ");
                uart_put_dec(g_woe_prev_hit);
                uart_puts(", ps ");
                uart_put_hex(g_woe_lost_ps);
            }
            uart_puts("   good crossings ");
            uart_put_dec(g_woe_seen_ok);
            uart_puts("\n");
        }

        uart_puts("  multiframe: ");
        uart_put_dec(task_multiframe_count());
        uart_puts(" switch-outs with >1 live frame");
        if (task_multiframe_count()) {
            uart_puts(", worst ");
            uart_put_dec(task_multiframe_worst());
            uart_puts(" frames, last task ");
            uart_put_dec((unsigned int)task_multiframe_task());
            uart_puts(" ws ");
            uart_put_hex(task_multiframe_ws());
        }
        uart_puts("\n");

        {
            /* [X11] EXCM contamination watch: see task.c. A nonzero count
             * names the exact switch-out that stored a poisoned resume ps. */
            extern volatile uint32_t g_excm_count, g_excm_seq, g_excm_ps;
            extern volatile int      g_excm_task;
            uart_puts("  excm-watch: ");
            uart_put_dec(g_excm_count);
            uart_puts(" switch-outs with EXCM set in EPS3");
            if (g_excm_count) {
                uart_puts(", last task ");
                uart_put_dec((unsigned int)g_excm_task);
                uart_puts(" seq ");
                uart_put_dec(g_excm_seq);
                uart_puts(" ps ");
                uart_put_hex(g_excm_ps);
            }
            uart_puts("\n");
        }

        {
            /* [X13] WOE-clear watch: see task.c. */
            extern volatile uint32_t g_woec_count, g_woec_seq, g_woec_ps;
            extern volatile int      g_woec_task;
            uart_puts("  woec-watch: ");
            uart_put_dec(g_woec_count);
            uart_puts(" switch-outs with WOE clear in EPS3");
            if (g_woec_count) {
                uart_puts(", last task ");
                uart_put_dec((unsigned int)g_woec_task);
                uart_puts(" seq ");
                uart_put_dec(g_woec_seq);
                uart_puts(" ps ");
                uart_put_hex(g_woec_ps);
            }
            uart_puts("\n");
        }

        {
            /* [X12] .text integrity watch: see task.c. A nonzero count means
             * kernel code bytes changed under the loader's copy -- the trap
             * sites were never the ELF's opcodes at all. */
            extern volatile uint32_t g_txt_bad_ticks, g_txt_seq, g_txt_exp, g_txt_act;
            extern volatile int      g_txt_off;
            uart_puts("  txt-watch : ");
            uart_put_dec(g_txt_bad_ticks);
            uart_puts(" ticks with .text diffs");
            if (g_txt_off >= 0) {
                uart_puts(", first seq ");
                uart_put_dec(g_txt_seq);
                uart_puts(" word ");
                uart_put_dec((unsigned int)g_txt_off);
                uart_puts(" exp ");
                uart_put_hex(g_txt_exp);
                uart_puts(" act ");
                uart_put_hex(g_txt_act);
            }
            uart_puts("\n");
        }

        {
            /* [H1 experiment] ticks that arrived mid-window-handler and were
             * deferred instead of switched. hits > 0 with a surviving wifiinit
             * confirms hypothesis H1; see docs/debug/2026-08-21-*.md */
            extern volatile uint32_t g_tick_excm_hits, g_tick_excm_pc, g_tick_excm_ps;
            uart_puts("  tick-excm : ");
            if (!g_tick_excm_hits) { uart_puts("never deferred"); }
            else {
                uart_put_dec(g_tick_excm_hits);
                uart_puts(" switches deferred, first hit epc ");
                uart_put_hex(g_tick_excm_pc);
                uart_puts(" ps ");
                uart_put_hex(g_tick_excm_ps);
            }
            uart_puts("\n");
        }

        for (int t = 0; t < 12; t++) {
            uint32_t w = 0u, base = task_stack_span(t, &w);
            if (!base) { continue; }
            uart_puts("   task ");
            uart_put_dec((unsigned int)t);
            uart_puts(" sp ");
            uart_put_hex(task_saved_sp(t));
            uart_puts(" stack ");
            uart_put_hex(base);
            uart_puts("+");
            uart_put_dec(w * 4u);
            uart_puts(" win ");
            uart_put_hex(task_win_mask(t));
            uart_puts("@");
            uart_put_dec(task_win_base(t));
            uart_puts(task_stack_intact(t) ? " guard ok\n" : " GUARD BROKEN\n");
        }
    }

    /* How deep the PHY got before dying. Meaningless unless a PHY call was in
     * flight, but when one was, this is the difference between a stack that
     * ran out and a fault that merely happened to land in a spill. */
#if BOARD_HAS_WIFI
    uart_puts("  phystack : ");
    uart_put_dec(phy_stack_used());
    uart_puts(" of ");
    uart_put_dec(phy_stack_size());
    uart_puts(" bytes used\n");
#endif /* BOARD_HAS_WIFI */

    halt_forever();
}
