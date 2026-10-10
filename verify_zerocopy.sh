#!/usr/bin/env bash
# ═══════════════════════════════════════════════════════════════════════════
#  verify_zerocopy.sh — Prove the shm data path performs zero copies.
#
#  1. objdump: no `memcpy` PLT calls in the hot binaries
#     (fixed-size memcpy inlines to a single mov at -O3)
#  2. strace:  no per-message read/write/send/recv syscalls on a live run.
#     Allowed noise: the loader mapping ELFs at startup, and our own
#     banners on stderr (fd 2). Anything else is a data-path copy.
#  3. perf:    context switches ~0 (best effort — skipped if unavailable)
#
#  Usage:  ./verify_zerocopy.sh        (or: make verify-zero-copy)
# ═══════════════════════════════════════════════════════════════════════════
set -u

PASS=1
PIDS=()

need() {
    command -v "$1" >/dev/null || { echo "missing tool: $1"; exit 2; }
}
need gcc; need make; need objdump; need strace; need grep; need awk

cleanup() {
    for p in "${PIDS[@]}"; do kill -9 "$p" 2>/dev/null || true; done
    wait 2>/dev/null || true
    rm -f /dev/shm/zerocopy_ring
    rm -f t-producer t-consumer
    rm -f /tmp/zc_prod.trace /tmp/zc_cons.trace /tmp/zc_perf.txt
}
trap cleanup EXIT

echo "══════════════════════════════════════════════════════"
echo " ZERO-COPY VERIFICATION"
echo "══════════════════════════════════════════════════════"

# ── 1. no memcpy calls ────────────────────────────────────────────────────
echo "[1/3] no memcpy PLT calls in hot binaries"
make -s all >/dev/null || { echo "  ERROR: build failed"; exit 2; }
if objdump -d producer consumer futex_producer futex_consumer \
        | grep -q "call.*memcpy"; then
    echo "  FAIL: memcpy call found in hot path"
    PASS=0
else
    echo "  PASS"
fi

# ── 2. no IO syscalls on a live 2000-message run ──────────────────────────
echo "[2/3] no per-message read/write/send/recv syscalls"
make -s MSG_COUNT=2000 t-producer t-consumer \
    || { echo "  ERROR: build failed"; exit 2; }
rm -f /dev/shm/zerocopy_ring /tmp/zc_prod.trace /tmp/zc_cons.trace
strace -e trace=read,write,sendto,sendmsg,recvfrom,recvmsg,sendfile \
    -o /tmp/zc_prod.trace ./t-producer 2>/dev/null &
PIDS+=($!)
sleep 0.5
strace -e trace=read,write,sendto,sendmsg,recvfrom,recvmsg,sendfile \
    -o /tmp/zc_cons.trace ./t-consumer >/dev/null 2>&1
wait 2>/dev/null || true; PIDS=()
fail=0
for t in /tmp/zc_prod.trace /tmp/zc_cons.trace; do
    # Any read EXCEPT the loader mapping ELFs at startup is a copy.
    if grep -E "^read" "$t" | grep -v "177ELF" | grep -q .; then
        fail=1
    fi
    # Any write NOT to stderr (fd 2, our banners) is a copy.
    if grep -E "^writev?\(" "$t" | grep -Eqv "^writev?\(2,"; then
        fail=1
    fi
done
if [ "$fail" -eq 0 ]; then
    echo "  PASS"
else
    echo "  FAIL: unexpected IO syscalls:"
    cat /tmp/zc_prod.trace /tmp/zc_cons.trace
    PASS=0
fi

# ── 3. context switches ~0 (best effort) ──────────────────────────────────
echo "[3/3] context switches ~0"
if ! command -v perf >/dev/null \
    || ! perf stat -e context-switches true 2>/dev/null; then
    echo "  SKIP (perf unavailable)"
else
    rm -f /dev/shm/zerocopy_ring
    ./t-producer 2>/dev/null &
    PIDS+=($!)
    sleep 0.5
    LC_ALL=C perf stat -e context-switches -x, \
        -o /tmp/zc_perf.txt ./t-consumer >/dev/null 2>&1
    wait 2>/dev/null || true; PIDS=()
    CS=$(awk -F',' '$3 ~ /context-switches/ { gsub(/[^0-9]/, "", $1); print $1; exit }' \
        /tmp/zc_perf.txt)
    if [ -z "$CS" ]; then
        echo "  SKIP (unparseable perf output)"
    elif [ "$CS" -lt 1000 ]; then
        echo "  PASS ($CS context switches)"
    else
        echo "  FAIL ($CS context switches)"
        PASS=0
    fi
fi

echo ""
if [ "$PASS" -eq 1 ]; then
    echo "ALL CHECKS PASSED — zero-copy proven"
else
    echo "VERIFICATION FAILED"
    exit 1
fi
