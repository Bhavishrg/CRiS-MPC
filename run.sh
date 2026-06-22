#!/bin/bash
# set -x

# Usage:
#   ./run.sh <benchmark_name> [benchmark_options...]
#
# Protocol options:
#   --protocol rss3                 # existing 3-party RSS backend, default
#   --protocol nph --num-parties n  # n compute parties + helper pid n
#
# Examples:
#   ./run.sh bench_gate --protocol rss3 --gate mul --x 10 --y 20 --vec-size 1000
#   ./run.sh bench_gate --protocol nph --num-parties 5 --gate mul --x 10 --y 20 --vec-size 1000
#   ./run.sh bench_sort --vec-size 10 --bit-width 3

set -euo pipefail

if [ $# -lt 1 ]; then
    echo "Usage: $0 <benchmark_name> [benchmark_options...]"
    echo ""
    echo "Available benchmarks:"
    echo "  - bench_gate"
    echo "  - bench_linear"
    echo "  - bench_mult"
    echo "  - bench_permsh"
    echo "  - bench_propagate"
    echo "  - bench_unshuffle"
    echo "  - bench_sort"
    echo ""
    echo "Examples:"
    echo "  $0 bench_gate --protocol rss3 --gate mul --x 10 --y 20 --vec-size 1000"
    echo "  $0 bench_gate --protocol nph --num-parties 5 --gate mul --x 10 --y 20 --vec-size 1000"
    exit 1
fi

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$SCRIPT_DIR"

BENCHMARK_NAME="$1"
shift

normalize_benchmark_args() {
    local benchmark_name="$1"
    shift

    local -a normalized=("$@")

    case "$benchmark_name" in
        bench_mult|bench_linear|bench_gate|bench_permsh|bench_unshuffle)
            if [ ${#normalized[@]} -gt 0 ] && [[ ! "${normalized[0]}" =~ ^-- ]]; then
                normalized=("--vec-size" "${normalized[0]}" "${normalized[@]:1}")
            fi
            ;;
        bench_sort)
            if [ ${#normalized[@]} -gt 0 ] && [[ ! "${normalized[0]}" =~ ^-- ]]; then
                normalized=("--vec-size" "${normalized[0]}" "${normalized[@]:1}")
            fi

            if [ ${#normalized[@]} -gt 2 ] && [ "${normalized[2]}" != "--bit-width" ] && [[ ! "${normalized[2]}" =~ ^-- ]]; then
                normalized=("${normalized[@]:0:2}" "--bit-width" "${normalized[2]}" "${normalized[@]:3}")
            fi
            ;;
        bench_propagate)
            if [ ${#normalized[@]} -gt 0 ] && [[ ! "${normalized[0]}" =~ ^-- ]]; then
                normalized=("--vec-size" "${normalized[0]}" "${normalized[@]:1}")
            fi

            if [ ${#normalized[@]} -gt 2 ] && [ "${normalized[2]}" != "--num-groups" ] && [[ ! "${normalized[2]}" =~ ^-- ]]; then
                normalized=("${normalized[@]:0:2}" "--num-groups" "${normalized[2]}" "${normalized[@]:3}")
            fi
            ;;
    esac

    printf '%s\n' "${normalized[@]}"
}

# --------------------------------------------------------------------
# Locate benchmark binary
# --------------------------------------------------------------------

CANDIDATE_PATHS=(
    "$PWD/benchmark/$BENCHMARK_NAME"
    "$PWD/benchmarks/$BENCHMARK_NAME"
    "$PWD/build/benchmark/$BENCHMARK_NAME"
    "$PWD/build/benchmarks/$BENCHMARK_NAME"
    "$REPO_ROOT/build/benchmark/$BENCHMARK_NAME"
    "$REPO_ROOT/build/benchmarks/$BENCHMARK_NAME"
    "$REPO_ROOT/benchmark/$BENCHMARK_NAME"
    "$REPO_ROOT/benchmarks/$BENCHMARK_NAME"
)

BENCHMARK_PATH=""

for path in "${CANDIDATE_PATHS[@]}"; do
    if [ -x "$path" ]; then
        BENCHMARK_PATH="$path"
        break
    fi
done

if [ -z "$BENCHMARK_PATH" ]; then
    echo "Error: Benchmark '$BENCHMARK_NAME' not found."
    echo ""
    echo "Tried:"
    for path in "${CANDIDATE_PATHS[@]}"; do
        echo "  $path"
    done
    echo ""
    echo "Build first, for example:"
    echo "  cmake --build build -j"
    exit 1
fi

mapfile -t BENCHMARK_OPTS < <(normalize_benchmark_args "$BENCHMARK_NAME" "$@")

# --------------------------------------------------------------------
# Defaults
# --------------------------------------------------------------------

DEFAULT_PROTOCOL="rss3"
DEFAULT_NUM_PARTIES=3       # compute parties; nph additionally runs helper pid n
DEFAULT_PORT=13700
DEFAULT_PEER="127.0.0.1"

# --------------------------------------------------------------------
# Extract options for directory naming and defaults
# --------------------------------------------------------------------

protocol="$DEFAULT_PROTOCOL"
num_parties="$DEFAULT_NUM_PARTIES"
vec_size="unspecified_vec_size"
num_groups="unspecified_num_groups"
bit_width="unspecified_bit_width"
port="$DEFAULT_PORT"
peer="$DEFAULT_PEER"

has_protocol=0
has_num_parties=0
has_port=0
has_peer=0

for ((i=0; i<${#BENCHMARK_OPTS[@]}; i++)); do
    case "${BENCHMARK_OPTS[$i]}" in
        --protocol)
            if (( i + 1 < ${#BENCHMARK_OPTS[@]} )); then
                protocol="${BENCHMARK_OPTS[$((i+1))]}"
                has_protocol=1
            fi
            ;;
        --num-parties)
            if (( i + 1 < ${#BENCHMARK_OPTS[@]} )); then
                num_parties="${BENCHMARK_OPTS[$((i+1))]}"
                has_num_parties=1
            fi
            ;;
        --vec-size|-v)
            if (( i + 1 < ${#BENCHMARK_OPTS[@]} )); then
                vec_size="${BENCHMARK_OPTS[$((i+1))]}"
            fi
            ;;
        --num-groups|-g)
            if (( i + 1 < ${#BENCHMARK_OPTS[@]} )); then
                num_groups="${BENCHMARK_OPTS[$((i+1))]}"
            fi
            ;;
        --bit-width|-b)
            if (( i + 1 < ${#BENCHMARK_OPTS[@]} )); then
                bit_width="${BENCHMARK_OPTS[$((i+1))]}"
            fi
            ;;
        --port)
            if (( i + 1 < ${#BENCHMARK_OPTS[@]} )); then
                port="${BENCHMARK_OPTS[$((i+1))]}"
                has_port=1
            fi
            ;;
        --peer)
            if (( i + 1 < ${#BENCHMARK_OPTS[@]} )); then
                peer="${BENCHMARK_OPTS[$((i+1))]}"
                has_peer=1
            fi
            ;;
    esac
done

# Add default protocol/networking options if not provided.
if [ "$has_protocol" -eq 0 ]; then
    BENCHMARK_OPTS+=("--protocol" "$protocol")
fi

if [ "$has_num_parties" -eq 0 ]; then
    BENCHMARK_OPTS+=("--num-parties" "$num_parties")
fi

if [ "$has_port" -eq 0 ]; then
    BENCHMARK_OPTS+=("--port" "$port")
fi

if [ "$has_peer" -eq 0 ]; then
    BENCHMARK_OPTS+=("--peer" "$peer")
fi

case "$protocol" in
    rss3|3pc|rss)
        NUM_PROCESSES=3
        num_parties=3
        ;;
    nph|nparty-helper|np-helper)
        if ! [[ "$num_parties" =~ ^[0-9]+$ ]] || [ "$num_parties" -lt 2 ]; then
            echo "Error: --num-parties for nph must be an integer >= 2."
            exit 1
        fi
        NUM_PROCESSES=$((num_parties + 1))
        ;;
    *)
        echo "Error: unknown protocol '$protocol'. Use rss3 or nph."
        exit 1
        ;;
esac

# --------------------------------------------------------------------
# Results directory
# --------------------------------------------------------------------

timestamp=$(date +"%Y%m%d_%H%M%S")

# You can override base results directory:
#   RESULTS_DIR=/tmp/my_results ./run.sh bench_gate ...
# By default, store under <repo-root>/benchmark/Results.
RESULTS_BASE="${RESULTS_DIR:-$REPO_ROOT/benchmark/Results}"

case "$BENCHMARK_NAME" in
    bench_sort)
        shape_dir="vec_${vec_size}/bits_${bit_width}"
        ;;
    *)
        shape_dir="vec_${vec_size}/groups_${num_groups}"
        ;;
esac

logdir="$RESULTS_BASE/$BENCHMARK_NAME/protocol_${protocol}/parties_${num_parties}/$shape_dir/$timestamp"
mkdir -p "$logdir"

echo "Running benchmark: $BENCHMARK_NAME"
echo "Benchmark path:    $BENCHMARK_PATH"
echo "Protocol:          $protocol"
echo "Compute parties:   $num_parties"
echo "Processes:         $NUM_PROCESSES"
if [[ "$protocol" == "nph" || "$protocol" == "nparty-helper" || "$protocol" == "np-helper" ]]; then
    echo "Helper pid:        $num_parties"
fi
echo "Benchmark options: ${BENCHMARK_OPTS[*]}"
echo "Results directory: $logdir"
echo ""

# Save command metadata.
{
    echo "benchmark_name=$BENCHMARK_NAME"
    echo "benchmark_path=$BENCHMARK_PATH"
    echo "protocol=$protocol"
    echo "num_compute_parties=$num_parties"
    echo "num_processes=$NUM_PROCESSES"
    echo "options=${BENCHMARK_OPTS[*]}"
    echo "timestamp=$timestamp"
    echo "called_from=$PWD"
    echo "repo_root=$REPO_ROOT"
} > "$logdir/meta.txt"

# --------------------------------------------------------------------
# Run processes
# --------------------------------------------------------------------

declare -a pids

for party in $(seq 0 $((NUM_PROCESSES - 1))); do
    log="$logdir/party_${party}.log"

    echo "Starting party $party ..."
    echo "  log: $log"

    "$BENCHMARK_PATH" \
        --pid "$party" \
        "${BENCHMARK_OPTS[@]}" \
        2>&1 | tee "$log" &

    pids[$party]=$!
done

echo ""
echo "All processes started."
echo ""

# --------------------------------------------------------------------
# Wait for all processes
# --------------------------------------------------------------------

status=0

for party in $(seq 0 $((NUM_PROCESSES - 1))); do
    if wait "${pids[$party]}"; then
        echo "Party $party completed successfully."
    else
        echo "Party $party failed."
        status=1
    fi
done

echo ""

if [ "$status" -ne 0 ]; then
    echo "Benchmark failed. Logs saved to: $logdir"
    exit "$status"
fi

echo "Benchmark execution completed."
echo "Logs saved to: $logdir"
echo ""

# --------------------------------------------------------------------
# Optional: simple summary extraction
# --------------------------------------------------------------------

summary_file="$logdir/summary.txt"

{
    echo "Summary for $BENCHMARK_NAME"
    echo "Directory: $logdir"
    echo ""

    for party in $(seq 0 $((NUM_PROCESSES - 1))); do
        log="$logdir/party_${party}.log"

        echo "----- party $party -----"
        grep -E "correctness|PASS|FAIL|SKIP|offline|online|total|time|sent|recv|Peak" "$log" || true
        echo ""
    done
} > "$summary_file"

echo "Summary saved to: $summary_file"
