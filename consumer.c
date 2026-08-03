/*
 * consumer.c — SPSC Zero-Copy Stress Test (Consumer)
 *
 * Receives 10 million messages and validates:
 *   1. Sequence order (monotonically increasing, no gaps, no duplicates)
 *   2. XOR checksum   (data integrity)
 *   3. Guard word     (full 4 KiB page coherency — no torn writes)
 *
 * Collects per-message latency (CLOCK_MONOTONIC delta from producer timestamp)
 * and reports min / mean / p50 / p99 / p99.9 / max.
 *
 * All reads come directly from shared memory. Zero memcpy.
 *
 * Build:  make consumer
 * Run:    ./consumer        (after producer is running)
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <inttypes.h>

#include "zerocopy.h"

/* ═══════════════════════════════════════════════════════════════════════════
 *  Test Parameters  (must match producer.c)
 * ═══════════════════════════════════════════════════════════════════════════ */

#define MSG_COUNT        10000000ULL
#define CHECKSUM_MAGIC   0xDEADBEEFCAFEBABEULL
#define GUARD_MAGIC      0xCAFED00DC0FFEEEEULL
#define MAX_ERRORS_PRINT 10                     /* cap spew on systemic fail */

/* ═══════════════════════════════════════════════════════════════════════════
 *  Helpers
 * ═══════════════════════════════════════════════════════════════════════════ */

static inline uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static int cmp_u64(const void *a, const void *b)
{
    const uint64_t va = *(const uint64_t *)a;
    const uint64_t vb = *(const uint64_t *)b;
    return (va > vb) - (va < vb);
}

/* ═══════════════════════════════════════════════════════════════════════════
 *  Main
 * ═══════════════════════════════════════════════════════════════════════════ */

int main(void)
{
    zc_handle_t h;

    /* Retry until the producer creates and initializes the segment.
     * zc_open includes an fstat size guard — no SIGBUS risk.             */
    fprintf(stderr, "[consumer] waiting for producer to create ring...\n");
    while (zc_open(&h, ZC_SHM_NAME) != 0) {
        struct timespec ts = { 0, 1000000 };   /* 1 ms */
        nanosleep(&ts, NULL);
    }

    fprintf(stderr,
        "[consumer] ══════════════════════════════════════════════\n"
        "[consumer] attached:  %p\n"
        "[consumer] expecting: %" PRIu64 " messages\n"
        "[consumer] ──────────────────────────────────────────────\n",
        (void *)h.ring, (uint64_t)MSG_COUNT);

    /* Allocate latency recording array: 10M × 8 bytes = 80 MiB           */
    uint64_t *latencies = (uint64_t *)malloc(MSG_COUNT * sizeof(uint64_t));
    if (!latencies) {
        perror("[consumer] malloc");
        return 1;
    }

    /* ── Hot loop ──────────────────────────────────────────────────────── */

    uint64_t errors     = 0;
    uint64_t printed    = 0;
    uint64_t t_start    = now_ns();

    for (uint64_t i = 0; i < MSG_COUNT; ++i) {
        /*
         * CONSUME: spin until data arrives.
         * Returns a raw pointer directly into shared memory.
         * The consumer reads here — no memcpy, no intermediate buffer.
         */
        const uint8_t *slot = (const uint8_t *)zc_consume(&h);
        uint64_t t_recv     = now_ns();

        /* Read fields directly from shared memory (zero-copy) */
        uint64_t seq      = *((const uint64_t *)(slot +  0));
        uint64_t ts_send  = *((const uint64_t *)(slot +  8));
        uint64_t checksum = *((const uint64_t *)(slot + 16));
        uint64_t guard    = *((const uint64_t *)(slot + ZC_SLOT_SIZE - 8));

        /* ── Validation ──────────────────────────────────────────────── */

        /* 1. Sequence: must be strictly monotonic, no gaps */
        if (seq != i) {
            ++errors;
            if (printed < MAX_ERRORS_PRINT) {
                fprintf(stderr, "[consumer] SEQ ERROR:   expected %" PRIu64
                        ", got %" PRIu64 "\n", i, seq);
                ++printed;
            }
        }

        /* 2. Checksum: proves header bytes are intact */
        if (checksum != (seq ^ CHECKSUM_MAGIC)) {
            ++errors;
            if (printed < MAX_ERRORS_PRINT) {
                fprintf(stderr, "[consumer] CSUM ERROR:  seq %" PRIu64 "\n", seq);
                ++printed;
            }
        }

        /* 3. Guard word: proves the LAST byte of the 4 KiB slot is
         *    coherent with the FIRST byte — no partial/torn writes     */
        if (guard != (seq ^ GUARD_MAGIC)) {
            ++errors;
            if (printed < MAX_ERRORS_PRINT) {
                fprintf(stderr, "[consumer] GUARD ERROR: seq %" PRIu64
                        "  (torn page / partial write)\n", seq);
                ++printed;
            }
        }

        /* Record one-way latency */
        latencies[i] = t_recv - ts_send;

        /*
         * RELEASE: release-store on tail.
         * Signals the producer that this slot can be recycled.
         */
        zc_release(&h);
    }

    uint64_t t_end = now_ns();

    /* ── Latency Statistics ────────────────────────────────────────────── */

    qsort(latencies, MSG_COUNT, sizeof(uint64_t), cmp_u64);

    uint64_t lat_min  = latencies[0];
    uint64_t lat_max  = latencies[MSG_COUNT - 1];
    uint64_t lat_p50  = latencies[(size_t)(MSG_COUNT * 50  / 100)];
    uint64_t lat_p90  = latencies[(size_t)(MSG_COUNT * 90  / 100)];
    uint64_t lat_p99  = latencies[(size_t)(MSG_COUNT * 99  / 100)];
    uint64_t lat_p999 = latencies[(size_t)(MSG_COUNT * 999 / 1000)];

    double lat_sum = 0.0;
    for (uint64_t i = 0; i < MSG_COUNT; ++i)
        lat_sum += (double)latencies[i];
    double lat_mean = lat_sum / (double)MSG_COUNT;

    double elapsed_s  = (double)(t_end - t_start) / 1e9;
    double msgs_per_s = (double)MSG_COUNT / elapsed_s;
    double gbps       = (msgs_per_s * ZC_SLOT_SIZE * 8.0) / 1e9;

    /* ── Report ────────────────────────────────────────────────────────── */

    fprintf(stderr,
        "\n"
        "[consumer] ══════════════════════════════════════════════\n"
        "[consumer] RESULTS\n"
        "[consumer] ──────────────────────────────────────────────\n"
        "[consumer] received:   %" PRIu64 " messages\n"
        "[consumer] errors:     %" PRIu64 "\n"
        "[consumer] elapsed:    %.3f s\n"
        "[consumer] throughput: %.2f M msg/s\n"
        "[consumer] bandwidth:  %.2f Gbit/s  (%.2f GiB/s)\n"
        "[consumer] ──────────────────────────────────────────────\n"
        "[consumer] LATENCY (one-way, ns)\n"
        "[consumer]   min:     %7" PRIu64 " ns\n"
        "[consumer]   mean:    %7.0f ns\n"
        "[consumer]   p50:     %7" PRIu64 " ns\n"
        "[consumer]   p90:     %7" PRIu64 " ns\n"
        "[consumer]   p99:     %7" PRIu64 " ns\n"
        "[consumer]   p99.9:   %7" PRIu64 " ns\n"
        "[consumer]   max:     %7" PRIu64 " ns\n"
        "[consumer] ══════════════════════════════════════════════\n",
        (uint64_t)MSG_COUNT,
        errors,
        elapsed_s,
        msgs_per_s / 1e6,
        gbps, gbps / 8.0,
        lat_min,
        lat_mean,
        lat_p50,
        lat_p90,
        lat_p99,
        lat_p999,
        lat_max);

    /* ── Verdict ───────────────────────────────────────────────────────── */

    if (errors == 0) {
        fprintf(stderr,
            "\n"
            "  ╔══════════════════════════════════════════════════╗\n"
            "  ║  ✓  ALL %" PRIu64 " MESSAGES VALIDATED          ║\n"
            "  ║     ZERO CORRUPTION · ZERO REORDERING           ║\n"
            "  ╚══════════════════════════════════════════════════╝\n\n",
            (uint64_t)MSG_COUNT);
    } else {
        fprintf(stderr,
            "\n"
            "  ╔══════════════════════════════════════════════════╗\n"
            "  ║  ✗  FAILED — %" PRIu64 " ERRORS DETECTED                ║\n"
            "  ╚══════════════════════════════════════════════════╝\n\n",
            errors);
    }

    free(latencies);
    zc_close(&h);
    zc_destroy(ZC_SHM_NAME);

    return errors ? 1 : 0;
}
