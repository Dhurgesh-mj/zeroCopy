/*
 * zerocopy_futex.h — Futex-backed hybrid sleep/wake strategy for zero-copy ring
 *
 * Same ring, same API shape as zerocopy.h, but after ZC_SPIN_LIMIT failed
 * spin iterations the waiter parks in the kernel via futex(2) instead of
 * burning a core.
 *
 * Futex words:
 *   The futex syscall operates on 32-bit words, while our counters are
 *   64-bit. Instead of punning/casting the 64-bit counter (endianness- and
 *   aliasing-fragile), each cache line carries a dedicated uint32_t word
 *   (head_futex / tail_futex, see zc_control_t) that mirrors the low 32
 *   bits of its counter. Only the line's owner ever stores it; the peer
 *   only passes its address to the futex syscall, deriving the `expected`
 *   value from its own last 64-bit counter load — never C-loading the
 *   remote word (that would be a data race).
 *
 *   The mirror store is sequenced after the counter release-store and
 *   before FUTEX_WAKE, so a waiter either sees the new counter (no wait
 *   needed) or the kernel's word-vs-expected check fails with EAGAIN and
 *   the loop re-reads the counter. No wakeup can be lost.
 *
 * Bounded waits:
 *   FUTEX_WAIT is given a short timeout (ZC_FUTEX_WAIT_MS) so a parked
 *   thread periodically re-checks the counter in userspace. This bounds
 *   kernel sleep time and lets applications layer stall/peer-death
 *   detection on top. Peer-death handling itself is the application's
 *   job (see the stall watchdog in the demo consumers).
 */

#ifndef ZEROCOPY_FUTEX_H
#define ZEROCOPY_FUTEX_H

#include "zerocopy.h"
#include <linux/futex.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#ifndef ZC_SPIN_LIMIT
#define ZC_SPIN_LIMIT 1000
#endif

/* Max time parked in the kernel per FUTEX_WAIT call. */
#ifndef ZC_FUTEX_WAIT_MS
#define ZC_FUTEX_WAIT_MS 100
#endif

static inline long zc_futex_wait(uint32_t *addr, uint32_t expected)
{
    struct timespec ts;
    ts.tv_sec  = ZC_FUTEX_WAIT_MS / 1000;
    ts.tv_nsec = (long)(ZC_FUTEX_WAIT_MS % 1000) * 1000000L;
    return syscall(SYS_futex, addr, FUTEX_WAIT, expected, &ts, NULL, 0);
}

static inline long zc_futex_wake(uint32_t *addr)
{
    return syscall(SYS_futex, addr, FUTEX_WAKE, 1, NULL, NULL, 0);
}

/*
 * zc_futex_acquire — Spin until a slot is available, with futex sleep fallback.
 */
static inline void *zc_futex_acquire(zc_handle_t *h)
{
    const uint64_t head = atomic_load_explicit(&h->ring->ctl.head,
                                                memory_order_relaxed);

    if (__builtin_expect((head - h->cached) < ZC_SLOT_COUNT, 1))
        goto slot;

    uint32_t spins = 0;
    for (;;) {
        h->cached = atomic_load_explicit(&h->ring->ctl.tail,
                                          memory_order_acquire);
        if ((head - h->cached) < ZC_SLOT_COUNT)
            break;

        if (++spins >= ZC_SPIN_LIMIT) {
            /* Wait on the consumer-owned word; expected = low 32 bits of
             * the last tail we observed. Stale expectation → EAGAIN →
             * loop re-reads the counter. */
            zc_futex_wait(&h->ring->ctl.tail_futex, (uint32_t)h->cached);
            spins = 0;
        } else {
            ZC_CPU_PAUSE();
        }
    }

slot:
    return (void *)&h->ring->slots[head & ZC_SLOT_MASK][0];
}

/*
 * zc_futex_publish — Make the written slot visible to the consumer and wake.
 */
static inline void zc_futex_publish(zc_handle_t *h)
{
    const uint64_t head = atomic_load_explicit(&h->ring->ctl.head,
                                                memory_order_relaxed);
    atomic_store_explicit(&h->ring->ctl.head, head + 1,
                           memory_order_release);

    /* Mirror AFTER the counter store, BEFORE the wake (see header note). */
    h->ring->ctl.head_futex = (uint32_t)(head + 1);
    zc_futex_wake(&h->ring->ctl.head_futex);
}

/*
 * zc_futex_consume — Spin until data arrives, with futex sleep fallback.
 */
static inline void *zc_futex_consume(zc_handle_t *h)
{
    const uint64_t tail = atomic_load_explicit(&h->ring->ctl.tail,
                                                memory_order_relaxed);

    if (__builtin_expect(h->cached != tail, 1))
        goto slot;

    uint32_t spins = 0;
    for (;;) {
        h->cached = atomic_load_explicit(&h->ring->ctl.head,
                                          memory_order_acquire);
        if (h->cached != tail)
            break;

        if (++spins >= ZC_SPIN_LIMIT) {
            /* Wait on the producer-owned word; expected = low 32 bits of
             * the last head we observed. */
            zc_futex_wait(&h->ring->ctl.head_futex, (uint32_t)h->cached);
            spins = 0;
        } else {
            ZC_CPU_PAUSE();
        }
    }

slot:
    return (void *)&h->ring->slots[tail & ZC_SLOT_MASK][0];
}

/*
 * zc_futex_release — Signal the producer that slot can be reused and wake.
 */
static inline void zc_futex_release(zc_handle_t *h)
{
    const uint64_t tail = atomic_load_explicit(&h->ring->ctl.tail,
                                                memory_order_relaxed);
    atomic_store_explicit(&h->ring->ctl.tail, tail + 1,
                           memory_order_release);

    /* Mirror AFTER the counter store, BEFORE the wake (see header note). */
    h->ring->ctl.tail_futex = (uint32_t)(tail + 1);
    zc_futex_wake(&h->ring->ctl.tail_futex);
}

#endif /* ZEROCOPY_FUTEX_H */
