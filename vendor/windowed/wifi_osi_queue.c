/* nat-os — the queue poll, compiled WINDOWED.
 *
 * next_moves/08 step 110, fix (2) from step 104.
 *
 * ---- why this file exists ------------------------------------------------
 *
 * `w2c_call3` does `entry a1, 32` and then `callx0`. CALLX0 does not rotate the
 * window, so the call0 callee shares the bridge's register window -- `a1`
 * included -- and moves it to make its own frame. Any window exception while the
 * callee is deep then captures a *call0* stack pointer as the windowed frame's
 * `a1`, and a later underflow walks that link into call0 locals. Step 103 proved
 * it by changing OSI_FOREVER_CAP and watching the fault address track it
 * exactly; step 109 confirmed the register-level symptom was downstream of the
 * same cause.
 *
 * Nothing arranged *around* the bridge fixes what the bridge does -- steps 105,
 * 106 and 108 each removed one instance of the pattern and found another behind
 * it. The bridge has to go.
 *
 * So the poll lives here instead, compiled `-mabi=windowed`. The windowed stub
 * reaches it with a real CALL8: the window rotates, the callee gets its own
 * frame, and no call0 code touches the caller's `a1`.
 *
 * ---- why it is safe to duplicate the layout ------------------------------
 *
 * This file must agree with kernel/wifi_osi_impl.c about `osi_queue_t` and the
 * handle encoding, and nothing but review enforces that. The alternative --
 * compiling the whole of wifi_osi_impl.c windowed -- turns every kernel call it
 * makes (task_sleep, heap_alloc, task_wake) into a CALL8 to a call0 function,
 * which is the bit-31 fault this project has hit four times. Duplicating twenty
 * lines is the smaller risk, and the _Static_assert below turns a size mismatch
 * into a build error rather than a silent one.
 *
 * ---- what it deliberately does NOT do ------------------------------------
 *
 * No waking of blocked senders. `wake_all()` calls `task_wake()`, which is
 * call0, and calling it from here would reintroduce exactly the boundary this
 * file exists to remove. The caller does the wake through a bridge, pinned,
 * where no switch can land in it.
 *
 * No blocking. This is a poll: it either takes an item or reports empty. The
 * waiting happens in `osi_windowed_idle()`, in windowed frames, unpinned.
 */

typedef unsigned int u32;
typedef unsigned char u8;

typedef struct {
    int  used;
    u32  item_size, capacity, head, tail, len;
    u32  waiters_recv, waiters_send;
    u8  *buf;
} osi_queue_t;

_Static_assert(sizeof(osi_queue_t) == 36u,
               "osi_queue_t layout diverged from kernel/wifi_osi_impl.c");

#define OSI_QUEUE_MAX  8u
#define H_TAG          0x05100000u
#define H_MASK         0xFFF00000u
#define H_INDEX(h)     (((u32)(h)) & 0xFFFu)
#define H_OK(h, n)     ((((u32)(h)) & H_MASK) == H_TAG && H_INDEX(h) < (n))

extern osi_queue_t g_queue[OSI_QUEUE_MAX];

/* Returns 1 and fills `item` if a message was taken, 0 if the queue was empty.
 * `*woke` is set non-zero when a sender should be woken -- the caller does that,
 * from call0, while pinned.
 *
 * Interrupts are masked by hand rather than through crit_enter(): that is a
 * static inline in a call0 header, and while it would very likely inline
 * cleanly, "very likely" is not a property this investigation has been well
 * served by. Two instructions here are exact. */
u32 g_qmsg_have, g_qmsg_size;
u8  g_qmsg[8];
int osi_qpoll_w(void *h, void *item, u32 *woke);

int osi_qpoll_w(void *h, void *item, u32 *woke)
{
    u32 ps;
    osi_queue_t *q;
    u32 i, n;
    u8 *src;
    u8 *dst;
    int took = 0;

    *woke = 0u;
    if (!H_OK(h, OSI_QUEUE_MAX) || !item) {
        return 0;
    }
    q = &g_queue[H_INDEX(h)];

    __asm__ volatile ("rsil %0, 3" : "=r"(ps));
    if (q->used && q->len) {
        src = &q->buf[q->head * q->item_size];
        dst = (u8 *)item;
        n   = q->item_size;
        for (i = 0; i < n; i++) { dst[i] = src[i]; }
        /* [step 184] The FIRST message the worker is ever handed.
         * The fault is a jump through a null register in the blob's dispatch,
         * so what it dispatched on is the question. Copy only, no new read of
         * anything the blocking path did not already touch. */
        if (!g_qmsg_have) {
            g_qmsg_have = 1u;
            g_qmsg_size = n;
            for (i = 0; i < 8u && i < n; i++) { g_qmsg[i] = src[i]; }
        }
        q->head = (q->head + 1u) % q->capacity;
        q->len--;
        *woke = q->waiters_send;
        took  = 1;
    }
    __asm__ volatile ("wsr.ps %0; rsync" :: "r"(ps));

    return took;
}
