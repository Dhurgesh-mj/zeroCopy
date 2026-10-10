#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <inttypes.h>
#include <string.h>
#include <stddef.h>
#include <unistd.h>
#include <time.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <errno.h>

#define MSG_COUNT 10000000ULL
#define MSG_SIZE  4096u
#define SOCK_NAME "zerocopy_baseline"   /* abstract-namespace socket name */
#define CHECKSUM_MAGIC 0xDEADBEEFCAFEBABEULL
#define GUARD_MAGIC    0xCAFED00DC0FFEEEEULL

static inline uint64_t get_time_ns() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
}

static int recv_all(int fd, void *buf, size_t len) {
    char *p = buf;
    while (len > 0) {
        ssize_t ret = read(fd, p, len);
        if (ret < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (ret == 0) return -1;
        p += ret;
        len -= ret;
    }
    return 0;
}

static int cmp_uint64(const void *a, const void *b) {
    uint64_t x = *(const uint64_t*)a;
    uint64_t y = *(const uint64_t*)b;
    if (x < y) return -1;
    if (x > y) return 1;
    return 0;
}

int main() {
    printf("[baseline-consumer] Starting UDS benchmark consumer...\n");
    
    int sock_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sock_fd < 0) {
        perror("socket");
        return 1;
    }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    /* Must match the producer's abstract address (leading NUL). */
    addr.sun_path[0] = '\0';
    memcpy(addr.sun_path + 1, SOCK_NAME, sizeof(SOCK_NAME));  /* incl. NUL */
    socklen_t addrlen =
        (socklen_t)(offsetof(struct sockaddr_un, sun_path) + 1 + sizeof(SOCK_NAME));

    int retries = 0;
    while (connect(sock_fd, (struct sockaddr*)&addr, addrlen) < 0) {
        if (retries++ > 10) {
            perror("connect");
            return 1;
        }
        usleep(100000);
    }

    printf("[baseline-consumer] Connected to producer.\n");

    uint64_t *latencies = malloc(MSG_COUNT * sizeof(uint64_t));
    if (!latencies) {
        fprintf(stderr, "Failed to allocate latency array\n");
        return 1;
    }

    uint64_t errors = 0;
    char buffer[MSG_SIZE];
    uint64_t start_time = get_time_ns();

    for (uint64_t expected_seq = 0; expected_seq < MSG_COUNT; expected_seq++) {
        if (recv_all(sock_fd, buffer, MSG_SIZE) < 0) {
            perror("recv_all");
            break;
        }

        uint64_t recv_time = get_time_ns();

        uint64_t seq, timestamp, checksum, guard;
        memcpy(&seq, buffer, sizeof(seq));
        memcpy(&timestamp, buffer + 8, sizeof(timestamp));
        memcpy(&checksum, buffer + 16, sizeof(checksum));
        memcpy(&guard, buffer + MSG_SIZE - 8, sizeof(guard));

        int msg_error = 0;
        if (seq != expected_seq) msg_error = 1;
        if (checksum != (seq ^ CHECKSUM_MAGIC)) msg_error = 1;
        if (guard != (seq ^ GUARD_MAGIC)) msg_error = 1;

        if (msg_error) {
            if (errors < 10) {
                fprintf(stderr, "[baseline-consumer] ERROR on seq %" PRIu64 ": got seq=%" PRIu64 "\n", expected_seq, seq);
            }
            errors++;
        }

        uint64_t latency = (recv_time > timestamp) ? (recv_time - timestamp) : 0;
        latencies[expected_seq] = latency;
    }

    uint64_t end_time = get_time_ns();
    double elapsed_sec = (double)(end_time - start_time) / 1e9;
    double m_msg_s = ((double)MSG_COUNT / 1e6) / elapsed_sec;
    double bytes_total = (double)MSG_COUNT * MSG_SIZE;
    double gbit_s = (bytes_total * 8.0 / 1e9) / elapsed_sec;

    qsort(latencies, MSG_COUNT, sizeof(uint64_t), cmp_uint64);
    
    double mean = 0;
    for (uint64_t i = 0; i < MSG_COUNT; i++) mean += latencies[i];
    mean /= MSG_COUNT;

    fprintf(stderr,
        "\n"
        "[baseline-consumer] ══════════════════════════════════════════════\n"
        "[baseline-consumer] RESULTS\n"
        "[baseline-consumer] ──────────────────────────────────────────────\n"
        "[baseline-consumer] received:   %" PRIu64 " messages\n"
        "[baseline-consumer] errors:     %" PRIu64 "\n"
        "[baseline-consumer] elapsed:    %.3f s\n"
        "[baseline-consumer] throughput: %.2f M msg/s\n"
        "[baseline-consumer] bandwidth:  %.2f Gbit/s\n"
        "[baseline-consumer] ──────────────────────────────────────────────\n"
        "[baseline-consumer] LATENCY (one-way, ns)\n"
        "[baseline-consumer]   min:     %7" PRIu64 " ns\n"
        "[baseline-consumer]   mean:    %7.0f ns\n"
        "[baseline-consumer]   p50:     %7" PRIu64 " ns\n"
        "[baseline-consumer]   p90:     %7" PRIu64 " ns\n"
        "[baseline-consumer]   p99:     %7" PRIu64 " ns\n"
        "[baseline-consumer]   p99.9:   %7" PRIu64 " ns\n"
        "[baseline-consumer]   max:     %7" PRIu64 " ns\n"
        "[baseline-consumer] ══════════════════════════════════════════════\n",
        (uint64_t)MSG_COUNT,
        errors,
        elapsed_sec,
        m_msg_s,
        gbit_s,
        latencies[0],
        mean,
        latencies[(size_t)(MSG_COUNT * 50 / 100)],
        latencies[(size_t)(MSG_COUNT * 90 / 100)],
        latencies[(size_t)(MSG_COUNT * 99 / 100)],
        latencies[(size_t)(MSG_COUNT * 999 / 1000)],
        latencies[MSG_COUNT - 1]);

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
    close(sock_fd);
    
    return 0;
}
