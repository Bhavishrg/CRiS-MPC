# Multiprocess tests

The eight executables in this directory test the shared MPC primitives used by Graphiti and GraSP. CMake builds them under `build/test/`, but does not register them with CTest. Launch every required process manually; `run.sh` is a benchmark launcher and does not handle these positional test arguments.

## Targets

| Target | Coverage | Compute PIDs | Helper PID | Default base port |
| --- | --- | --- | --- | --- |
| `test_shuffle3p` | RSS3 shuffle | `0,1,2` | None | `13400` |
| `test_eqz3p` | RSS3 zero/equality checks | `0,1,2` | None | `13640` |
| `test_ltz3p` | RSS3 signed comparison | `0,1,2` | None | `13660` |
| `test_eqz_nph` | NPH zero/equality checks | `0,1,2` | `3` | `13600` |
| `test_ltz_nph` | NPH signed comparison | `0..n-1`, default `n=3` | `n` | `13700` |
| `test_shuffle_nph_2p` | Optimized NPH shuffle | `0,1` | `2` | `13700` |
| `test_shuffle_nph_2p_grouped` | Aligned grouped shuffle and inverse restoration | `0,1` | `2` | `13740` |
| `test_amor_permshare_nph` | Amortized Permute+Share for all targets, including aligned payloads | `0,1,2` | `3` | `13780` |

`test_eqz3p`, `test_ltz3p`, and `test_ltz_nph` exercise 8-, 16-, 32-, and 64-bit rings. The helper in NPH performs preprocessing; a helper-only `PASS` is not a check of reconstructed application outputs.

## Build

From the repository root, with the dependencies in the [root README](../README.md) installed:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
```

## Run

Start P0 first and launch all remaining processes for the same test. Each executable accepts a positional PID and an optional peer address, which defaults to `127.0.0.1`.

RSS3 equality example:

```bash
build/test/test_eqz3p 0 &
build/test/test_eqz3p 1 127.0.0.1 &
build/test/test_eqz3p 2 127.0.0.1 &
wait
```

Two-compute-party NPH grouped shuffle example (PID 2 is the helper):

```bash
build/test/test_shuffle_nph_2p_grouped 0 &
build/test/test_shuffle_nph_2p_grouped 1 127.0.0.1 &
build/test/test_shuffle_nph_2p_grouped 2 127.0.0.1 &
wait
```

Amortized Permute+Share requires four processes, including helper PID 3. Its source usage string currently lists only PIDs `0|1|2`, but the implementation uses three compute parties:

```bash
build/test/test_amor_permshare_nph 0 &
build/test/test_amor_permshare_nph 1 127.0.0.1 &
build/test/test_amor_permshare_nph 2 127.0.0.1 &
build/test/test_amor_permshare_nph 3 127.0.0.1 &
wait
```

`test_eqz_nph` also requires PIDs 0 through 3. It optionally accepts `--pking` on every process.

`test_eqz3p` and `test_ltz3p` accept an optional base port after the peer address. `test_ltz_nph` additionally accepts `--num-parties N` and `--pking`. For example, two compute parties and a helper on a custom base port:

```bash
build/test/test_ltz_nph 0 127.0.0.1 15000 --num-parties 2 --pking &
build/test/test_ltz_nph 1 127.0.0.1 15000 --num-parties 2 --pking &
build/test/test_ltz_nph 2 127.0.0.1 15000 --num-parties 2 --pking &
wait
```

Run test groups sequentially unless their port ranges are known not to overlap. The shuffle, amortized Permute+Share, and NPH equality tests have fixed base ports. Inspect every compute party's `PASS`/`FAIL` output; bare shell `wait` is not an aggregate test-success check.

## Application checks

The [Graphiti](../benchmark/graphiti/README.md) and [GraSP](../benchmark/grasp/README.md) application benchmarks include comparisons against plaintext reference computations. Run small workloads without `--no-check` and inspect their correctness status; some benchmarks skip checks at large sizes. These checks supplement the primitive tests and do not establish complete paper conformance or cryptographic security.
