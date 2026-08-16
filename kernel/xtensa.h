/* nat-os — Xtensa special-register access and interrupt primitives.
 *
 * Thin inline wrappers, no abstraction: kernel code that touches these needs to
 * see exactly which register it is hitting. Special registers are read/written
 * with RSR/WSR, and several require a synchronising instruction afterwards
 * before the effect is architecturally visible:
 *
 *   ISYNC  after changing anything affecting instruction fetch (VECBASE)
 *   RSYNC  after changing PS
 *   ESYNC  after changing INTENABLE
 *
 * Omitting those produces failures that appear several instructions later, so
 * they are baked into the accessors rather than left to call sites.
 */

#ifndef NATOS_XTENSA_H
#define NATOS_XTENSA_H

#include <stdint.h>

/* ---- cycle counter and comparators ---- */

static inline uint32_t xt_ccount(void)
{
    uint32_t v;
    __asm__ volatile ("rsr.ccount %0" : "=a"(v));
    return v;
}

/* Writing CCOMPAREn also acknowledges that comparator's pending interrupt —
 * there is no separate acknowledge step for these. */
static inline void xt_set_ccompare1(uint32_t v)
{
    __asm__ volatile ("wsr.ccompare1 %0; esync" :: "a"(v));
}

static inline uint32_t xt_get_ccompare1(void)
{
    uint32_t v;
    __asm__ volatile ("rsr.ccompare1 %0" : "=a"(v));
    return v;
}

/* ---- interrupt control ---- */

static inline uint32_t xt_get_intenable(void)
{
    uint32_t v;
    __asm__ volatile ("rsr.intenable %0" : "=a"(v));
    return v;
}

static inline void xt_set_intenable(uint32_t v)
{
    __asm__ volatile ("wsr.intenable %0; esync" :: "a"(v));
}

static inline void xt_enable_interrupt(uint32_t bit)
{
    xt_set_intenable(xt_get_intenable() | (1u << bit));
}

static inline void xt_disable_interrupt(uint32_t bit)
{
    xt_set_intenable(xt_get_intenable() & ~(1u << bit));
}

/* Which CPU interrupt lines are asserting RIGHT NOW, regardless of whether they
 * are enabled. A handler must mask this with INTENABLE before acting on it:
 * INTERRUPT reports the hardware's opinion, and a line can be pending for a
 * peripheral this kernel has not enabled and cannot service. */
static inline uint32_t xt_get_interrupt(void)
{
    uint32_t v;
    __asm__ volatile ("rsr.interrupt %0" : "=a"(v));
    return v;
}

/* ---- processor state ---- */

static inline uint32_t xt_get_ps(void)
{
    uint32_t v;
    __asm__ volatile ("rsr.ps %0" : "=a"(v));
    return v;
}

static inline void xt_set_ps(uint32_t v)
{
    __asm__ volatile ("wsr.ps %0; rsync" :: "a"(v));
}

/* PS.INTLEVEL masks interrupts at or below its value. Setting it to 0 admits
 * every level; the other PS bits are preserved because clobbering WOE or UM
 * here would change the execution mode out from under the kernel. */
static inline void xt_set_intlevel(uint32_t level)
{
    xt_set_ps((xt_get_ps() & ~0xFu) | (level & 0xFu));
}

/* ---- vector base ---- */

static inline void xt_set_vecbase(uint32_t base)
{
    __asm__ volatile ("wsr.vecbase %0; isync" :: "a"(base));
}

static inline uint32_t xt_get_vecbase(void)
{
    uint32_t v;
    __asm__ volatile ("rsr.vecbase %0" : "=a"(v));
    return v;
}

/* CCOMPARE1 fires internal interrupt 15, which is a level-3 interrupt on this
 * core. Level 3 has a dedicated vector, so the handler is entered only for this
 * class of event and needs no EXCCAUSE decoding. */
#define XT_TIMER1_INTERRUPT   15u
#define XT_TIMER1_LEVEL        3u

#endif /* NATOS_XTENSA_H */
