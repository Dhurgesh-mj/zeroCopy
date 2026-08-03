/*
 * zerocopy_futex.h — Futex-backed hybrid sleep/wake strategy for zero-copy ring
 */

#ifndef ZEROCOPY_FUTEX_H
#define ZEROCOPY_FUTEX_H

#include "zerocopy.h"
#include <linux/futex.h>
#include <sys/syscall.h>
#include <unistd.h>

#ifndef ZC_SPIN_LIMIT
#define ZC_SPIN_LIMIT 1000
#endif

static inline long zc_futex_wait(void *addr, uint32_t expected)
{
    return syscall(SYS_futex, addr, FUTEX_WAIT, expected, NULL, NULL, 0);
}

static inline long zc_futex_wake(void *addr)
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
            uint32_t *tail_ptr = (uint32_t *)&h->ring->ctl.tail;
            zc_futex_wait(tail_ptr, (uint32_t)h->cached);
            spins = 0;
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
    
    uint32_t *head_ptr = (uint32_t *)&h->ring->ctl.head;
    zc_futex_wake(head_ptr);
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
            uint32_t *head_ptr = (uint32_t *)&h->ring->ctl.head;
            zc_futex_wait(head_ptr, (uint32_t)h->cached);
            spins = 0;
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
                           
    uint32_t *tail_ptr = (uint32_t *)&h->ring->ctl.tail;
    zc_futex_wake(tail_ptr);
}

#endif /* ZEROCOPY_FUTEX_H */
