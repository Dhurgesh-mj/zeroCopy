/*
 * test_version.c — Unit test: zc_open must reject a corrupt segment.
 *
 * Creates a ring, corrupts its metadata magic (simulating a stale build
 * or foreign segment), then verifies zc_open() fails with errno == EPROTO
 * instead of attaching to garbage.
 *
 * Build:  make t-version
 * Run:    ./t-version            (expects exit 0 + PASS)
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <errno.h>

#include "zerocopy.h"

int main(void)
{
    zc_handle_t h, h2;

    zc_destroy(ZC_SHM_NAME);
    if (zc_create(&h, ZC_SHM_NAME) != 0) {
        perror("[test-version] zc_create");
        return 2;
    }

    h.ring->meta.magic ^= 1u;   /* simulate a foreign/stale layout */

    if (zc_open(&h2, ZC_SHM_NAME) == 0) {
        printf("[test-version] FAIL: corrupt segment was accepted\n");
        zc_close(&h2);
        zc_close(&h);
        zc_destroy(ZC_SHM_NAME);
        return 1;
    }
    if (errno != EPROTO) {
        printf("[test-version] FAIL: expected EPROTO, got errno=%d\n", errno);
        zc_close(&h);
        zc_destroy(ZC_SHM_NAME);
        return 1;
    }

    printf("[test-version] PASS: corrupt segment rejected with EPROTO\n");
    zc_close(&h);
    zc_destroy(ZC_SHM_NAME);
    return 0;
}
