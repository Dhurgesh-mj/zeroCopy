#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <inttypes.h>
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

static int send_all(int fd, const void *buf, size_t len) {
    const char *p = buf;
    while (len > 0) {
        ssize_t ret = write(fd, p, len);
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

int main() {
    printf("[baseline-producer] Starting UDS benchmark producer...\n");
    
    int server_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (server_fd < 0) {
        perror("socket");
        return 1;
    }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, SOCK_PATH, sizeof(addr.sun_path) - 1);

    unlink(SOCK_PATH);

    if (bind(server_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("bind");
        return 1;
    }

    if (listen(server_fd, 1) < 0) {
        perror("listen");
        return 1;
    }

    printf("[baseline-producer] Waiting for consumer...\n");
    int client_fd = accept(server_fd, NULL, NULL);
    if (client_fd < 0) {
        perror("accept");
        return 1;
    }

    printf("[baseline-producer] Consumer connected, sending %" PRIu64 " messages...\n", (uint64_t)MSG_COUNT);

    uint64_t start_time = get_time_ns();

    char buffer[MSG_SIZE];
    for (uint64_t seq = 0; seq < MSG_COUNT; seq++) {
        uint64_t timestamp = get_time_ns();
        uint64_t checksum = seq ^ CHECKSUM_MAGIC;
        uint64_t guard = seq ^ GUARD_MAGIC;

        memcpy(buffer, &seq, sizeof(seq));
        memcpy(buffer + 8, &timestamp, sizeof(timestamp));
        memcpy(buffer + 16, &checksum, sizeof(checksum));
        
        memset(buffer + 24, 0, MSG_SIZE - 24 - 8);
        
        memcpy(buffer + MSG_SIZE - 8, &guard, sizeof(guard));

        if (send_all(client_fd, buffer, MSG_SIZE) < 0) {
            perror("send_all");
            break;
        }
    }

    uint64_t end_time = get_time_ns();
    double elapsed_sec = (double)(end_time - start_time) / 1e9;
    double m_msg_s = ((double)MSG_COUNT / 1e6) / elapsed_sec;
    double bytes_total = (double)MSG_COUNT * MSG_SIZE;
    double gbit_s = (bytes_total * 8.0 / 1e9) / elapsed_sec;

    printf("[baseline-producer] Done in %.3f s\n", elapsed_sec);
    printf("[baseline-producer] Throughput: %.3f M msg/s\n", m_msg_s);
    printf("[baseline-producer] Bandwidth:  %.3f Gbit/s\n", gbit_s);

    close(client_fd);
    close(server_fd);
    unlink(SOCK_PATH);
    
    return 0;
}
