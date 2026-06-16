// benchmark/bench_unshuffle.cpp
//
// Protocol-flexible benchmark and correctness test for grouped shuffle/unshuffle.
//
// Circuit:
//   1. P0 provides a secret input vector x[0..n-1].
//   2. The circuit applies a grouped shuffle using perm_group_id.
//   3. The circuit applies an unshuffle gate with the same perm_group_id.
//   4. The final vector is reconstructed to all parties.
//
// Expected output:
//   unshuffle(shuffle(x)) = x
//
// Protocols:
//   rss3  -> existing 3-party replicated secret sharing
//   nph   -> n-party additive sharing with one helper in preprocessing
//
// Usage:
//   ./run.sh bench_unshuffle --protocol rss3 --vec-size 1000
//   ./run.sh bench_unshuffle --protocol nph --num-parties 5 --vec-size 1000
//   ./run.sh bench_unshuffle --protocol nph --num-parties 5 --vec-size 1000 --pking

#include "3pc/circuit/circuit.h"
#include "benchmark/utils.h"
#include "common/protocol_runner.h"

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
     * The unshuffle gate must reuse exactly the same hidden permutation group
     * and apply the inverse permutation chain.
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

    std::string protocol_name = "rss3";
    protocol::ProtocolKind protocol = protocol::ProtocolKind::Rss3;
    int num_parties = 3;  // compute parties; nph has one extra helper process
    bool pking = false;   // nph only: reconstruct through P0 in two rounds

    size_t vec_size = 0;

    int port = 14200;
    std::string peer = "127.0.0.1";

    int repeat = 1;
    std::string output;
};

static void printUsage(const char* prog) {
    std::fprintf(stderr,
        "Usage: %s --pid <pid> --protocol <rss3|nph> --num-parties <n> "
        "--vec-size <N> [--pking] [--port <p>] [--peer <addr>] "
        "[--repeat <r>] [--output <file>]\n\n"
        "Protocols:\n"
        "  rss3: --num-parties must be 3, pids 0..2\n"
        "  nph : --num-parties is the number of compute parties, helper pid is n\n"
        "        --pking enables two-round reconstruction through P0\n",
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
        } else if (std::strcmp(argv[i], "--port") == 0 && i + 1 < argc) {
            a.port = std::atoi(argv[++i]);
        } else if (std::strcmp(argv[i], "--pking") == 0) {
            a.pking = true;
        } else if (std::strcmp(argv[i], "--peer") == 0 && i + 1 < argc) {
            a.peer = argv[++i];
        } else if (std::strcmp(argv[i], "--repeat") == 0 && i + 1 < argc) {
            a.repeat = std::atoi(argv[++i]);
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

    if (!pid_ok || a.vec_size == 0 || a.repeat <= 0) {
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
    using Runner = protocol::IProtocolRunner<T>;
    using SP = bench::StatsPoint<Runner>;

    const int pid = args.pid;
    const size_t n = args.vec_size;

    std::printf("\n=== bench_unshuffle ===\n");
    std::printf("  protocol    : %s\n", protocol::protocolName(args.protocol));
    std::printf("  num_parties : %d%s\n",
                args.num_parties,
                args.protocol == protocol::ProtocolKind::Nph
                    ? " compute parties + 1 helper"
                    : "");
    std::printf("  pking       : %s\n", args.pking ? "true" : "false");
    std::printf("  pid         : %d%s\n",
                pid,
                (args.protocol == protocol::ProtocolKind::Nph && pid == args.num_parties)
                    ? " (helper)"
                    : "");
    std::printf("  vec_size    : %zu\n", n);
    std::printf("  port        : %d\n", args.port);
    std::printf("  peer        : %s\n", args.peer.c_str());
    std::printf("  repeat      : %d\n\n", args.repeat);

    std::printf("[P%d] Building circuit...\n", pid);
    CircuitData cd = generateCircuit(n);
    const LevelOrderedCircuit& lc = cd.lc;

    std::printf("[P%d] Circuit: %zu gates, %zu wires, depth %zu\n\n",
                pid, lc.num_gates, lc.num_wires, lc.depth());

    std::vector<T> input_vals = makeInputValues(n);
    std::vector<T> expected = input_vals;

    if (n <= 20) {
        printVector("input", pid, input_vals);
        printVector("expected", pid, expected);
    }

    protocol::ProtocolConfig pcfg;
    pcfg.kind = args.protocol;
    pcfg.pid = args.pid;
    pcfg.num_compute_parties = args.num_parties;
    pcfg.port = args.port;
    pcfg.peer = args.peer;
    pcfg.pking = args.pking;

    std::printf("\n[P%d] Connecting...\n", pid);
    auto runner = protocol::makeProtocolRunner<T>(pcfg);
    std::printf("[P%d] Connected.\n\n", pid);

    bench::increaseSocketBuffers(*runner, 128 * 1024 * 1024);

    // P0 owns the input vector. The NPH helper has no input wires.
    if (!runner->isHelper() && pid == P0) {
        runner->setInputs(cd.input_wires, input_vals);
    }

    nlohmann::json output_doc;
    output_doc["details"] = {
        {"benchmark", "bench_unshuffle"},
        {"protocol", protocol::protocolName(args.protocol)},
        {"num_compute_parties", args.num_parties},
        {"pking", args.pking},
        {"pid", pid},
        {"is_helper", runner->isHelper()},
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

        runner->resetCounters();

        std::printf("[P%d] Offline...\n", pid);
        SP offline_start(*runner);
        runner->offline(lc);
        SP offline_end(*runner);
        auto offline_stats = offline_end - offline_start;

        std::printf("[P%d] Online...\n", pid);
        SP online_start(*runner);
        runner->online(lc);
        auto out = runner->getOutputs(lc);
        SP online_end(*runner);
        auto online_stats = online_end - online_start;

        bool ok = true;
        if (!runner->isHelper()) {
            ok = (out == expected);
        }

        std::printf("[P%d] Unshuffle correctness: %s%s\n",
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

        std::printf("\n[P%d] --- Run %d stats ---\n", pid, run + 1);
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

        output_doc["runs"].push_back({
            {"run", run + 1},
            {"correct", runner->isHelper() ? true : ok},
            {"helper_skip", runner->isHelper()},
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
