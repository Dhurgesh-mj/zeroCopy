/*
 * futex_consumer.c — SPSC Zero-Copy Stress Test (Futex Hybrid Consumer)
 *
 * Identical to consumer.c but uses the futex-backed hybrid API:
 *   zc_futex_consume / zc_futex_release
 *
 * Build:  make futex_consumer
 * Run:    ./futex_consumer   (after futex_producer is running)
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <inttypes.h>
#include <errno.h>

#include "zerocopy_futex.h"

#define MSG_COUNT        10000000ULL
#define CHECKSUM_MAGIC   0xDEADBEEFCAFEBABEULL
#define GUARD_MAGIC      0xCAFED00DC0FFEEEEULL
#define MAX_ERRORS_PRINT 10

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

int main(void)
{
    zc_handle_t h;

    fprintf(stderr, "[futex-consumer] waiting for producer...\n");
    uint64_t t_wait_start = now_ns();
    while (zc_open(&h, ZC_SHM_NAME) != 0) {
        if (errno == EACCES || errno == EPROTO) {
            fprintf(stderr, "[futex-consumer] FATAL: cannot attach to ring: %s\n",
                    errno == EACCES ? "segment owned by another user"
                                    : "segment failed validation");
            return 1;
        }
        if (now_ns() - t_wait_start > 30000000000ULL) {
            fprintf(stderr, "[futex-consumer] FATAL: producer did not create "
                            "ring within 30 s\n");
            return 1;
        }
        struct timespec ts = { 0, 1000000 };
        nanosleep(&ts, NULL);
    }

    fprintf(stderr,
        "[futex-consumer] ════════════════════════════════════════\n"
        "[futex-consumer] attached:   %p\n"
        "[futex-consumer] spin limit: %u iterations before futex sleep\n"
        "[futex-consumer] expecting:  %" PRIu64 " messages\n"
        "[futex-consumer] ────────────────────────────────────────\n",
        (void *)h.ring,
        (unsigned)ZC_SPIN_LIMIT,
        (uint64_t)MSG_COUNT);

    uint64_t *latencies = (uint64_t *)malloc(MSG_COUNT * sizeof(uint64_t));
    if (!latencies) {
        perror("[futex-consumer] malloc");
        return 1;
    }

    uint64_t errors  = 0;
    uint64_t printed = 0;
    uint64_t t_start = now_ns();
    uint64_t t_last_progress = t_start;

    for (uint64_t i = 0; i < MSG_COUNT; ++i) {
        const uint8_t *slot = (const uint8_t *)zc_futex_consume(&h);
        uint64_t t_recv     = now_ns();

        /* memcpy, not pointer casts (strict aliasing — see producer.c) */
        uint64_t seq, ts_send, checksum, guard;
        memcpy(&seq,      slot +  0, sizeof(seq));
        memcpy(&ts_send,  slot +  8, sizeof(ts_send));
        memcpy(&checksum, slot + 16, sizeof(checksum));
        memcpy(&guard,    slot + ZC_SLOT_SIZE - 8, sizeof(guard));

        if (seq != i) {
            ++errors;
            if (printed < MAX_ERRORS_PRINT) {
                fprintf(stderr, "[futex-consumer] SEQ ERROR:   expected %" PRIu64
                        ", got %" PRIu64 "\n", i, seq);
                ++printed;
            }
        }
        if (checksum != (seq ^ CHECKSUM_MAGIC)) {
            ++errors;
            if (printed < MAX_ERRORS_PRINT) {
                fprintf(stderr, "[futex-consumer] CSUM ERROR:  seq %" PRIu64 "\n", seq);
                ++printed;
            }
        }
        if (guard != (seq ^ GUARD_MAGIC)) {
            ++errors;
            if (printed < MAX_ERRORS_PRINT) {
                fprintf(stderr, "[futex-consumer] GUARD ERROR: seq %" PRIu64 "\n", seq);
                ++printed;
            }
        }

        latencies[i] = t_recv - ts_send;
        zc_futex_release(&h);

        /* Stall watchdog: bounded futex waits alone can't detect a dead
         * peer — bail out instead of looping forever. Checked once per
         * 1024 messages; 5 s is far above any legitimate gap. */
        if ((i & 1023u) == 0) {
            if (t_recv - t_last_progress > 5000000000ULL) {
                fprintf(stderr, "[futex-consumer] FATAL: no progress for 5 s — "
                                "producer presumed dead\n");
                free(latencies);
                zc_close(&h);
                return 1;
            }
            t_last_progress = t_recv;
        }
    }

    uint64_t t_end = now_ns();

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

    fprintf(stderr,
        "\n"
        "[futex-consumer] ════════════════════════════════════════\n"
        "[futex-consumer] RESULTS\n"
        "[futex-consumer] ────────────────────────────────────────\n"
        "[futex-consumer] received:   %" PRIu64 " messages\n"
        "[futex-consumer] errors:     %" PRIu64 "\n"
        "[futex-consumer] elapsed:    %.3f s\n"
        "[futex-consumer] throughput: %.2f M msg/s\n"
        "[futex-consumer] bandwidth:  %.2f Gbit/s  (%.2f GiB/s)\n"
        "[futex-consumer] ────────────────────────────────────────\n"
        "[futex-consumer] LATENCY (one-way, ns)\n"
        "[futex-consumer]   min:     %7" PRIu64 " ns\n"
        "[futex-consumer]   mean:    %7.0f ns\n"
        "[futex-consumer]   p50:     %7" PRIu64 " ns\n"
        "[futex-consumer]   p90:     %7" PRIu64 " ns\n"
        "[futex-consumer]   p99:     %7" PRIu64 " ns\n"
        "[futex-consumer]   p99.9:   %7" PRIu64 " ns\n"
        "[futex-consumer]   max:     %7" PRIu64 " ns\n"
        "[futex-consumer] ════════════════════════════════════════\n",
        (uint64_t)MSG_COUNT,
        errors,
        elapsed_s,
        msgs_per_s / 1e6,
        gbps, gbps / 8.0,
        lat_min, lat_mean, lat_p50, lat_p90, lat_p99, lat_p999, lat_max);

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
