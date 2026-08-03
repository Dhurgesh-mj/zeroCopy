/*
 * zerocopy.h — SPSC Zero-Copy Shared-Memory IPC Framework
 *
 * Architecture:
 *   Single-Producer, Single-Consumer lock-free ring buffer over POSIX shm.
 *   1024 slots × 4 KiB = 4 MiB payload. Monotonically increasing 64-bit
 *   counters; slot index derived via bitmask (& 0x3FF), never modular wrap.
 *
 * Data-Path Guarantees:
 *   - ZERO memcpy: caller gets a raw pointer directly into shared memory.
 *   - ZERO syscalls: no mutex, no semaphore, no futex on the hot path.
 *   - ZERO false sharing: head and tail proven ≥64 bytes apart at compile time.
 *
 * Memory Ordering Contract (SPSC):
 *   Producer: relaxed load own head → acquire load tail → release store head.
 *   Consumer: relaxed load own tail → acquire load head → release store tail.
 *
 * Build:
 *   cc -std=c11 -O3 -o producer producer.c -lrt
 *   cc -std=c11 -O3 -o consumer consumer.c -lrt
 *
 * Copyright (c) 2026 — Hackathon Demo. Public Domain.
 */

#ifndef ZEROCOPY_H
#define ZEROCOPY_H

/* ═══════════════════════════════════════════════════════════════════════════
 *  System Headers
 * ═══════════════════════════════════════════════════════════════════════════ */

#include <stdint.h>
#include <stddef.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

/* ═══════════════════════════════════════════════════════════════════════════
 *  Ring Buffer Constants
 * ═══════════════════════════════════════════════════════════════════════════ */

#define ZC_SLOT_SIZE    4096u                       /* bytes per slot (page)  */
#define ZC_SLOT_COUNT   1024u                       /* number of slots        */
#define ZC_SLOT_MASK    (ZC_SLOT_COUNT - 1u)        /* 0x3FF — bitmask index  */
#define ZC_SHM_NAME     "/zerocopy_ring"            /* default shm object     */

/* Compile-time proof: slot count is a power of two.
 * If not, the bitmask trick silently produces wrong indices.              */
static_assert(
    (ZC_SLOT_COUNT & ZC_SLOT_MASK) == 0,
    "FATAL: ZC_SLOT_COUNT must be a power of 2 for bitmask indexing"
);

/* Compile-time proof: slot size is a power of two (page-aligned writes).  */
static_assert(
    (ZC_SLOT_SIZE & (ZC_SLOT_SIZE - 1u)) == 0,
    "FATAL: ZC_SLOT_SIZE must be a power of 2"
);

/* ═══════════════════════════════════════════════════════════════════════════
 *  Shared Memory Layout
 *
 *  ┌─────────────────────────────────────────────────┐  offset 0
 *  │  head  (64-byte cache line, producer-owned)     │
 *  ├─────────────────────────────────────────────────┤  offset 64
 *  │  tail  (64-byte cache line, consumer-owned)     │
 *  ├─────────────────────────────────────────────────┤  offset 4096 (page)
 *  │  slots[0]      4 KiB                            │
 *  │  slots[1]      4 KiB                            │
 *  │  ...                                            │
 *  │  slots[1023]   4 KiB                            │
 *  └─────────────────────────────────────────────────┘
 *
 *  Total: sizeof(zc_control_t) padded to 4096 + (1024 × 4096) ≈ 4 MiB + 4 KiB
 * ═══════════════════════════════════════════════════════════════════════════ */

/*
 * Control block: two monotonically increasing 64-bit counters, each pinned
 * to its own 64-byte cache line.
 *
 * Full condition:  head - tail == ZC_SLOT_COUNT
 * Empty condition: head == tail
 * Count:           head - tail
 *
 * Because counters are unsigned 64-bit and monotonically increasing,
 * they will not wrap in any human lifetime at any throughput.
 * (2^64 messages at 10 billion msg/sec = ~58 years.)
 */
typedef struct {
    _Alignas(64) atomic_uint_least64_t head;       /* written ONLY by producer */
    _Alignas(64) atomic_uint_least64_t tail;       /* written ONLY by consumer */
} zc_control_t;

/* ── Compile-Time Proof: No False Sharing ────────────────────────────────
 *
 * CPU cache lines are 64 bytes on all modern x86-64 and ARMv8.
 * If head and tail share a cache line, every produce/consume bounces the
 * line between cores via MESI — destroying throughput.
 *
 * We PROVE they are on separate lines at compile time.                    */

static_assert(
    offsetof(zc_control_t, tail) - offsetof(zc_control_t, head) >= 64,
    "FATAL: head and tail separated by < 64 bytes — false sharing guaranteed"
);

/* Belt-and-suspenders: also verify control block is exactly 128 bytes
 * (two full cache lines, no surprises).                                   */
static_assert(
    sizeof(zc_control_t) == 128,
    "FATAL: zc_control_t is not exactly 128 bytes — unexpected padding"
);

/* ── The Complete Shared-Memory Region ─────────────────────────────────── */

typedef struct {
    zc_control_t            ctl;
    _Alignas(4096) uint8_t  slots[ZC_SLOT_COUNT][ZC_SLOT_SIZE];
} zc_ring_t;

#define ZC_RING_SIZE  sizeof(zc_ring_t)

/* Compile-time proof: payload area is at least 4 MiB.                     */
static_assert(
    sizeof(((zc_ring_t *)0)->slots) == ZC_SLOT_COUNT * ZC_SLOT_SIZE,
    "FATAL: slot array size mismatch"
);

/* ═══════════════════════════════════════════════════════════════════════════
 *  Process-Local Handle
 *
 *  NOT in shared memory. Lives on each process's stack/heap.
 *  Caches the remote counter to avoid hitting the other core's cache line
 *  on every operation. Only re-reads when the cached value says "full"
 *  or "empty" — amortizing cross-core traffic to ≈ 0.
 * ═══════════════════════════════════════════════════════════════════════════ */

typedef struct {
    zc_ring_t  *ring;
    uint64_t    cached;        /* producer caches tail; consumer caches head */
} zc_handle_t;

/* ═══════════════════════════════════════════════════════════════════════════
 *  Shared Memory Lifecycle
 * ═══════════════════════════════════════════════════════════════════════════ */

/*
 * zc_create — Create and initialize the shared-memory ring (producer).
 *
 * - Creates the POSIX shm object, sizes it, and mmaps it.
 * - Zero-initializes head and tail (ring starts empty).
 * - Returns 0 on success, -1 on failure (check errno).
 */
static inline int zc_create(zc_handle_t *h, const char *name)
{
    int fd = shm_open(name, O_CREAT | O_RDWR | O_TRUNC, 0600);
    if (fd < 0) return -1;

    if (ftruncate(fd, (off_t)ZC_RING_SIZE) < 0) {
        close(fd);
        return -1;
    }

    void *p = mmap(NULL, ZC_RING_SIZE, PROT_READ | PROT_WRITE,
                    MAP_SHARED, fd, 0);
    close(fd);                                      /* fd no longer needed   */
    if (p == MAP_FAILED) return -1;

    h->ring   = (zc_ring_t *)p;
    h->cached = 0;

    /* Initialize counters — ring starts empty (head == tail == 0).         */
    atomic_store_explicit(&h->ring->ctl.head, 0, memory_order_relaxed);
    atomic_store_explicit(&h->ring->ctl.tail, 0, memory_order_relaxed);

    return 0;
}

/*
 * zc_open — Attach to an existing shared-memory ring (consumer).
 *
 * - Opens the POSIX shm object and mmaps it.
 * - Does NOT initialize counters (producer owns initialization).
 * - Returns 0 on success, -1 on failure (check errno).
 */
static inline int zc_open(zc_handle_t *h, const char *name)
{
    int fd = shm_open(name, O_RDWR, 0);
    if (fd < 0) return -1;

    /* Guard against startup race: if the producer hasn't called ftruncate
     * yet, the shm object is zero-sized and mmap would SIGBUS on access. */
    struct stat sb;
    if (fstat(fd, &sb) < 0 || (size_t)sb.st_size < ZC_RING_SIZE) {
        close(fd);
        return -1;
    }

    void *p = mmap(NULL, ZC_RING_SIZE, PROT_READ | PROT_WRITE,
                    MAP_SHARED, fd, 0);
    close(fd);
    if (p == MAP_FAILED) return -1;

    h->ring   = (zc_ring_t *)p;
    h->cached = 0;

    return 0;
}

/*
 * zc_close — Unmap the shared-memory region.
 */
static inline void zc_close(zc_handle_t *h)
{
    if (h->ring) {
        munmap(h->ring, ZC_RING_SIZE);
        h->ring = NULL;
    }
}

/*
 * zc_destroy — Unlink the POSIX shm object from the filesystem namespace.
 *              Call after both sides have closed.
 */
static inline void zc_destroy(const char *name)
{
    shm_unlink(name);
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  Producer API — True Zero-Copy
 *
 *  Usage:
 *      void *slot = zc_acquire(&h);       // spin until a slot is free
 *      my_message_t *msg = (my_message_t *)slot;
 *      msg->seq   = 42;                   // write DIRECTLY into shm
 *      msg->value = 3.14;
 *      zc_publish(&h);                    // release-fence, visible to consumer
 *
 *  Guarantee: between acquire and publish, the pointer is EXCLUSIVELY
 *  owned by the producer. The consumer cannot see it.
 * ═══════════════════════════════════════════════════════════════════════════ */

/*
 * zc_acquire — Spin until a slot is available; return a raw pointer into it.
 *
 * Memory ordering rationale:
 *   We load our own head with relaxed (we are the only writer).
 *   We load the consumer's tail with acquire: this synchronizes-with the
 *   consumer's release store in zc_release(), guaranteeing the consumer
 *   is done reading the slot before we hand it back to the producer.
 */
static inline void *zc_acquire(zc_handle_t *h)
{
    const uint64_t head = atomic_load_explicit(&h->ring->ctl.head,
                                                memory_order_relaxed);

    /* ── Fast path: is there room based on the CACHED tail? ─────────────
     * If yes, we skip the expensive cross-core atomic load entirely.     */
    if (__builtin_expect((head - h->cached) < ZC_SLOT_COUNT, 1))
        goto slot;

    /* ── Slow path: buffer LOOKS full — re-read the real tail ────────── */
    for (;;) {
        h->cached = atomic_load_explicit(&h->ring->ctl.tail,
                                          memory_order_acquire);
        if ((head - h->cached) < ZC_SLOT_COUNT)
            break;
        /* tight spin — baseline wait strategy */
    }

slot:
    return (void *)&h->ring->slots[head & ZC_SLOT_MASK][0];
}

/*
 * zc_publish — Make the written slot visible to the consumer.
 *
 * Memory ordering rationale:
 *   Release store on head: all prior writes to the slot (by the producer)
 *   are guaranteed visible to any thread that subsequently performs an
 *   acquire load of head (the consumer in zc_consume).
 */
static inline void zc_publish(zc_handle_t *h)
{
    const uint64_t head = atomic_load_explicit(&h->ring->ctl.head,
                                                memory_order_relaxed);
    atomic_store_explicit(&h->ring->ctl.head, head + 1,
                           memory_order_release);
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  Consumer API — True Zero-Copy
 *
 *  Usage:
 *      void *slot = zc_consume(&h);       // spin until data arrives
 *      my_message_t *msg = (my_message_t *)slot;
 *      printf("seq=%lu\n", msg->seq);     // read DIRECTLY from shm
 *      zc_release(&h);                    // release-fence, slot recyclable
 *
 *  Guarantee: between consume and release, the pointer is EXCLUSIVELY
 *  owned by the consumer. The producer cannot overwrite it.
 * ═══════════════════════════════════════════════════════════════════════════ */

/*
 * zc_consume — Spin until a message is available; return a raw pointer.
 *
 * Memory ordering rationale:
 *   We load our own tail with relaxed (we are the only writer).
 *   We load the producer's head with acquire: this synchronizes-with the
 *   producer's release store in zc_publish(), guaranteeing the slot data
 *   is fully written before we read it.
 */
static inline void *zc_consume(zc_handle_t *h)
{
    const uint64_t tail = atomic_load_explicit(&h->ring->ctl.tail,
                                                memory_order_relaxed);

    /* ── Fast path: is there data based on the CACHED head? ─────────── */
    if (__builtin_expect(h->cached != tail, 1))
        goto slot;

    /* ── Slow path: buffer LOOKS empty — re-read the real head ──────── */
    for (;;) {
        h->cached = atomic_load_explicit(&h->ring->ctl.head,
                                          memory_order_acquire);
        if (h->cached != tail)
            break;
        /* tight spin — baseline wait strategy */
    }

slot:
    return (void *)&h->ring->slots[tail & ZC_SLOT_MASK][0];
}

/*
 * zc_release — Signal the producer that this slot can be reused.
 *
 * Memory ordering rationale:
 *   Release store on tail: all prior reads from the slot (by the consumer)
 *   are ordered before this store. The producer's subsequent acquire load
 *   of tail in zc_acquire() will see the updated tail AND be guaranteed
 *   the consumer is done with the slot.
 */
static inline void zc_release(zc_handle_t *h)
{
    const uint64_t tail = atomic_load_explicit(&h->ring->ctl.tail,
                                                memory_order_relaxed);
    atomic_store_explicit(&h->ring->ctl.tail, tail + 1,
                           memory_order_release);
}

#endif /* ZEROCOPY_H */
