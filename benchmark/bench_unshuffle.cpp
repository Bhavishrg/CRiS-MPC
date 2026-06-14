// bench_unshuffle.cpp
//
// Benchmark and correctness test for the grouped shuffle/unshuffle gates.
//
// Circuit:
//   1. P0 provides a secret input vector x[0..n-1].
//   2. The circuit applies a random grouped shuffle using perm_group_id.
//   3. The circuit applies an unshuffle gate with the same perm_group_id.
//   4. The final vector is reconstructed to all parties.
//
// Expected output:
//   unshuffle(shuffle(x)) = x
//
// This benchmark directly tests that kUnshuffle applies the inverse of the
// hidden permutation cached for the grouped kShuffle gate.

#include "3pc/arith/offline_evaluator.h"
#include "3pc/arith/online_evaluator.h"
#include "3pc/circuit/circuit.h"
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

struct CircuitData {
    LevelOrderedCircuit lc;

    std::vector<wire_t> input_wires;
    std::vector<wire_t> shuffled_wires;
    std::vector<wire_t> unshuffled_wires;
};

static CircuitData generateCircuit(size_t vec_size) {
    Circuit<T> c;
    CircuitData cd;

    cd.input_wires.resize(vec_size);
    for (size_t i = 0; i < vec_size; ++i) {
        cd.input_wires[i] = c.newInputWire(P0);
    }

    /*
     * Use one explicit fresh permutation group for the pair:
     *
     *   shuffle(x, gid)
     *   unshuffle(shuffle(x), gid)
     *
     * The unshuffle gate must reuse exactly the same cached pairwise
     * permutations from this group and apply their inverses in reverse order.
     */
    const int perm_group_id = c.freshPermGroupId();

    cd.shuffled_wires = c.addShuffleGate(cd.input_wires, perm_group_id);
    cd.unshuffled_wires = c.addUnshuffleGate(cd.shuffled_wires, perm_group_id);

    for (wire_t w : cd.unshuffled_wires) {
        c.setAsOutput(w);
    }

    cd.lc = c.orderGatesByLevel();
    return cd;
}

static std::vector<T> makeInputValues(size_t vec_size) {
    std::vector<T> vals(vec_size);
    for (size_t i = 0; i < vec_size; ++i) {
        // Deterministic nontrivial values, easy to visually inspect.
        vals[i] = static_cast<T>(1000 + 17 * i + (i % 5));
    }
    return vals;
}

struct Args {
    int pid = -1;
    size_t vec_size = 0;
    int port = 14200;
    std::string peer = "127.0.0.1";
    int repeat = 1;
    std::string output;
};

static void printUsage(const char* prog) {
    std::fprintf(stderr,
        "Usage: %s --pid <0|1|2> --vec-size <N> "
        "[--port <p>] [--peer <addr>] [--repeat <r>] [--output <file>]\n",
        prog);
}

static Args parseArgs(int argc, char* argv[]) {
    Args a;

    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--pid") == 0 && i + 1 < argc) {
            a.pid = std::atoi(argv[++i]);
        } else if (std::strcmp(argv[i], "--vec-size") == 0 && i + 1 < argc) {
            a.vec_size = static_cast<size_t>(std::atoll(argv[++i]));
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

    if (a.pid < 0 || a.pid > 2 || a.vec_size == 0 || a.repeat <= 0) {
        printUsage(argv[0]);
        std::exit(1);
    }

    return a;
}

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

static void benchmark(const Args& args) {
    using SP = bench::StatsPoint<Net3P>;

    const int pid = args.pid;
    const size_t n = args.vec_size;

    std::printf("\n=== bench_unshuffle ===\n");
    std::printf("  pid      : %d\n", pid);
    std::printf("  vec_size : %zu\n", n);
    std::printf("  port     : %d\n", args.port);
    std::printf("  peer     : %s\n", args.peer.c_str());
    std::printf("  repeat   : %d\n\n", args.repeat);

    std::printf("[P%d] Building circuit...\n", pid);
    auto cd = generateCircuit(n);
    const LevelOrderedCircuit& lc = cd.lc;

    std::printf("[P%d] Circuit: %zu gates, %zu wires, depth %zu\n\n",
                pid, lc.num_gates, lc.num_wires, lc.depth());

    std::vector<T> input_vals = makeInputValues(n);
    std::vector<T> expected = input_vals;

    if (n <= 20) {
        printVector("input", pid, input_vals);
        printVector("expected", pid, expected);
    }

    const char* peer_c = args.peer.c_str();
    const char* ips[3] = {peer_c, peer_c, peer_c};

    std::printf("\n[P%d] Connecting...\n", pid);
    Net3P net(pid, ips, args.port);
    std::printf("[P%d] Connected.\n\n", pid);

    bench::increaseSocketBuffers(net, 128 * 1024 * 1024);

    nlohmann::json output_doc;
    output_doc["details"] = {
        {"pid", pid},
        {"vec_size", n},
        {"port", args.port},
        {"peer", args.peer},
        {"repeat", args.repeat}
    };
    output_doc["runs"] = nlohmann::json::array();

    for (int run = 0; run < args.repeat; ++run) {
        if (args.repeat > 1) {
            std::printf("[P%d] --- Run %d / %d ---\n",
                        pid, run + 1, args.repeat);
        }

        net.resetCounters();

        std::printf("[P%d] Offline...\n", pid);
        SP offline_start(net);

        OfflineEvaluator<T> offline(pid, net);
        offline.run(lc);

        SP offline_end(net);
        auto offline_stats = offline_end - offline_start;

        std::printf("[P%d] Online...\n", pid);
        OnlineEvaluator<T> ev(pid, net, offline.take_prg());

        if (pid == P0) {
            ev.setInputs(cd.input_wires, input_vals);
        }

        SP online_start(net);

        ev.evaluate(lc);
        auto outputs = ev.getOutputs(lc);

        SP online_end(net);
        auto online_stats = online_end - online_start;

        const std::vector<T>& out = outputs.vals;

        bool ok = (out == expected);

        std::printf("[P%d] Unshuffle correctness: %s\n",
                    pid, ok ? "PASS" : "FAIL");

        if (!ok || n <= 20) {
            printVector("output", pid, out);
        }

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
