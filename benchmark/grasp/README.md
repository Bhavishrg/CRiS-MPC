# GraSP

This directory contains the implementation  of
**GraSP: Secure Collaborative Graph Processing Made Scalable** : [IACR ePrint 2025/590](https://eprint.iacr.org/2025/590).

GraSP securely evaluates message-passing graph algorithms when a graph is
distributed among multiple data owners. Its Decompose-Compute-Combine (DCC)
paradigm decomposes the global computation into party-specific subgraph
computations, evaluates those computations in parallel, and combines their
results. The construction is designed to have online round complexity
independent of both graph size and the number of data owners.

This is research and benchmarking code. It has not been independently audited
and should not be used as a production cryptographic implementation.

## Paper-to-code map

| Paper component | Implementation | Build target |
| --- | --- | --- |
| DCC PageRank | `bench_PrMpaGraSP.cpp` | `bench_PrMpaGraSP` |
| Transaction-weighted risk propagation | `RiskPropagation.cpp` | `RiskPropagation` |
| Bounded-hop group-connection detection | `GroupConnection.cpp` | `GroupConnection` |
| Amortised Permute and Share | `../primitives/bench_amor_permshare.cpp` and the NPH evaluator | `bench_amor_permshare` |
| Experiment orchestration | `eval.py` | — |
| Result aggregation and tables | `parse_results.py` | — |

The three application targets implement the DCC initialization and iterative
message-passing phases and report offline preprocessing, initialization,
online, total, communication, and memory measurements.

## Security and execution model

The GraSP applications currently use the repository's NPH backend. There are
`n` compute parties with PIDs `0..n-1` and one preprocessing helper with PID
`n`. The implementation targets semi-honest security against collusion among
up to `n-1` compute parties and assumes that the helper does not collude with
the compute parties. See the paper for the complete model and proof.

`--num-parties` always denotes the number of compute parties; `run.sh` starts
the additional helper automatically. The optional `--pking` mode reconstructs
through P0 before redistributing values to the other compute parties.

## Build

Build from the repository root:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
```

The executables are written to `build/benchmark/`. See the root
[`README.md`](../../README.md) for dependencies and Docker instructions.

## Run the applications

Run all commands below from the repository root. For every application, either
provide a total graph size or explicit dimensions:

- `--graph-size N` sets `V=N/10` and `E=N-V`.
- `--num-verts V --num-edges E` sets the dimensions directly.
- `--seed S` makes synthetic graph generation deterministic.
- `--no-check` skips the cleartext correctness check, which is useful for large
  benchmark runs.
- `--output FILE` writes detailed per-process statistics as JSON.
- `--disable-optimized-shuffle` disables the optimized shuffle path for
  comparison experiments.

### PageRank

The PageRank benchmark defaults to ten message-passing iterations:

```bash
./run.sh bench_PrMpaGraSP \
  --protocol nph --num-parties 2 \
  --graph-size 10000 --num-iters 10
```

An equivalent run with explicit dimensions and five compute parties is:

```bash
./run.sh bench_PrMpaGraSP \
  --protocol nph --num-parties 5 --pking \
  --num-verts 1000 --num-edges 9000 \
  --num-iters 10
```

### Risk propagation

`RiskPropagation` implements the paper's transaction-weighted risk propagation
application. `--delta-a` and `--delta-b` configure the fixed-point attenuation
factor as `delta-a / 2^delta-b`; the defaults are `1` and `0`.

```bash
./run.sh RiskPropagation \
  --protocol nph --num-parties 5 --pking \
  --graph-size 10000 --num-iters 10 \
  --delta-a 1 --delta-b 0
```

### Group-connection detection

`GroupConnection` checks for a path, bounded by `--num-hops`, between two
private vertex groups. The group cardinalities default to one vertex each.

```bash
./run.sh GroupConnection \
  --protocol nph --num-parties 5 --pking \
  --graph-size 10000 --num-hops 10 \
  --group-a-size 1 --group-b-size 1
```

## Results

By default, `run.sh` creates one timestamped directory per run:

```text
Results/<target>/protocol_nph/parties_<n>/<workload>/<timestamp>/
├── meta.txt
├── party_0.log
├── ...
└── summary.txt
```

Set `RESULTS_DIR` to choose another base directory:

```bash
RESULTS_DIR=/tmp/grasp-results ./run.sh bench_PrMpaGraSP \
  --protocol nph --num-parties 3 \
  --num-verts 1000 --num-edges 3000 --num-iters 5
```

For each phase, `parse_results.py` reports the maximum elapsed time across
processes and the sum of bytes sent by all processes. The phases are `offline`,
`online`, and `total`. If several timestamped runs match a configuration, the
latest is selected.

## Reproduce evaluation sweeps

`eval.py` runs the supported evaluation suites through `run.sh` and generates
tables. Network emulation uses functions from `network.sh` and normally
requires `sudo`; pass `--skip-network` to skip network configuration.

### GraSP and RingSG comparison

Run both the graph-size and party-count sweeps under the default LAN profile
(`1ms`, `4Gbit`):

```bash
python3 benchmark/grasp/eval.py --comparison-ringsg
```

The default parameters are:

| Setting | Default |
| --- | --- |
| PageRank iterations | `10` |
| Fixed compute-party count | `5` |
| Fixed local graph size per party | `65536` |
| Fixed parties | `5` |
| Varyinh local graph size | `4096,8192,16384,32768,65536` |
| Varying parties | `3,4,5,6,7,8,9,10` |

Use `--mode graph-sweep` or `--mode party-sweep` to select one sweep. The WAN variant uses the corresponding paper comparison profile:

```bash
python3 benchmark/grasp/eval.py --comparison-ringsg-wan
```

### GraSP and Graphiti comparison

Run the fixed five-party graph-size comparison or the fixed-size party-count comparison:

```bash
python3 benchmark/grasp/eval.py --graphiti_graph_size
python3 benchmark/grasp/eval.py --graphiti_num_parties
```

### Application party-count evaluation

Run the implemented application sweep at graph size `2^21` for two through ten parties:

```bash
python3 benchmark/grasp/eval.py --applications_num_parties
```

### Regenerate tables without rerunning

Add `--skip-run --skip-network` to any suite to build tables from existing
and `--tc-off-at-end` to request best-effort traffic-control cleanup.


## Run the parser directly

The parser can auto-detect a populated results tree:

```bash
python3 benchmark/grasp/parse_results.py --skip-png
```

Or specify the GraSP protocol results directory:

```bash
python3 benchmark/grasp/parse_results.py \
  --results-root benchmark/grasp/Results/comparison_ringsg/bench_PrMpaGraSP/protocol_nph \
  --out-dir benchmark/grasp/Results/Tables/comparison_ringsg \
  --skip-png
```

CSV and Markdown output require only Python's standard library. Omit
`--skip-png` to generate PNG tables when Matplotlib is installed.

## Troubleshooting

- Build the repository before running a benchmark or evaluation driver.
- Run commands from the repository root unless `--repo-root` is supplied.
- Start P0 first if launching the processes manually; all processes must use
  the same peer and port.
- Use `--skip-network` if `sudo tc` is unavailable.
- Inspect `summary.txt` and the individual party logs after a failed run.
- Ensure parser sweep arguments match those used to generate the results.
- For high-bandwidth, high-latency experiments, use `raise_socket_mem_max` from
  `network.sh` to increase kernel socket-buffer limits.

## Citation

If you use this implementation, cite:

```bibtex
@article{kapoor2025mathsf,
  title={$$\backslash$mathsf $\{$GraSP$\}$ $: Secure Collaborative Graph Processing Made Scalable},
  author={Kapoor, Siddharth and Koti, Nishat and Kukkala, Varsha Bhat and Patra, Arpita and Gopal, Bhavish Raj},
  journal={Cryptology ePrint Archive},
  year={2025}
}
```
