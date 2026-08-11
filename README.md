# CRiS-MPC

CRiS-MPC is a C++17 framework for semi-honest secure multiparty computation over rings of the form $\mathbb{Z}_{2^k}$. It provides a circuit-building API, offline and online evaluators, and two protocol backends for research and benchmarking.

## Protocols

| Protocol | Sharing | Processes |
| --- | --- | --- |
| `rss3` | Three-party replicated secret sharing | Three compute parties, with PIDs `0`, `1`, and `2` |
| `nph` | N-party additive sharing | `n` compute parties with PIDs `0..n-1`, plus a preprocessing helper with PID `n` |

## Arithmetic primitives

The circuit API provides:

- Secret input sharing
- Addition and subtraction of shared values
- Addition, subtraction, and multiplication by a public constant
- Multiplication of two shared values
- Equality to zero and equality of two shared values
- Signed less-than-zero comparison
- Reconstruction to all parties
- Reconstruction to one selected party

Arithmetic is performed using native unsigned 8-, 16-, 32-, or 64-bit types. Overflow implements reduction modulo $2^k$. Signed comparison interprets the most significant bit using two's-complement representation.

## Repository structure

```text
CRiS-MPC/
├── src/
│   ├── common/
│   │   ├── circuit/           # Gates, circuit construction, and level ordering
│   │   ├── protocol_runner.h  # Common RSS3/NPH execution interface
│   │   └── types.h            # Ring and wire types
│   ├── 3pc/
│   │   ├── arith/             # RSS3 offline and online evaluators
│   │   ├── net/               # Three-party networking
│   │   └── utils/             # Replicated shares and pairwise PRGs
│   └── nph/
│       ├── arith/             # NPH offline and online evaluators
│       ├── net/               # N-party networking
│       └── utils/             # Additive shares, PRGs, and preprocessing types
├── benchmark/primitives/      # Primitive and arithmetic benchmarks
├── test/                      # Multi-process end-to-end tests
├── CMakeLists.txt
├── Dockerfile
└── run.sh
```

## Requirements

- A C++17-compatible compiler
- CMake 3.14 or newer
- A processor with AES-NI and SSE4.2 support
- [`emp-tool`](https://github.com/emp-toolkit/emp-tool), installed with its CMake package configuration
- OpenSSL and GMP, as required by `emp-tool`
- OpenMP (optional, used when available)
- Internet access during initial CMake configuration to fetch `nlohmann/json` 3.11.3

## Build

Install `emp-tool`, then configure and build from the repository root:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
```

Executables are generated under `build/benchmark/` and `build/test/`.

If `emp-tool` is installed under a non-standard prefix, pass it to CMake:

```bash
cmake -S . -B build \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_PREFIX_PATH=/path/to/emp-tool/install
```

### Docker

The supplied image builds `emp-tool` and CRiS-MPC on Ubuntu 22.04:

```bash
docker build -t cris-mpc .
docker run --rm -it cris-mpc
```

The image starts a shell in `/workspace`; compiled programs are in `/workspace/build/`.

## Circuit API

The following example constructs a vector of multiplications and reconstructs the results:

```cpp
#include "src/common/circuit/circuit.h"
#include "src/common/protocol_runner.h"

using T = uint64_t;
using namespace threepc;

Circuit<T> circuit;
auto x = circuit.newInputWire(P0);
auto y = circuit.newInputWire(P1);
auto product = circuit.addGate(GateType::kMul, x, y);
auto opened = circuit.addRecGate(product);
circuit.setAsOutput(opened);

auto ordered = circuit.orderGatesByLevel();

protocol::ProtocolConfig config;
config.kind = protocol::ProtocolKind::Rss3;
config.pid = pid;
config.num_compute_parties = 3;
config.peer = "127.0.0.1";
config.port = 13700;

auto runner = protocol::makeProtocolRunner<T>(config);
if (pid == P0) runner->setInputs({x}, {T{6}});
if (pid == P1) runner->setInputs({y}, {T{7}});
runner->offline(ordered);
runner->online(ordered);
auto outputs = runner->getOutputs(ordered);
```

For NPH, select `ProtocolKind::Nph`, set `num_compute_parties` to at least two, and start one additional process whose PID equals `num_compute_parties`.

## GraSP secure graph processing

This repository includes the implementation of **GraSP: Secure Collaborative
Graph Processing Made Scalable**. GraSP uses a Decompose-Compute-Combine
paradigm to evaluate message-passing graph algorithms over a graph distributed
among multiple data owners. The implementation provides PageRank,
transaction-weighted risk propagation, and bounded-hop group-connection
detection using the NPH backend.

Further details are available in [`benchmark/grasp` README](benchmark/grasp/README.md).

## Arithmetic benchmarks

The following primitive benchmarks are available:

| Target | Purpose | Protocol support |
| --- | --- | --- |
| `bench_gate` | Vectorized add, subtract, multiply, and public-constant gates | RSS3 and NPH |
| `bench_linear` | Chains of linear arithmetic and secret multiplication | RSS3 and NPH |
| `bench_mult` | Batched secret multiplication | RSS3 |

`run.sh` launches all required processes locally, records per-process logs, and writes a summary. For example:

```bash
./run.sh bench_gate \
  --protocol rss3 \
  --gate mul --x 10 --y 20 --vec-size 1000
```

Run the same gate using five compute parties and one NPH helper:

```bash
./run.sh bench_gate \
  --protocol nph --num-parties 5 \
  --gate mul --x 10 --y 20 --vec-size 1000
```

For NPH, `--num-parties` counts compute parties only. The launcher starts the helper automatically. The optional `--pking` flag makes P0 reconstruct values and redistribute them to the other compute parties.

Run the linear arithmetic chain through RSS3:

```bash
./run.sh bench_linear \
  --protocol rss3 \
  --vec-size 1000 --chain-depth 10
```

Or run it with five NPH compute parties and one helper:

```bash
./run.sh bench_linear \
  --protocol nph --num-parties 5 \
  --vec-size 1000 --chain-depth 10
```

The older `bench_mult` program accepts only RSS3 arguments and currently needs to be launched directly as three processes. For example:

```bash
build/benchmark/bench_mult --pid 0 --vec-size 1000 &
build/benchmark/bench_mult --pid 1 --vec-size 1000 &
build/benchmark/bench_mult --pid 2 --vec-size 1000 &
wait
```

All parties must use the same `--port` and `--peer` settings. Start P0 first when launching processes manually.

### Results

By default, `run.sh` stores results under:

```text
Results/<benchmark>/protocol_<protocol>/parties_<n>/<workload>/<timestamp>/
├── meta.txt
├── party_0.log
├── ...
└── summary.txt
```

Set `RESULTS_DIR` to change the base directory:

```bash
RESULTS_DIR=/tmp/cris-mpc-results ./run.sh bench_gate \
  --gate add --x 10 --y 20 --vec-size 1000
```

If `OMP_NUM_THREADS` is unset, the launcher divides the available hardware threads among the processes to avoid local oversubscription.

## Tests

The test programs are multi-process executables; they are built by CMake but are not registered with CTest.

| Target | Coverage | Processes |
| --- | --- | --- |
| `test_eqz3p` | RSS3 zero and equality checks | 3 |
| `test_ltz3p` | RSS3 signed less-than-zero | 3 |
| `test_eqz_nph` | NPH zero and equality checks | 3 compute + 1 helper |
| `test_ltz_nph` | NPH LTZ over 8-, 16-, 32-, and 64-bit rings | Configurable compute parties + 1 helper |

To run an RSS3 test locally:

```bash
build/test/test_eqz3p 0 &
build/test/test_eqz3p 1 127.0.0.1 &
build/test/test_eqz3p 2 127.0.0.1 &
wait
```

To run the default three-compute-party NPH equality test:

```bash
build/test/test_eqz_nph 0 &
build/test/test_eqz_nph 1 127.0.0.1 &
build/test/test_eqz_nph 2 127.0.0.1 &
build/test/test_eqz_nph 3 127.0.0.1 &
wait
```

Each program prints `PASS` or `FAIL` after reconstructing and checking its outputs. Use distinct ports when running multiple test groups concurrently.

## Security model

CRiS-MPC targets semi-honest adversaries i.e. parties are assumed to follow the protocol but may inspect their local views. The NPH backend assumes a dedicated preprocessing helper and assumes that the helper does not collude with other computing parties. The code is intended for research and experimental benchmarking and has not been independently audited for production use.

## License

No license is currently provided. All rights remain with the repository owner unless a license is added.
