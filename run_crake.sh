#!/bin/bash
# run_crake.sh — build Crake and detect cycles in a graph, end to end.
#
#   ./run_crake.sh GRAPH_FILE [options]
#
# Options:
#   -k, --k K          detect cycles up to length K          (default 4)
#   -m, --clients M    data holders to split the graph over  (default 3)
#   -p, --port P       base TCP port                         (default 15000)
#       --verify       check the result against a brute-force enumeration
#                      (exponential in K — small graphs only)
#       --no-build     skip cmake/make, use the existing build
#   -h, --help
#
# Graph format, one edge per line, vertex ids 1..n, no self-loops:
#   1 2
#   2 3
#   3 1
#
# What it does:
#   1. builds crake_holder and crake_protocol
#   2. runs crake_holder  — the M data holders secret-share their subgraphs
#      (arithmetic RSS over Z_2^64; this is Step 0 and is offline, so it is not
#      timed, exactly as Oryx's graph_holder is not timed)
#   3. launches the three computing servers as three local processes
#   4. streams party 0's output; all three logs are kept
#
# Console output mirrors Oryx's so the two can be diffed line for line.

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

K=4
CLIENTS=3
PORT=15000
VERIFY=""
DO_BUILD=1
GRAPH=""

usage() { sed -n '2,28p' "${BASH_SOURCE[0]}" | sed 's/^# \?//'; exit "${1:-0}"; }

while [ $# -gt 0 ]; do
    case "$1" in
        -k|--k)        K="$2"; shift 2 ;;
        -m|--clients)  CLIENTS="$2"; shift 2 ;;
        -p|--port)     PORT="$2"; shift 2 ;;
        --verify)      VERIFY="--verify"; shift ;;
        --no-build)    DO_BUILD=0; shift ;;
        -h|--help)     usage 0 ;;
        -*)            echo "unknown option: $1" >&2; usage 1 ;;
        *)             GRAPH="$1"; shift ;;
    esac
done

[ -n "$GRAPH" ] || { echo "error: no graph file given" >&2; usage 1; }
[ -f "$GRAPH" ] || { echo "error: no such graph file: $GRAPH" >&2; exit 1; }
GRAPH="$(cd "$(dirname "$GRAPH")" && pwd)/$(basename "$GRAPH")"

case "$K" in ''|*[!0-9]*) echo "error: --k must be an integer" >&2; exit 1 ;; esac
[ "$K" -ge 2 ] || { echo "error: --k must be at least 2" >&2; exit 1; }

BUILD="$ROOT/build"

# ── 1. build ────────────────────────────────────────────────────────────────
if [ "$DO_BUILD" -eq 1 ]; then
    echo "==> building"
    cmake -S "$ROOT" -B "$BUILD" > "$BUILD.cmake.log" 2>&1 || {
        echo "cmake failed; see $BUILD.cmake.log" >&2; tail -20 "$BUILD.cmake.log" >&2; exit 1; }
    cmake --build "$BUILD" --target crake_holder crake_protocol -j"$(nproc)" \
        > "$BUILD.make.log" 2>&1 || {
        echo "build failed; see $BUILD.make.log" >&2
        grep -E "error" "$BUILD.make.log" | head -20 >&2; exit 1; }
fi

HOLDER="$BUILD/crake/crake_holder"
SERVER="$BUILD/crake/crake_protocol"
for b in "$HOLDER" "$SERVER"; do
    [ -x "$b" ] || { echo "error: not built: $b (drop --no-build?)" >&2; exit 1; }
done

# ── 2. Step 0: the data holders share the graph ─────────────────────────────
WORK="$ROOT/crake-run/$(basename "${GRAPH%.*}")_k${K}_m${CLIENTS}"
rm -rf "$WORK"; mkdir -p "$WORK/shares"

echo "==> sharing the graph across $CLIENTS data holder(s)"
"$HOLDER" --graph "$GRAPH" --out "$WORK/shares" --clients "$CLIENTS" --k "$K" \
    > "$WORK/holder.log" 2>&1 || {
    echo "crake_holder failed; see $WORK/holder.log" >&2; tail -20 "$WORK/holder.log" >&2; exit 1; }
[ -f "$WORK/shares/g_p0.share" ] || {
    echo "crake_holder produced no shares; see $WORK/holder.log" >&2; exit 1; }
grep -E "^(warning|note)" "$WORK/holder.log" || true

# ── 3. run the three computing servers ──────────────────────────────────────
# Each party process spawns its own OpenMP pool; three of them on one host
# would oversubscribe badly, so cap threads per process.
if [ -z "${OMP_NUM_THREADS:-}" ]; then
    cores="$(nproc 2>/dev/null || echo 3)"
    export OMP_NUM_THREADS=$(( cores / 3 > 0 ? cores / 3 : 1 ))
fi

echo "==> running 3 computing servers (OMP_NUM_THREADS=$OMP_NUM_THREADS per party)"
echo

pids=()
for p in 0 1 2; do
    "$SERVER" --dir "$WORK/shares" --graph "$GRAPH" --pid "$p" \
              --clients "$CLIENTS" --k "$K" --port "$PORT" $VERIFY \
              --output "$WORK/party${p}.json" \
        > "$WORK/party${p}.log" 2>&1 &
    pids+=($!)
    sleep 1   # party 0 must be listening before the others dial in
done

# Party 0's log is the one worth watching; the other two are identical by
# construction (every party evaluates the same circuit).
tail -f --pid="${pids[0]}" -n +1 "$WORK/party0.log" 2>/dev/null &
tailpid=$!

status=0
for i in 0 1 2; do wait "${pids[$i]}" || status=1; done
kill "$tailpid" 2>/dev/null || true
wait "$tailpid" 2>/dev/null || true

echo
if [ "$status" -ne 0 ]; then
    echo "one or more parties failed; logs in $WORK" >&2
    exit "$status"
fi
echo "logs and per-party JSON in $WORK"
