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
#    make bench        Full benchmark via bench.sh
# ═══════════════════════════════════════════════════════════════════════════

CC       = gcc
# Standard hardening: stack protector, _FORTIFY_SOURCE, PIE, full RELRO.
# -march=native is benchmark-only: build with BENCH=1 to enable it
# (default build stays portable across x86-64 machines).
CFLAGS   = -std=c11 -O3 -Wall -Wextra -Wpedantic \
           -fstack-protector-strong -D_FORTIFY_SOURCE=2 -fPIE \
           -Werror=format-security
LDFLAGS  = -pie -Wl,-z,relro,-z,now -lrt

ifeq ($(BENCH),1)
CFLAGS += -march=native
endif

HEADER       = zerocopy.h
HEADER_FUTEX = zerocopy_futex.h

TARGETS = producer consumer \
          futex_producer futex_consumer \
          baseline_producer baseline_consumer

.PHONY: all clean test test-futex test-uds test-all bench

all: $(TARGETS)

# ── Zero-Copy (Spin) ──────────────────────────────────────────────────────

producer: producer.c $(HEADER)
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

consumer: consumer.c $(HEADER)
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

# ── Zero-Copy (Futex Hybrid) ──────────────────────────────────────────────

futex_producer: futex_producer.c $(HEADER_FUTEX) $(HEADER)
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

futex_consumer: futex_consumer.c $(HEADER_FUTEX) $(HEADER)
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

# ── UDS Baseline (Kernel Path) ────────────────────────────────────────────

baseline_producer: baseline_producer.c
	$(CC) $(CFLAGS) -o $@ $<

baseline_consumer: baseline_consumer.c
	$(CC) $(CFLAGS) -o $@ $<

# ── Cleanup ────────────────────────────────────────────────────────────────

clean:
	rm -f $(TARGETS)

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
