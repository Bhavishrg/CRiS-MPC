#!/bin/bash
# grasp/grasp_bench.sh
#
# Sweeps bench_dcc_pagerank_mpa (NPH protocol) over two axes. Graph shape is
# specified explicitly via --num-verts/--num-edges (V=size*5, E=size*15,
# i.e. a fixed 1:3 vertex:edge ratio) rather than --graph-size.
#
#   1. Size sweep: size in {4096, 8192, 16384, 32768, 65536} -> num-verts =
#      size*5, num-edges = size*15, with a fixed 5 compute parties.
#
#   2. Party-count sweep: --num-parties in {3, 4, ..., 10} with a fixed
#      size of 65536 (num-verts = 65536*5, num-edges = 65536*15).
#
# Each invocation is delegated to the repo's run.sh, which already takes
# care of process launching, OMP_NUM_THREADS capping, and saving results
# under benchmark/Results/bench_dcc_pagerank_mpa/protocol_nph/parties_<n>/
# verts_<V>/edges_<E>/<timestamp>/.
#
# Usage:
#   ./grasp/grasp_bench.sh
#   ./grasp/grasp_bench.sh graph-sweep   # only run sweep 1
#   ./grasp/grasp_bench.sh party-sweep   # only run sweep 2
#
# Environment overrides:
#   FIXED_PARTIES (default: 5, used for size sweep)
#   FIXED_SIZE    (default: 65536, used for party sweep)
#   VERT_MULT     (default: 5)
#   EDGE_MULT     (default: 15)

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
RUN_SH="$REPO_ROOT/run.sh"

NUM_ITERS=10
FIXED_PARTIES="${FIXED_PARTIES:-5}"
FIXED_SIZE="${FIXED_SIZE:-65536}"
VERT_MULT="${VERT_MULT:-5}"
EDGE_MULT="${EDGE_MULT:-15}"

SIZES=(4096 8192 16384 32768 65536)
PARTY_COUNTS=(3 4 5 6 7 8 9 10)

MODE="${1:-all}"

common_opts() {
    local -a opts=(--protocol nph --num-iters "$NUM_ITERS" --pking)
    printf '%s\n' "${opts[@]}"
}

run_graph_sweep() {
    echo "=== Size sweep: num-parties=$FIXED_PARTIES, size in ${SIZES[*]} (verts=size*${VERT_MULT}, edges=size*${EDGE_MULT}) ==="
    for size in "${SIZES[@]}"; do
        local num_verts=$((size * VERT_MULT))
        local num_edges=$((size * EDGE_MULT))
        mapfile -t opts < <(common_opts)
        echo ""
        echo "--- size=$size, num-verts=$num_verts, num-edges=$num_edges, num-parties=$FIXED_PARTIES ---"
        "$RUN_SH" bench_dcc_pagerank_mpa \
            "${opts[@]}" \
            --num-parties "$FIXED_PARTIES" \
            --num-verts "$num_verts" \
            --num-edges "$num_edges"
    done
}

run_party_sweep() {
    local num_verts=$((FIXED_SIZE * VERT_MULT))
    local num_edges=$((FIXED_SIZE * EDGE_MULT))
    echo "=== Party-count sweep: size=$FIXED_SIZE (verts=$num_verts, edges=$num_edges), num-parties in ${PARTY_COUNTS[*]} ==="
    for num_parties in "${PARTY_COUNTS[@]}"; do
        mapfile -t opts < <(common_opts)
        echo ""
        echo "--- num-verts=$num_verts, num-edges=$num_edges, num-parties=$num_parties ---"
        "$RUN_SH" bench_dcc_pagerank_mpa \
            "${opts[@]}" \
            --num-parties "$num_parties" \
            --num-verts "$num_verts" \
            --num-edges "$num_edges"
    done
}

case "$MODE" in
    graph-sweep)
        run_graph_sweep
        ;;
    party-sweep)
        run_party_sweep
        ;;
    all)
        run_graph_sweep
        run_party_sweep
        ;;
    *)
        echo "Usage: $0 [graph-sweep|party-sweep|all]"
        exit 1
        ;;
esac

echo ""
echo "All sweeps complete."
