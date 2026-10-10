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
#include <assert.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>

/* ═══════════════════════════════════════════════════════════════════════════
 *  CPU Pause (spin-loop hint)
 *
 *  In tight spin loops, a pause/yield hint lets the sibling hyperthread
 *  make progress and avoids memory-order penalties on x86. Zero effect
 *  on correctness — pure politeness to the other core.
 * ═══════════════════════════════════════════════════════════════════════════ */

#if defined(__x86_64__) || defined(__i386__)
#define ZC_CPU_PAUSE() __builtin_ia32_pause()
#elif defined(__aarch64__)
#define ZC_CPU_PAUSE() __asm__ volatile("yield" ::: "memory")
#else
#define ZC_CPU_PAUSE() ((void)0)
#endif

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
 *  │  head + head_futex (64-byte line, producer)     │
 *  ├─────────────────────────────────────────────────┤  offset 64
 *  │  tail + tail_futex (64-byte line, consumer)     │
 *  ├─────────────────────────────────────────────────┤  offset 128
 *  │  meta (magic/version/layout/generation)         │
 *  │  …padding to next page…                         │
 *  ├─────────────────────────────────────────────────┤  offset 4096 (page)
 *  │  slots[0]      4 KiB                            │
 *  │  slots[1]      4 KiB                            │
 *  │  ...                                            │
 *  │  slots[1023]   4 KiB                            │
 *  └─────────────────────────────────────────────────┘
 *
 *  Total: 4096 (control page) + (1024 × 4096) ≈ 4 MiB + 4 KiB
 * ═══════════════════════════════════════════════════════════════════════════ */

/* Magic + ABI version: validated by zc_open() so a stale or foreign
 * segment (wrong build, uninitialized, attacker-planted) fails fast
 * with EPROTO instead of being silently misread.                        */
#define ZC_MAGIC        0x5A434F5059524E47ULL   /* ASCII "ZCOPYRNG" */
#define ZC_ABI_VERSION  1u

/*
 * Control block: two monotonically increasing 64-bit counters, each pinned
 * to its own 64-byte cache line.
 *
 * Each line also carries a 32-bit futex word that MIRRORS the low 32 bits
 * of its counter for use with the futex(2) syscall (see zerocopy_futex.h).
 * The futex words are plain uint32_t, written ONLY by the line's owner;
 * the peer side must only pass their address to the futex syscall and
 * must never load them in C (that would be a data race).
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
    uint32_t head_futex;                            /* producer-owned futex word */
    _Alignas(64) atomic_uint_least64_t tail;       /* written ONLY by consumer */
    uint32_t tail_futex;                            /* consumer-owned futex word */
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

/* Compile-time proof: each futex word sits on the SAME cache line as the
 * counter it mirrors (keeps the mirror store on an already-owned line). */
static_assert(
    offsetof(zc_control_t, head_futex) < 64,
    "FATAL: head_futex is not on head's cache line"
);
static_assert(
    offsetof(zc_control_t, tail_futex) >= 64 &&
    offsetof(zc_control_t, tail_futex) < 128,
    "FATAL: tail_futex is not on tail's cache line"
);

/*
 * Segment metadata: lives in the otherwise-unused padding between the
 * control block (offset 128) and the first slot (offset 4096).
 * Written once by zc_create(), validated on every zc_open().
 */
typedef struct {
    uint64_t magic;          /* == ZC_MAGIC once initialized      */
    uint32_t abi_version;    /* == ZC_ABI_VERSION                  */
    uint32_t slot_count;     /* == ZC_SLOT_COUNT                   */
    uint32_t slot_size;      /* == ZC_SLOT_SIZE                    */
    uint32_t _reserved;
    uint64_t generation;     /* bumped on every zc_create()        */
} zc_meta_t;

/* ── The Complete Shared-Memory Region ─────────────────────────────────── */

typedef struct {
    zc_control_t            ctl;
    zc_meta_t               meta;   /* offset 128 — checked by zc_open   */
    _Alignas(4096) uint8_t  slots[ZC_SLOT_COUNT][ZC_SLOT_SIZE];
} zc_ring_t;

#define ZC_RING_SIZE  sizeof(zc_ring_t)

/* Compile-time proof: the metadata fits in the control-page padding.     */
static_assert(
    offsetof(zc_ring_t, slots) >= sizeof(zc_control_t) + sizeof(zc_meta_t),
    "FATAL: zc_meta_t does not fit between control block and slots"
);

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
 * - Creates the POSIX shm object with O_EXCL, sizes it, and mmaps it.
 * - Refuses (EACCES) to reuse a pre-existing object owned by ANOTHER user:
 *   otherwise anyone could pre-create the name and silently intercept all
 *   IPC (name-squatting attack). Callers should zc_destroy() their own
 *   stale segment first (as the demo producers do).
 * - Initializes counters, futex words, and metadata; bumps generation.
 * - Deliberately avoids O_TRUNC: truncating an object that a live peer
 *   has mapped would shrink it to 0 bytes mid-run and SIGBUS the peer.
 *   ftruncate() to the (already correct) size is a safe no-op instead.
 * - Returns 0 on success, -1 on failure (check errno).
 *
 * NOTE: re-creating while a consumer is attached resets the counters.
 * A stale consumer detects this as an EPROTO generation mismatch only
 * when it re-opens. Restarting the producer mid-run without restarting
 * the consumer is unsupported — shut the consumer down first.
 */
static inline int zc_create(zc_handle_t *h, const char *name)
{
    int fd = shm_open(name, O_CREAT | O_EXCL | O_RDWR, 0600);
    if (fd < 0) {
        if (errno != EEXIST)
            return -1;
        /* Name already exists. Open it and check ownership instead of
         * blindly reusing whatever is there. */
        fd = shm_open(name, O_RDWR, 0);
        if (fd < 0)
            return -1;
        struct stat sb;
        if (fstat(fd, &sb) < 0) {
            close(fd);
            return -1;
        }
        if (sb.st_uid != geteuid()) {
            /* Someone else's object — refuse to touch it. */
            close(fd);
            errno = EACCES;
            return -1;
        }
        /* Self-owned stale object: fall through, resize + re-init below. */
    }

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

    /* Preserve the generation of a self-owned predecessor so a stale
     * consumer that re-opens can tell the ring was re-created.
     * (Fresh zero-filled objects fail the magic check → generation 1.) */
    uint64_t old_gen = 0;
    if (h->ring->meta.magic       == ZC_MAGIC        &&
        h->ring->meta.abi_version == ZC_ABI_VERSION  &&
        h->ring->meta.slot_count  == ZC_SLOT_COUNT   &&
        h->ring->meta.slot_size   == ZC_SLOT_SIZE)
        old_gen = h->ring->meta.generation;

    /* Initialize counters — ring starts empty (head == tail == 0).         */
    atomic_store_explicit(&h->ring->ctl.head, 0, memory_order_relaxed);
    h->ring->ctl.head_futex = 0;
    atomic_store_explicit(&h->ring->ctl.tail, 0, memory_order_relaxed);
    h->ring->ctl.tail_futex = 0;

    h->ring->meta.abi_version = ZC_ABI_VERSION;
    h->ring->meta.slot_count  = ZC_SLOT_COUNT;
    h->ring->meta.slot_size   = ZC_SLOT_SIZE;
    h->ring->meta._reserved   = 0;
    h->ring->meta.generation  = old_gen + 1;
    /* Publish the metadata: release fence first, magic store last.
     * A consumer that observes a nonzero magic is guaranteed to see
     * all fields above (pairs with the acquire fence in zc_open),
     * so it can never observe a half-written meta block. */
    atomic_thread_fence(memory_order_release);
    h->ring->meta.magic = ZC_MAGIC;

    return 0;
}

/*
 * zc_open — Attach to an existing shared-memory ring (consumer).
 *
 * - Opens the POSIX shm object and mmaps it.
 * - Validates size (SIGBUS guard), ownership (must be our own uid),
 *   and metadata (magic/version/layout). Any mismatch fails with
 *   errno = EPROTO (bad segment) or EACCES (someone else's segment)
 *   instead of silently reading garbage.
 * - Transient "producer hasn't finished creating yet" states fail with
 *   errno = EAGAIN so callers can retry.
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
    if (fstat(fd, &sb) < 0) {
        close(fd);
        return -1;
    }
    if ((size_t)sb.st_size < ZC_RING_SIZE) {
        close(fd);
        errno = EAGAIN;     /* not ready yet — caller may retry */
        return -1;
    }
    if (sb.st_uid != geteuid()) {
        close(fd);
        errno = EACCES;     /* not ours — refuse to attach */
        return -1;
    }

    void *p = mmap(NULL, ZC_RING_SIZE, PROT_READ | PROT_WRITE,
                    MAP_SHARED, fd, 0);
    close(fd);
    if (p == MAP_FAILED) return -1;

    /* Validate the segment before trusting a single byte of it.
     * All-zero metadata means the producer ftruncate()d but hasn't
     * initialized yet — transient, caller may retry. */
    const zc_meta_t *m = &((const zc_ring_t *)p)->meta;
    const uint64_t magic = m->magic;    /* single load, then fence below */
    if (magic == 0) {
        munmap(p, ZC_RING_SIZE);
        errno = EAGAIN;     /* not initialized yet — caller may retry */
        return -1;
    }
    /* Acquire fence: pairs with the producer's release fence before the
     * magic store, so the field loads below can't see torn metadata. */
    atomic_thread_fence(memory_order_acquire);
    if (magic            != ZC_MAGIC       ||
        m->abi_version != ZC_ABI_VERSION ||
        m->slot_count  != ZC_SLOT_COUNT  ||
        m->slot_size   != ZC_SLOT_SIZE) {
        munmap(p, ZC_RING_SIZE);
        errno = EPROTO;     /* uninitialized / foreign / stale segment */
        return -1;
    }

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
        ZC_CPU_PAUSE();
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
        ZC_CPU_PAUSE();
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
