#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <errno.h>

#define MSG_COUNT 10000000ULL
#define MSG_SIZE  4096u
#define SOCK_PATH "/tmp/zerocopy_baseline.sock"
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
    strncpy(addr.sun_path, SOCK_PATH, sizeof(addr.sun_path) - 1);

    int retries = 0;
    while (connect(sock_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
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
                fprintf(stderr, "[baseline-consumer] ERROR on seq %llu: got seq=%llu\n", expected_seq, seq);
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

    printf("[baseline-consumer] Done in %.3f s\n", elapsed_sec);
    printf("[baseline-consumer] Throughput: %.3f M msg/s\n", m_msg_s);
    printf("[baseline-consumer] Bandwidth:  %.3f Gbit/s\n", gbit_s);
    printf("[baseline-consumer] Latency (ns): Min: %llu | Mean: %.1f | p50: %llu | p90: %llu | p99: %llu | p99.9: %llu | Max: %llu\n",
           latencies[0], mean, 
           latencies[(uint64_t)(MSG_COUNT * 0.50)],
           latencies[(uint64_t)(MSG_COUNT * 0.90)],
           latencies[(uint64_t)(MSG_COUNT * 0.99)],
           latencies[(uint64_t)(MSG_COUNT * 0.999)],
           latencies[MSG_COUNT - 1]);

    if (errors > 0) {
        printf("[baseline-consumer] FAIL: %llu errors detected!\n", errors);
    } else {
        printf("[baseline-consumer] PASS: All %llu messages validated successfully.\n", MSG_COUNT);
    }

    free(latencies);
    close(sock_fd);
    
    return 0;
}
