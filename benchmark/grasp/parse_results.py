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
    r"(?P<phase>offline|init|online|total)\s+"
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


GRAPHITI_GRASP_SIZES: Sequence[int] = tuple(2**power for power in range(16, 23))
GRAPHITI_INIT_SIZES: Sequence[int] = tuple(2**power for power in range(16, 22))
GRAPHITI_INIT_OOM_SIZE = 2**22
GRAPHITI_PARTY_COUNTS: Sequence[int] = tuple(range(2, 11))
GRAPHITI_PARTY_GRAPH_SIZE = 2**19
APPLICATIONS: Sequence[str] = ("GroupConnection", "RiskPropagation")
APPLICATION_PARTY_COUNTS: Sequence[int] = tuple(range(2, 11))
APPLICATION_GRAPH_SIZE = 2**21
RINGSG_GRAPH_SIZE_RESULTS: Mapping[int, tuple[float, float]] = {
    2**12: (11.29, 495.81),
    2**13: (12.23, 977.01),
    2**14: (13.36, 1938.85),
    2**15: (15.26, 3861.45),
    2**16: (18.74, 7704.84),
}
RINGSG_PARTY_RESULTS: Mapping[int, tuple[float, float]] = {
    3: (18.71, 4614.01),
    4: (18.89, 6159.60),
    5: (18.74, 7704.84),
    6: (18.90, 9248.94),
    7: (19.00, 10794.68),
    8: (18.93, 12337.26),
    9: (19.18, 13881.84),
    10: (19.34, 15428.15),
}
RINGSG_WAN_GRAPH_SIZE_RESULTS: Mapping[int, tuple[float, float]] = {
    2**12: (111.019, 498.44),
    2**13: (120.945, 981.58),
    2**14: (136.489, 1949.90),
    2**15: (163.679, 3889.96),
    2**16: (207.320, 7768.59),
}
RINGSG_WAN_PARTY_RESULTS: Mapping[int, tuple[float, float]] = {
    3: (205.934, 4652.20),
    5: (207.320, 7768.59),
    7: (207.800, 10886.57),
    9: (210.329, 13996.74),
    10: (207.435, 15557.03),
}


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


def parse_summary(
    summary_file: Path,
    required_phases: Sequence[str] = PHASES,
) -> RunAggregate:
    """Parse one summary.txt and aggregate per-phase metrics across parties."""
    if not summary_file.exists():
        raise FileNotFoundError(f"Summary file not found: {summary_file}")

    per_phase_times: Dict[str, Dict[int, float]] = {
        phase: {} for phase in required_phases
    }
    per_phase_sent: Dict[str, Dict[int, int]] = {
        phase: {} for phase in required_phases
    }

    for line in summary_file.read_text(encoding="utf-8").splitlines():
        match = LINE_RE.match(line.strip())
        if not match:
            continue

        phase = match.group("phase")
        if phase not in per_phase_times:
            continue

        pid = int(match.group("pid"))
        per_phase_times[phase][pid] = float(match.group("time_ms"))
        per_phase_sent[phase][pid] = int(match.group("sent_bytes"))

    phases: Dict[str, PhaseAggregate] = {}
    for phase in required_phases:
        if not per_phase_times[phase]:
            raise ValueError(
                f"No parseable '{phase}' lines found in summary file: {summary_file}"
            )
        if per_phase_times[phase].keys() != per_phase_sent[phase].keys():
            raise ValueError(
                f"Incomplete '{phase}' lines found in summary file: {summary_file}"
            )
        phases[phase] = PhaseAggregate(
            max_time_ms=max(per_phase_times[phase].values()),
            sum_sent_bytes=sum(per_phase_sent[phase].values()),
        )

    return RunAggregate(summary_file=summary_file, phases=phases)


def parse_party_logs(run_dir: Path, required_phases: Sequence[str]) -> RunAggregate:
    """Aggregate logs using max party time and sum of all parties' sent bytes."""
    log_files = sorted(run_dir.glob("party_*.log"))
    if not log_files:
        raise FileNotFoundError(f"No party logs found under: {run_dir}")

    # Index by pid so every party contributes exactly once per phase, even if a
    # launcher or copied log happens to contain duplicate statistics lines.
    times: Dict[str, Dict[int, float]] = {phase: {} for phase in required_phases}
    sent: Dict[str, Dict[int, int]] = {phase: {} for phase in required_phases}
    for log_file in log_files:
        for line in log_file.read_text(encoding="utf-8", errors="replace").splitlines():
            match = LINE_RE.match(line.strip())
            if not match or match.group("phase") not in times:
                continue
            phase = match.group("phase")
            pid = int(match.group("pid"))
            times[phase][pid] = float(match.group("time_ms"))
            sent[phase][pid] = int(match.group("sent_bytes"))

    phases: Dict[str, PhaseAggregate] = {}
    for phase in required_phases:
        if not times[phase]:
            raise ValueError(f"No '{phase}' statistics found in {run_dir}/party_*.log")
        if times[phase].keys() != sent[phase].keys():
            raise ValueError(f"Incomplete '{phase}' statistics in {run_dir}/party_*.log")
        phases[phase] = PhaseAggregate(
            max_time_ms=max(times[phase].values()),
            sum_sent_bytes=sum(sent[phase].values()),
        )
    return RunAggregate(run_dir, phases)


def parse_party_log_phase_groups(
    run_dir: Path,
    phase_groups: Mapping[str, Sequence[str]],
) -> RunAggregate:
    """Aggregate phase groups after combining their values per party."""
    log_files = sorted(run_dir.glob("party_*.log"))
    if not log_files:
        raise FileNotFoundError(f"No party logs found under: {run_dir}")

    required_phases = {
        phase for group in phase_groups.values() for phase in group
    }
    times: Dict[str, Dict[int, float]] = {
        phase: {} for phase in required_phases
    }
    sent: Dict[str, Dict[int, int]] = {
        phase: {} for phase in required_phases
    }
    for log_file in log_files:
        for line in log_file.read_text(
            encoding="utf-8", errors="replace"
        ).splitlines():
            match = LINE_RE.match(line.strip())
            if not match or match.group("phase") not in required_phases:
                continue
            phase = match.group("phase")
            pid = int(match.group("pid"))
            times[phase][pid] = float(match.group("time_ms"))
            sent[phase][pid] = int(match.group("sent_bytes"))

    grouped: Dict[str, PhaseAggregate] = {}
    for group_name, group_phases in phase_groups.items():
        party_ids = set(times[group_phases[0]])
        if not party_ids:
            raise ValueError(
                f"No '{group_phases[0]}' statistics found in "
                f"{run_dir}/party_*.log"
            )
        for phase in group_phases:
            if set(times[phase]) != party_ids or set(sent[phase]) != party_ids:
                raise ValueError(
                    f"Incomplete '{phase}' statistics in "
                    f"{run_dir}/party_*.log"
                )
        grouped[group_name] = PhaseAggregate(
            max_time_ms=max(
                sum(times[phase][pid] for phase in group_phases)
                for pid in party_ids
            ),
            sum_sent_bytes=sum(
                sent[phase][pid]
                for phase in group_phases
                for pid in party_ids
            ),
        )
    return RunAggregate(run_dir, grouped)


def _latest_party_graph_phase_groups(
    results_dir: Path,
    benchmark: str,
    num_parties: int,
    graph_size: int,
    phase_groups: Mapping[str, Sequence[str]],
) -> RunAggregate:
    graph_dir = (
        results_dir / benchmark / "protocol_nph" /
        f"parties_{num_parties}" / f"graph_{graph_size}"
    )
    if not graph_dir.exists():
        raise FileNotFoundError(f"Result directory not found: {graph_dir}")

    errors: List[str] = []
    for run_dir in sorted(
        (path for path in graph_dir.iterdir() if path.is_dir()), reverse=True
    ):
        try:
            return parse_party_log_phase_groups(run_dir, phase_groups)
        except (FileNotFoundError, ValueError) as exc:
            errors.append(str(exc))
    raise FileNotFoundError(
        f"No complete run for {benchmark}, {num_parties} parties, "
        f"graph size {graph_size}. " + "; ".join(errors)
    )


def _latest_graph_run(
    results_dir: Path,
    benchmark: str,
    graph_size: int,
    required_phases: Sequence[str],
) -> RunAggregate:
    graph_dir = (
        results_dir / benchmark / "protocol_nph" / "parties_5" /
        f"graph_{graph_size}"
    )
    if not graph_dir.exists():
        raise FileNotFoundError(f"Result directory not found: {graph_dir}")

    errors: List[str] = []
    for run_dir in sorted(
        (path for path in graph_dir.iterdir() if path.is_dir()), reverse=True
    ):
        try:
            return parse_party_logs(run_dir, required_phases)
        except (FileNotFoundError, ValueError) as exc:
            errors.append(str(exc))
    raise FileNotFoundError(
        f"No complete run for {benchmark}, graph size {graph_size}. "
        + "; ".join(errors)
    )


def _latest_party_graph_run(
    results_dir: Path,
    benchmark: str,
    num_parties: int,
    graph_size: int,
    required_phases: Sequence[str],
) -> RunAggregate:
    graph_dir = (
        results_dir / benchmark / "protocol_nph" /
        f"parties_{num_parties}" / f"graph_{graph_size}"
    )
    if not graph_dir.exists():
        raise FileNotFoundError(f"Result directory not found: {graph_dir}")

    errors: List[str] = []
    for run_dir in sorted(
        (path for path in graph_dir.iterdir() if path.is_dir()), reverse=True
    ):
        try:
            return parse_party_logs(run_dir, required_phases)
        except (FileNotFoundError, ValueError) as exc:
            errors.append(str(exc))
    raise FileNotFoundError(
        f"No complete run for {benchmark}, {num_parties} parties, "
        f"graph size {graph_size}. " + "; ".join(errors)
    )


def _time_s(
    agg: RunAggregate | None, *phases: str, missing: str = "—"
) -> str:
    if agg is None:
        return missing
    return f"{sum(agg.phases[p].max_time_ms for p in phases) / 1000.0:.2f}"


def _sent_mb(
    agg: RunAggregate | None, *phases: str, missing: str = "—"
) -> str:
    if agg is None:
        return missing
    total = sum(agg.phases[p].sum_sent_bytes for p in phases)
    return f"{total / BYTES_PER_MB:.2f}"


def build_graphiti_grasp_tables(
    results_dir: Path,
    strict_missing: bool = False,
) -> Mapping[str, List[Mapping[str, object]]]:
    """Build online and preprocessing tables for the graph-size sweep."""
    online_rows: List[Mapping[str, object]] = []
    preprocessing_rows: List[Mapping[str, object]] = []

    for graph_size in GRAPHITI_GRASP_SIZES:
        runs: Dict[str, RunAggregate | None] = {}
        specs = {
            "graphiti_init": ("microbench_InitGraphiti", ("offline", "online")),
            "graphiti_mpa": ("bench_PrMpaGraphiti", ("offline", "online")),
            "grasp": ("bench_PrMpaGraSP", ("offline", "init", "online")),
        }
        for key, (benchmark, phases) in specs.items():
            if key == "graphiti_init" and graph_size not in GRAPHITI_INIT_SIZES:
                runs[key] = None
                continue
            try:
                runs[key] = _latest_graph_run(
                    results_dir, benchmark, graph_size, phases
                )
            except FileNotFoundError as exc:
                if strict_missing:
                    raise
                print(f"Warning: {exc}", file=sys.stderr)
                runs[key] = None

        graphiti_init = runs["graphiti_init"]
        graphiti_mpa = runs["graphiti_mpa"]
        grasp = runs["grasp"]
        graphiti_init_missing = (
            "OOM" if graph_size == GRAPHITI_INIT_OOM_SIZE else "—"
        )
        online_rows.extend((
            {
                "Ref": "Graphiti",
                "Graph Size": graph_size,
                "Init Time (s)": _time_s(
                    graphiti_init, "online", missing=graphiti_init_missing
                ),
                "MPA Time (s)": _time_s(graphiti_mpa, "online"),
                "Init Comm (MiB)": _sent_mb(
                    graphiti_init, "online", missing=graphiti_init_missing
                ),
                "MPA Comm (MiB)": _sent_mb(graphiti_mpa, "online"),
            },
            {
                "Ref": "GraSP",
                "Graph Size": graph_size,
                "Init Time (s)": _time_s(grasp, "init"),
                "MPA Time (s)": _time_s(grasp, "online"),
                "Init Comm (MiB)": _sent_mb(grasp, "init"),
                "MPA Comm (MiB)": _sent_mb(grasp, "online"),
            },
        ))

        graphiti_available = graphiti_init is not None and graphiti_mpa is not None
        preprocessing_rows.extend((
            {
                "Ref": "Graphiti",
                "Graph Size": graph_size,
                "Preprocessing Time (s)": (
                    f"{(graphiti_init.phases['offline'].max_time_ms + graphiti_mpa.phases['offline'].max_time_ms) / 1000.0:.2f}"
                    if graphiti_available else graphiti_init_missing
                ),
                "Preprocessing Comm (MiB)": (
                    f"{(graphiti_init.phases['offline'].sum_sent_bytes + graphiti_mpa.phases['offline'].sum_sent_bytes) / BYTES_PER_MB:.2f}"
                    if graphiti_available else graphiti_init_missing
                ),
            },
            {
                "Ref": "GraSP",
                "Graph Size": graph_size,
                "Preprocessing Time (s)": _time_s(grasp, "offline"),
                "Preprocessing Comm (MiB)": _sent_mb(grasp, "offline"),
            },
        ))

    return {
        "online": online_rows,
        "preprocessing": preprocessing_rows,
    }


def build_graphiti_party_tables(
    results_dir: Path,
    strict_missing: bool = False,
) -> Mapping[str, List[Mapping[str, object]]]:
    """Build online and preprocessing tables for the fixed party sweep."""
    online_rows: List[Mapping[str, object]] = []
    preprocessing_rows: List[Mapping[str, object]] = []

    for num_parties in GRAPHITI_PARTY_COUNTS:
        runs: Dict[str, RunAggregate | None] = {}
        specs = {
            "graphiti_init": ("microbench_InitGraphiti", ("offline", "online")),
            "graphiti_mpa": ("bench_PrMpaGraphiti", ("offline", "online")),
            "grasp": ("bench_PrMpaGraSP", ("offline", "init", "online")),
        }
        for key, (benchmark, phases) in specs.items():
            try:
                runs[key] = _latest_party_graph_run(
                    results_dir,
                    benchmark,
                    num_parties,
                    GRAPHITI_PARTY_GRAPH_SIZE,
                    phases,
                )
            except FileNotFoundError as exc:
                if strict_missing:
                    raise
                print(f"Warning: {exc}", file=sys.stderr)
                runs[key] = None

        graphiti_init = runs["graphiti_init"]
        graphiti_mpa = runs["graphiti_mpa"]
        grasp = runs["grasp"]
        online_rows.extend((
            {
                "Ref": "Graphiti",
                "Num Parties": num_parties,
                "Init Time (s)": _time_s(graphiti_init, "online"),
                "MPA Time (s)": _time_s(graphiti_mpa, "online"),
                "Init Comm (MiB)": _sent_mb(graphiti_init, "online"),
                "MPA Comm (MiB)": _sent_mb(graphiti_mpa, "online"),
            },
            {
                "Ref": "GraSP",
                "Num Parties": num_parties,
                "Init Time (s)": _time_s(grasp, "init"),
                "MPA Time (s)": _time_s(grasp, "online"),
                "Init Comm (MiB)": _sent_mb(grasp, "init"),
                "MPA Comm (MiB)": _sent_mb(grasp, "online"),
            },
        ))

        graphiti_complete = graphiti_init is not None and graphiti_mpa is not None
        preprocessing_rows.extend((
            {
                "Ref": "Graphiti",
                "Num Parties": num_parties,
                "Preprocessing Time (s)": (
                    f"{(graphiti_init.phases['offline'].max_time_ms + graphiti_mpa.phases['offline'].max_time_ms) / 1000.0:.2f}"
                    if graphiti_complete else "—"
                ),
                "Preprocessing Comm (MiB)": (
                    f"{(graphiti_init.phases['offline'].sum_sent_bytes + graphiti_mpa.phases['offline'].sum_sent_bytes) / BYTES_PER_MB:.2f}"
                    if graphiti_complete else "—"
                ),
            },
            {
                "Ref": "GraSP",
                "Num Parties": num_parties,
                "Preprocessing Time (s)": _time_s(grasp, "offline"),
                "Preprocessing Comm (MiB)": _sent_mb(grasp, "offline"),
            },
        ))

    return {"online": online_rows, "preprocessing": preprocessing_rows}


def build_application_party_tables(
    results_dir: Path,
    strict_missing: bool = False,
) -> Mapping[str, List[Mapping[str, object]]]:
    """Build application online and preprocessing tables by party count."""
    online_rows: List[Mapping[str, object]] = []
    preprocessing_rows: List[Mapping[str, object]] = []

    for application in APPLICATIONS:
        for num_parties in APPLICATION_PARTY_COUNTS:
            try:
                run = _latest_party_graph_phase_groups(
                    results_dir,
                    application,
                    num_parties,
                    APPLICATION_GRAPH_SIZE,
                    {
                        "online": ("init", "online"),
                        "preprocessing": ("offline",),
                    },
                )
            except FileNotFoundError as exc:
                if strict_missing:
                    raise
                print(f"Warning: {exc}", file=sys.stderr)
                run = None

            online_rows.append({
                "Application": application,
                "Num Parties": num_parties,
                "Online Time (Init + Online) (s)": _time_s(
                    run, "online"
                ),
                "Online Comm (Init + Online) (MiB)": _sent_mb(
                    run, "online"
                ),
            })
            preprocessing_rows.append({
                "Application": application,
                "Num Parties": num_parties,
                "Preprocessing Time (s)": _time_s(run, "preprocessing"),
                "Preprocessing Comm (MiB)": _sent_mb(run, "preprocessing"),
            })

    return {"online": online_rows, "preprocessing": preprocessing_rows}


def write_application_party_tables(
    tables: Mapping[str, List[Mapping[str, object]]], out_dir: Path
) -> None:
    """Write application party-count tables in CSV and Markdown."""
    titles = {
        "online": (
            "Application Online (Init + Online) Runtime and Communication "
            "by Number of Parties"
        ),
        "preprocessing": (
            "Application Preprocessing Runtime and Communication "
            "by Number of Parties"
        ),
    }
    out_dir.mkdir(parents=True, exist_ok=True)
    combined: List[str] = []
    for name in ("online", "preprocessing"):
        rows = tables[name]
        write_csv(rows, out_dir / f"{name}_table.csv")
        md_file = out_dir / f"{name}_table.md"
        write_markdown_table(rows, md_file, titles[name])
        combined.append(md_file.read_text(encoding="utf-8").rstrip())
    (out_dir / "comparison_tables.md").write_text(
        "\n\n".join(combined) + "\n", encoding="utf-8"
    )


def write_graphiti_party_tables(
    tables: Mapping[str, List[Mapping[str, object]]], out_dir: Path
) -> None:
    """Write party-count tables in CSV, Markdown, and combined Markdown."""
    titles = {
        "online": "Online Runtime and Communication by Number of Parties",
        "preprocessing": "Preprocessing Runtime and Communication by Number of Parties",
    }
    out_dir.mkdir(parents=True, exist_ok=True)
    combined: List[str] = []
    for name in ("online", "preprocessing"):
        rows = tables[name]
        write_csv(rows, out_dir / f"{name}_table.csv")
        md_file = out_dir / f"{name}_table.md"
        write_markdown_table(rows, md_file, titles[name])
        combined.append(md_file.read_text(encoding="utf-8").rstrip())
    (out_dir / "comparison_tables.md").write_text(
        "\n\n".join(combined) + "\n", encoding="utf-8"
    )


def write_comparison_tables(
    tables: Mapping[str, List[Mapping[str, object]]], out_dir: Path
) -> None:
    titles = {
        "online": "Online Runtime and Communication by Graph Size",
        "preprocessing": "Preprocessing Runtime and Communication by Graph Size",
    }
    out_dir.mkdir(parents=True, exist_ok=True)
    combined: List[str] = []
    for name in ("online", "preprocessing"):
        rows = tables[name]
        write_csv(rows, out_dir / f"{name}_table.csv")
        md_file = out_dir / f"{name}_table.md"
        write_markdown_table(rows, md_file, titles[name])
        combined.append(md_file.read_text(encoding="utf-8").rstrip())
    (out_dir / "comparison_tables.md").write_text(
        "\n\n".join(combined) + "\n", encoding="utf-8"
    )


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
    ringsg_wan: bool = False,
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
        agg = parse_summary(summary_file, ("init", "online"))
        rows.append({
            "Ref": "GraSP",
            "Graph Size": size,
            "Online Time (s)": round(
                (agg.phases["init"].max_time_ms
                 + agg.phases["online"].max_time_ms) / 1000.0,
                2,
            ),
            "Online Comm (MiB)": round(
                (agg.phases["init"].sum_sent_bytes
                 + agg.phases["online"].sum_sent_bytes) / BYTES_PER_MB,
                2,
            ),
        })
        ringsg_results = (
            RINGSG_WAN_GRAPH_SIZE_RESULTS
            if ringsg_wan else RINGSG_GRAPH_SIZE_RESULTS
        )
        if fixed_parties == 5 and size in ringsg_results:
            runtime_s, communication_mib = ringsg_results[size]
            rows.append({
                "Ref": "RingSG",
                "Graph Size": size,
                "Online Time (s)": (
                    f"{runtime_s:.3f}" if ringsg_wan else runtime_s
                ),
                "Online Comm (MiB)": communication_mib,
            })

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
    ringsg_wan: bool = False,
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
        agg = parse_summary(summary_file, ("init", "online"))
        rows.append({
            "Ref": "GraSP",
            "Num Parties": num_parties,
            "Online Time (s)": round(
                (agg.phases["init"].max_time_ms
                 + agg.phases["online"].max_time_ms) / 1000.0,
                2,
            ),
            "Online Comm (MiB)": round(
                (agg.phases["init"].sum_sent_bytes
                 + agg.phases["online"].sum_sent_bytes) / BYTES_PER_MB,
                2,
            ),
        })
        ringsg_results = (
            RINGSG_WAN_PARTY_RESULTS if ringsg_wan else RINGSG_PARTY_RESULTS
        )
        if fixed_size == 2**16 and num_parties in ringsg_results:
            runtime_s, communication_mib = ringsg_results[num_parties]
            rows.append({
                "Ref": "RingSG",
                "Num Parties": num_parties,
                "Online Time (s)": (
                    f"{runtime_s:.3f}" if ringsg_wan else runtime_s
                ),
                "Online Comm (MiB)": communication_mib,
            })

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
