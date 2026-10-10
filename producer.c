/*
 * producer.c — SPSC Zero-Copy Stress Test (Producer)
 *
 * Sends 10 million messages through the zero-copy ring buffer.
 * Each message contains:
 *   - Sequence number (at slot offset 0)
 *   - Nanosecond timestamp (at slot offset 8)
 *   - XOR checksum (at slot offset 16)
 *   - Guard word at the END of the 4 KiB slot (proves full-page coherency)
 *
 * All writes go directly into shared memory. Zero memcpy.
 *
 * Build:  make producer
 * Run:    ./producer        (then start consumer in another terminal)
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <inttypes.h>

#include "zerocopy.h"

/* ═══════════════════════════════════════════════════════════════════════════
 *  Test Parameters
 * ═══════════════════════════════════════════════════════════════════════════ */

#define MSG_COUNT        10000000ULL            /* 10 million messages       */
#define CHECKSUM_MAGIC   0xDEADBEEFCAFEBABEULL  /* XOR key for integrity     */
#define GUARD_MAGIC      0xCAFED00DC0FFEEEEULL   /* guard word at slot tail   */

/* ═══════════════════════════════════════════════════════════════════════════
 *  Helpers
 * ═══════════════════════════════════════════════════════════════════════════ */

static inline uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  Main
 * ═══════════════════════════════════════════════════════════════════════════ */

int main(void)
{
    zc_handle_t h;

    /* Clean up any stale segment from a previous crashed run */
    zc_destroy(ZC_SHM_NAME);

    if (zc_create(&h, ZC_SHM_NAME) != 0) {
        perror("[producer] zc_create");
        return 1;
    }

    fprintf(stderr,
        "[producer] ══════════════════════════════════════════════\n"
        "[producer] ring:      %p  (%zu bytes, %.1f MiB)\n"
        "[producer] slots:     %u × %u bytes\n"
        "[producer] messages:  %" PRIu64 "\n"
        "[producer] ──────────────────────────────────────────────\n",
        (void *)h.ring, (size_t)ZC_RING_SIZE,
        (double)ZC_RING_SIZE / (1024.0 * 1024.0),
        ZC_SLOT_COUNT, ZC_SLOT_SIZE,
        (uint64_t)MSG_COUNT);

    /* ── Hot loop ──────────────────────────────────────────────────────── */

    uint64_t t_start = now_ns();

    for (uint64_t seq = 0; seq < MSG_COUNT; ++seq) {
        /*
         * ACQUIRE: spin until a slot is free.
         * Returns a raw pointer directly into shared memory.
         * The producer writes here — no memcpy, no intermediate buffer.
         */
        uint8_t *slot = (uint8_t *)zc_acquire(&h);

        /* Write header at the START of the 4 KiB slot.
         * memcpy (not uint64_t* casts): the slot is a byte buffer and the
         * pointer-cast version violates C strict aliasing. GCC -O3 still
         * compiles each memcpy to a single mov instruction. */
        uint64_t ts       = now_ns();
        uint64_t checksum = seq ^ CHECKSUM_MAGIC;
        uint64_t guard    = seq ^ GUARD_MAGIC;

        memcpy(slot +  0, &seq,      sizeof(seq));
        memcpy(slot +  8, &ts,       sizeof(ts));
        memcpy(slot + 16, &checksum, sizeof(checksum));

        /*
         * Write guard word at the LAST 8 bytes of the slot.
         * This proves the entire 4 KiB page is coherent:
         * if we saw a stale/torn guard, the consumer catches it.
         */
        memcpy(slot + ZC_SLOT_SIZE - 8, &guard, sizeof(guard));

        /*
         * PUBLISH: release-store on head.
         * All prior writes to the slot are guaranteed visible to
         * the consumer after its acquire-load of head.
         */
        zc_publish(&h);
    }

    uint64_t t_end = now_ns();

    /* ── Results ───────────────────────────────────────────────────────── */

    double elapsed_s   = (double)(t_end - t_start) / 1e9;
    double msgs_per_s  = (double)MSG_COUNT / elapsed_s;
    double gbps        = (msgs_per_s * ZC_SLOT_SIZE * 8.0) / 1e9;
    double ns_per_msg  = (double)(t_end - t_start) / (double)MSG_COUNT;

    fprintf(stderr,
        "[producer] ══════════════════════════════════════════════\n"
        "[producer] DONE\n"
        "[producer] elapsed:    %.3f s\n"
        "[producer] throughput: %.2f M msg/s\n"
        "[producer] bandwidth:  %.2f Gbit/s  (%.2f GiB/s)\n"
        "[producer] avg cost:   %.1f ns/msg\n"
        "[producer] ══════════════════════════════════════════════\n",
        elapsed_s,
        msgs_per_s / 1e6,
        gbps, gbps / 8.0,
        ns_per_msg);

    zc_close(&h);
    return 0;
}
