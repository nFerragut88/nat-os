/* nat-os — MP3 decoding: minimp3 on its own task, in SRAM1, on the FPU.
 *
 * Step 4 of docs/next_moves/11 is only the measurement: can this 80 MHz core
 * decode these files in real time at all? `mp3 bench` decodes N frames and
 * reports the cost in the DECODER TASK'S OWN CYCLES, against how long those
 * frames last. Wall-clock would fold in the ~75% of the CPU the display task
 * currently takes (step 3c) and answer a different question.
 *
 * ---- three things this file is the only owner of ---------------------------
 *
 *   SRAM1 (0x3FFF1000, 60 KB, linker.ld). Decoder state, scratch, buffers and
 *   the task's stack all live there, so the decoder costs the heap nothing.
 *
 *   The FPU. CPENABLE is set once, by the decoder task. nat-os does NOT save
 *   FPU registers on a task switch, which is only correct while one task uses
 *   them, so build.ps1 fails any build in which an object other than mp3.c
 *   contains an FPU instruction.
 *
 *   __divsf3. See mp3.c for why it is here and what it does not handle.
 */

#ifndef NATOS_MP3_H
#define NATOS_MP3_H

void mp3_shell(char *arg);

#endif /* NATOS_MP3_H */
