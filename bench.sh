#!/usr/bin/env bash
set -euo pipefail

# ═══════════════════════════════════════════════════════════════════════════
#  Zero-Copy SPSC IPC — Benchmark Script
#
#  Usage:
#    ./bench.sh              Full benchmark with CPU pinning
#    ./bench.sh --no-pin     Skip taskset
#    ./bench.sh --no-perf    Skip perf stat
# ═══════════════════════════════════════════════════════════════════════════

PIN=1
USE_PERF=1
for arg in "$@"; do
    case "$arg" in
        --no-pin)  PIN=0 ;;
        --no-perf) USE_PERF=0 ;;
    esac
done

# ── System Info ────────────────────────────────────────────────────────────
echo "══════════════════════════════════════════════════════════════════"
echo " ZERO-COPY SHARED-MEMORY IPC — BENCHMARK"
echo "══════════════════════════════════════════════════════════════════"
echo ""
if [[ -f /proc/cpuinfo ]]; then
    CPU=$(awk -F': ' '/model name/{print $2; exit}' /proc/cpuinfo)
    CORES=$(grep -c ^processor /proc/cpuinfo)
else
    CPU="Unknown"; CORES="?"
fi
echo "  Kernel:  $(uname -r)"
echo "  CPU:     $CPU ($CORES cores)"
echo "  Pinning: $([ $PIN -eq 1 ] && echo 'ON (core 0 + core 1)' || echo 'OFF')"

# ── Check perf permissions ─────────────────────────────────────────────────
if [[ $USE_PERF -eq 1 ]]; then
    if ! command -v perf &>/dev/null; then
        echo "  perf:    NOT FOUND — running without hardware counters"
        USE_PERF=0
    elif ! perf stat -e cycles true 2>/dev/null; then
        echo "  perf:    PERMISSION DENIED — running without hardware counters"
        echo "           (fix: sudo sysctl kernel.perf_event_paranoid=-1)"
        USE_PERF=0
    else
        echo "  perf:    OK"
    fi
fi
echo ""

# ── Build if needed ────────────────────────────────────────────────────────
for bin in producer consumer futex_producer futex_consumer baseline_producer baseline_consumer; do
    if [[ ! -x "./$bin" ]]; then
        echo "Building..."
        make BENCH=1 all
        break
    fi
done

# ── Cleanup trap ───────────────────────────────────────────────────────────
BG_PIDS=()
TMPFILES=()
cleanup() {
    for pid in "${BG_PIDS[@]}"; do
        kill -9 "$pid" 2>/dev/null || true
        wait "$pid" 2>/dev/null || true
    done
    for f in "${TMPFILES[@]}"; do
        rm -f "$f"
    done
}
trap cleanup EXIT

# ── Helper: wrap command with optional taskset ─────────────────────────────
pin() {
    local core=$1; shift
    if [[ $PIN -eq 1 ]] && command -v taskset &>/dev/null; then
        taskset -c "$core" "$@"
    else
        "$@"
    fi
}

# ── Run one benchmark variant ──────────────────────────────────────────────
run_variant() {
    local label="$1"
    local prod="$2"
    local cons="$3"
    local out_file="$4"
    local perf_file="$5"

    echo "────────────────────────────────────────────────────────────────"
    echo "  ▶ $label"
    echo "────────────────────────────────────────────────────────────────"

    # Clean stale shm
    rm -f /dev/shm/zerocopy_ring 2>/dev/null || true

    # Start producer in background (show its stderr so errors are visible)
    pin 0 ./"$prod" &
    local ppid=$!
    BG_PIDS+=("$ppid")

    sleep 0.3

    # Start consumer (capture stderr for parsing, show it on screen too via tee)
    local tmp=$(mktemp)
    TMPFILES+=("$tmp")

    if [[ $USE_PERF -eq 1 ]]; then
        local ptmp=$(mktemp)
        TMPFILES+=("$ptmp")
        pin 1 perf stat -e context-switches,page-faults,cpu-migrations,cache-misses \
            -o "$ptmp" ./"$cons" 2>&1 | tee "$tmp"
        cp "$ptmp" "$perf_file"
    else
        pin 1 ./"$cons" 2>&1 | tee "$tmp"
    fi

    cp "$tmp" "$out_file"

    # Wait for producer to finish
    wait "$ppid" 2>/dev/null || true
    echo ""
}

# ── Storage for results ────────────────────────────────────────────────────
OUTDIR=$(mktemp -d)
TMPFILES+=("$OUTDIR")

run_variant "Zero-Copy (Spin)"    producer          consumer          "$OUTDIR/zc.txt"   "$OUTDIR/zc_perf.txt"
run_variant "Futex Hybrid"        futex_producer     futex_consumer    "$OUTDIR/fx.txt"   "$OUTDIR/fx_perf.txt"
run_variant "UDS Baseline"        baseline_producer  baseline_consumer "$OUTDIR/uds.txt"  "$OUTDIR/uds_perf.txt"

# ── Parse results ──────────────────────────────────────────────────────────
get_lat() {
    # $1 = file, $2 = keyword (e.g. "min", "p50")
    grep -i "$2" "$1" 2>/dev/null | grep -oP '[0-9]+\.?[0-9]*(?=\s*ns)' | head -1 || echo "N/A"
}

get_perf() {
    # $1 = file, $2 = counter name
    grep "$2" "$1" 2>/dev/null | grep -oP '[\d,]+' | head -1 | tr -d ',' || echo "N/A"
}

# ── Print comparison table ─────────────────────────────────────────────────
echo ""
echo "══════════════════════════════════════════════════════════════════"
echo " RESULTS — SIDE-BY-SIDE COMPARISON"
echo "══════════════════════════════════════════════════════════════════"
echo ""

ZC="$OUTDIR/zc.txt";  ZCP="$OUTDIR/zc_perf.txt"
FX="$OUTDIR/fx.txt";  FXP="$OUTDIR/fx_perf.txt"
UD="$OUTDIR/uds.txt"; UDP="$OUTDIR/uds_perf.txt"

printf "  %-14s  %14s  %14s  %14s\n" "METRIC" "ZERO-COPY" "FUTEX" "UDS BASELINE"
printf "  %-14s  %14s  %14s  %14s\n" "--------------" "--------------" "--------------" "--------------"

for m in min mean p50 p90 p99 "p99.9" max; do
    printf "  %-14s  %11s ns  %11s ns  %11s ns\n" \
        "lat $m" "$(get_lat "$ZC" "$m")" "$(get_lat "$FX" "$m")" "$(get_lat "$UD" "$m")"
done

if [[ $USE_PERF -eq 1 ]]; then
    printf "  %-14s  %14s  %14s  %14s\n" "--------------" "--------------" "--------------" "--------------"
    for c in context-switches page-faults cache-misses; do
        printf "  %-14s  %14s  %14s  %14s\n" \
            "$c" "$(get_perf "$ZCP" "$c")" "$(get_perf "$FXP" "$c")" "$(get_perf "$UDP" "$c")"
    done
fi

echo ""
echo "══════════════════════════════════════════════════════════════════"
echo ""
echo "  Key takeaways:"
echo "    • Zero-Copy latency should be 50-100× lower than UDS"
echo "    • Zero-Copy context switches should be ~0 vs millions for UDS"
echo "    • Futex hybrid trades ~1-2× latency for CPU efficiency"
echo ""
