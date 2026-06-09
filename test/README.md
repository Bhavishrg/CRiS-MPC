# test

This folder contains the PSI 2PC test executables declared in `test/CMakeLists.txt`.

## Targets

- `test_net`: Basic network wrapper checks.
- `test_share`: Share-type behavior checks.
- `test_circuit`: Circuit construction and ordering checks.
- `test_eval`: End-to-end arithmetic evaluation and reconstruction behavior.
- `test_mul`: Beaver-triple-backed multiplication flow.
- `test_gc`: Garbled-circuit max on arithmetic inputs.
- `test_gc_arith`: Mixed GC plus arithmetic follow-up computation.
- `test_a2b`: Arithmetic-to-boolean conversion path.
- `test_shared_gc`: Shared-input garbled-circuit execution.
- `test_bool_shared_gc`: Bool-shared garbled-circuit execution.

## Build Output

After building, executables are available under `PSI/build/test/`.

## Running

Most tests are 2-party programs. Start party `0` first, then connect with party `1`.

Example:

```bash
cd PSI/build
./test/test_gc 0
./test/test_gc 1 127.0.0.1
```

Tests that depend on TinyGarble netlists receive the netlist root through CMake compile definitions.