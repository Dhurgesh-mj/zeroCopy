/*
 * futex_producer.c — SPSC Zero-Copy Stress Test (Futex Hybrid Producer)
 *
 * Identical to producer.c but uses the futex-backed hybrid API:
 *   zc_futex_acquire / zc_futex_publish
 *
 * Under sustained load, behaves identically to the spin version.
 * Under bursty/idle workloads, sleeps via futex instead of burning CPU.
 *
 * Build:  make futex_producer
 * Run:    ./futex_producer   (then start futex_consumer)
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <inttypes.h>

#include "zerocopy_futex.h"

#define MSG_COUNT        10000000ULL
#define CHECKSUM_MAGIC   0xDEADBEEFCAFEBABEULL
#define GUARD_MAGIC      0xCAFED00DC0FFEEEEULL

static inline uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

int main(void)
{
    zc_handle_t h;

    zc_destroy(ZC_SHM_NAME);

    if (zc_create(&h, ZC_SHM_NAME) != 0) {
        perror("[futex-producer] zc_create");
        return 1;
    }

    fprintf(stderr,
        "[futex-producer] ════════════════════════════════════════\n"
        "[futex-producer] ring:      %p  (%zu bytes)\n"
        "[futex-producer] spin limit: %u iterations before futex sleep\n"
        "[futex-producer] messages:  %" PRIu64 "\n"
        "[futex-producer] ────────────────────────────────────────\n",
        (void *)h.ring, (size_t)ZC_RING_SIZE,
        (unsigned)ZC_SPIN_LIMIT,
        (uint64_t)MSG_COUNT);

    uint64_t t_start = now_ns();

    for (uint64_t seq = 0; seq < MSG_COUNT; ++seq) {
        uint8_t *slot = (uint8_t *)zc_futex_acquire(&h);

        uint64_t ts       = now_ns();
        uint64_t checksum = seq ^ CHECKSUM_MAGIC;

        *((uint64_t *)(slot +  0)) = seq;
        *((uint64_t *)(slot +  8)) = ts;
        *((uint64_t *)(slot + 16)) = checksum;
        *((uint64_t *)(slot + ZC_SLOT_SIZE - 8)) = seq ^ GUARD_MAGIC;

        zc_futex_publish(&h);
    }

    uint64_t t_end = now_ns();

    double elapsed_s  = (double)(t_end - t_start) / 1e9;
    double msgs_per_s = (double)MSG_COUNT / elapsed_s;
    double gbps       = (msgs_per_s * ZC_SLOT_SIZE * 8.0) / 1e9;
    double ns_per_msg = (double)(t_end - t_start) / (double)MSG_COUNT;

    fprintf(stderr,
        "[futex-producer] ════════════════════════════════════════\n"
        "[futex-producer] DONE\n"
        "[futex-producer] elapsed:    %.3f s\n"
        "[futex-producer] throughput: %.2f M msg/s\n"
        "[futex-producer] bandwidth:  %.2f Gbit/s  (%.2f GiB/s)\n"
        "[futex-producer] avg cost:   %.1f ns/msg\n"
        "[futex-producer] ════════════════════════════════════════\n",
        elapsed_s,
        msgs_per_s / 1e6,
        gbps, gbps / 8.0,
        ns_per_msg);

    zc_close(&h);
    return 0;
}
