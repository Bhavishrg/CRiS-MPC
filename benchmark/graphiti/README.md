# Graphiti benchmarks

This directory contains the implementation of **Graphiti: Secure Graph Computation Made More Scalable**. See [IACR ePrint 2024/1756](https://eprint.iacr.org/2024/1756). It includes secure BFS and PageRank computation, shuffle protocol benchmarks, and a microbenchmark for Graphiti's initialization. The code is intended for research and experimental comparison.

## Implementations


### `bench_bfs_mpa`

This benchmark implements bounded-hop breadth-first search over the secret-shared graph list.

Program options:

- `--graph-size N`: Vertices + Edges
- `--num-hops H`: number of BFS expansion rounds; default `10`
- `--source-vertex V`: public source vertex; default `0`
- `--skip-final-applyv`: leave the raw count-oriented result instead of applying the final vertex ordering
- `--no-check`: disable cleartext validation
- `--seed S`: deterministic graph seed

`--graph-size N` sets:

```text
num_vertices = N / 10
num_edges    = N - num_vertices
```

Use `--num-verts V --num-edges E` when an explicit split is required.

Correctness checking is skipped for sufficiently large graphs.

### `bench_pagerank_mpa`

This benchmark implements PageRank computation using message passing.

The current benchmark uses `alpha = 1` and a secret-shared `rho = 1`.

Program options:

- `--num-iters R`: PageRank iterations; default `10`
- `--no-check`: disable cleartext validation
- `--seed S`: deterministic graph seed

Correctness checking is skipped for large graphs.

### `bench_repeat_shuffle`

This benchmark sequentially applies the same random permutation to a vector using secure shuffle.

Program options:

- `--vec-size N`: number of elements
- `--num-repeats R`: number of shuffle applications

### `microbench_graphiti_init`

This benchmark builds a synthetic circuit that models Graphiti initialization, including three aligned-column shuffles and source/destination sorts. Each sort instantiates the full balanced quicksort comparison model at its natural depth, $\lceil\log_2(N)\rceil$. Comparisons at the same depth are evaluated in parallel, while consecutive depths and initialization stages retain their communication dependencies.

Program options:

- `--graph-size N`: total number of vertex and edge rows, using the standard `N/10` vertex split
- `--num-verts V --num-edges E`: explicit graph dimensions
- `--seed S`: deterministic graph seed

The number of modeled comparisons grows as $O(N\log N)$, and each comparison creates LTZ and reconstruction gates. Large graph sizes can therefore require substantial circuit-construction time, memory, preprocessing, and communication.

## Build

From the repository root:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
```

The resulting programs are:

```text
build/benchmark/bench_bfs_mpa
build/benchmark/bench_pagerank_mpa
build/benchmark/bench_repeat_shuffle
build/benchmark/microbench_graphiti_init
```

See the root `README.md` for dependencies and protocol details.

## Run

Run commands from the repository root. `run.sh` starts all processes locally, adds PID and network arguments, and saves their logs.

### BFS

Run BFS with two NPH compute parties and one helper:

```bash
./run.sh bench_bfs_mpa \
  --protocol nph --num-parties 2 \
  --graph-size 10000 --num-hops 10 --source-vertex 0
```

Run the same workload with explicit graph dimensions:

```bash
./run.sh bench_bfs_mpa \
  --protocol nph --num-parties 3 \
  --num-verts 1000 --num-edges 9000 \
  --num-hops 10 --source-vertex 5
```

### PageRank

```bash
./run.sh bench_pagerank_mpa \
  --protocol nph --num-parties 2 \
  --graph-size 10000 --num-iters 10
```

RSS3 is also supported by the BFS and PageRank implementations:

```bash
./run.sh bench_pagerank_mpa \
  --protocol rss3 --num-parties 3 \
  --graph-size 10000 --num-iters 10
```

### Repeated shuffle

```bash
./run.sh bench_repeat_shuffle \
  --protocol nph --num-parties 2 \
  --vec-size 10000 --num-repeats 10
```

### Initialization microbenchmark

```bash
./run.sh microbench_graphiti_init \
  --protocol nph --num-parties 2 \
  --graph-size 10000
```

## Protocol and common options

The Graphiti programs accept these common options:

| Option | Meaning |
| --- | --- |
| `--protocol rss3\|nph` | Select the MPC backend |
| `--num-parties N` | Compute-party count; RSS3 requires `3`, while NPH requires at least `2` |
| `--pking` | For NPH, reconstruct through P0 and redistribute to compute parties |
| `--disable-optimized-shuffle` | Force the generic shuffle protocol based on Chase et. al. instead of the two-party optimized shuffle proposed in Graphiti |
| `--port P` | Base TCP port |
| `--peer ADDRESS` | Address used for local or remote peer connections |
| `--output FILE` | Write detailed benchmark statistics as JSON |

For NPH, compute-party PIDs are `0..N-1` and the preprocessing helper PID is `N`. The launcher starts all `N+1` processes.

## Results

By default, runs are stored under:

```text
Results/<benchmark>/protocol_<protocol>/parties_<n>/<graph-shape>/<timestamp>/
├── meta.txt
├── party_0.log
├── ...
└── summary.txt
```

For example, explicit graph dimensions produce:

```text
Results/bench_bfs_mpa/protocol_nph/parties_2/verts_1000/edges_9000/<timestamp>/
```

Override the base directory with `RESULTS_DIR`:

```bash
RESULTS_DIR=/tmp/graphiti-results ./run.sh bench_pagerank_mpa \
  --protocol nph --num-parties 2 --graph-size 10000
```

The logs report circuit size and depth, phase runtimes, bytes sent and received, peak memory, and correctness status.

## Network emulation

`network.sh` defines shell functions for loopback experiments. Source it before invoking a function:

```bash
source network.sh
tc_lan 1ms 1Gbit
```

For an NPH run, independent bandwidth and latency can be assigned to every unordered process pair. The base port must match the benchmark command:

```bash
source network.sh
tc_nph_pairs 2 14900 50ms 100Mbit

./run.sh bench_pagerank_mpa \
  --protocol nph --num-parties 2 --port 14900 \
  --graph-size 10000 --num-iters 10
```

