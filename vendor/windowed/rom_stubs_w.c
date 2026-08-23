/* nat-os -- ROM newlib syscall stub table.  WINDOWED.
 *
 * [step 186] The ESP32 mask ROM contains a newlib -- malloc, printf, the string
 * and float routines -- but not the operating system those routines need. Every
 * ROM entry point that touches libc state reaches through a table of callbacks
 * the runtime installs at
 *
 *     syscall_table_ptr_pro = 0x3FFAE024      (and _app at 0x3FFAE020)
 *
 * nat-os never wrote either. Measured, both read 0x00000000, and ROM
 * __getreent -- the first thing ROM malloc calls -- dereferenced it:
 *
 *     exccause 28  LoadProhibited   epc 0x4000be94  (__getreent + 0x8)
 *     excvaddr 0x00000000
 *
 * That is the fault the WiFi worker hit after _malloc_internal, and it was not
 * the blob's doing: the blob called a ROM routine, and the ROM routine reached
 * for an operating system that was not there.
 *
 * WHY THIS FILE IS IN vendor/windowed.
 *
 * The first attempt put it in kernel/, which builds -mabi=call0, and it failed
 * in a way worth recording: ROM is windowed, so it calls these entries with
 * CALL8, which writes the return address into a8 and leaves the rotation to the
 * callee's ENTRY. A call0 function has no ENTRY, so nothing rotates and the
 * function returns through an a0 the caller never set:
 *
 *     exccause 2  InstructionFetchError   epc 0x3ffb2770   (task 9's stack)
 *
 * The boundary is by file in this project, so the stubs live here and reach the
 * kernel through the w2c_* bridges, exactly as the OS adapter stubs do.
 *
 * SCOPE. The minimum that makes ROM libc safe to enter, not a newlib port. The
 * allocator entries are real and route to nat-os's heap; __getreent hands back a
 * writable zeroed block so ROM code has somewhere to put errno; the lock family
 * is deliberately empty; everything else refuses. Nothing is NULL, which is the
 * whole point -- see the guard in kernel/rom_stubs.c.
 *
 * Field order is copied from ESP-IDF's
 * components/esp_rom/include/esp32/rom/libc_stubs.h, struct syscall_stub_table.
 * The order is load-bearing: wrong order sends ROM code to the wrong function
 * with the wrong arguments. The _Static_assert on the size is a weak check on
 * that and is not a substitute for reading the header.
 */

#include <stdint.h>
#include <stddef.h>

#include "window.h"

extern void heap_alloc(void);          /* call0; address only, see w2c_call1 */
extern void heap_free(void);
extern void kernel_panic_msg(void);

/* Every entry is declared void * rather than its true signature. Nothing in
 * nat-os calls these; only ROM does, and ROM knows the signatures. Declaring
 * them honestly would drag newlib's struct _reent, FILE, va_list and struct tms
 * into a file that needs none of them. The comment at each assignment names the
 * real shape. */
struct rom_stub_table {
    void *__getreent;
    void *_malloc_r;
    void *_free_r;
    void *_realloc_r;
    void *_calloc_r;
    void *_abort;
    void *_system_r;
    void *_rename_r;
    void *_times_r;
    void *_gettimeofday_r;
    void *_raise_r;
    void *_unlink_r;
    void *_link_r;
    void *_stat_r;
    void *_fstat_r;
    void *_sbrk_r;
    void *_getpid_r;
    void *_kill_r;
    void *_exit_r;
    void *_close_r;
    void *_open_r;
    void *_write_r;
    void *_lseek_r;
    void *_read_r;
    void *_lock_init;
    void *_lock_init_recursive;
    void *_lock_close;
    void *_lock_close_recursive;
    void *_lock_acquire;
    void *_lock_acquire_recursive;
    void *_lock_try_acquire;
    void *_lock_try_acquire_recursive;
    void *_lock_release;
    void *_lock_release_recursive;
    void *_printf_float;
    void *_scanf_float;
};

_Static_assert(sizeof(struct rom_stub_table) == 36u * 4u,
               "rom_stub_table must match libc_stubs.h entry for entry");

/* ROM's idea of per-thread libc state.
 *
 * newlib's struct _reent is a few hundred bytes and its layout is a newlib
 * build detail. ROM code reaching it here does so to record errno and to look
 * for stdio pointers, and everything it finds is zero. One shared block is
 * correct for as long as exactly one context is inside ROM libc at a time,
 * which the blob mutex already guarantees for blob calls. It is NOT per-task
 * and is not thread-safe if that ever stops being true. */
static uint32_t g_rom_reent[128];       /* 512 B, zeroed, 4-byte aligned */

static void *stub_getreent(void)
{
    return (void *)g_rom_reent;
}

/* The allocators. `r` is the reent pointer and is ignored: nat-os has one heap
 * and no per-thread allocator state. */
static void *stub_malloc_r(void *r, size_t n)
{
    (void)r;
    return (void *)w2c_call1((uint32_t)&heap_alloc, (uint32_t)(n ? n : 1u));
}

static void stub_free_r(void *r, void *p)
{
    (void)r;
    (void)w2c_call1((uint32_t)&heap_free, (uint32_t)p);
}

static void *stub_calloc_r(void *r, size_t count, size_t size)
{
    (void)r;
    uint32_t n = (uint32_t)count * (uint32_t)size;
    uint8_t *p = (uint8_t *)w2c_call1((uint32_t)&heap_alloc,
                                      (uint32_t)(n ? n : 1u));
    if (p) {
        for (uint32_t i = 0; i < n; i++) { p[i] = 0; }
    }
    return p;
}

/* Refused, not faked. nat-os's heap has no realloc and the old size is not
 * recoverable from the pointer, so there is no way to copy the right number of
 * bytes. NULL is a failure the caller can see; a fresh block with
 * uninitialised contents is corruption it cannot. */
static void *stub_realloc_r(void *r, void *p, size_t n)
{
    (void)r; (void)p; (void)n;
    return 0;
}

static void stub_abort(void)
{
    (void)w2c_call2((uint32_t)&kernel_panic_msg,
                    (uint32_t)"ROM libc called abort()", 0u);
}

/* The lock family. Empty on purpose.
 *
 * ROM libc takes these around its own critical sections. nat-os already
 * serialises every path that can reach ROM libc -- blob calls hold the blob
 * mutex -- so there is nothing for them to protect, and making them real would
 * mean a call0 excursion that can block from inside ROM code, the condition
 * step 104 identified as unsafe. They must exist and must do nothing. */
static void stub_lock_noop(void *lock)
{
    (void)lock;
}

static int stub_lock_try(void *lock)
{
    (void)lock;
    return 0;                       /* 0 == acquired, newlib convention */
}

/* Everything with no meaning here. Non-NULL so a ROM path that reaches one gets
 * a refusal instead of a jump to address zero -- the fault this file removes. */
static int stub_enosys(void)
{
    return -1;
}

static void stub_void(void)
{
}

struct rom_stub_table g_rom_stubs = {
    .__getreent      = (void *)&stub_getreent,   /* struct _reent *(void)         */
    ._malloc_r       = (void *)&stub_malloc_r,   /* void *(reent, size_t)         */
    ._free_r         = (void *)&stub_free_r,     /* void  (reent, void *)         */
    ._realloc_r      = (void *)&stub_realloc_r,  /* void *(reent, void *, size_t) */
    ._calloc_r       = (void *)&stub_calloc_r,   /* void *(reent, size_t, size_t) */
    ._abort          = (void *)&stub_abort,      /* void  (void)                  */
    ._system_r       = (void *)&stub_enosys,
    ._rename_r       = (void *)&stub_enosys,
    ._times_r        = (void *)&stub_enosys,
    ._gettimeofday_r = (void *)&stub_enosys,
    ._raise_r        = (void *)&stub_void,
    ._unlink_r       = (void *)&stub_enosys,
    ._link_r         = (void *)&stub_enosys,
    ._stat_r         = (void *)&stub_enosys,
    ._fstat_r        = (void *)&stub_enosys,
    ._sbrk_r         = (void *)&stub_enosys,
    ._getpid_r       = (void *)&stub_enosys,
    ._kill_r         = (void *)&stub_enosys,
    ._exit_r         = (void *)&stub_void,
    ._close_r        = (void *)&stub_enosys,
    ._open_r         = (void *)&stub_enosys,
    ._write_r        = (void *)&stub_enosys,
    ._lseek_r        = (void *)&stub_enosys,
    ._read_r         = (void *)&stub_enosys,

    ._lock_init                  = (void *)&stub_lock_noop,
    ._lock_init_recursive        = (void *)&stub_lock_noop,
    ._lock_close                 = (void *)&stub_lock_noop,
    ._lock_close_recursive       = (void *)&stub_lock_noop,
    ._lock_acquire               = (void *)&stub_lock_noop,
    ._lock_acquire_recursive     = (void *)&stub_lock_noop,
    ._lock_try_acquire           = (void *)&stub_lock_try,
    ._lock_try_acquire_recursive = (void *)&stub_lock_try,
    ._lock_release               = (void *)&stub_lock_noop,
    ._lock_release_recursive     = (void *)&stub_lock_noop,

    ._printf_float   = (void *)&stub_enosys,
    ._scanf_float    = (void *)&stub_enosys,
};

/* A DATA symbol, not a function.
 *
 * This was a function, and kernel/rom_stubs.c -- which is call0 -- called it
 * directly. Windowed callee, call0 caller: the entry executes with a0 holding a
 * call0 return address and the window protocol never started.
 *
 *     exccause 0  IllegalInstruction   epc 0x4008de55  (rom_stub_words)
 *
 * The same boundary that sent the first attempt at this table into task 9's
 * stack, crossed the other way, in the file written to fix it. A word of data
 * needs no ABI at all. */
const uint32_t g_rom_stub_words = (uint32_t)(sizeof(g_rom_stubs) / 4u);
