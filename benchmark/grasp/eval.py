#!/usr/bin/env python3
"""Run GraSP evaluation suites and generate result tables.

With --comparison-ringsg, this script:
1) applies the requested network profile;
2) invokes run.sh directly for the graph-size and party-count sweeps; and
3) generates graph-size and party-count tables.

Raw logs are stored below benchmark/grasp/Results/comparison_ringsg and tables
below benchmark/grasp/Results/Tables by default.
"""

from __future__ import annotations

import argparse
import os
import shlex
import subprocess
import sys
from pathlib import Path

from parse_results import (
    build_graph_size_table,
    build_party_count_table,
    write_csv,
    write_markdown_table,
)


def run_cmd(cmd: str, cwd: Path) -> None:
    print(f"$ {cmd}")
    subprocess.run(["bash", "-lc", cmd], cwd=str(cwd), check=True)


def run_process(args: list[str], cwd: Path, env: dict[str, str] | None = None) -> None:
    print(f"$ {shlex.join(args)}")
    subprocess.run(args, cwd=str(cwd), env=env, check=True)


def apply_tc_lan(repo_root: Path, latency: str, bandwidth: str) -> None:
    network_sh = repo_root / "network.sh"
    if not network_sh.exists():
        raise FileNotFoundError(f"Missing network script: {network_sh}")

    # tc_lan is a shell function defined in network.sh, so source + call in bash.
    cmd = f"source {network_sh} && tc_lan {latency} {bandwidth}"
    run_cmd(cmd, cwd=repo_root)


def run_comparison_ringsg(
    repo_root: Path,
    results_dir: Path,
    mode: str,
    fixed_parties: int,
    fixed_size: int,
    sizes: list[int],
    party_counts: list[int],
    vert_mult: int,
    edge_mult: int,
    num_iters: int,
) -> None:
    run_sh = repo_root / "run.sh"
    if not run_sh.exists():
        raise FileNotFoundError(f"Missing benchmark launcher: {run_sh}")

    results_dir.mkdir(parents=True, exist_ok=True)
    env = os.environ.copy()
    env["RESULTS_DIR"] = str(results_dir)

    def run_case(num_parties: int, size: int) -> None:
        num_verts = size * vert_mult
        num_edges = size * edge_mult
        run_process(
            [
                str(run_sh),
                "bench_dcc_pagerank_mpa",
                "--protocol", "nph",
                "--num-iters", str(num_iters),
                "--pking",
                "--num-parties", str(num_parties),
                "--num-verts", str(num_verts),
                "--num-edges", str(num_edges),
            ],
            cwd=repo_root,
            env=env,
        )

    if mode in ("all", "graph-sweep"):
        print(
            f"=== Graph-size sweep: parties={fixed_parties}, "
            f"sizes={sizes}, vert_mult={vert_mult}, edge_mult={edge_mult} ==="
        )
        for size in sizes:
            run_case(fixed_parties, size)

    if mode in ("all", "party-sweep"):
        print(
            f"=== Party-count sweep: size={fixed_size}, "
            f"parties={party_counts}, vert_mult={vert_mult}, edge_mult={edge_mult} ==="
        )
        for num_parties in party_counts:
            run_case(num_parties, fixed_size)


def generate_tables(
    results_root: Path,
    out_dir: Path,
    fixed_parties: int,
    fixed_size: int,
    sizes: list[int],
    party_counts: list[int],
    vert_mult: int,
    edge_mult: int,
    strict_missing: bool,
) -> None:
    print(f"Using results root: {results_root}")

    graph_rows = build_graph_size_table(
        results_root,
        fixed_parties,
        sizes,
        vert_mult=vert_mult,
        edge_mult=edge_mult,
        strict_missing=strict_missing,
    )
    party_rows = build_party_count_table(
        results_root,
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


def main() -> int:
    parser = argparse.ArgumentParser(description="Run full grasp benchmark pipeline")
    parser.add_argument(
        "--comparison-ringsg",
        action="store_true",
        help="Run the RingSG graph-size and party-count comparison sweeps",
    )
    parser.add_argument("--repo-root", default=".", help="CRiS-MPC repository root")
    parser.add_argument("--latency", default="1ms")
    parser.add_argument("--bandwidth", default="4Gbit")
    parser.add_argument(
        "--mode",
        default="all",
        choices=["all", "graph-sweep", "party-sweep"],
        help="Select which comparison-ringsg sweep to run",
    )
    parser.add_argument("--num-iters", type=int, default=10)
    parser.add_argument("--fixed-parties", type=int, default=5)
    parser.add_argument("--fixed-size", type=int, default=65536)
    parser.add_argument("--sizes", default="4096,8192,16384,32768,65536")
    parser.add_argument("--party-counts", default="3,4,5,6,7,8,9,10")
    parser.add_argument("--vert-mult", type=int, default=5, help="num_vertices = size * vert-mult")
    parser.add_argument("--edge-mult", type=int, default=15, help="num_edges = size * edge-mult")
    parser.add_argument(
        "--results-dir",
        default="benchmark/grasp/Results/comparison_ringsg",
        help="Base directory passed to run.sh through RESULTS_DIR",
    )
    parser.add_argument(
        "--out-dir",
        default="benchmark/grasp/Results/Tables",
        help="Directory for generated comparison tables",
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

    if not args.comparison_ringsg:
        parser.error("no evaluation selected; use --comparison-ringsg")

    repo_root = Path(args.repo_root).resolve()
    results_dir = (repo_root / args.results_dir).resolve()
    results_root = (
        results_dir / "bench_dcc_pagerank_mpa" / "protocol_nph"
    )
    out_dir = (repo_root / args.out_dir).resolve()

    sizes = [int(x) for x in args.sizes.split(",") if x.strip()]
    party_counts = [int(x) for x in args.party_counts.split(",") if x.strip()]

    try:
        if not args.skip_network:
            apply_tc_lan(repo_root, args.latency, args.bandwidth)

        if not args.skip_run:
            run_comparison_ringsg(
                repo_root=repo_root,
                results_dir=results_dir,
                mode=args.mode,
                fixed_parties=args.fixed_parties,
                fixed_size=args.fixed_size,
                sizes=sizes,
                party_counts=party_counts,
                vert_mult=args.vert_mult,
                edge_mult=args.edge_mult,
                num_iters=args.num_iters,
            )

        generate_tables(
            results_root=results_root,
            out_dir=out_dir,
            fixed_parties=args.fixed_parties,
            fixed_size=args.fixed_size,
            sizes=sizes,
            party_counts=party_counts,
            vert_mult=args.vert_mult,
            edge_mult=args.edge_mult,
            strict_missing=args.strict_missing,
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
