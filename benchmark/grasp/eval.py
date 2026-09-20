#!/usr/bin/env python3
"""Run GraSP evaluation suites and generate result tables.

With --comparison-ringsg, this script:
1) applies the requested network profile;
2) invokes run.sh directly for the graph-size and party-count sweeps; and
3) generates graph-size and party-count tables.

Raw logs are stored below benchmark/grasp/Results/comparison_ringsg and tables
below benchmark/grasp/Results/Tables/comparison_ringsg by default.
"""

from __future__ import annotations

import argparse
import os
import shlex
import subprocess
import sys
from pathlib import Path

from parse_results import (
    build_application_party_tables,
    build_graph_size_table,
    build_party_count_table,
    build_graphiti_grasp_tables,
    build_graphiti_party_tables,
    write_application_party_tables,
    write_comparison_tables,
    write_graphiti_party_tables,
    write_csv,
    write_markdown_table,
)


def run_cmd(cmd: str, cwd: Path) -> None:
    print(f"$ {cmd}")
    subprocess.run(["bash", "-lc", cmd], cwd=str(cwd), check=True)


def run_process(args: list[str], cwd: Path, env: dict[str, str] | None = None) -> None:
    print(f"$ {shlex.join(args)}")
    subprocess.run(args, cwd=str(cwd), env=env, check=True)


def write_oom_marker(results_dir: Path, benchmark: str, graph_size: int) -> None:
    marker_dir = (
        results_dir / benchmark / "protocol_nph" / "parties_5" /
        f"graph_{graph_size}"
    )
    marker_dir.mkdir(parents=True, exist_ok=True)
    (marker_dir / "OOM").write_text(
        f"{benchmark} exhausted memory at graph size {graph_size}\n",
        encoding="utf-8",
    )


def apply_tc_lan(repo_root: Path, latency: str, bandwidth: str) -> None:
    network_sh = repo_root / "network.sh"
    if not network_sh.exists():
        raise FileNotFoundError(f"Missing network script: {network_sh}")

    # tc_lan is a shell function defined in network.sh, so source + call in bash.
    cmd = f"source {network_sh} && tc_lan {latency} {bandwidth}"
    run_cmd(cmd, cwd=repo_root)


def apply_tc_nph_parties(
    repo_root: Path,
    num_parties: int = 5,
    latency: str = "100ms",
    bandwidth: str = "100Mbit",
) -> None:
    """Apply an NPH party profile."""
    network_sh = repo_root / "network.sh"
    if not network_sh.exists():
        raise FileNotFoundError(f"Missing network script: {network_sh}")
    run_cmd(
        f"source {shlex.quote(str(network_sh))} && "
        f"tc_nph_parties {num_parties} 14900 {latency} {bandwidth}",
        cwd=repo_root,
    )


def apply_tc_nph_pairs(
    repo_root: Path,
    num_parties: int = 5,
    latency: str = "100ms",
    bandwidth: str = "100Mbit",
) -> None:
    """Apply the pairwise NPH profile for a party-count sweep case."""
    network_sh = repo_root / "network.sh"
    if not network_sh.exists():
        raise FileNotFoundError(f"Missing network script: {network_sh}")
    run_cmd(
        f"source {shlex.quote(str(network_sh))} && "
        f"tc_nph_pairs {num_parties} 14900 {latency} {bandwidth}",
        cwd=repo_root,
    )


def run_graphiti_graph_size(repo_root: Path, results_dir: Path) -> None:
    """Run the fixed Graphiti-versus-GraSP graph-size sweep."""
    run_sh = repo_root / "run.sh"
    if not run_sh.exists():
        raise FileNotFoundError(f"Missing benchmark launcher: {run_sh}")

    results_dir.mkdir(parents=True, exist_ok=True)
    env = os.environ.copy()
    env["RESULTS_DIR"] = str(results_dir)

    # Report the known memory limit even though this point is not launched.
    write_oom_marker(results_dir, "microbench_InitGraphiti", 2**22)

    suites = (
        ("bench_PrMpaGraSP", [2**power for power in range(16, 23)]),
        ("bench_PrMpaGraphiti", [2**power for power in range(16, 23)]),
        ("microbench_InitGraphiti", [2**power for power in range(16, 21)]),
    )
    for benchmark_name, graph_sizes in suites:
        print(f"=== {benchmark_name}: graph sizes {graph_sizes} ===")
        for graph_size in graph_sizes:
            cmd = [
                str(run_sh), benchmark_name,
                "--protocol", "nph",
                "--num-parties", "5",
                "--pking",
                "--graph-size", str(graph_size),
                "--port", "14900",
            ]
            if benchmark_name != "microbench_InitGraphiti":
                cmd.extend(["--num-iters", "10", "--no-check"])
            run_process(cmd, cwd=repo_root, env=env)


def run_graphiti_num_parties(
    repo_root: Path,
    results_dir: Path,
    configure_network: bool,
) -> None:
    """Run the fixed-size Graphiti party-count sweeps."""
    run_sh = repo_root / "run.sh"
    if not run_sh.exists():
        raise FileNotFoundError(f"Missing benchmark launcher: {run_sh}")

    results_dir.mkdir(parents=True, exist_ok=True)
    env = os.environ.copy()
    env["RESULTS_DIR"] = str(results_dir)

    graph_size = 2**19
    party_counts = range(2, 4)
    suites = (
        "bench_PrMpaGraphiti",
        "microbench_InitGraphiti",
        "bench_PrMpaGraSP",
    )
    for benchmark_name in suites:
        print(
            f"=== {benchmark_name}: graph size {graph_size}, "
            f"parties={list(party_counts)} ==="
        )
        for num_parties in party_counts:
            if configure_network:
                apply_tc_nph_pairs(repo_root, num_parties)
            cmd = [
                str(run_sh), benchmark_name,
                "--protocol", "nph",
                "--num-parties", str(num_parties),
                "--pking",
                "--graph-size", str(graph_size),
                "--port", "14900",
            ]
            if benchmark_name != "microbench_InitGraphiti":
                cmd.extend(["--num-iters", "10", "--no-check"])
            run_process(cmd, cwd=repo_root, env=env)


def run_applications_num_parties(
    repo_root: Path,
    results_dir: Path,
    configure_network: bool,
) -> None:
    """Run the applications at graph size 2**21 for 2 through 10 parties."""
    run_sh = repo_root / "run.sh"
    if not run_sh.exists():
        raise FileNotFoundError(f"Missing benchmark launcher: {run_sh}")

    results_dir.mkdir(parents=True, exist_ok=True)
    env = os.environ.copy()
    env["RESULTS_DIR"] = str(results_dir)

    graph_size = 2**21
    party_counts = range(2, 11)
    applications = ["RiskPropagation", "GroupConnection"]
    for application in applications:
        print(
            f"=== {application}: graph size {graph_size}, "
            f"parties={list(party_counts)} ==="
        )
        for num_parties in party_counts:
            if configure_network:
                apply_tc_nph_pairs(
                    repo_root,
                    num_parties,
                    latency="100ms",
                    bandwidth="100Mbit",
                )
            run_process(
                [
                    str(run_sh), application,
                    "--protocol", "nph",
                    "--pking",
                    "--num-parties", str(num_parties),
                    "--graph-size", str(graph_size),
                    "--port", "14900",
                ],
                cwd=repo_root,
                env=env,
            )


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
    configure_network: bool,
    wan: bool = False,
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
        if configure_network:
            if wan:
                apply_tc_nph_pairs(
                    repo_root, num_parties, "10ms", "200Mbit"
                )
            else:
                apply_tc_nph_pairs(repo_root, num_parties, "1ms", "4Gbit")
        run_process(
            [
                str(run_sh),
                "bench_PrMpaGraSP",
                "--protocol", "nph",
                "--num-iters", str(num_iters),
                "--pking",
                "--num-parties", str(num_parties),
                "--num-verts", str(num_verts),
                "--num-edges", str(num_edges),
                "--port", "14900",
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
    ringsg_wan: bool = False,
) -> None:
    print(f"Using results root: {results_root}")

    graph_rows = build_graph_size_table(
        results_root,
        fixed_parties,
        sizes,
        vert_mult=vert_mult,
        edge_mult=edge_mult,
        strict_missing=strict_missing,
        ringsg_wan=ringsg_wan,
    )
    party_rows = build_party_count_table(
        results_root,
        fixed_size,
        party_counts,
        vert_mult=vert_mult,
        edge_mult=edge_mult,
        strict_missing=strict_missing,
        ringsg_wan=ringsg_wan,
    )

    write_csv(party_rows, out_dir / "party_count_table.csv")
    write_csv(graph_rows, out_dir / "graph_size_table.csv")
    party_md = out_dir / "party_count_table.md"
    graph_md = out_dir / "graph_size_table.md"
    write_markdown_table(
        party_rows,
        party_md,
        (
            "WAN GraSP and RingSG Comparison for Varying Number of "
            "Parties[^ringsg]"
            if ringsg_wan else
            "GraSP and RingSG Comparison for Varying Number of Parties[^ringsg]"
        ),
    )
    write_markdown_table(
        graph_rows,
        graph_md,
        (
            "WAN GraSP and RingSG Comparison for Varying Graph Size[^ringsg]"
            if ringsg_wan else
            "GraSP and RingSG Comparison for Varying Graph Size[^ringsg]"
        ),
    )
    ringsg_footnote = (
        "\n[^ringsg]: RingSG numbers are precomputed by running RingSG on "
        "Ubuntu servers equipped with AMD Ryzen Threadripper PRO 5965WX "
        "processors and 512 GB RAM."
        + (
            " The WAN network uses 200 Mbps bandwidth and 10 ms latency."
            if ringsg_wan else ""
        )
        + "\n"
    )
    party_table = party_md.read_text(encoding="utf-8").rstrip()
    graph_table = graph_md.read_text(encoding="utf-8").rstrip()
    for table_file in (party_md, graph_md):
        with table_file.open("a", encoding="utf-8") as stream:
            stream.write(ringsg_footnote)
    (out_dir / "comparison_tables.md").write_text(
        party_table + "\n\n" + graph_table + ringsg_footnote,
        encoding="utf-8",
    )
    print(f"Wrote tables to: {out_dir}")


def main() -> int:
    parser = argparse.ArgumentParser(description="Run full grasp benchmark pipeline")
    parser.add_argument(
        "--comparison-ringsg",
        action="store_true",
        help="Run the RingSG graph-size and party-count comparison sweeps",
    )
    parser.add_argument(
        "--comparison-ringsg-wan",
        action="store_true",
        help=(
            "Run the RingSG comparison sweeps using "
            "tc_nph_parties <num-parties> 14900 10ms 200Mbit"
        ),
    )
    parser.add_argument(
        "--graphiti_graph_size",
        action="store_true",
        help=(
            "Run the five-party Graphiti/GraSP sweep using "
            "tc_nph_parties 5 14900 100ms 100Mbit"
        ),
    )
    parser.add_argument(
        "--graphiti_num_parties",
        action="store_true",
        help=(
            "Run the Graphiti party-count sweeps at graph size 2**19, "
            "applying tc_nph_pairs before each run"
        ),
    )
    parser.add_argument(
        "--applications_num_parties",
        action="store_true",
        help=(
            "Run GroupConnection and RiskPropagation at graph size 2**21 "
            "for 2 through 10 parties, applying "
            "tc_nph_pairs <num-parties> 14900 100ms 100Mbit before each run"
        ),
    )
    parser.add_argument(
        "--repo-root",
        default=str(Path(__file__).resolve().parents[2]),
        help="CRiS-MPC repository root (default: inferred from eval.py)",
    )
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

    if not (
        args.comparison_ringsg
        or args.comparison_ringsg_wan
        or args.graphiti_graph_size
        or args.graphiti_num_parties
        or args.applications_num_parties
    ):
        parser.error(
            "no evaluation selected; use --comparison-ringsg or "
            "--comparison-ringsg-wan or --graphiti_graph_size or "
            "--graphiti_num_parties or --applications_num_parties"
        )

    repo_root = Path(args.repo_root).resolve()
    results_dir = (repo_root / args.results_dir).resolve()
    if args.comparison_ringsg_wan:
        results_dir = (
            repo_root / "benchmark/grasp/Results/comparison_ringsg_wan"
        ).resolve()
    results_root = (
        results_dir / "bench_PrMpaGraSP" / "protocol_nph"
    )
    out_dir = (repo_root / args.out_dir).resolve()

    sizes = [int(x) for x in args.sizes.split(",") if x.strip()]
    party_counts = [int(x) for x in args.party_counts.split(",") if x.strip()]

    try:
        if args.applications_num_parties:
            applications_results = (
                repo_root / "benchmark/grasp/Results/applications_num_parties"
            ).resolve()
            applications_tables = (
                repo_root /
                "benchmark/grasp/Results/Tables/applications_num_parties"
            ).resolve()
            if not args.skip_run:
                run_applications_num_parties(
                    repo_root,
                    applications_results,
                    configure_network=not args.skip_network,
                )
            tables = build_application_party_tables(
                applications_results,
                strict_missing=args.strict_missing,
            )
            write_application_party_tables(tables, applications_tables)
            print(f"Wrote application party tables to: {applications_tables}")
            return 0

        if args.graphiti_num_parties:
            graphiti_results = (
                repo_root / "benchmark/grasp/Results/graphiti_num_parties"
            ).resolve()
            graphiti_tables = (
                repo_root / "benchmark/grasp/Results/Tables/graphiti_num_parties"
            ).resolve()
            if not args.skip_run:
                run_graphiti_num_parties(
                    repo_root,
                    graphiti_results,
                    configure_network=not args.skip_network,
                )
            tables = build_graphiti_party_tables(
                graphiti_results,
                strict_missing=args.strict_missing,
            )
            write_graphiti_party_tables(tables, graphiti_tables)
            print(f"Wrote Graphiti/GraSP party tables to: {graphiti_tables}")
            return 0

        if args.graphiti_graph_size:
            graphiti_results = (
                repo_root / "benchmark/grasp/Results/graphiti_graph_size"
            ).resolve()
            graphiti_tables = (
                repo_root / "benchmark/grasp/Results/Tables/graphiti_graph_size"
            ).resolve()
            if not args.skip_network:
                apply_tc_nph_pairs(repo_root)
            if not args.skip_run:
                run_graphiti_graph_size(repo_root, graphiti_results)
            tables = build_graphiti_grasp_tables(
                graphiti_results,
                strict_missing=args.strict_missing,
            )
            write_comparison_tables(tables, graphiti_tables)
            print(f"Wrote Graphiti/GraSP tables to: {graphiti_tables}")
            return 0

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
                configure_network=not args.skip_network,
                wan=args.comparison_ringsg_wan,
            )

        generate_tables(
            results_root=results_root,
            out_dir=(repo_root / "benchmark/grasp/Results/Tables" / (
                "comparison_ringsg_wan"
                if args.comparison_ringsg_wan
                else "comparison_ringsg"
            )),
            fixed_parties=args.fixed_parties,
            fixed_size=args.fixed_size,
            sizes=sizes,
            party_counts=party_counts,
            vert_mult=args.vert_mult,
            edge_mult=args.edge_mult,
            strict_missing=args.strict_missing,
            ringsg_wan=args.comparison_ringsg_wan,
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
