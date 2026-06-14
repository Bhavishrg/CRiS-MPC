#!/bin/bash
# set -x

# Usage:
#   ./run.sh <benchmark_name> [benchmark_options...]
#
# Examples:
#   ./run.sh bench_propagate --vec-size 10 --num-groups 3
#   ./run.sh bench_ops --num-muls 100000
#   ./run.sh bench_propagate --vec-size 10 --num-groups 3 --port 13700 --peer 127.0.0.1
#
# Output:
#   Results/<benchmark_name>/<vec_size>/<num_groups>/<timestamp>/party_0.log
#   Results/<benchmark_name>/<vec_size>/<num_groups>/<timestamp>/party_1.log
#   Results/<benchmark_name>/<vec_size>/<num_groups>/<timestamp>/party_2.log

set -euo pipefail

if [ $# -lt 1 ]; then
    echo "Usage: $0 <benchmark_name> [benchmark_options...]"
    echo ""
    echo "Available benchmarks:"
    echo "  - bench_ops"
    echo "  - bench_propagate"
    echo "  - bench_unshuffle"
    echo "  - bench_sort"
    echo ""
    echo "Example:"
    echo "  $0 bench_propagate --vec-size 10 --num-groups 3"
    echo "  $0 bench_propagate --vec-size 10 --num-groups 3 --port 13700"
    exit 1
fi

BENCHMARK_NAME="$1"
shift

normalize_benchmark_args() {
    local benchmark_name="$1"
    shift

    local -a normalized=("$@")

    case "$benchmark_name" in
        bench_mult|bench_linear|bench_gate|bench_unshuffle)
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
    "./build/benchmark/$BENCHMARK_NAME"
    "./build/benchmarks/$BENCHMARK_NAME"
    "./benchmark/$BENCHMARK_NAME"
    "./benchmarks/$BENCHMARK_NAME"
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

NUM_PARTIES=3
DEFAULT_PORT=13700
DEFAULT_PEER="127.0.0.1"

# --------------------------------------------------------------------
# Extract options for directory naming and defaults
# --------------------------------------------------------------------

vec_size="unspecified_vec_size"
num_groups="unspecified_num_groups"
bit_width="unspecified_bit_width"
port="$DEFAULT_PORT"
peer="$DEFAULT_PEER"

has_port=0
has_peer=0

for ((i=0; i<${#BENCHMARK_OPTS[@]}; i++)); do
    case "${BENCHMARK_OPTS[$i]}" in
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

# Add default networking options if not provided.
if [ "$has_port" -eq 0 ]; then
    BENCHMARK_OPTS+=("--port" "$port")
fi

if [ "$has_peer" -eq 0 ]; then
    BENCHMARK_OPTS+=("--peer" "$peer")
fi

# --------------------------------------------------------------------
# Results directory
# --------------------------------------------------------------------

timestamp=$(date +"%Y%m%d_%H%M%S")

# You can override base results directory:
#   RESULTS_DIR=/tmp/my_results ./run.sh bench_propagate ...
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
RESULTS_BASE="${RESULTS_DIR:-$SCRIPT_DIR/benchmark/Results}"

case "$BENCHMARK_NAME" in
    bench_sort)
        logdir="$RESULTS_BASE/$BENCHMARK_NAME/vec_${vec_size}/bits_${bit_width}/$timestamp"
        ;;
    *)
        logdir="$RESULTS_BASE/$BENCHMARK_NAME/vec_${vec_size}/groups_${num_groups}/$timestamp"
        ;;
esac
mkdir -p "$logdir"

echo "Running benchmark: $BENCHMARK_NAME"
echo "Benchmark path:    $BENCHMARK_PATH"
echo "Number of parties: $NUM_PARTIES"
echo "Benchmark options: ${BENCHMARK_OPTS[*]}"
echo "Results directory: $logdir"
echo ""

# Save command metadata.
{
    echo "benchmark_name=$BENCHMARK_NAME"
    echo "benchmark_path=$BENCHMARK_PATH"
    echo "num_parties=$NUM_PARTIES"
    echo "options=${BENCHMARK_OPTS[*]}"
    echo "timestamp=$timestamp"
    echo "pwd=$PWD"
} > "$logdir/meta.txt"

# --------------------------------------------------------------------
# Run parties
# --------------------------------------------------------------------

declare -a pids

for party in $(seq 0 $((NUM_PARTIES - 1))); do
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
echo "All parties started."
echo ""

# --------------------------------------------------------------------
# Wait for all parties
# --------------------------------------------------------------------

status=0

for party in $(seq 0 $((NUM_PARTIES - 1))); do
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

    for party in $(seq 0 $((NUM_PARTIES - 1))); do
        log="$logdir/party_${party}.log"

        echo "----- party $party -----"

        grep -E "correctness|PASS|FAIL|offline|online|total|time|sent|recv|Peak" "$log" || true

        echo ""
    done
} > "$summary_file"

echo "Summary saved to: $summary_file"