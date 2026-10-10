#!/usr/bin/env bash
# ═══════════════════════════════════════════════════════════════════════════
#  test_unit.sh — Correctness suite (fast: small rings, small counts)
#
#  1. wraparound   — 16 slots, 20000 msgs, checksums must hold
#  2. restart      — producer re-creates; fresh consumer attaches clean
#  3. peer death   — kill -9 each side; survivor must abort via watchdog
#  4. version      — corrupt metadata must be rejected with EPROTO
#  5. owner        — foreign-owned segment must be refused (EACCES)
#
#  Usage:  ./test_unit.sh        (or: make test-unit)
# ═══════════════════════════════════════════════════════════════════════════
set -u

PASS=0; FAIL=0; SKIP=0
PIDS=()

ok()   { echo "  PASS: $1"; PASS=$((PASS + 1)); }
bad()  { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); }
skipp(){ echo "  SKIP: $1"; SKIP=$((SKIP + 1)); }

need() {
    command -v "$1" >/dev/null || { echo "missing tool: $1"; exit 2; }
}
need gcc; need make; need timeout; need grep; need awk

cleanup() {
    for p in "${PIDS[@]}"; do kill -9 "$p" 2>/dev/null || true; done
    wait 2>/dev/null || true
    rm -f /dev/shm/zerocopy_ring
    rm -f t-producer t-consumer t-futex_producer t-futex_consumer t-version
    rm -f /tmp/zc_kill.log /tmp/zc_kill2.log /tmp/zc_owner.log
}
trap cleanup EXIT

echo "══════════════════════════════════════════════════════"
echo " UNIT TESTS"
echo "══════════════════════════════════════════════════════"

# ── 1. wraparound ─────────────────────────────────────────────────────────
echo "[1/5] wraparound — 16 slots, 20000 messages"
make -s SLOT_COUNT=16 MSG_COUNT=20000 t-producer t-consumer
rm -f /dev/shm/zerocopy_ring
timeout 60 ./t-producer 2>/dev/null & PIDS+=($!)
sleep 0.3
if timeout 60 ./t-consumer >/dev/null 2>&1; then
    ok "wraparound, checksums intact"
else
    bad "wraparound run failed"
fi
wait 2>/dev/null || true; PIDS=()

# ── 2. restart ────────────────────────────────────────────────────────────
echo "[2/5] producer restart — generation bump, fresh attach"
make -s MSG_COUNT=2000 t-producer t-consumer
rm -f /dev/shm/zerocopy_ring
timeout 60 ./t-producer 2>/dev/null & PIDS+=($!)
sleep 0.3
timeout 60 ./t-consumer >/dev/null 2>&1; r1=$?
wait 2>/dev/null || true; PIDS=()
timeout 60 ./t-producer 2>/dev/null & PIDS+=($!)
sleep 0.3
timeout 60 ./t-consumer >/dev/null 2>&1; r2=$?
wait 2>/dev/null || true; PIDS=()
if [ "$r1" -eq 0 ] && [ "$r2" -eq 0 ]; then
    ok "two consecutive runs clean"
else
    bad "restart (run1=$r1 run2=$r2)"
fi

# ── 3a. kill producer ─────────────────────────────────────────────────────
echo "[3/5] peer death — kill -9 producer, consumer must abort (not hang)"
make -s MSG_COUNT=100000000 t-producer t-consumer
rm -f /dev/shm/zerocopy_ring
./t-producer >/dev/null 2>&1 & PP=$!; PIDS+=($PP)
sleep 0.3
./t-consumer >/tmp/zc_kill.log 2>&1 & CP=$!; PIDS+=($CP)
sleep 2
kill -9 "$PP" 2>/dev/null || true
# Safety timer: proves the test bounded even if the watchdog regresses.
( sleep 60; kill -9 "$CP" 2>/dev/null ) & KILLER=$!; PIDS+=($KILLER)
wait "$CP"; rc=$?
kill "$KILLER" 2>/dev/null || true
wait "$PP" 2>/dev/null || true
if [ "$rc" -eq 1 ] && grep -q "presumed dead" /tmp/zc_kill.log; then
    ok "consumer aborted via stall detection"
elif [ "$rc" -eq 137 ]; then
    bad "consumer hung after producer death (safety timer fired)"
else
    bad "consumer exited rc=$rc without stall message"
fi
PIDS=()

# ── 3b. kill consumer ─────────────────────────────────────────────────────
echo "[3b/5] peer death — kill -9 consumer, producer must abort (not hang)"
rm -f /dev/shm/zerocopy_ring
./t-producer >/tmp/zc_kill2.log 2>&1 & PP=$!; PIDS+=($PP)
sleep 0.3
./t-consumer >/dev/null 2>&1 & CP=$!; PIDS+=($CP)
sleep 2
kill -9 "$CP" 2>/dev/null || true
( sleep 60; kill -9 "$PP" 2>/dev/null ) & KILLER=$!; PIDS+=($KILLER)
wait "$PP"; rc=$?
kill "$KILLER" 2>/dev/null || true
wait "$CP" 2>/dev/null || true
if [ "$rc" -eq 1 ] && grep -q "presumed dead" /tmp/zc_kill2.log; then
    ok "producer aborted via stall detection"
elif [ "$rc" -eq 137 ]; then
    bad "producer hung after consumer death (safety timer fired)"
else
    bad "producer exited rc=$rc without stall message"
fi
wait 2>/dev/null || true; PIDS=()

# ── 4. corrupt metadata ───────────────────────────────────────────────────
echo "[4/5] corrupt metadata — attach must fail EPROTO"
make -s t-version
if ./t-version; then
    ok "EPROTO on corrupt segment"
else
    bad "version check"
fi

# ── 5. foreign owner ──────────────────────────────────────────────────────
echo "[5/5] foreign-owned segment — attach must fail EACCES"
make -s MSG_COUNT=2000 t-producer t-consumer
rm -f /dev/shm/zerocopy_ring
timeout 60 ./t-producer 2>/dev/null & PP=$!; PIDS+=($PP)
sleep 0.5
if ! command -v unshare >/dev/null; then
    skipp "unshare unavailable"
elif ! unshare -U -r true 2>/dev/null; then
    skipp "user namespaces unavailable"
else
    # Inside a user ns we are uid 0 while the segment belongs to the real
    # uid -> the ownership check must refuse with EACCES.
    if unshare -U -r ./t-consumer >/tmp/zc_owner.log 2>&1; then
        bad "foreign attach was accepted"
    else
        ok "EACCES on foreign-owned segment"
    fi
fi
kill -9 "$PP" 2>/dev/null || true
wait 2>/dev/null || true; PIDS=()

echo ""
echo "══════════════════════════════════════════════════════"
echo " unit: $PASS passed, $FAIL failed, $SKIP skipped"
echo "══════════════════════════════════════════════════════"
[ "$FAIL" -eq 0 ]
