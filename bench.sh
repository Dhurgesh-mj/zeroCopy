#!/usr/bin/env bash
set -euo pipefail

PIN_CORES=1
if [[ "${1:-}" == "--no-pin" ]]; then
    PIN_CORES=0
fi

if [[ -f /proc/cpuinfo ]]; then
    CPU_MODEL=$(awk -F': ' '/model name/ {print $2; exit}' /proc/cpuinfo)
    NUM_CORES=$(grep -c processor /proc/cpuinfo)
else
    CPU_MODEL="Unknown"
    NUM_CORES="Unknown"
fi
UNAME_R=$(uname -r || echo "Unknown")

echo "System Info:"
echo "  OS: Linux $UNAME_R"
echo "  CPU: $CPU_MODEL ($NUM_CORES cores)"
echo ""

# Cleanup
PIDS_TO_KILL=()
cleanup() {
    for pid in "${PIDS_TO_KILL[@]}"; do
        if kill -0 "$pid" 2>/dev/null; then
            kill -9 "$pid" 2>/dev/null || true
        fi
    done
}
trap cleanup EXIT

# Check binaries
BINARIES=("producer" "consumer" "futex_producer" "futex_consumer" "baseline_producer" "baseline_consumer")
NEED_BUILD=0
for bin in "${BINARIES[@]}"; do
    if [[ ! -x "./$bin" ]]; then
        NEED_BUILD=1
        break
    fi
done

if [[ $NEED_BUILD -eq 1 ]]; then
    echo "Building targets..."
    make all
fi

# perf check
USE_PERF=0
if command -v perf >/dev/null 2>&1; then
    USE_PERF=1
fi

declare -A RESULTS

extract_latency() {
    local keyword="$1"
    local file="$2"
    # Convert to lowercase and find the keyword, then extract the first number found
    awk -v kw="$keyword" 'tolower($0) ~ kw {match($0, /[0-9]+\.?[0-9]*/); if (RSTART>0) {print substr($0, RSTART, RLENGTH); exit}}' "$file"
}

run_bench() {
    local name="$1"
    local prod_bin="$2"
    local cons_bin="$3"
    
    echo "Running benchmark: $name"
    
    local tmp_err=$(mktemp)
    local perf_err=$(mktemp)
    
    local prod_cmd="./$prod_bin"
    local cons_cmd="./$cons_bin"
    
    if [[ $PIN_CORES -eq 1 ]]; then
        if command -v taskset >/dev/null 2>&1; then
            prod_cmd="taskset -c 0 ./$prod_bin"
            cons_cmd="taskset -c 1 ./$cons_bin"
        fi
    fi
    
    if [[ $USE_PERF -eq 1 ]]; then
        cons_cmd="perf stat -e context-switches,page-faults,cpu-migrations,cache-misses,instructions,cycles -o $perf_err $cons_cmd"
    fi
    
    $prod_cmd >/dev/null 2>&1 &
    local prod_pid=$!
    PIDS_TO_KILL+=($prod_pid)
    
    sleep 0.2
    
    # Run consumer (blocks until done)
    $cons_cmd 2> "$tmp_err"
    
    # Parse latency
    RESULTS["$name,lat min"]=$(extract_latency "min" "$tmp_err")
    RESULTS["$name,lat mean"]=$(extract_latency "mean" "$tmp_err")
    RESULTS["$name,lat p50"]=$(extract_latency "p50" "$tmp_err")
    RESULTS["$name,lat p99"]=$(extract_latency "p99" "$tmp_err")
    RESULTS["$name,lat p99.9"]=$(extract_latency "p99.9" "$tmp_err")
    RESULTS["$name,lat max"]=$(extract_latency "max" "$tmp_err")
    
    # Provide fallbacks if parsing failed
    for metric in "lat min" "lat mean" "lat p50" "lat p99" "lat p99.9" "lat max"; do
        if [[ -z "${RESULTS["$name,$metric"]}" ]]; then
            RESULTS["$name,$metric"]="N/A"
        fi
    done
    
    # Parse perf counters
    if [[ $USE_PERF -eq 1 ]]; then
        RESULTS["$name,ctx-switches"]=$(awk '/context-switches/ {gsub(/,/,"",$1); print $1; exit}' "$perf_err")
        RESULTS["$name,page-faults"]=$(awk '/page-faults/ {gsub(/,/,"",$1); print $1; exit}' "$perf_err")
        RESULTS["$name,cache-misses"]=$(awk '/cache-misses/ {gsub(/,/,"",$1); print $1; exit}' "$perf_err")
        
        # Fallbacks
        for metric in "ctx-switches" "page-faults" "cache-misses"; do
            if [[ -z "${RESULTS["$name,$metric"]}" ]]; then
                RESULTS["$name,$metric"]="N/A"
            fi
        done
    else
        RESULTS["$name,ctx-switches"]="N/A"
        RESULTS["$name,page-faults"]="N/A"
        RESULTS["$name,cache-misses"]="N/A"
    fi
    
    rm -f "$tmp_err" "$perf_err"
}

run_bench "Zero-Copy" "producer" "consumer"
run_bench "Futex Hybrid" "futex_producer" "futex_consumer"
run_bench "UDS Baseline" "baseline_producer" "baseline_consumer"

echo ""
echo "══════════════════════════════════════════════════════════════════"
echo " ZERO-COPY SHARED-MEMORY IPC — BENCHMARK RESULTS"
echo "══════════════════════════════════════════════════════════════════"
echo ""
echo "┌─────────────┬──────────────┬──────────────┬──────────────┐"
echo "│ Metric      │ Zero-Copy    │ Futex Hybrid │ UDS Baseline │"
echo "├─────────────┼──────────────┼──────────────┼──────────────┤"

printf "│ %-11s │ %10s ns │ %10s ns │ %10s ns │\n" "lat min" "${RESULTS["Zero-Copy,lat min"]}" "${RESULTS["Futex Hybrid,lat min"]}" "${RESULTS["UDS Baseline,lat min"]}"
printf "│ %-11s │ %10s ns │ %10s ns │ %10s ns │\n" "lat mean" "${RESULTS["Zero-Copy,lat mean"]}" "${RESULTS["Futex Hybrid,lat mean"]}" "${RESULTS["UDS Baseline,lat mean"]}"
printf "│ %-11s │ %10s ns │ %10s ns │ %10s ns │\n" "lat p50" "${RESULTS["Zero-Copy,lat p50"]}" "${RESULTS["Futex Hybrid,lat p50"]}" "${RESULTS["UDS Baseline,lat p50"]}"
printf "│ %-11s │ %10s ns │ %10s ns │ %10s ns │\n" "lat p99" "${RESULTS["Zero-Copy,lat p99"]}" "${RESULTS["Futex Hybrid,lat p99"]}" "${RESULTS["UDS Baseline,lat p99"]}"
printf "│ %-11s │ %10s ns │ %10s ns │ %10s ns │\n" "lat p99.9" "${RESULTS["Zero-Copy,lat p99.9"]}" "${RESULTS["Futex Hybrid,lat p99.9"]}" "${RESULTS["UDS Baseline,lat p99.9"]}"
printf "│ %-11s │ %10s ns │ %10s ns │ %10s ns │\n" "lat max" "${RESULTS["Zero-Copy,lat max"]}" "${RESULTS["Futex Hybrid,lat max"]}" "${RESULTS["UDS Baseline,lat max"]}"
echo "├─────────────┼──────────────┼──────────────┼──────────────┤"
printf "│ %-11s │ %12s │ %12s │ %12s │\n" "ctx-switch" "${RESULTS["Zero-Copy,ctx-switches"]}" "${RESULTS["Futex Hybrid,ctx-switches"]}" "${RESULTS["UDS Baseline,ctx-switches"]}"
printf "│ %-11s │ %12s │ %12s │ %12s │\n" "page-faults" "${RESULTS["Zero-Copy,page-faults"]}" "${RESULTS["Futex Hybrid,page-faults"]}" "${RESULTS["UDS Baseline,page-faults"]}"
printf "│ %-11s │ %12s │ %12s │ %12s │\n" "cache-misses" "${RESULTS["Zero-Copy,cache-misses"]}" "${RESULTS["Futex Hybrid,cache-misses"]}" "${RESULTS["UDS Baseline,cache-misses"]}"
echo "└─────────────┴──────────────┴──────────────┴──────────────┘"

echo ""
echo "### Interpretation ###"
echo "- Zero-Copy (Spin): Expect lowest latency, no context switches, but 100% CPU usage as it constantly polls."
echo "- Futex Hybrid: Slightly higher latency than spin, but much better CPU efficiency. Avoids context switches when uncontended, sleeps via futex on contention."
echo "- UDS Baseline: High latency due to kernel involvement (read/write syscalls) and high context switches on every message."
echo ""
