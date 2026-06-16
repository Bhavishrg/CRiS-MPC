# CRiS-MPC

CRiS-MPC is a C++ implementation of a **3-party semi-honest secure computation framework** based on replicated secret sharing.

The framework supports arithmetic computation over rings and includes MPC primitives such as multiplication, reconstruction, permutation, shuffle, and higher-level subcircuits.

CRiS-MPC is intended for research and benchmarking.


## Repository Structure

```text
CRiS-MPC/
├── 3pc/
│   ├── arith/
│   │   ├── offline_evaluator.h
│   │   └── online_evaluator.h
│   ├── utils/
│   │   ├── prg3p.h
│   │   ├── share.h
│   │   └── types.h
│   └── net/
│       └── net3p.h
├── common/
│   ├── circuit/
│   │   ├── circuit.h
│   │   └── gate.h
│   ├── protocol_runner.h
│   └── types.h
├── nph/
│   ├── arith/
│   │   ├── offline_evaluator.h
│   │   └── online_evaluator.h
│   ├── net/
│   │   └── net_np.h
│   └── utils/
│       ├── prg_np.h
│       ├── share.h
│       └── types.h
├── benchmark/
│   ├── bench_gate.cpp
│   ├── bench_linear.cpp
│   ├── bench_mult.cpp
│   ├── bench_propagate.cpp
│   ├── bench_sort.cpp
│   ├── bench_unshuffle.cpp
│   ├── utils.cpp
│   ├── utils.h
│   └── CMakeLists.txt
├── test/
│   └── test_shuffle3p.cpp
├── CMakeLists.txt
├── Dockerfile
├── run.sh
└── README.md
```

## Dependencies

CRiS-MPC requires:

- C++17-compatible compiler
- CMake
- `emp-tool`
- `nlohmann_json`


## Building

From the repository root:

```bash
mkdir -p build
cd build
cmake ..
cmake --build . -j
```

The benchmark binaries are generated under:

```text
build/benchmark/
```


## Running Benchmarks

The repository includes a helper script `run.sh` that launches the three parties locally and saves their outputs.

Example:

```bash
./run.sh bench_propagate --vec-size 10 --num-groups 3
```

This runs parties `P0`, `P1`, and `P2` locally and stores logs in:

```text
Results/bench_propagate/vec_10/groups_3/<timestamp>/
```

Each party’s output is saved separately:

```text
party_0.log
party_1.log
party_2.log
summary.txt
```

A custom results directory can be specified using:

```bash
RESULTS_DIR=./Results/cris_mpc_results ./run.sh bench_propagate --vec-size 10 --num-groups 3
```

## Available Benchmarks

### `bench_gate`

Benchmarks vectorized basic gates such as addition, multiplication, and
constant operations.

```bash
./run.sh bench_gate --gate mul --x 10 --y 20 --vec-size 1000
```

### `bench_propagate`

Benchmarks the propagate subcircuit, which expands compact group-level values back to a larger list.

```bash
./run.sh bench_propagate --vec-size 10 --num-groups 3
```

For the default test layout with `vec-size = 10` and `num-groups = 3`, the benchmark uses three groups of sizes:

```text
3, 2, 5
```

and propagates compact group values over the corresponding larger list.

## Adding a New Benchmark

To add a new benchmark, create a file in the `benchmark/` directory.

For example:

```text
benchmark/bench_gather.cpp
```

Then add it to `benchmark/CMakeLists.txt`:

```cmake
add_3pc_benchmark(bench_gather)
```

The benchmark helper automatically links the common benchmark dependencies and includes the project root.

## Circuit API Overview

A circuit is built using the `Circuit<T>` class.

Example:

```cpp
Circuit<uint64_t> circ;

auto x = circ.newInputWire(P0);
auto y = circ.newInputWire(P1);

auto z = circ.addGate(GateType::kMul, x, y);

circ.setAsOutput(z);

auto lc = circ.orderGatesByLevel();
```

The resulting level-ordered circuit can then be evaluated using the offline and online evaluators.

## Evaluation Flow

A typical evaluation follows this structure:

```cpp
Net3P net(pid, ips, port);

OfflineEvaluator<uint64_t> offline(pid, net);
offline.run(level_ordered_circuit);

OnlineEvaluator<uint64_t> eval(pid, net, offline.take_prg());

eval.setInputs(input_wires, input_values);
eval.evaluate(level_ordered_circuit);

auto outputs = eval.getOutputs(level_ordered_circuit);
```

## Security Model

CRiS-MPC targets the **semi-honest 3-party setting**.

The implementation assumes that all parties follow the protocol specification but may try to learn additional information from their local views.

The current implementation does not provide malicious-security protections such as MAC verification, sacrifice, or consistency checks.

## Notes

- The implementation is intended for research and experimental benchmarking.
- Benchmarks are designed to run three local parties by default.
- Generated results, logs, and build files should not be committed.
- If `emp-tool` is installed separately, the local `emp-tool/` directory can be omitted from the repository.

## License

Add your license information here.
