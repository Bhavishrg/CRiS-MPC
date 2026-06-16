// bench_linear.cpp
//
// Benchmark for straight-line arithmetic computations in the 3PC RSS framework.
//
// This benchmark exercises:
//   - kAdd
//   - kSub
//   - kMul
//   - kCAdd
//   - kCSub
//   - kCMul
//
// Options:
//   --pid          <0|1|2>   Party ID
//   --vec-size     <N>       Number of independent lanes
//   --chain-depth  <D>       Number of arithmetic rounds per lane
//   --port         <int>     Base port
//   --peer         <addr>    Peer IP address
//   --repeat       <int>     Number of repetitions
//   --output       <file>    Save JSON output
//
// Circuit per lane:
//
//   state = x
//
//   For r = 0 to chain_depth - 1:
//
//     a = state + y
//     b = a + const
//     c = b - x
//     d = c * y
//     e = d * const
//     f = e - const
//     g = const - f
//     state = g + state
//
//   output = state
//
// Inputs:
//   x is owned by P0.
//   y is owned by P1.

#include "3pc/arith/offline_evaluator.h"
#include "3pc/arith/online_evaluator.h"
#include "common/circuit/circuit.h"
#include "3pc/net/net3p.h"
#include "benchmark/utils.h"

#include <algorithm>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

using namespace threepc;
using T = uint64_t;

// ── Circuit construction ──────────────────────────────────────────────────────

struct CircuitData {
    LevelOrderedCircuit lc;

    std::vector<wire_t> in0;      // P0-owned inputs
    std::vector<wire_t> in1;      // P1-owned inputs
    std::vector<wire_t> outputs;  // final arithmetic outputs
};

CircuitData generateCircuit(size_t n, size_t chain_depth) {
    Circuit<T> c;
    CircuitData cd;

    cd.in0.resize(n);
    cd.in1.resize(n);
    cd.outputs.resize(n);

    for (size_t i = 0; i < n; ++i) {
        cd.in0[i] = c.newInputWire(P0);
    }

    for (size_t i = 0; i < n; ++i) {
        cd.in1[i] = c.newInputWire(P1);
    }

    for (size_t i = 0; i < n; ++i) {
        wire_t state = cd.in0[i];

        for (size_t r = 0; r < chain_depth; ++r) {
            const T c_add = static_cast<T>(r + 5);
            const T c_mul = static_cast<T>(r + 2);
            const T c_sub = static_cast<T>(r + 3);
            const T c_inv = static_cast<T>(1000 + r);

            // a = state + y
            wire_t a = c.addGate(GateType::kAdd, state, cd.in1[i]);

            // b = a + c_add
            wire_t b = c.addCGate(GateType::kCAdd, a, c_add);

            // cval = b - x
            wire_t cval = c.addGate(GateType::kSub, b, cd.in0[i]);

            // d = cval * y
            wire_t d = c.addGate(GateType::kMul, cval, cd.in1[i]);

            // e = d * c_mul
            wire_t e = c.addCGate(GateType::kCMul, d, c_mul);

            // f = e - c_sub
            wire_t f = c.addCGate(GateType::kCSub, e, c_sub);

            // state = f + state
            state = c.addGate(GateType::kAdd, f, state);
        }

        cd.outputs[i] = state;
        c.setAsOutput(cd.outputs[i]);
    }

    cd.lc = c.orderGatesByLevel();
    return cd;
}

// ── Plaintext expected computation ────────────────────────────────────────────

static std::vector<T> computeExpected(const std::vector<T>& in0,
                                      const std::vector<T>& in1,
                                      size_t chain_depth) {
    const size_t n = in0.size();
    std::vector<T> expected(n);

    for (size_t i = 0; i < n; ++i) {
        T state = in0[i];

        for (size_t r = 0; r < chain_depth; ++r) {
            const T c_add = static_cast<T>(r + 5);
            const T c_mul = static_cast<T>(r + 2);
            const T c_sub = static_cast<T>(r + 3);
            const T c_inv = static_cast<T>(1000 + r);

            T a = state + in1[i];
            T b = a + c_add;
            T cval = b - in0[i];
            T d = cval * in1[i];
            T e = d * c_mul;
            T f = e - c_sub;

            state = f + state;
        }

        expected[i] = state;
    }

    return expected;
}

// ── CLI ───────────────────────────────────────────────────────────────────────

struct Args {
    int pid = -1;
    size_t vec_size = 0;
    size_t chain_depth = 1;
    int port = 13800;
    std::string peer = "127.0.0.1";
    int repeat = 1;
    std::string output;
};

static void printUsage(const char* prog) {
    std::fprintf(stderr,
        "Usage: %s --pid <0|1|2> --vec-size <N> "
        "[--chain-depth <D>] [--port <p>] [--peer <addr>] "
        "[--repeat <r>] [--output <file>]\n",
        prog);
}

static Args parseArgs(int argc, char* argv[]) {
    Args a;

    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--pid") == 0 && i + 1 < argc) {
            a.pid = std::atoi(argv[++i]);
        } else if (std::strcmp(argv[i], "--vec-size") == 0 && i + 1 < argc) {
            a.vec_size = static_cast<size_t>(std::atoll(argv[++i]));
        } else if (std::strcmp(argv[i], "--chain-depth") == 0 && i + 1 < argc) {
            a.chain_depth = static_cast<size_t>(std::atoll(argv[++i]));
        } else if (std::strcmp(argv[i], "--port") == 0 && i + 1 < argc) {
            a.port = std::atoi(argv[++i]);
        } else if (std::strcmp(argv[i], "--peer") == 0 && i + 1 < argc) {
            a.peer = argv[++i];
        } else if (std::strcmp(argv[i], "--repeat") == 0 && i + 1 < argc) {
            a.repeat = std::atoi(argv[++i]);
        } else if (std::strcmp(argv[i], "--output") == 0 && i + 1 < argc) {
            a.output = argv[++i];
        } else {
            std::fprintf(stderr, "Unknown argument: %s\n", argv[i]);
            printUsage(argv[0]);
            std::exit(1);
        }
    }

    if (a.pid < 0 || a.pid > 2 ||
        a.vec_size == 0 ||
        a.chain_depth == 0 ||
        a.repeat <= 0) {
        printUsage(argv[0]);
        std::exit(1);
    }

    return a;
}

// ── Printing helpers ──────────────────────────────────────────────────────────

static void printVector(const char* label,
                        int pid,
                        const std::vector<T>& v,
                        size_t max_items = 20) {
    std::printf("[P%d]   %s: [", pid, label);

    const size_t m = std::min(max_items, v.size());
    for (size_t i = 0; i < m; ++i) {
        std::printf("%llu%s",
                    static_cast<unsigned long long>(v[i]),
                    i + 1 < m ? ", " : "");
    }

    if (v.size() > max_items) {
        std::printf(", ...");
    }

    std::printf("]\n");
}

// ── Benchmark ─────────────────────────────────────────────────────────────────

static void benchmark(const Args& args) {
    using SP = bench::StatsPoint<Net3P>;

    const int pid = args.pid;
    const size_t n = args.vec_size;
    const size_t chain_depth = args.chain_depth;

    std::printf("\n=== bench_linear ===\n");
    std::printf("  pid         : %d\n", pid);
    std::printf("  vec_size    : %zu\n", n);
    std::printf("  chain_depth : %zu\n", chain_depth);
    std::printf("  port        : %d\n", args.port);
    std::printf("  peer        : %s\n", args.peer.c_str());
    std::printf("  repeat      : %d\n\n", args.repeat);

    // ── Circuit ───────────────────────────────────────────────────────────────

    std::printf("[P%d] Building circuit...\n", pid);
    auto cd = generateCircuit(n, chain_depth);
    const LevelOrderedCircuit& lc = cd.lc;

    std::printf("[P%d] Circuit: %zu gates, %zu wires, depth %zu\n\n",
                pid, lc.num_gates, lc.num_wires, lc.depth());

    std::vector<T> vals0(n);
    std::vector<T> vals1(n);

    for (size_t i = 0; i < n; ++i) {
        vals0[i] = static_cast<T>(i + 1);
        vals1[i] = static_cast<T>((i % 7) + 2);
    }

    std::vector<T> expected = computeExpected(vals0, vals1, chain_depth);

    if (n <= 20) {
        printVector("in0", pid, vals0);
        printVector("in1", pid, vals1);
        printVector("expected", pid, expected);
    }

    // ── Network ───────────────────────────────────────────────────────────────

    const char* peer_c = args.peer.c_str();
    const char* ips[3] = {peer_c, peer_c, peer_c};

    std::printf("\n[P%d] Connecting...\n", pid);
    Net3P net(pid, ips, args.port);
    std::printf("[P%d] Connected.\n\n", pid);

    bench::increaseSocketBuffers(net, 128 * 1024 * 1024);

    // ── JSON accumulator ──────────────────────────────────────────────────────

    nlohmann::json output_doc;
    output_doc["details"] = {
        {"pid", pid},
        {"vec_size", n},
        {"chain_depth", chain_depth},
        {"port", args.port},
        {"peer", args.peer},
        {"repeat", args.repeat}
    };
    output_doc["runs"] = nlohmann::json::array();

    // ── Benchmark loop ────────────────────────────────────────────────────────

    for (int run = 0; run < args.repeat; ++run) {
        if (args.repeat > 1) {
            std::printf("[P%d] --- Run %d / %d ---\n",
                        pid, run + 1, args.repeat);
        }

        net.resetCounters();

        // Offline
        std::printf("[P%d] Offline...\n", pid);
        SP offline_start(net);

        OfflineEvaluator<T> offline(pid, net);
        offline.run(lc);

        SP offline_end(net);
        auto offline_stats = offline_end - offline_start;

        // Online
        std::printf("[P%d] Online...\n", pid);
        OnlineEvaluator<T> ev(pid, net, offline.take_prg());

        if (pid == P0) {
            ev.setInputs(cd.in0, vals0);
        }

        if (pid == P1) {
            ev.setInputs(cd.in1, vals1);
        }

        SP online_start(net);

        ev.evaluate(lc);
        auto outputs = ev.getOutputs(lc);

        SP online_end(net);
        auto online_stats = online_end - online_start;

        const std::vector<T>& out = outputs.vals;

        // ── Correctness ───────────────────────────────────────────────────────

        bool ok = (out.size() == expected.size());

        if (ok) {
            for (size_t i = 0; i < n; ++i) {
                if (out[i] != expected[i]) {
                    ok = false;
                    break;
                }
            }
        }

        std::printf("[P%d] Linear arithmetic correctness: %s\n",
                    pid, ok ? "PASS" : "FAIL");

        if (!ok || n <= 20) {
            printVector("output", pid, out);
        }

        // ── Stats ─────────────────────────────────────────────────────────────

        nlohmann::json total_stats = {
            {"time_ms",
             offline_stats["time_ms"].get<double>() +
             online_stats["time_ms"].get<double>()},
            {"total_bytes_sent",
             offline_stats["total_bytes_sent"].get<uint64_t>() +
             online_stats["total_bytes_sent"].get<uint64_t>()},
            {"total_bytes_recv",
             offline_stats["total_bytes_recv"].get<uint64_t>() +
             online_stats["total_bytes_recv"].get<uint64_t>()}
        };

        std::printf("\n[P%d] --- Run %d stats ---\n", pid, run + 1);
        bench::printPhaseStats(pid, "offline", offline_stats);
        bench::printPhaseStats(pid, "online", online_stats);

        std::printf("[P%d] %-18s  time: %9.3f ms  sent: %zu B  recv: %zu B\n\n",
                    pid,
                    "total",
                    total_stats["time_ms"].get<double>(),
                    static_cast<size_t>(total_stats["total_bytes_sent"].get<uint64_t>()),
                    static_cast<size_t>(total_stats["total_bytes_recv"].get<uint64_t>()));

        output_doc["runs"].push_back({
            {"run", run + 1},
            {"correct", ok},
            {"offline", offline_stats},
            {"online", online_stats},
            {"total", total_stats}
        });
    }

    output_doc["memory"] = {
        {"peak_virtual_memory_kb", bench::peakVirtualMemory()},
        {"peak_resident_set_size_kb", bench::peakResidentSetSize()}
    };

    std::printf("[P%d] Peak virtual memory:  %" PRId64 " kB\n",
                pid, bench::peakVirtualMemory());
    std::printf("[P%d] Peak resident memory: %" PRId64 " kB\n\n",
                pid, bench::peakResidentSetSize());

    if (!args.output.empty()) {
        bench::saveJson(output_doc, args.output);
    }
}

int main(int argc, char* argv[]) {
    Args args = parseArgs(argc, argv);

    try {
        benchmark(args);
    } catch (const std::exception& ex) {
        std::fprintf(stderr, "[P%d] Fatal: %s\n", args.pid, ex.what());
        return 1;
    }

    return 0;
}
