#!/usr/bin/env python3
"""Run network setup + grasp benchmark sweep, then generate result tables.

Workflow:
1) source network.sh and apply tc_lan 1ms 4Gbit
2) execute grasp/grasp_bench.sh
3) parse benchmark summaries and produce graph-size / party-count tables

This script intentionally runs shell scripts through bash to preserve behavior.
"""

from __future__ import annotations

import argparse
import subprocess
import sys
from pathlib import Path

from parse_results import (
    auto_detect_results_root,
    build_graph_size_table,
    build_party_count_table,
    write_csv,
    write_markdown_table,
)


def run_cmd(cmd: str, cwd: Path) -> None:
    print(f"$ {cmd}")
    subprocess.run(["bash", "-lc", cmd], cwd=str(cwd), check=True)


def apply_tc_lan(repo_root: Path, latency: str, bandwidth: str) -> None:
    network_sh = repo_root / "network.sh"
    if not network_sh.exists():
        raise FileNotFoundError(f"Missing network script: {network_sh}")

    # tc_lan is a shell function defined in network.sh, so source + call in bash.
    cmd = f"source {network_sh} && tc_lan {latency} {bandwidth}"
    run_cmd(cmd, cwd=repo_root)


def run_grasp_bench(repo_root: Path, mode: str) -> None:
    bench_sh = repo_root / "grasp" / "grasp_bench.sh"
    if not bench_sh.exists():
        raise FileNotFoundError(f"Missing benchmark script: {bench_sh}")

    # Use bash explicitly so executable bit is not required.
    cmd = f"bash {bench_sh} {mode}" if mode else f"bash {bench_sh}"
    run_cmd(cmd, cwd=repo_root)


def generate_tables(
    repo_root: Path,
    out_dir: Path,
    fixed_parties: int,
    fixed_size: int,
    sizes: list[int],
    party_counts: list[int],
    vert_mult: int,
    edge_mult: int,
    strict_missing: bool,
    results_root: Path | None,
) -> None:
    resolved_results_root = (
        results_root
        if results_root is not None
        else select_results_root_for_sweeps(
            repo_root,
            fixed_parties,
            fixed_size,
            sizes,
            party_counts,
            vert_mult,
            edge_mult,
        )
    )
    print(f"Using results root: {resolved_results_root}")

    graph_rows = build_graph_size_table(
        resolved_results_root,
        fixed_parties,
        sizes,
        vert_mult=vert_mult,
        edge_mult=edge_mult,
        strict_missing=strict_missing,
    )
    party_rows = build_party_count_table(
        resolved_results_root,
        fixed_size,
        party_counts,
        vert_mult=vert_mult,
        edge_mult=edge_mult,
        strict_missing=strict_missing,
    )

    write_csv(graph_rows, out_dir / "graph_size_table.csv")
    write_csv(party_rows, out_dir / "party_count_table.csv")
    write_markdown_table(graph_rows, out_dir / "graph_size_table.md", "Graph Size Sweep")
    write_markdown_table(party_rows, out_dir / "party_count_table.md", "Party Count Sweep")


    print(f"Wrote tables to: {out_dir}")


def _candidate_results_roots(repo_root: Path) -> list[Path]:
    return [
        repo_root / "Results" / "bench_dcc_pagerank_mpa" / "protocol_nph",
        repo_root / "benchmark" / "Results" / "bench_dcc_pagerank_mpa" / "protocol_nph",
    ]


def _coverage_score(
    root: Path,
    fixed_parties: int,
    fixed_size: int,
    sizes: list[int],
    party_counts: list[int],
    vert_mult: int,
    edge_mult: int,
) -> int:
    score = 0
    for size in sizes:
        num_verts = size * vert_mult
        num_edges = size * edge_mult
        if (root / f"parties_{fixed_parties}" / f"verts_{num_verts}" / f"edges_{num_edges}").exists():
            score += 1
    fixed_num_verts = fixed_size * vert_mult
    fixed_num_edges = fixed_size * edge_mult
    for num_parties in party_counts:
        if (root / f"parties_{num_parties}" / f"verts_{fixed_num_verts}" / f"edges_{fixed_num_edges}").exists():
            score += 1
    return score


def select_results_root_for_sweeps(
    repo_root: Path,
    fixed_parties: int,
    fixed_size: int,
    sizes: list[int],
    party_counts: list[int],
    vert_mult: int,
    edge_mult: int,
) -> Path:
    candidates = [p for p in _candidate_results_roots(repo_root) if p.exists()]
    if not candidates:
        return auto_detect_results_root(repo_root)

    best_root: Path | None = None
    best_score = -1
    for root in candidates:
        score = _coverage_score(
            root, fixed_parties, fixed_size, sizes, party_counts, vert_mult, edge_mult
        )
        if score > best_score:
            best_score = score
            best_root = root

    if best_root is None:
        return auto_detect_results_root(repo_root)

    return best_root


def main() -> int:
    parser = argparse.ArgumentParser(description="Run full grasp benchmark pipeline")
    parser.add_argument("--repo-root", default=".", help="CRiS-MPC repository root")
    parser.add_argument("--latency", default="1ms")
    parser.add_argument("--bandwidth", default="4Gbit")
    parser.add_argument(
        "--mode",
        default="all",
        choices=["all", "graph-sweep", "party-sweep"],
        help="Mode passed to grasp_bench.sh",
    )
    parser.add_argument("--fixed-parties", type=int, default=5)
    parser.add_argument("--fixed-size", type=int, default=65536)
    parser.add_argument("--sizes", default="4096,8192,16384,32768,65536")
    parser.add_argument("--party-counts", default="3,4,5,6,7,8,9,10")
    parser.add_argument("--vert-mult", type=int, default=5, help="num_vertices = size * vert-mult")
    parser.add_argument("--edge-mult", type=int, default=15, help="num_edges = size * edge-mult")
    parser.add_argument("--out-dir", default="grasp/results_tables")
    parser.add_argument(
        "--results-root",
        default="auto",
        help=(
            "Override results root path, or 'auto' to detect between "
            "Results/... and benchmark/Results/..."
        ),
    )
    parser.add_argument("--skip-network", action="store_true")
    parser.add_argument("--skip-run", action="store_true")
    parser.add_argument(
        "--strict-missing",
        action="store_true",
        help="Fail if any requested size or party count result is missing",
    )
    parser.add_argument(
        "--tc-off-at-end",
        action="store_true",
        help="Run tc_off at end (best effort)",
    )

    args = parser.parse_args()

    repo_root = Path(args.repo_root).resolve()
    out_dir = (repo_root / args.out_dir).resolve()
    results_root = None if args.results_root == "auto" else Path(args.results_root).resolve()

    sizes = [int(x) for x in args.sizes.split(",") if x.strip()]
    party_counts = [int(x) for x in args.party_counts.split(",") if x.strip()]

    try:
        if not args.skip_network:
            apply_tc_lan(repo_root, args.latency, args.bandwidth)

        if not args.skip_run:
            run_grasp_bench(repo_root, args.mode)

        generate_tables(
            repo_root=repo_root,
            out_dir=out_dir,
            fixed_parties=args.fixed_parties,
            fixed_size=args.fixed_size,
            sizes=sizes,
            party_counts=party_counts,
            vert_mult=args.vert_mult,
            edge_mult=args.edge_mult,
            strict_missing=args.strict_missing,
            results_root=results_root,
        )
    except subprocess.CalledProcessError as exc:
        print(f"Command failed with exit code {exc.returncode}: {exc.cmd}", file=sys.stderr)
        return exc.returncode
    except Exception as exc:  # pragma: no cover
        print(f"Error: {exc}", file=sys.stderr)
        return 1
    finally:
        if args.tc_off_at_end:
            try:
                run_cmd(f"source {repo_root / 'network.sh'} && tc_off", cwd=repo_root)
            except Exception as exc:  # pragma: no cover
                print(f"Warning: tc_off failed: {exc}", file=sys.stderr)

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
