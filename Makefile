# ═══════════════════════════════════════════════════════════════════════════
#  Zero-Copy SPSC IPC Framework — Build System
#
#  Targets:
#    make              Build all binaries
#    make clean        Remove all binaries
#    make test         Stress test: zero-copy spin (10M messages)
#    make test-futex   Stress test: futex hybrid   (10M messages)
#    make test-uds     Stress test: UDS baseline   (10M messages)
#    make test-all     Run all three stress tests sequentially
#    make test-unit    Correctness suite (wraparound, restart, kill, version, owner)
#    make verify-zero-copy  Prove zero-copy: no memcpy calls, no IO syscalls, ~0 ctx switches
#    make demo         Fast live showcase (spin + futex, 100k messages)
#    make bench        Full benchmark via bench.sh
#    make install      Install headers to $(PREFIX)/include
#
#  Build knobs:
#    make SLOT_COUNT=256 SLOT_SIZE=1024 MSG_COUNT=100000
#    make BENCH=1      (adds -march=native, benchmark builds only)
#    make MLOCK=1      (mlock the ring: no page faults, deterministic latency)
#    make CROSS_COMPILE=aarch64-linux-gnu-  (cross builds)
# ═══════════════════════════════════════════════════════════════════════════

CC       = $(CROSS_COMPILE)gcc
# Standard hardening: stack protector, _FORTIFY_SOURCE, PIE, full RELRO.
# -march=native is benchmark-only: build with BENCH=1 to enable it
# (default build stays portable across x86-64 machines).
CFLAGS   = -std=c11 -O3 -Wall -Wextra -Wpedantic \
           -fstack-protector-strong -D_FORTIFY_SOURCE=2 -fPIE \
           -Werror=format-security
LDFLAGS  = -pie -Wl,-z,relro,-z,now -lrt

SLOT_COUNT ?= 1024
SLOT_SIZE  ?= 4096
MSG_COUNT  ?= 10000000
# ULL suffix: MSG_COUNT must stay 64-bit (MSG_COUNT * 999 overflows int).
DEFS = -DZC_SLOT_COUNT=$(SLOT_COUNT) -DZC_SLOT_SIZE=$(SLOT_SIZE) \
       -DMSG_COUNT=$(MSG_COUNT)ULL

ifeq ($(BENCH),1)
CFLAGS += -march=native
endif

ifeq ($(MLOCK),1)
DEFS += -DZC_MLOCK
endif

HEADER       = zerocopy.h
HEADER_FUTEX = zerocopy_futex.h

TARGETS = producer consumer \
          futex_producer futex_consumer \
          baseline_producer baseline_consumer

TESTBINS = t-producer t-consumer t-futex_producer t-futex_consumer \
           t-baseline_producer t-baseline_consumer t-version

PREFIX ?= /usr/local

.PHONY: all clean test test-futex test-uds test-all test-unit verify-zero-copy demo bench install

all: $(TARGETS)

# ── Zero-Copy (Spin) ──────────────────────────────────────────────────────

producer: producer.c $(HEADER)
	$(CC) $(CFLAGS) $(DEFS) -o $@ $< $(LDFLAGS)

consumer: consumer.c $(HEADER)
	$(CC) $(CFLAGS) $(DEFS) -o $@ $< $(LDFLAGS)

# ── Zero-Copy (Futex Hybrid) ──────────────────────────────────────────────

futex_producer: futex_producer.c $(HEADER_FUTEX) $(HEADER)
	$(CC) $(CFLAGS) $(DEFS) -o $@ $< $(LDFLAGS)

futex_consumer: futex_consumer.c $(HEADER_FUTEX) $(HEADER)
	$(CC) $(CFLAGS) $(DEFS) -o $@ $< $(LDFLAGS)

# ── UDS Baseline (Kernel Path) ────────────────────────────────────────────

baseline_producer: baseline_producer.c
	$(CC) $(CFLAGS) $(DEFS) -o $@ $<

baseline_consumer: baseline_consumer.c
	$(CC) $(CFLAGS) $(DEFS) -o $@ $<

# ── Test Variants (t-*) — same sources, test-time knobs ───────────────────
# Usage: make SLOT_COUNT=16 MSG_COUNT=20000 t-producer t-consumer
# Always rebuilt: make does not track flag changes, so stale bins are
# never reused across different knob values.

FORCE:

t-%: %.c $(HEADER) $(HEADER_FUTEX) FORCE
	$(CC) $(CFLAGS) $(DEFS) -o $@ $< $(LDFLAGS)

t-version: test_version.c $(HEADER) FORCE
	$(CC) $(CFLAGS) $(DEFS) -o $@ $< $(LDFLAGS)

# ── Cleanup ────────────────────────────────────────────────────────────────

clean:
	rm -f $(TARGETS) $(TESTBINS)

# ── Stress Tests ───────────────────────────────────────────────────────────

test: producer consumer
	@echo "══════════════════════════════════════════════════════"
	@echo " Zero-Copy (Spin) — 10M messages"
	@echo "══════════════════════════════════════════════════════"
	./producer &
	sleep 0.2
	./consumer

test-futex: futex_producer futex_consumer
	@echo "══════════════════════════════════════════════════════"
	@echo " Zero-Copy (Futex Hybrid) — 10M messages"
	@echo "══════════════════════════════════════════════════════"
	./futex_producer &
	sleep 0.2
	./futex_consumer

test-uds: baseline_producer baseline_consumer
	@echo "══════════════════════════════════════════════════════"
	@echo " UDS Baseline (Kernel Path) — 10M messages"
	@echo "══════════════════════════════════════════════════════"
	./baseline_producer &
	sleep 0.2
	./baseline_consumer

test-all: all
	@echo ""
	@$(MAKE) test
	@echo ""
	@echo "────────────────────────────────────────────────────"
	@echo ""
	@$(MAKE) test-futex
	@echo ""
	@echo "────────────────────────────────────────────────────"
	@echo ""
	@$(MAKE) test-uds

# ── Full Benchmark ─────────────────────────────────────────────────────────

bench:
	$(MAKE) BENCH=1 all
	chmod +x bench.sh
	./bench.sh

# ── Live Demo (fast: 100k messages, spin + futex) ──────────────────────────

demo:
	$(MAKE) -s MSG_COUNT=100000 t-producer t-consumer t-futex_producer t-futex_consumer
	@echo "══════════════════════════════════════════════════════"
	@echo " DEMO — Zero-Copy (Spin), 100k messages"
	@echo "══════════════════════════════════════════════════════"
	rm -f /dev/shm/zerocopy_ring
	./t-producer 2>/dev/null &
	sleep 0.3
	./t-consumer 2>&1 | tail -25
	wait
	@echo ""
	@echo "══════════════════════════════════════════════════════"
	@echo " DEMO — Zero-Copy (Futex Hybrid), 100k messages"
	@echo "══════════════════════════════════════════════════════"
	rm -f /dev/shm/zerocopy_ring
	./t-futex_producer 2>/dev/null &
	sleep 0.3
	./t-futex_consumer 2>&1 | tail -25
	wait
	@echo ""
	@echo "UDS baseline (full 10M run): 0.20 M msg/s, 1420 ns min — see README."

# ── Correctness Suite ──────────────────────────────────────────────────────

test-unit:
	chmod +x test_unit.sh
	./test_unit.sh

# ── Zero-Copy Proof ────────────────────────────────────────────────────────

verify-zero-copy:
	chmod +x verify_zerocopy.sh
	./verify_zerocopy.sh

# ── Install (header-only library) ──────────────────────────────────────────

install: $(HEADER) $(HEADER_FUTEX)
	install -d $(DESTDIR)$(PREFIX)/include
	install -m 644 $(HEADER) $(HEADER_FUTEX) $(DESTDIR)$(PREFIX)/include/
