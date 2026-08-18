/* nat-os — the validated-argument harness. See vmarg.h for why it exists. */

#include "vmarg.h"

static uint32_t g_checks;
static uint32_t g_rejects;

uint32_t vmarg_checks(void)  { return g_checks; }
uint32_t vmarg_rejects(void) { return g_rejects; }

static int reject(vm_t *vm, int code, uint32_t detail)
{
    g_rejects++;
    vm_raise(vm, code, detail);
    return 0;
}

int vmarg_span(vm_t *vm, uint32_t off, uint32_t len, uint32_t align,
               vm_span_t *out)
{
    g_checks++;

    /* Alignment first, because a misaligned offset is wrong regardless of
     * whether it happens to land inside the arena, and reporting ALIGN is more
     * useful than reporting BOUNDS for the same argument. */
    if (align > 1u && (off & (align - 1u)) != 0u) {
        return reject(vm, VM_FAULT_ALIGN, off);
    }

    /* Offset domain. `off + len` would wrap; this cannot. */
    if (!vm_in_bounds(vm, off, len)) {
        return reject(vm, VM_FAULT_BOUNDS, off);
    }

    out->ptr = (const uint8_t *)(vm->base + off);
    out->len = len;
    return 1;
}

int vmarg_items(vm_t *vm, uint32_t off, uint32_t count, uint32_t elem,
                uint32_t max_items, uint32_t align, vm_span_t *out)
{
    g_checks++;

    /* Zero is not a fault -- a service asked for nothing and gets nothing --
     * but it must not become a zero-length span pointing at a valid address,
     * because callers loop on `len`. An empty span is safe to hand back. */
    if (count == 0u || elem == 0u) {
        out->ptr = (const uint8_t *)vm->base;
        out->len = 0;
        return 1;
    }

    /* THE rule this harness exists for. `count` is compared against the
     * service's own ceiling BEFORE any multiplication, so `count * elem` is
     * bounded by construction and cannot wrap. SYS BLIT does this by checking w
     * and h against the panel; every other service now gets it for free. */
    if (count > max_items) {
        return reject(vm, VM_FAULT_BOUNDS, count);
    }

    return vmarg_span(vm, off, count * elem, align, out);
}

int vmarg_string(vm_t *vm, uint32_t off, char *dst, uint32_t max)
{
    g_checks++;

    if (max == 0u) {
        return 1;                       /* nowhere to put it; not a fault */
    }

    /* One bounds-checked byte at a time, COPIED rather than pointed at. The
     * arena belongs to a program that runs again the instant this returns, so a
     * borrowed pointer can change under a renderer mid-glyph. */
    for (uint32_t n = 0; n < max - 1u; n++) {
        if (!vm_in_bounds(vm, off + n, 1u)) {
            return reject(vm, VM_FAULT_BOUNDS, off + n);
        }
        char ch = (char)*(volatile uint8_t *)(vm->base + off + n);
        dst[n] = ch;
        if (ch == 0) {
            return 1;
        }
    }

    /* Truncation is deliberate and is not a fault: the existing contract, kept
     * so porting the current services onto this harness changes nothing an
     * application can observe. */
    dst[max - 1u] = 0;
    return 1;
}

int vmarg_u32(vm_t *vm, uint32_t off, uint32_t *out)
{
    vm_span_t s;
    if (!vmarg_span(vm, off, 4u, 4u, &s)) {
        return 0;
    }
    *out = *(volatile const uint32_t *)s.ptr;
    return 1;
}

int vmarg_store(vm_t *vm, uint32_t off, const void *src, uint32_t len)
{
    g_checks++;

    if (len == 0u) {
        return 1;
    }
    if (!vm_in_bounds(vm, off, len)) {
        return reject(vm, VM_FAULT_BOUNDS, off);
    }

    /* Byte at a time, into the arena. No writable pointer is produced, so
     * there is nothing for a caller to hold on to past the check. */
    const uint8_t *s = (const uint8_t *)src;
    volatile uint8_t *d = (volatile uint8_t *)(vm->base + off);
    for (uint32_t i = 0; i < len; i++) {
        d[i] = s[i];
    }
    return 1;
}
