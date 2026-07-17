#!/usr/bin/env python3
"""Parse and summarize bench_dcc_pagerank_mpa sweep results.

This module reads summary files created by run.sh under:
benchmark/grasp/Results/comparison_ringsg/bench_dcc_pagerank_mpa/
protocol_nph/parties_<n>/verts_<V>/edges_<E>/<timestamp>/summary.txt

Graph shape is specified explicitly via num_vertices/num_edges (rather than
a single --graph-size), typically derived from a "size" using fixed
vertex/edge multipliers (e.g. V=size*5, E=size*15).

For each run, metrics are aggregated as:
- runtime (ms): max across parties
- communication (B): sum of sent bytes across parties

Only offline, online, and total phases are reported.
"""

from __future__ import annotations

import argparse
import csv
import re
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Dict, Iterable, List, Mapping, Sequence

PHASES: Sequence[str] = ("offline", "online", "total")
LINE_RE = re.compile(
    r"^\[P(?P<pid>\d+)\]\s+"
    r"(?P<phase>offline|online|total)\s+"
    r"time:\s*(?P<time_ms>[0-9]+(?:\.[0-9]+)?)\s*ms\s+"
    r"sent:\s*(?P<sent_bytes>\d+)\s*B\s+"
    r"recv:\s*(?P<recv_bytes>\d+)\s*B\s*$"
)


@dataclass
class PhaseAggregate:
    max_time_ms: float
    sum_sent_bytes: int


@dataclass
class RunAggregate:
    summary_file: Path
    phases: Dict[str, PhaseAggregate]


def auto_detect_results_root(repo_root: Path) -> Path:
    """Pick the most populated known results root for this benchmark."""
    candidates = [
        repo_root / "benchmark" / "grasp" / "Results" /
        "comparison_ringsg" / "bench_dcc_pagerank_mpa" / "protocol_nph",
        repo_root / "Results" / "bench_dcc_pagerank_mpa" / "protocol_nph",
        repo_root / "benchmark" / "Results" / "bench_dcc_pagerank_mpa" / "protocol_nph",
    ]

    best_path: Path | None = None
    best_score = -1

    for candidate in candidates:
        if not candidate.exists():
            continue
        score = len(list(candidate.glob("parties_*/verts_*/edges_*/*/summary.txt")))
        if score > best_score:
            best_score = score
            best_path = candidate

    if best_path is None:
        checked = "\n".join(str(p) for p in candidates)
        raise FileNotFoundError(
            "Could not find benchmark results root. Checked:\n"
            f"{checked}"
        )

    return best_path


def parse_summary(summary_file: Path) -> RunAggregate:
    """Parse one summary.txt and aggregate per-phase metrics across parties."""
    if not summary_file.exists():
        raise FileNotFoundError(f"Summary file not found: {summary_file}")

    per_phase_times: Dict[str, List[float]] = {phase: [] for phase in PHASES}
    per_phase_sent: Dict[str, List[int]] = {phase: [] for phase in PHASES}

    for line in summary_file.read_text(encoding="utf-8").splitlines():
        match = LINE_RE.match(line.strip())
        if not match:
            continue

        phase = match.group("phase")
        if phase not in per_phase_times:
            continue

        per_phase_times[phase].append(float(match.group("time_ms")))
        per_phase_sent[phase].append(int(match.group("sent_bytes")))

    phases: Dict[str, PhaseAggregate] = {}
    for phase in PHASES:
        if not per_phase_times[phase]:
            raise ValueError(
                f"No parseable '{phase}' lines found in summary file: {summary_file}"
            )
        phases[phase] = PhaseAggregate(
            max_time_ms=max(per_phase_times[phase]),
            sum_sent_bytes=sum(per_phase_sent[phase]),
        )

    return RunAggregate(summary_file=summary_file, phases=phases)


def _latest_summary_for(
    results_root: Path, num_parties: int, num_vertices: int, num_edges: int
) -> Path:
    run_dir = (
        results_root
        / f"parties_{num_parties}"
        / f"verts_{num_vertices}"
        / f"edges_{num_edges}"
    )
    if not run_dir.exists():
        raise FileNotFoundError(f"Run directory not found: {run_dir}")

    timestamps = sorted([p for p in run_dir.iterdir() if p.is_dir()])
    if not timestamps:
        raise FileNotFoundError(f"No timestamp directories found under: {run_dir}")

    summary = timestamps[-1] / "summary.txt"
    if not summary.exists():
        raise FileNotFoundError(f"summary.txt not found: {summary}")

    return summary


BYTES_PER_MB = 1024 * 1024


def _flatten_row(
    x_value_name: str,
    x_value: int,
    num_vertices: int,
    num_edges: int,
    agg: RunAggregate,
) -> Mapping[str, object]:
    row: Dict[str, object] = {
        x_value_name: x_value,
        "num_vertices": num_vertices,
        "num_edges": num_edges,
        "summary_file": str(agg.summary_file),
    }

    for phase in PHASES:
        time_s = agg.phases[phase].max_time_ms / 1000.0
        sent_mb = agg.phases[phase].sum_sent_bytes / BYTES_PER_MB
        row[f"{phase}_time_s_max"] = round(time_s, 2)
        row[f"{phase}_sent_mb_sum"] = round(sent_mb, 2)

    return row


def build_graph_size_table(
    results_root: Path,
    fixed_parties: int,
    sizes: Iterable[int],
    vert_mult: int = 5,
    edge_mult: int = 15,
    strict_missing: bool = False,
) -> List[Mapping[str, object]]:
    rows: List[Mapping[str, object]] = []
    missing: List[int] = []
    for size in sizes:
        num_vertices = size * vert_mult
        num_edges = size * edge_mult
        try:
            summary_file = _latest_summary_for(
                results_root, fixed_parties, num_vertices, num_edges
            )
        except FileNotFoundError:
            if strict_missing:
                raise
            missing.append(size)
            continue
        agg = parse_summary(summary_file)
        rows.append(_flatten_row("size", size, num_vertices, num_edges, agg))

    if missing:
        print(
            "Warning: Missing size-sweep runs for parties "
            f"{fixed_parties}: {', '.join(str(x) for x in missing)}",
            file=sys.stderr,
        )

    if not rows:
        raise ValueError(
            "No size-sweep rows available to build table. "
            "Run the size sweep first or pass --strict-missing to fail early."
        )

    return rows


def build_party_count_table(
    results_root: Path,
    fixed_size: int,
    party_counts: Iterable[int],
    vert_mult: int = 5,
    edge_mult: int = 15,
    strict_missing: bool = False,
) -> List[Mapping[str, object]]:
    num_vertices = fixed_size * vert_mult
    num_edges = fixed_size * edge_mult

    rows: List[Mapping[str, object]] = []
    missing: List[int] = []
    for num_parties in party_counts:
        try:
            summary_file = _latest_summary_for(
                results_root, num_parties, num_vertices, num_edges
            )
        except FileNotFoundError:
            if strict_missing:
                raise
            missing.append(num_parties)
            continue
        agg = parse_summary(summary_file)
        rows.append(_flatten_row("num_parties", num_parties, num_vertices, num_edges, agg))

    if missing:
        print(
            "Warning: Missing party-count runs for size "
            f"{fixed_size}: {', '.join(str(x) for x in missing)}",
            file=sys.stderr,
        )

    if not rows:
        raise ValueError(
            "No party-count rows available to build table. "
            "Run party sweep first or pass --strict-missing to fail early."
        )

    return rows


def _format_cell(v: object) -> str:
    if isinstance(v, float):
        return f"{v:.2f}"
    return str(v)


def write_csv(rows: Sequence[Mapping[str, object]], out_file: Path) -> None:
    if not rows:
        raise ValueError(f"Cannot write empty table: {out_file}")
    out_file.parent.mkdir(parents=True, exist_ok=True)
    fieldnames = list(rows[0].keys())
    with out_file.open("w", encoding="utf-8", newline="") as f:
        writer = csv.DictWriter(f, fieldnames=fieldnames)
        writer.writeheader()
        for row in rows:
            writer.writerow({k: _format_cell(v) for k, v in row.items()})


def write_markdown_table(rows: Sequence[Mapping[str, object]], out_file: Path, title: str) -> None:
    if not rows:
        raise ValueError(f"Cannot write empty table: {out_file}")

    headers = list(rows[0].keys())
    out_file.parent.mkdir(parents=True, exist_ok=True)

    lines = [f"# {title}", ""]
    lines.append("| " + " | ".join(headers) + " |")
    lines.append("| " + " | ".join(["---"] * len(headers)) + " |")

    for row in rows:
        lines.append("| " + " | ".join(_format_cell(row[h]) for h in headers) + " |")

    out_file.write_text("\n".join(lines) + "\n", encoding="utf-8")


def plot_table_png(
    rows: Sequence[Mapping[str, object]],
    out_file: Path,
    title: str,
) -> None:
    """Render a simple table image if matplotlib is available."""
    try:
        import matplotlib.pyplot as plt  # type: ignore
    except Exception as exc:  # pragma: no cover
        raise RuntimeError(
            "matplotlib is required for PNG table output; install it or skip PNG"
        ) from exc

    headers = list(rows[0].keys())
    data = [[_format_cell(row[h]) for h in headers] for row in rows]

    fig_height = max(2.5, 0.55 * (len(rows) + 2))
    fig, ax = plt.subplots(figsize=(max(11.0, len(headers) * 1.7), fig_height))
    ax.axis("off")
    table = ax.table(cellText=data, colLabels=headers, loc="center")
    table.auto_set_font_size(False)
    table.set_fontsize(9)
    table.scale(1.0, 1.3)
    ax.set_title(title, pad=14)

    out_file.parent.mkdir(parents=True, exist_ok=True)
    fig.tight_layout()
    fig.savefig(out_file, dpi=200)
    plt.close(fig)


def main() -> None:
    parser = argparse.ArgumentParser(description="Parse bench_dcc_pagerank_mpa sweep results")
    parser.add_argument(
        "--results-root",
        default="auto",
        help=(
            "Root containing parties_<n>/verts_<V>/edges_<E>/<timestamp>/summary.txt, "
            "or 'auto' to detect from repo"
        ),
    )
    parser.add_argument("--fixed-parties", type=int, default=5)
    parser.add_argument("--fixed-size", type=int, default=65536)
    parser.add_argument(
        "--sizes",
        default="4096,8192,16384,32768,65536",
        help="Comma-separated sizes for the size-sweep table (V=size*vert-mult, E=size*edge-mult)",
    )
    parser.add_argument(
        "--party-counts",
        default="3,4,5,6,7,8,9,10",
        help="Comma-separated party counts for party-count table",
    )
    parser.add_argument("--vert-mult", type=int, default=5, help="num_vertices = size * vert-mult")
    parser.add_argument("--edge-mult", type=int, default=15, help="num_edges = size * edge-mult")
    parser.add_argument(
        "--out-dir",
        default="benchmark/grasp/Results/Tables",
        help="Directory for generated CSV/MD/PNG tables",
    )
    parser.add_argument(
        "--skip-png",
        action="store_true",
        help="Skip PNG table rendering (matplotlib not required)",
    )
    parser.add_argument(
        "--strict-missing",
        action="store_true",
        help="Fail if any requested sweep point is missing",
    )

    args = parser.parse_args()

    if args.results_root == "auto":
        results_root = auto_detect_results_root(Path.cwd())
    else:
        results_root = Path(args.results_root)
    out_dir = Path(args.out_dir)

    sizes = [int(x) for x in args.sizes.split(",") if x.strip()]
    party_counts = [int(x) for x in args.party_counts.split(",") if x.strip()]

    graph_rows = build_graph_size_table(
        results_root,
        args.fixed_parties,
        sizes,
        vert_mult=args.vert_mult,
        edge_mult=args.edge_mult,
        strict_missing=args.strict_missing,
    )
    party_rows = build_party_count_table(
        results_root,
        args.fixed_size,
        party_counts,
        vert_mult=args.vert_mult,
        edge_mult=args.edge_mult,
        strict_missing=args.strict_missing,
    )

    write_csv(graph_rows, out_dir / "graph_size_table.csv")
    write_csv(party_rows, out_dir / "party_count_table.csv")
    write_markdown_table(graph_rows, out_dir / "graph_size_table.md", "Graph Size Sweep")
    write_markdown_table(party_rows, out_dir / "party_count_table.md", "Party Count Sweep")

    if not args.skip_png:
        plot_table_png(graph_rows, out_dir / "graph_size_table.png", "Graph Size Sweep")
        plot_table_png(party_rows, out_dir / "party_count_table.png", "Party Count Sweep")

    print(f"Wrote tables to: {out_dir}")


if __name__ == "__main__":
    main()
