// benchmark/bench_repeat_shuffle.cpp
//
// Benchmark for applying the same hidden shuffle permutation sequentially.
//
// Circuit:
//   1. P0 provides a secret input vector x[0..n-1].
//   2. The circuit applies kShuffle --num-repeats times, always using the same
//      perm_group_id, so every shuffle uses the same hidden permutation group.
//   3. The final vector is reconstructed.
//
// Correctness:
//   A repeated permutation preserves the multiset of input values, so the
//   benchmark checks that the final output is a permutation of the input.
//
// Usage:
//   ./run.sh bench_repeat_shuffle --protocol nph --num-parties 2 --vec-size 1000 --num-repeats 10
//   ./run.sh bench_repeat_shuffle --protocol nph --num-parties 2 --vec-size 1000 --num-repeats 10 --disable-optimized-shuffle
//   ./run.sh bench_repeat_shuffle --protocol rss3 --num-parties 3 --vec-size 1000 --num-repeats 10

#include "src/common/circuit/circuit.h"
#include "benchmark/utils.h"
#include "src/common/protocol_runner.h"

#include <algorithm>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

using namespace threepc;
using T = uint64_t;

namespace protocol = ::threepc::protocol;

struct CircuitData {
    LevelOrderedCircuit lc;
    std::vector<wire_t> input_wires;
    std::vector<wire_t> output_wires;
};

static CircuitData generateCircuit(size_t vec_size, int num_repeats) {
    Circuit<T> c;
    CircuitData cd;

    cd.input_wires.resize(vec_size);
    for (size_t i = 0; i < vec_size; ++i) {
        cd.input_wires[i] = c.newInputWire(P0);
    }

    const int perm_group_id = c.freshPermGroupId();
    std::vector<wire_t> current = cd.input_wires;
    for (int r = 0; r < num_repeats; ++r) {
        current = c.addShuffleGate(current, perm_group_id);
    }

    cd.output_wires = current;
    for (wire_t w : cd.output_wires) {
        c.setAsOutput(w);
    }

    cd.lc = c.orderGatesByLevel();
    return cd;
}

static std::vector<T> makeInputValues(size_t vec_size) {
    std::vector<T> vals(vec_size);
    for (size_t i = 0; i < vec_size; ++i) {
        vals[i] = static_cast<T>(1000003 + 37 * i + (i % 11));
    }
    return vals;
}

static bool sameMultiset(std::vector<T> got, std::vector<T> expected) {
    std::sort(got.begin(), got.end());
    std::sort(expected.begin(), expected.end());
    return got == expected;
}

struct Args {
    int pid = -1;

    std::string protocol_name = "nph";
    protocol::ProtocolKind protocol = protocol::ProtocolKind::Nph;
    int num_parties = 2;  // compute parties; nph has one extra helper process
    bool pking = false;
    bool disable_optimized_shuffle = false;

    size_t vec_size = 0;
    int num_repeats = 0;

    int port = 14700;
    std::string peer = "127.0.0.1";
    std::string output;
};

static void printUsage(const char* prog) {
    std::fprintf(stderr,
        "Usage: %s --pid <pid> --protocol <rss3|nph> --num-parties <n> "
        "--vec-size <N> --num-repeats <r> [--pking] [--port <p>] "
        "[--peer <addr>] [--output <file>] [--disable-optimized-shuffle]\n\n"
        "Protocols:\n"
        "  rss3: --num-parties must be 3, pids 0..2\n"
        "  nph : --num-parties is the number of compute parties, helper pid is n\n"
        "        --disable-optimized-shuffle forces generic shuffle\n",
        prog);
}

static Args parseArgs(int argc, char* argv[]) {
    Args a;

    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--pid") == 0 && i + 1 < argc) {
            a.pid = std::atoi(argv[++i]);
        } else if (std::strcmp(argv[i], "--protocol") == 0 && i + 1 < argc) {
            a.protocol_name = argv[++i];
            a.protocol = protocol::parseProtocolKind(a.protocol_name);
        } else if (std::strcmp(argv[i], "--num-parties") == 0 && i + 1 < argc) {
            a.num_parties = std::atoi(argv[++i]);
        } else if (std::strcmp(argv[i], "--vec-size") == 0 && i + 1 < argc) {
            a.vec_size = static_cast<size_t>(std::atoll(argv[++i]));
        } else if (std::strcmp(argv[i], "--num-repeats") == 0 && i + 1 < argc) {
            a.num_repeats = std::atoi(argv[++i]);
        } else if (std::strcmp(argv[i], "--port") == 0 && i + 1 < argc) {
            a.port = std::atoi(argv[++i]);
        } else if (std::strcmp(argv[i], "--pking") == 0) {
            a.pking = true;
        } else if (std::strcmp(argv[i], "--disable-optimized-shuffle") == 0) {
            a.disable_optimized_shuffle = true;
        } else if (std::strcmp(argv[i], "--peer") == 0 && i + 1 < argc) {
            a.peer = argv[++i];
        } else if (std::strcmp(argv[i], "--output") == 0 && i + 1 < argc) {
            a.output = argv[++i];
        } else {
            std::fprintf(stderr, "Unknown or incomplete argument: %s\n", argv[i]);
            printUsage(argv[0]);
            std::exit(1);
        }
    }

    bool pid_ok = false;
    if (a.protocol == protocol::ProtocolKind::Rss3) {
        pid_ok = (a.pid >= 0 && a.pid < 3 && a.num_parties == 3);
    } else if (a.protocol == protocol::ProtocolKind::Nph) {
        pid_ok = (a.num_parties >= 2 && a.pid >= 0 && a.pid <= a.num_parties);
    }

    if (!pid_ok || a.vec_size == 0 || a.num_repeats <= 0) {
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

    if (v.size() > max_items) std::printf(", ...");
    std::printf("]\n");
}

static void benchmark(const Args& args) {
    using Runner = protocol::IProtocolRunner<T>;
    using SP = bench::StatsPoint<Runner>;

    const int pid = args.pid;
    const size_t n = args.vec_size;

    std::printf("\n=== bench_repeat_shuffle ===\n");
    std::printf("  protocol    : %s\n", protocol::protocolName(args.protocol));
    std::printf("  num_parties : %d%s\n",
                args.num_parties,
                args.protocol == protocol::ProtocolKind::Nph
                    ? " compute parties + 1 helper"
                    : "");
    std::printf("  pking       : %s\n", args.pking ? "true" : "false");
    std::printf("  opt_shuffle : %s\n",
                args.disable_optimized_shuffle ? "disabled" : "enabled");
    std::printf("  pid         : %d%s\n",
                pid,
                (args.protocol == protocol::ProtocolKind::Nph && pid == args.num_parties)
                    ? " (helper)"
                    : "");
    std::printf("  vec_size    : %zu\n", n);
    std::printf("  num_repeats : %d\n", args.num_repeats);
    std::printf("  port        : %d\n", args.port);
    std::printf("  peer        : %s\n\n", args.peer.c_str());

    std::printf("[P%d] Building circuit...\n", pid);
    CircuitData cd = generateCircuit(n, args.num_repeats);
    const LevelOrderedCircuit& lc = cd.lc;

    std::printf("[P%d] Circuit: %zu gates, %zu wires, depth %zu\n\n",
                pid, lc.num_gates, lc.num_wires, lc.depth());

    std::vector<T> input_vals = makeInputValues(n);

    if (n <= 20) {
        printVector("input", pid, input_vals);
    }

    protocol::ProtocolConfig pcfg;
    pcfg.kind = args.protocol;
    pcfg.pid = args.pid;
    pcfg.num_compute_parties = args.num_parties;
    pcfg.port = args.port;
    pcfg.peer = args.peer;
    pcfg.pking = args.pking;
    pcfg.disable_optimized_shuffle = args.disable_optimized_shuffle;

    std::printf("\n[P%d] Connecting...\n", pid);
    auto runner = protocol::makeProtocolRunner<T>(pcfg);
    std::printf("[P%d] Connected.\n\n", pid);

    bench::increaseSocketBuffers(*runner, 128 * 1024 * 1024);

    if (!runner->isHelper() && pid == P0) {
        runner->setInputs(cd.input_wires, input_vals);
    }

    runner->resetCounters();

    std::printf("[P%d] Offline...\n", pid);
    SP offline_start(*runner);
    runner->offline(lc);
    SP offline_end(*runner);
    auto offline_stats = offline_end - offline_start;

    std::printf("[P%d] Online...\n", pid);
    SP online_start(*runner);
    runner->online(lc);
    SP online_end(*runner);
    auto out = runner->getOutputs(lc);
    auto online_stats = online_end - online_start;

    bool ok = true;
    if (!runner->isHelper()) {
        ok = sameMultiset(out, input_vals);
    }

    std::printf("[P%d] repeated shuffle correctness: %s%s\n",
                pid,
                runner->isHelper() ? "SKIP" : (ok ? "PASS" : "FAIL"),
                runner->isHelper() ? " (helper has no outputs)" : "");

    if (!runner->isHelper() && (!ok || n <= 20)) {
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

    std::printf("\n[P%d] --- stats ---\n", pid);
    bench::printPhaseStats(pid, "offline", offline_stats);
    bench::printPhaseStats(pid, "online", online_stats);

    std::printf("[P%d] %-18s  time: %9.3f ms  sent: %zu B  recv: %zu B\n\n",
                pid,
                "total",
                total_stats["time_ms"].get<double>(),
                static_cast<size_t>(
                    total_stats["total_bytes_sent"].get<uint64_t>()),
                static_cast<size_t>(
                    total_stats["total_bytes_recv"].get<uint64_t>()));

    nlohmann::json output_doc;
    output_doc["details"] = {
        {"benchmark", "bench_repeat_shuffle"},
        {"protocol", protocol::protocolName(args.protocol)},
        {"num_compute_parties", args.num_parties},
        {"pking", args.pking},
        {"optimized_shuffle_disabled", args.disable_optimized_shuffle},
        {"pid", pid},
        {"is_helper", runner->isHelper()},
        {"vec_size", n},
        {"num_repeats", args.num_repeats},
        {"port", args.port},
        {"peer", args.peer}
    };
    output_doc["correct"] = runner->isHelper() ? true : ok;
    output_doc["helper_skip"] = runner->isHelper();
    output_doc["offline"] = offline_stats;
    output_doc["online"] = online_stats;
    output_doc["total"] = total_stats;
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
